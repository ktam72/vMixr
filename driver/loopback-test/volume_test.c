// Per-channel volume and mute verification for the Mixr virtual device set
// (REQ-107), through the modern client API:
//   device volume scalar (scope out/in, element 0=main, 1=left, 2=right)
//   device mute        (scope out/in, element main)
//
// Plays 0.9-amplitude stereo tones (440 Hz left, 660 Hz right) into vMixr 1's
// output and checks, through the device's own loopback input:
//   1. baseline capture is near 0.9 on both channels
//   2. output left volume 0.5 attenuates the left capture to about 0.45
//   3. output main volume 0.5 attenuates both channels
//   4. output mute silences the capture
//   5. unmuting restores the capture
//   6. input left volume 0.5 attenuates the left capture at capture time
//   7. input mute silences the capture, unmuting restores it
//
// NOTE: quit any running mixer app (LadioCast) first, for the same reason as
// loopback_test.

#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define kChannels       2
#define kRunSeconds     1
#define kMaxCaptureFrames (96000 * kRunSeconds)
#define kAmp            0.90f

static AudioObjectID gDevice;
static AudioObjectID gOutStream;
static double gRate;
static double gPhaseL;
static double gPhaseR;
static double gPhaseIncL;
static double gPhaseIncR;
static float gCapture[2 * kMaxCaptureFrames];
static unsigned gFill;

static OSStatus IOProc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
                       const AudioBufferList* inInputData,
                       const AudioTimeStamp* inInputTime,
                       AudioBufferList* inOutputData,
                       const AudioTimeStamp* inOutputTime,
                       void* inClientData) {
    (void)inDevice; (void)inNow; (void)inInputTime; (void)inOutputTime; (void)inClientData;
    if (inOutputData) {
        AudioBuffer* out = &inOutputData->mBuffers[0];
        if (out->mData && out->mNumberChannels == kChannels) {
            float* data = (float*)out->mData;
            for (UInt32 i = 0; i < out->mDataByteSize / sizeof(float); i += kChannels) {
                data[i + 0] = kAmp * (float)sin(gPhaseL);
                data[i + 1] = kAmp * (float)sin(gPhaseR);
                gPhaseL += gPhaseIncL;
                gPhaseR += gPhaseIncR;
            }
        }
    }
    if (inInputData) {
        const AudioBuffer* in = &inInputData->mBuffers[0];
        if (in->mData && in->mNumberChannels == kChannels) {
            const float* data = (const float*)in->mData;
            for (UInt32 i = 0; i < in->mDataByteSize / sizeof(float); i += kChannels) {
                if (gFill < (unsigned)(gRate * kRunSeconds)) {
                    gCapture[gFill * 2 + 0] = data[i + 0];
                    gCapture[gFill * 2 + 1] = data[i + 1];
                    gFill++;
                }
            }
        }
    }
    return noErr;
}

static bool FindDevice(const char* uid, AudioObjectID* out) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr) return false;
    AudioObjectID ids[64];
    if (size / sizeof(AudioObjectID) > 64) size = 64 * sizeof(AudioObjectID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids) != noErr) return false;
    for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
        AudioObjectPropertyAddress na = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, ids[i] };
        CFStringRef ref = NULL;
        UInt32 nsz = sizeof(ref);
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &nsz, &ref) != noErr) continue;
        if (ref != NULL) {
            char buf[128];
            if (CFStringGetCString(ref, buf, sizeof(buf), kCFStringEncodingUTF8) && strcmp(buf, uid) == 0) {
                *out = ids[i];
                CFRelease(ref);
                return true;
            }
            CFRelease(ref);
        }
    }
    return false;
}

static bool GetStreams(AudioObjectID dev, AudioObjectID* outOut, AudioObjectID* inIn) {
    bool gotOut = false, gotIn = false;
    const AudioObjectPropertyScope scopes[2] = { kAudioObjectPropertyScopeOutput, kAudioObjectPropertyScopeInput };
    for (int s = 0; s < 2; s++) {
        AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, scopes[s], kAudioObjectPropertyElementMain };
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr) continue;
        AudioObjectID ids[8];
        if (size / sizeof(AudioObjectID) > 8) size = 8 * sizeof(AudioObjectID);
        if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, ids) != noErr) continue;
        if (scopes[s] == kAudioObjectPropertyScopeOutput) { *outOut = ids[0]; gotOut = true; }
        else { *inIn = ids[0]; gotIn = true; }
    }
    return gotOut && gotIn;
}

// Get the device volume scalar for a scope + element (0=main, 1=left, 2=right).
static float GetVolume(AudioObjectPropertyScope scope, UInt32 element, const char* what, int* failures) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyVolumeScalar, scope, element };
    float v = -1.0f;
    UInt32 vSize = sizeof(v);
    OSStatus st = AudioObjectGetPropertyData(gDevice, &a, 0, NULL, &vSize, &v);
    if (st != noErr || vSize != sizeof(v)) {
        printf("FAIL: reading %s st=%d\n", what, (int)st);
        (*failures)++;
    }
    return v;
}

