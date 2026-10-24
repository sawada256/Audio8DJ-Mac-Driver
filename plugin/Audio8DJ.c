// Audio8DJ.c — AudioServerPlugIn for the Native Instruments Audio 8 DJ
//
// Runs inside coreaudiod. Requires no entitlement and no Apple Developer Program
// membership: an ad-hoc code signature is enough for local use.
//
// coreaudiod is permitted to open the device directly, so the plug-in owns the
// USB stream itself (see usb_engine.c). There is no helper daemon and no shared
// memory: the device's own isochronous IN stream is the clock CoreAudio slaves to.

#include <CoreAudio/AudioServerPlugIn.h>
#include "usb_engine.h"
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <os/log.h>
#include <stdatomic.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
#include <math.h>

#pragma mark - Configuration

#define kDeviceUID          "Audio8DJ:0"
#define kDeviceModelUID     "Audio8DJ:Model"
#define kDeviceName         "Audio 8 DJ"
#define kManufacturerName   "Native Instruments"
#define kBoxUID             "Audio8DJ:Box"

// The hardware is 8 analogue in / 8 analogue out.
#define kChannelsPerDirection   8
#define kDefaultSampleRate      48000.0

// How many frames the published clock advances between zero timestamps. The
// engine's ring is a separate, larger buffer (RING_FRAMES in usb_engine.c).
#define kZeroTimeStampPeriod    1024

enum {
    kObjectID_PlugIn        = kAudioObjectPlugInObject,  // 1
    kObjectID_Box           = 2,
    kObjectID_Device        = 3,
    kObjectID_Stream_Input  = 4,
    kObjectID_Stream_Output = 5,
};

#pragma mark - State

static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;

static AudioServerPlugInHostRef gHost = NULL;
static UInt32   gRefCount           = 0;
static _Atomic UInt32 gIORunningCount     = 0;
static _Atomic Float64 gSampleRate         = kDefaultSampleRate;
static _Atomic Boolean gStreamInputActive  = true;
static _Atomic Boolean gStreamOutputActive = true;

// USB lifecycle work never holds the property/clock mutex. In particular,
// a DAW can query state while the previous client's USB transfers are draining.
static pthread_mutex_t gIOMutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic Boolean gDevicePresent = false;
static uint64_t gRegistryID; // protected by gIOMutex
static dispatch_queue_t gNotifyQueue;
typedef struct Client {
    UInt32 id;
    Boolean running;
    struct Client *next;
} Client;
static Client *gClients; // protected by gIOMutex

static Client *FindClient(UInt32 id)
{
    for (Client *c = gClients; c; c = c->next)
        if (c->id == id) return c;
    return NULL;
}

