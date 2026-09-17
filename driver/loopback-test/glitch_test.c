// glitch_test: vMixr のグリッチ（波形不連続）計測ツール。
//   glitch_test play    <devIndex> <bufFrames> <seconds>   : 1kHz 正弦を出力（書き手）
//   glitch_test capture <devIndex> <bufFrames> <seconds>   : 入力を取得し不連続点を解析（読み手）
//   glitch_test both    <devIndex> <bufFrames> <seconds>   : 同一クライアントで両方
// 解析: 二階差分 |x[n]-2x[n-1]+x[n-2]| が閾値超え = 不連続。1kHz/48k, 振幅0.9 の
// 理論最大値は約 0.0154 なので閾値 0.06 は十分な余裕。
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "analyze.h"

#define SR 48000.0
#define FREQ 1000.0
#define AMP 0.9f
#define MAXSEC 20
static float gCap[2 * 48000 * MAXSEC];
static unsigned gFill, gCapMax;
static double gPhase, gInc; static unsigned gCtr; static int gRamp;
static int gDoPlay, gDoCap;
static unsigned gCycles, gInFrames[4096], gInCount;

static OSStatus IOProc(AudioObjectID dev, const AudioTimeStamp* now, const AudioBufferList* in,
                       const AudioTimeStamp* inT, AudioBufferList* out, const AudioTimeStamp* outT, void* cd) {
    (void)dev;(void)now;(void)inT;(void)outT;(void)cd;
    gCycles++;
    if (gDoPlay && out && out->mNumberBuffers) {
        AudioBuffer* b = &out->mBuffers[0];
        float* d = (float*)b->mData;
        unsigned n = b->mDataByteSize / sizeof(float);
        for (unsigned i = 0; i + 1 < n; i += 2) {
            float s = gRamp ? (float)(gCtr & 0xFFFFF) : AMP * (float)sin(gPhase);
            d[i] = s; d[i + 1] = s; gPhase += gInc; gCtr++;
        }
    }
    if (gDoCap && in && in->mNumberBuffers) {
        const AudioBuffer* b = &in->mBuffers[0];
        unsigned n = b->mDataByteSize / sizeof(float);
        if (gInCount < 4096) gInFrames[gInCount++] = n / 2;
        if (gFill + n > gCapMax) n = gCapMax - gFill;
        memcpy(gCap + gFill, b->mData, n * sizeof(float));
        gFill += n;
    }
    return noErr;
}

static AudioObjectID FindDevice(int idx) {
    char want[32]; snprintf(want, sizeof want, "vMixr%d_UID", idx + 1);
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, ids);
    for (unsigned i = 0; i < sz / sizeof(AudioObjectID); i++) {
        AudioObjectPropertyAddress u = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        CFStringRef s = NULL; UInt32 ss = sizeof s;
        if (AudioObjectGetPropertyData(ids[i], &u, 0, NULL, &ss, &s) == noErr && s) {
            char buf[64]; CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8); CFRelease(s);
            if (strcmp(buf, want) == 0) return ids[i];
        }
    }
    return 0;
}

static void Analyze(unsigned frames, unsigned bufFrames) { if (gRamp) AnalyzeRamp(gCap, frames, bufFrames); else AnalyzeCommon(gCap, frames, bufFrames, AMP, gInc); }

int main(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s play|capture|both devIndex bufFrames seconds\n", argv[0]); return 2; }
    const char* mode = argv[1]; int idx = atoi(argv[2]); unsigned buf = (unsigned)atoi(argv[3]); int sec = atoi(argv[4]);
    if (sec > MAXSEC) sec = MAXSEC;
    gDoPlay = strcmp(mode, "play") == 0 || strcmp(mode, "both") == 0;
    gDoCap  = strcmp(mode, "capture") == 0 || strcmp(mode, "both") == 0;
    gRamp = getenv("RAMP") != NULL; gInc = 2 * M_PI * FREQ / SR; gCapMax = 2 * 48000 * sec;
    AudioObjectID dev = FindDevice(idx);
    if (!dev) { fprintf(stderr, "device not found\n"); return 1; }
    AudioObjectPropertyAddress bs = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 b = buf; OSStatus st = AudioObjectSetPropertyData(dev, &bs, 0, NULL, sizeof b, &b);
    UInt32 got = 0, gs = sizeof got; AudioObjectGetPropertyData(dev, &bs, 0, NULL, &gs, &got);
    printf("[%s] dev=%u pid=%d bufFrames set=%u (st=%d) actual=%u\n", mode, dev, getpid(), buf, (int)st, got);
    AudioDeviceIOProcID pid = NULL;
    AudioDeviceCreateIOProcID(dev, IOProc, NULL, &pid);
    AudioDeviceStart(dev, pid);
    sleep(sec);
    AudioDeviceStop(dev, pid);
    AudioDeviceDestroyIOProcID(dev, pid);
    if (gDoCap) Analyze(gFill / 2, got ? got : 512);
    return 0;
}
