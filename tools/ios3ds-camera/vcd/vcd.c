/*
 * vcd.mediacapture: the 3DS cameras as an iOS 4.2.1 capture device.
 *
 * Celestial (mediaserverd's recorder, figCreateVideoCaptureDevice 0x32e01520)
 * loads /System/Library/MediaCapture/vcd.mediacapture and calls
 * FigVirtualCameraDeviceCreate(allocator, NULL, &device) when the preference
 * "vcd" of com.apple.coremedia is true, before it looks for Apple's camera
 * services (AppleH1..H4CamIn). This file implements that device on the same
 * FigBaseObject interface as Apple's own H3ISP.mediacapture (RE notes:
 * docs/camera.md section 3, docs/re-log.md section 65):
 *
 *   device: StreamArray (two streams), Clock (host time), SupportedPropertiesDictionary
 *   stream: PortType (Camera = outer left 3DS camera, FrontFacingCamera = inner),
 *           SupportedFormatsArray / FormatDescription / FormatIndex (one format:
 *           320x240 'BGRA'), Min/MaximumFrameRate, BufferQueue and
 *           StillImageBufferQueue (CMBufferQueues the plug-in owns and fills with
 *           CMSampleBuffers of IOSurface-backed CVPixelBuffers),
 *           StillImageCaptureNow (the next frame also goes to the still queue);
 *           Start/Stop post StreamStarted / StreamStopped.
 *
 * Frames come from the kernel shim (sysctl ios3ds_cam, boot/common/cam_proto.h):
 * YUV422 from the 3DS CAM block, converted to BGRA here: CoreAnimation
 * composites BGRA surfaces itself, while it hands 4:2:0 ones to an AppleM2CLCD
 * overlay window that ios3ds does not show (docs/camera.md section 1). With the preference
 * "ios3dsPattern" set (or IOS3DS_CAM_PATTERN=1) the plug-in draws the test
 * pattern itself instead (bring-up without the kernel path).
 *
 * Every Celestial/CoreMedia/CoreVideo entry point is looked up with dlsym: the
 * stub SDK has no headers or stubs for them.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/types.h>
#include "cam_proto.h"
#include "cam_pattern.h"

/* ---- CoreFoundation / CoreMedia / CoreVideo declarations (iOS 4.2.1 ABI) ---- */
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFArray *CFMutableArrayRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFNumber *CFNumberRef;
typedef const struct __CFBoolean *CFBooleanRef;
typedef unsigned long CFTypeID;
typedef long CFIndex;
typedef unsigned char Boolean;
typedef int32_t OSStatus;
typedef struct { CFIndex version; void *retain, *release, *copyDescription, *equal; } CFArrayCallBacks;
typedef struct { CFIndex version; void *retain, *release, *copyDescription, *equal, *hash; } CFDictionaryKeyCallBacks;
typedef struct { CFIndex version; void *retain, *release, *copyDescription, *equal; } CFDictionaryValueCallBacks;
extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFBooleanRef kCFBooleanTrue, kCFBooleanFalse;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
CFTypeRef CFRetain(CFTypeRef);
void CFRelease(CFTypeRef);
Boolean CFEqual(CFTypeRef, CFTypeRef);
CFTypeID CFGetTypeID(CFTypeRef);
CFTypeID CFNumberGetTypeID(void);
CFTypeID CFBooleanGetTypeID(void);
CFNumberRef CFNumberCreate(CFAllocatorRef, CFIndex type, const void *);
Boolean CFNumberGetValue(CFNumberRef, CFIndex type, void *);
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef, CFIndex, const CFArrayCallBacks *);
void CFArrayAppendValue(CFMutableArrayRef, const void *);
CFIndex CFArrayGetCount(CFArrayRef);
const void *CFArrayGetValueAtIndex(CFArrayRef, CFIndex);
CFArrayRef CFArrayCreateCopy(CFAllocatorRef, CFArrayRef);
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, CFIndex, const CFDictionaryKeyCallBacks *,
                                                 const CFDictionaryValueCallBacks *);
void CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, uint32_t);
Boolean CFStringGetCString(CFStringRef, char *, CFIndex, uint32_t);
Boolean CFPreferencesGetAppBooleanValue(CFStringRef, CFStringRef, Boolean *);
#define kCFNumberSInt32Type  3
#define kCFNumberFloat64Type 6
#define kCFNumberIntType     9
#define kCFStringEncodingUTF8 0x08000100u

typedef struct { int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; } CMTime;
typedef struct { CMTime duration, pts, dts; } CMSampleTimingInfo;
_Static_assert(sizeof(CMTime) == 24, "CMTime layout (iOS ARM ABI: 64-bit fields 4-byte aligned)");

