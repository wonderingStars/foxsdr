// test_scope_window.cpp - THE TWO SCOPES READ THEIR ENGINE API SESSION (window port, group 12: drawScopeMode,
// drawScopeModeControl, gatherDemodScope and pipelineIsWfm; engine-stage4.md 6.5, API 0.3 section 8.11).
//
// WHAT THEY DRAW. The DEMOD scope (the audio, its spectrum, the I/Q as two traces and as a vector, the FM multiplex)
// draws from the table's read_scope - the three taps' frames and their `written` counters, read at the call, never a
// publish old - the state's channelRateHz, demodMode and audioPlayingPlugin, and the tap's capacityFrames. The RADAR
// scope (the aircraft plan-position indicator that replaces the window) draws from the state (the receiver's position,
// signalDb, the RUNNING, DEVICE_OPEN and DEVICE_AGC flags), the first GAINS entry through the window's source cache,
// and the PLUGINS census; its two commands are FOXAPI_OP_RUN and FOXAPP_OP_SET_GAIN_NO_READBACK through uiSubmit. The
// AIRCRAFT are the HOST PROCESS's (the plugin windows' own poll, hostTracks()) and are not driven here: no module in a
// hermetic test is a track source, so the aircraft count, the selected altitude and the range drum are held by the
// code (they read hostTracks() exactly as the old members read the plugin UI), and this file says so.
//
//   A  the demod scope's feed is what the old member made of the pipeline's own rings (the old model restated over the
//      frozen taps and held equal): every signal, three time bases; the rates from the table; whose sound from the
//      state; the WFM gate and its one pass of lag; the clamp to the tap's capacity (a sweep longer than the ring); a
//      failed read draws nothing and never turns a negative count into a length; the liveness of each tap
//   B  the radar scope's empty state, the position states and what the scope is told, the rail's block
//   C  the radar scope's knob and POWER key against a radio: the readout, the travel, the grey of the radio's own AGC,
//      a detent's command, THE LOST DETENT (two detents before the engine has shown the first are two detents), the
//      readback catching up or not
//   D  allocations: a settled demod frame and a settled radar frame; and that the members name no engine object
//
// THE ACCESSOR TRAP: a ported member reads the SESSION, so each frame here looks first (the engine's pass takes the
// session's queue, publishes, the window copies the status lines and reads its state), draws, and lets the NEXT look
// take what the draw sent. The radar cabinet uploads its badge texture on first use (a GL call and no context here):
// the test hands the window a texture id it never uploads, and takes it back before the window goes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "gui/demod_scope.hpp"
#include "gui/demod_scope_face.hpp"
#include "gui/fm_mpx_plan.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

