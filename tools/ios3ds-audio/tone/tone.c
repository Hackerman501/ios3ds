/*
 * ios3ds-tone: play a known test tone through the normal iOS audio path
 * (AudioQueue -> mediaserverd -> IOAudio2 -> AppleEmbeddedAudio -> the
 * shim's PL080/I2S emulation -> CSND), so the output can be checked: in QEMU
 * the board's CSND model records it (emu/qemu/hw/arm/ctr_csnd.c,
 * IOS3DS_CSND_WAV), on the 3DS you hear it and the shim logs the stream.
 *
 *   ios3ds-tone [count [interval_s [ms [hz [amplitude [delay_s]]]]]]
 *   defaults:    1      10          400  1000  6000      30
 *
 * Left channel = the tone, right channel = the tone at half amplitude, so a
 * swapped or mono path shows in the recording. AudioToolbox is resolved with
 * dlopen (the stub SDK has no AudioToolbox headers); the declarations below
 * follow the iOS 4 SDK. Nothing here can hang the system: every failure is
 * logged once and the program exits.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <math.h>

typedef int32_t OSStatus;
typedef void *AudioQueueRef;
typedef struct {
    double mSampleRate;
    uint32_t mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket;
    uint32_t mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved;
} AudioStreamBasicDescription;
typedef struct {
    const uint32_t mAudioDataBytesCapacity;
    void *const mAudioData;
    uint32_t mAudioDataByteSize;
    void *mUserData;
    const uint32_t mPacketDescriptionCapacity;
    void *const mPacketDescriptions;
    uint32_t mPacketDescriptionCount;
} AudioQueueBuffer, *AudioQueueBufferRef;
typedef void (*AQOutputCallback)(void *user, AudioQueueRef aq, AudioQueueBufferRef buf);

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define kAudioFormatLinearPCM               FOURCC('l', 'p', 'c', 'm')
#define kLinearPCMFlagsSignedPacked         ((1u << 2) | (1u << 3))
#define kAudioSessionProperty_AudioCategory FOURCC('a', 'c', 'a', 't')
#define kAudioSessionCategory_MediaPlayback FOURCC('m', 'e', 'd', 'i')

static OSStatus (*AQNewOutput)(const AudioStreamBasicDescription *, AQOutputCallback, void *,
                               void *, void *, uint32_t, AudioQueueRef *);
static OSStatus (*AQAllocateBuffer)(AudioQueueRef, uint32_t, AudioQueueBufferRef *);
static OSStatus (*AQEnqueueBuffer)(AudioQueueRef, AudioQueueBufferRef, uint32_t, const void *);
static OSStatus (*AQStart)(AudioQueueRef, const void *);
static OSStatus (*AQStop)(AudioQueueRef, unsigned char);
static OSStatus (*AQDispose)(AudioQueueRef, unsigned char);
static OSStatus (*ASInitialize)(void *, void *, void *, void *);
static OSStatus (*ASSetProperty)(uint32_t, uint32_t, const void *);
static OSStatus (*ASSetActive)(unsigned char);

#define RATE    44100
#define NBUF    3
#define FRAMES  1024

static struct {
    double phase, step;
    int amp;
    uint32_t left;          /* frames still to play */
    volatile int drained;   /* buffers that came back after the tone ended */
} T;

static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    int n, fd;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    fd = open("/dev/console", O_WRONLY);
    if (write(fd < 0 ? 2 : fd, line, (size_t)n) < 0) { /* nothing to do */ }
    if (fd >= 0) close(fd);
}

static void fill(AudioQueueRef aq, AudioQueueBufferRef b)
{
    int16_t *p = b->mAudioData;
    uint32_t n = 0;
    if (!T.left) {
        T.drained++;
        return;                                 /* not re-queued: the queue runs dry */
    }
    while (n < FRAMES && T.left) {
        int s = (int)(sin(T.phase) * T.amp);
        p[2 * n] = (int16_t)s;
        p[2 * n + 1] = (int16_t)(s / 2);
        T.phase += T.step;
        if (T.phase > 2 * M_PI) T.phase -= 2 * M_PI;
        n++;
        T.left--;
    }
    b->mAudioDataByteSize = n * 4;
    AQEnqueueBuffer(aq, b, 0, NULL);
}

