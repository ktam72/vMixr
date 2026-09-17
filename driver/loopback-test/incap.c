// 入力専用クライアント (AUHAL, output disabled) で vMixr の入力を取得し解析する。
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "analyze.h"
static float gCap[2*48000*20]; static unsigned gFill, gCapMax; static AudioUnit gAU; static AudioBufferList* gABL; static unsigned gBuf;
static OSStatus InCB(void* r, AudioUnitRenderActionFlags* f, const AudioTimeStamp* ts, UInt32 bus, UInt32 n, AudioBufferList* io) {
    (void)r;(void)io; gABL->mBuffers[0].mDataByteSize = n*2*sizeof(float);
    if (AudioUnitRender(gAU, f, ts, bus, n, gABL) != noErr) return noErr;
    unsigned s = n*2; if (gFill + s > gCapMax) s = gCapMax - gFill;
    memcpy(gCap+gFill, gABL->mBuffers[0].mData, s*sizeof(float)); gFill += s; return noErr;
}
int main(int argc, char** argv) {
    int idx = atoi(argv[1]); gBuf = atoi(argv[2]); int sec = atoi(argv[3]); gCapMax = 2*48000*sec;
    char want[32]; snprintf(want,32,"vMixr%d_UID",idx+1);
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 sz=0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&a,0,NULL,&sz); AudioObjectID ids[64]; AudioObjectGetPropertyData(kAudioObjectSystemObject,&a,0,NULL,&sz,ids);
    AudioObjectID dev=0; for(unsigned i=0;i<sz/4;i++){ AudioObjectPropertyAddress u={kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}; CFStringRef s=NULL; UInt32 ss=sizeof s; if(AudioObjectGetPropertyData(ids[i],&u,0,NULL,&ss,&s)==noErr&&s){char b[64];CFStringGetCString(s,b,64,kCFStringEncodingUTF8);CFRelease(s); if(!strcmp(b,want)) dev=ids[i];}}
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponentInstanceNew(AudioComponentFindNext(NULL,&d), &gAU);
    UInt32 on=1, off=0;
    AudioUnitSetProperty(gAU, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &on, sizeof on);
    AudioUnitSetProperty(gAU, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &off, sizeof off);
    AudioUnitSetProperty(gAU, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev);
    AudioObjectPropertyAddress bs={kAudioDevicePropertyBufferFrameSize,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}; UInt32 b=gBuf; AudioObjectSetPropertyData(dev,&bs,0,NULL,4,&b);
    AudioStreamBasicDescription f = { 48000, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0 };
    AudioUnitSetProperty(gAU, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &f, sizeof f);
    AURenderCallbackStruct cb = { InCB, NULL }; AudioUnitSetProperty(gAU, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &cb, sizeof cb);
    gABL = calloc(1, sizeof(AudioBufferList)); gABL->mNumberBuffers=1; gABL->mBuffers[0].mNumberChannels=2; gABL->mBuffers[0].mData=malloc(8192*8);
    AudioUnitInitialize(gAU); AudioOutputUnitStart(gAU); printf("[incap] dev=%u pid=%d buf=%u\n", dev, getpid(), gBuf); sleep(sec); AudioOutputUnitStop(gAU);
    if (getenv("RAMP")) AnalyzeRamp(gCap, gFill/2, gBuf); else AnalyzeCommon(gCap, gFill/2, gBuf, 0.9, 2*M_PI*1000.0/48000.0); return 0;
}