static void NotifyRunning(void *unused)
{
    (void)unused;
    AudioObjectPropertyAddress address = { kAudioDevicePropertyDeviceIsRunning,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (gHost) gHost->PropertiesChanged(gHost, kObjectID_Device, 1, &address);
}

static void NotifyActive(void *context)
{
    AudioObjectPropertyAddress address = { kAudioStreamPropertyIsActive,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (gHost) gHost->PropertiesChanged(gHost, (AudioObjectID)(uintptr_t)context, 1, &address);
}

static void RunningChanged(void)
{
    if (gNotifyQueue) dispatch_async_f(gNotifyQueue, NULL, NotifyRunning);
}

static void NotifyPresence(void)
{
    AudioObjectPropertyAddress alive = { kAudioDevicePropertyDeviceIsAlive,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectPropertyAddress lists[] = {
        { kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
        { kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }
    };
    // No driver lock may be held across host callbacks: the host can reenter.
    if (gHost) {
        if (!gDevicePresent) gHost->PropertiesChanged(gHost, kObjectID_Device, 1, &alive);
        gHost->PropertiesChanged(gHost, kObjectID_PlugIn, 2, lists);
    }
}

// Configuration-change action meaning "latency/safety offsets changed".
// Sample rates are the other actions; no supported rate can be 1.
#define kChangeActionTiming 1

// The latency config changed between sessions. coreaudiod caches the safety
// offsets: measured, a plain PropertiesChanged left it on the old values
// while the engine ran with the new ones. A configuration change makes the
// host re-read the device, which is how it is meant to learn of this.
static void RequestTimingChange(void *unused)
{
    (void)unused;
    if (!gHost) return;
    gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, kChangeActionTiming, NULL);
    AudioObjectPropertyAddress a[4] = {
        { kAudioDevicePropertyLatency, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain },
        { kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain },
        { kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain },
        { kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain },
    };
    gHost->PropertiesChanged(gHost, kObjectID_Device, 4, a);
}

static void RefreshPresence(void)
{
    pthread_mutex_lock(&gIOMutex);
    if (gIORunningCount == 0 && A8DJ_UsbReloadConfig()) {
        os_log(OS_LOG_DEFAULT, "Audio8DJ: latency config reloaded");
        if (gNotifyQueue) dispatch_async_f(gNotifyQueue, NULL, RequestTimingChange);
    }
    uint64_t identity = A8DJ_UsbPollDevice();
    if (identity == gRegistryID) { pthread_mutex_unlock(&gIOMutex); return; }
    if (gRegistryID) {
        gDevicePresent = false;
        gRegistryID = 0;
        for (Client *c = gClients; c; c = c->next) c->running = false;
        gIORunningCount = 0;
        pthread_mutex_unlock(&gIOMutex);
        NotifyPresence();
        RunningChanged();
        pthread_mutex_lock(&gIOMutex);
        A8DJ_UsbStop();
        // Publish a replacement on the next pass, after the old USB session
        // has drained. This also handles unplug/replug between two polls.
    } else if (identity) {
        gRegistryID = identity;
        gDevicePresent = true;
        pthread_mutex_unlock(&gIOMutex);
        NotifyPresence();
        return;
    }
    pthread_mutex_unlock(&gIOMutex);
}

static void *PresenceThread(void *unused)
{
    (void)unused;
    pthread_setname_np("Audio8DJ presence");
    for (;;) { RefreshPresence(); usleep(250000); }
    return NULL;
}

// Anchor for the zero timestamp clock.
static _Atomic Float64 gHostTicksPerFrame = 0.0;
static UInt64   gAnchorHostTime    = 0;
static UInt64   gTimestampCount    = 0;
static UInt64   gTimestampSeed     = 0;

static const Float64 kSupportedSampleRates[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
#define kNumSupportedSampleRates (sizeof(kSupportedSampleRates) / sizeof(Float64))

#pragma mark - Helpers

static void FillStreamFormat(AudioStreamBasicDescription *outFormat, Float64 inRate)
{
    outFormat->mSampleRate       = inRate;
    outFormat->mFormatID         = kAudioFormatLinearPCM;
    outFormat->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian
                                 | kAudioFormatFlagIsPacked;
    outFormat->mBytesPerPacket   = 4 * kChannelsPerDirection;
    outFormat->mFramesPerPacket  = 1;
    outFormat->mBytesPerFrame    = 4 * kChannelsPerDirection;
    outFormat->mChannelsPerFrame = kChannelsPerDirection;
    outFormat->mBitsPerChannel   = 32;
    outFormat->mReserved         = 0;
}

static Boolean IsSupportedSampleRate(Float64 inRate)
{
    for (UInt32 i = 0; i < kNumSupportedSampleRates; i++)
        if (kSupportedSampleRates[i] == inRate) return true;
    return false;
}

#pragma mark - Prototypes

static HRESULT   Audio8DJ_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG     Audio8DJ_AddRef(void *inDriver);
static ULONG     Audio8DJ_Release(void *inDriver);
static OSStatus  Audio8DJ_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus  Audio8DJ_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus  Audio8DJ_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus  Audio8DJ_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus  Audio8DJ_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus  Audio8DJ_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus  Audio8DJ_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean   Audio8DJ_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus  Audio8DJ_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus  Audio8DJ_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static OSStatus  Audio8DJ_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
static OSStatus  Audio8DJ_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
static OSStatus  Audio8DJ_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus  Audio8DJ_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus  Audio8DJ_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus  Audio8DJ_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus  Audio8DJ_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus  Audio8DJ_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus  Audio8DJ_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    Audio8DJ_QueryInterface,
    Audio8DJ_AddRef,
    Audio8DJ_Release,
    Audio8DJ_Initialize,
    Audio8DJ_CreateDevice,
    Audio8DJ_DestroyDevice,
    Audio8DJ_AddDeviceClient,
    Audio8DJ_RemoveDeviceClient,
    Audio8DJ_PerformDeviceConfigurationChange,
    Audio8DJ_AbortDeviceConfigurationChange,
    Audio8DJ_HasProperty,
    Audio8DJ_IsPropertySettable,
    Audio8DJ_GetPropertyDataSize,
    Audio8DJ_GetPropertyData,
    Audio8DJ_SetPropertyData,
    Audio8DJ_StartIO,
    Audio8DJ_StopIO,
    Audio8DJ_GetZeroTimeStamp,
    Audio8DJ_WillDoIOOperation,
    Audio8DJ_BeginIOOperation,
    Audio8DJ_DoIOOperation,
    Audio8DJ_EndIOOperation,
};

static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef        gDriverRef    = &gInterfacePtr;

#pragma mark - Factory

void *Audio8DJ_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

void *Audio8DJ_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;
    if (!inRequestedTypeUUID || !CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return NULL;
    return gDriverRef;
}

#pragma mark - IUnknown

static HRESULT Audio8DJ_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
    if (inDriver != gDriverRef || outInterface == NULL) return kAudioHardwareBadObjectError;

    *outInterface = NULL;
    CFUUIDRef uuid = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (uuid == NULL) return kAudioHardwareIllegalOperationError;

    HRESULT result = E_NOINTERFACE;
    if (CFEqual(uuid, IUnknownUUID) || CFEqual(uuid, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex);
        gRefCount++;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(uuid);
    return result;
}

static ULONG Audio8DJ_AddRef(void *inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    ULONG r = ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG Audio8DJ_Release(void *inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    ULONG r = (gRefCount > 0) ? --gRefCount : 0;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

#pragma mark - Lifecycle

static OSStatus Audio8DJ_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;

    gHost = inHost;

    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    // Host ticks are nanoseconds scaled by the timebase ratio.
    gHostTicksPerFrame = ((Float64)tb.denom / (Float64)tb.numer) * 1.0e9 / gSampleRate;

    // Gets the firmware onto the device now, so the first StartIO does not have
    // to wait seconds for it and get torn down for being unresponsive.
    A8DJ_UsbPrepare();
    gNotifyQueue = dispatch_queue_create("Audio8DJ notifications", DISPATCH_QUEUE_SERIAL);
    pthread_t presenceThread;
    if (pthread_create(&presenceThread, NULL, PresenceThread, NULL) != 0)
        return kAudioHardwareUnspecifiedError;
    pthread_detach(presenceThread);

    os_log(OS_LOG_DEFAULT, "Audio8DJ: initialized (%d in / %d out @ %.0f Hz)",
           kChannelsPerDirection, kChannelsPerDirection, gSampleRate);
    return kAudioHardwareNoError;
}

// Device creation is driven by USB presence, not by application requests.
static OSStatus Audio8DJ_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription,
                                      const AudioServerPlugInClientInfo *inClientInfo, AudioObjectID *outDeviceObjectID)
{
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus Audio8DJ_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus Audio8DJ_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                         const AudioServerPlugInClientInfo *inClientInfo)
{
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device || !inClientInfo)
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    if (!FindClient(inClientInfo->mClientID)) {
        Client *c = calloc(1, sizeof(*c));
        if (!c) { pthread_mutex_unlock(&gIOMutex); return kAudioHardwareUnspecifiedError; }
        c->id = inClientInfo->mClientID;
        c->next = gClients;
        gClients = c;
    }
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus Audio8DJ_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                            const AudioServerPlugInClientInfo *inClientInfo)
{
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device || !inClientInfo)
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    Client **slot = &gClients;
    while (*slot && (*slot)->id != inClientInfo->mClientID) slot = &(*slot)->next;
    if (*slot) {
        Client *c = *slot;
        *slot = c->next;
        if (c->running && --gIORunningCount == 0) {
            A8DJ_UsbStop();
            RunningChanged();
        }
        free(c);
    }
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus Audio8DJ_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                          AudioObjectID inDeviceObjectID,
                                                          UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inChangeInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    // Nothing to do here: the values are already current, and the host
    // re-reads them once this returns.
    if (inChangeAction == kChangeActionTiming) return kAudioHardwareNoError;

    // inChangeAction carries the requested sample rate, set by SetPropertyData.
    Float64 newRate = (Float64)inChangeAction;
    if (!IsSupportedSampleRate(newRate)) return kAudioHardwareIllegalOperationError;

    // The host is not obliged to stop IO before a configuration change, and a
    // DAW that keeps the device open would otherwise never get its new rate.
    // Restart the USB session underneath the running clients instead; the new
    // clock seed tells CoreAudio the timeline started over.
    pthread_mutex_lock(&gIOMutex);
    Boolean wasRunning = gIORunningCount > 0;
    if (wasRunning) A8DJ_UsbStop();
    pthread_mutex_lock(&gStateMutex);
    gSampleRate = newRate;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gHostTicksPerFrame = ((Float64)tb.denom / (Float64)tb.numer) * 1.0e9 / newRate;
    gTimestampCount = 0;
    gTimestampSeed = 0;
    gAnchorHostTime = mach_absolute_time();
    pthread_mutex_unlock(&gStateMutex);
    OSStatus err = kAudioHardwareNoError;
    if (wasRunning && A8DJ_UsbStart(newRate) != 0) err = kAudioHardwareNotRunningError;
    pthread_mutex_unlock(&gIOMutex);
    return err;
}

static OSStatus Audio8DJ_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver,
                                                        AudioObjectID inDeviceObjectID,
                                                        UInt64 inChangeAction, void *inChangeInfo)
{
    (void)inChangeAction; (void)inChangeInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}

