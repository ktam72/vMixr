// Mixr audio server driver (BlackHole-style virtual loopback device set).
// Implemented from Apple's public CoreAudio headers only (no BlackHole source).
//
// The driver presents four full-duplex stereo devices:
//   - vMixr 1 .. vMixr 4 : each 2ch (stereo), 32-bit float
// All four devices share one sample rate: 44100 / 48000 / 96000 Hz
// (default 48000), settable per device (REQ-106).
// Each device has one output stream and one input stream. Audio played into a
// device's output stream is routed to the same device's input stream (loopback),
// so an app that captures from the device hears what other apps played into it.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardwareBase.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreFoundation/CFUUID.h>
#include <mach/mach_time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>

// ---- diagnostic logging (writes to a world-readable path) ----
static void MixrLogf(const char* fmt, ...) {
    static FILE* sLog = NULL;
    if (sLog == NULL) {
        sLog = fopen("/tmp/mixr_driver.log", "a");
        if (sLog != NULL) setvbuf(sLog, NULL, _IOLBF, 0);
    }
    if (sLog != NULL) {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        fprintf(sLog, "%s\n", buf);
    }
}

// Object model: one plug-in, one box, and four full-duplex stereo devices.
// Each device has one output stream and one input stream.
//   device N (N=0..3): object 3+3N (device), 4+3N (out stream), 5+3N (in stream)
//   device 0: 3,4,5    device 1: 6,7,8    device 2: 9,10,11    device 3: 12,13,14
// REQ-107: volume and mute are exposed as Control objects (the modern HAL
// model; the built-in device presents the same structure). Eight control
// objects per device (15+8N..22+8N):
//   role 0..2 : output volume control (main / left / right)
//   role 3    : output mute control
//   role 4..6 : input volume control (main / left / right)
//   role 7    : input mute control
// A control's element (celm) is 0 (main), 1 (left) or 2 (right).
#define kMixrFirstControlID      15
#define kMixrControlsPerDevice   8
// REQ-106: the rates the devices support. All four devices share one rate
// (default 48000 Hz); any device accepts only these three values.
#define kMixrDefaultSampleRate     48000.0
static const Float64 kMixrSupportedRates[3] = { 44100.0, 48000.0, 96000.0 };
#define kMixrSupportedRateCount    3
#define kMixrChannelCount          4      // number of devices
#define kMixrDeviceChannels        2      // each device is stereo
#define kMixrBoxObjectID           2
#define kMixrFirstDeviceID         3
// REQ-105: 1024 frames (4x the measured 256-frame HAL IO buffer). The read
// position is one buffer behind the write head, so the ring size only bounds
// the zero-timestamp period (21.3 ms at 48 kHz), not the loopback delay.
#define kMixrRingFrameCount        1024
#define kMixrBitsPerSample         32
#define kMixrDeviceCount           kMixrChannelCount
#define kMixrStreamCount           (2 * kMixrChannelCount)

#define kMixrNameCString           "vMixr"
#define kMixrBoxUID                "vMixrBox_UID"

