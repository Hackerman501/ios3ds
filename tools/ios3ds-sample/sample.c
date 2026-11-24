/*
 * ios3ds-sample: periodic all-process thread sampler for iOS 4.2.1 (armv6).
 *
 * Every N seconds (argv[1], default 15) walks every process and writes one
 * compact line per thread to /dev/console: run state, pc/lr/sp, the syscall
 * or Mach trap the thread is parked in (with the port/semaphore it waits
 * on), and an r7 frame-pointer backtrace.
 *
 * Pure C; all Mach/BSD declarations are local (the stub SDK has none).
 * Needs root and the task_for_pid-allow entitlement; if task_for_pid() is
 * refused it falls back to processor_set_tasks() via the host-priv port.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* ---- Mach (32-bit ARM, xnu-1504) ---------------------------------- */
typedef unsigned int mach_port_t;
typedef int kern_return_t;
typedef unsigned int natural_t;
typedef unsigned int mach_msg_type_number_t;
typedef unsigned int vm_address_t;
typedef unsigned int vm_size_t;

extern mach_port_t mach_task_self_;
mach_port_t mach_host_self(void);
kern_return_t task_for_pid(mach_port_t target, int pid, mach_port_t *task);
kern_return_t pid_for_task(mach_port_t task, int *pid);
kern_return_t task_threads(mach_port_t task, mach_port_t **threads,
                           mach_msg_type_number_t *count);
kern_return_t thread_get_state(mach_port_t thread, int flavor,
                               natural_t *state, mach_msg_type_number_t *count);
kern_return_t thread_info(mach_port_t thread, int flavor, int *info,
                          mach_msg_type_number_t *count);
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr,
                                vm_size_t size, vm_address_t data,
                                vm_size_t *outsize);
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size);
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name);
kern_return_t processor_set_default(mach_port_t host, mach_port_t *pset_name);
kern_return_t host_processor_set_priv(mach_port_t host_priv, mach_port_t pset_name,
                                      mach_port_t *pset);
kern_return_t processor_set_tasks(mach_port_t pset, mach_port_t **tasks,
                                  mach_msg_type_number_t *count);

#define ARM_THREAD_STATE        1
#define ARM_THREAD_STATE_COUNT  17
struct arm_state {              /* r0-r12, sp, lr, pc, cpsr */
    unsigned r[13], sp, lr, pc, cpsr;
};
#define PSR_T 0x20

#define THREAD_BASIC_INFO        3
#define THREAD_BASIC_INFO_COUNT  10
struct thread_basic {
    int user_sec, user_usec, sys_sec, sys_usec;
    int cpu_usage, policy, run_state, flags, suspend_count, sleep_time;
};

#define MACH_SEND_MSG 1
#define MACH_RCV_MSG  2

/* ---- BSD ----------------------------------------------------------- */
int sysctl(int *name, unsigned namelen, void *oldp, size_t *oldlenp,
           void *newp, size_t newlen);
int proc_listpids(unsigned type, unsigned typeinfo, void *buf, int bufsize);
int proc_name(int pid, void *buf, unsigned bufsize);

#define CTL_KERN       1
#define KERN_PROC      14
#define KERN_PROC_ALL  0
#define PROC_ALL_PIDS  1

/*
 * struct kinfo_proc, 32-bit (xnu-1504 bsd/sys/sysctl.h + proc.h):
 *   extern_proc is 196 bytes: p_un 0(8) p_vmspace 8 p_sigacts 12 p_flag 16
 *   p_stat 20 p_pid 24 p_oppid 28 p_dupfd 32 user_stack 36 exit_thread 40
 *   p_debugger 44 sigwait 48 p_estcpu 52 p_cpticks 56 p_pctcpu 60 p_wchan 64
 *   p_wmesg 68 p_swtime 72 p_slptime 76 p_realtimer 80(16) p_rtime 96(8)
 *   p_uticks 104 p_sticks 112 p_iticks 120 p_traceflag 128 p_tracep 132
 *   p_siglist 136 p_textvp 140 p_holdcnt 144 p_sigmask 148 p_sigignore 152
 *   p_sigcatch 156 p_priority 160 p_usrpri 161 p_nice 162 p_comm 163(17)
 *   p_pgrp 180 p_addr 184 p_xstat 188 p_acflag 190 p_ru 192
 *   eproc (296 bytes) follows: e_paddr 0 e_sess 4 e_pcred 8(96) e_ucred
 *   104(76) e_vm 180(40) e_ppid 220 ...
 */
