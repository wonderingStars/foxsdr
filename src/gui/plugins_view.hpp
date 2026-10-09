// plugins_view.hpp - the FITTED MODULES window: the operating panel for the
// plugins this machine actually has.
//
// WHY IT IS SEPARATE FROM THE PLUGIN STORE. The store is a catalogue and answers
// "what could I have" - browse, search, fit. This window answers the other
// question, the one a user asks when something is wrong: "what have I got, and is
// it working". They are two windows rather than one section because a function
// that gets its own window gets a shape, and the rail row becomes the key that
// opens it rather than a lid over a drawer. Everything about a fitted module is in
// here.
//
// SIMPLE, BY THE OWNER'S WORDS (2026-10-08: "I want to keep separate windows but
// they both need to be simple"). A top bar - search, the six feed-state chips,
// SCAN AGAIN and RESET WINDOW SIZES - the one verdict line, and one full-width row
// per module: glyph, name, version, the reach warning or the refusal under the
// name, the state word with its lamp, STOP or START, and a two-step REMOVE. A
// click on a name opens the module's PAGE in the same window: an ON THIS MACHINE
// box, then - when the module is in the catalogue - the very pictures,
// description and notes the store's page draws (gui/plugin_store_view.hpp's
// drawModulePageBody, one renderer for both), and the facts folded behind SHOW
// DETAILS.
//
// THE STATE MODEL IS THE POINT OF THIS WINDOW. Before it, the Plugins section of
// the rail could say "loaded" and "stopped" and nothing else, which left the two
// most common faults unanswerable:
//
//   - "my decoder is installed and produces nothing" - because nothing is ROUTED
//     to it. The application knows this precisely (PluginRunner keeps a
//     DecoderStatus per instance with a ready-to-display sentence).
//
//   - "my plugin does not appear" - because the host FOUND the file, read its
//     descriptor and REFUSED it. LoadedPlugin::error has carried the exact reason
//     since the host was written, and it is printed here, verbatim.
//
// So this window distinguishes six states, and every one of them is derived from
// a predicate that already exists in the product rather than from a new opinion:
//
//   FED           loaded, not stopped, PluginRunner::isFeeding(key), and the
//                 receiver is running.
//   IDLE          loaded, not stopped, set to AUTO and DORMANT (0.99.73): nothing is
//                 using it, so it has no instance and costs nothing. It starts by
//                 itself when its window, map or output is opened. Not a fault.
//   NOT FED       loaded, not stopped, running, and something is between it and the
//                 samples. The reason is the runner's own sentence, quoted.
//                 (Lettered NOT DECODING, the word of the chip that counts it.)
//   TAKES NO      loaded, not stopped, and it declares no decoder at all. A
//   SIGNAL        basemap or a track source is fed nothing by design.
//   STOPPED       in the stop set. The user's own choice, lettered as a choice.
//   REFUSED       the file was found and rejected; `error` says why.
//
// PRECEDENCE, which is the order a module is asked in: REFUSED, STOPPED, TAKES NO
// SIGNAL, FED, IDLE, NOT DECODING.
//
// WHAT THIS WINDOW DOES NOT COVER, deliberately: the catalogue, held updates, and
// the RETIRED modules the version policy quarantines out of the scan. Those are
// the store's.
//
// PURE FIRST, DRAWN SECOND, which is the split track_detail_view.hpp uses and for
// the same reason. WHAT the window says - which state a module is in, what
// sentence explains it, what words its capability bits become - is decided by the
// free functions below, which have no ImGui in them and can be exercised without a
// graphics context. HOW it is drawn is the rest, and contains no decisions.
//
// GUI THREAD ONLY, like everything else in this directory.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PLUGINS_VIEW_HPP
#define CASCADE_GUI_PLUGINS_VIEW_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "core/plugin_host.hpp"
#include "gui/plugin_store_view.hpp"
#include "imgui.h"

