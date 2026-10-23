#include <assert.h>
#include <stdio.h>
// Keep status/config IO local to this test, not the installed driver's files.
static FILE *TestOpen(const char *path, const char *mode)
{ (void)path; (void)mode; return tmpfile(); }
static FILE *TestStatusOpen(const char *path) { (void)path; return tmpfile(); }
#define A8DJ_STATUS_OPEN TestStatusOpen
#define fopen TestOpen
#include "../plugin/usb_engine.c"
#undef fopen

static void *DelayedSetup(void *unused)
{
    (void)unused;
    // Mimic a setup that finishes only after Start's timeout initiated Stop.
    for (;;) {
        pthread_mutex_lock(&g.startMutex);
        int cancelled = g.cancelStart;
        pthread_mutex_unlock(&g.startMutex);
        if (cancelled) break;
        usleep(100);
    }
    SignalStart(-1);
    return NULL;
}
static int reads, writes;
static IOReturn FakeRead(void *self, UInt8 pipe, void *buf, UInt32 size,
                         IOAsyncCallback1 callback, void *refcon)
{
    (void)self; (void)pipe; (void)buf; (void)size; (void)callback; (void)refcon;
    reads++;
    return kIOReturnSuccess;
}
static IOReturn FakeWrite(void *self, UInt8 pipe, void *buf, UInt32 size, UInt32 a, UInt32 b)
{
    (void)self; (void)pipe; (void)buf; (void)size; (void)a; (void)b;
    writes++;
    return kIOReturnSuccess;
}
static _Atomic int senderEntered;
static void *WaitingSender(void *unused)
{
    (void)unused;
    uint8_t note[] = { 0x90, 60, 100 };
    senderEntered = 1;
    assert(A8DJ_UsbMidiSend(note, sizeof(note)) == -1);
    return NULL;
}
static void TestMidiOwnership(void)
{
    IOUSBInterfaceInterface182 table = { .ReadPipeAsync = FakeRead, .WritePipeTO = FakeWrite };
    IOUSBInterfaceInterface182 *pointer = &table;
    g.intf = &pointer;
    g.running = 1;
    g.pipeBulkIn = 1; g.pipeBulkOut = 2;
    MidiRxPost(); MidiRxPost(); MidiRxPost();
    assert(reads == 1 && g.midiPosted);
    g.midiRx[0] = EP1_CMD_MIDI_READ;
    g.midiRx[2] = 1; g.midiRx[3] = 0x7f;
    MidiRxComplete(NULL, kIOReturnSuccess, (void *)(uintptr_t)4);
    assert(reads == 2 && g.midiPosted);
    uint8_t out[8];
    assert(A8DJ_UsbMidiReceive(out, -1) == 0);
    assert(A8DJ_UsbMidiReceive(NULL, 8) == 0);
    assert(A8DJ_UsbMidiReceive(out, 8) == 1 && out[0] == 0x7f);
    unsigned received = g.midiIn;
    MidiRxComplete(NULL, kIOReturnSuccess, (void *)(uintptr_t)513);
    assert(g.midiIn == received); // oversized completion never reads beyond buffer
    uint8_t note[] = { 0x90, 60, 100 };
    assert(A8DJ_UsbMidiSend(note, 3) == 0 && writes == 1);
    assert(A8DJ_UsbMidiSend(NULL, 3) == -1);
    pthread_mutex_lock(&g.ep1Mutex);
    pthread_t sender;
    assert(pthread_create(&sender, NULL, WaitingSender, NULL) == 0);
    while (!senderEntered) usleep(100);
    // Retire the interface while a sender is waiting to acquire its mutex.
    g.running = 0;
    g.intf = NULL;
    pthread_mutex_unlock(&g.ep1Mutex);
    pthread_join(sender, NULL);
    assert(writes == 1);
    MidiRxComplete(NULL, kIOReturnAborted, NULL);
    assert(!g.midiPosted && reads == 3);
    g.buffersQuarantined = 1;
    assert(A8DJ_UsbStart(48000) == -1 && A8DJ_UsbPollDevice() == 0);
    g.buffersQuarantined = 0;
}