#define KP_SIZE   492
#define KP_PID    24
#define KP_COMM   163
#define KP_COMMSZ 17
#define KP_PPID   (196 + 220)

#define MAXPROC   512
#define MAXFRAMES 16

struct pent { int pid, ppid; char name[36]; };

static int con = -1;
static unsigned seq;
static char line[1536];
static int llen;

static void lreset(void) { llen = 0; }

static void lput(const char *fmt, ...)
{
    va_list ap;
    int n, room = (int)sizeof line - 2 - llen;
    if (room <= 0)
        return;
    va_start(ap, fmt);
    n = vsnprintf(line + llen, (size_t)room, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    llen += n >= room ? room - 1 : n;
}

static void lflush(void)
{
    if (con < 0)
        con = open("/dev/console", O_WRONLY);
    line[llen++] = '\n';
    if (write(con < 0 ? 2 : con, line, (size_t)llen) < 0 && con >= 0) {
        close(con);             /* console went away: reopen next time */
        con = -1;
    }
    llen = 0;
}

static int rd(mach_port_t task, unsigned addr, void *buf, unsigned len)
{
    vm_size_t got = 0;
    if (vm_read_overwrite(task, addr, len, (vm_address_t)buf, &got) != 0)
        return -1;
    return got == len ? 0 : -1;
}

/* ---- process list -------------------------------------------------- */
static void clean(char *s)
{
    for (; *s; s++)
        if (*s <= ' ' || *s > '~')
            *s = '_';
}

static int list_sysctl(struct pent *out)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t len = 0;
    unsigned char *buf;
    int i, n, cnt = 0;

    if (sysctl(mib, 4, 0, &len, 0, 0) != 0 || len < KP_SIZE)
        return -1;
    len += 16 * KP_SIZE;
    if (!(buf = malloc(len)))
        return -1;
    if (sysctl(mib, 4, buf, &len, 0, 0) != 0 || len % KP_SIZE) {
        free(buf);
        return -1;
    }
    n = (int)(len / KP_SIZE);
    for (i = 0; i < n && cnt < MAXPROC; i++) {
        unsigned char *kp = buf + (size_t)i * KP_SIZE;
        struct pent *p = &out[cnt];
        memcpy(&p->pid, kp + KP_PID, 4);
        memcpy(&p->ppid, kp + KP_PPID, 4);
        if (p->pid < 0 || p->pid > 99999) {     /* layout mismatch */
            free(buf);
            return -1;
        }
        memcpy(p->name, kp + KP_COMM, KP_COMMSZ);
        p->name[KP_COMMSZ - 1] = 0;
        clean(p->name);
        cnt++;
    }
    free(buf);
    return cnt;
}

static int list_libproc(struct pent *out)
{
    static int pids[MAXPROC];
    int n = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)sizeof pids);
    int i, cnt = 0;
    if (n <= 0)
        return -1;
    n /= (int)sizeof(int);
    for (i = 0; i < n && cnt < MAXPROC; i++) {
        struct pent *p = &out[cnt];
        if (pids[i] <= 0)
            continue;
        p->pid = pids[i];
        p->ppid = -1;
        p->name[0] = 0;
        if (proc_name(p->pid, p->name, sizeof p->name) <= 0)
            strcpy(p->name, "?");
        p->name[sizeof p->name - 1] = 0;
        clean(p->name);
        cnt++;
    }
    return cnt;
}

/* ---- task ports: task_for_pid, else processor_set_tasks ------------- */
static mach_port_t *ps_tasks;
static int *ps_pids;
static mach_msg_type_number_t ps_n;
static int ps_state;            /* 0 not tried this round, 1 ok, -1 failed */

