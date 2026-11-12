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
#include "frame_meter.hpp"
#include "gaps_logic.hpp"
#include "gaps_win.hpp"
#include <chrono>
#include <thread>
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
#include "core/airband_data.hpp"
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
#include "radio_view.hpp"
#include "transmit_publish.hpp"
#include "scope_publish.hpp"
#include "view3d_publish.hpp"
#include "workspace_logic.hpp"
#include "workspace_publish.hpp"
#include "radio_mix_logic.hpp"
#include "radio_mix_publish.hpp"
#include "link_logic.hpp"
#include "workspace_prefs.hpp"
#include "layouts_prefs.hpp"
#include "export_logic.hpp"
#include "mark_layout.hpp"
#include "core/disk_job.hpp"
#include "lanec_publish.hpp"   // screen lane C (docs/multi-radio-records/screen-c.md)
#include "workspace_testseam.hpp"
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

constexpr double kZoomPerNotch = foxsdr::slinthost::kZoomStepPerNotch;   // the panes' wheel zooms by the same factor (workspace_logic.hpp)
// The bookmark names' stacking rule works in pixels (docs 6.52): the plot is taken at the trace's painted width, and a caption glyph (Theme.font-cap,
// 10 px) at 6 px - the window never hands its plot width back here, and the rule needs only the proportions to decide which names touch.
// (The painted width is the radio's own: RadioView::wells.size.traceW, radio_view.hpp.)
constexpr double kSpectrumCapCharPx = 6.0;

// What the host reads from the table's lists when listSeq moves.
struct ListsSnapshot {
    std::string gainStage, gainUnit; double gainMin = 0, gainMax = 1, gainStep = 1; bool haveGain = false;
    std::string antenna; int antennaCount = 0;
    bool converter = false; std::string converterName; double converterLoHz = 0;
    struct Band { std::string name; double startHz, stopHz; };
    std::vector<Band> bands;
    struct Mark { std::string name; double hz, widthHz; std::uint32_t rgba; bool span, dashed, spectrumOnly; std::string plugin; };   // plugin: the module that set it (the list item's detail)
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

enum class MenuKind { None, Counter, Trace, Marker, MarkerClear, TuneStep, App, Modes, Layouts, MarkTab };
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

    // THE FREQUENCY LOCK (slint-gaps.md item 3): every publisher is handed this copy of the table, whose submit and submit_text refuse the tuning commands of a locked radio (gaps_logic.hpp)
    const FoxEngineApi* const rawApi = engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
    const FoxEngineApi* api = foxsdr::slintlanec::deltaLockApi(foxsdr::slintgaps::lockedApi(rawApi), &foxsdr::slintlanec::deltaMaskNow, &foxsdr::slintlanec::deltaRadioOf);   // screen lane C, T8: the delta lock sits over the frequency lock
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
    if (std::getenv("FOXSDR_SLINT_DECODE_DRAWER") != nullptr && bounded.bounded()) { publisher.openPluginDrawerForCapture(); }
    // slint-gaps.md items 5 and 7: the map pages' Line / Ribbon keys save mapTrailStyle in the interface half; a bounded run's FOXSDR_FILE_PICK is the answer of the BROWSE FILES key (no dialog)
    publisher.setTrailStyleSink([&duties](int style) {
        cascade::core::AppConfig half = duties.interfaceHalf();
        if (half.mapTrailStyle != style) { half.mapTrailStyle = style; (void)duties.saveInterfaceHalf(half); }
    });
    if (const char* pick = std::getenv("FOXSDR_FILE_PICK"); pick != nullptr && bounded.bounded()) { publisher.setFilePickSeam(pick); }
    if (const char* sd = std::getenv("FOXSDR_SLINT_SAVE_DELAY_MS"); sd != nullptr && bounded.bounded()) { publisher.setSaveDelayForTest(std::atoi(sd)); }
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
    if (std::getenv("FOXSDR_SLINT_SDRPLAY_TEST") != nullptr && bounded.bounded()) {   // slint-gaps.md item 9: the key is shown as if the driver had trouble, and a stand-in takes the place of the elevated restart (nothing is ever asked of Windows)
        signalBank.forceSdrPlayKeyForTest("SDRplayAPIService");
        signalBank.setSdrPlayLauncherForTest(nullptr);
    }
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
    std::uint32_t lastListRadio = 0;         // the radio the lists were last read for (screen stage S6, B1): a new selection re-reads them
    // THE RADIOS' PICTURES (radio_view.hpp, docs/multi-radio-records/screen-s1-s2.md): everything one radio's picture needs - its state, spectrum, trace, the two
    // pixel buffers, the zoom window, the signature cache of its figures - is a RadioView; `state` below stays the SELECTED radio's state, read once a tick for the
    // strip, the panels and the banks. Slots are made on first use; with one radio only slot 0 exists and the window is today's.
    RadioSet radios;
    const auto selected = [&]() -> RadioView& { return radios.selectedView(); };
    // THE RADIO WORKSPACE (docs/multi-radio-records/screen-s3.md, stage S3): the radios the engine has, the layout and the page, the selection, the panes' rows in the model of ui/workspace.slint. With ONE radio
    // (today's engine) nothing here shows: layout One is the receiver view as it was. The engine's multi-radio stage is not in this tree: see the SEAM notes below.
    Workspace workspace(radios);
    foxsdr::slintworkspace::WorkspacePublisher workspacePublisher(*ui);
    // THE STRIP, THE AUDIO MIX, THE CARD (docs/multi-radio-records/screen-s4.md, stage S4): what the engine says about each radio (fact.radio.<n>.*, read with the radios), the host's own record of the solo
    // radio (the engine publishes no read-back of it), the engine's last refusal to an add (shown unchanged), and the question before a running radio is removed.
    foxsdr::slintworkspace::DeckPublisher deckPublisher(*ui);
    std::array<RadioFacts, kMaxRadios> radioFacts{};
    int soloRadio = 0;                       // the radio heard alone, 0 = nobody: read back from the engine's fact.radio.solo (screen stage S6, B4), so it is true after a restart and when another client changes it
    long soloAskedTick = 0;
    bool soloPending = false;                // a SOLO was asked and not yet answered: the fact is not trusted until the engine has answered
    std::string addNote;
    std::array<std::string, kMaxRadios> radioDecoder;   // what each radio's pane chip says: the first decoder FED on that radio (read_radio_list, screen stage S6)
    std::string restoreText;                 // fact.radio.restore: the saved radios whose device was missing at start, one "Radio n: ..." line each
    std::function<void(int)> addSheetCapture;   // FOXSDR_SLINT_ADD_SHEET=1 (a bounded run: the clip sweep reaches the card in any language, whose key labels are translated): opens the sheet, then asks for the sound card
    std::string lastAddAnswer;               // "<status>:<message>" of the newest RADIO_ADD answer (a bounded run reads it: workspace.lastadd)
    bool addOpen = false, addSwitchLayout = false;
    int removeConfirm = 0;
    std::uint64_t addTicket = 0, lastTicket = 0;
    std::vector<DeviceRow> deviceRows;
    std::array<FoxReceiverState, kMaxRadios> deckStates{};
    std::array<bool, kMaxRadios> deckHave{};
    std::vector<int> chipRadios;               // the radio of each status chip the deck put in front of today's (a click on the chip selects it)
    // STAGE S5 (docs/multi-radio-records/screen-s5.md): Link tuning (linked radios move together and keep their gaps), what the screen remembers of the workspace (the saved layout, in the 3D views' interface-half
    // setting), a bounded run's tuning ranges (the product cannot ask a device for its range) and the note Start together leaves for the record (the time between the radios' first frames, measured in a bounded run).
    LinkGroup linkGroup;
    WorkspaceMemory wsMemory;
    // LANE S1 (docs/multi-radio-records/screen-s1.md): the user's layouts, each radio's store of levels for the waterfall export and its export job, the persistence trace's decay, the names a "+N" tab lists
    foxsdr::slintlayouts::LayoutBook layoutBook;
    std::string layoutNote;
    bool layoutNoteBad = false;
    int layoutRenaming = -1;
    std::array<foxsdr::slintexport::SpectrumStore, kMaxRadios> spectrumStores;
    int exportLengthIdx = 1, exportFormatIdx = 0;
    std::string exportNote, exportContextSig;
    bool exportNoteBad = false;
    cascade::core::DiskJob<foxsdr::slintexport::Outcome> exportJob{"Waterfall export"};
    std::string exportTypedPath;                  // what was typed when the last export was asked (for the sentence about it)
    std::string exportLastPath;                   // the file the last export wrote (a bounded run's drive reads it back: export.fileinfo, export.peakoffset)
    int persistenceDecayIdx = 1;                  // 0 fast, 1 medium, 2 slow
    std::vector<std::pair<std::string, double>> markTabItems;   // the names the open "+N" menu lists
    int markTabRadio = 0;                         // the radio that tab belongs to (1-based; 0 = layout One's selected radio)
    int testMarks = 0;
    bool testMarksDashed = true;
    if (const char* e = std::getenv("FOXSDR_SLINT_TEST_MARKS"); e != nullptr && bounded.bounded()) { testMarks = std::clamp(std::atoi(e), 0, 512); }
    if (const char* e = std::getenv("FOXSDR_SLINT_TEST_MARKS_DASHED"); e != nullptr && bounded.bounded()) { testMarksDashed = std::atoi(e) != 0; }
    std::array<LinkRange, kMaxRadios> testRanges{};
    long wsSaveSince = -1;
    std::string noteText, dropNote, helpUrlOverride;   // the transient note under the counter; the last drop's verdict; a bounded run's help address
    int dropsTaken = 0, dropsRefused = 0, helpOpened = 0;
    std::function<void(const std::string&)> handleDrop;
    bool dropInstalled = false;
    struct DropStep { int tick = 0; std::string path; bool done = false; };
    std::vector<DropStep> dropSeam;   // FOXSDR_SLINT_DROP=<tick>:<path>[;<tick>:<path>...] (a bounded run): the drop handler is called directly, a real drag cannot be scripted
    if (const char* d = std::getenv("FOXSDR_SLINT_DROP"); d != nullptr && bounded.bounded()) {
        std::string all = d;
        std::size_t at = 0;
        while (at < all.size()) {
            std::size_t end = all.find(';', at);
            if (end == std::string::npos) { end = all.size(); }
            const std::string piece = all.substr(at, end - at);
            const std::size_t colon = piece.find(':');
            if (colon != std::string::npos && colon > 0) { dropSeam.push_back({std::atoi(piece.substr(0, colon).c_str()), piece.substr(colon + 1), false}); }
            at = end + 1;
        }
    }
    const char* const helpTest = bounded.bounded() ? std::getenv("FOXSDR_SLINT_HELP_TEST") : nullptr;   // =<tick>: the app menu's Help entry is activated at that tick (a popup's rows are not reachable by label)
    // the counter menu's Copy and Paste frequency use the OS clipboard; a bounded run keeps its own, so the desk's clipboard is neither read nor changed
    std::string fakeClip;
    const bool fakeClipOn = bounded.bounded();
    const auto clipSet = [&](const std::string& t) { if (fakeClipOn) { fakeClip = t; } else { (void)foxsdr::slintgaps::setClipboardText(t); } };
    const auto clipGet = [&]() { return fakeClipOn ? fakeClip : foxsdr::slintgaps::clipboardText(); };
    std::vector<std::pair<int, int>> menuSeam;   // FOXSDR_SLINT_MENU=<tick>:<id>[;...] (a bounded run): a menu entry is activated through the menu's own callback
    if (const char* m = bounded.bounded() ? std::getenv("FOXSDR_SLINT_MENU") : nullptr; m != nullptr) {
        std::string all = m;
        std::size_t at = 0;
        while (at < all.size()) {
            std::size_t end = all.find(';', at);
            if (end == std::string::npos) { end = all.size(); }
            const std::string piece = all.substr(at, end - at);
            const std::size_t colon = piece.find(':');
            if (colon != std::string::npos && colon > 0) { menuSeam.emplace_back(std::atoi(piece.substr(0, colon).c_str()), std::atoi(piece.substr(colon + 1).c_str())); }
            at = end + 1;
        }
    }
    const bool helpSeam = helpTest != nullptr;
    const int helpAtTick = helpTest != nullptr ? std::atoi(helpTest) : 0;   // a bounded run does not open a browser: the address is kept for the script
    long lockSeen = 0;          // slint-gaps.md item 3: the lock's refusal count last seen, and the tick of the last refusal (the note under the counter)
    int noteTick = -1000;
    std::string togetherNote = "no start together yet";
    // TEST ONLY (workspace_testseam.hpp): a BOUNDED run's FOXSDR_SLINT_TEST_RADIOS=<2..8> makes radios 2 to N from made-up answers, so the drive harness can show, drive and shoot the panes before stage 4
    // has live radios. Never in a session: the gate is the bounded run, and no control reaches it.
    std::unique_ptr<SeamRadios> seam;
    if (const int n = testRadiosFromEnv(std::getenv("FOXSDR_SLINT_TEST_RADIOS"), bounded.bounded()); n > 0) {
        seam = std::make_unique<SeamRadios>(n);
        std::fprintf(stderr, "foxsdr-slint: TEST SEAM: %d radios, radios 2 to %d are made up (FOXSDR_SLINT_TEST_RADIOS)\n", n, n);
    }
    FoxReceiverState realState{}; realState.structSize = sizeof(realState);   // the engine's own read_state, before the seam puts a made-up radio's in `state`
    std::array<int, kMaxRadios> paneTicks{};                                   // per slot: how many ticks its pictures were read and painted (the drive script's read-back)
    std::array<std::pair<float, float>, kMaxRadios> paneHover;                 // per slot: the pointer over its spectrum or waterfall, fractions; -1 when not over it
    for (auto& h : paneHover) { h = {-1.f, -1.f}; }
    int lastShownLayout = -1, lastSelectedSlot = -1;
    std::unique_ptr<PaneGestures> paneGestures;                                // the panes' gestures (workspace_logic.hpp), wired below
    std::string paneGainOfSelected;                                            // the strip's gain figure, for the selected radio's pane
    int traceMode = 1;                        // 0 normal 1 peak hold 2 average
    double averageLengthS = 1.0;
    bool rangeManual = false;
    bool bandPlanOn = true;
    double tuneStepHz = kDefaultTuneStepHz;   // Ctrl+Up/Down and the Tune step picker (10 kHz, as the key table's text says)
    bool showNudges = true;
    if (const char* e = std::getenv("FOXSDR_SLINT_TUNE_STEP"); e != nullptr && bounded.bounded()) { const double v = std::atof(e); if (v >= 1.0 && v <= 1.0e7) { tuneStepHz = v; } }   // a bounded run only: the Link scripts keep their 12.5 kHz arithmetic
    Face face = Face::Plain;
    bool followBandPlan = true;                // the engine's switch (fact.bandPlan.followMode), read when the lists move
    // (THE PICTURE FOLLOWS A RETUNE THAT LEAVES IT, core/view_follow.hpp, 2026-11-09: the radio's lastTunedHz and lastBandDragTick are its RadioView's.)
    int tickNo = 0;
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
    // THE 3D VIEWS (view3d_publish.hpp, docs 6.64): the receiver's waterfall as a surface; the views of the other pages are published here too as they land
    foxsdr::slintview3d::View3DPublisher view3dBank(*ui, duties, bounded.bounded(), kit);
    view3dBank.wire();
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
    // `radio` is the radio's number (slot + 1) a pane's gesture aims at; 0 = the selected radio, the engine's focus (FoxCommand.target, radio_view.hpp aimAt).
    const auto submit = [&](FoxCommand c, std::uint32_t radio = 0) {
        c.structSize = sizeof(c);
        // TEST SEAM: a command aimed at (or, for the strip, following the selection to) a made-up radio changes ITS state; the engine has no such radio
        if (seam) {
            const int slot = radio != 0 ? static_cast<int>(radio) - 1 : radios.selected;
            if (seam->fake(slot)) {
                if (foxsdr::slintgaps::FrequencyLock::instance().refuses(c, radio)) { lastTicket = 0; return; }
                if (seam->apply(slot, c, realState)) { return; }
            }
        }
        aimAt(c, radio);
        FoxSubmitResult r{}; r.structSize = sizeof(r);
        const int32_t rc = api->submit(session, &c, 1, &r);
        lastTicket = rc < 0 ? 0 : r.ticket;
        if (rc < 0) { std::fprintf(stderr, "foxsdr-slint: submit op 0x%04x refused by the call: %d\n", c.op, rc); }
        else if (r.status == FOXAPI_BUSY) { std::fprintf(stderr, "foxsdr-slint: submit op 0x%04x: the session's queue is full\n", c.op); }
    };
    const auto op = [&](std::uint32_t code) { FoxCommand c{}; c.op = code; return c; };
    // SCREEN LANE C (docs/multi-radio-records/screen-c.md): the scanner block, the memories, the quick memories, the delta lock, the clock and the two sheets
    const std::string laneCDir = configPath.empty() ? std::string() : std::filesystem::path(configPath).parent_path().string();
    foxsdr::slintlanec::LaneCPublisher::Env laneCEnv;
    laneCEnv.ui = &*ui;
    laneCEnv.api = api;
    laneCEnv.session = session;
    laneCEnv.storePath = laneCDir.empty() ? std::string() : (std::filesystem::path(laneCDir) / "lanec.json").string();
    laneCEnv.scanLogPath = laneCDir.empty() ? std::string() : (std::filesystem::path(laneCDir) / "scan-log.csv").string();
    laneCEnv.bounded = bounded.bounded();
    laneCEnv.submit = [&](const FoxCommand& c, std::uint32_t radio) { submit(c, radio); };
    laneCEnv.nowMs = [] { return static_cast<double>(nowMs()); };
    laneCEnv.unixNow = [] { return static_cast<std::int64_t>(std::time(nullptr)); };
    laneCEnv.openIq = [&](const std::string& path) { signalBank.openDroppedRecording(path); };
    laneCEnv.recorderDir = [&] { std::string d; foxsdr::slintkit::LiveTable t(api, session); if (!t.fact("fact.recorder.dir", d)) { d.clear(); } return d; };
    laneCEnv.selectedRadio = [&] { return workspace.selectedRadio(); };
    foxsdr::slintlanec::LaneCPublisher laneC(std::move(laneCEnv));
    const bool laneCOff = bounded.bounded() && std::getenv("FOXSDR_SLINT_LANEC_OFF") != nullptr;
    laneC.wire();
    publisher.setClipboardSink([&](const std::string& t) { clipSet(t); });   // W16c: the Decoder output page's Copy key (clipSet is the window's clipboard, or the bounded run's stand-in)
    // THE ENGINE AS THE WORKSPACE SEES IT (workspace_logic.hpp EngineReads): the facts fact.radio.count and fact.radio.<n>.state, and RADIO_FOCUS. With the test seam they are answered from the made-up radios.
    // SEAM FOR ENGINE STAGE 4: nothing else changes when stage 4 is merged: the facts name the radios it opens and RADIO_FOCUS moves the session's focus for real.
    const EngineReads engineReads{
        [&](const char* name, std::string& out) -> int {
            if (seam) {
                const std::string n = name;
                if (n == "fact.radio.count") { out = std::to_string(seam->count()); return static_cast<int>(out.size()); }
                for (int r = 1; r <= seam->count(); ++r) { if (n == "fact.radio." + std::to_string(r) + ".state") { out = "running"; return static_cast<int>(out.size()); } }
                return -1;
            }
            char buf[64] = {};
            const int32_t n = api->get_setting(session, name, buf, sizeof(buf));
            if (n < 0) { return n; }
            out.assign(buf, static_cast<std::size_t>(std::max<int32_t>(0, std::min<int32_t>(n, 63))));
            return n;
        },
        [&](std::uint32_t radio) -> int32_t {
            if (seam) { return seam->focus(radio); }
            FoxCommand c{}; c.structSize = sizeof(c); c.op = FOXAPI_OP_RADIO_FOCUS; c.ival[0] = static_cast<std::int64_t>(radio);
            FoxSubmitResult r{}; r.structSize = sizeof(r);
            const int32_t rc = api->submit(session, &c, 1, &r);
            return rc < 0 ? rc : r.status;
        }};
    // THE RADIOS THAT EXIST, from the engine's facts. A slot that has emptied loses its picture; a selected radio that has gone hands the selection to the lowest one that is left AND the session's
    // focus follows (the engine keeps a removed radio's number as the session's focus and answers NO_DEVICE until it is told another).
    const auto refreshRadios = [&] {
        const std::uint32_t before = workspace.mask();
        const int selBefore = radios.selected;
        workspace.readFacts(engineReads);
        const std::uint32_t after = workspace.mask();
        for (int s = 0; s < kMaxRadios; ++s) { radioFacts[static_cast<std::size_t>(s)] = ((after >> s) & 1u) != 0u ? readRadioFacts(engineReads, s + 1) : RadioFacts{}; }
        { std::string v; restoreText.clear(); if (engineReads.fact("fact.radio.restore", v) >= 0) { restoreText = v; } }
        for (int s = 0; s < kMaxRadios; ++s) {   // each pane's decoder chip: the first decoder fed ON THAT RADIO (the selected radio's list is not every radio's)
            radioDecoder[static_cast<std::size_t>(s)].clear();
            if (((after >> s) & 1u) == 0u || (seam && seam->fake(s))) { continue; }
            for (std::uint32_t i = 0; i < 256; ++i) {
                FoxListItem it{}; it.structSize = sizeof(it);
                const int32_t n = readListFor(api, session, static_cast<std::uint32_t>(s) + 1u, FOXAPI_LIST_DECODERS, i, &it);
                if (n <= static_cast<int32_t>(i)) { break; }
                if ((it.flags & FOXAPI_ITEM_FED) != 0u) { radioDecoder[static_cast<std::size_t>(s)] = it.name; break; }
            }
        }
        if (soloPending && tickNo > soloAskedTick + 45) { soloPending = false; }   // an answer that never came must not blind the read-back for good
        if (!soloPending) {   // THE SOLO READ-BACK (fact.radio.solo): true after a restart and when another client changes it; an engine without the fact leaves the host's own record
            std::string v;
            if (engineReads.fact("fact.radio.solo", v) >= 0) { soloRadio = soloFromFact(v, after); }
        }
        if (soloRadio != 0 && ((after >> (soloRadio - 1)) & 1u) == 0u) { soloRadio = 0; }   // the engine clears the solo of a radio that goes
        for (int s = 0; s < kMaxRadios; ++s) {
            if (((before >> s) & 1u) != 0u && ((after >> s) & 1u) == 0u) {
                radios.reset(s);
                spectrumStores[static_cast<std::size_t>(s)].clear();
                paneTicks[static_cast<std::size_t>(s)] = 0;
                paneHover[static_cast<std::size_t>(s)] = {-1.f, -1.f};
            }
        }
        if (radios.selected != selBefore) { (void)engineReads.focus(static_cast<std::uint32_t>(radios.selected) + 1u); }
    };
    // The engine's answer to the card's RADIO_ADD: a refusal is kept and shown as the engine worded it (it names every holder; nothing here rewords it); an add that was taken closes the sheet,
    // refreshes the radios at once and, for the first radio beside the first, turns the layout from One to Side by side. Any answer to a removal refreshes the radios too.
    const auto deckOnAnswer = [&](const FoxCommandResult& r) {
        if (r.op == FOXAPI_OP_RADIO_ADD) { lastAddAnswer = std::to_string(r.status) + ":" + r.message; }   // every add's answer, for `state workspace.lastadd`
        if (r.op == FOXAPI_OP_RADIO_ADD && addTicket != 0 && r.ticket == addTicket) {
            addTicket = 0;
            const AddOutcome outcome = addOutcome(r.status, r.message);
            if (outcome.accepted) {
                addNote.clear();
                addOpen = false;
                refreshRadios();
                if (addSwitchLayout && workspace.radioCount() > 1) { workspace.setLayout(kLayoutTwo); }
                addSwitchLayout = false;
            } else {
                addNote = outcome.note;   // the engine's words, unchanged
                addSwitchLayout = false;
            }
        } else if (r.op == FOXAPI_OP_RADIO_REMOVE || r.op == FOXAPI_OP_RADIO_ADD || r.op == FOXAPI_OP_RADIO_AUDIO || r.op == FOXAPI_OP_RADIO_SOLO) {
            if (r.op == FOXAPI_OP_RADIO_SOLO) { soloPending = false; }
            refreshRadios();
            if (r.op == FOXAPI_OP_RADIO_REMOVE && r.status == FOXAPI_OK && wsMemory.userSelected(workspace.selectedRadio(), workspace.page())) { wsSaveSince = static_cast<long>(tickNo); }
        }
    };
    // Selecting a radio is the session's focus (design 3.2): the engine is asked, and the window's selection follows when it says yes.
    // (a selection the USER made is part of the saved layout; one the engine forced - a radio went away - is not: workspace_prefs.hpp)
    const auto selectRadio = [&](int radio) {
        if (workspace.select(radio, engineReads)) { if (wsMemory.userSelected(workspace.selectedRadio(), workspace.page())) { wsSaveSince = static_cast<long>(tickNo); } }
    };
    const auto setFrequency = [&](double hz) { FoxCommand c = op(FOXAPI_OP_SET_FREQUENCY); c.num[0] = hz; submit(c); };
    const auto stepTune = [&](int dir, double step) { FoxCommand c = op(FOXAPI_OP_STEP_TUNE); c.ival[0] = dir; c.num[0] = step; submit(c); };
    const auto setMode = [&](std::uint32_t demod) { FoxCommand c = op(FOXAPI_OP_SET_MODE); c.ival[0] = static_cast<int64_t>(demod); submit(c); };
    const auto setVolume = [&](double v) { FoxCommand c = op(FOXAPI_OP_SET_VOLUME); c.num[0] = std::clamp(v, 0.0, 1.0); submit(c); };
    const auto setSquelch = [&](double db) { FoxCommand c = op(FOXAPI_OP_SET_SQUELCH); c.num[0] = db; submit(c); };
    const auto setDisplayRange = [&](double lo, double hi) { FoxCommand c = op(FOXAPI_OP_SET_DISPLAY_RANGE); c.num[0] = lo; c.num[1] = hi; submit(c); };

