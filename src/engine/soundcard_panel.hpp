// soundcard_panel.hpp - the Source section's sound card rules that a test can
// reach: how the saved settings map to the source's, and how a TUNE works on a
// source whose centre cannot move.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/freq_converter.hpp"
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
//
// "The same card" is the card's IDENTITY (source::sameSoundCard): a patch
// saved while ALSA called the card hw:1,0 still matches the section's card
// after a boot that calls it hw:2,0. The key's own name is kept - it is what
// the open resolves (matchSoundCard, by the same identity).
inline std::string soundCardArgsForPatch(const std::string& keyArgs,
                                         const cascade::source::SoundCardSettings& section) {
    cascade::source::SoundCardSettings s;
    if (!cascade::source::parseSoundCardArgs(keyArgs, s)) { return keyArgs; }
    if (cascade::source::sameSoundCard(s.device, s.hostApi, section.device, section.hostApi)) {
        const std::string device = s.device;
        const std::string api = s.hostApi;
        s = section;
        s.device = device;
        s.hostApi = api;
    }
    return cascade::source::soundCardArgs(s);
}

// THE CONVERTER IN FRONT OF A SOUND CARD (0.99.37's converter, merged in).
//
// KEPT PER CARD, under the same key a patch radio on that card has -
// core::converterRadioKey("soundcard", "device=...,api=...") - so two cards
// never share one setting (the receiver's key used to be "soundcard|" for
// every card, because a card leaves deviceArgs_ empty).
//
// WHAT A CARD CAN HAVE depends on its format:
//   - I/Q (stereo): NOTHING. The centre typed in the Source section is where
//     the external receiver is tuned - it already IS the translation from
//     the card's baseband to the air. A converter on top would store the
//     radio frequency in the source while the section and the config kept
//     the typed air one, and the next launch would translate it twice.
//   - Real (mono): a DOWN-converter only. The card samples 0 .. rate/2 (a few
//     tens of kHz); an up-converter's output is its LO and above - VHF -
//     which no sound card samples. A stored Up (another session, a key
//     shared with a patch radio) reads as Off, and the section says why.
//
// KEYED BY THE CARD'S IDENTITY (source::soundCardIdentityName): on Linux the
// ALSA name carries a card number that can change at every boot, so the key
// has "(hw:*,M)" where the name has "(hw:N,M)" - otherwise a renumbered boot
// would find no converter for the card. Every other host API keys by the
// exact name, as before. A patch radio's key (the full name) is brought to
// the same form before any lookup (soundCardCanonicalKey).
inline std::string soundCardConverterKey(const std::string& device, const std::string& hostApi) {
    return cascade::core::converterRadioKey(
        "soundcard",
        cascade::source::soundCardDeviceArgs(cascade::source::soundCardIdentityName(device, hostApi), hostApi));
}

// The card's device args out of a converter key; false for any other key.
inline bool soundCardKeyArgs(const std::string& key, std::string& args) {
    static const std::string prefix = cascade::core::converterRadioKey("soundcard", "");
    if (key.compare(0, prefix.size(), prefix) != 0) { return false; }
    args = key.substr(prefix.size());
    return true;
}

// Any key naming a sound card, in the form its converter is kept under; any
// other key unchanged.
inline std::string soundCardCanonicalKey(const std::string& key) {
    std::string args;
    if (!soundCardKeyArgs(key, args)) { return key; }
    cascade::source::SoundCardSettings s;
    if (!cascade::source::parseSoundCardArgs(args, s)) { return key; }
    return soundCardConverterKey(s.device, s.hostApi);
}

// A config written before the converters were keyed by identity names an
// ALSA card by its full name: moved to the identity key when the config is
// read. A key already in the identity form wins over a moved one, and of two
// old keys for the same card (two boots' numbers) the first in the map wins.
inline std::map<std::string, cascade::core::ConverterSetting> migrateSoundCardConverterKeys(
    const std::map<std::string, cascade::core::ConverterSetting>& in) {
    std::map<std::string, cascade::core::ConverterSetting> out;
    for (const auto& [key, s] : in) {
        if (soundCardCanonicalKey(key) == key) { out[key] = s; }
    }
    for (const auto& [key, s] : in) {
        const std::string canon = soundCardCanonicalKey(key);
        if (canon != key) { out.emplace(canon, s); }
    }
    return out;
}

