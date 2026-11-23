# Boot time and cold app launches

Where the time goes from the ARM9 payload's start to a settled home screen,
and on cold launches of Settings, Notes and Safari. Everything here was
measured in QEMU with `--icount 2` (instruction-clocked guest time). The
reverse-engineering steps are in [re-log §50](re-log.md).

## 1. Tools

| Tool | What it gives |
|---|---|
| `boot.cfg ioprof=1` | ARM9: one `[io]` line per second with md0 activity. It has XNU buffers vs 32 KiB chunks, KiB read/written, metadata (`B_META`) and page-in (`B_PAGEIO`) KiB, how much was sequential, the ARM9's service time, and running totals. |
| `boot.cfg ioprof=2` | Also one `[blk] t op lba+sectors buf <XNU bytes> f<buf_flags> <us>` line per chunk. The shim passes each chunk's XNU buffer size and `buf_flags` in the block mailbox (`xnu_bytes`, `xnu_flags`; bit 31 = first chunk of a buffer). |
| `boot.cfg log_ts=1` | `[s.ms]` stamp before every forwarded console line: the ARM9 clock, seconds since the ARM9 started. |
| `[boot] s: ...` | ARM9 phase stamps before XNU starts (always on, 8 lines). |
| `N3DS_PROF=<file>` (QEMU) | Every 1 ms of guest time it samples whether each CPU is in WFI and whether an md0 request is outstanding (blkmbox `seq != done_seq`). It writes one `[qprof]` line per 250 ms: ARM11 busy %, ARM9 busy %, md0 pending %, and "md0 pending while the ARM11 idles". The line carries the ARM9 clock (`a9t`) so it lines up with the log. |
| `emu/bootprof.py` | Boots with `ioprof=2 log_ts=1` plus the profiler (boot.cfg is restored afterwards), waits for the boot to settle, cold-launches Settings, Notes and Safari, and writes `report.md`/`report.json` (phases and per-app stats), `blk.csv` and the log. `--report-only` re-analyses; `--cfg key=value` changes boot.cfg for one run. |
| `emu/hfsmap.py rootfs.hfs <dir>` | Maps every md0 sector in `blk.csv` to its HFS+ file (data or resource fork) or to the catalog, extents or attributes B-tree, per phase, and writes `files.md`. |
| `boot.cfg diag=1` | Turns the test jobs back on (§3), plus `[iobusy]`: the IOServices that are still busy 0–3 s after launchd loads the daemons. |

**Caveat on guest time.** With `-icount` QEMU's round-robin TCG gives both vCPUs one shared instruction clock. The ARM9 monitor never sleeps while the screen is on (`a9 100%` in every `[qprof]` line), so the ARM11 gets about half of the guest instructions. Its ~125 M instructions per guest second are close to the real ARM11 (268 MHz, CPI > 1).

The emulated SD card costs only the ARM9's own instructions: md0 serves ~30 MB per guest second. The real card does not; see §5. The numbers are therefore good for comparing runs. Treat them as hardware boot times only for the CPU-bound parts.

## 2. Results

Before = origin/main 50ac259 plus the instrumentation, with the boot.cfg it shipped (`probes=1`, `countdown_ms=2000`, test jobs at boot). After = this branch. Seconds of guest time since the ARM9 started:

| | before, 1st boot | before, 2nd boot | after, 1st boot | after, 2nd boot |
|---|---|---|---|---|
| XNU starts | 6.7 | 6.7 | 5.5 | 5.5 |
| launchd starts | 9.7 | 9.6 | 8.7 | 8.9 |
| SpringBoard's HID system up (input daemon connects) | 24.9 | 23.6 | 17.6 | 16.4 |
| boot settled (3 s with ARM11 < 10 % and no md0) | **34.0** | **30.7** | **26.2** | **23.5** |
| md0 read during boot | 84.2 MB | 81.2 MB | 78.5 MB | 78.2 MB |
| md0 chunks (XNU buffers) | 4935 (3161) | 4212 (2532) | 3797 (2089) | 3632 (1976) |
| ARM9 time serving md0 | 2.97 s | 2.52 s | 2.37 s | 1.85 s |

Cold app launches, from touch-down until md0 has gone quiet and the ARM11 has settled (1 s under 10 %), first boot:

| app | before: s / chunks / KiB read | after: s / chunks / KiB read |
|---|---|---|
| Settings | 3.35 / 571 / 3661 | 2.98 / 125 / 1804 |
| Notes | 1.81 / 114 / 1645 | 1.71 / 85 / 1524 |
| Safari | 4.96 / 269 / 3577 | 2.73 / 244 / 2864 |

