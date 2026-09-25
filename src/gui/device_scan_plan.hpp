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
#include <cstdint>
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
// Mirics chip is claimed by the SDRplay module as well as SoapyMiri, and the
// claim runs BOTH WAYS: an RSP1/RSP1A/RSP2 is the same Mirics chip on the bus
// whether it was opened through the native Mirics driver (mirisdr), the native
// SDRplay API driver (sdrplay) or either SoapySDR module - so every one of
// those leaves all three module names out. Through 0.99.34 "sdrplay" left out
// only {"sdrplay"}, and a scan beside an open RSP still ran SoapyMiri's probe.
// An RX888 is SoapySDDC's.
inline std::vector<std::string> soapyModulesForFamily(const std::string& kind,
                                                      const std::string& args, bool& known) {
    known = true;
    static const std::vector<std::string> kMirics = {"sdrplay", "miri", "mirisdr"};
    const std::string k = detail::lowerAscii(kind);
    if (k == "siggen" || k == "file" || k.empty()) { return {}; }
    if (k == "rtlsdr") { return {"rtlsdr"}; }
    if (k == "hackrf") { return {"hackrf"}; }
    if (k == "airspy") { return {"airspy"}; }
    if (k == "airspyhf") { return {"airspyhf"}; }
    if (k == "sdrplay" || k == "mirisdr") { return kMirics; }
    if (k == "rx888") { return {"sddc"}; }
    if (k == "pluto") { return {"plutosdr"}; }
    if (k == "soapy") {
        const std::string d = detail::driverOf(args);
        if (d == "sdrplay" || d == "miri" || d == "mirisdr") { return kMirics; }
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
    // A sound card is reached through the audio stack, never the USB bus a
    // SoapySDR probe walks.
    if (k == "siggen" || k == "file" || k == "soundcard" || k.empty()) { return false; }
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

// ---------------------------------------------------------------------------
// DRIVERS WITH NOTHING TO FIND (2026-09-25)
// ---------------------------------------------------------------------------
//
// Field report F204602B5329B268 (0.99.35, Windows 10.0.28000): the SoapySDR
// scan's child probing driver=uhd died, on a machine whose only radio was an
// SDRplay RSPdx. UHD's discovery loads libusb and walks the whole USB bus on a
// thread per device family, and that walk is where this product's known UHD
// faults live - so on a machine with no USRP it costs a crash risk and finds
// nothing. It is asked only when a USRP could be there.
//
// One USB device as the read-only SetupAPI / sysfs listing reports it
// (usb::presentUsbIds - nothing is opened to produce it).
struct UsbVidPid {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
};

// THE USB IDS UHD LOOKS FOR, taken from UHD itself rather than remembered
// (checked 2026-09-25 against EttusResearch/uhd master and v4.8.0.0:
// host/lib/usrp/b200/b200_iface.hpp B200_VENDOR_ID 0x2500, B200_VENDOR_NI_ID
// 0x3923, B200_PRODUCT_ID 0x0020, B200MINI 0x0021, B205MINI 0x0022, B206MINI
// 0x0023, B200_PRODUCT_NI_ID 0x7813, B210_PRODUCT_NI_ID 0x7814 - the list
// b200_find searches, b200_vid_pid_pairs in b200_impl.hpp; host/utils/
// uhd-usrp.rules, which adds the USRP1 fffe:0002 and the B100 2500:0002; and
// the WinUSB INFs UHD 4.10's installer ships in share/uhd/images). Every
// Ettus product is matched by vendor alone, so a future one is not missed;
// National Instruments makes a great deal that is not a USRP, so its vendor
// id counts only with the two B2xx product ids. The FX3 bootloader ids
// (04b4:00f3/00f0) are NOT here: b200_find never searches them - only UHD's
// b2xx_fx3_utils recovery tool does.
inline bool isUsrpUsbId(const UsbVidPid& id) {
    if (id.vid == 0x2500) { return true; }
    if (id.vid == 0x3923 && (id.pid == 0x7813 || id.pid == 0x7814)) { return true; }
    if (id.vid == 0xFFFE && id.pid == 0x0002) { return true; }
    return false;
}

// Which SoapySDR drivers this scan should not ask, because nothing of theirs
// can be here. Today at most {"uhd"}, which is left out unless ANY of:
//
//   - `usbListed` is false: the USB listing itself failed, so absence is
//     unknown and the probe runs as it always did (never a guess towards
//     hiding a radio);
//   - a USRP is on the USB bus (isUsrpUsbId);
//   - one of `namedSoapyArgs` is a UHD device ("driver=uhd,...") - the saved
//     source, the open one, or a patch radio: the user has one, and a
//     network USRP's address lives in exactly these args;
//   - `lookForNetworkUsrps`: the Settings switch (off by default) for USRPs
//     UHD finds over the network (N2xx, X3xx, N3xx, E3xx over Ethernet) or on
//     PCIe (X3xx via NI-RIO), none of which is on the USB bus.
inline std::vector<std::string> soapyDriversWithNoHardware(
    const std::vector<UsbVidPid>& presentUsb, bool usbListed,
    const std::vector<std::string>& namedSoapyArgs, bool lookForNetworkUsrps) {
    if (!usbListed || lookForNetworkUsrps) { return {}; }
    for (const UsbVidPid& id : presentUsb) {
        if (isUsrpUsbId(id)) { return {}; }
    }
    for (const std::string& args : namedSoapyArgs) {
        if (detail::driverOf(args) == "uhd") { return {}; }
    }
    return {"uhd"};
}

}  // namespace cascade::gui