struct SoundCardConverter {
    cascade::core::ConverterSetting effective;  // what the pipeline applies
    bool offered = true;                        // the Converter controls are drawn
    bool upRefused = false;                     // a stored up-converter reads as Off
};

inline SoundCardConverter soundCardConverter(const cascade::core::ConverterSetting& stored,
                                             cascade::source::SoundCardFormat format) {
    SoundCardConverter c;
    c.effective = stored;
    if (format == cascade::source::SoundCardFormat::IqStereo) {
        c.effective.mode = cascade::core::ConverterMode::Off;
        c.offered = false;
        return c;
    }
    if (stored.mode == cascade::core::ConverterMode::Up) {
        c.effective.mode = cascade::core::ConverterMode::Off;
        c.upRefused = cascade::core::converterLoValid(stored.loHz);
    }
    return c;
}

// The format the card behind a converter key runs - or would open - in: the
// running card's own when the receiver is on it; otherwise what a patch radio
// on that card opens with (soundCardArgsForPatch: the section's settings for
// the same card, real mono for any other).
inline cascade::source::SoundCardFormat soundCardFormatForKey(
    const std::string& keyArgs, bool receiverOnCard, const cascade::source::SoundCardSettings& live,
    const cascade::source::SoundCardSettings& section) {
    cascade::source::SoundCardSettings k;
    if (!cascade::source::parseSoundCardArgs(keyArgs, k)) {
        return cascade::source::SoundCardFormat::RealMono;
    }
    if (receiverOnCard && cascade::source::sameSoundCard(k.device, k.hostApi, live.device, live.hostApi)) {
        return live.format;
    }
    cascade::source::SoundCardSettings opened;
    if (!cascade::source::parseSoundCardArgs(soundCardArgsForPatch(keyArgs, section), opened)) {
        return cascade::source::SoundCardFormat::RealMono;
    }
    return opened.format;
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
//
// THE CARD IS DESCRIBED BY WHAT IS RUNNING, never by the Source section's
// controls: those are what the user is editing and has not necessarily
// Opened, and the card handed back when the patch stops has to come back as
// it was taken. So `card` is the RUNNING card's settings (AppWindow::
// soundCardLive_), and the loan carries them for the hand-back.
struct ReceiverLoan {
    bool take = false;
    std::string kind;  // the device key's driver
    std::string args;  // the device key's args
    cascade::source::SoundCardSettings card;  // a sound card: as it was running when taken
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
        l.card = card;
    }
    return l;
}

// RE-OPENING THE CARD THAT IS RUNNING. Windows will not open a second stream
// on a card that is in exclusive use, nor exclusive mode on a card that is
// already streaming (IAudioClient::Initialize, AUDCLNT_E_DEVICE_IN_USE, which
// PortAudio reports as an invalid device) - so a new rate, a new channel or a
// new format on the SAME card cannot be opened beside the stream it replaces.
// True when `want` names the card that is running (`live`): the application
// then releases it first, and opens `want` with `live` to fall back on
// (source::openSoundCardOrRestore). A DIFFERENT card keeps the other order -
// the new one opens while the old one still runs, and a refusal leaves the
// old one exactly as it was. So does a `want` that is ambiguous (two identical
// ALSA cards): its open is refused whatever happens, and releasing the
// running card for it would only interrupt it.
inline bool soundCardReopenReleasesFirst(bool liveIsSoundCard,
                                         const cascade::source::SoundCardSettings& live,
                                         const cascade::source::SoundCardSettings& want,
                                         const std::vector<cascade::source::SoundCardDevice>& list) {
    if (!liveIsSoundCard || live.device.empty()) { return false; }
    const cascade::source::SoundCardMatch m =
        cascade::source::matchSoundCard(list, want.device, want.hostApi, want.pickedFromList);
    if (m.at >= 0) {
        const cascade::source::SoundCardDevice& d = list[static_cast<std::size_t>(m.at)];
        return d.name == live.device && d.hostApi == live.hostApi;
    }
    if (!m.candidates.empty()) { return false; }
    return want.device == live.device && want.hostApi == live.hostApi;
}

