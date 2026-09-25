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

#include <map>
#include <string>
#include <vector>

#include "core/bias_tee_memory.hpp"
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

// THE PANEL'S STATE: what the box shows, and the memory behind it.
//
//   present - draw the box at all (the open radio has a bias tee we reach)
//   shown   - the box. ALWAYS the driver's readback, never the request: a
//             control transfer the radio refused leaves it where it was,
//             because a ticked box over a port with no power on it (or an
//             unticked one over a port that has) is the lie the antenna combo
//             was fixed for.
//   remembered - AppConfig::biasTee: PER RADIO, keyed by
//             core::biasTeeRadioKey (the driver and the serial), what the user
//             last switched each radio to and the radio accepted.
//
// ONE MEMORY PER RADIO, FOR EVERY FAMILY (repair round 1 of the deck key,
// 2026-09-25). There used to be two: nativeBiasT, ONE bool that every HackRF,
// Airspy, Airspy HF+, SDRplay, Mirics and RX888 opened with - so an "on" given
// for the mast-head amplifier behind one radio put 4.5 V on the next radio of
// any of those families at its open, silently, this launch and every later
// one - and the RTL-SDR's own slot, which held one dongle (switching a second
// dongle off forgot the first dongle's "on"). Now a radio the user switched
// on is switched on again at ITS OWN next open, whether later this session or
// at a later launch, and no other radio ever inherits it. What is put back and
// when is biasTeeAfterOpen's; what is written and when is biasTeeRemember's.
struct BiasTeePanel {
    bool present = false;
    bool shown = false;
    std::map<std::string, bool> remembered;
};

// WHAT A SWITCH THE RADIO ACCEPTED WRITES INTO THE MEMORY, for the radio
// `kind` opened with `args`, now reading back `on`:
//   * an ON is remembered only for a radio named by SERIAL. A radio known only
//     by its place in a USB walk ("index=0") cannot be told from the next one
//     in the same socket, so its "on" is not kept - it opens as its driver
//     opens it every time (off; an RTL-SDR whose EEPROM says the maker wired
//     it on, on - that is the dongle's own rule, not a memory) - and any
//     earlier entry for it is dropped;
//   * an OFF is remembered for any radio. Switching power off is never the
//     dangerous direction, and it is what keeps an RTL-SDR whose EEPROM forces
//     the bias tee on OFF at its next open after the user switched it off.
// The map is capped (core::kBiasTeeMemoryCap): a new radio past the cap is not
// remembered, which errs towards no power.
inline void biasTeeRemember(BiasTeePanel& p, const std::string& kind, const std::string& args,
                            bool on) {
    const std::string key = cascade::core::biasTeeRadioKey(kind, args);
    if (on && !cascade::core::biasTeeArgsNameARadio(args)) {
        p.remembered.erase(key);
        return;
    }
    if (p.remembered.count(key) == 0 && p.remembered.size() >= cascade::core::kBiasTeeMemoryCap) {
        return;
    }
    p.remembered[key] = on;
}

// What the memory holds for this radio: 1 on, 0 off, -1 nothing.
inline int biasTeeRecalled(const BiasTeePanel& p, const std::string& kind,
                           const std::string& args) {
    const auto it = p.remembered.find(cascade::core::biasTeeRadioKey(kind, args));
    if (it == p.remembered.end()) { return -1; }
    return it->second ? 1 : 0;
}

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
// each dongle its own serial is the fix, not anything this rule could do. The
// per-radio memory (core::biasTeeRadioKey, the driver and the serial) keeps
// that residual exactly: two dongles sharing a serial share ONE entry, so an
// "on" remembered for one is applied to the other at its open. The same holds
// for any two radios of one family that report the same serial.
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
// antenna port with nothing on screen saying so - which means a remembered
// "on" has to be re-applied here or a mast-head amplifier would go dark on
// every launch. It is applied only from THIS radio's own entry, and only when
// the args name it by serial (biasTeeRemember never writes an "on" for any
// other, but a hand-edited file could). The RTL-SDR follows rtlBiasTeeAtOpen
// against its own entry, because its EEPROM has an opinion the others'
// firmware does not. An open never rewrites the memory: that changes only when
// the user switches the radio, with the checkbox or the deck's key.
//
// A RADIO WITHOUT ONE LEAVES THE MEMORY ALONE. The checkbox is not drawn for
// it, so nothing on screen can claim power that is not there - and a user who
// switched it on for the HackRF on their bench and then spent an evening on a
// file or a Soapy radio should not find it off when they go back.
inline void biasTeeAfterOpen(BiasTeePanel& p, cascade::source::DeviceSource& dev,
                             const std::string& args) {
    p.shown = false;
    p.present = withBiasTee(&dev, [](auto&) { return true; });
    if (!p.present) { return; }
    const int recalled = biasTeeRecalled(p, dev.driverKey(), args);
    if (auto* rtl = dynamic_cast<cascade::source::RtlSdrSource*>(&dev)) {
        switch (rtlBiasTeeAtOpen(recalled < 0 ? std::string() : args, recalled == 1, args,
                                 rtl->eepromValid())) {
            case RtlBiasTeeAtOpen::ApplyOn: rtl->setBiasT(true); break;
            case RtlBiasTeeAtOpen::ApplyOff: rtl->setBiasT(false); break;
            case RtlBiasTeeAtOpen::LeaveAsOpened: break;
        }
        p.shown = rtl->biasT();
        return;
    }
    const bool on = recalled == 1 && cascade::core::biasTeeArgsNameARadio(args);
    withBiasTee(&dev, [&p, on](auto& d) {
        d.setBiasT(on);
        p.shown = d.biasT();
        return true;
    });
}