static void pset_load(void)
{
    mach_port_t host = mach_host_self(), name = 0, pset = 0;
    mach_msg_type_number_t i;

    ps_state = -1;
    if (processor_set_default(host, &name) != 0)
        return;
    if (host_processor_set_priv(host, name, &pset) != 0)
        return;
    if (processor_set_tasks(pset, &ps_tasks, &ps_n) != 0) {
        ps_tasks = 0;
        ps_n = 0;
        return;
    }
    if (!(ps_pids = malloc(sizeof(int) * (ps_n + 1)))) {
        ps_n = 0;
        return;
    }
    for (i = 0; i < ps_n; i++)
        if (pid_for_task(ps_tasks[i], &ps_pids[i]) != 0)
            ps_pids[i] = -1;
    ps_state = 1;
}

static void pset_free(void)
{
    mach_msg_type_number_t i;
    if (ps_tasks) {
        for (i = 0; i < ps_n; i++)
            if (ps_tasks[i])
                mach_port_deallocate(mach_task_self_, ps_tasks[i]);
        vm_deallocate(mach_task_self_, (vm_address_t)ps_tasks,
                      ps_n * sizeof(mach_port_t));
    }
    free(ps_pids);
    ps_tasks = 0;
    ps_pids = 0;
    ps_n = 0;
    ps_state = 0;
}

/* Returns a task send right the caller must deallocate, or 0 (*kr set). */
static mach_port_t get_task(int pid, kern_return_t *kr, const char **via)
{
    mach_port_t t = 0;
    mach_msg_type_number_t i;

    *via = "tfp";
    *kr = task_for_pid(mach_task_self_, pid, &t);
    if (*kr == 0 && t)
        return t;
    if (!ps_state)
        pset_load();
    if (ps_state == 1)
        for (i = 0; i < ps_n; i++)
            if (ps_pids[i] == pid && ps_tasks[i]) {
                t = ps_tasks[i];
                ps_tasks[i] = 0;        /* ownership moves to the caller */
                *via = "pset";
                return t;
            }
    return 0;
}

/* ---- what is the thread parked in? --------------------------------- */
static const char *mach_trap_name(int n)
{
    switch (n) {
    case 26: return "mach_reply_port";
    case 27: return "thread_self";
    case 28: return "task_self";
    case 29: return "host_self";
    case 31: return "mach_msg";
    case 32: return "mach_msg_overwrite";
    case 33: return "semaphore_signal";
    case 34: return "semaphore_signal_all";
    case 35: return "semaphore_signal_thread";
    case 36: return "semaphore_wait";
    case 37: return "semaphore_wait_signal";
    case 38: return "semaphore_timedwait";
    case 39: return "semaphore_timedwait_signal";
    case 45: return "task_for_pid";
    case 59: return "swtch_pri";
    case 60: return "swtch";
    case 61: return "thread_switch";
    case 62: return "clock_sleep";
    case 90: return "mach_wait_until";
    case 93: return "mk_timer_arm";
    }
    return 0;
}

static const char *bsd_name(unsigned n)
{
    switch (n) {
    case 1: return "exit";
    case 3: return "read";
    case 4: return "write";
    case 5: return "open";
    case 6: return "close";
    case 7: return "wait4";
    case 27: return "recvmsg";
    case 29: return "recvfrom";
    case 30: return "accept";
    case 54: return "ioctl";
    case 93: return "select";
    case 98: return "connect";
    case 111: return "sigsuspend";
    case 202: return "sysctl";
    case 230: return "poll";
    case 271: return "sem_wait";
    case 301: return "psynch_mutexwait";
    case 305: return "psynch_cvwait";
    case 328: return "pthread_kill";
    case 330: return "sigwait";
    case 334: return "semwait_signal";
    case 363: return "kevent";
    case 368: return "workq_kernreturn";
    case 369: return "kevent64";
    case 396: return "read_nocancel";
    case 407: return "select_nocancel";
    case 417: return "poll_nocancel";
    case 423: return "semwait_signal_nocancel";
    }
    return 0;
}

