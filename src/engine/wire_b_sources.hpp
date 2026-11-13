// wire_b_sources.hpp - the wire-b lane's SOURCE TABLE: network and vendor radios that open by an id the user types, registered in one table instead of ten
// places per source (wire-b; docs/multi-radio-records/wire-b.md).
//
// A row here makes SELECT_SOURCE and RADIO_ADD accept "open-<key>:<arguments>" for that radio (the way "open-kiwisdr:" and "open-rawiq:" are accepted), builds the
// source on the device-open worker (Engine::makeDeviceSource), keeps the same radio from being opened twice (the device key), and refuses the id to a REMOTE session
// (an outward connection or a library load at an address or path the remote client chose). EVERY SOURCE HERE IS OFF until a person sends the id: nothing in this
// table scans, connects or loads anything at start-up, and a source that is absent (no server, no library, no device) refuses with its own sentence and raises no
// Windows dialog. None of these radios was on the desk: every row is UNVERIFIED on hardware (each source is proved against a stand-in peer on 127.0.0.1).
//
// The arguments are the source's own "key=value" list (docs/multi-radio-records/wire-b.md names each). A row that NEEDS AN ADDRESS takes the host (and port) as the
// first item with the key left out ("open-rfspace:192.168.1.50:50000,rate=250000" is "rfspace=192.168.1.50:50000,rate=250000"); an empty address is refused, never read
// as the local machine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace cascade::source { class DeviceSource; }

namespace cascade::engine::wireb {

struct SourceKind {
    const char* key;            // the driver key (DeviceSource::driverKey()) and the argument prefix: "rfspace"
    const char* name;           // the English name for a Source list: "RFspace NetSDR / SDR-IP / Afedri (network)"
    bool needsAddress;          // the first item of the arguments is a host[:port]
    double requestRateHz;       // the rate the open asks the source for (a default; a rate in the arguments wins inside the source)
    std::unique_ptr<cascade::source::DeviceSource> (*make)();
};

// The table (every row, in order) and its length.
const SourceKind* sourceKinds(std::size_t& count);
const SourceKind* findSourceByKey(std::string_view key);
// "open-rfspace" -> the row; nullptr for any other word.
const SourceKind* findSourceByOpenWord(std::string_view word);
// True when `id` is "open-<key>:..." of one of the rows (a REMOTE session is refused it; the device ledger treats it as an address, not a scanned device).
bool isSourceOpenId(std::string_view id);

// The text typed after "open-<key>:" as the arguments the source parses ("<key>=..."): `why` says what is wrong with it when false. Pure; touches nothing.
bool normaliseSourceArgs(const SourceKind& kind, const std::string& typed, std::string& args, std::string& why);

}  // namespace cascade::engine::wireb
