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

#include <cstddef>
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

// THE WINDOW A MODULE OPENS WHEN ITS ROW IS DOUBLE-CLICKED (0.99.77), in the order a plugin with
// several is asked: its panel, else its picture window, else its instrument, else its map page. The
// owner (2026-10-13): "when you open the plugins Fitted modules window, when I double click on the
// plugin it should show the plugin window". None is a module with no window of its own (a text
// decoder is read in the shared Decoder output window, a basemap draws nothing): its row opens its
// module page, which is what a single click on its name does.
enum class FittedOwnWindow { None, Panel, Picture, Instrument, Map };

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
    // --- HOST API LEVEL 2 (0.99.74): the TRANSMIT grant -----------------------------
    // The runner has seen this module's active() non-zero at least once, so the grant
    // means something for it: the key is offered only then, as SETTINGS is, and a module
    // that has never wanted the transmitter never has a TRANSMIT key on its plate.
    bool txCapable = false;
    bool txAllowed = false;
    // What the plate says about its transmit state (TxPlateState as an int, so this header
    // needs nothing from the page's); NotAsked prints nothing.
    int txPlate = 0;
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

    // WHERE IT WAS INSTALLED FROM, when that was a plugin site rather than the FoxSDR
    // catalogue (0.99.74; from the install record's `source`): "Installed from <host>, not
    // from the FoxSDR catalogue.", or - for a site the user has since removed - that it is
    // from a site they removed and gets no updates. One muted line on the module's row. Empty
    // for every other module.
    std::string originNote;

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

    // Which window a double-click on the row opens (0.99.77): from the TABLE POINTERS the host
    // kept (fittedOwnWindowOf), never from the declared bits, for the reason tuneCapable is.
    FittedOwnWindow ownWindow = FittedOwnWindow::None;
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

// What the window is told: the window's cache of the engine's table (FittedListCache, below), built when
// listSeq moves, with the receiver's run state and the install sentence set on it every frame.
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
    // THE FIRST CLICK OF A DOUBLE-CLICK ON A ROW'S NAME OR PICTURE (0.99.77). That click opens the
    // module's page, as it always has, so the second press of the pair lands on the page and not on
    // the row: the page reads it (a double-click within the double-click time of this click) and
    // hands the user the module's window in place of the page. The module file the click opened, and
    // the ImGui time it did.
    std::string rowOpenedFile;
    double rowOpenedAt = 0.0;
    // SET WHEN THE SECOND PRESS HAS BEEN TAKEN AS THE DOUBLE-CLICK, until that press is released: the list
    // is drawn again in the same frame, with the button still down over the name that opened the page,
    // and its release must not open the page a second time.
    bool swallowRowClick = false;
    // The decoded pictures of the page, kept for the window's life.
    PagePictureCache pictures;
};

