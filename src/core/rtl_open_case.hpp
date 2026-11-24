// rtl_open_case.hpp - WHICH of three things happened when an RTL-SDR open found no radio it could use, and the sentence for each.
//
// WHY (field survey 2026-11-24, report 28): the one sentence the open used to say, "no RTL-SDR is bound to WinUSB on this machine", is what an UNPLUGGED
// dongle and a dongle on the WRONG WINDOWS DRIVER (the television driver every RTL-SDR arrives with) both produced, because the open lists only WinUSB-bound
// devices. 36 installs hit it, 30 reports of it were saved radios at start-up, and the person was never told whether to plug the radio in or to install a
// driver. FoxSDR already has the second fact: usb::enumerateUnbound lists the dongles that are on the bus with no WinUSB interface.
//
//   (a) WrongDriver: an RTL-SDR is on the bus, not on WinUSB. The sentence says so and that FoxSDR can install the driver (the one-click install the Radio setup
//       page has; the new window offers its key beside the sentence).
//   (b) NotPlugged: the bus listing was taken and holds no RTL-SDR at all.
//   (c) CannotTell: anything else - the bus listing could not be taken, or an RTL-SDR is on the bus and the transport cannot say why it is not usable (Linux has
//       no "unbound" list: the permission problem is a usbfs one).
//
// The decision is PURE (decide()), so a test drives all three with plain values. The sentences go through tr(); the classifier (health_events.cpp) matches each in
// English and in the language in force, so a translated window files the same class word: (a) bind, (b) absent, (c) other. No new report word.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RTL_OPEN_CASE_HPP
#define CASCADE_CORE_RTL_OPEN_CASE_HPP

#include <string>
#include <string_view>

#include "core/i18n.hpp"

namespace cascade::core::rtlopen {

enum class Case { WrongDriver, NotPlugged, CannotTell };

// THE ENGLISH SENTENCES, the translation keys. Each is the whole error the open sets (the engine wraps it as "<radio> did not open: <sentence>. The receiver is
// running on the signal generator."), so each ends in a full stop and none names a Windows menu path: the setup page has those words.
inline constexpr const char kWrongDriverEnglish[] = FOX_TR_NOOP(
    "An RTL-SDR is plugged in, but Windows has not given it the WinUSB driver that FoxSDR needs, so FoxSDR cannot open it. FoxSDR can install that driver for you from the Radio setup page.");
inline constexpr const char kNotPluggedEnglish[] = FOX_TR_NOOP(
    "No RTL-SDR is plugged into this computer. Plug it in, then pick it again.");
inline constexpr const char kCannotTellEnglish[] = FOX_TR_NOOP(
    "FoxSDR found no RTL-SDR it can open. Check that it is plugged in; if it is, unplug it and plug it back in. Then pick it again.");

// The decision. `listed`: the list of every USB device on the bus could be taken (usb::presentUsbIds returned true). `rtlOnBus`: that list holds an RTL-SDR id.
// `anyUnbound`: usb::enumerateUnbound found an RTL-SDR with no WinUSB interface. Order matters: an unbound dongle is the answer whatever else is true.
inline Case decide(bool listed, bool rtlOnBus, bool anyUnbound) {
    if (anyUnbound) { return Case::WrongDriver; }
    if (listed && !rtlOnBus) { return Case::NotPlugged; }
    return Case::CannotTell;
}

inline const char* english(Case c) {
    switch (c) {
        case Case::WrongDriver: return kWrongDriverEnglish;
        case Case::NotPlugged: return kNotPluggedEnglish;
        case Case::CannotTell: break;
    }
    return kCannotTellEnglish;
}

inline std::string sentence(Case c) { return cascade::i18n::tr(english(c)); }

// Which case `error` says - the sentence in English or in the language in force, anywhere inside `error` (the engine wraps the open's sentence). false for any
// other error text. The one test the engine's `source.fix` fact and the classifier both use.
inline bool says(std::string_view error, Case c) {
    const char* en = english(c);
    if (error.find(en) != std::string_view::npos) { return true; }
    const std::string local = cascade::i18n::tr(en);
    return !local.empty() && error.find(local) != std::string_view::npos;
}

inline bool saysWrongDriver(std::string_view error) { return says(error, Case::WrongDriver); }

}  // namespace cascade::core::rtlopen

#endif  // CASCADE_CORE_RTL_OPEN_CASE_HPP