static int isoReads, isoWrites;
static IOReturn FakeIso(void *self, UInt8 pipe, void *buffer, UInt64 start, UInt32 frames,
                        IOUSBIsocFrame *list, IOAsyncCallback1 cb, void *refcon)
{
    (void)self; (void)pipe; (void)buffer; (void)start; (void)frames; (void)list; (void)cb;
    XferRef *ref = refcon;
    if (ref->isInput) isoReads++; else isoWrites++;
    return kIOReturnSuccess;
}
static void TestIsoOwnership(void)
{
    IOUSBInterfaceInterface182 table = { .ReadIsochPipeAsync = FakeIso, .WriteIsochPipeAsync = FakeIso };
    IOUSBInterfaceInterface182 *pointer = &table;
    g.intf = &pointer;
    g.running = 1;
    g.rate = 48000;
    g.deviceFrames = 0;
    g.lastWriteEnd = 0;
    for (int i = 0; i < NBUF; i++) {
        g.inList[i] = calloc(XFER_UFRAMES_MAX, sizeof(IOUSBIsocFrame));
        g.outList[i] = calloc(XFER_UFRAMES_MAX, sizeof(IOUSBIsocFrame));
        g.inBuf[i] = calloc(XFER_UFRAMES_MAX, ISO_MAXPKT);
        g.outBuf[i] = calloc(XFER_UFRAMES_MAX, ISO_MAXPKT);
        assert(g.inList[i] && g.outList[i] && g.inBuf[i] && g.outBuf[i]);
        g.inRef[i] = (XferRef){i, 1, 0, 0, 0}; g.outRef[i] = (XferRef){i, 0, 0, 0, 0};
    }
    assert(SubmitIn(0) == 1 && SubmitIn(0) == 0 && isoReads == 1);
    assert(SubmitOut(0) == 1 && SubmitOut(0) == 0 && isoWrites == 1);
    g.midiPosted = 1; // watchdog must leave the existing MIDI read alone
    WatchdogFired(NULL, NULL); // recover inactive slots, including an absent direction
    assert(isoReads == NBUF && isoWrites == NBUF && g.inflight == 2 * NBUF);
    WatchdogFired(NULL, NULL);
    assert(isoReads == NBUF && isoWrites == NBUF);
    // The device must not make the decoder walk past the transfer's backing memory.
    g.inList[0][0].frActCount = UINT16_MAX;
    g.inList[0][0].frStatus = kIOReturnSuccess;
    XferComplete(&g.inRef[0], kIOReturnSuccess, NULL);
    // A corrupt packet still took a microframe of device time: 6 frames at 48 kHz.
    assert(g.frErrIn == 1 && g.deviceFrames == 6 && g.inLostFrames == 6);
    assert(g.inflight == 2 * NBUF); // completed slot was requeued exactly once
    g.running = 0;
    for (int i = 0; i < NBUF; i++) {
        XferComplete(&g.inRef[i], kIOReturnAborted, NULL);
        XferComplete(&g.outRef[i], kIOReturnAborted, NULL);
        free(g.inList[i]); free(g.outList[i]); free(g.inBuf[i]); free(g.outBuf[i]);
        g.inList[i] = g.outList[i] = NULL;
        g.inBuf[i] = g.outBuf[i] = NULL;
    }
    assert(g.inflight == 0 && g.midiPosted); // MIDI still owns its buffer
    XferComplete(&g.inRef[0], kIOReturnAborted, NULL); // duplicate callback ignored
    assert(g.inflight == 0);
    MidiRxComplete(NULL, kIOReturnAborted, NULL);
    assert(!g.midiPosted);
    g.intf = NULL;
}