static void callback(void *user, AudioQueueRef aq, AudioQueueBufferRef b)
{
    (void)user;
    fill(aq, b);
}

static int load(void)
{
    void *h = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    if (!h) {
        say("tone: no AudioToolbox: %s", dlerror());
        return 0;
    }
#define SYM(v, n) if (!(*(void **)&v = dlsym(h, n))) { say("tone: no %s", n); return 0; }
    SYM(AQNewOutput, "AudioQueueNewOutput");
    SYM(AQAllocateBuffer, "AudioQueueAllocateBuffer");
    SYM(AQEnqueueBuffer, "AudioQueueEnqueueBuffer");
    SYM(AQStart, "AudioQueueStart");
    SYM(AQStop, "AudioQueueStop");
    SYM(AQDispose, "AudioQueueDispose");
    SYM(ASInitialize, "AudioSessionInitialize");
    SYM(ASSetProperty, "AudioSessionSetProperty");
    SYM(ASSetActive, "AudioSessionSetActive");
#undef SYM
    return 1;
}

static int play(int ms, int hz, int amp)
{
    AudioStreamBasicDescription f;
    AudioQueueRef aq = NULL;
    AudioQueueBufferRef b[NBUF];
    OSStatus st;
    int i, waited;

    memset(&f, 0, sizeof f);
    f.mSampleRate = RATE;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFlagsSignedPacked;
    f.mBytesPerPacket = f.mBytesPerFrame = 4;
    f.mFramesPerPacket = 1;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    st = AQNewOutput(&f, callback, NULL, NULL, NULL, 0, &aq);
    if (st) return st;
    T.phase = 0;
    T.step = 2 * M_PI * hz / RATE;
    T.amp = amp;
    T.left = (uint32_t)((int64_t)RATE * ms / 1000);
    T.drained = 0;
    for (i = 0; i < NBUF; i++) {
        st = AQAllocateBuffer(aq, FRAMES * 4, &b[i]);
        if (st) goto out;
        fill(aq, b[i]);
    }
    st = AQStart(aq, NULL);
    if (st) goto out;
    for (waited = 0; T.drained < NBUF && waited < ms + 5000; waited += 20) usleep(20000);
    AQStop(aq, 0);
out:
    AQDispose(aq, 1);
    return st;
}

int main(int argc, char **argv)
{
    int count = argc > 1 ? atoi(argv[1]) : 1;
    int interval = argc > 2 ? atoi(argv[2]) : 10;
    int ms = argc > 3 ? atoi(argv[3]) : 400;
    int hz = argc > 4 ? atoi(argv[4]) : 1000;
    int amp = argc > 5 ? atoi(argv[5]) : 6000;
    int delay = argc > 6 ? atoi(argv[6]) : 30;
    uint32_t cat = kAudioSessionCategory_MediaPlayback;
    OSStatus st;
    int i, tries;

    if (!load()) return 1;
    sleep((unsigned)delay);                         /* let SpringBoard and mediaserverd settle */
    for (tries = 0; (st = ASInitialize(NULL, NULL, NULL, NULL)) && tries < 60; tries++) sleep(2);
    if (st) say("tone: AudioSessionInitialize %d (carrying on)", (int)st);
    st = ASSetProperty(kAudioSessionProperty_AudioCategory, sizeof cat, &cat);
    if (st) say("tone: audio category %d (carrying on)", (int)st);
    st = ASSetActive(1);
    if (st) say("tone: AudioSessionSetActive %d (carrying on)", (int)st);
    for (i = 0; i < count; i++) {
        if (i) sleep((unsigned)interval);
        say("tone: #%d %d Hz, %d ms, amplitude %d (L), %d (R)", i + 1, hz, ms, amp, amp / 2);
        st = play(ms, hz, amp);
        if (st) {
            say("tone: AudioQueue error %d: giving up", (int)st);
            return 1;
        }
        say("tone: #%d done", i + 1);
    }
    return 0;
}