// What the frame's clicks asked for. The window itself changes nothing: it
// owns no host, no runner and no config. AppWindow::drawFittedModulesWindow sends each
// action as a command on its engine API session (window port, group 9): the op
// named beside it below, which the engine applies at its next pass.
struct FittedModulesAction {
    enum class Kind {
        None,
        Rescan,    // FOXAPI_OP_PLUGIN_RESCAN
        Start,     // FOXAPI_OP_DECODER_START (the file)
        Stop,      // FOXAPI_OP_DECODER_STOP (the file)
        // The "keep running" tick on the module's page (0.99.73): AppWindow::setPluginRun(file,
        // flag ? ALWAYS : AUTO). `flag` is the state being asked for.
        // FIXME(2026-10-21 merge, ledger PLUGIN-RUN): no engine op carries it yet; the window never emits it.
        SetKeepRunning,
        Remove,    // FOXAPI_OP_STORE_REMOVE (the file)
        // AppWindow::removeOrphanedPlugin(file) (0.99.69): the same two-step key on a file that is
        // not in the plugin index and not running - and the app re-checks that before it deletes
        // anything. FIXME(2026-10-21 merge, ledger PLUGIN-ORPHAN): no engine op yet; never emitted.
        RemoveOrphan,
        SetTune,   // FOXAPI_OP_PLUGIN_GRANT, slot 1 (the file, the flag)
        // Host API level 1: FOXAPI_OP_PLUGIN_GRANT, slot 2 (the file, the flag),
        // and a press of one of the module's own command keys: FOXAPI_OP_PLUGIN_COMMAND (the file, the id).
        SetSettings,
        // Host API level 2: AppWindow::setPluginTransmitAllowed(file, flag).
        SetTransmit,
        Command,
        // AppWindow::resetPageWindows() (the window's own: no command). THE WAY BACK FROM A WINDOW DRAGGED
        // TOO SMALL TO USE. A page's resize grip is invisible by design, so a
        // decoder window pulled down to its rail leaves almost nothing to take
        // hold of - reported by a beta tester who could not get the ACARS
        // printer back. It is on THIS window because it is the one panel that
        // is about every fitted module at once, and because it is reachable
        // from the rail whatever state the other windows are in.
        ResetWindows,
        // CLEAN UP OLD VERSIONS, confirmed once for all of them
        // (AppWindow::cleanUpOldVersionsConfirmed).
        CleanUp,
        // A DOUBLE-CLICK ON A ROW (0.99.77): AppWindow::openPluginOwnWindow(file) shows the module's own
        // window - its panel, picture, instrument or map page, in that order - and the plugin lifecycle
        // wakes it. Only ever asked for a module that fittedOpensOwnWindow() says has one.
        OpenWindow,
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

// Which modules the chips leave on screen, and in what order: sorted by the store's group
// (fittedGroupOf) and then by name (the file name for a module whose name was never read),
// filtered by the chips and by `lowerQuery` over name, file and version. Indices into
// `modules`. With no category anywhere every module is in one group, so this is the name
// sort it was before 0.99.77.
std::vector<int> fittedVisibleRows(const std::vector<FittedModule>& modules, bool receiverRunning,
                                   const FittedModulesDeck& deck);

// THE LIST UNDER THE STORE'S HEADINGS (0.99.77): a US tester, through the owner (2026-10-13), "I really
// like how the store groups plugins by function ... Implement this sorting on the Plugins panel ...
// if you install them all, the list is quite long, intermixed and kind-of alphabetical." So the
// rows above are ordered by the store's group first (AIRCRAFT, MARINE, SATELLITES AND WEATHER, METERS
// AND PAGING, VOICE AND DATA, MAPS AND TOOLS, OTHER) and by name inside it. The group is the store's
// own answer for the module's catalogue category (FittedModule::category); a module the catalogue does
// not know, or one whose category this build does not, is OTHER, and sorts last.
StoreCategory fittedGroupOf(const FittedModule& m);
// Whether the list carries headings: at least one fitted module has a catalogue category. A fresh
// install that never read or kept a catalogue has none, and its list is the flat name-sorted list it
// always was. A fact of the WHOLE list and not of what the chips and the search leave on screen, so
// headings do not come and go as someone types.
bool fittedIsGrouped(const std::vector<FittedModule>& modules);
// Where the headings go: for each position in `rows` (as fittedVisibleRows returns them) whose group
// differs from the row before it - the first row included - the position and the group. Empty when
// the list is not grouped, and when no row is left.
struct FittedGroupBreak {
    std::size_t at = 0;
    StoreCategory group = StoreCategory::Other;
};
std::vector<FittedGroupBreak> fittedGroupBreaks(const std::vector<FittedModule>& modules,
                                                const std::vector<int>& rows);

// The window a double-click on this module's row opens, from the table pointers the host kept: the
// panel, else the picture window, else the instrument, else the map page, else None. A module that is
// not loaded has none.
FittedOwnWindow fittedOwnWindowOf(const cascade::core::LoadedPlugin& p);
// Whether the double-click opens a WINDOW for this row: loaded, has one, and is not stopped. A stopped
// plugin has no instances, so no window of its could draw (the rail lists no window rows for one, and
// a stopped tracker has no map page); its double-click opens its module page, which says STOPPED and
// offers START.
bool fittedOpensOwnWindow(const FittedModule& m);

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

// --- the window's own copy of the engine's table (window port, group 9) -------
//
// What ONE module is, as the engine's table says it: its PLUGINS entry (the display name, the file in
// `detail`, the capability word, the file's size, the level of its newest notice, and the flags) and its
// per-file facts (API 0.3 section 8.8: whole texts, "" when there is none). The window reads these when
// listSeq moves and nowhere else; makeFittedModule's inputs - a LoadedPlugin, the stop set, the runner,
// the grants - are the engine's own, and a window that reads only the table has none of them.
struct FittedRow {
    std::string file;      // PLUGINS `detail`: the module file name (the key everything is filed under)
    std::string name;      // PLUGINS `name`: the display name, or the file name when no descriptor was read
    std::uint32_t caps = 0;           // value[0]: the capability word as declared (0 = no descriptor read)
    std::uint64_t sizeBytes = 0;      // value[1]: the file's size, measured by the engine; 0 = not measured
    std::uint32_t noticeLevel = 0;    // value[2]: 0 none, 2 warning, 3 error
    bool loaded = false;              // ACTIVE
    bool stopped = false;             // STOPPED
    bool tuneAllowed = false;         // TUNE_ALLOWED
    bool settingsAllowed = false;     // SETTINGS_ALLOWED
    bool matched = false;             // MATCHED: the runner would feed it, the receiver running or not
    bool askedSettings = false;       // ASKED_SETTINGS
    std::string path, version, author, licence, error, idle, notice, integrity;  // fact.plugin.<file>.*
};

// A module from its row. Everything FittedModule names a source for is taken from the row's one answer for
// it, and the host-client key (`tuneCapable`) from the capability word of a LOADED module - the host refuses
// a module that declares the bit with no usable table, and the key is offered only to a loaded one. A file
// refused before a descriptor was read has a caps word of 0 and a `name` the table filled with the file's
// own name: `m.name` is empty then, so makeModulePlate's "no descriptor read" test is unchanged. The command
// keys are NOT in the row (the table has them as a list of their own): m.commands is left empty.
FittedModule makeFittedModuleFromRow(const FittedRow& r);
// The same, written into an existing record so its strings keep their room (the cache's rebuild).
void fillFittedModule(FittedModule& m, const FittedRow& r);

// What the window keeps of the table (AppWindow::fittedLists): the whole model, built when listSeq moves and
// kept, so a frame in which nothing changed builds nothing - the receiver's run state, and the install
// report and error, are set on it once a frame (they are not the lists' and the last one depends on whether
// the store window drew). `refreshes` is read by a test.
struct FittedListCache {
    std::uint64_t listSeq = 0;         // 0: never read
    std::uint32_t refreshes = 0;
    FittedModulesModel model;
    FittedRow row;                     // scratch: one module's row, its strings keeping their room
    std::string key;                   // scratch: a fact's key
};

// --- the window --------------------------------------------------------------

// Draws the whole panel into the CURRENT ImGui window and returns whatever the user
// asked for this frame. The caller opens the window, places it and applies the action;
// nothing here touches the host.
FittedModulesAction drawFittedModulesPanel(FittedModulesDeck& deck, const FittedModulesModel& model);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGINS_VIEW_HPP
