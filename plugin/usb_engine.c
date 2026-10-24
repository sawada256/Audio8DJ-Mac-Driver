// usb_engine.c — isochronous streaming for the Audio 8 DJ, run inside coreaudiod.
//
// Wire format:
//   32 bytes = one sample frame = two 16-byte blocks
//   block = [4 check bytes][3 byte-planes of 4 streams each]
//   stream s has its sample at [4+s], [8+s], [12+s], most significant first
//   check(s, blk)   = (s << 1) | (~blk & 1)
//   channel(s, blk) = s * 2 + (~blk & 1)

#include "usb_engine.h"
#include "midi.h"
#include "audio_ring.h"
#include "status_file.h"
#include "sample_convert.h"
#ifndef A8DJ_STATUS_OPEN
#define A8DJ_STATUS_OPEN A8DJ_OpenStatusFile
#endif
#ifndef A8DJ_LOG_OPEN
#define A8DJ_LOG_OPEN A8DJ_OpenLogFile
#endif
#define A8DJ_EVENT_LOG "/tmp/a8dj-events.log"
#include <time.h>
#include <math.h>
#include <limits.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>

#include "../firmware/audio8dj_firmware.h"

#define NI_VID          0x17CC
#define PID_RUN         0x1978
#define BCD_UPLOADED    0x0018
#define FX2_CPUCS       0xE600
#define FX2_REQ_FWLOAD  0xA0

#define EP1_BULK_OUT    0x01
#define EP1_BULK_IN     0x81
#define EP_ISO_IN       0x82
#define EP_ISO_OUT      0x06

#define EP1_CMD_GET_DEVICE_INFO  1
#define EP1_CMD_MIDI_READ        6
#define EP1_CMD_MIDI_WRITE       7
#define EP1_CMD_AUDIO_PARAMS     9
#define DEPTH_24                 2

#define WIRE_FRAME_BYTES  32
#define WIRE_STREAMS       4
#define WIRE_BLOCKS        2

#define ISO_MAXPKT     512
// The queue depth is the dominant term in output latency: the engine reads the
// playback ring NBUF transfers ahead of what the device is playing.
//
// Queue depth is configurable through LoadConfig; see docs/development.md.
#define XFER_UFRAMES_MAX 32
#define NBUF_MAX          4

static int gUframes = 16;              // microframes per transfer
static int gNbuf    = 4;               // transfers in flight per direction

#define XFER_UFRAMES    gUframes
#define XFER_FRAMES     (gUframes / 8)
#define NBUF            gNbuf

// Most sample frames that can land in one microframe, at the highest rate the
// device offers (96 kHz -> 12, plus one for the fractional carry, plus one for
// a lengthened microframe from the drift loop). SubmitOut builds a stack array
// of this many frames, so raising the advertised rates without raising this
// would overflow it -- 192 kHz alone needs 26.
#define OUT_MAX_UFRAME_FRAMES 16
#define A8DJ_MAX_RATE (8000 * (OUT_MAX_UFRAME_FRAMES - 2))

// Power of two so ring indexing is a mask. 32768 frames is ~0.68 s at 48 kHz.
#define RING_FRAMES   32768
#define RING_MASK     (RING_FRAMES - 1)

#define NCHUNKS (sizeof(audio8dj_firmware) / sizeof(audio8dj_firmware[0]))

typedef IOUSBDeviceInterface       **DevIf;
typedef IOUSBInterfaceInterface182 **IfIf;

typedef struct {
    int idx;
    int isInput;
    int pending; // owned by the USB run-loop thread
    UInt64   busFrame;    // first bus frame (1 ms) the transfer was queued for
    unsigned gapUframes;  // IN: microframes skipped just before this transfer
} XferRef;

static struct {
    DevIf   dev;
    IfIf    intf;
    UInt8   pipeIsoIn, pipeIsoOut, pipeBulkOut, pipeBulkIn;

    pthread_t     thread;
    int           threadValid;  // thread was created and not yet joined
    CFRunLoopRef  runLoop;
    _Atomic int   running;
    _Atomic int   started;      // set once the thread has finished setting up
    int           cancelStart; // protected by startMutex
    int           startResult;
    pthread_mutex_t startMutex;
    pthread_cond_t  startCond;

    pthread_t     monThread;    // publishes the status file off the RT thread
    int           monValid;

    uint8_t         midiRx[512];   // must be at least the pipe's maxPacketSize
    _Atomic int     midiPosted;
    pthread_mutex_t ep1Mutex;      // EP1 OUT is shared with AUDIO_PARAMS
    _Atomic unsigned midiIn, midiOut;
    _Atomic unsigned midiPkts, midiAcks;   // EP1 IN completions / MIDI_WRITE ACKs
    // Device -> host staging. Written by the EP1 completion on the streaming
    // thread, drained by the MIDI thread. Single producer, single consumer.
    uint8_t         midiRing[4096];
    _Atomic unsigned midiHead, midiTail;
    _Atomic unsigned midiLost;

    int           buffersQuarantined; // undrained DMA buffers must never be reused
    int           inited;       // one-time setup done
    pthread_t     prepThread;   // uploads the firmware ahead of first use
    int           prepValid;    // prepThread created and not yet joined
    _Atomic int   prepBusy;     // prepThread still running
    _Atomic int   prepDone;     // a prepare pass has completed

    _Atomic int rate;
    uint64_t uframeCursor;

    uint8_t        *inBuf[NBUF_MAX],  *outBuf[NBUF_MAX];
    IOUSBIsocFrame *inList[NBUF_MAX], *outList[NBUF_MAX];
    XferRef         inRef[NBUF_MAX],   outRef[NBUF_MAX];
    UInt64          nextIn, nextOut;
    _Atomic int     inflight;

    Float32 *capture;    // device -> host
    Float32 *playback;   // host -> device
    A8DJ_RingSlot *captureSlots, *playbackSlots;
    int buffersReady;

    _Atomic uint64_t  clockSequence; // even = coherent clock publication
    _Atomic uint64_t  clockSeed;         // bumped per session; CoreAudio uses it
                                         // to notice the timeline restarted
    _Atomic uint64_t  deviceFrames;      // frames clocked in from the device
    _Atomic uint64_t  deviceHostTime;    // mach_absolute_time at that count
    _Atomic uint64_t  outFrames;         // frames handed to the OUT pipe
    uint64_t          outBytePos;        // position in the continuous block stream

    // Instrumentation: peaks tell us whether we are reading the part of the
    // playback ring CoreAudio actually wrote.
    _Atomic float lastOutPeak, lastInPeak;
    uint32_t leadFrames;                 // how far ahead of the device we read
    _Atomic unsigned resyncs;                    // transfers rescheduled after falling behind
    _Atomic unsigned submitFails;                // transfers that could not be rescheduled
    _Atomic unsigned rearms;                     // whole-chain restarts
    _Atomic long lastDriftErr;
    double   driftAvg;                   // smoothed outFrames - deviceFrames - lead
    int      driftPrimed;
    _Atomic unsigned driftFix;                   // microframes lengthened or shortened
    _Atomic uint64_t lastWriteFrame;             // where CoreAudio last wrote
    _Atomic uint64_t lastWriteEnd;               // one past the last frame it wrote
    _Atomic uint32_t lastWriteFrames;            // size of its last write (its buffer)

    // Pop-noise accounting. A click has exactly three possible origins and
    // these separate them:
    //   xferErr/frErr/shortIn  the bus or the device did not deliver
    //   outStarve/outMargin    we sent the device ring contents CoreAudio had
    //                          not written yet, i.e. our own scheduling
    //   all zero               the glitch is analog, downstream of the data
    _Atomic unsigned xferErrIn, xferErrOut;      // completions that failed outright
    _Atomic unsigned frErrIn, frErrOut;          // microframes with a non-zero frStatus
    _Atomic unsigned shortIn;                    // microframes the device left empty
    _Atomic unsigned outStarve;                  // output read ahead of CoreAudio
    _Atomic unsigned outIdleEvents;              // times CoreAudio stopped writing output
    int      outIdle;                            // streaming thread only
    uint64_t outIdleStart;                       // frame where writes stopped
    unsigned starveRun;                          // short microframes not yet classified
    long     starveRunMin;
    _Atomic unsigned startStarve;                // same, but in the first second
    _Atomic long outMarginMin;               // closest approach, in frames
    _Atomic unsigned inStarve;                   // capture read ahead of the device
    _Atomic long inMarginMin;                // closest approach, in frames
    _Atomic unsigned inGap;                      // capture frames never filled
    _Atomic unsigned inLostFrames;               // capture frames counted as silence
    unsigned publishTick;

    // Microframes the bus skipped because a transfer had to be rescheduled.
    // The device kept converting through them, so the timelines must too.
    unsigned pendingInGapUframes, pendingOutGapUframes;
    double   inLossCarry;                        // fractional frames of lost IN time

    // Clock filter: a delay-locked loop over (frames received, bus time).
    // Run on the streaming thread only; the result goes out via PublishClock.
    int      dllPrimed;
    uint64_t dllPos;
    double   dllTime, dllTicksPerFrame, dllNominal;
    double   dllB, dllC;
    _Atomic long     clockErrMaxUs;              // worst raw-vs-filtered disagreement
    _Atomic unsigned clockResets;                // filter restarts after a jump

    // Diagnostics, to tell apart what a single cumulative counter cannot:
    // which direction fell behind, how late the callbacks were running, and
    // whether clock error is constant or only appears around those events.
    _Atomic unsigned resyncsIn, resyncsOut;
    _Atomic long     clockErrWinUs;              // max since the monitor last looked
    _Atomic int      cbLateWinMs, cbLateMaxMs;   // IN completion delay past its last bus frame
    _Atomic long     inMarginWin, outMarginWin;  // closest approach since last look
    uint64_t         sessionHost;                // mach time streaming started
    uint64_t         lastSpikeHost;              // rate-limits clock-spike events
    // Events from the streaming thread (single producer) to the monitor
    // thread (single consumer). Overwrites when full; the consumer notices.
    struct {
        uint64_t host;
        int      kind, a, b;
    } events[64];
    _Atomic unsigned evHead;
    unsigned evTail;

    char status[64];
} g;

enum { EV_RESYNC_IN = 1, EV_RESYNC_OUT, EV_REARM, EV_CLOCK_RESET, EV_CLOCK_SPIKE,
       EV_OUT_IDLE, EV_OUT_RESUME };
#define EVENT_RING 64u

// Streaming thread only. No locks, no IO.
static void PushEvent(int kind, int a, int b, uint64_t host)
{
    unsigned head = atomic_load_explicit(&g.evHead, memory_order_relaxed);
    unsigned i = head % EVENT_RING;
    g.events[i].host = host ? host : mach_absolute_time();
    g.events[i].kind = kind;
    g.events[i].a = a;
    g.events[i].b = b;
    atomic_store_explicit(&g.evHead, head + 1, memory_order_release);
}

static inline void AtomicMaxLong(_Atomic long *v, long x)
{
    long cur = atomic_load_explicit(v, memory_order_relaxed);
    while (x > cur && !atomic_compare_exchange_weak(v, &cur, x)) {}
}

static inline void AtomicMinLong(_Atomic long *v, long x)
{
    long cur = atomic_load_explicit(v, memory_order_relaxed);
    while (x < cur && !atomic_compare_exchange_weak(v, &cur, x)) {}
}

static inline void AtomicMaxInt(_Atomic int *v, int x)
{
    int cur = atomic_load_explicit(v, memory_order_relaxed);
    while (x > cur && !atomic_compare_exchange_weak(v, &cur, x)) {}
}