// THE USER SWITCHED IT - the Source panel's checkbox or the deck's key, which
// both come here. The box and the key then show the readback, and the memory
// for THIS radio records what the radio actually did (biasTeeRemember); on a
// refusal nothing is remembered, and the reason goes to `error`.
inline void biasTeeTicked(BiasTeePanel& p, cascade::source::DeviceSource* dev,
                          const std::string& args, bool want, std::string* error) {
    withBiasTee(dev, [&](auto& d) {
        const bool accepted = d.setBiasT(want);
        if (!accepted && error != nullptr) { *error = d.lastError(); }
        p.shown = d.biasT();
        if (accepted) { biasTeeRemember(p, dev->driverKey(), args, p.shown); }
        return true;
    });
}

// --- THE DECK'S BIAS TEE KEY (2026-09-25) ------------------------------------
//
// A tester asked for a bias-tee button, and the owner's words were "add bias
// tee to the main panel": the checkbox above lives in the Source section, a
// rail bank and a scroll away from the deck a user actually looks at. So the
// deck carries a key for it - drawn ONLY while the open radio has a bias tee
// this panel reaches (BiasTeePanel::present), its lamp lit exactly when the
// driver's readback says the power is on (BiasTeePanel::shown), and every
// change it makes goes through biasTeeTicked, the checkbox's own path. There
// is one state and two controls showing it, so they cannot disagree, and a
// refusal leaves both where they were.
//
// WHAT THE KEY ADDS IS A GATE ON THE DANGEROUS DIRECTION. The checkbox is a
// labelled control inside a settings panel, reached on purpose. A key on the
// deck sits beside START, under the hand, and a stray click there must not put
// about 4.5 V up a coax that may end in something not built to take it. So:
//
//   * OFF IS ALWAYS IMMEDIATE. Taking power off is never the harmful way, and
//     a user reaching for it in a hurry must not meet a question.
//   * ON ASKS, the first time for each radio in a session: a confirmation
//     dialog, which is the application's existing way of making an action
//     deliberate ("Stop following?", "Sound is muted"). Press-and-hold was the
//     alternative and was not chosen: nothing on this deck is held to confirm
//     (the one held key in FoxSDR is the transmitter's PTT, which means "while
//     held", the opposite contract), a short click on a hold key does nothing
//     at all and reads as a broken key, and a hold cannot carry the warning -
//     the dialog puts the same sentence the checkbox's tooltip carries in front
//     of the user at exactly the moment it matters.
//   * ONCE CONFIRMED FOR A RADIO, later presses in the same session switch at
//     once - but only for a radio named by its SERIAL. "index=0" names a place
//     in a USB walk, which is a different radio the day a second one is
//     plugged in (the same argument rtlArgsNameADongle makes), so a radio known
//     only by position is asked every time. The confirmation itself is not
//     saved: it lasts the session.
//
// WHAT AN "ON" LEAVES BEHIND IS NOT THE GATE'S BUSINESS, and it is the same
// whichever control switched it: biasTeeTicked writes this radio's own entry
// in the per-radio memory (biasTeeRemember), so a serial-named radio switched
// on comes up on again at its own next open - this launch or a later one -
// until the user switches it off, and no other radio ever inherits it. The
// dialog says exactly that (or, for a radio known only by position, that it
// will open off next time).
//
// WHY THE CONFIRMATION IS PER SESSION AND NOT "PER REMEMBERED RADIO". A radio
// whose "on" is remembered is already on when it opens, so the key is lit and
// a press switches it OFF, which is never asked about; a remembered radio the
// key finds dark is one the user switched off (which the memory records) or
// one whose driver refused the "on" at open. Either way the memory has nothing
// to vouch for, so it cannot stand in for the question; the session gate only
// spares a user toggling an amplifier on and off while they check a cable
// from being asked every time.
//
// No ImGui here: AppWindow draws the key and the dialog, and these functions
// decide what a press and an answer DO, so tests/test_bias_key.cpp can hold
// them without an open frame.
// THE CENSUS AND CAPTURE STAND-IN (FOXSDR_FORCE_BIAS_KEY=accept|refuse): with
// no radio with a bias tee open, the key is drawn over a stand-in that takes
// or refuses every change, so the theme census can place the key in every
// theme and a test can press it. BOUNDED RUNS ONLY (--frames), like
// FOXSDR_INPUT_SCRIPT: an interactive launch ignores the variable, so a stray
// environment can never put a key on a user's deck that switches nothing.
enum class BiasStandIn { None, Accept, Refuse };
inline BiasStandIn biasStandInFor(const char* value, bool boundedRun) {
    if (!boundedRun || value == nullptr) { return BiasStandIn::None; }
    const std::string v(value);
    if (v == "accept") { return BiasStandIn::Accept; }
    if (v == "refuse") { return BiasStandIn::Refuse; }
    return BiasStandIn::None;
}