namespace cascade::gui {

// The five distinct answers this application can give to "is my module working".
// Derived by fittedState() and by nothing else, so the word on the row, the lamp
// beside it and the sentence on the page cannot disagree.
enum class FittedState {
    Fed,        // being given signal right now
    NotFed,     // could be fed, and is not
    NoSignal,   // takes no signal by design - not a decoder
    Stopped,    // the user stopped it
    Refused,    // found on disk and rejected at load
    Idle,       // AUTO and dormant: not in use, so not running (0.99.73)
};

// One module as this window shows it. EVERY FIELD NAMES ITS SOURCE, because the
// whole value of this window is that nothing on it was invented here.
struct FittedModule {
    // core::pluginKey(p) - the module file name, which is the identity every
    // per-module decision in this product is keyed on (the stop, the tune grant,
    // the mute override). Never the display name, which the module itself chooses
    // and two modules may share.
    std::string file;
    // THE ROW'S NAME FOR THE CENSUS AND FOR THE PAGE'S PICTURES: the catalogue id
    // the install record gave this file when there is one, otherwise the file name.
    std::string id;
    // The catalogue id alone ("" for a module the catalogue does not know): what the
    // page asks the store for pictures with.
    std::string catalogueId;
    // The catalogue's category for the glyph ("aircraft", ...), or "" when the module
    // is not in the catalogue and the kind's glyph is drawn instead.
    std::string category;
    // LoadedPlugin::path - the absolute path the module was loaded FROM.
    std::string path;

    // LoadedPlugin descriptor fields, copied by the host. All empty on a module that
    // was refused before its descriptor could be read.
    std::string name;
    std::string version;
    std::string author;
    std::string licence;
    std::uint32_t capabilities = 0;

    bool loaded = false;   // LoadedPlugin::loaded
    // LoadedPlugin::error - empty if and only if loaded. Printed VERBATIM; this
    // string is the answer to "my plugin does not appear" and paraphrasing it would
    // throw away the numbers it carries ("expected 3, plugin reports 2").
    std::string error;

    // AppWindow::pluginIsStopped(file) - the durable stop set.
    bool stopped = false;
    // AppWindow::pluginIsIdle(file): the module is set to AUTO and is dormant - it has no
    // instance because nothing is using it (core/plugin_run.hpp). False for a module with
    // nothing to start (a basemap, say) and for one that is running.
    bool idle = false;
    // The module is set to ALWAYS ("keep running"): the tick on its page.
    bool keepRunning = false;
    // PluginRunner::isFeeding(file) - the runner has an instance for this module
    // MATCHED to the rate the pipeline is delivering.
    bool fed = false;
    // DecoderStatus::detail for this module when its reason is not Running - the
    // runner's own ready-to-display sentence, quoted rather than rewritten. Empty
    // when the runner recorded nothing.
    std::string idleDetail;

    // LoadedPlugin::hostClient != nullptr - the module declared CASCADE_CAP_HOST_CLIENT
    // and can therefore ASK to move the receiver.
    bool tuneCapable = false;
    // PluginUi::tuneAllowed(file). The one reach in this product that is actually
    // enforced: request_tune is answered CASCADE_TUNE_DENIED unless the user granted
    // it, per module, defaulting to off.
    bool tuneAllowed = false;

    // --- HOST API LEVEL 1 (0.99.31) -----------------------------------------
    // PluginApiCore::settingsRequesters() contains this file: the module has asked at
    // least once to change a receiver SETTING, so the SETTINGS grant means something
    // for it.
    bool settingsCapable = false;
    bool settingsAllowed = false;
    // The module's own command keys (CascadeHostApi::add_command), in the order it
    // added them. Labels are the module's own text, drawn as given.
    struct Command {
        std::uint32_t id = 0;
        std::string label;
    };
    std::vector<Command> commands;
    // The newest WARN/ERROR it logged (CascadeHostApi::log), or empty.
    std::uint32_t noticeLevel = 0;
    std::string notice;

    // PluginRepo::changedSinceInstallNote for this file: the bytes on disk are not the
    // ones sha256-verified when it was installed. Empty when they are, or when no
    // install record exists. The HOST's finding, never the module's.
    std::string integrityNote;

    // ORPHANED (0.99.69): core::classifyPluginFiles() says this file is not known to
    // the plugin index AND is not running. The row then says so and REMOVE deletes
    // that one file (AppWindow::removeOrphanedPlugin re-checks before it does).
    bool orphaned = false;

    // Size of the file on disk in bytes. 0 means NOT MEASURED; it never prints a
    // clean zero, which would be the opposite claim.
    std::uint64_t sizeBytes = 0;

    // From the install record (installed.json), when there is one: the plugin ABI the
    // fitted build was made for (0 = not recorded) and when it was fitted (0 = not
    // recorded, drawn as nothing and never as 1970).
    std::uint32_t abiVersion = 0;
    std::int64_t fittedAtUnix = 0;

