// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/driver_policy.hpp"

#include <cstring>

#include "core/radio_setup.hpp"

namespace cascade::core::driverpolicy {

namespace rs = cascade::core::radiosetup;

const char* actionName(Action a) {
    switch (a) {
    case Action::InstallWinUsb: return "install-winusb";
    case Action::NoInstallNeeded: return "no-install-needed";
    case Action::NotAllowed: return "not-allowed";
    }
    return "not-allowed";
}

const std::vector<PolicyRow>& policyTable() {
    static const std::vector<PolicyRow> table = {
        {rs::kFamilyRtl, Action::InstallWinUsb, true,
         "core/winusb_install.hpp (shipped 0.99.88); Microsoft Learn, WinUSB (Winusb.sys) installation",
         "RTL2832U composite device: interface 0 only, WinUSB from the in-box Microsoft INF"},
        {rs::kFamilyAirspy, Action::NoInstallNeeded, true,
         "https://airspy.com/quickstart/ (opened 2026-11-11)",
         "the vendor says Airspy is plug-and-play on Windows Vista to 10 with no driver install; "
         "the manual fallback is the vendor's own WinUSB Compatibility Driver, not bundled here"},
        {rs::kFamilyAirspyHf, Action::NotAllowed, false,
         "airspyhf: no Windows driver page opened",
         "no vendor Windows driver description was read for the HF+ family"},
        {rs::kFamilyHackRf, Action::NotAllowed, false,
         "https://hackrf.readthedocs.io/en/latest/installing_hackrf_software.html (opened 2026-11-11, silent on drivers)",
         "the vendor documentation names no Windows driver step; community advice is not authoritative"},
        {rs::kFamilySdrPlay, Action::NotAllowed, true,
         "core/radio_setup.cpp: an RSP needs the SDRplay API service from the vendor's installer",
         "the vendor's installer owns this driver; this installer never replaces it"},
        {rs::kFamilyUsrp, Action::NotAllowed, true,
         "core/radio_setup.cpp: a USRP needs UHD",
         "UHD's own installer owns this driver; this installer never replaces it"},
        {rs::kFamilyLime, Action::NotAllowed, false,
         "limesdr: no Windows driver page opened",
         "no vendor Windows driver description was read for LimeSDR"},
    };
    return table;
}

const PolicyRow* findPolicy(const std::string& family) {
    for (const PolicyRow& r : policyTable()) {
        if (family == r.family) { return &r; }
    }
    return nullptr;
}

Decision decide(std::uint16_t vid, std::uint16_t pid, int interfaceNumber) {
    Decision d;
    const rs::KnownDevice* dev = rs::findKnownDevice(vid, pid);
    if (dev == nullptr) {
        d.reason = "unknown USB id: not a radio this installer has a reviewed row for";
        return d;
    }
    d.family = dev->family;
    const PolicyRow* row = findPolicy(d.family);
    if (row == nullptr) {
        d.reason = "no reviewed policy row for this family";
        return d;
    }
    d.evidenceVerified = row->evidenceVerified;
    if (row->action != Action::InstallWinUsb) {
        d.action = row->action;
        d.reason = row->note;
        return d;
    }
    if (interfaceNumber != 0) {
        d.reason = "not interface 0 of a composite device (never the parent, never another interface)";
        return d;
    }
    d.action = Action::InstallWinUsb;
    d.reason = row->note;
    return d;
}

}  // namespace cascade::core::driverpolicy
