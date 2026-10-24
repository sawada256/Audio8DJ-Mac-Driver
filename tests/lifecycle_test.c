// Exercise the real HAL entry points with a deterministic USB backend.
// No hardware is opened and no installed driver is modified.
#include <assert.h>
#include <stdio.h>
#include "../plugin/Audio8DJ.c"

static uint64_t deviceID;
static int starts, stops, startFailure, listChanges, deadChanges;
static OSStatus requestResult;
static UInt64 requestedRate;
static int clockAvailable;
static uint64_t clockFrames, clockHost, clockSeed;
static int captures, playbacks;
static int restartAllowed;
static int timingChanges;
static void Flush(void *u) { (void)u; } // a rate change restarts USB under running clients
static UInt32 Scalar(AudioObjectID object, AudioObjectPropertySelector selector)
{
    AudioObjectPropertyAddress a = { selector, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 result = 99, size = 0;
    assert(Audio8DJ_GetPropertyData(gDriverRef, object, 0, &a, 0, NULL,
                                   sizeof(result), &size, &result) == 0);
    assert(size == sizeof(result));
    return result;
}
static OSStatus Changed(AudioServerPlugInHostRef host, AudioObjectID object,
                        UInt32 count, const AudioObjectPropertyAddress *addresses)
{
    (void)host;
    // Reentrant host queries must not deadlock on a lock held by the notifier.
    assert(pthread_mutex_trylock(&gStateMutex) == 0);
    pthread_mutex_unlock(&gStateMutex);
    if (object == kObjectID_PlugIn) {
        assert(pthread_mutex_trylock(&gIOMutex) == 0);
        pthread_mutex_unlock(&gIOMutex);
        assert(count == 2);
        listChanges++;
    } else if (addresses[0].mSelector == kAudioDevicePropertyLatency) {
        assert(count == 4 && addresses[2].mSelector == kAudioDevicePropertySafetyOffset);
        timingChanges++;
    } else if (addresses[0].mSelector == kAudioDevicePropertyDeviceIsAlive) {
        assert(Scalar(kObjectID_Device, kAudioDevicePropertyDeviceIsAlive) == 0);
        deadChanges++;
    }
    return 0;
}
static int timingRequests;
static OSStatus Request(AudioServerPlugInHostRef h, AudioObjectID o, UInt64 action, void *info)
{
    (void)h; (void)o; (void)info;
    if (action == 1) { timingRequests++; return 0; }
    requestedRate = action;
    return requestResult;
}
void A8DJ_UsbPrepare(void) {}
uint64_t A8DJ_UsbPollDevice(void) { return deviceID; }
static int configChanged, reloads;
int A8DJ_UsbReloadConfig(void) { reloads++; int c = configChanged; configChanged = 0; return c; }
int A8DJ_UsbStart(double rate) { (void)rate; starts++; return startFailure; }
void A8DJ_UsbStop(void)
{
    // USB teardown can be slow: properties/clock state must remain accessible.
    assert(pthread_mutex_trylock(&gStateMutex) == 0);
    pthread_mutex_unlock(&gStateMutex);
    if (!restartAllowed) assert(Scalar(kObjectID_Device, kAudioDevicePropertyDeviceIsRunning) == 0);
    stops++;
}
int A8DJ_UsbGetClock(uint64_t *f, uint64_t *t, uint64_t *s)
{ *f = clockFrames; *t = clockHost; *s = clockSeed; return clockAvailable; }
void A8DJ_UsbReadCapture(Float32 *b, uint64_t f, uint32_t n) { (void)b; (void)f; (void)n; captures++; }
void A8DJ_UsbWritePlayback(const Float32 *b, uint64_t f, uint32_t n) { (void)b; (void)f; (void)n; playbacks++; }
uint32_t A8DJ_UsbOutputLeadFrames(double r) { return (uint32_t)(r * .008); }
uint32_t A8DJ_UsbTransferFrames(double r) { return (uint32_t)(r * .002); }
uint32_t A8DJ_UsbSafetyOffsetFrames(double r, int in) { return in ? 2 * A8DJ_UsbTransferFrames(r)
    : A8DJ_UsbOutputLeadFrames(r) + A8DJ_UsbTransferFrames(r); }
uint32_t A8DJ_UsbDeviceLatencyFrames(double r, int in) { return (uint32_t)(r * (in ? .0005 : .001)); }
const char *A8DJ_UsbStatus(void) { return "test"; }

static void CheckPresence(Boolean present)
{
    const AudioObjectPropertySelector selectors[] = {
        kAudioPlugInPropertyDeviceList, kAudioObjectPropertyOwnedObjects
    };
    for (size_t i = 0; i < 2; i++) {
        AudioObjectPropertyAddress a = { selectors[i], kAudioObjectPropertyScopeGlobal, 0 };
        UInt32 size = 99, written = 99;
        AudioObjectID id = 99;
        assert(Audio8DJ_GetPropertyDataSize(gDriverRef, kObjectID_PlugIn, 0,
                                           &a, 0, NULL, &size) == 0);
        assert(size == (present ? sizeof(id) : 0));
        assert(Audio8DJ_GetPropertyData(gDriverRef, kObjectID_PlugIn, 0, &a,
                                       0, NULL, sizeof(id), &written, &id) == 0);
        assert(written == size);
        if (present) assert(id == kObjectID_Device);
        else {
            assert(Audio8DJ_GetPropertyData(gDriverRef, kObjectID_PlugIn, 0, &a,
                                           0, NULL, 0, &written, NULL) == 0);
            assert(written == 0);
        }
    }
    AudioObjectPropertyAddress a = { kAudioPlugInPropertyTranslateUIDToDevice,
                                     kAudioObjectPropertyScopeGlobal, 0 };
    CFStringRef uid = CFSTR(kDeviceUID);
    AudioObjectID id = 99;
    UInt32 size;
    assert(Audio8DJ_HasProperty(gDriverRef, kObjectID_PlugIn, 0, &a));
    assert(Audio8DJ_GetPropertyData(gDriverRef, kObjectID_PlugIn, 0, &a,
                                   sizeof(uid), &uid, sizeof(id), &size, &id) == 0);
    assert(id == (present ? kObjectID_Device : kAudioObjectUnknown));
    assert(Scalar(kObjectID_Device, kAudioDevicePropertyDeviceIsAlive) == present);
}
static void TestClockAndIO(void)
{
    gIORunningCount = 1;
    gDevicePresent = true;
    gTimestampSeed = 0;
    gHostTicksPerFrame = 1;
    clockAvailable = 1;
    clockFrames = 100; clockHost = 1000; clockSeed = 5;
    Float64 sample; UInt64 host, seed;
    assert(Audio8DJ_GetZeroTimeStamp(gDriverRef, kObjectID_Device, 0, &sample, &host, &seed) == 0);
    assert(sample == 0 && host == 900 && seed == 5);
    clockFrames = 900; clockHost = 1750;
    assert(Audio8DJ_GetZeroTimeStamp(gDriverRef, kObjectID_Device, 0, &sample, &host, &seed) == 0);
    assert(sample == 0 && host == 900); // completion jitter cannot move a published pair
    clockFrames = 1100; clockHost = 2200;
    Audio8DJ_GetZeroTimeStamp(gDriverRef, kObjectID_Device, 0, &sample, &host, &seed);
    assert(sample == 1024 && host == 2124);
    clockAvailable = 0;
    Audio8DJ_GetZeroTimeStamp(gDriverRef, kObjectID_Device, 0, &sample, &host, &seed);
    assert(sample == 1024 && host == 2124 && seed == 5);
    clockAvailable = 1; clockFrames = 0; clockHost = 3000; clockSeed = 6;
    Audio8DJ_GetZeroTimeStamp(gDriverRef, kObjectID_Device, 0, &sample, &host, &seed);
    assert(sample == 0 && host == 3000 && seed == 6);
    assert(Audio8DJ_GetZeroTimeStamp(gDriverRef, kObjectID_Device, 0, NULL, &host, &seed) != 0);
    Float32 buffer[8] = {1,1,1,1,1,1,1,1};
    AudioServerPlugInIOCycleInfo cycle = {0};
    cycle.mInputTime.mSampleTime = INFINITY;
    assert(Audio8DJ_DoIOOperation(gDriverRef, kObjectID_Device, kObjectID_Stream_Input, 0,
        kAudioServerPlugInIOOperationReadInput, 1, &cycle, buffer, NULL) != 0);
    for (int c = 0; c < 8; c++) assert(buffer[c] == 0);
    cycle.mOutputTime.mSampleTime = 0x1p64;
    assert(Audio8DJ_DoIOOperation(gDriverRef, kObjectID_Device, kObjectID_Stream_Output, 0,
        kAudioServerPlugInIOOperationWriteMix, 1, &cycle, buffer, NULL) != 0);
    assert(!playbacks && !captures);
    cycle.mInputTime.mSampleTime = 0;
    gStreamInputActive = false;
    assert(Audio8DJ_DoIOOperation(gDriverRef, kObjectID_Device, kObjectID_Stream_Input, 0,
        kAudioServerPlugInIOOperationReadInput, 1, &cycle, buffer, NULL) == 0);
    assert(!captures);
    gStreamInputActive = true;
    assert(Audio8DJ_DoIOOperation(gDriverRef, kObjectID_Device, kObjectID_Stream_Output, 0,
        kAudioServerPlugInIOOperationReadInput, 1, &cycle, buffer, NULL) != 0);
    gIORunningCount = 0;
    assert(Audio8DJ_GetZeroTimeStamp(gDriverRef, kObjectID_Device, 0, &sample, &host, &seed) != 0);
    AudioObjectPropertyAddress address = {kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, 0};
    Boolean settable = false;
    assert(Audio8DJ_IsPropertySettable(gDriverRef, 9999, 0, &address, &settable) == kAudioHardwareBadObjectError);
    assert(Audio8DJ_IsPropertySettable(gDriverRef, kObjectID_Stream_Input, 0, &address, &settable) == 0 && settable);
    AudioStreamBasicDescription format;
    FillStreamFormat(&format, 44100);
    requestResult = 0;
    assert(Audio8DJ_SetPropertyData(gDriverRef, kObjectID_Stream_Input, 0, &address,
                                   0, NULL, sizeof(format), &format) == 0 && requestedRate == 44100);
    format.mChannelsPerFrame = 2;
    assert(Audio8DJ_SetPropertyData(gDriverRef, kObjectID_Stream_Input, 0, &address,
                                   0, NULL, sizeof(format), &format) == kAudioDeviceUnsupportedFormatError);
}

int main(void)
{
    AudioServerPlugInHostInterface host = { .PropertiesChanged = Changed,
                                           .RequestDeviceConfigurationChange = Request };
    gHost = &host;
    AudioServerPlugInClientInfo a = { .mClientID = 10 }, b = { .mClientID = 20 };
    assert(Audio8DJ_AddDeviceClient(gDriverRef, kObjectID_Device, &a) == 0);
    assert(Audio8DJ_AddDeviceClient(gDriverRef, kObjectID_Device, &b) == 0);
    CheckPresence(false);
    assert(Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 10) != 0 && starts == 0);
    deviceID = 100;
    RefreshPresence(); CheckPresence(true);
    assert(listChanges == 1);
    RefreshPresence(); assert(listChanges == 1);
    startFailure = 1;
    assert(Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 10) != 0);
    assert(gIORunningCount == 0);
    startFailure = 0;
    assert(Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 10) == 0);
    assert(Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 10) == 0);
    assert(Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 20) == 0);
    assert(starts == 2 && gIORunningCount == 2);
    Audio8DJ_StopIO(gDriverRef, kObjectID_Device, 10);
    Audio8DJ_StopIO(gDriverRef, kObjectID_Device, 10);
    Audio8DJ_StopIO(gDriverRef, kObjectID_Device, 999);
    assert(stops == 0 && gIORunningCount == 1);
    Audio8DJ_RemoveDeviceClient(gDriverRef, kObjectID_Device, &b);
    assert(stops == 1 && gIORunningCount == 0);
    Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 10);
    deviceID = 0;
    RefreshPresence(); CheckPresence(false);
    assert(stops == 2 && gIORunningCount == 0 && deadChanges == 1);
    Audio8DJ_StopIO(gDriverRef, kObjectID_Device, 10);
    assert(stops == 2);
    deviceID = 200;
    RefreshPresence(); CheckPresence(true);
    Audio8DJ_AddDeviceClient(gDriverRef, kObjectID_Device, &b);
    Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 20);
    Audio8DJ_StopIO(gDriverRef, kObjectID_Device, 10); // stale stop from old session
    assert(gIORunningCount == 1 && stops == 2);
    deviceID = 300; // unplug/replug faster than the polling interval
    RefreshPresence(); CheckPresence(false);
    RefreshPresence(); CheckPresence(true);
    assert(stops == 3);
    gDevicePresent = false;
    pthread_mutex_lock(&gIOMutex);
    assert(Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 20) != 0);
    pthread_mutex_unlock(&gIOMutex);
    gDevicePresent = true;
    AudioObjectPropertyAddress rate = { kAudioDevicePropertyNominalSampleRate,
                                       kAudioObjectPropertyScopeGlobal, 0 };
    Float64 hz = 96000;
    requestResult = kAudioHardwareIllegalOperationError;
    assert(Audio8DJ_SetPropertyData(gDriverRef, kObjectID_Device, 0, &rate,
                                   0, NULL, sizeof(hz), &hz) == requestResult);
    assert(requestedRate == 96000 && gSampleRate == 48000);
    assert(Audio8DJ_PerformDeviceConfigurationChange(gDriverRef, kObjectID_Device, 96000, NULL) == 0);
    assert(gSampleRate == 96000);
    assert(Audio8DJ_PerformDeviceConfigurationChange(gDriverRef, kObjectID_Device, 192000, NULL) != 0);
    assert(gSampleRate == 96000);
    // A rate change while a client runs restarts USB at the new rate rather
    // than refusing (the host need not stop IO first).
    assert(Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 20) == 0);
    int stopsBefore = stops, startsBefore = starts;
    restartAllowed = 1;
    assert(Audio8DJ_PerformDeviceConfigurationChange(gDriverRef, kObjectID_Device, 44100, NULL) == 0);
    assert(gSampleRate == 44100 && stops == stopsBefore + 1 && starts == startsBefore + 1);
    assert(gIORunningCount == 1);
    startFailure = 1;
    assert(Audio8DJ_PerformDeviceConfigurationChange(gDriverRef, kObjectID_Device, 96000, NULL) != 0);
    startFailure = 0;
    restartAllowed = 0;
    Audio8DJ_StopIO(gDriverRef, kObjectID_Device, 20);
    assert(gSampleRate == 96000);
    // Latency must not repeat the queue lead that the safety offset carries.
    AudioObjectPropertyAddress lat = { kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, 0 };
    UInt32 v, sz = sizeof(v);
    assert(Audio8DJ_GetPropertyData(gDriverRef, kObjectID_Device, 0, &lat, 0, NULL, sizeof(v), &sz, &v) == 0);
    assert(v == A8DJ_UsbDeviceLatencyFrames(96000, 0));
    lat.mScope = kAudioObjectPropertyScopeInput;
    assert(Audio8DJ_GetPropertyData(gDriverRef, kObjectID_Device, 0, &lat, 0, NULL, sizeof(v), &sz, &v) == 0);
    assert(v == A8DJ_UsbDeviceLatencyFrames(96000, 1));
    lat.mSelector = kAudioDevicePropertySafetyOffset;
    assert(Audio8DJ_GetPropertyData(gDriverRef, kObjectID_Device, 0, &lat, 0, NULL, sizeof(v), &sz, &v) == 0);
    assert(v == A8DJ_UsbSafetyOffsetFrames(96000, 1));
    lat.mScope = kAudioObjectPropertyScopeOutput;
    assert(Audio8DJ_GetPropertyData(gDriverRef, kObjectID_Device, 0, &lat, 0, NULL, sizeof(v), &sz, &v) == 0);
    assert(v == A8DJ_UsbSafetyOffsetFrames(96000, 0));
    for (int i = 0; i < 1000; i++) {
        assert(Audio8DJ_StartIO(gDriverRef, kObjectID_Device, 20) == 0);
        Audio8DJ_StopIO(gDriverRef, kObjectID_Device, 20);
    }
    Audio8DJ_RemoveDeviceClient(gDriverRef, kObjectID_Device, &a);
    Audio8DJ_RemoveDeviceClient(gDriverRef, kObjectID_Device, &b);
    assert(!gClients && !gIORunningCount);
    AudioObjectPropertyAddress property = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, 0};
    UInt8 unaligned[sizeof(Float64) + 1];
    UInt32 written;
    assert(Audio8DJ_GetPropertyData(gDriverRef, kObjectID_Device, 0, &property, 0, NULL,
                                   sizeof(Float64), &written, unaligned + 1) == 0);
    Float64 value; memcpy(&value, unaligned + 1, sizeof(value));
    assert(value == gSampleRate);
    assert(Audio8DJ_SetPropertyData(gDriverRef, kObjectID_Device, 0, &property, 0, NULL,
                                   sizeof(Float64), unaligned + 1) == 0);
    property.mElement = 1;
    assert(!Audio8DJ_HasProperty(gDriverRef, kObjectID_Device, 0, &property));
    assert(Audio8DJ_SetPropertyData(gDriverRef, kObjectID_Device, 0, &property, 0, NULL,
                                   sizeof(Float64), unaligned + 1) == kAudioHardwareUnknownPropertyError);
    property.mElement = 0; property.mScope = kAudioObjectPropertyScopeInput;
    assert(!Audio8DJ_HasProperty(gDriverRef, kObjectID_Device, 0, &property));
    // Latency config edits apply between sessions only, and tell the host.
    gNotifyQueue = dispatch_queue_create("test", DISPATCH_QUEUE_SERIAL);
    UInt32 savedRunning = gIORunningCount;
    gIORunningCount = 1; configChanged = 1;
    int reloadsBefore = reloads;
    RefreshPresence();
    assert(reloads == reloadsBefore && configChanged == 1);
    gIORunningCount = 0;
    RefreshPresence();
    dispatch_sync_f(gNotifyQueue, NULL, Flush);
    assert(reloads == reloadsBefore + 1 && timingRequests == 1);
    RefreshPresence();                              // unchanged file: no notification
    dispatch_sync_f(gNotifyQueue, NULL, Flush);
    assert(timingRequests == 1 && timingChanges == 1);
    Float64 rateBefore = gSampleRate;
    assert(Audio8DJ_PerformDeviceConfigurationChange(gDriverRef, kObjectID_Device, 1, NULL) == 0);
    assert(gSampleRate == rateBefore);
    gIORunningCount = savedRunning;
    TestClockAndIO();
    puts("PASS: presence, UID, notifications, replug, clients, start failure, rate, 1000 switches, config reload, clock jitter and IO validation");
}
