// ftd2xx_api.hpp - the eight calls of FTDI's D2XX "classic interface" that FoxSDR makes, declared HERE from FTDI's published programmer's guide, not
// from FTDI's header.
//
// SOURCE. "D2XX Programmer's Guide", Version 2.01 (2002), FTDI's own document, read on 2026-11-12 from a copy at
// https://www.mikrocontroller.net/attachment/57942/D2XXPG21.pdf (FTDI's own site answered 403 to this tool for every current edition, so the newer
// editions - 1.4 / 1.5 - were NOT opened). The guide prints every prototype used below and, in its appendix, the numeric values of FT_STATUS, of the
// flags of FT_OpenEx / FT_ListDevices and of FT_PURGE_RX / FT_PURGE_TX. FTDI's header (ftd2xx.h) was not opened and nothing of it is in this tree.
//
// WHAT IS NOT IN THE GUIDE AND IS THEREFORE AN ASSUMPTION (reported UNVERIFIED): the guide's year-2002 table says FT_HANDLE is "unsigned long (4
// bytes)", which is how a 32-bit program saw it. A 64-bit program needs a handle that can hold a pointer, and the library of this period and ours
// (x64, ftd2xx.dll in System32) hands out one, so the handle is declared pointer sized. That is the one declaration that goes beyond the printed text.
//
// RECEIVE ONLY. Only the calls the SDR-IQ link needs are declared: no EEPROM call, no baud-rate or modem-line call, no bit-bang mode. The
// SDR-IQ's firmware-update and security-code messages are the source's concern and are never sent (see sdriq_source.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_FTD2XX_API_HPP
#define CASCADE_SOURCE_FTD2XX_API_HPP

#include <cstdint>

#if defined(_WIN32)
#define FOX_FT_CALL __stdcall
#else
#define FOX_FT_CALL
#endif

namespace cascade::source::ftd2xx {

using Handle = void*;
using Status = std::uint32_t;  // "FT_STATUS (DWORD)"

// The guide's "FT_STATUS (DWORD)" list, in its own order and values.
inline constexpr Status kOk = 0;
inline constexpr Status kInvalidHandle = 1;
inline constexpr Status kDeviceNotFound = 2;
inline constexpr Status kDeviceNotOpened = 3;
inline constexpr Status kIoError = 4;
inline constexpr Status kInsufficientResources = 5;
inline constexpr Status kInvalidParameter = 6;
inline constexpr Status kOtherError = 17;

// "Flags (see FT_OpenEx)" and "Flags (see FT_ListDevices)".
inline constexpr std::uint32_t kOpenBySerialNumber = 1;
inline constexpr std::uint32_t kOpenByDescription = 2;
inline constexpr std::uint32_t kListNumberOnly = 0x80000000u;
inline constexpr std::uint32_t kListByIndex = 0x40000000u;
// "FT_PURGE_RX = 1, FT_PURGE_TX = 2"
inline constexpr std::uint32_t kPurgeRx = 1;
inline constexpr std::uint32_t kPurgeTx = 2;

// FT_STATUS FT_ListDevices ( PVOID pvArg1, PVOID pvArg2, DWORD dwFlags )
using ListDevicesFn = Status(FOX_FT_CALL*)(void* arg1, void* arg2, std::uint32_t flags);
// FT_STATUS FT_OpenEx ( PVOID pvArg1, DWORD dwFlags, FT_HANDLE *ftHandle )
using OpenExFn = Status(FOX_FT_CALL*)(void* arg1, std::uint32_t flags, Handle* handle);
// FT_STATUS FT_Close ( FT_HANDLE ftHandle )
using CloseFn = Status(FOX_FT_CALL*)(Handle handle);
// FT_STATUS FT_Read ( FT_HANDLE ftHandle, LPVOID lpBuffer, DWORD dwBytesToRead, LPDWORD lpdwBytesReturned )
using ReadFn = Status(FOX_FT_CALL*)(Handle handle, void* buffer, std::uint32_t bytesToRead, std::uint32_t* bytesReturned);
// FT_STATUS FT_Write ( FT_HANDLE ftHandle, LPVOID lpBuffer, DWORD dwBytesToWrite, LPDWORD lpdwBytesWritten )
using WriteFn = Status(FOX_FT_CALL*)(Handle handle, void* buffer, std::uint32_t bytesToWrite, std::uint32_t* bytesWritten);
// FT_STATUS FT_SetTimeouts ( FT_HANDLE ftHandle, DWORD dwReadTimeout, DWORD dwWriteTimeout )
using SetTimeoutsFn = Status(FOX_FT_CALL*)(Handle handle, std::uint32_t readMs, std::uint32_t writeMs);
// FT_STATUS FT_Purge ( FT_HANDLE ftHandle, DWORD dwMask )
using PurgeFn = Status(FOX_FT_CALL*)(Handle handle, std::uint32_t mask);
// FT_STATUS FT_GetQueueStatus ( FT_HANDLE ftHandle, LPDWORD lpdwAmountInRxQueue )
using GetQueueStatusFn = Status(FOX_FT_CALL*)(Handle handle, std::uint32_t* amountInRxQueue);

// The exported names, in the order the Api struct is filled.
inline constexpr const char* kExports[] = {"FT_ListDevices", "FT_OpenEx",     "FT_Close", "FT_Read",
                                           "FT_Write",       "FT_SetTimeouts", "FT_Purge", "FT_GetQueueStatus"};

struct Api {
    ListDevicesFn ListDevices = nullptr;
    OpenExFn OpenEx = nullptr;
    CloseFn Close = nullptr;
    ReadFn Read = nullptr;
    WriteFn Write = nullptr;
    SetTimeoutsFn SetTimeouts = nullptr;
    PurgeFn Purge = nullptr;
    GetQueueStatusFn GetQueueStatus = nullptr;
};

}  // namespace cascade::source::ftd2xx

#endif  // CASCADE_SOURCE_FTD2XX_API_HPP
