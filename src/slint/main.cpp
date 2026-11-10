// foxsdr-slint - THE SLINT WINDOW (docs/engine-stage4.md 6.36; phase 2, step 2 of the port): the Workbench main
// screen as a client of the C API table, HOSTED AS THE APPLICATION.
//
// The host owns an EngineService exactly as src/main.cpp does, opens a LOCAL session with VIEW|TUNE|SETTINGS, and
// on a 33 ms slint::Timer heartbeats, read_state()s into the window's properties (through host_logic.hpp, the
// tested unit), read_list()s the lists the screen draws when listSeq moves, read_spectrum()s on a NEW frame only
// (painting into two persistent SharedPixelBuffers per well, alternated so a buffer the renderer still holds is
// never written: the detach count printed at exit is the proof), and drains poll_results. Every key, gesture and
// shortcut of ui/main.slint becomes one submit() here; the app-level opcodes (click-to-tune, edge drag, display
// floor/ceiling, markers) come from core/app_commands.hpp, a constants-only header, through the same submit().
// Nothing of the receiver is reached except through the table.
//
// THE BOOT IS THE IMGUI WINDOW'S, STEP FOR STEP (src/main.cpp): bootProcess(ProcessKind::Host, ...) - which owns the
// single-instance claim, the crash handlers, the diagnostics folders, the stored Diagnostics switch, the sentinel, the
// console handler, the helper and tool modes and the refusal of arguments it does not know - then the service built with
// the window application's options, the settings file's lock (a real session only), the restore, the session on the
// table, the start, the run, the stop. bootProcess decides the settings file now: in a real session the user's own
// per-user file, %APPDATA%\foxsdr\config.json; in a bounded run (`--frames N`) none, unless CASCADE_CONFIG_TEST names one.
// The preview's own settings folder, --config, FOXSDR_SLINT_CONFIG and the refusal to touch the real file are GONE, and
// the window is no longer labelled a preview. A SCRATCH RUN is therefore a bounded run with CASCADE_CONFIG_TEST naming
// the file, or an interactive run with APPDATA, LOCALAPPDATA and USERPROFILE redirected to a scratch profile (the GUI test
// gate's way; slint-run.bat does both). A test never runs this program on the real profile.
//
// THE HOUSEKEEPING the ImGui window does around its frame loop - the update check and its card, the crash-report sweep,
// the two senders, the sentinel's phases and heartbeat, the console's close request, the settings handed to the engine at
// the end - is host_duties.hpp, tested without a window. The tick below calls its poll(); the order at the end is
// finish(), the service's stop(), closed() (tests/test_slint_duties.cpp holds this file's order from its text).

#include <algorithm>
#include <array>
#include <chrono>
#include <type_traits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
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
#include "core/config_halves.hpp"
#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/version.hpp"
#include "core/utf8_text.hpp"
#include "core/view_follow.hpp"
#include "engine/engine_service.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/process_boot.hpp"
#include "engine/window_service_options.hpp"
#include "extend_publish.hpp"
#include "host_duties.hpp"
#include "host_logic.hpp"
#include "input_script.hpp"
#include "key_dispatch.hpp"
#include "look_logic.hpp"
#include "kit_publish.hpp"
#include "cjk_register.hpp"
#include "patch_publish.hpp"
#include "transmit_publish.hpp"
#include "scope_publish.hpp"
#include "plugin_kit.hpp"
#include "signal_publish.hpp"
#include "signatures.hpp"
#include "system_publish.hpp"
#include "tools_publish.hpp"

#include "main.h"  // generated from ui/main.slint by slint_target_sources
#include "input_target.hpp"   // after main.h: the scripted input drives the generated MainWindow's elements (docs 6.47)

using namespace foxsdr::slinthost;