Second boot (the same images rebooted; the caches from the first boot exist):

| app | before: s / chunks / KiB read | after: s / chunks / KiB read |
|---|---|---|
| Settings | 3.40 / 584 / 3749 | 2.87 / 138 / 2040 |
| Notes | 1.55 / 80 / 1433 | 1.53 / 75 / 1468 |
| Safari | 4.62 / 167 / 2733 | 2.48 / 155 / 3052 |

Safari's gain is CPU: before, it too scanned the time zones (§4.2).

During boot the ARM11 is 80–97 % busy in every phase. md0 has a request outstanding 10–15 % of the time, and the ARM11 sits idle waiting for md0 only 1–2 % of the time. In QEMU, boot is CPU-bound, apart from the 5 s IOKit wait (§4.1).

## 3. Test jobs are opt-in (`diag=1`)

`ios3ds-gltest` (GL self-test at boot), `ios3ds-tone` (boot tone after 30 s), `ios3ds-sample` (`[smp]` thread dump every 60 s) and the new `iobusy` check now run only with `diag=1` in boot.cfg. The ARM9 then appends `ios3ds_diag=1` to the kernel boot-args. Each job's plist starts `/usr/libexec/ios3ds-diag <program> <args>` (tools/ios3ds-diag): the wrapper reads `kern.bootargs` and either `execv`s the program or exits 0. The sampler's `KeepAlive` is now `SuccessfulExit = false`, so the exit does not respawn it.

`diag=1` also runs the ARM11 instruction probes. `probes=0` is the new default (`probes=1` alone still works). The jump countdown is 1 s instead of 2 s, and B still cancels it.

**App logs (re-log 62).** With `diag=1`, or `ios3ds_applog=1` added to boot.cfg's `cmdline`, `/usr/lib/ios3ds-applog.dylib` (tools/ios3ds-diag/applog.c) sends SpringBoard's and every app's NSLog/stderr to the console, logs each alert's title and text (`[applog] <program> alert ...`) and, on every `viewDidAppear:`, the controller's class and view tree. SpringBoard loads it through its plist's `DYLD_INSERT_LIBRARIES`; the apps get it from SpringBoard's own debug switch `SBGuardMallocAlways`, which inserts `/usr/lib/libgmalloc.dylib` (a symlink to it, the device has no libgmalloc). Without either boot-arg the library returns at once.

**For hardware test runs, set `diag=1`.** Otherwise the `gltest:`, `tone:` and `[smp]` lines that docs/pica200.md, docs/audio.md and docs/real-hardware.md ask for are not in the log.

## 4. What was fixed

### 4.1 launchctl's 5 s IOKitWaitQuiet (every boot)

After mounting the root filesystem, launchctl calls `IOKitWaitQuiet()` with a 5 s timeout before it loads the launch daemons. In every boot nothing ran from 10.7 to 15.7 s: no md0, ARM11 idle.

`[iobusy]` showed the culprits: `clkrstgen` and `wdt` (AppleARMIODevice, state 0xa = matched but not registered), busy 1, together with their parents.

A SELECT+START thread dump taken in that window had two IOKit matching threads, both blocked in `IOService::waitForService` without a timeout:

| Driver | Called from | Waits for |
|---|---|---|
| `AppleS5L8720XClockController::start` | AppleS5L8720X+0x2f68 → AppleARMPlatform+0x2831 | `function-core_voltage_%ld` |
| `AppleARMWatchDogTimer::start` | AppleARMPlatform+0xa017 | `serviceMatching("IOPMU")` |

Both wait for the iPod's PMU, which the 3DS doesn't have.

