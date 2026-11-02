// foxsdr-slint - THE SLINT WINDOW (phase 1 of the port, docs/engine-stage4.md 6.36): the Workbench main screen as
// a client of the C API table.
//
// The host owns an EngineService exactly as src/main.cpp does, opens a LOCAL session with VIEW|TUNE|SETTINGS, and
// on a 33 ms slint::Timer heartbeats, read_state()s into the window's properties (through host_logic.hpp, the
// tested unit), read_list()s the lists the screen draws when listSeq moves, read_spectrum()s on a NEW frame only
// (painting into two persistent SharedPixelBuffers per well, alternated so a buffer the renderer still holds is
// never written: the detach count printed at exit is the proof), and drains poll_results. Every key, gesture and
// shortcut of ui/main.slint becomes one submit() here; the app-level opcodes (click-to-tune, edge drag, display
// floor/ceiling, markers) come from core/app_commands.hpp, a constants-only header, through the same submit().
// Nothing of the engine is reached except through the table.
//
// THE REAL SETTINGS FILE IS NEVER TOUCHED. This window ships as a labelled PREVIEW beside the ImGui window, and
// launching it is the opt-in. Its settings live in a folder of their own, %APPDATA%\foxsdr\slint-preview\ (the
// engine keeps every side file - markers.json, recordings' notes - beside its config path, so the folder holds
// all of them): on the first run that folder is seeded with a READ-ONLY copy of the user's config.json and
// markers.json, so the preview starts with their radio, bookmarks and markers, and from then on it reads and
// writes only its own copies. --config <path> or FOXSDR_SLINT_CONFIG name another file (the scratch runs), and
// a path that resolves to the user's own %APPDATA%\foxsdr\config.json is refused outright unless
// FOXSDR_SLINT_REAL=1 (nothing here sets it). slint-run.bat additionally redirects APPDATA/LOCALAPPDATA/
// USERPROFILE to a scratch profile, as the GUI test gate does.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <slint.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "foxsdr_api.h"
#include "core/app_commands.hpp"
#include "core/version.hpp"
#include "engine/engine_service.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/window_service_options.hpp"
#include "host_logic.hpp"

#include "main.h"  // generated from ui/main.slint by slint_target_sources

using namespace foxsdr::slinthost;

namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

// The settings file: never the product's. See the header comment. `seeded` reports what the first run copied.
std::string configPathFor(int argc, char** argv, std::string& seeded) {
    for (int i = 1; i + 1 < argc; ++i) { if (std::strcmp(argv[i], "--config") == 0) { return argv[i + 1]; } }
    if (const char* p = std::getenv("FOXSDR_SLINT_CONFIG"); p != nullptr && *p != '\0') { return p; }
    const char* appdata = std::getenv("APPDATA");
    if (appdata == nullptr || *appdata == '\0') {
        std::filesystem::path exe = std::filesystem::absolute(argc > 0 && argv[0] != nullptr ? argv[0] : ".");
        return (exe.parent_path() / "slint-preview-config.json").string();
    }
    const std::filesystem::path real = std::filesystem::path(appdata) / "foxsdr";
    const std::filesystem::path preview = real / "slint-preview";
    std::error_code ec;
    std::filesystem::create_directories(preview, ec);
    const std::filesystem::path mine = preview / "config.json";
    if (!std::filesystem::exists(mine, ec)) {
        // the one-time seed: the user's own files are only ever READ here
        for (const char* name : {"config.json", "markers.json"}) {
            const std::filesystem::path from = real / name;
            if (std::filesystem::exists(from, ec) && std::filesystem::is_regular_file(from, ec)) {
                if (std::filesystem::copy_file(from, preview / name, std::filesystem::copy_options::skip_existing, ec) && !ec) {
                    seeded += std::string(seeded.empty() ? "" : ", ") + name;
                }
            }
        }
    }
    return mine.string();
}

bool isTheRealConfig(const std::string& path) {
    const char* appdata = std::getenv("APPDATA");
    if (appdata == nullptr) { return false; }
    std::error_code ec;
    const auto real = std::filesystem::weakly_canonical(std::filesystem::path(appdata) / "foxsdr" / "config.json", ec);
    const auto mine = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    return lower(real.string()) == lower(mine.string());
}

std::string fmt(const char* f, double v) { char b[64]; std::snprintf(b, sizeof(b), f, v); return b; }
slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

// Publishing a list model replaces every repeater item behind it, so a model is set only when its content changed:
// each publisher keeps the signature of what it last sent; changed() says whether `sig` differs from the stored one.
struct Signatures {
    std::map<std::string, std::string> last;
    bool changed(const std::string& key, const std::string& sig) {
        auto it = last.find(key);
        if (it != last.end() && it->second == sig) { return false; }
        last[key] = sig;
        return true;
    }
};
std::string sigOf(const std::vector<Tick>& t) { std::string s; for (const auto& k : t) { s += fmt("%.6f|", k.frac) + k.label + ";"; } return s; }
std::int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

constexpr std::uint32_t kSpecW = 1024, kSpecH = 340;   // the spectrum trace's painted resolution (image-fit fill scales it)
constexpr std::uint32_t kWfW = 1024, kWfH = 238;       // the waterfall picture: one row per published frame
constexpr double kZoomPerNotch = 1.3;
static_assert(sizeof(Rgba) == sizeof(slint::Rgba8Pixel));

// Two persistent buffers per well, alternated: the one the window still shows is never written, so begin() never
// detaches (copies) and nothing is allocated per frame. `detaches` counts the times it did anyway.
struct WellBuffers {
    std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> trace{slint::SharedPixelBuffer<slint::Rgba8Pixel>(kSpecW, kSpecH), slint::SharedPixelBuffer<slint::Rgba8Pixel>(kSpecW, kSpecH)};
    std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> waterfall{slint::SharedPixelBuffer<slint::Rgba8Pixel>(kWfW, kWfH), slint::SharedPixelBuffer<slint::Rgba8Pixel>(kWfW, kWfH)};
    std::array<const void*, 2> tracePtr{nullptr, nullptr};
    std::array<const void*, 2> wfPtr{nullptr, nullptr};
    std::vector<Rgba> picture = std::vector<Rgba>(kWfW * kWfH, Rgba{0x06, 0x07, 0x0a, 255});  // the canonical waterfall picture
    int which = 0;
    int detaches = 0;
    int frames = 0;

    Rgba* traceMemory() {
        auto& b = trace[static_cast<std::size_t>(which)];
        auto* p = b.begin();
        if (tracePtr[static_cast<std::size_t>(which)] != nullptr && tracePtr[static_cast<std::size_t>(which)] != p) { ++detaches; }
        tracePtr[static_cast<std::size_t>(which)] = p;
        return reinterpret_cast<Rgba*>(p);
    }
    Rgba* waterfallMemory() {
        auto& b = waterfall[static_cast<std::size_t>(which)];
        auto* p = b.begin();
        if (wfPtr[static_cast<std::size_t>(which)] != nullptr && wfPtr[static_cast<std::size_t>(which)] != p) { ++detaches; }
        wfPtr[static_cast<std::size_t>(which)] = p;
        return reinterpret_cast<Rgba*>(p);
    }
    slint::Image traceImage() { return slint::Image(trace[static_cast<std::size_t>(which)]); }
    slint::Image waterfallImage() { return slint::Image(waterfall[static_cast<std::size_t>(which)]); }
    void flip() { which ^= 1; ++frames; }
};

// What the host reads from the table's lists when listSeq moves.
struct ListsSnapshot {
    std::string gainStage, gainUnit; double gainMin = 0, gainMax = 1, gainStep = 1; bool haveGain = false;
    std::string antenna; int antennaCount = 0;
    bool converter = false; std::string converterName; double converterLoHz = 0;
    struct Band { std::string name; double startHz, stopHz; };
    std::vector<Band> bands;
    struct Mark { std::string name; double hz, widthHz; std::uint32_t rgba; bool span, dashed, spectrumOnly; };
    std::vector<Mark> marks;
    struct Bookmark { std::uint64_t id; std::string name; double hz; bool favourite; };
    std::vector<Bookmark> bookmarks;
    struct Marker { std::uint64_t id; std::string note; double hz; };
    std::vector<Marker> markers;
    std::vector<std::string> decodersFed;
    std::vector<std::string> muteCauses;
    std::vector<std::string> pluginNames;
    int webPort = 0; bool webListening = false; std::string webRefused;
    int catPort = 0; bool catListening = false;
    std::string recorderValue, recorderDetail; bool recording = false;
    unsigned fittedPlugins = 0;
};

enum class MenuKind { None, Counter, Trace, Marker, MarkerClear, TuneStep, App, Modes };
enum class Face { Plain, Drum, Nixie };

struct PaletteAction { std::string kind, title, detail; std::function<void()> run; };

}  // namespace