// Set the device volume scalar and read it back.
static void SetVolume(AudioObjectPropertyScope scope, UInt32 element, float v, const char* what, int* failures) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyVolumeScalar, scope, element };
    OSStatus st = AudioObjectSetPropertyData(gDevice, &a, 0, NULL, sizeof(v), &v);
    if (st != noErr) { printf("FAIL: setting %s to %.2f st=%d\n", what, v, (int)st); exit(1); }
    float got = GetVolume(scope, element, what, failures);
    if (fabsf(got - v) > 0.001f) {
        printf("FAIL: %s readback %.3f (wanted %.2f)\n", what, got, v);
        (*failures)++;
    }
}

static void SetMute(AudioObjectPropertyScope scope, bool muted, const char* what, int* failures) {
    UInt32 v = muted ? 1 : 0;
    AudioObjectPropertyAddress a = { kAudioDevicePropertyMute, scope, kAudioObjectPropertyElementMain };
    OSStatus st = AudioObjectSetPropertyData(gDevice, &a, 0, NULL, sizeof(v), &v);
    if (st != noErr) { printf("FAIL: setting %s st=%d\n", what, (int)st); exit(1); }
    UInt32 got = 0;
    UInt32 gotSize = sizeof(got);
    if (AudioObjectGetPropertyData(gDevice, &a, 0, NULL, &gotSize, &got) != noErr || got != v) {
        printf("FAIL: %s readback %u (wanted %u)\n", what, (unsigned)got, (unsigned)v);
        (*failures)++;
    }
}

// Run one second of IO and report the per-channel peak amplitude of the
// captured second half of the buffer (skips the first half of settling).
static void RunSecond(const char* phase, float* peakL, float* peakR) {
    gFill = 0;
    unsigned frames = (unsigned)(gRate * kRunSeconds);
    printf("running %d s (%s)...\n", kRunSeconds, phase);
    for (int i = 0; i < kRunSeconds * 2; i++) usleep(500000);
    unsigned start = frames / 2;
    if (gFill < frames) { printf("FAIL: %s only %u/%u frames captured\n", phase, gFill, frames); exit(1); }
    float pl = 0.0f, pr = 0.0f;
    for (unsigned i = start; i < frames; i++) {
        pl = fmaxf(pl, fabsf(gCapture[i * 2 + 0]));
        pr = fmaxf(pr, fabsf(gCapture[i * 2 + 1]));
    }
    printf("  %s: capture peak L=%.4f R=%.4f\n", phase, pl, pr);
    *peakL = pl;
    *peakR = pr;
}

static void CheckRange(const char* what, float peak, float lo, float hi, int* failures) {
    bool ok = peak >= lo && peak <= hi;
    printf("check %s: peak %.4f (expected %.3f..%.3f) %s\n", what, peak, lo, hi, ok ? "OK" : "MISMATCH");
    if (!ok) (*failures)++;
}