static _Atomic int clockDone;
static void *ClockWriter(void *unused)
{
    (void)unused;
    for (uint64_t frame = 1; frame <= 200000; frame++) PublishClock(frame, 100 + 10 * frame, 0);
    clockDone = 1;
    return NULL;
}
static void TestAudioAndClock(void)
{
    g.capture = calloc(RING_FRAMES * A8DJ_CHANNELS, sizeof(Float32));
    g.playback = calloc(RING_FRAMES * A8DJ_CHANNELS, sizeof(Float32));
    g.captureSlots = calloc(RING_FRAMES, sizeof(A8DJ_RingSlot));
    g.playbackSlots = calloc(RING_FRAMES, sizeof(A8DJ_RingSlot));
    assert(g.capture && g.playback && g.captureSlots && g.playbackSlots);
    for (size_t i = 0; i < RING_FRAMES; i++) {
        atomic_flag_clear(&g.captureSlots[i].busy);
        atomic_flag_clear(&g.playbackSlots[i].busy);
    }
    g.rate = 48000;
    g.running = 1;
    PublishClock(0, 100, 1);
    Float32 source[1][A8DJ_CHANNELS] = {{ NAN, INFINITY, -INFINITY, 1, -1, .5f, -.5f, 0 }};
    uint8_t wire[32];
    Float32 captured[1][A8DJ_CHANNELS] = {{1253632.0f / 8388607.0f, 578816.0f / 8388607.0f}};
    const uint8_t expected[32] = {
        1,3,5,7,8,0,0,0,0xd5,0,0,0,0,0,0,0,
        0,2,4,6,0x13,0,0,0,0x21,0,0,0,0,0,0,0
    };
    EncodeStream(wire, sizeof(wire), 0, captured);
    assert(!memcmp(wire, expected, sizeof(wire))); // actual production encoder vs captured bytes
    assert(A8DJ_UsbStart(NAN) == -1 && A8DJ_UsbStart(48000.5) == -1);
    assert(FrameDistance(0, UINT64_MAX) == -INT64_MAX);
    assert(FrameDistance(UINT64_MAX, 0) == INT64_MAX);
    EncodeStream(wire, sizeof(wire), 0, source);
    DecodeFrames(wire, 1, 0);
    PublishClock(1, 110, 0);
    Float32 output[A8DJ_CHANNELS];
    A8DJ_UsbReadCapture(output, 0, 1);
    assert(output[0] == 0 && output[1] == 0 && output[2] == 0);
    for (int c = 3; c < A8DJ_CHANNELS; c++) assert(fabsf(output[c] - source[0][c]) < 0.000001f);
    A8DJ_UsbWritePlayback(source[0], 50, 1);
    assert(g.lastWriteEnd == 51);
    assert(A8DJ_RingRead(g.playback + 50 * A8DJ_CHANNELS, &g.playbackSlots[50],
                         50, g.clockSeed, output, A8DJ_CHANNELS));
    assert(isnan(output[0])); // invalid samples become silence at wire encoding
    DecodeFrames(wire, 1, RING_FRAMES);
    PublishClock(RING_FRAMES + 1, 200, 0);
    A8DJ_UsbReadCapture(output, 0, 1);
    for (int c = 0; c < A8DJ_CHANNELS; c++) assert(output[c] == 0);
    g.running = 0;
    A8DJ_UsbReadCapture(output, RING_FRAMES, 1);
    for (int c = 0; c < A8DJ_CHANNELS; c++) assert(output[c] == 0);
    g.running = 1;
    PublishClock(0, 100, 1);
    A8DJ_UsbReadCapture(output, RING_FRAMES, 1);
    for (int c = 0; c < A8DJ_CHANNELS; c++) assert(output[c] == 0);
    A8DJ_UsbWritePlayback(source[0], UINT64_MAX, 1);
    assert(g.lastWriteEnd == 51);
    pthread_t writer;
    assert(pthread_create(&writer, NULL, ClockWriter, NULL) == 0);
    do {
        uint64_t frame, host, seed;
        if (A8DJ_UsbGetClock(&frame, &host, &seed)) {
            assert(host == 100 + 10 * frame);
            assert(seed == g.clockSeed + 1);
        }
    } while (!clockDone);
    pthread_join(writer, NULL);
    g.running = 0;
    free(g.capture); free(g.playback); free(g.captureSlots); free(g.playbackSlots);
    g.capture = g.playback = NULL;
    g.captureSlots = g.playbackSlots = NULL;
}