// ---------------------------------------------------------------------------
// Object (the CFPlugIn instance). Its first members mirror the
// AudioServerPlugInDriverInterface layout so the host can treat the object
// as the vtable.
// ---------------------------------------------------------------------------
struct MixrDriver {
    // ---- AudioServerPlugInDriverInterface (COM vtable) ----
    void*                                 _reserved;
    HRESULT   (STDMETHODCALLTYPE *QueryInterface)   (void* inDriver, REFIID inUUID, LPVOID* outInterface);
    ULONG     (STDMETHODCALLTYPE *AddRef)           (void* inDriver);
    ULONG     (STDMETHODCALLTYPE *Release)          (void* inDriver);
    OSStatus  (STDMETHODCALLTYPE *Initialize)       (AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
    OSStatus  (STDMETHODCALLTYPE *CreateDevice)     (AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
    OSStatus  (STDMETHODCALLTYPE *DestroyDevice)    (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
    OSStatus  (STDMETHODCALLTYPE *AddDeviceClient)  (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
    OSStatus  (STDMETHODCALLTYPE *RemoveDeviceClient)(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
    OSStatus  (STDMETHODCALLTYPE *PerformDeviceConfigurationChange) (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
    OSStatus  (STDMETHODCALLTYPE *AbortDeviceConfigurationChange)   (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
    Boolean   (STDMETHODCALLTYPE *HasProperty)      (AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
    OSStatus  (STDMETHODCALLTYPE *IsPropertySettable)(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
    OSStatus  (STDMETHODCALLTYPE *GetPropertyDataSize)(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
    OSStatus  (STDMETHODCALLTYPE *GetPropertyData)  (AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
    OSStatus  (STDMETHODCALLTYPE *SetPropertyData)  (AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData);
    OSStatus  (STDMETHODCALLTYPE *StartIO)          (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
    OSStatus  (STDMETHODCALLTYPE *StopIO)           (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
    OSStatus  (STDMETHODCALLTYPE *GetZeroTimeStamp) (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
    OSStatus  (STDMETHODCALLTYPE *WillDoIOOperation)(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace);
    OSStatus  (STDMETHODCALLTYPE *BeginIOOperation) (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
    OSStatus  (STDMETHODCALLTYPE *DoIOOperation)    (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
    OSStatus  (STDMETHODCALLTYPE *EndIOOperation)   (AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

    // ---- state ----
    uint32_t                              refCount;
    AudioServerPlugInHostRef              host;
    uint32_t                              clientCount;
    // Per-stream active flag. The HAL sets this when it opens the device and
    // reads it back, so it has to be real state, not a constant.
    // Slot 2*D is device D's out stream, 2*D+1 its in stream.
    bool                                  streamActive[kMixrStreamCount];
    // The HAL acquires and releases the box, so this is state, not a constant.
    bool                                  boxAcquired;
    // REQ-106: the rate shared by all four devices. Must be one of
    // kMixrSupportedRates; changing it updates hostTicksPerFrame and the
    // advertised stream formats.
    Float64                               sampleRate;
    Float64                               hostClockFrequency;
    // Per-device timeline anchor for GetZeroTimeStamp. The HAL builds each
    // device's sample clock from these, so they advance one ring buffer at a
    // time. Indexed by device (0..3).
    Float64                               hostTicksPerFrame;
    UInt64                                anchorHostTime[kMixrChannelCount];
    uint32_t                              timeStampCount[kMixrChannelCount];
    // Realtime-safe counters: plain increments on the IO thread, read from the
    // property/StopIO path. Never log from the IO thread itself.
    uint32_t                              ioCycles;
    uint32_t                              ioFramesWritten;
    uint32_t                              ioFramesRead;
    // Per-device output ring buffer. Each device D has its own 2-channel ring
    // holding its own output. Device D's input is its OWN ring (self loopback),
    // so audio sent to D's output appears on D's own input. Cross-device
    // routing is left to the host mixer app.
    // Index: D * (2 * R) + C * R + position. ringHead[D] is D's next write pos.
    float                                 ringBuffer[kMixrChannelCount * kMixrDeviceChannels * kMixrRingFrameCount];
    uint32_t                              ringHead[kMixrChannelCount];
    // REQ-107: per-device volume (default 1.0) and per-scope mute. Index 0 is
    // the main (whole-device) volume, index 1 the left channel, index 2 the
    // right channel. The effective gain of output channel c is
    // outVolume[d][0] * outVolume[d][c+1], applied before the ring write, so
    // the loopback capture is attenuated too. The input gain
    // inVolume[d][0] * inVolume[d][c+1] is applied at capture time.
    float                                 outVolume[kMixrChannelCount][3];
    float                                 inVolume[kMixrChannelCount][3];
    bool                                  outMuted[kMixrChannelCount];
    bool                                  inMuted[kMixrChannelCount];
};
typedef struct MixrDriver MixrDriver;

static MixrDriver* MixrObject(AudioServerPlugInDriverRef inDriver) {
    return (MixrDriver*)(*inDriver);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool MixrRateSupported(Float64 rate) {
    for (int i = 0; i < kMixrSupportedRateCount; i++) {
        if (rate == kMixrSupportedRates[i]) return true;
    }
    return false;
}

static void MixrMakeASBD(Float64 rate, AudioStreamBasicDescription* asbd) {
    // Interleaved float32 stereo: every stream carries two channels.
    memset(asbd, 0, sizeof(*asbd));
    asbd->mSampleRate      = rate;
    asbd->mFormatID        = kAudioFormatLinearPCM;
    asbd->mFormatFlags     = kAudioFormatFlagsNativeEndian
                            | kAudioFormatFlagIsFloat
                            | kAudioFormatFlagIsPacked;
    asbd->mBytesPerPacket  = (kMixrBitsPerSample / 8) * kMixrDeviceChannels;
    asbd->mFramesPerPacket = 1;
    asbd->mBytesPerFrame   = (kMixrBitsPerSample / 8) * kMixrDeviceChannels;
    asbd->mChannelsPerFrame = kMixrDeviceChannels;
    asbd->mBitsPerChannel  = kMixrBitsPerSample;
    asbd->mReserved        = 0;
}

static bool MixrIsDeviceUUID(REFIID inUUID) {
    CFUUIDBytes target = CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID);
    return memcmp(&inUUID, &target, sizeof(CFUUIDBytes)) == 0;
}

static void MixrResetRing(MixrDriver* obj) {
    memset(obj->ringBuffer, 0, sizeof(obj->ringBuffer));
    for (uint32_t d = 0; d < kMixrChannelCount; d++) obj->ringHead[d] = 0;
}

// ---------------------------------------------------------------------------
// Object classification. Maps an AudioObjectID to its kind and, for
// devices/streams, the device index (0..3).
// ---------------------------------------------------------------------------
typedef enum {
    MixrKind_Unknown,
    MixrKind_Plugin,
    MixrKind_Box,
    MixrKind_Device,
    MixrKind_StreamOut,
    MixrKind_StreamIn,
    MixrKind_Control
} MixrObjectKind;

static MixrObjectKind MixrObjectKindOf(AudioObjectID id, int* outDevice) {
    *outDevice = -1;
    if (id == kAudioObjectPlugInObject) return MixrKind_Plugin;
    if (id == kMixrBoxObjectID)         return MixrKind_Box;
    if (id >= kMixrFirstControlID) {
        uint32_t offset = id - kMixrFirstControlID;
        if (offset < kMixrChannelCount * kMixrControlsPerDevice) {
            *outDevice = (int)(offset / kMixrControlsPerDevice);
            return MixrKind_Control;
        }
        return MixrKind_Unknown;
    }
    if (id >= kMixrFirstDeviceID) {
        uint32_t offset = id - kMixrFirstDeviceID;
        uint32_t dev = offset / 3;
        if (dev < kMixrChannelCount) {
            uint32_t role = offset % 3;
            *outDevice = (int)dev;
            if (role == 0) return MixrKind_Device;
            if (role == 1) return MixrKind_StreamOut;
            return MixrKind_StreamIn;
        }
    }
    return MixrKind_Unknown;
}

// REQ-107: control object helpers.
static uint32_t MixrControlRole(AudioObjectID id) {
    return (uint32_t)(id - kMixrFirstControlID) % kMixrControlsPerDevice;
}

static AudioObjectID MixrControlID(int dev, uint32_t role) {
    return (AudioObjectID)(kMixrFirstControlID + kMixrControlsPerDevice * dev + role);
}

static bool MixrControlIsMute(uint32_t role) {
    return role == 3 || role == 7;
}

static bool MixrControlIsInput(uint32_t role) {
    return role >= 4;
}

// Volume roles (0, 1, 2, 4, 5, 6) carry the element: 0 (main), 1 (left), 2 (right).
static uint32_t MixrControlElement(uint32_t role) {
    return MixrControlIsMute(role) ? 0u : (role % 3);
}

// The stream slot (index into streamActive): device D out = 2D, in = 2D+1.
static int MixrStreamSlot(MixrObjectKind kind, int dev) {
    if (kind == MixrKind_StreamOut) return 2 * dev;
    if (kind == MixrKind_StreamIn)  return 2 * dev + 1;
    return -1;
}

static bool MixrIsDevice(MixrObjectKind kind) {
    return kind == MixrKind_Device;
}

static bool MixrIsStream(MixrObjectKind kind) {
    return kind == MixrKind_StreamOut || kind == MixrKind_StreamIn;
}

static AudioObjectID MixrDeviceID(int dev) {
    return (AudioObjectID)(kMixrFirstDeviceID + 3 * dev);
}

static AudioObjectID MixrOutStreamID(int dev) {
    return (AudioObjectID)(kMixrFirstDeviceID + 3 * dev + 1);
}

static AudioObjectID MixrInStreamID(int dev) {
    return (AudioObjectID)(kMixrFirstDeviceID + 3 * dev + 2);
}

// REQ-106: a rate change moves the device's rate and every stream's formats,
// so announce all of them on every device and stream.
static void MixrNotifySampleRateChanged(MixrDriver* obj) {
    if (obj->host == NULL || obj->host->PropertiesChanged == NULL) return;
    for (int dev = 0; dev < kMixrDeviceCount; dev++) {
        AudioObjectPropertyAddress rateAddr = {
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
        obj->host->PropertiesChanged(obj->host, MixrDeviceID(dev), 1, &rateAddr);

        AudioObjectPropertyAddress streamAddrs[4] = {
            { kAudioStreamPropertyPhysicalFormat,                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioStreamPropertyVirtualFormat,                  kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioStreamPropertyAvailablePhysicalFormats,       kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
            { kAudioStreamPropertyAvailableVirtualFormats,        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }
        };
        obj->host->PropertiesChanged(obj->host, MixrOutStreamID(dev), 4, streamAddrs);
        obj->host->PropertiesChanged(obj->host, MixrInStreamID(dev), 4, streamAddrs);
    }
    MixrLogf("rate_changed -> %.0f Hz", obj->sampleRate);
}

// REQ-107: announce a volume change on both the device (the client-side
// property) and the control object that implements it.
static void MixrNotifyVolumeChanged(MixrDriver* obj, int dev, bool isInput, uint32_t element) {
    if (obj->host == NULL || obj->host->PropertiesChanged == NULL) return;
    AudioObjectPropertyScope scope = isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
    AudioObjectPropertyAddress deviceAddr = {
        kAudioDevicePropertyVolumeScalar,
        scope,
        (AudioObjectPropertyElement)element
    };
    obj->host->PropertiesChanged(obj->host, MixrDeviceID(dev), 1, &deviceAddr);
    AudioObjectPropertyAddress controlAddr = {
        kAudioLevelControlPropertyScalarValue,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    obj->host->PropertiesChanged(obj->host, MixrControlID(dev, (isInput ? 4u : 0u) + element), 1, &controlAddr);
    MixrLogf("volume_changed dev=%d scope=%s element=%u", dev, isInput ? "in" : "out", (unsigned)element);
}

// REQ-107: announce a mute change on both the device and the mute control.
static void MixrNotifyMuteChanged(MixrDriver* obj, int dev, bool isInput) {
    if (obj->host == NULL || obj->host->PropertiesChanged == NULL) return;
    AudioObjectPropertyScope scope = isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
    AudioObjectPropertyAddress deviceAddr = {
        kAudioDevicePropertyMute,
        scope,
        kAudioObjectPropertyElementMain
    };
    obj->host->PropertiesChanged(obj->host, MixrDeviceID(dev), 1, &deviceAddr);
    AudioObjectPropertyAddress controlAddr = {
        kAudioBooleanControlPropertyValue,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    obj->host->PropertiesChanged(obj->host, MixrControlID(dev, (isInput ? 7u : 3u)), 1, &controlAddr);
    MixrLogf("mute_changed dev=%d scope=%s", dev, isInput ? "in" : "out");
}

static void MixrDeviceNameString(int dev, char* outBuf, size_t cap) {
    snprintf(outBuf, cap, "vMixr %d", dev + 1);
}

static void MixrDeviceUIDString(int dev, char* outBuf, size_t cap) {
    snprintf(outBuf, cap, "vMixr%d_UID", dev + 1);
}

// Fill an array with all four device object IDs.
static uint32_t MixrFillDeviceIDs(AudioObjectID* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kMixrChannelCount && n < cap; i++) out[n++] = MixrDeviceID((int)i);
    return n;
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------
static ULONG MixrAddRef(void* inDriver);

static HRESULT MixrQueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface) {
    if (inDriver == NULL || outInterface == NULL) return E_INVALIDARG;
    Boolean isDev = MixrIsDeviceUUID(inUUID);
    MixrLogf("query_interface isDev=%d first4=%08X", (int)isDev, *(unsigned*)&inUUID);
    if (isDev) {
        // COM contract: QueryInterface returns a new reference on the interface.
        MixrAddRef(inDriver);
        *outInterface = inDriver;
        return S_OK;
    }
    *outInterface = NULL;
    return E_NOINTERFACE;
}

static ULONG MixrAddRef(void* inDriver) {
    MixrDriver* obj = (MixrDriver*)inDriver;
    return ++obj->refCount;
}

static ULONG MixrRelease(void* inDriver) {
    MixrDriver* obj = (MixrDriver*)inDriver;
    uint32_t newCount = --obj->refCount;
    MixrLogf("release count=%u", (unsigned)newCount);
    if (newCount == 0) {
        free(obj);
        return 0;
    }
    return newCount;
}

// ---------------------------------------------------------------------------
// Basic operations
// ---------------------------------------------------------------------------

static OSStatus MixrInitialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost) {
    MixrDriver* obj = MixrObject(inDriver);
    obj->host = inHost;
    obj->clientCount = 0;
    obj->boxAcquired = true;
    // REQ-106: start at the default rate; the setter moves it.
    obj->sampleRate = kMixrDefaultSampleRate;
    // REQ-107: full gain, not muted.
    for (int d = 0; d < kMixrDeviceCount; d++) {
        obj->outVolume[d][0] = 1.0f;
        obj->outVolume[d][1] = 1.0f;
        obj->outVolume[d][2] = 1.0f;
        obj->inVolume[d][0]  = 1.0f;
        obj->inVolume[d][1]  = 1.0f;
        obj->inVolume[d][2]  = 1.0f;
        obj->outMuted[d] = false;
        obj->inMuted[d]  = false;
    }
    struct mach_timebase_info timeBase;
    mach_timebase_info(&timeBase);
    obj->hostClockFrequency = ((Float64)timeBase.denom / (Float64)timeBase.numer) * 1000000000.0;
    obj->hostTicksPerFrame = obj->hostClockFrequency / obj->sampleRate;
    MixrResetRing(obj);
    MixrLogf("init pid=%d host=%p", (int)getpid(), (void*)inHost);
    // The HAL scans the plug-in's owned objects / device list after this returns,
    // so no PropertiesChanged announcement is needed for initial enumeration.
    return noErr;
}

static OSStatus MixrCreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID) {
    (void)inDescription; (void)inClientInfo;
    if (outDeviceObjectID != NULL) *outDeviceObjectID = kMixrFirstDeviceID;
    MixrLogf("create_device outid=%d", outDeviceObjectID ? (int)*outDeviceObjectID : -1);
    return noErr;
}

static OSStatus MixrDestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID) {
    MixrLogf("destroydevice device=%u", (unsigned)inDeviceObjectID);
    return noErr;
}

static OSStatus MixrAddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    MixrDriver* obj = MixrObject(inDriver);
    (void)inClientInfo;
    obj->clientCount++;
    MixrLogf("addclient device=%u", (unsigned)inDeviceObjectID);
    return noErr;
}

static OSStatus MixrRemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo) {
    MixrDriver* obj = MixrObject(inDriver);
    (void)inClientInfo;
    if (obj->clientCount > 0) obj->clientCount--;
    MixrLogf("removeclient device=%u", (unsigned)inDeviceObjectID);
    return noErr;
}

static OSStatus MixrPerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) {
    (void)inChangeInfo;
    MixrLogf("performconfig device=%u action=%llu", (unsigned)inDeviceObjectID, (unsigned long long)inChangeAction);
    return noErr;
}

static OSStatus MixrAbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo) {
    (void)inDeviceObjectID; (void)inChangeAction; (void)inChangeInfo;
    return noErr;
}

// ---------------------------------------------------------------------------
// Property write helpers (follow the CoreAudio property-data convention:
// report the size, and only write when the caller's buffer is big enough).
// ---------------------------------------------------------------------------
static CFStringRef MixrCFStr(const char* cstr) {
    return CFStringCreateWithCString(NULL, cstr, kCFStringEncodingUTF8);
}

static void MixrWriteCFStr(const char* cstr, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    *outDataSize = sizeof(CFStringRef);
    if (outData != NULL && inDataSize >= sizeof(CFStringRef)) {
        *(CFStringRef*)outData = MixrCFStr(cstr);
    }
}

static void MixrWriteObjID(AudioObjectID id, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    *outDataSize = sizeof(AudioObjectID);
    if (outData != NULL && inDataSize >= sizeof(AudioObjectID)) {
        *(AudioObjectID*)outData = id;
    }
}

static void MixrWriteClass(AudioClassID cls, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    *outDataSize = sizeof(AudioClassID);
    if (outData != NULL && inDataSize >= sizeof(AudioClassID)) {
        *(AudioClassID*)outData = cls;
    }
}

static void MixrWriteU32(UInt32 v, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    *outDataSize = sizeof(UInt32);
    if (outData != NULL && inDataSize >= sizeof(UInt32)) {
        *(UInt32*)outData = v;
    }
}

// Write a fixed list of items, clamped to the caller's buffer.
static void MixrWriteList(const void* items, uint32_t count, size_t itemSize, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    // NullAudio convention: a NULL outData is a size query and reports the full
    // size; otherwise write at most inDataSize bytes (whole items only) and
    // report exactly how many bytes were written. Never write past the buffer.
    uint32_t fullSize = (uint32_t)(count * itemSize);
    if (outData == NULL) {
        *outDataSize = fullSize;
        return;
    }
    uint32_t writable = (uint32_t)((inDataSize / itemSize) * itemSize);
    if (writable > fullSize) {
        writable = fullSize;
    }
    if (writable > 0) {
        memcpy(outData, items, writable);
    }
    *outDataSize = writable;
}

// Write the stream object ID(s) a device exposes, filtered by scope. Each
// device is full-duplex: it has one output stream and one input stream.
//   global scope   -> both streams
//   output scope   -> the out stream only
//   input scope    -> the in stream only
static void MixrWriteDeviceStreams(AudioObjectID deviceID, AudioObjectPropertyScope scope, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    int dev; MixrObjectKind kind = MixrObjectKindOf(deviceID, &dev);
    if (kind != MixrKind_Device) {
        *outDataSize = 0;
        return;
    }
    AudioObjectID streams[2];
    uint32_t n = 0;
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput) {
        streams[n++] = MixrOutStreamID(dev);
    }
    if (scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput) {
        streams[n++] = MixrInStreamID(dev);
    }
    MixrWriteList(streams, n, sizeof(AudioObjectID), inDataSize, outDataSize, outData);
}

// ---------------------------------------------------------------------------
// Property operations
// ---------------------------------------------------------------------------
static OSStatus MixrGetPropertyDataImpl(MixrDriver* obj, AudioObjectID objectID, const AudioObjectPropertyAddress* address, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    AudioObjectPropertySelector selector = address->mSelector;
    AudioObjectPropertyScope scope = address->mScope;
    int index;
    MixrObjectKind kind = MixrObjectKindOf(objectID, &index);

    if (kind == MixrKind_Box) {
        // A box groups the device(s) the driver presents. The HAL walks the
        // plug-in's box list and enumerates each acquired box's device list, so
        // the box must be present and acquired for the device to appear.
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
                MixrWriteClass(kAudioObjectClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyClass:
                MixrWriteClass(kAudioBoxClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyOwner:
                MixrWriteObjID(kAudioObjectPlugInObject, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyManufacturer:
                MixrWriteCFStr(kMixrNameCString, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyOwnedObjects:
                // Boxes don't own anything; the devices are owned by the plug-in.
                *outDataSize = 0;
                return noErr;
            case kAudioObjectPropertyIdentify:
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertySerialNumber:
                MixrWriteCFStr("00000001", inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyFirmwareVersion:
                MixrWriteCFStr("1.0", inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyModelName:
                MixrWriteCFStr(kMixrNameCString, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioBoxPropertyBoxUID:
                MixrWriteCFStr(kMixrBoxUID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioBoxPropertyTransportType:
                MixrWriteU32(kAudioDeviceTransportTypeVirtual, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioBoxPropertyHasAudio:
                MixrWriteU32(1, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioBoxPropertyHasVideo:
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioBoxPropertyHasMIDI:
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioBoxPropertyIsProtected:
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioBoxPropertyAcquired:
                MixrWriteU32(obj->boxAcquired ? 1 : 0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioBoxPropertyAcquisitionFailed:
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioBoxPropertyDeviceList: {
                AudioObjectID items[kMixrDeviceCount];
                uint32_t n = MixrFillDeviceIDs(items, kMixrDeviceCount);
                MixrWriteList(items, n, sizeof(AudioObjectID), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
                MixrWriteCFStr(kMixrNameCString, inDataSize, outDataSize, outData);
                return noErr;
            default:
                break;
        }
    }

    if (kind == MixrKind_Plugin) {
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
                MixrWriteClass(kAudioObjectClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyClass:
                MixrWriteClass(kAudioPlugInClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyOwner:
                MixrWriteObjID(kAudioObjectUnknown, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyManufacturer:
                MixrWriteCFStr(kMixrNameCString, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyOwnedObjects: {
                // The HAL discovers the devices by walking the plug-in's owned
                // objects. Publish the box and all eight devices.
                AudioObjectID items[1 + kMixrDeviceCount];
                items[0] = kMixrBoxObjectID;
                uint32_t n = MixrFillDeviceIDs(items + 1, kMixrDeviceCount) + 1;
                MixrWriteList(items, n, sizeof(AudioObjectID), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioPlugInPropertyBoxList: {
                AudioObjectID items[1] = { kMixrBoxObjectID };
                MixrWriteList(items, 1, sizeof(AudioObjectID), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioPlugInPropertyTranslateUIDToBox:
                MixrWriteObjID(kMixrBoxObjectID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioPlugInPropertyDeviceList: {
                AudioObjectID items[kMixrDeviceCount];
                uint32_t n = MixrFillDeviceIDs(items, kMixrDeviceCount);
                MixrWriteList(items, n, sizeof(AudioObjectID), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioPlugInPropertyTranslateUIDToDevice:
                // Map the default UID to the first device.
                MixrWriteObjID(kMixrFirstDeviceID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioPlugInPropertyResourceBundle:
                MixrWriteCFStr(kMixrNameCString, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
                MixrWriteCFStr(kMixrNameCString, inDataSize, outDataSize, outData);
                return noErr;
            default:
                break;
        }
    }

    if (MixrIsDevice(kind)) {
        char nameBuf[64];
        char uidBuf[64];
        MixrDeviceNameString(index, nameBuf, sizeof(nameBuf));
        MixrDeviceUIDString(index, uidBuf, sizeof(uidBuf));
        bool anyActive = obj->streamActive[2 * index] || obj->streamActive[2 * index + 1];
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
                MixrWriteClass(kAudioObjectClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyClass:
                MixrWriteClass(kAudioDeviceClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyOwner:
                MixrWriteObjID(kAudioObjectPlugInObject, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyManufacturer:
                MixrWriteCFStr(kMixrNameCString, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyOwnedObjects: {
                // Both streams and the eight controls belong to the device.
                AudioObjectID items[2 + kMixrControlsPerDevice];
                uint32_t n = 0;
                items[n++] = MixrOutStreamID(index);
                items[n++] = MixrInStreamID(index);
                for (uint32_t i = 0; i < kMixrControlsPerDevice; i++) items[n++] = MixrControlID(index, i);
                MixrWriteList(items, n, sizeof(AudioObjectID), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioDevicePropertyDeviceUID:
                MixrWriteCFStr(uidBuf, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyModelUID: {
                char modelBuf[80];
                snprintf(modelBuf, sizeof(modelBuf), "%s_ModelUID", uidBuf);
                MixrWriteCFStr(modelBuf, inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioDevicePropertyTransportType:
                MixrWriteU32(kAudioDeviceTransportTypeVirtual, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyRelatedDevices: {
                AudioObjectID items[1] = { objectID };
                MixrWriteList(items, 1, sizeof(AudioObjectID), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioDevicePropertyClockDomain:
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyDeviceIsAlive:
                MixrWriteU32(1, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyDeviceIsRunning:
                MixrWriteU32(anyActive ? 1 : 0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
                MixrWriteU32(1, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
                MixrWriteU32(1, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyLatency:
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyMute: {
                // REQ-107: scope-specific mute (output or input).
                if (scope != kAudioObjectPropertyScopeOutput && scope != kAudioObjectPropertyScopeInput) {
                    *outDataSize = 0;
                    return kAudioHardwareIllegalOperationError;
                }
                bool muted = (scope == kAudioObjectPropertyScopeOutput) ? obj->outMuted[index] : obj->inMuted[index];
                MixrWriteU32(muted ? 1 : 0, inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioObjectPropertyControlList: {
                // REQ-107: list the device's volume and mute controls. The
                // modern HAL derives the client-side volume/mute properties
                // from this list (the built-in device presents the same shape,
                // and returns the same list for every scope).
                AudioObjectID items[kMixrControlsPerDevice];
                for (uint32_t i = 0; i < kMixrControlsPerDevice; i++) items[i] = MixrControlID(index, i);
                MixrWriteList(items, kMixrControlsPerDevice, sizeof(AudioObjectID), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioDevicePropertyVolumeScalar: {
                // REQ-107: the modern client addresses per-channel volume as
                // device scope plus element: main (0) for the whole-device
                // volume, 1 (left) and 2 (right) for the channel volumes.
                if (scope != kAudioObjectPropertyScopeOutput && scope != kAudioObjectPropertyScopeInput) {
                    *outDataSize = 0;
                    return kAudioHardwareIllegalOperationError;
                }
                if (address->mElement > 2) {
                    *outDataSize = 0;
                    return kAudioHardwareUnknownPropertyError;
                }
                const float* vols = (scope == kAudioObjectPropertyScopeOutput) ? obj->outVolume[index] : obj->inVolume[index];
                Float32 v = vols[address->mElement];
                *outDataSize = sizeof(Float32);
                if (outData != NULL && inDataSize >= sizeof(Float32)) *(Float32*)outData = v;
                return noErr;
            }
            case kAudioDevicePropertyNominalSampleRate: {
                // REQ-106: the rate shared by all four devices.
                Float64 rate = obj->sampleRate;
                *outDataSize = sizeof(Float64);
                if (outData != NULL && inDataSize >= sizeof(Float64)) *(Float64*)outData = rate;
                return noErr;
            }
            case kAudioDevicePropertyAvailableNominalSampleRates: {
                MixrWriteList(kMixrSupportedRates, kMixrSupportedRateCount,
                             sizeof(Float64), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioDevicePropertyIsHidden:
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyZeroTimeStampPeriod:
                MixrWriteU32(kMixrRingFrameCount, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyStreams:
                MixrWriteDeviceStreams(objectID, scope, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyStreamConfiguration: {
                // One buffer carrying the device's two channels.
                UInt32 ablSize = sizeof(AudioBufferList) + kMixrDeviceChannels * sizeof(AudioBuffer);
                *outDataSize = ablSize;
                if (outData != NULL && inDataSize >= ablSize) {
                    AudioBufferList* abl = (AudioBufferList*)outData;
                    memset(abl, 0, ablSize);
                    abl->mNumberBuffers = 1;
                    abl->mBuffers[0].mNumberChannels = kMixrDeviceChannels;
                    abl->mBuffers[0].mDataByteSize = 0;
                    abl->mBuffers[0].mData = NULL;
                }
                return noErr;
            }
            case kAudioDevicePropertySafetyOffset:
                // Loopback through a ring buffer, so the HAL can read and write right up to now.
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioDevicePropertyPreferredChannelsForStereo: {
                UInt32 ch[2] = { 1, 2 };
                MixrWriteList(ch, 2, sizeof(UInt32), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioDevicePropertyPreferredChannelLayout: {
                // A 2-channel stereo layout.
                UInt32 size = offsetof(AudioChannelLayout, mChannelDescriptions) + kMixrDeviceChannels * sizeof(AudioChannelDescription);
                *outDataSize = size;
                if (outData != NULL && inDataSize >= size) {
                    AudioChannelLayout* layout = (AudioChannelLayout*)outData;
                    memset(layout, 0, size);
                    layout->mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
                    layout->mChannelBitmap = 0;
                    layout->mNumberChannelDescriptions = kMixrDeviceChannels;
                    layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Left;
                    layout->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
                }
                return noErr;
            }
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
                MixrWriteCFStr(nameBuf, inDataSize, outDataSize, outData);
                return noErr;
            default:
                break;
        }
    }

    if (MixrIsStream(kind)) {
        bool isInput = (kind == MixrKind_StreamIn);
        int slot = MixrStreamSlot(kind, index);
        AudioObjectID deviceID = MixrDeviceID(index);
        char nameBuf[64];
        MixrDeviceNameString(index, nameBuf, sizeof(nameBuf));
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
                MixrWriteClass(kAudioObjectClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyClass:
                MixrWriteClass(kAudioStreamClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyOwner:
                MixrWriteObjID(deviceID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
                MixrWriteCFStr(nameBuf, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioStreamPropertyIsActive:
                MixrWriteU32(obj->streamActive[slot] ? 1 : 0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioStreamPropertyLatency:
                // The loopback adds no latency beyond the IO buffer itself.
                MixrWriteU32(0, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioStreamPropertyTerminalType:
                MixrWriteU32(isInput ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioStreamPropertyAvailablePhysicalFormats:
            case kAudioStreamPropertyAvailableVirtualFormats: {
                // REQ-106: float32 stereo at any of the supported rates.
                AudioStreamRangedDescription range;
                memset(&range, 0, sizeof(range));
                MixrMakeASBD(obj->sampleRate, &range.mFormat);
                range.mSampleRateRange.mMinimum = kMixrSupportedRates[0];
                range.mSampleRateRange.mMaximum = kMixrSupportedRates[kMixrSupportedRateCount - 1];
                MixrWriteList(&range, 1, sizeof(range), inDataSize, outDataSize, outData);
                return noErr;
            }
            case kAudioStreamPropertyDirection:
                MixrWriteU32(isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioStreamPropertyPhysicalFormat:
            case kAudioStreamPropertyVirtualFormat: {
                AudioStreamBasicDescription asbd;
                MixrMakeASBD(obj->sampleRate, &asbd);
                *outDataSize = sizeof(asbd);
                if (outData != NULL && inDataSize >= sizeof(asbd)) memcpy(outData, &asbd, sizeof(asbd));
                return noErr;
            }
            case kAudioStreamPropertyStartingChannel:
                // Both streams of a device start at channel 1 (stereo).
                MixrWriteU32(1, inDataSize, outDataSize, outData);
                return noErr;
            default:
                break;
        }
    }

    // REQ-107: a Control object (the modern HAL model). The device's volume
    // and mute are implemented by these objects, and the client-side device
    // volume/mute properties are reachable because they are listed in the
    // device's ControlList.
    if (kind == MixrKind_Control) {
        uint32_t role = MixrControlRole(objectID);
        bool isMute = MixrControlIsMute(role);
        bool isInput = MixrControlIsInput(role);
        uint32_t element = MixrControlElement(role);
        char nameBuf[32];
        const char* side = isInput ? "Input" : "Output";
        const char* what = isMute ? "Mute"
            : (element == 0) ? "Volume"
            : (element == 1) ? "Volume Left"
            : "Volume Right";
        snprintf(nameBuf, sizeof(nameBuf), "%s %s", side, what);
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
                MixrWriteClass(isMute ? kAudioBooleanControlClassID : kAudioLevelControlClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyClass:
                MixrWriteClass(isMute ? kAudioMuteControlClassID : kAudioVolumeControlClassID, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyOwner:
                MixrWriteObjID(MixrDeviceID(index), inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyManufacturer:
                MixrWriteCFStr(kMixrNameCString, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
                MixrWriteCFStr(nameBuf, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioControlPropertyScope:
                MixrWriteU32(isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioControlPropertyElement:
                MixrWriteU32(element, inDataSize, outDataSize, outData);
                return noErr;
            case kAudioLevelControlPropertyScalarValue: {
                if (isMute) break;
                const float* vols = isInput ? obj->inVolume[index] : obj->outVolume[index];
                Float32 v = vols[element];
                *outDataSize = sizeof(Float32);
                if (outData != NULL && inDataSize >= sizeof(Float32)) *(Float32*)outData = v;
                return noErr;
            }
            case kAudioBooleanControlPropertyValue: {
                if (!isMute) break;
                bool muted = isInput ? obj->inMuted[index] : obj->outMuted[index];
                MixrWriteU32(muted ? 1 : 0, inDataSize, outDataSize, outData);
                return noErr;
            }
            default:
                break;
        }
    }

    *outDataSize = 0;
    return kAudioHardwareUnknownPropertyError;
}

static Boolean MixrHasPropertyImpl(MixrDriver* obj, AudioObjectID objectID, const AudioObjectPropertyAddress* address) {
    AudioObjectPropertySelector selector = address->mSelector;
    int index;
    MixrObjectKind kind = MixrObjectKindOf(objectID, &index);
    (void)obj;
    if (kind == MixrKind_Box) {
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
            case kAudioObjectPropertyIdentify:
            case kAudioObjectPropertySerialNumber:
            case kAudioObjectPropertyFirmwareVersion:
            case kAudioObjectPropertyModelName:
            case kAudioBoxPropertyBoxUID:
            case kAudioBoxPropertyTransportType:
            case kAudioBoxPropertyHasAudio:
            case kAudioBoxPropertyHasVideo:
            case kAudioBoxPropertyHasMIDI:
            case kAudioBoxPropertyIsProtected:
            case kAudioBoxPropertyAcquired:
            case kAudioBoxPropertyAcquisitionFailed:
            case kAudioBoxPropertyDeviceList:
                return true;
            default:
                break;
        }
    }
    if (kind == MixrKind_Plugin) {
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
            case kAudioPlugInPropertyBoxList:
            case kAudioPlugInPropertyTranslateUIDToBox:
            case kAudioPlugInPropertyDeviceList:
            case kAudioPlugInPropertyTranslateUIDToDevice:
            case kAudioPlugInPropertyResourceBundle:
                return true;
            default:
                break;
        }
    }
    if (MixrIsDevice(kind)) {
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyOwnedObjects:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
            case kAudioDevicePropertyDeviceUID:
            case kAudioDevicePropertyModelUID:
            case kAudioDevicePropertyTransportType:
            case kAudioDevicePropertyRelatedDevices:
            case kAudioDevicePropertyClockDomain:
            case kAudioDevicePropertyDeviceIsAlive:
            case kAudioDevicePropertyDeviceIsRunning:
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
            case kAudioDevicePropertyLatency:
            case kAudioDevicePropertyMute:
            case kAudioObjectPropertyControlList:
            case kAudioDevicePropertyVolumeScalar:
            case kAudioDevicePropertyNominalSampleRate:
            case kAudioDevicePropertyAvailableNominalSampleRates:
            case kAudioDevicePropertyIsHidden:
            case kAudioDevicePropertyZeroTimeStampPeriod:
            case kAudioDevicePropertyStreams:
            case kAudioDevicePropertyStreamConfiguration:
            case kAudioDevicePropertySafetyOffset:
            case kAudioDevicePropertyPreferredChannelsForStereo:
            case kAudioDevicePropertyPreferredChannelLayout:
                return true;
            default:
                break;
        }
    }
    if (MixrIsStream(kind)) {
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
            case kAudioStreamPropertyDirection:
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyLatency:
            case kAudioStreamPropertyTerminalType:
            case kAudioStreamPropertyPhysicalFormat:
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyAvailablePhysicalFormats:
            case kAudioStreamPropertyAvailableVirtualFormats:
            case kAudioStreamPropertyStartingChannel:
                return true;
            default:
                break;
        }
    }
    // REQ-107: Control objects expose their class identity, scope, element,
    // and value. Volume controls carry the scalar value; mute controls carry
    // the boolean value.
    if (kind == MixrKind_Control) {
        uint32_t role = MixrControlRole(objectID);
        switch (selector) {
            case kAudioObjectPropertyBaseClass:
            case kAudioObjectPropertyClass:
            case kAudioObjectPropertyOwner:
            case kAudioObjectPropertyManufacturer:
            case kAudioObjectPropertyName:
            case kAudioObjectPropertyElementName:
            case kAudioControlPropertyScope:
            case kAudioControlPropertyElement:
                return true;
            case kAudioLevelControlPropertyScalarValue:
                return !MixrControlIsMute(role);
            case kAudioBooleanControlPropertyValue:
                return MixrControlIsMute(role);
            default:
                break;
        }
    }
    return false;
}

static Boolean MixrHasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID objectID, pid_t clientPID, const AudioObjectPropertyAddress* address) {
    MixrDriver* obj = MixrObject(inDriver);
    (void)clientPID;
    Boolean r = MixrHasPropertyImpl(obj, objectID, address);
    MixrLogf("hasproperty obj=%u sel=%.4s/%08x sc=%.4s/%08x el=%u r=%d", (unsigned)objectID, (const char*)&address->mSelector, (unsigned)address->mSelector, (const char*)&address->mScope, (unsigned)address->mScope, (unsigned)address->mElement, (int)r);
    return r;
}

static OSStatus MixrIsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID objectID, pid_t clientPID, const AudioObjectPropertyAddress* address, Boolean* outIsSettable) {
    (void)clientPID;
    if (outIsSettable != NULL) *outIsSettable = false;
    int index;
    MixrObjectKind kind = MixrObjectKindOf(objectID, &index);
    // REQ-106: the nominal sample rate is settable on every device (all four
    // devices share the one rate).
    if (MixrIsDevice(kind) && address->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (outIsSettable != NULL) *outIsSettable = true;
    }
    // REQ-107: the scope-specific mute is settable on every device.
    if (MixrIsDevice(kind) && address->mSelector == kAudioDevicePropertyMute
        && (address->mScope == kAudioObjectPropertyScopeOutput || address->mScope == kAudioObjectPropertyScopeInput)) {
        if (outIsSettable != NULL) *outIsSettable = true;
    }
    // REQ-107: the scope-specific, element-addressed volume is settable.
    if (MixrIsDevice(kind) && address->mSelector == kAudioDevicePropertyVolumeScalar
        && (address->mScope == kAudioObjectPropertyScopeOutput || address->mScope == kAudioObjectPropertyScopeInput)
        && address->mElement <= 2) {
        if (outIsSettable != NULL) *outIsSettable = true;
    }
    // REQ-107: Control objects are settable through their value selector.
    if (kind == MixrKind_Control) {
        uint32_t role = MixrControlRole(objectID);
        if ((MixrControlIsMute(role) && address->mSelector == kAudioBooleanControlPropertyValue)
            || (!MixrControlIsMute(role) && address->mSelector == kAudioLevelControlPropertyScalarValue)) {
            if (outIsSettable != NULL) *outIsSettable = true;
        }
    }
    if (kind == MixrKind_Box && address->mSelector == kAudioBoxPropertyAcquired) {
        if (outIsSettable != NULL) *outIsSettable = true;
    }
    if (MixrIsStream(kind)) {
        switch (address->mSelector) {
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyPhysicalFormat:
            case kAudioStreamPropertyVirtualFormat:
                if (outIsSettable != NULL) *outIsSettable = true;
                break;
            default:
                break;
        }
    }
    MixrLogf("issettable obj=%u sel=%.4s/%08x sc=%.4s/%08x el=%u", (unsigned)objectID, (const char*)&address->mSelector, (unsigned)address->mSelector, (const char*)&address->mScope, (unsigned)address->mScope, (unsigned)address->mElement);
    return noErr;
}

static OSStatus MixrGetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID objectID, pid_t clientPID, const AudioObjectPropertyAddress* address, UInt32 qualifierSize, const void* qualifierData, UInt32* outDataSize) {
    MixrDriver* obj = MixrObject(inDriver);
    (void)clientPID; (void)qualifierSize; (void)qualifierData;
    UInt32 size = 0;
    OSStatus r = MixrGetPropertyDataImpl(obj, objectID, address, 0, &size, NULL);
    if (outDataSize != NULL) {
        *outDataSize = size;
    }
    MixrLogf("getpropdatasize obj=%u sel=%.4s/%08x sc=%.4s/%08x el=%u r=%d size=%u", (unsigned)objectID, (const char*)&address->mSelector, (unsigned)address->mSelector, (const char*)&address->mScope, (unsigned)address->mScope, (unsigned)address->mElement, (int)r, (unsigned)size);
    return r;
}

static OSStatus MixrGetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID objectID, pid_t clientPID, const AudioObjectPropertyAddress* address, UInt32 qualifierSize, const void* qualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData) {
    MixrDriver* obj = MixrObject(inDriver);
    (void)clientPID; (void)qualifierSize; (void)qualifierData;
    OSStatus r = MixrGetPropertyDataImpl(obj, objectID, address, inDataSize, outDataSize, outData);
    unsigned osize = (outDataSize != NULL) ? (unsigned)*outDataSize : 0xFFFFFFFFu;
    unsigned outnull = (outData == NULL) ? 1 : 0;
    unsigned first4 = (outData != NULL && inDataSize >= 4 && outDataSize != NULL && *outDataSize >= 4) ? *(unsigned*)outData : 0xdeadbeef;
    MixrLogf("getpropdata obj=%u sel=%.4s/%08x sc=%.4s/%08x el=%u insz=%u osize=%u outnull=%u first4=%08X r=%d", (unsigned)objectID, (const char*)&address->mSelector, (unsigned)address->mSelector, (const char*)&address->mScope, (unsigned)address->mScope, (unsigned)address->mElement, (unsigned)inDataSize, osize, outnull, first4, (int)r);
    return r;
}

static OSStatus MixrSetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID objectID, pid_t clientPID, const AudioObjectPropertyAddress* address, UInt32 qualifierSize, const void* qualifierData, UInt32 dataSize, const void* data) {
    MixrDriver* obj = MixrObject(inDriver);
    (void)clientPID; (void)qualifierSize; (void)qualifierData;
    OSStatus r = kAudioHardwareUnsupportedOperationError;
    int dummyIndex = 0;

    if (objectID == kMixrBoxObjectID && address->mSelector == kAudioBoxPropertyAcquired) {
        if (dataSize != sizeof(UInt32) || data == NULL) {
            r = kAudioHardwareBadPropertySizeError;
        } else {
            bool wanted = (*(const UInt32*)data != 0);
            if (obj->boxAcquired != wanted) {
                obj->boxAcquired = wanted;
                // Acquiring or releasing the box changes which devices exist,
                // so both the box and the plug-in's device list move.
                if (obj->host != NULL && obj->host->PropertiesChanged != NULL) {
                    AudioObjectPropertyAddress boxChanged[2] = {
                        { kAudioBoxPropertyAcquired,   kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain },
                        { kAudioBoxPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain }
                    };
                    AudioObjectPropertyAddress plugInChanged = {
                        kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
                    };
                    obj->host->PropertiesChanged(obj->host, kMixrBoxObjectID, 2, boxChanged);
                    obj->host->PropertiesChanged(obj->host, kAudioObjectPlugInObject, 1, &plugInChanged);
                }
            }
            r = noErr;
        }
    } else if (MixrIsStream(MixrObjectKindOf(objectID, &dummyIndex))
        && address->mSelector == kAudioStreamPropertyIsActive) {
        if (dataSize != sizeof(UInt32) || data == NULL) {
            r = kAudioHardwareBadPropertySizeError;
        } else {
            // coreaudiod (the HAL) sets IsActive itself, so it already knows
            // the new state. Firing a PropertiesChanged notification would make
            // coreaudiod re-enumerate the device, which re-sets IsActive and
            // re-fires the notification — a self-sustaining loop that pegs
            // coreaudiod's CPU and starves the audio I/O (causing glitches).
            // Record the state locally and return noErr without notifying.
            int slot = MixrStreamSlot(MixrObjectKindOf(objectID, &dummyIndex), dummyIndex);
            obj->streamActive[slot] = (*(const UInt32*)data != 0);
            r = noErr;
        }
    } else if (MixrIsDevice(MixrObjectKindOf(objectID, &dummyIndex))
        && address->mSelector == kAudioDevicePropertyNominalSampleRate) {
        // REQ-106: one rate is shared by all four devices. Accept only the
        // supported rates; anything else is rejected so the HAL never sees a
        // "succeeded but unchanged" value (see docs/design.md).
        if (dataSize != sizeof(Float64) || data == NULL) {
            r = kAudioHardwareBadPropertySizeError;
        } else {
            Float64 wanted = *(const Float64*)data;
            if (wanted == obj->sampleRate) {
                r = noErr;
            } else if (!MixrRateSupported(wanted)) {
                r = kAudioHardwareIllegalOperationError;
            } else {
                obj->sampleRate = wanted;
                obj->hostTicksPerFrame = obj->hostClockFrequency / wanted;
                // In-flight audio belongs to the old rate; discard it.
                MixrResetRing(obj);
                MixrNotifySampleRateChanged(obj);
                r = noErr;
            }
        }
    } else if (MixrIsDevice(MixrObjectKindOf(objectID, &dummyIndex))
        && address->mSelector == kAudioDevicePropertyMute) {
        // REQ-107: scope-specific mute (output or input).
        if (address->mScope != kAudioObjectPropertyScopeOutput && address->mScope != kAudioObjectPropertyScopeInput) {
            r = kAudioHardwareIllegalOperationError;
        } else if (dataSize != sizeof(UInt32) || data == NULL) {
            r = kAudioHardwareBadPropertySizeError;
        } else {
            bool wanted = (*(const UInt32*)data != 0);
            bool* flag = (address->mScope == kAudioObjectPropertyScopeOutput) ? &obj->outMuted[dummyIndex] : &obj->inMuted[dummyIndex];
            if (*flag != wanted) {
                *flag = wanted;
                MixrNotifyMuteChanged(obj, dummyIndex, address->mScope == kAudioObjectPropertyScopeInput);
            }
            r = noErr;
        }
    } else if (MixrIsDevice(MixrObjectKindOf(objectID, &dummyIndex))
        && address->mSelector == kAudioDevicePropertyVolumeScalar) {
        // REQ-107: the modern client addresses the volume by device scope plus
        // element (0=main, 1=left, 2=right) as a single Float32.
        if (address->mScope != kAudioObjectPropertyScopeOutput && address->mScope != kAudioObjectPropertyScopeInput) {
            r = kAudioHardwareIllegalOperationError;
        } else if (dataSize != sizeof(Float32) || data == NULL) {
            r = kAudioHardwareBadPropertySizeError;
        } else if (address->mElement > 2) {
            r = kAudioHardwareUnknownPropertyError;
        } else {
            bool isInput = (address->mScope == kAudioObjectPropertyScopeInput);
            float* vols = isInput ? obj->inVolume[dummyIndex] : obj->outVolume[dummyIndex];
            uint32_t element = address->mElement;
            float wanted = *(const Float32*)data;
            if (vols[element] != wanted) {
                vols[element] = wanted;
                MixrNotifyVolumeChanged(obj, dummyIndex, isInput, element);
            }
            r = noErr;
        }
    } else if (MixrObjectKindOf(objectID, &dummyIndex) == MixrKind_Control
        && (address->mSelector == kAudioLevelControlPropertyScalarValue
            || address->mSelector == kAudioBooleanControlPropertyValue)) {
        // REQ-107: a Control object's value, set through its own selector.
        uint32_t role = MixrControlRole(objectID);
        bool isInput = MixrControlIsInput(role);
        if (MixrControlIsMute(role)) {
            if (address->mSelector != kAudioBooleanControlPropertyValue) {
                r = kAudioHardwareUnknownPropertyError;
            } else if (dataSize != sizeof(UInt32) || data == NULL) {
                r = kAudioHardwareBadPropertySizeError;
            } else {
                bool wanted = (*(const UInt32*)data != 0);
                bool* flag = isInput ? &obj->inMuted[dummyIndex] : &obj->outMuted[dummyIndex];
                if (*flag != wanted) {
                    *flag = wanted;
                    MixrNotifyMuteChanged(obj, dummyIndex, isInput);
                }
                r = noErr;
            }
        } else {
            if (address->mSelector != kAudioLevelControlPropertyScalarValue) {
                r = kAudioHardwareUnknownPropertyError;
            } else if (dataSize != sizeof(Float32) || data == NULL) {
                r = kAudioHardwareBadPropertySizeError;
            } else {
                float* vols = isInput ? obj->inVolume[dummyIndex] : obj->outVolume[dummyIndex];
                uint32_t element = MixrControlElement(role);
                float wanted = *(const Float32*)data;
                if (vols[element] != wanted) {
                    vols[element] = wanted;
                    MixrNotifyVolumeChanged(obj, dummyIndex, isInput, element);
                }
                r = noErr;
            }
        }
    } else if (MixrIsStream(MixrObjectKindOf(objectID, &dummyIndex))
               && (address->mSelector == kAudioStreamPropertyPhysicalFormat
                   || address->mSelector == kAudioStreamPropertyVirtualFormat)) {
        // Fixed format: accept a request for the format already in use, reject the rest.
        if (dataSize != sizeof(AudioStreamBasicDescription) || data == NULL) {
            r = kAudioHardwareBadPropertySizeError;
        } else {
            const AudioStreamBasicDescription* asked = (const AudioStreamBasicDescription*)data;
            AudioStreamBasicDescription mine;
            MixrMakeASBD(obj->sampleRate, &mine);
            r = (asked->mSampleRate == mine.mSampleRate
                 && asked->mFormatID == mine.mFormatID
                 && asked->mChannelsPerFrame == mine.mChannelsPerFrame)
                ? noErr : kAudioDeviceUnsupportedFormatError;
        }
    }

    MixrLogf("setproperty obj=%u sel=%.4s/%08x sc=%.4s/%08x el=%u sz=%u r=%d", (unsigned)objectID, (const char*)&address->mSelector, (unsigned)address->mSelector, (const char*)&address->mScope, (unsigned)address->mScope, (unsigned)address->mElement, (unsigned)dataSize, (int)r);
    return r;
}

// ---------------------------------------------------------------------------
// IO operations
// ---------------------------------------------------------------------------
static OSStatus MixrStartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID deviceID, UInt32 clientID) {
    (void)clientID;
    MixrDriver* obj = MixrObject(inDriver);
    // Anchor this device's timeline at the moment IO starts. The ring buffer is
    // left untouched so other devices' audio is not wiped.
    int dev; MixrObjectKind kind = MixrObjectKindOf(deviceID, &dev);
    if (MixrIsDevice(kind)) {
        obj->anchorHostTime[dev] = mach_absolute_time();
        obj->timeStampCount[dev] = 0;
    }
    MixrLogf("startio device=%u ticksPerFrame=%.3f", (unsigned)deviceID, obj->hostTicksPerFrame);
    return noErr;
}

static OSStatus MixrStopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID deviceID, UInt32 clientID) {
    MixrDriver* obj = MixrObject(inDriver);
    MixrLogf("stopio device=%u cycles=%u written=%u read=%u", (unsigned)deviceID, (unsigned)obj->ioCycles, (unsigned)obj->ioFramesWritten, (unsigned)obj->ioFramesRead);
    return noErr;
}

static OSStatus MixrGetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID deviceID, UInt32 clientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed) {
    (void)clientID;
    MixrDriver* obj = MixrObject(inDriver);
    int dev; MixrObjectKind kind = MixrObjectKindOf(deviceID, &dev);
    // The timeline advances one ring buffer per period, anchored at this device's StartIO.
    UInt64 now = mach_absolute_time();
    Float64 ticksPerRing = obj->hostTicksPerFrame * (Float64)kMixrRingFrameCount;
    if (MixrIsDevice(kind)) {
        Float64 nextOffset = ((Float64)(obj->timeStampCount[dev] + 1)) * ticksPerRing;
        if (obj->anchorHostTime[dev] + (UInt64)nextOffset <= now) {
            ++obj->timeStampCount[dev];
        }
        if (outSampleTime != NULL) *outSampleTime = (Float64)(obj->timeStampCount[dev] * kMixrRingFrameCount);
        if (outHostTime != NULL) *outHostTime = obj->anchorHostTime[dev] + (UInt64)(((Float64)obj->timeStampCount[dev]) * ticksPerRing);
    }
    if (outSeed != NULL) *outSeed = 1;
    return noErr;
}

static OSStatus MixrWillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID deviceID, UInt32 clientID, UInt32 operationID, Boolean* outWillDo, Boolean* outWillDoInPlace) {
    (void)deviceID; (void)clientID;
    if (operationID == kAudioServerPlugInIOOperationReadInput || operationID == kAudioServerPlugInIOOperationWriteMix) {
        if (outWillDo != NULL) *outWillDo = true;
        if (outWillDoInPlace != NULL) *outWillDoInPlace = true;
    } else {
        if (outWillDo != NULL) *outWillDo = false;
        if (outWillDoInPlace != NULL) *outWillDoInPlace = true;
    }
    return noErr;
}

static OSStatus MixrBeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID deviceID, UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo* ioCycleInfo) {
    return noErr;
}

static OSStatus MixrDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID deviceID, AudioObjectID streamID, UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo* ioCycleInfo, void* mainBuffer, void* secondaryBuffer) {
    (void)deviceID; (void)clientID; (void)ioCycleInfo; (void)secondaryBuffer;
    MixrDriver* obj = MixrObject(inDriver);
    float* buffer = (float*)mainBuffer;
    uint32_t frames = ioBufferFrameSize;
    if (buffer == NULL) return noErr;

    // The stream carries the device's two channels (interleaved). Each device
    // loops back to itself: its output feeds only its own input.
    int dev; MixrObjectKind kind = MixrObjectKindOf(streamID, &dev);
    if (!MixrIsStream(kind)) return noErr;
    const uint32_t chCount = kMixrDeviceChannels;
    const uint32_t R = kMixrRingFrameCount;
    obj->ioCycles++;
    // REQ-107: per-channel gain, 0 while muted. Plain float reads are safe
    // from the property path (tearing only matters across a whole buffer,
    // which cannot happen here).
    // The main volume (index 0) multiplies the channel volume (index 1=left,
    // index 2=right).
    const float outGain[2] = {
        obj->outMuted[dev] ? 0.0f : obj->outVolume[dev][0] * obj->outVolume[dev][1],
        obj->outMuted[dev] ? 0.0f : obj->outVolume[dev][0] * obj->outVolume[dev][2]
    };
    const float inGain[2] = {
        obj->inMuted[dev] ? 0.0f : obj->inVolume[dev][0] * obj->inVolume[dev][1],
        obj->inMuted[dev] ? 0.0f : obj->inVolume[dev][0] * obj->inVolume[dev][2]
    };
    if (operationID == kAudioServerPlugInIOOperationWriteMix) {
        // out stream: interleaved stereo -> this device's own ring. Overwrite
        // at the write position and advance it (single writer per ring). The
        // output volume is applied before the ring write, so the loopback
        // capture is attenuated too (REQ-107).
        for (uint32_t f = 0; f < frames; f++) {
            uint32_t pos = obj->ringHead[dev];
            for (uint32_t c = 0; c < chCount; c++) {
                obj->ringBuffer[(dev * chCount + c) * R + pos] = buffer[f * chCount + c] * outGain[c];
            }
            obj->ringHead[dev] = (obj->ringHead[dev] + 1) % R;
        }
        obj->ioFramesWritten += frames;
    } else if (operationID == kAudioServerPlugInIOOperationReadInput) {
        // in stream: the device's own output ring feeds its own input, read one
        // buffer behind the write position. Summing every device's ring here
        // would put a device's output back on its own input, which closes a
        // feedback loop as soon as a mixer app reads that input. Cross-device
        // routing is the mixer app's job, not the driver's. The input volume
        // is applied at capture time (REQ-107).
        for (uint32_t f = 0; f < frames; f++) {
            uint32_t pos = (obj->ringHead[dev] + R - frames + f) % R;
            for (uint32_t c = 0; c < chCount; c++) {
                buffer[f * chCount + c] = obj->ringBuffer[(dev * chCount + c) * R + pos] * inGain[c];
            }
        }
        obj->ioFramesRead += frames;
    }
    return noErr;
}

static OSStatus MixrEndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID deviceID, UInt32 clientID, UInt32 operationID, UInt32 ioBufferFrameSize, const AudioServerPlugInIOCycleInfo* ioCycleInfo) {
    return noErr;
}

// ---------------------------------------------------------------------------
// Factory (exported symbol)
// ---------------------------------------------------------------------------
static void MixrVTableInit(MixrDriver* obj) {
    obj->_reserved = obj;
    obj->QueryInterface           = MixrQueryInterface;
    obj->AddRef                   = MixrAddRef;
    obj->Release                  = MixrRelease;
    obj->Initialize               = MixrInitialize;
    obj->CreateDevice             = MixrCreateDevice;
    obj->DestroyDevice            = MixrDestroyDevice;
    obj->AddDeviceClient          = MixrAddDeviceClient;
    obj->RemoveDeviceClient       = MixrRemoveDeviceClient;
    obj->PerformDeviceConfigurationChange = MixrPerformDeviceConfigurationChange;
    obj->AbortDeviceConfigurationChange   = MixrAbortDeviceConfigurationChange;
    obj->HasProperty              = MixrHasProperty;
    obj->IsPropertySettable       = MixrIsPropertySettable;
    obj->GetPropertyDataSize      = MixrGetPropertyDataSize;
    obj->GetPropertyData          = MixrGetPropertyData;
    obj->SetPropertyData          = MixrSetPropertyData;
    obj->StartIO                  = MixrStartIO;
    obj->StopIO                   = MixrStopIO;
    obj->GetZeroTimeStamp         = MixrGetZeroTimeStamp;
    obj->WillDoIOOperation        = MixrWillDoIOOperation;
    obj->BeginIOOperation         = MixrBeginIOOperation;
    obj->DoIOOperation            = MixrDoIOOperation;
    obj->EndIOOperation           = MixrEndIOOperation;
}

void*
vMixr_Create(  CFUUIDRef inFactoryUUID,
              CFMutableDictionaryRef inModuleInfo,
              CFDictionaryRef inPlugInInfo,
              UInt32* outRefcon) {
    (void)inFactoryUUID; (void)inModuleInfo; (void)inPlugInInfo;
    MixrDriver* obj = (MixrDriver*)calloc(1, sizeof(MixrDriver));
    if (obj == NULL) return NULL;
    obj->refCount = 1;
    for (uint32_t i = 0; i < kMixrStreamCount; i++) obj->streamActive[i] = true;
    obj->sampleRate = kMixrDefaultSampleRate;
    // REQ-107: calloc zeroes the volume state, which would mean full mute.
    for (int d = 0; d < kMixrDeviceCount; d++) {
        obj->outVolume[d][0] = 1.0f;
        obj->outVolume[d][1] = 1.0f;
        obj->outVolume[d][2] = 1.0f;
        obj->inVolume[d][0]  = 1.0f;
        obj->inVolume[d][1]  = 1.0f;
        obj->inVolume[d][2]  = 1.0f;
    }
    MixrVTableInit(obj);
    if (outRefcon != NULL) *outRefcon = (UInt32)(uintptr_t)obj;
    MixrLogf("create pid=%d obj=%p", (int)getpid(), (void*)obj);
    return obj;
}
