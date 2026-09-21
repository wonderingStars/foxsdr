// rsp_rows.hpp - one radio, one row in the Source list.
//
// WHAT THIS IS FOR. FoxSDR can reach an SDRplay RSP two ways: natively, over
// USB, because an RSP is a Mirics MSi2500 + MSi001 pair (source/msi2500.hpp);
// and through SDRplay's own API, if the user has installed it
// (source/sdrplay_source.cpp). Both enumerations run, so an RSP owner with the
// API installed was offered the SAME RADIO TWICE, under two names, with
// nothing on the screen saying which to pick - and picking wrong means the
// less capable of the two. 0.99.9's correction to the product ids made the
// pair more visible, because both rows finally called the device by the same
// model name.
//
// THE OWNER'S DECISION (2026-09-21): hide the native row. The API is the
// supported route for every RSP - it is the only one that reaches an RSPduo,
// RSPdx, RSP1B or RSPdx-R2 at all, and on the models this driver does know it
// still drives front-end hardware the native path cannot.
//
// AND THE HIDING IS NARROW, because the failure it could cause is worse than
// the duplicate it removes: a hidden row that the API turns out not to offer
// is a radio the user cannot select at all.
//
//   - Only a row this driver believes is an SDRplay unit is ever hidden. A
//     Mirics television stick shares the driver and must always be listed.
//   - Only when the API actually LISTED something. An API that is installed
//     but whose service is not running lists nothing, and then the native row
//     is the only way in.
//   - When both sides carry a serial, they must MATCH. Two RSPs where the API
//     can see only one (the other being open elsewhere) must not lose the one
//     it cannot see.
//   - A native row with no serial is hidden whenever the API listed anything,
//     because there is nothing left to tell them apart with and the duplicate
//     is the likelier reading.
//
// PURE, so tests/test_rsp_rows.cpp can drive every one of those cases without
// an API, a radio, or a USB bus.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_RSP_ROWS_HPP
#define CASCADE_SOURCE_RSP_ROWS_HPP

#include <string>
#include <vector>

#include "source/device_source.hpp"

namespace cascade::source {

// The "serial=..." value from an args string, or empty for "index=N" and for
// anything else. The args grammar is the one enumerate* builds and open()
// parses: comma-separated key=value.
inline std::string serialFromArgs(const std::string& args) {
    const std::string key = "serial=";
    std::size_t at = args.find(key);
    while (at != std::string::npos) {
        // Only at the start or straight after a separator, so a key that
        // merely ENDS in "serial=" cannot be mistaken for this one.
        if (at == 0 || args[at - 1] == ',' || args[at - 1] == ' ') {
            const std::size_t from = at + key.size();
            const std::size_t end = args.find(',', from);
            return args.substr(from, (end == std::string::npos) ? std::string::npos : end - from);
        }
        at = args.find(key, at + 1);
    }
    return std::string();
}

// Is this row the native driver offering an SDRplay unit? The label is what
// msi2500::resolveModel decided, so a television stick answers false here even
// though it is the same driver and the same silicon.
inline bool isNativeRspRow(const NativeDeviceInfo& row) {
    return row.driver == "mirisdr" && row.label.rfind("SDRplay", 0) == 0;
}

// Would the SDRplay API's own list already show this native row's radio?
inline bool apiAlreadyLists(const NativeDeviceInfo& nativeRow,
                            const std::vector<NativeDeviceInfo>& apiRows) {
    if (apiRows.empty()) { return false; }
    const std::string serial = serialFromArgs(nativeRow.args);
    if (serial.empty()) { return true; }  // nothing to match on; see the header
    bool anyApiSerial = false;
    for (const NativeDeviceInfo& api : apiRows) {
        const std::string apiSerial = serialFromArgs(api.args);
        if (apiSerial.empty()) { continue; }
        anyApiSerial = true;
        if (apiSerial == serial) { return true; }
    }
    // The API listed devices but named no serials at all: it cannot be
    // matched, and the duplicate is still the likelier reading.
    return !anyApiSerial;
}

// The Source list with the duplicates taken out. `rows` is every row, in the
// order the list will show them; the API rows are found within it by driver,
// so the caller does not have to keep the two halves apart.
inline std::vector<NativeDeviceInfo> withoutDuplicateRsps(
    const std::vector<NativeDeviceInfo>& rows) {
    std::vector<NativeDeviceInfo> apiRows;
    for (const NativeDeviceInfo& r : rows) {
        if (r.driver == "sdrplay") { apiRows.push_back(r); }
    }
    if (apiRows.empty()) { return rows; }
    std::vector<NativeDeviceInfo> out;
    out.reserve(rows.size());
    for (const NativeDeviceInfo& r : rows) {
        if (isNativeRspRow(r) && apiAlreadyLists(r, apiRows)) { continue; }
        out.push_back(r);
    }
    return out;
}

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_RSP_ROWS_HPP
