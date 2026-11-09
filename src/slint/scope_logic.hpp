// scope_logic.hpp - THE DEMOD SCOPE'S RULES (docs/engine-stage4.md 6.57, the Slint port's scope modes): the bench oscilloscope the ImGui window opens from the TOOLS
// rail's "Demod scope" row, as pure functions and one state object with no Slint, no ImGui and no engine in it. The classic's gather (app_window.cpp gatherDemodScope
// 18333, the page 18531) is ported rule for rule over the same ImGui-free headers (gui/demod_scope.hpp: the trigger, the ladders, the reductions; gui/scope_memory.hpp: AVG and
// PERSIST; gui/fm_mpx_plan.hpp: the multiplex's bands), so what the Slint tube draws is what the classic tube draws and tests/test_demod_scope.cpp pins the arithmetic for both.
//
// THE DATA ARRIVES THROUGH ONE SEAM, ScopeTapReader: the table's read_scope (FOXAPI_CAP_SCOPE) in the window, a script in the tests. The state object owns every buffer the
// feed points into, so a frame costs no allocation once the buffers have grown; its clock is injected (DemodInputs::nowS) - liveness, the AVG blend and the PERSIST fade are
// timed by it, never counted in frames.
//
// WHAT IS NOT HERE: the picture (scope_compositor.cpp rasterises the glass into an RGBA buffer), the words on the glass and the keys (ui/scope_modes.slint), the page the window
// draws them on and the settings file (scope_publish.cpp). The multiplex transform is a radix-2 transform written here (2048 audio bins, 8192 multiplex bins) so the host links
// neither pffft nor the engine's dsp for it; fftPowerDb is tested against a pure tone.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "foxsdr_api.h"
#include "gui/demod_scope.hpp"
#include "gui/scope_memory.hpp"

namespace foxsdr::slintscope {

// ---- the seam: the table's read_scope ----
struct ScopeTapReader {
    virtual ~ScopeTapReader() = default;
    // As FoxEngineApi::read_scope: the tap's info (info->structSize set by the caller) and/or its newest `capFrames` frames into `out`; frames copied, negative on an error.
    virtual std::int32_t readScope(std::uint32_t tap, FoxScopeInfo* info, float* out, std::uint32_t capFrames) = 0;
};

// What the window knows besides the taps.
struct DemodInputs {
    bool wfm = false;                  // state.demodMode == FOXAPI_DEMOD_WFM: the multiplex exists
    double audioRateHz = 0.0;          // state.audioRateHz: stands in when the tap says no rate
    double channelRateHz = 0.0;        // state.channelRateHz
    std::string audioFrom;             // state.audioPlayingPlugin: a plugin owns the speakers
    double nowS = 0.0;                 // a steady clock, seconds
};

constexpr std::size_t kFftAudio = 2048;   // gui kScopeFftSize
constexpr std::size_t kFftMpx = 8192;     // gui kScopeMpxFftSize

// A real input's power spectrum in dB (a full-scale tone reads 0 dB): Hann window, 2/(N * coherent gain), cascade::gui::scopeSpectrumDb. `n` must be a power of two; the first
// `bins` bins are written (bins <= n/2).
void fftPowerDb(const float* in, std::size_t n, std::size_t bins, std::vector<float>& outDb);

// What the tube draws from: the classic's DemodScopeFeed (gui/demod_scope_face.hpp, which includes imgui.h) without the memory pointer. Plain pointers into the state object's buffers.
struct DemodFeed {
    const float* audio = nullptr;
    std::size_t audioCount = 0;
    double audioRateHz = 0.0;
    const float* iqI = nullptr;
    const float* iqQ = nullptr;
    std::size_t iqCount = 0;
    double iqRateHz = 0.0;
    const float* spectrumDb = nullptr;
    std::size_t spectrumBins = 0;
    double spectrumBinHz = 0.0;
    double spectrumSpanHz = 0.0;
    bool live = false;
};

// The peak the AUTO ranging is fed (the classic's demodScopePeak): the larger of |I|, |Q| for the baseband positions, the audio's for the others; 0 holds the step.
float demodPeak(const DemodFeed& feed, cascade::gui::ScopeSignal signal);

// One band of the multiplex plan placed on the glass (fractions of its width), for the Slint labels.
struct MpxMark {
    double x0 = 0.0, x1 = 0.0;         // 0..1; a tone has x0 == x1
    double room = 0.0;                 // the label's room as a fraction of the width (a band's extent, a tone's 9 kHz either side): drawn only if the text fits
    std::string label;                 // the plan's key, translated
    bool tone = false, optional = false;
};

// What the words on and around the tube say.
struct DemodReadouts {
    bool spectral = false;
    std::string caption;               // the signal's long caption, translated
    std::string display;               // "" for NORM, else the translated key
    std::string centre;                // "10 ms/DIV", "0 - 20.0 kHz", "--"
    std::string gain;                  // "100 mV/DIV", "10 dB/DIV"
    std::string audioFrom;             // "AUDIO FROM %s" or ""
    std::string dead;                  // the dead tube's sentence or ""
    std::string timebaseKey, gainKey;  // the page's TIME and GAIN readouts ("-- (spectrum)" on a spectrum)
};

class DemodScope {
public:
    cascade::gui::DemodScopeState state;     // the settings the panel keeps (saved in the interface half)
    bool open = false;                       // saved, never restored (startupState clears it)

    // One frame: the taps' info, the gather for the selected signal, the AUTO ranging, the memory. `inputs` is the window's.
    void tick(ScopeTapReader& taps, const DemodInputs& inputs);

    // ---- the keys (each returns whether it changed anything) ----
    bool setSignal(int index);
    bool stepTimebase(int steps);
    bool stepGain(int steps);                // refused while AUTO is ticked or on a spectrum
    bool setAutoGain(bool on);
    bool setDisplay(int index);

    // ---- the reads ----
    cascade::gui::ScopeSignal signalNow() const;                 // the saved signal against today's mode (MPX only in WFM)
    bool mpxAvailable() const { return wfm_; }
    bool spectral() const;
    bool live() const { return live_; }
    const DemodFeed& feed() const { return feed_; }
    DemodReadouts readouts() const;
    std::vector<MpxMark> mpxMarks() const;                       // empty unless the multiplex is selected and has bins
    cascade::gui::ScopeMemory& memory() { return memory_; }
    double dtS() const { return dtS_; }
    double nowS() const { return nowS_; }
    std::uint64_t frames() const { return frames_; }
    // the tap writes moved since the last tick on the selected signal's tap, or a setting changed: the picture needs a repaint
    bool dirty() const { return dirty_; }
    void markPainted() { dirty_ = false; }

private:
    void gather(ScopeTapReader& taps, const DemodInputs& in);
    void touch() { dirty_ = true; }

    DemodFeed feed_;
    cascade::gui::ScopeMemory memory_;
    cascade::gui::ScopeLiveness audioLive_, iqLive_, mpxLive_;
    bool live_ = false, wfm_ = false, dirty_ = true;
    int memoryMode_ = -1;
    double memoryLastS_ = 0.0, dtS_ = 0.0, nowS_ = 0.0;
    std::uint64_t frames_ = 0, lastWritten_ = 0;
    std::string audioFrom_;
    std::vector<float> audioBuf_, iBuf_, qBuf_, specDb_, scratch_;
    std::vector<std::complex<float>> iqBuf_;
};

}  // namespace foxsdr::slintscope
