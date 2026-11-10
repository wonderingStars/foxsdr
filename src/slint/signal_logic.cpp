// signal_logic.cpp - the SIGNAL bank's pure logic (signal_logic.hpp): the ImGui window's rules and sentences over the table's lists, facts and state,
// with no window and no engine. Every sentence is the drawer's own, through cascade::i18n::tr(); a format is composed with formatText/formatUtf8 into a
// std::string, never into a fixed buffer (tests/test_i18n.cpp refuses a translated format into a sizeof'd one).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "signal_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/device_scan_plan.hpp"
#include "core/engine_vocab.hpp"
#include "core/freq_converter.hpp"
#include "core/i18n.hpp"
#include "core/iq_file_box.hpp"
#include "core/ppm_correction.hpp"
#include "core/utf8_text.hpp"
#include "gui/section_reads.hpp"
#include "gui/tuning_reads.hpp"

namespace foxsdr::slintsignal {

using cascade::i18n::tr;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

namespace {

constexpr double kDefaultConverterLoHz = 125.0e6;   // app_window_converter.cpp kDefaultLoHz
constexpr double kSoapyGainMinDb = 0.0;             // app_window.cpp: the fallback span of a driver that reports no range
constexpr double kSoapyGainMaxDb = 60.0;
constexpr double kNotchMinHz = 10.0, kNotchMaxHz = 20000.0;   // the Notch Freq slider's span (logarithmic)
constexpr double kNotchQMin = 1.0, kNotchQMax = 200.0;
constexpr double kPlainCardRateHz = 48000.0;        // SoundCardSettings::cardRateHz' default: what a one-field command carries in the fields it does not name

void entry(FoxListItem& it) {
    it = FoxListItem{};
    it.structSize = sizeof(it);
}

std::string factOf(TableReader& t, const std::string& key) {
    std::string s;
    if (!t.fact(key, s)) { s.clear(); }
    return s;
}

std::int32_t listSize(TableReader& t, std::uint32_t list) { return std::max<std::int32_t>(0, t.readList(list, 0, nullptr)); }

// Every entry of a list, in order; `got <= i` is the end of a list that changed under the read (what was read stands).
template <typename F>
void eachItem(TableReader& t, std::uint32_t list, F&& f) {
    const std::int32_t n = listSize(t, list);
    for (std::int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        entry(it);
        if (t.readList(list, static_cast<std::uint32_t>(i), &it) <= i) { break; }
        f(i, it);
    }
}

std::string hzId(double hz) { return cc::formatText("%.17g", hz); }

bool parseNumber(const std::string& text, double& out) {
    const char* b = text.c_str();
    char* end = nullptr;
    const double v = std::strtod(b, &end);
    if (end == b || !std::isfinite(v)) { return false; }
    out = v;
    return true;
}

std::string joinLines(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (s.empty()) { continue; }
        if (!out.empty()) { out += '\n'; }
        out += s;
    }
    return out;
}

Command make(const cmd::QueuedCommand& q) {
    Command c;
    c.q = q;
    c.grant = grantFor(q.c.op);
    c.valid = true;
    return c;
}
Command make(const FoxCommand& f) {
    cmd::QueuedCommand q;
    q.c = f;
    return make(q);
}

}  // namespace

// =================================================================================================================================
// the small pure rules

std::string sourceChipText(const std::string& deviceName) {
    if (deviceName == "Signal generator") { return tr("Signal gen"); }
    // Through tr() as before: "IQ file" is ours too, and a device name is in no catalogue and comes back unchanged.
    std::string chip = tr(deviceName.c_str());
    const std::size_t colon = chip.rfind(": ");
    if (colon != std::string::npos) { chip = chip.substr(colon + 2); }
    const char* end = chip.c_str();
    for (int i = 0; i < 10 && *end != '\0'; ++i) { end = cc::utf8Next(end); }
    chip.resize(static_cast<std::size_t>(end - chip.c_str()));
    return chip;
}

std::string sourceChip(const std::string& deviceName, const std::vector<std::string>& patchHeld) {
    if (!patchHeld.empty()) { return tr("ON PATCH"); }
    return sourceChipText(deviceName);
}

std::string sourceRowText(const std::string& receiverSource, const std::vector<std::string>& patchRadios) {
    if (patchRadios.empty()) { return receiverSource; }
    std::string names;
    for (const std::string& n : patchRadios) { names += (names.empty() ? "" : ", ") + n; }
    return cc::formatText(tr("%s - the patch has %s"), receiverSource.c_str(), names.c_str());
}

bool radioNotOpenLamp(const std::string& keepKind, bool deviceOpen, bool generatorInUse, bool cardOpening) {
    return !keepKind.empty() && keepKind != "file" && !deviceOpen && generatorInUse && !cardOpening;
}

bool noRadioHardwareFound(bool scanned, bool partial, bool busy, std::size_t soapyRows, std::size_t nativeRows) {
    return scanned && !partial && !busy && soapyRows == 0 && nativeRows == 0;
}

int sourceVariant(int row, bool plutoRow, bool rtltcpRow, bool deviceOpen, bool airspy, bool haveOptions, bool converterInForce) {
    if (row == 1) { return kVariantIqFile; }
    if (row == cascade::core::kSoundCardRow) { return kVariantSoundCard; }
    if (row >= cascade::core::kNativeRowBase && plutoRow) { return kVariantPluto; }
    if (row >= cascade::core::kNativeRowBase && rtltcpRow) { return kVariantRtlTcp; }
    if (deviceOpen && airspy) { return kVariantAirspy; }
    if (deviceOpen && haveOptions) { return kVariantOptions; }
    if (converterInForce) { return kVariantConverter; }
    return kVariantNone;
}

float gainNorm(double lo, double hi, double value) {
    if (!(hi > lo)) {
        lo = kSoapyGainMinDb;
        hi = kSoapyGainMaxDb;
    }
    return static_cast<float>(std::clamp((value - lo) / (hi - lo), 0.0, 1.0));
}

double gainFromNorm(double lo, double hi, float norm, bool steps) {
    if (!(hi > lo)) {
        lo = kSoapyGainMinDb;
        hi = kSoapyGainMaxDb;
    }
    const double v = lo + static_cast<double>(std::clamp(norm, 0.0f, 1.0f)) * (hi - lo);
    return steps ? std::round(v) : v;
}

std::string gainText(double value, bool steps) { return steps ? cc::formatText("%.0f", value) : cc::formatText("%.1f dB", value); }

// The buttons are in the reference application's order, Sensitive, Linear, Free; the engine's code is AirspySource::GainMode { Linearity, Sensitivity, Free }.
int airspyGainModeCode(int buttonIndex) {
    switch (buttonIndex) {
        case 0: return 1;   // Sensitivity
        case 1: return 0;   // Linearity
        default: return 2;  // Free
    }
}
int airspyGainModeIndex(int code) {
    switch (code) {
        case 1: return 0;
        case 0: return 1;
        case 2: return 2;
        default: return 0;
    }
}
std::string airspyModeWord(int buttonIndex) {
    switch (buttonIndex) {
        case 0: return tr("Sensitive");
        case 1: return tr("Linear");
        default: return tr("Free");
    }
}
std::string airspyModeTooltip(int buttonIndex) {
    switch (buttonIndex) {
        case 0:
            return tr("One gain slider over Airspy's sensitivity table: the most gain early in the "
                      "chain, for weak signals.");
        case 1:
            return tr("One gain slider over Airspy's linearity table: gain held back early in the chain, "
                      "for when strong signals are nearby.");
        default:
            return tr("Set the LNA, mixer and VGA yourself, with the LNA's and the mixer's own "
                      "automatic gain control each switched on or off.");
    }
}
std::string airspyDecimationLabel(unsigned factor) { return factor == 1 ? std::string(tr("None")) : std::to_string(factor); }
std::string airspyHardwareLine(double hardwareRateHz, unsigned decimation) {
    if (decimation <= 1) { return {}; }
    return cc::formatText(tr("radio at %.4g MS/s, decimated by %u"), hardwareRateHz / 1.0e6, decimation);
}

// "192 kHz", "44.1 kHz", "7.0500 MHz": the Source section's frequency words (core/receiver_tables.hpp soundCardHzText).
std::string soundCardHzText(double hz) {
    if (std::fabs(hz) >= 1.0e6) { return cc::formatText("%.4f MHz", hz / 1.0e6); }
    if (std::fabs(std::fmod(hz, 1000.0)) < 0.5) { return cc::formatText("%.0f kHz", hz / 1000.0); }
    return cc::formatText("%.1f kHz", hz / 1000.0);
}
std::string soundCardRateLabel(double hz, bool exclusive) {
    const std::string text = soundCardHzText(hz);
    return exclusive ? cc::formatText(tr("%s (exclusive)"), text.c_str()) : text;
}
std::string soundCardReceivesLine(double loHz, double hiHz) {
    const std::string lo = soundCardHzText(loHz);
    const std::string hi = soundCardHzText(hiHz);
    return cc::formatText(tr("Receives %s to %s."), lo.c_str(), hi.c_str());
}

std::string converterAliasNote(const std::string& state) {
    if (state == "notCarried") {
        return tr("Opened through SoapySDR because the native driver refused this radio. The "
                  "converter set for it was not carried over: with no serial number the two "
                  "drivers cannot be shown to be the same dongle. Set one here if this radio "
                  "needs it.");
    }
    if (state == "carried") {
        return tr("Opened through SoapySDR because the native driver refused this radio - the "
                  "converter set for it still applies.");
    }
    return {};
}
std::string converterUnusableNote(bool upIgnored) {
    if (!upIgnored) { return {}; }
    return tr("A sound card takes a down-converter only: the up-converter set for this card is not used.");
}
std::string converterOnAirLine(double airHz, double radioHz) {
    return cc::formatText(tr("Listening on %s on the air - %s at the radio."), cc::converterHzText(airHz).c_str(), cc::converterHzText(radioHz).c_str());
}
std::string converterLoError(const std::string& typed) { return cc::formatText(tr("could not read frequency \"%s\""), typed.c_str()); }

