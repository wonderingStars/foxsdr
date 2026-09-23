// device_scan_plan.hpp - which SoapySDR drivers may be asked for devices while
// radios are open (2026-09-23).
//
// THE REPORT. The owner's B200 never appeared on the patch page: the patch
// only read the NATIVE list when it opened, and the SoapySDR scan - the only
// way a B200 is found - was DEFERRED OUTRIGHT while any radio was open. With an
// RTL-SDR in the receiver, the B200 could not be listed until it had been put
// into the receiver itself.
//
// WHY IT WAS DEFERRED, and why that was broader than it had to be. The 0.90.0
// field fault (gui/tune_control.hpp, deviceScanAllowed) was SoapyRTLSDR's probe
// opening and resetting every RTL dongle on the bus - the one this process was
// streaming from included. That danger is to a device of the SAME FAMILY as the
// driver doing the probing: UHD's discovery looks for Ettus hardware and never
// opens an RTL dongle. So the scan can run while radios stream, as long as the
// drivers whose family is open are left out.
//
// WHAT THIS DECIDES, from the radios open in this process (the receiver's and
// every patch radio's): scan everything (none open), scan all but some
// (every open radio's family is known), or defer the whole scan as before -
// whenever it cannot vouch for a driver: a radio still OPENING, a family it
// does not know, or a SoapySDR device open that no receiver or patch radio
// accounts for (one the dead-device policy abandoned, whose driver is unknown).
//
// Pure: no device, no pipeline, no ImGui. test_device_scan_plan pins it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace cascade::gui {

// One open radio: its family as the receiver and the patch name it ("rtlsdr",
// "hackrf", "soapy", ...) and its args ("driver=uhd,serial=..." for "soapy").
struct OpenRadio {
    std::string kind;
    std::string args;
};

enum class SoapyScanMode {
    Full,      // no radio open: the ordinary whole-bus scan
    SkipSome,  // radios open, all of known family: every driver but theirs
    Defer,     // cannot vouch for a driver: no scan now, exactly as before
};

struct SoapyScanPlan {
    SoapyScanMode mode = SoapyScanMode::Full;
    std::vector<std::string> skipDrivers;  // lower-case SoapySDR module keys
};

namespace detail {

inline std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

// "driver" out of "a=1,driver=uhd,serial=31", case-insensitive on the name.
inline std::string driverOf(const std::string& args) {
    std::size_t pos = 0;
    while (pos <= args.size()) {
        std::size_t end = args.find(',', pos);
        if (end == std::string::npos) { end = args.size(); }
        const std::string field = args.substr(pos, end - pos);
        const std::size_t eq = field.find('=');
        if (eq != std::string::npos) {
            std::string name = field.substr(0, eq);
            std::string value = field.substr(eq + 1);
            const auto trim = [](std::string& s) {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.erase(0, 1); }
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.pop_back(); }
            };
            trim(name);
            trim(value);
            if (lowerAscii(name) == "driver") { return lowerAscii(value); }
        }
        if (end == args.size()) { break; }
        pos = end + 1;
    }
    return std::string();
}

}  // namespace detail

// The SoapySDR modules whose discovery probe could open a radio of this
// family. `known` is false for a family this table does not cover - and then
// the caller must not scan at all, because it cannot say what is safe.
//
// Native families map to the Soapy module that drives the same hardware. The
// Mirics chip (mirisdr) is claimed by the SDRplay module as well as SoapyMiri,
// so all of those are left out; an RX888 is SoapySDDC's.
inline std::vector<std::string> soapyModulesForFamily(const std::string& kind,
                                                      const std::string& args, bool& known) {
    known = true;
    const std::string k = detail::lowerAscii(kind);
    if (k == "siggen" || k == "file" || k.empty()) { return {}; }
    if (k == "rtlsdr") { return {"rtlsdr"}; }
    if (k == "hackrf") { return {"hackrf"}; }
    if (k == "airspy") { return {"airspy"}; }
    if (k == "airspyhf") { return {"airspyhf"}; }
    if (k == "sdrplay") { return {"sdrplay"}; }
    if (k == "mirisdr") { return {"sdrplay", "miri", "mirisdr"}; }
    if (k == "rx888") { return {"sddc"}; }
    if (k == "pluto") { return {"plutosdr"}; }
    if (k == "soapy") {
        const std::string d = detail::driverOf(args);
        if (!d.empty()) { return {d}; }
    }
    known = false;
    return {};
}

// The decision. `soapyOpenUnaccounted` is how many SoapySDR devices are open
// in this process beyond the ones in `open` (SoapySource::openDeviceCount()
// less the SoapySDR radios listed) - above zero, a radio is open whose driver
// nobody here can name. `opening` is true while any radio is still being
// opened: its device may already be inside a driver, and the scan waits.
inline SoapyScanPlan planSoapyScan(const std::vector<OpenRadio>& open, int soapyOpenUnaccounted,
                                   bool opening) {
    SoapyScanPlan plan;
    if (opening || soapyOpenUnaccounted > 0) {
        plan.mode = SoapyScanMode::Defer;
        return plan;
    }
    bool anyRadio = false;
    for (const OpenRadio& r : open) {
        bool known = true;
        const std::vector<std::string> mods = soapyModulesForFamily(r.kind, r.args, known);
        if (!known) {
            plan.mode = SoapyScanMode::Defer;
            plan.skipDrivers.clear();
            return plan;
        }
        const std::string k = detail::lowerAscii(r.kind);
        if (k != "siggen" && k != "file" && !k.empty()) { anyRadio = true; }
        for (const std::string& m : mods) {
            if (std::find(plan.skipDrivers.begin(), plan.skipDrivers.end(), m) ==
                plan.skipDrivers.end()) {
                plan.skipDrivers.push_back(m);
            }
        }
    }
    plan.mode = anyRadio ? SoapyScanMode::SkipSome : SoapyScanMode::Full;
    return plan;
}

// WHETHER A SCAN IN FLIGHT MAY PROBE A RADIO ABOUT TO BE OPENED - the other
// half of the same rule. The Source panel greys itself out while a scan runs,
// so the receiver never opens a radio under one; the patch opens its radios
// by itself (START, a saved patch, a radio switched on), and did not wait.
// `scanSkip` is the drivers the scan in flight left out - empty for a whole-bus
// scan, which may probe anything. The generator is never touched; a family the
// table does not know is assumed touched.
inline bool scanMayProbe(const std::vector<std::string>& scanSkip, const std::string& kind,
                         const std::string& args) {
    const std::string k = detail::lowerAscii(kind);
    if (k == "siggen" || k == "file" || k.empty()) { return false; }
    if (scanSkip.empty()) { return true; }
    bool known = true;
    const std::vector<std::string> mods = soapyModulesForFamily(kind, args, known);
    if (!known || mods.empty()) { return true; }
    for (const std::string& m : mods) {
        if (std::find(scanSkip.begin(), scanSkip.end(), m) == scanSkip.end()) { return true; }
    }
    return false;
}

// Whether a device row belongs to one of the drivers a scan left out - such a
// row is KEPT from the previous list, because that scan could not have seen it.
inline bool rowFromSkippedDriver(const std::vector<std::string>& skipDrivers,
                                 const std::string& rowArgs) {
    const std::string d = detail::driverOf(rowArgs);
    return !d.empty() &&
           std::find(skipDrivers.begin(), skipDrivers.end(), d) != skipDrivers.end();
}

}  // namespace cascade::gui