// One address policy shared by Has/Get/Size/Set. Claiming a property at a
// nonexistent element or scope makes hosts cache a capability we cannot honor.
static OSStatus ValidatePropertyAddress(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
    if (object != kObjectID_PlugIn && object != kObjectID_Device &&
        object != kObjectID_Stream_Input && object != kObjectID_Stream_Output)
        return kAudioHardwareBadObjectError;
    if (!address || address->mElement != kAudioObjectPropertyElementMain)
        return kAudioHardwareUnknownPropertyError;
    AudioObjectPropertyScope scope = address->mScope;
    if (object == kObjectID_Device) {
        switch (address->mSelector) {
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList:
            return scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput ||
                   scope == kAudioObjectPropertyScopeOutput ? 0 : kAudioHardwareUnknownPropertyError;
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
            return scope == kAudioObjectPropertyScopeInput || scope == kAudioObjectPropertyScopeOutput
                ? 0 : kAudioHardwareUnknownPropertyError;
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            return scope == kAudioObjectPropertyScopeOutput ? 0 : kAudioHardwareUnknownPropertyError;
        default: break;
        }
    }
    return scope == kAudioObjectPropertyScopeGlobal ? 0 : kAudioHardwareUnknownPropertyError;
}

#pragma mark - Property dispatch

