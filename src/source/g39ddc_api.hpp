// g39ddc_api.hpp - the few calls of WiNRADiO's G39DDC API that FoxSDR makes, declared HERE from the maker's published document, not from the maker's header.
//
// SOURCE. "WiNRADiO G39DDC SDK - API Documentation", https://winradio.com/home/g39ddc_sdk-doc.htm (the non-object-oriented interface), read on
// 2026-11-12: the C declarations, structure listings, "Remarks" and the statement that the library is loaded with LoadLibrary / LoadLibraryEx and
// its functions fetched with GetProcAddress by their plain names. The page states no licence and asks for no agreement. The maker's header
// (G39DDCAPI.h, a zip on the same site) was NOT opened and nothing of it is in this tree.
//
// WHAT THE DOCUMENT DOES NOT GIVE, therefore not used: the numeric values of G39DDC_OPEN_FIRST, G39DDC_OPEN_DEMO, G39DDC_ATTENUATOR_* and the
// DDC1 mode constants (they are named in the document and defined only in the header). The source opens a receiver by the serial number read from
// GetDeviceList, never with OPEN_FIRST, and offers no attenuator.
//
// ABI SAFETY. The one structure FoxSDR allocates for the library to fill, G39DDC_DEVICE_INFO, is declared packed to the document's listing
// (kDeviceInfoBytes = 315). GetDeviceList reports the size of one item it writes; the source REFUSES the library when that size is not 315, so a
// different build can never be read through this layout.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_G39DDC_API_HPP
#define CASCADE_SOURCE_G39DDC_API_HPP

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define FOX_G39_CALL __stdcall
#else
#define FOX_G39_CALL
#endif