// WHICH ENTRY TO PICK IN ZADIG IS NOT THE SAME FOR EVERY RADIO (the drawer's argument): an RTL2832U is a composite device whose SDR half is interface 0; the RX888 has
// to go through Zadig TWICE; any other presents under its own name.
std::string zadigAdvice(const std::string& name, const std::string& family) {
    const std::string what = name.empty() ? std::string(tr("A USB radio")) : name;
    if (family == "rtl") {
        return cc::formatText(tr("%s is plugged in but is not bound to WinUSB, so nothing can open it. Run "
                                 "Zadig (zadig.akeo.ie), tick Options -> List All Devices, select \"Bulk-In, "
                                 "Interface (Interface 0)\", choose WinUSB and click Replace Driver, then "
                                 "press Refresh."),
                              what.c_str());
    }
    if (family == "rx888") {
        return cc::formatText(tr("%s is plugged in but is not bound to WinUSB, so nothing can open it. An "
                                 "RX888 has to be done TWICE, because it is two devices: run Zadig "
                                 "(zadig.akeo.ie), tick Options -> List All Devices, bind \"WestBridge\" (USB "
                                 "ID 04B4:00F3, the bootloader) to WinUSB, then open the radio here once so "
                                 "FoxSDR loads its firmware - it will disappear and come back as \"RX888mk2\" "
                                 "(04B4:00F1), which has to be bound to WinUSB as well. Press Refresh after "
                                 "each."),
                              what.c_str());
    }
    return cc::formatText(tr("%s is plugged in but is not bound to WinUSB, so nothing can open it. Run "
                             "Zadig (zadig.akeo.ie), tick Options -> List All Devices, select this radio "
                             "in the dropdown, choose WinUSB and click Replace Driver, then press "
                             "Refresh."),
                          what.c_str());
}

std::string ppmValueText(double ppm) { return cc::formatText("%+.1f ppm", cc::sanitisePpm(ppm)); }

bool parsePpm(const std::string& text, double& ppm) {
    const char* b = text.c_str();
    char* end = nullptr;
    const double v = std::strtod(b, &end);
    if (end == b || !std::isfinite(v)) { return false; }
    std::string rest(end);
    rest.erase(std::remove_if(rest.begin(), rest.end(), [](unsigned char c) { return std::isspace(c) != 0; }), rest.end());
    for (char& c : rest) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    if (!rest.empty() && rest != "ppm") { return false; }
    ppm = cc::sanitisePpm(v);
    return true;
}

std::string ppmHowText(const std::string& method, bool wholeRadio, int wholeValue) {
    if (method == "radio") {
        if (wholeRadio) { return cc::formatText(tr("Corrected in the radio itself, in whole ppm: %+d."), wholeValue); }
        return tr("Corrected in the radio itself.");
    }
    return tr("Corrected by retuning - the centre frequency only; the sample rate keeps "
              "its error.");
}

std::string ppmChip(bool on, const std::string& card) {
    if (!on) { return tr("OFF"); }
    if (card.empty()) { return tr("ON"); }
    return cc::formatText(tr("PPM %+.1f"), std::atof(card.c_str()));
}

std::string autoNotchStatus(bool enabled, bool engaged, double hz) {
    if (!enabled) { return {}; }
    if (engaged) { return cc::formatText(tr("on %.0f Hz"), hz); }
    return tr("searching");
}

float notchNormFromHz(double hz) {
    hz = std::clamp(hz, kNotchMinHz, kNotchMaxHz);
    return static_cast<float>(std::log(hz / kNotchMinHz) / std::log(kNotchMaxHz / kNotchMinHz));
}
double notchHzFromNorm(float norm) {
    const double n = std::clamp(static_cast<double>(norm), 0.0, 1.0);
    return kNotchMinHz * std::pow(kNotchMaxHz / kNotchMinHz, n);
}
float notchQNorm(double q) { return static_cast<float>(std::clamp((q - kNotchQMin) / (kNotchQMax - kNotchQMin), 0.0, 1.0)); }
double notchQFromNorm(float norm) { return kNotchQMin + std::clamp(static_cast<double>(norm), 0.0, 1.0) * (kNotchQMax - kNotchQMin); }

std::string filtersChip(int filtersOn) {
    if (filtersOn <= 0) { return tr("OFF"); }
    return cc::formatText(tr("%d ON"), filtersOn);
}

std::vector<int> bufferChoices() { return {0, 120, 240, 480, 960}; }
std::string bufferLabel(int ms) { return ms == 0 ? std::string(tr("AUTOMATIC")) : cc::formatText("%d ms", ms); }
std::string bufferPreview(int bufferMs, int leadMs) {
    if (bufferMs == 0) { return cc::formatText("%s  %d ms", tr("AUTOMATIC"), leadMs); }
    return cc::formatText("%d ms", bufferMs);
}
std::string audioLeadNotice(const std::string& f) {
    if (f.empty()) { return {}; }
    const std::size_t a = f.find(' ');
    const std::size_t b = a == std::string::npos ? std::string::npos : f.find(' ', a + 1);
    if (a == std::string::npos || b == std::string::npos) { return {}; }
    const std::string kind = f.substr(0, a);
    const int ms = std::atoi(f.substr(a + 1, b - a - 1).c_str());
    const int count = std::atoi(f.substr(b + 1).c_str());
    if (kind == "raised") {
        return cc::formatText(tr("Audio buffer raised to %d ms: this computer fell behind %d times in the last minute."), ms, count);
    }
    if (kind == "restored") {
        return cc::formatText(tr("Audio buffer restored to %d ms: the depth this computer needed when it last ran."), ms);
    }
    if (kind == "lowered") { return cc::formatText(tr("Audio buffer lowered to %d ms after %d quiet minutes."), ms, count); }
    return {};
}