/* If pc sits right after "svc 0x80", describe the trap (r12 selects it). */
static void put_syscall(mach_port_t task, const struct arm_state *s)
{
    unsigned insn = 0;
    unsigned short tinsn = 0;
    int n = (int)s->r[12];
    const char *nm;

    if (s->cpsr & PSR_T) {
        if (rd(task, (s->pc & ~1u) - 2, &tinsn, 2) != 0 || tinsn != 0xdf80)
            return;
    } else {
        if (rd(task, s->pc - 4, &insn, 4) != 0 || insn != 0xef000080)
            return;
    }

    if (n < 0) {                                /* Mach trap -n */
        n = -n;
        nm = mach_trap_name(n);
        if (n == 31 || n == 32) {
            /* r0 msg, r1 option, r2 send_size, r3 rcv_size, r4 rcv_name, r5 timeout */
            unsigned hdr[6];
            lput(" sys=%s(opt=0x%x", nm, s->r[1]);
            if ((s->r[1] & MACH_SEND_MSG) && rd(task, s->r[0], hdr, sizeof hdr) == 0)
                lput(",snd=0x%x,id=%d", hdr[2], (int)hdr[5]);
            if (s->r[1] & MACH_RCV_MSG)
                lput(",rcv=0x%x", s->r[4]);
            lput(",to=%u)", s->r[5]);
        } else if (n >= 33 && n <= 39) {
            lput(" sys=%s(sem=0x%x,0x%x,0x%x)", nm, s->r[0], s->r[1], s->r[2]);
        } else if (nm) {
            lput(" sys=%s(0x%x,0x%x,0x%x)", nm, s->r[0], s->r[1], s->r[2]);
        } else {
            lput(" sys=mach%d(0x%x,0x%x,0x%x)", n, s->r[0], s->r[1], s->r[2]);
        }
        return;
    }
    if ((unsigned)n == 0x80000000u) {           /* ARM machine-dependent call */
        lput(" sys=md(0x%x,0x%x)", s->r[3], s->r[0]);
        return;
    }
    nm = bsd_name((unsigned)n);
    if (nm)
        lput(" sys=%s(0x%x,0x%x,0x%x,0x%x)", nm, s->r[0], s->r[1], s->r[2], s->r[3]);
    else
        lput(" sys=bsd%d(0x%x,0x%x,0x%x,0x%x)", n, s->r[0], s->r[1], s->r[2], s->r[3]);
}

static const char *run_state(int st)
{
    switch (st) {
    case 1: return "RUN";
    case 2: return "STOP";
    case 3: return "WAIT";
    case 4: return "UNINT";
    case 5: return "HALT";
    }
    return "?";
}

/* ---- one thread ---------------------------------------------------- */
static void sample_thread(const struct pent *p, mach_port_t task,
                          mach_port_t th, unsigned idx, unsigned total)
{
    struct arm_state s;
    struct thread_basic bi;
    mach_msg_type_number_t cnt;
    kern_return_t kr;
    unsigned fp, frame[2];
    int i;

    lreset();
    lput("[smp %u] pid=%d %s t=%u/%u", seq, p->pid, p->name, idx, total);

    memset(&bi, 0, sizeof bi);
    cnt = THREAD_BASIC_INFO_COUNT;
    kr = thread_info(th, THREAD_BASIC_INFO, (int *)&bi, &cnt);
    if (kr == 0)
        lput(" %s%s susp=%d cpu=%d ut=%d.%03d st=%d.%03d", run_state(bi.run_state),
             (bi.flags & 2) ? "/idle" : (bi.flags & 1) ? "/swap" : "",
             bi.suspend_count, bi.cpu_usage, bi.user_sec, bi.user_usec / 1000, bi.sys_sec, bi.sys_usec / 1000);
    else
        lput(" ERR thread_info=%d", kr);

    memset(&s, 0, sizeof s);
    cnt = ARM_THREAD_STATE_COUNT;
    kr = thread_get_state(th, ARM_THREAD_STATE, (natural_t *)&s, &cnt);
    if (kr != 0) {
        lput(" ERR thread_get_state=%d", kr);
        lflush();
        return;
    }
    lput(" pc=0x%08x lr=0x%08x sp=0x%08x %c", s.pc, s.lr, s.sp,
         (s.cpsr & PSR_T) ? 'T' : 'A');
    put_syscall(task, &s);

    /* r7 frame chain: [fp] = caller's fp, [fp+4] = return address. */
    lput(" bt=");
    fp = s.r[7];
    for (i = 0; i < MAXFRAMES; i++) {
        if (fp < 0x1000 || (fp & 3) || rd(task, fp, frame, 8) != 0)
            break;
        if (frame[1] < 0x1000)
            break;
        lput(i ? ",0x%08x" : "0x%08x", frame[1]);
        if (frame[0] <= fp)             /* frames must move up the stack */
            break;
        fp = frame[0];
    }
    if (i == 0)
        lput("-");
    lflush();
}

