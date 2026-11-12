// Lock the radio while recording: a pure state object that says which settings are frozen while a recording is open on a radio, and why.
//
// STANDALONE. Nothing here is wired (docs/feature-records/recplay.md). The engine's set-setting paths ask check() before they apply a change; a refusal
// carries the text the screen shows. Up to eight radios (the engine's limit): a recording on radio 3 freezes radio 3 only.
//
// WHAT IS FROZEN AND WHY (defaults; all in one table below, the owner's to change):
//   * Baseband I/Q recording (a WAV I/Q file or a SigMF pair): the sample rate (the file header holds one rate), the centre frequency, the frequency
//     correction (it moves the true centre), the frequency converter offset, the source (switching it would splice another radio into the file), and closing
//     the radio. A WAV file has no field for the centre, so a retune would leave the file's single recorded centre wrong; a SigMF file CAN log a retune
//     (a new capture segment), so when the recording says it does (metadataLogsRetunes) centre and correction changes are allowed.
//   * Channel (IF) recording: everything above, plus the channel's frequency, mode and bandwidth (the file header records them once).
//   * Audio recording: the audio output rate and device (the file has one rate), and closing the radio.
//   * Gain, squelch, the demodulator volume, tuning inside a baseband recording's band and every display setting are never frozen, unless the owner turns
//     lockGain on (a calibrated capture).
// A request that sets a frozen setting to the value it already has is allowed (a screen that re-sends its state must not be refused).
//
// THREADS: every call is thread-safe (one mutex, no I/O, no allocation except the reason text of a refusal).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::core::reclock {

constexpr int kMaxRadios = 8;

enum class RecordKind : int { BasebandIq = 0, ChannelIf = 1, Audio = 2 };

enum class Setting : int {
    CenterFrequency = 0,
    SampleRate,
    FrequencyCorrection,
    ConverterOffset,
    Source,
    CloseRadio,
    ChannelFrequency,
    ChannelMode,
    ChannelBandwidth,
    AudioRate,
    Gain,
    Count
};

inline const char* settingName(Setting s) {
    switch (s) {
        case Setting::CenterFrequency: return "centre frequency";
        case Setting::SampleRate: return "sample rate";
        case Setting::FrequencyCorrection: return "frequency correction";
        case Setting::ConverterOffset: return "converter offset";
        case Setting::Source: return "source";
        case Setting::CloseRadio: return "closing the radio";
        case Setting::ChannelFrequency: return "channel frequency";
        case Setting::ChannelMode: return "channel mode";
        case Setting::ChannelBandwidth: return "channel bandwidth";
        case Setting::AudioRate: return "audio output rate";
        case Setting::Gain: return "gain";
        default: return "setting";
    }
}

inline const char* kindName(RecordKind k) {
    switch (k) {
        case RecordKind::BasebandIq: return "an I/Q recording";
        case RecordKind::ChannelIf: return "a channel (IF) recording";
        case RecordKind::Audio: return "an audio recording";
    }
    return "a recording";
}

struct Verdict {
    bool allowed = true;
    std::string reason;          // empty when allowed
};

struct Frozen {                  // what the recording was started with (numbers compared exactly as given)
    double centerHz = 0.0;
    double sampleRateHz = 0.0;
    double correctionPpm = 0.0;
    double converterHz = 0.0;
    double channelHz = 0.0;
    double channelBandwidthHz = 0.0;
    double audioRateHz = 0.0;
    int sourceId = 0;
    int channelMode = 0;
};

struct Policy {
    bool lockWhileRecording = true;     // master switch
    bool lockGain = false;
};

class RecordLock {
public:
    explicit RecordLock(const Policy& p = Policy()) : policy_(p) {}

    void setPolicy(const Policy& p) { std::lock_guard<std::mutex> lk(m_); policy_ = p; }

    // False when the radio number is out of range or a recording is already open on it (nothing changes). `metadataLogsRetunes`: the file format records a
    // retune itself (SigMF captures).
    bool recordingStarted(int radio, RecordKind kind, const Frozen& frozen, bool metadataLogsRetunes = false) {
        if (radio < 0 || radio >= kMaxRadios) { return false; }
        std::lock_guard<std::mutex> lk(m_);
        Slot& s = slot_[static_cast<std::size_t>(radio)];
        if (s.open) { return false; }
        s.open = true; s.kind = kind; s.frozen = frozen; s.logsRetunes = metadataLogsRetunes;
        return true;
    }

    // Idempotent; false when nothing was open on that radio.
    bool recordingStopped(int radio) {
        if (radio < 0 || radio >= kMaxRadios) { return false; }
        std::lock_guard<std::mutex> lk(m_);
        Slot& s = slot_[static_cast<std::size_t>(radio)];
        const bool was = s.open;
        s = Slot();
        return was;
    }

    bool recording(int radio) const {
        if (radio < 0 || radio >= kMaxRadios) { return false; }
        std::lock_guard<std::mutex> lk(m_);
        return slot_[static_cast<std::size_t>(radio)].open;
    }