// The Sinks row's chip (app_window.cpp drawSinksSection): an open in flight, no device ever opened, a stream that died, a plugin's mute, else on.
SinkChip sinkChip(const FoxReceiverState& st) {
    const bool muted = st.audioMutedBy[0] != '\0';
    const bool opening = (st.audioFlags & FOXAPI_AUDIO_OPENING) != 0u;
    const bool opened = !opening && (st.audioFlags & FOXAPI_AUDIO_EVER_OPENED) != 0u;
    const bool alive = opened && (st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u;
    SinkChip c;
    c.lit = opened || opening;
    c.word = tr("ON");
    c.tone = Tone::Good;
    if (opening) {
        c.word = tr("OPENING");
        c.tone = Tone::Warn;
    } else if (!opened) {
        c.word = tr("NO DEV");
        c.tone = Tone::Neutral;
    } else if (!alive) {
        c.word = tr("DEAD");
        c.tone = Tone::Err;
    } else if (muted) {
        c.word = tr("MUTED");
        c.tone = Tone::Warn;
    }
    return c;
}

std::string recorderLabel(Take take, int start, bool recording) {
    if (take == Take::Iq) {
        if (start == 2) { return tr("Cancelling IQ start..."); }
        if (start != 0) { return tr("Starting IQ - click to cancel"); }
        return recording ? std::string(tr("Stop IQ")) : std::string(tr("Record IQ"));
    }
    if (start == 2) { return tr("Cancelling audio start..."); }
    if (start != 0) { return tr("Starting audio - click to cancel"); }
    return recording ? std::string(tr("Stop audio")) : std::string(tr("Record audio"));
}

std::string takeProgress(Take take, double seconds, double bytes) {
    const unsigned long long samples = static_cast<unsigned long long>(cascade::gui::takeSamples(bytes, take == Take::Iq ? cascade::gui::kIqFrameBytes : cascade::gui::kAudioFrameBytes));
    if (take == Take::Iq) { return cc::formatText(tr("IQ %.1f s | %llu samples | %.1f MB"), seconds, samples, bytes / 1.0e6); }
    return cc::formatText(tr("Audio %.1f s | %llu samples | %.1f MB"), seconds, samples, bytes / 1.0e6);
}

std::string diskWaitLine(double waitedS) {
    if (waitedS < 1.0) { return {}; }
    return cc::formatText(tr("Waiting for the disk to open the file: %.0f s"), waitedS);
}

int recorderStartOf(const std::string& startFact, Take take) {
    const std::size_t at = take == Take::Iq ? 0u : 1u;
    if (startFact.size() <= at) { return 0; }
    const int v = startFact[at] - '0';
    return v >= 0 && v <= 2 ? v : 0;
}

TxChip transmitChip(bool haveRadio, bool faulted, bool onAir) {
    TxChip c;
    if (faulted) {
        c.word = tr("FAULT");
        c.tone = Tone::Err;
    } else if (!haveRadio) {
        c.word = tr("NO RADIO");
        c.tone = Tone::Neutral;
    } else if (onAir) {
        c.word = tr("ON AIR");
        c.tone = Tone::Err;
    } else {
        c.word = tr("READY");
        c.tone = Tone::Warn;
    }
    return c;
}

// =================================================================================================================================
// the models

namespace {

struct DeviceRow {
    std::string name, caption;
    std::uint64_t id = 0;
    bool soapy = false, pluto = false, rtltcp = false;
};

struct Evidence { int kind = 0; std::string name, detail; };

std::vector<Evidence> readEvidence(TableReader& t) {
    std::vector<Evidence> out;
    eachItem(t, FOXAPI_LIST_SCAN_EVIDENCE, [&](int, const FoxListItem& it) { out.push_back({static_cast<int>(it.value[0]), it.name, it.detail}); });
    return out;
}

std::vector<UnboundRow> readUnbound(TableReader& t) {
    std::vector<UnboundRow> out;
    eachItem(t, FOXAPI_LIST_UNBOUND_RADIOS, [&](int, const FoxListItem& it) {
        UnboundRow r;
        r.name = it.name[0] != '\0' ? std::string(it.name) : std::string(tr("A USB radio"));
        r.family = it.detail;
        r.vid = cc::formatText("%04X", static_cast<unsigned>(it.value[0]));
        r.pid = cc::formatText("%04X", static_cast<unsigned>(it.value[1]));
        r.advice = zadigAdvice(it.name, it.detail);
        out.push_back(std::move(r));
    });
    return out;
}

struct Take_ { bool found = false, active = false, stalled = false, full = false; double bytes = 0.0, seconds = 0.0; };

Take_ readTakeOf(TableReader& t, int kind) {
    Take_ out;
    eachItem(t, FOXAPI_LIST_RECORDINGS, [&](int, const FoxListItem& it) {
        if (out.found || static_cast<int>(it.value[0]) != kind) { return; }
        out.found = true;
        out.active = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
        out.stalled = (it.flags & FOXAPI_ITEM_STALLED) != 0u;
        out.full = (it.flags & FOXAPI_ITEM_FULL) != 0u;
        out.bytes = it.value[1];
        out.seconds = it.value[3];
    });
    return out;
}

}  // namespace

SourceModel readSource(TableReader& t, const FoxReceiverState& st, const ReadOptions& o) {
    SourceModel m;
    const std::uint32_t flags = st.flags;
    const std::uint32_t src = st.sourceFlags;
    const bool deviceOpen = (flags & FOXAPI_RX_DEVICE_OPEN) != 0u;
    const bool deviceOpening = (flags & FOXAPI_RX_DEVICE_OPENING) != 0u;
    const bool faulted = (flags & FOXAPI_RX_FAULTED) != 0u;
    const bool patchRunning = (flags & FOXAPI_RX_PATCH_RUNNING) != 0u;
    const bool soapyBusy = (src & FOXAPI_SRC_SCAN_PENDING) != 0u || deviceOpening;
    const int nativeBase = cascade::core::kNativeRowBase;
    const int cardRow = cascade::core::kSoundCardRow;

    // ---- the combo's rows (DEVICES: an entry's index is its row) and the facts the section's sentences are made of ----
    std::vector<DeviceRow> rows;
    bool generatorActive = false;
    std::size_t soapyRows = 0, nativeRows = 0;
    eachItem(t, FOXAPI_LIST_DEVICES, [&](int i, const FoxListItem& it) {
        DeviceRow r;
        r.name = it.name;
        r.id = it.id;
        r.soapy = std::strncmp(it.detail, "soapy:", 6) == 0;
        r.pluto = std::strncmp(it.detail, "pluto:", 6) == 0;
        r.rtltcp = std::strncmp(it.detail, "rtltcp:", 7) == 0;
        if (i == 0 && (it.flags & FOXAPI_ITEM_ACTIVE) != 0u) { generatorActive = true; }
        if (r.soapy) { ++soapyRows; }
        if (i >= nativeBase && !r.soapy && !r.pluto && !r.rtltcp) { ++nativeRows; }
        // THE CAPTION THE COMBO LETTERS: a native radio is "<name> (native)"; the Pluto and rtl_tcp rows are translated; a Soapy row is its own label.
        if (i == 0) {
            r.caption = tr("Signal generator");
        } else if (i == 1) {
            r.caption = tr("IQ file");
        } else if (i == cardRow) {
            r.caption = tr("Sound card");
        } else if (r.soapy) {
            r.caption = r.name;
        } else if (r.pluto || r.rtltcp) {
            r.caption = tr(r.name.c_str());
        } else {
            r.caption = cc::formatText(tr("%s (native)"), r.name.c_str());
        }
        rows.push_back(std::move(r));
    });
    const std::string rowFact = factOf(t, "fact.source.row");
    const int row = rowFact.empty() ? -1 : static_cast<int>(std::strtod(rowFact.c_str(), nullptr));
    const std::string keepKind = factOf(t, "fact.source.keep");
    const std::string opening = factOf(t, "fact.source.opening");
    const std::string scanGate = factOf(t, "fact.source.scanGate");
    const std::string scanSkip = factOf(t, "fact.source.scanSkip");
    const std::string faultedDrivers = factOf(t, "fact.source.faultedDrivers");
    const bool uhdNoImages = factOf(t, "fact.source.uhdNoImages") == "1";
    const bool usrpOnUsb = factOf(t, "fact.source.usrpOnUsb") == "1";
    const std::string restoreKeepLabel = factOf(t, "fact.status.restoreKeepLabel");

    const bool keepPreview = !keepKind.empty() && !deviceOpen && generatorActive && !restoreKeepLabel.empty() && (src & FOXAPI_SRC_CARD_OPENING) == 0u;
    const auto rowLabel = [&](int idx) -> std::string {
        if (idx == 0) { return tr("Signal generator"); }
        if (idx == 1) { return tr("IQ file"); }
        if (idx == cardRow) { return tr("Sound card"); }
        if (idx >= nativeBase && idx < static_cast<int>(rows.size())) { return rows[static_cast<std::size_t>(idx)].caption; }
        return tr(st.deviceName);   // -1: the active device dropped by a Refresh falls back to the live source name
    };

    m.device_index = row;
    m.devices.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        SelectItem s;
        s.label = rows[i].caption;
        s.id = std::to_string(rows[i].id);
        // THE PATCH HAS THE RADIOS while it runs: a device row would open a radio a patch node may already hold. Shown, greyed, and saying why.
        s.enabled = !(patchRunning && static_cast<int>(i) >= nativeBase);
        if (!s.enabled) {
            s.tooltip = tr("The patch is running and has the radios - press STOP or ALL OFF\n"
                           "on the patch page to use one here.");
        }
        m.devices.push_back(std::move(s));
    }
    if (keepPreview) {
        m.device_label = cc::formatText(tr("%s (saved, not open)"), restoreKeepLabel.c_str());
    } else if (o.patch_held.empty()) {
        m.device_label = rowLabel(row);
    } else {
        m.device_label = sourceRowText(rowLabel(row), o.patch_held);
    }
    m.refresh_enabled = !soapyBusy;
    m.look_for_usrps = (src & FOXAPI_SRC_NETWORK_USRPS) != 0u;
    m.setup_key_label = tr("RADIO SETUP");
    m.device_open = deviceOpen;
    m.opening = deviceOpening;
    m.usrps_tooltip = tr("Ask UHD for USRPs on the network (N200, X310, N310 and the "
                         "like) as well as on USB. A USB USRP is always looked for "
                         "when one is plugged in.");
    m.ppm_tooltip = tr("Corrects the error of this radio's crystal, in parts per million. Positive "
                       "when stations show up below their real frequency. Tune to a signal whose "
                       "frequency you know exactly and change the value until it sits where it "
                       "should. The switch is for every radio; the value is remembered for each "
                       "radio.");
    if (o.sdrplay_key_shown) {
        m.sdrplay_key_shown = true;
        m.sdrplay_key_enabled = !o.sdrplay_restarting;
        m.sdrplay_key_label = tr("RESTART SDRPLAY SERVICE");
        m.sdrplay_line = o.sdrplay_restarting ? std::string(tr("Restarting the SDRplay API Service - answer the Windows prompt.")) : o.sdrplay_note;
    }

    // ---- the row's chip and lamp ----
    m.chip = sourceChip(st.deviceName, o.patch_held);
    const bool radioNotOpen = radioNotOpenLamp(keepKind, deviceOpen, generatorActive, (src & FOXAPI_SRC_CARD_OPENING) != 0u);
    m.chip_tone = faulted ? Tone::Err : (deviceOpen ? Tone::Good : Tone::Neutral);
    m.lamp_lit = faulted || radioNotOpen || (flags & FOXAPI_RX_RUNNING) != 0u;
    m.lamp_tone = (faulted || radioNotOpen) ? Tone::Err : Tone::Good;

    // ---- the open radio's panel: rate, antenna, gain, bias tee, the radio's own switches ----
    std::vector<FoxListItem> gainItems;
    bool airspy = false;
    if (deviceOpen) {
        int rateIndex = -1;
        double bestDelta = 0.0;
        std::vector<double> rateHz;
        eachItem(t, FOXAPI_LIST_SAMPLE_RATES, [&](int, const FoxListItem& it) {
            SelectItem s;
            s.label = it.detail[0] != '\0' ? std::string(it.detail) : cc::formatText("%.3f MS/s", it.value[0] / 1.0e6);
            s.id = hzId(it.value[0]);
            const double delta = std::fabs(it.value[0] - st.sampleRateHz);
            if (rateIndex < 0 || delta < bestDelta) {
                rateIndex = static_cast<int>(m.rates.size());
                bestDelta = delta;
            }
            m.rates.push_back(std::move(s));
        });
        // The row that is current is the one NEAREST the state's rate: a radio that coerces a rate is on the row it landed on.
        if (rateIndex >= 0) { m.rate_text = m.rates[static_cast<std::size_t>(rateIndex)].label; }
        m.actual_rate = cc::formatText(tr("actual %.4g MS/s"), st.sampleRateHz / 1.0e6);

        std::string antenna;
        eachItem(t, FOXAPI_LIST_ANTENNAS, [&](int, const FoxListItem& it) {
            SelectItem s;
            s.label = it.name;
            s.id = it.name;
            m.antennas.push_back(std::move(s));
            if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) { antenna = it.name; }
        });
        if (m.antennas.size() > 1) {
            m.antenna_text = antenna;
        } else if (!antenna.empty()) {
            m.single_port = true;
            m.antenna_text = cc::formatText(tr("Antenna: %s"), antenna.c_str());
        }

        eachItem(t, FOXAPI_LIST_GAIN_MODES, [&](int, const FoxListItem&) { airspy = true; });
        eachItem(t, FOXAPI_LIST_GAINS, [&](int, const FoxListItem& it) { gainItems.push_back(it); });

        const bool agcOn = (flags & FOXAPI_RX_DEVICE_AGC) != 0u;
        if (!airspy) {
            if ((flags & FOXAPI_RX_AGC_SUPPORTED) != 0u) {
                m.auto_gain_shown = true;
                m.auto_gain = agcOn;
            } else {
                m.agc_note = tr("Auto gain: not supported");
            }
            // EACH SLIDER SPANS WHAT ITS OWN STAGE WILL ACCEPT (the driver's range; 0..60 dB for one that reports none), lettered in its own unit.
            for (const FoxListItem& it : gainItems) {
                GainStage g;
                g.name = it.name;
                g.unit = std::strcmp(it.detail, "step") == 0 ? "step" : "dB";
                const bool steps = g.unit == "step";
                g.lo = it.value[1] > it.value[0] ? it.value[0] : kSoapyGainMinDb;
                g.hi = it.value[1] > it.value[0] ? it.value[1] : kSoapyGainMaxDb;
                g.value = it.value[3];
                g.norm = gainNorm(g.lo, g.hi, g.value);
                g.text = gainText(g.value, steps);
                g.enabled = !agcOn;   // greyed while hardware AGC drives the stages
                m.stages.push_back(std::move(g));
            }
        }
        if ((flags & FOXAPI_RX_BIAS_TEE_PRESENT) != 0u) {
            m.bias_shown = true;
            m.bias_on = (flags & FOXAPI_RX_BIAS_TEE_ON) != 0u;
            m.bias_enabled = !deviceOpening;
            m.bias_tooltip = tr("Sends about 4.5 V up the antenna cable to power an amplifier at the "
                                "mast. Leave it off unless you have one: equipment that is not "
                                "expecting power on the connector can be damaged by it.");
        }

        // THE RADIO'S OWN SWITCHES, each drawn only when the OPEN model has it (the RX888's pair together: either one shows both).
        bool rfNotch = false, rfNotchOn = false, dab = false, dabOn = false, hdr = false, hdrOn = false, adc = false, dither = false, randomiser = false;
        eachItem(t, FOXAPI_LIST_DEVICE_OPTIONS, [&](int, const FoxListItem& it) {
            const bool on = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
            if (std::strcmp(it.name, "rf_notch") == 0) { rfNotch = true; rfNotchOn = on; }
            else if (std::strcmp(it.name, "dab_notch") == 0) { dab = true; dabOn = on; }
            else if (std::strcmp(it.name, "hdr") == 0) { hdr = true; hdrOn = on; }
            else if (std::strcmp(it.name, "dither") == 0) { adc = true; dither = on; }
            else if (std::strcmp(it.name, "randomiser") == 0) { adc = true; randomiser = on; }
        });
        const auto option = [&](const char* name, const std::string& label, bool on, const std::string& tip) {
            OptionRow r;
            r.name = name;
            r.label = label;
            r.on = on;
            r.enabled = true;
            r.tooltip = tip;
            m.options.push_back(std::move(r));
        };
        if (rfNotch) {
            option("rf_notch", tr("FM notch"), rfNotchOn,
                   tr("The RSP's own broadcast-FM notch filter. Worth switching on when a "
                      "strong local FM transmitter is desensitising everything else."));
        }
        if (dab) { option("dab_notch", tr("DAB notch"), dabOn, tr("The RSP's own DAB band III notch filter.")); }
        if (hdr) {
            option("hdr", tr("HDR mode"), hdrOn,
                   tr("An RSPdx's high-dynamic-range path below 2 MHz, for medium wave and "
                      "below where the strong stations are the problem."));
        }
        if (adc) {
            option("dither", tr("ADC dither"), dither,
                   tr("Adds a small noise signal inside the converter so its spurious tones "
                      "stop landing on exact frequencies. Costs a little noise floor."));
            option("randomiser", tr("ADC randomiser"), randomiser,
                   tr("Scrambles the converter's output to keep the digital side from "
                      "coupling back into the analogue one. FoxSDR unscrambles it on this "
                      "end in the same step, so it is one switch and not two."));
        }
    }

    // ---- the Airspy's own panel ----
    if (deviceOpen && airspy) {
        int activeCode = 1;
        eachItem(t, FOXAPI_LIST_GAIN_MODES, [&](int, const FoxListItem& it) {
            if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) { activeCode = static_cast<int>(it.value[0]); }
        });
        m.airspy_gain_mode = airspyGainModeIndex(activeCode);
        unsigned current = 1;
        double hardwareRate = 0.0;
        eachItem(t, FOXAPI_LIST_DECIMATIONS, [&](int, const FoxListItem& it) {
            const unsigned factor = static_cast<unsigned>(it.value[0]);
            SelectItem s;
            s.label = airspyDecimationLabel(factor);
            s.id = std::to_string(factor);
            if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) {
                current = factor;
                hardwareRate = it.value[1] * it.value[0];
                m.airspy_decimation_index = static_cast<int>(m.airspy_decimations.size());
            }
            m.airspy_decimations.push_back(std::move(s));
        });
        m.airspy_hardware_rate = airspyHardwareLine(hardwareRate, current);
        for (const FoxListItem& it : gainItems) {
            GainStage g;
            g.name = it.name;
            g.unit = "step";
            g.lo = 0.0;
            g.hi = it.value[1];
            g.value = it.value[3];
            g.norm = gainNorm(0.0, g.hi > 0.0 ? g.hi : 1.0, g.value);
            g.text = gainText(g.value, true);
            g.enabled = (it.flags & FOXAPI_ITEM_AGC) == 0u;   // a stage its AGC is driving is greyed, not hidden
            if (g.name == "LNA") { m.airspy_lna_agc = !g.enabled; m.airspy_lna = g.norm; }
            if (g.name == "MIXER") { m.airspy_mixer_agc = !g.enabled; m.airspy_mixer = g.norm; }
            if (g.name == "VGA") { m.airspy_vga = g.norm; }
            m.airspy_stages.push_back(std::move(g));
        }
        if (!m.airspy_stages.empty()) { m.airspy_gain_norm = m.airspy_stages[0].norm; }   // Sensitive and Linear: the one slider over stage 0
    }

    // ---- the converter ----
    {
        const std::string modesFact = factOf(t, "fact.converter.modes");
        std::vector<int> offered;
        for (const char* p = modesFact.c_str(); *p != '\0';) {
            char* end = nullptr;
            const long code = std::strtol(p, &end, 10);
            if (end == p) { break; }
            if (code >= 0 && code <= 2) { offered.push_back(static_cast<int>(code)); }
            p = (*end == ',') ? end + 1 : end;
        }
        cc::ConverterSetting stored;
        eachItem(t, FOXAPI_LIST_CONVERTERS, [&](int, const FoxListItem& it) {
            if ((it.flags & FOXAPI_ITEM_ACTIVE) == 0u || stored.loHz != 0.0 || stored.mode != cc::ConverterMode::Off) { return; }
            if (it.value[0] >= 0.0 && it.value[0] <= 2.0) { stored.mode = static_cast<cc::ConverterMode>(static_cast<int>(it.value[0])); }
            stored.loHz = it.value[1];
            stored.inverted = it.value[2] != 0.0;
        });
        cc::ConverterSetting live;
        {
            int mode = 0, inverted = 0;
            double lo = 0.0;
            if (std::sscanf(factOf(t, "fact.converter.live").c_str(), "%d %lf %d", &mode, &lo, &inverted) == 3 && mode >= 0 && mode <= 2) {
                live.mode = static_cast<cc::ConverterMode>(mode);
                live.loHz = lo;
                live.inverted = inverted != 0;
            }
        }
        m.converter_shown = !offered.empty();
        if (m.converter_shown) {
            m.converter_tooltip = tr("An up- or down-converter between the antenna and this radio. FoxSDR "
                                     "then shows and tunes the frequency on the air, and tells the radio "
                                     "the converted one. Remembered for this radio only.");
            m.converter_lo_tooltip = tr("Type it in Hz, kHz or MHz - 125 MHz, 125000 kHz and "
                                        "125000000 Hz are the same. A number with no unit is MHz.");
            m.converter_inverts_tooltip = tr("Tick this when the converter's oscillator sits ABOVE the signal (a "
                                             "high-side LO): the radio is then tuned to the oscillator minus the "
                                             "frequency, and the band arrives back to front. FoxSDR turns it the "
                                             "right way round. Most up-converters, a Ham-It-Up included, do not.");
        }
        int mine = static_cast<int>(stored.mode);
        // A stored mode this source cannot take reads as Off (its LO is kept for the radio or mode that can).
        if (std::find(offered.begin(), offered.end(), mine) == offered.end()) { mine = 0; }
        m.converter_mode = mine;
        m.converter_lo = cc::converterLoValid(stored.loHz) ? cc::converterHzText(stored.loHz) : std::string();
        m.converter_lo_hz = stored.loHz;
        m.converter_inverts = stored.inverted;
        for (const int code : offered) {
            SelectItem s;
            s.label = code == 1 ? std::string(tr("Up-converter")) : code == 2 ? std::string(tr("Down-converter")) : std::string(tr("Off"));
            s.id = std::to_string(code);
            m.converter_modes.push_back(std::move(s));
        }
        m.converter_alias = joinLines({converterAliasNote(factOf(t, "fact.converter.alias")), converterUnusableNote(factOf(t, "fact.converter.upIgnored") == "1")});
        if (mine != 0) {
            if (!o.converter_lo_bad.empty()) { m.converter_error = converterLoError(o.converter_lo_bad); }
            if (cc::converterActive(live)) { m.converter_on_air = converterOnAirLine(st.tunedHz, cc::radioFromAir(live, st.tunedHz)); }
        }
        const bool inForce = cc::converterActive(live);
        bool pluto = false, rtltcp = false;
        if (row >= nativeBase && row < static_cast<int>(rows.size())) {
            pluto = rows[static_cast<std::size_t>(row)].pluto;
            rtltcp = rows[static_cast<std::size_t>(row)].rtltcp;
        }
        m.variant = sourceVariant(row, pluto, rtltcp, deviceOpen, airspy, !m.options.empty(), inForce);
    }

    // ---- frequency correction ----
    {
        const bool on = factOf(t, "fact.ppm.on") == "1";
        const std::string method = factOf(t, "fact.ppm.method");
        const double stored = std::atof(factOf(t, "fact.ppm.value").c_str());
        const std::string whole = factOf(t, "fact.ppm.whole");
        const std::string error = factOf(t, "fact.ppm.error");
        const std::string card = factOf(t, "fact.ppm.card");
        const std::string model = factOf(t, "fact.ppm.model");
        const std::string serial = factOf(t, "fact.ppm.serial");
        m.ppm_on = on;
        m.ppm_value = stored;
        m.ppm_stored = cc::ppmText(stored);
        m.ppm_chip = ppmChip(on, card);
        m.ppm_enabled = !deviceOpening;
        if (method != "na") { m.ppm_text = ppmValueText(stored); }
        if (on && method == "na") {
            m.ppm_note = tr("Not used for the signal generator, a sound card or an I/Q "
                            "file - none has a crystal of its own to correct.");
        } else if (on) {
            m.ppm_shown_value = true;
            m.ppm_radio_line = serial.empty() ? cc::formatText(tr("For %s"), model.c_str()) : cc::formatText(tr("For %s, serial %s"), model.c_str(), serial.c_str());
            m.ppm_note = ppmHowText(method, !whole.empty(), std::atoi(whole.c_str()));
            if (!error.empty()) { m.ppm_refusal = cc::formatText(tr("The radio refused the correction: %s"), error.c_str()); }
        }
    }

    // ---- the status lines under the controls ----
    m.error_line = factOf(t, "fact.status.sourceError");
    m.mismatch_note = factOf(t, "fact.status.tuneMismatchNote");
    if (faulted) {
        // THE WHOLE SENTENCE (fact.status.deviceFault), never the state's faultMessage, which is cut at 127 bytes.
        const std::string fault = factOf(t, "fact.status.deviceFault");
        m.stopped_line = cc::formatText(tr("Device stopped: %s"), fault.c_str());
        // Not under a driver that has already said what to do (the SDRplay lost-session sentences end "then restart FoxSDR").
        if (fault.find("restart FoxSDR") == std::string::npos) {
            m.stopped_line += "\n";
            m.stopped_line += tr("Reconnect it and pick the source again, or switch to the signal generator.");
        }
    }

    // ---- the advisory: the amber and the dim paragraphs, in the drawer's order ----
    {
        struct Para { std::string text; bool warn; };
        std::vector<Para> paras;
        if (soapyBusy) {
            m.busy_line = deviceOpening ? cc::formatText(tr("Opening %s..."), opening.c_str()) : std::string(tr("Scanning for devices..."));
            paras.push_back({m.busy_line, true});
        }
        if (!faultedDrivers.empty()) {
            paras.push_back({cc::formatText(tr("Not scanned again this session: %s (it crashed the device scan)"), faultedDrivers.c_str()), true});
        }
        if ((src & FOXAPI_SRC_UHD_LEFT_OUT) != 0u && !m.look_for_usrps) {
            const std::vector<std::string> left{"uhd"};
            if (const char* hint = cascade::gui::networkUsrpHint(left); hint != nullptr) { paras.push_back({tr(hint), false}); }
        }
        if (const char* hint = cascade::gui::uhdImagesMissingHint(uhdNoImages, usrpOnUsb); hint != nullptr) { paras.push_back({tr(hint), true}); }
        if ((src & FOXAPI_SRC_SCAN_GATED) != 0u) {
            // WHAT THE SCAN DOES BESIDE AN OPEN RADIO: usually it still runs, leaving out only the open radios' own drivers; it is deferred only when it cannot vouch for one.
            std::string why;
            if (!scanSkip.empty()) {
                why = cc::formatText(tr("While %s is open, Refresh looks for every other "
                                        "kind of radio but leaves out the %s driver - its probe would "
                                        "reset the open radio. Close it to look for more of that kind."),
                                     scanGate.c_str(), scanSkip.c_str());
            } else {
                why = cc::formatText(tr("Refreshed the native radios. The SoapySDR scan is deferred while "
                                        "%s is open - the vendor probe opens and resets every dongle it "
                                        "finds. Close the radio to look for other SoapySDR devices."),
                                     scanGate.c_str());
            }
            paras.push_back({why, false});
        }
        for (const UnboundRow& u : readUnbound(t)) { paras.push_back({u.advice, true}); }
        // AN SDRPLAY RSP IS INVISIBLE WITHOUT THE VENDOR API, and silence is the wrong answer: shown when there is no RSP row and the driver has something to say.
        const std::string sdrAdvice = factOf(t, "fact.status.sdrPlayAdvice");
        if ((src & FOXAPI_SRC_SDRPLAY_ROWS) == 0u && !sdrAdvice.empty()) {
            paras.push_back({sdrAdvice, true});
            const std::string detail = factOf(t, "fact.status.sdrPlayApiDetail");
            if (!detail.empty()) { paras.push_back({cc::formatText(tr("SDRplay API: %s"), detail.c_str()), false}); }
        }
        if (noRadioHardwareFound((src & FOXAPI_SRC_SCANNED) != 0u, (src & FOXAPI_SRC_SCAN_PARTIAL) != 0u, soapyBusy, soapyRows, nativeRows)) {
            int modules = 0;
            for (const Evidence& e : readEvidence(t)) { modules += e.kind == 3 ? 1 : 0; }
            std::vector<std::string> block;
            block.push_back(tr("No radio hardware found"));
            block.push_back(tr("FoxSDR reaches radios through SoapySDR vendor modules, which are a separate "
                               "install. The signal generator and IQ file playback need no hardware."));
            if (modules == 0) {
                block.push_back(tr("No vendor modules are loaded. Install PothosSDR "
                                   "(downloads.myriadrf.org/builds/PothosSDR) or radioconda "
                                   "(github.com/ryanvolz/radioconda), then press Refresh. Neither needs FoxSDR "
                                   "to be reinstalled."));
            } else {
                block.push_back(modules == 1 ? cc::formatText(tr("%d vendor module is loaded, but none reported a device."), modules)
                                             : cc::formatText(tr("%d vendor modules are loaded, but none reported a device."), modules));
            }
            block.push_back(tr("RTL-SDR dongle? Windows also needs a WinUSB driver bound to it. Run Zadig "
                               "(zadig.akeo.ie), tick Options -> List All Devices, select \"Bulk-In, Interface "
                               "(Interface 0)\", choose WinUSB and click Replace Driver. Until that is done the "
                               "dongle is invisible to every SDR application. In Device Manager an unconfigured "
                               "dongle shows as \"Bulk-In, Interface\" with a yellow warning."));
            paras.push_back({joinLines(block), true});
        }
        if (keepPreview) {
            const bool keepFile = keepKind == "file";
            const bool keepCard = keepKind == "soundcard";
            paras.push_back({cc::formatText(keepFile ? tr("%s is still the saved I/Q file. The signal generator is running in its "
                                                          "place for this session only - FoxSDR will try the file again next time it "
                                                          "starts. Choosing another source here replaces it.")
                                            : keepCard ? tr("%s is still the saved sound card. The signal generator is running in its "
                                                            "place for this session only - FoxSDR will try the card again next time it "
                                                            "starts. Choosing another source here replaces it.")
                                                       : tr("%s is still the saved radio. The signal generator is running in its place "
                                                            "for this session only - FoxSDR will try the radio again next time it "
                                                            "starts. Choosing another source here replaces it."),
                                            restoreKeepLabel.c_str()),
                             true});
        }
        std::vector<std::string> texts;
        bool anyWarn = false;
        for (const Para& p : paras) {
            texts.push_back(p.text);
            anyWarn = anyWarn || (p.warn && !p.text.empty());
        }
        m.advisory = joinLines(texts);
        m.advisory_tone = anyWarn ? Tone::Warn : Tone::Neutral;
    }

    // ---- the variant cards ----
    if (row == 1) {
        m.iq_path = o.iq_path;
        m.iq_browse = o.iq_browse_offered;
        // DISABLED WHILE THE HEADER IS BEING READ: the engine reads it on a worker (fact.source.iqOpening); a wait of a second or more says it is the disk's.
        m.iq_opening = factOf(t, "fact.source.iqOpening") == "1";
        if (m.iq_opening) { m.iq_note = diskWaitLine(o.iq_open_wait_s); }
        // The box's own line (an Open over an empty box on a machine with no file dialog) rides above the wait line.
        if (!o.iq_box_note.empty()) { m.iq_note = m.iq_note.empty() ? o.iq_box_note : o.iq_box_note + "\n" + m.iq_note; }
    }
    if (m.variant == kVariantPluto) {
        m.pluto_address = factOf(t, "fact.source.plutoUri");
        m.pluto_help = tr("A Pluto on its USB cable answers at ip:192.168.2.1. ip:pluto.local works if "
                          "this machine has an mDNS responder, and any address or host name can be "
                          "typed. Nothing is contacted until you press Open.");
    }
    if (m.variant == kVariantRtlTcp) {
        m.rtltcp_address = factOf(t, "fact.source.rtlTcpAddr");
        m.rtltcp_help = tr("The address of a running rtl_tcp server, as host or host:port (the port is "
                           "1234 unless the server was started with another). Nothing is contacted "
                           "until you press Open.");
    }
    if (row == cardRow) {
        const bool cardOpening = (src & FOXAPI_SRC_CARD_OPENING) != 0u;
        const bool cardScanning = (src & FOXAPI_SRC_CARD_SCANNING) != 0u;
        const bool cardListed = (src & FOXAPI_SRC_CARD_LISTED) != 0u;
        std::string device = factOf(t, "fact.soundcard.device");
        const std::string hostApi = factOf(t, "fact.soundcard.hostApi");
        const std::string label = device.empty() ? std::string() : device + " (" + hostApi + ")";
        int at = -1;
        eachItem(t, FOXAPI_LIST_SOUND_CARDS, [&](int i, const FoxListItem& it) {
            SelectItem s;
            s.label = it.name;
            s.id = std::to_string(it.id);
            m.cards.push_back(std::move(s));
            if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) { at = i; }
        });
        m.card_index = at;
        m.card_text = label.empty() ? "-" : label;
        double formRate = 0.0, formCentre = 0.0;
        int channel = 0;
        bool iqStereo = false, swapIq = false;
        FoxListItem form{};
        entry(form);
        if (t.readList(FOXAPI_LIST_SOUND_CARD_FORM, 0, &form) >= 1) {
            formRate = form.value[0];
            formCentre = form.value[1];
            channel = static_cast<int>(form.value[2]);
            iqStereo = (form.flags & FOXAPI_ITEM_IQ_STEREO) != 0u;
            swapIq = (form.flags & FOXAPI_ITEM_SWAP_IQ) != 0u;
        }
        (void)formRate;
        if (cardOpening) {
            m.card_busy = cc::formatText(tr("Opening %s..."), label.c_str());
        } else if (cardScanning) {
            m.card_busy = tr("Scanning for devices...");
        }
        if (cardListed && m.cards.empty()) { m.card_none = tr("No sound card inputs found."); }
        m.card_enabled = !(cardOpening || cardScanning || deviceOpening);
        m.card_can_open = !device.empty() || at >= 0;
        m.card_format_index = iqStereo ? 1 : 0;
        m.card_channel_index = channel == 1 ? 1 : 0;
        m.card_swap_iq = swapIq;
        m.card_centre_mhz = cc::formatText("%.6f", formCentre / 1.0e6);
        eachItem(t, FOXAPI_LIST_SOUND_CARD_RATES, [&](int i, const FoxListItem& it) {
            const bool exclusive = (it.flags & FOXAPI_ITEM_EXCLUSIVE) != 0u;
            SelectItem s;
            s.label = soundCardRateLabel(it.value[0], exclusive);
            s.id = hzId(it.value[0]);
            if (exclusive) { s.tooltip = tr("Exclusive mode: FoxSDR has the card to itself while it runs, at a rate its own hardware runs at."); }
            m.card_rates.push_back(std::move(s));
            if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) { m.card_rate_index = i; }
        });
        double spanLo = 0.0, spanHi = 0.0;
        {
            const std::string span = factOf(t, "fact.soundcard.span");
            char* end = nullptr;
            spanLo = std::strtod(span.c_str(), &end);
            spanHi = (end != nullptr && end != span.c_str()) ? std::strtod(end, nullptr) : spanLo;
        }
        m.card_receives = soundCardReceivesLine(spanLo, spanHi);
        m.card_warning = factOf(t, "fact.status.soundCardMissing");
    }
    return m;
}