/* ---- one process --------------------------------------------------- */
static void sample_proc(const struct pent *p)
{
    mach_port_t task, *threads = 0;
    mach_msg_type_number_t nth = 0, i;
    kern_return_t kr;
    const char *via;

    task = get_task(p->pid, &kr, &via);
    if (!task) {
        lreset();
        lput("[smp %u] pid=%d %s ERR task_for_pid=%d", seq, p->pid, p->name, kr);
        lflush();
        return;
    }
    kr = task_threads(task, &threads, &nth);
    if (kr != 0 || !threads) {
        lreset();
        lput("[smp %u] pid=%d %s ERR task_threads=%d", seq, p->pid, p->name, kr);
        lflush();
        mach_port_deallocate(mach_task_self_, task);
        return;
    }
    for (i = 0; i < nth; i++) {
        sample_thread(p, task, threads[i], i, nth);
        mach_port_deallocate(mach_task_self_, threads[i]);
    }
    vm_deallocate(mach_task_self_, (vm_address_t)threads, nth * sizeof(mach_port_t));
    mach_port_deallocate(mach_task_self_, task);
}

/* ---- memory report ("-mem"): [mem] lines --------------------------
 * host_statistics(HOST_VM_INFO) = vm_stat, task_info(TASK_BASIC_INFO_32)
 * per process (resident / virtual), host_zone_info (kernel zones, needs the
 * host-priv port: root), kern.ipc.mbstat (mbufs) and jetsam's
 * kern.memorystatus_level. The Mach calls are looked up with dlsym: the stub
 * SDK lists only the symbols the tools already import. */
#include <dlfcn.h>
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
typedef kern_return_t (*host_stat_fn)(mach_port_t, int, int *, mach_msg_type_number_t *);
typedef kern_return_t (*task_info_fn)(mach_port_t, int, int *, mach_msg_type_number_t *);
typedef kern_return_t (*zone_info_fn)(mach_port_t, void **, mach_msg_type_number_t *,
                                      void **, mach_msg_type_number_t *);
typedef kern_return_t (*host_priv_fn)(mach_port_t, mach_port_t *);
struct zname { char n[80]; };
struct zinfo { int count; unsigned cur, max, elem, alloc; int pageable, sleepable, exhaustible, collectable; };
struct rss { unsigned res, virt; int pid; char name[20]; };

static unsigned kib(int pages) { return (unsigned)pages * 4; }