    // May `radio` have `what` set to `newValue`? (`newValue` is the number the request carries; for Source, ChannelMode and CloseRadio pass the id /
    // enumerator / 1.0 for "close".) An invalid radio number is refused with a reason.
    Verdict check(int radio, Setting what, double newValue) const {
        Verdict v;
        if (radio < 0 || radio >= kMaxRadios) { v.allowed = false; v.reason = "not a valid radio number"; return v; }
        if (!(what >= Setting::CenterFrequency && what < Setting::Count)) { v.allowed = false; v.reason = "not a valid setting"; return v; }
        std::lock_guard<std::mutex> lk(m_);
        const Slot& s = slot_[static_cast<std::size_t>(radio)];
        if (!s.open || !policy_.lockWhileRecording) { return v; }
        if (!frozenFor(s, what)) { return v; }
        if (isSame(s.frozen, what, newValue)) { return v; }
        v.allowed = false;
        v.reason = reasonFor(s, what);
        return v;
    }

    // The settings frozen on `radio` right now with their reasons (for the screen to grey out); empty when nothing is open.
    struct Entry { Setting setting; std::string label; std::string reason; };
    std::vector<Entry> frozenSettings(int radio) const {
        std::vector<Entry> out;
        if (radio < 0 || radio >= kMaxRadios) { return out; }
        std::lock_guard<std::mutex> lk(m_);
        const Slot& s = slot_[static_cast<std::size_t>(radio)];
        if (!s.open || !policy_.lockWhileRecording) { return out; }
        for (int i = 0; i < static_cast<int>(Setting::Count); ++i) {
            const Setting st = static_cast<Setting>(i);
            if (frozenFor(s, st)) { out.push_back(Entry{st, settingName(st), reasonFor(s, st)}); }
        }
        return out;
    }

private:
    struct Slot {
        bool open = false;
        RecordKind kind = RecordKind::BasebandIq;
        Frozen frozen;
        bool logsRetunes = false;
    };

    bool frozenFor(const Slot& s, Setting what) const {
        switch (what) {
            case Setting::SampleRate:
            case Setting::Source:
            case Setting::ConverterOffset:
                return s.kind != RecordKind::Audio;
            case Setting::CenterFrequency:
            case Setting::FrequencyCorrection:
                return s.kind != RecordKind::Audio && !(s.kind == RecordKind::BasebandIq && s.logsRetunes);
            case Setting::CloseRadio:
                return true;
            case Setting::ChannelFrequency:
            case Setting::ChannelMode:
            case Setting::ChannelBandwidth:
                return s.kind == RecordKind::ChannelIf;
            case Setting::AudioRate:
                return s.kind == RecordKind::Audio;
            case Setting::Gain:
                return policy_.lockGain;
            default:
                return false;
        }
    }

    static bool same(double a, double b) { return a == b || std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); }

    static bool isSame(const Frozen& f, Setting what, double v) {
        if (!std::isfinite(v)) { return false; }
        switch (what) {
            case Setting::CenterFrequency: return same(v, f.centerHz);
            case Setting::SampleRate: return same(v, f.sampleRateHz);
            case Setting::FrequencyCorrection: return same(v, f.correctionPpm);
            case Setting::ConverterOffset: return same(v, f.converterHz);
            case Setting::Source: return v == static_cast<double>(f.sourceId);
            case Setting::CloseRadio: return false;
            case Setting::ChannelFrequency: return same(v, f.channelHz);
            case Setting::ChannelMode: return v == static_cast<double>(f.channelMode);
            case Setting::ChannelBandwidth: return same(v, f.channelBandwidthHz);
            case Setting::AudioRate: return same(v, f.audioRateHz);
            case Setting::Gain: return false;
            default: return false;
        }
    }

    static std::string reasonFor(const Slot& s, Setting what) {
        std::string why;
        switch (what) {
            case Setting::SampleRate: why = "the file stores one sample rate, and a different rate would make the rest of it play at the wrong speed"; break;
            case Setting::CenterFrequency: why = "the recording stores one centre frequency, and the signals after a retune would be labelled with the wrong one"; break;
            case Setting::FrequencyCorrection: why = "the correction moves the true centre frequency that the recording stores"; break;
            case Setting::ConverterOffset: why = "the converter offset changes the frequency the recording is labelled with"; break;
            case Setting::Source: why = "another source would be spliced into the same file"; break;
            case Setting::CloseRadio: why = "closing the radio would end the recording with a cut-off file"; break;
            case Setting::ChannelFrequency: why = "the channel recording stores the channel's frequency once"; break;
            case Setting::ChannelMode: why = "the channel recording stores the mode once"; break;
            case Setting::ChannelBandwidth: why = "the channel recording stores the bandwidth once"; break;
            case Setting::AudioRate: why = "the audio file stores one sample rate"; break;
            case Setting::Gain: why = "gain is locked for a calibrated capture"; break;
            default: why = "it is locked"; break;
        }
        std::string r = "The ";
        r += settingName(what);
        r += " is locked while ";
        r += kindName(s.kind);
        r += " is open: ";
        r += why;
        r += ". Stop the recording first.";
        return r;
    }

    mutable std::mutex m_;
    Policy policy_;
    std::array<Slot, kMaxRadios> slot_{};
};

}  // namespace cascade::core::reclock
