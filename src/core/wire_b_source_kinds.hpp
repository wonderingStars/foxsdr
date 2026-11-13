// wire_b_source_kinds.hpp - THE SOURCE KINDS OF THE WIRE-B SOURCE TABLE, as the configuration knows them (wire-b second pass, item 33 of not-wired-after-merge-3.md).
//
// engine/wire_b_sources.cpp registers twelve radios by a driver key (RFspace, HPSDR 1 and 2, Aaronia, SpyServer, SoapyRemote, SDRconnect, IC-R8600, FUNcube, G39DDC, G33DDC, SDR-IQ). A radio is
// opened for the first time by typing "open-<key>:<arguments>"; for it to come back at the next start the configuration must ACCEPT the key as a saved source kind (the top-level `sourceKind`
// of radio 1 and the `sourceKind` of an entry of `radios`), and the save and the restore must treat it as a native driver (gui::isNativeSourceKind), like "rtltcp" and "kiwisdr". The core
// cannot call the engine's table, so the keys are listed here, ONCE, and tests/test_wire_b_saved_kinds.cpp holds this list equal to the engine's table in both directions: a key added to one
// and not the other fails the test.
//
// WHAT IS SAVED: the arguments as the open normalised them ("rfspace=192.168.1.50:50000,rate=250000"): an address the user typed, a rate, a library or firmware path the user chose. NEVER a
// secret: no source of the table takes a password or a token (an Aaronia server that asks for one is refused), and the test pins that no saved argument carries one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>

namespace cascade::core {

inline constexpr const char* kWireBSourceKinds[] = {"rfspace", "hpsdr1", "hpsdr2", "aaronia", "spyserver", "soapyremote", "sdrconnect", "icr8600", "funcube", "g39ddc", "g33ddc", "sdriq"};
inline constexpr std::size_t kWireBSourceKindCount = sizeof(kWireBSourceKinds) / sizeof(kWireBSourceKinds[0]);

inline bool isWireBSourceKind(const std::string& kind) {
    for (const char* k : kWireBSourceKinds) {
        if (kind == k) { return true; }
    }
    return false;
}

}  // namespace cascade::core
