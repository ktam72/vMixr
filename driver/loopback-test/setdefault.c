#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int findDevByUID(AudioObjectID *outID, const char *wantUID, int verbose) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, 0 };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz) != noErr) { printf("  enum failed\n"); return 0; }
    size_t n = sz / sizeof(AudioObjectID);
    AudioObjectID *ids = malloc(sz);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids) != noErr) { free(ids); return 0; }
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        AudioObjectPropertyAddress ua = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, 0 };
        UInt32 usz = 0;
        CFStringRef ref = NULL;
        char buf[256] = "?";
        if (AudioObjectGetPropertyDataSize(ids[i], &ua, 0, NULL, &usz) == noErr && usz >= sizeof(CFStringRef)
            && AudioObjectGetPropertyData(ids[i], &ua, 0, NULL, &usz, &ref) == noErr && ref)
            CFStringGetCString(ref, buf, sizeof(buf), kCFStringEncodingUTF8);
        if (verbose) printf("  see id=%u uid=[%s] want=[%s] match=%d\n", (unsigned)ids[i], buf, wantUID, strcmp(buf, wantUID) == 0);
        if (strcmp(buf, wantUID) == 0) { *outID = ids[i]; found = 1; }
    }
    free(ids);
    return found;
}
static int setSysDefault(AudioObjectPropertySelector sel, AudioObjectID id) {
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, 0 };
    OSStatus r = AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, sizeof(id), &id);
    printf("  set sel=0x%08x id=%u status=%d\n", (unsigned)sel, (unsigned)id, (int)r);
    return r == noErr;
}
int main(int argc, char **argv) {
    const char *outUID = (argc > 1) ? argv[1] : "BuiltInSpeakerDevice";
    const char *inUID  = (argc > 2) ? argv[2] : "BuiltInMicrophoneDevice";
    int verbose = (argc > 3);
    AudioObjectID outID = 0, inID = 0;
    if (findDevByUID(&outID, outUID, verbose)) { printf("set default output -> %s (id=%u)\n", outUID, (unsigned)outID); setSysDefault(kAudioHardwarePropertyDefaultOutputDevice, outID); } else printf("device not found: %s\n", outUID);
    if (findDevByUID(&inID, inUID, verbose)) { printf("set default input  -> %s (id=%u)\n", inUID, (unsigned)inID); setSysDefault(kAudioHardwarePropertyDefaultInputDevice, inID); } else printf("device not found: %s\n", inUID);
    return 0;
}
