// engine_types.hpp - the engine-side vocabulary that used to ride in engine/engine_shell.hpp (shell retirement R10, 2026-10-29).
//
// engine_shell.hpp is deleted: the window's last contact with the engine is the API table, the plugin UI port (core/plugin_ui_port.hpp) and the bounded
// run's two hooks (core/bounded_hooks.hpp). What it held that only the ENGINE and its host speak lives here, in src/engine/ (which the interface may not
// include): how the control thread runs, what the front end hands over for the publish, the plugin half of the bounded run's status report, and the
// config test's read-back. What the window needs moved to core/: engine_vocab.hpp (the row constants), status_text.hpp (the status sentences),
// pass_part.hpp and plugin_ui_port.hpp (the data of the port); this header names the core ones back into cascade::engine so that the engine's code and
// the tests that write `cascade::engine::PassPart` or `cascade::engine::TrackFeed` are unchanged.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_ENGINE_TYPES_HPP
#define CASCADE_ENGINE_ENGINE_TYPES_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/engine_vocab.hpp"
#include "core/hang_watchdog.hpp"
#include "core/host_image.hpp"
#include "core/pass_part.hpp"
#include "core/plugin_ui_data.hpp"
#include "core/plugin_ui_port.hpp"
#include "core/status_text.hpp"
#include "core/tune_control.hpp"

