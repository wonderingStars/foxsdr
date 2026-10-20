// test_source_section_window.cpp - THE SOURCE SECTION AND THE SOUND CARD READ THEIR ENGINE API SESSION (window port,
// group 11: drawSourceSection, radioNotOpenLit, drawSoundCardControls, soundCardReceivesText and sourceLists;
// engine-stage4.md 6.5, API 0.3 section 8.10).
//
// What the section draws is the table's DEVICES (an entry's index is its row in the combo), fact.source.* (the row, the
// kept source, the radio being opened, the scan's gate and what it skips, the drivers that faulted, the Pluto's address,
// and the WHOLE id of every row), the sourceFlags word (the scan's states, the network box, UHD, the SDRplay rows, the
// sound card listing and opening), UNBOUND_RADIOS, SCAN_EVIDENCE, the open radio's SAMPLE_RATES (with their captions),
// ANTENNAS, GAINS and DEVICE_OPTIONS, the status line deviceFault (the WHOLE sentence) and, for the sound card,
// SOUND_CARDS, SOUND_CARD_RATES, SOUND_CARD_FORM and fact.soundcard.*; what it changes it changes by commands on its
// session. This holds the window's side of that against the real engine, its radios, its scans and its sound cards
// stood in for (Engine::testHooks_): NOTHING here lists, probes or opens anything on this desk, and the three real
// calls into the SoapySDR library that the window used to make every frame are never made - the engine's evidence
// hook counts every call that would have crossed.
//
//   A  the combo: its rows (the generator, the IQ file, the sound card, the radios with "(native)" composed), the
//      scan its opening asks for (once, on the edge), a click opening the radio by the WHOLE id of its row (a Soapy id
//      and a Pluto id longer than the table's 159-byte detail), the patch greying the radios
//   B  the scan's states drawn: pending, partial, gated (both sentences), the network box and the UHD note
//   C  "No radio hardware found" from SCAN_EVIDENCE with no call into the runtime, the drivers that faulted, one
//      sentence for each dongle that cannot be opened (three families) and the SDRplay note
//   D  the fault card: the WHOLE sentence, the reconnect advice not added under "restart FoxSDR", the lamp and the
//      saved-not-open preview
//   E  the open radio's panel: rate, antenna, gains, auto gain, bias tee, the RSP and RX888 switches
//   F  the Pluto's box and the IQ file's: the draft, Open
//   G  the sound card: the list, the pick (and the click on the card that is already the form's), the rate rule, the
//      form's field mask, the centre draft, Open, the empty list, a card opening, "Receives" and the converter
//   H  a widget sweep of every state: exactly the commands each control sends
//   I  a click shows nothing in its frame and the engine's value the frame after the pass
//   J  the cache: one read when listSeq moves, none otherwise, and the allocations of a settled frame
//
// THE TEST-ACCESSOR TRAP: a ported member reads the SESSION, so every frame here looks first, as drawUi does (the
// engine's pass takes the session's queue, publishes, the window copies the status lines and reads its state), draws,
// and lets the NEXT look take what the draw sent; the workers are polled after the draw, as drawUi polls them. What the
// window drew is read back through Dear ImGui's own text log (ImGui::LogToBuffer), which every widget that letters
// text writes to.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
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
#include "core/freq_converter.hpp"
#include "core/i18n.hpp"
#include "engine/engine.hpp"
#include "engine/soundcard_panel.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "source/soundcard_source.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

// --- allocations on THIS thread (the window and the test share it) ------------
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
using cascade::source::SoundCardDevice;
using cascade::source::SoundCardFormat;
using cascade::source::SoundCardSettings;

