// scope_logic.cpp - the demod scope's rules (see scope_logic.hpp). A port of app_window.cpp gatherDemodScope (18333) and drawDemodScopePage (18531), the AUTO ranging and the
// memory step first, over the headers the classic shares.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "scope_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/fm_mpx_plan.hpp"

namespace foxsdr::slintscope {

using cascade::gui::ScopeSignal;
using cascade::i18n::tr;

namespace {

constexpr double kPi = 3.14159265358979323846;

// An in-place iterative radix-2 transform of n (a power of two) complex values.
void fftInPlace(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) { j ^= bit; }
        j ^= bit;
        if (i < j) { std::swap(a[i], a[j]); }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<float> wl(static_cast<float>(std::cos(ang)), static_cast<float>(std::sin(ang)));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

bool isPow2(std::size_t n) { return n >= 2 && (n & (n - 1)) == 0; }

std::size_t framesOf(std::int32_t n) { return n > 0 ? static_cast<std::size_t>(n) : std::size_t{0}; }

}  // namespace

void fftPowerDb(const float* in, std::size_t n, std::size_t bins, std::vector<float>& outDb) {
    outDb.clear();
    if (in == nullptr || !isPow2(n)) { return; }
    bins = std::min(bins, n / 2);
    std::vector<std::complex<float>> a(n);
    double gain = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        gain += w;
        a[i] = std::complex<float>(in[i] * static_cast<float>(w), 0.0f);
    }
    gain /= static_cast<double>(n);   // the window's coherent gain
    fftInPlace(a);
    const float norm = 2.0f / (static_cast<float>(n) * static_cast<float>(gain));
    outDb.resize(bins);
    for (std::size_t k = 0; k < bins; ++k) { outDb[k] = cascade::gui::scopeSpectrumDb(std::abs(a[k]) * norm); }
}

float demodPeak(const DemodFeed& feed, ScopeSignal signal) {
    if (cascade::gui::scopeSignalIsBaseband(signal)) {
        const float pi = cascade::gui::scopePeak(feed.iqI, feed.iqCount);
        const float pq = cascade::gui::scopePeak(feed.iqQ, feed.iqCount);
        return pi > pq ? pi : pq;
    }
    return cascade::gui::scopePeak(feed.audio, feed.audioCount);
}

ScopeSignal DemodScope::signalNow() const {
    return cascade::gui::scopeSignalForMode(cascade::gui::scopeSignalFromIndex(state.signal), wfm_);
}

bool DemodScope::spectral() const {
    const ScopeSignal s = signalNow();
    return s == ScopeSignal::Spectrum || s == ScopeSignal::Mpx;
}

bool DemodScope::setSignal(int index) {
    const int v = static_cast<int>(cascade::gui::scopeSignalFromIndex(index));
    if (!cascade::gui::scopeSignalAvailable(static_cast<ScopeSignal>(v), wfm_) || v == state.signal) { return false; }
    state.signal = v;
    touch();
    return true;
}

bool DemodScope::stepTimebase(int steps) {
    if (spectral()) { return false; }   // the group is disabled on a spectrum
    const int v = cascade::gui::clampScopeTimebase(state.timebase + steps);
    if (v == state.timebase) { return false; }
    state.timebase = v;
    touch();
    return true;
}

bool DemodScope::stepGain(int steps) {
    if (spectral() || state.autoGain) { return false; }
    const int v = cascade::gui::clampScopeGain(state.gainIndex + steps);
    if (v == state.gainIndex) { return false; }
    state.gainIndex = v;
    touch();
    return true;
}

bool DemodScope::setAutoGain(bool on) {
    if (on == state.autoGain) { return false; }
    state.autoGain = on;
    touch();
    return true;
}

bool DemodScope::setDisplay(int index) {
    const int v = cascade::gui::clampScopeDisplay(index);
    if (v == state.display) { return false; }
    state.display = v;
    touch();
    return true;
}