namespace cascade::source::g39ddc {

using Bool = std::int32_t;  // BOOL
using Handle = std::int32_t;  // INT32 hDevice; negative = failure

#pragma pack(push, 1)
// "G39DDC_DEVICE_INFO" as the document lists it.
struct DeviceInfo {
    char devicePath[260];  // MAX_PATH
    std::uint8_t interfaceType;
    char serialNumber[9];
    std::uint16_t hwVersion;
    std::uint16_t fwVersion;
    std::uint8_t eepromVersion;
    std::uint32_t channelCount;
    std::uint64_t frontEndMinFrequency;
    std::uint64_t frontEndMaxFrequency;
    std::uint64_t frontEndMaxHFFrequency;
    std::uint32_t frontEndFrequencyStep;
    std::uint32_t frontEndWindowWidth;
    std::uint32_t hardwareOptions;
};
// "G39DDC_DDC_INFO"
struct DdcInfo {
    std::uint32_t sampleRate;
    std::uint32_t bandwidth;
    std::uint32_t bitsPerSample;
};
#pragma pack(pop)
inline constexpr std::size_t kDeviceInfoBytes = 260 + 1 + 9 + 2 + 2 + 1 + 4 + 8 + 8 + 8 + 4 + 4 + 4;
static_assert(sizeof(DeviceInfo) == kDeviceInfoBytes, "the document's G39DDC_DEVICE_INFO listing");
static_assert(sizeof(DdcInfo) == 12, "the document's G39DDC_DDC_INFO listing");

// void __stdcall DDC1StreamCallback(UINT32 Channel, CONST VOID *Buffer, UINT32 NumberOfSamples, UINT32 BitsPerSample, DWORD_PTR UserData)
using Ddc1Callback = void(FOX_G39_CALL*)(std::uint32_t channel, const void* buffer, std::uint32_t numberOfSamples, std::uint32_t bitsPerSample,
                                         std::uintptr_t userData);

#pragma pack(push, 1)
// "G39DDC_CALLBACKS": eight function pointers, in the document's order. Only DDC1StreamCallback is ever set; the rest stay NULL.
struct Callbacks {
    void* ifCallback = nullptr;
    Ddc1Callback ddc1StreamCallback = nullptr;
    void* ddc1PlaybackStreamCallback = nullptr;
    void* ddc2StreamCallback = nullptr;
    void* ddc2PreprocessedStreamCallback = nullptr;
    void* audioStreamCallback = nullptr;
    void* audioPlaybackStreamCallback = nullptr;
    void* sweepingCallback = nullptr;
};
#pragma pack(pop)
static_assert(sizeof(Callbacks) == 8 * sizeof(void*), "eight pointers");

// The document's declarations, one function pointer type each.
using GetDeviceListFn = Bool(FOX_G39_CALL*)(DeviceInfo* list, std::uint32_t bufferSize, std::uint32_t* count, std::uint32_t* itemSize);
using OpenDeviceFn = Handle(FOX_G39_CALL*)(const char* serialNumber);
using CloseDeviceFn = Bool(FOX_G39_CALL*)(Handle h);
using IsDeviceConnectedFn = Bool(FOX_G39_CALL*)(Handle h);
using GetDeviceInfoFn = Bool(FOX_G39_CALL*)(Handle h, DeviceInfo* info, std::uint32_t bufferLength);
using SetPowerFn = Bool(FOX_G39_CALL*)(Handle h, Bool power);
using SetFrontEndFrequencyFn = Bool(FOX_G39_CALL*)(Handle h, std::uint64_t frequency);
using SetPreamplifierFn = Bool(FOX_G39_CALL*)(Handle h, Bool preamp);
using GetDdcInfoFn = Bool(FOX_G39_CALL*)(std::uint32_t ddcTypeIndex, DdcInfo* info);
using GetDdc1CountFn = Bool(FOX_G39_CALL*)(Handle h, std::uint32_t channel, std::uint32_t* count);
using SetDdc1Fn = Bool(FOX_G39_CALL*)(Handle h, std::uint32_t channel, std::uint32_t ddcTypeIndex);
using SetDdc1FrequencyFn = Bool(FOX_G39_CALL*)(Handle h, std::uint32_t channel, std::int32_t frequency);
using StartDdc1Fn = Bool(FOX_G39_CALL*)(Handle h, std::uint32_t channel, std::uint32_t samplesPerBuffer);
using StopDdc1Fn = Bool(FOX_G39_CALL*)(Handle h, std::uint32_t channel);
using SetCallbacksFn = Bool(FOX_G39_CALL*)(Handle h, const Callbacks* callbacks, std::uintptr_t userData);

// The exported names (plain, as the document's "Address retrieval" lines show). Required ones first; the optional ones are used when present.
inline constexpr const char* kRequired[] = {"GetDeviceList", "OpenDevice",   "CloseDevice", "GetDeviceInfo", "SetPower",
                                            "SetFrontEndFrequency", "GetDDCInfo", "GetDDC1Count", "SetDDC1",     "SetDDC1Frequency",
                                            "StartDDC1",     "StopDDC1",   "SetCallbacks"};
inline constexpr const char* kOptional[] = {"IsDeviceConnected", "SetPreamplifier"};

struct Api {
    GetDeviceListFn GetDeviceList = nullptr;
    OpenDeviceFn OpenDevice = nullptr;
    CloseDeviceFn CloseDevice = nullptr;
    GetDeviceInfoFn GetDeviceInfo = nullptr;
    SetPowerFn SetPower = nullptr;
    SetFrontEndFrequencyFn SetFrontEndFrequency = nullptr;
    GetDdcInfoFn GetDDCInfo = nullptr;
    GetDdc1CountFn GetDDC1Count = nullptr;
    SetDdc1Fn SetDDC1 = nullptr;
    SetDdc1FrequencyFn SetDDC1Frequency = nullptr;
    StartDdc1Fn StartDDC1 = nullptr;
    StopDdc1Fn StopDDC1 = nullptr;
    SetCallbacksFn SetCallbacks = nullptr;
    IsDeviceConnectedFn IsDeviceConnected = nullptr;   // optional
    SetPreamplifierFn SetPreamplifier = nullptr;       // optional
};

}  // namespace cascade::source::g39ddc

#endif  // CASCADE_SOURCE_G39DDC_API_HPP