RadioExtrasModel readRadioExtras(const FoxReceiverState& st) {
    RadioExtrasModel m;
    m.wfm = st.demodMode == FOXAPI_DEMOD_WFM;
    if (!m.wfm) { return m; }
    m.stereo = (st.flags & FOXAPI_RX_STEREO_ENABLED) != 0u;
    m.stereo_tooltip = tr("Decode the 38 kHz difference channel when a 19 kHz\n"
                          "pilot is present. Unticked forces mono.");
    // The indicator reports the DECODER, not the checkbox: lit only when a pilot is locked AND stereo is enabled, amber while a pilot is locked and the user forced mono.
    const bool locked = (st.flags & FOXAPI_RX_PILOT_LOCKED) != 0u;
    const bool active = (st.flags & FOXAPI_RX_STEREO_ACTIVE) != 0u;
    if (active) {
        m.stereo_word = "ST";
        m.stereo_tone = Tone::Good;
    } else if (locked) {
        m.stereo_word = tr("ST (forced mono)");
        m.stereo_tone = Tone::Warn;
    } else {
        m.stereo_word = tr("MONO");
        m.stereo_tone = Tone::Neutral;
    }
    if (locked) { m.pilot_text = cc::formatText(tr("pilot %.2f"), st.pilotLevel); }
    m.rds_shown = true;
    const std::uint32_t rds = st.rdsFlags;
    if ((rds & (FOXAPI_RDS_PI_VALID | FOXAPI_RDS_PS_VALID | FOXAPI_RDS_RT_VALID)) == 0u) {
        // "listening, nothing yet" is not "not receiving at all": synced means the block decoder found the 1187.5 bit/s stream.
        m.rds_status = (rds & FOXAPI_RDS_SYNCED) != 0u ? std::string(tr("RDS: syncing...")) : std::string(tr("RDS: no data"));
        return m;
    }
    m.rds_ps = (rds & FOXAPI_RDS_PS_VALID) != 0u ? cc::formatText("PS  %s", st.rdsPs) : std::string("PS  --------");
    m.rds_pi = (rds & FOXAPI_RDS_PI_VALID) != 0u
                   ? cc::formatText("PI  %04X   PTY %u%s%s", st.rdsPi, st.rdsPty, (rds & FOXAPI_RDS_TP) != 0u ? "  TP" : "", (rds & FOXAPI_RDS_TA) != 0u ? "  TA" : "")
                   : std::string("PI  ----");
    if ((rds & FOXAPI_RDS_RT_VALID) != 0u) { m.rds_rt = st.rdsRadioText; }
    m.rds_stats = cc::formatText(tr("groups %u | block errors %u"), static_cast<unsigned>(st.rdsGroups), static_cast<unsigned>(st.rdsErrors));
    return m;
}

