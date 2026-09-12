// a8dj-audio — inspect the Audio 8 DJ CoreAudio device from outside coreaudiod.
//
// The device name is cached by CoreAudio, so it cannot be used to report live
// state. This queries the properties that actually matter for getting the ring
// buffer phase right: stream counts, latency, safety offset and running state.
//
// Build: make a8dj-audio

#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>

static AudioObjectID FindDevice(const char *uidWanted)
{
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr)
        return kAudioObjectUnknown;

    UInt32 count = size / sizeof(AudioObjectID);
    AudioObjectID ids[64];
    if (count > 64) count = 64;
    size = count * sizeof(AudioObjectID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, ids) != noErr)
        return kAudioObjectUnknown;

    for (UInt32 i = 0; i < count; i++) {
        CFStringRef uid = NULL;
        UInt32 s = sizeof(uid);
        AudioObjectPropertyAddress ua = { kAudioDevicePropertyDeviceUID,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
        if (AudioObjectGetPropertyData(ids[i], &ua, 0, NULL, &s, &uid) != noErr || !uid) continue;
        char buf[256];
        CFStringGetCString(uid, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(uid);
        if (strcmp(buf, uidWanted) == 0) return ids[i];
    }
    return kAudioObjectUnknown;
}

static UInt32 U32(AudioObjectID dev, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    UInt32 v = 0, s = sizeof(v);
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &s, &v) != noErr) return (UInt32)-1;
    return v;
}

static void ReportChannels(AudioObjectID dev, AudioObjectPropertyScope scope, const char *label)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, scope,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size == 0) {
        printf("  %-8s : (取得不可)\n", label);
        return;
    }
    unsigned char storage[4096];
    if (size > sizeof(storage)) size = sizeof(storage);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, storage) != noErr) {
        printf("  %-8s : (取得失敗)\n", label);
        return;
    }
    AudioBufferList *bl = (AudioBufferList *)storage;
    UInt32 ch = 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; i++) ch += bl->mBuffers[i].mNumberChannels;
    printf("  %-8s : %u ch (%u バッファ)\n", label, ch, bl->mNumberBuffers);
}

int main(void)
{
    AudioObjectID dev = FindDevice("Audio8DJ:0");
    if (dev == kAudioObjectUnknown) {
        fprintf(stderr, "Audio8DJ:0 が CoreAudio に見つかりません。\n");
        return 2;
    }
    printf("AudioObjectID : %u\n", dev);

    CFStringRef name = NULL;
    UInt32 s = sizeof(name);
    AudioObjectPropertyAddress na = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &na, 0, NULL, &s, &name) == noErr && name) {
        char buf[256];
        CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        CFRelease(name);
        printf("名前          : %s\n", buf);
    }

    Float64 rate = 0; s = sizeof(rate);
    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    AudioObjectGetPropertyData(dev, &ra, 0, NULL, &s, &rate);
    printf("サンプルレート: %.0f Hz\n", rate);
    printf("動作中        : %u\n", U32(dev, kAudioDevicePropertyDeviceIsRunning,
                                       kAudioObjectPropertyScopeGlobal));

    printf("\nチャンネル構成:\n");
    ReportChannels(dev, kAudioObjectPropertyScopeInput,  "入力");
    ReportChannels(dev, kAudioObjectPropertyScopeOutput, "出力");

    printf("\nタイミング (フレーム):\n");
    printf("  入力 latency      : %u\n", U32(dev, kAudioDevicePropertyLatency,
                                             kAudioObjectPropertyScopeInput));
    printf("  出力 latency      : %u\n", U32(dev, kAudioDevicePropertyLatency,
                                             kAudioObjectPropertyScopeOutput));
    printf("  入力 safetyOffset : %u\n", U32(dev, kAudioDevicePropertySafetyOffset,
                                             kAudioObjectPropertyScopeInput));
    printf("  出力 safetyOffset : %u\n", U32(dev, kAudioDevicePropertySafetyOffset,
                                             kAudioObjectPropertyScopeOutput));
    printf("  バッファフレーム  : %u\n", U32(dev, kAudioDevicePropertyBufferFrameSize,
                                             kAudioObjectPropertyScopeGlobal));
    return 0;
}