// Lost IN time must advance the clock, and a resync gap must be accounted
// before the transfer that follows it, on both directions.
static void TestLossAndGaps(void)
{
    IOUSBInterfaceInterface182 table = { .ReadIsochPipeAsync = FakeIso, .WriteIsochPipeAsync = FakeIso };
    IOUSBInterfaceInterface182 *pointer = &table;
    g.intf = &pointer;
    ResetSession();
    g.running = 1;
    g.rate = 44100;
    for (int i = 0; i < NBUF; i++) {
        g.inList[i] = calloc(XFER_UFRAMES_MAX, sizeof(IOUSBIsocFrame));
        g.outList[i] = calloc(XFER_UFRAMES_MAX, sizeof(IOUSBIsocFrame));
        g.inBuf[i] = calloc(XFER_UFRAMES_MAX, ISO_MAXPKT);
        g.outBuf[i] = calloc(XFER_UFRAMES_MAX, ISO_MAXPKT);
        g.inRef[i] = (XferRef){i, 1, 0, 0, 0}; g.outRef[i] = (XferRef){i, 0, 0, 0, 0};
    }
    // A whole failed transfer: XFER_UFRAMES microframes at 44.1 kHz, with
    // the fractional carry exact over many transfers.
    assert(SubmitIn(0) == 1);
    for (int k = 0; k < 80; k++) {                 // each completion requeues itself
        XferComplete(&g.inRef[0], kIOReturnNotResponding, NULL);
        assert(g.inRef[0].pending);
    }
    uint64_t expect = (uint64_t)(80.0 * XFER_UFRAMES * 44100.0 / 8000.0);
    assert(g.deviceFrames == expect || g.deviceFrames + 1 == expect);
    // A rearm after a 3 ms gap: IN carries it into the next completion,
    // OUT skips the frames that would have played in it.
    g.nextIn = g.nextOut = 1000;
    g.pendingInGapUframes += GapUframes(g.nextIn, 1003);
    g.pendingOutGapUframes += GapUframes(g.nextOut, 1003);
    assert(g.pendingInGapUframes == 24 && g.pendingOutGapUframes == 24);
    uint64_t before = g.deviceFrames;
    assert(SubmitIn(1) == 1 && g.inRef[1].gapUframes == 24 && g.pendingInGapUframes == 0);
    XferComplete(&g.inRef[1], kIOReturnSuccess, NULL); // all microframes empty
    assert(g.deviceFrames - before == 132 || g.deviceFrames - before == 133);
    uint64_t outBefore = g.outFrames;
    assert(SubmitOut(1) == 1 && g.pendingOutGapUframes == 0);
    assert(g.outFrames - outBefore == 132 + (uint64_t)(XFER_UFRAMES * 44100 / 8000) ||
           g.outFrames - outBefore == 133 + (uint64_t)(XFER_UFRAMES * 44100 / 8000) ||
           g.outFrames - outBefore == 132 + (uint64_t)(XFER_UFRAMES * 44100 / 8000) + 1);
    g.running = 0;
    for (int i = 0; i < NBUF; i++) {
        XferComplete(&g.inRef[i], kIOReturnAborted, NULL);
        XferComplete(&g.outRef[i], kIOReturnAborted, NULL);
        free(g.inList[i]); free(g.outList[i]); free(g.inBuf[i]); free(g.outBuf[i]);
        g.inList[i] = g.outList[i] = NULL;
        g.inBuf[i] = g.outBuf[i] = NULL;
    }
    g.intf = NULL;
    ResetSession();
}

// The clock filter must recover a straight line from jittered arrivals,
// including a device crystal 50 ppm off nominal.
static void TestClockFilter(void)
{
    ResetSession();
    g.rate = 48000;
    double tpf = TicksPerSecond() / 48000.0 * (1.0 + 50e-6);
    double ticksPerUs = TicksPerSecond() / 1e6;
    uint32_t rng = 12345;
    double worst = 0;
    for (int k = 1; k <= 5000; k++) {             // 10 s of 2 ms transfers
        uint64_t pos = (uint64_t)k * 96;
        rng = rng * 1664525u + 1013904223u;
        double jitter = ((double)(rng >> 8) / (double)(1u << 24)) * 1000.0 * ticksPerUs; // 0..1 ms late
        double truth = 1e9 + (double)pos * tpf;
        uint64_t host = ClockObserve(pos, truth + jitter);
        if (k > 2500) {
            double err = fabs((double)host - (truth + 500.0 * ticksPerUs)) / ticksPerUs;
            if (err > worst) worst = err;
        }
    }
    // Raw arrivals wander over 1000 us; the filtered line stays within a
    // small fraction of that around their mean.
    assert(worst < 150.0);
    assert(fabs(g.dllTicksPerFrame / tpf - 1.0) < 20e-6);
    // A multi-second stall restarts the filter instead of slewing.
    uint64_t pos = 5001 * 96;
    ClockObserve(pos, 1e9 + (double)pos * tpf + 5e5 * ticksPerUs);
    assert(g.clockResets == 1);
    ResetSession();
}