AudioFiltersModel readAudioFilters(const FoxReceiverState& st) {
    AudioFiltersModel m;
    const bool nr = (st.flags & FOXAPI_RX_NR) != 0u;
    const bool notch = (st.flags & FOXAPI_RX_NOTCH) != 0u;
    const bool autoNotch = (st.flags & FOXAPI_RX_AUTO_NOTCH) != 0u;
    const int filtersOn = (nr ? 1 : 0) + (notch ? 1 : 0) + (autoNotch ? 1 : 0);
    m.chip = filtersChip(filtersOn);
    m.lamp_lit = filtersOn > 0;
    m.chain_note = tr("chain: notch -> auto-notch -> noise reduction");
    m.nr_on = nr;
    m.nr_strength = static_cast<float>(std::clamp(st.nrStrength, 0.0, 1.0));
    m.nr_text = cc::formatText("%.2f", st.nrStrength);
    m.notch_on = notch;
    m.notch_freq = notchNormFromHz(st.notchHz);
    m.notch_freq_text = cc::formatText("%.0f Hz", st.notchHz);
    m.notch_q = notchQNorm(st.notchQ);
    m.notch_q_text = cc::formatText("%.0f", st.notchQ);
    m.auto_notch_on = autoNotch;
    m.auto_notch_locked = autoNotch && (st.flags & FOXAPI_RX_AUTO_NOTCH_ENGAGED) != 0u;
    m.auto_notch_status = autoNotchStatus(autoNotch, m.auto_notch_locked, st.autoNotchHz);
    return m;
}