static void mem_report(struct pent *procs, int n, int top)
{
    static host_stat_fn hs;
    static task_info_fn ti;
    static zone_info_fn zi;
    static host_priv_fn hp;
    static mach_port_t priv;
    static struct rss r[MAXPROC];
    int vm[15], i, j, nr = 0;
    unsigned tot = 0, zsum = 0;
    mach_msg_type_number_t cnt = 15;

    if (!hs) {
        hs = (host_stat_fn)dlsym(RTLD_DEFAULT, "host_statistics");
        ti = (task_info_fn)dlsym(RTLD_DEFAULT, "task_info");
        zi = (zone_info_fn)dlsym(RTLD_DEFAULT, "host_zone_info");
        hp = (host_priv_fn)dlsym(RTLD_DEFAULT, "host_get_host_priv_port");
        if (hp && hp(mach_host_self(), &priv) != 0)
            priv = 0;
        if (!priv) {
            /* root's own task can get host_priv through the host_priv_self()
               mach trap (root, unsandboxed: exactly these jobs); the port
               call host_get_host_priv_port wants a host_priv send right and
               never succeeds here (re-log 75) */
            mach_port_t (*hps)(void) = (mach_port_t (*)(void))dlsym(RTLD_DEFAULT, "host_priv_self");
            if (hps) {
                mach_port_t p = hps();
                if (p && p != (mach_port_t)-1)
                    priv = p;
            }
        }
        if (!zi || !priv) {
            lreset();
            lput("[mem %u] zones: no %s", seq, !zi ? "host_zone_info" : "host-priv port");
            lflush();
        }
    }
    lreset();
    lput("[mem %u]", seq);
    memset(vm, 0, sizeof vm);
    if (hs && hs(mach_host_self(), 2 /* HOST_VM_INFO */, vm, &cnt) == 0)
        lput(" KiB free %u active %u inactive %u spec %u wired %u purgeable %u | faults %u cow %u pageins %u"
             " pageouts %u purges %u | free_nospec %u", kib(vm[0]), kib(vm[1]), kib(vm[2]), kib(vm[14]), kib(vm[3]),
             kib(vm[12]), vm[8], vm[9], vm[6], vm[7], vm[13],
             vm[0] > vm[14] ? kib(vm[0] - vm[14]) : 0);
    {
        int lvl = -1;
        size_t l = sizeof lvl;
        if (sysctlbyname("kern.memorystatus_level", &lvl, &l, 0, 0) == 0)
            lput(" | memorystatus_level %d", lvl);
    }
    {
        unsigned char mb[600];
        size_t l = sizeof mb;
        unsigned v[7], f;
        if (sysctlbyname("kern.ipc.mbstat", mb, &l, 0, 0) == 0 && l >= 544) {
            memcpy(v, mb, sizeof v);
            memcpy(&f, mb + 540, sizeof f);
            int nmb = 0;
            size_t ln = sizeof nmb;
            sysctlbyname("kern.ipc.nmbclusters", &nmb, &ln, 0, 0);
            lput(" | mbuf %u cl %u/%d clfree %u drops %u wait %u drain %u mcfail %u", v[0], v[1], nmb, v[3], v[4],
                 v[5], v[6], f);
        }
    }
    lflush();

    /* iOS 4.2.1 has no VM compressor (that arrived in iOS 7): there are no
       compressed pages to count. Say so explicitly, so a hardware log reader
       does not mistake the absent field for an unmeasured one. */
    if (seq == 1) {
        lreset();
        lput("[mem %u] VM: compressed n/a (no compressor in iOS 4.2.1);"
             " free_nospec = free - speculative file pages", seq);
        lflush();
    }

    /* VM limits at the first report: the values that decide whether a large
       Safari allocation can be satisfied (docs/re-log: memory) */
    if (seq == 1) {
        static const char *const names[] = {
            "vm.zone_map_size", "vm.page_free_count", "vm.pages", "vm.page_pageable_internal_count",
            "vm.max_proc_mmap", "vm.max_map_count", "kern.max_task", "kern.max_task_pages",
        };
        lreset();
        lput("[mem %u] sysctl", seq);
        for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
            unsigned long long v = 0;
            size_t l = sizeof v;
            if (sysctlbyname(names[i], &v, &l, 0, 0) == 0)
                lput(" %s=%llu", names[i], v);
        }
        lflush();
    }

    /* kernel zones: total and the biggest few */
    if (zi && priv) {
        struct zname *zn = 0;
        struct zinfo *zf = 0;
        mach_msg_type_number_t nn = 0, nf = 0, k;
        kern_return_t zr = zi(priv, (void **)&zn, &nn, (void **)&zf, &nf);
        if (zr != 0 && seq == 1) {
            lreset();
            lput("[mem %u] zones: host_zone_info failed (%d)", seq, zr);
            lflush();
        }
        if (zr == 0 && zn && zf) {
            unsigned used[6] = { 0 }, idx[6] = { 0 };
            for (k = 0; k < nf && k < nn; k++) {
                unsigned u = zf[k].cur;
                zsum += u;
                for (i = 0; i < 6; i++)
                    if (u > used[i]) {
                        for (j = 5; j > i; j--) {
                            used[j] = used[j - 1];
                            idx[j] = idx[j - 1];
                        }
                        used[i] = u;
                        idx[i] = k;
                        break;
                    }
            }
            lreset();
            lput("[mem %u] zones %u KiB in %u:", seq, zsum >> 10, nf);
            for (i = 0; i < 6 && used[i]; i++) {
                zn[idx[i]].n[79] = 0;
                lput(" %s %u/%u", zn[idx[i]].n, used[i] >> 10, zf[idx[i]].max >> 10);
            }
            lflush();
            vm_deallocate(mach_task_self_, (vm_address_t)zn, nn * sizeof *zn);
            vm_deallocate(mach_task_self_, (vm_address_t)zf, nf * sizeof *zf);
        }
    }

    /* processes by resident size (KiB) and virtual size (MiB) */
    for (i = 0; i < n && nr < MAXPROC; i++) {
        kern_return_t kr;
        const char *via;
        mach_port_t t;
        int b[8];
        mach_msg_type_number_t c = 8;
        if (procs[i].pid <= 0 || !ti)
            continue;
        if (!(t = get_task(procs[i].pid, &kr, &via)))
            continue;
        if (ti(t, 4 /* TASK_BASIC_INFO_32 */, b, &c) == 0) {
            r[nr].res = (unsigned)b[2];
            r[nr].virt = (unsigned)b[1];
            r[nr].pid = procs[i].pid;
            strncpy(r[nr].name, procs[i].name, sizeof r[nr].name - 1);
            r[nr].name[sizeof r[nr].name - 1] = 0;
            tot += r[nr].res;
            nr++;
        }
        mach_port_deallocate(mach_task_self_, t);
    }
    pset_free();
    for (i = 1; i < nr; i++)            /* insertion sort, biggest first */
        for (j = i; j > 0 && r[j].res > r[j - 1].res; j--) {
            struct rss x = r[j];
            r[j] = r[j - 1];
            r[j - 1] = x;
        }
    lreset();
    lput("[mem %u] %d procs, resident %u KiB:", seq, nr, tot >> 10);
    for (i = 0; i < nr && i < top; i++)
        lput(" %s(%d) %u/%u", r[i].name, r[i].pid, r[i].res >> 10, r[i].virt >> 20);
    lflush();

    /* Virtual size per process (same data, sorted the other way). A large
       resident total with plenty of free pages but a stopped WebKit load
       points at per-process address-space fragmentation; the caller's VA is
       the only figure a root launchd job can get without the host-priv port. */
    for (i = 1; i < nr; i++)
        for (j = i; j > 0 && r[j].virt > r[j - 1].virt; j--) {
            struct rss x = r[j];
            r[j] = r[j - 1];
            r[j - 1] = x;
        }
    lreset();
    lput("[mem %u] VA MiB:", seq);
    for (i = 0; i < nr && i < 8; i++)
        lput(" %s(%d) %u", r[i].name, r[i].pid, r[i].virt >> 20);
    lflush();
}

