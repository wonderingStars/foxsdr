// g33ddc_api.hpp - the few calls of WiNRADiO's G33DDC API that FoxSDR makes, declared HERE from the maker's published document, not from the maker's header.
//
// SOURCE. "WiNRADiO G33DDC SDK - API Documentation", https://winradio.com/home/g33ddc_sdk-doc.htm (the non-object-oriented interface), read on
// 2026-11-12: the C declarations, structure listings and "Remarks", and the statement that the library is loaded with LoadLibrary / LoadLibraryEx and
// its functions fetched with GetProcAddress by their plain names. The page states no licence and asks for no agreement. The maker's header
// (G33DDCAPI.h, a zip on the same site) was NOT opened and nothing of it is in this tree.
//
// HOW IT DIFFERS FROM THE G39DDC (g39ddc_api.hpp), all from the two documents: GetDeviceList takes (list, bufferSize) and RETURNS the count (negative on
// failure) and does not report an item size; the DDC calls take no channel and GetDDCInfo takes the handle; the one DDC1 frequency is absolute
// (there is no separate front end); the DDC1 stream callback carries no channel; the callback structure has seven pointers, not eight.
//
// WHAT THE DOCUMENT DOES NOT GIVE, therefore not used: the numeric values of G3XDDC_OPEN_FIRST / _OPEN_DEMO and of the attenuator constants (named in
// the document, defined only in the header) - the receiver is opened by the serial read from GetDeviceList and no attenuator is offered; the tuning
// range of the receiver - not claimed, the library refuses what it cannot tune.
//
// ABI SAFETY. G33DDC_DEVICE_INFO is declared packed to the document's listing (kDeviceInfoBytes = 278). Because GetDeviceList does not report its
// item size, the source gives the library a buffer with slack and a sentinel, checks that the sentinel is intact and that every record is plausible
// (terminated strings, a small channel and DDC count), and refuses the library otherwise.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_G33DDC_API_HPP
#define CASCADE_SOURCE_G33DDC_API_HPP

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define FOX_G33_CALL __stdcall
#else
#define FOX_G33_CALL
#endif

namespace cascade::source::g33ddc {

using Bool = std::int32_t;
using Handle = std::int32_t;  // negative = failure

#pragma pack(push, 1)
// "G33DDC_DEVICE_INFO"
struct DeviceInfo {
    char devicePath[260];  // MAX_PATH
    std::uint8_t interfaceType;
    char serialNumber[9];
    std::uint32_t channelCount;
    std::uint32_t ddcTypeCount;
};
// "G3XDDC_DDC_INFO"
struct DdcInfo {
    std::uint32_t sampleRate;
    std::uint32_t bandwidth;
    std::uint32_t bitsPerSample;
};
#pragma pack(pop)
inline constexpr std::size_t kDeviceInfoBytes = 260 + 1 + 9 + 4 + 4;
static_assert(sizeof(DeviceInfo) == kDeviceInfoBytes, "the document's G33DDC_DEVICE_INFO listing");
static_assert(sizeof(DdcInfo) == 12, "the document's G3XDDC_DDC_INFO listing");

// VOID __stdcall DDC1StreamCallback(CONST VOID *Buffer, UINT32 NumberOfSamples, UINT32 BitsPerSample, DWORD_PTR UserData)
using Ddc1Callback = void(FOX_G33_CALL*)(const void* buffer, std::uint32_t numberOfSamples, std::uint32_t bitsPerSample, std::uintptr_t userData);

#pragma pack(push, 1)
// "G33DDC_CALLBACKS": seven function pointers in the document's order. Only DDC1StreamCallback is ever set.
struct Callbacks {
    void* ifCallback = nullptr;
    Ddc1Callback ddc1StreamCallback = nullptr;
    void* ddc1PlaybackStreamCallback = nullptr;
    void* ddc2StreamCallback = nullptr;
    void* ddc2PreprocessedStreamCallback = nullptr;
    void* audioStreamCallback = nullptr;
    void* audioPlaybackStreamCallback = nullptr;
};
#pragma pack(pop)
static_assert(sizeof(Callbacks) == 7 * sizeof(void*), "seven pointers");

using GetDeviceListFn = std::int32_t(FOX_G33_CALL*)(DeviceInfo* list, std::uint32_t bufferSize);
using OpenDeviceFn = Handle(FOX_G33_CALL*)(const char* serialNumber);
using CloseDeviceFn = Bool(FOX_G33_CALL*)(Handle h);
using IsDeviceConnectedFn = Bool(FOX_G33_CALL*)(Handle h);
using GetDeviceInfoFn = Bool(FOX_G33_CALL*)(Handle h, DeviceInfo* info, std::uint32_t bufferLength);
using SetPowerFn = Bool(FOX_G33_CALL*)(Handle h, Bool power);
using SetPreampFn = Bool(FOX_G33_CALL*)(Handle h, Bool preamp);
using GetDdcInfoFn = Bool(FOX_G33_CALL*)(Handle h, std::uint32_t ddcTypeIndex, DdcInfo* info);
using GetDdc1CountFn = Bool(FOX_G33_CALL*)(Handle h, std::uint32_t* count);
using SetDdc1Fn = Bool(FOX_G33_CALL*)(Handle h, std::uint32_t ddcTypeIndex);
using SetDdc1FrequencyFn = Bool(FOX_G33_CALL*)(Handle h, std::uint32_t frequency);
using StartDdc1Fn = Bool(FOX_G33_CALL*)(Handle h, std::uint32_t samplesPerBuffer);
using StopDdc1Fn = Bool(FOX_G33_CALL*)(Handle h);
using SetCallbacksFn = Bool(FOX_G33_CALL*)(Handle h, const Callbacks* callbacks, std::uintptr_t userData);

inline constexpr const char* kRequired[] = {"GetDeviceList", "OpenDevice", "CloseDevice",       "GetDeviceInfo", "SetPower",  "GetDDCInfo",
                                            "GetDDC1Count",  "SetDDC1",    "SetDDC1Frequency",  "StartDDC1",     "StopDDC1",  "SetCallbacks"};
inline constexpr const char* kOptional[] = {"IsDeviceConnected", "SetPreamp"};

struct Api {
    GetDeviceListFn GetDeviceList = nullptr;
    OpenDeviceFn OpenDevice = nullptr;
    CloseDeviceFn CloseDevice = nullptr;
    GetDeviceInfoFn GetDeviceInfo = nullptr;
    SetPowerFn SetPower = nullptr;
    GetDdcInfoFn GetDDCInfo = nullptr;
    GetDdc1CountFn GetDDC1Count = nullptr;
    SetDdc1Fn SetDDC1 = nullptr;
    SetDdc1FrequencyFn SetDDC1Frequency = nullptr;
    StartDdc1Fn StartDDC1 = nullptr;
    StopDdc1Fn StopDDC1 = nullptr;
    SetCallbacksFn SetCallbacks = nullptr;
    IsDeviceConnectedFn IsDeviceConnected = nullptr;  // optional
    SetPreampFn SetPreamp = nullptr;                  // optional
};

}  // namespace cascade::source::g33ddc

#endif  // CASCADE_SOURCE_G33DDC_API_HPP