SinksModel readSinks(TableReader& t, const FoxReceiverState& st) {
    SinksModel m;
    eachItem(t, FOXAPI_LIST_AUDIO_DEVICES, [&](int i, const FoxListItem& it) {
        SelectItem s;
        s.label = it.name;
        s.id = std::to_string(static_cast<int>(it.value[0]));
        m.devices.push_back(std::move(s));
        if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) {
            m.device_index = i;
            m.device_text = it.name;
        }
    });
    if (m.devices.empty()) { m.none_line = tr("No audio output devices"); }
    // The watchdog's note: shown persistently once it has happened.
    m.health_note = factOf(t, "fact.status.audioHealth");
    const int bufferMs = static_cast<int>(std::atof(factOf(t, "fact.audio.bufferMs").c_str()));
    const int leadMs = static_cast<int>(std::atof(factOf(t, "fact.audio.leadMs").c_str()));
    m.buffer_text = bufferPreview(bufferMs, leadMs);
    for (const int choice : bufferChoices()) {
        SelectItem s;
        s.label = bufferLabel(choice);
        s.id = std::to_string(choice);
        if (choice == bufferMs) { m.buffer_index = static_cast<int>(m.buffers.size()); }
        m.buffers.push_back(std::move(s));
    }
    m.lead_notice = audioLeadNotice(factOf(t, "fact.audio.leadNote"));
    if (st.audioMutedBy[0] != '\0') {
        m.muted_by = cc::formatText(tr("Muted by %s"), st.audioMutedBy);
        m.muted_note = tr("sound returns when the plugin stops");
    }
    const SinkChip chip = sinkChip(st);
    m.chip = chip.word;
    m.chip_tone = chip.tone;
    m.lamp_lit = chip.lit;
    m.buffer_tooltip = tr("How much sound is held back before it plays. A larger buffer rides "
                          "out a slow computer, at the cost of a longer delay between the radio "
                          "and the speakers. AUTOMATIC starts at 120 ms and deepens itself, to "
                          "at most 960 ms, when this computer falls behind. After a quarter of "
                          "an hour of quiet minutes it eases back one step at a time.");
    return m;
}

TransmitModel readTransmit(const FoxReceiverState& st) {
    TransmitModel m;
    const bool have = (st.flags & FOXAPI_RX_TX_AVAILABLE) != 0u;
    const bool onAir = (st.flags & FOXAPI_RX_TX_KEYED) != 0u;
    const bool faulted = have && (st.txFlags & FOXAPI_TX_FAULTED) != 0u;
    const TxChip chip = transmitChip(have, faulted, onAir);
    m.chip = chip.word;
    m.tone = chip.tone;
    m.available = have;
    m.keyed = onAir;
    m.armed = (st.flags & FOXAPI_RX_TX_REMOTE_ARMED) != 0u;
    m.fault = st.txError;
    return m;
}

RecorderModel readRecorder(TableReader& t, const FoxReceiverState& st, const ReadOptions& o) {
    RecorderModel m;
    const bool iqOn = (st.flags & FOXAPI_RX_RECORDING_IQ) != 0u;
    const bool audioOn = (st.flags & FOXAPI_RX_RECORDING_AUDIO) != 0u;
    const bool taping = iqOn || audioOn;
    const std::string startFact = factOf(t, "fact.recorder.start");
    const Take_ iq = readTakeOf(t, 1);
    const Take_ audio = readTakeOf(t, 2);
    m.folder = factOf(t, "fact.recorder.dir");
    m.iq_start = recorderStartOf(startFact, Take::Iq);
    m.audio_start = recorderStartOf(startFact, Take::Audio);
    // THE OPEN IS ON A WORKER: between the press and the file being there the button says so, and after a second the line under it says how long.
    m.iq_label = recorderLabel(Take::Iq, m.iq_start, iqOn);
    m.iq_enabled = m.iq_start != 2;
    m.iq_running = iqOn;
    if (m.iq_start != 0) {
        m.iq_progress = diskWaitLine(o.iq_start_wait_s);
    } else if (iqOn) {
        m.iq_progress = takeProgress(Take::Iq, iq.seconds, iq.bytes);
    }
    m.audio_label = recorderLabel(Take::Audio, m.audio_start, audioOn);
    m.audio_enabled = m.audio_start != 2;
    m.audio_running = audioOn;
    if (m.audio_start != 0) {
        m.audio_progress = diskWaitLine(o.audio_start_wait_s);
    } else if (audioOn) {
        m.audio_progress = takeProgress(Take::Audio, audio.seconds, audio.bytes);
        // THE TAKE IS SILENT while a plugin mutes the audio: the counters climb as for a real capture, so this is the only place that can say so.
        if (cascade::gui::audioHardMuted(st) && st.audioMutedBy[0] != '\0') { m.muted_warning = cc::formatText(tr("Muted by %s - this take is silent"), st.audioMutedBy); }
    }
    if ((st.flags & FOXAPI_RX_RUNNING) == 0u && taping) { m.hint = tr("(press Play to feed the recorders)"); }

    // The status column's RECORDER card: a disk that stopped taking the file, or a take that reached the 4 GB limit.
    const bool diskFault = (iq.active && iq.stalled) || (audio.active && audio.stalled);
    std::vector<std::string> errors{factOf(t, "fact.status.recordError")};
    std::vector<std::string> notices{factOf(t, "fact.status.recordNotice")};
    if (diskFault) { errors.push_back(tr("disk refused the file - nothing more is being kept")); }
    if (!(iq.active || audio.active) && (iq.full || audio.full)) {
        notices.push_back(iq.full ? std::string(tr("I/Q take reached the 4 GB limit for a WAV file")) : std::string(tr("audio take reached the 4 GB limit for a WAV file")));
        notices.push_back(tr("the file was closed and is complete"));
    }
    m.error_line = joinLines(errors);
    m.notice = joinLines(notices);
    if (diskFault) {
        m.chip = tr("STALLED");
        m.chip_tone = Tone::Err;
        m.lamp_lit = true;
    } else if (taping) {
        m.chip = tr("REC");
        m.chip_tone = Tone::Err;
        m.lamp_lit = true;
    } else if (iq.full || audio.full) {
        m.chip = tr("FULL");
        m.chip_tone = Tone::Warn;
    } else {
        m.chip = tr("OFF");
    }
    m.import_label = tr("Import");
    return m;
}