int main(int argc, char** argv) {
    using namespace cascade::engine;

    std::string seeded;
    const std::string configPath = configPathFor(argc, argv, seeded);
    if (isTheRealConfig(configPath) && std::getenv("FOXSDR_SLINT_REAL") == nullptr) {
        std::fprintf(stderr, "foxsdr-slint: refusing to run on the product's own settings file (%s)\n", configPath.c_str());
        return 2;
    }
    std::fprintf(stderr, "foxsdr-slint: settings %s%s%s\n", configPath.c_str(), seeded.empty() ? "" : " (seeded from your FoxSDR settings: ", seeded.empty() ? "" : (seeded + ")").c_str());

    // THE ENGINE'S SERVICE, as src/main.cpp holds it: built first, destroyed last; the window is its client.
    WindowServiceRun run;
    run.configPath = configPath;
    run.frames = -1;
    EngineService service(windowServiceOptions(run));
    const EngineService::Restored restored = service.restore();
    (void)restored;

    const FoxEngineApi* api = engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
    FoxEngine* handle = service.engine().apiHandle();
    if (api == nullptr || handle == nullptr) { std::fprintf(stderr, "foxsdr-slint: no API table\n"); return 1; }

    FoxSessionParams p{};
    p.structSize = sizeof(p);
    p.flags = FOXAPI_SESSION_LOCAL | FOXAPI_SESSION_FRAME;   // FRAME: our heartbeat kicks the free-running pass, as the window's does
    p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS;
    p.clientName = "FoxSDR Slint interface";
    p.clientVersion = cascade::versionString();
    FoxSession* session = nullptr;
    if (api->open_session(handle, &p, &session) != FOXAPI_OK || session == nullptr) {
        std::fprintf(stderr, "foxsdr-slint: open_session failed\n");
        return 1;
    }

    std::string why;
    if (!service.start(why)) { std::fprintf(stderr, "foxsdr-slint: %s\n", why.c_str()); return 1; }

    auto ui = MainWindow::create();
    ui->set_version(ss(cascade::versionString()));
    ui->set_preview(true);
    ui->set_about_text(ss("The Slint window over the FoxSDR engine, a preview beside the FoxSDR window. It works on its own copy of your settings: " + configPath +
                          (seeded.empty() ? std::string() : " (seeded on first run from " + seeded + ")") + ". Your FoxSDR settings are never written by it."));

    // ---- host state ----
    FoxReceiverState state{}; state.structSize = sizeof(state);
    ListsSnapshot lists;
    std::uint64_t lastListSeq = ~0ull;
    WellBuffers wells;
    TraceState traceState;
    std::vector<float> bins(16384);
    std::vector<float> frame;                 // the last published frame (dBFS per bin)
    FoxSpectrumInfo lastInfo{}; lastInfo.structSize = sizeof(lastInfo);
    std::uint64_t sinceSeq = 0;
    std::deque<std::int64_t> frameTimes;      // publish stamps for the lines/s and history figures
    std::int64_t lastFrameMs = 0;
    ViewWindow view{0, 0};                    // the zoomed window; spanHz 0 = the full span
    int traceMode = 1;                        // 0 normal 1 peak hold 2 average
    double averageLengthS = 1.0;
    bool rangeManual = false;
    bool bandPlanOn = true;
    double tuneStepHz = 12500;                // Ctrl+Up/Down and the Tune step picker
    bool showNudges = true;
    Face face = Face::Plain;
    bool followBandPlan = true;
    int bankIndex = 0;
    std::map<int, std::set<int>> openSections{{0, {2}}};   // SIGNAL's Radio row open, as the sheet
    MenuKind menuKind = MenuKind::None;
    double menuHz = 0; int menuMarkerNear = -1;
    OutputRate outputRate;
    double hoverX = -1, hoverY = -1;
    std::vector<PaletteAction> paletteActions;
    std::string statusLineBand;
    Signatures sigs;

    // ---- commands: every key becomes one submit() ----
    const auto submit = [&](FoxCommand c) {
        c.structSize = sizeof(c);
        FoxSubmitResult r{}; r.structSize = sizeof(r);
        const int32_t rc = api->submit(session, &c, 1, &r);
        if (rc < 0) { std::fprintf(stderr, "foxsdr-slint: submit op 0x%04x refused by the call: %d\n", c.op, rc); }
        else if (r.status == FOXAPI_BUSY) { std::fprintf(stderr, "foxsdr-slint: submit op 0x%04x: the session's queue is full\n", c.op); }
    };
    const auto op = [&](std::uint32_t code) { FoxCommand c{}; c.op = code; return c; };
    const auto setFrequency = [&](double hz) { FoxCommand c = op(FOXAPI_OP_SET_FREQUENCY); c.num[0] = hz; submit(c); };
    const auto stepTune = [&](int dir, double step) { FoxCommand c = op(FOXAPI_OP_STEP_TUNE); c.ival[0] = dir; c.num[0] = step; submit(c); };
    const auto setMode = [&](std::uint32_t demod) { FoxCommand c = op(FOXAPI_OP_SET_MODE); c.ival[0] = static_cast<int64_t>(demod); submit(c); };
    const auto setVolume = [&](double v) { FoxCommand c = op(FOXAPI_OP_SET_VOLUME); c.num[0] = std::clamp(v, 0.0, 1.0); submit(c); };
    const auto setSquelch = [&](double db) { FoxCommand c = op(FOXAPI_OP_SET_SQUELCH); c.num[0] = db; submit(c); };
    const auto setDisplayRange = [&](double lo, double hi) { FoxCommand c = op(FOXAPI_OP_SET_DISPLAY_RANGE); c.num[0] = lo; c.num[1] = hi; submit(c); };

    // ---- the view window and its fractions ----
    const auto fullCentre = [&] { return lastInfo.spanHz > 0 ? lastInfo.centreHz : state.centreHz; };
    const auto fullSpan = [&] { return lastInfo.spanHz > 0 ? lastInfo.spanHz : state.sampleRateHz; };
    const auto currentView = [&]() -> ViewWindow {
        if (view.spanHz > 0 && view.spanHz < fullSpan()) { return view; }
        return {fullCentre(), fullSpan()};
    };
    const auto frac = [&](double hz) { const ViewWindow v = currentView(); return static_cast<float>(fracOf(hz, v.centreHz, v.spanHz)); };
    const auto hzAtFrac = [&](double f) { const ViewWindow v = currentView(); return hzAt(f, v.centreHz, v.spanHz); };
    const auto dbAtFrac = [&](double f) -> double {
        if (frame.empty() || !(lastInfo.spanHz > 0)) { return state.dbMin; }
        const double hz = hzAtFrac(f);
        const double fullFrac = fracOf(hz, lastInfo.centreHz, lastInfo.spanHz);
        const std::size_t i = static_cast<std::size_t>(std::clamp(fullFrac, 0.0, 0.999999) * static_cast<double>(frame.size()));
        return frame[std::min(i, frame.size() - 1)];
    };
    const auto zoomView = [&](double factor, double aboutFrac) {
        view = zoomAbout(currentView(), fullCentre(), fullSpan(), factor, aboutFrac, std::max(1.0, state.sampleRateHz / 1000.0));
        if (view.spanHz >= fullSpan() * 0.999) { view = {0, 0}; }
    };

    // ---- the lists, re-read when listSeq moves ----
    const auto readList = [&](std::uint32_t list, const std::function<void(const FoxListItem&)>& each) {
        for (std::uint32_t i = 0; i < 4096; ++i) {
            FoxListItem it{}; it.structSize = sizeof(it);
            const int32_t n = api->read_list(session, list, i, &it);
            if (n <= 0) { break; }
            each(it);
        }
    };
    const auto refreshLists = [&] {
        ListsSnapshot l;
        readList(FOXAPI_LIST_GAINS, [&](const FoxListItem& it) { if (!l.haveGain) { l.haveGain = true; l.gainStage = it.name; l.gainUnit = it.detail; l.gainMin = it.value[0]; l.gainMax = it.value[1]; l.gainStep = it.value[2]; } });
        readList(FOXAPI_LIST_ANTENNAS, [&](const FoxListItem& it) { ++l.antennaCount; if (it.flags & FOXAPI_ITEM_ACTIVE) { l.antenna = it.name; } });
        readList(FOXAPI_LIST_CONVERTERS, [&](const FoxListItem& it) { if ((it.flags & FOXAPI_ITEM_ACTIVE) && it.value[0] != 0) { l.converter = true; l.converterName = it.name; l.converterLoHz = it.value[1]; } });
        readList(FOXAPI_LIST_BAND_PLAN, [&](const FoxListItem& it) { l.bands.push_back({it.name, it.value[0], it.value[1]}); });
        readList(FOXAPI_LIST_MARKERS, [&](const FoxListItem& it) { l.marks.push_back({it.name, it.value[0], it.value[1], static_cast<std::uint32_t>(it.value[2]), (it.flags & FOXAPI_ITEM_SPAN) != 0, (it.flags & FOXAPI_ITEM_DASHED) != 0, (it.flags & FOXAPI_ITEM_SPECTRUM_ONLY) != 0}); });
        readList(FOXAPI_LIST_BOOKMARKS, [&](const FoxListItem& it) { l.bookmarks.push_back({it.id, it.name, it.value[0], (it.flags & FOXAPI_ITEM_FAVOURITE) != 0}); });
        readList(FOXAPI_LIST_FREQ_MARKERS, [&](const FoxListItem& it) { l.markers.push_back({it.id, it.name, it.value[0]}); });
        readList(FOXAPI_LIST_DECODERS, [&](const FoxListItem& it) { if (it.flags & FOXAPI_ITEM_FED) { l.decodersFed.push_back(it.name); } });
        readList(FOXAPI_LIST_MUTE_CAUSES, [&](const FoxListItem& it) { l.muteCauses.push_back(it.name); });
        readList(FOXAPI_LIST_PLUGINS, [&](const FoxListItem& it) { ++l.fittedPlugins; l.pluginNames.push_back(it.name); });
        readList(FOXAPI_LIST_SERVERS, [&](const FoxListItem& it) {
            if (it.id == 1) { l.webPort = static_cast<int>(it.value[0]); l.webListening = (it.flags & FOXAPI_ITEM_ACTIVE) != 0; if (it.flags & FOXAPI_ITEM_FAULT) { l.webRefused = it.detail; } }
            if (it.id == 2) { l.catPort = static_cast<int>(it.value[0]); l.catListening = (it.flags & FOXAPI_ITEM_ACTIVE) != 0; }
        });
        readList(FOXAPI_LIST_RECORDINGS, [&](const FoxListItem& it) {
            if (!(it.flags & FOXAPI_ITEM_ACTIVE)) { return; }
            l.recording = true;
            const int kind = static_cast<int>(it.value[0]);
            const std::string what = kind == 1 ? "IQ" : kind == 2 ? "AUDIO" : "PATCH";
            l.recorderValue = l.recorderValue.empty() ? what : (l.recorderValue == what ? what : "IQ + AUDIO");
            if (it.flags & FOXAPI_ITEM_FULL) { l.recorderValue = "FULL"; }
            if (it.flags & FOXAPI_ITEM_STALLED) { l.recorderValue = "STALLED"; }
            const int secs = static_cast<int>(it.value[3]);
            l.recorderDetail = fmt("%d", secs / 60) + ":" + fmt("%02d", secs % 60) + " elapsed \xC2\xB7 " + fmt("%.1f MB written", it.value[1] / 1e6);
        });
        lists = std::move(l);
    };

    // ---- the sections panel: five banks of rows from the table's state ----
    const auto sectionsFor = [&](int bank) {
        std::vector<SectionRow> rows;
        const auto add = [&](int i, const char* name, std::string chip, ChipTone tone) {
            rows.push_back({ss(name), ss(chip), tone, openSections[bank].count(i) > 0, LampState::Off, false});
        };
        const bool running = (state.flags & FOXAPI_RX_RUNNING) != 0;
        switch (bank) {
            case 0:
                add(0, "Patch", (state.flags & FOXAPI_RX_PATCH_RUNNING) ? "PATCH RUNNING" : "RECEIVER VIEW", (state.flags & FOXAPI_RX_PATCH_RUNNING) ? ChipTone::Warn : ChipTone::Neutral);
                add(1, "Source", (state.flags & FOXAPI_RX_DEVICE_OPEN) ? lower(state.deviceName).find("rtl") != std::string::npos ? "RTL-SDR" : "OPEN" : "GENERATOR", (state.flags & FOXAPI_RX_DEVICE_OPEN) ? ChipTone::Good : ChipTone::Neutral);
                add(2, "Radio", modeName(state.demodMode), ChipTone::Warn);
                add(3, "Audio filters", (state.flags & (FOXAPI_RX_NR | FOXAPI_RX_NOTCH | FOXAPI_RX_AUTO_NOTCH)) ? "ON" : "OFF", (state.flags & (FOXAPI_RX_NR | FOXAPI_RX_NOTCH | FOXAPI_RX_AUTO_NOTCH)) ? ChipTone::Good : ChipTone::Neutral);
                add(4, "Sinks", (state.flags & FOXAPI_RX_SINK_OPEN) ? "OPEN" : "CLOSED", (state.flags & FOXAPI_RX_SINK_OPEN) ? ChipTone::Good : ChipTone::Neutral);
                add(5, "Transmit", (state.flags & FOXAPI_RX_TX_AVAILABLE) ? ((state.flags & FOXAPI_RX_TX_KEYED) ? "ON AIR" : "READY") : "NO TX", (state.flags & FOXAPI_RX_TX_KEYED) ? ChipTone::Err : (state.flags & FOXAPI_RX_TX_AVAILABLE) ? ChipTone::Good : ChipTone::Neutral);
                add(6, "Recorder", lists.recording ? "REC" : "OFF", lists.recording ? ChipTone::Err : ChipTone::Neutral);
                add(7, "Frequency correction", "--", ChipTone::Neutral);   // the table does not publish the ppm (phase 2)
                break;
            case 1:
                add(0, "Plugin store", "--", ChipTone::Neutral);
                add(1, "Fitted modules", std::to_string(lists.fittedPlugins) + " FITTED", lists.fittedPlugins > 0 ? ChipTone::Good : ChipTone::Neutral);
                add(2, "Decoders", state.decodersRunning > 0 ? std::to_string(state.decodersRunning) + " RUNNING" : "IDLE", state.decodersRunning > 0 ? ChipTone::Good : ChipTone::Neutral);
                add(3, "Target details", "--", ChipTone::Neutral);
                add(4, "Maps", "--", ChipTone::Neutral);
                add(5, "Decoder output", std::to_string(state.decoderLinesTotal) + " LINES", ChipTone::Neutral);
                break;
            case 2:
                add(0, "Demod scope", "--", ChipTone::Neutral);
                add(1, "Radar", "--", ChipTone::Neutral);
                add(2, "Bookmarks", std::to_string(lists.bookmarks.size()), ChipTone::Neutral);
                add(3, "Scanner", (state.flags & FOXAPI_RX_SCANNER_ACTIVE) ? "SCANNING" : "OFF", (state.flags & FOXAPI_RX_SCANNER_ACTIVE) ? ChipTone::Good : ChipTone::Neutral);
                add(4, "Airband", "--", ChipTone::Neutral);
                break;
            case 3:
                add(0, "Web access", lists.webListening ? "PORT " + std::to_string(lists.webPort) : "OFF", lists.webListening ? ChipTone::Good : ChipTone::Neutral);
                add(1, "CAT", lists.catListening ? "PORT " + std::to_string(lists.catPort) : "OFF", lists.catListening ? ChipTone::Good : ChipTone::Neutral);
                break;
            default:
                add(0, "Display", "--", ChipTone::Neutral);
                add(1, "Language", "EN", ChipTone::Neutral);
                add(2, "Updates", "--", ChipTone::Neutral);
                add(3, "Serial ports", "--", ChipTone::Neutral);
                add(4, "Key bindings", "--", ChipTone::Neutral);
                add(5, "Diagnostics", "--", ChipTone::Neutral);
                add(6, "Usage reporting", "--", ChipTone::Neutral);
                add(7, "About", running ? "" : "", ChipTone::Neutral);
                break;
        }
        return rows;
    };

    // ---- the menus: one Menu, filled per kind, dispatched by id ----
    const auto entry = [](MenuItemKind kind, int id, const char* label, const char* hint = "", bool checked = false, bool enabled = true, float value = 0.f) {
        return MenuEntry{kind, id, slint::SharedString(label), slint::SharedString(hint), checked, enabled, value};
    };
    const auto displayBlock = [&](std::vector<MenuEntry>& e) {
        e.push_back(entry(MenuItemKind::Separator, 0, ""));
        e.push_back(entry(MenuItemKind::Heading, 300, "Display"));
        e.push_back(entry(MenuItemKind::Radio, 301, "Range: Automatic", "", !rangeManual));
        e.push_back(entry(MenuItemKind::Radio, 302, "Range: Manual", "", rangeManual));
        e.push_back(entry(MenuItemKind::Heading, 0, ("Floor " + fmt("%.0f dB", state.dbMin)).c_str()));
        e.push_back(entry(MenuItemKind::Slider, 303, "Floor", "", false, rangeManual, static_cast<float>(std::clamp((state.dbMin + 160.0) / 140.0, 0.0, 1.0))));
        e.push_back(entry(MenuItemKind::Heading, 0, ("Ceiling " + fmt("%.0f dB", state.dbMax)).c_str()));
        e.push_back(entry(MenuItemKind::Slider, 304, "Ceiling", "", false, rangeManual, static_cast<float>(std::clamp((state.dbMax + 100.0) / 120.0, 0.0, 1.0))));
        e.push_back(entry(MenuItemKind::Item, 305, "Reset display to defaults"));
    };
    const auto openMenu = [&](MenuKind kind, float x, float y) {
        std::vector<MenuEntry> e;
        menuKind = kind;
        switch (kind) {
            case MenuKind::Counter:
                e.push_back(entry(MenuItemKind::Heading, 0, "TUNED - HERTZ"));
                e.push_back(entry(MenuItemKind::Checked, 101, "Enlarge figures", "", false));
                e.push_back(entry(MenuItemKind::Checked, 102, "Normal size", "", true));
                e.push_back(entry(MenuItemKind::Checked, 103, "Show tuner switches", "", showNudges));
                e.push_back(entry(MenuItemKind::Heading, 0, "Counter face"));
                e.push_back(entry(MenuItemKind::Radio, 111, "Nixie tubes", "", face == Face::Nixie));
                e.push_back(entry(MenuItemKind::Radio, 112, "Neon", "", face == Face::Drum));
                e.push_back(entry(MenuItemKind::Radio, 113, "Plain", "", face == Face::Plain));
                e.push_back(entry(MenuItemKind::Separator, 0, ""));
                e.push_back(entry(MenuItemKind::Checked, 104, "Enlarge every reading", "", false));
                break;
            case MenuKind::Trace:
                e.push_back(entry(MenuItemKind::Checked, 201, "Normal", "", traceMode == 0));
                e.push_back(entry(MenuItemKind::Checked, 202, "Peak hold", "", traceMode == 1));
                e.push_back(entry(MenuItemKind::Checked, 203, "Average", "", traceMode == 2));
                e.push_back(entry(MenuItemKind::Separator, 0, ""));
                e.push_back(entry(MenuItemKind::Item, 204, "Reset trace", "", false, traceMode != 0));
                e.push_back(entry(MenuItemKind::Heading, 0, "Average length"));
                {
                    const std::array<std::pair<const char*, double>, 7> presets{{{"100 ms", 0.1}, {"200 ms", 0.2}, {"500 ms", 0.5}, {"1 s", 1.0}, {"2 s", 2.0}, {"5 s", 5.0}, {"10 s", 10.0}}};
                    for (std::size_t i = 0; i < presets.size(); ++i) { e.push_back(entry(MenuItemKind::Radio, 211 + static_cast<int>(i), presets[i].first, "", std::fabs(averageLengthS - presets[i].second) < 1e-9)); }
                    e.push_back(entry(MenuItemKind::Slider, 218, "Average length", "", false, true, static_cast<float>(std::clamp(std::log(averageLengthS / 0.05) / std::log(200.0), 0.0, 1.0))));
                }
                displayBlock(e);
                break;
            case MenuKind::Marker: {
                menuMarkerNear = -1;
                for (std::size_t i = 0; i < lists.markers.size(); ++i) {
                    const ViewWindow v = currentView();
                    if (std::fabs(fracOf(lists.markers[i].hz, v.centreHz, v.spanHz) - fracOf(menuHz, v.centreHz, v.spanHz)) * 980.0 <= 6.0) { menuMarkerNear = static_cast<int>(i); break; }
                }
                e.push_back(entry(MenuItemKind::Item, 401, "Drop marker here", fmt("%.3f MHz", menuHz / 1e6).c_str(), false, menuHz > 0 && lists.markers.size() < 200));
                if (menuMarkerNear >= 0) { e.push_back(entry(MenuItemKind::Item, 402, ("Remove marker M" + std::to_string(lists.markers[static_cast<std::size_t>(menuMarkerNear)].id)).c_str())); }
                e.push_back(entry(MenuItemKind::Separator, 0, ""));
                e.push_back(entry(MenuItemKind::Item, 403, "List markers...", std::to_string(lists.markers.size()).c_str()));
                e.push_back(entry(MenuItemKind::Item, 404, "Copy markers to clipboard", "", false, !lists.markers.empty()));
                e.push_back(entry(MenuItemKind::Submenu, 405, "Clear all markers", "", false, !lists.markers.empty()));
                displayBlock(e);
                break;
            }
            case MenuKind::MarkerClear:
                e.push_back(entry(MenuItemKind::Item, 406, ("Yes, remove all " + std::to_string(lists.markers.size())).c_str()));
                break;
            case MenuKind::TuneStep: {
                const std::array<std::pair<const char*, double>, 9> steps{{{"1 kHz", 1000}, {"5 kHz", 5000}, {"6.25 kHz", 6250}, {"9 kHz", 9000}, {"10 kHz", 10000}, {"12.5 kHz", 12500}, {"25 kHz", 25000}, {"50 kHz", 50000}, {"100 kHz", 100000}}};
                e.push_back(entry(MenuItemKind::Heading, 0, "Tune step"));
                for (std::size_t i = 0; i < steps.size(); ++i) { e.push_back(entry(MenuItemKind::Radio, 501 + static_cast<int>(i), steps[i].first, "", std::fabs(tuneStepHz - steps[i].second) < 1e-9)); }
                break;
            }
            case MenuKind::Modes:   // the compact strip's picker: the eight modes as radio items
                e.push_back(entry(MenuItemKind::Heading, 0, "Mode"));
                for (int m = 0; m < 8; ++m) { e.push_back(entry(MenuItemKind::Radio, 701 + m, modeName(demodOfIndex(m)), "", modeIndex(state.demodMode) == m)); }
                break;
            case MenuKind::App:   // the top-level menu: About FoxSDR holds the AboutSlint widget the licence requires
                e.push_back(entry(MenuItemKind::Item, 601, "About FoxSDR..."));
                e.push_back(entry(MenuItemKind::Item, 602, "Key bindings", "F7"));
                e.push_back(entry(MenuItemKind::Item, 603, "Save a screenshot", "Ctrl W"));
                e.push_back(entry(MenuItemKind::Separator, 0, ""));
                e.push_back(entry(MenuItemKind::Item, 604, "Quit"));
                break;
            default: break;
        }
        ui->set_menu_entries(std::make_shared<slint::VectorModel<MenuEntry>>(std::move(e)));
        ui->invoke_open_menu(x, y);
    };
    const auto applyFace = [&] {
        Skin s = ui->global<Theme>().get_skin();
        s.digit_face = face == Face::Nixie ? DigitFace::Nixie : face == Face::Drum ? DigitFace::Drum : DigitFace::Plain;
        ui->global<Theme>().set_skin(s);
    };
    const auto copyMarkers = [&] {
        std::string text;
        for (const auto& m : lists.markers) { text += "M" + std::to_string(m.id) + "\t" + fmt("%.6f", m.hz / 1e6) + "\t" + m.note + "\n"; }
        std::fprintf(stderr, "foxsdr-slint: markers copied (%zu); the clipboard is reached in phase 2\n%s", lists.markers.size(), text.c_str());
    };
    ui->on_menu_activated([&](int id) {
        switch (id) {
            case 101: case 102: case 104: std::fprintf(stderr, "foxsdr-slint: counter scale %d: the interface-size setting of phase 2\n", id); break;
            case 103: showNudges = !showNudges; ui->set_show_nudges(showNudges); break;
            case 111: face = Face::Nixie; applyFace(); break;
            case 112: face = Face::Drum; applyFace(); break;
            case 113: face = Face::Plain; applyFace(); break;
            case 201: case 202: case 203: traceMode = id - 201; ui->set_trace_mode(traceMode); resetTrace(traceState); break;
            case 204: resetTrace(traceState); break;
            case 211: case 212: case 213: case 214: case 215: case 216: case 217: {
                const double presets[7] = {0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0};
                averageLengthS = presets[id - 211];
                break;
            }
            case 301: rangeManual = false; ui->set_range_manual(false); setDisplayRange(-110, 0); break;
            case 302: rangeManual = true; ui->set_range_manual(true); break;
            case 305: rangeManual = false; ui->set_range_manual(false); setDisplayRange(-110, 0); traceMode = 0; ui->set_trace_mode(0); resetTrace(traceState); break;
            case 401: { FoxCommand c = op(FOXAPP_OP_MARKER_DROP); c.num[0] = menuHz; c.num[1] = 2.0 * currentView().spanHz / 980.0; submit(c); break; }
            case 402: if (menuMarkerNear >= 0) { FoxCommand c = op(FOXAPP_OP_MARKER_REMOVE); c.ival[0] = static_cast<int64_t>(lists.markers[static_cast<std::size_t>(menuMarkerNear)].id); submit(c); } break;
            case 403: std::fprintf(stderr, "foxsdr-slint: the Markers window is a companion screen (phase 3)\n"); break;
            case 404: copyMarkers(); break;
            case 406: submit(op(FOXAPP_OP_MARKER_CLEAR)); break;
            case 601: ui->set_about_open(true); break;
            case 602: bankIndex = 4; ui->set_bank_index(4); openSections[4].insert(4); break;
            case 603: ui->invoke_shortcut(ss("screenshot")); break;
            case 604: slint::quit_event_loop(); break;
            case 701: case 702: case 703: case 704: case 705: case 706: case 707: case 708: setMode(demodOfIndex(id - 701)); break;
            default:
                if (id >= 501 && id <= 509) {
                    const double steps[9] = {1000, 5000, 6250, 9000, 10000, 12500, 25000, 50000, 100000};
                    tuneStepHz = steps[id - 501];
                    ui->set_tune_step_text(ss(fmtKHz(tuneStepHz)));
                }
                break;
        }
    });
    ui->on_menu_slider_changed([&](int id, float v) {
        if (id == 218) { averageLengthS = 0.05 * std::pow(200.0, std::clamp(static_cast<double>(v), 0.0, 1.0)); }
        if (id == 303 && rangeManual) { FoxCommand c = op(FOXAPP_OP_SET_DISPLAY_MIN); c.num[0] = -160.0 + 140.0 * v; submit(c); }
        if (id == 304 && rangeManual) { FoxCommand c = op(FOXAPP_OP_SET_DISPLAY_MAX); c.num[0] = -100.0 + 120.0 * v; submit(c); }
    });

    // ---- the strip ----
    ui->on_stop_clicked([&] { FoxCommand c = op(FOXAPI_OP_RUN); c.ival[0] = (state.flags & FOXAPI_RX_RUNNING) ? 0 : 1; submit(c); });
    ui->on_nudge([&](int digit, int dir) { stepTune(dir, stepOfDigit(digit)); });
    ui->on_edit_requested([&] { ui->set_editor_text(ss(fmt("%.6f", state.tunedHz / 1e6))); ui->set_editing(true); });
    ui->on_typed([&](slint::SharedString text) {
        double hz = 0;
        if (parseFrequencyHz(std::string(text), hz)) { setFrequency(hz); }
        else { std::fprintf(stderr, "foxsdr-slint: could not read frequency \"%s\"\n", text.data()); }
        ui->set_editing(false);
    });
    ui->on_edit_cancelled([&] { ui->set_editing(false); });
    ui->on_counter_menu_requested([&](float x, float y) { openMenu(MenuKind::Counter, x, y); });
    ui->on_mode_selected([&](int i) { setMode(demodOfIndex(i)); });
    ui->on_mode_picker_requested([&](float x, float y) { openMenu(MenuKind::Modes, x, y); });
    ui->on_squelch_changed([&](float v) { setSquelch(squelchDbFromNorm(v)); });
    ui->on_squelch_auto_clicked([&] { setSquelch(squelchAuto(state.squelchDb) ? -85.0 : -200.0); });
    ui->on_volume_changed([&](float v) { setVolume(v); });
    ui->on_gain_changed([&](float v) {
        if (!lists.haveGain) { return; }
        FoxCommand c = op(FOXAPI_OP_SET_GAIN); std::snprintf(c.text, sizeof(c.text), "%s", lists.gainStage.c_str());
        c.num[0] = lists.gainMin + (lists.gainMax - lists.gainMin) * v; submit(c);
    });
    ui->on_gain_auto_clicked([&] { FoxCommand c = op(FOXAPI_OP_SET_DEVICE_AGC); c.ival[0] = (state.flags & FOXAPI_RX_DEVICE_AGC) ? 0 : 1; submit(c); });
    ui->on_bias_tee_clicked([&] { FoxCommand c = op(FOXAPI_OP_SET_BIAS_TEE); c.ival[0] = 0; submit(c); });          // on -> off at once
    ui->on_bias_tee_confirmed([&] { FoxCommand c = op(FOXAPI_OP_SET_BIAS_TEE); c.ival[0] = 1; submit(c); });        // the question's "Turn it on"
    ui->on_stop_muting_plugin([&] { for (const auto& name : lists.muteCauses) { FoxCommand c = op(FOXAPI_OP_DECODER_STOP); std::snprintf(c.text, sizeof(c.text), "%s", name.c_str()); submit(c); } });

    // ---- rail + panel ----
    ui->on_bank_selected([&](int i) { bankIndex = std::clamp(i, 0, 4); ui->set_bank_index(bankIndex); });
    ui->on_section_clicked([&](int i) {
        if (bankIndex == 4 && i == 7) { ui->set_about_open(true); return; }
        auto& open = openSections[bankIndex];
        if (open.count(i)) { open.erase(i); } else { open.insert(i); }
    });
    ui->on_passband_changed([&](float v) {
        const double lo = 3000.0, hi = std::max(lo, 0.9 * state.channelRateHz);
        FoxCommand c = op(FOXAPI_OP_SET_BANDWIDTH); c.num[0] = lo + (hi - lo) * v; submit(c);
    });
    ui->on_tune_step_clicked([&](float x, float y) { openMenu(MenuKind::TuneStep, x, y); });
    ui->on_follow_band_plan_toggled([&](bool v) { followBandPlan = v; });
    ui->on_team_message_seen([&] {});
    ui->on_update_primary_clicked([&] {});
    ui->on_update_dismissed([&] { ui->set_update_shown(false); });

    // ---- centre ----
    ui->on_view_selected([&](int i) { ui->set_view_index(i); });
    ui->on_window_tab_selected([&](int) {});
    ui->on_window_tab_closed([&](int) {});
    ui->on_add_window_clicked([&] { std::fprintf(stderr, "foxsdr-slint: docked windows come with the plugin kit (phase 3)\n"); });
    ui->on_trace_mode_selected([&](int i) { traceMode = i; ui->set_trace_mode(i); resetTrace(traceState); });
    ui->on_range_manual_selected([&](bool manual) { rangeManual = manual; ui->set_range_manual(manual); if (!manual) { setDisplayRange(-110, 0); } });
    ui->on_markers_clicked([&] { menuHz = state.tunedHz; openMenu(MenuKind::Marker, 700.f, 180.f); });
    ui->on_band_plan_toggled([&] { bandPlanOn = !bandPlanOn; ui->set_band_plan_on(bandPlanOn); });
    ui->on_zoom([&](int dir) { if (dir == 0) { view = {0, 0}; } else { zoomView(dir > 0 ? kZoomPerNotch : 1.0 / kZoomPerNotch, 0.5); } });
    const auto hover = [&](float x, float y) {
        hoverX = x; hoverY = y;
        const bool visible = x >= 0 && y >= 0;
        ui->set_cursor_visible(visible);
        ui->set_cursor_pos(x);
        if (visible) { ui->set_cursor_text(ss(cursorText(hzAtFrac(x), dbAtFrac(x)))); }
    };
    ui->on_spectrum_hover(hover);
    ui->on_waterfall_hover(hover);
    const auto clickTune = [&](float x, bool shift) {
        FoxCommand c = op(FOXAPP_OP_VFO_TO_ABSOLUTE); c.num[0] = hzAtFrac(x); c.ival[0] = shift ? 0 : 1; submit(c);
    };
    ui->on_spectrum_clicked(clickTune);
    ui->on_waterfall_clicked(clickTune);
    ui->on_spectrum_double_clicked([&](float) { view = {0, 0}; });
    ui->on_waterfall_double_clicked([&] { view = {0, 0}; });
    ui->on_spectrum_menu_requested([&](float x, float px, float py) { menuHz = hzAtFrac(x); openMenu(MenuKind::Trace, px + 360.f, py + 172.f); });
    ui->on_waterfall_menu_requested([&](float x, float px, float py) { menuHz = hzAtFrac(x); openMenu(MenuKind::Marker, px + 360.f, py + 172.f); });
    ui->on_band_dragged([&](float dx) { setFrequency(state.tunedHz + dx * currentView().spanHz); });
    ui->on_edge_dragged([&](float half) { FoxCommand c = op(FOXAPP_OP_SET_BANDWIDTH_DRAG); c.num[0] = 2.0 * half * currentView().spanHz; submit(c); });
    const auto wheel = [&](float x, int notches, bool shift) {
        if (shift) { stepTune(notches > 0 ? 1 : -1, modeStepHz(state.demodMode)); return; }
        zoomView(notches > 0 ? kZoomPerNotch : 1.0 / kZoomPerNotch, x);
    };
    ui->on_spectrum_wheel(wheel);
    ui->on_waterfall_wheel(wheel);
    ui->on_waterfall_panned([&](float dx) { view = panBy(currentView(), fullCentre(), fullSpan(), dx); if (view.spanHz >= fullSpan() * 0.999) { view = {0, 0}; } });
    ui->on_marker_hovered([&](int i) {
        if (i >= 0 && i < static_cast<int>(lists.markers.size())) {
            const auto& m = lists.markers[static_cast<std::size_t>(i)];
            ui->set_marker_tooltip(ss("M" + std::to_string(m.id) + " " + fmtMHz(m.hz) + (m.note.empty() ? "" : " \xC2\xB7 " + m.note) + " \xC2\xB7 right-click to remove"));
        }
    });
    ui->on_status_clicked([&](int) {});
    ui->on_feature_request_clicked([&] { std::fprintf(stderr, "foxsdr-slint: the feature-request page is a companion screen (phase 3)\n"); });
    ui->on_bug_report_clicked([&] { std::fprintf(stderr, "foxsdr-slint: the problem-report page is a companion screen (phase 3)\n"); });

    // ---- the command palette ----
    const auto rebuildPalette = [&](const std::string& query) {
        paletteActions.clear();
        const std::string q = lower(query);
        const auto matches = [&](const std::string& s) { return q.empty() || lower(s).find(q) != std::string::npos; };
        const auto add = [&](const char* kind, const std::string& title, const std::string& detail, std::function<void()> fn) {
            if (matches(title) || matches(detail)) { paletteActions.push_back({kind, title, detail, std::move(fn)}); }
        };
        double hz = 0;
        if (!q.empty() && parseFrequencyHz(query, hz)) { paletteActions.push_back({"FREQUENCY", "Tune to " + fmtMHz(hz), "the receiver", [=] { setFrequency(hz); }}); }
        const char* bankNames[5] = {"Signal path", "Decode", "Tools", "Extend", "System"};
        for (int b = 0; b < 5; ++b) { add("BANK", bankNames[b], "show the bank", [=, &bankIndex] { bankIndex = b; ui->set_bank_index(b); }); }
        for (int m = 0; m < 8; ++m) { add("MODE", std::string("Mode ") + modeName(demodOfIndex(m)), "demodulation", [=] { setMode(demodOfIndex(m)); }); }
        for (const auto& bm : lists.bookmarks) { const auto id = bm.id; add("BOOKMARK", bm.name, fmtMHz(bm.hz), [=] { FoxCommand c = op(FOXAPI_OP_BOOKMARK_TUNE); c.ival[0] = static_cast<int64_t>(id); submit(c); }); }
        for (const auto& pl : lists.pluginNames) { add("PLUGIN", pl, "fitted module", [=, &bankIndex] { bankIndex = 1; ui->set_bank_index(1); }); }
        const char* settings[] = {"Display", "Key bindings", "Updates", "Language", "Diagnostics", "Usage reporting", "Serial ports"};
        for (int i = 0; i < 7; ++i) { const int row = i == 0 ? 0 : i == 1 ? 4 : i == 2 ? 2 : i == 3 ? 1 : i == 4 ? 5 : i == 5 ? 6 : 3; add("SETTING", settings[i], "System bank", [=, &bankIndex, &openSections] { bankIndex = 4; ui->set_bank_index(4); openSections[4].insert(row); }); }
        add("ABOUT", "About FoxSDR", "version, licences", [&] { ui->set_about_open(true); });
        if (paletteActions.size() > 12) { paletteActions.resize(12); }
        std::vector<PaletteResult> results;
        for (std::size_t i = 0; i < paletteActions.size(); ++i) { results.push_back({ss(paletteActions[i].kind), ss(paletteActions[i].title), ss(paletteActions[i].detail), static_cast<int>(i)}); }
        ui->set_palette_results(std::make_shared<slint::VectorModel<PaletteResult>>(std::move(results)));
    };
    rebuildPalette("");
    ui->on_palette_query_changed([&](slint::SharedString q) { rebuildPalette(std::string(q)); });
    ui->on_palette_activated([&](int id) { if (id >= 0 && id < static_cast<int>(paletteActions.size())) { paletteActions[static_cast<std::size_t>(id)].run(); } });

    // ---- the shortcuts, by config id (the key table of the ImGui window; rebinding is the SYSTEM bank's, phase 3) ----
    const auto screenshot = [&] {
        auto shot = ui->window().take_snapshot();
        if (!shot) { std::fprintf(stderr, "foxsdr-slint: the snapshot was refused\n"); return; }
        std::filesystem::path dir = std::filesystem::path(configPath).parent_path() / "screenshots";
        std::error_code ec; std::filesystem::create_directories(dir, ec);
        const std::time_t t = std::time(nullptr); char stamp[32]; std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
        const std::string path = (dir / (std::string("foxsdr-") + stamp + ".png")).string();
        std::vector<unsigned char> out;
        const auto sink = [](void* ctx, void* data, int size) { auto* v = static_cast<std::vector<unsigned char>*>(ctx); const auto* b = static_cast<const unsigned char*>(data); v->insert(v->end(), b, b + size); };
        stbi_write_png_to_func(sink, &out, static_cast<int>(shot->width()), static_cast<int>(shot->height()), 4, shot->begin(), static_cast<int>(shot->width()) * 4);
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(out.data(), 1, out.size(), f); std::fclose(f); std::fprintf(stderr, "foxsdr-slint: screenshot %s\n", path.c_str()); }
    };
    ui->on_shortcut([&](slint::SharedString idS) {
        const std::string id(idS);
        if (id == "startStop") { FoxCommand c = op(FOXAPI_OP_RUN); c.ival[0] = (state.flags & FOXAPI_RX_RUNNING) ? 0 : 1; submit(c); }
        else if (id == "mute") { FoxCommand c = op(FOXAPI_OP_SET_MUTED); c.ival[0] = (state.flags & FOXAPI_RX_MUTED) ? 0 : 1; submit(c); }
        else if (id == "volumeUp") { setVolume(state.volume + 0.05); }
        else if (id == "volumeDown") { setVolume(state.volume - 0.05); }
        else if (id == "squelchUp") { setSquelch(std::min(0.0, (squelchAuto(state.squelchDb) ? -120.0 : state.squelchDb) + 2.0)); }
        else if (id == "squelchDown") { setSquelch(std::max(-120.0, (squelchAuto(state.squelchDb) ? -120.0 : state.squelchDb) - 2.0)); }
        else if (id == "modeNfm") { setMode(FOXAPI_DEMOD_NFM); } else if (id == "modeWfm") { setMode(FOXAPI_DEMOD_WFM); }
        else if (id == "modeAm") { setMode(FOXAPI_DEMOD_AM); } else if (id == "modeDsb") { setMode(FOXAPI_DEMOD_DSB); }
        else if (id == "modeUsb") { setMode(FOXAPI_DEMOD_USB); } else if (id == "modeCw") { setMode(FOXAPI_DEMOD_CW); }
        else if (id == "modeLsb") { setMode(FOXAPI_DEMOD_LSB); } else if (id == "modeRaw") { setMode(FOXAPI_DEMOD_RAW); }
        else if (id == "tuneStepUp") { stepTune(1, tuneStepHz); } else if (id == "tuneStepDown") { stepTune(-1, tuneStepHz); }
        else if (id == "tuneSpanUp") { stepTune(1, currentView().spanHz); } else if (id == "tuneSpanDown") { stepTune(-1, currentView().spanHz); }
        else if (id == "quickTune") { ui->invoke_edit_requested(); }
        else if (id == "zoomIn") { zoomView(kZoomPerNotch, 0.5); } else if (id == "zoomOut") { zoomView(1.0 / kZoomPerNotch, 0.5); } else if (id == "zoomReset") { view = {0, 0}; }
        else if (id == "record") { FoxCommand c = op(FOXAPI_OP_RECORD_AUDIO); c.ival[0] = (state.flags & FOXAPI_RX_RECORDING_AUDIO) ? 0 : 1; submit(c); }
        else if (id == "screenshot") { screenshot(); }
        else if (id == "fullscreen") { ui->window().set_maximized(!ui->window().is_maximized()); }
        else if (id == "bankSignal") { bankIndex = 0; ui->set_bank_index(0); } else if (id == "bankDecode") { bankIndex = 1; ui->set_bank_index(1); }
        else if (id == "bankView") { bankIndex = 2; ui->set_bank_index(2); } else if (id == "bankExtend") { bankIndex = 3; ui->set_bank_index(3); }
        else if (id == "bankSystem") { bankIndex = 4; ui->set_bank_index(4); }
        else if (id == "openSettings") { bankIndex = 4; ui->set_bank_index(4); openSections[4].insert(4); }
        else if (id == "escape") { ui->set_editing(false); ui->set_about_open(false); ui->set_bias_question_open(false); }
        else if (id == "uiSizeUp" || id == "uiSizeAuto") { std::fprintf(stderr, "foxsdr-slint: the interface size is phase 2's (%s)\n", id.c_str()); }
        else { std::fprintf(stderr, "foxsdr-slint: shortcut %s is not bound here\n", id.c_str()); }
    });

    // ---- the frameless window's chrome ----
    const auto scale = [&] { return ui->window().scale_factor(); };
    ui->on_title_drag([&](float dx, float dy) {
        const auto pos = ui->window().position();
        ui->window().set_position(slint::PhysicalPosition({pos.x + static_cast<int>(std::lround(dx * scale())), pos.y + static_cast<int>(std::lround(dy * scale()))}));
    });
    ui->on_title_double_clicked([&] { ui->window().set_maximized(!ui->window().is_maximized()); });
    ui->on_system_menu_requested([&](float, float) { std::fprintf(stderr, "foxsdr-slint: the system menu needs the native handle (phase 2)\n"); });
    ui->on_brand_clicked([&] { openMenu(MenuKind::App, 12.f, 36.f); });
    ui->on_minimise_clicked([&] { ui->window().set_minimized(true); });
    ui->on_maximise_clicked([&] { ui->window().set_maximized(!ui->window().is_maximized()); });
    ui->on_close_clicked([&] { slint::quit_event_loop(); });
    ui->on_resize_drag([&](int edge, float dx, float dy) {
        const auto sz = ui->window().size();
        const auto w = static_cast<std::uint32_t>(std::max(624.0f * scale(), sz.width + (edge != 2 ? dx * scale() : 0.f)));
        const auto h = static_cast<std::uint32_t>(std::max(400.0f * scale(), sz.height + (edge != 1 ? dy * scale() : 0.f)));
        ui->window().set_size(slint::PhysicalSize({w, h}));
    });

    // ---- the tick: heartbeat, read_state, lists, read_spectrum, poll_results -> properties ----
    slint::Timer tick;
    tick.start(slint::TimerMode::Repeated, std::chrono::milliseconds(33), [&] {
        (void)api->heartbeat(session);
        if (api->read_state(session, &state) < 0) { return; }
        if (state.listSeq != lastListSeq) { lastListSeq = state.listSeq; refreshLists(); }
        const bool running = (state.flags & FOXAPI_RX_RUNNING) != 0;

        // the strip
        ui->set_running(running);
        ui->set_decoding((state.flags & FOXAPI_RX_DECODER_ACTIVE) != 0);
        ui->set_muted((state.flags & FOXAPI_RX_MUTED) != 0 || state.audioMutedBy[0] != '\0');
        ui->set_failed((state.flags & FOXAPI_RX_FAULTED) != 0);
        ui->set_maximised(ui->window().is_maximized());
        ui->set_tuned_text(ss(fmtMHz(state.tunedHz)));
        const auto d = digitsOf(state.tunedHz);
        {
            std::string sig; for (int v : d) { sig += static_cast<char>('0' + v); }
            if (sigs.changed("digits", sig)) { ui->set_digits(std::make_shared<slint::VectorModel<int>>(std::vector<int>(d.begin(), d.end()))); }
        }
        ui->set_leading_zeros(leadingZeros(d));
        ui->set_converter_text(ss(lists.converter ? "via " + lists.converterName : std::string()));
        ui->set_converter_warn(lists.converter);
        ui->set_tune_hint(ss(tuneHint(lists.converter)));
        ui->set_mode_index(modeIndex(state.demodMode));
        ui->set_mode_hint(ss(modeHint(state)));
        const bool agc = (state.flags & FOXAPI_RX_DEVICE_AGC) != 0;
        ui->set_gain_auto(agc);
        ui->set_gain_enabled(lists.haveGain && !agc);
        ui->set_gain_auto_enabled((state.flags & FOXAPI_RX_AGC_SUPPORTED) != 0);
        ui->set_gain_auto_tooltip(ss(!(state.flags & FOXAPI_RX_DEVICE_OPEN) ? "No radio is open" : !(state.flags & FOXAPI_RX_AGC_SUPPORTED) ? "This radio has no automatic gain" : "Automatic gain - the radio sets its own gain"));
        std::string gainTxt = gainText(lists.haveGain, agc, 0, lists.gainUnit);
        if (lists.haveGain) {
            FoxListItem it{}; it.structSize = sizeof(it);
            if (api->read_list(session, FOXAPI_LIST_GAINS, 0, &it) > 0) {
                const double range = std::max(1e-9, lists.gainMax - lists.gainMin);
                ui->set_gain_norm(static_cast<float>(std::clamp((it.value[3] - lists.gainMin) / range, 0.0, 1.0)));
                gainTxt = gainText(true, agc, it.value[3], lists.gainUnit);
            }
            ui->set_gain_hint(ss(lists.gainStage + " stage \xC2\xB7 " + state.deviceName));
            ui->set_gain_tooltip(ss("Gain - the radio's first stage; drag the slider, or scroll over it (" + lists.gainStage + ")"));
        } else {
            ui->set_gain_norm(0.f); ui->set_gain_hint(ss("no gain stage"));
            ui->set_gain_tooltip(ss((state.flags & FOXAPI_RX_DEVICE_OPEN) ? "This radio has no gain stage" : "No radio is open - choose one in the Source section"));
        }
        ui->set_gain_text(ss(gainTxt));
        const bool sqAuto = squelchAuto(state.squelchDb);
        ui->set_squelch_auto(sqAuto);
        ui->set_squelch_norm(static_cast<float>(squelchNorm(state.squelchDb)));
        ui->set_squelch_text(ss(sqAuto ? std::string("open") : fmt("%.0f dB", state.squelchDb)));
        ui->set_squelch_hint(ss(sqAuto ? "Automatic: follows the noise floor" : "Manual threshold"));
        ui->set_squelch_auto_tooltip(ss(sqAuto ? "Automatic squelch - sits 8 dB above the measured noise floor" : "Automatic squelch - follows the noise floor so weak carriers open it and noise does not"));
        ui->set_volume_norm(static_cast<float>(state.volume));
        ui->set_volume_text(ss(fmt("%.2f", state.volume)));
        const bool haveAudio = running && (state.flags & FOXAPI_RX_SINK_OPEN);
        ui->set_level_norm(static_cast<float>(haveAudio ? levelNorm(state.audioLevelDb) : 0.0));
        ui->set_level_text(ss(levelText(state.audioLevelDb, haveAudio)));
        const bool biasPresent = (state.flags & FOXAPI_RX_BIAS_TEE_PRESENT) != 0, biasOn = (state.flags & FOXAPI_RX_BIAS_TEE_ON) != 0;
        ui->set_bias_tee_present(biasPresent); ui->set_bias_tee_on(biasOn);
        ui->set_bias_hint(ss(biasHint(biasPresent, biasOn)));
        ui->set_bias_question_text(ss(std::string(state.deviceName) + ": FoxSDR will switch it on again whenever this radio is opened, until you turn it off."));
        ui->set_muted_by(ss(state.audioMutedBy));
        ui->set_show_nudges(showNudges);

        // the rail and the panel
        ui->set_bank_index(bankIndex);
        {
            std::vector<SectionRow> rows = sectionsFor(bankIndex);
            std::string sig = std::to_string(bankIndex) + ":";
            for (const auto& r : rows) { sig += std::string(r.name) + "|" + std::string(r.chip) + "|" + std::to_string(static_cast<int>(r.tone)) + "|" + (r.open ? "1" : "0") + ";"; }
            if (sigs.changed("sections", sig)) { ui->set_sections(std::make_shared<slint::VectorModel<SectionRow>>(std::move(rows))); }
        }
        ui->set_bank_chip(ss(bankIndex == 0 ? (running ? "RECEIVING" : "STOPPED") : "--"));
        ui->set_bank_chip_tone(bankIndex == 0 && running ? ChipTone::Good : ChipTone::Neutral);
        const double lo = 3000.0, hi = std::max(lo + 1.0, 0.9 * state.channelRateHz);
        ui->set_passband_norm(static_cast<float>(std::clamp((state.bandwidthHz - lo) / (hi - lo), 0.0, 1.0)));
        ui->set_passband_text(ss(fmtKHz(state.bandwidthHz)));
        ui->set_tune_step_text(ss(fmtKHz(tuneStepHz)));
        std::string band;
        for (const auto& b : lists.bands) { if (state.tunedHz >= b.startHz && state.tunedHz <= b.stopHz) { band = b.name; break; } }
        ui->set_source_line(ss(sourceLine(state, band)));
        ui->set_rate_line(ss(rateLine(state)));
        ui->set_converter_line(ss(lists.converter ? "via " + lists.converterName + " \xC2\xB7 LO " + fmtMHz(lists.converterLoHz) : std::string()));
        ui->set_update_shown(false);        // no updater state crosses the table in API 0.3 (phase 2 adds it)
        ui->set_team_message_shown(false);

        // the status bar
        HostExtras x;
        x.gainText = gainTxt == "N/A" ? std::string() : gainTxt;
        x.antenna = lists.antennaCount > 1 ? lists.antenna : std::string();
        x.webPort = lists.webPort; x.webRefused = lists.webRefused;
        outputRate.add(state.decoderLinesTotal, nowMs());
        x.outputMeasured = outputRate.measured(); x.outputRate = outputRate.perSecond(); x.outputMeanS = outputRate.meanSeconds();
        x.recorderValue = lists.recorderValue; x.recorderDetail = lists.recorderDetail;
        std::vector<StatusCard> cards;
        std::string cardSig;
        for (const auto& c : statusCards(state, x)) {
            cardSig += c.key + "|" + c.value + "|" + c.detail + "|" + std::to_string(static_cast<int>(c.tone)) + "|" + (c.lampLit ? "L" : "-") + (c.lampRed ? "R" : "-");
            for (const auto& l : c.lines) { cardSig += "/" + l; }
            cardSig += ";";
            std::vector<slint::SharedString> lines;
            for (const auto& l : c.lines) { lines.push_back(ss(l)); }
            cards.push_back({ss(c.key), ss(c.value), ss(c.detail),
                             c.tone == foxsdr::slinthost::CardTone::Good ? ChipTone::Good : c.tone == foxsdr::slinthost::CardTone::Warn ? ChipTone::Warn : c.tone == foxsdr::slinthost::CardTone::Err ? ChipTone::Err : ChipTone::Neutral,
                             c.lampLit ? (c.lampRed ? LampState::Red : LampState::Green) : LampState::Off, c.lampShown,
                             std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(lines))});
        }
        if (sigs.changed("status", cardSig)) { ui->set_status(std::make_shared<slint::VectorModel<StatusCard>>(std::move(cards))); }

        // the spectrum: only when the engine published a newer frame
        FoxSpectrumInfo info{}; info.structSize = sizeof(info);
        const int32_t rc = api->read_spectrum(session, sinceSeq, &info, bins.data(), static_cast<uint32_t>(bins.size()));
        const ViewWindow v = currentView();
        if (rc == FOXAPI_OK && info.copied > 0 && isNewFrame(info.seq, sinceSeq)) {
            sinceSeq = info.seq;
            lastInfo = info;
            frame.assign(bins.begin(), bins.begin() + info.copied);
            const std::int64_t t = nowMs();
            lastFrameMs = t;
            frameTimes.push_back(t);
            while (frameTimes.size() > 2 && t - frameTimes.front() > 5000) { frameTimes.pop_front(); }
            const double fps = frameTimes.size() >= 2 ? 1000.0 * static_cast<double>(frameTimes.size() - 1) / static_cast<double>(std::max<std::int64_t>(1, frameTimes.back() - frameTimes.front())) : 0.0;
            // the zoomed window: the bins inside it
            const ViewWindow cur = currentView();
            const std::size_t b0 = static_cast<std::size_t>(std::clamp(fracOf(cur.centreHz - cur.spanHz / 2.0, info.centreHz, info.spanHz), 0.0, 1.0) * frame.size());
            const std::size_t b1 = std::max(b0 + 1, static_cast<std::size_t>(std::clamp(fracOf(cur.centreHz + cur.spanHz / 2.0, info.centreHz, info.spanHz), 0.0, 1.0) * frame.size()));
            const float* sub = frame.data() + b0;
            const std::size_t subN = std::min(b1, frame.size()) - b0;
            if (traceState.peak.size() == kSpecW && (b0 != 0 || subN != frame.size())) { /* a zoomed hold keeps its columns: columns are the view's */ }
            const foxsdr::slinthost::TraceMode mode = traceMode == 1 ? foxsdr::slinthost::TraceMode::PeakHold : traceMode == 2 ? foxsdr::slinthost::TraceMode::Average : foxsdr::slinthost::TraceMode::Normal;
            traceState.averageAlpha = static_cast<float>(fps > 0 ? 1.0 - std::exp(-1.0 / (averageLengthS * fps)) : 0.1);
            paintTrace(wells.traceMemory(), kSpecW, kSpecH, sub, subN, state.dbMin, state.dbMax, traceState, mode,
                       Rgba{0x7a, 0xd7, 0xff, 255}, Rgba{0xff, 0x9f, 0x43, 255}, Rgba{0xf1, 0xea, 0xd6, 255});
            scrollWaterfallRow(wells.picture.data(), kWfW, kWfH, sub, subN, state.dbMin, state.dbMax);
            std::memcpy(wells.waterfallMemory(), wells.picture.data(), sizeof(Rgba) * kWfW * kWfH);
            ui->set_trace_image(wells.traceImage());
            ui->set_waterfall_image(wells.waterfallImage());
            wells.flip();
            // the figures around the wells
            ui->set_spectrum_caption(ss("Spectrum \xC2\xB7 " + std::to_string(info.binCount) + " bin" + (cur.spanHz < info.spanHz * 0.999 ? " \xC2\xB7 zoom" : "")));
            ui->set_span_text(ss(spanText(cur.spanHz)));
            const double history = fps > 0 ? kWfH / fps : 0.0;
            ui->set_waterfall_foot_left(ss(fps > 0 ? fmt("%.0f lines/s", fps) + " \xC2\xB7 " + fmt("%.0f s of history", history) : std::string()));
            {
                const auto at = ageTicks(history, kWfH);
                if (sigs.changed("age", sigOf(at))) {
                    std::vector<AgeTick> ticks;
                    for (const auto& tk : at) { ticks.push_back({static_cast<float>(tk.frac == 0.0 ? 22.0 / kWfH : tk.frac), ss(tk.label)}); }   // "now" sits under the Age caption, as the sheet draws it
                    ui->set_age_ticks(std::make_shared<slint::VectorModel<AgeTick>>(std::move(ticks)));
                }
            }
            // the peak inside the passband
            const double lo = fracOf(state.tunedHz - state.bandwidthHz / 2.0, info.centreHz, info.spanHz), hi = fracOf(state.tunedHz + state.bandwidthHz / 2.0, info.centreHz, info.spanHz);
            const std::size_t p0 = static_cast<std::size_t>(std::clamp(lo, 0.0, 1.0) * frame.size()), p1 = std::max(p0 + 1, static_cast<std::size_t>(std::clamp(hi, 0.0, 1.0) * frame.size()));
            float peak = -300.f;
            for (std::size_t i = p0; i < p1 && i < frame.size(); ++i) { peak = std::max(peak, frame[i]); }
            ui->set_show_peak(peak > -299.f);
            ui->set_peak_text(ss(fmt("%.1f dBFS", peak)));
            ui->set_peak_age(ss("now"));
        } else if (lastFrameMs > 0) {
            const double age = static_cast<double>(nowMs() - lastFrameMs) / 1000.0;
            ui->set_peak_age(ss(age < 0.25 ? "now" : fmt("%.1f s ago", age)));
        }
        // the axes and overlays follow the view every tick (a zoom or pan changes them without a new frame)
        {
            {
                const auto fts = frequencyTicks(v.centreHz, v.spanHz, 980);
                if (sigs.changed("freq", sigOf(fts))) { std::vector<AxisTick> ft; for (const auto& tk : fts) { ft.push_back({static_cast<float>(tk.frac), ss(tk.label)}); } ui->set_freq_ticks(std::make_shared<slint::VectorModel<AxisTick>>(std::move(ft))); }
                const auto dts = dbTicks(state.dbMin, state.dbMax, 330);
                if (sigs.changed("db", sigOf(dts))) { std::vector<DbTick> dt; for (const auto& tk : dts) { dt.push_back({static_cast<float>(tk.frac), ss(tk.label)}); } ui->set_db_ticks(std::make_shared<slint::VectorModel<DbTick>>(std::move(dt))); }
            }
            ui->set_vfo_pos(frac(state.tunedHz));
            ui->set_vfo_half(static_cast<float>(v.spanHz > 0 ? state.bandwidthHz / 2.0 / v.spanHz : 0.0));
            ui->set_show_vfo(true);
            std::vector<BandSegment> ribbon;
            for (const auto& b : lists.bands) {
                const double f0 = fracOf(b.startHz, v.centreHz, v.spanHz), f1 = fracOf(b.stopHz, v.centreHz, v.spanHz);
                if (f1 < 0 || f0 > 1) { continue; }
                ribbon.push_back({static_cast<float>(std::clamp(f0, 0.0, 1.0)), static_cast<float>(std::clamp(f1, 0.0, 1.0)), ss(b.name), slint::Color::from_argb_uint8(0, 0, 0, 0)});
            }
            {
                std::string sig; for (const auto& b : ribbon) { sig += fmt("%.5f|", b.from) + fmt("%.5f|", b.to) + std::string(b.label) + ";"; }
                if (sigs.changed("ribbon", sig)) { ui->set_ribbon(std::make_shared<slint::VectorModel<BandSegment>>(std::move(ribbon))); }
            }
            std::vector<BookmarkMark> bms;
            for (const auto& b : lists.bookmarks) { const double f = fracOf(b.hz, v.centreHz, v.spanHz); if (f >= 0 && f <= 1 && bms.size() < 64) { bms.push_back({static_cast<float>(f), ss(b.name), b.favourite}); } }
            {
                std::string sig; for (const auto& b : bms) { sig += fmt("%.5f|", b.pos) + std::string(b.label) + (b.favourite ? "*" : "") + ";"; }
                if (sigs.changed("bookmarks", sig)) { ui->set_bookmarks(std::make_shared<slint::VectorModel<BookmarkMark>>(std::move(bms))); }
            }
            std::vector<PluginMark> marks;
            std::vector<PluginMark> wfMarks;
            for (const auto& m : lists.marks) {
                const double f = fracOf(m.hz, v.centreHz, v.spanHz);
                const double w = m.span && v.spanHz > 0 ? m.widthHz / v.spanHz : 0.0;
                if (f + w < 0 || f > 1) { continue; }
                const std::uint8_t a = static_cast<std::uint8_t>(std::max<std::uint32_t>(0x60, m.rgba & 0xffu));
                const slint::Color colour = m.rgba == 0 ? slint::Color::from_argb_uint8(0, 0, 0, 0) : slint::Color::from_argb_uint8(a, static_cast<std::uint8_t>(m.rgba >> 24), static_cast<std::uint8_t>(m.rgba >> 16), static_cast<std::uint8_t>(m.rgba >> 8));
                marks.push_back({static_cast<float>(f), static_cast<float>(w), ss(m.name), colour, m.dashed});
                if (!m.spectrumOnly) { wfMarks.push_back({static_cast<float>(f), static_cast<float>(w), ss(m.name), colour, m.dashed}); }
            }
            {
                std::string sig; for (const auto& m : marks) { sig += fmt("%.5f|", m.pos) + fmt("%.5f|", m.width) + std::string(m.label) + (m.dashed ? "d" : "") + ";"; }
                std::string wsig; for (const auto& m : wfMarks) { wsig += fmt("%.5f|", m.pos) + fmt("%.5f|", m.width) + ";"; }
                if (sigs.changed("marks", sig)) { ui->set_marks(std::make_shared<slint::VectorModel<PluginMark>>(std::move(marks))); }
                if (sigs.changed("wfmarks", wsig)) { ui->set_wf_marks(std::make_shared<slint::VectorModel<PluginMark>>(std::move(wfMarks))); }
            }
            std::vector<UserMarker> ums;
            for (const auto& m : lists.markers) {
                const double f = fracOf(m.hz, v.centreHz, v.spanHz);
                if (f < 0 || f > 1) { continue; }
                const slint::SharedString label = ss("M" + std::to_string(m.id) + " " + fmt("%.3f MHz", m.hz / 1e6));
                ums.push_back({static_cast<float>(f), label});
            }
            {
                std::string sig; for (const auto& m : ums) { sig += fmt("%.5f|", m.pos) + std::string(m.label) + ";"; }
                if (sigs.changed("markers", sig)) { ui->set_markers(std::make_shared<slint::VectorModel<UserMarker>>(std::move(ums))); }
            }
            ui->set_marker_count(static_cast<int>(lists.markers.size()));
            ui->set_key_foot(ss("FLOOR " + fmt("%.0f dB", state.dbMin) + " \xC2\xB7 CEILING " + fmt("%.0f dB", state.dbMax) + (rangeManual ? " \xC2\xB7 manual" : " \xC2\xB7 auto")));
            {
                std::vector<slint::SharedString> keyLabels; std::string sig;
                for (int i = 0; i < 5; ++i) { keyLabels.push_back(ss(fmt("%.0f", state.dbMin + (state.dbMax - state.dbMin) * i / 4.0))); sig += std::string(keyLabels.back()) + ";"; }
                if (sigs.changed("keylabels", sig)) { ui->set_key_labels(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(keyLabels))); }
            }
            ui->set_waterfall_foot_tuned(ss(fmtMHz(state.tunedHz)));
            ui->set_waterfall_foot_rest(ss(" \xC2\xB7 " + waterfallFootRight(state, lists.decodersFed.empty() ? std::string() : lists.decodersFed.front())));
            if (hoverX >= 0) { ui->set_cursor_text(ss(cursorText(hzAtFrac(hoverX), dbAtFrac(hoverX)))); }
        }

        // answers: log anything the engine refused (a refusal UI is the notices of phase 3)
        FoxCommandResult r[8]; for (auto& x2 : r) { x2.structSize = sizeof(x2); }
        const int32_t got = api->poll_results(session, r, 8);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].status != FOXAPI_OK && r[i].status != FOXAPI_NO_CHANGE) { std::fprintf(stderr, "foxsdr-slint: op 0x%04x answered %d: %s\n", r[i].op, r[i].status, r[i].message); }
        }
    });

    ui->show();
    ui->window().set_size(slint::LogicalSize({1600.f, 900.f}));   // the design size: left to the layout, the window opened at its minimum height
    slint::run_event_loop();

    tick.stop();
    std::fprintf(stderr, "foxsdr-slint: %d frames painted, %d buffer detaches (0 = every frame reused its buffer)\n", wells.frames, wells.detaches);
    api->close_session(session);
    service.stop();
    return 0;
}