static Boolean Audio8DJ_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                    pid_t inClientPID, const AudioObjectPropertyAddress *inAddress)
{
    if (inDriver != gDriverRef || ValidatePropertyAddress(inObjectID, inAddress) != 0) return false;
    if (inObjectID == kObjectID_PlugIn &&
        inAddress->mSelector == kAudioPlugInPropertyTranslateUIDToDevice) return true;
    UInt32 size = 0;
    return Audio8DJ_GetPropertyDataSize(inDriver, inObjectID, inClientPID, inAddress, 0, NULL, &size)
           == kAudioHardwareNoError;
}

static OSStatus Audio8DJ_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                            pid_t inClientPID, const AudioObjectPropertyAddress *inAddress,
                                            Boolean *outIsSettable)
{
    (void)inClientPID;
    if (inDriver != gDriverRef || inAddress == NULL || outIsSettable == NULL)
        return kAudioHardwareBadObjectError;

    if (inObjectID != kObjectID_PlugIn && inObjectID != kObjectID_Device &&
        inObjectID != kObjectID_Stream_Input && inObjectID != kObjectID_Stream_Output)
        return kAudioHardwareBadObjectError;
    if (!Audio8DJ_HasProperty(inDriver, inObjectID, inClientPID, inAddress))
        return kAudioHardwareUnknownPropertyError;
    *outIsSettable = (inObjectID == kObjectID_Device &&
                     inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) ||
        ((inObjectID == kObjectID_Stream_Input || inObjectID == kObjectID_Stream_Output) &&
         (inAddress->mSelector == kAudioStreamPropertyIsActive ||
          inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
          inAddress->mSelector == kAudioStreamPropertyPhysicalFormat));
    return kAudioHardwareNoError;
}

static OSStatus Audio8DJ_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                             pid_t inClientPID, const AudioObjectPropertyAddress *inAddress,
                                             UInt32 inQualifierDataSize, const void *inQualifierData,
                                             UInt32 *outDataSize)
{
    (void)inClientPID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef || inAddress == NULL || outDataSize == NULL)
        return kAudioHardwareBadObjectError;

    OSStatus addressError = ValidatePropertyAddress(inObjectID, inAddress);
    if (addressError) return addressError;

    if (inObjectID == kObjectID_PlugIn &&
        inAddress->mSelector == kAudioPlugInPropertyTranslateUIDToDevice) {
        *outDataSize = sizeof(AudioObjectID);
        return kAudioHardwareNoError;
    }
    // Resolving the size by asking for the data keeps the two paths from
    // drifting apart; the buffer is large enough for every property here.
    union { max_align_t alignment; UInt8 bytes[1024]; } scratch;
    UInt32 written = 0;
    OSStatus err = Audio8DJ_GetPropertyData(inDriver, inObjectID, inClientPID, inAddress,
                                            inQualifierDataSize, inQualifierData,
                                            sizeof(scratch.bytes), &written, scratch.bytes);
    if (err == kAudioHardwareNoError) *outDataSize = written;
    return err;
}

#define RETURN_VALUE(expr, type)                                              \
    do {                                                                      \
        if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; \
        type propertyValue = (expr);                                        \
        memcpy(outData, &propertyValue, sizeof(type));                                          \
        *outDataSize = sizeof(type);                                          \
        return kAudioHardwareNoError;                                         \
    } while (0)