typedef CFTypeRef FigObj;                       /* FigBaseObject / FigCaptureDevice / FigCaptureStream */
typedef struct {
    uint32_t version;
    uint32_t derivedStorageSize;
    void *equal;
    OSStatus (*invalidate)(FigObj);
    void (*finalize)(FigObj);
    void *copyDebugDescription;
    OSStatus (*copyProperty)(FigObj, CFStringRef, CFAllocatorRef, void *);
    OSStatus (*setProperty)(FigObj, CFStringRef, CFTypeRef);
    void *notificationBarrier;
} FigBaseClass;
typedef struct { uint32_t version; OSStatus (*start)(FigObj); OSStatus (*stop)(FigObj); } FigStreamClass;
typedef struct { uint32_t version; void *reserved[4]; } FigDeviceClass;
typedef struct { void *reserved; const FigBaseClass *base; const void *derived; } FigVTable;

#define kFigPropertyNotFound   (-12784)         /* H3ISP's answer for an unknown key */
#define kFigInvalidated        (-12785)
#define kFigParamErr           (-12780)
#define PIX_BGRA 0x42475241u                    /* kCVPixelFormatType_32BGRA */

static struct {
    OSStatus (*DerivedObjectCreate)(CFAllocatorRef, const FigVTable *, CFTypeID, FigObj *);
    void *(*GetDerivedStorage)(FigObj);
    CFTypeID (*DeviceGetClassID)(void);
    CFTypeID (*StreamGetClassID)(void);
    CFTypeRef (*DeviceGetNotificationCenter)(void);
    OSStatus (*PostNotification)(CFTypeRef, CFStringRef, CFTypeRef, CFDictionaryRef, uint32_t);
    CFTypeRef (*HostTimeClock)(void);
    uint64_t (*HostTimeToNanoseconds)(uint64_t);
    const void *(*BufQueueCallbacks)(void);
    OSStatus (*BufQueueCreate)(CFAllocatorRef, CFIndex, const void *, CFTypeRef *);
    OSStatus (*BufQueueEnqueue)(CFTypeRef, CFTypeRef);
    CFIndex (*BufQueueCount)(CFTypeRef);
    OSStatus (*VideoFmtCreate)(CFAllocatorRef, uint32_t, int32_t, int32_t, CFDictionaryRef, CFTypeRef *);
    OSStatus (*VideoFmtForImage)(CFAllocatorRef, CFTypeRef, CFTypeRef *);
    OSStatus (*SampleForImage)(CFAllocatorRef, CFTypeRef, Boolean, void *, void *, CFTypeRef,
                               const CMSampleTimingInfo *, CFTypeRef *);
    OSStatus (*PoolCreate)(CFAllocatorRef, CFDictionaryRef, CFDictionaryRef, CFTypeRef *);
    OSStatus (*PoolCreatePB)(CFAllocatorRef, CFTypeRef, CFTypeRef *);
    void (*PoolRelease)(CFTypeRef);
    OSStatus (*Lock)(CFTypeRef, uint64_t);
    OSStatus (*Unlock)(CFTypeRef, uint64_t);
    uint8_t *(*BaseAddress)(CFTypeRef);
    size_t (*BPR)(CFTypeRef);
    uint64_t (*mach_absolute_time)(void);
    int (*sysctl)(int *, unsigned, void *, size_t *, void *, size_t);
    int (*sysctlnametomib)(const char *, int *, size_t *);
    /* data */
    CFStringRef kDevStreamArray, kDevClock, kSupportedProps;
    CFStringRef kPortType, kPortCamera, kPortFront, kBufferQueue, kStillQueue, kBufferAllocator,
                kSupportedFormats, kFormatDescription, kFormatIndex, kMinFps, kMaxFps, kStillNow,
                kSFFormatDescription, kSFMaxFps, kSFScale, kSFBinned,
                kNoteStarted, kNoteStopped, kNoteDropped,
                kPTypeKey, kPRWKey, kPSerKey, kTypeNumber, kTypeString, kTypeBool, kTypeBufferQueue,
                kTypeFormatDescription, kTypeCollection, kTypeClock, kTypeAllocator,
                kRO, kRW, kWO;
    CFStringRef kScaledPreview;
    CFArrayRef (*SampleAttachments)(CFTypeRef, Boolean);
    CFStringRef kPBFormat, kPBWidth, kPBHeight, kPBIOSurface, kPoolMin, kIOSurfaceGlobal;
} F;

static int logfd = -1;
static void lg(const char *fmt, ...)
{
    char b[300];
    va_list ap;
    int n = snprintf(b, sizeof b, "[cam] ");
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof b - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n';
    if (logfd < 0) logfd = open("/dev/console", O_WRONLY | O_NOCTTY);
    if (logfd >= 0) write(logfd, b, (size_t)n);
}

static void *sym(const char *name, int *bad)
{
    void *p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        lg("missing symbol %s", name);
        *bad = 1;
    }
    return p;
}
#define FN(field, name) (*(void **)&F.field = sym(name, &bad))
#define KEY(field, name) do { CFStringRef *p_ = (CFStringRef *)sym(name, &bad); F.field = p_ ? *p_ : 0; } while (0)
/* Celestial keeps some of its constants private (bound into Apple's plug-ins by
   the shared cache, not exported to dlsym); their CFStrings hold these texts and
   the keys are compared with CFEqual, so an equal string does the same job */
