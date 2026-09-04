// plugins_view.hpp - the FITTED MODULES window: the operating panel for the
// plugins this machine actually has.
//
// WHY IT IS SEPARATE FROM THE PLUGIN STORE. The store is a catalogue and
// answers "what could I have" - browse, search, filter, fit. This window
// answers the other question, which is the one a user asks when something is
// wrong: "what have I got, and is it working". They are two windows rather
// than one section because a function that gets its own window gets a shape,
// and the rail row becomes the key that opens it rather than a lid over a
// drawer. Everything about a fitted module is in here.
//
// THE STATE MODEL IS THE POINT OF THIS WINDOW. Before it, the Plugins section
// of the rail could say "loaded" and "stopped" and nothing else, which left
// the two most common faults unanswerable:
//
//   - "my decoder is installed and produces nothing" - because nothing is
//     ROUTED to it. The application knows this precisely (PluginRunner keeps a
//     DecoderStatus per instance with a ready-to-display sentence) and the
//     rail showed the sentence in a place nobody associates with the module.
//
//   - "my plugin does not appear" - because the host FOUND the file, read its
//     descriptor and REFUSED it. LoadedPlugin::error has carried the exact
//     reason since the host was written - wrong ABI version, a missing entry
//     point, a declared capability with no table behind it - and no surface in
//     this product ever printed it. It is printed here, verbatim.
//
// So this window distinguishes five states, and every one of them is derived
// from a predicate that already exists in the product rather than from a new
// opinion invented here:
//
//   FED           loaded, not stopped, PluginRunner::isFeeding(key), and the
//                 receiver is running. This is the same composition
//                 core/plugin_ui.hpp calls ACTUALLY DECODING and the audio
//                 mute already acts on, with the receiver's own run state
//                 added - a decoder matched to a rate nobody is producing is
//                 not being fed, whatever the runner's table says.
//   NOT FED       loaded, not stopped, and something is between it and the
//                 samples. The reason is the runner's own sentence, quoted.
//   TAKES NO      loaded, not stopped, and it declares no decoder at all.
//   SIGNAL        A basemap or a track source is fed nothing by design, and
//                 calling that "not fed" would put a fault on a module doing
//                 exactly what it was fitted to do.
//   STOPPED       in the stop set. The user's own choice, so it is lettered as
//                 a choice and not as trouble.
//   REFUSED       the file was found and rejected; `error` says why.
//
// WHAT THIS WINDOW DOES NOT COVER, deliberately: the catalogue, held updates,
// and the RETIRED modules the version policy quarantines out of the scan.
// Those are the store's - a retired module is a fact about what the catalogue
// now says, not about what the host loaded, and PluginHost never sees it.
//
// THE DATA PLATE IS NOT DRAWN HERE. The design makes it a component SHARED
// with the plugin store so one module reads identically in both windows, and
// it lives in gui/plugin_store_view.hpp: ModulePlate, moduleDataPlateHeight,
// drawModuleDataPlate, moduleKindTag, moduleReachSummary, moduleReachColour.
// This window builds a ModulePlate from its own record (makeModulePlate) and
// hands it over. The identity, the facts, the reach panel and the refusal
// reason are all that component's; what this file adds around it is the
// operating state, the path the module was loaded from, and the keys.
//
// PURE FIRST, DRAWN SECOND, which is the split track_detail_view.hpp uses and
// for the same reason. WHAT the window says - which state a module is in, what
// sentence explains it, what words its capability bits become - is decided by
// the free functions below, which have no ImGui in them and can be exercised
// without a graphics context. HOW it is drawn is the rest, and contains no
// decisions.
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

// The five distinct answers this application can give to "is my module
// working". Derived by fittedState() and by nothing else, so the word on the
// row, the lamp beside it and the sentence on the plate cannot disagree.
enum class FittedState {
    Fed,        // being given signal right now
    NotFed,     // could be fed, and is not
    NoSignal,   // takes no signal by design - not a decoder
    Stopped,    // the user stopped it
    Refused,    // found on disk and rejected at load
};

