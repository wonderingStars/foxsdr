// soundcard_panel.hpp - the Source section's sound card rules that a test can
// reach: how the saved settings map to the source's, and how a TUNE works on a
// source whose centre cannot move.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <string>

#include "core/config.hpp"
#include "source/soundcard_source.hpp"

namespace cascade::gui {

// AppConfig::SoundCard <-> SoundCardSettings. The config spells the format as
// a word so a hand-edited file reads as what it means; the loader has already
// replaced anything it cannot read with the default.
inline cascade::source::SoundCardSettings soundCardFromConfig(
    const cascade::core::AppConfig::SoundCard& c) {
    cascade::source::SoundCardSettings s;
    s.device = c.device;
    s.hostApi = c.hostApi;
    s.cardRateHz = c.rateHz;
    s.format = (c.format == "iq") ? cascade::source::SoundCardFormat::IqStereo
                                  : cascade::source::SoundCardFormat::RealMono;
    s.channel = (c.channel == 1) ? 1 : 0;
    s.swapIq = c.swapIq;
    s.iqCentreHz = c.centreHz;
    return s;
}

inline cascade::core::AppConfig::SoundCard soundCardToConfig(
    const cascade::source::SoundCardSettings& s) {
    cascade::core::AppConfig::SoundCard c;
    c.device = s.device;
    c.hostApi = s.hostApi;
    c.rateHz = s.cardRateHz;
    c.format = (s.format == cascade::source::SoundCardFormat::IqStereo) ? "iq" : "real";
    c.channel = (s.channel == 1) ? 1 : 0;
    c.swapIq = s.swapIq;
    c.centreHz = s.iqCentreHz;
    return c;
}

// A PATCH RADIO ON A SOUND CARD. Its key names only the card
// ("device=...,api=..."), because a patch cannot set a format; so the card
// opens the way the Source section has it set up when it is the same card -
// real or I/Q, the channel, the swap, the centre - and as real mono on the
// left channel otherwise. The patch's own rate is applied after the open.
inline std::string soundCardArgsForPatch(const std::string& keyArgs,
                                         const cascade::source::SoundCardSettings& section) {
    cascade::source::SoundCardSettings s;
    if (!cascade::source::parseSoundCardArgs(keyArgs, s)) { return keyArgs; }
    if (s.device == section.device && s.hostApi == section.hostApi) {
        const std::string device = s.device;
        const std::string api = s.hostApi;
        s = section;
        s.device = device;
        s.hostApi = api;
    }
    return cascade::source::soundCardArgs(s);
}

// TUNING A SOURCE THAT HAS NO TUNER.
//
// Every tune in the application - the frequency counter, a bookmark, a band
// preset, the scanner, a plugin's preset, the web remote - ends up asking the
// SOURCE for a new centre with the VFO offset kept (AppWindow::retuneSourceHz).
// A sound card cannot move: in real mode its centre is fs/4 by arithmetic, and
// in I/Q mode it is wherever the external receiver was set by hand. Asked to
// move it would refuse, and a user typing "16.4 kHz" would get a refusal for a
// frequency that is plainly on the screen.
//
// So the request is turned into what it means - put the VFO on the absolute
// frequency the caller wanted (the requested centre plus the offset it was
// keeping) - and the VFO moves inside the span instead.
//
// WHERE THE VFO CAN GO is not the whole span: the receive filter has to fit
// inside it too, so the VFO's reach is the span less half the filter at each
// end - exactly the limit AppWindow::setVfoToAbsoluteHz holds every VFO move
// to (vfoOffsetInsideSpan below, shared by both). A frequency outside that
// reach is REFUSED, and `inside` says so, with the reach in loHz..hiHz for the
// sentence; it is never clamped to the nearest edge, which would tune
// somewhere nobody asked for. A filter wider than the whole span leaves no
// reach at all (`tooWide`), and every tune is refused with that reason.
struct FixedCentreTune {
    bool inside = false;
    bool tooWide = false;    // the filter is wider than what the source receives
    double wantAbsHz = 0.0;  // where the VFO goes (absolute, Hz)
    double loHz = 0.0;       // the VFO's reach with this filter (absolute, Hz)
    double hiHz = 0.0;
};

// THE ONE LIMIT ON THE VFO: the offset from the source's centre that keeps the
// whole filter inside +/- inputRate/2. A filter wider than the span has
// nowhere to go but the middle, so the VFO is centred (offset 0) - never left
// where it was, half outside what the source delivers.
inline double vfoOffsetInsideSpan(double offsetHz, double inputRateHz, double bandwidthHz) {
    const double lim = 0.5 * inputRateHz - 0.5 * bandwidthHz;
    if (!(lim > 0.0)) { return 0.0; }
    return offsetHz < -lim ? -lim : (offsetHz > lim ? lim : offsetHz);
}

inline FixedCentreTune tuneWithFixedCentre(double requestedCentreHz, double vfoOffsetHz,
                                           double fixedCentreHz, double rateHz,
                                           double bandwidthHz) {
    FixedCentreTune t;
    t.wantAbsHz = requestedCentreHz + vfoOffsetHz;
    const double lim = 0.5 * rateHz - 0.5 * bandwidthHz;
    t.tooWide = !(lim > 0.0);
    t.loHz = fixedCentreHz - (t.tooWide ? 0.0 : lim);
    t.hiHz = fixedCentreHz + (t.tooWide ? 0.0 : lim);
    t.inside = !t.tooWide && std::isfinite(t.wantAbsHz) && t.wantAbsHz >= t.loHz &&
               t.wantAbsHz <= t.hiHz;
    return t;
}

// THE CENTRE BOX takes effect at once only when the card RUNNING is in I/Q
// mode: its centre is then a record of where the external receiver is tuned,
// and the whole receiver follows it. A card running in REAL mode has a centre
// fixed by arithmetic, so a centre typed while the section is set up for I/Q
// (and not yet reopened) is kept for the next Open and not sent to the live
// source - which would refuse it, and say so, for a frequency nobody tuned.
inline bool soundCardCentreAppliesLive(bool liveIsSoundCard, bool openPending,
                                       cascade::source::SoundCardFormat liveFormat) {
    return liveIsSoundCard && !openPending &&
           liveFormat == cascade::source::SoundCardFormat::IqStereo;
}

// WHAT THE PATCH PAGE TAKES FROM THE RECEIVER when it starts (the receiver
// then runs on the signal generator until the patch stops, and gets it back).
// A radio is taken once it is open; so is the receiver's SOUND CARD, under
// the same key the patch's device list gives that card - otherwise a patch
// radio on the same card would open a second stream on it, which WASAPI
// exclusive mode refuses outright. Nothing is taken while an open is still
// resolving, or from a file or the generator.
struct ReceiverLoan {
    bool take = false;
    std::string kind;  // the device key's driver
    std::string args;  // the device key's args
};

inline ReceiverLoan receiverSourceForPatch(bool patchRunning, const std::string& sourceKind,
                                           bool hasDevice, bool deviceOpenPending,
                                           const std::string& deviceArgs, bool soundCardOpenPending,
                                           const cascade::source::SoundCardSettings& card) {
    ReceiverLoan l;
    // A radio open under way blocks every source switch (selectSource), so
    // nothing can be handed over until it has resolved.
    if (!patchRunning || deviceOpenPending) { return l; }
    if (hasDevice) {
        l.take = true;
        l.kind = sourceKind;
        l.args = deviceArgs;
        return l;
    }
    if (sourceKind == "soundcard" && !soundCardOpenPending && !card.device.empty()) {
        l.take = true;
        l.kind = "soundcard";
        l.args = cascade::source::soundCardDeviceArgs(card.device, card.hostApi);
    }
    return l;
}

}  // namespace cascade::gui