#define KEYS(field, name, text) do { CFStringRef *p_ = (CFStringRef *)dlsym(RTLD_DEFAULT, name); \
        F.field = p_ ? *p_ : CFStringCreateWithCString(kCFAllocatorDefault, text, kCFStringEncodingUTF8); } while (0)

static int resolve(void)
{
    static int done, ok;
    int bad = 0;
    if (done) return ok;
    done = 1;
    FN(DerivedObjectCreate, "FigDerivedObjectCreate");
    FN(GetDerivedStorage, "FigBaseObjectGetDerivedStorage");
    FN(DeviceGetClassID, "FigCaptureDeviceGetClassID");
    FN(StreamGetClassID, "FigCaptureStreamGetClassID");
    FN(DeviceGetNotificationCenter, "FigCaptureDeviceGetNotificationCenter");
    FN(PostNotification, "CMNotificationCenterPostNotification");
    FN(HostTimeClock, "FigClockGetHostTimeClock");
    FN(HostTimeToNanoseconds, "FigHostTimeToNanoseconds");
    FN(BufQueueCallbacks, "CMBufferQueueGetCallbacksForUnsortedSampleBuffers");
    FN(BufQueueCreate, "CMBufferQueueCreate");
    FN(BufQueueEnqueue, "CMBufferQueueEnqueue");
    FN(BufQueueCount, "CMBufferQueueGetBufferCount");
    FN(VideoFmtCreate, "CMVideoFormatDescriptionCreate");
    FN(VideoFmtForImage, "CMVideoFormatDescriptionCreateForImageBuffer");
    FN(SampleForImage, "CMSampleBufferCreateForImageBuffer");
    FN(PoolCreate, "CVPixelBufferPoolCreate");
    FN(PoolCreatePB, "CVPixelBufferPoolCreatePixelBuffer");
    FN(PoolRelease, "CVPixelBufferPoolRelease");
    FN(Lock, "CVPixelBufferLockBaseAddress");
    FN(Unlock, "CVPixelBufferUnlockBaseAddress");
    FN(BaseAddress, "CVPixelBufferGetBaseAddress");
    FN(BPR, "CVPixelBufferGetBytesPerRow");
    FN(mach_absolute_time, "mach_absolute_time");
    FN(sysctl, "sysctl");
    FN(sysctlnametomib, "sysctlnametomib");
    KEY(kDevStreamArray, "kFigCaptureDeviceProperty_StreamArray");
    KEY(kDevClock, "kFigCaptureDeviceProperty_Clock");
    KEY(kSupportedProps, "kFigCaptureProperty_SupportedPropertiesDictionary");
    KEYS(kPortType, "kFigCaptureStreamProperty_PortType", "PortType");
    KEYS(kPortCamera, "kFigCapturePortType_Camera", "PortTypeBack");
    KEYS(kPortFront, "kFigCapturePortType_FrontFacingCamera", "PortTypeFront");
    KEY(kBufferQueue, "kFigCaptureStreamProperty_BufferQueue");
    KEY(kStillQueue, "kFigCaptureStreamProperty_StillImageBufferQueue");
    KEY(kBufferAllocator, "kFigCaptureStreamProperty_BufferAllocator");
    KEY(kSupportedFormats, "kFigCaptureStreamProperty_SupportedFormatsArray");
    KEY(kFormatDescription, "kFigCaptureStreamProperty_FormatDescription");
    KEY(kFormatIndex, "kFigCaptureStreamProperty_FormatIndex");
    KEY(kMinFps, "kFigCaptureStreamProperty_MinimumFrameRate");
    KEY(kMaxFps, "kFigCaptureStreamProperty_MaximumFrameRate");
    KEY(kStillNow, "kFigCaptureStreamProperty_StillImageCaptureNow");
    KEY(kSFFormatDescription, "kFigSupportedFormat_FormatDescription");
    KEY(kSFMaxFps, "kFigSupportedFormat_VideoMaxFrameRate");
    KEY(kSFScale, "kFigSupportedFormat_VideoScaleFactor");
    KEY(kSFBinned, "kFigSupportedFormat_VideoIsBinned");
    KEY(kNoteStarted, "kFigCaptureStreamNotification_StreamStarted");
    KEY(kNoteStopped, "kFigCaptureStreamNotification_StreamStopped");
    KEYS(kNoteDropped, "kFigCaptureStreamNotification_FrameDroppedFromQueue", "FrameDroppedFromQueue");
    KEY(kPTypeKey, "kFigCapturePropertyTypeKey");
    KEY(kPRWKey, "kFigCapturePropertyReadWriteStatusKey");
    KEY(kPSerKey, "kFigCapturePropertyShouldBeSerializedKey");
    KEY(kTypeNumber, "kFigCapturePropertyType_Number");
    KEY(kTypeString, "kFigCapturePropertyType_String");
    KEY(kTypeBool, "kFigCapturePropertyType_Boolean");
    KEY(kTypeBufferQueue, "kFigCapturePropertyType_BufferQueue");
    KEY(kTypeFormatDescription, "kFigCapturePropertyType_FormatDescription");
    KEY(kTypeCollection, "kFigCapturePropertyType_Collection");
    KEY(kTypeClock, "kFigCapturePropertyType_Clock");
    KEY(kTypeAllocator, "kFigCapturePropertyType_Allocator");
    KEY(kRO, "kFigCapturePropertyReadWriteStatus_ReadOnly");
    KEY(kRW, "kFigCapturePropertyReadWriteStatus_ReadWrite");
    KEY(kWO, "kFigCapturePropertyReadWriteStatus_WriteOnly");
    KEYS(kScaledPreview, "kFigCaptureSampleBufferAttachmentKey_ScaledPreviewPixelBuffer", "ScaledPreviewPixelBuffer");
    FN(SampleAttachments, "CMSampleBufferGetSampleAttachmentsArray");
    KEY(kPBFormat, "kCVPixelBufferPixelFormatTypeKey");
    KEY(kPBWidth, "kCVPixelBufferWidthKey");
    KEY(kPBHeight, "kCVPixelBufferHeightKey");
    KEY(kPBIOSurface, "kCVPixelBufferIOSurfacePropertiesKey");
    KEY(kPoolMin, "kCVPixelBufferPoolMinimumBufferCountKey");
    KEY(kIOSurfaceGlobal, "kIOSurfaceIsGlobal");
    ok = !bad;
    return ok;
}