// One module as this window shows it. EVERY FIELD NAMES ITS SOURCE, because
// the whole value of this window is that nothing on it was invented here.
struct FittedModule {
    // core::pluginKey(p) - the module file name, which is the identity every
    // per-module decision in this product is keyed on (the stop, the tune
    // grant, the mute override). Never the display name, which the module
    // itself chooses and two modules may share.
    std::string file;
    // LoadedPlugin::path - the absolute path the module was loaded FROM.
    // "Which copy is running" is the other question with no answer today: a
    // plugin file name embeds its version, so an upgrade ADDS a file, and the
    // host's duplicate resolver turns the loser off with a reason.
    std::string path;

    // LoadedPlugin descriptor fields, copied by the host. All empty on a
    // module that was refused before its descriptor could be read.
    std::string name;
    std::string version;
    std::string author;
    std::string licence;
    std::uint32_t capabilities = 0;

    bool loaded = false;   // LoadedPlugin::loaded
    // LoadedPlugin::error - empty if and only if loaded. Printed VERBATIM;
    // this string is the answer to "my plugin does not appear" and paraphrasing
    // it would throw away the numbers it carries ("expected 3, plugin reports 2").
    std::string error;

    // AppWindow::pluginIsStopped(file) - the durable stop set.
    bool stopped = false;
    // PluginRunner::isFeeding(file) - the runner has an instance for this
    // module MATCHED to the rate the pipeline is delivering. Not the same
    // question as "is it stopped", and not the same question as "is the
    // receiver running" either; see FittedState above.
    bool fed = false;
    // DecoderStatus::detail for this module when its reason is not Running -
    // the runner's own ready-to-display sentence, quoted rather than rewritten
    // so this window and the rail cannot describe one idle decoder two ways.
    // Empty when the runner recorded nothing.
    std::string idleDetail;

    // LoadedPlugin::hostClient != nullptr - the module declared
    // CASCADE_CAP_HOST_CLIENT and can therefore ASK to move the receiver.
    bool tuneCapable = false;
    // PluginUi::tuneAllowed(file). The one reach in this product that is
    // actually enforced: request_tune is answered CASCADE_TUNE_DENIED unless
    // the user granted it, per module, defaulting to off.
    bool tuneAllowed = false;

    // Size of the file on disk in bytes. 0 means NOT MEASURED and the shared
    // plate says so; it never prints a clean zero, which would be the opposite
    // claim. There is no size in any descriptor, so this can only ever come
    // from the caller stat-ing the file.
    //
    // NO FITTED DATE. InstalledPlugin::installedAtUnix does record one, but
    // the shared plate has no field for it, and a fact drawn only in this
    // window would break the one property the shared plate exists for - that a
    // module reads identically in both.
    std::uint64_t sizeBytes = 0;
};

// The record as the SHARED data plate wants it. One adapter, so the fitted
// window and the store cannot describe the same module differently.
//
// The catalogue-only fields are left absent rather than guessed: a LoadedPlugin
// carries no summary, no homepage, no legal notice, no platform list, no
// retirement floor, and - the one that matters - no ABI version, because the
// host checks the descriptor's abiVersion at load and does not copy it into the
// record. haveAbi is therefore FALSE, which the plate reads as "not recorded"
// and never as a mismatch.
//
// AND NEITHER ARE THE IDENTITY FIELDS OF A MODULE NOBODY READ. PluginHost
// copies name, version, author and licence out of the descriptor only after it
// accepts one, so a file refused before that arrives here with all four empty
// - and the plate would have drawn a maker, a version and "no licence
// declared" for a module it had never opened. ModulePlate::haveDescriptor says
// which of those two this record is, and it is decided by the record itself
// rather than by `loaded`: the host's duplicate resolver turns a module off
// AFTER reading it, and that one is refused with its identity perfectly known.
ModulePlate makeModulePlate(const FittedModule& m);

// What the window is told, once per frame.
struct FittedModulesModel {
    std::vector<FittedModule> modules;  // PluginHost::plugins() order
    std::string directory;              // PluginHost::directory()
    // Pipeline::running(). Gates FED for every module at once, which is why it
    // is stated on the panel rather than left to be inferred from four idle
    // rows.
    bool receiverRunning = false;
    // AppWindow's last install/remove outcome, verbatim. Either may be empty.
    std::string report;
    std::string error;
};