static void PublishClock(uint64_t frames, uint64_t hostTime, int newSession)
{
    atomic_fetch_add(&g.clockSequence, 1);
    if (newSession) atomic_fetch_add(&g.clockSeed, 1);
    atomic_store(&g.deviceFrames, frames);
    atomic_store(&g.deviceHostTime, hostTime);
    atomic_fetch_add(&g.clockSequence, 1);
}

static pthread_mutex_t gStatusMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gStatusFileMutex = PTHREAD_MUTEX_INITIALIZER;

static void SetStatus(const char *fmt, ...)
{
    char value[sizeof(g.status)] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(value, sizeof(value), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&gStatusMutex);
    memcpy(g.status, value, sizeof(value));
    pthread_mutex_unlock(&gStatusMutex);
}

const char *A8DJ_UsbStatus(void)
{
    static _Thread_local char value[sizeof(g.status)];
    pthread_mutex_lock(&gStatusMutex);
    memcpy(value, g.status, sizeof(value));
    pthread_mutex_unlock(&gStatusMutex);
    return value[0] ? value : "idle";
}

// One transfer covers XFER_UFRAMES microframes; NBUF of them are always queued,
// so the OUT read pointer runs that far ahead of what the device has played.
uint32_t A8DJ_UsbTransferFrames(double inRate)
{
    return (uint32_t)(XFER_UFRAMES * (inRate / 8000.0));
}

uint32_t A8DJ_UsbOutputLeadFrames(double inRate)
{
    return NBUF * A8DJ_UsbTransferFrames(inRate);
}

// Tuning read from the config file, in microseconds so one value holds at
// every rate. See LoadConfig.
static int gInMarginUs     = -1;   // -1: one transfer
static int gOutMarginUs    = 0;
static int gHwLatencyInUs  = 0;
static int gHwLatencyOutUs = 0;

static uint32_t UsToFrames(int us, double inRate)
{
    if (us <= 0 || !(inRate > 0)) return 0;
    return (uint32_t)llround((double)us * inRate / 1.0e6);
}

// How far from "now" CoreAudio must stay.
//
// Output: the engine reads the ring when it queues a transfer, NBUF transfers
// ahead of the bus, and that transfer spans one more. Anything less and
// CoreAudio writes frames the engine has already sent.
//
// Input: frames arrive a whole transfer at a time, so the newest transfer's
// worth is never readable yet; the margin on top covers completion latency
// and the device's uneven packet sizes.
uint32_t A8DJ_UsbSafetyOffsetFrames(double inRate, int isInput)
{
    uint32_t xfer = A8DJ_UsbTransferFrames(inRate);
    if (isInput)
        return xfer + (gInMarginUs < 0 ? xfer : UsToFrames(gInMarginUs, inRate));
    return A8DJ_UsbOutputLeadFrames(inRate) + xfer + UsToFrames(gOutMarginUs, inRate);
}

// What lies outside the published timeline: converters and the device's own
// buffering. The queue lead is already in the safety offset; reporting it
// here as well counted it twice in every app's latency compensation.
uint32_t A8DJ_UsbDeviceLatencyFrames(double inRate, int isInput)
{
    return UsToFrames(isInput ? gHwLatencyInUs : gHwLatencyOutUs, inRate);
}

static int64_t FrameDistance(uint64_t a, uint64_t b)
{
    if (a >= b) return a - b > INT64_MAX ? INT64_MAX : (int64_t)(a - b);
    return b - a > INT64_MAX ? -INT64_MAX : -(int64_t)(b - a);
}

// Per-second view, kept by the monitor thread and shown in the status file.
static _Atomic long gSecClockErrUs;
static _Atomic long gSecInMargin = 1L << 30, gSecOutMargin = 1L << 30;
static _Atomic int  gSecCbLateMs;

static void FormatMargin(char *buf, size_t cap, long v)
{
    if (v >= (1L << 30)) snprintf(buf, cap, "-");
    else snprintf(buf, cap, "%ld", v);
}

// The device name is cached by CoreAudio, so it cannot report live state.
static void PublishStatus(void)
{
    char secIn[24], secOut[24];
    FormatMargin(secIn, sizeof(secIn), gSecInMargin);
    FormatMargin(secOut, sizeof(secOut), gSecOutMargin);
    pthread_mutex_lock(&gStatusFileMutex);
    FILE *f = A8DJ_STATUS_OPEN("/tmp/a8dj-status.txt");
    if (!f) { pthread_mutex_unlock(&gStatusFileMutex); return; }
    fprintf(f,
            "%s rate=%d\n"
            "deviceFrames = %llu   (isoc IN 到着数 = 公開クロック)\n"
            "outFrames    = %llu   (OUT 読み出し位置)\n"
            "lastWrite    = %llu   (CoreAudio が最後に書いた位置)\n"
            "out-read - device = %lld フレーム\n"
            "CoreAudio書込 - out-read = %lld フレーム\n"
            "capture peak = %.4f\n"
            "playback peak(読めた値) = %.4f\n"
            "drift 補正誤差 = %ld フレーム\n"
            "resync=%u submitFail=%u rearm=%u\n"
            "USB: 転送失敗 in=%u out=%u  µframe異常 in=%u out=%u  空 in=%u  端数 in=%u\n"
            "出力枯渇=%u 回(起動時 %u)  最小余裕=%ld フレーム  クロック補正=%u 回\n"
            "出力未使用=%u 回%s (CoreAudio が出力を書いていない間は枯渇に数えない)\n"
            "録音先読み=%u 回  最小余裕=%ld フレーム  欠落を無音で補完=%u フレーム\n"
            "クロック: 最大ずれ=%ld µs  フィルタ再始動=%u 回\n"
            "resync 内訳: IN=%u OUT=%u  完了遅延最大=%d ms\n"
            "直近1秒: クロック最大ずれ=%ld µs  完了遅延最大=%d ms  録音余裕最小=%s  出力余裕最小=%s\n"
            "イベントログ: " A8DJ_EVENT_LOG "\n"
            "MIDI in=%u バイト out=%u バイト  EP1受信=%u パケット(ACK %u) 読み出し%s 取りこぼし%u\n"
            "MIDI ポート: %s\n",
            A8DJ_UsbStatus(), g.rate,
            (unsigned long long)g.deviceFrames,
            (unsigned long long)g.outFrames,
            (unsigned long long)g.lastWriteFrame,
            (long long)FrameDistance(g.outFrames, g.deviceFrames),
            (long long)FrameDistance(g.lastWriteFrame, g.outFrames),
            g.lastInPeak, g.lastOutPeak, g.lastDriftErr,
            g.resyncs, g.submitFails, g.rearms,
            g.xferErrIn, g.xferErrOut, g.frErrIn, g.frErrOut, g.shortIn, g.inGap,
            g.outStarve, g.startStarve, g.outMarginMin, g.driftFix,
            g.outIdleEvents, g.outIdle ? " (現在 未使用)" : "",
            g.inStarve, g.inMarginMin, g.inLostFrames,
            g.clockErrMaxUs, g.clockResets,
            g.resyncsIn, g.resyncsOut, g.cbLateMaxMs,
            (long)gSecClockErrUs, (int)gSecCbLateMs, secIn, secOut,
            g.midiIn, g.midiOut,
            g.midiPkts, g.midiAcks, g.midiPosted ? "あり" : "なし", g.midiLost,
            A8DJ_MidiStatus());
    fclose(f);
    pthread_mutex_unlock(&gStatusFileMutex);
}

#pragma mark - Wire helpers

static inline int     WireChannel(int stream, int blk) { return stream * 2 + (~blk & 1); }

#pragma mark - Device discovery and firmware

static CFMutableDictionaryRef DeviceMatching(void)
{
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    if (!match) return NULL;
    int vendor = NI_VID, product = PID_RUN;
    CFNumberRef vid = CFNumberCreate(NULL, kCFNumberIntType, &vendor);
    CFNumberRef pid = CFNumberCreate(NULL, kCFNumberIntType, &product);
    if (!vid || !pid) {
        if (vid) CFRelease(vid);
        if (pid) CFRelease(pid);
        CFRelease(match);
        return NULL;
    }
    CFDictionarySetValue(match, CFSTR("idVendor"), vid);
    CFDictionarySetValue(match, CFSTR("idProduct"), pid);
    CFRelease(vid); CFRelease(pid);
    return match;
}

static DevIf FindDevice(uint16_t *outPid, uint16_t *outRel)
{
    CFMutableDictionaryRef match = DeviceMatching();
    if (!match) return NULL;

    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS)
        return NULL;

    DevIf found = NULL;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter))) {
        IOCFPlugInInterface **plug = NULL;
        SInt32 score = 0;
        kern_return_t kr = IOCreatePlugInInterfaceForService(
            svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
        IOObjectRelease(svc);
        if (kr != KERN_SUCCESS || !plug) continue;

        DevIf dev = NULL;
        (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID), (LPVOID *)&dev);
        (*plug)->Release(plug);
        if (!dev) continue;

        UInt16 vid = 0, pid = 0, rel = 0;
        (*dev)->GetDeviceVendor(dev, &vid);
        (*dev)->GetDeviceProduct(dev, &pid);
        (*dev)->GetDeviceReleaseNumber(dev, &rel);
        if (vid == NI_VID && pid == PID_RUN) {
            if (outPid) *outPid = pid;
            if (outRel) *outRel = rel;
            found = dev;
            break;
        }
        (*dev)->Release(dev);
    }
    IOObjectRelease(iter);
    return found;
}

// Registry-only discovery: do not open unrelated USB devices or claim the
// audio interface while checking presence. The registry ID changes on replug.
static uint64_t ReadyDeviceID(int *needsFirmware)
{
    *needsFirmware = 0;
    CFMutableDictionaryRef match = DeviceMatching();
    if (!match) return 0;
    io_iterator_t iter = IO_OBJECT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, match, &iter) != KERN_SUCCESS)
        return 0;
    uint64_t identity = 0;
    io_service_t svc;
    while ((svc = IOIteratorNext(iter))) {
        CFTypeRef value = IORegistryEntryCreateCFProperty(svc, CFSTR("bcdDevice"), NULL, 0);
        int release = 0;
        if (value && CFGetTypeID(value) == CFNumberGetTypeID())
            CFNumberGetValue(value, kCFNumberIntType, &release);
        if (value) CFRelease(value);
        if (release == BCD_UPLOADED)
            IORegistryEntryGetRegistryEntryID(svc, &identity);
        else if (release == 0x000e)
            *needsFirmware = 1;
        IOObjectRelease(svc);
        if (identity) break;
    }
    IOObjectRelease(iter);
    return identity;
}

// Called by the HAL presence worker, serialized with Start/Stop. Firmware
// preparation stays on its own thread and is retried after every cold replug.
uint64_t A8DJ_UsbPollDevice(void)
{
    if (g.buffersQuarantined || !g.buffersReady ||
        (g.threadValid && g.started && !g.running)) return 0;
    int needsFirmware = 0;
    uint64_t identity = ReadyDeviceID(&needsFirmware);
    if (g.prepBusy) return 0;
    static CFAbsoluteTime nextPrepare = 0;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
    if (!identity && needsFirmware && !g.threadValid && now >= nextPrepare) {
        nextPrepare = now + 2.0;
        g.prepDone = 0;
        A8DJ_UsbPrepare();
    }
    return identity;
}