void DemodScope::gather(ScopeTapReader& taps, const DemodInputs& in) {
    const ScopeSignal sig = signalNow();
    const bool baseband = cascade::gui::scopeSignalIsBaseband(sig);
    const bool mpx = (sig == ScopeSignal::Mpx);
    feed_ = DemodFeed{};

    // THE THREE TAPS' INFO, every tick whichever signal is selected (an info-only read of each).
    FoxScopeInfo infoAudio{}, infoIq{}, infoMpx{};
    const auto tapInfo = [&taps](std::uint32_t tap, FoxScopeInfo& info) {
        info = FoxScopeInfo{};
        info.structSize = sizeof(info);
        return taps.readScope(tap, &info, nullptr, 0);
    };
    const std::int32_t readAudio = tapInfo(FOXAPI_SCOPE_AUDIO, infoAudio);
    const std::int32_t readIq = tapInfo(FOXAPI_SCOPE_IQ, infoIq);
    const std::int32_t readMpx = tapInfo(FOXAPI_SCOPE_MPX, infoMpx);

    feed_.audioRateHz = infoAudio.rateHz > 0.0 ? infoAudio.rateHz : in.audioRateHz;
    feed_.iqRateHz = infoIq.rateHz > 0.0 ? infoIq.rateHz : in.channelRateHz;
    audioFrom_ = in.audioFrom;

    // LIVE = samples arrived since the last look, measured on the tap this signal reads; a failed read is a counter that did not move.
    const bool audioLive = cascade::gui::scopeTapLive(audioLive_, readAudio >= 0 ? infoAudio.written : audioLive_.lastSeen, in.nowS);
    const bool iqLive = cascade::gui::scopeTapLive(iqLive_, readIq >= 0 ? infoIq.written : iqLive_.lastSeen, in.nowS);
    const bool mpxLive = cascade::gui::scopeTapLive(mpxLive_, readMpx >= 0 ? infoMpx.written : mpxLive_.lastSeen, in.nowS);
    live_ = baseband ? iqLive : (mpx ? mpxLive : audioLive);
    feed_.live = live_;
    const std::uint64_t written = baseband ? infoIq.written : (mpx ? infoMpx.written : infoAudio.written);
    if (written != lastWritten_) { lastWritten_ = written; touch(); }
    if (!live_) { return; }

    if (sig == ScopeSignal::Spectrum || mpx) {
        const std::size_t fftSize = mpx ? kFftMpx : kFftAudio;
        const double rateHz = mpx ? feed_.iqRateHz : feed_.audioRateHz;
        const double spanHz = mpx ? cascade::gui::mpxSpanHz(rateHz) : cascade::gui::scopeSpectrumSpanHz(rateHz);
        if (!(rateHz > 0.0) || !(spanHz > 0.0)) { return; }
        audioBuf_.resize(fftSize);
        const std::size_t got = framesOf(taps.readScope(mpx ? FOXAPI_SCOPE_MPX : FOXAPI_SCOPE_AUDIO, nullptr, audioBuf_.data(), static_cast<std::uint32_t>(fftSize)));
        if (got < fftSize) { return; }
        const double binHz = rateHz / static_cast<double>(fftSize);
        const std::size_t bins = cascade::gui::scopeSpectrumBins(fftSize, rateHz, spanHz);
        fftPowerDb(audioBuf_.data(), fftSize, bins, specDb_);
        if (specDb_.size() != bins) { return; }
        feed_.spectrumDb = specDb_.data();
        feed_.spectrumBins = bins;
        feed_.spectrumBinHz = binHz;
        // THE AXIS THE BINS ACTUALLY COVER, not the span that was asked for.
        feed_.spectrumSpanHz = (bins > 1) ? static_cast<double>(bins - 1) * binHz : spanHz;
        return;
    }

    // --- the three TRIGGERED positions ---
    const double rate = baseband ? feed_.iqRateHz : feed_.audioRateHz;
    std::size_t sweep = cascade::gui::scopeSweepSamples(cascade::gui::scopeTimebaseMs(state.timebase), rate);
    if (sweep == 0) { return; }
    const std::size_t cap = static_cast<std::size_t>(baseband ? infoIq.capacityFrames : infoAudio.capacityFrames);
    std::size_t want = sweep * 2;
    if (cap != 0 && want > cap) { want = cap; }
    if (sweep > want) { sweep = want; }
    const std::size_t search = want - sweep;
    const float hyst = cascade::gui::scopeHysteresis(state.gainIndex);
    const std::size_t holdoff = cascade::gui::scopeHoldoffSamples(sweep);

    if (!baseband) {
        audioBuf_.resize(want);
        const std::size_t got = framesOf(taps.readScope(FOXAPI_SCOPE_AUDIO, nullptr, audioBuf_.data(), static_cast<std::uint32_t>(want)));
        if (got < sweep) { return; }
        std::size_t start = got - sweep;   // free-running: the newest sweep
        if (search > 0) {
            const std::size_t t = cascade::gui::scopeFindTrigger(audioBuf_.data(), got, sweep, 0.0f, hyst, holdoff);
            if (t != cascade::gui::kScopeNoTrigger) { start = t; }
        }
        feed_.audio = audioBuf_.data() + start;
        feed_.audioCount = sweep;
        return;
    }

    // The I/Q tap's frames are two floats each; `want` is a count of FRAMES.
    iqBuf_.resize(want);
    const std::size_t got = framesOf(taps.readScope(FOXAPI_SCOPE_IQ, nullptr, reinterpret_cast<float*>(iqBuf_.data()), static_cast<std::uint32_t>(want)));
    if (got < sweep) { return; }
    iBuf_.resize(got);
    qBuf_.resize(got);
    for (std::size_t i = 0; i < got; ++i) {
        iBuf_[i] = iqBuf_[i].real();
        qBuf_[i] = iqBuf_[i].imag();
    }
    std::size_t start = got - sweep;
    if (search > 0) {
        // TRIGGERED ON I, AND BOTH ARMS SHIFTED TOGETHER: the phase between the two is the whole content of the vector display.
        const std::size_t t = cascade::gui::scopeFindTrigger(iBuf_.data(), got, sweep, 0.0f, hyst, holdoff);
        if (t != cascade::gui::kScopeNoTrigger) { start = t; }
    }
    feed_.iqI = iBuf_.data() + start;
    feed_.iqQ = qBuf_.data() + start;
    feed_.iqCount = sweep;
}