static OSStatus GetPlugInProperty(const AudioObjectPropertyAddress *inAddress, UInt32 inQualifierDataSize,
                                  const void *inQualifierData, UInt32 inDataSize, UInt32 *outDataSize,
                                  void *outData)
{
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:   RETURN_VALUE(kAudioObjectClassID, AudioClassID);
    case kAudioObjectPropertyClass:       RETURN_VALUE(kAudioPlugInClassID, AudioClassID);
    case kAudioObjectPropertyOwner:       RETURN_VALUE(kAudioObjectUnknown, AudioObjectID);
    case kAudioObjectPropertyManufacturer:
        RETURN_VALUE(CFSTR(kManufacturerName), CFStringRef);

    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList: {
        AudioObjectID ids[1] = { kObjectID_Device };
        UInt32 n = (UInt32)(inDataSize / sizeof(AudioObjectID));
        if (n > 1) n = 1;
        if (!gDevicePresent) n = 0;
        memcpy(outData, ids, n * sizeof(AudioObjectID));
        *outDataSize = n * sizeof(AudioObjectID);
        return kAudioHardwareNoError;
    }

    case kAudioPlugInPropertyBoxList: {
        *outDataSize = 0;
        return kAudioHardwareNoError;
    }

    case kAudioPlugInPropertyTranslateUIDToDevice: {
        if (inQualifierDataSize != sizeof(CFStringRef) || !inQualifierData) return kAudioHardwareBadPropertySizeError;
        CFStringRef uid;
        memcpy(&uid, inQualifierData, sizeof(uid));
        CFStringRef mine = CFSTR(kDeviceUID);
        RETURN_VALUE(gDevicePresent && uid && CFEqual(uid, mine) ? kObjectID_Device : kAudioObjectUnknown, AudioObjectID);
    }

    case kAudioPlugInPropertyResourceBundle:
        RETURN_VALUE(CFSTR(""), CFStringRef);

    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus GetDeviceProperty(const AudioObjectPropertyAddress *inAddress, UInt32 inDataSize,
                                  UInt32 *outDataSize, void *outData)
{
    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:   RETURN_VALUE(kAudioObjectClassID, AudioClassID);
    case kAudioObjectPropertyClass:       RETURN_VALUE(kAudioDeviceClassID, AudioClassID);
    case kAudioObjectPropertyOwner:       RETURN_VALUE(kObjectID_PlugIn, AudioObjectID);

    case kAudioObjectPropertyName:
        // Must be constant. Hosts key their saved device selection off this
        // string, so folding live status into it made them lose the device and
        // put up a re-selection dialog every time the state changed. Live
        // state goes to /tmp/a8dj-status.txt instead.
        RETURN_VALUE(CFSTR(kDeviceName), CFStringRef);
    case kAudioObjectPropertyManufacturer:
        RETURN_VALUE(CFSTR(kManufacturerName), CFStringRef);
    case kAudioDevicePropertyDeviceUID:
        RETURN_VALUE(CFSTR(kDeviceUID), CFStringRef);
    case kAudioDevicePropertyModelUID:
        RETURN_VALUE(CFSTR(kDeviceModelUID), CFStringRef);

    case kAudioDevicePropertyTransportType:
        RETURN_VALUE(kAudioDeviceTransportTypeUSB, UInt32);
    case kAudioDevicePropertyClockDomain:
        RETURN_VALUE(0, UInt32);
    case kAudioDevicePropertyDeviceIsAlive:
        RETURN_VALUE(gDevicePresent ? 1 : 0, UInt32);
    case kAudioDevicePropertyDeviceIsRunning:
        RETURN_VALUE(gIORunningCount > 0 ? 1 : 0, UInt32);
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        RETURN_VALUE(1, UInt32);
    case kAudioObjectPropertyIdentify:
    case kAudioDevicePropertyIsHidden:
        RETURN_VALUE(0, UInt32);

    case kAudioDevicePropertyLatency:
        // CoreAudio adds Latency and SafetyOffset for the latency apps see.
        // The queue lead lives in the safety offset (it is what makes the HAL
        // write early), so this holds only what lies outside the published
        // timeline. It used to repeat the lead and overstate output latency.
        RETURN_VALUE(A8DJ_UsbDeviceLatencyFrames(gSampleRate,
                         inAddress->mScope == kAudioObjectPropertyScopeInput), UInt32);
    case kAudioDevicePropertySafetyOffset:
        // Measured: kAudioDevicePropertyLatency does not move CoreAudio's write
        // position at all (it is reported to apps, not used for scheduling).
        // How far ahead the HAL writes is set by the safety offset, so for
        // output it has to cover the whole lead the engine reads with, plus the
        // transfer it is filling. Capture only needs one transfer of slack,
        // since the newest transfer's frames are not all present yet.
        //
        // Output: one transfer of slack on top of the lead. Measured worst case
        // over ten minutes on preset=low is +368 frames, so this is ample.
        //
        // Input: two transfers, not one. Frames arrive a whole transfer at a
        // time, but the clock CoreAudio interpolates from runs on smoothly, so
        // just before a transfer lands "now" is already a transfer ahead of
        // what the ring actually holds. One transfer of slack lands exactly on
        // that boundary with nothing left for jitter, and it measured negative
        // at every buffer size -- -10 frames and 613 reads past the end in 25 s
        // at 32 frames, which is the crackle heard while recording. The second
        // transfer is the jitter allowance (in_margin_us in the config).
        RETURN_VALUE(A8DJ_UsbSafetyOffsetFrames(gSampleRate,
                         inAddress->mScope != kAudioObjectPropertyScopeOutput), UInt32);
    case kAudioDevicePropertyZeroTimeStampPeriod:
        RETURN_VALUE(kZeroTimeStampPeriod, UInt32);

    case kAudioDevicePropertyNominalSampleRate:
        RETURN_VALUE(gSampleRate, Float64);

    case kAudioDevicePropertyAvailableNominalSampleRates: {
        UInt32 n = (UInt32)(inDataSize / sizeof(AudioValueRange));
        if (n > kNumSupportedSampleRates) n = kNumSupportedSampleRates;
        AudioValueRange *r = (AudioValueRange *)outData;
        for (UInt32 i = 0; i < n; i++) {
            r[i].mMinimum = kSupportedSampleRates[i];
            r[i].mMaximum = kSupportedSampleRates[i];
        }
        *outDataSize = n * sizeof(AudioValueRange);
        return kAudioHardwareNoError;
    }

    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams: {
        AudioObjectID ids[2];
        UInt32 count = 0;
        // Scope decides which streams are visible.
        if (inAddress->mScope == kAudioObjectPropertyScopeGlobal
            || inAddress->mScope == kAudioObjectPropertyScopeInput)
            ids[count++] = kObjectID_Stream_Input;
        if (inAddress->mScope == kAudioObjectPropertyScopeGlobal
            || inAddress->mScope == kAudioObjectPropertyScopeOutput)
            ids[count++] = kObjectID_Stream_Output;

        UInt32 n = (UInt32)(inDataSize / sizeof(AudioObjectID));
        if (n > count) n = count;
        memcpy(outData, ids, n * sizeof(AudioObjectID));
        *outDataSize = n * sizeof(AudioObjectID);
        return kAudioHardwareNoError;
    }

    case kAudioObjectPropertyControlList:
        *outDataSize = 0;
        return kAudioHardwareNoError;

    case kAudioDevicePropertyRelatedDevices: {
        AudioObjectID ids[1] = { kObjectID_Device };
        UInt32 n = (UInt32)(inDataSize / sizeof(AudioObjectID));
        if (n > 1) n = 1;
        memcpy(outData, ids, n * sizeof(AudioObjectID));
        *outDataSize = n * sizeof(AudioObjectID);
        return kAudioHardwareNoError;
    }

    case kAudioDevicePropertyPreferredChannelsForStereo: {
        if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        ((UInt32 *)outData)[0] = 1;
        ((UInt32 *)outData)[1] = 2;
        *outDataSize = 2 * sizeof(UInt32);
        return kAudioHardwareNoError;
    }

    case kAudioDevicePropertyPreferredChannelLayout: {
        UInt32 need = offsetof(AudioChannelLayout, mChannelDescriptions)
                    + kChannelsPerDirection * sizeof(AudioChannelDescription);
        if (inDataSize < need) return kAudioHardwareBadPropertySizeError;
        AudioChannelLayout *l = (AudioChannelLayout *)outData;
        memset(l, 0, need);
        l->mChannelLayoutTag         = kAudioChannelLayoutTag_UseChannelDescriptions;
        l->mNumberChannelDescriptions = kChannelsPerDirection;
        for (UInt32 i = 0; i < kChannelsPerDirection; i++)
            l->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Discrete_0 + i;
        *outDataSize = need;
        return kAudioHardwareNoError;
    }

    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus GetStreamProperty(AudioObjectID inObjectID, const AudioObjectPropertyAddress *inAddress,
                                  UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    const Boolean isInput = (inObjectID == kObjectID_Stream_Input);

    switch (inAddress->mSelector) {
    case kAudioObjectPropertyBaseClass:   RETURN_VALUE(kAudioObjectClassID, AudioClassID);
    case kAudioObjectPropertyClass:       RETURN_VALUE(kAudioStreamClassID, AudioClassID);
    case kAudioObjectPropertyOwner:       RETURN_VALUE(kObjectID_Device, AudioObjectID);

    case kAudioObjectPropertyOwnedObjects:
        *outDataSize = 0;
        return kAudioHardwareNoError;

    case kAudioStreamPropertyIsActive:
        RETURN_VALUE(isInput ? (gStreamInputActive ? 1 : 0) : (gStreamOutputActive ? 1 : 0), UInt32);
    case kAudioStreamPropertyDirection:
        RETURN_VALUE(isInput ? 1 : 0, UInt32);
    case kAudioStreamPropertyTerminalType:
        RETURN_VALUE(isInput ? kAudioStreamTerminalTypeLine : kAudioStreamTerminalTypeLine, UInt32);
    case kAudioStreamPropertyStartingChannel:
        RETURN_VALUE(1, UInt32);
    case kAudioStreamPropertyLatency:
        RETURN_VALUE(0, UInt32);

    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        if (inDataSize < sizeof(AudioStreamBasicDescription))
            return kAudioHardwareBadPropertySizeError;
        FillStreamFormat((AudioStreamBasicDescription *)outData, gSampleRate);
        *outDataSize = sizeof(AudioStreamBasicDescription);
        return kAudioHardwareNoError;
    }

    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        UInt32 n = (UInt32)(inDataSize / sizeof(AudioStreamRangedDescription));
        if (n > kNumSupportedSampleRates) n = kNumSupportedSampleRates;
        AudioStreamRangedDescription *d = (AudioStreamRangedDescription *)outData;
        for (UInt32 i = 0; i < n; i++) {
            FillStreamFormat(&d[i].mFormat, kSupportedSampleRates[i]);
            d[i].mSampleRateRange.mMinimum = kSupportedSampleRates[i];
            d[i].mSampleRateRange.mMaximum = kSupportedSampleRates[i];
        }
        *outDataSize = n * sizeof(AudioStreamRangedDescription);
        return kAudioHardwareNoError;
    }

    default:
        return kAudioHardwareUnknownPropertyError;
    }
}

