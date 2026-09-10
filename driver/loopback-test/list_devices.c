// List all audio devices with their IDs, names, and UIDs.
// Used to verify the Mixr HAL driver is loaded and to find its device UIDs.
//
// The HAL returns these two string properties in different formats:
//   - kAudioDevicePropertyDeviceName: NUL-terminated C string
//   - kAudioDevicePropertyDeviceUID:  CFStringRef pointer (8 bytes)
// so each is read with its own helper.

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

static void PropCStr(AudioObjectID id, AudioObjectPropertySelector sel, char* out, size_t cap) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &sz) != noErr) { out[0] = '?'; out[1] = 0; return; }
    char buf[256] = {};
    if (sz >= sizeof(buf)) sz = (UInt32)sizeof(buf) - 1;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, buf) != noErr) { out[0] = '?'; out[1] = 0; return; }
    buf[sz] = 0;
    snprintf(out, cap, "%s", buf);
}

static void PropCFStr(AudioObjectID id, AudioObjectPropertySelector sel, char* out, size_t cap) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &sz) != noErr) { out[0] = '?'; out[1] = 0; return; }
    if (sz < sizeof(CFStringRef)) { out[0] = '?'; out[1] = 0; return; }
    CFStringRef ref = NULL;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &ref) != noErr) { out[0] = '?'; out[1] = 0; return; }
    if (ref == NULL) { out[0] = '?'; out[1] = 0; return; }
    if (!CFStringGetCString(ref, out, (CFIndex)cap, kCFStringEncodingUTF8)) { out[0] = '?'; out[1] = 0; }
    CFRelease(ref);
}

int main(void) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr) {
        printf("FAIL: cannot read device list\n");
        return 1;
    }
    AudioObjectID ids[128];
    if (size / sizeof(AudioObjectID) > 128) size = 128 * sizeof(AudioObjectID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids) != noErr) {
        printf("FAIL: cannot read device list data\n");
        return 1;
    }
    int count = size / sizeof(AudioObjectID);
    printf("found %d device(s):\n", count);
    for (int i = 0; i < count; i++) {
        char name[256] = "?", uid[256] = "?";
        PropCStr(ids[i], kAudioDevicePropertyDeviceName, name, sizeof(name));
        PropCFStr(ids[i], kAudioDevicePropertyDeviceUID, uid, sizeof(uid));
        printf("  obj %3u  name=%-32s  uid=%s\n", (unsigned)ids[i], name, uid);
    }
    return 0;
}