void DemodScope::tick(ScopeTapReader& taps, const DemodInputs& in) {
    wfm_ = in.wfm;
    nowS_ = in.nowS;
    ++frames_;
    gather(taps, in);
    const ScopeSignal sig = signalNow();

    // THE ATTENUATOR RANGES BEFORE THE TUBE IS DRAWN, from the pre-average feed, one detent a tick.
    if (state.autoGain) {
        const int g = cascade::gui::scopeAutoGain(demodPeak(feed_, sig), state.gainIndex);
        if (g != state.gainIndex) { state.gainIndex = g; touch(); }
    }

    // THE MEMORY: started afresh when the mode changes and whenever the chain is not producing; timed by the injected clock.
    dtS_ = (memoryLastS_ > 0.0) ? in.nowS - memoryLastS_ : 0.0;
    memoryLastS_ = in.nowS;
    if (state.display != memoryMode_ || !feed_.live) {
        memory_.reset();
        memoryMode_ = state.display;
    }
    if (cascade::gui::scopeDisplayFromIndex(state.display) == cascade::gui::ScopeDisplay::Average) {
        using cascade::gui::ScopeMemory;
        using cascade::gui::scopeMemoryKey;
        const auto tb = static_cast<std::uint64_t>(state.timebase);
        if (feed_.audio != nullptr && feed_.audioCount > 0) {
            feed_.audio = memory_.average(ScopeMemory::kAudio, feed_.audio, feed_.audioCount, dtS_,
                                          scopeMemoryKey(10u, feed_.audioCount, tb, static_cast<std::uint64_t>(feed_.audioRateHz)));
        }
        if (feed_.iqI != nullptr && feed_.iqQ != nullptr && feed_.iqCount > 0) {
            const std::uint64_t k = scopeMemoryKey(11u, feed_.iqCount, tb, static_cast<std::uint64_t>(feed_.iqRateHz));
            feed_.iqI = memory_.average(ScopeMemory::kI, feed_.iqI, feed_.iqCount, dtS_, k);
            feed_.iqQ = memory_.average(ScopeMemory::kQ, feed_.iqQ, feed_.iqCount, dtS_, k);
        }
        if (feed_.spectrumDb != nullptr && feed_.spectrumBins > 0) {
            feed_.spectrumDb = memory_.average(ScopeMemory::kSpectrum, feed_.spectrumDb, feed_.spectrumBins, dtS_,
                                               scopeMemoryKey(12u, static_cast<std::uint64_t>(sig), feed_.spectrumBins));
        }
    }
}