static OSStatus Audio8DJ_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                         pid_t inClientPID, const AudioObjectPropertyAddress *inAddress,
                                         UInt32 inQualifierDataSize, const void *inQualifierData,
                                         UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    (void)inClientPID;
    if (inDriver != gDriverRef || inAddress == NULL || outDataSize == NULL || (outData == NULL && inDataSize != 0))
        return kAudioHardwareBadObjectError;
    OSStatus addressError = ValidatePropertyAddress(inObjectID, inAddress);
    if (addressError) return addressError;
    union { max_align_t alignment; UInt8 bytes[1024]; } value;
    UInt32 capacity = inDataSize < sizeof(value.bytes) ? inDataSize : (UInt32)sizeof(value.bytes);
    UInt32 written = 0;
    OSStatus result;
    switch (inObjectID) {
    case kObjectID_PlugIn:
        result = GetPlugInProperty(inAddress, inQualifierDataSize, inQualifierData,
                                   capacity, &written, value.bytes);
        break;
    case kObjectID_Device:
        result = GetDeviceProperty(inAddress, capacity, &written, value.bytes);
        break;
    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output:
        result = GetStreamProperty(inObjectID, inAddress, capacity, &written, value.bytes);
        break;
    default:
        return kAudioHardwareBadObjectError;
    }
    *outDataSize = 0;
    if (result == kAudioHardwareNoError) {
        if (written) memcpy(outData, value.bytes, written);
        *outDataSize = written;
    }
    return result;
}

