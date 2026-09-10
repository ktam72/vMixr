#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>

static void getUID(AudioObjectID id, char *out, int cap) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &sz) != noErr) { out[0] = '?'; out[1] = 0; return; }
    if (sz < sizeof(CFStringRef)) { out[0] = '?'; out[1] = 0; return; }
    CFStringRef ref = NULL;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &ref) != noErr || ref == NULL) { out[0] = '?'; out[1] = 0; return; }
    if (!CFStringGetCString(ref, out, (CFIndex)cap, kCFStringEncodingUTF8)) { out[0] = '?'; out[1] = 0; }
}

static void sysProp(AudioObjectPropertySelector sel, AudioObjectID *outID) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = sizeof(*outID);
    OSStatus r = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, outID);
    printf("  query sel=0x%08x status=%d -> id=%u\n", (unsigned)sel, (int)r, (unsigned)*outID);
}

static float outputVolume(AudioObjectID id) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyVolumeScalar, kAudioDevicePropertyScopeOutput, 0 };
    float v = -1.0f; UInt32 sz = sizeof(v);
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &v) == noErr) return v;
    return -1.0f;
}

int main(void) {
    AudioObjectID out = 0, mic = 0;
    printf("== default output ==\n");
    sysProp(kAudioHardwarePropertyDefaultOutputDevice, &out);
    char uid[256]; getUID(out, uid, sizeof(uid));
    printf("  default output uid=%s volume=%f\n", uid, outputVolume(out));
    printf("== default input ==\n");
    sysProp(kAudioHardwarePropertyDefaultInputDevice, &mic);
    getUID(mic, uid, sizeof(uid));
    printf("  default input uid=%s\n", uid);
    // enumerate devices and mark which is default output
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz) == noErr) {
        size_t n = sz / sizeof(AudioObjectID);
        AudioObjectID *ids = malloc(sz);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids) == noErr) {
            for (size_t i = 0; i < n; i++) {
                char u[256]; getUID(ids[i], u, sizeof(u));
                printf("  dev id=%u uid=%s %s\n", (unsigned)ids[i], u, (ids[i] == out) ? "<= default output" : "");
            }
        }
        free(ids);
    }
    return 0;
}