// THE TUNE STEPS, with the words the menu shows (docs 6.52): the key wore fmtKHz's "25.0 kHz" while its menu row said "25 kHz" - one table
// names both, so the key reads what the row the user picked read; a step the table does not hold (a saved value) falls back to fmtKHz.
constexpr std::array<std::pair<const char*, double>, 9> kTuneSteps{{{"1 kHz", 1000}, {"5 kHz", 5000}, {"6.25 kHz", 6250}, {"9 kHz", 9000}, {"10 kHz", 10000}, {"12.5 kHz", 12500}, {"25 kHz", 25000}, {"50 kHz", 50000}, {"100 kHz", 100000}}};
static std::string tuneStepLabel(double hz) {
    for (const auto& s : kTuneSteps) { if (std::fabs(hz - s.second) < 1e-9) { return s.first; } }
    return fmtKHz(hz);
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::string fmt(const char* f, double v) { std::string s; cascade::core::formatUtf8(s, f, v); return s; }   // a std::string, never a fixed buffer (test_i18n)
slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

// (Signatures - publish a model only when its content changed - is signatures.hpp, shared with the plugin kit's publisher.)
std::string sigOf(const std::vector<Tick>& t) { std::string s; for (const auto& k : t) { s += fmt("%.6f|", k.frac) + k.label + ";"; } return s; }
std::int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

constexpr std::uint32_t kSpecW = 1024, kSpecH = 340;   // the spectrum trace's painted resolution (image-fit fill scales it)
constexpr std::uint32_t kWfW = 1024, kWfH = 238;       // the waterfall picture: one row per published frame
constexpr double kZoomPerNotch = 1.3;
// The bookmark names' stacking rule works in pixels (docs 6.52): the plot is taken at the trace's painted width, and a caption glyph (Theme.font-cap,
// 10 px) at 6 px - the window never hands its plot width back here, and the rule needs only the proportions to decide which names touch.
constexpr double kSpectrumPlotWidthPx = kSpecW;
constexpr double kSpectrumCapCharPx = 6.0;
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

    // EVERYTHING before the window: the console, the helper modes, the crash handlers, the instance claim, the argument parse, the tool
    // modes, the stderr capture, the console handler and the sentinel - in that order, in bootProcess (src/main.cpp). A helper, a tool
    // mode, a refused argument or a link handed to the instance already running ends the process here.
    const BootResult boot = bootProcess(ProcessKind::Host, argc, argv);
    if (boot.exit) { return boot.exitCode; }
    BoundedRun bounded(boot.frames);
    // The settings file this run owns: boot's answer, whatever it is (see the header comment).
    const std::string configPath = boot.configPath;

    // THE ENGINE'S SERVICE, as src/main.cpp holds it: built first, destroyed last; the window is its client. The diagnostics directory is
    // empty unless this run is ALLOWED to write - which is not the same as whether the user wants it: the stored switch decides that.
    WindowServiceRun run;
    run.configPath = boot.configPath;
    run.diagnosticsDir = boot.mayWrite ? cascade::core::diagCrashDir() : std::string();
    run.frames = boot.frames;
    run.safePlugins = boot.safePlugins;
    if (boot.safePlugins) { std::fprintf(stderr, "foxsdr-slint: safe mode - no plugin will be loaded in this session\n"); }
    ServiceOptions options = windowServiceOptions(run);
    // A BOUNDED RUN'S PASS FREE-RUNS HERE. windowServiceOptions steps a bounded run's pass by the window's frames (the ImGui window runs each
    // part of the pass itself, through the host's hooks); this window has none of that and only reads what the engine publishes, so its N ticks
    // are N heartbeats of a pass that runs on its own thread - what FOXSDR_PASS=free asks of the ImGui window, and what a real session has.
    if (boot.frames >= 0) { options.stepped = false; }
    EngineService service(options);

    // THE SETTINGS FILE'S LOCK (src/main.cpp), for a real session only: the headless engine shares this file by design and refuses to run
    // beside the window on it, so the window application says it is here by taking the lock it looks for. BEST EFFORT, never a refusal (the
    // service's options say so: a second window has always been allowed); a refusal's sentence is shown here and in About all the same, for
    // the day a host asks for a strict lock. A bounded run takes none and stays as hermetic as it was.
    std::string lockSentence;
    const bool lockAsked = boot.frames < 0 && service.persistent();
    bool lockAccepted = true;
    if (lockAsked) {
        lockAccepted = service.acquireConfig(lockSentence);
        if (!lockAccepted) { std::fprintf(stderr, "foxsdr-slint: %s\n", lockSentence.c_str()); }
    }

    // THE RESTORE (the plugin scan, the settings file's load, the receiver's half applied...), then CASCADE_CONFIG_TEST's read-back line.
    const EngineService::Restored restored = service.restore();
    if (boot.announceConfig && service.persistent()) { EngineService::announce(restored); }

    const FoxEngineApi* api = engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
    FoxEngine* handle = service.engine().apiHandle();
    if (api == nullptr || handle == nullptr) { std::fprintf(stderr, "foxsdr-slint: no API table\n"); return 1; }

    FoxSessionParams p{};
    p.structSize = sizeof(p);
    p.flags = FOXAPI_SESSION_LOCAL | FOXAPI_SESSION_FRAME;   // FRAME: our heartbeat kicks the free-running pass, as the window's does
    // The plugin kit's grants too (docs 6.37): TRANSMIT for the transmit grant key, ADMIN for the retired-plugin rows and the catalogue URL.
    p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_ADMIN;
    p.clientName = "FoxSDR Slint interface";
    p.clientVersion = cascade::versionString();
    FoxSession* session = nullptr;
    if (api->open_session(handle, &p, &session) != FOXAPI_OK || session == nullptr) {
        std::fprintf(stderr, "foxsdr-slint: open_session failed\n");
        return 1;
    }
    // The two events the window asks to be told (API 0.3 8.7, 8.14): the receiver tuned off a muting plugin's preset (the "Sound is muted"
    // question), and a plugin's preset was applied (its windows open). The kit drains them once a tick.
    (void)api->subscribe(session, FOXAPI_EVENT_MASK(FOXAPI_EVENT_MUTE_TUNE_AWAY) | FOXAPI_EVENT_MASK(FOXAPI_EVENT_PRESET_APPLIED));

    // THE PLUGIN KIT'S PORT, attached on this thread after the restore and BEFORE the start (src/main.cpp: the engine replays its current
    // directive to a late attach, and a control thread already running would race it). Without the extension the kit stands on a null port
    // and the window has no plugin windows, as the ImGui window has none then.
    foxsdr::slintkit::PluginKit kit({api, handle, session, configPath});
    const bool kitAttached = kit.attach();
    if (!kitAttached) { std::fprintf(stderr, "foxsdr-slint: no plugin UI extension - no plugin windows in this session\n"); }

    // THE ENGINE STARTS HERE: the receiver (a real session), the usage heartbeat, the settings forced to disk once with the unclean-exit
    // marker false, the first pass, the control thread and the keeper thread. A start that fails is stopped all the same (src/main.cpp): the
    // receiver this process started is stopped, and the marker says the run ended cleanly.
    std::string why;
    if (!service.start(why)) {
        std::fprintf(stderr, "foxsdr-slint: %s\n", why.c_str());
        service.stop();
        api->close_session(session);
        return 1;
    }

    // THE HOUSEKEEPING (host_duties.hpp): the interface's half read from the table, the update check, the crash sweep armed, the sentinel
    // left at "creating the window". The close it asks for (the installer has started, the console asked) is the event loop's end.
    LiveWorld world(api, session);
    HostDuties duties(world, [] { slint::quit_event_loop(); });
    {
        DutyStart s;
        s.configPath = configPath;
        s.configLoaded = restored.loaded;
        s.diagnosticsDir = run.diagnosticsDir;
        s.version = cascade::versionString();
        s.frames = boot.frames;
        s.lastRunUnclean = restored.lastRunUnclean;
        s.lock = lockReportFor(lockAsked, lockAccepted, lockSentence, lockAsked ? readLockHolder(configPath) : std::string(), currentProcessId());
        duties.start(s);
    }

    auto ui = MainWindow::create();

    // THE INTERFACE LANGUAGE, as the ImGui window applies it (gui/app_window_language.cpp applyPendingLanguage): the stored `language`
    // setting - "auto" follows the operating system, anything else is a code from resources/lang - read from the interface's half the
    // engine serves (fact.config.interface; only when there is a settings file and it loaded, otherwise AppConfig's default, "auto"),
    // FOXSDR_LANGUAGE over it for this run only (a capture and test seam, never written back), and cascade::i18n::setLanguage's answer
    // is the code in force. The words themselves are the .po files tools/slint-po.py makes from the same catalogues and the Slint
    // compiler bundles (src/slint/CMakeLists.txt), chosen with select_bundled_translation, which Slint only allows once a component
    // exists - hence here, before the first show(). English is the source text and has no .po: nothing is selected for it.
    // FOXSDR_LANG_FILE (a translator's catalogue from disk) is NOT honoured: the bundle is compiled in, so a file read here could not
    // reach a single word on this window and the log line would claim a language the screen does not show.
    {
        std::string languageSetting = "auto";
        if (!configPath.empty() && restored.loaded) {
            std::string document, error;
            cascade::core::AppConfig half;
            if (world.fact("fact.config.interface", document) && !document.empty() && cascade::core::parseInterfaceHalf(document, half, error)) {
                languageSetting = half.language;
            }
        }
        if (const char* env = std::getenv("FOXSDR_LANGUAGE"); env != nullptr && env[0] != '\0') { languageSetting = env; }
        // A CJK language is drawable only where the machine has a face for it (docs 6.62): a saved Japanese on a machine with no Japanese face is kept but not applied, as the classic window does.
        cascade::i18n::setDrawablePredicate(&foxsdr::slintfonts::languageDrawable);
        const std::string inForce = cascade::i18n::setLanguage(languageSetting);
        const char* how = "english";
        if (inForce != "en") { how = slint::select_bundled_translation(inForce) ? "bundled" : "not bundled"; }
        std::fprintf(stderr, "foxsdr-slint: language %s (%s)\n", inForce.c_str(), how);
        // THE CJK FACES (docs 6.62): the system's own, chosen as the classic window chooses them, registered with this window (nothing for a language that needs none).
        const foxsdr::slintfonts::Registered faces = foxsdr::slintfonts::registerForLanguage(ui->window(), inForce);
        ui->global<Theme>().set_cjk_family(ss(faces.family));
        if (!faces.labels.empty()) { std::fprintf(stderr, "foxsdr-slint: CJK faces %s, font-family \"%s\"\n", faces.labels.c_str(), faces.family.c_str()); }
    }

    ui->set_version(ss(cascade::versionString()));
    ui->set_preview(false);   // this window boots as the host now; the property stays for a labelled beta
    ui->set_about_text(ss(aboutText(duties.about())));

    // THE UPDATE CARD'S EIGHT PROPERTIES, from the duties' model and only when it changed (a replaced list model recreates every repeater
    // item behind it). The card's two keys (below) and the tick both call this: what the duties decided is on screen the same tick.
    // THE PLUGIN KIT'S PAGES (kit_publish.hpp): the DECODE bank, the docked and torn-off pages and the "Sound is muted" question, built from
    // the kit's state after each tick and wired to it once.
    foxsdr::slintkit::KitPublisher publisher(*ui, kit);
    publisher.wire();
    // WHAT THE 3D VIEWS REMEMBER (docs 6.65): the view, camera and small choices of every globe view, in the framework's one interface-half text (view3d_prefs.hpp)
    publisher.setViewPrefsHooks([&duties] { return duties.interfaceHalf().view3d; },
                                [&duties](const std::string& text) {
                                    cascade::core::AppConfig half = duties.interfaceHalf();
                                    if (half.view3d == text) { return; }
                                    half.view3d = text;
                                    (void)duties.saveInterfaceHalf(half);
                                });
    // A bounded run's self-capture: the pages FOXSDR_SLINT_OPEN names are opened once the plugins are known, and FOXSDR_SLINT_SHOT=1 saves a
    // screenshot of the window at the last tick (the screenshot key's own path). Never set in a release; the GUI test gate's idiom.
    // FOXSDR_SLINT_STORE_PAGE=<catalogue id> (below) opens the store page on that plugin, so it names the page "store" for the kit as well.
    const char* storePageEnv = std::getenv("FOXSDR_SLINT_STORE_PAGE");
    {
        std::string openSpec;
        if (const char* e = std::getenv("FOXSDR_SLINT_OPEN")) { openSpec = e; }
        if (storePageEnv != nullptr && storePageEnv[0] != '\0' && (',' + openSpec + ',').find(",store,") == std::string::npos) { openSpec += (openSpec.empty() ? "" : ",") + std::string("store"); }
        if (!openSpec.empty()) { publisher.openFromEnv(openSpec); }
    }
    const bool shotAtEnd = std::getenv("FOXSDR_SLINT_SHOT") != nullptr;
    // THE SIGNAL BANK'S BODIES (signal_publish.hpp): the Source, Radio, Audio filters, Sinks, Transmit, Recorder and Frequency correction models, the Radio-setup
    // page and the import dialog, published from signal_logic's reads once a tick (after the kit's) and wired to the engine's ops.
    foxsdr::slintsignal::SignalPublisher signalBank(*ui, kit, api, session);
    signalBank.setBounded(bounded.bounded());   // the I/Q row's file seams (FOXSDR_FILE_PICK, FOXSDR_FILE_CLIPBOARD) exist only in a bounded run
    signalBank.wire();
    if (std::getenv("FOXSDR_SLINT_SETUP") != nullptr) { signalBank.raiseSetupForCapture(); }     // a bounded run's capture of the Radio-setup page
    if (std::getenv("FOXSDR_SLINT_IMPORT") != nullptr) { signalBank.raiseImportForCapture(); }   // ...and of the recorder's import dialog

    const auto applyUpdateCard = [&] {
        if (!duties.takeCardChange()) { return; }
        const UpdateCardModel& c = duties.card();
        std::vector<slint::SharedString> notes;
        for (const auto& n : c.notes) { notes.push_back(ss(n)); }
        ui->set_update_shown(c.shown);
        ui->set_update_headline(ss(c.headline));
        ui->set_update_critical(c.critical);
        ui->set_update_withdrawn(c.withdrawn);
        ui->set_update_notes(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(notes)));
        ui->set_update_primary(ss(c.primary));
        ui->set_update_progress(c.progress);
        ui->set_update_note(ss(c.note));
        ui->set_update_error(ss(c.error));
    };

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
    bool followBandPlan = true;                // the engine's switch (fact.bandPlan.followMode), read when the lists move
    // THE PICTURE FOLLOWS A RETUNE THAT LEAVES IT (core/view_follow.hpp, 2026-11-09): the tuned frequency as the last tick saw it (NaN before the
    // first), and the tick of the last drag of the band, so the picture is never moved under the hand (ten ticks of truce after a drag's last move).
    double lastTunedHz = std::numeric_limits<double>::quiet_NaN();
    int tickNo = 0;
    int lastBandDragTick = -1000;
    int bankIndex = 0;
    if (const char* bankEnv = std::getenv("FOXSDR_SLINT_BANK")) { bankIndex = std::clamp(std::atoi(bankEnv), 0, 4); }   // a bounded run's self-capture: the bank to show
    // SIGNAL's Source, Radio and Sinks rows open (Audio filters and Recorder closed), as the ImGui window; TOOLS' Bookmarks, Scanner and Airband (rows 2, 3, 4); EXTEND's Web access and CAT (the contract: both open); SYSTEM's Display and Key bindings (the LOGICAL rows of
    // the contract: 0 Display ... 5 Key bindings ... 9 About - the rail's positions differ only while the Beta tester row is hidden)
    std::map<int, std::set<int>> openSections{{0, {1, 2, 4}}, {2, {2, 3, 4}}, {3, {0, 1}}, {4, {0, 5}}};
    // A bounded run's self-capture: FOXSDR_SLINT_SECTIONS=<bank>:<i,j,k>[;<bank>:<i,j,k>] sets the open rows of a bank (next to FOXSDR_SLINT_BANK).
    if (const char* sectionsEnv = std::getenv("FOXSDR_SLINT_SECTIONS")) {
        std::string spec = sectionsEnv;
        std::size_t at = 0;
        while (at < spec.size()) {
            std::size_t end = spec.find(';', at);
            if (end == std::string::npos) { end = spec.size(); }
            const std::string part = spec.substr(at, end - at);
            const std::size_t colon = part.find(':');
            if (colon != std::string::npos) {
                const int bank = std::clamp(std::atoi(part.substr(0, colon).c_str()), 0, 4);
                std::set<int> rows;
                std::size_t p = colon + 1;
                while (p <= part.size()) {
                    std::size_t comma = part.find(',', p);
                    if (comma == std::string::npos) { comma = part.size(); }
                    const std::string num = part.substr(p, comma - p);
                    if (!num.empty()) { rows.insert(std::atoi(num.c_str())); }
                    p = comma + 1;
                }
                openSections[bank] = rows;
            }
            at = end + 1;
        }
    }
    MenuKind menuKind = MenuKind::None;
    double menuHz = 0; int menuMarkerNear = -1;
    OutputRate outputRate;
    double hoverX = -1, hoverY = -1;
    std::vector<PaletteAction> paletteActions;
    std::string statusLineBand;
    Signatures sigs;

    // THE SYSTEM BANK'S BODIES (system_publish.hpp): Display, Language & country, Updates, Serial ports, Key bindings, Diagnostics, Usage reporting, Beta tester and the
    // two report pages, published from system_logic's reads over the interface half the duties hold (once a tick, after the signal bank's) and wired to the effects
    // system_logic names. A bounded run sends and opens nothing.
    foxsdr::slintsystem::SystemPublisher::Hooks systemHooks;
    systemHooks.updateCardChanged = [&] { applyUpdateCard(); };
    systemHooks.rangeManual = [&] { rangeManual = true; ui->set_range_manual(true); };
    foxsdr::slintsystem::SystemPublisher systemBank(*ui, kit, duties, world, api, session, bounded.bounded(), systemHooks);
    systemBank.setLastRunUnclean(restored.lastRunUnclean);
    systemBank.wire();
    for (const int row : openSections[4]) { systemBank.sectionOpened(row); }   // a section that starts open asks for what it shows (the Serial ports scan)
    // A bounded run's self-captures (never set in a release): the Language dropdown picks <code> at tick 60 (the whole path, re-published in that language), and
    // FOXSDR_SLINT_REPORT opens the bug page ("feature" or "0" opens the feature request page instead).
    std::string languageChangeCapture;
    if (const char* e = std::getenv("FOXSDR_SLINT_LANGUAGE_CHANGE"); e != nullptr && e[0] != '\0') { languageChangeCapture = e; }
    if (const char* e = std::getenv("FOXSDR_SLINT_REPORT"); e != nullptr && e[0] != '\0') {
        systemBank.raiseReportForCapture((std::string(e) == "feature" || std::string(e) == "0") ? 0 : 1);
    }
    int captureTicks = 0;

    // THE TOOLS BANK'S BODIES (tools_publish.hpp): the Radar fold's position offers, Bookmarks, Scanner, Airband and the Markers sheet, published from tools_logic's reads
    // (once a tick, after the system bank's, whose GPS row the Radar fold shows) and wired to the effects tools_logic names. The Bookmarks Import key with an empty box
    // opens the SIGNAL bank's import dialog. A bounded run writes no clipboard and opens no folder. FOXSDR_SLINT_MARKERS=1 opens the Markers sheet for a capture.
    // THE SCOPE MODES (scope_publish.hpp, docs 6.57): the DEMOD SCOPE window and the radar scope, opened by the TOOLS rows' keys (the hooks below) and published once a tick.
    // FOXSDR_SLINT_DEMOD_SCOPE=<signal 0-4> opens the demod window on that input for a bounded run's capture.
    foxsdr::slintscope::ScopePublisher scopeBank(*ui, kit, api, session, duties, bounded.bounded());
    scopeBank.wire();
    if (const char* e = std::getenv("FOXSDR_SLINT_DEMOD_SCOPE"); e != nullptr && bounded.bounded()) { scopeBank.openDemodForCapture(e[0] >= '0' && e[0] <= '4' ? e[0] - '0' : -1); }
    foxsdr::slinttools::ToolsPublisher::Hooks toolsHooks;
    toolsHooks.demodScopeClicked = [&] { scopeBank.toggleDemod(); };
    toolsHooks.radarScopeClicked = [&] { scopeBank.toggleRadar(); };
    toolsHooks.openImportDialog = [&](const std::string& preset) { signalBank.openImportForPreset(preset); };
    // the two spectrum options are the interface half's (docs 6.52): read at start, saved on each tick of a box, read again every tick by the spectrum's marks
    toolsHooks.onSpectrum = duties.interfaceHalf().bookmarkMarkers;
    toolsHooks.stackNames = duties.interfaceHalf().bookmarkStackNames;
    toolsHooks.saveBookmarkOptions = [&duties](bool onSpectrum, bool stackNames) {
        cascade::core::AppConfig half = duties.interfaceHalf();
        half.bookmarkMarkers = onSpectrum;
        half.bookmarkStackNames = stackNames;
        (void)duties.saveInterfaceHalf(half);
    };
    foxsdr::slinttools::ToolsPublisher toolsBank(*ui, kit, api, session, bounded.bounded(), toolsHooks);
    toolsBank.wire();
    signalBank.setPresetImport([&](const std::string& path, const std::string& preset) { toolsBank.importIntoPreset(path, preset); });   // the import dialog opened for an Airband preset (docs 6.61)
    if (std::getenv("FOXSDR_SLINT_MARKERS") != nullptr) { toolsBank.openMarkers(); }
    // A bounded run's self-capture of the gestures that touch nothing but the window (never set in a release): at tick 60 the Bookmarks search is typed, the Radar fold's
    // position boxes are filled (the Set RX here key is NOT pressed) and the Airband code, frequency and name boxes are typed - the whole path of a gesture: callback,
    // Effect, local state, forced re-read, model, repeater.
    const bool toolsProbe = std::getenv("FOXSDR_SLINT_TOOLS_PROBE") != nullptr;
    // A bounded run's self-capture of a RETUNE WHILE ZOOMED (never set in a release; src/slint/tests/drive_retune_run.cpp reads it):
    // FOXSDR_SLINT_RETUNE_PROBE="<notches>;<MHz>[;off]" zooms the picture <notches> notches about its middle at tick 40, unticks the Radio
    // body's "Follow the band plan's mode when retuning" at tick 50 when "off" is given (the checkbox's own callback), and types <MHz> into
    // the counter at tick 60 (the counter's whole typed path). The bounded run's summary line then says where the picture, the tune and the
    // mode ended - the tester's complaint of 2026-11-09 (the picture left on the old band, the mode not following) made measurable.
    std::string retuneProbe;
    if (const char* e = std::getenv("FOXSDR_SLINT_RETUNE_PROBE"); e != nullptr && e[0] != '\0') { retuneProbe = e; }
    int retuneProbeTicks = 0;
    const auto retuneProbeField = [&](std::size_t i) -> std::string {
        std::size_t start = 0;
        for (std::size_t k = 0; k < i; ++k) {
            const std::size_t sep = retuneProbe.find(';', start);
            if (sep == std::string::npos) { return std::string(); }
            start = sep + 1;
        }
        const std::size_t end = retuneProbe.find(';', start);
        return retuneProbe.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };
    int toolsProbeTicks = 0;
    // a section that starts open asks for what it shows: the Radar fold's GPS row needs the Serial ports scan
    if (openSections[2].count(1) > 0) { systemBank.sectionOpened(4); }

    // THE EXTEND BANK'S BODIES AND THE PLUGIN STORE PAGE (extend_publish.hpp): Web access and CAT control, and the store's model handed to the kit's page of kind 9 (the DECODE
    // bank's "Plugin store" row opens it), published from extend_logic's reads once a tick - BEFORE the kit's publish, so the page is built from this tick's model - and wired
    // to the effects extend_logic names. A bounded run sends nothing (the first-open catalogue read, the pictures, the keys' commands are kept and said once by op).
    // FOXSDR_SLINT_STORE_PAGE=<catalogue id> opens the page on that plugin once the catalogue lists it (the page only: nothing is asked of the network).
    foxsdr::slintextend::ExtendPublisher extendBank(*ui, kit, publisher, api, session, bounded.bounded());
    extendBank.wire();
    if (storePageEnv != nullptr && storePageEnv[0] != '\0') { extendBank.openStorePageForCapture(storePageEnv); }
    if (const char* e = std::getenv("FOXSDR_SLINT_STORE_LOCAL"); e != nullptr && e[0] != '\0') { extendBank.useLocalCatalogueForCapture(e); }
    // FOXSDR_SLINT_EXTEND_PROBE=<file>;<file>... also runs each file through the store's picture decode (the worker, its words and the strip's mapping)
    if (const char* e = std::getenv("FOXSDR_SLINT_EXTEND_PROBE"); e != nullptr && std::strchr(e, '.') != nullptr) {
        std::vector<std::string> files;
        std::string rest = e;
        while (!rest.empty()) {
            const std::size_t semi = rest.find(';');
            const std::string one = rest.substr(0, semi);
            rest = semi == std::string::npos ? std::string() : rest.substr(semi + 1);
            if (!one.empty()) { files.push_back(one); }
        }
        extendBank.probePictures(files);
    }
    // A bounded run's self-capture of the gestures that touch nothing but the window (never set in a release): at tick 60 the web port and user boxes are typed, the store's
    // search is typed and its SITES fold opened with an address in the field - the callback, the Effect, the draft, the forced re-read, the model, the repeater. Nothing
    // is sent (no key that sends is pressed, and a bounded run would not send it).
    const bool extendProbe = std::getenv("FOXSDR_SLINT_EXTEND_PROBE") != nullptr;
    int extendProbeTicks = 0;

    // THE PATCH PAGE (patch_publish.hpp): the band, the canvas and the inspector of view 1 and the Patch presets page of kind 10, published from patch_logic's reads (once a tick,
    // BEFORE the kit's publish, so the presets page is built from this tick's model) and wired to the calls patch_logic names. The page's commands (the draft's graph, the
    // view-shown flag) go through the kit's submit; a bounded run sends only those two (the rest are logged by op and not sent). FOXSDR_SLINT_VIEW=patch starts on the patch view;
    // FOXSDR_SLINT_OPEN=patch-presets (the kit's openFromEnv) opens the presets page; FOXSDR_SLINT_PATCH_PROBE=1 runs the scripted pointer pass of a bounded run (patch_publish.cpp).
    foxsdr::slintpatch::PatchPublisher patchBank(*ui, kit, publisher, duties, api, session, bounded.bounded());
    patchBank.wire();
    // THE TRANSMIT PAGE (transmit_publish.hpp): page kind 11 of the kit, opened by the Transmit row's switch; its key is the engine's hold, re-asserted by this object's tick.
    foxsdr::slinttransmit::TransmitPublisher transmitBank(*ui, publisher, duties, api, session, bounded.bounded());
    transmitBank.wire();
    if (const char* e = std::getenv("FOXSDR_SLINT_VIEW"); e != nullptr && lower(e) == "patch") { ui->set_view_index(1); }
    patchBank.setViewShown(ui->get_view_index() == 1);   // the engine starts out believing "showing": the first tick offers the truth
    if (std::getenv("FOXSDR_SLINT_PATCH_PROBE") != nullptr) { patchBank.enableProbe(); }

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
            if (n <= static_cast<int32_t>(i)) { break; }   // read_list answers HOW MANY there are and fills `it` only for index < that count
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
            // The SIGNAL bank's chips are the host's own (signal_logic: the Source chip, the filters count, the sink state, the recorder, the ppm): the signal publisher
            // reads them with the models; the fallbacks below stand until its first read.
            if (bank == 0) {
                const auto face = signalBank.face(i);
                if (face.valid) { chip = face.chip; tone = face.tone; }
            }
            // The TOOLS bank's chips are the host's too (tools_logic: the scope tap word, OFF, the bookmark count, SCAN / IDLE, LISTEN / IDLE).
            if (bank == 2) {
                const auto face = toolsBank.face(i);
                if (face.valid) { chip = face.chip; tone = face.tone; }
            }
            // The EXTEND bank's too (extend_logic: Web access ON / FAIL / OFF, CAT FAIL / OFF / "N CLI" / ON - the ImGui rail rows' own words).
            if (bank == 3) {
                const auto face = extendBank.face(i);
                if (face.valid) { chip = face.chip; tone = face.tone; }
            }
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
                add(7, "Frequency correction", "--", ChipTone::Neutral);   // the chip is the signal publisher's (the ppm model's: OFF, ON or PPM +1.5)
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
            default: {
                // THE SYSTEM BANK, in the contract's order (main.slint places the bodies by these indices): 0 Display, 1 Language & country, 2 Frequency correction,
                // 3 Updates, 4 Serial ports, 5 Key bindings, 6 Diagnostics, 7 Usage reporting, 8 Beta tester (only while the tester feature is on), 9 About.
                // The chips are the host's (system_logic: the models' own words); row 2's is the SIGNAL family's ppm chip.
                static const char* const names[10] = {"Display", "Language & country", "Frequency correction", "Updates", "Serial ports", "Key bindings", "Diagnostics", "Usage reporting", "Beta tester", "About"};
                for (int logical = 0; logical < 10; ++logical) {
                    if (logical == 8 && !systemBank.testerShown()) { continue; }
                    std::string chip = logical == 9 ? "" : "--";
                    ChipTone tone = ChipTone::Neutral;
                    if (logical == 2) {
                        const auto f = signalBank.face(7);
                        if (f.valid) { chip = f.chip; tone = f.tone; }
                    } else if (logical != 9) {
                        const auto f = systemBank.face(logical);
                        if (f.valid) { chip = f.chip; tone = f.tone; }
                    }
                    rows.push_back({ss(cascade::i18n::tr(names[logical])), ss(chip), tone, logical != 9 && openSections[4].count(logical) > 0, LampState::Off, false});
                }
                break;
            }
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
                e.push_back(entry(MenuItemKind::Heading, 0, "Tune step"));
                for (std::size_t i = 0; i < kTuneSteps.size(); ++i) { e.push_back(entry(MenuItemKind::Radio, 501 + static_cast<int>(i), kTuneSteps[i].first, "", std::fabs(tuneStepHz - kTuneSteps[i].second) < 1e-9)); }
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
    // THE WINDOW'S LOOK (docs 6.61, look_logic.hpp): the saved theme names the skin, the saved tuner style the counter face, the saved interface size the
    // window's scale. Read from the interface half once a tick (a pick in SYSTEM > Display is saved there at once) and at the first tick: what changed since the
    // last look is applied, nothing else - so a counter face picked from the menu stays until the saved style moves.
    foxsdr::slintlook::ScaleFollower scaleFollower;
    std::string lookTheme, lookStyle;
    bool lookStarted = false;
    const auto applyLook = [&] {
        const cascade::core::AppConfig& half = duties.interfaceHalf();
        if (!lookStarted || half.uiTheme != lookTheme || half.tunerDisplayStyle != lookStyle) {
            const bool themeMoved = !lookStarted || half.uiTheme != lookTheme;
            lookStarted = true;
            lookTheme = half.uiTheme;
            lookStyle = half.tunerDisplayStyle;
            if (themeMoved) {
                const foxsdr::slintlook::SkinId id = foxsdr::slintlook::skinOfTheme(half.uiTheme);
                ui->global<Theme>().set_skin(id == foxsdr::slintlook::SkinId::NightWatch ? ui->global<NightWatch>().get_value() : ui->global<Workbench>().get_value());
                std::fprintf(stderr, "foxsdr-slint: look: theme '%s' is drawn in the %s skin\n", half.uiTheme.c_str(), foxsdr::slintlook::skinName(id));
            }
            const foxsdr::slintlook::CounterFace cf = foxsdr::slintlook::faceOfStyle(half.tunerDisplayStyle);
            face = cf == foxsdr::slintlook::CounterFace::Drum ? Face::Drum : cf == foxsdr::slintlook::CounterFace::Plain ? Face::Plain : Face::Nixie;
            applyFace();
        }
        const float cur = ui->window().scale_factor();
        const foxsdr::slintlook::ScalePlan plan = scaleFollower.plan(half.interfaceScale, cur);
        systemBank.setOsScale(plan.os);   // the Auto row says what the system reports, never the pinned number
        if (plan.change) {
            const auto sz = ui->window().size();
            ui->window().dispatch_scale_factor_change_event(plan.want);
            if (!ui->window().is_maximized()) {   // the logical size stays: the window grows with the scale
                const auto ns = foxsdr::slintlook::sizeForScale(sz.width, sz.height, cur, plan.want);
                ui->window().set_size(slint::PhysicalSize({ns.first, ns.second}));
            }
            scaleFollower.applied(plan.want);
            std::fprintf(stderr, "foxsdr-slint: look: interface size '%s' = scale %.2f (the system's %.2f)\n", half.interfaceScale.c_str(), static_cast<double>(plan.want), static_cast<double>(plan.os));
        } else {
            scaleFollower.applied(cur);
        }
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
            case 403: toolsBank.openMarkers(); break;
            case 404: ui->global<ToolsEvents>().invoke_markers_copy(); break;
            case 406: submit(op(FOXAPP_OP_MARKER_CLEAR)); break;
            case 601: ui->set_about_open(true); break;
            case 602: bankIndex = 4; ui->set_bank_index(4); openSections[4].insert(5); break;
            case 603: ui->invoke_shortcut(ss("screenshot")); break;
            case 604: slint::quit_event_loop(); break;
            case 701: case 702: case 703: case 704: case 705: case 706: case 707: case 708: setMode(demodOfIndex(id - 701)); break;
            default:
                if (id >= 501 && id <= 509) {
                    tuneStepHz = kTuneSteps[static_cast<std::size_t>(id - 501)].second;
                    ui->set_tune_step_text(ss(tuneStepLabel(tuneStepHz)));
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
    // (the bias tee's two callbacks - on -> off at once, and the question's "Turn it on" with the radio the question named - are the signal publisher's: signal_publish.cpp wire())
    ui->on_stop_muting_plugin([&] { for (const auto& name : lists.muteCauses) { FoxCommand c = op(FOXAPI_OP_DECODER_STOP); std::snprintf(c.text, sizeof(c.text), "%s", name.c_str()); submit(c); } });

    // ---- rail + panel ----
    ui->on_bank_selected([&](int i) { bankIndex = std::clamp(i, 0, 4); ui->set_bank_index(bankIndex); });
    ui->on_section_clicked([&](int i) {
        // SYSTEM's rail position -> its logical row: the Beta tester row (8) is absent while the tester feature is off, which moves About up one place.
        int row = i;
        if (bankIndex == 4 && i >= 8 && !systemBank.testerShown()) { row = i + 1; }
        if (bankIndex == 4 && row == 9) { ui->set_about_open(true); return; }   // About opens its sheet and never folds
        if (bankIndex == 0 && row == 5) { transmitBank.togglePage(); return; }   // the SIGNAL bank's Transmit row is a switch: it opens (or closes) the Transmit page, it does not fold
        // TOOLS' Demod scope row is a switch that opens a window, not a fold: its click is the scope-clicked gesture (deferred: logged) and nothing opens under it.
        if (bankIndex == 2 && row == 0) { ui->global<ToolsEvents>().invoke_scope_clicked(); return; }
        auto& open = openSections[bankIndex];
        if (open.count(row)) { open.erase(row); } else {
            open.insert(row);
            if (bankIndex == 4) { systemBank.sectionOpened(row); }
            if (bankIndex == 2 && row == 1) { systemBank.sectionOpened(4); }   // the Radar fold's GPS row reads the Serial ports scan
        }
    });
    ui->on_passband_changed([&](float v) {
        const double lo = 3000.0, hi = std::max(lo, 0.9 * state.channelRateHz);
        FoxCommand c = op(FOXAPI_OP_SET_BANDWIDTH); c.num[0] = lo + (hi - lo) * v; submit(c);
    });
    ui->on_tune_step_clicked([&](float x, float y) { openMenu(MenuKind::TuneStep, x, y); });
    // FOLLOW THE BAND PLAN'S MODE WHEN RETUNING: the engine's switch (Engine::followBandModeOnTune, the rule shared with the ImGui window and the web
    // remote); the fact follows on the next publish, so the box is set here as well for the tick in between.
    ui->on_follow_band_plan_toggled([&](bool v) { followBandPlan = v; FoxCommand c = op(FOXAPI_OP_SET_BAND_FOLLOW); c.ival[0] = v ? 1 : 0; submit(c); });
    ui->on_team_message_seen([&] {});
    ui->on_update_primary_clicked([&] { duties.updatePrimaryClicked(); applyUpdateCard(); });
    ui->on_update_dismissed([&] { duties.updateDismissed(); applyUpdateCard(); });

    // ---- centre ----
    ui->on_view_selected([&](int i) { ui->set_view_index(i); patchBank.setViewShown(i == 1); });
    // (the page tabs, the "+ Window" tab, the torn windows and every page body are the plugin kit's: kit_publish.cpp wires them)
    ui->on_trace_mode_selected([&](int i) { traceMode = i; ui->set_trace_mode(i); resetTrace(traceState); });
    ui->on_range_manual_selected([&](bool manual) { rangeManual = manual; ui->set_range_manual(manual); if (!manual) { setDisplayRange(-110, 0); } });
    ui->on_markers_clicked([&] { toolsBank.openMarkers(); });   // the toolbar's Markers key opens the Markers sheet (the marker menu is the waterfall's right-click)
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
    // A DRAG OF THE BAND moves the VFO inside the baseband (SET_VFO_OFFSET, clamped by the engine as the ImGui window's drag is) and not the SOURCE: it sent
    // SET_FREQUENCY before 2026-11-09, which retuned the radio on every pointer move and - once the engine applied the band plan's mode to SET_FREQUENCY -
    // would have flipped the mode under the hand at a band edge; a drag stays the hand's (core/band_mode.hpp). The tick's picture follow keeps off for
    // ten ticks after the last move.
    ui->on_band_dragged([&](float dx) {
        FoxCommand c = op(FOXAPI_OP_SET_VFO_OFFSET);
        c.num[0] = state.tunedHz + dx * currentView().spanHz - state.centreHz;
        submit(c);
        lastBandDragTick = tickNo;
    });
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
    // The status bar's two keys open the SYSTEM bank's report sheet through the report-open effect (system_publish.cpp: applyReportOpen -> the page of that kind).
    ui->on_feature_request_clicked([&] { ui->global<SystemEvents>().invoke_report_open(0); });
    ui->on_bug_report_clicked([&] { ui->global<SystemEvents>().invoke_report_open(1); });

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
        // the contract's logical rows of bank 4, in the order of `settings`: Display 0, Key bindings 5, Updates 3, Language 1, Diagnostics 6, Usage reporting 7, Serial ports 4
        static const int kRowOf[7] = {0, 5, 3, 1, 6, 7, 4};
        for (int i = 0; i < 7; ++i) { const int row = kRowOf[i]; add("SETTING", settings[i], "System bank", [=, &bankIndex, &openSections, &systemBank] { bankIndex = 4; ui->set_bank_index(4); openSections[4].insert(row); systemBank.sectionOpened(row); }); }
        add("PAGE", "Request a feature", "send a request to the FoxSDR team", [&] { systemBank.openReport(0); });
        add("PAGE", "Report a bug / dislike", "send a report to the FoxSDR team", [&] { systemBank.openReport(1); });
        add("ABOUT", "About FoxSDR", "version, licences", [&] { ui->set_about_open(true); });
        if (paletteActions.size() > 12) { paletteActions.resize(12); }
        std::vector<PaletteResult> results;
        for (std::size_t i = 0; i < paletteActions.size(); ++i) { results.push_back({ss(paletteActions[i].kind), ss(paletteActions[i].title), ss(paletteActions[i].detail), static_cast<int>(i)}); }
        ui->set_palette_results(std::make_shared<slint::VectorModel<PaletteResult>>(std::move(results)));
    };
    rebuildPalette("");
    ui->on_palette_query_changed([&](slint::SharedString q) { rebuildPalette(std::string(q)); });
    ui->on_palette_activated([&](int id) { if (id >= 0 && id < static_cast<int>(paletteActions.size())) { paletteActions[static_cast<std::size_t>(id)].run(); } });

    // ---- the shortcuts, by config id (the key table of the ImGui window, as the user has SAVED it: docs 6.56) ----
    // The root FocusScope names a key press and asks here; the answer is the config id of the action that holds the chord in the saved table (the interface half the
    // host keeps, so a rebinding the Key bindings body has just saved answers on the very next press), "" when nothing does.
    ui->on_key_answer([&](slint::SharedString key, bool ctrl, bool shift, bool alt, bool glyphShifted) {
        foxsdr::slintsystem::KeyPress press;
        press.key = std::string(key);
        press.ctrl = ctrl; press.shift = shift; press.alt = alt; press.glyphShifted = glyphShifted;
        return ss(foxsdr::slintsystem::shortcutForPress(press, foxsdr::slintsystem::keyTableFromConfig(duties.interfaceHalf().keyBindings)));
    });
    // A PNG of the window to `path` ("" = the refusal); the screenshot key stamps a name under the settings file's screenshots folder,
    // the scripted input's `shot` names its own file (relative to that folder, or absolute).
    const auto screenshotTo = [&](const std::string& pathIn) -> std::string {
        auto shot = ui->window().take_snapshot();
        if (!shot) { return "the snapshot was refused"; }
        std::filesystem::path dir = std::filesystem::path(configPath).parent_path() / "screenshots";
        std::filesystem::path path(pathIn);
        if (path.is_relative()) { path = dir / path; }
        std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec);
        std::vector<unsigned char> out;
        const auto sink = [](void* ctx, void* data, int size) { auto* v = static_cast<std::vector<unsigned char>*>(ctx); const auto* b = static_cast<const unsigned char*>(data); v->insert(v->end(), b, b + size); };
        stbi_write_png_to_func(sink, &out, static_cast<int>(shot->width()), static_cast<int>(shot->height()), 4, shot->begin(), static_cast<int>(shot->width()) * 4);
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        if (f == nullptr) { return "cannot write " + path.string(); }
        std::fwrite(out.data(), 1, out.size(), f); std::fclose(f);
        std::fprintf(stderr, "foxsdr-slint: screenshot %s\n", path.string().c_str());
        return std::string();
    };
    const auto screenshot = [&] {
        const std::time_t t = std::time(nullptr); char stamp[32]; std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
        const std::string why = screenshotTo(std::string("foxsdr-") + stamp + ".png");
        if (!why.empty()) { std::fprintf(stderr, "foxsdr-slint: %s\n", why.c_str()); }
    };
    ui->on_shortcut([&](slint::SharedString idS) {
        const std::string id(idS);
        if (id == "startStop") { FoxCommand c = op(FOXAPI_OP_RUN); c.ival[0] = (state.flags & FOXAPI_RX_RUNNING) ? 0 : 1; submit(c); }
        else if (id == "mute") { FoxCommand c = op(FOXAPI_OP_SET_MUTED); c.ival[0] = (state.flags & FOXAPI_RX_MUTED) ? 0 : 1; submit(c); }
        else if (id == "volumeUp") { setVolume(state.volume + 0.05); }
        else if (id == "volumeDown") { setVolume(state.volume - 0.05); }
        else if (id == "squelchUp") { setSquelch(std::min(0.0, (squelchAuto(state.squelchDb) ? -120.0 : state.squelchDb) + 2.0)); }
        else if (id == "squelchDown") { setSquelch(std::max(-120.0, (squelchAuto(state.squelchDb) ? -120.0 : state.squelchDb) - 2.0)); }
        else if (id == "modeNFM") { setMode(FOXAPI_DEMOD_NFM); } else if (id == "modeWFM") { setMode(FOXAPI_DEMOD_WFM); }
        else if (id == "modeAM") { setMode(FOXAPI_DEMOD_AM); } else if (id == "modeDSB") { setMode(FOXAPI_DEMOD_DSB); }
        else if (id == "modeUSB") { setMode(FOXAPI_DEMOD_USB); } else if (id == "modeCW") { setMode(FOXAPI_DEMOD_CW); }
        else if (id == "modeLSB") { setMode(FOXAPI_DEMOD_LSB); } else if (id == "modeRAW") { setMode(FOXAPI_DEMOD_RAW); }
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
        else if (id == "openSettings") { bankIndex = 4; ui->set_bank_index(4); openSections[4].insert(5); }
        else if (id == "escape" && scopeBank.radarOn() && !ui->get_editing() && !ui->get_about_open()) { scopeBank.leaveRadar(); }   // the radar scope's second way out (aw.cpp 13589): not while a field is being edited
        else if (id == "escape") {
            // ONE LAYER A PRESS (docs 6.61, key_dispatch.hpp escapeStep): the import dialog, then the Plugin store's plugin page (back to the grid), else the overlays
            foxsdr::slintsystem::EscapeState open;
            open.importDialog = ui->get_import_dialog().shown;
            open.storePluginPage = extendBank.models().store.plugin.shown;
            switch (foxsdr::slintsystem::escapeStep(open)) {
                case foxsdr::slintsystem::EscapeStep::ImportDialog: ui->global<SignalEvents>().invoke_import_closed(); break;
                case foxsdr::slintsystem::EscapeStep::StorePluginPage: ui->global<ExtendEvents>().invoke_store_plugin_back(); break;
                case foxsdr::slintsystem::EscapeStep::Overlays: ui->set_editing(false); ui->set_about_open(false); ui->set_bias_question_open(false); break;
            }
        }
        else if (id == "uiSizeAuto") { systemBank.setInterfaceSize(0); }   // Ctrl+0: back to Auto (saved; the look takes it on the next tick)
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
        ui->set_win_w(static_cast<float>(w) / scale());
        ui->set_win_h(static_cast<float>(h) / scale());
    });

    // ---- the tick: heartbeat, read_state, lists, read_spectrum, poll_results -> properties ----
    const auto onTick = [&] {
        (void)api->heartbeat(session);
        if (api->read_state(session, &state) < 0) { return; }
        const bool listMoved = state.listSeq != lastListSeq;
        if (listMoved) {
            lastListSeq = state.listSeq;
            refreshLists();
            // The Radio body's "Follow the band plan's mode when retuning": the engine's switch, a fact in the lists' block ("1"/"0"; not yet published reads as on).
            char followBuf[8] = {};
            const int32_t n = api->get_setting(session, "fact.bandPlan.followMode", followBuf, sizeof(followBuf));
            followBandPlan = n < 0 || std::string(followBuf, static_cast<std::size_t>(std::max<int32_t>(0, std::min<int32_t>(n, 7)))) != "0";
            ui->set_follow_band_plan(followBandPlan);
        }
        ++tickNo;
        // THE PICTURE FOLLOWS A RETUNE THAT LEAVES IT (a beta tester, 2026-11-09: the waterfall left on 7 MHz after a switch to 3.5 MHz): a zoomed picture
        // (view.spanHz > 0) is a window in absolute Hz that no retune touched; when the tuned frequency moved since the last tick and landed outside it, the
        // picture is recentred on it at the same width, inside the baseband the source now delivers (core/view_follow.hpp, the rule shared with the ImGui
        // window). Not within ten ticks of a drag of the band: the hand has it.
        if (view.spanHz > 0 && tickNo - lastBandDragTick > 10) {
            const double full = fullSpan();
            const cascade::core::ViewFollow f = cascade::core::recentreOnRetune(view.centreHz - view.spanHz / 2.0, view.centreHz + view.spanHz / 2.0,
                                                                                state.centreHz - full / 2.0, state.centreHz + full / 2.0, lastTunedHz, state.tunedHz);
            if (f.moved) { view = {0.5 * (f.lowHz + f.highHz), f.highHz - f.lowHz}; }
        }
        lastTunedHz = state.tunedHz;
        const bool running = (state.flags & FOXAPI_RX_RUNNING) != 0;

        // the strip
        ui->set_running(running);
        ui->set_decoding((state.flags & FOXAPI_RX_DECODER_ACTIVE) != 0);
        ui->set_muted((state.flags & FOXAPI_RX_MUTED) != 0 || state.audioMutedBy[0] != '\0');
        ui->set_failed((state.flags & FOXAPI_RX_FAULTED) != 0);
        ui->set_maximised(ui->window().is_maximized());
        {   // the screen follows the real window size (maximise, restore, a DPI change)
            const auto sz = ui->window().size();
            const float sf = std::max(0.5f, ui->window().scale_factor());
            if (sz.width > 0 && sz.height > 0) { ui->set_win_w(static_cast<float>(sz.width) / sf); ui->set_win_h(static_cast<float>(sz.height) / sf); }
        }
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
            if (bankIndex == 0 && rows.size() > 5) { rows[5].open = transmitBank.pageOpen(); }   // the Transmit row is lit while its page is open
            std::string sig = std::to_string(bankIndex) + ":";
            for (const auto& r : rows) { sig += std::string(r.name) + "|" + std::string(r.chip) + "|" + std::to_string(static_cast<int>(r.tone)) + "|" + (r.open ? "1" : "0") + ";"; }
            if (sigs.changed("sections", sig)) { ui->set_sections(std::make_shared<slint::VectorModel<SectionRow>>(std::move(rows))); }
        }
        ui->set_bank_chip(ss(bankIndex == 0 ? (running ? "RECEIVING" : "STOPPED") : "--"));
        ui->set_bank_chip_tone(bankIndex == 0 && running ? ChipTone::Good : ChipTone::Neutral);
        const double lo = 3000.0, hi = std::max(lo + 1.0, 0.9 * state.channelRateHz);
        ui->set_passband_norm(static_cast<float>(std::clamp((state.bandwidthHz - lo) / (hi - lo), 0.0, 1.0)));
        ui->set_passband_text(ss(fmtKHz(state.bandwidthHz)));
        ui->set_tune_step_text(ss(tuneStepLabel(tuneStepHz)));
        std::string band;
        for (const auto& b : lists.bands) { if (state.tunedHz >= b.startHz && state.tunedHz <= b.stopHz) { band = b.name; break; } }
        ui->set_source_line(ss(sourceLine(state, band)));
        ui->set_rate_line(ss(rateLine(state)));
        ui->set_converter_line(ss(lists.converter ? "via " + lists.converterName + " \xC2\xB7 LO " + fmtMHz(lists.converterLoHz) : std::string()));
        ui->set_team_message_shown(false);  // the team message cards are a later phase's (the update card is the duties': applyUpdateCard)

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
            // THE BOOKMARK MARKS (docs 6.52): none while "On the spectrum" is off (bookmarkMarkers, as the classic drawBookmarkMarkers has it); the
            // names stack onto up to three rows while "Stack close names" is on (bookmarkStackNames), one row otherwise - bookmarkNameRows is the rule.
            std::vector<BookmarkMark> bms;
            if (duties.interfaceHalf().bookmarkMarkers) {
                std::vector<BookmarkName> names;
                for (const auto& b : lists.bookmarks) { const double f = fracOf(b.hz, v.centreHz, v.spanHz); if (f >= 0 && f <= 1 && names.size() < 64) { names.push_back({f, b.name, b.favourite}); } }
                const std::vector<int> rows = bookmarkNameRows(names, kSpectrumPlotWidthPx, kSpectrumCapCharPx, duties.interfaceHalf().bookmarkStackNames ? kBookmarkNameMaxRows : 1);
                for (std::size_t i = 0; i < names.size(); ++i) { bms.push_back({static_cast<float>(names[i].pos), ss(names[i].label), names[i].favourite, rows[i]}); }
            }
            {
                std::string sig; for (const auto& b : bms) { sig += fmt("%.5f|", b.pos) + std::string(b.label) + (b.favourite ? "*" : "") + fmt("%d;", b.row); }
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
            toolsBank.onAnswer(r[i]);   // the answers the airband section waits for (an airport's rows, a preset's chunks, Listen, Export, Remove)
            extendBank.onAnswer(r[i]);  // a SERVER_CONFIG's (Web access and CAT control): a refusal becomes the web box's red line
            patchBank.onAnswer(r[i]);   // the draft's SET_GRAPH (its commit is settled by this answer) and the radio scan's
            transmitBank.onAnswer(r[i]);   // a refused LATCH or consent (the Transmit page's key note)
            if (r[i].status != FOXAPI_OK && r[i].status != FOXAPI_NO_CHANGE) { std::fprintf(stderr, "foxsdr-slint: op 0x%04x answered %d: %s\n", r[i].op, r[i].status, r[i].message); }
        }

        // THE PLUGIN KIT'S DUTIES (plugin_kit.hpp): the directive, the poll, the pictures, the lists when they moved, the front end's facts,
        // the browser's tiles, the track-info lines, the plugin-use table, the events and the tickets - then its pages (kit_publish.cpp).
        kit.tick(state, listMoved, face == Face::Nixie ? "nixie" : face == Face::Drum ? "neon" : "plain");
        extendBank.publish(state);   // before the kit's pages: the store page of kind 9 is built from the model this hands over
        for (const FoxCommandResult& r2 : kit.takeUnclaimedResults()) { patchBank.onAnswer(r2); transmitBank.onAnswer(r2); }   // an answer the kit's own poll took first: the draft waits for it
        patchBank.publish(state);    // before the kit's pages too: the Patch presets page of kind 10 is built from the model this hands over
        transmitBank.publish(state); // ... and the Transmit page of kind 11 (and the key's batch for this tick)
        // THE MAP'S LOOK, from the interface half each tick (SYSTEM > Display saves it there): the aircraft icon size, the trail width and the trail style, bounded
        // as the classic window bounds them (map_compositor.hpp mapLookOf). The kit republishes the maps only when the look changed.
        publisher.setMapLook(foxsdr::slintmap::mapLookOf(duties.interfaceHalf().aircraftIconPx, duties.interfaceHalf().mapTrailWidthPx, duties.interfaceHalf().mapTrailStyle));
        publisher.publish(state);
        signalBank.publish(state);
        systemBank.publish(state);
        scopeBank.publish(state, static_cast<double>(nowMs()) / 1000.0);
        toolsBank.setScopeFacts(scopeBank.demodSignal(), scopeBank.demodOpen(), scopeBank.demodLive(), scopeBank.radarOn());
        toolsBank.publish(state, systemBank.models().serial.gps);
        if (!retuneProbe.empty()) {
            ++retuneProbeTicks;
            if (retuneProbeTicks == 40) {
                const int notches = std::atoi(retuneProbeField(0).c_str());
                for (int n = 0; n < notches; ++n) { zoomView(kZoomPerNotch, 0.5); }
            }
            if (retuneProbeTicks == 50 && retuneProbeField(2) == "off") { ui->invoke_follow_band_plan_toggled(false); }
            if (retuneProbeTicks == 60) { ui->invoke_typed(ss(retuneProbeField(1))); }
        }
        if (toolsProbe && ++toolsProbeTicks == 60) {
            auto&& te = ui->global<ToolsEvents>();
            te.invoke_bookmark_search(ss("kord"));
            te.invoke_position_lat(ss("51.50"));
            te.invoke_position_lon(ss("-0.12"));
            te.invoke_airband_code(ss("kord"));
            te.invoke_airband_freq(ss("118.1"));
            te.invoke_airband_name(ss("Probe tower"));
        }
        if (extendProbe && ++extendProbeTicks == 60) {
            auto&& xe = ui->global<ExtendEvents>();
            xe.invoke_web_port(ss("9090"));
            xe.invoke_web_user(ss("probe"));
            xe.invoke_store_search(ss("ads"));
            xe.invoke_store_sites_toggle();
            xe.invoke_store_site_draft(ss("https://example.invalid/index.json"));
        }
        // the bounded run's self-capture of the language path: at tick 60 the Language dropdown picks the code (save, setLanguage, bundled translation, re-publish)
        if (!languageChangeCapture.empty() && ++captureTicks == 60) { systemBank.languageChangeForCapture(languageChangeCapture); }
    };

    // THE SCRIPTED INPUT (input_script.hpp, input_target.hpp; docs 6.47): FOXSDR_INPUT_SCRIPT names a file of steps that a bounded run plays
    // against this window's controls by their accessible labels, each step's effect read back from the engine through the table by name
    // (`state`). Only in a bounded run, like every other hook here; a script that does not parse ends the run before the window opens
    // (exit 2), a step that fails or is never reached makes the run exit 3.
    const auto readState = [&](const std::string& name, std::string& out) -> std::string {
        static const std::pair<const char*, std::uint32_t> flagNames[] = {
            {"running", FOXAPI_RX_RUNNING}, {"deviceOpen", FOXAPI_RX_DEVICE_OPEN}, {"faulted", FOXAPI_RX_FAULTED}, {"muted", FOXAPI_RX_MUTED},
            {"squelchOpen", FOXAPI_RX_SQUELCH_OPEN}, {"stereo", FOXAPI_RX_STEREO_ENABLED}, {"stereoActive", FOXAPI_RX_STEREO_ACTIVE}, {"nr", FOXAPI_RX_NR},
            {"notch", FOXAPI_RX_NOTCH}, {"autoNotch", FOXAPI_RX_AUTO_NOTCH}, {"agc", FOXAPI_RX_DEVICE_AGC}, {"agcSupported", FOXAPI_RX_AGC_SUPPORTED},
            {"recordingIq", FOXAPI_RX_RECORDING_IQ}, {"recordingAudio", FOXAPI_RX_RECORDING_AUDIO}, {"scanner", FOXAPI_RX_SCANNER_ACTIVE},
            {"decoderActive", FOXAPI_RX_DECODER_ACTIVE}, {"txAvailable", FOXAPI_RX_TX_AVAILABLE}, {"txKeyed", FOXAPI_RX_TX_KEYED}, {"sinkOpen", FOXAPI_RX_SINK_OPEN},
            {"webListening", FOXAPI_RX_WEB_LISTENING}, {"biasTeePresent", FOXAPI_RX_BIAS_TEE_PRESENT}, {"biasTeeOn", FOXAPI_RX_BIAS_TEE_ON},
            {"patchRunning", FOXAPI_RX_PATCH_RUNNING}, {"deviceOpening", FOXAPI_RX_DEVICE_OPENING}, {"positionSet", FOXAPI_RX_POSITION_SET}};
        for (const auto& f : flagNames) { if (name == f.first) { out = (state.flags & f.second) ? "1" : "0"; return std::string(); } }
        static const std::pair<const char*, std::uint32_t> listNames[] = {
            {"modes", FOXAPI_LIST_MODES}, {"bandwidths", FOXAPI_LIST_BANDWIDTHS}, {"devices", FOXAPI_LIST_DEVICES}, {"sampleRates", FOXAPI_LIST_SAMPLE_RATES},
            {"gains", FOXAPI_LIST_GAINS}, {"antennas", FOXAPI_LIST_ANTENNAS}, {"bookmarks", FOXAPI_LIST_BOOKMARKS}, {"decoders", FOXAPI_LIST_DECODERS},
            {"plugins", FOXAPI_LIST_PLUGINS}, {"catalogue", FOXAPI_LIST_CATALOGUE}, {"audioDevices", FOXAPI_LIST_AUDIO_DEVICES}, {"freqMarkers", FOXAPI_LIST_FREQ_MARKERS},
            {"marks", FOXAPI_LIST_MARKERS}, {"bandPlan", FOXAPI_LIST_BAND_PLAN}, {"presets", FOXAPI_LIST_PRESETS}, {"deviceOptions", FOXAPI_LIST_DEVICE_OPTIONS},
            {"serialPorts", FOXAPI_LIST_SERIAL_PORTS}, {"userPresets", FOXAPI_LIST_USER_PRESETS}, {"bookmarkGroups", FOXAPI_LIST_BOOKMARK_GROUPS},
            {"statusLines", FOXAPI_LIST_STATUS_LINES}, {"converters", FOXAPI_LIST_CONVERTERS}, {"patchNodes", FOXAPI_LIST_PATCH_NODES}, {"patchWires", FOXAPI_LIST_PATCH_WIRES},
            {"recordings", FOXAPI_LIST_RECORDINGS}, {"decoderLines", FOXAPI_LIST_DECODER_LINES}, {"bandPlans", FOXAPI_LIST_BAND_PLANS}, {"muteCauses", FOXAPI_LIST_MUTE_CAUSES},
            {"pluginCommands", FOXAPI_LIST_PLUGIN_COMMANDS}, {"servers", FOXAPI_LIST_SERVERS}, {"airbandChannels", FOXAPI_LIST_AIRBAND_CHANNELS},
            {"pluginSettings", FOXAPI_LIST_PLUGIN_SETTINGS}, {"storeUpdates", FOXAPI_LIST_STORE_UPDATES}, {"gps", FOXAPI_LIST_GPS}, {"patchPresetsList", FOXAPI_LIST_PATCH_PARTS}};
        // count.<list> / list.<list>.count|names|details|values - <list> a name above or the table's number; names and details are joined by " | "
        if (name.rfind("count.", 0) == 0 || name.rfind("list.", 0) == 0) {
            const bool count = name.rfind("count.", 0) == 0;
            std::string spec = name.substr(count ? 6 : 5);
            std::string what = "count";
            if (!count) {
                const std::size_t dot = spec.find('.');
                if (dot == std::string::npos) { return "list.<list>.count|names|details|values, not " + name; }
                what = spec.substr(dot + 1);
                spec = spec.substr(0, dot);
            }
            std::uint32_t id = 0;
            for (const auto& l : listNames) { if (spec == l.first) { id = l.second; } }
            if (id == 0 && !spec.empty() && std::isdigit(static_cast<unsigned char>(spec[0]))) { id = static_cast<std::uint32_t>(std::atoi(spec.c_str())); }
            if (id == 0) { return "no list named " + spec; }
            int n = 0;
            std::string joined;
            readList(id, [&](const FoxListItem& it) {
                ++n;
                const std::string piece = what == "names" ? std::string(it.name) : what == "details" ? std::string(it.detail) : fmt("%.6f", it.value[0]);
                joined += (joined.empty() ? "" : " | ") + piece;
            });
            if (what == "count") { out = std::to_string(n); return std::string(); }
            if (what == "names" || what == "details" || what == "values") { out = joined; return std::string(); }
            return "list.<list>.count|names|details|values, not " + name;
        }
        // update.<shown|headline|note|primary|withdrawn>: the update card as the duties made it (docs 6.63: the kill switch's notice is read back through these)
        if (name.rfind("update.", 0) == 0) {
            const UpdateCardModel& card = duties.card();
            const std::string what = name.substr(7);
            if (what == "shown") { out = card.shown ? "1" : "0"; return std::string(); }
            if (what == "headline") { out = card.headline; return std::string(); }
            if (what == "note") { out = card.note; return std::string(); }
            if (what == "primary") { out = card.primary; return std::string(); }
            if (what == "withdrawn") { out = card.withdrawn ? "1" : "0"; return std::string(); }
            return "update.shown|headline|note|primary|withdrawn, not " + name;
        }
        if (name.rfind("fact.", 0) == 0) { return world.fact(name.c_str(), out) ? std::string() : "no fact " + name; }
        if (name == "ui.language") { out = cascade::i18n::current(); return std::string(); }   // the code in force (docs 6.62: the CJK drive scripts read it and the face named below)
        if (name == "ui.cjkFamily") { out = std::string(ui->global<Theme>().get_cjk_family()); return std::string(); }
        if (name.rfind("transmit.", 0) == 0) { return transmitBank.readState(name, out); }   // the Transmit page's page / key / sent / withheld read-back (transmit_publish.hpp)
        // cfg.<field>: the interface half of the settings AS THE HOST HOLDS IT (host_duties interfaceHalf, kept by every bank's saveInterfaceHalf) - the
        // read-back for a control whose value never reaches the engine (the theme, the trail style, the diagnostics switch...). Scalars and strings;
        // a bool reads "true" / "false". An Engine-owned field reads the value the host loaded at start, not the engine's: ask the engine's state for those.
        if (name.rfind("cfg.", 0) == 0) {
            const std::string field = name.substr(4);
            const cascade::core::AppConfig& half = duties.interfaceHalf();
            bool hit = false, supported = true;
            cascade::core::forEachConfigField([&](const char* n, cascade::core::ConfigOwner, bool, auto member) {
                if (hit || field != n) { return; }
                hit = true;
                using T = std::decay_t<decltype(half.*member)>;
                if constexpr (std::is_same_v<T, bool>) { out = (half.*member) ? "true" : "false"; }
                else if constexpr (std::is_same_v<T, std::string>) { out = half.*member; }
                else if constexpr (std::is_floating_point_v<T>) { out = fmt("%.6f", static_cast<double>(half.*member)); }
                else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) { out = std::to_string(static_cast<long long>(half.*member)); }
                else { supported = false; }
            });
            if (!hit) { return "no settings field named " + field; }
            if (!supported) { return "cfg." + field + " is not a scalar or a string"; }
            return std::string();
        }
        // look.<what>: what the window is DRAWN with (docs 6.61), read from the window itself, not from the settings: scale (Window::scale_factor), width / height (the
        // logical size the screen lays out in), bg (the ground colour in force, "#rrggbb"), accent, face (nixie / neon / plain)
        // shown.<what>: whether a dialog or page is up (docs 6.61): import (the import dialog), storePlugin (the Plugin store's plugin page), about
        if (name.rfind("shown.", 0) == 0) {
            const std::string what = name.substr(6);
            if (what == "import") { out = ui->get_import_dialog().shown ? "1" : "0"; return std::string(); }
            if (what == "storePlugin") { out = extendBank.models().store.plugin.shown ? "1" : "0"; return std::string(); }
            if (what == "about") { out = ui->get_about_open() ? "1" : "0"; return std::string(); }
            return "shown.import|storePlugin|about, not " + name;
        }
        if (name.rfind("look.", 0) == 0) {
            const std::string what = name.substr(5);
            const auto hex = [](const slint::Color& c) {
                char b[16];
                std::snprintf(b, sizeof(b), "#%02x%02x%02x", static_cast<unsigned>(c.red()), static_cast<unsigned>(c.green()), static_cast<unsigned>(c.blue()));
                return std::string(b);
            };
            if (what == "scale") { out = fmt("%.2f", static_cast<double>(ui->window().scale_factor())); return std::string(); }
            if (what == "width") { out = fmt("%.0f", static_cast<double>(ui->get_win_w())); return std::string(); }
            if (what == "height") { out = fmt("%.0f", static_cast<double>(ui->get_win_h())); return std::string(); }
            if (what == "bg") { out = hex(ui->global<Theme>().get_bg()); return std::string(); }
            if (what == "accent") { out = hex(ui->global<Theme>().get_accent()); return std::string(); }
            if (what == "face") { out = face == Face::Nixie ? "nixie" : face == Face::Drum ? "neon" : "plain"; return std::string(); }
            return "look.scale|width|height|bg|accent|face, not " + name;
        }
        if (name == "tunedHz") { out = fmt("%.6f", state.tunedHz); return std::string(); }
        if (name == "centreHz") { out = fmt("%.6f", state.centreHz); return std::string(); }
        if (name == "vfoOffsetHz") { out = fmt("%.6f", state.vfoOffsetHz); return std::string(); }
        if (name == "sampleRateHz") { out = fmt("%.6f", state.sampleRateHz); return std::string(); }
        if (name == "channelRateHz") { out = fmt("%.6f", state.channelRateHz); return std::string(); }
        if (name == "bandwidthHz") { out = fmt("%.6f", state.bandwidthHz); return std::string(); }
        if (name == "squelchDb") { out = fmt("%.6f", state.squelchDb); return std::string(); }
        if (name == "volume") { out = fmt("%.6f", state.volume); return std::string(); }
        if (name == "dbMin") { out = fmt("%.6f", state.dbMin); return std::string(); }
        if (name == "dbMax") { out = fmt("%.6f", state.dbMax); return std::string(); }
        if (name == "nrStrength") { out = fmt("%.6f", state.nrStrength); return std::string(); }
        if (name == "notchHz") { out = fmt("%.6f", state.notchHz); return std::string(); }
        if (name == "txFrequencyHz") { out = fmt("%.6f", state.txFrequencyHz); return std::string(); }
        if (name == "demodMode") { out = std::to_string(state.demodMode); return std::string(); }
        if (name == "mode") { out = modeName(state.demodMode); return std::string(); }
        if (name == "deemphasis") { out = std::to_string(state.deemphasis); return std::string(); }
        if (name == "decodersRunning") { out = std::to_string(state.decodersRunning); return std::string(); }
        if (name == "decodersFitted") { out = std::to_string(state.decodersFitted); return std::string(); }
        if (name == "decoderLinesTotal") { out = std::to_string(state.decoderLinesTotal); return std::string(); }
        if (name == "gainDb") {
            FoxListItem it{}; it.structSize = sizeof(it);
            if (api->read_list(session, FOXAPI_LIST_GAINS, 0, &it) <= 0) { return std::string("no gain stage"); }
            out = fmt("%.6f", it.value[3]); return std::string();
        }
        if (name == "deviceName") { out = state.deviceName; return std::string(); }
        if (name == "sinkName") { out = state.sinkName; return std::string(); }
        if (name == "faultMessage") { out = state.faultMessage; return std::string(); }
        if (name == "audioMutedBy") { out = state.audioMutedBy; return std::string(); }
        // kit.<what>: what the plugin kit holds (the bounded summary's figures, docs 6.49): plugins, tracks, panels, instruments, pictures, lines, mapPages
        if (name.rfind("kit.", 0) == 0) {
            const std::string what = name.substr(4);
            std::size_t n = 0;
            if (what == "plugins") { n = kit.lists().rows.size(); }
            else if (what == "tracks") { n = kit.tracks().size(); }
            else if (what == "panels") { n = kit.panels().size(); }
            else if (what == "instruments") { n = kit.instruments().size(); }
            else if (what == "pictures") { n = kit.images().size(); }
            else if (what == "lines") { n = kit.decoderLines().size(); }
            else if (what == "mapPages") { n = kit.mapPages().size(); }
            else { return "kit.plugins|tracks|panels|instruments|pictures|lines|mapPages, not " + name; }
            out = std::to_string(n);
            return std::string();
        }
        // globe.<propagation|ft8|goes|solar|satellites|status>: the 3D views' state (globe_publish.hpp stateText, docs 6.65)
        if (name.rfind("globe.", 0) == 0) { out = publisher.globeState(name.substr(6)); return std::string(); }
        // scope.<what>: the scope modes' own state (scope_publish.hpp readState: demodOpen, signal, live, audioCount, bins, segments, ..., radarOn, range ...)
        if (name.rfind("scope.", 0) == 0) { return scopeBank.readState(name.substr(6), out); }
        if (name == "bank") { out = std::to_string(ui->get_bank_index()); return std::string(); }
        if (name == "view") { out = std::to_string(ui->get_view_index()); return std::string(); }
        if (name == "pages") { out = publisher.pagesLine(); return std::string(); }
        if (name == "tick") { out = std::to_string(bounded.ticks() + 1); return std::string(); }
        return "no state named " + name;
    };
    std::unique_ptr<foxsdr::slintdrive::SlintDriveTarget<MainWindow>> driveTarget;
    std::unique_ptr<foxsdr::slintdrive::Driver> drive;
    bool scriptRefused = false;
    if (const char* scriptPath = std::getenv("FOXSDR_INPUT_SCRIPT"); scriptPath != nullptr && scriptPath[0] != '\0' && bounded.bounded()) {
        std::string text;
        if (std::FILE* f = std::fopen(scriptPath, "rb")) {
            char buf[4096]; std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { text.append(buf, n); }
            std::fclose(f);
        } else {
            std::fprintf(stderr, "foxsdr-slint: input script: cannot read %s\n", scriptPath);
            scriptRefused = true;
        }
        if (!scriptRefused) {
            foxsdr::slintdrive::Script script = foxsdr::slintdrive::parseInputScript(text);
            for (const auto& e : script.errors) { std::fprintf(stderr, "foxsdr-slint: input script: %s\n", e.c_str()); }
            if (!script.errors.empty()) {
                std::fprintf(stderr, "foxsdr-slint: input script refused: %zu bad line(s) in %s\n", script.errors.size(), scriptPath);
                scriptRefused = true;
            } else {
                std::fprintf(stderr, "foxsdr-slint: input script: %zu steps from %s\n", script.steps.size(), scriptPath);
                driveTarget = std::make_unique<foxsdr::slintdrive::SlintDriveTarget<MainWindow>>(ui, readState, screenshotTo);
                drive = std::make_unique<foxsdr::slintdrive::Driver>(std::move(script), *driveTarget);
            }
        }
    }
    const auto driveSay = [](const std::string& line) { std::fprintf(stderr, "foxsdr-slint: %s\n", line.c_str()); };

    // THE TICK = THE FRAME: the screen's reads, then the housekeeping (the sentinel's beat, the senders, the sweep, the update check), then the
    // update card if the duties changed it, then the bounded run's count. `--frames N` is N ticks and exit 0 (BoundedRun); a real session never
    // ends itself.
    std::optional<slint::PhysicalSize> wantedSize;   // FOXSDR_SLINT_SIZE, below
    slint::Timer tick;
    tick.start(slint::TimerMode::Repeated, std::chrono::milliseconds(33), [&] {
        if (wantedSize.has_value() && bounded.ticks() < 3) {
            ui->window().set_size(*wantedSize);
            if (bounded.ticks() == 2) {
                const auto sz = ui->window().size();
                std::fprintf(stderr, "foxsdr-slint: bounded run: window size now %ux%u\n", sz.width, sz.height);
            }
        }
        applyLook();
        onTick();
        if (drive) { drive->tick(bounded.ticks() + 1, driveSay); }   // this tick's number: the steps due now, after the screen's reads
        TickInput in;
        in.deviceOpening = (state.flags & FOXAPI_RX_DEVICE_OPENING) != 0;
        duties.poll(in);
        applyUpdateCard();
        if (bounded.noteTick()) {
            if (shotAtEnd) { ui->invoke_shortcut(ss("screenshot")); }
            slint::quit_event_loop();
        }
    });

    // `--frames 0` produces no frame at all: the window is neither shown nor run. A refused script runs nothing either (exit 2 below).
    if (!bounded.skipsLoop() && !scriptRefused) {
        ui->show();
        // A bounded run's window size, FOXSDR_SLINT_SIZE=<w>x<h> in pixels (docs 6.47): a drive script whose section body is taller than the
        // default window reaches the controls below its bottom edge only when the window is tall enough - the element tree lists what is
        // laid out inside the window, nothing clipped away. The screen follows the real size at the next tick (set_win_w / set_win_h).
        if (const char* sizeEnv = std::getenv("FOXSDR_SLINT_SIZE"); sizeEnv != nullptr && bounded.bounded()) {
            unsigned w = 0, h = 0;
            if (std::sscanf(sizeEnv, "%ux%u", &w, &h) == 2 && w >= 624 && h >= 400 && w <= 8192 && h <= 8192) {
                ui->window().set_size(slint::PhysicalSize({w, h}));
                wantedSize = slint::PhysicalSize({w, h});   // asked again on the first ticks: a size set before the window is mapped was not kept
                std::fprintf(stderr, "foxsdr-slint: bounded run: window size %ux%u (FOXSDR_SLINT_SIZE)\n", w, h);
            } else {
                std::fprintf(stderr, "foxsdr-slint: FOXSDR_SLINT_SIZE=%s is not <w>x<h> within 624x400..8192x8192 - the default size stays\n", sizeEnv);
            }
        }
        duties.windowUp();
        slint::run_event_loop();
    }

    tick.stop();
    std::fprintf(stderr, "foxsdr-slint: %d frames painted, %d buffer detaches (0 = every frame reused its buffer)\n", wells.frames, wells.detaches);

    // THE END, in the order src/main.cpp and app_window.cpp hold: the window's part (the sentinel's first shutdown step, the sweep stopped and
    // its memory kept, the senders cancelled, the last half of the settings handed to the engine) while the engine is still running; then the
    // service's stop - one whole pass first, which applies what finish() handed over, the final-state save, the receiver's stop, the plugins'
    // release, the clean-exit marker; then the window's last two steps. The session is closed last: a session closed before the stop would
    // drop the half that finish() queued.
    duties.finish();
    // What the kit holds at the end of a bounded run, taken BEFORE the release lets the instances go.
    std::string kitLine;
    if (bounded.bounded()) {
        kitLine = "kit attached=" + std::to_string(kitAttached ? 1 : 0) + " plugins=" + std::to_string(kit.lists().rows.size()) + " tracks=" + std::to_string(kit.tracks().size()) +
                  " panels=" + std::to_string(kit.panels().size()) + " instruments=" + std::to_string(kit.instruments().size()) + " pictures=" + std::to_string(kit.images().size()) +
                  " lines=" + std::to_string(kit.decoderLines().size()) + " map pages=" + std::to_string(kit.mapPages().size()) + "; " + publisher.pagesLine() +
                  "; decode rows=" + std::to_string(ui->get_decode_rows() ? ui->get_decode_rows()->row_count() : std::size_t{0});
    }
    // The kit's own release of everything borrowed from the modules (the basemap and track-info tables, every instance) on this thread,
    // before the service's stop unmaps them (app_window.cpp 2339: release and disconnect precede stop()).
    kit.release();
    service.stop();
    duties.closed();
    api->close_session(session);

    int exitCode = scriptRefused ? 2 : 0;
    if (drive) {
        std::fprintf(stderr, "foxsdr-slint: %s\n", drive->summaryLine().c_str());
        if (!drive->allRun()) { exitCode = 3; }
    }
    if (bounded.bounded()) {
#ifdef FOXSDR_SLINT_PROBE
        // The clip probe (docs 6.62, probe/CMakeLists.txt): every Text of the probe build carries an accessible-description that says how much room it has (tools/slint-clip-probe.py), read here
        // through the testing API once the window has settled and printed between two marker lines, with the window's size. tools/slint_clip_sweep.py reads what lies between them.
        std::fprintf(stderr, "CLIPPROBE-LOG begin\n");
        slint::testing::ElementHandle::visit_elements(ui, [](const slint::testing::ElementHandle& e) {
            const auto d = e.accessible_description();
            if (d.has_value() && std::string_view(*d).substr(0, 9) == "CLIPPROBE") { std::fprintf(stderr, "%s\n", std::string(*d).c_str()); }
        });
        std::fprintf(stderr, "CLIPPROBE-WINDOW %d %d\n", static_cast<int>(ui->window().size().width), static_cast<int>(ui->window().size().height));
        std::fprintf(stderr, "CLIPPROBE-LOG end\n");
#endif
        // What the kit holds at the end of a bounded run: the evidence that the port answered on this thread.
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", kitLine.c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", signalBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", systemBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", toolsBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", scopeBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", extendBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", patchBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", transmitBank.summaryLine().c_str());
        if (!retuneProbe.empty()) {
            // The picture as the last tick drew it, the tune and the mode the state carries, and the Radio body's checkbox.
            const ViewWindow v = currentView();
            std::fprintf(stderr, "foxsdr-slint: bounded run: retune probe picture=[%.0f, %.0f] Hz span=%.0f Hz tuned=%.0f Hz mode=%u follow=%d\n",
                         v.centreHz - v.spanHz / 2.0, v.centreHz + v.spanHz / 2.0, v.spanHz, state.tunedHz, state.demodMode, followBandPlan ? 1 : 0);
        }
        for (const auto& r : kit.lists().rows) {
            std::fprintf(stderr, "foxsdr-slint: plugin %-40s caps=0x%04x %s%s%s file=%s\n", r.name.c_str(), r.caps, r.loaded ? "loaded" : "REFUSED",
                         r.stopped ? " stopped" : "", r.asleep ? " asleep" : "", r.file.c_str());
        }
        // What the window itself holds at the end of a bounded run: the evidence that the duties' results reached its properties.
        std::string about(ui->get_about_text());
        std::replace(about.begin(), about.end(), '\n', '|');
        std::fprintf(stderr, "foxsdr-slint: bounded run: update card shown=%d headline=\"%s\" primary=\"%s\" progress=%.2f; about: %s\n",
                     ui->get_update_shown() ? 1 : 0, std::string(ui->get_update_headline()).c_str(), std::string(ui->get_update_primary()).c_str(),
                     static_cast<double>(ui->get_update_progress()), about.c_str());
        std::fprintf(stderr, "%s\n", bounded.exitLine(exitCode).c_str());
    }
    return exitCode;
}