static OSStatus Audio8DJ_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID,
                                         pid_t inClientPID, const AudioObjectPropertyAddress *inAddress,
                                         UInt32 inQualifierDataSize, const void *inQualifierData,
                                         UInt32 inDataSize, const void *inData)
{
    (void)inClientPID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef || inAddress == NULL || inData == NULL)
        return kAudioHardwareBadObjectError;

    OSStatus addressError = ValidatePropertyAddress(inObjectID, inAddress);
    if (addressError) return addressError;
    switch (inObjectID) {
    case kObjectID_Device:
        if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
            if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
            Float64 rate;
            memcpy(&rate, inData, sizeof(rate));
            if (!IsSupportedSampleRate(rate)) return kAudioHardwareIllegalOperationError;
            if (rate != gSampleRate) {
                if (!gHost) return kAudioHardwareIllegalOperationError;
                return gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, (UInt64)rate, NULL);
            }
            return kAudioHardwareNoError;
        }
        break;

    case kObjectID_Stream_Input:
    case kObjectID_Stream_Output:
        if (inAddress->mSelector == kAudioStreamPropertyVirtualFormat ||
            inAddress->mSelector == kAudioStreamPropertyPhysicalFormat) {
            if (inDataSize != sizeof(AudioStreamBasicDescription))
                return kAudioHardwareBadPropertySizeError;
            AudioStreamBasicDescription requested;
            memcpy(&requested, inData, sizeof(requested));
            AudioStreamBasicDescription expected;
            FillStreamFormat(&expected, requested.mSampleRate);
            if (!IsSupportedSampleRate(requested.mSampleRate) ||
                requested.mFormatID != expected.mFormatID ||
                requested.mFormatFlags != expected.mFormatFlags ||
                requested.mBytesPerPacket != expected.mBytesPerPacket ||
                requested.mFramesPerPacket != expected.mFramesPerPacket ||
                requested.mBytesPerFrame != expected.mBytesPerFrame ||
                requested.mChannelsPerFrame != expected.mChannelsPerFrame ||
                requested.mBitsPerChannel != expected.mBitsPerChannel)
                return kAudioDeviceUnsupportedFormatError;
            if (requested.mSampleRate == gSampleRate) return kAudioHardwareNoError;
            if (!gHost) return kAudioHardwareIllegalOperationError;
            return gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device,
                                                           (UInt64)requested.mSampleRate, NULL);
        }
        if (inAddress->mSelector == kAudioStreamPropertyIsActive) {
            if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            UInt32 activeValue;
            memcpy(&activeValue, inData, sizeof(activeValue));
            Boolean active = (activeValue != 0);
            Boolean old = inObjectID == kObjectID_Stream_Input
                ? atomic_exchange(&gStreamInputActive, active) : atomic_exchange(&gStreamOutputActive, active);
            if (old != active && gNotifyQueue)
                dispatch_async_f(gNotifyQueue, (void *)(uintptr_t)inObjectID, NotifyActive);
            return kAudioHardwareNoError;
        }
        break;

    default:
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnknownPropertyError;
}

#pragma mark - IO

static OSStatus Audio8DJ_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device)
        return kAudioHardwareBadObjectError;
    // Reject absent hardware immediately; serialize concurrent clients so a
    // brief presence scan does not cause a spurious StartIO failure.
    if (!gDevicePresent) return kAudioHardwareNotRunningError;
    pthread_mutex_lock(&gIOMutex);
    Client *c = FindClient(inClientID);
    OSStatus err = kAudioHardwareNoError;
    if (!gDevicePresent || !c) err = kAudioHardwareNotRunningError;
    else if (!c->running) {
        if (gIORunningCount == 0) {
            pthread_mutex_lock(&gStateMutex);
            gTimestampCount = 0;
            gTimestampSeed = 0;
            gAnchorHostTime = mach_absolute_time();
            pthread_mutex_unlock(&gStateMutex);
            if (A8DJ_UsbStart(gSampleRate) != 0) err = kAudioHardwareNotRunningError;
        }
        if (err == kAudioHardwareNoError) {
            c->running = true;
            if (gIORunningCount++ == 0) RunningChanged();
        }
    }
    pthread_mutex_unlock(&gIOMutex);
    return err;
}