int main(void) {
    if (!FindDevice("vMixr1_UID", &gDevice)) { printf("FAIL: vMixr1_UID not found\n"); return 1; }
    AudioObjectID inStream = 0;
    if (!GetStreams(gDevice, &gOutStream, &inStream)) { printf("FAIL: no streams on vMixr 1\n"); return 1; }
    printf("found: vMixr1_UID -> obj %u (out stream %u, in stream %u)\n", (unsigned)gDevice, (unsigned)gOutStream, (unsigned)inStream);

    AudioObjectPropertyAddress ra = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 rateSize = sizeof(gRate);
    if (AudioObjectGetPropertyData(gDevice, &ra, 0, NULL, &rateSize, &gRate) != noErr || gRate <= 0.0) {
        printf("FAIL: cannot read sample rate\n");
        return 1;
    }
    printf("using sample rate %.0f Hz\n", gRate);
    gPhaseIncL = 2.0 * M_PI * 440.0 / gRate;
    gPhaseIncR = 2.0 * M_PI * 660.0 / gRate;

    int failures = 0;
    // Sanity: volumes default to 1.0 (main, left, right) on both scopes.
    for (int i = 0; i < 3; i++) {
        float vo = GetVolume(kAudioObjectPropertyScopeOutput, (UInt32)i, "out volume default", &failures);
        float vi = GetVolume(kAudioObjectPropertyScopeInput, (UInt32)i, "in volume default", &failures);
        if (vo != 1.0f || vi != 1.0f) {
            printf("FAIL: default volume element %d out=%.2f in=%.2f (wanted 1.0)\n", i, vo, vi);
            failures++;
        }
    }
    for (int s = 0; s < 2; s++) {
        AudioObjectPropertyScope scope = s ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
        AudioObjectPropertyAddress ma = { kAudioDevicePropertyMute, scope, kAudioObjectPropertyElementMain };
        UInt32 muted = 1;
        UInt32 mutedSize = sizeof(muted);
        if (AudioObjectGetPropertyData(gDevice, &ma, 0, NULL, &mutedSize, &muted) != noErr || muted != 0) {
            printf("FAIL: default mute %s = %u (wanted 0)\n", s ? "in" : "out", (unsigned)muted);
            failures++;
        }
    }
    if (failures) { printf("FAIL: sanity\n"); return 1; }

    AudioDeviceIOProcID ioID;
    OSStatus st = AudioDeviceCreateIOProcID(gDevice, IOProc, NULL, &ioID);
    if (st != noErr) { printf("FAIL: CreateIOProcID st=%d\n", (int)st); return 1; }
    st = AudioDeviceStart(gDevice, ioID);
    if (st != noErr) { printf("FAIL: Start st=%d\n", (int)st); return 1; }

    float pl, pr;
    // 1. Baseline: full volume, not muted.
    RunSecond("baseline", &pl, &pr);
    CheckRange("baseline L ~0.90", pl, kAmp - 0.05f, kAmp + 0.05f, &failures);
    CheckRange("baseline R ~0.90", pr, kAmp - 0.05f, kAmp + 0.05f, &failures);
    // 2. Output left volume 0.5 halves the left channel only.
    SetVolume(kAudioObjectPropertyScopeOutput, 1, 0.5f, "out volume L 0.5", &failures);
    RunSecond("out volume L 0.5", &pl, &pr);
    CheckRange("out L halved ~0.45", pl, 0.40f, 0.50f, &failures);
    CheckRange("out R full ~0.90", pr, kAmp - 0.05f, kAmp + 0.05f, &failures);
    // 3. Output main volume 0.5 halves both channels (L now 0.25).
    SetVolume(kAudioObjectPropertyScopeOutput, 0, 0.5f, "out volume main 0.5", &failures);
    RunSecond("out volume main 0.5", &pl, &pr);
    CheckRange("out L ~0.225", pl, 0.19f, 0.26f, &failures);
    CheckRange("out R ~0.45", pr, 0.40f, 0.50f, &failures);
    SetVolume(kAudioObjectPropertyScopeOutput, 0, 1.0f, "out volume main 1.0", &failures);
    // 4. Output mute silences the capture.
    SetMute(kAudioObjectPropertyScopeOutput, true, "out mute on", &failures);
    RunSecond("out muted", &pl, &pr);
    CheckRange("out muted L silence", pl, 0.0f, 0.01f, &failures);
    CheckRange("out muted R silence", pr, 0.0f, 0.01f, &failures);
    // 5. Unmuting restores the capture (out L is still 0.5).
    SetMute(kAudioObjectPropertyScopeOutput, false, "out mute off", &failures);
    RunSecond("out unmuted", &pl, &pr);
    CheckRange("out unmuted L ~0.45", pl, 0.40f, 0.50f, &failures);
    CheckRange("out unmuted R ~0.90", pr, kAmp - 0.05f, kAmp + 0.05f, &failures);
    SetVolume(kAudioObjectPropertyScopeOutput, 1, 1.0f, "out volume L 1.0", &failures);
    // 6. Input left volume 0.5 halves the left capture at capture time.
    SetVolume(kAudioObjectPropertyScopeInput, 1, 0.5f, "in volume L 0.5", &failures);
    RunSecond("in volume L 0.5", &pl, &pr);
    CheckRange("in L halved ~0.45", pl, 0.40f, 0.50f, &failures);
    CheckRange("in R full ~0.90", pr, kAmp - 0.05f, kAmp + 0.05f, &failures);
    // 7. Input mute silences the capture, unmuting restores it.
    SetMute(kAudioObjectPropertyScopeInput, true, "in mute on", &failures);
    RunSecond("in muted", &pl, &pr);
    CheckRange("in muted L silence", pl, 0.0f, 0.01f, &failures);
    CheckRange("in muted R silence", pr, 0.0f, 0.01f, &failures);
    SetMute(kAudioObjectPropertyScopeInput, false, "in mute off", &failures);
    RunSecond("in unmuted", &pl, &pr);
    CheckRange("in unmuted L ~0.45", pl, 0.40f, 0.50f, &failures);
    CheckRange("in unmuted R ~0.90", pr, kAmp - 0.05f, kAmp + 0.05f, &failures);
    SetVolume(kAudioObjectPropertyScopeInput, 1, 1.0f, "in volume L 1.0", &failures);

    AudioDeviceStop(gDevice, ioID);
    AudioDeviceDestroyIOProcID(gDevice, ioID);
    if (failures) { printf("FAIL\n"); return 1; }
    printf("PASS: per-channel volume and mute behave as specified\n");
    return 0;
}