DemodReadouts DemodScope::readouts() const {
    DemodReadouts r;
    const ScopeSignal sig = signalNow();
    r.spectral = spectral();
    r.caption = tr(cascade::gui::scopeSignalCaption(sig));
    const cascade::gui::ScopeDisplay disp = cascade::gui::scopeDisplayFromIndex(state.display);
    if (disp != cascade::gui::ScopeDisplay::Normal) { r.display = tr(cascade::gui::scopeDisplayKey(disp)); }

    char buf[48];
    if (r.spectral) {
        const double span = (feed_.spectrumSpanHz > 0.0) ? feed_.spectrumSpanHz : cascade::gui::scopeSpectrumSpanHz(feed_.audioRateHz);
        std::snprintf(buf, sizeof(buf), "0 - %.1f kHz", span / 1000.0);
        r.centre = buf;
        r.gain = "10 dB/DIV";
        r.timebaseKey = tr("-- (spectrum)");
        r.gainKey = "10 dB/DIV";
    } else {
        const double rate = cascade::gui::scopeSignalIsBaseband(sig) ? feed_.iqRateHz : feed_.audioRateHz;
        char tb[32];
        cascade::gui::formatScopeTimebase(tb, sizeof(tb), cascade::gui::scopeTimebaseMs(state.timebase));
        r.centre = rate > 0.0 ? tb : "--";
        char gn[32];
        cascade::gui::formatScopeGain(gn, sizeof(gn), cascade::gui::scopeGainPerDiv(state.gainIndex));
        r.gain = gn;
        r.timebaseKey = tb;
        r.gainKey = gn;
    }
    if (!audioFrom_.empty() && sig != ScopeSignal::Baseband && sig != ScopeSignal::Vector) {
        std::string via;
        cascade::core::formatUtf8(via, tr("AUDIO FROM %s"), audioFrom_.c_str());
        r.audioFrom = via;
    }

    // THE DEAD TUBE'S ONE SENTENCE (demod_scope_face.cpp 427-583): not a flat trace at zero, which would claim a measurement nobody made.
    if (!feed_.live) {
        r.dead = tr("NO SAMPLES - RECEIVER STOPPED");
    } else {
        switch (sig) {
            case ScopeSignal::Audio:
                if (feed_.audio == nullptr || feed_.audioCount == 0) { r.dead = tr("NO AUDIO IN THE TAP"); }
                break;
            case ScopeSignal::Spectrum:
            case ScopeSignal::Mpx:
                if (feed_.spectrumDb == nullptr || feed_.spectrumBins < 2 || !(feed_.spectrumBinHz > 0.0)) {
                    r.dead = sig == ScopeSignal::Mpx ? tr("NO MULTIPLEX IN THE TAP") : tr("NO AUDIO IN THE TAP");
                }
                break;
            case ScopeSignal::Baseband:
            case ScopeSignal::Vector:
                if (feed_.iqI == nullptr || feed_.iqQ == nullptr || feed_.iqCount == 0) { r.dead = tr("NO BASEBAND IN THE TAP"); }
                break;
        }
    }
    return r;
}

std::vector<MpxMark> DemodScope::mpxMarks() const {
    std::vector<MpxMark> out;
    if (signalNow() != ScopeSignal::Mpx || !feed_.live || feed_.spectrumDb == nullptr || feed_.spectrumBins < 2) { return out; }
    const double span = (feed_.spectrumSpanHz > 0.0) ? feed_.spectrumSpanHz : static_cast<double>(feed_.spectrumBins) * feed_.spectrumBinHz;
    if (!(span > 0.0)) { return out; }
    for (std::size_t i = 0; i < cascade::gui::kMpxBandCount; ++i) {
        const cascade::gui::MpxBand& band = cascade::gui::kMpxBands[i];
        if (!cascade::gui::mpxBandInSpan(band, span)) { continue; }
        MpxMark m;
        m.tone = band.loHz == band.hiHz;
        m.optional = band.optional;
        m.label = tr(band.label);
        m.x0 = cascade::gui::mpxFraction(band.loHz, span);
        m.x1 = m.tone ? m.x0 : cascade::gui::mpxFraction(std::min(band.hiHz, span), span);
        m.room = static_cast<double>(cascade::gui::mpxLabelRoomPx(band, span, 1.0f));
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace foxsdr::slintscope
