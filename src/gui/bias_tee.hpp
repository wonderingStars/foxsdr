// THE BIAS TEE CHECKBOX: which radios have one, what is put back after an
// open, and what a tick does.
//
// It lives in a header of its own, rather than in app_window.cpp where it
// began, for the reason every rule in gui/tune_control.hpp does: a rule kept
// inside the AppWindow translation unit is a rule no test can reach, and this
// is the one control in the Source panel that puts POWER on a connector.
// tests/test_rtl_bias_tee.cpp drives these functions against the shipping
// RTL-SDR driver on a fake USB transport.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/device_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/mirisdr_source.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"
#include "source/sdrplay_source.hpp"

namespace cascade::gui {

// THE BIAS TEE, AND WHY IT IS A dynamic_cast AND NOT A DeviceSource METHOD.
//
// Putting 4.5 V on an antenna connector is not a thing every radio does, and
// it is not "a port to choose": each of these drivers keeps it out of
// antennas() deliberately, so that something iterating a list of port names
// can never switch power on. Adding it to DeviceSource would give every
// future source - a file, the generator, a Soapy device whose vendor module
// has no such concept - a method it has to answer, and the honest answer for
// most of them is "there is no such thing here".
//
// So the panel asks the concrete type, in exactly one place. Seven drivers
// have one this panel can reach, all spelled setBiasT()/biasT():
//   HackRfSource     - always present on a HackRF One
//   AirspySource     - always present (a GPIO write, see its setBiasT)
//   AirspyHfSource   - only on some boards, which is why the driver ASKS at
//                      open (GET_BIAS_TEE_COUNT) and answers biasTeeSupported()
//   SdrPlaySource    - per model, and per ANTENNA on an RSPdx, which is why it
//                      too answers biasTeeSupported() rather than assuming
//   MiriSdrSource    - a bit in the band-switch word, always writable (its own
//                      header records the two bands where it does nothing)
//   Rx888Source      - the HF port's. The VHF port has a SECOND one, reached
//                      through setVhfBiasT, and it is deliberately not on this
//                      checkbox: one box that meant a different connector
//                      depending on the tuned frequency is exactly the kind of
//                      control that puts power somewhere nobody expected.
//   RtlSdrSource     - GPIO 0 of the RTL2832U, on every dongle. Whether
//                      anything is WIRED to that pin is the dongle maker's
//                      business (an RTL-SDR Blog V3/V4 and most NooElec
//                      sticks have a bias tee there; a bare television stick
//                      has nothing), so the box is offered on every native
//                      RTL-SDR and the tooltip says what it does. What is put
//                      back after an open is the RTL-SDR's own rule - see
//                      rtlBiasTeeAtOpen below - because the dongle's EEPROM
//                      has an opinion and the others' firmware does not.
// A SoapySDR-opened dongle is not here: it is a SoapySource, and its vendor
// module's bias tee is a module setting, not this switch.
//
// `fn` is called with the concrete driver when there is one; false is
// returned untouched when there is not, which is the common case.
template <typename Fn>
bool withBiasTee(cascade::source::DeviceSource* dev, Fn&& fn) {
    if (auto* h = dynamic_cast<cascade::source::HackRfSource*>(dev)) { return fn(*h); }
    if (auto* a = dynamic_cast<cascade::source::AirspySource*>(dev)) { return fn(*a); }
    if (auto* hf = dynamic_cast<cascade::source::AirspyHfSource*>(dev)) {
        if (!hf->biasTeeSupported()) { return false; }
        return fn(*hf);
    }
    if (auto* sp = dynamic_cast<cascade::source::SdrPlaySource*>(dev)) {
        if (!sp->biasTeeSupported()) { return false; }
        return fn(*sp);
    }
    if (auto* m = dynamic_cast<cascade::source::MiriSdrSource*>(dev)) { return fn(*m); }
    if (auto* r = dynamic_cast<cascade::source::Rx888Source*>(dev)) { return fn(*r); }
    if (auto* rtl = dynamic_cast<cascade::source::RtlSdrSource*>(dev)) { return fn(*rtl); }
    return false;
}

// THE PANEL'S STATE: what the box shows, and the two memories behind it.
//
//   present - draw the box at all (the open radio has a bias tee we reach)
//   shown   - the box. ALWAYS the driver's readback, never the request: a
//             control transfer the radio refused leaves it where it was,
//             because a ticked box over a port with no power on it (or an
//             unticked one over a port that has) is the lie the antenna combo
//             was fixed for.
//   other   - AppConfig::nativeBiasT: the one remembered setting for the six
//             other radios, applied after each of their opens exactly as
//             before the RTL-SDR joined (see config.hpp for why it is one bool)
//   rtlArgs / rtlOn - AppConfig::rtlBiasTArgs / rtlBiasT: the RTL-SDR's OWN
//             memory - which dongle the user last ticked or unticked, by its
//             saved args, and which way.
//
// WHY THE RTL-SDR HAS A SEPARATE MEMORY rather than sharing `other`. Sharing
// it would carry an "on" ticked for a HackRF on the bench straight onto
// whatever dongle is opened next, and the cheapest dongles - the ones with no
// bias-tee circuit worth the name - are exactly the ones most likely to be
// plugged into something that does not expect 4.5 V. And it would work the
// other way too: an evening on an RTL-SDR would overwrite the HackRF's
// setting with the dongle's readback, and the user's mast-head amplifier
// would be dark when they went back to it.
struct BiasTeePanel {
    bool present = false;
    bool shown = false;
    bool other = false;
    std::string rtlArgs;
    bool rtlOn = false;
};

// DOES THIS ARGS STRING NAME ONE DONGLE, rather than a place in a list?
// "serial=XXXXXXXX" names the dongle whose EEPROM carries that serial;
// "index=0" names whichever RTL2832U the USB walk happens to find first, which
// is a different dongle the day a second one is plugged in.
inline bool rtlArgsNameADongle(const std::string& args) {
    return !cascade::source::argValue(args, "serial").empty();
}

enum class RtlBiasTeeAtOpen {
    LeaveAsOpened,  // keep what the driver's own open() did (the EEPROM's rule)
    ApplyOn,
    ApplyOff,
};

// THE RTL-SDR's RULE FOR WHAT IS PUT BACK AFTER AN OPEN. The driver's open()
// has already applied the EEPROM's own policy - off, unless a VALID EEPROM
// (header 0x28 0x32) has byte 7 bit 1 clear, meaning the maker wired the bias
// tee permanently on. On top of that:
//
//   * A memory that belongs to a DIFFERENT dongle (or to none) changes
//     nothing. A factory-forced dongle therefore opens ticked, as it always
//     has, and every other dongle opens unticked.
//   * A remembered "off" for THIS dongle is applied. Switching power off is
//     never the dangerous direction, and it is what honours a user who
//     unticked a factory-forced dongle: without it the EEPROM would switch it
//     back on at every open.
//   * A remembered "on" is applied ONLY when all three hold:
//       - the memory is for exactly these args,
//       - those args name a dongle by SERIAL, not by index, and
//       - this dongle's EEPROM is valid.
//
// THE THIRD CONDITION IS THE ONE THAT MATTERS, and it is why this is not the
// other radios' rule. A dongle with no EEPROM reads every byte as zero, has
// no serial of its own, and cannot be told apart from the next EEPROM-less
// dongle plugged into the same socket - so "the same dongle as last time" is
// a claim nothing can check. Re-applying a remembered "on" to it would put
// 4.5 V on the antenna of whatever cheap stick happens to answer, which is the
// exact accident the driver's EEPROM-header check exists to prevent. So a
// dongle with no EEPROM is only ever switched on by a tick in this session;
// the tick is still remembered, and still harmless, because this rule will
// not act on it. The safer answer costs such a user one click per launch.
//
// A RESIDUAL, stated rather than hidden: two dongles programmed with the SAME
// serial (many ship as 00000001) are indistinguishable here, exactly as they
// are to the saved-radio restore itself - which is why rtl_eeprom -s to give
// each dongle its own serial is the fix, not anything this rule could do.
inline RtlBiasTeeAtOpen rtlBiasTeeAtOpen(const std::string& rememberedArgs, bool rememberedOn,
                                         const std::string& openedArgs, bool eepromValid) {
    if (rememberedArgs.empty() || rememberedArgs != openedArgs) {
        return RtlBiasTeeAtOpen::LeaveAsOpened;
    }
    if (!rememberedOn) { return RtlBiasTeeAtOpen::ApplyOff; }
    if (!rtlArgsNameADongle(openedArgs) || !eepromValid) {
        return RtlBiasTeeAtOpen::LeaveAsOpened;
    }
    return RtlBiasTeeAtOpen::ApplyOn;
}

// AFTER EVERY OPEN (AppWindow::adoptDeviceMirrors). `args` is what the radio
// was opened with - the same string the config saves as nativeArgs.
//
// Every one of the other drivers switches its bias tee OFF as part of open()
// - deliberately, so that a previous application cannot leave power on an
// antenna port with nothing on screen saying so - which means their saved
// setting has to be re-applied here or a mast-head amplifier would go dark on
// every launch. The RTL-SDR follows rtlBiasTeeAtOpen instead, and an open
// never rewrites its memory: that changes only when the user ticks the box.
//
// A RADIO WITHOUT ONE LEAVES BOTH MEMORIES ALONE. The checkbox is not drawn
// for it, so nothing on screen can claim power that is not there - but a user
// who ticked it for the HackRF on their bench and then spent an evening on a
// file or a Soapy radio should not find it unticked when they go back.
inline void biasTeeAfterOpen(BiasTeePanel& p, cascade::source::DeviceSource& dev,
                             const std::string& args) {
    p.shown = false;
    p.present = withBiasTee(&dev, [](auto&) { return true; });
    if (!p.present) { return; }
    if (auto* rtl = dynamic_cast<cascade::source::RtlSdrSource*>(&dev)) {
        switch (rtlBiasTeeAtOpen(p.rtlArgs, p.rtlOn, args, rtl->eepromValid())) {
            case RtlBiasTeeAtOpen::ApplyOn: rtl->setBiasT(true); break;
            case RtlBiasTeeAtOpen::ApplyOff: rtl->setBiasT(false); break;
            case RtlBiasTeeAtOpen::LeaveAsOpened: break;
        }
        p.shown = rtl->biasT();
        return;
    }
    withBiasTee(&dev, [&p](auto& d) {
        d.setBiasT(p.other);
        p.other = d.biasT();
        p.shown = p.other;
        return true;
    });
}

// THE USER TICKED OR UNTICKED THE BOX. The box then shows the readback, and
// the memory for this kind of radio records what the radio actually did - on
// a refusal, neither changes, and the reason goes to `error`.
inline void biasTeeTicked(BiasTeePanel& p, cascade::source::DeviceSource* dev,
                          const std::string& args, bool want, std::string* error) {
    withBiasTee(dev, [&](auto& d) {
        const bool accepted = d.setBiasT(want);
        if (!accepted && error != nullptr) { *error = d.lastError(); }
        p.shown = d.biasT();
        if (dynamic_cast<cascade::source::RtlSdrSource*>(dev) != nullptr) {
            if (accepted) {
                p.rtlArgs = args;
                p.rtlOn = p.shown;
            }
        } else {
            p.other = p.shown;
        }
        return true;
    });
}

}  // namespace cascade::gui
