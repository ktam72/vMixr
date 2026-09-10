// Per-device loopback verification for the Mixr virtual device set.
//
// The driver presents four full-duplex stereo devices (vMixr 1 .. vMixr 4).
// Each device loops back to itself: audio sent to a device's output appears on
// that same device's input, and on no other device's. Cross-device routing is
// the mixer app's job (LadioCast), not the driver's - a driver that summed
// every device's output into every input would put a device's output back on
// its own input, and any mixer reading that input closes a feedback loop.
//
// This tool plays a stereo signal into vMixr 1's output and checks that vMixr 1
// captures it while vMixr 2..4 stay silent.
//
// NOTE: quit any running mixer app (LadioCast) first. A mixer routing vMixr 1
// into another vMixr device is indistinguishable from a driver leak here, and
// shows up as a LEAK on whichever device it feeds.
//
// Uses AudioDeviceStart directly rather than AUHAL: one callback per device
// sees both sides, and no format negotiation sits between the test and driver.

#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define kDevices        4
#define kChannels       2
#define kSampleRate     48000.0
#define kRunSeconds     1
#define kCaptureFrames  (48000 * kRunSeconds)

// Source signal (played into vMixr 1's output, dev 0). Amplitude 0.9 is near
// full scale, so any accidental summing shows up immediately as clipping past
// 1.0; a correct 1:1 loopback stays at 0.9.
static const float kLeftAmp  = 0.90f;
static const float kRightAmp = 0.90f;
static const float kLeftFreq  = 220.0f;
static const float kRightFreq = 440.0f;

static AudioObjectID gDevice[kDevices];
static AudioDeviceIOProcID gIOProcID[kDevices];
static double gPhaseL;
static double gPhaseR;
static double gPhaseIncL;
static double gPhaseIncR;
// REQ-105 latency probe: total frames written to the source output, and the
// output-frame index at which a one-sample full-scale spike is written.
static unsigned gOutFrames;
#define kSpikeFrame ((unsigned)(kSampleRate / 2))
// Capture from all 4 devices' inputs (0..3).
static float gCapture[kDevices][2 * kCaptureFrames];
static unsigned gFill[kDevices];

static OSStatus IOProc(AudioObjectID inDevice, const AudioTimeStamp* inNow,
                       const AudioBufferList* inInputData,
                       const AudioTimeStamp* inInputTime,
                       AudioBufferList* inOutputData,
                       const AudioTimeStamp* inOutputTime,
                       void* inClientData) {
    (void)inNow; (void)inInputTime; (void)inOutputTime; (void)inClientData;
    int dev = -1;
    for (int i = 0; i < kDevices; i++) if (gDevice[i] == inDevice) { dev = i; break; }
    if (dev < 0) return noErr;

    // Only device 0 (vMixr 1) produces the source signal.
    if (inOutputData && dev == 0) {
        AudioBuffer* out = &inOutputData->mBuffers[0];
        if (out->mData && out->mNumberChannels == kChannels) {
            float* data = (float*)out->mData;
            for (UInt32 i = 0; i < out->mDataByteSize / sizeof(float); i += kChannels) {
                data[i + 0] = kLeftAmp  * (float)sin(gPhaseL);
                data[i + 1] = kRightAmp * (float)sin(gPhaseR);
                gPhaseL += gPhaseIncL;
                gPhaseR += gPhaseIncR;
                // REQ-105: write a one-sample full-scale spike at a known
                // output frame so the write->read delay can be measured.
                if (gOutFrames == kSpikeFrame) {
                    data[i + 0] = 1.0f;
                    data[i + 1] = 1.0f;
                }
                gOutFrames++;
            }
        }
    }

    // Every device captures its input.
    if (inInputData) {
        const AudioBuffer* in = &inInputData->mBuffers[0];
        if (in->mData && in->mNumberChannels == kChannels) {
            const float* data = (const float*)in->mData;
            for (UInt32 i = 0; i < in->mDataByteSize / sizeof(float); i += kChannels) {
                if (gFill[dev] < kCaptureFrames) {
                    gCapture[dev][gFill[dev] * 2 + 0] = data[i + 0];
                    gCapture[dev][gFill[dev] * 2 + 1] = data[i + 1];
                    gFill[dev]++;
                }
            }
        }
    }
    return noErr;
}

// The HAL hands kAudioDevicePropertyDeviceUID back as a CFStringRef pointer
// (8 bytes), not a C string: read the reference, then convert it.
static bool FindDevice(const char* uid, AudioObjectID* out) {
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &size) != noErr) return false;
    AudioObjectID ids[64];
    if (size / sizeof(AudioObjectID) > 64) size = 64 * sizeof(AudioObjectID);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids) != noErr) return false;
    bool ok = false;
    for (UInt32 i = 0; i < size / sizeof(AudioObjectID); i++) {
        AudioObjectPropertyAddress na = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, ids[i] };
        CFStringRef ref = NULL;
        UInt32 nsz = sizeof(ref);
        if (AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &nsz, &ref) != noErr) continue;
        if (ref != NULL) {
            char buf[128];
            if (CFStringGetCString(ref, buf, sizeof(buf), kCFStringEncodingUTF8) && strcmp(buf, uid) == 0) {
                *out = ids[i];
                ok = true;
            }
            CFRelease(ref);
        }
    }
    return ok;
}