// The device boots from EEPROM at bcdDevice 0x000E; the bundled image reports
// 0x0018. Uploading it makes the device drop off the bus and come back.
static int UploadFirmware(DevIf dev)
{
    IOUSBDevRequest req = {
        .bmRequestType = USBmakebmRequestType(kUSBOut, kUSBVendor, kUSBDevice),
        .bRequest      = FX2_REQ_FWLOAD,
        .wIndex        = 0,
    };
    uint8_t hold = 1, go = 0;

    if ((*dev)->USBDeviceOpen(dev) != KERN_SUCCESS) return -1;

    req.wValue = FX2_CPUCS; req.wLength = 1; req.pData = &hold;
    if ((*dev)->DeviceRequest(dev, &req) != KERN_SUCCESS) goto fail;

    for (size_t i = 0; i < NCHUNKS; i++) {
        const FirmwareChunk *c = &audio8dj_firmware[i];
        if (c->address == FX2_CPUCS) continue;
        req.wValue  = c->address;
        req.wLength = c->length;
        req.pData   = (void *)c->data;
        if ((*dev)->DeviceRequest(dev, &req) != KERN_SUCCESS) goto fail;
    }

    req.wValue = FX2_CPUCS; req.wLength = 1; req.pData = &go;
    if ((*dev)->DeviceRequest(dev, &req) != KERN_SUCCESS) goto fail;

    (*dev)->USBDeviceClose(dev);
    return 0;

fail:
    (*dev)->USBDeviceClose(dev);
    return -1;
}

static UInt8 FindPipe(IfIf intf, uint8_t epAddress)
{
    UInt8 n = 0;
    (*intf)->GetNumEndpoints(intf, &n);
    const UInt8 wantNum = epAddress & 0x0F;
    const UInt8 wantDir = (epAddress & 0x80) ? kUSBIn : kUSBOut;

    for (UInt8 i = 1; i <= n; i++) {
        UInt8 dir = 0, num = 0, tt = 0, interval = 0;
        UInt16 mps = 0;
        if ((*intf)->GetPipeProperties(intf, i, &dir, &num, &tt, &mps, &interval) != KERN_SUCCESS)
            continue;
        if (num == wantNum && dir == wantDir) return i;
    }
    return 0;
}

static int Ep1Send(uint8_t cmd, const uint8_t *payload, int len)
{
    if (len < 0 || len > 63 || (len && !payload)) return -1;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    buf[0] = cmd;
    if (payload && len > 0) memcpy(buf + 1, payload, (size_t)len);
    return (*g.intf)->WritePipeTO(g.intf, g.pipeBulkOut, buf, (UInt32)(1 + len), 200, 500)
           == KERN_SUCCESS ? 0 : -1;
}

static int Ep1Recv(uint8_t *buf, UInt32 cap, UInt32 *outLen)
{
    UInt32 got = cap;
    if ((*g.intf)->ReadPipeTO(g.intf, g.pipeBulkIn, buf, &got, 200, 800) != KERN_SUCCESS)
        return -1;
    if (outLen) *outLen = got;
    return 0;
}

// EP1 IN carries two unrelated things: the reply to whatever command we last
// wrote, and MIDI the device pushes on its own. A packet left over from the
// previous session desynchronises every exchange after it -- the reply to
// GET_DEVICE_INFO is read as the reply to AUDIO_PARAMS, which is exactly the
// "audio-params-no-ack" this used to fail with once MIDI IN started working.
// So empty the pipe before talking to the device.
static void Ep1Drain(void)
{
    uint8_t scratch[512];
    for (int i = 0; i < 16; i++) {
        UInt32 got = sizeof(scratch);
        if ((*g.intf)->ReadPipeTO(g.intf, g.pipeBulkIn, scratch, &got, 20, 40) != KERN_SUCCESS)
            return;                              // timed out: nothing left
        if (got == 0) return;
    }
}

// One command and its reply, with the pipe drained first and the command byte
// checked, so a stray packet costs one retry instead of the whole session.
static int Ep1Cmd(uint8_t cmd, const uint8_t *payload, int len,
                  uint8_t *reply, UInt32 cap, UInt32 *outLen)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        Ep1Drain();
        if (Ep1Send(cmd, payload, len) != 0) continue;
        UInt32 got = 0;
        if (Ep1Recv(reply, cap, &got) != 0) continue;
        if (got >= 1 && reply[0] == cmd) {
            if (outLen) *outLen = got;
            return 0;
        }
    }
    return -1;
}

static int RateCode(int rate)
{
    // The packet builder has a fixed-size stack array; refuse anything it
    // cannot hold rather than overflow it.
    if (rate > A8DJ_MAX_RATE) return -1;
    switch (rate) {
    case 44100:  return 0;
    case 48000:  return 1;
    case 96000:  return 2;
    case 192000: return 3;
    case 88200:  return 4;
    default:     return -1;
    }
}

#pragma mark - Sample conversion

static float DecodeFrames(const uint8_t *src, int nFrames, uint64_t startFrame)
{
    float peak = 0;
    uint64_t generation = g.clockSeed;
    for (int f = 0; f < nFrames; f++) {
        const uint8_t *fr  = src + (size_t)f * WIRE_FRAME_BYTES;
        Float32 dst[A8DJ_CHANNELS];
        uint64_t frame = startFrame + (uint64_t)f;
        size_t slot = (size_t)(frame & RING_MASK);

        for (int blk = 0; blk < WIRE_BLOCKS; blk++) {
            const uint8_t *b = fr + blk * 16;
            for (int s = 0; s < WIRE_STREAMS; s++) {
                int32_t v = (b[4 + s] << 16) | (b[8 + s] << 8) | b[12 + s];
                if (v & 0x800000) v -= 0x1000000;
                float sample = A8DJ_S24ToFloat(v);
                dst[WireChannel(s, blk)] = sample;
                if (fabsf(sample) > peak) peak = fabsf(sample);
            }
        }
        A8DJ_RingWrite(g.capture + slot * A8DJ_CHANNELS, &g.captureSlots[slot],
                       frame, generation, dst, A8DJ_CHANNELS);
    }
    return peak;
}

// The Windows driver never starts a packet on a block boundary: check bytes
// land at packet offsets 8 and 24, and packet lengths are all multiples of 16
// so that phase holds for the whole stream. Generating from one continuous
// byte position reproduces that exactly.
#define OUT_START_PHASE 24

// src holds the sample frames this packet spans, in order; entry j is the frame
// the block pair at wire index j carries.
static void EncodeStream(uint8_t *dst, int nBytes, uint64_t bytePos,
                         const Float32 (*src)[A8DJ_CHANNELS])
{
    if (nBytes <= 0) return;
    uint64_t blk0 = (bytePos / 16) / 2;
    uint64_t blkN = ((bytePos + (uint64_t)nBytes - 1) / 16) / 2;

    // Convert each sample once; the byte loop below needs it three times.
    int32_t v[OUT_MAX_UFRAME_FRAMES + 1][A8DJ_CHANNELS];
    int nFrames = (int)(blkN - blk0) + 1;
    if (nFrames > OUT_MAX_UFRAME_FRAMES + 1) nFrames = OUT_MAX_UFRAME_FRAMES + 1;
    for (int j = 0; j < nFrames; j++)
        for (int c = 0; c < A8DJ_CHANNELS; c++)
            v[j][c] = A8DJ_FloatToS24(src[j][c]);

    for (int i = 0; i < nBytes; i++) {
        uint64_t gp  = bytePos + (uint64_t)i;
        uint64_t blk = gp / 16;
        int      off = (int)(gp % 16);
        int      par = (int)(~blk & 1);

        if (off < 4) { dst[i] = (uint8_t)((off << 1) | par); continue; }

        int plane = (off - 4) / 4;
        int strm  = (off - 4) % 4;
        int ch    = strm * 2 + par;
        int j     = (int)(blk / 2 - blk0);
        if (j >= nFrames) { dst[i] = 0; continue; }

        dst[i] = (uint8_t)(v[j][ch] >> (16 - 8 * plane));
    }
}

static int SamplesInUframe(uint64_t n)
{
    n %= 8000; // pattern repeats; multiplication cannot overflow on long runs
    uint64_t end = ((n + 1) * (uint64_t)g.rate) / 8000;
    uint64_t beg = (n * (uint64_t)g.rate) / 8000;
    return (int)(end - beg);
}

#pragma mark - MIDI over EP1

// EP 0x81 has to have a read outstanding at all times. The device pushes MIDI
// IN bytes whether or not anything has opened a MIDI port, and when there is
// no traffic the read simply never completes -- that is the normal state, not
// an error.
static void MidiRxComplete(void *refcon, IOReturn result, void *arg0);

// Called from the EP1 completion, i.e. the time-constraint streaming thread.
// Nothing here may block or touch the filesystem.
static void MidiRingPut(const uint8_t *bytes, int count)
{
    if (!bytes || count <= 0) return;
    unsigned head = atomic_load_explicit(&g.midiHead, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&g.midiTail, memory_order_acquire);
    unsigned free = (unsigned)sizeof(g.midiRing) - 1 - ((head - tail) & (unsigned)(sizeof(g.midiRing) - 1));
    if ((unsigned)count > free) { g.midiLost += (unsigned)count - free; count = (int)free; }
    for (int i = 0; i < count; i++)
        g.midiRing[(head + (unsigned)i) & (sizeof(g.midiRing) - 1)] = bytes[i];
    atomic_store_explicit(&g.midiHead, head + (unsigned)count, memory_order_release);
}

int A8DJ_UsbMidiReceive(uint8_t *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    unsigned head = atomic_load_explicit(&g.midiHead, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&g.midiTail, memory_order_relaxed);
    unsigned avail = (head - tail) & (unsigned)(sizeof(g.midiRing) - 1);
    if ((unsigned)cap < avail) avail = (unsigned)cap;
    for (unsigned i = 0; i < avail; i++)
        buf[i] = g.midiRing[(tail + i) & (sizeof(g.midiRing) - 1)];
    atomic_store_explicit(&g.midiTail, tail + avail, memory_order_release);
    return (int)avail;
}

static void MidiRxPost(void)
{
    if (g.midiPosted || !g.running || !g.intf || !g.pipeBulkIn) return;
    IOReturn r = (*g.intf)->ReadPipeAsync(g.intf, g.pipeBulkIn, g.midiRx,
                                          sizeof(g.midiRx), MidiRxComplete, NULL);
    g.midiPosted = (r == kIOReturnSuccess);
}

static void MidiRxComplete(void *refcon, IOReturn result, void *arg0)
{
    (void)refcon;
    size_t n = (size_t)(uintptr_t)arg0;
    g.midiPosted = 0;

    if (g.running && result == kIOReturnSuccess && n >= 3 && n <= sizeof(g.midiRx)) {
        uint8_t cmd = g.midiRx[0], len = g.midiRx[2];
        g.midiPkts++;
        if (cmd == EP1_CMD_MIDI_READ) {
            if ((size_t)(3 + len) > n) len = (uint8_t)(n - 3);
            g.midiIn += len;
            MidiRingPut(g.midiRx + 3, len);
        } else if (cmd == EP1_CMD_MIDI_WRITE) {
            g.midiAcks++;   // the ACK for a MIDI_WRITE; carries no data
        }
    }
    MidiRxPost();
    if (!g.running && g.inflight == 0 && !g.midiPosted)
        CFRunLoopStop(CFRunLoopGetCurrent());
}

int A8DJ_UsbMidiSend(const uint8_t *bytes, int count)
{
    if (!bytes || count < 1 || count > 250 || !g.inited) return -1;
    // Test readiness under the same mutex Stop uses to retire the interface.
    // A pre-lock check allowed a blocked sender to dereference a freed g.intf.
    pthread_mutex_lock(&g.ep1Mutex);
    if (!g.running || !g.intf || !g.pipeBulkOut || !g.midiPosted) {
        pthread_mutex_unlock(&g.ep1Mutex);
        return -1;
    }

    uint8_t buf[256];
    buf[0] = EP1_CMD_MIDI_WRITE;
    buf[1] = 0;
    buf[2] = (uint8_t)count;
    memcpy(buf + 3, bytes, (size_t)count);

    IOReturn r = (*g.intf)->WritePipeTO(g.intf, g.pipeBulkOut, buf,
                                        (UInt32)(3 + count), 200, 500);
    pthread_mutex_unlock(&g.ep1Mutex);
    if (r == kIOReturnSuccess) g.midiOut += (unsigned)count;
    return (r == kIOReturnSuccess) ? 0 : -1;
}