struct BiasKeyGate {
    // The radios ("kind|args") whose first switch-on was confirmed this
    // session. Only serial-named radios are ever added.
    std::vector<std::string> confirmed;
    // The dialog is up, asking about this radio ("kind|args").
    bool asking = false;
    std::string askingFor;
};

enum class BiasKeyAction {
    Nothing,    // no bias tee on the open radio: the key is not even drawn
    SwitchOff,  // immediate, always
    SwitchOn,   // confirmed earlier this session for this very radio
    Ask,        // the dialog opens; nothing is switched yet
};

// The identity a confirmation is remembered under - the per-radio memory's own
// key (core::biasTeeRadioKey: the driver and the serial), so the gate and the
// memory cannot disagree about which radio is which.
inline std::string biasKeyRadio(const std::string& kind, const std::string& args) {
    return cascade::core::biasTeeRadioKey(kind, args);
}

// Whether this radio is named by serial: a confirmation may then be kept for
// the session, and an "on" is kept in the per-radio memory (biasTeeRemember).
inline bool biasKeyMayRemember(const std::string& args) {
    return cascade::core::biasTeeArgsNameARadio(args);
}

inline bool biasKeyConfirmedFor(const BiasKeyGate& g, const std::string& radio) {
    for (const std::string& r : g.confirmed) {
        if (r == radio) { return true; }
    }
    return false;
}

// THE KEY WAS PRESSED. `radio` is biasKeyRadio(kind, args) of the radio open
// now. Decides from the READBACK (p.shown), never from what the key last
// asked for: a lamp that is lit means power is on the port, so the press takes
// it off.
inline BiasKeyAction biasKeyPress(BiasKeyGate& g, const BiasTeePanel& p,
                                  const std::string& radio) {
    if (!p.present) { return BiasKeyAction::Nothing; }
    if (p.shown) { return BiasKeyAction::SwitchOff; }
    if (biasKeyConfirmedFor(g, radio)) { return BiasKeyAction::SwitchOn; }
    g.asking = true;
    g.askingFor = radio;
    return BiasKeyAction::Ask;
}

// THE DIALOG WAS ANSWERED "turn it on". True means switch on now. It is false
// - and nothing is remembered - when the question no longer applies: the
// radio it asked about is not the one open now, or the open radio has no bias
// tee any more. `mayRemember` is biasKeyMayRemember(args) of that radio.
inline bool biasKeyConfirm(BiasKeyGate& g, const BiasTeePanel& p, const std::string& radio,
                           bool mayRemember) {
    const bool applies = g.asking && p.present && radio == g.askingFor;
    g.asking = false;
    g.askingFor.clear();
    if (!applies) { return false; }
    if (mayRemember && !biasKeyConfirmedFor(g, radio)) { g.confirmed.push_back(radio); }
    return true;
}

// THE DIALOG WAS ANSWERED "cancel", or withdrawn: nothing is switched and
// nothing is remembered.
inline void biasKeyCancel(BiasKeyGate& g) {
    g.asking = false;
    g.askingFor.clear();
}

// Whether a dialog that is up still asks a question that applies: the radio
// it names is still the open one, and still has a bias tee. The dialog closes
// itself when this turns false, the way "Stop following?" does.
inline bool biasKeyQuestionStands(const BiasKeyGate& g, const BiasTeePanel& p,
                                  const std::string& radio) {
    return g.asking && p.present && radio == g.askingFor;
}

}  // namespace cascade::gui
