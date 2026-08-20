// GLFW + Dear ImGui application shell.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
#include "core/scanner.hpp"
#include "gui/basemap_cache.hpp"
#include "gui/track_info_cache.hpp"
#include "core/telemetry.hpp"
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

private:
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
    // Loaded plugins with their LICENCE, refused candidates with their
    // reason, a Remove button per install, a Rescan button, the RETIRED
    // plugins as red rows, and the "Get plugins" catalogue browser
    // (drawPluginBrowser).
    void drawPluginsSection();
    // The red rows: every plugin the cached catalogue policy retires, each
    // carrying PluginRepo::pluginBlockMessage() verbatim. Drawn above the
    // installed list because a disabled plugin is the news on this panel.
    void drawBlockedPluginRows();
    // The catalogue half of the Plugins section: the URL field, the Browse /
    // Refresh button (the ONLY thing in the product that contacts the
    // catalogue origin), the in-flight progress + Cancel, the entry list and
    // the selected entry's detail pane with the install gate.
    void drawPluginBrowser();
    // The last install/remove outcome: PluginRepo's own error verbatim in
    // red, or the success line in green. Its own method because BOTH halves
    // of the section produce one (Install lives in the browser, Remove in the
    // installed list) and it must be visible whichever half the user is
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
    // The list of decoded targets down the side of the Map window: callsign
    // or id, altitude and range, with click-to-go-to and double-click to
    // follow. A map answers "where is everything"; this answers "what am I
    // hearing", and clicking answers "take me to that one".
    void drawTrackList();
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
    void setPluginTuneAllowed(const std::string& plugin, bool allowed);
    // Pushes pluginTuneAllowed_ into pluginUi_. Called after every
    // PluginUi::rebuild, because rebuild follows a clear() that drops the
    // grants along with the instances — without this a rescan silently revoked
    // every permission the user had given.
    void applyPluginTuneGrants();
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
    // The map window and every plugin-declared panel window. Drawn as their
    // own top-level windows rather than inside the menu column: a map squeezed
    // into a 300 px sidebar is not a map, and a plugin's window should be
    // movable and resizable like any other.
    void drawPluginWindows();
    // Starting position and size for a window that should be its OWN operating
    // system window rather than a panel inside the main one. `slot` staggers
    // several of them. See the definition for why the position is what decides
    // this — ImGui has no flag for it.
    void placeAsSeparateWindow(int slot);
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
    void retuneSourceHz(double centerHz);

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
    std::unique_ptr<MapView> map_;
    // --- Anonymous usage reporting (opt-in; see PRIVACY.md) ----------------
    // Counters for the session in progress, journalled to the config at exit
    // and sent at the NEXT start-up. Nothing here is transmitted unless the
    // user has turned reporting on.
    cascade::core::TelemetryReporter telemetryReporter_;
    bool telemetryEnabled_ = false;
    std::string telemetryInstallId_;
    std::uint64_t telemetryLaunches_ = 0;
    std::uint64_t telemetryCrashes_ = 0;
    bool telemetryCleanExit_ = false;   // true only on the normal shutdown path
    double telemetrySessionStart_ = 0.0;                  // glfwGetTime at start
    double telemetryModeSince_ = 0.0;                     // when the current mode began
    std::map<std::string, std::uint64_t> telemetryModeSeconds_;
    std::vector<std::string> telemetryPanels_;
    // Called on every mode change and at exit, so the seconds land against
    // the mode that was actually running rather than the last one selected.
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

    // Who each map target actually is, answered by a track-info plugin
    // (registration, type, operator). Inactive when none is installed.
    TrackInfoCache trackInfo_;
    // Map imagery from a basemap plugin, as GL textures. Inactive - and the
    // map is the built-in coastline - unless such a plugin is installed, which
    // is the shipped configuration. Declared after pluginHost_ so it detaches
    // before the modules it borrows tiles from are unmapped.
    BasemapCache basemap_;
    bool mapOpen_ = false;
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
    std::future<bool> updateCheckFuture_;
    std::future<bool> updateDownloadFuture_;
    cascade::core::UpdateInfo updateResult_;
    std::string updateResultError_;
    std::string updateResultPath_;

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
