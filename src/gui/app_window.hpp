// GLFW + Dear ImGui application shell.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// Forward-declared rather than including GLFW here: this header is included
// by the tests, and pulling a windowing library into them would make a
// headless build depend on one.
struct GLFWwindow;

#include "core/band_plan.hpp"
#include "core/config.hpp"
#include "core/freq_manager.hpp"
#include "core/pipeline.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "core/plugin_repo.hpp"
#include "core/updater.hpp"
#include "core/recorder.hpp"
#include "core/retune_coalescer.hpp"
#include "core/scanner.hpp"
#include "gui/basemap_cache.hpp"
// The ADS-B radar scope. ImGui-free like track_metrics.hpp below, so it can be
// held by value here without breaking the rule that main() - and the tests -
// never see a GUI header.
#include "gui/scope_view.hpp"
#include "gui/track_info_cache.hpp"
// CoverageMap, TrackSortKey: the pure arithmetic behind the map's three
// receiver-relative features. Header-only and ImGui-free, so including it here
// keeps app_window.hpp usable from the tests (see the note below).
#include "gui/track_metrics.hpp"
#include "core/telemetry.hpp"
#include "core/crash_upload.hpp"
#include "core/hang_watchdog.hpp"
#include "gui/freq_scale.hpp"
// Pulls in the bind policy and the credential types too, but NOT httplib —
// web_server.hpp forward-declares it.
#include "net/cat_server.hpp"
#include "net/web_server.hpp"
// For SoapyDeviceInfo and the non-owning SoapySource* below; the header
// forward-declares the Soapy API types, so this pulls in no Soapy headers.
#include "source/soapy_source.hpp"