// --- allocations on THIS thread (the window and the test share it) -------------------------------------------
namespace {
thread_local long t_allocs = 0;
}
void* operator new(std::size_t n) {
    ++t_allocs;
    if (void* p = std::malloc(n != 0 ? n : 1)) { return p; }
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace cmd = cascade::core::cmd;
using cascade::gui::AppWindow;
using cascade::gui::ScopeSignal;

namespace cascade::gui {

struct AppWindowTestAccess {
    using Sent = std::vector<cmd::QueuedCommand>;
    using Engine = cascade::engine::Engine;

    // ---- the hooks: a radio that is only a name and a gain stage, no scan, no plugins ---------------------------------
    static std::string noPlugins() {
        static const std::string dir = (std::filesystem::temp_directory_path() / "foxsdr_scopewin_no_plugins").string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &noPlugins;
    }

    // ---- one frame's engine part, in drawUi's order -----------------------------------------------------------------
    static void look(AppWindow& a) {
        a.engine_.pumpFrameBegin();
        a.engine_.pumpInputs();
        a.engine_.pumpPlugins();
        (void)a.engine_.pumpPublish();
        a.statusLines_ = a.engine_.statusText();
        a.refreshUiState();
    }
    static void after(AppWindow& a) {
        a.engine_.pumpWorkers();
        a.engine_.pumpAudio();
    }
    static void submit(AppWindow& a, const cmd::QueuedCommand& q) { a.uiSubmit(q); }
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }

    // ---- the demod scope ---------------------------------------------------------------------------------------------
    static void gather(AppWindow& a, DemodScopeFeed& feed) { a.gatherDemodScope(feed); }
    static bool wfm(AppWindow& a) { return a.pipelineIsWfm(); }
    static ScopeSignal signalNow(AppWindow& a) { return a.scopeSignalNow(); }
    static DemodScopeState& scopeSettings(AppWindow& a) { return a.demodScope_; }
    static bool scopeLive(AppWindow& a) { return a.scopeLive_; }
    static void forgetLiveness(AppWindow& a) {
        a.scopeAudioLive_ = ScopeLiveness{};
        a.scopeIqLive_ = ScopeLiveness{};
        a.scopeMpxLive_ = ScopeLiveness{};
    }
    static const ScopeLiveness& audioLiveness(AppWindow& a) { return a.scopeAudioLive_; }
    static const ScopeLiveness& iqLiveness(AppWindow& a) { return a.scopeIqLive_; }
    static const ScopeLiveness& mpxLiveness(AppWindow& a) { return a.scopeMpxLive_; }
    static const std::string& audioFrom(AppWindow& a) { return a.scopeAudioFrom_; }
    static std::size_t iqBufSize(AppWindow& a) { return a.scopeIqBuf_.size(); }
    static bool& demodOpen(AppWindow& a) { return a.demodScopeOpen_; }
    static void drawDemodPage(AppWindow& a) { a.drawDemodScopePage(); }
    // The pipeline's own rings and figures, for the model the feed is held equal to.
    static const cascade::core::ScopeTap<float>& ringAudio(AppWindow& a) { return a.engine_.pipeline_.scopeAudio(); }
    static const cascade::core::ScopeTap<std::complex<float>>& ringIq(AppWindow& a) { return a.engine_.pipeline_.scopeIq(); }
    static const cascade::core::ScopeTap<float>& ringMpx(AppWindow& a) { return a.engine_.pipeline_.scopeMpx(); }
    static double pipelineChannelRate(AppWindow& a) { return a.engine_.pipeline_.channelRateHz(); }
    static bool pipelineWfm(AppWindow& a) { return a.engine_.pipeline_.demodMode() == cascade::dsp::DemodMode::WFM; }
    static void setPlaying(AppWindow& a, const std::string& name) { a.engine_.pluginRunner_.setPlayingForTest(name, 0, 0); }
    static const FoxEngineApi* engineApi(AppWindow& a) { return a.engineApi_; }
    static void setEngineApi(AppWindow& a, const FoxEngineApi* t) { a.engineApi_ = t; }
    static FoxSession* session(AppWindow& a) { return a.uiSession_; }

    // ---- the radar scope -----------------------------------------------------------------------------------------------
    static void drawRadar(AppWindow& a) { a.drawScopeMode(); }
    static void drawRail(AppWindow& a) { a.drawScopeModeControl(); }
    static const ScopeView& scope(AppWindow& a) { return a.scope_; }
    static const ScopeKnobView& knob(AppWindow& a) { return a.scopeKnob_; }
    static const ScopeGainLocal& gainLocal(AppWindow& a) { return a.scopeGainLocal_; }
    static unsigned int& badge(AppWindow& a) { return a.scopeBadgeTex_; }
    static std::uint64_t listSeq(AppWindow& a) { return a.uiState_.listSeq; }
    static const AppWindow::PluginListCache& pluginRows(AppWindow& a) { return a.pluginLists(); }

    // ---- the engine, for what a hermetic window cannot make and for comparison ---------------------------------------------
    static std::vector<float>& gainsDb(AppWindow& a) { return a.engine_.deviceGainsDb_; }
    static float engineGain(AppWindow& a) { return a.engine_.deviceGainsDb_.empty() ? -1.0f : a.engine_.deviceGainsDb_[0]; }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;

// A window with the hooks laid out and its state read.
struct Env {
    AppWindow app;
    Env() {
        Access::look(app);
        Access::look(app);
    }
    // The badge texture is the window's to delete at its end; the test never gave GL one.
    ~Env() { Access::badge(app) = 0u; }
};

// --- frames -----------------------------------------------------------------------------------------------------
const char* const kWin = "scopewin";
std::string g_text;  // what the last captured frame lettered

// One ImGui frame with `body` drawn in a window of the given size (no engine pass), the text it lettered kept.
template <class F>
void imguiFrame(F body, float w = 1500.0f, float h = 900.0f, bool capture = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::Begin(kWin, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    if (capture) { ImGui::LogToBuffer(); }
    body();
    if (capture) {
        g_text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    ImGui::End();
    ImGui::Render();
}

// The engine's pass only (nothing drawn).
void pass(AppWindow& a) {
    Access::look(a);
    Access::after(a);
}
template <class Pred>
bool passUntil(AppWindow& a, Pred pred, int max = 4000) {
    for (int i = 0; i < max; ++i) {
        if (pred()) { return true; }
        pass(a);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
bool rxFlag(AppWindow& a, std::uint32_t bit) { return (Access::state(a).flags & bit) != 0u; }

void send(AppWindow& a, const cmd::QueuedCommand& q) {
    Access::submit(a, q);
    pass(a);
    pass(a);
}
void send(AppWindow& a, const FoxCommand& c) { send(a, cmd::QueuedCommand{c, {}}); }
void setPosition(AppWindow& a, double lat, double lon) { send(a, cmd::makeNum(FOXAPI_OP_SET_POSITION, lat, lon)); }
void setMode(AppWindow& a, std::uint32_t mode) { send(a, cmd::makeInt(FOXAPI_OP_SET_MODE, static_cast<std::int64_t>(mode))); }
void openFakeRadio(AppWindow& a) {
    send(a, cmd::make(FOXAPI_OP_SCAN_DEVICES));
    send(a, cmd::makeText(FOXAPI_OP_SELECT_SOURCE, engine_api_test::kFakeRadioId));
    CHECK(passUntil(a, [&] { return rxFlag(a, FOXAPI_RX_DEVICE_OPEN) && !rxFlag(a, FOXAPI_RX_DEVICE_OPENING); }));
    pass(a);
    pass(a);
}

// ======================================================================================================================
// A. THE DEMOD SCOPE'S FEED
// ======================================================================================================================

constexpr std::size_t kFftAudio = 2048;
constexpr std::size_t kFftMpx = 8192;

struct Model {
    bool ranToTheEnd = false;
    std::vector<float> audio;
    std::vector<float> i, q;
    std::vector<float> spec;
    double binHz = 0.0, span = 0.0, audioRate = 0.0, iqRate = 0.0;
    std::size_t sweep = 0, want = 0;
};

// THE OLD MEMBER, RESTATED over the pipeline's own rings (what gatherDemodScope did before group 12, as the tree at
// 2c8f96e has it): the live atomics and the rings' own snapshot and capacity, with the same pure functions of the face.
// `iqRate` of 0 or less is the pipeline's own channel rate.
Model oldModel(AppWindow& a, ScopeSignal sig, int timebase, int gainIndex, double iqRate = 0.0) {
    namespace g = cascade::gui;
    Model m;
    const bool baseband = g::scopeSignalIsBaseband(sig);
    const bool mpx = sig == ScopeSignal::Mpx;
    m.audioRate = cascade::core::Pipeline::kAudioRateHz;
    m.iqRate = iqRate > 0.0 ? iqRate : Access::pipelineChannelRate(a);
    if (sig == ScopeSignal::Spectrum || mpx) {
        const std::size_t fftSize = mpx ? kFftMpx : kFftAudio;
        const double rateHz = mpx ? m.iqRate : m.audioRate;
        const double spanHz = mpx ? g::mpxSpanHz(rateHz) : g::scopeSpectrumSpanHz(rateHz);
        if (!(rateHz > 0.0) || !(spanHz > 0.0)) { return m; }
        cascade::dsp::ComplexFFT fft(fftSize);
        const std::vector<float> win = cascade::dsp::makeWindow(cascade::dsp::WindowType::Hann, fftSize);
        std::vector<float> buf(fftSize);
        const std::size_t got = mpx ? Access::ringMpx(a).snapshot(buf.data(), fftSize)
                                    : Access::ringAudio(a).snapshot(buf.data(), fftSize);
        if (got < fftSize) { return m; }
        std::vector<std::complex<float>> in(fftSize), out(fftSize);
        for (std::size_t k = 0; k < fftSize; ++k) { in[k] = std::complex<float>(buf[k] * win[k], 0.0f); }
        fft.forward(in.data(), out.data());
        m.binHz = rateHz / static_cast<double>(fftSize);
        const std::size_t bins = g::scopeSpectrumBins(fftSize, rateHz, spanHz);
        m.spec.resize(bins);
        const float norm = 2.0f / (static_cast<float>(fftSize) * cascade::dsp::coherentGain(win.data(), fftSize));
        for (std::size_t k = 0; k < bins; ++k) { m.spec[k] = g::scopeSpectrumDb(std::abs(out[k]) * norm); }
        m.span = (bins > 1) ? static_cast<double>(bins - 1) * m.binHz : spanHz;
        m.ranToTheEnd = true;
        return m;
    }
    const double rate = baseband ? m.iqRate : m.audioRate;
    std::size_t sweep = g::scopeSweepSamples(g::scopeTimebaseMs(timebase), rate);
    if (sweep == 0) { return m; }
    const std::size_t cap = baseband ? Access::ringIq(a).capacity() : Access::ringAudio(a).capacity();
    std::size_t want = sweep * 2;
    if (want > cap) { want = cap; }
    if (sweep > want) { sweep = want; }
    const std::size_t search = want - sweep;
    const float hyst = g::scopeHysteresis(gainIndex);
    const std::size_t holdoff = g::scopeHoldoffSamples(sweep);
    m.sweep = sweep;
    m.want = want;
    if (!baseband) {
        std::vector<float> buf(want);
        const std::size_t got = Access::ringAudio(a).snapshot(buf.data(), want);
        if (got < sweep) { return m; }
        std::size_t start = got - sweep;
        if (search > 0) {
            const std::size_t t = g::scopeFindTrigger(buf.data(), got, sweep, 0.0f, hyst, holdoff);
            if (t != g::kScopeNoTrigger) { start = t; }
        }
        m.audio.assign(buf.begin() + static_cast<std::ptrdiff_t>(start),
                       buf.begin() + static_cast<std::ptrdiff_t>(start + sweep));
        m.ranToTheEnd = true;
        return m;
    }
    std::vector<std::complex<float>> buf(want);
    const std::size_t got = Access::ringIq(a).snapshot(buf.data(), want);
    if (got < sweep) { return m; }
    std::vector<float> I(got), Q(got);
    for (std::size_t k = 0; k < got; ++k) {
        I[k] = buf[k].real();
        Q[k] = buf[k].imag();
    }
    std::size_t start = got - sweep;
    if (search > 0) {
        const std::size_t t = g::scopeFindTrigger(I.data(), got, sweep, 0.0f, hyst, holdoff);
        if (t != g::kScopeNoTrigger) { start = t; }
    }
    m.i.assign(I.begin() + static_cast<std::ptrdiff_t>(start), I.begin() + static_cast<std::ptrdiff_t>(start + sweep));
    m.q.assign(Q.begin() + static_cast<std::ptrdiff_t>(start), Q.begin() + static_cast<std::ptrdiff_t>(start + sweep));
    m.ranToTheEnd = true;
    return m;
}

bool sameFloats(const float* a, const std::vector<float>& b) {
    return !b.empty() && a != nullptr && std::memcmp(a, b.data(), b.size() * sizeof(float)) == 0;
}

std::uint64_t writtenOf(AppWindow& a, std::uint32_t tap) {
    FoxScopeInfo info{};
    info.structSize = sizeof(info);
    (void)Access::engineApi(a)->read_scope(Access::session(a), tap, &info, nullptr, 0);
    return info.written;
}

// The generator running until the taps hold more than a sweep, then stopped and the rings frozen.
bool freezeRings(AppWindow& a, std::uint64_t needAudio, std::uint64_t needIq) {
    send(a, cmd::makeInt(FOXAPI_OP_RUN, 1));
    const bool filled = passUntil(a, [&] {
        return writtenOf(a, FOXAPI_SCOPE_AUDIO) >= needAudio && writtenOf(a, FOXAPI_SCOPE_IQ) >= needIq;
    }, 20000);
    send(a, cmd::makeInt(FOXAPI_OP_RUN, 0));
    std::uint64_t last = ~0ull;
    int stable = 0;
    for (int i = 0; i < 4000 && stable < 30; ++i) {
        pass(a);
        const std::uint64_t now = writtenOf(a, FOXAPI_SCOPE_AUDIO) + writtenOf(a, FOXAPI_SCOPE_IQ);
        stable = now == last ? stable + 1 : 0;
        last = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return filled && stable >= 30;
}

// A table in the window's place whose read_scope can be made to report what the test wants: a rate and a capacity of its
// own for the I/Q tap, a read with frames that fails, every read that fails.
struct Wrap {
    const FoxEngineApi* real = nullptr;
    bool failData = false;
    bool failInfo = false;
    double iqRate = -1.0;       // < 0: the engine's
    long long iqCapacity = -1;  // < 0: the engine's
};
Wrap g_wrap;
std::int32_t FOXAPI_CALL wrappedReadScope(FoxSession* s, std::uint32_t tap, FoxScopeInfo* info, float* out,
                                          std::uint32_t cap) {
    if (g_wrap.failInfo) { return FOXAPI_DETACHED; }
    if (cap != 0u && g_wrap.failData) { return FOXAPI_BAD_ARGUMENT; }
    const std::int32_t n = g_wrap.real->read_scope(s, tap, info, out, cap);
    if (n >= 0 && info != nullptr && tap == FOXAPI_SCOPE_IQ) {
        if (g_wrap.iqRate >= 0.0) { info->rateHz = g_wrap.iqRate; }
        if (g_wrap.iqCapacity >= 0) { info->capacityFrames = static_cast<std::uint32_t>(g_wrap.iqCapacity); }
    }
    return n;
}
struct Wrapped {
    FoxEngineApi table{};
    AppWindow& app;
    const FoxEngineApi* before;
    explicit Wrapped(AppWindow& a) : app(a), before(Access::engineApi(a)) {
        g_wrap = Wrap{};
        g_wrap.real = before;
        table = *before;
        table.read_scope = &wrappedReadScope;
        Access::setEngineApi(app, &table);
    }
    ~Wrapped() { Access::setEngineApi(app, before); }
};

cascade::gui::DemodScopeFeed gatherInFrame(AppWindow& a) {
    cascade::gui::DemodScopeFeed feed;
    imguiFrame([&] { Access::gather(a, feed); });
    return feed;
}

void testFeed() {
    std::printf("  A1: the demod scope's feed is the old member's, over the frozen taps: every signal, three time bases\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    setMode(a, FOXAPI_DEMOD_WFM);
    CHECK(freezeRings(a, 60000, 300000));
    CHECK(writtenOf(a, FOXAPI_SCOPE_MPX) > 8192u);  // the multiplex ring advances while WFM runs: it did, the whole run
    CHECK(Access::wfm(a));
    CHECK(Access::pipelineWfm(a));
    const double chanRate = Access::state(a).channelRateHz;
    std::printf("      channel rate %.0f Hz, audio written %llu, I/Q %llu, multiplex %llu\n", chanRate,
                static_cast<unsigned long long>(writtenOf(a, FOXAPI_SCOPE_AUDIO)),
                static_cast<unsigned long long>(writtenOf(a, FOXAPI_SCOPE_IQ)),
                static_cast<unsigned long long>(writtenOf(a, FOXAPI_SCOPE_MPX)));
    CHECK(chanRate > 0.0);
    const ScopeSignal sigs[] = {ScopeSignal::Audio, ScopeSignal::Spectrum, ScopeSignal::Baseband, ScopeSignal::Vector,
                                ScopeSignal::Mpx};
    int compared = 0;
    for (const int timebase : {0, 3, static_cast<int>(cascade::gui::kScopeTimebaseCount) - 1}) {
        for (const ScopeSignal sig : sigs) {
            Access::scopeSettings(a).signal = static_cast<int>(sig);
            Access::scopeSettings(a).timebase = timebase;
            Access::scopeSettings(a).gainIndex = 5;
            Access::forgetLiveness(a);
            const cascade::gui::DemodScopeFeed feed = gatherInFrame(a);
            const Model m = oldModel(a, sig, timebase, 5);
            const bool baseband = cascade::gui::scopeSignalIsBaseband(sig);
            // The time axis' rates: the audio tap's constant and the channel's, as the table says them.
            CHECK(feed.audioRateHz == 48000.0);
            CHECK(feed.iqRateHz == m.iqRate);
            CHECK(feed.iqRateHz == chanRate);
            CHECK(feed.live);
            CHECK(m.ranToTheEnd);
            if (sig == ScopeSignal::Spectrum || sig == ScopeSignal::Mpx) {
                CHECK_EQ(feed.spectrumBins, m.spec.size());
                CHECK(feed.spectrumBins > 0u);
                CHECK(feed.spectrumBinHz == m.binHz);
                CHECK(feed.spectrumSpanHz == m.span);
                CHECK(sameFloats(feed.spectrumDb, m.spec));
                CHECK(feed.audio == nullptr && feed.iqI == nullptr);
            } else if (!baseband) {
                CHECK_EQ(feed.audioCount, m.audio.size());
                CHECK(feed.audioCount > 0u);
                CHECK(sameFloats(feed.audio, m.audio));
            } else {
                CHECK_EQ(feed.iqCount, m.i.size());
                CHECK(feed.iqCount > 0u);
                CHECK(sameFloats(feed.iqI, m.i));
                CHECK(sameFloats(feed.iqQ, m.q));
            }
            ++compared;
        }
    }
    std::printf("      %d signal and time base pairs, each equal to the old member's\n", compared);

    std::printf("  A2: the three liveness records are fed from every tap whichever signal is selected; a frozen ring goes stale\n");
    {
        Access::scopeSettings(a).signal = static_cast<int>(ScopeSignal::Audio);
        Access::scopeSettings(a).timebase = 3;
        Access::forgetLiveness(a);
        const cascade::gui::DemodScopeFeed first = gatherInFrame(a);
        CHECK(first.live);  // (the first look at a tap that has ever written is live)
        CHECK_EQ(Access::audioLiveness(a).lastSeen, writtenOf(a, FOXAPI_SCOPE_AUDIO));
        CHECK_EQ(Access::iqLiveness(a).lastSeen, writtenOf(a, FOXAPI_SCOPE_IQ));  // the taps nobody is looking at
        CHECK_EQ(Access::mpxLiveness(a).lastSeen, writtenOf(a, FOXAPI_SCOPE_MPX));
        CHECK(Access::audioLiveness(a).started && Access::iqLiveness(a).started && Access::mpxLiveness(a).started);
        // The counters have stopped (the receiver is off): a quarter of a second of frames and it is stale.
        cascade::gui::DemodScopeFeed f{};
        for (int i = 0; i < 12; ++i) { f = gatherInFrame(a); }
        CHECK(f.live);  // 12 frames = 0.2 s: not yet
        for (int i = 0; i < 12; ++i) { f = gatherInFrame(a); }
        CHECK(!f.live);
        CHECK(!Access::scopeLive(a));
        CHECK(f.audio == nullptr && f.iqI == nullptr && f.spectrumDb == nullptr);
    }

    std::printf("  A3: the WFM gate comes from the state (and so is one pass late); the setting is never rewritten\n");
    {
        Access::scopeSettings(a).signal = static_cast<int>(ScopeSignal::Mpx);
        CHECK(Access::wfm(a));
        CHECK(Access::signalNow(a) == ScopeSignal::Mpx);
        // WFM -> AM: the command is sent, and the window's view is the last publish's until the pass has run.
        Access::submit(a, cmd::makeInt(FOXAPI_OP_SET_MODE, FOXAPI_DEMOD_AM));
        CHECK(Access::wfm(a));
        CHECK(Access::signalNow(a) == ScopeSignal::Mpx);
        pass(a);
        CHECK(!Access::wfm(a));
        CHECK(!Access::pipelineWfm(a));
        CHECK(Access::state(a).demodMode == FOXAPI_DEMOD_AM);
        // The multiplex position falls back to the audio spectrum; the saved setting is still the multiplex.
        CHECK(Access::signalNow(a) == ScopeSignal::Spectrum);
        CHECK_EQ(Access::scopeSettings(a).signal, static_cast<int>(ScopeSignal::Mpx));
        // Back to WFM: the scope is where it was left.
        Access::submit(a, cmd::makeInt(FOXAPI_OP_SET_MODE, FOXAPI_DEMOD_WFM));
        CHECK(!Access::wfm(a));
        pass(a);
        CHECK(Access::wfm(a));
        CHECK(Access::signalNow(a) == ScopeSignal::Mpx);
        // Every other mode is not wideband FM.
        for (const std::uint32_t mode : {FOXAPI_DEMOD_NFM, FOXAPI_DEMOD_AM, FOXAPI_DEMOD_DSB, FOXAPI_DEMOD_USB,
                                         FOXAPI_DEMOD_CW, FOXAPI_DEMOD_LSB, FOXAPI_DEMOD_RAW}) {
            setMode(a, mode);
            CHECK(!Access::wfm(a));
            CHECK(Access::wfm(a) == Access::pipelineWfm(a));
        }
        setMode(a, FOXAPI_DEMOD_WFM);
        CHECK(Access::wfm(a) && Access::pipelineWfm(a));
    }

    std::printf("  A4: whose sound comes from the state's audioPlayingPlugin\n");
    {
        Access::scopeSettings(a).signal = static_cast<int>(ScopeSignal::Audio);
        Access::forgetLiveness(a);
        const cascade::gui::DemodScopeFeed none = gatherInFrame(a);
        CHECK(none.audioFrom == nullptr);
        const std::string name = "A Decoder Plugin With A Name Longer Than Any Small String Buffer Holds";
        Access::setPlaying(a, name);
        pass(a);
        CHECK_EQ(std::string(Access::state(a).audioPlayingPlugin), name);
        Access::forgetLiveness(a);
        const cascade::gui::DemodScopeFeed named = gatherInFrame(a);
        CHECK(named.audioFrom != nullptr && std::string(named.audioFrom) == name);
        CHECK_EQ(Access::audioFrom(a), name);
        // (A name past the state's 127 bytes is cut there: the old member drew it whole; no plugin names itself so.)
        const std::string huge(150, 'n');
        Access::setPlaying(a, huge);
        pass(a);
        Access::forgetLiveness(a);
        const cascade::gui::DemodScopeFeed cut = gatherInFrame(a);
        CHECK(cut.audioFrom != nullptr && std::strlen(cut.audioFrom) == FOXAPI_MESSAGE_CHARS - 1);
        Access::setPlaying(a, "");
        pass(a);
        Access::forgetLiveness(a);
        CHECK(gatherInFrame(a).audioFrom == nullptr);
    }

    std::printf("  A5: a sweep longer than the ring is cut to the tap's capacityFrames; without a capacity the request stands\n");
    {
        Access::scopeSettings(a).timebase = static_cast<int>(cascade::gui::kScopeTimebaseCount) - 1;  // 50 ms/DIV
        Access::scopeSettings(a).signal = static_cast<int>(ScopeSignal::Baseband);
        Wrapped w(a);
        // A channel at 700 kHz (the table says so): a sweep of 350000 frames against a ring of 262144.
        g_wrap.iqRate = 700000.0;
        Access::forgetLiveness(a);
        const cascade::gui::DemodScopeFeed f = gatherInFrame(a);
        const Model m = oldModel(a, ScopeSignal::Baseband, Access::scopeSettings(a).timebase, 5, 700000.0);
        std::printf("      700 kHz at 50 ms/DIV: sweep asked %zu, ring %zu: the model draws %zu, the window %zu\n",
                    static_cast<std::size_t>(0.5 * 700000.0), Access::ringIq(a).capacity(), m.i.size(), f.iqCount);
        CHECK(f.iqRateHz == 700000.0);
        CHECK(m.ranToTheEnd);
        CHECK_EQ(m.i.size(), Access::ringIq(a).capacity());  // the sweep is shortened to the ring
        CHECK_EQ(f.iqCount, m.i.size());
        CHECK(sameFloats(f.iqI, m.i) && sameFloats(f.iqQ, m.q));
        CHECK_EQ(Access::iqBufSize(a), Access::ringIq(a).capacity());  // and the buffer is the ring's size, no bigger
        // A table that does not say (capacityFrames 0: an engine that predates it): the window asks for what the sweep
        // needs and takes what the ring has, which is short of a sweep - nothing is drawn, rather than a wrong sweep.
        g_wrap.iqCapacity = 0;
        Access::forgetLiveness(a);
        const cascade::gui::DemodScopeFeed unknown = gatherInFrame(a);
        CHECK(unknown.live);
        CHECK(unknown.iqI == nullptr && unknown.iqCount == 0u);
        // The rate the table does not give (0): the state's own channel rate stands in, and the face is not blanked.
        g_wrap.iqCapacity = -1;
        g_wrap.iqRate = 0.0;
        Access::scopeSettings(a).timebase = 3;
        Access::forgetLiveness(a);
        const cascade::gui::DemodScopeFeed norate = gatherInFrame(a);
        CHECK(norate.iqRateHz == Access::state(a).channelRateHz);
        CHECK(norate.iqRateHz > 0.0 && norate.iqCount > 0u);
    }

    std::printf("  A6: a read that fails draws nothing and is never a length (the count is an int32_t that can be negative)\n");
    {
        Access::scopeSettings(a).timebase = 0;
        for (const ScopeSignal sig : {ScopeSignal::Audio, ScopeSignal::Spectrum, ScopeSignal::Baseband, ScopeSignal::Vector,
                                      ScopeSignal::Mpx}) {
            Access::scopeSettings(a).signal = static_cast<int>(sig);
            Wrapped w(a);
            g_wrap.failData = true;
            Access::forgetLiveness(a);
            const cascade::gui::DemodScopeFeed f = gatherInFrame(a);
            CHECK(f.live);  // the taps are counting (their info reads were fine)
            CHECK(f.audio == nullptr && f.iqI == nullptr && f.iqQ == nullptr && f.spectrumDb == nullptr);
            CHECK(f.audioCount == 0u && f.iqCount == 0u && f.spectrumBins == 0u);
            // Every read failing (the session is gone): not live, nothing, and no crash.
            g_wrap.failInfo = true;
            Access::forgetLiveness(a);
            const cascade::gui::DemodScopeFeed g2 = gatherInFrame(a);
            CHECK(!g2.live && !Access::scopeLive(a));
            CHECK(g2.audio == nullptr && g2.iqI == nullptr && g2.spectrumDb == nullptr);
        }
    }

    std::printf("  A7: the page drawn in real frames, live, over the running generator\n");
    {
        send(a, cmd::makeInt(FOXAPI_OP_RUN, 1));
        Access::demodOpen(a) = true;
        Access::forgetLiveness(a);
        for (const ScopeSignal sig : {ScopeSignal::Audio, ScopeSignal::Spectrum, ScopeSignal::Baseband, ScopeSignal::Vector,
                                      ScopeSignal::Mpx}) {
            Access::scopeSettings(a).signal = static_cast<int>(sig);
            for (int i = 0; i < 12; ++i) {
                Access::look(a);
                imguiFrame([&] { Access::drawDemodPage(a); });
                Access::after(a);
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
            }
            CHECK(Access::scopeLive(a));
        }
        Access::demodOpen(a) = false;
        send(a, cmd::makeInt(FOXAPI_OP_RUN, 0));
    }
}

// ======================================================================================================================
// B. THE RADAR SCOPE: the empty state, the position, the rail's block
// ======================================================================================================================

void radarDraw(AppWindow& a, bool capture = false, float w = 1500.0f, float h = 900.0f) {
    imguiFrame([&] { Access::drawRadar(a); }, w, h, capture);
}
void radarFrame(AppWindow& a, bool capture = false, float w = 1500.0f, float h = 900.0f) {
    Access::look(a);
    radarDraw(a, capture, w, h);
    Access::after(a);
}
std::vector<cmd::QueuedCommand> withoutNotes(const Access::Sent& sent) {
    std::vector<cmd::QueuedCommand> out;
    for (const auto& q : sent) {
        if (q.c.op != FOXAPP_OP_NOTE_PANEL) { out.push_back(q); }
    }
    return out;
}

void testPosition() {
    std::printf("  B1: no position is the honest empty state, from the state's flag, and the scope is told nothing\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    Access::badge(a) = 1u;
    CHECK(!rxFlag(a, FOXAPI_RX_POSITION_SET));
    radarFrame(a, true);
    CHECK(has(g_text, "The scope needs the receiver's position."));
    CHECK(has(g_text, "Everything on a radar scope is a range and a bearing"));
    CHECK(!Access::scope(a).hasReceiver());
    CHECK(!Access::knob(a).drawn);

    std::printf("  B2: a position set: the scope draws one pass after the command and is told the pair, latitude first\n");
    const double lat = 51.5074, lon = -0.1278;
    Access::submit(a, cmd::makeNum(FOXAPI_OP_SET_POSITION, lat, lon));
    radarDraw(a, true);  // (no pass yet: the window's view is the last publish's)
    CHECK(has(g_text, "The scope needs the receiver's position."));
    CHECK(!Access::scope(a).hasReceiver());
    pass(a);
    CHECK(rxFlag(a, FOXAPI_RX_POSITION_SET));
    CHECK_NEAR(Access::state(a).rxLatDeg, lat, 0.0);
    radarDraw(a, true);
    CHECK(!has(g_text, "The scope needs the receiver's position."));
    CHECK(Access::scope(a).hasReceiver());
    CHECK_NEAR(Access::scope(a).receiverLatDeg(), lat, 0.0);
    CHECK_NEAR(Access::scope(a).receiverLonDeg(), lon, 0.0);
    CHECK(Access::knob(a).drawn);  // the cabinet: 1500 px of room

    std::printf("  B3: a position moved from elsewhere reaches the scope at once (it is pushed in every frame)\n");
    setPosition(a, -33.8688, 151.2093);
    radarFrame(a);
    CHECK_NEAR(Access::scope(a).receiverLatDeg(), -33.8688, 0.0);
    CHECK_NEAR(Access::scope(a).receiverLonDeg(), 151.2093, 0.0);

    std::printf("  B4: with no room for the cabinet the bare scope draws (no knob, no gauges), told the same position\n");
    radarFrame(a, false, 700.0f, 600.0f);
    CHECK(!Access::knob(a).drawn);
    CHECK(Access::scope(a).hasReceiver());
    CHECK_NEAR(Access::scope(a).receiverLatDeg(), -33.8688, 0.0);
    radarFrame(a);
    CHECK(Access::knob(a).drawn);

    std::printf("  B5: the rail's block: the offers with no position, the fold with one, and why the scope will be empty\n");
    {
        Env fresh;  // (a fresh window: no position)
        AppWindow& b = fresh.app;
        Access::look(b);
        imguiFrame([&] { Access::drawRail(b); }, 420.0f, 900.0f, true);
        CHECK(has(g_text, "The scope needs the receiver's position first."));
        CHECK(!has(g_text, "Receiver position"));
        CHECK(has(g_text, "The scope will be empty."));                  // no module is a track source: nothing live
        CHECK(has(g_text, "An ADS-B decoder is what fills this face"));  // ...and none of that kind is fitted at all
        CHECK(Access::pluginRows(b).plugins.empty());
        setPosition(b, 48.8566, 2.3522);
        Access::look(b);
        imguiFrame([&] { Access::drawRail(b); }, 420.0f, 900.0f, true);
        CHECK(!has(g_text, "The scope needs the receiver's position first."));
        CHECK(has(g_text, "Receiver position"));
        CHECK(has(g_text, "Radar scope"));
        CHECK(has(g_text, "The scope will be empty."));
    }
}

// ======================================================================================================================
// C. THE KNOB AND THE POWER KEY
// ======================================================================================================================

struct Pointer {
    ImGuiID knobId = 0;
    float x = 0.0f;
    float knobY = 0.0f;
    float powerX = 0.0f, powerY = 0.0f;
    bool found = false;
};

// Where the knob and the POWER key are, in the frame the cabinet draws at this size: the POWER key is the last item the
// scope submits; the knob is found by walking the pointer down the middle of the window.
Pointer locate(AppWindow& a, float w = 1500.0f, float h = 900.0f) {
    Pointer p;
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 powerMin, powerMax;
    imguiFrame([&] {
        Access::drawRadar(a);
        powerMin = ImGui::GetItemRectMin();
        powerMax = ImGui::GetItemRectMax();
    }, w, h);
    p.powerX = (powerMin.x + powerMax.x) * 0.5f;
    p.powerY = (powerMin.y + powerMax.y) * 0.5f;
    ImGuiWindow* win = ImGui::FindWindowByName(kWin);
    p.knobId = win != nullptr ? win->GetID("##scopeknob") : 0u;
    p.x = w * 0.5f;
    for (float y = h * 0.55f; y < h && !p.found; y += 6.0f) {
        io.AddMousePosEvent(p.x, y);
        radarDraw(a, false, w, h);
        radarDraw(a, false, w, h);
        if (GImGui->HoveredId == p.knobId && p.knobId != 0u) {
            p.found = true;
            p.knobY = y;
        }
    }
    io.AddMousePosEvent(2000.0f, 2000.0f);
    radarDraw(a, false, w, h);
    return p;
}

void putAway(AppWindow& a) {
    ImGui::GetIO().AddMousePosEvent(2000.0f, 2000.0f);
    radarDraw(a);
    radarDraw(a);
}
// A detent (or several): the pointer over the knob, the wheel turned, one frame (no engine pass).
void turn(AppWindow& a, const Pointer& p, float wheel) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(p.x, p.knobY);
    radarDraw(a);
    radarDraw(a);
    io.AddMouseWheelEvent(0.0f, wheel);
    radarDraw(a);
}
void clickPower(AppWindow& a, const Pointer& p) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(p.powerX, p.powerY);
    radarDraw(a);
    radarDraw(a);
    io.AddMouseButtonEvent(0, true);
    radarDraw(a);
    io.AddMouseButtonEvent(0, false);
    radarDraw(a);
    radarDraw(a);
}

// The one command of `op` in what the window sent, or null (none, or more than one).
const cmd::QueuedCommand* only(const std::vector<cmd::QueuedCommand>& sent, std::uint32_t op) {
    const cmd::QueuedCommand* hit = nullptr;
    int n = 0;
    for (const auto& q : sent) {
        if (q.c.op == op) {
            hit = &q;
            ++n;
        }
    }
    return n == 1 ? hit : nullptr;
}

void testKnob() {
    std::printf("  C1: with no radio the knob says N/A and no turn of it sends anything\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    Access::badge(a) = 1u;
    setPosition(a, 51.5074, -0.1278);
    radarFrame(a);
    CHECK(Access::knob(a).drawn);
    CHECK(!Access::knob(a).haveGain && !Access::knob(a).live);
    CHECK_EQ(Access::knob(a).readout, std::string("N/A"));
    CHECK_EQ(Access::knob(a).note, std::string("no device"));
    CHECK_EQ(Access::knob(a).lit, -1);
    CHECK(Access::knob(a).fraction == 0.5f);
    CHECK(Access::knob(a).loDb == 0.0f && Access::knob(a).hiDb == 60.0f);  // the old constants, the fallback travel
    const Pointer p = locate(a);
    CHECK(p.found);
    CHECK(p.knobId != 0u);
    Access::Sent log;
    Access::tap(a, &log);
    turn(a, p, 1.0f);
    turn(a, p, -1.0f);
    putAway(a);
    CHECK(withoutNotes(log).empty());

    std::printf("  C2: with a radio open the knob lettered is the first gain stage: its travel, readout, lit tick and pointer\n");
    openFakeRadio(a);
    CHECK(rxFlag(a, FOXAPI_RX_DEVICE_OPEN));
    radarFrame(a);
    const cascade::gui::ScopeKnobView& k = Access::knob(a);
    const float readback = Access::engineGain(a);
    std::printf("      the radio reads %.1f dB; the knob: %s, note %s, lit %d, fraction %.4f, travel %.1f..%.1f\n",
                static_cast<double>(readback), k.readout.c_str(), k.note.c_str(), k.lit, static_cast<double>(k.fraction),
                static_cast<double>(k.loDb), static_cast<double>(k.hiDb));
    CHECK(k.haveGain && k.live);
    CHECK_NEAR(readback, 29.7, 0.01);
    CHECK(k.loDb == 0.0f && k.hiDb == 49.6f);  // the stage's own range
    CHECK_EQ(k.readout, std::string("30 dB"));  // %.0f of 29.7
    CHECK_EQ(k.note, std::string("TUNER"));
    CHECK_EQ(k.lit, 2);  // 29.7 / 49.6 * 4 = 2.4 -> the third tick
    CHECK_NEAR(k.fraction, 29.7 / 49.6, 1e-5);
    CHECK_EQ(k.ticks[0], 0);
    CHECK_EQ(k.ticks[4], 49);
    CHECK_NEAR(k.shownDb, 29.7, 1e-4);

    std::printf("  C3: a detent sends the stage's name and the figure two units on; the readback comes a pass later\n");
    {
        const Pointer q = locate(a);
        CHECK(q.found);
        log.clear();
        turn(a, q, 1.0f);
        std::vector<cmd::QueuedCommand> sent = withoutNotes(log);
        CHECK_EQ(sent.size(), 1u);
        const cmd::QueuedCommand* c = only(sent, FOXAPP_OP_SET_GAIN_NO_READBACK);
        CHECK(c != nullptr);
        if (c != nullptr) {
            CHECK_EQ(cmd::textOf(c->c, c->longText), std::string("TUNER"));
            CHECK_NEAR(c->c.num[0], 31.7, 1e-4);
        }
        // Nothing has landed in the click's frame: the radio is where it was.
        CHECK_NEAR(Access::engineGain(a), 29.7, 0.01);
        putAway(a);
        pass(a);
        pass(a);
        CHECK_NEAR(Access::engineGain(a), 31.7, 0.01);
        radarFrame(a);
        CHECK_EQ(Access::knob(a).readout, std::string("32 dB"));
        CHECK_NEAR(Access::knob(a).shownDb, 31.7, 1e-4);
        // The engine has shown the figure the knob sent: the knob lets go of its own copy.
        CHECK(!Access::gainLocal(a).valid);

        log.clear();
        turn(a, q, -1.0f);
        sent = withoutNotes(log);
        const cmd::QueuedCommand* d = only(sent, FOXAPP_OP_SET_GAIN_NO_READBACK);
        CHECK(d != nullptr);
        if (d != nullptr) { CHECK_NEAR(d->c.num[0], 29.7, 1e-4); }
        putAway(a);
        pass(a);
        pass(a);
        CHECK_NEAR(Access::engineGain(a), 29.7, 0.01);
    }

    std::printf("  C4: THE LOST DETENT: two detents in two frames before the engine has published the first are two detents\n");
    {
        const Pointer q = locate(a);
        putAway(a);
        radarFrame(a);  // (settled: the readback is shown, no copy kept)
        CHECK(!Access::gainLocal(a).valid);
        const float base = Access::knob(a).shownDb;
        log.clear();
        turn(a, q, 1.0f);  // frame N: a detent
        turn(a, q, 1.0f);  // frame N+1: another, with no engine pass between (the first has not landed)
        const std::vector<cmd::QueuedCommand> sent = withoutNotes(log);
        std::printf("      base %.1f, sent:", static_cast<double>(base));
        for (const auto& s : sent) { std::printf(" %.1f", s.c.num[0]); }
        std::printf("\n");
        CHECK_EQ(sent.size(), 2u);
        if (sent.size() == 2u) {
            CHECK_NEAR(sent[0].c.num[0], static_cast<double>(base) + 2.0, 1e-4);
            CHECK_NEAR(sent[1].c.num[0], static_cast<double>(base) + 4.0, 1e-4);  // NOT base + 2 again
        }
        CHECK(Access::gainLocal(a).valid);
        // The knob shows the figure it has sent, not the old readback, while the engine has not caught up.
        radarDraw(a);
        CHECK_NEAR(Access::knob(a).shownDb, static_cast<double>(base) + 4.0, 1e-4);
        // A third, still before any pass, from the second's figure.
        log.clear();
        turn(a, q, 1.0f);
        const std::vector<cmd::QueuedCommand> third = withoutNotes(log);
        CHECK_EQ(third.size(), 1u);
        if (third.size() == 1u) { CHECK_NEAR(third[0].c.num[0], static_cast<double>(base) + 6.0, 1e-4); }
        // The engine applies all three, in order: the readback is the last, and the knob lets go.
        putAway(a);
        pass(a);
        pass(a);
        CHECK_NEAR(Access::engineGain(a), static_cast<double>(base) + 6.0, 0.01);
        radarFrame(a);
        CHECK(!Access::gainLocal(a).valid);
        CHECK_NEAR(Access::knob(a).shownDb, static_cast<double>(base) + 6.0, 1e-3);
        // Back down to where it began, for what follows.
        for (int i = 0; i < 3; ++i) {
            turn(a, q, -1.0f);
            radarFrame(a);
        }
        putAway(a);
        pass(a);
        pass(a);
        CHECK_NEAR(Access::engineGain(a), static_cast<double>(base), 0.01);
    }

    std::printf("  C5: a readback that never comes to equal the figure sent (the radio took something else) is the truth after the hold\n");
    {
        const Pointer q = locate(a);
        putAway(a);
        radarFrame(a);
        const float base = Access::knob(a).shownDb;
        log.clear();
        turn(a, q, -1.0f);
        CHECK_EQ(withoutNotes(log).size(), 1u);
        CHECK(Access::gainLocal(a).valid);
        // The engine takes it, then the radio is told something else (quantised, or set from elsewhere): 0.3 dB off.
        pass(a);
        pass(a);
        Access::gainsDb(a)[0] = base - 1.7f;
        pass(a);
        // Held for the hold's length (half a second of frames), then the readback shows.
        for (int i = 0; i < 20; ++i) { radarDraw(a); }
        CHECK(Access::gainLocal(a).valid);
        CHECK_NEAR(Access::knob(a).shownDb, static_cast<double>(base) - 2.0, 1e-3);
        for (int i = 0; i < 20; ++i) { radarDraw(a); }
        CHECK(!Access::gainLocal(a).valid);
        CHECK_NEAR(Access::knob(a).shownDb, static_cast<double>(base) - 1.7, 1e-3);
        putAway(a);
        Access::gainsDb(a)[0] = 29.7f;
        pass(a);
        pass(a);
    }

    std::printf("  C6: the travel's ends: a detent at the end sends nothing\n");
    {
        const Pointer q = locate(a);
        Access::gainsDb(a)[0] = 49.6f;
        pass(a);
        pass(a);
        putAway(a);
        radarFrame(a);
        log.clear();
        turn(a, q, 1.0f);
        CHECK(withoutNotes(log).empty());
        Access::gainsDb(a)[0] = 0.0f;
        pass(a);
        pass(a);
        putAway(a);
        radarFrame(a);
        log.clear();
        turn(a, q, -1.0f);
        CHECK(withoutNotes(log).empty());
        Access::gainsDb(a)[0] = 29.7f;
        pass(a);
        pass(a);
        putAway(a);
    }

    std::printf("  C7: the radio's own AGC kills the knob: it reads AUTO, lights nothing and sends nothing\n");
    {
        send(a, cmd::makeInt(FOXAPI_OP_SET_DEVICE_AGC, 1));
        CHECK(rxFlag(a, FOXAPI_RX_DEVICE_AGC));
        const Pointer q = locate(a);
        putAway(a);
        radarFrame(a);
        const cascade::gui::ScopeKnobView& v = Access::knob(a);
        CHECK(v.haveGain && !v.live);
        CHECK_EQ(v.readout, std::string("AUTO"));
        CHECK_EQ(v.note, std::string("auto gain is on"));
        CHECK_EQ(v.lit, -1);
        log.clear();
        turn(a, q, 1.0f);
        turn(a, q, -1.0f);
        CHECK(withoutNotes(log).empty());
        putAway(a);
        send(a, cmd::makeInt(FOXAPI_OP_SET_DEVICE_AGC, 0));
        CHECK(!rxFlag(a, FOXAPI_RX_DEVICE_AGC));
        radarFrame(a);
        CHECK(Access::knob(a).live);
        CHECK_EQ(Access::knob(a).readout, std::string("30 dB"));
    }

    std::printf("  C8: POWER sends RUN with the other state than the lamp shows\n");
    {
        const Pointer q = locate(a);
        CHECK(q.powerX > 0.0f && q.powerY > 0.0f);
        // Stopped: START.
        CHECK(!rxFlag(a, FOXAPI_RX_RUNNING));
        log.clear();
        clickPower(a, q);
        std::vector<cmd::QueuedCommand> sent = withoutNotes(log);
        const cmd::QueuedCommand* on = only(sent, FOXAPI_OP_RUN);
        CHECK(on != nullptr);
        if (on != nullptr) { CHECK_EQ(on->c.ival[0], 1); }
        CHECK_EQ(sent.size(), 1u);
        // Nothing has landed in the click's frame; the lamp follows the state a pass later.
        CHECK(!rxFlag(a, FOXAPI_RX_RUNNING));
        pass(a);
        pass(a);
        CHECK(passUntil(a, [&] { return rxFlag(a, FOXAPI_RX_RUNNING); }));
        // Running: STOP.
        log.clear();
        clickPower(a, q);
        sent = withoutNotes(log);
        const cmd::QueuedCommand* off = only(sent, FOXAPI_OP_RUN);
        CHECK(off != nullptr);
        if (off != nullptr) { CHECK_EQ(off->c.ival[0], 0); }
        CHECK_EQ(sent.size(), 1u);
        pass(a);
        pass(a);
        CHECK(passUntil(a, [&] { return !rxFlag(a, FOXAPI_RX_RUNNING); }));
    }

    std::printf("  C9: the radio going away takes the knob's copy with it: nothing is inherited by another source\n");
    {
        const Pointer q = locate(a);
        log.clear();
        turn(a, q, 1.0f);
        CHECK(Access::gainLocal(a).valid);
        putAway(a);
        send(a, cmd::makeText(FOXAPI_OP_SELECT_SOURCE, "siggen"));
        CHECK(passUntil(a, [&] { return !rxFlag(a, FOXAPI_RX_DEVICE_OPEN); }));
        radarFrame(a);
        CHECK(!Access::gainLocal(a).valid);
        CHECK(!Access::knob(a).haveGain);
        CHECK_EQ(Access::knob(a).readout, std::string("N/A"));
    }
}

// ======================================================================================================================
// D. ALLOCATIONS, AND THE MEMBERS' SOURCE
// ======================================================================================================================

std::string bodyOf(const std::string& text, const std::string& head) {
    const std::size_t at = text.find(head);
    if (at == std::string::npos) { return {}; }
    const std::size_t end = text.find("\n}\n", at);
    return end == std::string::npos ? text.substr(at) : text.substr(at, end - at + 3);
}
// The same without its // comments (the members' prose names the engine's objects to say what they no longer do).
std::string withoutComments(const std::string& body) {
    std::istringstream in(body);
    std::string line, out;
    while (std::getline(in, line)) {
        const std::size_t c = line.find("//");
        out += (c == std::string::npos ? line : line.substr(0, c)) + "\n";
    }
    return out;
}

void testAllocationsAndSource() {
    std::printf("  D1: a settled demod frame allocates nothing (the old one built a string from the runner's lock every frame)\n");
    Access::install();
    {
        Env env;
        AppWindow& a = env.app;
        setMode(a, FOXAPI_DEMOD_WFM);
        Access::setPlaying(a, "A Decoder Plugin With A Name Longer Than Any Small String Buffer Holds");
        send(a, cmd::makeInt(FOXAPI_OP_RUN, 1));
        CHECK(passUntil(a, [&] { return writtenOf(a, FOXAPI_SCOPE_IQ) > 20000u; }, 20000));
        for (const ScopeSignal sig : {ScopeSignal::Audio, ScopeSignal::Spectrum, ScopeSignal::Baseband, ScopeSignal::Vector,
                                      ScopeSignal::Mpx}) {
            Access::scopeSettings(a).signal = static_cast<int>(sig);
            Access::scopeSettings(a).timebase = 3;
            Access::forgetLiveness(a);
            long measured = 0;
            int live = 0;
            for (int frame = 0; frame < 70; ++frame) {
                Access::look(a);
                cascade::gui::DemodScopeFeed feed;
                imguiFrame([&] {
                    const long before = t_allocs;
                    Access::gather(a, feed);
                    if (frame >= 10) { measured += t_allocs - before; }
                });
                live += feed.live ? 1 : 0;
                Access::after(a);
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
            std::printf("      signal %d: 60 settled frames, %ld heap allocations (live in %d of 70)\n", static_cast<int>(sig),
                        measured, live);
            CHECK(live >= 60);
            CHECK_EQ(measured, 0L);
        }
        send(a, cmd::makeInt(FOXAPI_OP_RUN, 0));
    }

    std::printf("  D2: a settled radar frame (the cabinet, a radio, a position)\n");
    {
        Env env;
        AppWindow& a = env.app;
        Access::badge(a) = 1u;
        setPosition(a, 51.5074, -0.1278);
        openFakeRadio(a);
        for (int i = 0; i < 20; ++i) { radarFrame(a); }
        long measured = 0;
        for (int i = 0; i < 60; ++i) {
            Access::look(a);
            const long before = t_allocs;
            radarDraw(a);
            measured += t_allocs - before;
            Access::after(a);
        }
        std::printf("      60 settled frames: %ld heap allocations (%.2f a frame)\n", measured, static_cast<double>(measured) / 60.0);
        CHECK(Access::knob(a).drawn);
        CHECK_EQ(measured, 0L);
    }

    std::printf("  D3: the members' source names no engine object (the guard's rule, restated; the runner's lock is not reachable)\n");
    {
        const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
        std::ifstream in(root / "src" / "gui" / "app_window.cpp", std::ios::binary);
        CHECK(in.good());
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        for (const char* head : {"void AppWindow::drawScopeMode() {", "void AppWindow::drawScopeModeControl() {",
                                 "void AppWindow::gatherDemodScope(", "bool AppWindow::pipelineIsWfm() const"}) {
            const std::string body = withoutComments(bodyOf(text, head));
            CHECK(!body.empty());
            CHECK(body.find("engine_.") == std::string::npos);
            CHECK(body.find("pluginRunner_") == std::string::npos);
            CHECK(body.find("playingPlugin") == std::string::npos);
            CHECK(body.find("pipeline_") == std::string::npos);
        }
        // The POWER press stays in drawScopeMode's own text (tests/test_stop_ends_recordings.cpp reads it there).
        CHECK(bodyOf(text, "void AppWindow::drawScopeMode() {").find("FOXAPI_OP_RUN") != std::string::npos);
        // The one accessor the radar scope keeps is only that: the window's own plugin UI's tracks (it named the engine's before the
        // pump move's step P3, which made the plugin UI the window's) - and names the engine nowhere.
        const std::string tracks = bodyOf(text, "const std::vector<cascade::core::HostTrack>& AppWindow::hostTracks()");
        CHECK(tracks.find("pluginUi_.tracks()") != std::string::npos);
        CHECK(tracks.find("engine_.") == std::string::npos);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_scope_window\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_scopewin_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testFeed();
    testPosition();
    testKnob();
    testAllocationsAndSource();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_scope_window");
}