/* ---- the frame source ------------------------------------------------------ */
static int pattern_mode;            /* draw the test pattern here instead of asking the kernel */

static int cam_call(cam_req_t *r)
{
    static int mib[4];
    static size_t n;
    if (!n) {
        size_t m = 3;
        if (F.sysctlnametomib(CAM_SYSCTL_NAME, mib, &m) != 0) return r->status = CAM_ERR_NO_SERVICE;
        n = m;
    }
    mib[n] = (int)(intptr_t)r;
    if (F.sysctl(mib, (unsigned)n + 1, NULL, NULL, NULL, 0) != 0) return r->status = CAM_ERR_NO_SERVICE;
    return (int)r->status;
}

/* ---- objects ---------------------------------------------------------------- */
typedef struct {
    FigObj self;
    CFMutableArrayRef streams;
    CFTypeRef clock;
    CFMutableDictionaryRef props;
    int invalidated;
} Dev;

typedef struct {
    FigObj self, dev;
    uint32_t cam;
    CFTypeRef bufq, stillq, allocator, fmt, pool, still_pool;
    CFArrayRef formats;
    CFMutableDictionaryRef props;
    int32_t min_fps, max_fps;
    volatile int running, still_pending;
    pthread_t thread;
    uint32_t frames, drops, stills;
    int invalidated;
} Stream;

static Dev *devs(FigObj o) { return (Dev *)F.GetDerivedStorage(o); }
static Stream *st(FigObj o) { return (Stream *)F.GetDerivedStorage(o); }

static const char *kname(CFStringRef k, char *b, int n)
{
    if (!k || !CFStringGetCString(k, b, n, kCFStringEncodingUTF8)) snprintf(b, (size_t)n, "?");
    return b;
}

static CFNumberRef num(int32_t v) { return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &v); }

static int32_t numval(CFTypeRef v, int32_t dflt)
{
    int32_t r = dflt;
    double d;
    if (!v) return dflt;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) return v == (CFTypeRef)kCFBooleanTrue;
    if (CFGetTypeID(v) != CFNumberGetTypeID()) return dflt;
    if (!CFNumberGetValue(v, kCFNumberSInt32Type, &r) && CFNumberGetValue(v, kCFNumberFloat64Type, &d))
        r = (int32_t)d;
    return r;
}

static void add_prop(CFMutableDictionaryRef d, CFStringRef key, CFStringRef type, CFStringRef rw)
{
    CFMutableDictionaryRef e = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    if (!e || !key) return;
    CFDictionarySetValue(e, F.kPTypeKey, type);
    CFDictionarySetValue(e, F.kPRWKey, rw);
    CFDictionarySetValue(e, F.kPSerKey, kCFBooleanFalse);
    CFDictionarySetValue(d, key, e);
    CFRelease(e);
}

/* ---- stream ------------------------------------------------------------------ */
static void post(FigObj stream, CFStringRef name)
{
    CFTypeRef c = F.DeviceGetNotificationCenter();
    if (c && name) F.PostNotification(c, name, stream, NULL, 0);
}