namespace cascade::gui {

// Forward declarations keep ImGui types out of this header (waterfall_view.hpp
// includes imgui.h), preserving the rule that main() never sees GUI headers.
class SpectrumView;
class WaterfallView;
class MapView;

// Whether a device open that finished on a worker thread should still be
// applied to the pipeline.
//
// Opening a SoapySDR device takes seconds, and the user is not frozen while it
// happens: they can pick the built-in generator, open an IQ file, or drive
// either from the web UI in the meantime. Without this check the open simply
// landed when it landed and replaced whatever they had chosen — the radio
// changed itself, several seconds after they told it not to.
//
// The rule is a sequence number rather than a "cancelled" flag because the
// question is not "was it cancelled" but "is the answer still about the source
// the user is looking at": every install of a source bumps the counter, the
// request records the value it was made at, and a result whose value has moved
// on is about a source selection that no longer exists.
inline bool asyncOpenStillWanted(std::uint64_t requestedAtGen,
                                 std::uint64_t currentGen) {
    return requestedAtGen == currentGen;
}

// One monitor's usable area, in the virtual-desktop coordinates ImGui and the
// window manager both speak. Its own type rather than an ImGui one so this
// stays testable without a platform backend.
struct ScreenRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// How much of the map window has to be reachable for a SAVED geometry to be
// worth restoring: a title bar's worth. An ImGui window is dragged by its
// title bar and by nothing else, so a rectangle whose title bar is off the
// screen cannot be moved back on by any means the user has - the resize grip
// in the far corner can only resize. 120x30 is a bar wide enough to grab and
// tall enough to hit.
constexpr float kMapReachableW = 120.0f;
constexpr float kMapReachableH = 30.0f;

// Whether a map-window rectangle saved on some PREVIOUS run can still be
// reached on THIS machine's monitors.
//
// The config sanitizer only checks the numbers are sane in isolation
// (AppConfig::kMapWindowMinPx/kMapWindowMaxPx); it has no idea what displays
// exist. A geometry saved on a second monitor that has since been unplugged is
// a perfectly legal rectangle in a place that no longer exists, and restoring
// it hands the user a window they cannot see or move. That is a NEW failure
// mode: before the map window's size was persisted it always opened beside the
// main window, where it could not be lost.
//
// The test is on the TITLE BAR STRIP, not on the window as a whole, because
// overlap somewhere is not the same as being usable - see kMapReachableH.
// WHICH monitor the title-bar strip landed on, as an index into `workAreas`,
// or -1 for none. The index is what the size clamp below needs: "it is on
// screen somewhere" does not say which screen's dimensions the window has to
// fit inside.
inline int mapReachableMonitor(int x, int y, int w, int h,
                               const std::vector<ScreenRect>& workAreas) {
    if (w <= 0 || h <= 0) { return -1; }
    const float left = static_cast<float>(x);
    const float top = static_cast<float>(y);
    const float right = left + static_cast<float>(w);
    // The strip is the window's width but only a title bar's height, and it
    // cannot be taller than the window itself.
    const float strip = (static_cast<float>(h) < kMapReachableH)
                            ? static_cast<float>(h)
                            : kMapReachableH;
    const float bottom = top + strip;
    for (std::size_t i = 0; i < workAreas.size(); ++i) {
        const ScreenRect& r = workAreas[i];
        const float ix0 = (left > r.x) ? left : r.x;
        const float ix1 = (right < r.x + r.w) ? right : r.x + r.w;
        const float iy0 = (top > r.y) ? top : r.y;
        const float iy1 = (bottom < r.y + r.h) ? bottom : r.y + r.h;
        if (ix1 - ix0 >= kMapReachableW && iy1 - iy0 >= kMapReachableH) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

inline bool mapGeometryOnScreen(int x, int y, int w, int h,
                                const std::vector<ScreenRect>& workAreas) {
    return mapReachableMonitor(x, y, w, h, workAreas) >= 0;
}

// The share of a monitor's work area the map window OPENS at when nothing was
// saved. A DEFAULT ONLY: a window that exactly fills the work area looks
// maximised, and the user then cannot see what it is covering - which is a
// judgement about a size the host picks, not a limit on one the user picked.
// Applying it to a restored rectangle shrank a perfectly usable window on
// every launch (measured: a saved 1120x1300 at 600,60 - bottom 1360, well
// inside this machine's 1392 px work area - came back 1120x1183, and the 1183
// was written to the config, so the chosen height degraded once per restart
// until it reached the share).
constexpr float kMapWorkAreaShare = 0.85f;

// A RESTORED rectangle is clamped to WHAT ACTUALLY FITS WHERE IT SITS, and to
// nothing else.
//
// Only the position was being checked at first. A geometry saved on a taller
// or wider display - a 1440p monitor at the office, a laptop panel at home -
// is a perfectly reachable rectangle whose BOTTOM, and with it the resize
// grip, is off the screen it reopens on: the user can drag the window but
// cannot make it smaller, and the geometry is then saved back oversized every
// frame. The harm is the grip being off the screen, so the test is the screen
// edge - the work area's right and bottom measured from the window's own
// origin - and not a share of the monitor.
//
// Position is deliberately left alone. mapReachableMonitor has already said
// the title bar can be grabbed, and moving a window the user placed is a
// bigger liberty than shrinking one that cannot fit.
inline void mapClampRestoredSize(int x, int y, int& w, int& h,
                                 const std::vector<ScreenRect>& workAreas) {
    const int idx = mapReachableMonitor(x, y, w, h, workAreas);
    if (idx < 0) { return; }  // not restorable at all; the caller falls back
    const ScreenRect& r = workAreas[static_cast<std::size_t>(idx)];
    // Both are positive by construction: the title-bar strip overlapped this
    // work area by at least kMapReachableW x kMapReachableH, so the window's
    // own origin is left of its right edge and above its bottom one.
    int fitW = static_cast<int>(r.x + r.w) - x;
    int fitH = static_cast<int>(r.y + r.h) - y;
    // ...but never shrunk to something that is no longer a window. A rectangle
    // parked with only its title bar showing at the bottom of the screen would
    // otherwise be restored as a 30 px sliver AND saved that way; a little
    // overhang is the lesser harm, and the sanitizer's own minimum is the
    // smallest rectangle this product accepts anywhere.
    if (fitW < cascade::core::AppConfig::kMapWindowMinPx) {
        fitW = cascade::core::AppConfig::kMapWindowMinPx;
    }
    if (fitH < cascade::core::AppConfig::kMapWindowMinPx) {
        fitH = cascade::core::AppConfig::kMapWindowMinPx;
    }
    if (w > fitW) { w = fitW; }
    if (h > fitH) { h = fitH; }
}

// Where the map window OPENS when there is no saved geometry: the proposed
// rectangle, MOVED so all of it - the resize grip in the bottom-right corner
// included - is on the screen it lands on.
//
// The default size is capped at kMapWorkAreaShare, but the default POSITION
// was never checked against anything, and the two only make a usable window
// together. placeAsSeparateWindow anchors the map at the main window's top
// right plus 60 px, so once the default height grew to use the screen the
// monitor offers, the bottom fell off the work area whenever the main window
// sat low: measured on this 5120x1440 desktop with the main window at y=400,
// the map opened at (1248,491) 1120x1168 - a bottom of 1659 against a work
// area of 1392 - and that rectangle was then persisted and restored verbatim,
// because it is not oversized, only misplaced.
//
// MOVED rather than shrunk, because the size is the part that was chosen for
// this monitor on purpose; shrinking it would hand back the scrollbar the
// derived height exists to avoid. Only a rectangle larger than the whole work
// area is shrunk, which the share makes impossible unless the small-screen
// floor in mapDefaultSize is what set it.
inline void mapPlaceDefaultRect(float& x, float& y, float& w, float& h,
                                const std::vector<ScreenRect>& workAreas) {
    if (workAreas.empty()) { return; }
    // The monitor the proposed rectangle's title bar lands on. A default that
    // lands nowhere at all means the main window is itself somewhere odd, and
    // the first work area is a better answer than leaving the map off-screen.
    int idx = mapReachableMonitor(static_cast<int>(x), static_cast<int>(y),
                                  static_cast<int>(w), static_cast<int>(h), workAreas);
    if (idx < 0) { idx = 0; }
    const ScreenRect& r = workAreas[static_cast<std::size_t>(idx)];
    if (w > r.w) { w = r.w; }
    if (h > r.h) { h = r.h; }
    if (x + w > r.x + r.w) { x = r.x + r.w - w; }
    if (y + h > r.y + r.h) { y = r.y + r.h - h; }
    // Bottom-right first, then top-left: a rectangle as large as the work area
    // has to end up at its origin, not pushed off the top by the nudge above.
    if (x < r.x) { x = r.x; }
    if (y < r.y) { y = r.y; }
}

// Owns the GLFW window, the ImGui context and the top-level panel layout.
// All GLFW/ImGui usage stays behind this interface so main() (and any future
// headless harness) never needs GUI headers.
class AppWindow {
public:
    // Constructs the render pipeline with the demo SigGen signal already
    // configured, so the first Play click shows spectrum content immediately.
    //
    // configPath: where the persistent AppConfig is loaded from at
    // construction and saved to (debounced during run(), and on clean exit).
    // An EMPTY path disables persistence entirely — nothing is read or
    // written — which is the hermetic mode the --frames/--selftest CI
    // contract requires, and the default so a bare AppWindow can never touch
    // the user's real config by accident. announceConfig prints the one-line
    // "config applied: ..." diagnostic to stdout after the startup load (the
    // CASCADE_CONFIG_TEST hook; normal runs keep stdout byte-identical).
    explicit AppWindow(std::string configPath = {}, bool announceConfig = false);

    // Out-of-line: the unique_ptr members delete forward-declared types.
    // Also finalizes any recording still in flight (uninstall the pipeline
    // taps, then Recorder::stop) BEFORE the recorder members — which are
    // declared after pipeline_ and therefore destroyed first — can dangle
    // under a still-running DSP thread. run()'s teardown already does this
    // on the normal exit path; the destructor is the safety net.
    ~AppWindow();

    // Runs the shell until the window is closed, or — when `frames` >= 0 —
    // for exactly that many rendered frames. The bounded mode is the
    // `--frames N` self-test contract that the app_smoke ctest entry relies
    // on: render N frames, shut down cleanly, exit 0.
    //
    // Returns the process exit code: 0 on clean shutdown, 1 when GLFW or the
    // ImGui backends fail to initialize (the reason is printed to stderr).
    int run(int frames = -1);

    // Where hang reports go, and whether the frame loop should stage a
    // deliberate stall. Both are decided in main(), because only main() knows
    // whether this is a real session or a bounded CI run - and a bounded run
    // must leave nothing on the machine it ran on.
    void setDiagnosticsDir(std::string crashDir);
    void setDiagStallMs(int ms);
    // --diag-toggle on|off: +1, -1, or 0 for "leave it alone". Flips the
    // Settings > Diagnostics switch on frame 30, through the same function the
    // checkbox calls, so the mid-session behaviour of that switch can be
    // proved against the real application.
    void setDiagToggle(int mode);

private:
    // One plugin's map page — declared ahead of the drawing methods that take
    // it, defined in full beside the map state below.
    struct MapPage;

    void drawUi();
    void drawToolbar();
    void drawFrequencyReadout();
    void drawMenuColumn();
    // The update banner, and the work behind it. Drawn at the top of the menu
    // column because a build that cannot see the user's radio is the most
    // useful thing this application can say to them, and it is worth more than
    // whatever they opened the panel for.
    void drawUpdatesSection();
    void drawUpdateBanner();
    void startUpdateCheck();
    void startUpdateDownload();
    void pollUpdateAsync();
    // Once-a-second check that the output stream is still alive, reopening it
    // if it is not. See AudioOut::streamAlive() for what kills one; the short
    // version is that a dead sink is invisible from inside the app, so the
    // only fix is to keep asking.
    void pollAudioHealth();
    // Takes every live plugin handle off the pipeline and the UI, in the one
    // order that is safe, then unmaps the modules. The ONLY way any code here
    // may call PluginHost::unloadAll() — see the note in its body.
    void detachAndUnloadPlugins();

    // Starts the downloaded installer and asks the run loop to exit. Separate
    // because an installer cannot replace a binary that is still running, so
    // "install" necessarily means "and close this".
    bool launchInstaller(const std::string& path);
    void requestClose() { closeRequested_ = true; }

    void drawSourceSection();
    // Runs one SoapySDR enumeration into soapyDevices_ and re-points the
    // combo selection at the active device by args (labels can repeat; a
    // device that vanished from the scan leaves sourceSel_ = -1 and the
    // preview falls back to the live source name). Called from the combo's
    // first open and from Refresh — deliberately never from the constructor
    // (see soapyDevices_ below for why).
    void scanSoapy();
    // Combo-row click handler: 0 = generator, 1 = IQ file (panel only — the
    // pipeline switches on a successful Open), 2+i = soapyDevices_[i]
    // (opens immediately; on failure the combo selection is left unchanged).
    void selectSource(int idx);
    void drawCenterPanels();
    // The slim tick strip between the spectrum and the waterfall. tickHz /
    // labels / count come from FreqScale::ticks, computed once per frame in
    // drawCenterPanels and shared with the spectrum's vertical gridlines so
    // strip and grid can never disagree.
    void drawFreqAxis(float width, const double* tickHz, const char (*labels)[16],
                      int count);

    // --- Recorder / Bookmarks / Scanner (P6) ----------------------------------
    void drawRecorderSection();
    void drawBookmarksSection();
    void drawScannerSection();

    // --- Stereo / RDS / audio filters / band plan / plugins (P7) --------------
    // Drawn inside the Radio section, and only while WFM is the active mode:
    // the pilot indicator, the force-mono toggle, and the RDS readout are
    // meaningless for every other demodulator.
    void drawStereoRdsControls();
    // "Audio filters": noise reduction + manual/auto notch, in the order the
    // pipeline applies them (notch -> auto-notch -> NR; see Pipeline).
    void drawAudioFilterSection();
    // "Plugin store": everything that talks to the CATALOGUE — the "Get
    // plugins" toggle and the browser it opens (drawPluginBrowser), which is
    // where fetching, the update plans and installing live. Split out of the
    // Plugins section because those are an app store and the section below is
    // an inventory: one reaches the network, the other never does, and mixing
    // them made "am I about to download something?" a question the user had to
    // answer by reading button labels.
    void drawPluginStoreSection();
    // "Plugins": what is installed on THIS machine and nothing else. Loaded
    // plugins with their LICENCE, refused candidates with their reason, a
    // Remove button per install, a Rescan button, the RETIRED plugins as red
    // rows, the decoder idle reasons and the per-plugin receiver-control
    // grants. Nothing here contacts the catalogue.
    void drawPluginsSection();
    // The red rows: every plugin the cached catalogue policy retires, each
    // carrying PluginRepo::pluginBlockMessage() verbatim. Drawn above the
    // installed list because a disabled plugin is the news on this panel.
    void drawBlockedPluginRows();
    // The body of the Plugin store section: the URL field, the Browse /
    // Refresh button (the ONLY thing in the product that contacts the
    // catalogue origin), the in-flight progress + Cancel, the entry list and
    // the selected entry's detail pane with the install gate.
    void drawPluginBrowser();
    // The last install/remove outcome: PluginRepo's own error verbatim in
    // red, or the success line in green. Its own method because BOTH sections
    // produce one (Install lives in the store's browser, Remove in the
    // installed list) and it must be visible whichever one the user is
    // looking at — a failed remove with the browser collapsed would otherwise
    // report nothing at all.
    void drawPluginCatalogueDetail(int idx);
    void drawPluginResultText();
    // "Receiver control": the checkbox that grants one plugin permission to
    // tune the radio. Without it the permission PluginUi enforces could never
    // be given — every request_tune was refused and the user had no way to say
    // yes — which made a satellite tracker's Doppler correction unreachable.
    // The one-click rows under an installed plugin: "ADS-B 1090 MHz" and the
    // like, declared by the plugin itself through CASCADE_CAP_PRESET.
    // The list of decoded targets down the side of the Map window: callsign,
    // id and a details button per row, with click-to-go-to and double-click to
    // follow. A map answers "where is everything"; this answers "what am I
    // hearing", and clicking answers "take me to that one".
    //
    // THREE THINGS, NOT EIGHT. It was an eight-column sortable table, and in
    // the width the list actually gets every heading was truncated - a table
    // sorted by columns nobody can read is worse than the plain list it
    // replaced. The other six values moved into the details window below, and
    // the eight sort keys into one labelled menu above the table, so nothing
    // that could be asked before has stopped being answerable.
    //
    // Per page now: `tracks` is the page's plugin's tracks only, already
    // filtered by the caller, and every selection/follow gesture lands on that
    // page's own MapView.
    void drawTrackList(MapPage& page,
                       const std::vector<cascade::core::HostTrack>& tracks);
    // The compact sort control above the list: which key, and which way. It is
    // the ONLY way the list is ordered - the table itself is no longer
    // ImGui-sortable, because ImGui offers no public way to write the header's
    // sort arrow back, so a menu and a clickable header would sooner or later
    // have shown a "Callsign" arrow over rows ordered by distance.
    void drawTrackSortControl();
    // "Target details": the full block for the one target whose row button was
    // pressed, in a window of its own. See the implementation for why it is a
    // window rather than a popup or a panel under the list.
    void drawTargetDetailsWindow();
    // The same block as a section in the MAIN window's menu column, so the
    // craft being watched is readable without the Map window arrangement —
    // the map is often on another monitor, or closed. Shows the target whose
    // Details button was pressed, else the followed one, else the selected
    // one.
    void drawTargetDetailsSection();
    // The by-id lookup both of those share: the host's track vector is rebuilt
    // every poll, so a stored pointer or index would go stale within a frame —
    // find it fresh, and only among tracks the staleness rule still shows.
    const cascade::core::HostTrack* findVisibleTrack(const std::string& id) const;

    // --- the ADS-B radar scope (see gui/scope_view.hpp) ---------------------
    //
    // THE WHOLE MAIN WINDOW, drawn instead of the spectrum, the waterfall and
    // the menu column. That is what the mode IS: a beta tester's dedicated
    // ADS-B receiver shows a scope and nothing else, and a scope squeezed into
    // a panel beside a waterfall would be a smaller version of the map rather
    // than the instrument he asked for.
    //
    // IT ALWAYS DRAWS ITS OWN WAY OUT, in a bar above the face, before
    // anything that can fail to have room. A mode with no exit is the
    // map-page latch again in a larger form: there, every reopen affordance
    // lived inside the window the user had just closed.
    void drawScopeMode();
    // The switch that turns it on, in the Decoders section of the left rail.
    void drawScopeModeControl();
    // The receiver position entry - two coordinate fields and "Set RX here" -
    // drawn by BOTH the map pages and the scope's no-position state. One copy,
    // because the button has consequences beyond the two numbers (every map
    // page's home moves, the coverage accumulator is discarded because every
    // wedge in it was measured from somewhere else) and a second copy would
    // sooner or later do only some of them.
    void drawRxPositionEntry();

    void drawPluginPresets(const cascade::core::LoadedPlugin& p);
    // Tunes to a preset, sets the mode/bandwidth/device rate it asks for,
    // rebuilds the decoders against the new receiver state, and opens what
    // that plugin contributes. The ONLY caller is a button: a preset is a
    // plugin publishing where it listens, never a plugin retuning the radio —
    // that still needs the separate per-plugin permission.
    void applyPluginPreset(const cascade::core::LoadedPlugin& p, const CascadePreset& ps);
    void drawPluginTuneControls();
    // Grants or revokes one plugin, updating both the live PluginUi and the
    // persisted list. One function so the two can never disagree: a grant that
    // took effect but was not saved would come back revoked next launch.
    // `pluginKey` is a PluginUi::tuneKey() — the module file name, never the
    // display name, which the plugin itself chooses.
    void setPluginTuneAllowed(const std::string& pluginKey, bool allowed);
    // Pushes pluginTuneAllowed_ into pluginUi_. Called after every
    // PluginUi::rebuild, because rebuild follows a clear() that drops the
    // grants along with the instances — without this a rescan silently revoked
    // every permission the user had given.
    void applyPluginTuneGrants();
    // Whether the user has stopped this plugin. `pluginKey` is a module file
    // name (cascade::core::pluginKey), the same identity the tune grant uses.
    bool pluginIsStopped(const std::string& pluginKey) const;
    // Records a stop or a start WITHOUT rebuilding: updates the durable list
    // and pushes it into the runner and the UI half, so the next rebuild sees
    // it. Split from the button's action below because applyPluginPreset has
    // to start a plugin and then rebuild ONCE, having also moved the receiver.
    void recordPluginStopped(const std::string& pluginKey, bool stopped);
    // The Stop/Start button's action: record it, then rebuild through the one
    // lifecycle path everything else uses, so a stop tears the plugin's
    // instances down and a start builds them against the CURRENT receiver.
    void setPluginStopped(const std::string& pluginKey, bool stopped);

    // --- Audio mute while a data decoder is running (see plugin_ui.hpp) -------
    // The EFFECTIVE "mute audio while running" setting for one plugin: the
    // default its capabilities imply, flipped if the user has overridden it.
    bool pluginMutes(const cascade::core::LoadedPlugin& p) const;
    // Records the user's choice as an override of the capability default, so
    // ticking the box back to the default REMOVES the entry rather than
    // recording a second kind of "yes". Rebuilds the mute snapshot.
    void setPluginMutes(const cascade::core::LoadedPlugin& p, bool mutes);
    // Rebuilds muteStates_ from the loaded plugins: identity, running state,
    // effective setting, and the plugin's presets.
    //
    // A SNAPSHOT REBUILT ON CHANGE, not read per frame, because reading the
    // presets means CALLING the plugin - count() and get() are its own code -
    // and doing that once per plugin per frame to decide whether to be quiet
    // would put third-party code on the frame path for no gain. Presets are a
    // property of the plugin and not of a running instance (the ABI says so),
    // so they cannot change without a rescan.
    void rebuildMuteStates();
    // Once per frame: evaluate the policy against where the receiver actually
    // is, push the result into the pipeline, and arm the popup on the edge.
    void updateAudioMute();
    // The modal that offers to stop the plugins holding the audio down, and
    // the banner that stays when the user declines.
    void drawMutePopup();
    void drawMuteBanner();
    // Stops exactly the plugins named by `keys`, through the ordinary stop
    // path, in one rebuild. The caller passes the keys its own message named -
    // the banner passes mutedByKeys_, the popup passes what it captured - so a
    // button can never stop something other than what the words above it said.
    void stopMutingPlugins(const std::vector<std::string>& keys);
    // "ADS-B decoder", or "ADS-B decoder and AIS decoder", or a comma list.
    // One place, because the popup, the banner and the Sinks panel all have to
    // name the same plugins the same way.
    static std::string muteNameList(const std::vector<std::string>& names);
    std::string muteSubjectText() const;
    // Decoder OUTPUT: what the loaded plugins are actually decoding, plus a
    // line per plugin that is loaded but not being fed and why. Drained from
    // PluginRunner every frame, because the runner's buffer is bounded and a
    // GUI that stops reading would silently drop the newest lines.
    // Idle reasons and the button that opens the output window; stays in the
    // Plugins section because it is about installation, not traffic.
    void drawDecoderStatusRows();
    // The decoded text, in its own operating system window.
    void drawDecoderWindow();
    // Moves decoded lines out of the runner into decoderLog_. Called from
    // drawUi unconditionally, because the runner's buffer is bounded and
    // draining only when the panel is visible would drop output silently.
    void pumpDecoderOutput();
    // Rebuilds every decoder instance against the CURRENT source rate and
    // centre frequency. Called after any source change, because both are
    // passed to a decoder's create() and cannot be changed afterwards.
    void refreshPluginRunner();
    // The map pages and every plugin-declared panel window. Drawn as their
    // own top-level windows rather than inside the menu column: a map squeezed
    // into a 300 px sidebar is not a map, and a plugin's window should be
    // movable and resizable like any other.
    void drawPluginWindows();
    // The page for a plugin, by the display name every HostTrack carries.
    // find returns null for a plugin with no page yet; ensure creates one
    // lazily — its own MapView, the receiver position applied, and geometry
    // seeded from the saved entry or from the legacy single-window rectangle
    // staggered by page index.
    MapPage* findMapPage(const std::string& plugin);
    MapPage& ensureMapPage(const std::string& plugin);
    // Folds every live page's rectangle and open flag into mapPagesSaved_,
    // which is what currentConfig() writes out. Entries for plugins with no
    // page this session ride through untouched, so a geometry saved for a
    // plugin that is temporarily uninstalled is not erased by unrelated saves.
    void syncMapPagesToSaved();
    // Starting position and size for a window that should be its OWN operating
    // system window rather than a panel inside the main one. `slot` staggers
    // several of them. See the definition for why the position is what decides
    // this — ImGui has no flag for it.
    void placeAsSeparateWindow(int slot);
    // The same anchor as a VALUE rather than as a side effect. The map needs
    // it before it is used: its default rectangle has to be checked against
    // the monitor (mapPlaceDefaultRect), and a function that only calls
    // SetNextWindowPos cannot answer where the window would have gone.
    static void separateWindowAnchor(int slot, float& x, float& y);
    // Where the map window should open when the config has no saved geometry:
    // a size derived from the MONITOR's work area, not a constant. See the
    // definition for why a constant was the "map screen isn't large enough"
    // report.
    static void mapDefaultSize(float& widthPx, float& heightPx);
    // Translucent service-band rectangles over the spectrum panel, plus the
    // labels that fit. `pos` is the panel's screen-space top-left as recorded
    // before the spectrum was drawn.
    void drawBandPlanOverlay(float x0, float y0, float width, float height);
    // Band plan (optional program data) and plugins (optional user
    // installs) — both silently absent when their directory does not exist.
    void loadBandPlan();
    void rescanPlugins();
    // Every tune that moves the SOURCE centre has to tell the pipeline, which
    // cannot see it: the RDS/stereo decoders must forget the old station.
    //
    // For a hardware (Soapy) source this is a REQUEST, paced through
    // retuneCoalescer_: bursts (one wheel notch per frame is 60-144 tunes a
    // second) collapse to at most one device call per ~50 ms, latest value
    // winning — the gesture that produced the most frequent 0.62.0 field
    // crash. A single tune still applies immediately. The generator and IQ
    // file sources apply immediately always (no USB to pace).
    void retuneSourceHz(double centerHz);
    // The unpaced apply: setCenterFrequencyHz + decoder resets + readback.
    // Call directly only where the readback must be valid on return (the
    // carry-across on a fresh device open); everything else goes through
    // retuneSourceHz.
    void applyRetuneNow(double centerHz);
    // Frame-loop poll releasing a held retune once its interval has passed.
    void pollPendingRetune();

    // Uninstalls the matching pipeline tap, THEN stops the recorder — the
    // order the Recorder contract requires (see Pipeline::set*Recorder).
    // Both are harmless no-ops when nothing is recording, so the toolbar
    // Stop path calls them unconditionally.
    void stopIqRecording();
    void stopAudioRecording();

    // ONE absolute-tune path shared by bookmark click-to-tune and scanner
    // retunes: commands the SOURCE center to (absHz - VFO offset) through
    // activeSource().setCenterFrequencyHz — the same setter + readback path
    // the toolbar digit wheel uses — so the VFO band (whose offset is
    // preserved) lands on absHz and the display follows the readback.
    void tuneAbsoluteHz(double absHz);
    // The tuned station: source center readback + VFO offset (what the VFO
    // band marks on the spectrum). This is what a bookmark captures and what
    // the scanner's user-tune detection compares.
    double currentAbsoluteHz();

    // Once-per-GUI-frame scanner driver (called at the end of drawUi):
    // detects manual tunes (user wins -> stop), feeds tick() with ImGui's
    // clock and the squelch-open state, applies returned retunes.
    void scannerFrame();

    // Persists the bookmark list after every mutation; failures land in
    // bookmarkError_ (red text). No-op in hermetic mode (empty path).
    void saveBookmarks();

    // --- Config persistence (P5) ---------------------------------------------
    // Pushes every AppConfig field into the pipeline/panel mirrors; source
    // restore failures (file gone, device unplugged) fall back to the
    // generator silently except for lastError surfaced via sourceError_.
    void applyConfig(const cascade::core::AppConfig& cfg);
    cascade::core::AppConfig currentConfig();  // snapshot of the live state
    void maybeSaveConfig(double nowS);  // debounced: ~2 s after the LAST change
    void saveConfigNow();               // clean-exit save (unconditional)

    // Opens a Soapy device by kwargs, pushes the requested rate + default
    // gains, and fills the Soapy panel mirrors. Null (with sourceError_ set)
    // when the open fails. Shared by selectSource and the config restore so
    // the two open paths cannot drift apart.
    std::unique_ptr<cascade::source::SoapySource> openSoapy(const std::string& args,
                                                            double requestRateHz);

    // Makes the DSP chain follow activeSource().sampleRateHz() (rate-follow).
    // A pipeline refusal — fractional channel rate — keeps the old chain and
    // surfaces the reason in sourceError_.
    void followInputRate();

    // DSP pipeline plus the two live display widgets it feeds. The views are
    // held by unique_ptr for two reasons: the forward declarations above, and
    // the waterfall's GL texture, whose deletion needs the creating GL context
    // current — run() tears the view down explicitly before destroying the
    // context, because AppWindow itself outlives it (destroyed in main()).
    cascade::core::Pipeline pipeline_;
    std::unique_ptr<SpectrumView> spectrum_;
    std::unique_ptr<WaterfallView> waterfall_;

    // Newest frame received from the pipeline. Cached here (not just handed
    // to the views) so the spectrum keeps drawing the last data after Stop —
    // SpectrumView::draw takes bins per call and holds no history of its own.
    cascade::core::SpectrumFrame lastFrame_;

    // Display range for both the spectrum axis and the waterfall colormap.
    float dbMin_ = -110.0f;
    float dbMax_ = 0.0f;

    // Radio/Sinks control state. The pipeline owns the live DSP values; these
    // mirrors exist because ImGui widgets edit by pointer. Defaults match the
    // pipeline's own defaults (WFM, 150 kHz bandwidth, -50 dB squelch) except
    // the VFO offset, which the constructor pushes to +300 kHz so the demo
    // tone 0 sits on the VFO — near-silent in WFM (an unmodulated carrier
    // demodulates to DC), a clean 700 Hz sidetone in CW.
    float volume_ = 0.5f;
    int modeIndex_ = 1;                              // WFM
    float vfoOffsetKhz_ = 300.0f;
    int bandwidthIndex_ = 1;                         // 150k
    float squelchDb_ = -50.0f;
    // Output devices, enumerated once at construction (a hot-plug refresh can
    // come with the settings work in P5); index into devices_, -1 when empty.
    std::vector<cascade::sink::AudioDevice> devices_;
    int deviceIndex_ = -1;
    // Output-stream watchdog (see pollAudioHealth). The note is shown in the
    // Sinks panel: a stream that had to be restarted is something the user
    // should be told about, because the alternative reading of the same
    // event — audio that stopped and came back on its own — is indistinguish-
    // able from a fault in their radio.
    double lastAudioProbeSec_ = 0.0;
    int audioRecoveries_ = 0;
    std::string audioHealthNote_;
    float splitRatio_ = 0.4f;  // spectrum's share of the center area

    // --- Source menu state (P4) ---------------------------------------------
    // The frequency readout no longer keeps a mirror: it always displays
    // pipeline_.activeSource().centerFrequencyHz() readback (nominal for the
    // generator/file, real device readback for Soapy). The generator's 100 MHz
    // default preserves the parity-spec startup display.
    //
    // Enumerated SoapySDR devices behind combo rows 2..N+1 (rows 0/1 are the
    // generator and the IQ file). Filled LAZILY by scanSoapy() — first
    // dropdown open, or Refresh — never at construction: enumeration loads
    // vendor modules (SoapyUHD -> uhd.dll -> libusb) whose USB discovery
    // crashed in-process in ~2% of measured runs (libusb-1.0.dll AV during
    // uhd::device::find, P6a investigation 2026-08-15). Deferring the scan
    // keeps generator/file sessions — and every bounded --frames CI run —
    // from ever executing that code.
    // Direct frequency entry: double-clicking the readout swaps the digit
    // strip for a text field (SDR++-style typing). Wheeling digits alone
    // cannot get you from 100 MHz to 433 MHz in any reasonable number of
    // notches, which is what made tuning feel broken.
    // Waterfall press tracking: a press that ends without crossing the drag
    // threshold is a CLICK (tune here); one that crosses it is a PAN. Both
    // gestures share the left button, so they can only be told apart on
    // release.
    float wfPressX_ = 0.0f;
    bool wfMoved_ = false;

    // Moves the VFO so the tuned frequency lands on wantAbsHz, snapping to the
    // mode's raster unless the caller says otherwise, and clamping the band
    // inside the baseband span. Shared by click-to-tune and the drag path.
    void setVfoToAbsoluteHz(double wantAbsHz, bool snap);

    int deemphIndex_ = 0;  // index into kDeemphUs; 0 = 50 us (global default)

    bool freqEditing_ = false;
    bool freqEditFocus_ = false;   // request keyboard focus on the first frame
    bool freqEditWasActive_ = false;  // field has held focus at least once
    char freqEditBuf_[32] = {0};

    std::vector<cascade::source::SoapyDeviceInfo> soapyDevices_;
    bool soapyScanned_ = false;  // one lazy scan done (scanSoapy())

    // --- Off-thread SoapySDR discovery and open --------------------------
    // SoapySDR::Device::enumerate()/make() do USB bus discovery and, for a
    // B200, an FPGA/firmware load: seconds of blocking work. Run inline they
    // froze the GUI for ~3 s on every source click. Both now run on a worker
    // thread; the GUI polls each frame and applies the result. The device
    // itself is only ever touched by the GUI thread once the future resolves,
    // so no locking is needed beyond the future's own synchronization.
    struct SoapyOpenResult {
        std::unique_ptr<cascade::source::SoapySource> dev;  // null on failure
        std::string args;
        std::string error;
        int row = -1;
        double requestRateHz = 0.0;
    // The frequency the user was listening to when they changed device.
    //
    // A newly opened radio sits wherever its driver defaults to - an RTL-SDR
    // comes up at 100 MHz - so without carrying this across, changing device
    // silently retunes the receiver and the audio stops. Captured before the
    // switch because by the time the open finishes, the old source is gone.
    double keepCenterHz = 0.0;
    };
    std::future<std::vector<cascade::source::SoapyDeviceInfo>> soapyScanFuture_;
    std::future<SoapyOpenResult> soapyOpenFuture_;
    bool soapyScanPending_ = false;
    bool soapyOpenPending_ = false;
    std::string soapyBusyLabel_;  // device name shown while an open is in flight

    // Paces hardware retunes — see retuneSourceHz. 50 ms: invisible against
    // the wheel gesture, one apply per notch burst instead of one per frame.
    cascade::core::RetuneCoalescer retuneCoalescer_{50.0};

    // Source-selection sequence number, incremented by EVERY install of a
    // source into the pipeline (generator, IQ file, or a resolved device).
    // soapyOpenReqGen_ records the value an in-flight open was requested at;
    // asyncOpenStillWanted() compares the two when it resolves. See the
    // predicate's comment above for why a counter and not a flag.
    std::uint64_t sourceGen_ = 0;
    std::uint64_t soapyOpenReqGen_ = 0;

    // Drains a pending device open OFF the GUI thread at shutdown. See the
    // definition for the semantics chosen and what they cost.
    void reapPendingSoapyOpen();
    // The same for a pending device SCAN. Separate because the futures are
    // separate and either may be in flight alone; the definition explains why
    // this reaper has nothing to release where the open reaper has a handle.
    void reapPendingSoapyScan();

    // Consumes finished scan/open futures; called once per frame.
    void pollSoapyAsync();
    // Applies a resolved open on the GUI thread (panel mirrors, gain priming,
    // pipeline install). Takes ownership of r.dev.
    void finishSoapyOpen(SoapyOpenResult r);
    // Combo selection. -1 means "active device no longer in the list" (a
    // Refresh dropped it); the preview then falls back to the active source
    // name. Distinct from the ACTIVE source: selecting "IQ file" only shows
    // the path controls — the pipeline keeps its source until Open succeeds.
    int sourceSel_ = 0;
    char iqPath_[512] = "";     // InputText buffer for the IQ file path
    std::string sourceError_;   // red text under the Source controls; "" = none
    // Non-owning view of the SoapySource installed in the pipeline (the
    // pipeline owns it via setSource). Null whenever the active source is not
    // Soapy; must be nulled BEFORE any setSource that destroys the object.
    cascade::source::SoapySource* soapy_ = nullptr;
    std::string soapyArgs_;     // args of the open device (re-find on Refresh)
    int soapyRateIndex_ = 1;    // index into the 1/2/4/8 MS/s combo; 2M default
    std::vector<std::string> soapyGainNames_;  // listGainNames() at open
    std::vector<float> soapyGainsDb_;          // slider mirrors, one per name
    bool soapyAgcSupported_ = false;
    bool soapyAgc_ = false;

    // --- Frequency scale + view interaction state (P5) -----------------------
    // ONE scale owns the x <-> Hz <-> bin mapping for both center panels, fed
    // every frame from the active source's center readback and the pipeline's
    // DSP input rate, so spectrum, waterfall, axis strip and VFO overlay can
    // never disagree about what frequency a pixel column shows.
    FreqScale scale_;
    // Last REQUESTED VFO bandwidth (Hz): combo presets and band-edge drags
    // both land here, and this is what the overlay and the config store use.
    // (The combo keeps showing its last preset after an edge drag — the combo
    // is a preset picker, not a readback; the overlay is the truth.)
    double vfoBandwidthHz_ = 150000.0;
    enum class VfoDrag { None, Center, EdgeLow, EdgeHigh };
    VfoDrag vfoDrag_ = VfoDrag::None;
    // mouseHz - band center at grab time, so a center drag never makes the
    // band jump to put its center under the cursor.
    double vfoGrabDeltaHz_ = 0.0;
    bool wfPanning_ = false;  // horizontal waterfall click-drag in progress

    // --- Config persistence state (P5) ----------------------------------------
    std::string configPath_;       // empty = persistence disabled (hermetic)
    bool configAnnounce_ = false;  // print "config applied: ..." (test hook)
    // The ACTIVE source's kind as the config store spells it. Tracked at each
    // successful switch because the pipeline does not expose source identity.
    std::string sourceKind_ = "siggen";  // "siggen" | "file" | "soapy"
    // RX antenna ports the open device offers, and the one selected. Empty
    // until a Soapy device is opened. Persisted, because which port carries
    // the antenna is a property of the user's cabling, not of a session.
    std::vector<std::string> soapyAntennas_;
    std::string soapyAntenna_;
    std::string iqOpenPath_;  // last successfully opened IQ file (persisted;
                              // iqPath_ is just the edit buffer)
    cascade::core::AppConfig savedCfg_;    // what the config file holds now
    cascade::core::AppConfig pendingCfg_;  // debounce comparator
    double lastChangeTimeS_ = -1.0;  // glfwGetTime() of the last observed
                                     // change; < 0 = nothing pending

    // --- Recorder state (P6) --------------------------------------------------
    // Two independent Recorder instances so IQ and audio takes can run
    // simultaneously (each records ONE kind at a time by its contract). The
    // pipeline holds non-owning pointers to them only while a take is live;
    // stop*Recording clears the pointer before stopping the recorder.
    cascade::core::Recorder iqRecorder_;
    cascade::core::Recorder audioRecorder_;
    std::string recordDir_;    // %USERPROFILE%/Documents/SDR-recordings
    std::string recordError_;  // red text in the Recorder section; "" = none
    double iqRecordStartS_ = 0.0;     // ImGui::GetTime() at take start, for
    double audioRecordStartS_ = 0.0;  // the elapsed-wall-time readout
    // Input rate the live IQ take's WAV header was written for. A rate-follow
    // change (source switch, Soapy rate change) finalizes the take: a WAV
    // whose header rate disagrees with its samples would replay detuned.
    double iqRecordRateHz_ = 0.0;

    // --- Bookmarks state (P6) --------------------------------------------------
    // Loaded at startup from FreqManager::defaultPath() and saved after every
    // mutation — but ONLY when config persistence is enabled: hermetic runs
    // (empty configPath_, i.e. every --frames/--selftest CI run) leave
    // bookmarkPath_ empty and never read or write the user's bookmark file.
    cascade::core::FreqManager freqMgr_;
    std::string bookmarkPath_;   // empty = bookmark persistence disabled
    std::string bookmarkError_;  // red text in the Bookmarks section
    char bookmarkName_[128] = "";  // editable name for the next "Add current"

    // --- Scanner state (P6) -----------------------------------------------------
    // The Scanner itself is a pure state machine (core/scanner.hpp); these
    // mirrors exist because ImGui edits by pointer. Defaults come from
    // Scanner::Params's own member initializers so the two can never drift.
    // --- P7 feature state -----------------------------------------------------
    // Panel mirrors for the pipeline's stereo / NR / notch settings (ImGui
    // edits by pointer). Defaults match AppConfig's, which match the
    // pipeline's own construction defaults, so the three can never disagree
    // before the first user click.
    bool stereoEnabled_ = true;
    bool nrEnabled_ = false;
    float nrStrength_ = 0.5f;
    bool notchEnabled_ = false;
    float notchFreqHz_ = 1000.0f;
    float notchQ_ = 30.0f;
    bool autoNotch_ = false;
    bool bandPlanOverlay_ = true;

    // Band plan: OPTIONAL display data merged from
    // BandPlan::defaultDir() at construction. A missing directory is the
    // normal case for a run-from-build-tree session and is silent — there is
    // simply no overlay. A directory that EXISTS but fails to parse keeps its
    // reason here and shows it in the Display section, because that one is a
    // user-visible mistake worth reporting.
    cascade::core::BandPlan bandPlan_;
    std::string bandPlanError_;

    // Plugin host: scanned once at construction and on Rescan. Owns the
    // loaded modules, so it must outlive nothing in particular here — but it
    // is declared before the pipeline-dependent members so it unloads last.
    cascade::core::PluginHost pluginHost_;
    // Drives the loaded decoders with real audio. Declared AFTER pluginHost_
    // so it is destroyed BEFORE it: the runner's destructor calls each
    // plugin's destroy(), which is code inside a module the host unmaps.
    cascade::core::PluginRunner pluginRunner_;
    // GUI-side plugin capabilities: map targets, plugin windows, host
    // services. Declared after pluginHost_ for the same destruction-order
    // reason as pluginRunner_.
    cascade::core::PluginUi pluginUi_;

    // --- Per-plugin map pages ---------------------------------------------
    // ONE MAP PAGE PER PLUGIN THAT HAS A TRACK INSTANCE, replacing the single
    // "Map" window that drew every plugin's targets merged — which is why
    // switching from the Satellites plugin to ADS-B still showed "the
    // satellite map". Capability decides the page set, not content: an ADS-B
    // page with no aircraft decoded yet still exists, because "the page is
    // there but empty" answers the user's question and "the page is missing"
    // does not.
    //
    // Each page owns its own MapView, so view centre, zoom, follow and
    // selection are all per-page — that separation is the point of the
    // feature. Pages are created lazily the first frame their plugin appears
    // and never destroyed while the app runs; rescanPlugins() clears them the
    // way it clears closedWindows_, and they rebuild from mapPagesSaved_ on
    // the next frame.
    struct MapPage {
        std::string plugin;  // display name, as HostTrack::plugin carries it
        std::unique_ptr<MapView> view;
        bool open = false;
        // The page has self-opened once already this session. The first
        // visible track opens the page exactly once; this flag is what stops
        // it re-opening after the user closes it.
        // The self-open EDGE's memory (see core/plugin_ui.hpp's
        // mapSelfOpens): whether this plugin had a visible target last
        // frame. Never persisted - a restart is a quiet gap by
        // definition, so a session opens on its first arrival exactly
        // the way the single map always did.
        bool hadVisible = false;
        // THE USER CLOSED THIS PAGE, AND MEANT IT ACROSS RESTARTS.
        //
        // hadVisible is deliberately not persisted, on the reasoning that a
        // restart is a quiet gap and a session should open on its first
        // arrival. That is right for a plugin whose targets ARRIVE - an
        // aircraft is heard or it is not. It is wrong for one that COMPUTES
        // them: the satellite tracker propagates from stored elements, so it
        // has a full sky of visible targets on the first frame of every
        // launch, with no radio and no user action. The edge therefore fired
        // at start-up every time and reopened a map the user had closed,
        // which is exactly what they reported.
        //
        // Set when a page is created from a saved entry that says closed, and
        // cleared the moment the user opens the page by any route, so a close
        // survives a restart while every deliberate reopen still works.
        bool selfOpenSuppressed = false;
        // The window's rectangle: seeded at creation from the saved entry (or
        // the legacy single-window rectangle), read back from ImGui every
        // frame the window is drawn, and written to AppConfig::mapPages.
        // Zero width/height means nothing saved — default placement.
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };
    // Creation order, which is the plugins' load order — that is what keeps
    // the default cascade of fresh pages stable across launches.
    std::vector<MapPage> mapPages_;
    // AppConfig::mapPages as loaded, re-synced from the live pages by
    // syncMapPagesToSaved(). Kept separately from mapPages_ so an entry for a
    // plugin that is not installed this session survives the session's saves.
    std::vector<cascade::core::AppConfig::MapPage> mapPagesSaved_;
    // Scratch for one page's filtered tracks and paths, reused across pages
    // so the per-frame filter allocates nothing in the steady state. The
    // per-plugin caps in PluginUi bound what lands here.
    std::vector<cascade::core::HostTrack> pageTracks_;
    std::vector<cascade::core::HostPath> pagePaths_;
    // --- Anonymous usage reporting (opt-in; see PRIVACY.md) ----------------
    // Counters for the session in progress, journalled to the config at exit
    // and sent at the NEXT start-up. Nothing here is transmitted unless the
    // user has turned reporting on.
    cascade::core::TelemetryReporter telemetryReporter_;
    // "Running now" beats: a minimal ping every five minutes while the app is
    // open, only while reporting is on. See HeartbeatSender in telemetry.hpp.
    cascade::core::HeartbeatSender telemetryHeartbeat_;
    bool telemetryEnabled_ = false;
    std::string telemetryInstallId_;
    std::uint64_t telemetryLaunches_ = 0;
    std::uint64_t telemetryCrashes_ = 0;
    bool telemetryCleanExit_ = false;   // true only on the normal shutdown path
    double telemetrySessionStart_ = 0.0;                  // glfwGetTime at start
    // Time in the current mode. A plain "since" mark cannot be used here: the
    // accrual runs once per frame, and a per-frame delta truncated to whole
    // seconds is always zero, which is what kept modeSeconds empty.
    cascade::core::SecondAccrual telemetryModeAccrual_;
    std::map<std::string, std::uint64_t> telemetryModeSeconds_;
    std::vector<std::string> telemetryPanels_;
    // Called once per frame, so the seconds land against the mode that was
    // actually running rather than the last one selected.
    void telemetryAccrueMode();
    void telemetryNotePanel(const char* name);
    // Reads the previous session out of the config, counts a crash if it
    // never finished, and sends its report. Start-up only.
    void telemetryStartup(const cascade::core::AppConfig& cfg);
    // Builds this session's report into `cfg` for the next start-up to send.
    void telemetryJournal(cascade::core::AppConfig& cfg);
    // "Usage reporting" settings section: the opt-in switch and what it sends.
    void drawUsageReportingSection();
    bool privacyNoticeOpen_ = false;

    // -----------------------------------------------------------------------
    // Diagnostics (see core/crash_handler.hpp, core/hang_watchdog.hpp)
    // -----------------------------------------------------------------------
    //
    // The watchdog is fed once per rendered frame from run(). It is the only
    // thing in this application that can see a hang - and every fault this
    // product has actually shipped was a hang, not a crash.
    cascade::core::HangWatchdog watchdog_;
    // Where reports go, decided in main() so that a bounded CI run leaves
    // nothing on the machine. Empty means the watchdog still runs (a recovered
    // stall is still logged) but writes no file.
    std::string diagCrashDir_;
    // --diag-stall N: wedge the frame loop for N ms, once, on frame 60. The
    // only way to prove the shipped threshold fires against the real loop.
    int diagStallMs_ = 0;
    // --diag-toggle on|off: flip the diagnostics switch on frame 30. See
    // setDiagToggle().
    int diagToggle_ = 0;
    // The whole diagnostics switch - crash handler, log AND watchdog - in one
    // place. The checkbox, the arming path in run() and the test hook all go
    // through it; they used to disagree about the watchdog.
    void applyDiagnosticsEnabled(bool on);
    // Set for the ONE heartbeat after a deliberate stall, so a stall the test
    // asked for cannot become "the worst frame gap this build measured".
    bool diagSkipNextGap_ = false;
    // Rebuilds the report context out of state the application already has -
    // mode, source, rate, radio model, loaded plugins with versions. Nothing
    // here is re-derived.
    void refreshDiagContext();
    std::size_t diagPluginCount_ = static_cast<std::size_t>(-1);
    // The previous session did not reach its clean-exit save. Reuses
    // telemetryCleanExit, which already detects exactly this, and is the
    // trigger for offering a report on this start.
    bool lastRunUnclean_ = false;
    bool diagOfferOpen_ = false;

    // --- Sending a captured report (see core/crash_upload.hpp) -------------
    //
    // ON THE NEXT START, NEVER FROM THE FAULT HANDLER. The process that faulted
    // could not safely open a socket; this one can. The sweep runs on a
    // background thread the moment the frame loop is armed, and is cancelled
    // and joined before the clean-exit save - which is what keeps a server that
    // accepts and never answers from delaying shutdown by so much as a frame.
    cascade::core::CrashUploader crashUploader_;
    // Dedup and rate-limit memory, read from the config at start and written
    // back after the sweep. Persisted because a crash loop IS a sequence of
    // runs: a limit held only in memory would reset on every restart.
    cascade::core::UploadPolicyState crashUploadState_;
    // False unless a sweep was actually started on this run. Without it, a run
    // in which the sweep never ran (diagnostics off) would journal a DEFAULT
    // state back over the user's real one and quietly reset the crash-loop
    // limiter - so switching diagnostics off for one session would re-arm the
    // sender for the next.
    bool crashUploadSwept_ = false;
    void crashUploadStart();
    void crashUploadFinish();
    // "Diagnostics" settings section: where the log is, what a report carries,
    // and the one-click bundle.
    void drawDiagnosticsSection();
    // Shown once, at the start of the run AFTER an unclean exit. A crash
    // handler can write a report but it cannot ask anything - by the time it
    // runs there is no user interface left to ask with. This is the ask.
    void drawDiagnosticsOffer();
    void copyDiagnosticsBundle();
    std::string diagBundleStatus_;
    bool diagnosticsEnabled_ = true;
    bool diagnosticsMinidump_ = false;

    // Who each map target actually is, answered by a track-info plugin
    // (registration, type, operator). Inactive when none is installed.
    TrackInfoCache trackInfo_;
    // Map imagery from a basemap plugin, as GL textures. Inactive - and the
    // map is the built-in coastline - unless such a plugin is installed, which
    // is the shipped configuration. Declared after pluginHost_ so it detaches
    // before the modules it borrows tiles from are unmapped.
    BasemapCache basemap_;
    // The LEGACY single-window rectangle, seeded from AppConfig at start-up
    // and never changed after: it is only the default rectangle for a page
    // with no saved mapPages entry of its own (staggered per page index so
    // pages do not stack exactly). Kept so an install upgrading from the
    // one-window map reopens its pages where that window used to sit. The
    // legacy config keys are read but no longer written — see AppConfig.
    // All zero means nothing was ever saved (or the config sanitizer rejected
    // what was), which is what selects the monitor-derived default instead.
    int mapWinW_ = 0;
    int mapWinH_ = 0;
    int mapWinX_ = 0;
    int mapWinY_ = 0;
    // The receiver's own position, seeded from AppConfig and written back to
    // it. This used to be two static locals beside the "Set RX here" button,
    // which meant it died with the process: range rings had to be re-entered
    // every launch, and a distance column built on that would have measured
    // every target from 0N 0E. See AppConfig::rxPositionSet for why an
    // explicit flag rather than a sentinel coordinate.
    //
    // The two INPUT fields are separate from the applied position on purpose:
    // typing a latitude must not move the range rings until the button is
    // pressed, or every intermediate keystroke would be a different receiver.
    // THE RADAR UNIT'S LEASE ON THE DISPLAY. foxsdr-radar.exe is a separate
    // desktop application showing the same receiver, so while it is up this
    // window puts itself away - and the lease, not a flag, is what decides:
    // the radar renews it every few seconds, and this window comes back on
    // its own if the renewals stop. A radar that crashes must not be able to
    // leave the user with no way back to their receiver.
    // Three missed renewals at the radar's four-second cadence. Long enough
    // that a stalled frame or a paused debugger does not pull the desktop
    // back out from under a running radar; short enough that a crash is a
    // pause, not a lockout.
    static constexpr double kRadarLeaseSeconds = 12.0;
    void setRadarHoldsDisplay(bool held);
    void applyRadarWindowVisibility();
    // Set for the one frame that gives the display back, so the torn-off
    // windows are shown once rather than forced visible for ever.
    bool radarRestoreWindows_ = false;
    // Scope mode hides the torn-off windows the same way the radar lease does;
    // these two carry the edge that puts them back on the way out.
    bool scopeWasOn_ = false;
    bool scopeLeftThisFrame_ = false;

    GLFWwindow* mainWindow_ = nullptr;
    bool radarHoldsDisplay_ = false;
    double radarLeaseExpiry_ = 0.0;

    bool rxSet_ = false;
    double rxLat_ = 0.0;
    double rxLon_ = 0.0;
    double rxLatInput_ = 0.0;
    double rxLonInput_ = 0.0;
    // How far anything has been heard, per five-degree bearing bucket. Fed
    // once per frame from the visible tracks - which is every track source at
    // once, not just ADS-B - and drawn over the map when coverageShow_ is on.
    // Session-scoped by design; see CoverageMap.
    CoverageMap coverage_;
    bool coverageShow_ = false;
    // The two trail switches, pushed into every page's MapView each frame and
    // persisted (AppConfig::mapTrails, AppConfig::mapTrailAltitudeColours).
    // They live here rather than in a MapPage because they are a statement
    // about how this user wants trails drawn, not about one plugin's window:
    // two map pages disagreeing about whether a trail is coloured by altitude
    // would be two answers to one question.
    bool mapTrails_ = true;
    bool mapTrailAltColours_ = true;
    int mapTrailStyle_ = 0;  // 0 line, 1 ribbon - see AppConfig::mapTrailStyle

    // --- the ADS-B radar scope ----------------------------------------------
    // The renderer, and the two pieces of state that outlive it. The RANGE
    // lives here rather than only in the view because it is persisted
    // (AppConfig::scopeRangeNm) and because two controls set it - the stepper
    // in the scope's own bar and the wheel over the face - so the value is
    // pushed in before each draw and read back after it.
    //
    // Held BY VALUE, unlike the map pages' views: there is exactly one scope
    // for the application (it is a mode of the main window, not a window per
    // plugin), and it owns nothing that needs a stable address.
    ScopeView scope_;
    bool scopeMode_ = false;
    int scopeRangeNm_ = kScopeDefaultRangeNm;
    // Whether ANYTHING asked the basemap for a tile this frame - the map pages
    // and, now, the scope. BasemapCache::endFrame() evicts every tile nothing
    // asked for, so it must run exactly once per frame and only AFTER every
    // surface that wants tiles has asked. It used to be called at the end of
    // the map-page loop, which was correct while that loop was the only asker;
    // with the scope drawing later in the same frame, an endFrame there would
    // have thrown away the scope's tiles and made it re-upload every one of
    // them, every frame.
    bool basemapUsedThisFrame_ = false;
    // Sort state for the track list, held here because the sort menu above the
    // table is the only thing that sets it and the table below has to be
    // ordered by it every frame. SEEDED FROM THE SAME CONSTANT the menu opens
    // on, so the opening order the code states is the one the screen shows -
    // written separately, the two disagreed and the constant here was dead.
    TrackSortKey trackSortKey_ = trackSortKeyForMenuIndex(kTrackSortDefaultIndex);
    bool trackSortAscending_ = true;
    // THE TARGET THE DETAILS WINDOW IS SHOWING, by id, empty when it is shut.
    // An id rather than a pointer or an index because the host's track vector
    // is rebuilt on every poll: an index would point at a different aircraft a
    // frame later, and a pointer would dangle. An id that is no longer in the
    // vector is a target that has gone, which the window says rather than
    // silently closing - a detail view that vanishes on its own looks like a
    // crash.
    std::string detailsTrackId_;
    bool detailsOpen_ = false;

    // WINDOWS THE USER HAS CLOSED, by ImGui id. The decoded-image windows and
    // the plugin-declared panels are opened BY their content arriving rather
    // than by a menu, so they had no close state at all and were drawn with a
    // null p_open. That was survivable while torn-off viewports were
    // undecorated - there was no close button to press. Once they carry the
    // operating system's own frame (see ConfigViewportsNoDecoration in
    // run()), a null p_open means a real X that silently does nothing, which
    // is worse than no X at all.
    //
    // Closing one keeps it closed for the session; a plugin rescan clears the
    // set, because that is the point at which the whole set of panels and
    // decoders is rebuilt and "closed" no longer refers to the same thing.
    std::set<std::string> closedWindows_;
    // Decoded images, refreshed from PluginRunner once per frame. Owned HERE
    // rather than by the runner because it is written only when a decoder
    // produces a new picture: keeping the GUI's copy out of the runner is what
    // lets a megapixel frame be copied once per change instead of once per
    // rendered frame.
    std::vector<cascade::core::HostImage> pluginImages_;
    // GL textures for plugin images, one per image, keyed by index. Uploaded
    // only when the plugin says the pixels changed. imageTexPlugin_ records
    // whose picture each slot currently holds, so a rescan that puts a
    // different plugin in a slot cannot leave the old texture on screen.
    // The maker's badge on the scope face, uploaded ONCE from the RGBA icon
    // compiled into this binary (resources/icon/foxsdr_icon_rgba.hpp) - the
    // same pixels the window and taskbar use, so the badge cannot drift from
    // the application's own identity. 0 until the scope is first drawn.
    unsigned int scopeBadgeTex_ = 0;

    std::vector<unsigned int> imageTex_;
    std::vector<std::uint64_t> imageTexRev_;
    std::vector<std::string> imageTexPlugin_;
    std::string imageSaveNote_;
    // Decoded output, newest last, bounded. The panel is a tail, not an
    // archive; the recorder is where a permanent copy belongs.
    std::deque<cascade::core::DecodedLine> decoderLog_;
    static constexpr std::size_t kDecoderLogMax = 500;
    bool decoderAutoScroll_ = true;
    // The output window opens itself the first time a decoder says something,
    // and not before — the same rule the map follows. Once the user closes it
    // that stays closed, which is why the "ever opened" latch exists.
    bool decoderWindowOpen_ = false;
    bool decoderWindowEverOpened_ = false;
    std::string pluginDir_;

    // --- Retirement enforcement (P11) -----------------------------------------
    //
    // WHY THERE IS A QUARANTINE STEP AND NOT JUST A RED ROW. A retired plugin
    // that is merely painted red is still mapped into this process: its
    // DllMain has run, its static initialisers are live, and its decoder
    // callbacks are one click away. The whole point of the feature is that the
    // stale CODE does not execute, so enforcement has to happen BEFORE
    // LoadLibrary, not after.
    //
    // PluginHost::scan() takes a DIRECTORY and loads every ".dll" in it (there
    // is no per-file load entry point, and plugin_host is fixed), so the only
    // way to keep one file out of a scan is to make it not look like a plugin
    // for the duration. rescanPlugins() therefore runs one ordered sequence:
    //
    //   unloadAll -> un-quarantine everything -> loadInventory (the disk is
    //   now complete, so reconciliation and planUpdates see the truth) ->
    //   blockedPlugins -> rename each blocked file to "<name>.dll.disabled"
    //   -> scan.
    //
    // The renamed file has no plugin extension, so PluginHost never sees it
    // and LoadLibrary is never called on it - not even once, not even at
    // startup. The rename is reversible and local: drop the floor (or update
    // the plugin) and the next rescan puts the name back.
    //
    // FAIL CLOSED on a failed rename. If a blocked file cannot be moved aside
    // (locked, read-only), the scan is SKIPPED entirely and the reason is
    // shown: loading every other plugin while the retired one loads with them
    // would be exactly the state this exists to prevent.
    static const char* pluginQuarantineSuffix();  // ".disabled"
    // Renames every "<name>.dll.disabled" back to "<name>.dll". A leftover
    // whose live name already exists (an update landed while it was aside) is
    // DELETED instead - it is a copy of a file this code renamed, and keeping
    // stale bytes around under a hidden name helps nobody.
    bool restoreQuarantinedPlugins(std::string& error);
    // Renames every currently blocked plugin file out of the scan's way.
    bool quarantineBlockedPlugins(std::string& error);
    // The manifest + cached policy, as of the last rescan and with the whole
    // directory present (see the ordering above). Everything downstream -
    // the red rows, the badge, planUpdates - reads THIS, not a fresh
    // loadInventory, so nothing ever plans against a quarantined file.
    cascade::core::PluginInventory pluginInventory_;
    std::vector<cascade::core::BlockedPlugin> pluginBlocked_;
    // Why enforcement could not be completed (red, above the list). Empty in
    // the normal case, which is every case where nothing is retired.
    std::string pluginEnforceError_;
    // What the catalogue currently offers over what is installed. Pure, cheap,
    // and empty until the user has fetched a catalogue this session - there is
    // no startup fetch, so an update can only ever be offered on request.
    std::vector<cascade::core::PluginUpdate> plannedPluginUpdates() const;
    // AppConfig::pluginLastUpdateCheck, stamped when a catalogue fetch
    // succeeds and persisted with the rest of the config.
    std::int64_t pluginLastUpdateCheck_ = 0;
    // AppConfig::pluginTuneAllowed. THIS is the durable copy of the grant, and
    // PluginUi holds the live one: PluginUi::clear() drops its set with the
    // instances on every rescan, so the permission has to survive somewhere
    // that a rescan does not touch.
    std::vector<std::string> pluginTuneAllowed_;
    // AppConfig::pluginsStopped. The durable copy of "the user stopped this
    // plugin", by module file name, held here for the same reason the grants
    // are: PluginRunner and PluginUi are rebuilt on every source change and
    // cleared on every rescan, so the decision has to live somewhere neither
    // touches.
    std::vector<std::string> pluginsStopped_;
    // AppConfig::pluginMuteOverride. Held here for the same reason: the value
    // is a decision about a plugin, and the objects that act on it are torn
    // down and rebuilt underneath it.
    std::vector<std::string> pluginMuteOverride_;
    // What the mute policy is evaluated against, rebuilt whenever the plugin
    // set, a stop, or an override changes (see rebuildMuteStates).
    std::vector<cascade::core::MutePlugin> muteStates_;
    // Display names of the plugins currently holding the audio down. Empty
    // when nothing is. Read by the Sinks panel, the banner, the popup and the
    // web snapshot, so all four say the same thing.
    std::vector<std::string> mutedBy_;
    // Their module file names, in the same order. Kept beside the names rather
    // than looked up from them because the popup's Stop button must act on
    // EXACTLY the plugins the sentence above it named.
    //
    // Measured on the running application: stopping "every running plugin that
    // mutes" instead stopped AIS as well, which was running on a band 900 MHz
    // away and muting nothing - a dialog that said "ADS-B" and switched off
    // two decoders.
    std::vector<std::string> mutedByKeys_;
    // Was the receiver on a muting plugin's preset LAST frame. The popup fires
    // on the true -> false edge (cascade::core::tuneAwayEdge) and re-arms only
    // when this goes true again, which is what stops a modal reappearing on
    // every frame of a slow tune.
    bool mutePrevOnPreset_ = false;
    // The user tuned away and chose "Keep it running". The audio stays muted -
    // that is the model they were offered, sound comes back when the plugin
    // stops - and the banner stays up until it does, or until the tune returns
    // to a preset.
    bool muteKeptRunning_ = false;
    // WHAT THE POPUP IS ASKING ABOUT: the plugin names and keys captured when
    // the edge opened it, and never re-read from the live decision afterwards.
    // See cascade::core::advanceMutePopup for the measured failure that made
    // this a captured value rather than a call to muteSubjectText().
    cascade::core::MutePopupSubject mutePopup_;
    // Set when mutePopup_ goes open, consumed by the next drawMutePopup().
    // Deferred rather than calling ImGui::OpenPopup from the tune path because
    // the evaluation runs before the frame's windows exist, and a popup opened
    // against no ID stack is a popup that never appears.
    bool mutePopupQueued_ = false;
    // Bound on how many one-click presets a single plugin may put on the
    // panel. A plugin is third-party code and a list this long is not a menu.
    static constexpr std::uint32_t kMaxPresetsPerPlugin = 16u;
    // What the last preset click did, shown under the list — a receiver that
    // moved with no acknowledgement reads as a button that did nothing.
    std::string presetNote_;

    // --- Plugin browser (P9) --------------------------------------------------
    //
    // THE PRIVACY PROMISE. Nothing here contacts the catalogue origin until
    // the user presses Browse. Not at startup, not when the section is
    // expanded, not on a config restore that remembers the browser was open.
    // The published catalogue's README makes that promise to plugin authors
    // and users, and a paid product has to keep it, so the ONLY caller of
    // startCatalogFetch() is a button.
    //
    // THREADING. Identical in shape to the Soapy scan/open pair above and for
    // the same reason: a catalogue fetch is a TLS handshake plus an HTTP
    // round trip, and an install is a multi-megabyte download — seconds of
    // blocking work that would otherwise freeze the window and stall the
    // radio. Both run on a worker via std::async; the GUI polls the future
    // once per frame (pollPluginAsync) and applies the result on the GUI
    // thread. The worker touches pluginRepo_ and nothing else the GUI reads,
    // except progress()/cancel(), which are atomics for exactly this.
    cascade::core::PluginRepo pluginRepo_;

    // Result carriers. The catalogue entries are COPIED out of the repo on
    // the worker thread so the GUI never reads pluginRepo_.entries() while a
    // transfer could be rewriting it.
    struct CatalogFetchResult {
        bool ok = false;
        std::vector<cascade::core::PluginCatalogEntry> entries;
        std::string error;
        // cacheCataloguePolicies() runs on the SAME worker, immediately after
        // a successful fetch: that call is the only moment a retirement floor
        // is ever written to this machine, and deferring it to "some later
        // fetch" would mean a user who browses once and never again is never
        // protected. It is done off the GUI thread because it re-hashes every
        // installed plugin. A failure here is reported, never swallowed - the
        // catalogue still loaded, but the policy the user just saw was not
        // remembered.
        std::string policyError;
    };
    struct PluginInstallResult {
        bool ok = false;
        std::string name;           // display name, for the report
        std::string installedPath;  // set on success
        std::string error;          // verbatim from PluginRepo on failure
        bool isUpdate = false;      // applyUpdate (records itself) vs install
        // recordInstall's failure, for a PLAIN install only. The file is
        // installed and verified either way; what failed is the manifest
        // write, which leaves the plugin unmanaged (and therefore fail-open,
        // never retired) until an install or a catalogue fetch repairs it.
        std::string recordError;
    };
    std::future<CatalogFetchResult> catalogFuture_;
    std::future<PluginInstallResult> installFuture_;
    bool catalogPending_ = false;
    bool installPending_ = false;
    std::string installBusyName_;  // shown in "Downloading <name>..."

    // Starts the catalogue fetch on a worker. https:// goes through
    // PluginRepo::fetchIndex; a path with no "://" scheme is read from disk
    // and parsed with the same parseIndex — see AppConfig::pluginCatalogueUrl
    // for why the local form exists and why it grants nothing extra.
    void startCatalogFetch();
    // Starts one install on a worker. Takes the entry BY VALUE: the worker
    // outlives the frame that spawned it, and catalog_ can be replaced by a
    // Refresh in the meantime.
    void startInstall(cascade::core::PluginCatalogEntry entry);
    // Starts ONE user-requested update on the same worker slot as an install
    // (PluginRepo has a single progress/cancel pair, so only one transfer runs
    // at a time). The plan's `entry` aliases catalog_, which a Refresh can
    // replace mid-transfer, so the worker takes its own COPY of the entry and
    // re-points the plan at it before calling applyUpdate.
    void startUpdate(const cascade::core::PluginUpdate& u);
    // Consumes finished catalogue/install futures; called once per frame from
    // drawUi, right beside pollSoapyAsync.
    void pollPluginAsync();
    // True if the catalogue entry's file name already exists in the plugins
    // directory, comparing against every record PluginHost produced — loaded
    // AND refused — and against the manifest's own records. A refused DLL
    // still occupies the name, so treating it as "not installed" would offer
    // an install that could not replace it; and a RETIRED plugin has been
    // renamed out of the scan, so the host has no record of it at all — the
    // manifest is what keeps it from looking uninstalled and sending the user
    // down an Install path when Update is the remedy.
    bool catalogEntryInstalled(const cascade::core::PluginCatalogEntry& e) const;

    // THE INSTALL GATE, as one named predicate so the button, the tooltip and
    // the --frames diagnostic can never disagree about it. Returns the reason
    // Install must stay disabled for catalog_[idx], or an EMPTY string when it
    // may be clicked. `acknowledged` is the state of the legal-notice
    // checkbox; it is a parameter rather than a member read so the same
    // function answers "would this be installable if the box were ticked",
    // which is what makes the gate observable in a headless run.
    std::string pluginInstallBlockedReason(int idx, bool acknowledged) const;

    // Deletes one installed plugin (see drawPluginsSection for the
    // unload-first rationale) and rescans.
    void removeInstalledPlugin(const std::string& fileName);

    // Deletes one BLOCKED (retired or ABI-mismatched) plugin. Separate from
    // removeInstalledPlugin because a blocked plugin is not on disk under its
    // own name: it has been renamed aside with pluginQuarantineSuffix(), so it
    // needs PluginRepo::removeQuarantined rather than remove().
    void removeBlockedPlugin(const std::string& fileName);

    bool pluginBrowseOpen_ = false;  // "Get plugins" view expanded
    // Whether the browser was ACTUALLY DRAWN this frame, which is not the same
    // question as pluginBrowseOpen_ now that the browser lives in its own
    // collapsible section: the flag can be true while the Plugin store header
    // is collapsed, and in that state nothing has drawn the install/remove
    // result text yet. The installed section reads this to decide whether to
    // draw it — printing it twice in one column reads as two separate
    // failures, printing it in neither loses a failed remove entirely.
    // Reset at the top of drawPluginStoreSection, which runs first.
    bool pluginBrowserDrawnThisFrame_ = false;
    char pluginUrlBuf_[512] = "";    // edit buffer for the catalogue URL
    std::string pluginCatalogueUrl_;  // committed value (persisted)
    std::vector<cascade::core::PluginCatalogEntry> catalog_;
    std::string catalogError_;   // red: fetch/parse failure, verbatim
    std::string catalogStatus_;  // neutral: "N plugins in the catalogue"
    // --- update check --------------------------------------------------------
    bool closeRequested_ = false;     // set by the updater; the run loop honours it
    bool updateCheckEnabled_ = true;
    bool updateStarted_ = false;      // one check per launch, no retry storm
    bool updateDismissed_ = false;    // "not now" hides it until next launch
    bool updatePending_ = false;      // a check or a download is in flight
    bool updateDownloading_ = false;
    cascade::core::UpdateInfo update_;
    std::string updateError_;
    std::string updateReadyPath_;     // verified installer, waiting to be run
    // The app-update transfer's own progress and cancel, NOT pluginRepo_'s.
    // The banner used to read pluginRepo_.progress(), which nothing on this
    // path ever writes — the bar sat at 0 for the whole download — and
    // pluginRepo_.cancel() could not reach a transfer started through the
    // static fetch helper, so quitting mid-update blocked in the future's
    // destructor until the download finished. One pair per transfer keeps the
    // plugin browser's bar and this one from reporting each other's bytes.
    //
    // DECLARED BEFORE THE FUTURES, deliberately: members are destroyed in
    // reverse declaration order, and the download future's destructor BLOCKS
    // until its worker returns — a worker that is still polling this flag. A
    // pair declared after the future would be destroyed while that read is in
    // flight.
    //
    // THE SAME APPLIES TO EVERY OTHER MEMBER EITHER WORKER TOUCHES, which is
    // why the three result slots below moved up here from after the futures.
    // The atomics are only what the workers READ; these are what they WRITE.
    // downloadUpdate() is handed updateResultPath_ and updateResultError_ by
    // reference and assigns to them as it goes, and checkForUpdate() does the
    // same with updateResult_ and updateResultError_ — so declared after the
    // futures they were std::string and UpdateInfo destructors running while a
    // worker thread was mid-assignment into them, a use-after-free reached
    // through a dangling `this` rather than a data race the flags could stop.
    // The blocking future destructor that makes quit slow is also the only
    // thing that makes this ordering enough: it guarantees both workers have
    // returned before anything declared above the futures is destroyed.
    std::atomic<float> updateProgress_{0.0f};
    std::atomic<bool> updateCancel_{false};
    cascade::core::UpdateInfo updateResult_;
    std::string updateResultError_;
    std::string updateResultPath_;
    std::future<bool> updateCheckFuture_;
    std::future<bool> updateDownloadFuture_;

    int catalogSel_ = -1;        // index into catalog_, -1 = nothing selected
    bool legalAck_ = false;      // legal-notice checkbox for the SELECTED entry
    std::string installReport_;  // green: last successful install/remove
    std::string installError_;   // red: last failed install/remove, verbatim
    int removeConfirmIdx_ = -1;  // installed-list row awaiting confirmation
    int blockedRemoveConfirmIdx_ = -1;  // disabled-list row awaiting confirmation

    // --- Bounded-run test hook (P9) --------------------------------------------
    // CASCADE_PLUGIN_TEST=<url-or-path>, honored ONLY by run(frames >= 0),
    // exactly like main()'s CASCADE_CONFIG_TEST: it points the browser at a
    // catalogue, opens the section, starts ONE fetch on the first frame and
    // prints a machine-readable summary of the catalogue and of every entry's
    // install-gate decision. No interactive session can be redirected by a
    // stray environment variable, and a normal --frames run prints nothing
    // extra, so the byte-identical-stdout contract is untouched.
    std::string pluginTestHook_;
    bool pluginTestStarted_ = false;
    // Frames rendered so far, published so the hook's diagnostic can name the
    // frame the fetch resolved on — which is what proves the window kept
    // rendering while the transfer was in flight.
    int frameCounter_ = 0;
    // Prints that summary from the LIVE member state (catalog_ /
    // catalogError_), not from the future's payload, so what it reports is
    // exactly what the UI is about to draw.
    void reportPluginTestResult();

    // CASCADE_PLUGIN_STATUS=1, honored ONLY by run(frames >= 0), like the hook
    // above. Prints what the ENFORCEMENT decided: how many candidates the host
    // actually mapped, what blockedCount() says, and one line per retired
    // plugin including whether the host has any record of it (mapped=0 is the
    // proof that the stale code is not in the process). It is printed at the
    // end of the run so it reflects any catalogue fetch the run performed.
    bool pluginStatusHook_ = false;
    void reportPluginStatus();

    // --- Web server mode (P11) ------------------------------------------------
    //
    // WHY THE SERVER IS FED FROM A PUBLISHED SNAPSHOT rather than reading the
    // pipeline directly. Its provider callbacks run on HTTP worker threads, and
    // two of the things a browser wants are documented GUI-THREAD-ONLY:
    // Pipeline::activeSourceName() returns a const char* valid only until the
    // next setSource, and the source's own centre-frequency readback has the
    // same contract. Calling either from a request handler would be a race that
    // shows up as a torn string or a dangling pointer under exactly the
    // conditions — a source swap — that are hardest to reproduce. So the GUI
    // thread assembles one consistent snapshot per frame under webMutex_, and
    // the providers do nothing but copy it out.
    void drawCatSection();
    void drawWebSection();
    // Copies audio produced since the last frame into the server's ring.
    //
    // The pipeline's tap is a rolling 4096-frame window with no per-reader
    // position, so "what is new" comes from audioSamplesProduced(), which is
    // monotonic and counts FRAMES in the same unit the tap returns. The
    // difference between two readings is exactly what to copy — that is what
    // makes the stream gap-free and repeat-free rather than "whatever the
    // window happened to hold". If the GUI ever stalls longer than the window
    // (85 ms at 48 kHz) the excess is unrecoverable and is dropped; the
    // listener hears a glitch, which beats hearing stale audio for ever after.
    void publishWebAudio();
    // Re-encodes decoded pictures for the browser, but ONLY when a decoder's
    // revision has actually moved: encoding a megapixel BMP every frame would
    // cost more than everything else the server does put together.
    void publishWebImages();
    // Revisions the last publish encoded, one per image slot.
    std::vector<std::uint64_t> webImageRevs_;
    // Serves the browser map's tile wants from the basemap plugin. The
    // browser's viewport drives WHICH tiles; this frame-time pump is what
    // keeps the plugin on the GUI thread, the only thread the ABI lets call
    // it: the server records requests it cannot answer, this drains a bounded
    // number per frame, fetches, encodes, publishes, and the browser retries.
    void pumpWebTiles();
    // What the last pump saw, so a plugin change or removal clears the
    // server's store instead of serving the old source's imagery as the new's.
    bool webTilesActive_ = false;
    std::string webTileAttribution_;
    // Copies the current radio state and newest spectrum frame into the
    // members below. Called once per frame from drawUi, unconditionally: the
    // panel being collapsed must not stop the browser being served.
    void publishWebSnapshot();
    // Drains the server's control queue and applies each request to the radio.
    // Called once per frame from drawUi, on the GUI thread, because that is
    // the only thread allowed to move the SOURCE — the HTTP handler validated
    // the request and queued it precisely so it would not have to.
    //
    // Applying here rather than in the handler also keeps the panel mirrors
    // (modeIndex_, vfoOffsetKhz_, squelchDb_ ...) in step, so a change made
    // from a browser shows up on the desktop window and in the debounced
    // config save exactly as though it had been clicked.
    // Drains BOTH servers' queued requests — the browser's and CAT's — and
    // applies them through one body of code, so the two can never drift.
    void applyWebControls();
    // Starts or stops the CAT server to match the current configuration, and
    // records why in catStatus_ when it will not start.
    void refreshCatServer();
    // Applies webCfg_ to the server: starts, restarts or stops it, and puts
    // the outcome in webError_ / webNote_. The ONE place that calls
    // WebServer::start, so the panel, the config restore and the password
    // dialog cannot drift apart.
    void applyWebSettings();
    // Hashes `password` and stores the record, then re-applies. An empty
    // string CLEARS the password, which the policy will refuse if the binding
    // is not loopback — deliberately, since that is the user removing the only
    // thing protecting an exposed receiver.
    void setWebPassword(const std::string& password);

    cascade::net::CatServer catServer_;
    // Shown in the panel: empty while things are as configured, otherwise the
    // reason the server is not listening (a port already held by a real
    // rigctld being much the most common).
    std::string catStatus_;
    // Panel mirrors, assembled into an AppConfig by currentConfig(). There is
    // no separate dirty flag anywhere in this window: the debounced save
    // compares the live state against the last saved one, so a setting is
    // persisted purely by appearing in currentConfig() and configsEqual().
    bool catEnabled_ = false;
    bool catBindAll_ = false;
    int catPortMirror_ = static_cast<int>(cascade::net::kDefaultCatPort);

    cascade::net::WebServer webServer_;
    cascade::net::WebServerConfig webCfg_;
    // Panel mirrors (ImGui edits by pointer). webBindChoice_: 0 = this machine
    // only, 1 = every network interface. A specific interface address loaded
    // from the config that matches neither shows as choice 2 and is left alone.
    int webBindChoice_ = 0;
    int webPortMirror_ = cascade::net::kDefaultWebPort;
    char webUserBuf_[64] = "admin";
    char webPassBuf_[128] = "";
    char webPassConfirmBuf_[128] = "";
    std::string webError_;   // red: the policy's refusal, or a bind failure
    std::string webNote_;    // neutral/green: "serving at http://..."
    // Edits are staged and applied on a button rather than taking effect as
    // they are typed. Two reasons: restarting the listener on every keystroke
    // of the port field is nonsense, and — the real one — a setting that
    // decides who can reach the receiver should be reviewed before it takes
    // effect, not applied halfway through being typed.
    bool webDirty_ = false;
    // Addresses of this machine's own interfaces, for the "open this on your
    // phone" hint. Filled lazily the first time the section is drawn, because
    // enumerating adapters is a syscall nobody needs on a headless run.
    std::vector<std::string> webLocalAddresses_;
    bool webAddressesScanned_ = false;

    // audioSamplesProduced() at the last publish, and the scratch buffer the
    // tap is read into (a member so a steady stream never allocates).
    std::uint64_t webAudioLastProduced_ = 0;
    std::vector<float> webAudioBuf_;

    mutable std::mutex webMutex_;
    cascade::net::RadioStatus webStatus_;
    std::vector<float> webBins_;
    std::uint64_t webSeq_ = 0;
    double webSnapCenterHz_ = 0.0;
    double webSnapSpanHz_ = 0.0;

    cascade::core::Scanner scanner_;
    double scanStartMhz_ = cascade::core::Scanner::Params{}.startHz / 1.0e6;
    double scanStopMhz_ = cascade::core::Scanner::Params{}.stopHz / 1.0e6;
    double scanStepKhz_ = cascade::core::Scanner::Params{}.stepHz / 1.0e3;
    double scanDwellMs_ = cascade::core::Scanner::Params{}.dwellMs;
    double scanHoldMs_ = cascade::core::Scanner::Params{}.holdMs;
    double scanResumeMs_ = cascade::core::Scanner::Params{}.resumeMs;
    // Readback (center + offset) right after the last scanner-commanded
    // retune. Any later frame where the live readback differs is a tune the
    // scanner did not make — a manual tune, and the user wins (scan stops).
    double scannerExpectedAbsHz_ = 0.0;
    bool scannerHasExpected_ = false;  // false until the scan's first retune
};

}  // namespace cascade::gui
