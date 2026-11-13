// sdriq_d2xx.hpp - the byte link of the RFspace SDR-IQ over FTDI's D2XX library, loaded at run time from the driver the USER installed.
//
// WHAT THIS FINISHES. sdriq_source.hpp stops at SdriqLink (write / read with a timeout / interrupt / close) and says the rest is "a small job once
// the owner permits it". The owner permitted it on 2026-11-12 (21:50): FoxSDR may load a vendor library the user installed, shipping, downloading and
// redistributing none. This is that job: an SdriqLink over D2XX's FT_ListDevices / FT_OpenEx / FT_SetTimeouts / FT_Purge / FT_GetQueueStatus /
// FT_Read / FT_Write / FT_Close, nothing else. The calls are declared in ftd2xx_api.hpp from FTDI's own programmer's guide; FTDI's header was not read.
//
// THE LIBRARY. Looked for as ftd2xx.dll, then ftd2xx64.dll, through the shared loader (vendor_lib): a folder or file the person gave, or the absolute
// entries of PATH (System32 is one), never the current directory or the executable's folder; judged from its bytes first (a 32-bit file is named
// plainly and not loaded); loaded with the thread's error mode set to fail silently, so a missing dependency is a sentence and never a Windows box.
// When it is absent, too old (an export missing) or of the wrong bitness the opener answers with a plain sentence that names FTDI's D2XX driver and
// where it comes from (text only: nothing is opened or downloaded).
//
// WHICH DEVICE. A serial number the person gave is opened as given (the source then asks the device its name and refuses anything that is not an
// SDR-IQ). With none given, the devices whose D2XX description contains "SDR-IQ" (any case) are used, the first one; FoxSDR never writes to an FTDI
// device that does not say it is an SDR-IQ unless the person named its serial. UNVERIFIED: that a real SDR-IQ's FTDI description contains that
// text (the interface specification does not print the description string).
//
// CALLS INTO THE LIBRARY run under callGuardingVendorFaults: a fault inside FTDI's code makes the link fail ("the FTDI driver failed"), never the
// application. Every read and write is bounded by the timeouts set at open (a D2XX timeout of 0 means "wait forever" and is never passed).
//
// Receive only: no EEPROM, baud-rate, modem-line or bit-bang call exists in this file or in ftd2xx_api.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_SDRIQ_D2XX_HPP
#define CASCADE_SOURCE_SDRIQ_D2XX_HPP

#include <memory>
#include <string>
#include <vector>

#include "source/needed_library.hpp"
#include "source/sdriq_source.hpp"

namespace cascade::source {

// What an open attempt found, for tests and the health row. Contains no serial number.
struct D2xxOpenInfo {
    std::string verdict;      // "ok", or a vendorlib verdict word, or "no-device", "no-sdriq", "open-failed", "bad-driver"
    int devicesSeen = 0;      // FTDI devices the driver listed
};

// Opens the SDR-IQ with this serial number (empty = the first device described as an SDR-IQ) through the D2XX library found at `libraryWhere`
// (empty = the PATH directories; a folder; or a file). Null with a plain sentence in `error` on every failure.
std::unique_ptr<SdriqLink> openD2xxSdriq(const std::string& libraryWhere, const std::string& serial, std::string& error,
                                         D2xxOpenInfo* info = nullptr);

// The same as an SdriqOpener for SdriqSource::setOpener(). `libraryWhere` is captured by value.
SdriqOpener d2xxSdriqOpener(const std::string& libraryWhere = std::string());

// THE NEEDED-LIBRARY DATA (owner rule 2026-11-12 22:00): what the radio-setup page shows for the SDR-IQ, as structured fields, and "Check again".
const needed::Info& ftd2xxNeededInfo();
// Looks for the library (a folder or file the person chose, or the PATH directories), judges it from its bytes, loads it silently, checks that it offers
// the eight calls, unloads it again and reports. Opens no device, downloads and opens nothing, keeps nothing: calling it again after the person installed
// the driver finds it without restarting FoxSDR.
needed::Check checkFtd2xx(const std::string& libraryWhere = std::string());

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_SDRIQ_D2XX_HPP
