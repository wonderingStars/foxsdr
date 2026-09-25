// soundcard_panel.hpp - the Source section's sound card rules that a test can
// reach: how the saved settings map to the source's, and how a TUNE works on a
// source whose centre cannot move.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>

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
// keeping) - and the VFO moves inside the span instead. A frequency the source
// does not receive at all is refused, and `inside` says so; it is never
// clamped to the nearest edge, which would tune somewhere nobody asked for.
struct FixedCentreTune {
    bool inside = false;
    double wantAbsHz = 0.0;  // where the VFO goes (absolute, Hz)
    double loHz = 0.0;       // what the source covers
    double hiHz = 0.0;
};

inline FixedCentreTune tuneWithFixedCentre(double requestedCentreHz, double vfoOffsetHz,
                                           double fixedCentreHz, double rateHz) {
    FixedCentreTune t;
    t.wantAbsHz = requestedCentreHz + vfoOffsetHz;
    t.loHz = fixedCentreHz - rateHz / 2.0;
    t.hiHz = fixedCentreHz + rateHz / 2.0;
    t.inside = std::isfinite(t.wantAbsHz) && rateHz > 0.0 && t.wantAbsHz >= t.loHz &&
               t.wantAbsHz <= t.hiHz;
    return t;
}

}  // namespace cascade::gui