static CFTypeRef make_pool(uint32_t width, uint32_t height)
{
    CFTypeRef pool = NULL;
    CFMutableDictionaryRef pa = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef ba = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef ios = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks);
    CFNumberRef f = num((int32_t)PIX_BGRA), w = num((int32_t)width), h = num((int32_t)height), m = num(2);
    CFDictionarySetValue(pa, F.kPoolMin, m);
    CFDictionarySetValue(ba, F.kPBFormat, f);
    CFDictionarySetValue(ba, F.kPBWidth, w);
    CFDictionarySetValue(ba, F.kPBHeight, h);
    /* frames cross into the client app as IOSurfaces (Apple's plug-in does the same) */
    CFDictionarySetValue(ios, F.kIOSurfaceGlobal, kCFBooleanTrue);
    CFDictionarySetValue(ba, F.kPBIOSurface, ios);
    if (F.PoolCreate(kCFAllocatorDefault, pa, ba, &pool) != 0) pool = NULL;
    CFRelease(f); CFRelease(w); CFRelease(h); CFRelease(m);
    CFRelease(pa); CFRelease(ba); CFRelease(ios);
    return pool;
}

/* A still goes out at twice the frame size, interpolated. Celestial makes the
   photo's review image (512x384 BGRA, figrecorder_createPreviewImageSurfaceFromSbuf)
   with VTPixelTransferSession, which on this device refuses to scale up
   (-12905): from a 320x240 still no review image is made, Recorder_PhotoPreviewImageReady
   never comes and Camera.app never saves the photo (docs/camera.md section 4). */
static void still(Stream *s, CFTypeRef src, const CMSampleTimingInfo *t)
{
    CFTypeRef pb = NULL, fd = NULL, sb = NULL;
    OSStatus e = 0;
    if (!s->still_pool) s->still_pool = make_pool(CAM_W * 2, CAM_H * 2);
    if (!s->still_pool || F.PoolCreatePB(kCFAllocatorDefault, s->still_pool, &pb) != 0 || !pb) {
        lg("stream %u: no still buffer", s->cam);
        return;
    }
    F.Lock(pb, 0);
    F.Lock(src, 0);
    cam_bgra_upscale2x(F.BaseAddress(src), (uint32_t)F.BPR(src), CAM_W, CAM_H, F.BaseAddress(pb), (uint32_t)F.BPR(pb));
    F.Unlock(src, 0);
    F.Unlock(pb, 0);
    if ((e = F.VideoFmtForImage(kCFAllocatorDefault, pb, &fd)) == 0 &&
        (e = F.SampleForImage(kCFAllocatorDefault, pb, 1, NULL, NULL, fd, t, &sb)) == 0 && sb) {
        CFArrayRef att = F.SampleAttachments(sb, 1);
        if (att && CFArrayGetCount(att) > 0)
            CFDictionarySetValue((CFMutableDictionaryRef)CFArrayGetValueAtIndex(att, 0), F.kScaledPreview, src);
        else
            lg("stream %u: still without attachments: no review image, Camera.app may drop the photo", s->cam);
        F.BufQueueEnqueue(s->stillq, sb);
        s->stills++;
        lg("stream %u: still image #%u (frame %u, %ux%u)", s->cam, s->stills, s->frames, CAM_W * 2, CAM_H * 2);
    }
    else
        lg("stream %u: still sample buffer failed (%d)", s->cam, (int)e);
    if (sb) CFRelease(sb);
    if (fd) CFRelease(fd);
    CFRelease(pb);
}