- `wdt-no-pmu-wait` (kernel/patches-boot.json): the watchdog's `waitForService` becomes `movs r0,#0`. Its code already handles NULL: it only reads the PMU's `IOPMUBootDebug`.
- The clock controller cannot simply be skipped. A NULL function panics with "Bad DPSM Tables in Device Tree", and the platform already uses the half-started object (re-log 41's `s5l-perf-nowait`). `clkctl-voltage-quiet` instead points the lookup's call at `shim_quiet_clk_function` (kernel/shim/quiet.c, shim +0x28). That function calls `IOService::adjustBusy(-1)` on `IODeviceTree:/arm-io/clkrstgen` before the endless wait, and `+1` should the wait ever return.

Result: `[iobusy] 0s: 0 of 80 services busy`, and the daemons start ~5 s earlier. That is −5 s on hardware too, because the wait is a fixed timeout.

### 4.2 Every process scanned /usr/share/zoneinfo

The UTC change (re-log 38) made `/private/var/db/timezone/localtime` a regular file. Apple ships it as a symlink. CoreFoundation names the system time zone after the symlink target. For a regular file it searches /usr/share/zoneinfo for a file with the same contents. That meant ~340 small reads plus catalog lookups in every process that asks for the time zone (SpringBoard, Mail, Settings, ...). `hfsmap.py` found them as 339-file bursts at 19 s, 32 s and 57 s (Settings: 454 of its 571 chunks).

The recipe now creates a symlink to `/usr/share/zoneinfo/UTC`, using a new `symlink` section that scripts/mkrootfs.py writes with xpwn's `hfsplus symlink`. Settings' cold launch drops from 571 to 125 md0 chunks.

### 4.3 Mail no longer starts at boot

MobileMail has the `continuous` background mode, so SpringBoard auto-launches it at boot. That cost 1.7 s of CPU (`[smp]` ut+st), 0.8 MB of md0 and its memory, with no account configured. SpringBoard's own default `SBDisableMailAutoLaunch` (in the recipe's com.apple.springboard.plist) turns that off. Mail now starts when it is opened.

## 5. Read sizes, caching and HFS+ access patterns (measured, not changed)

| What | Measurement |
|---|---|
| What XNU asks for | Page-ins are up to 128 KiB buffers. `mnt_maxreadcnt` defaults to MAXPHYS because memdev answers none of the DKIOCGETMAX* ioctls (mdevioctl 0x80072a14). The VM's pre-heat gathers up to 1 MiB per fault. During the launchd phase alone, 140 runs of 128 KiB to 1.7 MiB (33 MB in total) come from the shared cache. Catalog nodes are synchronous 4 KiB reads; the attributes B-tree uses 8 KiB. File read-ahead mostly comes in as 4 KiB async reads. |
| What the shim does | It splits each buffer into 32 KiB bounce chunks. During boot that is 3161 XNU requests → 4935 round trips. |
| Where the bytes go | 63 of the 84 MB read during boot are the dyld shared cache (800 KiB of it is its code signature, read when launchd starts). VirtualAudio.plugin is 2.8 MB, SpringBoard 1.7 MB, the catalog 2.3 MB, fairplayd 0.9 MB. |
| Catalog | 567 distinct catalog nodes are read once each; almost nothing is read twice (1 node). Over the whole boot only 4 % of the bytes are re-reads. |

- **ARM9 read-ahead/cache:** UBC already caches everything and re-reads are rare, so an ARM9-side cache could only win on re-reads. The ARM9 also has no spare memory: its 1 MiB holds the monitor, FCRAM belongs to XNU, and VRAM to the screens. Not done.
- **Bigger transfers:** a 128 KiB bounce would save 1774 of the 4935 round trips. It needs a new shared-memory layout (the shim region is 64 KiB and its upper half is the bounce), or a scatter list of the XNU buffer's physical pages for the ARM9. Its value depends on the real card's per-command cost (below). Not done.
- **Smaller page-in clusters:** a smaller pre-heat would read less of the shared cache, but turn into more requests. Whether that wins depends on the same cost model. Not done.

The deciding unknown is the real SD card: what one md0 chunk costs (fixed command overhead vs per-byte). A hardware run with `ioprof=1` measures it. Each `[io]` line has the ARM9's service time for that second's chunks and KiB. If the cost is per byte, page-in clusters are the lever; if it is per command, the bounce size is. During boot the run13 card did ~68 MB in ~80 s together with the CPU screen copy.

## 6. Daemons at boot (`[smp]`, CPU seconds after the boot, first boot)

| CPU s | Process |
|---|---|
| 8.4 | SpringBoard |
| 2.2 | lsd |
| 1.8 | configd |
| 1.7 | MobileMail (gone, §4.3) |
| 1.6 | launchd |
| 1.6 | mediaserverd |
| 0.6 | profiled |
| 0.3–0.4 each | syslogd, mDNSResponder, ios3ds-mcud, installd |
| ≤ 0.2 | input, notifyd, fairplayd, wifid |