static OSStatus Audio8DJ_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device)
        return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    Client *c = FindClient(inClientID);
    if (c && c->running) {
        c->running = false;
        if (--gIORunningCount == 0) {
            A8DJ_UsbStop();
            RunningChanged();
        }
    }
    pthread_mutex_unlock(&gIOMutex);
    return kAudioHardwareNoError;
}

static OSStatus Audio8DJ_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                          UInt32 inClientID, Float64 *outSampleTime, UInt64 *outHostTime,
                                          UInt64 *outSeed)
{
    (void)inClientID;
    if (inDriver != gDriverRef || inDeviceObjectID != kObjectID_Device)
        return kAudioHardwareBadObjectError;
    if (!outSampleTime || !outHostTime || !outSeed)
        return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gStateMutex);
    if (!gDevicePresent || !gIORunningCount) {
        pthread_mutex_unlock(&gStateMutex);
        return kAudioHardwareNotRunningError;
    }
    uint64_t frames, host, seed;
    if (A8DJ_UsbGetClock(&frames, &host, &seed)) {
        uint64_t count = frames / kZeroTimeStampPeriod;
        double delta = (double)(frames % kZeroTimeStampPeriod) * gHostTicksPerFrame;
        if (delta >= 0 && delta < 0x1p64 && host >= (uint64_t)delta) {
            uint64_t zeroHost = host - (uint64_t)delta;
            // Keep the exact same pair throughout one timestamp period. USB
            // completion jitter must not move host time backwards even
            // when sample time had not advanced at all.
            if (seed != gTimestampSeed ||
                (count > gTimestampCount && zeroHost > gAnchorHostTime)) {
                gTimestampCount = count;
                gAnchorHostTime = zeroHost;
                gTimestampSeed = seed;
            }
        }
    }
    // A preempted USB clock writer must not switch us to an unrelated
    // synthetic clock/seed. Reuse the last coherent hardware timestamp.
    if (!gTimestampSeed) {
        pthread_mutex_unlock(&gStateMutex);
        return kAudioHardwareNotRunningError;
    }
    *outSampleTime = (Float64)gTimestampCount * kZeroTimeStampPeriod;
    *outHostTime = gAnchorHostTime;
    *outSeed = gTimestampSeed;
    pthread_mutex_unlock(&gStateMutex);
    return kAudioHardwareNoError;
}

static OSStatus Audio8DJ_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                           UInt32 inClientID, UInt32 inOperationID, Boolean *outWillDo,
                                           Boolean *outWillDoInPlace)
{
    (void)inClientID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;

    Boolean willDo = false, inPlace = true;
    switch (inOperationID) {
    case kAudioServerPlugInIOOperationReadInput:
    case kAudioServerPlugInIOOperationWriteMix:
        willDo = true;
        break;
    default:
        break;
    }
    if (outWillDo)        *outWillDo = willDo;
    if (outWillDoInPlace) *outWillDoInPlace = inPlace;
    return kAudioHardwareNoError;
}

static OSStatus Audio8DJ_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                          UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                          const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}

static OSStatus Audio8DJ_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                       AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID,
                                       UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo *inIOCycleInfo,
                                       void *ioMainBuffer, void *ioSecondaryBuffer)
{
    (void)inClientID; (void)ioSecondaryBuffer;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (inOperationID != kAudioServerPlugInIOOperationWriteMix &&
        inOperationID != kAudioServerPlugInIOOperationReadInput) return kAudioHardwareNoError;
    if (!inIOCycleInfo || !ioMainBuffer) return kAudioHardwareIllegalOperationError;
    if ((inOperationID == kAudioServerPlugInIOOperationWriteMix && inStreamObjectID != kObjectID_Stream_Output) ||
        (inOperationID == kAudioServerPlugInIOOperationReadInput && inStreamObjectID != kObjectID_Stream_Input))
        return kAudioHardwareBadObjectError;

    Float32 *buffer = (Float32 *)ioMainBuffer;

    if (inOperationID == kAudioServerPlugInIOOperationWriteMix) {
        Float64 t = inIOCycleInfo->mOutputTime.mSampleTime;
        if (!isfinite(t) || t >= 0x1p64) return kAudioHardwareIllegalOperationError;
        if (gStreamOutputActive && t >= 0.0)
            A8DJ_UsbWritePlayback(buffer, (uint64_t)t, inIOBufferFrameSize);
    } else if (inOperationID == kAudioServerPlugInIOOperationReadInput) {
        Float64 t = inIOCycleInfo->mInputTime.mSampleTime;
        if (!isfinite(t) || t >= 0x1p64) {
            memset(buffer, 0, (size_t)inIOBufferFrameSize * kChannelsPerDirection * sizeof(Float32));
            return kAudioHardwareIllegalOperationError;
        }
        if (gStreamInputActive && t >= 0.0)
            A8DJ_UsbReadCapture(buffer, (uint64_t)t, inIOBufferFrameSize);
        else
            memset(buffer, 0, (size_t)inIOBufferFrameSize * kChannelsPerDirection * sizeof(Float32));
    }
    return kAudioHardwareNoError;
}

static OSStatus Audio8DJ_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID,
                                        UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize,
                                        const AudioServerPlugInIOCycleInfo *inIOCycleInfo)
{
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inDeviceObjectID != kObjectID_Device) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}