RadioSetupModel readRadioSetup(TableReader& t, const FoxReceiverState& st, const ReadOptions& o) {
    (void)st;
    RadioSetupModel m;
    m.shown = o.setup_shown;
    if (!m.shown) { return m; }
    m.title = tr("RADIO SETUP");
    if (o.setup_checking) { m.busy = tr("Checking this computer..."); }
    if (o.setup_have_answer) {
        m.lines.push_back(o.setup_show_page ? std::string(tr("FoxSDR found no radio it can use. This is what this computer shows, "
                                                              "and what to do about it:"))
                                            : std::string(tr("This is what this computer shows about your radios:")));
        for (const SetupFinding& f : o.findings) {
            m.lines.push_back(f.title);
            m.lines.push_back(f.explanation);
            if (!f.install.empty()) { m.lines.push_back(f.install); }
        }
    }
    // WHERE FOXSDR LOOKED (the evidence of a scan that found nothing), as the folded tree lists it.
    const std::vector<Evidence> evidence = readEvidence(t);
    if (!evidence.empty()) {
        m.lines.push_back(tr("Where FoxSDR looked"));
        int vendors = 0;
        for (const Evidence& e : evidence) { vendors += e.kind == 1 ? 1 : 0; }
        if (vendors > 0) {
            for (const Evidence& e : evidence) {
                if (e.kind == 1) { m.lines.push_back(cc::formatText(tr("found %s at %s"), e.name.c_str(), e.detail.c_str())); }
            }
        } else {
            m.lines.push_back(tr("no vendor SDR installation detected"));
        }
        for (const Evidence& e : evidence) {
            if (e.kind == 2) { m.lines.push_back(cc::formatText(tr("search: %s"), e.detail.c_str())); }
        }
        for (const Evidence& e : evidence) {
            if (e.kind == 3) { m.lines.push_back(cc::formatText(tr("module: %s"), e.detail.c_str())); }
        }
    }
    if (!o.setup_install_status.empty()) { m.lines.push_back(o.setup_install_status); }
    m.unbound = readUnbound(t);
    for (const SetupFinding& f : o.findings) {
        if (!f.install_target.empty()) {
            SetupKey k;
            k.id = "install-driver:" + f.install_target;
            k.label = tr("INSTALL DRIVER");
            k.enabled = !o.setup_installing;
            k.tooltip = tr("One Windows permission prompt, no Zadig. Or by hand:");
            m.keys.push_back(std::move(k));
        }
        if (!f.url.empty()) {
            SetupKey k;
            k.id = "link:" + f.url;
            k.label = f.url.rfind("https://", 0) == 0 ? f.url.substr(8) : f.url;
            k.tooltip = f.url;
            m.keys.push_back(std::move(k));
        }
    }
    SetupKey again;
    again.id = "check-again";
    again.label = tr("CHECK AGAIN");
    again.enabled = !o.setup_checking;
    m.keys.push_back(std::move(again));
    SetupKey dont;
    dont.id = "dont-show";
    dont.label = tr("Don't show this again");
    dont.on = o.setup_dont_show;
    m.keys.push_back(std::move(dont));
    return m;
}

ImportDialogModel readImportDialog(const ReadOptions& o) {
    ImportDialogModel m;
    m.shown = o.import_shown;
    if (!m.shown) { return m; }
    m.path = o.import_path;
    m.error = o.import_reason;
    m.preset = o.import_preset;
    if (!m.preset.empty()) { m.preset_line = cc::formatText(tr("The rows will join the preset \"%s\"."), m.preset.c_str()); }
    SelectItem freq;
    freq.label = tr("Frequency lists (*.xml, *.csv)");
    freq.id = "frequencies";
    SelectItem iq;
    iq.label = tr("IQ file");
    iq.id = "iq";
    // An import for a PRESET is a frequency list and nothing else (the ImGui dialog's forPreset): no "Import as" choice.
    if (m.preset.empty()) {
        m.kinds.push_back(std::move(freq));
        m.kinds.push_back(std::move(iq));
        m.kind_index = std::clamp(o.import_kind, 0, 1);
    }
    if (o.import_reading) { m.reading_line = diskWaitLine(o.import_wait_s); }
    return m;
}

SignalModels readSignal(TableReader& t, const FoxReceiverState& st, const ReadOptions& o) {
    SignalModels all;
    all.source = readSource(t, st, o);
    all.radio = readRadioExtras(st);
    all.filters = readAudioFilters(st);
    all.sinks = readSinks(t, st);
    all.transmit = readTransmit(st);
    all.recorder = readRecorder(t, st, o);
    all.setup = readRadioSetup(t, st, o);
    all.import = readImportDialog(o);
    return all;
}

// =================================================================================================================================
// the commands

std::uint64_t grantFor(std::uint32_t op) {
    // The grant each op needs (fox_engine_api.cpp grantFor): TUNE for tuning, TRANSMIT for the transmitter, ADMIN for administration, VIEW for the reports, SETTINGS for the rest.
    switch (op) {
        case FOXAPI_OP_SET_CENTRE: case FOXAPI_OP_SET_FREQUENCY: case FOXAPI_OP_SET_VFO_OFFSET:
        case FOXAPI_OP_STEP_TUNE: case FOXAPI_OP_BOOKMARK_TUNE: case FOXAPI_OP_SCANNER_RUN:
        case FOXAPI_OP_SCANNER_SKIP: case FOXAPI_OP_SCANNER_CONFIG:
        case FOXAPP_OP_VFO_TO_ABSOLUTE: case FOXAPP_OP_SET_VFO_OFFSET_FREE: case FOXAPP_OP_SCANNER_RANGE:
        case FOXAPP_OP_AIRBAND: case FOXAPP_OP_SCANNER_LIST:
            return FOXAPI_GRANT_TUNE;
        case FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN: case FOXAPP_OP_SET_TRANSMIT_ARGS:
        case FOXAPP_OP_PLUGIN_TX_GRANT: case FOXAPP_OP_TX_PLUGIN_ARM: case FOXAPP_OP_TX_DATA_FLAT:
            return FOXAPI_GRANT_TRANSMIT;
        case FOXAPI_OP_SETTING_SET: case FOXAPI_OP_TELEMETRY_ENABLE: case FOXAPI_OP_SERVER_CONFIG:
        case FOXAPI_OP_UPDATE_CHECK: case FOXAPI_OP_STORE_FETCH: case FOXAPI_OP_STORE_INSTALL:
        case FOXAPI_OP_STORE_REMOVE: case FOXAPI_OP_STORE_CANCEL: case FOXAPI_OP_STORE_UPDATE_ALL:
        case FOXAPP_OP_STORE_UPDATE: case FOXAPP_OP_STORE_REMOVE_BLOCKED: case FOXAPP_OP_STORE_ADD_ALL:
        case FOXAPP_OP_STORE_SITE: case FOXAPP_OP_STORE_PICTURES_WANT: case FOXAPP_OP_STORE_CLEAN_OLD:
        case FOXAPP_OP_TELEMETRY_CONSENT: case FOXAPP_OP_SET_CATALOGUE_URL:
            return FOXAPI_GRANT_ADMIN;
        case FOXAPI_OP_PROBLEM_REPORT: case FOXAPI_OP_FEATURE_REQUEST: case FOXAPP_OP_NOTE_PANEL:
        case FOXAPP_OP_SET_PATCH_PAGE_OPEN: case FOXAPP_OP_TRACK_INFO_LINE: case FOXAPP_OP_PLUGIN_USE:
            return FOXAPI_GRANT_VIEW;
        default:
            if ((op & 0xFF00u) == 0x0A00u) { return FOXAPI_GRANT_TRANSMIT; }
            return FOXAPI_GRANT_SETTINGS;
    }
}

std::string textOfCommand(const Command& c) { return cmd::textOf(c.q.c, c.q.longText); }

ConverterSetting converterStored(const SourceModel& m) {
    ConverterSetting s;
    s.mode = m.converter_mode;
    s.lo_hz = m.converter_lo_hz;
    s.inverted = m.converter_inverts;
    return s;
}

namespace {
bool inRange(int index, std::size_t size) { return index >= 0 && static_cast<std::size_t>(index) < size; }
}  // namespace

Command opDeviceSelected(TableReader& t, const SourceModel& m, int index) {
    if (!inRange(index, m.devices.size())) { return {}; }
    const SelectItem& row = m.devices[static_cast<std::size_t>(index)];
    if (!row.enabled) { return {}; }
    // SELECT_SOURCE with the row's id, whole: from fact.source.id.<entry id>, read now and not kept (the entry's detail is cut at 159 bytes).
    const std::string id = factOf(t, "fact.source.id." + row.id);
    if (id.empty()) { return {}; }
    return make(cmd::makeText(FOXAPI_OP_SELECT_SOURCE, id));
}
Command opRefresh() { return make(cmd::make(FOXAPI_OP_SCAN_DEVICES)); }
Command opScanOnOpen() { return make(cmd::make(FOXAPP_OP_SCAN_DEVICES_ON_OPEN)); }
Command opUsrps(bool on) { return make(cmd::makeInt(FOXAPP_OP_SET_NETWORK_USRP_SCAN, on ? 1 : 0)); }
Command opRateSelected(const SourceModel& m, int index) {
    if (!inRange(index, m.rates.size())) { return {}; }
    const SelectItem& r = m.rates[static_cast<std::size_t>(index)];
    if (r.label == m.rate_text) { return {}; }   // the row already shown
    double hz = 0.0;
    if (!parseNumber(r.id, hz)) { return {}; }
    return make(cmd::makeNum(FOXAPI_OP_SET_SAMPLE_RATE, hz));
}
Command opAntennaSelected(const SourceModel& m, int index) {
    if (!inRange(index, m.antennas.size())) { return {}; }
    const SelectItem& a = m.antennas[static_cast<std::size_t>(index)];
    if (m.single_port || a.id == m.antenna_text) { return {}; }   // the port already in use (a single port is a readout: nothing to pick)
    return make(cmd::makeText(FOXAPI_OP_SET_ANTENNA, a.id));
}
Command opAutoGain(bool on) { return make(cmd::makeInt(FOXAPI_OP_SET_DEVICE_AGC, on ? 1 : 0)); }
Command opGainChanged(const SourceModel& m, int stage, float norm) {
    if (!inRange(stage, m.stages.size())) { return {}; }
    const GainStage& g = m.stages[static_cast<std::size_t>(stage)];
    return make(cmd::makeText(FOXAPI_OP_SET_GAIN, g.name, 0, 0, gainFromNorm(g.lo, g.hi, norm, g.unit == "step")));
}
Command opBiasTee(bool on, const std::string& radioKey) {
    // OFF is immediate and names nothing; an ON carries the radio the person was asked about (FOXAPI_NOT_FOUND if another is open now).
    if (!on) { return make(cmd::makeInt(FOXAPI_OP_SET_BIAS_TEE, 0)); }
    return make(cmd::makeText(FOXAPI_OP_SET_BIAS_TEE, radioKey, 1));
}
Command opPpmToggled(bool on, double stored) { return make(cmd::makeNumInt(FOXAPP_OP_SET_PPM, stored, on ? 1 : 0)); }
Command opPpmCommitted(const std::string& text, double stored) {
    double v = 0.0;
    if (!parsePpm(text, v) || v == stored) { return {}; }
    return make(cmd::makeNumInt(FOXAPP_OP_SET_PPM, v, 1));
}
Command opIqOpen(const std::string& path) {
    if (path.empty()) { return {}; }
    return make(cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "file:" + path));
}
Command opIqRow() { return make(cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "row:iqfile")); }
IqOpenPlan planIqOpen(const std::string& typed, bool pickOffered) {
    IqOpenPlan p;
    p.path = cc::typedIqPath(typed);
    if (!p.path.empty()) {
        p.open = opIqOpen(p.path);
        return p;
    }
    // AN EMPTY BOX IS NOT A FILE THAT WOULD NOT OPEN (a tester's "the Open does nothing", 0.99.101): the dialog where there is one, else what the box is for.
    if (pickOffered) {
        p.browse = true;
    } else {
        p.note = tr("Type or paste the path of a WAV recording, then press Open.");
        p.log = "source: Open pressed with no I/Q file named and no file dialog to offer";
    }
    return p;
}
Command opPlutoAddress(const std::string& text) { return make(cmd::makeText(FOXAPP_OP_SET_PLUTO_URI, text)); }
Command opPlutoOpen(const std::string& address) { return make(cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "open-pluto:uri=" + address)); }
Command opRtlTcpAddress(const std::string& text) { return make(cmd::makeText(FOXAPP_OP_SET_RTLTCP_ADDR, text)); }
Command opRtlTcpOpen(const std::string& address) { return make(cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "open-rtltcp:rtltcp=" + address)); }

