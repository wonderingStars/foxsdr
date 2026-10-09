// plugin_run.hpp - WHEN A FITTED PLUGIN RUNS: the three run states, the one pure rule that
// says a plugin is WANTED, and the 30-second clock that lets it go dormant.
//
// THE FAULT THIS EXISTS TO END (0.99.73, report 12CF: a Store user on 0.99.64, an RSP1A at
// 2.048 MS/s, 30 plugins fitted, 110 to 126 starved audio callbacks a minute and frames of
// 260 to 320 ms). Fitting a plugin used to create its decoder at once, and from then on every
// DSP block fed it and every GUI frame polled it, window or no window. Thirty fitted plugins
// were thirty decoders and thirty polls on every block and every frame, whether the user was
// looking at one of them or not.
//
// THREE RUN STATES, per plugin, persisted by the version-stripped plugin id (pluginRunId) so
// that an update - which changes the file name - keeps the decision:
//
//   AUTO     the default for every plugin, new and existing. It runs while it is WANTED
//            (pluginWanted) and is dormant otherwise: no instance, no samples, no polling.
//   ALWAYS   the user pinned it ("keep running", or a START press, or a preset press): it
//            runs as every plugin used to.
//   STOPPED  the user stopped it: no instance anywhere, as before.
//
// WANTED is a pure function of the signals below - what is on the screen or wired to something
// that reads the plugin's output - computed once per frame on the GUI thread, and NOTHING ELSE
// counts. Coverage accumulation, the web snapshot and aircraft-info lookups are passive
// consumers: a user who wants background feeding ticks "keep running".
//
// PURE, AND OUT OF app_window.cpp, so tests/test_plugin_run.cpp can drive the whole table
// without a window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_RUN_HPP
#define CASCADE_CORE_PLUGIN_RUN_HPP

#include <cstdint>
#include <string>

namespace cascade::core {

enum class PluginRun {
    Auto = 0,
    Always,
    Stopped,
};

// The words the config file carries: "auto", "always", "stopped". Never null.
const char* pluginRunWord(PluginRun r);
// The inverse. False, and `out` untouched, for any other word (the loader drops such an entry).
bool parsePluginRun(const std::string& word, PluginRun& out);

// THE PLUGIN'S ID, version-stripped: the key a run state is persisted under.
//   "pocsag-decoder-1.0.2-abi3-win-x64.dll" -> "pocsag-decoder"
//   "my-decoder.dll"                         -> "my-decoder"
//   "pocsag-decoder" (already an id)         -> "pocsag-decoder"
//   ""                                       -> ""
// A file name is recognised by its module extension (.dll, .so, .dylib), and a name that
// carries none is taken to be an id already, so applying this twice changes nothing - which a
// plain "strip the last dot" would not guarantee for an id that contains a dot. A path is
// accepted; only its last component is used. An empty id matches nothing, anywhere.
std::string pluginRunId(const std::string& moduleFileOrId);

// HOW LONG A PLUGIN KEEPS RUNNING AFTER ITS LAST WANT DISAPPEARS: long enough that opening and
// closing a window is not a create and a destroy each time, short enough that a plugin the
// user has finished with stops costing the machine something within the minute.
inline constexpr std::int64_t kPluginDormantAfterMs = 30000;

// THE SIGNALS THAT MAKE A PLUGIN WANTED, and no others. Each is a fact about the screen or the
// patch, read on the GUI thread.
struct PluginUse {
    bool window = false;          // one of its image / panel / instrument windows is shown
    bool mapPage = false;         // its map page is open
    bool radarScope = false;      // the radar scope is showing and it is a track source
    bool patchNode = false;       // a running patch node names it
    bool playingAudio = false;    // it is the audio-out plugin holding the speakers
    bool decoderOutput = false;   // the Decoder output window is open and it is a text decoder
    bool browserSession = false;  // a browser session is alive and it is a track or image source
    bool textSink = false;        // its decoded output is wired to a Text sink
    // A plugin whose capabilities give it nothing to open - an in-chain audio processor that
    // rewrites the sound the user hears, or a module that only attaches to the host - is in use
    // for as long as it is fitted: there is no window, map or output that could ever wake it,
    // and a processor left dormant would silently stop processing the audio. (This one is not
    // in the owner's list of signals; it is what keeps those two kinds working at all.)
    bool standing = false;
};

// True when any signal is set.
bool pluginWanted(const PluginUse& u);
// The first set signal, in the fixed order of the struct, as the words the log line uses
// ("window open", "map page open", ...); empty when none is set.
const char* pluginUseSignal(const PluginUse& u);

// WHAT A MODULE WITH THESE CAPABILITIES HAS TO START. The host creates and destroys instances for
// a decoder, an I/Q decoder, an image decoder, a track source, a panel, an instrument, an in-chain
// audio processor and a host client (the attach); a module with none of them - a basemap, a
// track-info provider, a preset or settings table, an audio-out table with no decoder to ride on -
// has nothing to run, so it has no run state worth showing: it is neither idle nor running.
bool pluginCapsHaveLifecycle(std::uint32_t capabilities);
// A module that is in use for as long as it is fitted (PluginUse::standing): an in-chain audio
// processor, or a host client with nothing else to wake it by.
bool pluginCapsStandingDuty(std::uint32_t capabilities);

// One plugin's life: whether it has been woken, and when it was last wanted.
struct PluginLife {
    bool running = false;
    std::int64_t lastWantedMs = 0;
};

enum class PluginLifeStep {
    None,   // nothing to do
    Wake,   // create this plugin's instances now
    Sleep,  // destroy this plugin's instances now
};

// ONE FRAME of one plugin's life, as a pure function of its run state, whether it is wanted
// this frame and the clock.
//   STOPPED  never runs: a plugin found running is put down.
//   ALWAYS   runs from the first frame; its clock is kept current, so un-pinning it starts the
//            30 seconds from the last frame it was pinned.
//   AUTO     wakes on the first frame it is wanted (a wake is never delayed), and goes dormant
//            only once `dormantAfterMs` has passed since it was last wanted.
PluginLifeStep stepPluginLife(PluginRun mode, bool wanted, std::int64_t nowMs,
                              std::int64_t dormantAfterMs, PluginLife& life);

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_RUN_HPP