namespace cascade::engine {

// The constants and the status sentences, named back (they are core/ data now: the window includes them from there).
using cascade::core::kSoundCardRow;
using cascade::core::kNativeRowBase;
using cascade::core::kDecoderLogMax;
using cascade::core::EngineStatusText;

// How the engine's control thread runs (Engine::startControlThread; stage 3b, docs/engine-stage3.md section 10).
struct ControlThreadOptions {
    // One pass of the control loop per period (the GUI's frame, by default).
    std::chrono::milliseconds period{16};
    // The control thread's own hang watchdog (OPEN 8): where a report goes
    // (empty: logged to the ring, no file) and after how long a pass that has
    // not come back is a hang.
    std::string watchdogReportDir;
    unsigned watchdogThresholdMs = cascade::core::HangWatchdog::kDefaultThresholdMs;
    // THE PASS FLOOR (pump move P4, adopted decision 6; the survey's section 3.4): the least time between the STARTS of two passes. A
    // submit wakes the loop at once (that is what keeps a tune-to-audio inside one pass of its submit), so without a floor a flood
    // of submits - a drag at the frame rate, a script, a misbehaving client - becomes a flood of passes, each paying the whole
    // pass's cost (the publish allocates a status block). The floor turns a flood into at most 1000/floor passes a second and
    // costs a burst nothing it was not going to wait for: a wake that arrives after the floor has passed (any submit after an
    // idle spell) is served at once. 0 turns it off. A wake from runPassNow() is exempt (the caller asked for exactly one pass).
    std::chrono::milliseconds floor{2};
    // STEPPED MODE (adopted decision 3): the thread exists - so onControlThread(), the control watchdog, the table's thread
    // gating and every hook's real call path are all real - but runs NO pass of its own. It runs one PART of a pass (PassPart)
    // for each stepPass() the front end makes and the front end waits for it, so a bounded run is deterministic: the parts run
    // at the same places in the frame the pump ran them before the move. Stepped mode orders threads; it cannot find a race (the
    // front end is parked while a part runs) - the free-running mode and ThreadSanitizer do that.
    bool stepped = false;
    // THE TIMER TICK, NOW ONLY A FALLBACK (Windows only; 0 leaves it alone). A wait of 16 ms on the system's default 15.6 ms tick ends on the
    // SECOND tick, ~31 ms (a "62 passes a second" thread ran at 35, found by the interactive smoke of step P5), and a floor of 2 ms is ~15 ms.
    // The free-running loop no longer asks the process for a finer tick: it waits its period, its floor and its wake on its own
    // high-resolution waitable timer and an event, which need none (and a process-wide 1 ms request made the window's own frame-cap
    // wait overshoot by ~2 ms, 12x as often - docs/engine-stage4.md, the capped-tail record). This resolution (timeBeginPeriod, from the
    // thread's start to its stop) is asked for ONLY when that timer cannot be made (Windows 10 before 1803), where the loop falls back to a
    // condition variable's millisecond waits.
    unsigned timerResolutionMs = 1;
    // THE FRAME'S KICK (Engine::frameKick; docs/engine-stage4.md 6.9). A window kicks the free-running pass once a frame, so the period above
    // is the FALLBACK cadence: the loop's timer is what starts a pass when no frame does (a headless engine, a minimized or stalled
    // window). While kicks arrive the fallback waits `kickGrace` instead of `period` (a 60 Hz frame is 16.7 ms: a 16 ms timer would fire
    // just before every kick, which is the timed wake this exists to avoid), and a kick that arrives less than `kickGap` after a pass began is
    // dropped - the pass already running saw the frame's state, and a window that draws 1 000 frames a second must not ask for 500 passes.
    // When the frames stop the next pass is at most `kickGrace` after the last kick, then the period again. kickGrace 0 turns the extension
    // off (the fallback is always `period`); kickGap 0 turns the drop off (the floor alone bounds the kicks).
    std::chrono::milliseconds kickGrace{24};
    std::chrono::milliseconds kickGap{8};
    // WHERE A HANG REPORT MAY GO AT ALL (shell retirement R5): the directory this run is allowed to write reports into, whatever the
    // Diagnostics switch says now - empty for a run that may not touch the disk (a bounded one). `watchdogReportDir` above is where the
    // watchdog STARTS; this is where it goes when the interface's half later says the switch is on (Engine::applyInterfaceConfig), so the
    // switch reaches the control thread's watchdog as part of the half and not as a call of its own. Empty (the default) leaves the
    // watchdog wherever `watchdogReportDir` put it and makes the half's switch a no-op for it.
    std::string diagnosticsDir;
};

// THE THREE PARTS OF A PASS (pump move P4/P5) are core/pass_part.hpp (shell retirement R9, 2026-10-28): the word a bounded run's host and its window
// share, in namespace cascade::core, named back here for the engine's code and its tests.
using cascade::core::PassPart;

// THE TRACK FEED, THE BASEMAP'S FIGURES AND THE BOUNDED-RUN STATUS REPORT'S PLUGIN HALF moved to core/plugin_ui_port.hpp with the plugin UI plane
// (shell retirement R4, 2026-10-28), beside the tile vocabulary and what one directive says to the window: they are the port's data, which the
// interface includes. They are in namespace cascade::core there, and named back here for the engine's code and its tests.
using cascade::core::TrackFeedInfo;
using cascade::core::TrackFeed;
using cascade::core::BasemapFacts;
using cascade::core::PluginUiReport;

// THE PLUGIN HALF OF THE BOUNDED-RUN STATUS REPORT AND THE PICTURES IT PRINTS (FrontEndFacts::pluginStatus; Engine::reportPluginStatus prints it).
struct PluginStatusFacts {
    PluginUiReport ui;
    std::vector<cascade::core::HostImage> images;
};

// WHAT THE FRONT END HANDS OVER FOR THE PUBLISH (engine/stage3b-pre, docs/engine-stage3.md OPEN 4): the facts only the window knows -
// the counter's face (the web page mirrors it), the basemap plugin's figures, the track feed -
// handed to the engine once a frame (PluginUiPort::setFrontEnd, which copies the window's PluginUiFrontEnd into this) instead of the engine
// calling the window for them while it publishes.
struct FrontEndFacts {
    // (webListening was here until stage 5b: the web server is the engine's, and the receiver state's WEB_LISTENING bit is its host's answer.)
    // A front end that has handed nothing over (the headless engine) shows
    // the product's default face, as EngineHost's own answer always was.
    std::string tunerDisplayStyle = cascade::gui::tunerStyleName(cascade::gui::TunerStyle::Nixie);
    BasemapFacts basemap;
    // The track feed (above); null when there is none, which is every pass of a headless engine.
    std::shared_ptr<const TrackFeed> trackFeed;
    // (A BOUNDED RUN'S CATALOGUE DIAGNOSTIC - CASCADE_PLUGIN_TEST: whether to print the report when a fetch resolves, and the frame to name in it - was
    // a pair of fields here until shell retirement R9, 2026-10-28. It is the bounded run's HOST's: the harness (engine/host_measure.hpp) arms it and is told
    // the frame at the end of each of the window's frames - Engine::setPluginTestReport, notePluginTestFrame.)
    // A BOUNDED RUN'S DIAGNOSTIC (CASCADE_PLUGIN_STATUS; shell retirement R4): what the window's plugin poll and picture buffer hold, handed over
    // once at the end of a run that asked for the report (PluginUiFrontEnd::pluginStatusReport) and printed, once, by Engine::shutdownQuiesce
    // right after its first phase - where the window used to call reportPluginStatus. Null in every other pass.
    std::shared_ptr<const PluginStatusFacts> pluginStatus;
};

// THE CONFIG TEST'S READ-BACK (CASCADE_CONFIG_TEST's one printed line; shell retirement R6 made it Engine::configReadback, public): the demod mode the
// pipeline mirrors hold (an index into the engine's mode table) and the centre the active source reports - values read back from the engine, not an
// echo of the file.
struct ConfigReadback {
    int modeIndex = 0;
    double centreHz = 0.0;
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_TYPES_HPP
