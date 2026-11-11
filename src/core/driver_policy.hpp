// driver_policy.hpp - WHICH radios the one-click driver installer may ever touch,
// as DATA, with a pure lookup. W12 (one-click drivers for HackRF, Airspy,
// RX888, AOR, Mirics): the buildable part is this table; nothing here installs,
// downloads or opens anything.
//
// Why a table and not an if-chain: "wrong-device installs are the risk" (the
// re-check). core/winusb_install.hpp already refuses anything but interface 0
// of an RTL2832U composite device. This table says, per radio FAMILY, what the
// installer's policy is and WHERE THAT WAS READ, so adding a family is adding a
// reviewed row, and an id nobody has verified is refused by construction.
//
// What was read (2026-11-11, pages opened):
//  * Airspy: https://airspy.com/quickstart/ says "Airspy is a plug-and-play
//    device and does not require any particular driver installation on Windows
//    Vista, 7, 8, 8.1 and 10" (heading also lists 11); the manual route is the
//    vendor's own "WinUSB Compatibility Driver" .inf, which this project does
//    not bundle. Policy: NoInstallNeeded (the installer never touches it).
//  * HackRF: https://hackrf.readthedocs.io/en/latest/installing_hackrf_software.html
//    does not mention Windows drivers, WinUSB or Zadig at all (libusb only as a
//    build dependency). Community pages say Zadig is rarely needed; those are
//    not authoritative. Policy: NotAllowed until a vendor description exists.
//  * RX888, AOR, Mirics: not opened; their USB ids are NOT in this table and
//    are NOT VERIFIED. (1df7:2500 is already named an SDRplay RSP1 by
//    core/radio_setup.cpp and is also the Mirics reference-design id.)
//  * RTL-SDR: the shipped installer (0.99.88), Microsoft Learn "WinUSB
//    (Winusb.sys) installation"; unchanged here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_DRIVER_POLICY_HPP
#define CASCADE_CORE_DRIVER_POLICY_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core::driverpolicy {

enum class Action {
    InstallWinUsb,    // the installer may bind WinUSB to this interface (RTL-SDR only today)
    NoInstallNeeded,  // the vendor says Windows needs no driver install: never touched
    NotAllowed        // never touched: unknown, unverified, or the vendor's installer owns it
};

const char* actionName(Action a);

// One reviewed row per radio family (the family words of core/radio_setup.hpp).
struct PolicyRow {
    const char* family;
    Action action;
    bool evidenceVerified;  // the source below was opened and says what the row claims
    const char* source;     // where it was read, one line
    const char* note;       // English, for the log: why this action
};

const std::vector<PolicyRow>& policyTable();
const PolicyRow* findPolicy(const std::string& family);

struct Decision {
    Action action = Action::NotAllowed;
    std::string family;  // empty when the id is unknown
    std::string reason;  // English, one line
    bool evidenceVerified = false;
};

// PURE. May the installer bind a driver to this USB node? `interfaceNumber` is
// the interface of a composite device (0, 1, ...) or -1 for the device node /
// parent. The answer is InstallWinUsb only when the id is a known radio of a
// family whose row says InstallWinUsb AND the node is interface 0 (never the
// composite parent, never another interface). Unknown ids are NotAllowed.
Decision decide(std::uint16_t vid, std::uint16_t pid, int interfaceNumber);

}  // namespace cascade::core::driverpolicy

#endif  // CASCADE_CORE_DRIVER_POLICY_HPP