    // ---- the view window and its fractions ----
    // (the selected radio's picture; a pane's gestures call the same methods on the pane's own RadioView)
    const auto currentView = [&]() -> ViewWindow { return selected().currentView(); };
    const auto frac = [&](double hz) { return selected().frac(hz); };
    const auto hzAtFrac = [&](double f) { return selected().hzAtFrac(f); };
    const auto dbAtFrac = [&](double f) -> double { return selected().dbAtFrac(f); };
    const auto zoomView = [&](double factor, double aboutFrac) { selected().zoom(factor, aboutFrac); };

    // ---- the lists, re-read when listSeq moves ----
    // The radio the four per-radio lists (gains, antennas, sample rates, decoders) are read for: the SELECTED one - asked for by number, not by where the session's focus is. A made-up radio of the test
    // seam has no lists of its own in the engine, so it reads the focus (as before).
    const auto listRadioNow = [&]() -> std::uint32_t { return (seam && seam->fake(radios.selected)) ? 0u : static_cast<std::uint32_t>(radios.selected) + 1u; };
    const auto readList = [&](std::uint32_t list, const std::function<void(const FoxListItem&)>& each) {
        for (std::uint32_t i = 0; i < 4096; ++i) {
            FoxListItem it{}; it.structSize = sizeof(it);
            const int32_t n = readListFor(api, session, listRadioNow(), list, i, &it);   // the SELECTED radio's gains, antennas, rates and decoders (screen stage S6, B1)
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
        readList(FOXAPI_LIST_MARKERS, [&](const FoxListItem& it) { l.marks.push_back({it.name, it.value[0], it.value[1], static_cast<std::uint32_t>(it.value[2]), (it.flags & FOXAPI_ITEM_SPAN) != 0, (it.flags & FOXAPI_ITEM_DASHED) != 0, (it.flags & FOXAPI_ITEM_SPECTRUM_ONLY) != 0, std::string(it.detail)}); });
        // TEST ONLY (a bounded run's FOXSDR_SLINT_TEST_MARKS=<n>): n made-up marks fed BELOW the plugin ABI (which caps a plugin at 64), spread over the selected radio's band with crowds, so the names' declutter
        // and its cost can be shown at 64 and 256 marks without a plugin that sets them
        if (testMarks > 0) {
            std::uint32_t seed = 20261111u;
            const auto next = [&] { seed = seed * 1664525u + 1013904223u; return static_cast<double>(seed >> 8) / 16777216.0; };
            const double span = state.sampleRateHz > 0 ? state.sampleRateHz : 2.0e6, lo = state.centreHz - span / 2.0;
            for (int i = 0; i < testMarks; ++i) {
                const double crowd = (i % 3 == 0) ? 0.25 + 0.02 * next() : next();   // a third of them crowd one place, so the stacking and the "+N" tab have something to do
                char name[24];
                std::snprintf(name, sizeof(name), "STN%03d %s", i, (i % 4 == 0) ? "Eng" : "Fr");
                l.marks.push_back({name, lo + crowd * span, 0.0, 0xffc864ffu, false, testMarksDashed && (i % 5) == 0, false, "made up"});   // (one in five dashed, as a plugin may: FOXSDR_SLINT_TEST_MARKS_DASHED=0 draws none dashed)
            }
        }
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
                add(5, cascade::i18n::tr("Export"), "--", ChipTone::Neutral);   // lane S1: the waterfall as a file
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
                e.push_back(entry(MenuItemKind::Checked, 101, "Enlarge figures", "", duties.interfaceHalf().counterScale >= 2));
                e.push_back(entry(MenuItemKind::Checked, 102, "Normal size", "", duties.interfaceHalf().counterScale < 2));
                e.push_back(entry(MenuItemKind::Checked, 103, "Show tuner switches", "", showNudges));
                e.push_back(entry(MenuItemKind::Heading, 0, "Counter face"));
                e.push_back(entry(MenuItemKind::Radio, 111, "Nixie tubes", "", face == Face::Nixie));
                e.push_back(entry(MenuItemKind::Radio, 112, "Neon", "", face == Face::Drum));
                e.push_back(entry(MenuItemKind::Radio, 113, "Plain", "", face == Face::Plain));
                e.push_back(entry(MenuItemKind::Separator, 0, ""));
                e.push_back(entry(MenuItemKind::Checked, 104, "Enlarge every reading", "", duties.interfaceHalf().readingsScale > 1.0f));
                e.push_back(entry(MenuItemKind::Separator, 0, ""));
                e.push_back(entry(MenuItemKind::Item, 105, "Copy frequency", fmt("%.6f MHz", state.tunedHz / 1e6).c_str()));
                e.push_back(entry(MenuItemKind::Item, 106, "Paste frequency", "", false, !!clipGet().empty()));
                e.push_back(entry(MenuItemKind::Separator, 0, ""));
                e.push_back(entry(MenuItemKind::Checked, 107, "Lock the tuning step", "", laneC.stepLocked()));
                e.push_back(entry(MenuItemKind::Checked, 108, "Delta lock", "", laneC.deltaOn(workspace.selectedRadio())));
                e.push_back(entry(MenuItemKind::Checked, 109, "Remember the width of each mode", "", laneC.bwMemoryOn()));
                break;
            case MenuKind::Trace:
                e.push_back(entry(MenuItemKind::Checked, 201, "Normal", "", traceMode == 0));
                e.push_back(entry(MenuItemKind::Checked, 202, "Peak hold", "", traceMode == 1));
                e.push_back(entry(MenuItemKind::Checked, 203, "Average", "", traceMode == 2));
                e.push_back(entry(MenuItemKind::Checked, 206, std::string(cascade::i18n::tr("Persistence")).c_str(), "", traceMode == 3));
                e.push_back(entry(MenuItemKind::Separator, 0, ""));
                e.push_back(entry(MenuItemKind::Item, 204, "Reset trace", "", false, traceMode != 0));
                e.push_back(entry(MenuItemKind::Heading, 0, "Average length"));
                {
                    const std::array<std::pair<const char*, double>, 7> presets{{{"100 ms", 0.1}, {"200 ms", 0.2}, {"500 ms", 0.5}, {"1 s", 1.0}, {"2 s", 2.0}, {"5 s", 5.0}, {"10 s", 10.0}}};
                    for (std::size_t i = 0; i < presets.size(); ++i) { e.push_back(entry(MenuItemKind::Radio, 211 + static_cast<int>(i), presets[i].first, "", std::fabs(averageLengthS - presets[i].second) < 1e-9)); }
                    e.push_back(entry(MenuItemKind::Slider, 218, "Average length", "", false, true, static_cast<float>(std::clamp(std::log(averageLengthS / 0.05) / std::log(200.0), 0.0, 1.0))));
                }
                e.push_back(entry(MenuItemKind::Heading, 0, std::string(cascade::i18n::tr("Persistence decay")).c_str()));
                e.push_back(entry(MenuItemKind::Radio, 221, std::string(cascade::i18n::tr("Fast")).c_str(), "", persistenceDecayIdx == 0));
                e.push_back(entry(MenuItemKind::Radio, 222, std::string(cascade::i18n::tr("Medium")).c_str(), "", persistenceDecayIdx == 1));
                e.push_back(entry(MenuItemKind::Radio, 223, std::string(cascade::i18n::tr("Slow")).c_str(), "", persistenceDecayIdx == 2));
                displayBlock(e);
                break;
            case MenuKind::Layouts: {   // lane S1: every layout is one item (the stock ones first), then the way to the sheet
                const std::vector<foxsdr::slintlayouts::Layout> all = layoutBook.all();
                e.push_back(entry(MenuItemKind::Heading, 0, std::string(cascade::i18n::tr("Layouts")).c_str()));
                for (std::size_t i = 0; i < all.size(); ++i) { e.push_back(entry(MenuItemKind::Item, 1000 + static_cast<int>(i), all[i].name.c_str())); }
                e.push_back(entry(MenuItemKind::Separator, 0, ""));
                e.push_back(entry(MenuItemKind::Item, 1990, std::string(cascade::i18n::tr("Save the current layout...")).c_str()));
                e.push_back(entry(MenuItemKind::Item, 1991, std::string(cascade::i18n::tr("Manage layouts...")).c_str()));
                break;
            }
            case MenuKind::MarkTab: {   // lane S1: the names a "+N" tab stands for; a pick tunes to that mark
                e.push_back(entry(MenuItemKind::Heading, 0, std::string(cascade::i18n::tr("Names that did not fit")).c_str()));
                for (std::size_t i = 0; i < markTabItems.size() && i < 60; ++i) {
                    e.push_back(entry(MenuItemKind::Item, 9000 + static_cast<int>(i), markTabItems[i].first.c_str(), fmt("%.4f MHz", markTabItems[i].second / 1e6).c_str()));
                }
                break;
            }
            case MenuKind::Marker: {
                menuMarkerNear = -1;
                for (std::size_t i = 0; i < lists.markers.size(); ++i) {
                    const ViewWindow v = currentView();
                    if (std::fabs(fracOf(lists.markers[i].hz, v.centreHz, v.spanHz) - fracOf(menuHz, v.centreHz, v.spanHz)) * selected().plot.w <= 6.0) { menuMarkerNear = static_cast<int>(i); break; }
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
                e.push_back(entry(MenuItemKind::Item, 605, "Help: the manual"));
                e.push_back(entry(MenuItemKind::Item, 606, "Keyboard shortcuts", "Shift F1"));
                e.push_back(entry(MenuItemKind::Item, 607, "Recordings..."));
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
    bool lookHc = false, winHc = false;   // slint-gaps.md item 11: the high-contrast skin in force, and Windows' own setting (polled once a second; FOXSDR_SLINT_WINDOWS_HC=1 stands in for it in a bounded run)
    std::string skinNow = "workbench";
    bool lookStarted = false;
    const auto applyLook = [&] {
        const cascade::core::AppConfig& half = duties.interfaceHalf();
        const bool hcNow = wsMemory.saved().highContrast || winHc;
        if (!lookStarted || half.uiTheme != lookTheme || half.tunerDisplayStyle != lookStyle || hcNow != lookHc) {
            const bool themeMoved = !lookStarted || half.uiTheme != lookTheme || hcNow != lookHc;
            lookHc = hcNow;
            lookStarted = true;
            lookTheme = half.uiTheme;
            lookStyle = half.tunerDisplayStyle;
            if (themeMoved) {
                const foxsdr::slintlook::SkinId id = hcNow ? foxsdr::slintlook::SkinId::HighContrast : foxsdr::slintlook::skinOfTheme(half.uiTheme);
                ui->global<Theme>().set_skin(id == foxsdr::slintlook::SkinId::HighContrast ? ui->global<HighContrast>().get_value() : id == foxsdr::slintlook::SkinId::NightWatch ? ui->global<NightWatch>().get_value() : ui->global<Workbench>().get_value());
                skinNow = foxsdr::slintlook::skinName(id);
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
    // ======================================================== LANE S1: one-click layouts (layouts_prefs.hpp) and the waterfall's export ========================================================
    layoutBook.load(duties.interfaceHalf().view3d);
    const auto layoutWindow = [&]() {
        foxsdr::slintlayouts::Window w;
        w.bank = bankIndex;
        for (const int row : openSections[bankIndex]) { if (row >= 0 && row < 10) { w.sections |= 1u << row; } }
        w.railFolded = ui->get_rail_folded();
        w.view = ui->get_view_index();
        w.splitPct = static_cast<int>(std::lround(ui->get_split_ratio() * 100.0f));
        w.workspace = workspace.layout();
        w.link = linkGroup.on();
        w.wf3d = ui->global<View3D>().get_wf_on();
        w.selected = workspace.selectedRadio();
        w.openRadios = workspace.mask();
        w.tunedHz = static_cast<std::int64_t>(std::llround(state.tunedHz));
        w.mode = std::max(0, modeIndex(state.demodMode));
        w.patchRunning = (state.flags & FOXAPI_RX_PATCH_RUNNING) != 0;
        return w;
    };
    // The ONLY things a layout can do to the window (layouts_prefs.hpp Actions): there is no member that starts or stops anything.
    const auto layoutActions = [&]() {
        foxsdr::slintlayouts::Actions a;
        a.setBank = [&](int b) { bankIndex = std::clamp(b, 0, 4); ui->set_bank_index(bankIndex); };
        a.setSections = [&](int b, std::uint32_t mask) {
            std::set<int> rows;
            for (int i = 0; i < 10; ++i) { if (((mask >> i) & 1u) != 0u) { rows.insert(i); } }
            const std::set<int> before = openSections[b];
            openSections[b] = rows;
            if (b == 4) { for (const int r : rows) { if (before.count(r) == 0) { systemBank.sectionOpened(r); } } }   // a section that opens asks for what it shows
            if (b == 2 && rows.count(1) > 0 && before.count(1) == 0) { systemBank.sectionOpened(4); }
        };
        a.setRail = [&](bool folded) { ui->set_rail_folded(folded); };
        a.setView = [&](int v) { ui->set_view_index(v); patchBank.setViewShown(v == 1); };
        a.setSplitPct = [&](int p) { ui->set_split_ratio(static_cast<float>(p) / 100.0f); };
        a.setWorkspace = [&](int l) { workspace.setLayout(l); };
        a.setLink = [&](bool on) { linkGroup.setOn(on, workspace.mask()); };
        a.selectRadio = [&](int r) { selectRadio(r); };
        a.setWf3d = [&](bool on) { ui->global<View3D>().invoke_wf_view_selected(on); };
        a.tune = [&](std::int64_t hz) { setFrequency(static_cast<double>(hz)); };
        a.setMode = [&](int m) { setMode(demodOfIndex(m)); };
        return a;
    };
    const auto publishLayouts = [&] {
        auto& lm = ui->global<LayoutsModel>();
        std::vector<LayoutRow> rows;
        const std::vector<foxsdr::slintlayouts::Layout> all = layoutBook.all();
        for (std::size_t i = 0; i < all.size(); ++i) { rows.push_back(LayoutRow{ss(all[i].name), all[i].builtin, !all[i].builtin && layoutRenaming == static_cast<int>(i)}); }
        lm.set_rows(std::make_shared<slint::VectorModel<LayoutRow>>(std::move(rows)));
        lm.set_note(ss(layoutNote));
        lm.set_note_bad(layoutNoteBad);
    };
    const auto layoutsChanged = [&] {   // saved at once (a rare, deliberate act), then shown
        if (layoutBook.dirty()) {
            cascade::core::AppConfig half = duties.interfaceHalf();
            const std::string text = layoutBook.text(half.view3d);
            if (text != half.view3d) { half.view3d = text; (void)duties.saveInterfaceHalf(half); }
            layoutBook.markWritten();
        }
        publishLayouts();
    };
    const auto applyLayoutNamed = [&](const std::string& name) {
        const foxsdr::slintlayouts::Layout* l = layoutBook.find(name);
        if (l == nullptr) { return; }
        const int held = foxsdr::slintlayouts::apply(*l, layoutWindow(), layoutActions());
        layoutNote = cascade::core::formatText(cascade::i18n::tr("Applied the layout %s"), l->name.c_str());
        layoutNoteBad = false;
        if ((held & foxsdr::slintlayouts::kHeldView) != 0) { layoutNote += ". " + std::string(cascade::i18n::tr("The patch is running, so the window stays on its tab")); layoutNoteBad = true; }
        if ((held & foxsdr::slintlayouts::kHeldSelection) != 0) { layoutNote += ". " + std::string(cascade::i18n::tr("The radio that layout names is not open, so the selection stays")); layoutNoteBad = true; }
        publishLayouts();
    };
    {
        using SR = foxsdr::slintlayouts::SaveResult;
        auto& lm = ui->global<LayoutsModel>();
        lm.on_menu_requested([&](float x, float y) { openMenu(MenuKind::Layouts, x, y); });
        lm.on_applied([&](int i) {
            const std::vector<foxsdr::slintlayouts::Layout> all = layoutBook.all();
            if (i >= 0 && static_cast<std::size_t>(i) < all.size()) { applyLayoutNamed(all[static_cast<std::size_t>(i)].name); }
        });
        lm.on_saved([&](slint::SharedString nameS, bool tuning) {
            const std::string name = foxsdr::slintlayouts::cleanName(std::string(nameS));
            const SR r = layoutBook.save(name, foxsdr::slintlayouts::capture(layoutWindow(), tuning));
            layoutNoteBad = r != SR::Saved && r != SR::Replaced;
            switch (r) {
                case SR::Saved: layoutNote = cascade::core::formatText(cascade::i18n::tr("Saved the layout %s"), name.c_str()); break;
                case SR::Replaced: layoutNote = cascade::core::formatText(cascade::i18n::tr("Replaced the layout %s"), name.c_str()); break;
                case SR::BadName: layoutNote = cascade::i18n::tr("Type a name for the layout first"); break;
                case SR::ReservedName: layoutNote = cascade::i18n::tr("That is the name of a built-in layout"); break;
                case SR::Full: layoutNote = cascade::core::formatText(cascade::i18n::tr("You can keep %d layouts of your own: delete one first"), static_cast<int>(foxsdr::slintlayouts::kMaxUserLayouts)); break;
            }
            layoutsChanged();
        });
        lm.on_rename_begun([&](int i) {
            const std::vector<foxsdr::slintlayouts::Layout> all = layoutBook.all();
            if (i < 0 || static_cast<std::size_t>(i) >= all.size() || all[static_cast<std::size_t>(i)].builtin) { return; }
            layoutRenaming = i;
            ui->global<LayoutsModel>().set_rename_text(ss(all[static_cast<std::size_t>(i)].name));
            publishLayouts();
        });
        lm.on_rename_committed([&](int i, slint::SharedString textS) {
            const std::vector<foxsdr::slintlayouts::Layout> all = layoutBook.all();
            if (i < 0 || static_cast<std::size_t>(i) >= all.size() || all[static_cast<std::size_t>(i)].builtin) { return; }
            const std::string to = foxsdr::slintlayouts::cleanName(std::string(textS));
            const SR r = layoutBook.rename(all[static_cast<std::size_t>(i)].name, to);
            layoutNoteBad = r != SR::Saved;
            if (r == SR::Saved) { layoutNote = cascade::core::formatText(cascade::i18n::tr("Renamed to %s"), to.c_str()); layoutRenaming = -1; }
            else if (r == SR::BadName) { layoutNote = cascade::i18n::tr("Type a name for the layout first"); }
            else {
                const foxsdr::slintlayouts::Layout* other = layoutBook.find(to);
                layoutNote = (other != nullptr && other->builtin) ? cascade::i18n::tr("That is the name of a built-in layout") : cascade::i18n::tr("Another layout has that name");
            }
            layoutsChanged();
        });
        lm.on_removed([&](int i) {
            const std::vector<foxsdr::slintlayouts::Layout> all = layoutBook.all();
            if (i < 0 || static_cast<std::size_t>(i) >= all.size() || all[static_cast<std::size_t>(i)].builtin) { return; }
            const std::string name = all[static_cast<std::size_t>(i)].name;
            (void)layoutBook.remove(name);
            layoutRenaming = -1;
            layoutNote = cascade::core::formatText(cascade::i18n::tr("Deleted the layout %s"), name.c_str());
            layoutNoteBad = false;
            layoutsChanged();
        });
    }
    publishLayouts();
    // TEST ONLY (a bounded run's FOXSDR_SLINT_LAYOUTS_SHEET=1): the clip sweep reaches the sheet in any language, whose key labels are translated, with the longest note the sheet can show
    if (std::getenv("FOXSDR_SLINT_LAYOUTS_SHEET") != nullptr && bounded.bounded()) {
        layoutNote = cascade::core::formatText(cascade::i18n::tr("Applied the layout %s"), "Quiet listening") + ". " + std::string(cascade::i18n::tr("The patch is running, so the window stays on its tab")) + ". " +
                     std::string(cascade::i18n::tr("The radio that layout names is not open, so the selection stays"));
        layoutNoteBad = true;
        ui->global<LayoutsModel>().set_open(true);
        publishLayouts();
    }

    // THE WATERFALL AS A FILE (export_logic.hpp): the store of levels is filled where the picture is painted (paneTick); a click copies what the file needs and a core::DiskJob writes it, off the frame
    const auto exportBase = [&] { return (std::filesystem::path(configPath).parent_path() / "exports").string(); };
    const auto utcNowMs = [] { return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); };
    {
        auto& em = ui->global<ExportModel>();
        em.set_path(ss((std::filesystem::path(exportBase()) / "foxsdr-waterfall").string()));
        em.on_length_selected([&](int i) { exportLengthIdx = std::clamp(i, 0, 3); });
        em.on_format_selected([&](int i) { exportFormatIdx = std::clamp(i, 0, 1); });
        const auto precheck = [&](const std::string& path) -> bool {
            exportNoteBad = true;
            if (exportJob.pending()) { return false; }   // one file at a time: the key is dimmed while one is written
            const RadioView& rv = selected();
            if (rv.frame.empty() || !(rv.lastInfo.spanHz > 0)) { exportNote = cascade::i18n::tr("This radio has shown no spectrum yet, so there is nothing to save"); return false; }
            if (path.find_first_not_of(" \t") == std::string::npos) { exportNote = cascade::i18n::tr("Type where to save first"); return false; }
            return true;
        };
        em.on_picture_clicked([&](slint::SharedString typed) {
            const std::string path(typed);
            if (!precheck(path)) { return; }
            RadioView& rv = selected();
            foxsdr::slintexport::PictureSpec spec;
            spec.w = rv.wells.size.wfW;
            spec.h = rv.wells.size.wfH;
            const ViewWindow cur = rv.currentView();
            spec.centreHz = cur.centreHz;
            spec.spanHz = cur.spanHz;
            spec.newestUtcMs = utcNowMs();
            spec.radio = rv.slot + 1;
            spec.dbMin = rv.state.dbMin;
            spec.dbMax = rv.state.dbMax;
            if (rv.frameTimes.size() >= 2 && rv.frameTimes.back() > rv.frameTimes.front()) {
                spec.secondsPerRow = static_cast<double>(rv.frameTimes.back() - rv.frameTimes.front()) / 1000.0 / static_cast<double>(rv.frameTimes.size() - 1);
            }
            std::vector<foxsdr::slintexport::Pixel> pic(rv.wells.picture.size());
            static_assert(sizeof(foxsdr::slintexport::Pixel) == sizeof(Rgba));
            std::memcpy(pic.data(), rv.wells.picture.data(), sizeof(Rgba) * rv.wells.picture.size());
            exportTypedPath = path;
            exportNote = cascade::i18n::tr("Saving...");
            exportNoteBad = false;
            const std::string base = exportBase();
            (void)exportJob.request([path, base, pic = std::move(pic), spec]() mutable { return foxsdr::slintexport::savePicture(path, base, std::move(pic), spec); });
        });
        em.on_data_clicked([&](slint::SharedString typed) {
            const std::string path(typed);
            if (!precheck(path)) { return; }
            const RadioView& rv = selected();
            static const int kMinutes[4] = {1, 5, 15, 60};
            foxsdr::slintexport::Snapshot snap = spectrumStores[static_cast<std::size_t>(rv.slot)].snapshot(kMinutes[exportLengthIdx], utcNowMs());
            if (snap.empty()) { exportNote = cascade::i18n::tr("This radio has shown no spectrum yet, so there is nothing to save"); return; }
            foxsdr::slintexport::FileFacts facts;
            facts.radio = rv.slot + 1;
            facts.note = std::string("FoxSDR ") + cascade::versionString();
            exportTypedPath = path;
            exportNote = cascade::i18n::tr("Saving...");
            exportNoteBad = false;
            const std::string base = exportBase();
            const int format = exportFormatIdx;
            (void)exportJob.request([path, base, snap = std::move(snap), format, facts]() mutable { return foxsdr::slintexport::saveData(path, base, std::move(snap), format, facts); });
        });
    }
    // once a tick: collect a finished file, and say what the selected radio's store holds
    const auto publishExport = [&] {
        auto& em = ui->global<ExportModel>();
        foxsdr::slintexport::Outcome r;
        if (exportJob.poll(r)) {
            exportNoteBad = !r.ok;
            if (r.ok) { exportNote = cascade::core::formatText(cascade::i18n::tr("Saved %s"), r.path.c_str()); exportLastPath = r.path; }
            else if (r.why == 1) { exportNote = cascade::i18n::tr("Type where to save first"); }
            else if (r.why == 2) { exportNote = cascade::i18n::tr("This radio has shown no spectrum yet, so there is nothing to save"); }
            else { exportNote = cascade::core::formatText(cascade::i18n::tr("The file could not be written to %s"), exportTypedPath.c_str()); }
        }
        const RadioView& rv = selected();
        const foxsdr::slintexport::SpectrumStore& store = spectrumStores[static_cast<std::size_t>(rv.slot)];
        const ViewWindow cur = rv.currentView();
        const std::string context = cascade::core::formatText(cascade::i18n::tr("Radio %d at %s, span %s: %d min stored, %.1f MB"), rv.slot + 1, fmtMHz(cur.centreHz).c_str(), spanText(cur.spanHz).c_str(),
                                                              static_cast<int>(store.rows() * foxsdr::slintexport::kRowGapMs / 60000), static_cast<double>(store.bytes()) / 1e6);
        const std::string sig = context + "|" + exportNote + (exportNoteBad ? "1" : "0") + (exportJob.pending() ? "w" : "i") + std::to_string(exportLengthIdx) + std::to_string(exportFormatIdx);
        if (sig == exportContextSig) { return; }
        exportContextSig = sig;
        em.set_context(ss(context));
        em.set_note(ss(exportNote));
        em.set_note_bad(exportNoteBad);
        em.set_working(exportJob.pending());
        em.set_length_index(exportLengthIdx);
        em.set_format_index(exportFormatIdx);
    };

    ui->on_menu_activated([&](int id) {
        switch (id) {
            case 101: case 102: case 104: {   // the same three settings the Display body saves (counterScale, readingsScale); this window does not draw the figures larger yet (the record, item 6)
                cascade::core::AppConfig half = duties.interfaceHalf();
                if (id == 101) { half.counterScale = 2; } else if (id == 102) { half.counterScale = 1; } else { half.readingsScale = half.readingsScale > 1.0f ? 1.0f : foxsdr::slintsystem::kEnlargedReadings; }
                (void)duties.saveInterfaceHalf(half);
                break;
            }
            case 105: clipSet(foxsdr::slintgaps::dottedFrequencyText(state.tunedHz, wsMemory.saved().dotStyle)); break;
            case 106: {   // a frequency from the clipboard, either style; a locked radio refuses it like any tuning
                double hz = 0;
                if (foxsdr::slintgaps::parseTypedFrequencyHz(clipGet(), hz)) { setFrequency(hz); }
                break;
            }
            case 103: showNudges = !showNudges; ui->set_show_nudges(showNudges); break;
            case 107: laneC.toggleStepLock(); break;                                   // T3
            case 108: laneC.toggleDelta(workspace.selectedRadio()); break;             // T8
            case 109: laneC.toggleBwMemory(); break;                                   // F7
            case 606: laneC.openShortcuts(foxsdr::slintsystem::keyTableFromConfig(duties.interfaceHalf().keyBindings)); break;   // U10
            case 607: laneC.openRecordings(); break;                                   // P11
            case 111: face = Face::Nixie; applyFace(); break;
            case 112: face = Face::Drum; applyFace(); break;
            case 113: face = Face::Plain; applyFace(); break;
            case 201: case 202: case 203: traceMode = id - 201; ui->set_trace_mode(traceMode); resetTrace(selected().traceState); break;
            case 204: resetTrace(selected().traceState); break;
            case 206: traceMode = 3; ui->set_trace_mode(3); resetTrace(selected().traceState); break;   // persistence (lane S1)
            case 221: case 222: case 223: persistenceDecayIdx = id - 221; break;
            case 1990: case 1991: layoutNote.clear(); layoutNoteBad = false; layoutRenaming = -1; ui->global<LayoutsModel>().set_open(true); publishLayouts(); break;
            case 211: case 212: case 213: case 214: case 215: case 216: case 217: {
                const double presets[7] = {0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0};
                averageLengthS = presets[id - 211];
                break;
            }
            case 301: rangeManual = false; ui->set_range_manual(false); setDisplayRange(-110, 0); break;
            case 302: rangeManual = true; ui->set_range_manual(true); break;
            case 305: rangeManual = false; ui->set_range_manual(false); setDisplayRange(-110, 0); traceMode = 0; ui->set_trace_mode(0); resetTrace(selected().traceState); break;
            case 401: { FoxCommand c = op(FOXAPP_OP_MARKER_DROP); c.num[0] = menuHz; c.num[1] = 2.0 * currentView().spanHz / selected().plot.w; submit(c); break; }
            case 402: if (menuMarkerNear >= 0) { FoxCommand c = op(FOXAPP_OP_MARKER_REMOVE); c.ival[0] = static_cast<int64_t>(lists.markers[static_cast<std::size_t>(menuMarkerNear)].id); submit(c); } break;
            case 403: toolsBank.openMarkers(); break;
            case 404: ui->global<ToolsEvents>().invoke_markers_copy(); break;
            case 406: submit(op(FOXAPP_OP_MARKER_CLEAR)); break;
            case 601: ui->set_about_open(true); break;
            case 602: bankIndex = 4; ui->set_bank_index(4); openSections[4].insert(5); break;
            case 603: ui->invoke_shortcut(ss("screenshot")); break;
            case 604: slint::quit_event_loop(); break;
            case 605: if (helpSeam) { ++helpOpened; helpUrlOverride = foxsdr::slintgaps::manualUrl(); } else { helpOpened += foxsdr::slintgaps::openUrl(foxsdr::slintgaps::manualUrl()) ? 1 : 0; } break;   // item 10: the manual, in the default browser
            case 701: case 702: case 703: case 704: case 705: case 706: case 707: case 708: setMode(demodOfIndex(id - 701)); break;
            default:
                if (id >= 1000 && id < 1000 + 64) {   // a layout of the Layouts menu
                    const std::vector<foxsdr::slintlayouts::Layout> all = layoutBook.all();
                    if (static_cast<std::size_t>(id - 1000) < all.size()) { applyLayoutNamed(all[static_cast<std::size_t>(id - 1000)].name); }
                    break;
                }
                if (id >= 9000 && id < 9000 + 64) {   // a name of a "+N" tab: tune to that mark (the radio the tab is on is selected first)
                    const std::size_t k = static_cast<std::size_t>(id - 9000);
                    if (k < markTabItems.size()) {
                        if (markTabRadio > 0) { selectRadio(markTabRadio); }
                        setFrequency(markTabItems[k].second);
                    }
                    break;
                }
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
    ui->on_nudge([&](int digit, int dir) { stepTune(dir, laneC.stepLocked() ? tuneStepHz : stepOfDigit(digit)); });   // T3: with the step locked every digit steps by the Tune step
    ui->on_edit_requested([&] { ui->set_editor_text(ss(wsMemory.saved().dotStyle == 1 ? foxsdr::slintgaps::dottedFrequencyText(state.tunedHz, 1) : fmt("%.6f", state.tunedHz / 1e6))); ui->set_editing(true); });
    ui->on_typed([&](slint::SharedString text) {
        double hz = 0;
        if (foxsdr::slintgaps::parseTypedFrequencyHz(std::string(text), hz)) { setFrequency(hz); }
        else { std::fprintf(stderr, "foxsdr-slint: could not read frequency \"%s\"\n", text.data()); }
        ui->set_editing(false);
    });
    ui->on_edit_cancelled([&] { ui->set_editing(false); });
    // THE PADLOCK BY THE COUNTER (slint-gaps.md item 3): the selected radio's lock, saved with the workspace; the table every publisher holds refuses the radio's tuning while it is on
    ui->on_lock_toggled([&] {
        auto& lk = foxsdr::slintgaps::FrequencyLock::instance();
        const int r = std::clamp(workspace.selectedRadio(), 1, 8);
        const std::uint32_t m = lk.mask() ^ (1u << (r - 1));
        lk.setMask(m);
        if (wsMemory.setLocked(m)) { wsSaveSince = static_cast<long>(tickNo); }
    });
    // THE COUNTER'S DIGIT GROUPING (item 2): the Display body's pick, saved with the workspace (`dg=`)
    ui->global<SystemEvents>().on_display_digit_grouping([&](int i) { if (wsMemory.setDotStyle(i == 1 ? 1 : 0)) { wsSaveSince = static_cast<long>(tickNo); } });
    systemBank.setDigitGroupingSource([&] { return wsMemory.saved().dotStyle; });
    ui->global<SystemEvents>().on_display_high_contrast([&](bool on) { if (wsMemory.setHighContrast(on)) { wsSaveSince = static_cast<long>(tickNo); } });
    systemBank.setHighContrastSource([&] { return wsMemory.saved().highContrast; });
    // A FILE DROPPED ON THE WINDOW (item 8; the classic's submitPendingDrop): a WAV is a recording to play, an XML or CSV a frequency list to import, anything else is refused with a note under the counter.
    // The OS drop reaches this through gaps_win's drop target; a bounded run calls it directly (FOXSDR_SLINT_DROP=<tick>:<path>[;...]) because a real drag cannot be scripted.
    handleDrop = [&](const std::string& path) {
        switch (foxsdr::slintgaps::classifyDrop(path)) {
            case foxsdr::slintgaps::DropKind::IqRecording: signalBank.openDroppedRecording(path); ++dropsTaken; dropNote = "recording"; break;
            case foxsdr::slintgaps::DropKind::FrequencyList: signalBank.importDroppedList(path); ++dropsTaken; dropNote = "list"; break;
            default: ++dropsRefused; dropNote = cascade::i18n::tr("That file is neither a recording nor a frequency list."); noteText = dropNote; noteTick = tickNo; break;
        }
    };
    ui->on_counter_menu_requested([&](float x, float y) { openMenu(MenuKind::Counter, x, y); });
    ui->on_mode_selected([&](int i) { setMode(demodOfIndex(i)); });
    ui->on_mode_picker_requested([&](float x, float y) { openMenu(MenuKind::Modes, x, y); });
    ui->on_squelch_changed([&](float v) { setSquelch(squelchDbFromNorm(v)); });
    ui->on_squelch_auto_clicked([&] { setSquelch(squelchAuto(state.squelchDb) ? -85.0 : -200.0); });
    ui->on_volume_changed([&](float v) { setVolume(v); });
    ui->on_gain_changed([&](float v) {
        if (!lists.haveGain) { return; }
        FoxCommand c = op(FOXAPI_OP_SET_GAIN); std::snprintf(c.text, sizeof(c.text), "%s", lists.gainStage.c_str());
        c.num[0] = lists.gainMin + (lists.gainMax - lists.gainMin) * v; submit(c, static_cast<std::uint32_t>(radios.selected) + 1u);   // aimed at the radio whose stage name this is
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
    ui->on_trace_mode_selected([&](int i) { traceMode = i; ui->set_trace_mode(i); resetTrace(selected().traceState); });
    ui->on_range_manual_selected([&](bool manual) { rangeManual = manual; ui->set_range_manual(manual); if (!manual) { setDisplayRange(-110, 0); } });
    ui->on_markers_clicked([&] { toolsBank.openMarkers(); });   // the toolbar's Markers key opens the Markers sheet (the marker menu is the waterfall's right-click)
    ui->on_band_plan_toggled([&] { bandPlanOn = !bandPlanOn; ui->set_band_plan_on(bandPlanOn); });
    ui->on_zoom([&](int dir) { if (dir == 0) { selected().resetView(); } else { zoomView(dir > 0 ? kZoomPerNotch : 1.0 / kZoomPerNotch, 0.5); } });
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
    ui->on_spectrum_double_clicked([&](float) { selected().resetView(); });
    ui->on_waterfall_double_clicked([&] { selected().resetView(); });
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
        selected().lastBandDragTick = tickNo;
    });
    ui->on_edge_dragged([&](float half) { FoxCommand c = op(FOXAPP_OP_SET_BANDWIDTH_DRAG); c.num[0] = 2.0 * half * currentView().spanHz; submit(c); });
    const auto wheel = [&](float x, int notches, bool shift) {
        if (shift) { stepTune(notches > 0 ? 1 : -1, modeStepHz(state.demodMode)); return; }
        zoomView(notches > 0 ? kZoomPerNotch : 1.0 / kZoomPerNotch, x);
    };
    ui->on_spectrum_wheel(wheel);
    ui->on_waterfall_wheel(wheel);
    ui->on_waterfall_panned([&](float dx) { selected().pan(dx); });
    ui->on_mark_tab_clicked([&](int i, bool wf, float x, float y) {   // lane S1: layout One's tab
        const auto& tabs = wf ? selected().fig.wfMarkTabs : selected().fig.markTabs;
        if (i < 0 || static_cast<std::size_t>(i) >= tabs.size()) { return; }
        markTabItems = tabs[static_cast<std::size_t>(i)].items;
        markTabRadio = 0;
        openMenu(MenuKind::MarkTab, x, y);
    });
    ui->on_marker_hovered([&](int i) {
        if (i >= 0 && i < static_cast<int>(lists.markers.size())) {
            const auto& m = lists.markers[static_cast<std::size_t>(i)];
            ui->set_marker_tooltip(ss("M" + std::to_string(m.id) + " " + fmtMHz(m.hz) + (m.note.empty() ? "" : " \xC2\xB7 " + m.note) + " \xC2\xB7 right-click to remove"));
        }
    });
    // THE PANES' GESTURES (ui/workspace.slint WorkspaceModel): each is aimed at its pane's radio (FoxCommand.target = the radio's number, radio_view.hpp aimAt) and does NOT move the selection;
    // the header, the tag and the readout row select, and so does a right-click (its menu is the selected radio's, as every menu of this window is). Click-to-tune, the drag of the band, the edge
    // drag, the wheel and the waterfall's pan are the receiver view's gestures over THAT radio's picture (its own zoom, its own state).
    {
        auto& wm = ui->global<WorkspaceModel>();
        const auto viewOf = [&](int radio) -> RadioView& { return radios.at(radio - 1); };
        paneGestures = std::make_unique<PaneGestures>(radios, [&](const FoxCommand& c, std::uint32_t radio) { submit(c, radio); });
        wm.on_layout_selected([&](int l) { workspace.setLayout(l); });
        wm.on_page_selected([&](int p) { workspace.setPage(p); if (wsMemory.userPage(workspace.page())) { wsSaveSince = static_cast<long>(tickNo); } });
        wm.on_select_requested([&](int r) { selectRadio(r); });
        wm.on_stop_clicked([&](int r) { paneGestures->stop(r); });
        const auto paneHoverAt = [&](int r, float x, float y) { paneHover[static_cast<std::size_t>(std::clamp(r - 1, 0, kMaxRadios - 1))] = (x >= 0 && y >= 0) ? std::make_pair(x, y) : std::make_pair(-1.f, -1.f); };
        wm.on_spectrum_hover(paneHoverAt);
        wm.on_waterfall_hover(paneHoverAt);
        wm.on_spectrum_clicked([&](int r, float x, bool shift) { paneGestures->click(r, x, shift); });
        wm.on_waterfall_clicked([&](int r, float x, bool shift) { paneGestures->click(r, x, shift); });
        wm.on_spectrum_double_clicked([&](int r, float) { paneGestures->resetView(r); });
        wm.on_waterfall_double_clicked([&](int r) { paneGestures->resetView(r); });
        wm.on_band_dragged([&](int r, float dx) { paneGestures->bandDrag(r, dx, tickNo); });
        wm.on_edge_dragged([&](int r, float half) { paneGestures->edgeDrag(r, half); });
        wm.on_spectrum_wheel([&](int r, float x, int n, bool shift) { paneGestures->wheel(r, x, n, shift); });
        wm.on_waterfall_wheel([&](int r, float x, int n, bool shift) { paneGestures->wheel(r, x, n, shift); });
        wm.on_waterfall_panned([&](int r, float dx) { paneGestures->pan(r, dx); });
        wm.on_spectrum_menu_requested([&](int r, float x, float px, float py) { selectRadio(r); menuHz = viewOf(r).hzAtFrac(x); openMenu(MenuKind::Trace, px, py); });
        wm.on_waterfall_menu_requested([&](int r, float x, float px, float py) { selectRadio(r); menuHz = viewOf(r).hzAtFrac(x); openMenu(MenuKind::Marker, px, py); });
        wm.on_marker_hovered([&](int, int) {});   // a pane's marker tips: stage S6
        wm.on_mark_tab_clicked([&](int r, int i, bool wf, float x, float y) {   // lane S1: a pane's "+N" tab (its radio is selected when a name is picked)
            if (r < 1 || r > kMaxRadios) { return; }
            const auto& tabs = wf ? radios.at(r - 1).fig.wfMarkTabs : radios.at(r - 1).fig.markTabs;
            if (i < 0 || static_cast<std::size_t>(i) >= tabs.size()) { return; }
            markTabItems = tabs[static_cast<std::size_t>(i)].items;
            markTabRadio = r;
            openMenu(MenuKind::MarkTab, x, y);
        });
    }
    // ---- STAGE S4: the strip, the audio mix, the card and the status chips ----
    {
        auto& wm = ui->global<WorkspaceModel>();
        const auto stateOf = [&](int radio) -> const FoxReceiverState& { return deckStates[static_cast<std::size_t>(std::clamp(radio - 1, 0, kMaxRadios - 1))]; };
        const auto anyRunning = [&] {
            for (int s = 0; s < kMaxRadios; ++s) { if (workspace.occupied(s) && (deckStates[static_cast<std::size_t>(s)].flags & FOXAPI_RX_RUNNING) != 0u) { return true; } }
            return false;
        };
        const auto readDeviceRows = [&] {
            deviceRows.clear();
            const std::int32_t n = api->read_list(session, FOXAPI_LIST_DEVICES, 0, nullptr);
            for (std::int32_t i = 0; i < n && i < 64; ++i) {
                FoxListItem it{}; it.structSize = sizeof(it);
                if (api->read_list(session, FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i), &it) <= i) { break; }
                DeviceRow row; row.index = i; row.name = it.name; row.detail = it.detail;
                deviceRows.push_back(std::move(row));
            }
        };
        readDeviceRows();
        wm.on_master_clicked([&](int r) { selectRadio(r); });
        wm.on_selector_clicked([&](int r) { selectRadio(r); });
        wm.on_stop_selected_clicked([&] { FoxCommand c = op(FOXAPI_OP_RUN); c.ival[0] = 0; submit(c, static_cast<std::uint32_t>(radios.selected) + 1u); });
        wm.on_stop_all_clicked([&] { FoxCommand c = op(FOXAPI_OP_RUN); c.ival[0] = anyRunning() ? 0 : 1; submit(c, FOXAPI_TARGET_ALL_RADIOS); });
        wm.on_route_clicked([&](int r) {
            FoxCommand c = op(FOXAPI_OP_RADIO_AUDIO);
            c.ival[0] = r;
            c.ival[1] = nextRoute(routeOfWord(radioFacts[static_cast<std::size_t>(std::clamp(r - 1, 0, kMaxRadios - 1))].audio));
            submit(c);
        });
        wm.on_mute_clicked([&](int r) { FoxCommand c = op(FOXAPI_OP_SET_MUTED); c.ival[0] = (stateOf(r).flags & FOXAPI_RX_MUTED) ? 0 : 1; submit(c, static_cast<std::uint32_t>(r)); });
        wm.on_solo_clicked([&](int r) {
            FoxCommand c = op(FOXAPI_OP_RADIO_SOLO);
            c.ival[0] = soloRadio == r ? 0 : r;
            soloRadio = static_cast<int>(c.ival[0]);
            soloAskedTick = static_cast<long>(tickNo);
            soloPending = !seam;   // an engine answers (deckOnAnswer) and the fact is read back; the made-up radios of the test seam have none
            submit(c);
        });
        wm.on_volume_changed([&](int r, float v) { FoxCommand c = op(FOXAPI_OP_SET_VOLUME); c.num[0] = std::clamp(static_cast<double>(v), 0.0, 1.0); submit(c, static_cast<std::uint32_t>(r)); });
        wm.on_add_open_requested([&] { addOpen = true; addNote.clear(); readDeviceRows(); });
        if (std::getenv("FOXSDR_SLINT_ADD_SHEET") != nullptr && bounded.bounded()) {
            addSheetCapture = [&](int at) {
                if (at == 20) { addOpen = true; addNote.clear(); readDeviceRows(); }
                if (at == 45) { FoxCommand c = op(FOXAPI_OP_RADIO_ADD); c.ival[0] = 0; c.ival[1] = 1; std::snprintf(c.text, sizeof(c.text), "soundcard:"); addTicket = 0; submit(c); addTicket = lastTicket; }
            };
        }
        wm.on_add_dismissed([&] { addOpen = false; addNote.clear(); });
        wm.on_add_rescan_clicked([&] { submit(op(FOXAPI_OP_SCAN_DEVICES)); addNote.clear(); readDeviceRows(); });
        const auto addFromCard = [&](int slot, int choice, const std::string& typed) {
            // The devices on offer are those not already held (radio_mix_logic addChoices); the engine's ledger has the last word and its refusal is shown as it worded it.
            std::vector<std::string> held;
            for (int s = 0; s < kMaxRadios; ++s) { if (workspace.occupied(s) && !radioFacts[static_cast<std::size_t>(s)].device.empty()) { held.push_back(radioFacts[static_cast<std::size_t>(s)].device); } }
            const std::vector<DeviceChoice> choices = addAllChoices(deviceRows, held);
            if (choice < 0 || choice >= static_cast<int>(choices.size())) { return; }
            const std::string id = addIdFor(choices[static_cast<std::size_t>(choice)], typed);   // "" = a source that needs text and none was typed
            if (id.empty()) { return; }
            FoxCommand c = op(FOXAPI_OP_RADIO_ADD);
            c.ival[0] = slot;
            c.ival[1] = 1;   // a radio added from the card starts running, as the first radio does
            std::snprintf(c.text, sizeof(c.text), "%s", id.c_str());
            addTicket = 0;
            submit(c);
            addTicket = lastTicket;
            addSwitchLayout = workspace.radioCount() == 1 && workspace.layout() == kLayoutOne;   // the first radio added shows beside the first (Side by side)
        };
        wm.on_add_chosen([&](int slot, int choice) { addFromCard(slot, choice, std::string()); });
        wm.on_add_entered([&](int slot, int choice, slint::SharedString text) { addFromCard(slot, choice, std::string(text)); });
        wm.on_remove_requested([&](int r) {
            if (r < 2) { return; }
            if (removeNeedsConfirm(stateOf(r))) { removeConfirm = r; return; }
            linkGroup.forget(r - 1);
            FoxCommand c = op(FOXAPI_OP_RADIO_REMOVE); c.ival[0] = r; submit(c);
        });
        wm.on_remove_confirmed([&] {
            if (removeConfirm >= 2) { linkGroup.forget(removeConfirm - 1); FoxCommand c = op(FOXAPI_OP_RADIO_REMOVE); c.ival[0] = removeConfirm; submit(c); }
            removeConfirm = 0;
        });
        wm.on_remove_cancelled([&] { removeConfirm = 0; });
        ui->on_status_clicked([&](int i) { if (i >= 0 && i < static_cast<int>(chipRadios.size())) { selectRadio(chipRadios[static_cast<std::size_t>(i)]); } });
    }
    // ---- STAGE S5: Link tuning, Start together and the saved layout (docs/multi-radio-records/screen-s5.md) ----
    {
        auto& wm = ui->global<WorkspaceModel>();
        // WHAT WAS SAVED comes back at the start: the chosen layout, each radio's splitter, Link and the radios left out of the group. The selected radio and the page wait for their radios (restoreDue, in the
        // tick). Nothing here writes: the record is changed only by what the user does from now on (a restart that comes back with one radio leaves the saved eight-radio layout as it was).
        wsMemory.load(duties.interfaceHalf().view3d);
        const SavedWorkspace& sv = wsMemory.saved();
        foxsdr::slintgaps::FrequencyLock::instance().setMask(sv.locked);   // the radios whose frequency was locked when the window last closed (slint-gaps.md item 3)
        if (sv.present) {
            workspace.setLayout(sv.layout);
            std::vector<float> sp(sv.splits.begin(), sv.splits.end());
            wm.set_splits(std::make_shared<slint::VectorModel<float>>(std::move(sp)));
            linkGroup.restore(sv.linkOn, sv.excluded);
        }
        wm.on_link_toggled([&] { linkGroup.setOn(!linkGroup.on(), workspace.mask()); });
        wm.on_pane_link_clicked([&](int r) { (void)linkGroup.setLinked(r - 1, !linkGroup.linked(r - 1, workspace.mask()), workspace.mask()); });
        // START TOGETHER is ONE command, RUN 1 aimed at every radio (FOXAPI_TARGET_ALL_RADIOS), which the engine applies in one pass. In a bounded run the host also MEASURES what "together" came to: the engine
        // stamps each frame with its own clock (FoxSpectrumInfo.unixMs), so the first frame each started radio publishes afterwards is read as soon as it is there and the spread between the first and the
        // last radio's is the answer (state workspace.together). The product does not measure: it sends the command.
        wm.on_together_clicked([&] {
            const bool probe = bounded.bounded() && FOXAPI_HAS(api, read_radio_spectrum);
            std::array<std::uint64_t, kMaxRadios> before{};
            std::array<bool, kMaxRadios> want{};
            float one[1] = {0.f};
            int expected = 0;
            if (probe) {
                for (int s = 0; s < kMaxRadios; ++s) {
                    if (!workspace.occupied(s) || (deckStates[static_cast<std::size_t>(s)].flags & FOXAPI_RX_RUNNING) != 0u) { continue; }
                    FoxSpectrumInfo info{}; info.structSize = sizeof(info);
                    (void)api->read_radio_spectrum(session, static_cast<std::uint32_t>(s) + 1u, 0, &info, one, 1);
                    before[static_cast<std::size_t>(s)] = info.seq;
                    want[static_cast<std::size_t>(s)] = true;
                    ++expected;
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            FoxCommand c = op(FOXAPI_OP_RUN); c.ival[0] = 1;
            submit(c, FOXAPI_TARGET_ALL_RADIOS);
            if (!probe) { return; }
            std::array<long long, kMaxRadios> engineMs{};
            std::array<double, kMaxRadios> hostMs{};
            std::array<bool, kMaxRadios> got{};
            int have = 0;
            while (have < expected && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000)) {
                for (int s = 0; s < kMaxRadios; ++s) {
                    if (!want[static_cast<std::size_t>(s)] || got[static_cast<std::size_t>(s)]) { continue; }
                    FoxSpectrumInfo info{}; info.structSize = sizeof(info);
                    const int32_t rc = api->read_radio_spectrum(session, static_cast<std::uint32_t>(s) + 1u, before[static_cast<std::size_t>(s)], &info, one, 1);
                    if (rc >= 0 && rc != FOXAPI_NO_CHANGE && info.seq > before[static_cast<std::size_t>(s)]) {
                        got[static_cast<std::size_t>(s)] = true;
                        engineMs[static_cast<std::size_t>(s)] = info.unixMs;
                        hostMs[static_cast<std::size_t>(s)] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                        ++have;
                    }
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            long long eMin = 0, eMax = 0; double hMin = 0, hMax = 0; bool first = true;
            for (int s = 0; s < kMaxRadios; ++s) {
                if (!got[static_cast<std::size_t>(s)]) { continue; }
                if (first) { eMin = eMax = engineMs[static_cast<std::size_t>(s)]; hMin = hMax = hostMs[static_cast<std::size_t>(s)]; first = false; }
                eMin = std::min(eMin, engineMs[static_cast<std::size_t>(s)]); eMax = std::max(eMax, engineMs[static_cast<std::size_t>(s)]);
                hMin = std::min(hMin, hostMs[static_cast<std::size_t>(s)]); hMax = std::max(hMax, hostMs[static_cast<std::size_t>(s)]);
            }
            char b[200];
            std::snprintf(b, sizeof(b), "radios=%d first=%d engineSpreadMs=%lld hostFirstMs=%.1f hostLastMs=%.1f hostSpreadMs=%.1f", expected, have, eMax - eMin, hMin, hMax, hMax - hMin);
            togetherNote = b;
            std::fprintf(stderr, "foxsdr-slint: start together: %s\n", b);
        });
    }
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
        else if (id == foxsdr::slintsystem::kShortcutOverlay) { laneC.openShortcuts(foxsdr::slintsystem::keyTableFromConfig(duties.interfaceHalf().keyBindings)); }   // U10
        else if (id == "zoomIn") { zoomView(kZoomPerNotch, 0.5); } else if (id == "zoomOut") { zoomView(1.0 / kZoomPerNotch, 0.5); } else if (id == "zoomReset") { selected().resetView(); }
        else if (id == "record") { FoxCommand c = op(FOXAPI_OP_RECORD_AUDIO); c.ival[0] = (state.flags & FOXAPI_RX_RECORDING_AUDIO) ? 0 : 1; submit(c); }
        else if (id == "screenshot") { screenshot(); }
        else if (id == "fullscreen") { ui->window().set_maximized(!ui->window().is_maximized()); }
        else if (id == "bankSignal") { bankIndex = 0; ui->set_bank_index(0); } else if (id == "bankDecode") { bankIndex = 1; ui->set_bank_index(1); }
        else if (id == "bankView") { bankIndex = 2; ui->set_bank_index(2); } else if (id == "bankExtend") { bankIndex = 3; ui->set_bank_index(3); }
        else if (id == "bankSystem") { bankIndex = 4; ui->set_bank_index(4); }
        else if (id == "openSettings") { bankIndex = 4; ui->set_bank_index(4); openSections[4].insert(5); }
        // STAGE S5: Ctrl+1 to Ctrl+8 select a radio (one that is not open is left alone), Alt+PageUp / Alt+PageDown turn the page of radios (where One is shown, they select the neighbouring radio)
        else if (id.size() == 12 && id.rfind("selectRadio", 0) == 0 && id[11] >= '1' && id[11] <= '8') { const int r = id[11] - '0'; if (workspace.occupied(r - 1)) { selectRadio(r); } }
        else if (id == "radioPagePrev" || id == "radioPageNext") {
            const int dW = static_cast<int>(ui->get_win_w()), dH = static_cast<int>(ui->get_win_h());
            const bool oneShown = workspace.shownLayout(dW, dH) == kLayoutOne;
            if (workspace.stepPage(id == "radioPagePrev" ? -1 : 1, dW, dH, engineReads)) {
                const bool saved = oneShown ? wsMemory.userSelected(workspace.selectedRadio(), workspace.page()) : wsMemory.userPage(workspace.page());
                if (saved) { wsSaveSince = static_cast<long>(tickNo); }
            }
        }
        else if (id == "escape" && scopeBank.radarOn() && !ui->get_editing() && !ui->get_about_open()) { scopeBank.leaveRadar(); }   // the radar scope's second way out (aw.cpp 13589): not while a field is being edited
        else if (id == "escape") {
            // ONE LAYER A PRESS (docs 6.61, key_dispatch.hpp escapeStep): the import dialog, then the Plugin store's plugin page (back to the grid), else the overlays
            foxsdr::slintsystem::EscapeState open;
            open.importDialog = ui->get_import_dialog().shown;
            open.storePluginPage = extendBank.models().store.plugin.shown;
            switch (foxsdr::slintsystem::escapeStep(open)) {
                case foxsdr::slintsystem::EscapeStep::ImportDialog: ui->global<SignalEvents>().invoke_import_closed(); break;
                case foxsdr::slintsystem::EscapeStep::StorePluginPage: ui->global<ExtendEvents>().invoke_store_plugin_back(); break;
                case foxsdr::slintsystem::EscapeStep::Overlays: ui->set_editing(false); ui->set_about_open(false); ui->set_bias_question_open(false); laneC.closeOverlays(); break;
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

    // ---- the pictures: one pane's work per visible pane, then its figures into the window ----
    // The plot of layout One: the pixels the spectrum and waterfall boxes of ui/main.slint lay their axes over. A pane's plot is its own size, passed the same way.
    // THE ONE-LAYOUT PLOT: the pixels the spectrum really has (the window reports them, main.slint one-plot-h / one-plot-w), 980 x 330 until it has
    PlotSize layoutOnePlot{980, 330};
    // The radio's spectrum frame. Today's engine API has one spectrum, the focused radio's; the per-radio read (read_radio_spectrum, engine stage 3) answers here by rv.slot.
    const auto readSpectrum = [&](RadioView& rv, FoxSpectrumInfo& info) -> int32_t {
        if (seam && seam->fake(rv.slot)) { return seam->spectrum(rv.slot, rv.sinceSeq, info, rv.bins.data(), static_cast<uint32_t>(rv.bins.size()), realState); }
        if (FOXAPI_HAS(api, read_radio_spectrum)) { return api->read_radio_spectrum(session, static_cast<uint32_t>(rv.slot) + 1u, rv.sinceSeq, &info, rv.bins.data(), static_cast<uint32_t>(rv.bins.size())); }
        return api->read_spectrum(session, rv.sinceSeq, &info, rv.bins.data(), static_cast<uint32_t>(rv.bins.size()));
    };
    // PER VISIBLE PANE: follow a retune that leaves the picture, read the radio's spectrum, paint its trace and waterfall on a new frame, and derive the figures around them
    // (axes laid out over `plot`, ribbon, marks, peak) into rv.fig. Touches no window property; `selectedRadio` adds the one thing only the selected radio feeds, the 3D history.
    const auto paneTick = [&](RadioView& rv, const PlotSize& plot, bool selectedRadio) {
        rv.plot = plot;
        PaneFigures& fig = rv.fig;
        fig = PaneFigures{};
        const FoxReceiverState& st = rv.state;
        // THE PICTURE FOLLOWS A RETUNE THAT LEAVES IT (a beta tester, 2026-11-09: the waterfall left on 7 MHz after a switch to 3.5 MHz): a zoomed picture
        // (view.spanHz > 0) is a window in absolute Hz that no retune touched; when the tuned frequency moved since the last tick and landed outside it, the
        // picture is recentred on it at the same width, inside the baseband the source now delivers (core/view_follow.hpp, the rule shared with the ImGui
        // window). Not within ten ticks of a drag of the band: the hand has it.
        if (rv.view.spanHz > 0 && tickNo - rv.lastBandDragTick > 10) {
            const double full = rv.fullSpan();
            const cascade::core::ViewFollow f = cascade::core::recentreOnRetune(rv.view.centreHz - rv.view.spanHz / 2.0, rv.view.centreHz + rv.view.spanHz / 2.0,
                                                                                st.centreHz - full / 2.0, st.centreHz + full / 2.0, rv.lastTunedHz, st.tunedHz);
            if (f.moved) { rv.view = {0.5 * (f.lowHz + f.highHz), f.highHz - f.lowHz}; }
        }
        rv.lastTunedHz = st.tunedHz;
        // the spectrum: only when the engine published a newer frame
        FoxSpectrumInfo info{}; info.structSize = sizeof(info);
        const int32_t rc = readSpectrum(rv, info);
        const ViewWindow v = rv.currentView();
        fig.view = v;
        if (rc == FOXAPI_OK && info.copied > 0 && isNewFrame(info.seq, rv.sinceSeq)) {
            fig.newFrame = true;
            rv.sinceSeq = info.seq;
            rv.lastInfo = info;
            rv.frame.assign(rv.bins.begin(), rv.bins.begin() + info.copied);
            const std::vector<float>& frame = rv.frame;
            const std::int64_t t = nowMs();
            rv.lastFrameMs = t;
            // the waterfall export's store: one row of levels a quarter second of this radio's frames (export_logic.hpp)
            spectrumStores[static_cast<std::size_t>(rv.slot)].push(static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()),
                                                                   info.centreHz, info.spanHz, frame.data(), frame.size());
            rv.frameTimes.push_back(t);
            while (rv.frameTimes.size() > 2 && t - rv.frameTimes.front() > 5000) { rv.frameTimes.pop_front(); }
            const double fps = rv.frameTimes.size() >= 2 ? 1000.0 * static_cast<double>(rv.frameTimes.size() - 1) / static_cast<double>(std::max<std::int64_t>(1, rv.frameTimes.back() - rv.frameTimes.front())) : 0.0;
            // the zoomed window: the bins inside it
            const ViewWindow cur = rv.currentView();
            const std::size_t b0 = static_cast<std::size_t>(std::clamp(fracOf(cur.centreHz - cur.spanHz / 2.0, info.centreHz, info.spanHz), 0.0, 1.0) * frame.size());
            const std::size_t b1 = std::max(b0 + 1, static_cast<std::size_t>(std::clamp(fracOf(cur.centreHz + cur.spanHz / 2.0, info.centreHz, info.spanHz), 0.0, 1.0) * frame.size()));
            const float* sub = frame.data() + b0;
            const std::size_t subN = std::min(b1, frame.size()) - b0;
            const foxsdr::slinthost::TraceMode mode = traceMode == 1 ? foxsdr::slinthost::TraceMode::PeakHold : traceMode == 2 ? foxsdr::slinthost::TraceMode::Average : traceMode == 3 ? foxsdr::slinthost::TraceMode::Persistence : foxsdr::slinthost::TraceMode::Normal;
            { static const double kTau[3] = {0.3, 1.0, 3.0}; rv.traceState.decay = fps > 0 ? static_cast<float>(std::exp(-1.0 / (kTau[persistenceDecayIdx] * fps))) : 0.9f; }   // the persistence trace's fraction kept a frame
            rv.traceState.averageAlpha = static_cast<float>(fps > 0 ? 1.0 - std::exp(-1.0 / (averageLengthS * fps)) : 0.1);
            const PictureSize& px = rv.wells.size;
            paintTrace(rv.wells.traceMemory(), px.traceW, px.traceH, sub, subN, st.dbMin, st.dbMax, rv.traceState, mode,
                       Rgba{0x7a, 0xd7, 0xff, 255}, Rgba{0xff, 0x9f, 0x43, 255}, Rgba{0xf1, 0xea, 0xd6, 255});
            scrollWaterfallRow(rv.wells.picture.data(), px.wfW, px.wfH, sub, subN, st.dbMin, st.dbMax);
            if (selectedRadio) {   // the 3D surface's history: one row of levels a frame, of THIS frequency window (a zoom, a pan or a retune starts it again)
                const auto q = [](double x) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::llround(x))); };
                const std::uint64_t windowKey = q(cur.centreHz) * 1000003ull ^ q(cur.spanHz) * 998244353ull ^ q(info.centreHz) * 31ull ^ q(info.spanHz) ^ (static_cast<std::uint64_t>(info.binCount) << 40);
                view3dBank.pushWaterfallFrame(t, sub, subN, windowKey);
            }
            std::memcpy(rv.wells.waterfallMemory(), rv.wells.picture.data(), sizeof(Rgba) * px.wfW * px.wfH);
            // the figures around the wells
            fig.spectrumCaption = "Spectrum \xC2\xB7 " + std::to_string(info.binCount) + " bin" + (cur.spanHz < info.spanHz * 0.999 ? " \xC2\xB7 zoom" : "");
            fig.spanText = spanText(cur.spanHz);
            const double history = fps > 0 ? px.wfH / fps : 0.0;
            fig.footLeft = fps > 0 ? fmt("%.0f lines/s", fps) + " \xC2\xB7 " + fmt("%.0f s of history", history) : std::string();
            fig.ageTicks = ageTicks(history, px.wfH);
            // the peak inside the passband
            const double lo = fracOf(st.tunedHz - st.bandwidthHz / 2.0, info.centreHz, info.spanHz), hi = fracOf(st.tunedHz + st.bandwidthHz / 2.0, info.centreHz, info.spanHz);
            const std::size_t p0 = static_cast<std::size_t>(std::clamp(lo, 0.0, 1.0) * frame.size()), p1 = std::max(p0 + 1, static_cast<std::size_t>(std::clamp(hi, 0.0, 1.0) * frame.size()));
            float peak = -300.f;
            for (std::size_t i = p0; i < p1 && i < frame.size(); ++i) { peak = std::max(peak, frame[i]); }
            fig.showPeak = peak > -299.f;
            fig.peakText = fmt("%.1f dBFS", peak);
            fig.peakAge = "now";
            fig.peakAgeSet = true;
        } else if (rv.lastFrameMs > 0) {
            const double age = static_cast<double>(nowMs() - rv.lastFrameMs) / 1000.0;
            fig.peakAge = age < 0.25 ? "now" : fmt("%.1f s ago", age);
            fig.peakAgeSet = true;
        }
        // the axes and overlays follow the view every tick (a zoom or pan changes them without a new frame)
        fig.freqTicks = frequencyTicks(v.centreHz, v.spanHz, plot.w);
        fig.dbTicks = dbTicks(st.dbMin, st.dbMax, plot.h);
        fig.vfoPos = rv.frac(st.tunedHz);
        fig.vfoHalf = static_cast<float>(v.spanHz > 0 ? st.bandwidthHz / 2.0 / v.spanHz : 0.0);
        for (const auto& b : lists.bands) {
            const double f0 = fracOf(b.startHz, v.centreHz, v.spanHz), f1 = fracOf(b.stopHz, v.centreHz, v.spanHz);
            if (f1 < 0 || f0 > 1) { continue; }
            fig.ribbon.push_back({static_cast<float>(std::clamp(f0, 0.0, 1.0)), static_cast<float>(std::clamp(f1, 0.0, 1.0)), b.name});
        }
        // THE BOOKMARK MARKS (docs 6.52): none while "On the spectrum" is off (bookmarkMarkers, as the classic drawBookmarkMarkers has it); the
        // names stack onto up to three rows while "Stack close names" is on (bookmarkStackNames), one row otherwise - bookmarkNameRows is the rule.
        if (duties.interfaceHalf().bookmarkMarkers) {
            std::vector<BookmarkName> names;
            for (const auto& b : lists.bookmarks) { const double f = fracOf(b.hz, v.centreHz, v.spanHz); if (f >= 0 && f <= 1 && names.size() < 64) { names.push_back({f, b.name, b.favourite}); } }
            const std::vector<int> rows = bookmarkNameRows(names, static_cast<double>(rv.wells.size.traceW), kSpectrumCapCharPx, duties.interfaceHalf().bookmarkStackNames ? kBookmarkNameMaxRows : 1);
            for (std::size_t i = 0; i < names.size(); ++i) { fig.bookmarks.push_back({static_cast<float>(names[i].pos), names[i].label, names[i].favourite, rows[i]}); }
        }
        int dashedSeen = 0;   // a crowd of dashed lines costs the frame (each is a stack of small rectangles, measured: 256 marks with one in five dashed took the tick interval from 33 to 40 ms): past 8 they are drawn solid
        for (const auto& m : lists.marks) {
            const double f = fracOf(m.hz, v.centreHz, v.spanHz);
            const double w = m.span && v.spanHz > 0 ? m.widthHz / v.spanHz : 0.0;
            if (f + w < 0 || f > 1) { continue; }
            const std::uint8_t a = static_cast<std::uint8_t>(std::max<std::uint32_t>(0x60, m.rgba & 0xffu));
            FigureMark fm{static_cast<float>(f), static_cast<float>(w), m.name, 0, 0, 0, 0, m.dashed};
            fm.hz = m.hz;
            if (fm.dashed && ++dashedSeen > 8) { fm.dashed = false; }
            fm.tip = m.plugin;   // (the module's name for now; the layout below turns it into the tooltip of a name that is drawn)
            if (m.rgba != 0) { fm.a = a; fm.r = static_cast<std::uint8_t>(m.rgba >> 24); fm.g = static_cast<std::uint8_t>(m.rgba >> 16); fm.b = static_cast<std::uint8_t>(m.rgba >> 8); }
            fig.marks.push_back(fm);
            if (!m.spectrumOnly) { fig.wfMarks.push_back(fm); }
        }
        {   // THE NAMES OF PLUGIN MARKS (lane S1, mark_layout.hpp): stacked into rows, what fits no row counted into "+N" tabs; a name that is drawn gets its tooltip (what it is, its frequency, the plugin it came from)
            const int specRows = std::clamp((plot.h - 24) / 14, 1, 3), wfRows = plot.h >= 200 ? 3 : 2;
            const auto place = [&](std::vector<FigureMark>& marks, int rows, std::vector<PaneFigures::MarkTabFig>& tabs) {
                std::vector<foxsdr::slintmarks::MarkName> names;
                names.reserve(marks.size());
                for (const FigureMark& m : marks) { names.push_back({m.pos, foxsdr::slintmarks::characterCount(m.label)}); }
                const foxsdr::slintmarks::MarkLayout lay = foxsdr::slintmarks::declutter(names, plot.w, kSpectrumCapCharPx, rows);
                for (std::size_t i = 0; i < marks.size(); ++i) {
                    FigureMark& m = marks[i];
                    m.row = lay.row[i];
                    const std::string plugin = m.tip;
                    m.tip.clear();
                    if (m.row >= 0) { m.tip = foxsdr::slintmarks::tooltipText(m.label, fmtMHz(m.hz), plugin.empty() ? std::string() : cascade::core::formatText(cascade::i18n::tr("from %s"), plugin.c_str())); }
                }
                for (const auto& t : lay.tabs) {
                    PaneFigures::MarkTabFig tab;
                    tab.pos = static_cast<float>(t.pos);
                    tab.count = static_cast<int>(t.items.size());
                    tab.row = rows;
                    for (const std::size_t i : t.items) { tab.items.emplace_back(marks[i].label, marks[i].hz); }
                    tabs.push_back(std::move(tab));
                }
            };
            place(fig.marks, specRows, fig.markTabs);
            place(fig.wfMarks, wfRows, fig.wfMarkTabs);
        }
        for (const auto& m : lists.markers) {
            const double f = fracOf(m.hz, v.centreHz, v.spanHz);
            if (f < 0 || f > 1) { continue; }
            fig.markers.push_back({static_cast<float>(f), "M" + std::to_string(m.id) + " " + fmt("%.3f MHz", m.hz / 1e6)});
        }
        fig.markerCount = static_cast<int>(lists.markers.size());
        fig.keyFoot = "FLOOR " + fmt("%.0f dB", st.dbMin) + " \xC2\xB7 CEILING " + fmt("%.0f dB", st.dbMax) + (rangeManual ? " \xC2\xB7 manual" : " \xC2\xB7 auto");
        for (int i = 0; i < 5; ++i) { fig.keyLabels.push_back(fmt("%.0f", st.dbMin + (st.dbMax - st.dbMin) * i / 4.0)); }
        fig.footTuned = fmtMHz(st.tunedHz);
        fig.footRest = " \xC2\xB7 " + waterfallFootRight(st, lists.decodersFed.empty() ? std::string() : lists.decodersFed.front());
    };
    // THE MAIN WINDOW'S PICTURE (layout One): the selected radio's pane figures as the properties of ui/main.slint, each model set only when its content changed.
    const auto publishPicture = [&](RadioView& rv) {
        const PaneFigures& fig = rv.fig;
        Signatures& psigs = rv.sigs;
        if (fig.newFrame) {
            ui->set_trace_image(rv.wells.traceImage());
            ui->set_waterfall_image(rv.wells.waterfallImage());
            rv.wells.flip();
            ui->set_spectrum_caption(ss(fig.spectrumCaption));
            ui->set_span_text(ss(fig.spanText));
            ui->set_waterfall_foot_left(ss(fig.footLeft));
            if (psigs.changed("age", sigOf(fig.ageTicks))) {
                std::vector<AgeTick> ticks;
                for (const auto& tk : fig.ageTicks) { ticks.push_back({static_cast<float>(tk.frac == 0.0 ? 22.0 / rv.wells.size.wfH : tk.frac), ss(tk.label)}); }   // "now" sits under the Age caption, as the sheet draws it
                ui->set_age_ticks(std::make_shared<slint::VectorModel<AgeTick>>(std::move(ticks)));
            }
            ui->set_show_peak(fig.showPeak);
            ui->set_peak_text(ss(fig.peakText));
        }
        if (fig.peakAgeSet) { ui->set_peak_age(ss(fig.peakAge)); }
        if (psigs.changed("freq", sigOf(fig.freqTicks))) { std::vector<AxisTick> ft; for (const auto& tk : fig.freqTicks) { ft.push_back({static_cast<float>(tk.frac), ss(tk.label)}); } ui->set_freq_ticks(std::make_shared<slint::VectorModel<AxisTick>>(std::move(ft))); }
        if (psigs.changed("db", sigOf(fig.dbTicks))) { std::vector<DbTick> dt; for (const auto& tk : fig.dbTicks) { dt.push_back({static_cast<float>(tk.frac), ss(tk.label)}); } ui->set_db_ticks(std::make_shared<slint::VectorModel<DbTick>>(std::move(dt))); }
        ui->set_vfo_pos(fig.vfoPos);
        ui->set_vfo_half(fig.vfoHalf);
        ui->set_show_vfo(true);
        {
            std::string sig; for (const auto& b : fig.ribbon) { sig += fmt("%.5f|", b.from) + fmt("%.5f|", b.to) + b.label + ";"; }
            if (psigs.changed("ribbon", sig)) {
                std::vector<BandSegment> ribbon;
                for (const auto& b : fig.ribbon) { ribbon.push_back({b.from, b.to, ss(b.label), slint::Color::from_argb_uint8(0, 0, 0, 0)}); }
                ui->set_ribbon(std::make_shared<slint::VectorModel<BandSegment>>(std::move(ribbon)));
            }
        }
        {
            std::string sig; for (const auto& b : fig.bookmarks) { sig += fmt("%.5f|", b.pos) + b.label + (b.favourite ? "*" : "") + fmt("%d;", b.row); }
            if (psigs.changed("bookmarks", sig)) {
                std::vector<BookmarkMark> bms;
                for (const auto& b : fig.bookmarks) { bms.push_back({b.pos, ss(b.label), b.favourite, b.row}); }
                ui->set_bookmarks(std::make_shared<slint::VectorModel<BookmarkMark>>(std::move(bms)));
            }
        }
        {
            const auto toMark = [](const FigureMark& m) { return PluginMark{m.pos, m.width, ss(m.label), slint::Color::from_argb_uint8(m.a, m.r, m.g, m.b), m.dashed, m.row, ss(m.tip)}; };
            const auto toTab = [](const PaneFigures::MarkTabFig& t) { return MarkTab{t.pos, t.count, t.row}; };
            std::string sig; for (const auto& m : fig.marks) { sig += fmt("%.5f|", m.pos) + fmt("%.5f|", m.width) + m.label + (m.dashed ? "d" : "") + std::to_string(m.row) + m.tip + ";"; }
            std::string wsig; for (const auto& m : fig.wfMarks) { wsig += fmt("%.5f|", m.pos) + fmt("%.5f|", m.width) + m.label + std::to_string(m.row) + ";"; }
            std::string tsig; for (const auto& t : fig.markTabs) { tsig += fmt("%.5f|", t.pos) + std::to_string(t.count) + ";"; }
            std::string wtsig; for (const auto& t : fig.wfMarkTabs) { wtsig += fmt("%.5f|", t.pos) + std::to_string(t.count) + ";"; }
            if (psigs.changed("marks", sig)) { std::vector<PluginMark> marks; for (const auto& m : fig.marks) { marks.push_back(toMark(m)); } ui->set_marks(std::make_shared<slint::VectorModel<PluginMark>>(std::move(marks))); }
            if (psigs.changed("wfmarks", wsig)) { std::vector<PluginMark> wfMarks; for (const auto& m : fig.wfMarks) { wfMarks.push_back(toMark(m)); } ui->set_wf_marks(std::make_shared<slint::VectorModel<PluginMark>>(std::move(wfMarks))); }
            if (psigs.changed("marktabs", tsig)) { std::vector<MarkTab> tabs; for (const auto& t : fig.markTabs) { tabs.push_back(toTab(t)); } ui->set_mark_tabs(std::make_shared<slint::VectorModel<MarkTab>>(std::move(tabs))); }
            if (psigs.changed("wfmarktabs", wtsig)) { std::vector<MarkTab> tabs; for (const auto& t : fig.wfMarkTabs) { tabs.push_back(toTab(t)); } ui->set_wf_mark_tabs(std::make_shared<slint::VectorModel<MarkTab>>(std::move(tabs))); }
        }
        {
            std::string sig; for (const auto& m : fig.markers) { sig += fmt("%.5f|", m.pos) + m.label + ";"; }
            if (psigs.changed("markers", sig)) {
                std::vector<UserMarker> ums;
                for (const auto& m : fig.markers) { ums.push_back({m.pos, ss(m.label)}); }
                ui->set_markers(std::make_shared<slint::VectorModel<UserMarker>>(std::move(ums)));
            }
        }
        ui->set_marker_count(fig.markerCount);
        ui->set_key_foot(ss(fig.keyFoot));
        {
            std::string sig; for (const auto& k : fig.keyLabels) { sig += k + ";"; }
            if (psigs.changed("keylabels", sig)) {
                std::vector<slint::SharedString> keyLabels;
                for (const auto& k : fig.keyLabels) { keyLabels.push_back(ss(k)); }
                ui->set_key_labels(std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(keyLabels)));
            }
        }
        ui->set_waterfall_foot_tuned(ss(fig.footTuned));
        ui->set_waterfall_foot_rest(ss(fig.footRest));
        if (hoverX >= 0) { ui->set_cursor_text(ss(cursorText(rv.hzAtFrac(hoverX), rv.dbAtFrac(hoverX)))); }
    };

    // ---- the tick: heartbeat, read_state, lists, read_spectrum, poll_results -> properties ----
    // THE HOST'S OWN TICK TIME (state workspace.tickms / tickp95, docs/multi-radio-records/screen-s4.md): the last 300 ticks' wall time from the heartbeat to the end of the tick, the host's work with the
    // live radios (reads, pictures, the models) - not the render, which the event loop does after the tick.
    std::deque<double> tickMs;
    struct TickTimer {
        std::deque<double>& d;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        explicit TickTimer(std::deque<double>& dq) : d(dq) {}
        ~TickTimer() { d.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()); if (d.size() > 300) { d.pop_front(); } }
    };
    const auto onTick = [&] {
        const TickTimer tickTimer(tickMs);
        (void)api->heartbeat(session);
        if (const int32_t rs = api->read_state(session, &state); rs < 0) {
            // A removed radio leaves the session's focus on a number with no radio behind it: every focused read answers NO_DEVICE until the session sets another (docs/api-0.3-additions.md 9.21) and the tick
            // would stop here for good. The facts say which radios are left, the selection moves to the lowest and the focus follows (refreshRadios); a radio that is still there is asked for again.
            if (rs == FOXAPI_NO_DEVICE) {
                refreshRadios();
                if (workspace.occupied(radios.selected)) { (void)engineReads.focus(static_cast<std::uint32_t>(radios.selected) + 1u); }
            }
            return;
        }
        foxsdr::slintkit::LiveTable::setListRadio(listRadioNow());
        const bool listMoved = state.listSeq != lastListSeq || listRadioNow() != lastListRadio;
        lastListRadio = listRadioNow();
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
        if (addSheetCapture) { addSheetCapture(tickNo); }
        // THE RADIOS THE ENGINE HAS (fact.radio.count and the per-radio facts): read when the lists moved and every eighth tick besides - a radio added or removed does not move the lists'
        // fingerprint (screen-s4.md), and the panes must follow within a quarter of a second.
        if (listMoved || tickNo % 8 == 1) { refreshRadios(); }
        // TEST SEAM: the strip shows the selected radio; when that is a made-up one its state replaces the engine's here (radio 1's is kept in realState)
        if (seam) { realState = state; if (radios.selected > 0) { state = seam->state(radios.selected, realState); } }
        selected().state = state;   // the selected radio's picture works from the state the strip reads (until the per-radio read of engine stage 3)
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
        {   // slint-gaps.md items 2 and 3: the digit grouping, the padlock, and the note a refused tuning gesture leaves under the counter for three seconds
            if (tickNo % 30 == 1) { winHc = (bounded.bounded() && std::getenv("FOXSDR_SLINT_WINDOWS_HC") != nullptr) ? std::getenv("FOXSDR_SLINT_WINDOWS_HC")[0] == '1' : foxsdr::slintgaps::windowsHighContrast(); }
            auto& lk = foxsdr::slintgaps::FrequencyLock::instance();
            lk.setSelected(workspace.selectedRadio());
            ui->set_freq_locked(lk.locked(workspace.selectedRadio()));
            ui->set_digit_dots(wsMemory.saved().dotStyle);
            if (lk.refusals() != lockSeen) { lockSeen = lk.refusals(); noteTick = tickNo; noteText = cascade::i18n::tr("Frequency locked"); }
            if (noteTick > 0 && tickNo - noteTick < 90) { ui->set_converter_text(ss(noteText)); ui->set_converter_warn(true); }
            if (!dropInstalled && tickNo >= 20 && tickNo % 15 == 0) {   // the OS file drop: once the window exists, our drop target takes the place of the backend's
                if (void* hw = foxsdr::slintgaps::findMainWindow()) { dropInstalled = foxsdr::slintgaps::installFileDrop(hw, [&](const std::string& p) { if (handleDrop) { handleDrop(p); } }); }
            }
            if (!laneCOff) { laneC.tick(state, static_cast<long>(tickNo)); }   // screen lane C: scanner block, memories, clock, sheets (FOXSDR_SLINT_LANEC_OFF=1, a bounded run only, leaves it out: the frame-cost comparison)
            if (!lists.converter && !(noteTick > 0 && tickNo - noteTick < 90)) {   // T9: the airband channel's name where the counter says "via ..." (a note or a converter keeps the place)
                const std::string ch = cascade::core::airbandChannelName(state.tunedHz);
                if (!ch.empty()) { ui->set_converter_text(ss(cascade::core::formatText(cascade::i18n::tr("Channel %s"), ch.c_str()))); }
            }
            if (helpAtTick > 0 && tickNo == helpAtTick) { ui->invoke_menu_activated(605); }
            for (const auto& ms : menuSeam) { if (tickNo == ms.first) { ui->invoke_menu_activated(ms.second); } }
            for (DropStep& ds : dropSeam) { if (!ds.done && tickNo >= ds.tick && handleDrop) { ds.done = true; handleDrop(ds.path); } }
        }
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
            if (readListFor(api, session, listRadioNow(), FOXAPI_LIST_GAINS, 0, &it) > 0) {
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

        // THE DECK (stage S4): every open radio's state - the selected one's is the strip's, the others are read per radio - then the strip, the lamps, the mix and the card into the window
        foxsdr::slintworkspace::DeckInputs deck;
        deck.mask = workspace.mask();
        deck.selectedSlot = radios.selected;
        {
            const int dW = static_cast<int>(ui->get_win_w()), dH = static_cast<int>(ui->get_win_h());
            deck.shownLayout = workspace.shownLayout(dW, dH);
            deck.page = workspace.page();
            for (int s = 0; s < kMaxRadios; ++s) {
                if (!workspace.occupied(s)) { deckStates[static_cast<std::size_t>(s)] = FoxReceiverState{}; deckHave[static_cast<std::size_t>(s)] = false; continue; }
                FoxReceiverState st{}; st.structSize = sizeof(st);
                bool have = true;
                if (s == radios.selected) { st = state; }
                else if (seam && s == 0) { st = realState; }
                else if (seam && seam->fake(s)) { st = seam->state(s, realState); }
                else if (FOXAPI_HAS(api, read_radio_state)) { have = api->read_radio_state(session, static_cast<std::uint32_t>(s) + 1u, &st) >= 0; }
                else { have = false; }
                if (have) { deckStates[static_cast<std::size_t>(s)] = st; }
                deckHave[static_cast<std::size_t>(s)] = have || deckHave[static_cast<std::size_t>(s)];
            }
            deck.facts = radioFacts;
            deck.states = deckStates;
            deck.haveState = deckHave;
            deck.soloRadio = soloRadio;
            std::vector<std::string> held;
            for (int s = 0; s < kMaxRadios; ++s) { if (workspace.occupied(s) && !radioFacts[static_cast<std::size_t>(s)].device.empty()) { held.push_back(radioFacts[static_cast<std::size_t>(s)].device); } }
            deck.choices = addAllChoices(deviceRows, held);
            deck.freeSlots = kMaxRadios - workspace.radioCount();
            deck.addNote = addNote;
            deck.addOpen = addOpen;
            deck.removeConfirm = removeConfirm;
            if (removeConfirm != 0 && !workspace.occupied(removeConfirm - 1)) { removeConfirm = 0; deck.removeConfirm = 0; }
            deck.linkOn = linkGroup.on();
            deckPublisher.publish(deck);
            // LINK TUNING (link_logic.hpp): every open radio's tuned frequency is read (above, once a tick); a move of one linked radio is asked of the others as SET_FREQUENCY aimed at each follower, so the gaps are
            // kept. A radio at a limit stays where its device leaves it (the pane says so), the others move.
            {
                std::array<LinkReading, kMaxRadios> rd{};
                for (int s = 0; s < kMaxRadios; ++s) { rd[static_cast<std::size_t>(s)].valid = workspace.occupied(s) && deckHave[static_cast<std::size_t>(s)]; rd[static_cast<std::size_t>(s)].tunedHz = deckStates[static_cast<std::size_t>(s)].tunedHz; }
                for (const LinkMove& m : linkGroup.step(rd, workspace.mask(), [&](int slot) { return testRanges[static_cast<std::size_t>(std::clamp(slot, 0, kMaxRadios - 1))]; })) {
                    FoxCommand c = op(FOXAPI_OP_SET_FREQUENCY);
                    c.num[0] = m.hz;
                    submit(c, static_cast<std::uint32_t>(m.slot) + 1u);
                }
            }
            // THE SAVED LAYOUT: a saved selection comes back when its radio is open (inside the first ten seconds); then what the user changed is kept, a moment after the last change
            {
                int rr = 0, rp = 0;
                if (wsMemory.restoreDue(static_cast<long>(tickNo), workspace.mask(), rr, rp)) { (void)workspace.select(rr, engineReads); workspace.setPage(rp); }
                std::array<float, 8> sp{};
                const auto splits = ui->global<WorkspaceModel>().get_splits();
                for (std::size_t i = 0; i < sp.size(); ++i) { sp[i] = splits && i < splits->row_count() ? splits->row_data(i).value_or(kPaneSplitDefault) : kPaneSplitDefault; }
                if (wsMemory.observe(workspace.layout(), sp, linkGroup.on(), linkGroup.excluded())) { wsSaveSince = static_cast<long>(tickNo); }
                if (wsMemory.dirty() && static_cast<long>(tickNo) - wsSaveSince >= 15) {
                    cascade::core::AppConfig half = duties.interfaceHalf();
                    const std::string text = wsMemory.text(half.view3d);
                    if (text != half.view3d) { half.view3d = text; (void)duties.saveInterfaceHalf(half); }
                    wsMemory.markWritten();
                }
            }
        }
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
        // STAGE S4: with several radios the bar starts with one chip per open radio (a click selects it); today's chips follow
        chipRadios.clear();
        if (workspace.radioCount() > 1) {
            // With more than four radios, or in a window narrower than 1500 px, the chip is the radio's tag and its lamp only (the status bar has no room for the state words beside today's chips and its two
            // keys); the state word and the figures are its detail lines, which a click on a chip opens
            const bool terse = workspace.radioCount() > 4 || static_cast<int>(ui->get_win_w()) < 1500;
            // STAGE S5 (found in the 1280 x 720 shot with eight radios: the status bar's last key was cut off): with more than four radios in a window under 1500 px even the tag-and-lamp chips are too many for the
            // bar, so they become ONE chip, "Radios 1-8", with the worst state's lamp and one line per radio (state, frequency, mode) that a click opens; the MASTER lamps in the bar over the view say the same
            // and select (the chip no longer does).
            // STAGE S6 (the sweep, 34 languages x 1280 and 800: with three or four radios the chips pushed the status bar's own words and its last keys past the window's edge, which one radio never did): the one chip from three
            // radios up under 1500 px, and from two radios up under 1000 px
            const bool collapse = (workspace.radioCount() > 2 && static_cast<int>(ui->get_win_w()) < 1500) || (workspace.radioCount() > 1 && static_cast<int>(ui->get_win_w()) < 1000);
            if (collapse) {
                const std::vector<RadioChipData> chips = deckPublisher.chips(deck);
                std::vector<slint::SharedString> chipLines;
                bool anyRun = false, anyFail = false;
                std::string sig = "radios";
                for (const RadioChipData& rc : chips) {
                    anyRun = anyRun || (rc.lampLit && !rc.lampRed);
                    anyFail = anyFail || rc.lampRed;
                    chipLines.push_back(ss(rc.key + "  " + rc.value + "  " + rc.detail));
                    sig += "|" + rc.key + rc.value + rc.detail;
                }
                const std::string key = pageLabel(chips.empty() ? 1 : chips.front().radio, chips.empty() ? 1 : chips.back().radio);
                chipRadios.push_back(0);
                cardSig += key + sig + (anyRun ? "G" : "-") + (anyFail ? "R" : "-") + ";";
                cards.push_back({ss(key), ss(""), ss(""), anyFail ? ChipTone::Err : ChipTone::Neutral, anyFail ? LampState::Red : anyRun ? LampState::Green : LampState::Off, true,
                                 std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(chipLines))});
            }
            for (const RadioChipData& rc : collapse ? std::vector<RadioChipData>() : deckPublisher.chips(deck)) {
                chipRadios.push_back(rc.radio);
                const std::string chipValue = terse ? std::string() : rc.value;
                cardSig += rc.key + "|" + chipValue + "|" + (terse ? rc.value + "," + rc.detail : rc.detail) + "|" + std::to_string(rc.tone) + (rc.lampLit ? "L" : "-") + (rc.lampRed ? "R" : "-") + ";";
                std::vector<slint::SharedString> chipLines;
                if (terse) { chipLines.push_back(ss(rc.value)); chipLines.push_back(ss(rc.detail)); }
                cards.push_back({ss(rc.key), ss(chipValue), ss(terse ? std::string() : rc.detail),
                                 rc.tone == 1 ? ChipTone::Good : rc.tone == 2 ? ChipTone::Warn : rc.tone == 3 ? ChipTone::Err : ChipTone::Neutral,
                                 rc.lampLit ? (rc.lampRed ? LampState::Red : LampState::Green) : LampState::Off, true,
                                 std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(chipLines))});
            }
        }
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

        // THE PICTURES: the per-pane work for each pane on screen (layout One: the selected radio's, the only one), then its figures into the window
        // THE WORKSPACE: layout One is today's path, untouched (the selected radio painted at the picture size of today and published into the window's own properties). A multi-pane layout reads and
        // paints each radio on screen at the size of its pane and publishes it into its row of the workspace model; the selected radio is read in either case (the strip, the banks and the 3D history
        // follow it), and a radio that is not on screen is not read. In the seam, radios 2 to N answer from made-up state.
        const int winW = static_cast<int>(ui->get_win_w()), winH = static_cast<int>(ui->get_win_h());
        const int shownLayout = workspace.shownLayout(winW, winH);
        if (shownLayout != lastShownLayout || radios.selected != lastSelectedSlot) {   // the window's own properties may hold another radio's figures: send them all again
            selected().sigs.last.clear();
            lastShownLayout = shownLayout;
            lastSelectedSlot = radios.selected;
        }
        workspacePublisher.publishLayout(workspace, winW, winH, ui->get_band_plan_on() ? ui->get_ribbon_size() : 0);
        paneGainOfSelected = gainTxt == "N/A" ? std::string() : gainTxt;
        if (shownLayout == kLayoutOne) {
            workspace.restorePictures(radios.selected);
            layoutOnePlot.w = std::max(60, static_cast<int>(ui->get_one_plot_w()));
            layoutOnePlot.h = std::max(30, static_cast<int>(ui->get_one_plot_h()));
            paneTick(selected(), layoutOnePlot, true);
            ++paneTicks[static_cast<std::size_t>(radios.selected)];
            publishPicture(selected());
        } else {
            const auto& wm = ui->global<WorkspaceModel>();
            const int cellW = static_cast<int>(wm.get_cell_w()), cellH = static_cast<int>(wm.get_cell_h());
            const auto splits = wm.get_splits();
            std::array<bool, kMaxRadios> onScreen{};
            for (const PaneCell& c : workspace.cells(winW, winH)) { if (c.occupied) { onScreen[static_cast<std::size_t>(c.slot)] = true; } }
            for (const int slot : workspace.slotsToTick(winW, winH)) {
                RadioView& rv = radios.at(slot);
                if (slot == radios.selected) { rv.state = state; }
                else if (seam && slot == 0) { rv.state = realState; }
                else if (seam && seam->fake(slot)) { rv.state = seam->state(slot, realState); }
                else if (FOXAPI_HAS(api, read_radio_state)) {
                    FoxReceiverState st{}; st.structSize = sizeof(st);
                    if (api->read_radio_state(session, static_cast<std::uint32_t>(slot) + 1u, &st) >= 0) { rv.state = st; }
                }
                const float split = splits && static_cast<std::size_t>(slot) < splits->row_count() ? splits->row_data(static_cast<std::size_t>(slot)).value_or(kPaneSplitDefault) : kPaneSplitDefault;
                if (cellW > 0 && cellH > 0) { workspace.fitPictures(slot, cellW, cellH, split); }
                paneTick(rv, rv.plot, slot == radios.selected);
                ++paneTicks[static_cast<std::size_t>(slot)];
                if (!onScreen[static_cast<std::size_t>(slot)]) { continue; }
                foxsdr::slintworkspace::PaneContext ctx;
                ctx.texts = paneTexts(slot + 1, rv.state, slot == radios.selected ? paneGainOfSelected : std::string());
                ctx.decoder = (seam && seam->fake(slot)) ? std::string() : radioDecoder[static_cast<std::size_t>(slot)];
                ctx.notice = paneNotice(radioFacts[static_cast<std::size_t>(slot)].refusal, restoreText, slot + 1);
                if (seam && slot == 1) { ctx.decoder = "APRS"; }   // TEST SEAM: one made-up decoder chip, so the pane's chip is seen
                ctx.traceMode = traceMode;
                const auto h = paneHover[static_cast<std::size_t>(slot)];
                ctx.hoverX = h.first;
                if (h.first >= 0) { ctx.cursorText = cursorText(rv.hzAtFrac(h.first), rv.dbAtFrac(h.first)); }
                ctx.linkShown = linkGroup.on();
                {
                    const LinkState ls = linkGroup.stateOf(slot, workspace.mask());
                    ctx.link = static_cast<int>(ls);
                    ctx.linkLabel = paneLinkLabel(slot + 1);
                    ctx.linkTip = ls == LinkState::CouldNotFollow ? couldNotFollowTip(slot + 1, fmtMHz(rv.state.tunedHz)) : paneLinkTip(ls == LinkState::Linked);
                }
                workspacePublisher.publishPane(rv, ctx);
            }
        }
        const ViewWindow v = selected().fig.view;   // the window the figures were laid out for (before this tick's frame moved the baseband)

        // answers: log anything the engine refused (a refusal UI is the notices of phase 3)
        FoxCommandResult r[8]; for (auto& x2 : r) { x2.structSize = sizeof(x2); }
        const int32_t got = api->poll_results(session, r, 8);
        for (int32_t i = 0; i < got; ++i) {
            toolsBank.onAnswer(r[i]);   // the answers the airband section waits for (an airport's rows, a preset's chunks, Listen, Export, Remove)
            extendBank.onAnswer(r[i]);  // a SERVER_CONFIG's (Web access and CAT control): a refusal becomes the web box's red line
            patchBank.onAnswer(r[i]);   // the draft's SET_GRAPH (its commit is settled by this answer) and the radio scan's
            transmitBank.onAnswer(r[i]);   // a refused LATCH or consent (the Transmit page's key note)
            deckOnAnswer(r[i]);
            if (r[i].status != FOXAPI_OK && r[i].status != FOXAPI_NO_CHANGE) { std::fprintf(stderr, "foxsdr-slint: op 0x%04x answered %d: %s\n", r[i].op, r[i].status, r[i].message); }
        }

        // THE PLUGIN KIT'S DUTIES (plugin_kit.hpp): the directive, the poll, the pictures, the lists when they moved, the front end's facts,
        // the browser's tiles, the track-info lines, the plugin-use table, the events and the tickets - then its pages (kit_publish.cpp).
        kit.tick(state, listMoved, face == Face::Nixie ? "nixie" : face == Face::Drum ? "neon" : "plain");
        extendBank.publish(state);   // before the kit's pages: the store page of kind 9 is built from the model this hands over
        for (const FoxCommandResult& r2 : kit.takeUnclaimedResults()) { patchBank.onAnswer(r2); transmitBank.onAnswer(r2); deckOnAnswer(r2); }   // an answer the kit's own poll took first: the draft waits for it
        patchBank.publish(state);    // before the kit's pages too: the Patch presets page of kind 10 is built from the model this hands over
        transmitBank.publish(state); // ... and the Transmit page of kind 11 (and the key's batch for this tick)
        // THE MAP'S LOOK, from the interface half each tick (SYSTEM > Display saves it there): the aircraft icon size, the trail width and the trail style, bounded
        // as the classic window bounds them (map_compositor.hpp mapLookOf). The kit republishes the maps only when the look changed.
        publisher.setRadios(workspace.mask());
        publisher.setMapLook(foxsdr::slintmap::mapLookOf(duties.interfaceHalf().aircraftIconPx, duties.interfaceHalf().mapTrailWidthPx, duties.interfaceHalf().mapTrailStyle));
        publisher.publish(state);
        signalBank.publish(state);
        systemBank.publish(state);
        scopeBank.publish(state, static_cast<double>(nowMs()) / 1000.0);
        {   // the 3D surface of the waterfall: repainted only while it is the page on screen and something it shows moved
            publishExport();   // lane S1: a finished file, and what the selected radio's store holds
            foxsdr::slintview3d::Wf3dContext wf3d;
            wf3d.visible = ui->get_view_index() == 0;
            wf3d.running = (state.flags & FOXAPI_RX_RUNNING) != 0;
            wf3d.dbMin = state.dbMin;
            wf3d.dbMax = state.dbMax;
            wf3d.centreHz = v.centreHz;
            wf3d.spanHz = v.spanHz;
            wf3d.tunedHz = state.tunedHz;
            wf3d.nowMs = nowMs();
            if (wf3d.visible) { for (const auto& m : lists.markers) { wf3d.markers.push_back({m.hz, "M" + std::to_string(m.id) + " " + fmt("%.3f MHz", m.hz / 1e6)}); } }
            view3dBank.publish(wf3d);
            view3dBank.publishRadar(scopeBank.radar3dFeed(), wf3d.nowMs);   // the radar scope's 3D airspace: its feed is empty unless the SCOPE tab is up with a position
            view3dBank.publishTunnel(scopeBank.tunnel3dFeed(), wf3d.nowMs); // the demod scope's 3D tunnel: its feed is empty unless the window is open on a baseband input
            view3dBank.publishSurvey(wf3d.nowMs);                           // the Survey Engine page's 3D surface: read from the kit's panel, only while that page is on screen
            {   // the Radiosondes page's 3D track: the host records the balloons' heard altitudes every tick, draws only while that page is on screen
                foxsdr::slintview3d::View3DPublisher::SondeContext sc;
                sc.nowMs = wf3d.nowMs;
                sc.hasRx = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
                sc.rxLatDeg = state.rxLatDeg;
                sc.rxLonDeg = state.rxLonDeg;
                view3dBank.publishSonde(sc);
            }
        }
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
    // `state radio.<n>.<field>` (design 3.6: the drive language's per-radio read-back) is `state workspace.r.<n>.<field>`: radio n's own state whichever radio is selected - running, tuned, centre, mode,
    // volume, muted, agc, agcfixed, nb, nbthreshold, device, rate, decoder, notice, gains, gainstage, rates, ratenames, antennas, antennanames. Plain `state <name>` stays the selected radio.
    std::function<std::string(const std::string&, std::string&)> readStateOf;
    const auto readState = [&](const std::string& nameIn, std::string& out) -> std::string {
        if (nameIn.rfind("radio.", 0) == 0) { return readStateOf("workspace.r." + nameIn.substr(6), out); }
        const std::string& name = nameIn;
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
        // LANE S1 read-backs. layouts.count (the user's own), layouts.names (all, "|" between), layouts.open (the sheet), layouts.note; layout.<bank|sections|rail|view|split|workspace|link|wf3d|selected|tuned|mode>
        // (the window as a layout would capture it NOW); export.<note|working|rows|segments|path|length|format> (the selected radio's store and the last file); marks.[r<n>.]<named|tabs|total|wfnamed|wftabs|tabitems0>
        // (the names of plugin marks as laid out: drawn, "+N" tabs, all marks, and how many names tab 0 stands for); trace.<mode|decay|density> (the trace display: 0 normal 1 peak 2 average 3 persistence)
        if (name.rfind("layouts.", 0) == 0) {
            const std::string what = name.substr(8);
            if (what == "count") { out = std::to_string(layoutBook.user().size()); return std::string(); }
            if (what == "names") { std::string j; for (const auto& l : layoutBook.all()) { j += (j.empty() ? "" : "|") + l.name; } out = j; return std::string(); }
            if (what == "open") { out = ui->global<LayoutsModel>().get_open() ? "1" : "0"; return std::string(); }
            if (what == "note") { out = layoutNote; return std::string(); }
            return "layouts.<count|names|open|note>, not " + name;
        }
        if (name.rfind("layout.", 0) == 0) {
            const std::string what = name.substr(7);
            const foxsdr::slintlayouts::Window w = layoutWindow();
            if (what == "bank") { out = std::to_string(w.bank); }
            else if (what == "sections") { out = std::to_string(w.sections); }
            else if (what == "rail") { out = w.railFolded ? "1" : "0"; }
            else if (what == "view") { out = std::to_string(w.view); }
            else if (what == "split") { out = std::to_string(w.splitPct); }
            else if (what == "workspace") { out = std::to_string(w.workspace); }
            else if (what == "link") { out = w.link ? "1" : "0"; }
            else if (what == "wf3d") { out = w.wf3d ? "1" : "0"; }
            else if (what == "selected") { out = std::to_string(w.selected); }
            else if (what == "tuned") { out = std::to_string(w.tunedHz); }
            else if (what == "mode") { out = std::to_string(w.mode); }
            else { return "layout.<bank|sections|rail|view|split|workspace|link|wf3d|selected|tuned|mode>, not " + name; }
            return std::string();
        }
        if (name.rfind("export.", 0) == 0) {
            const std::string what = name.substr(7);
            const foxsdr::slintexport::SpectrumStore& store = spectrumStores[static_cast<std::size_t>(radios.selected)];
            if (what == "note") { out = exportNote; }
            else if (what == "working") { out = exportJob.pending() ? "1" : "0"; }
            else if (what == "rows") { out = std::to_string(store.rows()); }
            else if (what == "segments") { out = std::to_string(store.segments()); }
            else if (what == "path") { out = std::string(ui->global<ExportModel>().get_path()); }
            else if (what == "length") { out = std::to_string(exportLengthIdx); }
            else if (what == "format") { out = std::to_string(exportFormatIdx); }
            else if (what == "lastpath") { out = exportLastPath; }
            else if (what == "fileinfo" || what == "peakoffset") {
                // A BOUNDED RUN'S read-back of the last file written: fileinfo = what kind of file and how much is in it, peakoffset = the strongest column of the newest row of a FoxSDR spectrum file, in Hz
                // from the segment's centre (the carrier's place, as a file reader would find it)
                std::vector<std::uint8_t> bytes;
                if (!exportLastPath.empty()) {
                    if (std::FILE* f = std::fopen(exportLastPath.c_str(), "rb")) {
                        std::uint8_t buf[8192];
                        std::size_t n;
                        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { bytes.insert(bytes.end(), buf, buf + n); }
                        std::fclose(f);
                    }
                }
                if (bytes.empty()) { return std::string("no file has been written"); }
                const bool fsw = bytes.size() > 4 && std::memcmp(bytes.data(), "FXSP", 4) == 0;
                const bool png = bytes.size() > 8 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
                if (what == "peakoffset") {
                    if (!fsw) { return std::string("the last file is not a FoxSDR spectrum file"); }
                    const auto snap = foxsdr::slintexport::readBinary(bytes);
                    if (!snap || snap->empty()) { return std::string("the file cannot be read back"); }
                    const foxsdr::slintexport::Segment& seg = snap->segments.back();
                    const std::size_t row = seg.rows() - 1;
                    std::uint32_t best = 0;
                    for (std::uint32_t c = 1; c < seg.columns; ++c) { if (seg.levels[row * seg.columns + c] > seg.levels[row * seg.columns + best]) { best = c; } }
                    out = fmt("%.0f", (static_cast<double>(best) + 0.5) * seg.spanHz / static_cast<double>(seg.columns) - seg.spanHz / 2.0);
                } else if (png) {
                    out = "png " + foxsdr::slintexport::pngText(bytes, "FoxSDR");
                } else if (fsw) {
                    const auto snap = foxsdr::slintexport::readBinary(bytes);
                    out = snap ? "fsw rows=" + std::to_string(snap->rows()) + " segments=" + std::to_string(snap->segments.size()) : std::string("fsw unreadable");
                } else {
                    std::size_t rows = 0;
                    std::size_t at = 0;
                    while (at < bytes.size()) {   // lines that are not '#' header lines
                        std::size_t end = at;
                        while (end < bytes.size() && bytes[end] != 10) { ++end; }
                        if (end > at && bytes[at] != '#') { ++rows; }
                        at = end + 1;
                    }
                    out = "csv rows=" + std::to_string(rows);
                }
            }
            else { return "export.<note|working|rows|segments|path|length|format|lastpath|fileinfo|peakoffset>, not " + name; }
            return std::string();
        }
        if (name.rfind("marks.", 0) == 0) {
            std::string what = name.substr(6);
            int slot = radios.selected;
            if (what.size() > 2 && what[0] == 'r' && std::isdigit(static_cast<unsigned char>(what[1])) != 0 && what.find('.') != std::string::npos) {
                slot = std::clamp(std::atoi(what.c_str() + 1) - 1, 0, kMaxRadios - 1);
                what = what.substr(what.find('.') + 1);
            }
            const PaneFigures& f = radios.at(slot).fig;
            const auto drawn = [](const std::vector<FigureMark>& v) { int n = 0; for (const auto& m : v) { n += m.row >= 0 ? 1 : 0; } return n; };
            if (what == "named") { out = std::to_string(drawn(f.marks)); }
            else if (what == "tabs") { out = std::to_string(f.markTabs.size()); }
            else if (what == "total") { out = std::to_string(f.marks.size()); }
            else if (what == "wfnamed") { out = std::to_string(drawn(f.wfMarks)); }
            else if (what == "wftabs") { out = std::to_string(f.wfMarkTabs.size()); }
            else if (what == "tabitems0") { out = std::to_string(f.markTabs.empty() ? 0 : f.markTabs[0].items.size()); }
            else { return "marks.[r<n>.]<named|tabs|total|wfnamed|wftabs|tabitems0>, not " + name; }
            return std::string();
        }
        if (name.rfind("trace.", 0) == 0) {
            const std::string what = name.substr(6);
            if (what == "mode") { out = std::to_string(traceMode); }
            else if (what == "decay") { out = std::to_string(persistenceDecayIdx); }
            else if (what == "density") { std::size_t n = 0; for (const float d : selected().traceState.density) { n += d > 0.0f ? 1 : 0; } out = std::to_string(n); }
            else { return "trace.<mode|decay|density>, not " + name; }
            return std::string();
        }
        // figure.<what> (lane S1): the One layout's picture as the host laid it out - plotheight / plotwidth (the pixels the window reports for the spectrum's plot, which the dB labels and the frequency
        // ticks are laid out over), dbticks (how many dB labels), dbpitch (the smallest gap between two of them in pixels, 0 with fewer than two)
        if (name.rfind("figure.", 0) == 0) {
            const std::string what = name.substr(7);
            if (what == "plotheight") { out = std::to_string(layoutOnePlot.h); return std::string(); }
            if (what == "plotwidth") { out = std::to_string(layoutOnePlot.w); return std::string(); }
            std::vector<Tick> ticks;   // the LABELLED ticks (the unlabelled ones are only lines)
            for (const Tick& t : selected().fig.dbTicks) { if (!t.label.empty()) { ticks.push_back(t); } }
            if (what == "dbticks") { out = std::to_string(ticks.size()); return std::string(); }
            if (what == "dbpitch") {
                double least = 0.0;
                for (std::size_t i = 1; i < ticks.size(); ++i) { const double gap = std::fabs(ticks[i].frac - ticks[i - 1].frac) * layoutOnePlot.h; least = i == 1 ? gap : std::min(least, gap); }
                out = fmt("%.1f", least);
                return std::string();
            }
            return "figure.<plotheight|plotwidth|dbticks|dbpitch>, not " + name;
        }
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
        // AGC choice and noise blanker (lane C): the selected radio's settings and its two measurements, as the engine publishes them
        if (name == "agcMode") { out = std::to_string(state.agcMode); return std::string(); }
        if (name == "agcFixedGainDb") { out = fmt("%.6f", state.agcFixedGainDb); return std::string(); }
        if (name == "agcGainDb") { out = fmt("%.6f", state.agcGainDb); return std::string(); }
        if (name == "nbEnabled") { out = std::to_string(state.nbEnabled); return std::string(); }
        if (name == "nbThresholdDb") { out = fmt("%.6f", state.nbThresholdDb); return std::string(); }
        if (name == "nbBlankedPct") { out = fmt("%.6f", state.nbBlankedPct); return std::string(); }
        if (name == "txFrequencyHz") { out = fmt("%.6f", state.txFrequencyHz); return std::string(); }
        if (name == "demodMode") { out = std::to_string(state.demodMode); return std::string(); }
        if (name == "mode") { out = modeName(state.demodMode); return std::string(); }
        if (name == "deemphasis") { out = std::to_string(state.deemphasis); return std::string(); }
        if (name == "decodersRunning") { out = std::to_string(state.decodersRunning); return std::string(); }
        if (name == "decodersFitted") { out = std::to_string(state.decodersFitted); return std::string(); }
        if (name == "decoderLinesTotal") { out = std::to_string(state.decoderLinesTotal); return std::string(); }
        if (name == "gainDb") {
            FoxListItem it{}; it.structSize = sizeof(it);
            if (readListFor(api, session, listRadioNow(), FOXAPI_LIST_GAINS, 0, &it) <= 0) { return std::string("no gain stage"); }
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
        // view3d.<what>: the 3D views' own state (view3d_publish.hpp readState: wf.on, wf.azimuth, wf.elevation, wf.zoom, wf.depth, wf.shading, wf.renders, wf.triangles, wf.pixels ...)
        if (name.rfind("view3d.", 0) == 0) { return view3dBank.readState(name.substr(7), out); }
        // lanec.<what>: screen lane C (lanec_publish.hpp readState)
        if (name == "lanec.logcopies") { out = std::to_string(publisher.copyCount()); return std::string(); }
        if (name == "lanec.logcopy") { out = publisher.lastCopy(); return std::string(); }
        if (name.rfind("lanec.", 0) == 0) { return laneC.readState(name.substr(6), out); }
        // gaps.<what>: the classic-window gaps (docs/multi-radio-records/slint-gaps.md)
        if (name.rfind("gaps.", 0) == 0) {
            const std::string w = name.substr(5);
            auto& lk = foxsdr::slintgaps::FrequencyLock::instance();
            if (w == "locked") { out = lk.locked(workspace.selectedRadio()) ? "1" : "0"; return std::string(); }
            if (w.rfind("locked.", 0) == 0) { out = lk.locked(std::atoi(w.c_str() + 7)) ? "1" : "0"; return std::string(); }
            if (w == "lockmask") { out = std::to_string(lk.mask()); return std::string(); }
            if (w == "lockrefusals") { out = std::to_string(lk.refusals()); return std::string(); }
            if (w == "savedlock") { out = std::to_string(wsMemory.saved().locked); return std::string(); }
            if (w == "dots") { out = std::to_string(wsMemory.saved().dotStyle); return std::string(); }
            if (w == "editor") { out = std::string(ui->get_editor_text()); return std::string(); }
            if (w == "lockedlabel") { out = std::string(ui->get_converter_text()); return std::string(); }
            if (w == "dropnote") { out = dropNote; return std::string(); }
            if (w == "drops") { out = std::to_string(dropsTaken); return std::string(); }
            if (w == "refuseddrops") { out = std::to_string(dropsRefused); return std::string(); }
            if (w == "sdrplay") { out = signalBank.sdrPlayRestartState(); return std::string(); }
            if (w == "sdrplaycalls") { out = std::to_string(signalBank.sdrPlayRestartCalls()); return std::string(); }
            if (w == "help") { out = std::to_string(helpOpened); return std::string(); }
            if (w == "diagcopies") { out = std::to_string(systemBank.diagCopies()); return std::string(); }
            if (w == "diagstatus") { out = systemBank.bundleStatus(); return std::string(); }
            if (w == "hc") { out = wsMemory.saved().highContrast ? "1" : "0"; return std::string(); }
            if (w == "winhc") { out = winHc ? "1" : "0"; return std::string(); }
            if (w == "skin") { out = skinNow; return std::string(); }
            if (w == "clip") { out = fakeClip; return std::string(); }
            if (w == "savenote") { out = publisher.lastSaveNote(); return std::string(); }
            if (w == "helpurl") { out = helpUrlOverride; return std::string(); }
            if (w == "dropinstalled") { out = dropInstalled ? "1" : "0"; return std::string(); }
            return "gaps.locked|locked.<n>|lockmask|lockrefusals|savedlock|dots|editor|lockedlabel|dropnote|drops|refuseddrops|sdrplay|sdrplaycalls|help, not " + name;
        }
        // workspace.<what>: the radio workspace's own state (docs/multi-radio-records/screen-s3.md): layout (chosen), shown, page, pages, selected (1-based), radios, cells (the shown cells' radio numbers, 0 = empty),
        // ticks.<n> (how many ticks radio n's pictures were read), tuned.<n> / running.<n> (the host's last read of radio n), pic.<n> (its trace picture, WxH), cell (one pane's size, WxH), fell (1 = the small-window notice)
        if (name.rfind("workspace.", 0) == 0) {
            const std::string what = name.substr(10);
            const int winW = static_cast<int>(ui->get_win_w()), winH = static_cast<int>(ui->get_win_h());
            const auto slotOf = [&](const std::string& tail) { return std::clamp(std::atoi(tail.c_str()) - 1, 0, kMaxRadios - 1); };
            if (what == "layout") { out = std::to_string(workspace.layout()); }
            else if (what == "shown") { out = std::to_string(workspace.shownLayout(winW, winH)); }
            else if (what == "page") { out = std::to_string(workspace.page()); }
            else if (what == "pages") { out = std::to_string(workspace.pageCount()); }
            else if (what == "selected") { out = std::to_string(workspace.selectedRadio()); }
            else if (what == "radios") { out = std::to_string(workspace.radioCount()); }
            else if (what == "fell") { out = workspace.fellBack(winW, winH) ? "1" : "0"; }
            else if (what == "cells") {
                out.clear();
                for (const PaneCell& c : workspace.cells(winW, winH)) { if (!out.empty()) { out += ","; } out += std::to_string(c.occupied ? c.slot + 1 : 0); }
            }
            else if (what.rfind("ticks.", 0) == 0) { out = std::to_string(paneTicks[static_cast<std::size_t>(slotOf(what.substr(6)))]); }
            else if (what.rfind("tuned.", 0) == 0) { out = fmt("%.0f", radios.at(slotOf(what.substr(6))).state.tunedHz); }
            else if (what.rfind("running.", 0) == 0) { out = (radios.at(slotOf(what.substr(8))).state.flags & FOXAPI_RX_RUNNING) ? "1" : "0"; }
            else if (what.rfind("pic.", 0) == 0) { const auto& sz = radios.at(slotOf(what.substr(4))).wells.size; out = std::to_string(sz.traceW) + "x" + std::to_string(sz.traceH); }
            // peakoff.<n>: where the highest bin of radio n's last spectrum frame is, in Hz from that frame's centre (a test radio's tone, docs/multi-radio-records/screen-s4.md); peakdb.<n> its level;
            // frames.<n> how many frames radio n's pane has had
            else if (what.rfind("peakoff.", 0) == 0 || what.rfind("peakdb.", 0) == 0) {
                const bool off = what[4] == 'o';
                const RadioView& rv = radios.at(slotOf(what.substr(off ? 8 : 7)));
                if (rv.frame.empty() || !(rv.lastInfo.spanHz > 0)) { return std::string("radio ") + what.substr(off ? 8 : 7) + " has no spectrum frame yet"; }
                std::size_t best = 0;
                for (std::size_t i = 1; i < rv.frame.size(); ++i) { if (rv.frame[i] > rv.frame[best]) { best = i; } }
                out = off ? fmt("%.0f", (static_cast<double>(best) + 0.5) / static_cast<double>(rv.frame.size()) * rv.lastInfo.spanHz - rv.lastInfo.spanHz / 2.0) : fmt("%.1f", static_cast<double>(rv.frame[best]));
            }
            // STAGE S4 (docs/multi-radio-records/screen-s4.md): master.<n> the radio's MASTER lamp (0 dark, 1 green, 2 red), audio.<n> its route word, mute.<n> / vol.<n> / agc.<n> / nb.<n> its own
            // state as the deck last read it (the radio's, never the selected radio's), solo (the host's record, 0 = nobody), chips (the radios the status bar has a chip for), free (slots that
            // can still be added), choices (the card's devices), addnote (the engine's refusal as it worded it), removeask (the radio a question waits on), open (the sheet is up)
            // r.<n>.<field>: radio n's OWN state as the deck last read it, on screen or not (running, tuned, centre, mode, volume, muted, agc, agcfixed, nb, nbthreshold, device, rate); an error when
            // radio n is not open
            else if (what.rfind("r.", 0) == 0) {
                const std::size_t dot = what.find('.', 2);
                if (dot == std::string::npos) { return std::string("workspace.r.<n>.<field>, not ") + name; }
                const int s = slotOf(what.substr(2, dot - 2));
                if (!workspace.occupied(s)) { return "radio " + std::to_string(s + 1) + " is not open"; }
                const std::string field = what.substr(dot + 1);
                const FoxReceiverState& st = deckStates[static_cast<std::size_t>(s)];
                if (field == "running") { out = (st.flags & FOXAPI_RX_RUNNING) ? "1" : "0"; }
                else if (field == "tuned") { out = fmt("%.0f", st.tunedHz); }
                else if (field == "centre") { out = fmt("%.0f", st.centreHz); }
                else if (field == "mode") { out = modeName(st.demodMode); }
                else if (field == "volume") { out = fmt("%.2f", st.volume); }
                else if (field == "muted") { out = (st.flags & FOXAPI_RX_MUTED) ? "1" : "0"; }
                else if (field == "agc") { out = std::to_string(st.agcMode); }
                else if (field == "agcfixed") { out = fmt("%.1f", st.agcFixedGainDb); }
                else if (field == "nb") { out = std::to_string(st.nbEnabled); }
                else if (field == "nbthreshold") { out = fmt("%.1f", st.nbThresholdDb); }
                else if (field == "device") { out = st.deviceName; }
                else if (field == "rate") { out = fmt("%.0f", st.sampleRateHz); }
                // SCREEN STAGE S6: what radio n's pane says about its device and what the engine lists for THIS radio (read_radio_list by number, whatever the selection is)
                else if (field == "decoder") { out = radioDecoder[static_cast<std::size_t>(s)]; }
                else if (field == "notice") { out = paneNotice(radioFacts[static_cast<std::size_t>(s)].refusal, restoreText, s + 1); }
                else if (field == "gains" || field == "rates" || field == "antennas" || field == "gainstage" || field == "ratenames" || field == "antennanames") {
                    const std::uint32_t list = (field == "gains" || field == "gainstage") ? FOXAPI_LIST_GAINS : (field == "rates" || field == "ratenames") ? FOXAPI_LIST_SAMPLE_RATES : FOXAPI_LIST_ANTENNAS;
                    int n = 0;
                    std::string joined;
                    for (std::uint32_t i = 0; i < 64; ++i) {
                        FoxListItem it{}; it.structSize = sizeof(it);
                        if (readListFor(api, session, static_cast<std::uint32_t>(s) + 1u, list, i, &it) <= static_cast<int32_t>(i)) { break; }
                        ++n;
                        if (field == "gainstage" && joined.empty()) { joined = it.name; }
                        if (field == "ratenames") { joined += (joined.empty() ? "" : " | ") + fmt("%.0f", it.value[0]); }
                        if (field == "antennanames") { joined += (joined.empty() ? "" : " | ") + std::string(it.name); }
                    }
                    out = (field == "gainstage" || field == "ratenames" || field == "antennanames") ? joined : std::to_string(n);
                }
                else { return "workspace.r.<n>.running|tuned|centre|mode|volume|muted|agc|agcfixed|nb|nbthreshold|device|rate|decoder|notice|gains|gainstage|rates|ratenames|antennas|antennanames, not " + name; }
            }
            // plugin.<file prefix>.radio / .picker: the radio a decoder plugin runs on (fact.plugin.<file>.radio as the kit read it) and the Runs-on picker's model for it ("<current>/<keys>", "" when not drawn)
            else if (what.rfind("plugin.", 0) == 0) {
                const std::size_t dot = what.rfind('.');
                const std::string prefix = what.substr(7, dot - 7), field = what.substr(dot + 1);
                const foxsdr::slintkit::PluginRow* row = nullptr;
                for (const auto& r : kit.lists().rows) { if (r.file.rfind(prefix + "-", 0) == 0) { row = &r; break; } }
                if (row == nullptr) { return "no plugin file starting " + prefix; }
                if (field == "radio") { out = std::to_string(row->radio); }
                else if (field == "picker") { const auto m = foxsdr::slintkit::runsOnFor(*row, workspace.mask()); out = m.shown() ? std::to_string(m.current) + "/" + std::to_string(m.keys.size()) : std::string(); }
                else if (field == "stopped") { out = row->stopped ? "1" : "0"; }
                else { return "workspace.plugin.<prefix>.radio|picker|stopped, not " + name; }
            }
            else if (what.rfind("master.", 0) == 0) {
                const int s = slotOf(what.substr(7));
                out = std::to_string(static_cast<int>(masterStateOf(radioFacts[static_cast<std::size_t>(s)], deckStates[static_cast<std::size_t>(s)])));
            }
            else if (what.rfind("audio.", 0) == 0) { out = radioFacts[static_cast<std::size_t>(slotOf(what.substr(6)))].audio; }
            else if (what.rfind("mute.", 0) == 0) { out = (deckStates[static_cast<std::size_t>(slotOf(what.substr(5)))].flags & FOXAPI_RX_MUTED) ? "1" : "0"; }
            else if (what.rfind("vol.", 0) == 0) { out = fmt("%.2f", deckStates[static_cast<std::size_t>(slotOf(what.substr(4)))].volume); }
            else if (what.rfind("agc.", 0) == 0) { out = std::to_string(deckStates[static_cast<std::size_t>(slotOf(what.substr(4)))].agcMode); }
            else if (what.rfind("nb.", 0) == 0) { out = std::to_string(deckStates[static_cast<std::size_t>(slotOf(what.substr(3)))].nbEnabled); }
            else if (what == "tickms" || what == "tickp95" || what == "tickn") {
                std::vector<double> v(tickMs.begin(), tickMs.end());
                if (v.size() < 10) { return std::string("fewer than ten ticks timed yet"); }
                std::sort(v.begin(), v.end());
                double sum = 0; for (const double x : v) { sum += x; }
                out = what == "tickn" ? std::to_string(v.size()) : what == "tickms" ? fmt("%.2f", sum / static_cast<double>(v.size())) : fmt("%.2f", v[static_cast<std::size_t>(static_cast<double>(v.size() - 1) * 0.95)]);
            }
            // STAGE S5: link (Link tuning is on, 0/1), linked (the radios in the group), link.<n> (0 left out or Link off, 1 linked, 2 could not follow), intended.<n> (the frequency the group wants of radio n, Hz),
            // together (what the last Start together measured, a bounded run), saved (the saved layout's text as the screen holds it), savedview (the interface half's whole setting text, as it would be written)
            else if (what == "link") { out = linkGroup.on() ? "1" : "0"; }
            else if (what == "linked") { out.clear(); for (int s = 0; s < kMaxRadios; ++s) { if (linkGroup.linked(s, workspace.mask())) { if (!out.empty()) { out += ","; } out += std::to_string(s + 1); } } }
            else if (what.rfind("link.", 0) == 0) { out = std::to_string(static_cast<int>(linkGroup.stateOf(slotOf(what.substr(5)), workspace.mask()))); }
            else if (what.rfind("intended.", 0) == 0) { out = fmt("%.0f", linkGroup.intendedHz(slotOf(what.substr(9)))); }
            else if (what == "pagekeys") { const auto pl = ui->global<WorkspaceModel>().get_page_labels(); out = std::to_string(pl ? pl->row_count() : 0); }   // (no model has been set until the first page key is offered)   // the page keys the bar has (0 where One is shown)
            else if (what == "together") { out = togetherNote; }
            else if (what == "togetherms") {   // the engine-clock spread of the first frames of the last Start together, in ms ("" until one has been measured)
                const std::size_t at = togetherNote.find("engineSpreadMs=");
                if (at == std::string::npos) { return std::string("no start together has been measured yet"); }
                out = std::to_string(std::atoll(togetherNote.c_str() + at + 15));
            }
            else if (what.rfind("split.", 0) == 0) {   // a radio's splitter share as the pane's model holds it, in percent
                const auto sp = ui->global<WorkspaceModel>().get_splits();
                const std::size_t i = static_cast<std::size_t>(slotOf(what.substr(6)));
                out = std::to_string(static_cast<int>(std::lround(static_cast<double>(sp && i < sp->row_count() ? sp->row_data(i).value_or(kPaneSplitDefault) : kPaneSplitDefault) * 100.0)));
            }
            else if (what == "saved") { out = workspaceBody(wsMemory.saved()); }
            else if (what == "savedview") { out = duties.interfaceHalf().view3d; }
            else if (what == "solo") { out = std::to_string(soloRadio); }
            else if (what == "chips") { out.clear(); for (const int r : chipRadios) { if (!out.empty()) { out += ","; } out += std::to_string(r); } }
            else if (what == "free") { out = std::to_string(kMaxRadios - workspace.radioCount()); }
            else if (what == "choices") { out = std::to_string(ui->global<WorkspaceModel>().get_add_choices()->row_count()); }
            else if (what == "addnote") { out = addNote; }
            else if (what == "lastadd") { out = lastAddAnswer; }
            else if (what == "removeask") { out = std::to_string(removeConfirm); }
            else if (what == "open") { out = addOpen ? "1" : "0"; }
            else if (what == "cell") { const auto& wm = ui->global<WorkspaceModel>(); out = fmt("%.0f", static_cast<double>(wm.get_cell_w())) + "x" + fmt("%.0f", static_cast<double>(wm.get_cell_h())); }
            else { return "workspace.layout|shown|page|pages|selected|radios|fell|cells|ticks.<n>|tuned.<n>|running.<n>|pic.<n>|peakoff.<n>|peakdb.<n>|master.<n>|audio.<n>|mute.<n>|vol.<n>|agc.<n>|nb.<n>|solo|chips|free|choices|addnote|removeask|open|cell, not " + name; }
            return std::string();
        }
        if (name == "bank") { out = std::to_string(ui->get_bank_index()); return std::string(); }
        if (name == "view") { out = std::to_string(ui->get_view_index()); return std::string(); }
        if (name == "pages") { out = publisher.pagesLine(); return std::string(); }
        if (name == "tick") { out = std::to_string(bounded.ticks() + 1); return std::string(); }
        return "no state named " + name;
    };
    readStateOf = readState;
    // The drive script's `radio add|remove` (input_script.hpp; a bounded run only): a GENERATOR radio opened or removed through the engine's real RADIO_ADD / RADIO_REMOVE, started running.
    const foxsdr::slintdrive::RadioHook radioHook = [&](const std::string& what, int n, double toneHz, double hz2) -> std::string {
        if (what == "range") {   // `radio range <n> <lo> <hi>`: a made-up tuning range for Link tuning (0 0 takes it away)
            if (n < 1 || n > kMaxRadios) { return "radio range takes a radio from 1 to 8"; }
            LinkRange& lr = testRanges[static_cast<std::size_t>(n - 1)];
            lr.known = hz2 > 0.0;
            lr.loHz = toneHz;
            lr.hiHz = hz2;
            return std::string();
        }
        if (what == "tune" || what == "mode" || what == "bw") {   // setup of a radio a script added: the ordinary op, aimed at that radio
            FoxCommand c = op(what == "tune" ? FOXAPI_OP_SET_FREQUENCY : what == "mode" ? FOXAPI_OP_SET_MODE : FOXAPI_OP_SET_BANDWIDTH);
            if (what == "mode") { c.ival[0] = static_cast<std::int64_t>(toneHz); } else { c.num[0] = toneHz; }
            submit(c, static_cast<std::uint32_t>(n));
            return std::string();
        }
        FoxCommand c = op(what == "add" ? FOXAPI_OP_RADIO_ADD : FOXAPI_OP_RADIO_REMOVE);
        c.ival[0] = n;
        if (what == "add") { c.ival[1] = 1; std::snprintf(c.text, sizeof(c.text), "generator"); c.num[0] = toneHz; }
        submit(c);
        return std::string();
    };
    // `radio addsrc <n> <id>`: RADIO_ADD with a source id (a file, an rtl_tcp or Pluto address, the sound card, a scanned device), started running; the engine's answer is kept in `workspace.lastadd`.
    const foxsdr::slintdrive::RadioSourceHook radioSourceHook = [&](int n, const std::string& source) -> std::string {
        FoxCommand c = op(FOXAPI_OP_RADIO_ADD);
        c.ival[0] = n; c.ival[1] = 1;
        std::snprintf(c.text, sizeof(c.text), "%s", source.c_str());
        submit(c);
        return std::string();
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
                driveTarget = std::make_unique<foxsdr::slintdrive::SlintDriveTarget<MainWindow>>(ui, readState, screenshotTo, radioHook, radioSourceHook);
                drive = std::make_unique<foxsdr::slintdrive::Driver>(std::move(script), *driveTarget);
            }
        }
    }
    const auto driveSay = [](const std::string& line) { std::fprintf(stderr, "foxsdr-slint: %s\n", line.c_str()); };

    // THE TICK = THE FRAME: the screen's reads, then the housekeeping (the sentinel's beat, the senders, the sweep, the update check), then the
    // update card if the duties changed it, then the bounded run's count. `--frames N` is N ticks and exit 0 (BoundedRun); a real session never
    // ends itself.
    std::optional<slint::PhysicalSize> wantedSize;   // FOXSDR_SLINT_SIZE, below
    // THE FRAME METER (frame_meter.hpp; B8): FOXSDR_SLINT_FRAME_LOG=1 in a bounded run measures the host's own 33 ms tick and, where the renderer allows it, each frame it finishes
    std::unique_ptr<foxsdr::slinthost::FrameMeter> frameMeter;
    if (std::getenv("FOXSDR_SLINT_FRAME_LOG") != nullptr && bounded.bounded()) {
        frameMeter = std::make_unique<foxsdr::slinthost::FrameMeter>();
        frameMeter->begin();
        const auto err = ui->window().set_rendering_notifier([&frameMeter](slint::RenderingState st, slint::GraphicsAPI) { if (st == slint::RenderingState::AfterRendering) { frameMeter->drawn(); } });
        frameMeter->setNotifier(!err.has_value());
        { const char* mode = std::getenv("FOXSDR_SLINT_FRAME_LOG"); frameMeter->setSnapshots(mode != nullptr && std::string(mode) == "2"); }   // 2 = also time a full render every fifth tick
    }
    slint::Timer tick;
    tick.start(slint::TimerMode::Repeated, std::chrono::milliseconds(33), [&] {
        const auto meterBegan = frameMeter ? frameMeter->tickStart() : foxsdr::slinthost::FrameMeter::Clock::time_point{};
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
        if (frameMeter) {
            frameMeter->tickEnd(meterBegan);
            if (frameMeter->wantsSnapshots() && bounded.ticks() % 5 == 0) {
                const auto t0 = foxsdr::slinthost::FrameMeter::Clock::now();
                (void)ui->window().take_snapshot();
                frameMeter->snapshotMs(std::chrono::duration<double, std::milli>(foxsdr::slinthost::FrameMeter::Clock::now() - t0).count());
            }
        }
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
    if (frameMeter) { std::fprintf(stderr, "foxsdr-slint: %s\n", frameMeter->summary().c_str()); }
    std::fprintf(stderr, "foxsdr-slint: %d frames painted, %d buffer detaches (0 = every frame reused its buffer)\n", selected().wells.frames, selected().wells.detaches);

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
        {   // the host's own tick time with the radios this run had open (state workspace.tickms), for the record of screen stage S4
            std::vector<double> v(tickMs.begin(), tickMs.end());
            if (v.size() >= 10) {
                std::sort(v.begin(), v.end());
                double sum = 0; for (const double x : v) { sum += x; }
                std::fprintf(stderr, "foxsdr-slint: bounded run: host tick time over the last %zu ticks: mean %.2f ms, p95 %.2f ms, worst %.2f ms, %d radios open\n", v.size(), sum / static_cast<double>(v.size()),
                             v[static_cast<std::size_t>(static_cast<double>(v.size() - 1) * 0.95)], v.back(), workspace.radioCount());
            }
        }
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", kitLine.c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", signalBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", systemBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", toolsBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", scopeBank.summaryLine().c_str());
        std::fprintf(stderr, "foxsdr-slint: bounded run: %s\n", view3dBank.summaryLine().c_str());
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