static void *frame_thread(void *arg)
{
    FigObj so = (FigObj)arg;
    Stream *s = st(so);
    uint32_t *yuyv = malloc(CAM_FRAME_BYTES);
    cam_req_t r;
    uint32_t seq = 0, n = 0, run = 0;        /* run: delivered this session (vcd counters stay per stream forever) */
    int warned = 0;

    if (!yuyv) return NULL;

    if (!pattern_mode) {
        memset(&r, 0, sizeof r);
        r.op = CAM_OP_START;
        r.a[0] = s->cam;
        for (unsigned attempt = 0; ; attempt++) {
            cam_call(&r);
            if (r.status != CAM_ERR_NO_MEM || attempt >= 4 || !s->running) break;
            /* XNU's contiguous heap can recover as another app frees pages;
               the shim now normally avoids this by taking the GPU arena. */
            if (!attempt) lg("stream %u: start: no frame buffers (status 3), retrying", s->cam);
            usleep(200000);
        }
        lg("stream %u: start camera %s -> status %u", s->cam, s->cam == CAM_OUTER ? "outer" : "inner", r.status);
    }
    while (s->running) {
        CFTypeRef pb = NULL, fd = NULL, sb = NULL;
        CMSampleTimingInfo t;
        uint64_t ns;
        if (pattern_mode) {
            for (uint32_t y = 0; y < CAM_H; y++)
                for (uint32_t x = 0; x < CAM_W; x += 2)
                    yuyv[(y * CAM_W + x) / 2] = cam_pattern_word(s->cam, n, x, y, CAM_W, CAM_H);
            n++;
            usleep(66000);
        } else {
            memset(&r, 0, sizeof r);
            r.op = CAM_OP_FRAME;
            r.a[0] = (uint32_t)(uintptr_t)yuyv;
            r.a[1] = seq;
            if (cam_call(&r) != CAM_OK) {
                if (r.status != CAM_ERR_AGAIN && !warned) {
                    lg("stream %u: frame request failed, status %u (docs/camera.md)", s->cam, r.status);
                    warned = 1;
                }
                usleep(r.status == CAM_ERR_AGAIN ? 10000 : 200000);
                continue;
            }
            seq = r.a[1];
        }
        if (!s->pool) s->pool = make_pool(CAM_W, CAM_H);
        if (!s->pool || F.PoolCreatePB(kCFAllocatorDefault, s->pool, &pb) != 0 || !pb) {
            if (!warned) lg("stream %u: no pixel buffer", s->cam);
            warned = 1;
            continue;
        }
        F.Lock(pb, 0);
        cam_yuyv_to_bgra(yuyv, CAM_W, CAM_H, F.BaseAddress(pb), (uint32_t)F.BPR(pb));
        F.Unlock(pb, 0);
        ns = F.HostTimeToNanoseconds(F.mach_absolute_time());
        memset(&t, 0, sizeof t);
        t.pts.value = (int64_t)ns;
        t.pts.timescale = 1000000000;
        t.pts.flags = 1;                        /* kCMTimeFlags_Valid */
        t.duration.flags = t.dts.flags = 0;     /* invalid = unknown, as Apple's plug-in */
        if (F.VideoFmtForImage(kCFAllocatorDefault, pb, &fd) == 0 &&
            F.SampleForImage(kCFAllocatorDefault, pb, 1, NULL, NULL, fd, &t, &sb) == 0 && sb) {
            /* The photo's review image: Celestial takes the sample's
               ScaledPreviewPixelBuffer attachment as it is if there is one
               (figrecorder_serviceStillImageQueue 0x32e31710); else it scales
               to 512x384 with VTPixelTransferSession, which refuses to scale
               up here (-12905), and without a review image Camera.app never
               saves the photo. The frame itself serves (docs/camera.md 4). */
            CFArrayRef att = F.SampleAttachments(sb, 1);
            if (att && CFArrayGetCount(att) > 0)
                CFDictionarySetValue((CFMutableDictionaryRef)CFArrayGetValueAtIndex(att, 0), F.kScaledPreview, pb);
            if (F.BufQueueCount(s->bufq) < 4) {
                F.BufQueueEnqueue(s->bufq, sb);
            } else {
                s->drops++;
                post(so, F.kNoteDropped);
            }
            if (s->still_pending) {
                s->still_pending = 0;
                still(s, pb, &t);
            }
            run++;
            if (!s->frames++) lg("stream %u: first frame delivered", s->cam);
        }
        if (sb) CFRelease(sb);
        if (fd) CFRelease(fd);
        CFRelease(pb);
    }
    if (!pattern_mode) {
        memset(&r, 0, sizeof r);
        r.op = CAM_OP_STOP;
        cam_call(&r);
    }
    lg("stream %u: stopped after %u frames this run (%u total, %u dropped, %u stills)",
       s->cam, run, s->frames, s->drops, s->stills);
    free(yuyv);
    return NULL;
}

static OSStatus stream_start(FigObj o)
{
    Stream *s = st(o);
    if (s->invalidated) return kFigInvalidated;
    lg("stream %u: start%s", s->cam, s->running ? " (already running)" : "");
    if (!s->running) {
        s->running = 1;
        if (pthread_create(&s->thread, NULL, frame_thread, (void *)o) != 0) {
            s->running = 0;
            return kFigParamErr;
        }
    }
    post(o, F.kNoteStarted);
    return 0;
}

/* StreamStopped only for a stream that ran (Celestial stops both streams of
   a device, and Apple's plug-in posts nothing for one that was not started) */
static OSStatus stream_stop(FigObj o)
{
    Stream *s = st(o);
    if (!s->running) return 0;
    lg("stream %u: stop", s->cam);
    s->running = 0;
    pthread_join(s->thread, NULL);
    post(o, F.kNoteStopped);
    return 0;
}

static OSStatus stream_invalidate(FigObj o)
{
    Stream *s = st(o);
    if (!s->invalidated) {
        stream_stop(o);
        s->invalidated = 1;
    }
    return 0;
}

static void stream_finalize(FigObj o)
{
    Stream *s = st(o);
    stream_invalidate(o);
    if (s->bufq) CFRelease(s->bufq);
    if (s->stillq) CFRelease(s->stillq);
    if (s->allocator) CFRelease(s->allocator);
    if (s->fmt) CFRelease(s->fmt);
    if (s->formats) CFRelease(s->formats);
    if (s->props) CFRelease(s->props);
    if (s->pool) F.PoolRelease(s->pool);
    if (s->still_pool) F.PoolRelease(s->still_pool);
}