#pragma mark - Isochronous transfer chain

static void XferComplete(void *refcon, IOReturn result, void *arg0);

// Isochronous callbacks have to be serviced within the queue's slack, or the
// frame number the next transfer asked for has already gone by
// (kIOReturnIsoTooOld). Measured with a plain thread: the run loop stalls
// 8-15 ms several times a minute, more than any sane queue depth covers, and
// both directions fail on the same bus frame. A time-constraint policy -- the
// same kind CoreAudio gives its own IO threads -- removes them.
static void SetRealtimeThread(double periodMs)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ticksPerMs = 1.0e6 * (double)tb.denom / (double)tb.numer;

    thread_time_constraint_policy_data_t p;
    p.period      = (uint32_t)(periodMs * ticksPerMs);
    p.computation = (uint32_t)(periodMs * 0.25 * ticksPerMs);
    p.constraint  = (uint32_t)(periodMs * 0.60 * ticksPerMs);
    p.preemptible = 0;
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

// How far ahead of the current bus frame a resynchronised transfer is placed.
// This is the size of the gap the device sees when a callback runs late, so
// keep it small -- but far enough ahead that the retry cannot miss as well.
#define RESYNC_FRAMES 4

static UInt64 BusFrameNow(void)
{
    UInt64 f = 0;
    AbsoluteTime at;
    if (!g.intf || !(*g.intf)->GetBusFrameNumber) return 0;
    if ((*g.intf)->GetBusFrameNumber(g.intf, &f, &at) != KERN_SUCCESS) return 0;
    return f;
}

static double TicksPerSecond(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return 1.0e9 * (double)tb.denom / (double)tb.numer;
}

// Host time at which bus frame `frame` began. The bus frame counter runs off
// the host controller's clock, so this does not move when our callback is
// late -- unlike mach_absolute_time() taken inside the callback, which carried
// every bit of scheduling jitter straight into the published clock.
static double BusFrameHostTime(UInt64 frame)
{
    UInt64 f = 0;
    AbsoluteTime at;
    if (g.intf && (*g.intf)->GetBusFrameNumber &&
        (*g.intf)->GetBusFrameNumber(g.intf, &f, &at) == KERN_SUCCESS &&
        f >= frame && f - frame < 1000) {
        // How long after its last bus frame this completion ran. It is the
        // slack the queue has to absorb; a resync means it ran out.
        int late = (int)(f - frame);
        AtomicMaxInt(&g.cbLateWinMs, late);
        AtomicMaxInt(&g.cbLateMaxMs, late);
        uint64_t t = ((uint64_t)at.hi << 32) | at.lo;
        return (double)t - (double)(f - frame) * (TicksPerSecond() / 1000.0);
    }
    return (double)mach_absolute_time();
}

// Delay-locked loop (Adriaensen, "Using a DLL to filter time") over the pairs
// (frames received, time they were complete). Arrival times jitter by up to a
// microframe from the device's uneven packet sizes, and by more when the
// bus-frame read itself is late; the loop keeps the line through them, which
// is what CoreAudio extrapolates from between zero timestamps.
static uint64_t ClockObserve(uint64_t pos, double t)
{
    if (!g.dllPrimed) {
        g.dllNominal = g.rate > 0 ? TicksPerSecond() / (double)g.rate : 0;
        g.dllTicksPerFrame = g.dllNominal;
        g.dllPos = pos;
        g.dllTime = t;
        g.dllPrimed = 1;
        return (uint64_t)llround(t);
    }
    if (pos <= g.dllPos || g.dllNominal <= 0) return (uint64_t)llround(g.dllTime);

    double frames = (double)(pos - g.dllPos);
    double pred = g.dllTime + frames * g.dllTicksPerFrame;
    double e = t - pred;
    double ticksPerUs = g.dllNominal * (double)g.rate / 1.0e6;
    double errUs = fabs(e) / ticksPerUs;

    // A jump this large is not jitter: the stream stalled or was rescheduled.
    // Start over from the new observation rather than slew for seconds.
    if (errUs > 20000.0) {
        g.clockResets++;
        PushEvent(EV_CLOCK_RESET, (int)(e / ticksPerUs / 1000.0), 0, 0);
        g.dllPos = pos;
        g.dllTime = t;
        return (uint64_t)llround(t);
    }
    if (pos > (uint64_t)g.rate) {
        if (errUs > (double)g.clockErrMaxUs) g.clockErrMaxUs = (long)errUs;
        AtomicMaxLong(&g.clockErrWinUs, (long)errUs);
        // Signed: positive means the frames completed later than the line.
        uint64_t now = mach_absolute_time();
        // The bus frame read is quantized to 1 ms (measured: the microframe
        // call returns frame * 8 on this controller), so up to ~1 ms of
        // scatter is normal. Only report what goes beyond it.
        if (errUs > 1500.0 && now - g.lastSpikeHost > (uint64_t)(TicksPerSecond() / 10)) {
            g.lastSpikeHost = now;
            PushEvent(EV_CLOCK_SPIKE, (int)(e / ticksPerUs), (int)(pos % 1000000000), now);
        }
    }

    // One late reading must not drag the line: clip what the loop sees.
    double clip = 1000.0 * ticksPerUs;
    if (e >  clip) e =  clip;
    if (e < -clip) e = -clip;

    // Wide for the first second to lock quickly, then narrow.
    double bandwidth = pos < (uint64_t)g.rate ? 4.0 : 1.0;
    double w = 2.0 * M_PI * bandwidth * frames / (double)g.rate;
    g.dllTime = pred + M_SQRT2 * w * e;
    g.dllTicksPerFrame += w * w * e / frames;
    double lo = g.dllNominal * 0.999, hi = g.dllNominal * 1.001;
    if (g.dllTicksPerFrame < lo) g.dllTicksPerFrame = lo;
    if (g.dllTicksPerFrame > hi) g.dllTicksPerFrame = hi;
    g.dllPos = pos;
    return (uint64_t)llround(g.dllTime);
}

// The device kept converting through microframes we never read, so the
// timeline advances by the time they covered and the ring keeps silence there.
// Otherwise the clock falls permanently behind real time, and the drift loop
// "corrects" that by starving the device's output FIFO.
static uint64_t LostInFrames(unsigned uframes)
{
    g.inLossCarry += (double)uframes * (double)g.rate / 8000.0;
    uint64_t n = (uint64_t)g.inLossCarry;
    g.inLossCarry -= (double)n;
    g.inLostFrames += (unsigned)n;
    return n;
}

// Microframes between where a direction was scheduled to continue and where
// it actually restarts. Capped so a garbage frame number cannot run away.
static unsigned GapUframes(UInt64 planned, UInt64 restart)
{
    if (restart <= planned) return 0;
    UInt64 ms = restart - planned;
    return ms > 10000 ? 80000u : (unsigned)(ms * 8);
}

// The OUT side of the same thing: skip the frames that would have played in
// the gap, so what follows stays aligned with the device and the IN clock.
static void SkipOutUframes(unsigned uframes)
{
    while (uframes--) g.outFrames += (uint64_t)SamplesInUframe(g.uframeCursor++);
}

// Schedules one transfer, resynchronising to the current bus frame if the one
// it asked for has already gone by (kIOReturnIsoTooOld). Without the retry a
// single late callback retired that buffer permanently; once all of them had
// gone the stream was silently dead -- output went quiet and the capture ring
// stopped being refilled, so input read the same stale contents round and
// round. The old 32 ms queue hid this; a 6 ms one does not.
static int SubmitIn(int idx)
{
    if (!g.running || g.inRef[idx].pending) return 0;
    for (int f = 0; f < XFER_UFRAMES; f++) {
        g.inList[idx][f].frReqCount = ISO_MAXPKT;
        g.inList[idx][f].frActCount = 0;
        g.inList[idx][f].frStatus   = 0;
    }
    IOReturn r = (*g.intf)->ReadIsochPipeAsync(g.intf, g.pipeIsoIn, g.inBuf[idx], g.nextIn,
                                               XFER_UFRAMES, g.inList[idx],
                                               XferComplete, &g.inRef[idx]);
    if (r != kIOReturnSuccess) {
        UInt64 now = BusFrameNow();
        if (!now) { g.submitFails++; return 0; }
        UInt64 restart = now + RESYNC_FRAMES;
        unsigned gap = GapUframes(g.nextIn, restart);
        g.pendingInGapUframes += gap;
        // a: how far the bus had already passed the planned frame (ms);
        // b: the gap the device sees, in microframes.
        PushEvent(EV_RESYNC_IN, (int)((int64_t)now - (int64_t)g.nextIn), (int)gap, 0);
        g.nextIn = restart;
        g.resyncs++;
        g.resyncsIn++;
        r = (*g.intf)->ReadIsochPipeAsync(g.intf, g.pipeIsoIn, g.inBuf[idx], g.nextIn,
                                          XFER_UFRAMES, g.inList[idx],
                                          XferComplete, &g.inRef[idx]);
        if (r != kIOReturnSuccess) { g.submitFails++; return 0; }
    }
    // Completions arrive in bus order, so the gap is accounted just before
    // the first frames that came after it.
    g.inRef[idx].busFrame = g.nextIn;
    g.inRef[idx].gapUframes = g.pendingInGapUframes;
    g.pendingInGapUframes = 0;
    g.nextIn += XFER_FRAMES;
    g.inRef[idx].pending = 1;
    g.inflight++;
    return 1;
}

// How far the smoothed drift may wander before a microframe is lengthened or
// shortened by one sample frame. Small enough that the device's own FIFO never
// accumulates a whole microframe, large enough not to chase the sawtooth that
// comes from IN arriving a transfer at a time.
#define DRIFT_SLACK 2.0

// Returns -1, 0 or +1: how many sample frames to add to this transfer.
//
// The microframe pattern in SamplesInUframe() is a function of the microframe
// counter, so the output rate is locked to the host's USB SOF clock. The device
// converts on its own crystal. Measured on this hardware the two differ by
// about 46 ppm: out-read minus device grew from 192 to 247 frames over 25 s.
// The device has to take every frame we send -- isochronous has no flow control
// -- so its output FIFO fills until it sheds a whole microframe, 6 frames at
// 48 kHz, roughly every 2.6 s. That is the periodic pop, and no amount of bus
// power changes it.
//
// The IN stream is the device's own clock made visible, so steer by it: hold
// (outFrames - deviceFrames) at the lead we publish. Only the packet boundaries
// move; every sample CoreAudio wrote is still sent, in order, so the correction
// itself is inaudible.
static int DriftAdjust(void)
{
    // Wait for the input stream to be a meaningful rate reference.
    if (g.deviceFrames < (uint64_t)(g.rate / 4)) return 0;

    long err = (long)FrameDistance(g.outFrames, g.deviceFrames + g.leadFrames);
    if (!g.driftPrimed) { g.driftAvg = (double)err; g.driftPrimed = 1; }
    g.driftAvg += ((double)err - g.driftAvg) * 0.002;   // ~1 s time constant
    g.lastDriftErr = (long)g.driftAvg;

    int adjust = 0;
    if (g.driftAvg >  DRIFT_SLACK) adjust = -1;
    else if (g.driftAvg < -DRIFT_SLACK) adjust = +1;
    if (adjust) {
        // Account for the correction now, or the slow average would keep
        // asking for it long after it has been applied.
        g.driftAvg += adjust;
        g.driftFix++;
    }
    return adjust;
}