namespace {
// ONE FIELD OF THE SOUND CARD FORM (gui::soundCardFormCommand with a default SoundCardSettings and one field set): the device and host API are empty (the engine keeps its
// own), the mask in ival[1] names the field that counts, and the rest ride at the settings' defaults (card rate 48000, centre 0).
cmd::QueuedCommand cardField(std::int64_t mask, std::int64_t bits, double rateHz, double centreHz) {
    cmd::QueuedCommand q = cmd::makeText(FOXAPP_OP_SOUND_CARD_FORM, std::string(1, '\x1F'), bits, mask);
    q.c.num[0] = rateHz;
    q.c.num[1] = centreHz;
    return q;
}
}  // namespace

Command opCardSelected(const SourceModel& m, int index) {
    if (!inRange(index, m.cards.size()) || index == m.card_index) { return {}; }   // a click on the card the form already has does nothing
    double id = 0.0;
    if (!parseNumber(m.cards[static_cast<std::size_t>(index)].id, id)) { return {}; }
    return make(cmd::makeInt(FOXAPP_OP_SOUND_CARD_PICK, static_cast<std::int64_t>(id)));
}
Command opCardFormat(int index) { return make(cardField(FOXAPP_SOUND_CARD_FIELD_FORMAT, index == 1 ? 1 : 0, kPlainCardRateHz, 0.0)); }
Command opCardChannel(int index) { return make(cardField(FOXAPP_SOUND_CARD_FIELD_CHANNEL, index == 1 ? 2 : 0, kPlainCardRateHz, 0.0)); }
Command opCardSwap(bool on) { return make(cardField(FOXAPP_SOUND_CARD_FIELD_SWAP_IQ, on ? 4 : 0, kPlainCardRateHz, 0.0)); }
std::vector<Command> opCardCentre(const std::string& mhzText) {
    double mhz = 0.0;
    if (!parseNumber(mhzText, mhz)) { return {}; }
    const double hz = mhz * 1.0e6;
    if (!std::isfinite(hz)) { return {}; }
    // THE FORM ALWAYS TAKES IT (the record of where the external receiver is tuned, kept for the next Open); a card already running in I/Q mode also takes it at once
    // (the engine answers NO_CHANGE when no I/Q card is running, so this is sent after every centre committed).
    std::vector<Command> out;
    out.push_back(make(cardField(FOXAPP_SOUND_CARD_FIELD_CENTRE, 0, kPlainCardRateHz, hz)));
    out.push_back(make(cmd::makeNum(FOXAPP_OP_SOUNDCARD_IQ_CENTRE, hz)));
    return out;
}
Command opCardRate(const SourceModel& m, int index) {
    if (!inRange(index, m.card_rates.size())) { return {}; }
    double hz = 0.0;
    if (!parseNumber(m.card_rates[static_cast<std::size_t>(index)].id, hz)) { return {}; }
    return make(cardField(FOXAPP_SOUND_CARD_FIELD_RATE, 0, hz, 0.0));
}
Command opCardOpen(const SourceModel& m) {
    if (!m.card_can_open) { return {}; }
    return make(cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "soundcard:open"));
}

Command opAirspyDecimation(const SourceModel& m, int index) {
    if (!inRange(index, m.airspy_decimations.size()) || index == m.airspy_decimation_index) { return {}; }
    double factor = 0.0;
    if (!parseNumber(m.airspy_decimations[static_cast<std::size_t>(index)].id, factor)) { return {}; }
    return make(cmd::makeInt(FOXAPP_OP_AIRSPY_DECIMATION, static_cast<std::int64_t>(factor)));
}
Command opAirspyGainMode(const SourceModel& m, int buttonIndex) {
    if (buttonIndex < 0 || buttonIndex > 2 || buttonIndex == m.airspy_gain_mode) { return {}; }
    return make(cmd::makeInt(FOXAPP_OP_AIRSPY_GAIN_MODE, airspyGainModeCode(buttonIndex)));
}
namespace {
Command airspyGainOf(const GainStage& g, float norm) {
    // The slider spans 0..the stage's top in the hardware's own whole steps; FOXAPI_OP_SET_GAIN (NOT the radar scope's no-readback op) so the figure is read back.
    return make(cmd::makeText(FOXAPI_OP_SET_GAIN, g.name, 0, 0, gainFromNorm(0.0, g.hi > 0.0 ? g.hi : 1.0, norm, true)));
}
}  // namespace
Command opAirspyGain(const SourceModel& m, float norm) {
    if (m.airspy_stages.empty()) { return {}; }
    return airspyGainOf(m.airspy_stages[0], norm);
}
Command opAirspyAgc(bool mixer, bool on) { return make(cmd::makeInt(FOXAPP_OP_AIRSPY_AGC, mixer ? 1 : 0, on ? 1 : 0)); }
Command opAirspyStage(const SourceModel& m, const std::string& stageName, float norm) {
    for (const GainStage& g : m.airspy_stages) {
        if (g.name == stageName) { return airspyGainOf(g, norm); }
    }
    return {};
}
Command opOptionToggled(const SourceModel& m, int index, bool on) {
    if (!inRange(index, m.options.size())) { return {}; }
    return make(cmd::makeText(FOXAPI_OP_SET_DEVICE_OPTION, m.options[static_cast<std::size_t>(index)].name, on ? 1 : 0));
}

namespace {
Command converterOf(const ConverterSetting& s) {
    FoxCommand c = cmd::makeInt(FOXAPP_OP_SET_CONVERTER, s.mode, s.inverted ? 1 : 0);
    c.num[0] = s.lo_hz;
    return make(c);
}
}  // namespace
Command opConverterMode(const ConverterSetting& stored, int code) {
    if (code < 0 || code > 2) { return {}; }
    ConverterSetting next = stored;
    next.mode = code;
    if (code != 0 && !cc::converterLoValid(next.lo_hz)) { next.lo_hz = kDefaultConverterLoHz; }
    return converterOf(next);
}
Command opConverterLo(const ConverterSetting& stored, const std::string& text) {
    double lo = 0.0;
    if (!cc::parseConverterLoHz(text, lo) || lo == stored.lo_hz) { return {}; }
    ConverterSetting next = stored;
    next.lo_hz = lo;
    return converterOf(next);
}
Command opConverterQuick(const ConverterSetting& stored, int index) {
    if (index < 0 || index > 2 || stored.lo_hz == kConverterQuickLoHz[index]) { return {}; }
    ConverterSetting next = stored;
    next.lo_hz = kConverterQuickLoHz[index];
    return converterOf(next);
}
Command opConverterInverts(const ConverterSetting& stored, bool on) {
    ConverterSetting next = stored;
    next.inverted = on;
    return converterOf(next);
}

Command opStereo(bool on) { return make(cmd::makeInt(FOXAPI_OP_SET_STEREO, on ? 1 : 0)); }
Command opNr(bool on) { return make(cmd::makeInt(FOXAPI_OP_SET_NR, on ? 1 : 0, 0)); }
Command opNrStrength(float strength) { return make(cmd::makeNum(FOXAPP_OP_SET_NR_STRENGTH, std::clamp(static_cast<double>(strength), 0.0, 1.0))); }
Command opNotch(bool on) { return make(cmd::makeInt(FOXAPI_OP_SET_NOTCH, on ? 1 : 0, 0)); }
Command opNotchFreq(float norm) { return make(cmd::makeNum(FOXAPP_OP_SET_NOTCH_FREQUENCY, notchHzFromNorm(norm))); }
Command opNotchQ(float norm) { return make(cmd::makeNum(FOXAPP_OP_SET_NOTCH_Q, notchQFromNorm(norm))); }
Command opAutoNotch(bool on) { return make(cmd::makeInt(FOXAPI_OP_SET_AUTO_NOTCH, on ? 1 : 0)); }

Command opSinkDevice(const SinksModel& m, int index) {
    if (!inRange(index, m.devices.size())) { return {}; }
    double idx = 0.0;
    if (!parseNumber(m.devices[static_cast<std::size_t>(index)].id, idx)) { return {}; }
    return make(cmd::makeInt(FOXAPI_OP_AUDIO_DEVICE, static_cast<std::int64_t>(idx)));
}
Command opSinkBuffer(const SinksModel& m, int index) {
    if (!inRange(index, m.buffers.size())) { return {}; }
    double ms = 0.0;
    if (!parseNumber(m.buffers[static_cast<std::size_t>(index)].id, ms)) { return {}; }
    return make(cmd::makeInt(FOXAPP_OP_SET_AUDIO_BUFFER, static_cast<std::int64_t>(ms)));
}
Command opTransmitPage(bool open) { return make(cmd::makeInt(FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN, open ? 1 : 0)); }

Command opRecorderIq(const RecorderModel& m) {
    if (m.iq_start == 2) { return {}; }   // while the start is being cancelled the key is dead
    return make(cmd::makeInt(FOXAPI_OP_RECORD_IQ, (m.iq_start == 0 && !m.iq_running) ? 1 : 0));
}
Command opRecorderAudio(const RecorderModel& m) {
    if (m.audio_start == 2) { return {}; }
    return make(cmd::makeInt(FOXAPI_OP_RECORD_AUDIO, (m.audio_start == 0 && !m.audio_running) ? 1 : 0));
}

Command opSetupCheckAgain() { return make(cmd::make(FOXAPI_OP_SCAN_DEVICES)); }

Command opImportPath(const std::string& path, int kind) {
    if (path.empty()) { return {}; }
    if (kind == 0) { return make(cmd::makeText(FOXAPP_OP_BOOKMARK_IMPORT_FILE, path)); }
    if (kind == 1) { return make(cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "file:" + path)); }
    return {};
}

}  // namespace foxsdr::slintsignal