static OSStatus stream_copy(FigObj o, CFStringRef key, CFAllocatorRef a, void *out)
{
    Stream *s = st(o);
    CFTypeRef v = NULL;
    char b[96];
    (void)a;
    if (s->invalidated) return kFigInvalidated;
    if (CFEqual(key, F.kBufferQueue)) v = CFRetain(s->bufq);
    else if (CFEqual(key, F.kStillQueue)) v = CFRetain(s->stillq);
    else if (CFEqual(key, F.kFormatDescription)) v = CFRetain(s->fmt);
    else if (CFEqual(key, F.kSupportedFormats)) v = CFRetain(s->formats);
    else if (CFEqual(key, F.kFormatIndex)) v = num(0);
    else if (CFEqual(key, F.kMinFps)) v = num(s->min_fps);
    else if (CFEqual(key, F.kMaxFps)) v = num(s->max_fps);
    else if (CFEqual(key, F.kPortType)) v = CFRetain(s->cam == CAM_OUTER ? F.kPortCamera : F.kPortFront);
    else if (CFEqual(key, F.kSupportedProps)) v = CFRetain(s->props);
    else if (CFEqual(key, F.kBufferAllocator)) v = s->allocator ? CFRetain(s->allocator) : NULL;
    else {
        lg("stream %u: copy %s: not supported", s->cam, kname(key, b, sizeof b));
        return kFigPropertyNotFound;
    }
    *(CFTypeRef *)out = v;
    return 0;
}

static OSStatus stream_set(FigObj o, CFStringRef key, CFTypeRef v)
{
    Stream *s = st(o);
    char b[96];
    if (s->invalidated) return kFigInvalidated;
    if (CFEqual(key, F.kStillNow)) {
        s->still_pending = 1;
        lg("stream %u: still requested (frame %u)%s", s->cam, s->frames, s->running ? "" : " while stopped");
    } else if (CFEqual(key, F.kFormatIndex)) {
        if (numval(v, 0) != 0) lg("stream %u: format index %d (only 0 exists)", s->cam, numval(v, 0));
    } else if (CFEqual(key, F.kMinFps)) {
        s->min_fps = numval(v, s->min_fps);
    } else if (CFEqual(key, F.kMaxFps)) {
        s->max_fps = numval(v, s->max_fps);
    } else if (CFEqual(key, F.kBufferAllocator)) {
        if (s->allocator) CFRelease(s->allocator);
        s->allocator = v ? CFRetain(v) : NULL;
    } else {
        /* exposure, focus, white balance, flash, noise reduction ...: the 3DS
           sensors run their own auto exposure / white balance; accept and ignore */
        lg("stream %u: set %s ignored", s->cam, kname(key, b, sizeof b));
    }
    return 0;
}

static const FigBaseClass stream_base = {
    1, sizeof(Stream), 0, stream_invalidate, stream_finalize, 0, stream_copy, stream_set, 0,
};
static const FigStreamClass stream_class = { 1, stream_start, stream_stop };
static const FigVTable stream_vt = { 0, &stream_base, &stream_class };

static OSStatus stream_create(FigObj dev, uint32_t cam, FigObj *out)
{
    FigObj o = NULL;
    Stream *s;
    CFMutableArrayRef fa;
    CFMutableDictionaryRef f;
    CFNumberRef n;
    OSStatus e = F.DerivedObjectCreate(kCFAllocatorDefault, &stream_vt, F.StreamGetClassID(), &o);
    if (e) return e;
    s = st(o);
    memset(s, 0, sizeof *s);
    s->self = o;
    s->dev = dev;                       /* not retained: the device owns its streams */
    s->cam = cam;
    s->min_fps = 15;
    s->max_fps = 15;
    if ((e = F.BufQueueCreate(kCFAllocatorDefault, 0, F.BufQueueCallbacks(), &s->bufq)) ||
        (e = F.BufQueueCreate(kCFAllocatorDefault, 0, F.BufQueueCallbacks(), &s->stillq)) ||
        (e = F.VideoFmtCreate(kCFAllocatorDefault, PIX_BGRA, CAM_W, CAM_H, NULL, &s->fmt))) {
        CFRelease(o);
        return e;
    }
    f = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                  &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(f, F.kSFFormatDescription, s->fmt);
    n = num(15);
    CFDictionarySetValue(f, F.kSFMaxFps, n);
    CFRelease(n);
    n = num(1);
    CFDictionarySetValue(f, F.kSFScale, n);
    CFRelease(n);
    CFDictionarySetValue(f, F.kSFBinned, kCFBooleanFalse);
    fa = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    CFArrayAppendValue(fa, f);
    CFRelease(f);
    s->formats = fa;
    s->props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks);
    add_prop(s->props, F.kBufferQueue, F.kTypeBufferQueue, F.kRO);
    add_prop(s->props, F.kStillQueue, F.kTypeBufferQueue, F.kRO);
    add_prop(s->props, F.kBufferAllocator, F.kTypeAllocator, F.kRW);
    add_prop(s->props, F.kSupportedFormats, F.kTypeCollection, F.kRO);
    add_prop(s->props, F.kFormatDescription, F.kTypeFormatDescription, F.kRO);
    add_prop(s->props, F.kFormatIndex, F.kTypeNumber, F.kRW);
    add_prop(s->props, F.kMinFps, F.kTypeNumber, F.kRW);
    add_prop(s->props, F.kMaxFps, F.kTypeNumber, F.kRW);
    add_prop(s->props, F.kPortType, F.kTypeString, F.kRO);
    add_prop(s->props, F.kStillNow, F.kTypeBool, F.kWO);
    *out = o;
    return 0;
}