// Reading past the newest frame gives silence, never a held copy of it.
static void TestCaptureBeyondHave(void)
{
    g.capture = calloc(RING_FRAMES * A8DJ_CHANNELS, sizeof(Float32));
    g.captureSlots = calloc(RING_FRAMES, sizeof(A8DJ_RingSlot));
    for (size_t i = 0; i < RING_FRAMES; i++) atomic_flag_clear(&g.captureSlots[i].busy);
    g.rate = 48000;
    g.running = 1;
    PublishClock(0, 100, 1);
    uint8_t wire[32 * 4];
    Float32 src[5][A8DJ_CHANNELS];
    for (int j = 0; j < 5; j++) for (int c = 0; c < A8DJ_CHANNELS; c++) src[j][c] = .25f;
    EncodeStream(wire, sizeof(wire), 0, src);
    DecodeFrames(wire, 4, 0);
    PublishClock(4, 200, 0);
    Float32 out[8][A8DJ_CHANNELS];
    A8DJ_UsbReadCapture(&out[0][0], 0, 8);
    for (int f = 0; f < 4; f++) assert(out[f][3] == .25f);
    for (int f = 4; f < 8; f++) for (int c = 0; c < A8DJ_CHANNELS; c++) assert(out[f][c] == 0);
    g.running = 0;
    free(g.capture); free(g.captureSlots);
    g.capture = NULL; g.captureSlots = NULL;
}

// Output conversion is bit-exact for 24-bit sources and clips cleanly.
static void TestConversion(void)
{
    for (int32_t v = -8388608; v <= 8388607; v += 4099)
        assert(A8DJ_FloatToS24(A8DJ_S24ToFloat(v)) == v);
    assert(A8DJ_FloatToS24(A8DJ_S24ToFloat(8388607)) == 8388607);
    assert(A8DJ_FloatToS24(2.0f) == 8388607 && A8DJ_FloatToS24(-2.0f) == -8388608);
    assert(A8DJ_FloatToS24(NAN) == 0 && A8DJ_FloatToS24(INFINITY) == 0);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--probe") == 0) {
        int needsFirmware;
        uint64_t id = ReadyDeviceID(&needsFirmware);
        printf("Registry-only probe: ready ID=0x%llx needsFirmware=%d\n",
               (unsigned long long)id, needsFirmware);
        return 0;
    }
    g.inited = 1;
    pthread_mutex_init(&g.startMutex, NULL);
    pthread_cond_init(&g.startCond, NULL);
    pthread_mutex_init(&g.ep1Mutex, NULL);
    for (int i = 0; i < 100; i++) {
        ResetSession();
        assert(pthread_create(&g.thread, NULL, DelayedSetup, NULL) == 0);
        g.threadValid = 1;
        A8DJ_UsbStop();
        assert(g.cancelStart && g.started && !g.running && !g.threadValid);
    }
    // A stale failed thread must never report successful StartIO.
    g.threadValid = 1;
    g.prepDone = 1;
    assert(A8DJ_UsbStart(48000) != 0);
    g.threadValid = 0;
    TestMidiOwnership();
    TestIsoOwnership();
    TestAudioAndClock();
    TestLossAndGaps();
    TestClockFilter();
    TestCaptureBeyondHave();
    TestConversion();
    puts("PASS: startup cancellation, stale/quarantined buffers, MIDI ownership, audio bounds, coherent clock, IN loss/gaps, clock filter, capture past end, conversion");
}