// Output starvation and "nobody is writing output" look the same from here:
// the ring is behind. IO keeps running for input-only clients, and then every
// microframe used to count as a starve (33,000 in an hour, margins of minus
// four seconds). So a run of short microframes is held back: if CoreAudio
// writes again it was a real starve and is counted; if it falls further behind
// than a writer ever would, the output is idle and the run is discarded.
static void CountOutputMargin(long margin, int ready)
{
    if (ready) {
        if (g.outIdle) {
            g.outIdle = 0;
            uint64_t idleFrames = g.outFrames - g.outIdleStart;
            PushEvent(EV_OUT_RESUME, (int)(idleFrames * 1000 / (uint64_t)g.rate), 0, 0);
        }
        if (g.starveRun) {
            g.outStarve += g.starveRun;
            if (g.starveRunMin < g.outMarginMin) g.outMarginMin = g.starveRunMin;
            AtomicMinLong(&g.outMarginWin, g.starveRunMin);
            g.starveRun = 0;
        }
        if (margin < g.outMarginMin) g.outMarginMin = margin;
        AtomicMinLong(&g.outMarginWin, margin);
        return;
    }
    if (g.outIdle) return;
    // A live writer stays ahead of us by its safety offset plus a buffer.
    // Two of its buffers plus 10 ms behind means it has stopped writing.
    long idleAfter = 2L * (long)g.lastWriteFrames + g.rate / 100;
    if (-margin > idleAfter) {
        g.outIdle = 1;
        g.outIdleEvents++;
        g.outIdleStart = g.lastWriteEnd;
        PushEvent(EV_OUT_IDLE, (int)g.starveRun, 0, 0);
        g.starveRun = 0;
        return;
    }
    if (!g.starveRun || margin < g.starveRunMin) g.starveRunMin = margin;
    g.starveRun++;
}

static int SubmitOut(int idx)
{
    if (!g.running || g.outRef[idx].pending) return 0;
    uint8_t *buf = g.outBuf[idx];
    if (g.pendingOutGapUframes) {
        SkipOutUframes(g.pendingOutGapUframes);
        g.pendingOutGapUframes = 0;
    }
    int adjust = DriftAdjust();
    // WriteIsochPipeAsync reads one *contiguous* buffer: frame f starts at the
    // sum of the preceding frReqCounts, not at a fixed stride. Laying the
    // packets out at f * ISO_MAXPKT while declaring a shorter frReqCount fed
    // the device len/ISO_MAXPKT real bytes and gaps for the rest, which is
    // where the 192/512 = 0.375 pitch ratio came from.
    size_t off = 0;
    float peak = 0;

    for (int f = 0; f < XFER_UFRAMES; f++) {
        int n = SamplesInUframe(g.uframeCursor++);
        // AUDIO_PARAMS told the device the largest packet it may expect, so a
        // lengthened microframe has to stay inside that. At 44.1 kHz only the
        // short microframes have room, hence "the first one that fits" rather
        // than always the first.
        int maxN = g.rate / 8000 + 1;
        if (adjust && n + adjust >= 1 && n + adjust <= maxN) {
            n += adjust;
            adjust = 0;                  // one frame per transfer is plenty
        }
        int bytes = n * WIRE_FRAME_BYTES;

        // Are the frames we are about to send ones CoreAudio has actually
        // written? Reading past lastWriteEnd means putting whatever the ring
        // still holds -- audio from a lap ago -- on the wire, which is a loud
        // click. Send silence instead; that is what an underrun should sound
        // like. It happens for the first few milliseconds of every session,
        // before CoreAudio's first IOProc has run.
        long margin = (long)FrameDistance(g.lastWriteEnd, g.outFrames + (uint64_t)n + 1);
        int  ready  = (g.lastWriteEnd != 0) && margin >= 0;
        if (g.lastWriteEnd) {
            // The first second is always short: we start sending before
            // CoreAudio's first IOProc has run. Counting it would bury the
            // number that matters, which is the worst case once running.
            if (g.deviceFrames > (uint64_t)g.rate) CountOutputMargin(margin, ready);
            else if (!ready) g.startStarve++;
        }

        // The packet starts 8 bytes into a block, so it straddles one more
        // block pair than it fully contains: n + 1 source frames for n steps.
        Float32 src[OUT_MAX_UFRAME_FRAMES + 1][A8DJ_CHANNELS];
        if (ready) {
            uint64_t generation = g.clockSeed;
            for (int j = 0; j <= n; j++) {
                uint64_t frame = g.outFrames + (uint64_t)j;
                size_t slot = (size_t)(frame & RING_MASK);
                A8DJ_RingRead(g.playback + slot * A8DJ_CHANNELS, &g.playbackSlots[slot],
                             frame, generation, src[j], A8DJ_CHANNELS);
            }
        } else {
            memset(src, 0, sizeof(src));
        }

        for (int j = 0; j <= n; j++)
            for (int c = 0; c < A8DJ_CHANNELS; c++)
                if (isfinite(src[j][c]) && fabsf(src[j][c]) > peak) peak = fabsf(src[j][c]);
        EncodeStream(buf + off, bytes, g.outBytePos, src);

        off            += (size_t)bytes;
        g.outBytePos   += (uint64_t)bytes;
        g.outFrames    += (uint64_t)n;
        g.outList[idx][f].frReqCount = (UInt16)bytes;
        g.outList[idx][f].frActCount = 0;
        g.outList[idx][f].frStatus   = 0;
    }
    g.lastOutPeak = peak;
    IOReturn r = (*g.intf)->WriteIsochPipeAsync(g.intf, g.pipeIsoOut, buf, g.nextOut,
                                                XFER_UFRAMES, g.outList[idx],
                                                XferComplete, &g.outRef[idx]);
    if (r != kIOReturnSuccess) {
        UInt64 now = BusFrameNow();
        if (!now) { g.submitFails++; return 0; }
        UInt64 restart = now + RESYNC_FRAMES;
        // This buffer is already built; the frames it pushed later are made
        // up by skipping ahead in the ones after it.
        unsigned gap = GapUframes(g.nextOut, restart);
        g.pendingOutGapUframes += gap;
        PushEvent(EV_RESYNC_OUT, (int)((int64_t)now - (int64_t)g.nextOut), (int)gap, 0);
        g.nextOut = restart;
        g.resyncs++;
        g.resyncsOut++;
        r = (*g.intf)->WriteIsochPipeAsync(g.intf, g.pipeIsoOut, buf, g.nextOut,
                                           XFER_UFRAMES, g.outList[idx],
                                           XferComplete, &g.outRef[idx]);
        if (r != kIOReturnSuccess) { g.submitFails++; return 0; }
    }
    g.outRef[idx].busFrame = g.nextOut;
    g.nextOut += XFER_FRAMES;
    g.outRef[idx].pending = 1;
    g.inflight++;
    return 1;
}

// Last resort: every buffer failed to go back out, so nothing will ever call
// back again. Put the whole chain back on the bus from the current frame.
static void RearmAll(void)
{
    if (!g.running) return;
    UInt64 now = BusFrameNow();
    if (!now) return;
    UInt64 restart = now + RESYNC_FRAMES;
    g.pendingInGapUframes  += GapUframes(g.nextIn, restart);
    g.pendingOutGapUframes += GapUframes(g.nextOut, restart);
    PushEvent(EV_REARM, (int)((int64_t)now - (int64_t)g.nextIn),
              (int)((int64_t)now - (int64_t)g.nextOut), 0);
    g.nextIn = g.nextOut = restart;
    g.rearms++;
    for (int i = 0; i < NBUF; i++) SubmitIn(i);
    for (int i = 0; i < NBUF; i++) SubmitOut(i);
    MidiRxPost();
}

static void XferComplete(void *refcon, IOReturn result, void *arg0)
{
    (void)arg0;
    XferRef *ref = refcon;
    if (!ref || !ref->pending) return;
    ref->pending = 0;
    g.inflight--;

    if (result != kIOReturnSuccess) {
        if (ref->isInput) g.xferErrIn++; else g.xferErrOut++;
    }

    // Per microframe status. The device reports a short or failed packet here
    // and nowhere else, so this is the only place a bus-level dropout -- the
    // signature of a power problem -- becomes visible.
    if (!ref->isInput) {
        for (int f = 0; f < XFER_UFRAMES; f++)
            if (g.outList[ref->idx][f].frStatus != kIOReturnSuccess) g.frErrOut++;
    }

    if (g.running && ref->isInput && result != kIOReturnAborted) {
        // The device's own delivery is the clock: every frame that arrives
        // advances the timeline the plug-in publishes to CoreAudio. Time the
        // bus spent on packets we lost advances it too (LostInFrames).
        uint64_t pos = g.deviceFrames;
        if (ref->gapUframes) pos += LostInFrames(ref->gapUframes);
        ref->gapUframes = 0;
        float ipk = 0;
        // A failed transfer leaves no trustworthy per-packet status; the
        // device converted a whole transfer's worth regardless.
        int whole = result != kIOReturnSuccess && result != kIOReturnUnderrun;
        if (whole) pos += LostInFrames((unsigned)XFER_UFRAMES);
        for (int f = 0; !whole && f < XFER_UFRAMES; f++) {
            UInt16 got = g.inList[ref->idx][f].frActCount;
            int    n   = got / WIRE_FRAME_BYTES;
            // We always ask for a full ISO_MAXPKT, so a packet carrying less
            // is reported as kIOReturnUnderrun. That is the normal case here,
            // not an error -- the device paces itself and sends nothing at all
            // in roughly one microframe in seven.
            IOReturn fs = g.inList[ref->idx][f].frStatus;
            if ((fs != kIOReturnSuccess && fs != kIOReturnUnderrun) || got > ISO_MAXPKT) {
                g.frErrIn++;
                pos += LostInFrames(1);
                continue;
            }
            if (n <= 0) { g.shortIn++; continue; }
            // A packet that is not a whole number of wire frames means the
            // device truncated mid-frame; the leftover bytes are lost and the
            // stream shifts, which is audible as a click.
            if (got % WIRE_FRAME_BYTES) g.inGap++;
            float peak = DecodeFrames(g.inBuf[ref->idx] + (size_t)f * ISO_MAXPKT, n, pos);
            if (peak > ipk) ipk = peak;
            pos += (uint64_t)n;
        }
        g.lastInPeak = ipk;
        // pos counts everything the device delivered by the end of this
        // transfer's last bus frame.
        uint64_t host = ClockObserve(pos, BusFrameHostTime(ref->busFrame + (UInt64)XFER_FRAMES));
        PublishClock(pos, host, 0);
    }

    if (g.running) {
        if (ref->isInput) SubmitIn(ref->idx);
        else              SubmitOut(ref->idx);
        if (g.inflight == 0) RearmAll();
    } else if (g.inflight == 0 && !g.midiPosted) {
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}

#pragma mark - Streaming thread

static void SignalStart(int result)
{
    pthread_mutex_lock(&g.startMutex);
    g.startResult = result;
    g.started     = 1;
    pthread_cond_signal(&g.startCond);
    pthread_mutex_unlock(&g.startMutex);
}

static int OpenAndConfigure(void)
{
    uint16_t pid = 0, rel = 0;
    DevIf dev = FindDevice(&pid, &rel);
    if (!dev) { SetStatus("no-device"); return -1; }

    if (rel != BCD_UPLOADED) {
        // The device was replugged and is back on its EEPROM image. Hand the
        // upload to the prepare thread and fail fast rather than blocking the
        // host for the seconds it takes to re-enumerate.
        // Only flag it; the next A8DJ_UsbStart spawns the upload. Spawning or
        // joining the prepare thread from here would race the caller's own
        // A8DJ_UsbPrepare on the CoreAudio thread.
        (*dev)->Release(dev);
        SetStatus("needs-firmware");
        g.prepDone = 0;
        return -1;
    }
    g.dev = dev;

    if ((*dev)->USBDeviceOpen(dev) != KERN_SUCCESS) { SetStatus("open-busy"); return -1; }

    UInt8 cfg = 0;
    (*dev)->GetConfiguration(dev, &cfg);
    if (cfg != 1) (*dev)->SetConfiguration(dev, 1);

    IOUSBFindInterfaceRequest req = {
        .bInterfaceClass    = kIOUSBFindInterfaceDontCare,
        .bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
        .bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
        .bAlternateSetting  = kIOUSBFindInterfaceDontCare,
    };
    io_iterator_t iter = IO_OBJECT_NULL;
    io_service_t  svc  = IO_OBJECT_NULL;
    // After a replug the device can sit unconfigured, and macOS only publishes
    // interface nubs once a configuration is set. GetConfiguration can still
    // report 1 in that state, so setting it again and waiting for the nub to
    // appear is the only reliable way through. Seen on hardware: without this
    // the device enumerates but has no IOUSBHostInterface child at all.
    for (int attempt = 0; attempt < 12 && !svc; attempt++) {
        if (attempt) {
            (*dev)->SetConfiguration(dev, 1);
            usleep(150 * 1000);
        }
        if ((*dev)->CreateInterfaceIterator(dev, &req, &iter) == KERN_SUCCESS) {
            svc = IOIteratorNext(iter);
            IOObjectRelease(iter);
        }
    }
    if (!svc) { SetStatus("no-interface"); return -1; }

    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID,
                                                         kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS || !plug) { SetStatus("no-intf-plugin"); return -1; }

    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID182),
                            (LPVOID *)&g.intf);
    (*plug)->Release(plug);
    if (!g.intf) { SetStatus("no-intf-iface"); return -1; }

    if ((*g.intf)->USBInterfaceOpen(g.intf) != KERN_SUCCESS) { SetStatus("intf-busy"); return -1; }

    // The isochronous endpoints only exist on alt setting 1.
    if ((*g.intf)->SetAlternateInterface(g.intf, 1) != KERN_SUCCESS) {
        SetStatus("alt1-failed");
        return -1;
    }

    g.pipeBulkOut = FindPipe(g.intf, EP1_BULK_OUT);
    g.pipeBulkIn  = FindPipe(g.intf, EP1_BULK_IN);
    g.pipeIsoIn   = FindPipe(g.intf, EP_ISO_IN);
    g.pipeIsoOut  = FindPipe(g.intf, EP_ISO_OUT);
    if (!g.pipeBulkOut || !g.pipeBulkIn || !g.pipeIsoIn || !g.pipeIsoOut) {
        SetStatus("pipes-missing");
        return -1;
    }

    uint8_t reply[512];
    UInt32  got = 0;
    Ep1Cmd(EP1_CMD_GET_DEVICE_INFO, NULL, 0, reply, sizeof(reply), &got);

    int rc = RateCode(g.rate);
    if (rc < 0) { SetStatus("bad-rate"); return -1; }

    // bpp is the largest packet the device should expect: one microframe's worth
    // of sample frames plus one frame of slack.
    int bpp = ((g.rate / 8000) + 1) * WIRE_FRAME_BYTES;
    uint8_t params[5] = { (uint8_t)rc, DEPTH_24,
                          (uint8_t)(bpp & 0xFF), (uint8_t)(bpp >> 8), 1 };
    got = 0;
    if (Ep1Cmd(EP1_CMD_AUDIO_PARAMS, params, sizeof(params),
               reply, sizeof(reply), &got) != 0 || got < 2) {
        SetStatus("audio-params-no-ack");
        return -1;
    }
    if (reply[1] != 1) { SetStatus("audio-params-nak-%u", reply[1]); return -1; }

    return 0;
}