static void ReportRms(int dev) {
    int n = gFill[dev];
    if (n < 100) { printf("vMixr %d: FAIL (only %d frames captured)\n", dev + 1, n); return; }
    int start = n / 2, len = n - start;
    double sumL = 0, sumR = 0;
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = start; i < n; i++) {
        sumL += (double)gCapture[dev][i * 2 + 0] * gCapture[dev][i * 2 + 0];
        sumR += (double)gCapture[dev][i * 2 + 1] * gCapture[dev][i * 2 + 1];
        peakL = fmaxf(peakL, fabsf(gCapture[dev][i * 2 + 0]));
        peakR = fmaxf(peakR, fabsf(gCapture[dev][i * 2 + 1]));
    }
    float rmsL = (float)sqrt(sumL / len);
    float rmsR = (float)sqrt(sumR / len);
    printf("vMixr %d: L rms=%f peak=%f | R rms=%f peak=%f | fill=%d/%d\n", dev + 1, rmsL, peakL, rmsR, peakR, n, (int)kCaptureFrames);
}

int main(void) {
    gPhaseIncL = 2.0 * M_PI * kLeftFreq / kSampleRate;
    gPhaseIncR = 2.0 * M_PI * kRightFreq / kSampleRate;

    bool allFound = true;
    for (int dev = 0; dev < kDevices; dev++) {
        char uid[32];
        snprintf(uid, sizeof(uid), "vMixr%d_UID", dev + 1);
        if (!FindDevice(uid, &gDevice[dev])) { printf("not found: %s\n", uid); allFound = false; }
        else printf("found: %s -> obj %u\n", uid, (unsigned)gDevice[dev]);
    }
    if (!allFound) { printf("FAIL: not all devices found\n"); return 1; }

    // Start all devices (one callback per device sees both output and input).
    // AudioDeviceStart takes an AudioDeviceIOProcID, not a raw function
    // pointer: create the ID first, then start with it.
    for (int dev = 0; dev < kDevices; dev++) {
        OSStatus st = AudioDeviceCreateIOProcID(gDevice[dev], IOProc, NULL, &gIOProcID[dev]);
        if (st != noErr) { printf("CreateIOProcID failed dev %d st=%d\n", dev, (int)st); return 1; }
        st = AudioDeviceStart(gDevice[dev], gIOProcID[dev]);
        if (st != noErr) { printf("Start failed dev %d st=%d\n", dev, (int)st); return 1; }
    }
    printf("running %d s (source=vMixr 1)...\n", kRunSeconds);
    for (int i = 0; i < kRunSeconds * 2; i++) usleep(500000);
    for (int dev = 0; dev < kDevices; dev++) {
        AudioDeviceStop(gDevice[dev], gIOProcID[dev]);
        AudioDeviceDestroyIOProcID(gDevice[dev], gIOProcID[dev]);
    }

    float expL = kLeftAmp * 0.7071f;
    float expR = kRightAmp * 0.7071f;
    printf("\nresults (expected L rms=%.4f R rms=%.4f):\n", expL, expR);
    int failures = 0;
    for (int dev = 0; dev < kDevices; dev++) ReportRms(dev);

    // Per-device loopback: the source device hears itself, the others hear nothing.
    for (int dev = 0; dev < kDevices; dev++) {
        int n = gFill[dev];
        int start = n / 2, len = n - start;
        double sL = 0, sR = 0;
        for (int i = start; i < n; i++) { sL += (double)gCapture[dev][i*2+0] * gCapture[dev][i*2+0]; sR += (double)gCapture[dev][i*2+1] * gCapture[dev][i*2+1]; }
        float rL = (float)sqrt(sL/len), rR = (float)sqrt(sR/len);
        if (dev == 0) {
            double ratioL = rL / expL, ratioR = rR / expR;
            int ok = fabs(ratioL - 1.0) <= 0.05 && fabs(ratioR - 1.0) <= 0.05;
            printf("check vMixr %d (source, expect loopback): L ratio %.3f R ratio %.3f %s\n",
                   dev + 1, ratioL, ratioR, ok ? "OK" : "MISMATCH");
            if (!ok) failures++;
        } else {
            // Anything here means the driver is leaking one device into another.
            int ok = (rL < 0.001f && rR < 0.001f);
            printf("check vMixr %d (expect silence):        L rms %.6f R rms %.6f %s\n",
                   dev + 1, rL, rR, ok ? "OK" : "LEAK");
            if (!ok) failures++;
        }
    }
    // REQ-105: measure the write->read loopback delay from the full-scale
    // spike. The sine (amplitude 0.9) never reaches the 0.99 threshold, so the
    // first sample past it is exactly the spike; its capture index minus the
    // spike's output frame index is the loopback latency in frames.
    int spikeIdx = -1;
    for (int c = (int)kSpikeFrame; c < (int)kCaptureFrames && c < (int)gFill[0]; c++) {
        if (fabsf(gCapture[0][c * 2 + 0]) >= 0.99f) { spikeIdx = c; break; }
    }
    if (spikeIdx < 0) {
        printf("check latency (REQ-105): FAIL (spike not found in capture)\n");
        failures++;
    } else {
        int latency = spikeIdx - (int)kSpikeFrame;
        double ms = latency * 1000.0 / kSampleRate;
        int ok = latency > 0 && latency <= 1000;
        printf("check latency (REQ-105): spike at frame %u, captured at %d -> %d frames (%.2f ms) %s\n",
               (unsigned)kSpikeFrame, spikeIdx, latency, ms, ok ? "OK" : "OUT OF RANGE");
        if (!ok) failures++;
    }
    if (failures) { printf("FAIL\n"); return 1; }
    printf("PASS: each device loops back to itself with no cross-device leakage\n");
    return 0;
}