// A RE-OPEN THAT CHANGES NOTHING - the dead card's row picked again, or Open
// pressed with the settings it is running with. The card is still released
// first, but there is nothing different to fall back to: it is tried once,
// and a refusal is said as "did not open", not as new settings refused and
// the old ones refused too (the third review's item 4). Only what the open
// would do differently counts: the channel in real mode, the swap and the
// centre in I/Q.
inline bool soundCardSameSettings(const cascade::source::SoundCardSettings& a,
                                  const cascade::source::SoundCardSettings& b) {
    if (!cascade::source::sameSoundCard(a.device, a.hostApi, b.device, b.hostApi)) { return false; }
    if (a.cardRateHz != b.cardRateHz || a.format != b.format) { return false; }
    if (a.format == cascade::source::SoundCardFormat::RealMono) { return a.channel == b.channel; }
    return a.swapIq == b.swapIq && a.iqCentreHz == b.iqCentreHz;
}

// WHAT A CARD RECEIVES ON THE AIR - the "Receives X to Y." line - with these
// settings and the converter stored for the card: the card's span (0 .. rate/2
// in real mode, the centre +/- rate/2 in I/Q) through what the card can have in
// front of it (soundCardConverter: nothing for I/Q, a down-converter for
// real), by the same conversion the frequency counter reads (airFromRadio).
// An inverting converter swaps the edges; nothing is below 0 Hz.
struct SoundCardAirSpan {
    double loHz = 0.0;
    double hiHz = 0.0;
};
inline SoundCardAirSpan soundCardAirSpan(const cascade::source::SoundCardSettings& s,
                                         const cascade::core::ConverterSetting& stored) {
    const cascade::core::ConverterSetting eff = soundCardConverter(stored, s.format).effective;
    const double rate = cascade::source::soundCardIqRateHz(s);
    const double centre = cascade::source::soundCardCentreHz(s);
    const double a = cascade::core::airFromRadio(eff, std::max(0.0, centre - rate / 2.0));
    const double b = cascade::core::airFromRadio(eff, centre + rate / 2.0);
    SoundCardAirSpan out;
    out.loHz = std::max(0.0, std::min(a, b));
    out.hiHz = std::max(a, b);
    return out;
}

// A DEAD CARD ON LINUX IS NOT OPENED AGAIN IN THE SAME SESSION (the third
// review's item 6). PortAudio's list of inputs is fixed when FoxSDR starts,
// and an ALSA entry opens by its card number: pull the card, plug a DIFFERENT
// one in, and ALSA gives the new card the old number - so "OldCard (hw:N,M)"
// would open the new card under the old card's name. The application already
// says to restart FoxSDR after plugging or unplugging a card; a re-pick or an
// Open of the dead card says it again instead of opening whatever is at that
// index now. A WASAPI entry keeps its identity (Windows names the endpoint,
// not a slot), so on Windows a dead card is reopened as before.
inline bool soundCardDeadReopenNeedsRestart(bool installedCardDead,
                                            const cascade::source::SoundCardSettings& live,
                                            const cascade::source::SoundCardSettings& want,
                                            const std::vector<cascade::source::SoundCardDevice>& list) {
    return installedCardDead && live.hostApi == cascade::source::kAlsaHostApi &&
           soundCardReopenReleasesFirst(true, live, want, list);
}

// WHICH CARD THE CONFIG NAMES (the third review's item 1): the card that RAN,
// never the Source section's edits that were never Opened - the next launch
// opens what is saved, so saving the edits would start it on a card, a rate
// or a format that never ran. In order: the card running now; the card the
// patch page has borrowed (`lent`, with the generator standing in); the card
// a failed restore, a release for a re-Open or a patch hand-back left
// remembered (`remembered`); and only when no card is running, lent or
// remembered, the section's settings - the card as last set up.
inline const cascade::source::SoundCardSettings& soundCardToSave(
    const std::string& liveKind, const cascade::source::SoundCardSettings& live, bool lent,
    const cascade::source::SoundCardSettings& lentCard, bool remembered,
    const cascade::source::SoundCardSettings& rememberedCard,
    const cascade::source::SoundCardSettings& section) {
    if (liveKind == "soundcard") { return live; }
    if (liveKind == "siggen" && lent) { return lentCard; }
    if (liveKind == "siggen" && remembered) { return rememberedCard; }
    return section;
}

}  // namespace cascade::gui