// Last line of defence. If every buffer somehow failed to go back on the bus,
// no completion will ever arrive to notice it, so nothing else would restart
// the stream -- that is the state the device used to get stuck in.
static void WatchdogFired(CFRunLoopTimerRef timer, void *info)
{
    (void)timer; (void)info;
    if (!g.running && g.inflight == 0 && !g.midiPosted) {
        CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    if (g.running && g.inflight == 0) RearmAll();
    else if (g.running) {
        // A failed IN chain cannot recover by waiting for OUT to also fail.
        for (int i = 0; i < NBUF; i++) {
            if (!g.inRef[i].pending) SubmitIn(i);
            if (!g.outRef[i].pending) SubmitOut(i);
        }
    }
    // A read that fails to go back out would silently end the MIDI IN path,
    // and nothing else would ever notice: the pipe is idle by design.
    if (g.running && !g.midiPosted) MidiRxPost();
}

// The streaming thread runs under a time-constraint policy, so it must not do
// file IO. This one publishes the counters instead, once a second.
typedef struct {
    unsigned resyncIn, resyncOut, rearms, inStarve, outStarve, lost;
    unsigned frErrIn, frErrOut, xferErrIn, xferErrOut, clockResets, outIdle;
} CounterSnapshot;

static CounterSnapshot TakeSnapshot(void)
{
    return (CounterSnapshot){
        g.resyncsIn, g.resyncsOut, g.rearms, g.inStarve, g.outStarve, g.inLostFrames,
        g.frErrIn, g.frErrOut, g.xferErrIn, g.xferErrOut, g.clockResets, g.outIdleEvents };
}

static double SessionSeconds(uint64_t host)
{
    if (!g.sessionHost || host < g.sessionHost) return 0;
    return (double)(host - g.sessionHost) / TicksPerSecond();
}

// One second of diagnostics: drain the event ring into the log, then write a
// summary line if anything happened. Lines carry wall-clock time for lining
// up with what was heard, and stream time (t=) for lining up with each other.
static void MonitorTick(FILE *log, CounterSnapshot *prev)
{
    long errWin  = atomic_exchange(&g.clockErrWinUs, 0);
    int  lateWin = atomic_exchange(&g.cbLateWinMs, 0);
    long inWin   = atomic_exchange(&g.inMarginWin, 1L << 30);
    long outWin  = atomic_exchange(&g.outMarginWin, 1L << 30);
    gSecClockErrUs = errWin;
    gSecCbLateMs   = lateWin;
    gSecInMargin   = inWin;
    gSecOutMargin  = outWin;
    if (!log) { *prev = TakeSnapshot(); g.evTail = g.evHead; return; }

    char wall[16];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(wall, sizeof(wall), "%H:%M:%S", &tmv);

    unsigned head = atomic_load_explicit(&g.evHead, memory_order_acquire);
    if (head - g.evTail > EVENT_RING) {
        fprintf(log, "[%s] イベント %u 件を取りこぼし\n", wall, head - g.evTail - EVENT_RING);
        g.evTail = head - EVENT_RING;
    }
    for (; g.evTail != head; g.evTail++) {
        unsigned i = g.evTail % EVENT_RING;
        double t = SessionSeconds(g.events[i].host);
        int a = g.events[i].a, b = g.events[i].b;
        switch (g.events[i].kind) {
        case EV_RESYNC_IN:
        case EV_RESYNC_OUT:
            fprintf(log, "[%s t=%.3fs] resync %s: 予定フレームを %d ms 過ぎていた  欠落 %.3f ms\n",
                    wall, t, g.events[i].kind == EV_RESYNC_IN ? "IN " : "OUT", a, b / 8.0);
            break;
        case EV_REARM:
            fprintf(log, "[%s t=%.3fs] rearm: 全転送が停止  IN %d ms / OUT %d ms 遅れ\n", wall, t, a, b);
            break;
        case EV_CLOCK_RESET:
            fprintf(log, "[%s t=%.3fs] クロックフィルタ再始動: ずれ %+d ms\n", wall, t, a);
            break;
        case EV_OUT_IDLE:
            fprintf(log, "[%s t=%.3fs] 出力未使用: CoreAudio の書き込みが止まった (直前の不足 %d µframe は枯渇に数えない)\n",
                    wall, t, a);
            break;
        case EV_OUT_RESUME:
            fprintf(log, "[%s t=%.3fs] 出力再開: 停止 %d ms%s\n", wall, t, a,
                    a < 1000 ? "  ※短い停止は書き込み側の遅れ(実際の音切れ)の可能性" : "");
            break;
        case EV_CLOCK_SPIKE:
            fprintf(log, "[%s t=%.3fs] クロックずれ %+d µs (到着が線より%s)\n",
                    wall, t, a, a > 0 ? "遅い" : "早い");
            break;
        default:
            break;
        }
    }

    CounterSnapshot cur = TakeSnapshot();
    int changed = memcmp(&cur, prev, sizeof(cur)) != 0;
    if (changed || errWin > 500) {
        char inM[24], outM[24];
        FormatMargin(inM, sizeof(inM), inWin);
        FormatMargin(outM, sizeof(outM), outWin);
        fprintf(log, "[%s t=%.0fs] 1秒集計: resync IN+%u OUT+%u rearm+%u | 録音先読み+%u (最小余裕 %s) | "
                     "出力枯渇+%u (最小余裕 %s) | 出力未使用+%u | 欠落補完+%u | µframe異常 in+%u out+%u | "
                     "転送失敗 in+%u out+%u | フィルタ再始動+%u | クロック最大ずれ %ld µs | 完了遅延最大 %d ms\n",
                wall, SessionSeconds(mach_absolute_time()),
                cur.resyncIn - prev->resyncIn, cur.resyncOut - prev->resyncOut, cur.rearms - prev->rearms,
                cur.inStarve - prev->inStarve, inM, cur.outStarve - prev->outStarve, outM,
                cur.outIdle - prev->outIdle,
                cur.lost - prev->lost, cur.frErrIn - prev->frErrIn, cur.frErrOut - prev->frErrOut,
                cur.xferErrIn - prev->xferErrIn, cur.xferErrOut - prev->xferErrOut,
                cur.clockResets - prev->clockResets, errWin, lateWin);
    }
    *prev = cur;
    fflush(log);
}

static void *MonitorThread(void *arg)
{
    (void)arg;
    pthread_setname_np("Audio8DJ status");
    // Created from the time-constraint thread; make sure it is an ordinary one,
    // because it does file IO.
    thread_standard_policy_data_t std = { 0 };
    thread_policy_set(pthread_mach_thread_np(pthread_self()),
                      THREAD_STANDARD_POLICY, (thread_policy_t)&std,
                      THREAD_STANDARD_POLICY_COUNT);
    FILE *log = A8DJ_LOG_OPEN(A8DJ_EVENT_LOG, 1 << 20);
    if (log) {
        char wall[32];
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(wall, sizeof(wall), "%Y-%m-%d %H:%M:%S", &tmv);
        fprintf(log, "=== %s 開始 %d Hz  uframes=%d nbuf=%d  lead=%u フレーム  "
                     "セーフティ in=%u out=%u ===\n",
                wall, g.rate, gUframes, gNbuf, g.leadFrames,
                A8DJ_UsbSafetyOffsetFrames(g.rate, 1), A8DJ_UsbSafetyOffsetFrames(g.rate, 0));
        fflush(log);
    }
    CounterSnapshot prev = TakeSnapshot();
    while (g.running) {
        A8DJ_MidiInit();          // no-op once the ports exist
        PublishStatus();
        // Short naps so A8DJ_UsbStop, which joins this thread, is not held up
        // for the best part of a second inside StopIO.
        for (int i = 0; i < 20 && g.running; i++) usleep(50 * 1000);
        MonitorTick(log, &prev);
    }
    if (log) {
        fprintf(log, "=== 停止 t=%.0fs  resync IN=%u OUT=%u  クロック最大ずれ %ld µs  完了遅延最大 %d ms ===\n",
                SessionSeconds(mach_absolute_time()), g.resyncsIn, g.resyncsOut,
                g.clockErrMaxUs, g.cbLateMaxMs);
        fclose(log);
    }
    return NULL;
}

static void *StreamThread(void *arg)
{
    (void)arg;
    pthread_setname_np("Audio8DJ USB");
    if (OpenAndConfigure() != 0) { PublishStatus(); SignalStart(-1); return NULL; }

    CFRunLoopSourceRef src = NULL;
    if ((*g.intf)->CreateInterfaceAsyncEventSource(g.intf, &src) != KERN_SUCCESS || !src) {
        SetStatus("no-event-source");
        SignalStart(-1);
        return NULL;
    }
    // Retained so A8DJ_UsbStop can safely poke it even if this thread has
    // already unwound; a run loop dies with its thread otherwise.
    g.runLoop = CFRunLoopGetCurrent();
    CFRetain(g.runLoop);
    CFRunLoopAddSource(g.runLoop, src, kCFRunLoopDefaultMode);

    UInt64 frame = 0;
    AbsoluteTime at;
    if ((*g.intf)->GetBusFrameNumber(g.intf, &frame, &at) != KERN_SUCCESS) {
        SetStatus("no-bus-frame");
        CFRunLoopRemoveSource(g.runLoop, src, kCFRunLoopDefaultMode);
        CFRelease(src);
        SignalStart(-1);
        return NULL;
    }
    g.nextIn = g.nextOut = frame + 16;

    // Stop may have timed out waiting for setup. Never resurrect a cancelled
    // session after Stop cleared running, otherwise its join can wait forever.
    pthread_mutex_lock(&g.startMutex);
    if (g.cancelStart) {
        pthread_mutex_unlock(&g.startMutex);
        CFRunLoopRemoveSource(g.runLoop, src, kCFRunLoopDefaultMode);
        CFRelease(src);
        SignalStart(-1);
        return NULL;
    }
    g.running = 1;
    pthread_mutex_unlock(&g.startMutex);
    g.sessionHost    = mach_absolute_time();
    PublishClock(0, g.sessionHost, 1);
    g.outFrames      = 0;
    g.outBytePos     = OUT_START_PHASE;
    g.uframeCursor   = 0;
    g.leadFrames     = A8DJ_UsbOutputLeadFrames((double)g.rate);

    CFRunLoopTimerRef watchdog =
        CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.25, 0.25,
                             0, 0, WatchdogFired, NULL);
    if (!watchdog) {
        g.running = 0;
        SetStatus("no-watchdog");
        CFRunLoopRemoveSource(g.runLoop, src, kCFRunLoopDefaultMode);
        CFRelease(src);
        SignalStart(-1);
        return NULL;
    }
    CFRunLoopAddTimer(g.runLoop, watchdog, kCFRunLoopDefaultMode);

    for (int i = 0; i < NBUF; i++) SubmitIn(i);
    for (int i = 0; i < NBUF; i++) SubmitOut(i);

    // Only now, with the setup commands done, does the async reader take over
    // EP1 IN. It stays posted for as long as we are running.
    // The SPSC cursors live across sessions: resetting them races the MIDI
    // consumer. It drains any remaining bytes in arrival order.
    MidiRxPost();


    int haveIn = 0, haveOut = 0;
    for (int i = 0; i < NBUF; i++) {
        haveIn += g.inRef[i].pending;
        haveOut += g.outRef[i].pending;
    }
    if (!haveIn || !haveOut) {
        g.running = 0;
        SetStatus("initial-transfer-failed");
    } else SetStatus("streaming");
    if (g.running && pthread_create(&g.monThread, NULL, MonitorThread, NULL) == 0) g.monValid = 1;
    SignalStart(g.running ? 0 : -1);

    SetRealtimeThread((double)XFER_UFRAMES / 8.0);
    CFRunLoopRun();
    if (atomic_exchange(&g.running, 0)) SetStatus("usb-loop-ended");

    if (watchdog) {
        CFRunLoopRemoveTimer(g.runLoop, watchdog, kCFRunLoopDefaultMode);
        CFRelease(watchdog);
    }
    CFRunLoopRemoveSource(g.runLoop, src, kCFRunLoopDefaultMode);
    CFRelease(src);
    return NULL;
}