    // The module's page needs the catalogue's record of it (pictures, description,
    // what is new) when the catalogue knows it: an index into
    // FittedModulesModel::catalogue, or -1. Filled only while this module's page is
    // open.
    int catalogueIndex = -1;
};

// The record as the SHARED page wants it, for a module the catalogue does not know
// (or for the facts of one it does). One adapter, so the fitted window and the store
// cannot describe the same module differently.
//
// The catalogue-only fields are left absent rather than guessed: a LoadedPlugin
// carries no summary, no homepage, no legal notice, no platform list, no retirement
// floor. haveAbi follows the install record's ABI when there is one.
//
// AND NEITHER ARE THE IDENTITY FIELDS OF A MODULE NOBODY READ. PluginHost copies
// name, version, author and licence out of the descriptor only after it accepts one,
// so a file refused before that arrives here with all four empty -
// ModulePlate::haveDescriptor says which of those two this record is, and it is
// decided by the record itself rather than by `loaded`.
ModulePlate makeModulePlate(const FittedModule& m);

// What the window is told, once per frame.
struct FittedModulesModel {
    std::vector<FittedModule> modules;  // PluginHost::plugins() order
    std::string directory;              // PluginHost::directory()
    // Pipeline::running(). Gates FED for every module at once, which is why it is
    // stated on the window rather than left to be inferred from four idle rows.
    bool receiverRunning = false;
    // AppWindow's last install/remove outcome, verbatim. Either may be empty.
    std::string report;
    std::string error;
    // THE OLD COPIES UPDATES LEFT BEHIND (core/plugin_cleanup.hpp), and what the last
    // clean-up did: "CLEAN UP OLD VERSIONS (n)" is at the foot of the list when there
    // are any.
    std::vector<StoreOldCopy> oldCopies;
    std::string cleanupReport;
    // The catalogue's records of the modules a page needs (see
    // FittedModule::catalogueIndex): empty while no page is open.
    std::vector<StoreModule> catalogue;
};

// The window's own persistent state, owned by the caller so it survives the frame.
struct FittedModulesDeck {
    char search[128] = {0};
    // The six chips, each a toggle, all on by default.
    bool showFed = true;
    bool showIdle = true;      // NotFed only - see showNoSignal
    // ITS OWN KEY, because it is its own state: one key over both NotFed and NoSignal
    // was labelled NOT DECODING, which made a basemap - a module that can never decode
    // anything - a decoder that is not decoding.
    bool showNoSignal = true;
    bool showStopped = true;
    bool showRefused = true;
    // The IDLE chip (0.99.73): modules that are dormant because nothing is using them.
    bool showIdleDormant = true;
    // The module file name awaiting a second press (CONFIRM), and the ImGui time the
    // arming lapses (five seconds). A file name rather than an index for the reason
    // the whole product keys on file names: a rescan reorders the list.
    std::string confirmRemove;
    double confirmUntil = 0.0;
    // The module file of the open page ("" = the list), and the page's own state.
    std::string pageFile;
    ModulePageState page;
    // The catalogue id of the page's module, set on the frame the page OPENS so the
    // caller asks for its pictures; the caller clears it.
    std::string pageOpenedCatalogueId;
    std::string lastPageFile;
    // The decoded pictures of the page, kept for the window's life.
    PagePictureCache pictures;
};

// What the frame's clicks asked for. The window itself changes nothing: it owns no
// host, no runner and no config, and every action here is one the application
// already has a method for.
struct FittedModulesAction {
    enum class Kind {
        None,
        Rescan,    // AppWindow::rescanPlugins()
        Start,     // AppWindow::setPluginStopped(file, false): the module is set to ALWAYS
        Stop,      // AppWindow::setPluginStopped(file, true)
        // The "keep running" tick on the module's page: AppWindow::setPluginRun(file,
        // flag ? ALWAYS : AUTO). `flag` is the state being asked for.
        SetKeepRunning,
        Remove,    // AppWindow::removeInstalledPlugin(file)
        // AppWindow::removeOrphanedPlugin(file) (0.99.69): the same two-step key on a
        // file that is not in the plugin index and not running - and the app re-checks
        // that before it deletes anything.
        RemoveOrphan,
        SetTune,   // AppWindow::setPluginTuneAllowed(file, flag)
        // Host API level 1: AppWindow::setPluginSettingsAllowed(file, flag), and a
        // press of one of the module's own command keys (id).
        SetSettings,
        Command,
        // AppWindow::resetPageWindows(). THE WAY BACK FROM A WINDOW DRAGGED TOO SMALL
        // TO USE: a page's resize grip is invisible by design, so a decoder window
        // pulled down to its rail leaves almost nothing to take hold of.
        ResetWindows,
        // CLEAN UP OLD VERSIONS, confirmed once for all of them
        // (AppWindow::cleanUpOldVersionsConfirmed).
        CleanUp,
    };
    Kind kind = Kind::None;
    std::string file;   // empty for Rescan
    bool flag = false;  // SetTune/SetSettings: the grant being asked for
    std::uint32_t commandId = 0;  // Command: which of the module's keys
};

// --- what the window SAYS, decided without ImGui -----------------------------

// The state of one module. `receiverRunning` is the pipeline's own run state: a
// decoder matched to a rate nobody is producing is not being fed.
FittedState fittedState(const FittedModule& m, bool receiverRunning);

// The word printed on the row, in capitals. Never null.
const char* fittedStateWord(FittedState s);

// WHICH KEY THE ROW OFFERS (0.99.73): START for a module that is not running - stopped, or idle -
// and STOP for one that is. The label is the ACTION, never the state, so a key reading "RUNNING"
// can never leave anyone guessing whether pressing it stops the module. START on an idle module
// pins it ("keep running"), the same thing a preset press does.
bool fittedOffersStart(const FittedModule& m);
const char* fittedKeyWord(const FittedModule& m);

// The sentence on the page: why the module is in that state, and what would change
// it. For NotFed this is the RUNNER'S OWN sentence wherever it recorded one, quoted
// rather than rewritten.
std::string fittedStateSentence(const FittedModule& m, bool receiverRunning);

// The ONE LINE an orphaned file's row carries (0.99.69): that it was not installed
// from the plugin store and is not running, then the host's own reason for the second
// half, verbatim, when it gave one. Empty for a file that is not an orphan.
std::string fittedOrphanSentence(const FittedModule& m);

// THE LINE UNDER A ROW'S NAME, or empty: an orphan's sentence, or a REFUSED module's
// one-line reason (the host's, verbatim), or - for a module that reaches outward - the
// reach warning ("asks to move the receiver", "may fetch from a server it chose").
// Nothing for a module that "publishes to the host only". `refusal` says the line is a
// refusal (letter it muted), `warning` that it is a reach warning (letter it in amber).
struct FittedRowNote {
    std::string text;
    bool refusal = false;
    bool warning = false;
};
FittedRowNote fittedRowNote(const FittedModule& m, bool receiverRunning);

// How many modules are in each state. The counts on the chips come from here so they
// cannot drift from the rows. ONE COUNTER PER STATE: nothing is added together on the
// way to the chips.
struct FittedCounts {
    int fed = 0;
    int idle = 0;      // Idle: dormant, nothing is using it
    int notFed = 0;    // NotFed: could be fed, and is not
    int noSignal = 0;  // NoSignal: takes no signal by design
    int stopped = 0;
    int refused = 0;
    int total = 0;
};
FittedCounts countStates(const std::vector<FittedModule>& modules, bool receiverRunning);

// The one muted verdict line under the top bar: the receiver's run state in one
// sentence ("The receiver is running, so a module with a matched decoder is being
// fed."), then, after a dot, "read from <plugins dir>" - or "No directory has been
// scanned yet." when none has been.
std::string fittedVerdictLine(bool receiverRunning, const std::string& directory);

// Which modules the chips leave on screen, and in what order: sorted by name (the file
// name for a module whose name was never read), filtered by the chips and by `lowerQuery`
// over name, file and version. Indices into `modules`.
std::vector<int> fittedVisibleRows(const std::vector<FittedModule>& modules, bool receiverRunning,
                                   const FittedModulesDeck& deck);

// "fitted 2026-10-07" for a module whose install record says when, else empty.
std::string fittedDateText(const FittedModule& m);

// Builds one record from the three places the application keeps these facts, so a
// caller cannot pair the wrong predicate with the wrong field. sizeBytes is the
// record's (LoadedPlugin::fileBytes, measured once by the scan); 0 is "not measured".
// NOTHING HERE OR IN THE CALLER MAY STAT THE FILE: this is built for every module on
// every frame the window is open (tests/test_fitted_modules_no_disk).
// `idle` and `keepRunning` (0.99.73) default to false - a module that is running and set to
// AUTO - so a caller that knows nothing of run states gets what it always got.
FittedModule makeFittedModule(const cascade::core::LoadedPlugin& p, bool stopped, bool fed,
                              std::string idleDetail, bool tuneAllowed, bool idle = false,
                              bool keepRunning = false);

// --- the window --------------------------------------------------------------

// Draws the whole panel into the CURRENT ImGui window and returns whatever the user
// asked for this frame. The caller opens the window, places it and applies the action;
// nothing here touches the host.
FittedModulesAction drawFittedModulesPanel(FittedModulesDeck& deck, const FittedModulesModel& model);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGINS_VIEW_HPP