int main(int argc, char **argv)
{
    static struct pent procs[MAXPROC];
    int memmode = argc > 1 && !strcmp(argv[1], "-mem");
    int interval = argc > 1 + memmode ? atoi(argv[1 + memmode]) : 15;
    int self = getpid();
    int n, i;
    const char *src;

    if (interval <= 0)
        interval = 15;

    if (memmode)        /* "ios3ds-sample -mem [secs]": only the [mem] lines */
        for (;;) {
            seq++;
            n = list_sysctl(procs);
            if (n <= 0)
                n = list_libproc(procs);
            mem_report(procs, n, 16);
            sleep((unsigned)interval);
        }

    for (;;) {
        seq++;
        src = "sysctl";
        n = list_sysctl(procs);
        if (n <= 0) {
            src = "libproc";
            n = list_libproc(procs);
        }
        lreset();
        lput("[smp %u] begin nproc=%d src=%s", seq, n, src);
        lflush();
        for (i = 0; i < n; i++) {
            if (procs[i].pid <= 0 || procs[i].pid == self)
                continue;               /* kernel_task and ourselves */
            sample_proc(&procs[i]);
        }
        pset_free();
        mem_report(procs, n, 16);
        lreset();
        lput("[smp %u] end", seq);
        lflush();
        sleep((unsigned)interval);
    }
    return 0;
}