#pragma mark - Public API

// Prepare firmware on a worker thread so StartIO does not block for USB
// re-enumeration. Presence monitoring retries preparation on cold replug.
static void *PrepThread(void *arg)
{
    (void)arg;
    pthread_setname_np("Audio8DJ prep");

    uint16_t pid = 0, rel = 0;
    DevIf dev = FindDevice(&pid, &rel);
    if (dev) {
        if (rel == 0x000e && UploadFirmware(dev) == 0) {
            (*dev)->Release(dev);
            dev = NULL;
            for (int i = 0; i < 50 && !dev; i++) {
                usleep(200 * 1000);
                dev = FindDevice(&pid, &rel);
                if (dev && rel != BCD_UPLOADED) { (*dev)->Release(dev); dev = NULL; }
            }
        }
        if (dev) (*dev)->Release(dev);
    }
    g.prepDone = 1;
    g.prepBusy = 0;
    return NULL;
}

// Latency profile, read once when the plug-in is initialised.
//
//   /Users/Shared/audio8dj.conf          (editable without sudo)
//   /Library/Preferences/com.sawada.audio8dj.conf
//
// Contents are one key per line:
//   preset=safe | mid | low | fast
//   (edits are picked up between sessions; see A8DJ_UsbReloadConfig)
//   uframes=8|16|24|32     microframes per transfer
//   nbuf=2|3|4             transfers in flight (raised until >= 3 ms queued)
//   in_margin_us=N         input slack beyond one transfer (default: one transfer)
//   out_margin_us=N        output slack beyond lead + one transfer (default 0)
//   hw_latency_in_us=N     converter/device latency reported to apps (default 0)
//   hw_latency_out_us=N    same, output side; set from a loopback measurement
//
// Read at Initialize, and again whenever the file changes while no stream is
// running (A8DJ_UsbReloadConfig), so the latency reported to CoreAudio always
// matches what the engine does. A change made mid-stream applies once every
// app has stopped using the device; no coreaudiod restart is needed.
static void ApplyProfile(int uframes, int nbuf)
{
    if (uframes >= 8 && uframes <= XFER_UFRAMES_MAX && (uframes % 8) == 0)
        gUframes = uframes;
    if (nbuf >= 2 && nbuf <= NBUF_MAX)
        gNbuf = nbuf;
}

static int ClampUs(int us)
{
    return us < 0 ? 0 : us > 100000 ? 100000 : us;
}

static const char *const kConfigPaths[] = {
    "/Users/Shared/audio8dj.conf",
    "/Library/Preferences/com.sawada.audio8dj.conf",
};

// Identity of the config file last read, so an edit can be noticed.
typedef struct {
    int    which;          // index into kConfigPaths, -1 if none exists
    dev_t  dev;
    ino_t  ino;
    off_t  size;
    struct timespec mtime;
} ConfigStamp;
static ConfigStamp gConfigStamp = { -2, 0, 0, 0, {0, 0} };

static ConfigStamp CurrentConfigStamp(void)
{
    ConfigStamp st = { -1, 0, 0, 0, {0, 0} };
    for (size_t p = 0; p < sizeof(kConfigPaths) / sizeof(kConfigPaths[0]); p++) {
        struct stat info;
        if (stat(kConfigPaths[p], &info) != 0) continue;
        st.which = (int)p;
        st.dev = info.st_dev;
        st.ino = info.st_ino;
        st.size = info.st_size;
        st.mtime = info.st_mtimespec;
        break;
    }
    return st;
}

static int SameStamp(ConfigStamp a, ConfigStamp b)
{
    return a.which == b.which && a.dev == b.dev && a.ino == b.ino && a.size == b.size &&
           a.mtime.tv_sec == b.mtime.tv_sec && a.mtime.tv_nsec == b.mtime.tv_nsec;
}

// Presets first, then individual keys, wherever they sit in the file:
// set-latency.sh appends the preset line after any tuning keys.
static void ApplyPreset(const char *line)
{
    if (strstr(line, "preset=safe"))       ApplyProfile(16, 4);
    else if (strstr(line, "preset=mid"))   ApplyProfile(16, 3);
    else if (strstr(line, "preset=low"))   ApplyProfile(8, 4);
    // Measured on a loopback (48 kHz): 6.7 ms round trip at a 32-frame
    // buffer, no dropouts in 5 minutes. 3 ms is the shortest queue that
    // schedules at all, and 0.5 ms is the smallest input slack that held
    // (0.25 ms left one frame to spare; 0 starved).
    else if (strstr(line, "preset=fast")) { ApplyProfile(8, 3); gInMarginUs = 500; }
}

static void ApplyKey(const char *line)
{
    int v = 0;
    if (sscanf(line, " uframes = %d", &v) == 1)                ApplyProfile(v, gNbuf);
    else if (sscanf(line, " nbuf = %d", &v) == 1)              ApplyProfile(gUframes, v);
    else if (sscanf(line, " in_margin_us = %d", &v) == 1)      gInMarginUs = ClampUs(v);
    else if (sscanf(line, " out_margin_us = %d", &v) == 1)     gOutMarginUs = ClampUs(v);
    else if (sscanf(line, " hw_latency_in_us = %d", &v) == 1)  gHwLatencyInUs = ClampUs(v);
    else if (sscanf(line, " hw_latency_out_us = %d", &v) == 1) gHwLatencyOutUs = ClampUs(v);
}

static void LoadConfig(void)
{
    gUframes = 16; gNbuf = 4;
    gInMarginUs = -1; gOutMarginUs = 0;
    gHwLatencyInUs = gHwLatencyOutUs = 0;
    gConfigStamp = CurrentConfigStamp();
    if (gConfigStamp.which < 0) return;
    FILE *f = fopen(kConfigPaths[gConfigStamp.which], "r");
    if (!f) return;
    char line[128];
    for (int pass = 0; pass < 2; pass++) {
        if (fseek(f, 0, SEEK_SET) != 0) break;
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == '#') continue;
            if (pass == 0) ApplyPreset(line);
            else ApplyKey(line);
        }
    }
    fclose(f);
    // Measured on this hardware: with less than about 3 ms queued, the next
    // transfer's frame has already passed by the time the previous one
    // completes, so every single submission is rescheduled (8x2 = 2 ms:
    // 12,000 resyncs a minute). Keep the queue at or above 3 ms.
    while (gNbuf * gUframes < 24 && gNbuf < NBUF_MAX) gNbuf++;
}

// Re-read the config if it changed, but only between sessions: the engine's
// geometry and the offsets CoreAudio was told must never disagree mid-stream.
// Returns nonzero when anything CoreAudio sees (offsets, latency) changed.
// Called from the HAL presence worker, serialized with Start/Stop.
int A8DJ_UsbReloadConfig(void)
{
    if (!g.inited || g.threadValid || g.running) return 0;
    if (SameStamp(CurrentConfigStamp(), gConfigStamp)) return 0;
    int u = gUframes, n = gNbuf, im = gInMarginUs, om = gOutMarginUs;
    int hi = gHwLatencyInUs, ho = gHwLatencyOutUs;
    LoadConfig();
    return u != gUframes || n != gNbuf || im != gInMarginUs || om != gOutMarginUs ||
           hi != gHwLatencyInUs || ho != gHwLatencyOutUs;
}