/* ---- device -------------------------------------------------------------------- */
static OSStatus dev_invalidate(FigObj o)
{
    Dev *d = devs(o);
    if (!d->invalidated) {
        d->invalidated = 1;
        if (d->streams) {
            CFIndex n = CFArrayGetCount(d->streams);
            for (CFIndex i = 0; i < n; i++) stream_invalidate((FigObj)CFArrayGetValueAtIndex(d->streams, i));
        }
    }
    return 0;
}

static void dev_finalize(FigObj o)
{
    Dev *d = devs(o);
    dev_invalidate(o);
    if (d->streams) CFRelease(d->streams);
    if (d->clock) CFRelease(d->clock);
    if (d->props) CFRelease(d->props);
}

static OSStatus dev_copy(FigObj o, CFStringRef key, CFAllocatorRef a, void *out)
{
    Dev *d = devs(o);
    char b[96];
    (void)a;
    if (d->invalidated) return kFigInvalidated;
    if (CFEqual(key, F.kDevStreamArray)) *(CFTypeRef *)out = CFArrayCreateCopy(kCFAllocatorDefault, d->streams);
    else if (CFEqual(key, F.kDevClock)) *(CFTypeRef *)out = CFRetain(d->clock);
    else if (CFEqual(key, F.kSupportedProps)) *(CFTypeRef *)out = CFRetain(d->props);
    else {
        lg("device: copy %s: not supported", kname(key, b, sizeof b));
        return kFigPropertyNotFound;
    }
    return 0;
}

static OSStatus dev_set(FigObj o, CFStringRef key, CFTypeRef v)
{
    char b[96];
    (void)o; (void)v;
    lg("device: set %s ignored", kname(key, b, sizeof b));
    return 0;
}

static const FigBaseClass dev_base = {
    1, sizeof(Dev), 0, dev_invalidate, dev_finalize, 0, dev_copy, dev_set, 0,
};
static const FigDeviceClass dev_class = { 1, { 0, 0, 0, 0 } };
static const FigVTable dev_vt = { 0, &dev_base, &dev_class };

__attribute__((visibility("default")))
OSStatus FigVirtualCameraDeviceCreate(CFAllocatorRef alloc, CFDictionaryRef options, FigObj *out)
{
    FigObj o = NULL, s0 = NULL, s1 = NULL;
    Dev *d;
    OSStatus e;
    cam_req_t r;
    CFStringRef dom, k;
    (void)alloc; (void)options;
    if (!out) return kFigParamErr;
    *out = NULL;
    if (!resolve()) {
        lg("device: CoreMedia symbols missing, no camera");
        return kFigParamErr;
    }
    dom = CFStringCreateWithCString(kCFAllocatorDefault, "com.apple.coremedia", kCFStringEncodingUTF8);
    k = CFStringCreateWithCString(kCFAllocatorDefault, "ios3dsPattern", kCFStringEncodingUTF8);
    pattern_mode = CFPreferencesGetAppBooleanValue(k, dom, NULL) || getenv("IOS3DS_CAM_PATTERN");
    CFRelease(k);
    CFRelease(dom);
    memset(&r, 0, sizeof r);
    r.op = CAM_OP_INFO;
    cam_call(&r);
    if (!pattern_mode && (r.status != CAM_OK || !r.a[1])) {
        lg("device: kernel camera service %s (status %u): no camera device",
           r.status == CAM_OK ? "disabled by boot.cfg" : "missing", r.status);
        return kFigParamErr;
    }
    lg("device: %s, kernel state %u, cameras present 0x%x",
       pattern_mode ? "test pattern (ios3dsPattern)" : "3DS cameras", r.a[2], r.a[5]);
    e = F.DerivedObjectCreate(kCFAllocatorDefault, &dev_vt, F.DeviceGetClassID(), &o);
    if (e) return e;
    d = devs(o);
    memset(d, 0, sizeof *d);
    d->self = o;
    d->clock = CFRetain(F.HostTimeClock());
    d->streams = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    d->props = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks);
    add_prop(d->props, F.kDevClock, F.kTypeClock, F.kRO);
    add_prop(d->props, F.kDevStreamArray, F.kTypeCollection, F.kRO);
    if ((e = stream_create(o, CAM_OUTER, &s0)) || (e = stream_create(o, CAM_INNER, &s1))) {
        lg("device: stream creation failed %d", (int)e);
        if (s0) CFRelease(s0);
        CFRelease(o);
        return e;
    }
    CFArrayAppendValue(d->streams, s0);
    CFArrayAppendValue(d->streams, s1);
    CFRelease(s0);
    CFRelease(s1);
    *out = o;
    lg("device: created (outer = PortType Camera, inner = FrontFacingCamera, %ux%u BGRA)", CAM_W, CAM_H);
    return 0;
}