`fairplayd.N72` (KeepAlive; it decrypts App Store binaries, and none are installed) costs 0.9 MB of md0 and little CPU. MobileStorageMounter starts through an IOKit USB launch event. VirtualAudio/AirTunesHAL belong to the audio stack (mediaserverd). These are left alone: removing them changes behaviour for little gain.

`enable-dylibs-to-override-cache` (docs/gpu.md) makes dyld stat each cached dylib's path at every launch. It is needed for MBXGLEngine.

## 7. Hardware: what the next run shows

| Log line | Checks |
|---|---|
| `[boot] s: ...` | ARM9 phases on hardware: kernel load from the card, fast-seek map, countdown. |
| `[iobusy] 0s: 0 of N services busy` (with `diag=1`) | The IOKit fix also holds on hardware, with no other driver stuck. |
| `[io] ...` (with `ioprof=1`) | Real md0 cost per second: ARM9 ms vs KiB and chunks, i.e. the cost model §5 needs. |
| `[wifid] waiting ...` right after `Running fsck` | No 5 s gap between launchctl and the first daemons. |

## 8. Bigger md0 chunks and a write-back cache (re-log NEXT)

§5 listed bigger transfers and an ARM9 cache as "not done". Both are now in,
aimed at Cydia/dpkg rather than boot (where UBC re-reads are rare):

- **Bounce:** the shim's 32 KiB bounce was the upper half of the 64 KiB shim
  region. There is unused FCRAM from the end of the XNU log ring
  (0x20890010) to the Wi-Fi mailbox (0x208A0000); the same static map covers
  it, so the bounce moved to PA 0x20890020 / VA 0x80890020 and is now 0xFFE0
  bytes (127 sectors, 63.5 KiB) instead of 0x8000. No new mapping or bounce
  memory is taken from XNU. `kernel/shim/blk.c` rounds the chunk length *down*
  to whole sectors: rounding 0xFFE0 up gave 128 sectors = 64 KiB, which the
  ARM9 rejected as too large (found by the first QEMU run of this change).
- **Write-back:** `boot/arm9/blkcache.c` keeps 512 dirty sectors (256 KiB) in
  ARM9 BSS, with a hash for overlay lookups so reads see a written sector
  before it reaches the card. Small writes (≤ 8 KiB, excluding the HFS+
  volume header at LBA 2) stay in the cache; larger writes go straight to the
  file. Both paths defer `f_sync` until the disk has been idle for 500 ms (2 s
  hard cap) or iOS asks for a clean unmount. `blksrv_mark_clean()` flushes and
  syncs *before* setting `kHFSVolumeUnmounted`; if that fails it clears the
  bit on disk, so lost write-back data can never look like a clean unmount.
  This keeps the crash behaviour of the original synchronous path.
- **Cost:** a first attempt sorted dirty slots with an O(n²) scan per flush,
  which cost more ARM9 time than it saved; flushing is one linear pass now.
  The cache keeps a dirty-slot count, so `blkc_dirty()` is O(1): it used to
  scan all 512 slots on every monitor loop (the idle check) and to test every
  sector of every read request. With no dirty overlay at all, an md0 read is
  one `f_read` as before; only a read that can see a dirty sector is split
  into runs. Host tests: `tests/test_blkcache.c` (`make test`).

With `ioprof=1` during a local-repo `dpkg -i` (same 32-file `.deb` before and
after, guest seconds, [cydia.md](cydia.md) has the full timing table):

| | md0 chunks | bytes written | ARM9 service time |
|---|---:|---:|---:|
| before | 618 | ~584 KiB | 316 ms |
| after | 556 | ~612 KiB | 288 ms |

The bytes are the same — dpkg writes mostly distinct sectors, so there is
little to coalesce — but there are 10 % fewer round trips and the small writes
reach the card in batches instead of one `f_sync` each. QEMU's SD is instant,
so this shows up as −0.07 s on the install; a real card's per-command cost is
what makes it useful. Re-runs under host load vary by ±1 s in guest wall time
and by a few hundred chunks, so compare the chunks/KiB/ARM9 columns, not one
wall-clock number. The `[io]` summary now ends in
`cache H hit E evict W flushed D direct F fail` (`W` is the cumulative number
of sectors written back, `D` direct-write sectors, `F` sync/idle-flush
errors). For a real-hardware A/B, boot with `ioprof=1` and compare the ARM9
ms/KiB/chunk figures here with the old ones; `[io] t=... rd/wr` and the
`cache` fields are enough to see whether the card, not the CPU, is the
bottleneck.