void A8DJ_UsbPrepare(void)
{
    if (!g.inited) {
        LoadConfig();
        pthread_mutex_init(&g.startMutex, NULL);
        pthread_cond_init(&g.startCond, NULL);
        pthread_mutex_init(&g.ep1Mutex, NULL);
        // These outlive every streaming session. CoreAudio's IO thread can
        // still be inside ReadCapture/WritePlayback when StopIO lands, and an
        // aborted isochronous transfer can still complete after it, so freeing
        // these per session would invalidate callbacks still using them.
        g.capture  = calloc((size_t)RING_FRAMES * A8DJ_CHANNELS, sizeof(Float32));
        g.playback = calloc((size_t)RING_FRAMES * A8DJ_CHANNELS, sizeof(Float32));
        g.captureSlots = calloc(RING_FRAMES, sizeof(A8DJ_RingSlot));
        g.playbackSlots = calloc(RING_FRAMES, sizeof(A8DJ_RingSlot));
        if (g.captureSlots && g.playbackSlots) {
            for (size_t i = 0; i < RING_FRAMES; i++) {
                atomic_flag_clear(&g.captureSlots[i].busy);
                atomic_flag_clear(&g.playbackSlots[i].busy);
            }
        }
        for (int i = 0; i < NBUF_MAX; i++) {
            g.inBuf[i]   = calloc(XFER_UFRAMES_MAX, ISO_MAXPKT);
            g.outBuf[i]  = calloc(XFER_UFRAMES_MAX, ISO_MAXPKT);
            g.inList[i]  = calloc(XFER_UFRAMES_MAX, sizeof(IOUSBIsocFrame));
            g.outList[i] = calloc(XFER_UFRAMES_MAX, sizeof(IOUSBIsocFrame));
            g.inRef[i]   = (XferRef){ .idx = i, .isInput = 1 };
            g.outRef[i]  = (XferRef){ .idx = i, .isInput = 0 };
        }
        g.buffersReady = g.capture && g.playback && g.captureSlots && g.playbackSlots;
        for (int i = 0; i < NBUF_MAX; i++)
            if (!g.inBuf[i] || !g.outBuf[i] || !g.inList[i] || !g.outList[i]) g.buffersReady = 0;
        g.inited = 1;
        if (g.buffersReady) A8DJ_MidiInit();
    }
    // So the MIDI state is visible even when no audio is running.
    PublishStatus();

    if (g.prepBusy) return;
    if (g.prepValid) { pthread_join(g.prepThread, NULL); g.prepValid = 0; }
    if (g.prepDone) return;
    g.prepBusy = 1;
    if (pthread_create(&g.prepThread, NULL, PrepThread, NULL) == 0) g.prepValid = 1;
    else { g.prepBusy = 0; g.prepDone = 1; }
}

// Clears only what belongs to one streaming session; the rings, the mutexes and
// the prepare state must survive.
static void ResetSession(void)
{
    g.pipeIsoIn = g.pipeIsoOut = g.pipeBulkOut = g.pipeBulkIn = 0;
    g.runLoop        = NULL;
    g.inflight       = 0;
    g.started        = 0;
    g.startResult    = 0;
    g.cancelStart    = 0;
    g.running        = 0;
    g.nextIn         = g.nextOut = 0;
    PublishClock(0, 0, 0);
    g.outFrames      = 0;
    g.outBytePos     = 0;
    g.uframeCursor   = 0;
    g.lastOutPeak    = g.lastInPeak = 0.0f;
    g.lastDriftErr   = 0;
    g.resyncs        = 0;
    g.submitFails    = 0;
    g.rearms         = 0;
    g.lastWriteFrame = 0;
    g.lastWriteEnd   = 0;
    g.publishTick    = 0;
    g.xferErrIn      = g.xferErrOut = 0;
    g.frErrIn        = g.frErrOut = 0;
    g.shortIn        = g.inGap = 0;
    g.outStarve      = 0;
    g.outIdleEvents  = 0;
    g.outIdle        = 0;
    g.outIdleStart   = 0;
    g.starveRun      = 0;
    g.starveRunMin   = 0;
    g.lastWriteFrames = 0;
    g.startStarve    = 0;
    g.outMarginMin   = 1L << 30;
    g.inStarve       = 0;
    g.inMarginMin    = 1L << 30;
    g.driftAvg       = 0.0;
    g.driftPrimed    = 0;
    g.driftFix       = 0;
    g.inLostFrames   = 0;
    g.pendingInGapUframes = g.pendingOutGapUframes = 0;
    g.inLossCarry    = 0.0;
    g.dllPrimed      = 0;
    g.clockErrMaxUs  = 0;
    g.clockResets    = 0;
    g.resyncsIn = g.resyncsOut = 0;
    g.clockErrWinUs  = 0;
    g.cbLateWinMs = g.cbLateMaxMs = 0;
    g.inMarginWin = g.outMarginWin = 1L << 30;
    g.sessionHost = g.lastSpikeHost = 0;
    g.evHead = 0;
    g.evTail = 0;
    for (int i = 0; i < NBUF_MAX; i++) g.inRef[i].gapUframes = 0;
}

int A8DJ_UsbStart(double inRate)
{
    if (!isfinite(inRate) || inRate < 1 || inRate > A8DJ_MAX_RATE ||
        inRate != (double)(int)inRate || RateCode((int)inRate) < 0) return -1;
    if (g.buffersQuarantined) return -1;
    A8DJ_UsbPrepare();
    if (g.running) return 0;
    if (g.threadValid) return -1;

    // Normally already done at Initialize; this only bites after a replug, and
    // has to stay short so StartIO does not look hung to CoreAudio.
    if (g.prepBusy || !g.prepDone) return -1;

    ResetSession();
    g.rate = (int)inRate;
    if (!g.buffersReady) { SetStatus("no-memory"); return -1; }
    // Generation tags invalidate old audio. Clearing live ring storage here
    // raced the last HAL callback from the previous session.

    if (pthread_create(&g.thread, NULL, StreamThread, NULL) != 0) {
        SetStatus("thread-failed");
        return -1;
    }
    g.threadValid = 1;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;

    pthread_mutex_lock(&g.startMutex);
    while (!g.started)
        if (pthread_cond_timedwait(&g.startCond, &g.startMutex, &deadline) != 0) break;
    int result = g.started ? g.startResult : -1;
    pthread_mutex_unlock(&g.startMutex);

    if (result != 0) A8DJ_UsbStop();
    return result;
}

void A8DJ_UsbStop(void)
{
    if (!g.inited) return;
    pthread_mutex_lock(&g.startMutex);
    g.cancelStart = 1;
    g.running = 0;
    // Setup owns the interface until it signals completion. In the timeout
    // path, touching/closing it concurrently with OpenAndConfigure is unsafe.
    while (g.threadValid && !g.started)
        pthread_cond_wait(&g.startCond, &g.startMutex);
    pthread_mutex_unlock(&g.startMutex);

    // A sender which was already in WritePipeTO must finish before any close.
    pthread_mutex_lock(&g.ep1Mutex);
    pthread_mutex_unlock(&g.ep1Mutex);
    if (g.monValid) { pthread_join(g.monThread, NULL); g.monValid = 0; }

    if (g.threadValid) {
        // Make the queued isochronous transfers complete so the stream thread's
        // callback sees inflight reach zero and stops its own run loop. Killing
        // the run loop here instead left transfers pending in the kernel that
        // later wrote into buffers the thread had already freed -- that is what
        // took coreaudiod down after a few device switches.
        if (g.intf) {
            if (g.pipeIsoIn)  (*g.intf)->AbortPipe(g.intf, g.pipeIsoIn);
            if (g.pipeIsoOut) (*g.intf)->AbortPipe(g.intf, g.pipeIsoOut);
            if (g.pipeBulkIn) (*g.intf)->AbortPipe(g.intf, g.pipeBulkIn);
        }
        for (int i = 0; i < 200 && (g.inflight > 0 || g.midiPosted); i++) usleep(5 * 1000);
        if (g.runLoop) CFRunLoopStop(g.runLoop);
        pthread_join(g.thread, NULL);
        g.threadValid = 0;
        if (g.inflight > 0 || g.midiPosted) {
            g.buffersQuarantined = 1;
            SetStatus("undrained-usb-restart-required");
        }
        if (g.runLoop) { CFRelease(g.runLoop); g.runLoop = NULL; }
    }

    pthread_mutex_lock(&g.ep1Mutex);
    if (g.intf) {
        (*g.intf)->SetAlternateInterface(g.intf, 0);
        (*g.intf)->USBInterfaceClose(g.intf);
        (*g.intf)->Release(g.intf);
        g.intf = NULL;
    }
    if (g.dev) {
        (*g.dev)->USBDeviceClose(g.dev);
        (*g.dev)->Release(g.dev);
        g.dev = NULL;
    }
    pthread_mutex_unlock(&g.ep1Mutex);
    if (strcmp(A8DJ_UsbStatus(), "streaming") == 0) SetStatus("stopped");
    PublishStatus();
}

int A8DJ_UsbGetClock(uint64_t *outFrames, uint64_t *outHostTime, uint64_t *outSeed)
{
    if (!g.running) return 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        uint64_t sequence = atomic_load(&g.clockSequence);
        if (sequence & 1) continue;
        uint64_t frames = atomic_load(&g.deviceFrames);
        uint64_t host = atomic_load(&g.deviceHostTime);
        uint64_t seed = atomic_load(&g.clockSeed);
        if (sequence != atomic_load(&g.clockSequence)) continue;
        if (!g.running || !host) return 0;
        if (outFrames) *outFrames = frames;
        if (outHostTime) *outHostTime = host;
        if (outSeed) *outSeed = seed + 1;
        return 1;
    }
    // The caller keeps its last coherent timestamp; never publish half a pair.
    return 0;
}

void A8DJ_UsbReadCapture(Float32 *outBuf, uint64_t inStartFrame, uint32_t inFrames)
{
    if (!outBuf) return;
    if (!g.capture || !g.captureSlots || !g.running || UINT64_MAX - inStartFrame < inFrames) {
        memset(outBuf, 0, (size_t)inFrames * A8DJ_CHANNELS * sizeof(Float32));
        return;
    }
    uint64_t generation = g.clockSeed;
    uint64_t have = g.deviceFrames;
    if (have > (uint64_t)g.rate) {
        // Signed distance in both directions: a read that starts past `have`
        // is a shortfall of exactly that many frames, not "infinitely short".
        // FrameDistance saturates at +-INT64_MAX, so only the subtraction can
        // still overflow.
        int64_t ahead = FrameDistance(have, inStartFrame);
        long margin = ahead < -INT64_MAX + (int64_t)inFrames
            ? -LONG_MAX : (long)(ahead - (int64_t)inFrames);
        if (margin < g.inMarginMin) g.inMarginMin = margin;
        AtomicMinLong(&g.inMarginWin, margin);
        if (margin < 0) g.inStarve++;
    }
    // Frames past `have` have not arrived. Their slots still hold the previous
    // lap, whose tags do not match, so they read as silence -- not as a copy
    // of the newest frame, which turned a late read into a stair-step.
    for (uint32_t f = 0; f < inFrames; f++) {
        uint64_t frame = inStartFrame + f;
        size_t slot = (size_t)(frame & RING_MASK);
        A8DJ_RingRead(g.capture + slot * A8DJ_CHANNELS, &g.captureSlots[slot],
                     frame, generation, outBuf + (size_t)f * A8DJ_CHANNELS, A8DJ_CHANNELS);
    }
}

void A8DJ_UsbWritePlayback(const Float32 *inBuf, uint64_t inStartFrame, uint32_t inFrames)
{
    if (!inBuf || !g.playback || !g.playbackSlots || !g.running ||
        UINT64_MAX - inStartFrame < inFrames) return;
    uint64_t generation = g.clockSeed;
    for (uint32_t f = 0; f < inFrames; f++) {
        uint64_t frame = inStartFrame + f;
        size_t slot = (size_t)(frame & RING_MASK);
        A8DJ_RingWrite(g.playback + slot * A8DJ_CHANNELS, &g.playbackSlots[slot],
                      frame, generation, inBuf + (size_t)f * A8DJ_CHANNELS, A8DJ_CHANNELS);
    }
    // Publish the watermark only after all stores, never before filling data.
    g.lastWriteFrames = inFrames;
    g.lastWriteFrame = inStartFrame;
    g.lastWriteEnd = inStartFrame + inFrames;
}
