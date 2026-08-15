// GLFW + Dear ImGui application shell.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/freq_manager.hpp"
#include "core/pipeline.hpp"
#include "core/recorder.hpp"
#include "core/scanner.hpp"
#include "gui/freq_scale.hpp"
// For SoapyDeviceInfo and the non-owning SoapySource* below; the header
// forward-declares the Soapy API types, so this pulls in no Soapy headers.
#include "source/soapy_source.hpp"

namespace cascade::gui {

// Forward declarations keep ImGui types out of this header (waterfall_view.hpp
// includes imgui.h), preserving the rule that main() never sees GUI headers.
class SpectrumView;
class WaterfallView;

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
    std::vector<cascade::source::SoapyDeviceInfo> soapyDevices_;
    bool soapyScanned_ = false;  // one lazy scan done (scanSoapy())
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