// The window's own persistent state, owned by the caller so it survives the
// frame. Same arrangement as MapView's SatelliteDeck.
struct FittedModulesDeck {
    // Index into the FILTERED list, clamped every frame - the model's vector
    // is rebuilt by every rescan and an index into the old one is a different
    // module.
    int selected = 0;
    bool showFed = true;
    bool showIdle = true;      // NotFed only - see showNoSignal
    // ITS OWN KEY, because it is its own state. One key over both NotFed and
    // NoSignal was labelled NOT DECODING, which made a basemap - a module that
    // can never decode anything - a decoder that is not decoding.
    bool showNoSignal = true;
    bool showStopped = true;
    bool showRefused = true;
    // The module file name awaiting a second press of Remove. Empty when
    // nothing is armed. A file name rather than an index for the reason the
    // whole product keys on file names: a rescan reorders the list.
    std::string confirmRemove;
};

// What the frame's clicks asked for. The window itself changes nothing: it
// owns no host, no runner and no config, and every action here is one the
// application already has a method for.
struct FittedModulesAction {
    enum class Kind {
        None,
        Rescan,    // AppWindow::rescanPlugins()
        Start,     // AppWindow::setPluginStopped(file, false)
        Stop,      // AppWindow::setPluginStopped(file, true)
        Remove,    // AppWindow::removeInstalledPlugin(file)
        SetTune,   // AppWindow::setPluginTuneAllowed(file, flag)
    };
    Kind kind = Kind::None;
    std::string file;   // empty for Rescan
    bool flag = false;  // SetTune: the grant being asked for
};

// --- what the window SAYS, decided without ImGui -----------------------------

// The state of one module. `receiverRunning` is the pipeline's own run state:
// a decoder matched to a rate nobody is producing is not being fed.
FittedState fittedState(const FittedModule& m, bool receiverRunning);

// The word printed on the row, in capitals. Never null.
const char* fittedStateWord(FittedState s);

// The sentence on the plate: why the module is in that state, and what would
// change it. For NotFed this is the RUNNER'S OWN sentence wherever it recorded
// one, quoted rather than rewritten.
std::string fittedStateSentence(const FittedModule& m, bool receiverRunning);

// The kind tag and the one-line reach summary on a row come from the SHARED
// component - moduleKindTag(), moduleReachSummary() and moduleReachColour() in
// gui/plugin_store_view.hpp, called on the ModulePlate this window builds. They
// are deliberately not reimplemented here: a module that reads "DECODER" in the
// store and "TRACKS" on this panel would be one module described two ways.

// How many modules are in each state. The counts on the strip come from here
// so they cannot drift from the rows.
//
// ONE COUNTER PER STATE, and NoSignal is not folded into notFed any more. It
// was, under a caption reading NOT DECODING, which counted a basemap - a
// module that declares no decoder and can never decode anything - as a decoder
// that is not decoding, and lit a lamp over the total. A count must answer the
// question its caption asks; these five are the five states fittedState()
// returns and nothing is added together on the way to the strip.
struct FittedCounts {
    int fed = 0;
    int notFed = 0;    // NotFed: could be fed, and is not
    int noSignal = 0;  // NoSignal: takes no signal by design
    int stopped = 0;
    int refused = 0;
    int total = 0;
};
FittedCounts countStates(const std::vector<FittedModule>& modules, bool receiverRunning);

// Builds one record from the three places the application keeps these facts,
// so a caller cannot pair the wrong predicate with the wrong field. sizeBytes
// is left at 0 - "not measured" - for the caller to fill in if it stats the
// file; a size nobody looked up must never be drawn as a clean zero.
FittedModule makeFittedModule(const cascade::core::LoadedPlugin& p, bool stopped, bool fed,
                              std::string idleDetail, bool tuneAllowed);

// --- the window --------------------------------------------------------------

// Draws the whole panel into the CURRENT ImGui window and returns whatever the
// user asked for this frame. The caller opens the window, places it and applies
// the action; nothing here touches the host.
FittedModulesAction drawFittedModulesPanel(FittedModulesDeck& deck,
                                           const FittedModulesModel& model);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGINS_VIEW_HPP