// --- the world: the radios, the scans, the runtime's diagnostics and the sound cards, all stood in for ----------------
namespace world {

struct World {
    std::vector<cascade::source::NativeDeviceInfo> natives;
    std::vector<cascade::usb::UsbDeviceInfo> unbound;
    std::string sdrAdvice, sdrDetail;
    bool sdrRows = false;
    std::vector<cascade::source::SoapyDeviceInfo> soapy;
    std::atomic<bool> soapyGate{true};
    std::atomic<int> soapyCalls{0};
    std::vector<std::string> modules, searchPaths, faulted;
    std::vector<cascade::source::VendorRoot> vendors;
    std::atomic<int> diagCalls{0};
    std::vector<SoundCardDevice> cards;
    std::atomic<bool> listGate{true}, openGate{true};
    std::atomic<int> listCalls{0}, openCalls{0};
};

inline World& get() {
    static World w;
    return w;
}

// The long texts of the ids: the Pluto's address (an address box takes 191 bytes) and a SoapySDR row's kwargs.
inline std::string plutoUriLong() { return "ip:" + std::string(186, 'p'); }  // 189 bytes
inline std::string soapyLongArgs() { return "driver=uhd,addr=192.168.10.2,type=" + std::string(262, 'x'); }  // 296 bytes

inline void waitOn(const std::atomic<bool>& gate) {
    for (int i = 0; i < 10000 && !gate.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}

class Card final : public cascade::source::SoundCardBackend {
public:
    std::vector<SoundCardDevice> listDevices() override {
        ++get().listCalls;
        waitOn(get().listGate);
        return get().cards;
    }
    bool open(const SoundCardDevice&, int, double, bool, PushFn, void*, std::string&) override {
        ++get().openCalls;
        waitOn(get().openGate);
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    bool alive() override { return open_.load(); }

private:
    std::atomic<bool> open_{false};
};

inline SoundCardDevice card(int index, const char* name, double defaultRate, std::vector<cascade::source::SoundCardRate> rates,
                            bool isDefault = false) {
    SoundCardDevice d;
    d.index = index;
    d.name = name;
    d.hostApi = "Fake API";
    d.maxInputChannels = 2;
    d.defaultRateHz = defaultRate;
    d.isDefault = isDefault;
    d.rates = std::move(rates);
    return d;
}

inline void reset() {
    World& w = get();
    w.natives = {{engine_api_test::kFakeRadioDriver, "Mock RTL-SDR (serial 00000001)", engine_api_test::kFakeRadioArgs},
                 {"hackrf", "HackRF One", "serial=0000000000000000457863c8"},
                 {"pluto", "ADALM-Pluto (network)", "uri=ip:192.168.2.1"}};
    w.unbound.clear();
    w.sdrAdvice.clear();
    w.sdrDetail.clear();
    w.sdrRows = false;
    w.soapy = {{"Fake Soapy A", "driver=fake,serial=A"}, {"Long Soapy", soapyLongArgs()}};
    w.soapyGate = true;
    w.soapyCalls = 0;
    w.modules.clear();
    w.searchPaths.clear();
    w.faulted.clear();
    w.vendors.clear();
    w.diagCalls = 0;
    // A: rates 44.1, 48 (its default) and a 96 that only exclusive mode reaches. B: 96 and 192 (its default). C: a card
    // whose default is not one of its rates, so a pick that must re-choose lands on its highest.
    w.cards = {card(0, "Fake In A", 48000.0, {{44100.0, false}, {48000.0, false}, {96000.0, true}}, true),
               card(1, "Fake In B", 192000.0, {{96000.0, false}, {192000.0, false}}),
               card(2, "Fake In C", 50000.0, {{32000.0, false}, {64000.0, false}})};
    w.listGate = true;
    w.openGate = true;
    w.listCalls = 0;
    w.openCalls = 0;
}

}  // namespace world

namespace cascade::gui {

struct AppWindowTestAccess {
    using Sent = std::vector<cmd::QueuedCommand>;
    using Answer = std::pair<std::uint32_t, std::int32_t>;  // {op, status}
    using Engine = cascade::engine::Engine;
    using Cache = AppWindow::SourceListCache;

    // ---- the hooks ------------------------------------------------------------------------------------------------
    static void walk(Engine::NativeScanResult& out) {
        const world::World& w = world::get();
        out.devices = w.natives;
        out.unbound = w.unbound;
        out.sdrPlayAdvice = w.sdrAdvice;
        out.sdrPlayApiDetail = w.sdrDetail;
        out.sdrPlayRowsFound = w.sdrRows;
    }
    static std::vector<cascade::source::SoapyDeviceInfo> soapyScan() {
        world::World& w = world::get();
        ++w.soapyCalls;
        world::waitOn(w.soapyGate);
        return w.soapy;
    }
    static Engine::ScanDiagnostics diagnostics() {
        world::World& w = world::get();
        ++w.diagCalls;
        Engine::ScanDiagnostics d;
        d.modules = w.modules;
        d.searchPaths = w.searchPaths;
        d.vendors = w.vendors;
        d.faultedDrivers = w.faulted;
        return d;
    }
    static std::shared_ptr<cascade::source::SoundCardBackend> cardBackend() { return std::make_shared<world::Card>(); }
    static std::string noPlugins() {
        static const std::string dir = (std::filesystem::temp_directory_path() / "foxsdr_srcwin_no_plugins").string();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }
    // `withDiagnostics` false leaves the runtime's diagnostics unhooked: a hermetic engine must then read none.
    static void install(bool withDiagnostics = true) {
        world::reset();
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = nullptr;
        Engine::testHooks_.nativeScanDetail = &walk;
        Engine::testHooks_.soapyScan = &soapyScan;
        Engine::testHooks_.scanDiagnostics = withDiagnostics ? &diagnostics : nullptr;
        Engine::testHooks_.soundCardBackend = &cardBackend;
        Engine::testHooks_.pluginDir = &noPlugins;
    }

    static void makeDevice(std::unique_ptr<cascade::source::DeviceSource> (*fn)(const std::string&)) { Engine::testHooks_.makeDevice = fn; }

    // ---- one frame's engine part, in drawUi's order ----------------------------------------------------------------
    static void look(AppWindow& a) {
        passseam::pass(a.engine_, [&] {
            a.engine_.pumpFrameBegin();
            a.engine_.pumpInputs();
            a.engine_.pumpPlugins();
            (void)a.engine_.pumpPublish();
        });
        a.statusLines_ = a.engine_.statusText();
        a.refreshUiState();
    }
    static void after(AppWindow& a) {
        passseam::workers(a.engine_, [&] {
            a.engine_.pumpWorkers();
            a.engine_.pumpAudio();
        });
    }
    static std::vector<Answer> answers(AppWindow& a) {
        std::vector<Answer> out;
        for (;;) {
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = a.engineApi_->poll_results(a.uiSession_, r, 16);
            if (got <= 0) { break; }
            for (int32_t i = 0; i < got; ++i) { out.push_back({r[i].op, r[i].status}); }
            if (got < 16) { break; }
        }
        return out;
    }
    static void submit(AppWindow& a, const cmd::QueuedCommand& q) { a.uiSubmit(q); }
    static void submit(AppWindow& a, const FoxCommand& c) { a.uiSubmit(c); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    static void dropQueued(AppWindow& a) {
        a.closeUiSession();
        a.openUiSession();
        a.refreshUiState();
    }
    static FoxCommandResult command(AppWindow& a, const cmd::QueuedCommand& q) { return a.engine_.applyCommand(q.c, q.longText); }
    static const FoxReceiverState& state(AppWindow& a) { return a.uiState_; }

    // ---- the window's own ------------------------------------------------------------------------------------------
    static const Cache& lists(AppWindow& a) { return a.sourceLists(); }
    static Cache& cache(AppWindow& a) { return a.sourceCache_; }
    static void drawSource(AppWindow& a) { a.drawSourceSection(); }
    static bool lamp(AppWindow& a) { return a.radioNotOpenLit(); }
    static std::string receives(AppWindow& a) { return a.soundCardReceivesText(); }
    static char* plutoDraft(AppWindow& a) { return a.plutoUriDraft_; }
    static bool plutoPending(AppWindow& a) { return a.plutoUriPending_; }
    static double& centreMhz(AppWindow& a) { return a.soundCardCentreMhz_; }
    static bool centrePending(AppWindow& a) { return a.soundCardCentrePending_; }
    static bool comboWasOpen(AppWindow& a) { return a.sourceComboWasOpen_; }
    static char* iqPath(AppWindow& a) { return a.iqPath_; }
    static const cascade::engine::EngineStatusText& statusLines(AppWindow& a) { return a.statusLines_; }
    static std::uint64_t listSeq(AppWindow& a) { return a.uiState_.listSeq; }
    static std::string fact(AppWindow& a, const char* key) {
        std::string s;
        a.readFactText(key, s);
        return s;
    }
    static std::int32_t listCount(AppWindow& a, std::uint32_t list) { return a.engineApi_->read_list(a.uiSession_, list, 0, nullptr); }
    static bool listItem(AppWindow& a, std::uint32_t list, std::uint32_t i, FoxListItem& it) {
        it = FoxListItem{};
        it.structSize = sizeof(it);
        return a.engineApi_->read_list(a.uiSession_, list, i, &it) > static_cast<std::int32_t>(i);
    }

    // ---- the engine, for what a hermetic window cannot make and for comparison ----------------------------------------
    static int& sourceSel(AppWindow& a) { return a.engine_.sourceSel_; }
    static bool& patchRunning(AppWindow& a) { return a.engine_.patchRunning_; }
    static std::string& testFault(AppWindow& a) { return a.engine_.testFault_; }
    static SoundCardSettings& form(AppWindow& a) { return a.engine_.soundCard_; }
    static const SoundCardSettings& liveCard(AppWindow& a) { return a.engine_.soundCardLive_; }
    static std::string& cardMissing(AppWindow& a) { return a.engine_.soundCardMissing_; }
    static std::string& keepKind(AppWindow& a) { return a.engine_.restoreKeep_.kind; }
    static std::string& keepLabel(AppWindow& a) { return a.engine_.restoreKeepLabel_; }
    static bool& cardOpening(AppWindow& a) { return a.engine_.soundCardOpenPending_; }
    static std::string& sourceError(AppWindow& a) { return a.engine_.sourceError_; }
    static std::string& sdrAdvice(AppWindow& a) { return a.engine_.sdrPlayAdvice_; }
    static std::string& tuneNote(AppWindow& a) { return a.engine_.tuneMismatchNote_; }
    static const std::string& sourceKind(AppWindow& a) { return a.engine_.sourceKind_; }
    static bool deviceOpen(AppWindow& a) { return a.engine_.device_ != nullptr; }
    static bool& agcSupported(AppWindow& a) { return a.engine_.deviceAgcSupported_; }
    static bool& agcOn(AppWindow& a) { return a.engine_.deviceAgc_; }
    static std::vector<std::string>& antennas(AppWindow& a) { return a.engine_.deviceAntennas_; }
    static std::string& antenna(AppWindow& a) { return a.engine_.deviceAntenna_; }
    static std::vector<cascade::source::GainInfo>& gainRanges(AppWindow& a) { return a.engine_.deviceGainRanges_; }
    static std::vector<std::string>& gainNames(AppWindow& a) { return a.engine_.deviceGainNames_; }
    static std::vector<float>& gainsDb(AppWindow& a) { return a.engine_.deviceGainsDb_; }
    static cascade::gui::BiasTeePanel& biasTee(AppWindow& a) { return a.engine_.biasTeePanel_; }
    static bool& rfNotchPresent(AppWindow& a) { return a.engine_.deviceRfNotchPresent_; }
    static bool& rfNotch(AppWindow& a) { return a.engine_.deviceRfNotch_; }
    static bool& dabNotchPresent(AppWindow& a) { return a.engine_.deviceDabNotchPresent_; }
    static bool& dabNotch(AppWindow& a) { return a.engine_.deviceDabNotch_; }
    static bool& hdrPresent(AppWindow& a) { return a.engine_.deviceHdrPresent_; }
    static bool& hdr(AppWindow& a) { return a.engine_.deviceHdr_; }
    static bool& adcPresent(AppWindow& a) { return a.engine_.deviceAdcSwitchesPresent_; }
    static bool& dither(AppWindow& a) { return a.engine_.deviceDither_; }
    static bool& randomiser(AppWindow& a) { return a.engine_.deviceRandomiser_; }
    static double engineRateNow(AppWindow& a) { return a.engine_.pipeline_.activeSource().sampleRateHz(); }
    static std::map<std::string, cascade::core::ConverterSetting>& converters(AppWindow& a) { return a.engine_.converters_; }
    static int soapyRowBase(AppWindow& a) { return a.engine_.soapyRowBase(); }
    static std::string idForRow(AppWindow& a, int row) { return a.engine_.sourceIdForRow(row); }
    static char* plutoBuffer(AppWindow& a) { return a.engine_.plutoUri_; }
    static bool gated(AppWindow& a) { return a.engine_.soapyScanGated(); }
    static std::vector<std::string>& absent(AppWindow& a) { return a.engine_.soapyAbsentDrivers_; }
    static bool& scanned(AppWindow& a) { return a.engine_.soapyScanned_; }
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
};

// --- frames --------------------------------------------------------------------------------------------------
const char* const kWin = "srcwin";
std::string g_text;  // what the last captured frame lettered

void imguiFrame(AppWindow& a, bool capture = false) {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(420.0f, 3200.0f), ImGuiCond_Always);
    ImGui::Begin(kWin, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
    if (capture) { ImGui::LogToBuffer(); }
    Access::drawSource(a);
    if (capture) {
        g_text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    ImGui::End();
    ImGui::Render();
}
// The engine's pass only (the lists are published; the window reads nothing).
void pass(AppWindow& a) {
    Access::look(a);
    Access::after(a);
}
// A frame of the application as far as this window goes: the engine's part, the window drawn, the workers polled.
void frame(AppWindow& a) {
    Access::look(a);
    imguiFrame(a);
    Access::after(a);
}
// ...and the same frame with what it lettered kept in g_text.
const std::string& shot(AppWindow& a) {
    Access::look(a);
    imguiFrame(a, true);
    Access::after(a);
    return g_text;
}
// The ImGui half alone (no engine pass between: a click's frames).
void drawOnly(AppWindow& a) { imguiFrame(a); }

template <class Pred>
bool passUntil(AppWindow& a, Pred pred, int max = 3000) {
    for (int i = 0; i < max; ++i) {
        if (pred()) { return true; }
        pass(a);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::uint32_t srcFlags(AppWindow& a) { return Access::state(a).sourceFlags; }
bool srcFlag(AppWindow& a, std::uint32_t bit) { return (srcFlags(a) & bit) != 0u; }
bool rxFlag(AppWindow& a, std::uint32_t bit) { return (Access::state(a).flags & bit) != 0u; }

// What a command sends, as text: its op, its two integers and its text - never a number that depends on where a slider
// was clicked (numOf has that).
std::string describe(const cmd::QueuedCommand& q) {
    char buf[600];
    std::snprintf(buf, sizeof(buf), "%s ival=%lld,%lld text=[%s]", cmd::opName(q.c.op), static_cast<long long>(q.c.ival[0]),
                  static_cast<long long>(q.c.ival[1]), q.longText.empty() ? q.c.text : q.longText.c_str());
    return buf;
}
std::string expect(const char* op, long long i0, long long i1, const std::string& text) {
    char buf[600];
    std::snprintf(buf, sizeof(buf), "%s ival=%lld,%lld text=[%s]", op, i0, i1, text.c_str());
    return buf;
}
std::string expect(const char* op) { return expect(op, 0, 0, ""); }

std::size_t countOf(const Access::Sent& sent, const char* op) {
    std::size_t n = 0;
    for (const auto& q : sent) { n += std::string(cmd::opName(q.c.op)) == op ? 1u : 0u; }
    return n;
}
// The commands in `sent` that are not the page-note the window sends once, in order, as describe()s.
std::vector<std::string> sentText(const Access::Sent& sent) {
    std::vector<std::string> out;
    for (const auto& q : sent) {
        if (q.c.op == FOXAPP_OP_NOTE_PANEL) { continue; }
        out.push_back(describe(q));
    }
    return out;
}

// --- the pointer -----------------------------------------------------------------------------------------------------
struct Hit {
    ImGuiID id;
    float x, y;
};

ImGuiWindow* popupWindow() {
    for (ImGuiWindow* w : GImGui->Windows) {
        if (w->Active && (w->Flags & ImGuiWindowFlags_Popup) != 0) { return w; }
    }
    return nullptr;
}

void putAway() { ImGui::GetIO().AddMousePosEvent(2000.0f, 2000.0f); }

void closePopups(AppWindow& a) {
    ImGui::ClosePopupToLevel(0, false);
    putAway();
    drawOnly(a);
    drawOnly(a);
}

// The distinct widgets in the rectangle, each at the first point of a grid the pointer finds it at, top to bottom.
std::vector<Hit> sweepRect(AppWindow& a, float x0, float y0, float x1, float y1, float stepX, float stepY) {
    std::vector<Hit> out;
    ImGuiIO& io = ImGui::GetIO();
    for (float y = y0; y < y1; y += stepY) {
        for (float x = x0; x < x1; x += stepX) {
            io.AddMousePosEvent(x, y);
            drawOnly(a);
            drawOnly(a);  // (ImGui finds what is under the pointer a frame after it moves)
            const ImGuiID hov = GImGui->HoveredId;
            if (hov == 0) { continue; }
            bool seen = false;
            for (const Hit& s : out) { seen = seen || s.id == hov; }
            if (!seen) { out.push_back({hov, x, y}); }
        }
    }
    putAway();
    drawOnly(a);
    return out;
}

// The section's own widgets, top to bottom.
std::vector<Hit> sweepSection(AppWindow& a, float stepX = 36.0f, float stepY = 8.0f) {
    drawOnly(a);
    drawOnly(a);  // (a window's content size is known a frame after it was drawn)
    ImGuiWindow* w = ImGui::FindWindowByName(kWin);
    CHECK(w != nullptr);
    if (w == nullptr) { return {}; }
    const float bottom = std::min(w->Pos.y + w->Size.y - 8.0f, w->Pos.y + w->ContentSize.y + 24.0f);
    // (below the section's header row, whose key and arrow fold the section: a folded section is drawn as a drawer
    // child that only the rail's own flush ends)
    return sweepRect(a, w->Pos.x + 8.0f, w->Pos.y + 38.0f, w->Pos.x + w->Size.x - 8.0f, bottom, stepX, stepY);
}
// The open combo's rows, top to bottom.
std::vector<Hit> sweepPopup(AppWindow& a) {
    drawOnly(a);
    ImGuiWindow* p = popupWindow();
    CHECK(p != nullptr);
    if (p == nullptr) { return {}; }
    return sweepRect(a, p->Pos.x + 4.0f, p->Pos.y + 2.0f, p->Pos.x + p->Size.x - 4.0f, p->Pos.y + p->Size.y - 2.0f, 40.0f, 6.0f);
}

// A click in frames of the window with no engine pass between them: the press, the release (which is where a key
// fires and sends its command), and the pointer put away.
void clickNoPass(AppWindow& a, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    drawOnly(a);
    drawOnly(a);
    io.AddMouseButtonEvent(0, true);
    drawOnly(a);
    io.AddMouseButtonEvent(0, false);
    drawOnly(a);
    putAway();
    drawOnly(a);
}

// A click that lands: the press with the engine's pass before it, and the release - where a key fires - with NO pass
// after it, so the caller can show nothing has happened yet. The next frame is the engine's next pass.
void clickThenHold(AppWindow& a, float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frame(a);
    frame(a);
    io.AddMouseButtonEvent(0, true);
    frame(a);
    io.AddMouseButtonEvent(0, false);
    drawOnly(a);
    putAway();
}

// A click with the commands it sent.
Access::Sent clickSent(AppWindow& a, float x, float y) {
    Access::Sent sent;
    Access::tap(a, &sent);
    clickNoPass(a, x, y);
    Access::tap(a, nullptr);
    return sent;
}

// The combo at the top of the section, opened by a click, its rows swept and the state left open.
std::vector<Hit> openCombo(AppWindow& a) {
    const std::vector<Hit> top = sweepSection(a);
    CHECK(!top.empty());
    if (top.empty()) { return {}; }
    // (the first widget is the combo: the lines above it are text)
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(top[0].x, top[0].y);
    frame(a);
    frame(a);
    io.AddMouseButtonEvent(0, true);
    frame(a);
    io.AddMouseButtonEvent(0, false);
    frame(a);
    frame(a);
    putAway();
    return sweepPopup(a);
}

// ---------------------------------------------------------------------------------------------------------------------
// A. THE COMBO
// ---------------------------------------------------------------------------------------------------------------------

using Engine = cascade::engine::Engine;

// A command through the session and two engine passes (the first applies it, the second collects what it started).
void send(AppWindow& a, const cmd::QueuedCommand& q) {
    Access::submit(a, q);
    pass(a);
    pass(a);
}
void send(AppWindow& a, const FoxCommand& c) { send(a, cmd::QueuedCommand{c, {}}); }
void selectRowId(AppWindow& a, const std::string& id) { send(a, cmd::makeText(FOXAPI_OP_SELECT_SOURCE, id)); }

// The state every scenario begins from: the generator running, a scan done over the world's radios.
void scanAll(AppWindow& a) {
    Access::submit(a, cmd::make(FOXAPI_OP_SCAN_DEVICES));
    const bool done = passUntil(a, [&] { return srcFlag(a, FOXAPI_SRC_SCANNED) && !srcFlag(a, FOXAPI_SRC_SCAN_PENDING); });
    CHECK(done);
    pass(a);
    pass(a);
}

bool answered(AppWindow& a, std::uint32_t op, std::int32_t status) {
    bool found = false;
    for (const Access::Answer& x : Access::answers(a)) { found = found || (x.first == op && x.second == status); }
    return found;
}

void testCombo() {
    std::printf("  A1: the combo's rows, as the engine lists them, lettered as the combo letters them\n");
    Access::install();
    world::get().natives[2].args = "uri=" + world::plutoUriLong();  // (the id of the Pluto's row, built from its address)
    Env env;
    AppWindow& a = env.app;
    scanAll(a);
    const auto& c = Access::lists(a);
    // The generator, the IQ file, the sound card, the two native radios and the Pluto, the two SoapySDR rows.
    CHECK_EQ(c.rows.size(), 8u);
    if (c.rows.size() == 8u) {
        CHECK_EQ(c.rows[0].name, "Signal generator");
        CHECK_EQ(c.rows[1].name, "IQ file");
        CHECK_EQ(c.rows[2].name, "Sound card");
        CHECK_EQ(c.rows[3].caption, "Mock RTL-SDR (serial 00000001) (native)");  // composed here, "(native)" and all
        CHECK_EQ(c.rows[4].caption, "HackRF One (native)");
        CHECK_EQ(c.rows[5].caption, "ADALM-Pluto (network)");  // the Pluto is not a radio that was found: no suffix
        CHECK_EQ(c.rows[6].caption, "Fake Soapy A");           // a SoapySDR row is its own label
        CHECK_EQ(c.rows[7].caption, "Long Soapy");
        CHECK(c.rows[5].pluto && !c.rows[3].pluto);
        CHECK(c.rows[6].soapy && c.rows[7].soapy && !c.rows[5].soapy);
        for (std::size_t i = 0; i < c.rows.size(); ++i) { CHECK_EQ(c.rows[i].id, static_cast<std::uint64_t>(i + 1u)); }
    }
    CHECK(c.anySoapyRow);
    CHECK(c.generatorActive);
    CHECK_EQ(c.row, 0);

    std::printf("  A2: opening the combo asks for a scan ONCE, on the edge, as a command - and letters every row\n");
    {
        Access::Sent sent;
        Access::tap(a, &sent);
        const std::vector<Hit> rows = openCombo(a);
        CHECK_EQ(rows.size(), 8u);
        CHECK_EQ(countOf(sent, "APP_SCAN_DEVICES_ON_OPEN"), 1u);  // not once a frame while it stays open
        const std::string t = shot(a);
        shot(a);
        CHECK_EQ(countOf(sent, "APP_SCAN_DEVICES_ON_OPEN"), 1u);
        for (const char* want : {"Signal generator", "IQ file", "Sound card", "Mock RTL-SDR (serial 00000001) (native)",
                                 "HackRF One (native)", "ADALM-Pluto (network)", "Fake Soapy A", "Long Soapy"}) {
            if (!has(t, want)) { std::printf("      the open combo did not letter [%s]\n", want); }
            CHECK(has(t, want));
        }
        closePopups(a);
        frame(a);
        CHECK(!Access::comboWasOpen(a));
        // ...and again when it is next opened.
        (void)openCombo(a);
        CHECK_EQ(countOf(sent, "APP_SCAN_DEVICES_ON_OPEN"), 2u);
        Access::tap(a, nullptr);
        closePopups(a);
        // The rows the scan found are the rows of the pass that took the command, a frame after it (the old window
        // applied it at once and drew the fresh rows two statements later).
        pass(a);
        pass(a);
    }

    std::printf("  A3: a click opens a radio by the WHOLE id of its row - from fact.source.id, never a detail the table cut\n");
    {
        FoxListItem it;
        CHECK(Access::listItem(a, FOXAPI_LIST_DEVICES, 7, it));
        const std::string whole = "soapy:" + world::soapyLongArgs();
        CHECK(whole.size() > 159u);
        CHECK_EQ(std::strlen(it.detail), 159u);  // the table's detail is cut...
        CHECK(whole.compare(0, 159, it.detail) == 0);
        CHECK_EQ(Access::fact(a, "fact.source.id.8"), whole);  // ...the fact is not
        // A cut id is refused by the engine: the bug a window reading the detail would have had.
        Access::submit(a, cmd::makeText(FOXAPI_OP_SELECT_SOURCE, std::string(it.detail)));
        pass(a);
        pass(a);
        CHECK(answered(a, FOXAPI_OP_SELECT_SOURCE, FOXAPI_NOT_FOUND));
        CHECK(!rxFlag(a, FOXAPI_RX_DEVICE_OPEN));
        // The click on the row.
        const std::vector<Hit> rows = openCombo(a);
        CHECK_EQ(rows.size(), 8u);
        if (rows.size() == 8u) {
            const Access::Sent sent = clickSent(a, rows[7].x, rows[7].y);
            const std::vector<std::string> text = sentText(sent);
            CHECK_EQ(text.size(), 1u);
            if (!text.empty()) { CHECK_EQ(text[0], expect("SELECT_SOURCE", 0, 0, whole)); }
            CHECK_EQ(sent.empty() ? 0u : sent[0].longText.size(), whole.size());  // (it rode the long-text channel)
        }
        CHECK(passUntil(a, [&] { return rxFlag(a, FOXAPI_RX_DEVICE_OPEN); }));
        pass(a);
        CHECK_EQ(Access::sourceKind(a), "soapy");
        CHECK_EQ(Access::fact(a, "fact.source.row"), "7");
        CHECK_EQ(Access::lists(a).row, 7);
        // Back to the generator, by its row.
        const std::vector<Hit> again = openCombo(a);
        CHECK_EQ(again.size(), 8u);
        if (again.size() == 8u) {
            const Access::Sent sent = clickSent(a, again[0].x, again[0].y);
            CHECK_EQ(sentText(sent), std::vector<std::string>{expect("SELECT_SOURCE", 0, 0, "siggen")});
        }
        CHECK(passUntil(a, [&] { return !rxFlag(a, FOXAPI_RX_DEVICE_OPEN); }));
        pass(a);
        CHECK_EQ(Access::lists(a).row, 0);

        // The Pluto's id is built from the address, up to 191 bytes: the whole of it, 199 bytes, goes out.
        const std::string pluto = "pluto:uri=" + world::plutoUriLong();
        CHECK_EQ(pluto.size(), 199u);
        CHECK_EQ(Access::fact(a, "fact.source.id.6"), pluto);
        const std::vector<Hit> prows = openCombo(a);
        if (prows.size() == 8u) {
            const Access::Sent sent = clickSent(a, prows[5].x, prows[5].y);
            CHECK_EQ(sentText(sent), std::vector<std::string>{expect("SELECT_SOURCE", 0, 0, pluto)});
        }
        pass(a);
        pass(a);
        CHECK_EQ(Access::lists(a).row, 5);  // (the Pluto's row only shows its address box until Open)
        CHECK(!rxFlag(a, FOXAPI_RX_DEVICE_OPEN));
        selectRowId(a, "siggen");
        CHECK_EQ(Access::lists(a).row, 0);
    }

    std::printf("  A4: while the patch runs the radios' rows are greyed and send nothing - the three fixed rows still do\n");
    {
        Access::patchRunning(a) = true;
        pass(a);
        pass(a);
        CHECK(rxFlag(a, FOXAPI_RX_PATCH_RUNNING));
        const std::vector<Hit> rows = openCombo(a);
        CHECK_EQ(rows.size(), 8u);
        if (rows.size() == 8u) {
            for (std::size_t i = 0; i < rows.size(); ++i) {
                const Access::Sent sent = clickSent(a, rows[i].x, rows[i].y);
                const std::vector<std::string> text = sentText(sent);
                if (i < 3u) {
                    CHECK_EQ(text.size(), 1u);
                } else {
                    CHECK(text.empty());
                }
                // (a click on a live row closed the popup: open it again for the next)
                if (i + 1 < rows.size()) {
                    closePopups(a);
                    const std::vector<Hit> reopened = openCombo(a);
                    CHECK_EQ(reopened.size(), 8u);
                    if (reopened.size() != 8u) { break; }
                }
            }
        }
        closePopups(a);
        Access::dropQueued(a);
        Access::patchRunning(a) = false;
        pass(a);
        pass(a);
        CHECK(!rxFlag(a, FOXAPI_RX_PATCH_RUNNING));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// B. THE SCAN'S STATES
// ---------------------------------------------------------------------------------------------------------------------

// A radio whose open is held on its worker until the test opens the gate: the engine's "opening" state, for as long as
// the test wants it.
std::unique_ptr<cascade::source::DeviceSource> gatedRadio(const std::string& kind) {
    world::waitOn(world::get().openGate);
    return engine_api_test::makeFakeRadio(kind);
}

void testScanStates() {
    std::printf("  B1: a scan in flight: the line, the combo and the keys greyed - and the scan's end lifts it\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    // Before any scan: nothing is said about hardware that was never looked for.
    {
        const std::string t = shot(a);
        CHECK(!has(t, "No radio hardware found"));
        CHECK(!has(t, "Scanning for devices..."));
        CHECK(!srcFlag(a, FOXAPI_SRC_SCANNED));
    }
    world::get().soapyGate = false;
    Access::submit(a, cmd::make(FOXAPI_OP_SCAN_DEVICES));
    CHECK(passUntil(a, [&] { return srcFlag(a, FOXAPI_SRC_SCAN_PENDING); }));
    {
        const std::string t = shot(a);
        CHECK(has(t, "Scanning for devices..."));
        CHECK(!has(t, "No radio hardware found"));
        // The combo, Refresh and the network box are greyed: a click on any sends nothing, and the combo stays shut.
        const std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 3u);
        for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 3u); ++i) {
            const Access::Sent sent = clickSent(a, top[i].x, top[i].y);
            CHECK(sentText(sent).empty());
        }
        CHECK(popupWindow() == nullptr);
        Access::dropQueued(a);
    }
    world::get().soapyGate = true;
    CHECK(passUntil(a, [&] { return !srcFlag(a, FOXAPI_SRC_SCAN_PENDING) && srcFlag(a, FOXAPI_SRC_SCANNED); }));
    pass(a);
    {
        const std::string t = shot(a);
        CHECK(!has(t, "Scanning for devices..."));
        CHECK(!has(t, "No radio hardware found"));  // (the scan found radios)
        const std::vector<Hit> top = sweepSection(a);
        const Access::Sent sent = clickSent(a, top.size() > 1 ? top[1].x : 0.0f, top.size() > 1 ? top[1].y : 0.0f);
        CHECK_EQ(sentText(sent), std::vector<std::string>{expect("SCAN_DEVICES")});  // Refresh is live again
        Access::dropQueued(a);
    }

    std::printf("  B2: a radio being opened: \"Opening ...\", the combo greyed - and its end lifts it\n");
    {
        Access::makeDevice(&gatedRadio);
        world::get().openGate = false;
        selectRowId(a, "rtlsdr:serial=00000001");
        CHECK(passUntil(a, [&] { return rxFlag(a, FOXAPI_RX_DEVICE_OPENING); }));
        const std::string opening = Access::fact(a, "fact.source.opening");
        CHECK_EQ(opening, "Mock RTL-SDR (serial 00000001)");  // (the row's label: a hooked walk adds no " (native)")
        const std::string t = shot(a);
        CHECK(has(t, "Opening " + opening + "..."));
        // While the open is in flight the whole SoapySDR scan is deferred (no radio can be vouched for): the other sentence,
        // quoting the radio being opened.
        CHECK(srcFlag(a, FOXAPI_SRC_SCAN_GATED));
        CHECK_EQ(Access::fact(a, "fact.source.scanGate"), opening);
        CHECK(Access::fact(a, "fact.source.scanSkip").empty());
        CHECK(has(t, "Refreshed the native radios. The SoapySDR scan is deferred while " + opening + " is open"));
        CHECK(!has(t, "leaves out the"));
        const std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 3u);
        for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 3u); ++i) {
            CHECK(sentText(clickSent(a, top[i].x, top[i].y)).empty());
        }
        Access::dropQueued(a);
        world::get().openGate = true;
        CHECK(passUntil(a, [&] { return rxFlag(a, FOXAPI_RX_DEVICE_OPEN) && !rxFlag(a, FOXAPI_RX_DEVICE_OPENING); }));
        pass(a);
        CHECK(!has(shot(a), "Opening "));
        Access::makeDevice(&engine_api_test::makeFakeRadio);
    }

    std::printf("  B3: beside an open radio the scan is gated: the sentence for what Refresh leaves out, or for a scan deferred\n");
    {
        // The radio is open (B2): its model is what the sentences quote, and its driver is what Refresh leaves out.
        CHECK(srcFlag(a, FOXAPI_SRC_SCAN_GATED));
        const std::string gate = Access::fact(a, "fact.source.scanGate");
        const std::string skip = Access::fact(a, "fact.source.scanSkip");
        CHECK_EQ(gate, "Mock RTL-SDR");
        CHECK_EQ(skip, "rtlsdr");
        const std::string t = shot(a);
        CHECK(has(t, "While " + gate + " is open, Refresh looks for every other kind of radio but leaves out the " + skip +
                         " driver"));
        CHECK(!has(t, "The SoapySDR scan is deferred while"));
    }
}

void testPartialAndNetworkBox() {
    std::printf("  B4: \"No radio hardware found\" waits for a WHOLE scan: a scan beside an open radio is partial and says nothing\n");
    Access::install();
    world::get().soapy.clear();  // (no SoapySDR radio at all)
    Env env;
    AppWindow& a = env.app;
    scanAll(a);
    CHECK(srcFlag(a, FOXAPI_SRC_SCANNED));
    CHECK(!srcFlag(a, FOXAPI_SRC_SCAN_PARTIAL));
    CHECK(!Access::lists(a).anySoapyRow);
    CHECK(has(shot(a), "No radio hardware found"));
    // A radio opens; the scan beside it leaves its driver out.
    selectRowId(a, "rtlsdr:serial=00000001");
    CHECK(passUntil(a, [&] { return rxFlag(a, FOXAPI_RX_DEVICE_OPEN); }));
    Access::submit(a, cmd::make(FOXAPI_OP_SCAN_DEVICES));
    CHECK(passUntil(a, [&] { return srcFlag(a, FOXAPI_SRC_SCAN_PARTIAL) && !srcFlag(a, FOXAPI_SRC_SCAN_PENDING); }));
    pass(a);
    CHECK(srcFlag(a, FOXAPI_SRC_SCANNED));
    CHECK(srcFlag(a, FOXAPI_SRC_SCAN_PARTIAL));
    CHECK(!has(shot(a), "No radio hardware found"));
    // The radio closes; the combo's next opening runs the whole scan that is owed.
    selectRowId(a, "siggen");
    CHECK(passUntil(a, [&] { return !rxFlag(a, FOXAPI_RX_DEVICE_OPEN); }));
    Access::submit(a, cmd::make(FOXAPP_OP_SCAN_DEVICES_ON_OPEN));
    CHECK(passUntil(a, [&] { return !srcFlag(a, FOXAPI_SRC_SCAN_PARTIAL) && !srcFlag(a, FOXAPI_SRC_SCAN_PENDING); }));
    pass(a);
    CHECK(has(shot(a), "No radio hardware found"));

    std::printf("  B5: the UHD note and the network box: drawn from two flags, and the tick is a command\n");
    {
        CHECK(!srcFlag(a, FOXAPI_SRC_NETWORK_USRPS));
        Access::absent(a).clear();  // a scan that left nothing out: nothing to say
        pass(a);
        pass(a);
        CHECK(!srcFlag(a, FOXAPI_SRC_UHD_LEFT_OUT));
        CHECK(!has(shot(a), "Network USRPs are not searched"));
        Access::absent(a) = {"uhd"};  // what a landed scan that found no USRP says
        pass(a);
        pass(a);
        CHECK(srcFlag(a, FOXAPI_SRC_UHD_LEFT_OUT));
        const char* const hint = "Network USRPs are not searched - tick Look for network USRPs to include them";
        std::string t = shot(a);
        CHECK(has(t, hint));
        CHECK(has(t, "[ ] Look for network USRPs"));
        const std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 3u);
        if (top.size() >= 3u) {
            const Access::Sent sent = clickSent(a, top[2].x, top[2].y);
            CHECK_EQ(sentText(sent), std::vector<std::string>{expect("APP_SET_NETWORK_USRP_SCAN", 1, 0, "")});
            // Nothing is ticked until the engine says so, a pass later.
            CHECK(!srcFlag(a, FOXAPI_SRC_NETWORK_USRPS));
            // The next frame is the engine's next pass: it takes the command, and the frame draws what it published.
            const std::string next = shot(a);
            CHECK(srcFlag(a, FOXAPI_SRC_NETWORK_USRPS));
            CHECK(has(next, "[x] Look for network USRPs"));
            CHECK(!has(next, hint));  // (ticked: the rescan it starts is about to answer it)
        }
        CHECK(passUntil(a, [&] { return srcFlag(a, FOXAPI_SRC_NETWORK_USRPS) && !srcFlag(a, FOXAPI_SRC_SCAN_PENDING); }));
        pass(a);
        t = shot(a);
        CHECK(has(t, "[x] Look for network USRPs"));
        CHECK(!has(t, hint));  // (ticked: the rescan it starts is about to answer it)
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// C. NO HARDWARE FOUND, THE DONGLES, THE SDRPLAY NOTE
// ---------------------------------------------------------------------------------------------------------------------

void testNoHardware() {
    std::printf("  C1: \"No radio hardware found\" draws from the engine's SCAN_EVIDENCE - and never crosses into the runtime\n");
    Access::install();
    world::World& w = world::get();
    w.soapy.clear();
    w.modules = {"libsoapy_a.dll", "libsoapy_b.dll"};
    w.searchPaths = {"C:/search/one", "C:/search/two"};
    w.vendors = {{"PothosSDR", "C:/Pothos", "C:/Pothos/lib", "C:/Pothos/bin"}};
    w.faulted = {"uhd", "audio"};
    {
        Env env;
        AppWindow& a = env.app;
        CHECK_EQ(w.diagCalls.load(), 0);
        scanAll(a);
        // The engine took it ONCE, when the scan landed having found nothing.
        CHECK_EQ(w.diagCalls.load(), 1);
        const std::string t = shot(a);
        CHECK(has(t, "No radio hardware found"));
        CHECK(has(t, "2 vendor modules are loaded, but none reported a device."));
        CHECK(has(t, "found PothosSDR at C:/Pothos"));
        CHECK(has(t, "search: C:/search/one"));
        CHECK(has(t, "search: C:/search/two"));
        CHECK(has(t, "module: libsoapy_a.dll"));
        CHECK(has(t, "module: libsoapy_b.dll"));
        CHECK(has(t, "Not scanned again this session: uhd, audio (it crashed the device scan)"));
        CHECK_EQ(Access::lists(a).modulesLoaded, 2);
        CHECK_EQ(Access::lists(a).evidence.size(), 5u);
        // A thousand more frames, however long the block stays up: no further crossing.
        for (int i = 0; i < 200; ++i) { frame(a); }
        CHECK_EQ(w.diagCalls.load(), 1);
        // The draw never asks again however often it is drawn: the cache is not even read (nothing moved).
        const std::uint32_t reads = Access::cache(a).refreshes;
        for (int i = 0; i < 200; ++i) { frame(a); }
        CHECK_EQ(Access::cache(a).refreshes, reads);
        CHECK_EQ(w.diagCalls.load(), 1);
        // The whole block is the REMOTE-denied list: a remote session is told nothing of this machine's paths.
        // (A second scan that lands empty reads it again: once more, not once a frame.)
        Access::submit(a, cmd::make(FOXAPI_OP_SCAN_DEVICES));
        CHECK(passUntil(a, [&] { return w.diagCalls.load() >= 2; }));
        for (int i = 0; i < 50; ++i) { frame(a); }
        CHECK(w.diagCalls.load() <= 2);
    }

    std::printf("  C2: one vendor module is said in the singular; none loaded and none installed says what to install\n");
    {
        w.modules = {"only.dll"};
        w.vendors.clear();
        w.searchPaths.clear();
        w.faulted.clear();
        Env env;
        AppWindow& a = env.app;
        scanAll(a);
        std::string t = shot(a);
        CHECK(has(t, "1 vendor module is loaded, but none reported a device."));
        CHECK(has(t, "no vendor SDR installation detected"));
        CHECK(has(t, "module: only.dll"));
        CHECK(!has(t, "search: "));
        CHECK(!has(t, "Not scanned again this session"));
        w.modules.clear();
        const int taken = w.diagCalls.load();
        Access::submit(a, cmd::make(FOXAPI_OP_SCAN_DEVICES));
        CHECK(passUntil(a, [&] { return w.diagCalls.load() > taken; }));
        pass(a);
        t = shot(a);
        CHECK(has(t, "No vendor modules are loaded. Install PothosSDR"));
        CHECK(!has(t, "module: "));
    }

    std::printf("  C3: an engine whose scan is stood in for reads none of the runtime's diagnostics at all\n");
    {
        Access::install(false);  // the scan hooked, the diagnostics not
        world::get().soapy.clear();
        Env env;
        AppWindow& a = env.app;
        scanAll(a);
        CHECK_EQ(world::get().diagCalls.load(), 0);
        const std::string t = shot(a);
        CHECK(has(t, "No radio hardware found"));
        CHECK(has(t, "No vendor modules are loaded. Install PothosSDR"));
        CHECK(Access::lists(a).evidence.empty());
        for (int i = 0; i < 20; ++i) { frame(a); }
        CHECK_EQ(world::get().diagCalls.load(), 0);
    }

    std::printf("  C4: one sentence for each dongle that cannot be opened - three wordings, chosen by the engine's family\n");
    {
        Access::install();
        world::World& w2 = world::get();
        const cascade::usb::UsbId rtl = cascade::source::rtlSdrUsbIds().front();
        const cascade::usb::UsbId rx888 = cascade::source::rx888UsbIds().front();
        cascade::usb::UsbDeviceInfo u1;
        u1.vid = rtl.vid;
        u1.pid = rtl.pid;
        u1.description = "Bulk-In, Interface";
        cascade::usb::UsbDeviceInfo u2;
        u2.vid = rx888.vid;
        u2.pid = rx888.pid;
        u2.description = "WestBridge";
        cascade::usb::UsbDeviceInfo u3;
        u3.vid = 0x1234;
        u3.pid = 0x5678;  // (no description: the sentence says "A USB radio")
        w2.unbound = {u1, u2, u3};
        Env env;
        AppWindow& a = env.app;
        scanAll(a);
        const auto& c = Access::lists(a);
        CHECK_EQ(c.unbound.size(), 3u);
        if (c.unbound.size() == 3u) {
            CHECK_EQ(c.unbound[0].family, "rtl");
            CHECK_EQ(c.unbound[1].family, "rx888");
            CHECK_EQ(c.unbound[2].family, "");
            CHECK_EQ(c.unbound[2].name, "");
        }
        const std::string t = shot(a);
        CHECK(has(t, "Bulk-In, Interface is plugged in but is not bound to WinUSB, so nothing can open it. Run Zadig"));
        CHECK(has(t, "select \"Bulk-In, Interface (Interface 0)\""));  // (the RTL's own step)
        CHECK(has(t, "WestBridge is plugged in but is not bound to WinUSB"));
        CHECK(has(t, "An RX888 has to be done TWICE"));
        CHECK(has(t, "A USB radio is plugged in but is not bound to WinUSB"));
        CHECK(has(t, "select this radio in the dropdown"));
        // Said whether or not anything else was found (a radio is listed here: the combo has rows).
        CHECK(Access::lists(a).rows.size() > 3u);
    }

    std::printf("  C5: the SDRplay note: the engine's advice and where it looked, until an RSP row is found\n");
    {
        Access::install();
        world::World& w3 = world::get();
        w3.sdrAdvice = "Install the SDRplay API 3.x from sdrplay.com to use an RSP.";
        w3.sdrDetail = "looked in C:/Program Files/SDRplay/API";
        Env env;
        AppWindow& a = env.app;
        scanAll(a);
        CHECK(!srcFlag(a, FOXAPI_SRC_SDRPLAY_ROWS));
        std::string t = shot(a);
        CHECK(has(t, w3.sdrAdvice));
        CHECK(has(t, "SDRplay API: " + w3.sdrDetail));
        w3.sdrRows = true;  // an API row was found: the advice is not for this machine
        Access::submit(a, cmd::make(FOXAPI_OP_SCAN_DEVICES));
        CHECK(passUntil(a, [&] { return srcFlag(a, FOXAPI_SRC_SDRPLAY_ROWS); }));
        pass(a);
        t = shot(a);
        CHECK(!has(t, w3.sdrAdvice));
        CHECK(!has(t, "SDRplay API: "));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// D. THE FAULT CARD, THE LAMP AND THE SAVED-NOT-OPEN PREVIEW
// ---------------------------------------------------------------------------------------------------------------------

void testFaultAndLamp() {
    std::printf("  D1: the fault card shows the WHOLE sentence; the reconnect advice is NOT added to \"restart FoxSDR\"\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    scanAll(a);
    const std::string zombie =
        "source thread: the radio's driver is holding an abandoned thread and this source cannot restart; pick another "
        "source or restart FoxSDR";
    CHECK_EQ(zombie.size(), 134u);
    const char* const advice = "Reconnect it and pick the source again, or switch to the signal generator.";
    {
        Access::testFault(a) = zombie;
        pass(a);
        pass(a);
        CHECK(rxFlag(a, FOXAPI_RX_FAULTED));
        const std::string t = shot(a);
        CHECK(has(t, "Device stopped: " + zombie));  // whole: its last word is there
        CHECK(!has(t, advice));
        // The state's faultMessage is the same sentence cut at 127 bytes: on that copy the advice WOULD have been added.
        const std::string cut = Access::state(a).faultMessage;
        CHECK_EQ(cut.size(), 127u);
        CHECK(cascade::gui::reconnectAdviceApplies(cut));
        CHECK(!cascade::gui::reconnectAdviceApplies(zombie));
        CHECK_EQ(Access::statusLines(a).deviceFault, zombie);
    }
    std::printf("  D2: any other fault is followed by the advice\n");
    {
        Access::testFault(a) = "usb: the device was removed while streaming";
        pass(a);
        pass(a);
        const std::string t = shot(a);
        CHECK(has(t, "Device stopped: usb: the device was removed while streaming"));
        CHECK(has(t, advice));
        Access::testFault(a).clear();
        pass(a);
        pass(a);
        // (the engine latches a test fault until the next start: the flag follows the line)
    }

    std::printf("  D3: the lamp and the saved-not-open preview: a kept radio, I/Q file or sound card\n");
    {
        Access::keepKind(a) = "soapy";
        Access::keepLabel(a) = "Kept Soapy Radio";
        pass(a);
        pass(a);
        CHECK(Access::lamp(a));
        std::string t = shot(a);
        CHECK(has(t, "Kept Soapy Radio (saved, not open)"));  // the combo's preview
        CHECK(has(t, "Kept Soapy Radio is still the saved radio. The signal generator is running in its place"));
        Access::keepKind(a) = "soundcard";
        pass(a);
        pass(a);
        CHECK(Access::lamp(a));
        t = shot(a);
        CHECK(has(t, "Kept Soapy Radio is still the saved sound card."));
        Access::keepKind(a) = "file";
        pass(a);
        pass(a);
        CHECK(!Access::lamp(a));  // (an I/Q file is said in its own words, not by the lamp)
        t = shot(a);
        CHECK(has(t, "Kept Soapy Radio is still the saved I/Q file."));
        CHECK(has(t, "Kept Soapy Radio (saved, not open)"));
        // Nothing kept: no lamp, no preview, the generator's own name.
        Access::keepKind(a).clear();
        Access::keepLabel(a).clear();
        pass(a);
        pass(a);
        CHECK(!Access::lamp(a));
        t = shot(a);
        CHECK(!has(t, "saved, not open"));
        CHECK(has(t, "Signal generator"));
        // A radio open: nothing is "standing in" for anything.
        Access::keepKind(a) = "soapy";
        Access::keepLabel(a) = "Kept Soapy Radio";
        selectRowId(a, "rtlsdr:serial=00000001");
        CHECK(passUntil(a, [&] { return rxFlag(a, FOXAPI_RX_DEVICE_OPEN); }));
        pass(a);
        CHECK(!Access::lamp(a));
        CHECK(!has(shot(a), "saved, not open"));
        selectRowId(a, "siggen");
        CHECK(passUntil(a, [&] { return !rxFlag(a, FOXAPI_RX_DEVICE_OPEN); }));
        Access::keepKind(a).clear();
        Access::keepLabel(a).clear();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// E. THE OPEN RADIO'S PANEL
// ---------------------------------------------------------------------------------------------------------------------

// A combo opened by a click on its widget, its rows swept and the popup left open.
std::vector<Hit> openAt(AppWindow& a, const Hit& h) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(h.x, h.y);
    frame(a);
    frame(a);
    io.AddMouseButtonEvent(0, true);
    frame(a);
    io.AddMouseButtonEvent(0, false);
    frame(a);
    frame(a);
    putAway();
    return sweepPopup(a);
}

// Every widget of the section found and clicked once, in the state the caller has set, and the commands each click
// sent (no pass runs: nothing is applied). A popup the click opened is shut again.
struct Found {
    std::string command;  // "" nothing
    float x, y;
};
std::vector<Found> survey(AppWindow& a) {
    std::vector<Found> out;
    const std::vector<Hit> spots = sweepSection(a);
    for (const Hit& s : spots) {
        Access::Sent sent;
        Access::tap(a, &sent);
        clickNoPass(a, s.x, s.y);
        Access::tap(a, nullptr);
        Found f{"", s.x, s.y};
        for (const std::string& t : sentText(sent)) { f.command += (f.command.empty() ? "" : " ; ") + t; }
        out.push_back(f);
        closePopups(a);
        ImGui::ClearActiveID();
        Access::dropQueued(a);
        drawOnly(a);
    }
    return out;
}
std::vector<std::string> commandsOf(const std::vector<Found>& v) {
    std::vector<std::string> out;
    for (const Found& f : v) {
        if (!f.command.empty()) { out.push_back(f.command); }
    }
    std::sort(out.begin(), out.end());
    return out;
}
// Exactly these, in any order; a difference is printed.
bool sameCommands(const std::vector<std::string>& got, std::vector<std::string> want, const char* when) {
    std::sort(want.begin(), want.end());
    const bool same = got == want;
    if (!same) {
        std::printf("      %s: the clicks sent something else\n", when);
        for (const std::string& g : got) { std::printf("        sent   %s\n", g.c_str()); }
        for (const std::string& w : want) { std::printf("        wanted %s\n", w.c_str()); }
    }
    CHECK(same);
    return same;
}

// The three that every state of the section has at the top: the combo's opening, Refresh and the network box.
const std::vector<std::string>& topKeys() {
    static const std::vector<std::string> keys = {expect("APP_SCAN_DEVICES_ON_OPEN"), expect("SCAN_DEVICES"),
                                                  expect("APP_SET_NETWORK_USRP_SCAN", 1, 0, "")};
    return keys;
}
std::vector<std::string> withTop(std::vector<std::string> more) {
    more.insert(more.end(), topKeys().begin(), topKeys().end());
    return more;
}

void openFakeRadio(AppWindow& a) {
    selectRowId(a, "rtlsdr:serial=00000001");
    CHECK(passUntil(a, [&] { return rxFlag(a, FOXAPI_RX_DEVICE_OPEN) && !rxFlag(a, FOXAPI_RX_DEVICE_OPENING); }));
    pass(a);
    pass(a);
}

void testRadioPanel() {
    std::printf("  E1: the open radio's panel is the table's: its rate, its antenna, its auto gain, its gain stage\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    scanAll(a);
    openFakeRadio(a);
    const auto& c = Access::lists(a);
    CHECK_EQ(c.rates.size(), 6u);
    if (c.rates.size() == 6u) {
        CHECK_EQ(c.rates[0].label, "0.250 MS/s");
        CHECK_EQ(c.rates[4].label, "2.400 MS/s");
        CHECK_NEAR(c.rates[4].hz, 2400000.0, 0.5);
    }
    CHECK_EQ(c.gains.size(), 1u);
    CHECK_EQ(c.antennas.size(), 1u);
    CHECK_EQ(c.antenna, "RX");
    {
        const std::string t = shot(a);
        CHECK(has(t, "2.048 MS/s"));          // the Rate combo's preview: the row nearest the state's rate
        CHECK(has(t, "actual 2.048 MS/s"));   // the readback beside it
        CHECK(has(t, "Antenna: RX"));         // one port: a line, not a combo
        CHECK(has(t, "[ ] Auto gain"));
        CHECK(has(t, "TUNER"));
        CHECK(has(t, "29.7 dB"));
        CHECK(!has(t, "Bias tee"));
        CHECK(!has(t, "FM notch"));
        CHECK(!has(t, "No radio hardware found"));
    }

    std::printf("  E2: a rate chosen in the Rate combo lands at the engine's next pass; the row that is current sends nothing\n");
    {
        const std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 6u);
        if (top.size() >= 6u) {
            const std::vector<Hit> rows = openAt(a, top[3]);  // the Rate combo
            CHECK_EQ(rows.size(), 6u);
            if (rows.size() == 6u) {
                CHECK(has(shot(a), "3.200 MS/s"));
                // The current row (2.048) is the 4th: a click on it does nothing.
                CHECK(sentText(clickSent(a, rows[3].x, rows[3].y)).empty());
                closePopups(a);
                const std::vector<Hit> again = openAt(a, top[3]);
                CHECK_EQ(again.size(), 6u);
                if (again.size() == 6u) {
                    const Access::Sent sent = clickSent(a, again[4].x, again[4].y);
                    CHECK_EQ(sent.size(), 1u);
                    if (sent.size() == 1u) {
                        CHECK_EQ(std::string(cmd::opName(sent[0].c.op)), "SET_SAMPLE_RATE");
                        CHECK_NEAR(sent[0].c.num[0], 2400000.0, 0.5);
                    }
                }
            }
        }
        // Nothing has landed in the click's frame: the state, the radio and the window's cache are what they were.
        CHECK_NEAR(Access::state(a).sampleRateHz, 2048000.0, 0.5);
        CHECK_NEAR(Access::engineRateNow(a), 2048000.0, 0.5);
        // ...and the engine's value is there the frame after the pass.
        const std::string t = shot(a);
        CHECK_NEAR(Access::state(a).sampleRateHz, 2400000.0, 0.5);
        CHECK(has(t, "actual 2.4 MS/s"));  // (%.4g)
        closePopups(a);
    }

    std::printf("  E3: Auto gain is a command and greys the stage sliders while it is on\n");
    {
        pass(a);
        std::vector<Hit> top = sweepSection(a);
        const std::size_t before = top.size();
        CHECK(before >= 6u);
        if (before >= 6u) {
            const Access::Sent sent = clickSent(a, top[4].x, top[4].y);
            CHECK_EQ(sentText(sent), std::vector<std::string>{expect("SET_DEVICE_AGC", 1, 0, "")});
            CHECK(!rxFlag(a, FOXAPI_RX_DEVICE_AGC));
            const std::string t = shot(a);  // the engine's next pass
            CHECK(rxFlag(a, FOXAPI_RX_DEVICE_AGC));
            CHECK(has(t, "[x] Auto gain"));
            // The stage sliders are greyed while it drives: no control sends a gain, and Auto gain sends its off.
            const std::vector<Found> greyed = survey(a);
            sameCommands(commandsOf(greyed), withTop({expect("SET_DEVICE_AGC", 0, 0, "")}), "E3 AGC on");
            frame(a);
            CHECK(rxFlag(a, FOXAPI_RX_DEVICE_AGC));  // (the survey's clicks were dropped: nothing was applied)
            Access::submit(a, cmd::makeInt(FOXAPI_OP_SET_DEVICE_AGC, 0));
            frame(a);
            CHECK(!rxFlag(a, FOXAPI_RX_DEVICE_AGC));
        }
    }

    std::printf("  E4: a gain stage's slider sends its name and the figure, and shows the readback the frame after the pass\n");
    {
        const std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 6u);
        if (top.size() >= 6u) {
            const float before = Access::lists(a).gains[0].db;
            CHECK_NEAR(before, 29.7, 0.01);
            const Access::Sent sent = clickSent(a, top[5].x, top[5].y);
            CHECK_EQ(sent.size(), 1u);
            if (sent.size() == 1u) {
                CHECK_EQ(std::string(cmd::opName(sent[0].c.op)), "SET_GAIN");
                CHECK_EQ(std::string(sent[0].c.text), "TUNER");
                CHECK(sent[0].c.num[0] >= 0.0 && sent[0].c.num[0] < 25.0);  // (the left of the slider: low)
                CHECK_NEAR(Access::lists(a).gains[0].db, 29.7, 0.01);  // nothing in the click's frame
                frame(a);
                CHECK_NEAR(Access::lists(a).gains[0].db, sent[0].c.num[0], 0.06);
                CHECK(Access::lists(a).gains[0].db < 25.0f);
            }
        }
    }

    std::printf("  E5: the antennas, the stages' own spans and units, the bias tee and the RSP and RX888 switches\n");
    {
        // What a driver with these things reports (the members the table's lists are built from, set as an open of
        // such a radio would have set them).
        Access::antennas(a) = {"RX", "TX/RX"};
        Access::antenna(a) = "RX";
        Access::agcSupported(a) = false;
        Access::gainNames(a) = {"LNA", "VGA", "STEPS"};
        cascade::source::GainInfo lna{"LNA", 0.0, 33.0, 1.0};
        cascade::source::GainInfo vga{"VGA", -4.7, 40.8, 0.1};
        cascade::source::GainInfo steps{"STEPS", 0.0, 15.0, 1.0, cascade::source::GainUnit::Steps};
        Access::gainRanges(a) = {lna, vga, steps};
        Access::gainsDb(a) = {20.0f, -3.2f, 7.0f};
        Access::biasTee(a).present = true;
        Access::biasTee(a).shown = false;
        Access::rfNotchPresent(a) = true;
        Access::rfNotch(a) = true;
        Access::dabNotchPresent(a) = true;
        Access::hdrPresent(a) = true;
        Access::adcPresent(a) = true;
        Access::randomiser(a) = true;
        pass(a);
        pass(a);
        const auto& cc = Access::lists(a);
        CHECK_EQ(cc.antennas.size(), 2u);
        CHECK_EQ(cc.gains.size(), 3u);
        if (cc.gains.size() == 3u) {
            CHECK(cc.gains[2].unit == cascade::source::GainUnit::Steps);
            CHECK(cc.gains[0].unit == cascade::source::GainUnit::Decibels);
            CHECK_NEAR(cc.gains[1].lo, -4.7, 0.001);
            CHECK_NEAR(cc.gains[1].hi, 40.8, 0.001);
        }
        CHECK(cc.rfNotchPresent && cc.rfNotch && cc.dabNotchPresent && !cc.dabNotch && cc.hdrPresent && !cc.hdr);
        CHECK(cc.adcPresent && !cc.dither && cc.randomiser);
        const std::string t = shot(a);
        CHECK(has(t, "Auto gain: not supported"));
        CHECK(!has(t, "Antenna: RX"));  // two ports: a combo (its preview is the port in use)
        CHECK(has(t, "LNA"));
        CHECK(has(t, "VGA"));
        CHECK(has(t, "-3.2 dB"));  // (a stage in decibels is lettered with its unit...)
        CHECK(has(t, "STEPS"));
        CHECK(!has(t, "7.0 dB"));  // (...a register step is not)
        CHECK(has(t, "[ ] Bias tee"));
        CHECK(has(t, "[x] FM notch"));
        CHECK(has(t, "[ ] DAB notch"));
        CHECK(has(t, "[ ] HDR mode"));
        CHECK(has(t, "[ ] ADC dither"));
        CHECK(has(t, "[x] ADC randomiser"));
        // Every control sends exactly its own command.
        const std::vector<Found> found = survey(a);
        sameCommands(commandsOf(found),
                     withTop({expect("SET_GAIN", 0, 0, "LNA"), expect("SET_GAIN", 0, 0, "VGA"), expect("SET_GAIN", 0, 0, "STEPS"),
                              expect("SET_BIAS_TEE", 1, 0, ""), expect("SET_DEVICE_OPTION", 0, 0, "rf_notch"),
                              expect("SET_DEVICE_OPTION", 1, 0, "dab_notch"), expect("SET_DEVICE_OPTION", 1, 0, "hdr"),
                              expect("SET_DEVICE_OPTION", 1, 0, "dither"), expect("SET_DEVICE_OPTION", 0, 0, "randomiser")}),
                     "E5 radio with every control");
        // The antenna combo: the port in use sends nothing, the other sends its name.
        const std::vector<Hit> top = sweepSection(a);
        // (combo, Refresh, network box, Rate, Antenna: the fifth)
        CHECK(top.size() > 5u);
        if (top.size() > 5u) {
            const std::vector<Hit> rows = openAt(a, top[4]);
            CHECK_EQ(rows.size(), 2u);
            if (rows.size() == 2u) {
                CHECK(sentText(clickSent(a, rows[0].x, rows[0].y)).empty());
                closePopups(a);
                const std::vector<Hit> again = openAt(a, top[4]);
                if (again.size() == 2u) {
                    CHECK_EQ(sentText(clickSent(a, again[1].x, again[1].y)),
                             std::vector<std::string>{expect("SET_ANTENNA", 0, 0, "TX/RX")});
                }
            }
            closePopups(a);
            Access::dropQueued(a);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// F. THE IQ FILE'S AND THE PLUTO'S BOXES
// ---------------------------------------------------------------------------------------------------------------------

void testIqFileAndPluto() {
    std::printf("  F1: the IQ file's box: the path is the window's, Open sends it - a long one on the long-text channel\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    scanAll(a);
    selectRowId(a, "row:iqfile");
    CHECK_EQ(Access::lists(a).row, 1);
    std::snprintf(Access::iqPath(a), 512, "%s", "C:/rec/a.wav");
    {
        const std::string t = shot(a);
        CHECK(has(t, "C:/rec/a.wav"));
        CHECK(!has(t, "A Pluto on its USB cable"));
        const std::vector<Found> found = survey(a);
        // The text box sends nothing; Open sends the path.
        sameCommands(commandsOf(found), withTop({expect("SELECT_SOURCE", 0, 0, "file:C:/rec/a.wav")}), "F1 IQ file");
        const std::string longPath = "C:/" + std::string(505, 'd');
        std::snprintf(Access::iqPath(a), 512, "%s", longPath.c_str());
        const std::vector<Found> again = survey(a);
        sameCommands(commandsOf(again), withTop({expect("SELECT_SOURCE", 0, 0, "file:" + longPath)}), "F1 long path");
    }

    std::printf("  F2: the Pluto's box is a draft: each edit is sent, a pass later the engine's address is the draft\n");
    selectRowId(a, "pluto:uri=ip:192.168.2.1");
    // (the world's Pluto row carries the address of its own: the id is its args)
    CHECK_EQ(Access::lists(a).row, 5);
    CHECK_EQ(Access::lists(a).plutoUri, "ip:192.168.2.1");
    {
        std::string t = shot(a);
        CHECK(has(t, "A Pluto on its USB cable answers at ip:192.168.2.1."));
        CHECK(has(t, "ip:192.168.2.1"));
        CHECK_EQ(std::string(Access::plutoDraft(a)), "ip:192.168.2.1");
        // The box is the 4th widget (the combo, Refresh, the network box, the address), Open the 5th.
        std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 5u);
        if (top.size() >= 5u) {
            // Click into the box and type: the End key first, so the characters land after the address.
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(top[3].x, top[3].y);
            drawOnly(a);
            drawOnly(a);
            io.AddMouseButtonEvent(0, true);
            drawOnly(a);
            io.AddMouseButtonEvent(0, false);
            drawOnly(a);
            io.AddKeyEvent(ImGuiKey_End, true);
            drawOnly(a);
            io.AddKeyEvent(ImGuiKey_End, false);
            drawOnly(a);
            Access::Sent sent;
            Access::tap(a, &sent);
            io.AddInputCharactersUTF8("7");
            drawOnly(a);
            Access::tap(a, nullptr);
            CHECK_EQ(sentText(sent), std::vector<std::string>{expect("APP_SET_PLUTO_URI", 0, 0, "ip:192.168.2.17")});
            CHECK(Access::plutoPending(a));
            // The box stays what was typed while the engine has not caught up - even when the click that leaves it
            // lands in the very frame the edit is still queued (a stale address must not come back for a pass).
            io.AddMousePosEvent(300.0f, 900.0f);
            drawOnly(a);
            io.AddMouseButtonEvent(0, true);
            drawOnly(a);
            io.AddMouseButtonEvent(0, false);
            drawOnly(a);
            drawOnly(a);
            putAway();
            drawOnly(a);
            CHECK_EQ(std::string(Access::plutoDraft(a)), "ip:192.168.2.17");
            CHECK_EQ(Access::lists(a).plutoUri, "ip:192.168.2.1");  // (the table still has the old one)
            // The engine's next pass takes the edit; the draft is then the engine's address, and the box is released.
            frame(a);
            CHECK_EQ(Access::lists(a).plutoUri, "ip:192.168.2.17");
            CHECK_EQ(std::string(Access::plutoDraft(a)), "ip:192.168.2.17");
            frame(a);
            CHECK(!Access::plutoPending(a));
            CHECK_EQ(std::string(Access::plutoDraft(a)), "ip:192.168.2.17");
            // The engine's own copy is what Open will use if the window's draft is stale?: no - Open sends the DRAFT.
            top = sweepSection(a);
            if (top.size() >= 5u) {
                CHECK_EQ(sentText(clickSent(a, top[4].x, top[4].y)),
                         std::vector<std::string>{expect("SELECT_SOURCE", 0, 0, "open-pluto:uri=ip:192.168.2.17")});
                Access::dropQueued(a);
            }
        }
        // An address changed ELSEWHERE (the web remote's open, a config reload) shows in the box that is not being typed in.
        (void)Access::command(a, cmd::makeText(FOXAPP_OP_SET_PLUTO_URI, "ip:10.0.0.9"));
        frame(a);
        frame(a);
        CHECK_EQ(std::string(Access::plutoDraft(a)), "ip:10.0.0.9");
        CHECK(has(shot(a), "ip:10.0.0.9"));
    }

    std::printf("  F3: an edit the engine never took is not held for ever: after two seconds the engine's address is the box's\n");
    {
        std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 5u);
        if (top.size() >= 5u) {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(top[3].x, top[3].y);
            drawOnly(a);
            drawOnly(a);
            io.AddMouseButtonEvent(0, true);
            drawOnly(a);
            io.AddMouseButtonEvent(0, false);
            drawOnly(a);
            io.AddKeyEvent(ImGuiKey_End, true);
            drawOnly(a);
            io.AddKeyEvent(ImGuiKey_End, false);
            drawOnly(a);
            io.AddInputCharactersUTF8("1");
            drawOnly(a);
            Access::dropQueued(a);  // (the session is dropped: the engine never sees the edit)
            io.AddMousePosEvent(300.0f, 900.0f);
            drawOnly(a);
            io.AddMouseButtonEvent(0, true);
            drawOnly(a);
            io.AddMouseButtonEvent(0, false);
            drawOnly(a);
            putAway();
            CHECK(Access::plutoPending(a));
            CHECK_EQ(std::string(Access::plutoDraft(a)), "ip:10.0.0.91");
            for (int i = 0; i < 100; ++i) { frame(a); }  // (1.7 s)
            CHECK(Access::plutoPending(a));
            for (int i = 0; i < 40; ++i) { frame(a); }   // (2.4 s)
            CHECK(!Access::plutoPending(a));
            CHECK_EQ(std::string(Access::plutoDraft(a)), "ip:10.0.0.9");
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// G. THE SOUND CARD
// ---------------------------------------------------------------------------------------------------------------------

std::string unitSep() { return std::string(1, '\x1F'); }

// The sound card form command one field of which is carried (what the engine's table takes).
cmd::QueuedCommand formField(const SoundCardSettings& s, std::int64_t field) { return cascade::gui::soundCardFormCommand(s, field); }

// Types into the box at `h` after clicking it: the whole text replaced, Return pressed. The commands it sent are in `sent`.
void typeInto(AppWindow& a, const Hit& h, const char* text, bool enter, Access::Sent& sent) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(h.x, h.y);
    drawOnly(a);
    drawOnly(a);
    io.AddMouseButtonEvent(0, true);
    drawOnly(a);
    io.AddMouseButtonEvent(0, false);
    drawOnly(a);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    drawOnly(a);
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    drawOnly(a);
    Access::tap(a, &sent);
    io.AddInputCharactersUTF8(text);
    drawOnly(a);
    if (enter) {
        io.AddKeyEvent(ImGuiKey_Enter, true);
        drawOnly(a);
        io.AddKeyEvent(ImGuiKey_Enter, false);
        drawOnly(a);
    }
    Access::tap(a, nullptr);
    putAway();
    drawOnly(a);
}

void testSoundCard() {
    std::printf("  G1: the sound card row: the engine's pass lists the inputs - the window sends no command a frame for it\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    scanAll(a);
    CHECK_EQ(world::get().listCalls.load(), 0);
    Access::Sent sent;
    Access::tap(a, &sent);
    selectRowId(a, "row:soundcard");
    CHECK(passUntil(a, [&] { return srcFlag(a, FOXAPI_SRC_CARD_LISTED); }));
    pass(a);
    for (int i = 0; i < 120; ++i) { frame(a); }
    Access::tap(a, nullptr);
    CHECK_EQ(world::get().listCalls.load(), 1);  // listed once, by the pass
    CHECK_EQ(sentText(sent).size(), 1u);          // (the selection itself: nothing a frame, no SOUND_CARDS_WANTED)
    CHECK_EQ(countOf(sent, "APP_SOUND_CARDS_WANTED"), 0u);
    CHECK_EQ(Access::lists(a).row, 2);
    {
        const auto& c = Access::lists(a);
        CHECK_EQ(c.cards.size(), 3u);
        if (c.cards.size() == 3u) {
            CHECK_EQ(c.cards[0].name, "Fake In A (Fake API)");
            CHECK_EQ(c.cards[1].name, "Fake In B (Fake API)");
            CHECK_EQ(c.cards[2].id, 3u);
        }
        CHECK_EQ(c.cardAt, 0);  // no card named: the system's default input is the form's
        CHECK_EQ(c.cardRates.size(), 3u);
        CHECK(c.form.device.empty());
        CHECK(!c.form.iqStereo);
        const std::string t = shot(a);
        CHECK(has(t, "Real (mono)"));
        CHECK(has(t, "Left"));
        CHECK(has(t, "Sample rate") || has(t, "Open"));
        CHECK(!has(t, "Centre (MHz)"));
        CHECK(!has(t, "Swap I/Q"));
        CHECK(!has(t, "Opening "));
        CHECK(!has(t, "No sound card inputs found."));
        CHECK(has(t, "Receives "));
    }

    std::printf("  G2: a click on a card sends the engine's pick (the row that is already the form's sends nothing)\n");
    {
        std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 8u);
        if (top.size() >= 8u) {
            std::vector<Hit> rows = openAt(a, top[3]);  // the Device combo
            CHECK_EQ(rows.size(), 3u);
            if (rows.size() == 3u) {
                const std::string t = shot(a);
                CHECK(has(t, "Fake In A (Fake API)"));
                CHECK(has(t, "Fake In B (Fake API)"));
                CHECK(has(t, "Fake In C (Fake API)"));
                // The form's card is A (the default input): a click on its row does nothing, as it never did.
                CHECK(sentText(clickSent(a, rows[0].x, rows[0].y)).empty());
                closePopups(a);
                Access::cardMissing(a) = "Fake In B is not connected.";
                pass(a);
                pass(a);
                CHECK(has(shot(a), "Fake In B is not connected."));
                rows = openAt(a, top[3]);
                CHECK_EQ(rows.size(), 3u);
                if (rows.size() == 3u) {
                    CHECK_EQ(sentText(clickSent(a, rows[1].x, rows[1].y)),
                             std::vector<std::string>{expect("APP_SOUND_CARD_PICK", 2, 0, "")});
                    // Nothing in the click's frame...
                    CHECK(Access::lists(a).form.device.empty());
                    CHECK_EQ(Access::lists(a).cardAt, 0);
                    // ...and the engine's form the frame after: the card, chosen from the list, with its own default rate.
                    const std::string after = shot(a);
                    const auto& c = Access::lists(a);
                    CHECK_EQ(c.form.device, "Fake In B");
                    CHECK_EQ(c.form.hostApi, "Fake API");
                    CHECK(c.form.picked);
                    CHECK_EQ(c.cardAt, 1);
                    CHECK_NEAR(c.form.rateHz, 192000.0, 0.5);  // (48 kHz is not B's: its default)
                    CHECK_EQ(c.cardLabel, "Fake In B (Fake API)");
                    CHECK(has(after, "Fake In B (Fake API)"));
                    CHECK(!has(after, "Fake In B is not connected."));  // the pick cleared the line
                    CHECK(has(after, "192 kHz"));
                }
            }
        }
        closePopups(a);
        Access::dropQueued(a);
        pass(a);
        pass(a);
        CHECK_EQ(Access::lists(a).cardAt, 1);
    }

    std::printf("  G3: the rate a pick lands on is the engine's rule: keep, else the card's default, else its highest\n");
    {
        const auto setRate = [&](double hz) {
            SoundCardSettings s;
            s.cardRateHz = hz;
            Access::submit(a, formField(s, FOXAPP_SOUND_CARD_FIELD_RATE));
            pass(a);
        };
        const auto pick = [&](int id) {
            Access::submit(a, cmd::makeInt(FOXAPP_OP_SOUND_CARD_PICK, id));
            pass(a);
            pass(a);
        };
        setRate(96000.0);
        pick(1);  // A offers 96 kHz (in exclusive mode)
        CHECK_EQ(Access::lists(a).form.device, "Fake In A");
        CHECK_NEAR(Access::lists(a).form.rateHz, 96000.0, 0.5);
        setRate(192000.0);
        pick(1);  // A does not offer it: its default, 48 kHz
        CHECK_NEAR(Access::lists(a).form.rateHz, 48000.0, 0.5);
        pick(3);  // C offers 32 and 64 kHz, its default (50) is not among them: its highest
        CHECK_EQ(Access::lists(a).form.device, "Fake In C");
        CHECK_NEAR(Access::lists(a).form.rateHz, 64000.0, 0.5);
        setRate(44100.0);
        pick(2);  // B offers neither: its default, 192 kHz
        CHECK_NEAR(Access::lists(a).form.rateHz, 192000.0, 0.5);
        // No such entry: nothing changes.
        (void)Access::answers(a);
        pick(9);
        CHECK(answered(a, FOXAPP_OP_SOUND_CARD_PICK, FOXAPI_NOT_FOUND));
        CHECK_EQ(Access::lists(a).form.device, "Fake In B");
    }

    std::printf("  G4: each control sends ONE field of the form: what the pass changed since the window read it is not written back\n");
    {
        pass(a);
        std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 8u);
        if (top.size() >= 8u) {
            std::vector<Hit> rows = openAt(a, top[4]);  // the Format combo
            CHECK_EQ(rows.size(), 2u);
            if (rows.size() == 2u) {
                // The engine's form moves behind the window's cache (a worker's open result does exactly this): the
                // rate and the card.
                SoundCardSettings other;
                other.device = "Fake In C";
                other.hostApi = "Fake API";
                other.cardRateHz = 64000.0;
                other.pickedFromList = true;
                (void)Access::command(a, cascade::gui::soundCardFormCommand(other));
                CHECK_NEAR(Access::form(a).cardRateHz, 64000.0, 0.5);
                const Access::Sent clicked = clickSent(a, rows[1].x, rows[1].y);  // I/Q (stereo)
                CHECK_EQ(sentText(clicked), std::vector<std::string>{expect("APP_SOUND_CARD_FORM", 1, FOXAPP_SOUND_CARD_FIELD_FORMAT, unitSep())});
                CHECK(Access::lists(a).form.device == "Fake In B");  // (the window's copy is the stale one)
                // A Format click shows the OLD dependent controls for one more pass (recorded): the Channel combo is
                // still where Swap I/Q and the centre will be.
                imguiFrame(a, true);
                CHECK(has(g_text, "Left"));
                CHECK(!has(g_text, "Swap I/Q"));
            }
        }
        closePopups(a);
        pass(a);
        pass(a);
        // The engine's own values stand: only the format changed.
        CHECK(Access::form(a).format == SoundCardFormat::IqStereo);
        CHECK_EQ(Access::form(a).device, "Fake In C");
        CHECK_NEAR(Access::form(a).cardRateHz, 64000.0, 0.5);
        CHECK(Access::lists(a).form.iqStereo);
        CHECK_EQ(Access::lists(a).form.device, "Fake In C");
    }

    std::printf("  G5: the I/Q form: Swap I/Q and the centre; the centre is a draft until the engine has it\n");
    {
        std::string t = shot(a);
        CHECK(has(t, "[ ] Swap I/Q"));
        CHECK(has(t, "Centre (MHz)"));
        CHECK(has(t, "I/Q (stereo)"));
        CHECK(!has(t, "Channel"));
        CHECK(!has(t, "Left"));
        std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 9u);
        if (top.size() >= 9u) {
            // Swap I/Q: one field.
            const Access::Sent sw = clickSent(a, top[5].x, top[5].y);
            CHECK_EQ(sentText(sw), std::vector<std::string>{expect("APP_SOUND_CARD_FORM", 4, FOXAPP_SOUND_CARD_FIELD_SWAP_IQ, unitSep())});
            Access::dropQueued(a);
            // The centre: typed, Return sends the form's centre and then the live one.
            Access::Sent typed;
            typeInto(a, top[6], "7.1", true, typed);
            CHECK_EQ(typed.size(), 2u);
            if (typed.size() == 2u) {
                CHECK_EQ(std::string(cmd::opName(typed[0].c.op)), "APP_SOUND_CARD_FORM");
                CHECK_EQ(typed[0].c.ival[1], FOXAPP_SOUND_CARD_FIELD_CENTRE);
                CHECK_NEAR(typed[0].c.num[1], 7.1e6, 0.5);
                CHECK_EQ(std::string(cmd::opName(typed[1].c.op)), "APP_SOUNDCARD_IQ_CENTRE");
                CHECK_NEAR(typed[1].c.num[0], 7.1e6, 0.5);
            }
            CHECK(Access::centrePending(a));
            CHECK_NEAR(Access::centreMhz(a), 7.1, 1e-9);
            // The pass that follows takes the form; the window's figure never went back to the engine's old one.
            CHECK_NEAR(Access::lists(a).form.centreHz, 0.0, 0.5);
            drawOnly(a);
            CHECK_NEAR(Access::centreMhz(a), 7.1, 1e-9);
            t = shot(a);
            CHECK_NEAR(Access::lists(a).form.centreHz, 7.1e6, 0.5);
            CHECK(has(t, "7.100000"));
            frame(a);
            CHECK(!Access::centrePending(a));
            CHECK_NEAR(Access::centreMhz(a), 7.1, 1e-9);
            // No I/Q card is running: the live retune is answered NO_CHANGE, which the window never asks first.
            CHECK(answered(a, FOXAPP_OP_SOUNDCARD_IQ_CENTRE, FOXAPI_NO_CHANGE));
            // A figure the engine never took is not held for ever: after two seconds the box shows the engine's.
            Access::Sent lost;
            typeInto(a, top[6], "9.9", true, lost);
            Access::dropQueued(a);
            CHECK(Access::centrePending(a));
            CHECK_NEAR(Access::centreMhz(a), 9.9, 1e-9);
            for (int i = 0; i < 100; ++i) { frame(a); }
            CHECK(Access::centrePending(a));
            for (int i = 0; i < 40; ++i) { frame(a); }
            CHECK(!Access::centrePending(a));
            CHECK_NEAR(Access::centreMhz(a), 7.1, 1e-9);
        }
    }

    std::printf("  G6: \"Receives X to Y.\" is the engine's span: before and after a converter kept for the card\n");
    {
        const auto expected = [&](const SoundCardSettings& s, const cascade::core::ConverterSetting& conv) {
            const cascade::gui::SoundCardAirSpan span = cascade::gui::soundCardAirSpan(s, conv);
            return "Receives " + cascade::gui::soundCardHzText(span.loHz) + " to " + cascade::gui::soundCardHzText(span.hiHz) + ".";
        };
        SoundCardSettings s = Access::form(a);
        CHECK(s.format == SoundCardFormat::IqStereo);
        std::string t = shot(a);
        CHECK_EQ(Access::receives(a), expected(s, cascade::core::ConverterSetting{}));
        CHECK(has(t, Access::receives(a)));
        // Real mode, with a down-converter stored for the card: the air is above what the card hears.
        SoundCardSettings real;
        real.cardRateHz = 96000.0;
        Access::submit(a, formField(real, FOXAPP_SOUND_CARD_FIELD_FORMAT | FOXAPP_SOUND_CARD_FIELD_RATE));
        const std::string key = cascade::gui::soundCardConverterKey("Fake In C", "Fake API");
        const cascade::core::ConverterSetting down{cascade::core::ConverterMode::Down, 100.0e6, false};
        Access::converters(a)[key] = down;
        pass(a);
        pass(a);
        s = Access::form(a);
        CHECK(s.format == SoundCardFormat::RealMono);
        t = shot(a);
        const std::string want = expected(s, down);
        CHECK_EQ(Access::receives(a), want);
        CHECK(has(t, want));
        CHECK(want != expected(s, cascade::core::ConverterSetting{}));  // (the converter is in it)
        Access::converters(a).erase(key);
        pass(a);
        pass(a);
        CHECK_EQ(Access::receives(a), expected(s, cascade::core::ConverterSetting{}));
    }

    std::printf("  G7: Open sends the engine's own form; while the card opens its controls are greyed and the lamp stays dark\n");
    {
        world::get().openGate = false;
        std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 7u);
        if (top.size() >= 7u) {
            // Real mode: the combo, Refresh, the network box, Device, Format, Channel, Sample rate, Open.
            CHECK(top.size() >= 8u);
            const Access::Sent opened = clickSent(a, top[7].x, top[7].y);
            CHECK_EQ(sentText(opened), std::vector<std::string>{expect("SELECT_SOURCE", 0, 0, "soundcard:open")});
        }
        CHECK(passUntil(a, [&] { return srcFlag(a, FOXAPI_SRC_CARD_OPENING); }));
        Access::keepKind(a) = "soundcard";  // (a restored card brought back: the lamp must stay dark meanwhile)
        Access::keepLabel(a) = "Fake In C";
        pass(a);
        const std::string t = shot(a);
        CHECK(has(t, "Opening Fake In C (Fake API)..."));
        CHECK(!Access::lamp(a));
        CHECK(!has(t, "(saved, not open)"));
        const std::vector<Found> found = survey(a);
        sameCommands(commandsOf(found), topKeys(), "G7 card opening");  // (the card's own controls send nothing)
        Access::keepKind(a).clear();
        Access::keepLabel(a).clear();
        world::get().openGate = true;
        CHECK(passUntil(a, [&] { return !srcFlag(a, FOXAPI_SRC_CARD_OPENING) && Access::sourceKind(a) == "soundcard"; }));
        pass(a);
        CHECK_EQ(Access::liveCard(a).device, "Fake In C");
        CHECK(!has(shot(a), "Opening "));
        CHECK_EQ(world::get().openCalls.load(), 1);
        selectRowId(a, "siggen");
    }

    std::printf("  G8: no card to list: the line, and Open is greyed\n");
    {
        Access::install();
        world::get().cards.clear();
        Env env2;
        AppWindow& b = env2.app;
        scanAll(b);
        // Before the cards are asked for, PICK has nothing to name.
        (void)Access::answers(b);
        Access::submit(b, cmd::makeInt(FOXAPP_OP_SOUND_CARD_PICK, 1));
        pass(b);
        pass(b);
        CHECK(answered(b, FOXAPP_OP_SOUND_CARD_PICK, FOXAPI_NOT_FOUND));
        selectRowId(b, "row:soundcard");
        CHECK(passUntil(b, [&] { return srcFlag(b, FOXAPI_SRC_CARD_LISTED); }));
        pass(b);
        const std::string t = shot(b);
        CHECK(has(t, "No sound card inputs found."));
        CHECK_EQ(Access::lists(b).cards.size(), 0u);
        CHECK_EQ(Access::lists(b).cardAt, -1);
        const std::vector<Found> found = survey(b);
        sameCommands(commandsOf(found), topKeys(), "G8 no card");  // (Open is greyed: it names no card)
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// H. A WIDGET SWEEP OF EVERY STATE: exactly the commands each control sends
// ---------------------------------------------------------------------------------------------------------------------

void testSweeps() {
    Access::install();
    Env env;
    AppWindow& a = env.app;
    scanAll(a);

    std::printf("  H1: the generator: the combo, Refresh and the network box - and the converter's combo, which sends nothing\n");
    {
        const std::vector<Found> found = survey(a);
        sameCommands(commandsOf(found), topKeys(), "H1 generator");
        CHECK(found.size() >= 4u);  // (...the converter combo is a widget that sent nothing)
    }

    std::printf("  H2: the Pluto's row: the address box sends nothing, Open sends the draft\n");
    {
        selectRowId(a, "pluto:uri=ip:192.168.2.1");
        CHECK_EQ(Access::lists(a).row, 5);
        const std::vector<Found> found = survey(a);
        sameCommands(commandsOf(found), withTop({expect("SELECT_SOURCE", 0, 0, "open-pluto:uri=ip:192.168.2.1")}), "H2 pluto");
    }

    std::printf("  H3: a radio open: its rate combo sends nothing, Auto gain and the stage's slider send theirs\n");
    {
        openFakeRadio(a);
        const std::vector<Found> found = survey(a);
        sameCommands(commandsOf(found), withTop({expect("SET_DEVICE_AGC", 1, 0, ""), expect("SET_GAIN", 0, 0, "TUNER")}), "H3 radio");
        selectRowId(a, "siggen");
        CHECK(passUntil(a, [&] { return !rxFlag(a, FOXAPI_RX_DEVICE_OPEN); }));
        pass(a);
    }

    std::printf("  H4: the sound card, real and I/Q: every control sends the one field it changed\n");
    {
        selectRowId(a, "row:soundcard");
        CHECK(passUntil(a, [&] { return srcFlag(a, FOXAPI_SRC_CARD_LISTED); }));
        pass(a);
        pass(a);
        const std::vector<Found> real = survey(a);
        // (the three combos - the device, the format, the channel - and the rate's open and send nothing)
        sameCommands(commandsOf(real), withTop({expect("SELECT_SOURCE", 0, 0, "soundcard:open")}), "H4 real");
        SoundCardSettings iq;
        Access::submit(a, formField(iq, FOXAPP_SOUND_CARD_FIELD_FORMAT));  // (ival[0] bit 1 clear: real; set it: I/Q)
        SoundCardSettings s;
        s.format = SoundCardFormat::IqStereo;
        Access::submit(a, formField(s, FOXAPP_SOUND_CARD_FIELD_FORMAT));
        pass(a);
        pass(a);
        CHECK(Access::lists(a).form.iqStereo);
        const std::vector<Found> stereo = survey(a);
        sameCommands(commandsOf(stereo),
                     withTop({expect("SELECT_SOURCE", 0, 0, "soundcard:open"),
                              expect("APP_SOUND_CARD_FORM", 4, FOXAPP_SOUND_CARD_FIELD_SWAP_IQ, unitSep())}),
                     "H4 I/Q");
        selectRowId(a, "siggen");
    }

    std::printf("  H5: a fault does not stop the controls: the same keys, and the fault says why\n");
    {
        Access::testFault(a) = "usb: the device was removed while streaming";
        pass(a);
        pass(a);
        const std::vector<Found> found = survey(a);
        sameCommands(commandsOf(found), topKeys(), "H5 fault");
        Access::testFault(a).clear();
        pass(a);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// I. A CLICK SHOWS NOTHING IN ITS FRAME, AND THE ENGINE'S VALUE THE FRAME AFTER THE PASS
// ---------------------------------------------------------------------------------------------------------------------

void testClicksLand() {
    std::printf("  I1: Refresh and the combo's rows: the scan and the selection happen in the engine's pass, not the click's frame\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    scanAll(a);
    {
        const int scans = world::get().soapyCalls.load();
        std::vector<Hit> top = sweepSection(a);
        CHECK(top.size() >= 3u);
        if (top.size() >= 3u) {
            clickThenHold(a, top[1].x, top[1].y);  // Refresh
            CHECK_EQ(world::get().soapyCalls.load(), scans);  // nothing yet
            CHECK(!srcFlag(a, FOXAPI_SRC_SCAN_PENDING));
            frame(a);  // the engine's next pass takes it
            CHECK(passUntil(a, [&] { return world::get().soapyCalls.load() == scans + 1; }));
        }
        CHECK(passUntil(a, [&] { return !srcFlag(a, FOXAPI_SRC_SCAN_PENDING); }));
        pass(a);
    }
    {
        // A row of the combo: the selection is the engine's a pass later.
        std::vector<Hit> top = sweepSection(a);
        CHECK(!top.empty());
        if (!top.empty()) {
            ImGuiIO& io = ImGui::GetIO();
            io.AddMousePosEvent(top[0].x, top[0].y);
            frame(a);
            frame(a);
            io.AddMouseButtonEvent(0, true);
            frame(a);
            io.AddMouseButtonEvent(0, false);
            frame(a);
            frame(a);
            putAway();
            const std::vector<Hit> rows = sweepPopup(a);
            CHECK_EQ(rows.size(), 8u);
            if (rows.size() == 8u) {
                // IQ file: the row's own panel is drawn only once the engine's row is the IQ file's.
                io.AddMousePosEvent(rows[1].x, rows[1].y);
                frame(a);
                frame(a);
                io.AddMouseButtonEvent(0, true);
                frame(a);
                io.AddMouseButtonEvent(0, false);
                drawOnly(a);
                putAway();
                CHECK_EQ(Access::lists(a).row, 0);
                imguiFrame(a, true);  // (no pass: what the window draws is still the generator's section)
                CHECK(!has(g_text, "Open"));
                const std::string t = shot(a);  // the engine's next pass
                CHECK_EQ(Access::lists(a).row, 1);
                CHECK(has(t, "Open"));
            }
        }
        closePopups(a);
        selectRowId(a, "siggen");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// J. THE CACHE: ONE READ WHEN LISTSEQ MOVES, NONE OTHERWISE
// ---------------------------------------------------------------------------------------------------------------------

void testCache() {
    std::printf("  J1: a settled window reads nothing: the cache is read when listSeq moves, once, and allocates nothing otherwise\n");
    Access::install();
    world::get().soapy.clear();  // (a scan that finds nothing: the evidence is taken)
    world::get().vendors = {{"PothosSDR", "C:/Pothos", "", ""}};
    world::get().modules = {"a.dll", "b.dll"};
    Env env;
    AppWindow& a = env.app;
    scanAll(a);
    selectRowId(a, "row:soundcard");
    CHECK(passUntil(a, [&] { return srcFlag(a, FOXAPI_SRC_CARD_LISTED); }));
    pass(a);
    pass(a);
    // A busy world: a radio's lists too (the sound card's are there already).
    {
        Access::antennas(a) = {"RX", "TX/RX"};
        pass(a);
    }
    const std::uint64_t seq = Access::listSeq(a);
    long before = t_allocs;
    const auto& c = Access::lists(a);  // (the first read of all: every list and fact, into strings that have no room yet)
    const long cold = t_allocs - before;
    std::printf("      the first read, cold: %ld allocations (6 rows, 3 cards, the evidence, the facts)\n", cold);
    CHECK(cold < 400);
    std::printf("      rows %zu cards %zu evidence %zu\n", c.rows.size(), c.cards.size(), c.evidence.size());
    CHECK(c.rows.size() == 6u && c.cards.size() == 3u && c.evidence.size() == 3u);
    const std::uint32_t reads = c.refreshes;
    before = t_allocs;
    for (int i = 0; i < 1000; ++i) { (void)Access::lists(a); }
    const long settled = t_allocs - before;
    std::printf("      1000 settled reads: %ld allocations, %u refreshes\n", settled, c.refreshes - reads);
    CHECK_EQ(settled, 0L);
    CHECK_EQ(c.refreshes, reads);
    CHECK_EQ(Access::listSeq(a), seq);

    std::printf("  J2: a move of listSeq is one read of everything, bounded\n");
    {
        // Settle the strings' room first (a second identical rebuild is the steady cost), then count.
        for (int round = 0; round < 3; ++round) {
            Access::keepKind(a) = round % 2 == 0 ? "soapy" : "";
            pass(a);
            (void)Access::lists(a);
        }
        long worst = 0;
        for (int round = 0; round < 6; ++round) {
            Access::keepKind(a) = round % 2 == 0 ? "" : "soapy";
            pass(a);
            const std::uint64_t moved = Access::listSeq(a);
            const std::uint32_t r0 = c.refreshes;
            before = t_allocs;
            (void)Access::lists(a);
            const long cost = t_allocs - before;
            worst = std::max(worst, cost);
            if (c.refreshes != r0 + 1u) { std::printf("      round %d: refreshes %u -> %u, listSeq %llu\n", round, r0, c.refreshes, static_cast<unsigned long long>(moved)); }
            CHECK_EQ(c.refreshes, r0 + 1u);  // (once, however often it is asked)
            (void)Access::lists(a);
            CHECK_EQ(c.refreshes, r0 + 1u);
            (void)moved;
        }
        std::printf("      a rebuild: at most %ld allocations (the table's own get_setting text; the strings keep their room)\n", worst);
        CHECK(worst < 120);
    }

    std::printf("  J3: a drawn frame over an unchanged state allocates only what the draw itself builds\n");
    {
        frame(a);
        frame(a);
        before = t_allocs;
        for (int i = 0; i < 20; ++i) { imguiFrame(a); }
        std::printf("      20 section frames (the sound card's panel, a radio's lists): %ld allocations\n", t_allocs - before);
        CHECK(t_allocs - before < 20 * 120);
    }
    selectRowId(a, "siggen");
}


void testLanguage() {
    std::printf("  J4: a language switch rebuilds the captions (the engine's fingerprint hashes the language): \"(native)\" is composed in it\n");
    Access::install();
    Env env;
    AppWindow& a = env.app;
    scanAll(a);
    CHECK_EQ(Access::lists(a).rows[3].caption, "Mock RTL-SDR (serial 00000001) (native)");
    const std::uint32_t r0 = Access::cache(a).refreshes;
    (void)cascade::i18n::setLanguage("de");
    CHECK_EQ(cascade::i18n::current(), "de");
    pass(a);
    pass(a);
    const std::string german = Access::lists(a).rows[3].caption;
    CHECK(Access::cache(a).refreshes > r0);
    CHECK_EQ(german, "Mock RTL-SDR (serial 00000001) (nativ)");
    (void)cascade::i18n::setLanguage("en");
    pass(a);
    pass(a);
    CHECK_EQ(Access::lists(a).rows[3].caption, "Mock RTL-SDR (serial 00000001) (native)");
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_source_section_window\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_srcwin_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1200.0f, 3400.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());

    testCombo();
    testScanStates();
    testPartialAndNetworkBox();
    testNoHardware();
    testFaultAndLamp();
    testRadioPanel();
    testIqFileAndPluto();
    testSoundCard();
    testSweeps();
    testClicksLand();
    testCache();
    testLanguage();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_source_section_window");
}
