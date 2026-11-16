// fobos_api.hpp - the calls FoxSDR makes into the Fobos SDR maker's own library, declared HERE in FoxSDR's own words.
//
// WHERE THE NAMES COME FROM. The maker's public header for that library (the RigExpert libfobos project, LGPL-2.1) was read on 2026-11-16 with the owner's
// permission, for the exported function names, their argument types, the numeric values of the error codes and the one size constant. Nothing of it is
// copied into this tree: the declarations below are written afresh, and the maker's file is not in the repository. The library itself is never shipped,
// bundled or fetched: the person installs it and FoxSDR loads it at run time (fobos_source.hpp).
//
// WHAT IS NOT USED, ON PURPOSE. Four calls of that library change the device itself (they write a firmware image, read one out, reset the board, or drive
// the general-purpose output pins). They are not resolved, not declared and not named anywhere in this tree except in kForbidden below, which the test
// reads to prove that no resolved name is one of them. The explicit synthesizer calls, the synchronous read calls and the library-version call are
// not resolved either: a receive-only bridge does not need them.
//
// WHAT THE HEADER DOES NOT SAY (so nothing here claims it): whether a stream buffer length counts floats or complex samples (fobos_source.hpp has a
// setting for it and a check), whether the streaming call blocks until it is cancelled, which thread may make which call, the tuning range, the units of
// the two gain settings, and the shape of the sample values.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_FOBOS_API_HPP
#define CASCADE_SOURCE_FOBOS_API_HPP

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define FOX_FOBOS_CALL __cdecl
#else
#define FOX_FOBOS_CALL
#endif

namespace cascade::source::fobos {

struct Device;  // opaque: only ever handled by pointer

inline constexpr int kErrOk = 0;
inline constexpr int kErrNoDevice = -1;
inline constexpr int kErrNotOpen = -2;
inline constexpr int kErrNoMemory = -3;
inline constexpr int kErrControl = -4;
inline constexpr int kErrUnsupported = -8;
inline constexpr int kErrUsb = -9;
inline constexpr std::size_t kInfoLen = 64;  // the size the library documents for each text field it fills

using StreamCallback = void(FOX_FOBOS_CALL*)(float* buffer, std::uint32_t length, void* context);

using GetDeviceCountFn = int(FOX_FOBOS_CALL*)();
using ListDevicesFn = int(FOX_FOBOS_CALL*)(char* serials);
using OpenFn = int(FOX_FOBOS_CALL*)(Device** out, std::uint32_t index);
using CloseFn = int(FOX_FOBOS_CALL*)(Device* dev);
using GetBoardInfoFn = int(FOX_FOBOS_CALL*)(Device* dev, char* hwRevision, char* fwVersion, char* manufacturer, char* product, char* serial);
using SetFrequencyFn = int(FOX_FOBOS_CALL*)(Device* dev, double hz, double* actual);
using SetDirectSamplingFn = int(FOX_FOBOS_CALL*)(Device* dev, unsigned int enabled);
using SetLnaGainFn = int(FOX_FOBOS_CALL*)(Device* dev, unsigned int value);
using SetVgaGainFn = int(FOX_FOBOS_CALL*)(Device* dev, unsigned int value);
using GetSampleRatesFn = int(FOX_FOBOS_CALL*)(Device* dev, double* values, unsigned int* count);
using SetSampleRateFn = int(FOX_FOBOS_CALL*)(Device* dev, double hz, double* actual);
using ReadAsyncFn = int(FOX_FOBOS_CALL*)(Device* dev, StreamCallback cb, void* context, std::uint32_t bufferCount, std::uint32_t bufferLength);
using CancelAsyncFn = int(FOX_FOBOS_CALL*)(Device* dev);
using SetClockSourceFn = int(FOX_FOBOS_CALL*)(Device* dev, int value);
using ErrorNameFn = const char*(FOX_FOBOS_CALL*)(int error);

// The names resolved from the library, in the order of Api's members. kRequired must all be there; kOptional are used when present.
inline constexpr const char* kRequired[] = {"fobos_rx_get_device_count", "fobos_rx_list_devices", "fobos_rx_open",          "fobos_rx_close",
                                            "fobos_rx_get_board_info",   "fobos_rx_set_frequency", "fobos_rx_get_samplerates", "fobos_rx_set_samplerate",
                                            "fobos_rx_read_async",       "fobos_rx_cancel_async"};
inline constexpr const char* kOptional[] = {"fobos_rx_set_lna_gain", "fobos_rx_set_vga_gain", "fobos_rx_set_direct_sampling", "fobos_rx_set_clk_source",
                                            "fobos_rx_error_name"};
// The calls of this library that change the device itself. NEVER resolved; tests/test_fobos_source.cpp proves it.
inline constexpr const char* kForbidden[] = {"fobos_rx_write_firmware", "fobos_rx_read_firmware", "fobos_rx_reset", "fobos_rx_set_user_gpo"};

struct Api {
    GetDeviceCountFn GetDeviceCount = nullptr;
    ListDevicesFn ListDevices = nullptr;
    OpenFn Open = nullptr;
    CloseFn Close = nullptr;
    GetBoardInfoFn GetBoardInfo = nullptr;
    SetFrequencyFn SetFrequency = nullptr;
    GetSampleRatesFn GetSampleRates = nullptr;
    SetSampleRateFn SetSampleRate = nullptr;
    ReadAsyncFn ReadAsync = nullptr;
    CancelAsyncFn CancelAsync = nullptr;
    SetLnaGainFn SetLnaGain = nullptr;          // optional
    SetVgaGainFn SetVgaGain = nullptr;          // optional
    SetDirectSamplingFn SetDirectSampling = nullptr;  // optional
    SetClockSourceFn SetClockSource = nullptr;  // optional
    ErrorNameFn ErrorName = nullptr;            // optional
};

// The file names tried when the person gave no `lib=`: not read from any document (the header names no file), so each is a guess and the page says so.
inline constexpr const char* kLibraryNames[] = {"fobos.dll", "libfobos.dll"};

}  // namespace cascade::source::fobos

#endif  // CASCADE_SOURCE_FOBOS_API_HPP
