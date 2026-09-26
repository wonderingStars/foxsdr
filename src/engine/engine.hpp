// engine.hpp - THE RECEIVER'S ENGINE: its state and, from stage 3a on, its
// machinery, moved out of the window (engine extraction stage 3a; the plan is
// docs/ENGINE-EXTRACTION.md in the foxsdr-api repository, the record of this
// stage docs/engine-stage3.md).
//
// WHAT IT OWNS. The Pipeline, the open radio and the workers that open and
// scan radios, the recorders, the transmitter, the plugin host, runner and
// host services, the patch page's runtime and its radios, the scanner, the
// bookmarks, the receiver part of the configuration, the telemetry sender,
// and the one receiver snapshot every reader answers from. The window
// (gui::AppWindow) holds a reference to one and asks it; the window keeps
// what is drawn and how.
//
// THE LINE IS ENFORCED, NOT REMEMBERED: this file and everything in
// src/engine compile into cascade_engine, which may not include a gui/
// header, imgui, GLFW or OpenGL (checked at configure time and by
// cascade_engine_link_check).
//
// THREADING. Stage 3a changes none: everything here is still called on the
// GUI thread, exactly where the window called it before (the per-frame order
// is in docs/engine-stage3.md). Stage 3b moves it onto the engine's own
// control thread; every GUI-THREAD-ONLY note in the code below is a checklist
// item for that (docs/engine-stage3.md lists them).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_ENGINE_HPP
#define CASCADE_ENGINE_ENGINE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "core/band_plan.hpp"
#include "core/config.hpp"
#include "core/freq_manager.hpp"
#include "core/gps_reader.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_radio.hpp"
#include "core/patch_runner.hpp"
#include "core/pipeline.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_repo.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/recorder.hpp"
#include "core/retune_coalescer.hpp"
#include "core/scanner.hpp"
#include "core/telemetry.hpp"
#include "core/transmitter.hpp"
#include "engine/audio_open.hpp"
#include "engine/bias_tee.hpp"
#include "engine/source_fallback.hpp"
#include "engine/tune_control.hpp"
#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/mirisdr_source.hpp"
#include "source/pluto_source.hpp"
#include "source/pluto_tx.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"
#include "source/sdrplay_source.hpp"
#include "source/soapy_source.hpp"
#include "source/soundcard_source.hpp"
#include "usb/usb_device.hpp"

// The window and its tests are named, never included: the engine does not
// depend on them. Stage 3a's window still reaches the engine's members
// directly (as friends); stage 4 replaces that with the engine API's table.
namespace cascade::gui {
class AppWindow;
struct AppWindowTestAccess;
}  // namespace cascade::gui

namespace cascade::engine {

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

private:
    friend class cascade::gui::AppWindow;
    friend struct cascade::gui::AppWindowTestAccess;

    // NO DEFAULT MEMBER INITIALISERS, on purpose: GCC refuses them on a
    // nested struct used by an inline static member of the enclosing class
    // ("required before the end of its enclosing class"). testHooks_ below is
    // value-initialised, which makes both pointers null all the same.
    struct TestHooks {
        // Replaces makeDeviceSource's construction when set: the "radio" a
        // worker or the restore opens is the test's own recording fake.
        std::unique_ptr<cascade::source::DeviceSource> (*makeDevice)(const std::string& kind);
        // Replaces scanNative's USB walk when set: the list it returns IS the
        // native device list, so no test ever enumerates the desk's radios.
        std::vector<cascade::source::NativeDeviceInfo> (*nativeScan)();
        // Replaces PortAudio for the Source section's SOUND CARD when set:
        // the list scanSoundCards asks for and every backend an open makes
        // (tests/test_soundcard_app_paths.cpp), so no test lists or opens
        // the desk's audio inputs.
        std::shared_ptr<cascade::source::SoundCardBackend> (*soundCardBackend)();
        // Replaces PluginHost::defaultPluginDir() in rescanPlugins when set:
        // tests/test_apply_command.cpp loads its fixture plugin from its own
        // scratch directory, never from beside the test binaries, where every
        // other test that builds an AppWindow would load it too.
        std::string (*pluginDir)();
        // Replaces SoapySource::enumerate on scanSoapy's worker when set: the
        // device-scan commands are applied in tests without a vendor probe
        // ever touching the desk's radios (it opens and resets what it finds).
        std::vector<cascade::source::SoapyDeviceInfo> (*soapyScan)();
    };
    // Set by the test before any AppWindow exists and never changed while one
    // does, so the worker threads that read makeDevice race with nothing.
    inline static TestHooks testHooks_{};
    // Each queued command carries the sourceGen_ it was asked under. A
    // command addressed to THE RADIO (rate, gain, AGC, antenna, bias tee,
    // device switch) whose radio has since been closed or replaced is
    // dropped at the drain rather than landing on whatever is open now:
    // every assignment of device_ bumps sourceGen_, so an unchanged
    // generation means the very device the widget was drawn for
    // (docs/engine-stage1.md, "queued device commands").
    struct LocalCommand {
        cascade::core::cmd::QueuedCommand q;
        std::uint64_t sourceGen = 0;
    };
    std::vector<LocalCommand> localCommands_;
    double lastRefusedRequestHz_ = -1.0;
    bool faultSeen_ = false;  // endTakesOnFault's edge: the latch as last frame saw it
    // THE USER'S OWN MUTE, kept apart from the plugin mute (mutedBy_ and the
    // rest). They are different things with different lifetimes: a plugin's
    // mute is recomputed from the tuning every frame in updateAudioMute, and
    // one that also cleared a user's mute would make the Mute key stop working
    // the moment a decoder was running. The pipeline is told the OR of the two.
    bool userMuted_ = false;

    // WHERE EACH FAMILY'S ROWS START IN THE SOURCE COMBO. Row 0 is the
    // generator, row 1 the IQ file and row 2 the sound card; the native radios
    // come next, and the SoapySDR devices after them. Named rather than written
    // as "3" at the dozen sites that index this list, because one of those
    // sites forgetting that the native block exists is an off-by-N that opens
    // the wrong radio.
    static constexpr int kSoundCardRow = 2;
    static constexpr int kNativeRowBase = 3;

    // DSP pipeline plus the two live display widgets it feeds. The views are
    // held by unique_ptr for two reasons: the forward declarations above, and
    // the waterfall's GL texture, whose deletion needs the creating GL context
    // current — run() tears the view down explicitly before destroying the
    // context, because AppWindow itself outlives it (destroyed in main()).
    cascade::core::Pipeline pipeline_;

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
    // WHICH OF THE OFFERED BANDWIDTH STEPS THE VFO IS ON, or -1 for none of
    // them: a plugin preset may ask for a width the list does not carry (the
    // NOAA APT one asks for 40 kHz), and the combo then letters the real
    // figure and ticks nothing. It used to be the NEAREST step, which made the
    // control show 12.5k over a 40 kHz VFO and apply that 12.5k on the next
    // click. Never used to index kBwHz without a literal step beside it.
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
    // THE DEVICE OPEN, OFF THIS THREAD. Field report "hang ntdll.dll @
    // InitializeWaveHandles" (0.96.4): picking an output device put the GUI
    // thread inside waveOutOpen for 57 seconds. See engine/audio_open.hpp for the
    // whole argument; what matters here is that nothing on this thread may
    // query the sink while inFlight() is true.
    cascade::gui::AudioOpen audioOpen_;
    // The AudioOpen::Result tag that says which of the two asked: the audio
    // watchdog reopening a dead stream (counted as a recovery, and the only
    // one that writes the health note) or the user picking a device. Carried
    // by the request rather than kept in a member here, because a click queued
    // behind a reopen would otherwise relabel the open already in flight.
    static constexpr int kAudioOpenByUser = 0;
    static constexpr int kAudioOpenByWatchdog = 1;
    // Once-a-minute starvation digest (see pollAudioHealth). Sampled every
    // frame — not gated behind the 1 Hz watchdog above — because a ring can
    // dip and recover well inside a second at 48 kHz, and a low-water mark
    // read only once a second would miss most of the dips it exists to
    // report. SIZE_MAX so the very first frame's real reading always beats
    // the sentinel instead of needing a separate "have we sampled yet" flag.
    double lastAudioLogSec_ = 0.0;
    std::size_t audioRingLowWaterFrames_ = SIZE_MAX;
    std::uint64_t audioUnderrunsAtLogStart_ = 0;
    std::uint64_t audioPrimingAtLogStart_ = 0;

    int deemphIndex_ = 0;  // index into kDeemphUs; 0 = 50 us (global default)

    std::vector<cascade::source::SoapyDeviceInfo> soapyDevices_;
    bool soapyScanned_ = false;  // one lazy scan done (scanSoapy())
    // AppConfig::lookForNetworkUsrps, and the Soapy args the config named at
    // start-up - both read by scanSoapy() for the UHD rule
    // (gui::soapyDriversWithNoHardware).
    bool lookForNetworkUsrps_ = false;
    std::string startupSoapyArgs_;
    // The drivers the scan in flight left out for having nothing to find,
    // written by the scan's own thread before its future is ready (so read
    // safely once it is) - and the last completed scan's, which is what
    // gui::networkUsrpHint draws from under the Source list.
    std::shared_ptr<std::vector<std::string>> soapyScanAbsent_;
    std::vector<std::string> soapyAbsentDrivers_;
    // A deferral has been logged for the radio currently open. The combo's
    // lazy scan asks on every frame the dropdown is open, so without this the
    // one diag line would be written sixty times a second; cleared the moment
    // the gate opens again (drawSourceSection), so the next radio gets its
    // own line.
    bool soapyScanDeferredLogged_ = false;
    // THE SCAN BESIDE AN OPEN RADIO (2026-09-23; engine/device_scan_plan.hpp).
    // soapyScanSkip_ is the drivers the scan in flight left out - the open
    // radios' own families - and pollSourceAsync keeps the rows of those
    // drivers from the old list, since that scan could not have seen them.
    // soapyScanPartial_ says the last scan was one of those, so the next
    // chance with no radio open does a whole one.
    std::vector<std::string> soapyScanSkip_;
    bool soapyScanPartial_ = false;
    // The patch page asked for a SoapySDR scan when it opened and has not had
    // one yet (the plan was deferring - a radio still opening). See patchReconcile.
    bool patchScanWanted_ = false;

    // --- Reopening after an absorbed driver fault (0.90.1) -----------------
    // When the automatic reopen was last attempted, in ImGui::GetTime()
    // seconds; negative = never. gui::autoReopenDue holds the next attempt
    // off for kSoapyReopenHoldoffSec after this, so a radio that is really
    // gone is tried once, not in a loop.
    double soapyReopenAttemptSec_ = -1.0;

    // --- Off-thread SoapySDR discovery and open --------------------------
    // SoapySDR::Device::enumerate()/make() do USB bus discovery and, for a
    // B200, an FPGA/firmware load: seconds of blocking work. Run inline they
    // froze the GUI for ~3 s on every source click. Both now run on a worker
    // thread; the GUI polls each frame and applies the result. The device
    // itself is only ever touched by the GUI thread once the future resolves,
    // so no locking is needed beyond the future's own synchronization.
    struct DeviceOpenResult {
        std::unique_ptr<cascade::source::DeviceSource> dev;  // null on failure
        // WHICH DRIVER THE WORKER SHOULD CONSTRUCT, and afterwards which one
        // it did: "soapy", or one of the eight native driver keys - the
        // same spellings
        // AppConfig::sourceKind uses. The kind has to travel with the request
        // because the worker is what decides the concrete type, and it has to
        // come back with the answer because sourceKind_ is set from it.
        std::string kind = "soapy";
        std::string args;
        // THE SOAPY ARGS THE PREFER-NATIVE DECISION WAS MADE FROM, carried
        // along so the worker can fall back to them. Empty for an open the
        // user asked for directly. See launchDeviceOpen: a dongle whose tuner
        // the native driver does not support (E4000, FC0012/13) must still
        // open the way it always did, and by the time that is known the
        // worker is the only thing still holding the request.
        std::string fallbackSoapyArgs;
        std::string error;
        int row = -1;
        double requestRateHz = 0.0;
    // The frequency the user was listening to when they changed device.
    //
    // A newly opened radio sits wherever its driver defaults to - an RTL-SDR
    // comes up at 100 MHz - so without carrying this across, changing device
    // silently retunes the receiver and the audio stops. Captured before the
    // switch because by the time the open finishes, the old source is gone.
    //
    // AN AIR FREQUENCY, AND IT MAY BE NEGATIVE: with the VFO parked above a
    // VLF station the band centre sits below 0 Hz on the air, which through an
    // up-converter is an ordinary tune (core::airReachable). So "nothing to
    // carry" is its own state - no value - and never a zero or a sign
    // (carriedAirCentre decides which). The new radio's converter judges
    // whether it can be delivered.
    std::optional<double> keepCenterHz;
    // THE RADIO FREQUENCY AN RSP IS TUNED TO BEFORE ITS STREAM STARTS (the
    // 0.99.36 pre-Init tune), already converted through that radio's
    // converter on the GUI thread by launchDeviceOpen - the worker has no
    // converter state, and keepCenterHz is an AIR frequency. No value: no
    // pre-tune (nothing to carry, not an RSP, or not deliverable).
    std::optional<double> preTuneRadioHz;
    // THE NATIVE RADIO THIS OPEN FELL BACK FROM (converter key), when the
    // worker opened the dongle through SoapySDR because the native driver
    // refused its tuner; empty otherwise. finishDeviceOpen uses it so the
    // converter set for that radio still applies (converterKeyAlias_).
    std::string fellBackFromKey;
    // WHAT AN AUTOMATIC REOPEN HAS TO PUT BACK (pollSoapyRecovery, 0.90.1).
    // The ordinary open primes every gain to its default and leaves AGC off;
    // a reopen after a driver fault is not a new radio to the user, so the
    // gains, the gain mode and - if the receiver was running when the driver
    // faulted - the running state are restored once the device is up. The
    // antenna needs nothing here: deviceAntenna_ is applied by every open.
    // Carried INSIDE the result rather than in a member so an answer the
    // user has moved on from (asyncOpenStillWanted) drops it with the rest.
    bool recovery = false;
    std::vector<std::string> recoveryGainNames;
    std::vector<float> recoveryGainsDb;
    bool recoveryAgc = false;
    bool recoveryRestart = false;
    // THE RADIO CLOSED TO MAKE THIS ATTEMPT (0.99.36), as the config would
    // have named it, and what it was called. Invalid when nothing was closed.
    // A failed open remembers it (gui::rememberAfterFailedSwitch) so the exit
    // save does not write the generator in its place.
    cascade::gui::RememberedSource closedRadio;
    std::string closedLabel;
    };
    std::future<std::vector<cascade::source::SoapyDeviceInfo>> soapyScanFuture_;
    std::future<DeviceOpenResult> deviceOpenFuture_;
    bool soapyScanPending_ = false;
    bool deviceOpenPending_ = false;
    std::string deviceBusyLabel_;  // device name shown while an open is in flight

    // Paces hardware retunes — see retuneSourceHz. 50 ms: invisible against
    // the wheel gesture, one apply per notch burst instead of one per frame.
    cascade::core::RetuneCoalescer retuneCoalescer_{50.0};
    // The isPluginPreset a retune was requested with, carried across the
    // coalescer alongside its frequency — see retuneSourceHz. Overwritten on
    // every request, so a deferred apply reads the LATEST caller's context,
    // never a stale one from an earlier request the coalescer already
    // superseded.
    bool pendingRetuneIsPreset_ = false;

    // Set by noteTuneMismatch when a retune's readback disagreed with what it
    // asked for by more than kTuneMismatchToleranceHz; drawn in warning
    // colour under the Source controls, "" = the last retune landed where it
    // was asked. NaN so the very first mismatch this session sees is always
    // logged (NaN != NaN), never suppressed by an uninitialised zero that
    // happens to equal a real request.
    std::string tuneMismatchNote_;
    double lastMismatchRequestHz_ = std::numeric_limits<double>::quiet_NaN();
    double lastMismatchAnswerHz_ = std::numeric_limits<double>::quiet_NaN();

    // Source-selection sequence number, incremented by EVERY install of a
    // source into the pipeline (generator, IQ file, or a resolved device).
    // deviceOpenReqGen_ records the value an in-flight open was requested at;
    // asyncOpenStillWanted() compares the two when it resolves. See the
    // predicate's comment above for why a counter and not a flag.
    std::uint64_t sourceGen_ = 0;
    std::uint64_t deviceOpenReqGen_ = 0;

    // --- THE SOUND CARD SOURCE (app_window_soundcard.cpp) ---------------------
    // Row kSoundCardRow of the Source combo. Like the IQ file and the Pluto,
    // choosing the row shows its controls and opens nothing; Open does, on a
    // worker, because enumerating and opening an audio device are calls into
    // the host API that can take as long as it likes. See
    // source/soundcard_source.hpp for the source itself.
    struct SoundCardOpenResult {
        std::unique_ptr<cascade::source::SoundCardSource> src;  // null on failure
        std::string error;    // why it failed, or a coerced rate on success
        std::vector<cascade::source::SoundCardDevice> devices;  // the list it opened from
        std::uint64_t gen = 0;  // sourceGen_ when it was asked for
        bool restore = false;   // the startup restore asked, not the user
        cascade::source::SoundCardSettings wanted;  // what was asked for
        // THE SAME CARD WAS RELEASED FIRST (gui::soundCardReopenReleasesFirst):
        // the running card was closed before this open, with `previous` - its
        // settings - to fall back on.
        bool released = false;
        cascade::source::SoundCardSettings previous;
        // `wanted` IS `previous` (gui::soundCardSameSettings): released and
        // tried once, with nothing different to fall back to.
        bool sameAsPrevious = false;
        bool restoredPrevious = false;  // src runs `previous`: `wanted` was refused
        std::string previousRefused;    // ...and why `previous` did not come back either
    };
    // The card restoreKeep_ names when its kind is "soundcard" - the saved
    // card a startup restore could not open, the card released for a re-Open,
    // the card handed back by the patch page - as it last RAN (or as the
    // config had it). Meaningful only while restoreKeep_ names a card.
    cascade::source::SoundCardSettings soundCardRemembered_;
    // The settings of the card as it is RUNNING (set when one is installed;
    // meaningful only while sourceKind_ is "soundcard"). The centre box reads
    // its format: a real-mode card has no centre to move. The patch page's
    // loan of the card takes these, never soundCard_ (which may have been
    // edited and not Opened), and so does a re-Open's fallback.
    cascade::source::SoundCardSettings soundCardLive_;
    std::vector<cascade::source::SoundCardDevice> soundCardDevices_;
    bool soundCardListed_ = false;  // soundCardDevices_ holds a finished enumeration
    std::future<std::vector<cascade::source::SoundCardDevice>> soundCardScanFuture_;
    bool soundCardScanPending_ = false;
    std::future<SoundCardOpenResult> soundCardOpenFuture_;
    bool soundCardOpenPending_ = false;
    // The saved card was not in the list at startup: say so under the
    // controls until the user opens something (never replaced by another).
    std::string soundCardMissing_;
    // Combo selection. -1 means "active device no longer in the list" (a
    // Refresh dropped it); the preview then falls back to the active source
    // name. Distinct from the ACTIVE source: selecting "IQ file" only shows
    // the path controls — the pipeline keeps its source until Open succeeds.
    int sourceSel_ = 0;
    std::string sourceError_;   // red text under the Source controls; "" = none
    // THE RADIO THE PANEL DRIVES, whatever kind it is. Non-owning view of the
    // DeviceSource installed in the pipeline (the pipeline owns it via
    // setSource); null whenever the active source is the generator or a file,
    // and must be nulled BEFORE any setSource that destroys the object.
    //
    // This was a SoapySource* until 0.91.0 and everything the Source section
    // did went through the concrete class. It is the interface now because
    // there are two more kinds of radio behind it - RtlSdrSource and
    // HackRfSource, which speak WinUSB and need no vendor module at all - and
    // a panel written against one of the three would have had to be written
    // three times. Rate, gains, AGC, antenna, tuning range, dead/faulted: all
    // of it is DeviceSource now, and a Soapy device answers exactly as it did.
    cascade::source::DeviceSource* device_ = nullptr;

    // ...AND THE SAME OBJECT AS A SoapySource WHEN IT IS ONE, for the four
    // things that are genuinely Soapy-specific and have no meaning for a
    // native driver: the child-process scan gate (a native enumeration opens
    // nothing and needs no gate), the module/vendor diagnostics under "no
    // radio hardware found", the "close the radio to look for other devices"
    // caption, and the automatic reopen after an absorbed vendor fault
    // (deadReason() distinguishes a faulted driver from a wedged one, which
    // only SoapySource has). Null whenever device_ is not a SoapySource -
    // including when it is a native radio - and set and cleared with it.
    cascade::source::SoapySource* soapyView_ = nullptr;

    std::string deviceArgs_;     // args of the open device (re-find on Refresh)
    // WHICH ROW OF supportedSampleRatesHz() the Rate combo is on. The list
    // used to be the fixed 1/2/4/8 MS/s table for every radio on every
    // driver; it is now the DEVICE's own list - the RTL-SDR's twelve standard
    // rates, the HackRF's 2..20 MS/s menu, whatever a Soapy driver reports -
    // so the index only means anything alongside deviceRatesHz_.
    int deviceRateIndex_ = 1;
    std::vector<double> deviceRatesHz_;         // supportedSampleRatesHz() at open
    std::vector<std::string> deviceRateLabels_;  // "2.400 MS/s", one per rate
    std::vector<std::string> deviceGainNames_;  // gains() at open, names only
    // The RANGE each of those gains will accept, parallel to the names. The
    // sliders were drawn 0..60 dB for every stage of every radio before this,
    // which is right for none of them: a B200's PGA goes to 76 dB and an
    // RTL-SDR's VGA starts at -4.7, and SoapySDR clamps silently so nothing
    // ever said so.
    std::vector<cascade::source::GainInfo> deviceGainRanges_;
    std::vector<float> deviceGainsDb_;          // slider mirrors, one per name
    bool deviceAgcSupported_ = false;
    bool deviceAgc_ = false;

    // THE BIAS TEE. Present only when the OPEN device is one of the native
    // drivers that has one and can say so (see withBiasTee in
    // engine/bias_tee.hpp for which, and for why this is not a DeviceSource
    // method). `shown` is the checkbox and is always the driver's READBACK;
    // `remembered` is AppConfig::biasTee, the memory PER RADIO (driver and
    // serial) for every family. It is seeded at restore, applied by
    // adoptDeviceMirrors after every open through biasTeeAfterOpen - to that
    // radio only - and changed only when the user switches a radio, with the
    // checkbox or the deck's key (biasTeeTicked).
    cascade::gui::BiasTeePanel biasTeePanel_;
    // THE CENSUS AND CAPTURE SEAM, like FOXSDR_FORCE_MUTE_BANNER: with
    // FOXSDR_FORCE_BIAS_KEY=accept (or =refuse) and no radio with a bias tee
    // open, the key is drawn over a STAND-IN bias tee that takes (or refuses)
    // every change, so tests/test_theme_census.cpp can place the key in every
    // theme and a capture can show it off, on and refused off the generator.
    // It never touches a radio: the moment a real one with a bias tee is open,
    // that radio's readback is what the key shows. READ ONLY BY run() FOR A
    // BOUNDED (--frames) RUN (gui::biasStandInFor), like FOXSDR_INPUT_SCRIPT:
    // an interactive launch never has a stand-in.
    cascade::gui::BiasStandIn biasStandIn_ = cascade::gui::BiasStandIn::None;
    bool biasStandInOn_ = false;
    // THE SAME RADIO UNDER ANOTHER KEY FOR THIS SESSION. When a native open
    // falls back to SoapySDR the dongle is the one the user chose, but its key
    // is the Soapy one; the alias maps that key to the native one so the
    // converter set for the radio still applies - and changes made while it is
    // open are kept under the native key, where the next native open looks.
    // Not saved: the next session makes the same decision again. Installed
    // only when the Soapy key has no ACTIVE converter of its own (an Off
    // record is none) and both keys name the dongle by the same serial
    // (gui::fallbackNamesTheSameDongle).
    std::map<std::string, std::string> converterKeyAlias_;
    // Soapy keys a fallback opened WITHOUT carrying the native radio's
    // converter, because nothing showed it was the same dongle; the Source
    // section says so (converterAliasNote). This session only.
    std::set<std::string> converterNotCarried_;
    // The air centre a converter change could NOT keep (the radio could not
    // follow it, so the counter relabelled), with the radio it was held for and
    // where that radio sat. The next change uses it instead of the relabelled
    // figure while the radio is still there, so off-then-on and a typo-then-
    // fix come back to the same station. Cleared by every source install.
    // (No default member initialisers: GCC refuses them on a nested struct
    // an std::optional member instantiates inside the class - see TestHooks.)
    struct ConverterHeldAir {
        std::string key;
        double airHz;
        double radioHz;
    };
    std::optional<ConverterHeldAir> converterHeldAir_;
    std::map<std::string, cascade::core::ConverterSetting> converters_;

    // THE SWITCHES THAT BELONG TO ONE RADIO EACH, and are NOT persisted.
    //
    // The bias tee above is saved because leaving it off silently costs a
    // user an evening of a dead band. None of these does: an RSP's notches
    // and HDR mode and an RX888's dither and output randomiser change what is
    // heard, are visible in the spectrum the moment they move, and each
    // driver deliberately puts the radio into a known state at open. So these
    // mirror the DRIVER'S READBACK for the session and nothing more - which
    // also means there is no stale saved value to reconcile against a
    // driver's open-time policy, the reconciliation the RTL-SDR's bias tee
    // needed a rule of its own for (engine/bias_tee.hpp, rtlBiasTeeAtOpen).
    //
    // "Present" is asked of the CONCRETE TYPE once per open, because these
    // are per-model even within one driver: an RSP1A has no HDR mode, an
    // RSPdx has no DAB notch, and a panel that offered either would be
    // offering a control the API answers with an error.
    bool deviceRfNotchPresent_ = false;
    bool deviceRfNotch_ = false;
    bool deviceDabNotchPresent_ = false;
    bool deviceDabNotch_ = false;
    bool deviceHdrPresent_ = false;
    bool deviceHdr_ = false;
    bool deviceAdcSwitchesPresent_ = false;  // the RX888's pair, together
    bool deviceDither_ = false;
    bool deviceRandomiser_ = false;

    // --- Native radios ----------------------------------------------------
    // Every radio one of our own drivers can open, from the eight enumerate*
    // functions. UNGATED and refreshed freely, unlike soapyDevices_: a native
    // enumeration reads SetupAPI properties and NEVER OPENS A DEVICE
    // (src/usb/usb_device.hpp rule 1), which is the exact rule the vendor
    // probe breaks and the whole reason scanSoapy() has a gate. It is cheap
    // enough to run on the GUI thread.
    //
    // TWO OF THE EIGHT ARE NOT USB AND ARE STILL IN HERE. enumerateSdrPlay()
    // asks the SDRplay service for its list, which is safe at any time for
    // the same reason rule 1 exists - it does not touch the bus. The Pluto's
    // row is not a discovery at all: a network cannot be walked, so
    // scanNative appends ONE row for it unconditionally, at the end, and that
    // row opens nothing until the user presses Open on an address. See
    // plutoUri_ and kPlutoDriverKey.
    std::vector<cascade::source::NativeDeviceInfo> nativeDevices_;
    // Their combo captions, composed once by scanNative(): the row label with
    // " (native)" appended. Stored rather than built per frame because the
    // combo hands ImGui a const char* that has to outlive the call.
    std::vector<std::string> nativeRowLabels_;
    // Dongles that are PRESENT but not bound to WinUSB - the DVB-T driver, or
    // no driver at all (problem code 28). They cannot be opened by anything,
    // so they are not offered as rows; the Source section says so in one
    // sentence instead, because "my dongle is not in the list" with no
    // explanation was the single worst thing this panel used to do.
    std::vector<cascade::usb::UsbDeviceInfo> nativeUnbound_;

    // WHAT THE SOURCE SECTION SAYS ABOUT THE SDRPLAY API WHEN THERE IS NO RSP
    // ROW TO SHOW. Composed by scanNative() from the driver's own pure
    // sdrPlayPanelAdvice(), so the sentence the user is given is the one a
    // test pins; empty when the API is installed and new enough, which is when
    // there is nothing to say. It covers BOTH ways an RSP goes missing - no
    // API and an API too old - which took the enumeration's recorded reason,
    // because the table's own version field is written only by a session that
    // got past the version gate. sdrPlayApiDetail_ is the loader's own account
    // of where it looked, shown dimmed underneath for whoever is helping.
    std::string sdrPlayAdvice_;
    std::string sdrPlayApiDetail_;
    bool sdrPlayRowsFound_ = false;
    // The label each SDRplay API row last showed, by its args (0.99.36). A
    // radio this process has selected is not in the API's list, so its row is
    // put back from here (source::withClaimedSdrPlayRows) under the name the
    // user picked it by.
    std::map<std::string, std::string> sdrPlaySeenLabels_;
    // Last REQUESTED VFO bandwidth (Hz): combo presets and band-edge drags
    // both land here, and this is what the overlay and the config store use.
    // (The combo keeps showing its last preset after an edge drag — the combo
    // is a preset picker, not a readback; the overlay is the truth.)
    double vfoBandwidthHz_ = 150000.0;
    // The ACTIVE source's kind as the config store spells it. Tracked at each
    // successful switch because the pipeline does not expose source identity.
    // "siggen"|"file"|"soapy"|"rtlsdr"|"hackrf"|"airspy"|"airspyhf"|
    // "sdrplay"|"mirisdr"|"rx888"|"pluto"|"soundcard"
    std::string sourceKind_ = "siggen";

    // WHAT THE CONFIG REMEMBERS, ONE SLOT PER FAMILY, and they are separate
    // on purpose. deviceArgs_ above is the LIVE device's args; these two are
    // the last Soapy device's kwargs and the last native device's args, kept
    // apart because the prefer-native rule needs both at once: it reads the
    // SAVED SOAPY args to learn "driver=rtlsdr, serial=00000001" and decide
    // whether a native row is the same dongle, and it must still have those
    // Soapy args to fall back to when the native open refuses the tuner. One
    // shared field would be overwritten by whichever family opened last, and
    // the fallback would then have nothing to fall back to.
    std::string cfgSoapyArgs_;
    std::string cfgNativeArgs_;

    // THE SAVED RADIO A RESTORE COULD NOT OPEN, held so the exit save can
    // write it back instead of the generator that stood in for it. Set only
    // by the startup restore's failure path and cleared the moment the user
    // deliberately chooses any other source; see
    // gui::rememberedSourceAfterFailedOpen for why one session with the
    // dongle unplugged used to lose the radio permanently. The label is what
    // the Source section shows in the combo while this is set - a radio that
    // is saved but not open, rather than a generator presented as if it had
    // been chosen.
    cascade::gui::RememberedSource restoreKeep_;
    std::string restoreKeepLabel_;

    // The open radio's MODEL, with no serial in it - what every diagnostic
    // line, the crash context and the scan-gate caption name it by. Kept as a
    // member because it cannot be derived from the args for a native device:
    // core::sanitiseDevice's allow list is driver/product/type, and a native
    // row's args are nothing but a serial, so sanitising them yields "".
    std::string deviceModel_;
    // RX antenna ports the open device offers, and the one selected. Empty
    // until a Soapy device is opened. Persisted, because which port carries
    // the antenna is a property of the user's cabling, not of a session.
    std::vector<std::string> deviceAntennas_;
    std::string deviceAntenna_;
    std::string iqOpenPath_;  // last successfully opened IQ file (persisted;

    // --- Recorder state (P6) --------------------------------------------------
    // Two independent Recorder instances so IQ and audio takes can run
    // simultaneously (each records ONE kind at a time by its contract). The
    // pipeline holds non-owning pointers to them only while a take is live;
    // stop*Recording clears the pointer before stopping the recorder.
    cascade::core::Recorder iqRecorder_;
    cascade::core::Recorder audioRecorder_;
    std::string recordDir_;    // %USERPROFILE%/Documents/SDR-recordings
    std::string recordError_;  // red text in the Recorder section; "" = none
    // Why a take ended when nothing went WRONG (the user changed source):
    // muted text in the Recorder section and its own field on the web page,
    // so it never lights FAIL there. Cleared with recordError_ by a Record.
    std::string recordNotice_;
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
    std::string bookmarkImportNote_;     // what the last import did
    bool bookmarkSaveDirty_ = false;
    double bookmarkSaveDueS_ = 0.0;

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

    // Band plan: OPTIONAL display data merged from
    // BandPlan::defaultDir() at construction. A missing directory is the
    // normal case for a run-from-build-tree session and is silent — there is
    // simply no overlay. A directory that EXISTS but fails to parse keeps its
    // reason here and shows it in the Display section, because that one is a
    // user-visible mistake worth reporting.
    cascade::core::BandPlan bandPlan_;
    std::string bandPlanError_;
    // The active plan's id and the menu of installed plans. The list is built
    // ONCE in loadBandPlan() rather than per frame: available() opens and
    // parses every file in the directory, which is fine at startup and at a
    // deliberate re-scan, and is not something to do sixty times a second
    // inside a combo box.
    std::string bandPlanSelection_ = "world";
    std::vector<cascade::core::PlanInfo> bandPlanChoices_;

    // Plugin host: scanned once at construction and on Rescan. Owns the
    // loaded modules, so it must outlive nothing in particular here — but it
    // is declared before the pipeline-dependent members so it unloads last.
    cascade::core::PluginHost pluginHost_;
    // GUI-side plugin capabilities: map targets, plugin windows, and THE HOST
    // SERVICES a plugin calls back through.
    //
    // DECLARED BEFORE pluginRunner_, so it is destroyed AFTER it, and the
    // three are torn down host-services-last: runner, then UI, then host. That
    // is the order detachAndUnloadPlugins() has always performed and the order
    // ~AppWindow now performs explicitly; this declaration is the net under
    // that, because reverse declaration order is what the destructor falls
    // back on and it used to have these two the wrong way round. A decoder's
    // destroy() may ask the host for the time (Survey Engine 0.1.0 does, to
    // timestamp the dwell it is finishing), and with pluginUi_ destroyed first
    // that call reached a dead host: an access violation on Windows and an
    // abort inside libc++ on Android, both reported from the field on
    // 2026-09-16 from the same plugin at shutdown.
    //
    // THE ONE RECEIVER SNAPSHOT (engine stage 2, core/receiver_snapshot.hpp),
    // declared first so pluginUi_ can be built on it: publishReceiverState
    // fills it once a frame, and the plugin host API, the web server and CAT
    // all answer from it. Shared, because plugin bridges outlive this window.
    std::shared_ptr<cascade::core::ReceiverSnapshot> receiverSnapshot_ =
        std::make_shared<cascade::core::ReceiverSnapshot>();
    cascade::core::PluginUi pluginUi_{receiverSnapshot_};
    // Drives the loaded decoders with real audio. Declared AFTER pluginHost_
    // and pluginUi_ so it is destroyed BEFORE both: the runner's destructor
    // calls each plugin's destroy(), which is code inside a module the host
    // unmaps and which may call a host service on its way out.
    cascade::core::PluginRunner pluginRunner_;
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
    std::size_t diagPluginCount_ = static_cast<std::size_t>(-1);

    bool rxSet_ = false;
    double rxLat_ = 0.0;
    double rxLon_ = 0.0;
    // --- the GPS the position can be read from (0.86.0) ---------------------
    // The reader owns its thread and joins in its destructor; run() also
    // stops it at the top of the shutdown path so the join precedes the GL
    // teardown. gpsPort_ and gpsBaud_ are the persisted choice
    // (AppConfig::gpsPort / gpsBaud); gpsPortInput_ is the text field's own
    // buffer, one byte over the port layer's limit for the terminator, so
    // the field cannot hold a name the sanitiser would cut. gpsPorts_ is the
    // last enumeration, taken when the drop-down opened. gpsRefusal_ is the
    // one sentence shown when a fix the reader accepted was refused by
    // applyReceiverPosition - which cannot happen while both apply the same
    // predicate, and is shown rather than swallowed precisely so a day it
    // does happen is visible.
    cascade::core::GpsReader gpsReader_;
    std::string gpsRefusal_;
    cascade::core::patch::Graph patchGraph_;
    // THE PATCH'S OWN TRANSPORT (0.99.18). Opening the page no longer starts
    // anything: START on the page opens the radios that are switched on and
    // takes the receiver's radio; STOP or ALL OFF closes them and gives it
    // back. Session-only - a patch never starts itself at launch.
    bool patchRunning_ = false;
    bool patchWasRunning_ = false;
    // This frame's compile() of the patch: what would be built, and every
    // reason it could not be. Recomputed while the page is open.
    cascade::core::patch::Plan patchPlan_;
    // WHAT THE RUNNING SET WAS BUILT FROM, as core::patch::dspSignature()
    // spells it. A new set is published only when this changes. It used to
    // be the rate and centre plus "anything was edited", which rebuilt the
    // set on every frame of a node drag; now that a set holds plugin
    // instances, that would restart every decoder sixty times a second.
    std::string patchDspSig_;
    bool patchWasOpen_ = false;
    // The installed decoder plugins as the patch sees them, parallel lists.
    // Rebuilt from the plugin host each frame the page is open (a dozen
    // records), and EMPTIED in detachAndUnloadPlugins() before any module is
    // unmapped - the API pointers point into those modules.
    std::vector<cascade::core::patch::DecoderInfo> patchCatalogue_;
    std::vector<cascade::core::patch::PluginApis> patchApis_;
    // Decoder nodes whose plugin returned no instance from create() in the
    // last build, so the node can say so rather than look ready.
    std::vector<cascade::core::patch::NodeId> patchRefused_;
    // Decoder nodes whose FIRST line since the last build has been logged.
    // One line per decoder per build goes to the application log - enough to
    // prove from a user's log that a patch decoder was fed and produced
    // output, without copying a busy decoder's whole stream into it.
    std::set<cascade::core::patch::NodeId> patchFirstLineLogged_;
    // Each decoder node's latest line and running count since the last
    // build, for its face on the canvas. Kept whether or not the node is
    // wired to a Text out, because a decoder working with nowhere to send its
    // text is still working, and the face is where that is visible.
    struct PatchDecoderFace {
        std::string last;
        std::uint64_t lines = 0;
    };
    std::map<cascade::core::patch::NodeId, PatchDecoderFace> patchDecoderFaces_;
    // Each Text out node's recent lines, newest last, for its face - the
    // patch's own decoder log. Bounded per node.
    std::map<cascade::core::patch::NodeId, std::deque<std::string>> patchSinkLines_;

    // --- the patch's own radios (0.99.17, app_window_patch_radios.cpp) --------
    // UP TO FIVE, EACH ITS OWN DEVICE. Every Radio node with a device chosen
    // runs a core::patch::PatchRadio while the page is open: its own device
    // open, reader thread, runner and spectrum. Keyed by node.
    std::map<cascade::core::patch::NodeId, std::unique_ptr<cascade::core::patch::PatchRadio>>
        patchRadios_;
    // What each running radio was opened AS ("<device key>@<rate>"), so a
    // changed device or rate reopens it and nothing else does.
    std::map<cascade::core::patch::NodeId, std::string> patchRadioOpenedAs_;
    // A hardware open in flight, per node. The open is a multi-second USB
    // walk, so it runs on a worker exactly as the receiver's own does.
    struct PatchRadioOpen {
        std::unique_ptr<cascade::source::IqSource> src;
        std::string label;
        std::string error;
    };
    std::map<cascade::core::patch::NodeId, std::future<PatchRadioOpen>> patchRadioPending_;
    std::map<cascade::core::patch::NodeId, std::string> patchRadioPendingAs_;
    // Why a radio is not running, when it tried and failed; drawn on its face
    // and turned into Problem::RadioFailed for the plan.
    std::map<cascade::core::patch::NodeId, std::string> patchRadioError_;
    // The "<key>@<rate>" that failed, so a radio that would not open is not
    // retried every frame - only when its device or rate is changed.
    std::map<cascade::core::patch::NodeId, std::string> patchRadioFailedAs_;
    // Decoder nodes that refused to start, per radio, merged into
    // patchRefused_ for the plan.
    std::map<cascade::core::patch::NodeId, std::vector<cascade::core::patch::NodeId>>
        patchRefusedBy_;
    // Per radio: what its running set was built from (radioSignature).
    std::map<cascade::core::patch::NodeId, std::string> patchRadioSig_;
    // Each speaker's output, and the output key it was made for.
    cascade::core::patch::DestTable patchDests_;
    std::map<cascade::core::patch::NodeId, std::string> patchDestMadeFor_;
    std::map<cascade::core::patch::NodeId, std::string> patchDestError_;
    // Each radio's newest spectrum, for its Spectrum parts and channel levels.
    struct PatchSpectrum {
        std::vector<float> db;
        std::uint64_t seq = 0;
    };
    std::map<cascade::core::patch::NodeId, PatchSpectrum> patchSpectra_;
    // THE RECEIVER'S RADIO, HANDED TO THE PATCH (owner: "when the patch panel
    // is running unpopulate the sdr from the main sdr software so it show
    // signal generator"). Opening the page switches the receiver to the
    // generator and remembers the radio here; closing the page opens it again.
    struct PatchMainKeep {
        bool valid = false;
        std::string kind;
        std::string args;
        std::string label;
        double rateHz = 0.0;
        // The receiver's AIR centre when the patch took the radio; no value
        // when it had none to give. May be below 0 Hz (see
        // DeviceOpenResult::keepCenterHz).
        std::optional<double> centreHz;
        // A lent SOUND CARD, as it was running when the patch took it - what
        // the hand-back reopens (gui::receiverSourceForPatch).
        cascade::source::SoundCardSettings card;
    };
    PatchMainKeep patchMainKeep_;
    // One device a patch Radio can be set to, for the inspector's list.
    struct PatchDeviceChoice {
        std::string key;     // core/patch_devices.hpp
        std::string label;
    };

    // --- THE TRANSMITTER (0.95.0) --------------------------------------------
    //
    // The TX thread, the modulator, the microphone and the radio all live
    // inside Transmitter; this is everything the PAGE needs and nothing more.
    //
    // NOTHING HERE CAN KEY IT. transmitPttHeld_ is written from the page's
    // own key and from the spacebar while the page has focus, and it is reset
    // to false at the top of every frame - so a page that stops being drawn
    // stops asking, which is the behaviour a window losing focus should have.
    // The LATCH is a deliberate switch whose state lives in Transmitter (with
    // its own failsafe); the page only records a PRESS. All three per-frame
    // flags reach the transmitter together, in drawUi just before its tick,
    // through gui::txPageKey - so a page that is not live releases the key.
    // None is persisted; core/config.hpp says why at length.
    cascade::core::Transmitter transmitter_;
    bool transmitSplit_ = false;
    double transmitSplitHz_ = 145.5e6;
    int transmitModeIndex_ = 0;    // dsp::TxMode
    int transmitInputIndex_ = 1;   // core::TxInput; 1 is TONE, and deliberately
    double transmitPowerDb_ = -89.75;
    double transmitToneHz_ = cascade::dsp::kToneDefaultHz;
    // LISTEN WHILE TRANSMITTING. Off by default: a receiver left unmuted on
    // the frequency it is transmitting on is a howl, and on a full-duplex
    // board like the Pluto the receiver really is still running. On is what
    // somebody working split, or listening to their own signal through a
    // second radio, actually wants.
    bool transmitMonitor_ = false;
    std::string transmitError_;
    // THE MICROPHONE'S OPEN, OFF THIS THREAD. Pa_OpenStream on an input is
    // waveInOpen, with the same missing timeout as the output side's
    // waveOutOpen that held this thread 57 s in the field, so the MIC key goes
    // through the same gate as audioOpen_ (bug hunt 2026-09-24,
    // audio-sink-02). Bound to Transmitter::microphoneOpener(), which owns the
    // microphone, so the worker can be abandoned at quit. Nothing on this
    // thread may query the microphone while inFlight() is true.
    cascade::gui::AudioOpen micOpen_;
    // Decoded output, newest last, bounded. The panel is a tail, not an
    // archive; the recorder is where a permanent copy belongs.
    std::deque<cascade::core::DecodedLine> decoderLog_;
    static constexpr std::size_t kDecoderLogMax = 500;
    // THE ONLY MESSAGE COUNT THIS APPLICATION HAS. No decoder plugin reports a
    // message tally over the ABI and the runner keeps none, so the one
    // countable thing a decoder produces is a LINE of output, counted here as
    // pumpDecoderOutput drains it. Cumulative and never reset, so the status
    // column can difference it over a window; decoderLog_ itself cannot serve
    // (it is a bounded tail and drops its oldest entries).
    std::uint64_t decoderLinesTotal_ = 0;
    std::string pluginDir_;
    // The manifest + cached policy, as of the last rescan and with the whole
    // directory present (see the ordering above). Everything downstream -
    // the red rows, the badge, planUpdates - reads THIS, not a fresh
    // loadInventory, so nothing ever plans against a quarantined file.
    cascade::core::PluginInventory pluginInventory_;
    std::vector<cascade::core::BlockedPlugin> pluginBlocked_;
    // Why enforcement could not be completed (red, above the list). Empty in
    // the normal case, which is every case where nothing is retired.
    std::string pluginEnforceError_;
    // AppConfig::pluginLastUpdateCheck, stamped when a catalogue fetch
    // succeeds and persisted with the rest of the config.
    std::int64_t pluginLastUpdateCheck_ = 0;
    // AppConfig::pluginTuneAllowed. THIS is the durable copy of the grant, and
    // PluginUi holds the live one: PluginUi::clear() drops its set with the
    // instances on every rescan, so the permission has to survive somewhere
    // that a rescan does not touch.
    std::vector<std::string> pluginTuneAllowed_;
    // AppConfig::pluginSettingsAllowed - the durable copy of the level-1
    // SETTINGS grant, for exactly the reason pluginTuneAllowed_ is one.
    std::vector<std::string> pluginSettingsAllowed_;
    // AppConfig::pluginSettings - the durable copy of every plugin's own
    // settings. PluginApiCore holds the live one; applyPluginApi copies it
    // here whenever its generation moves, and currentConfig() saves this.
    std::map<std::string, std::map<std::string, std::string>> pluginSettings_;
    std::uint64_t pluginSettingsGen_ = 0;
    // The newest WARN or ERROR each plugin logged, by module file name, for
    // the notice on its plate. Session only, like the decoder output.
    struct PluginNotice {
        std::uint32_t level = 0;
        std::string text;
    };
    std::map<std::string, PluginNotice> pluginNotices_;
    // The plugins' marks, copied out of the core only when its sequence moves.
    std::vector<cascade::core::HostMarker> pluginMarkers_;
    std::uint64_t pluginMarkersSeq_ = 0;
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

    // --- The user's own presets (0.99.4) ---------------------------------------
    // AppConfig::userPresets, for every plugin (see core/user_presets.hpp for
    // what they are and why). Held here, like the plugin lists above, because
    // everything that acts on them is rebuilt underneath them.
    std::vector<cascade::core::UserPreset> userPresets_;
    // A SAVE OR A FORGET. Both end in rebuildMuteStates(), which replaces the
    // muteStates_ entry a preset bar is reading its keys from while it draws
    // them - so a press only submits a command (USER_PRESET_SAVE,
    // APP_USER_PRESET_FORGET_AT) and the edit happens at the top of the next
    // frame. `pluginFileKey` is the module file name (core::pluginKey); the
    // version-stripped key the list is stored under is derived from it when
    // the edit is applied.
    struct PendingUserPresetEdit {
        enum class Op { None, Save, Forget };
        Op op = Op::None;
        std::string pluginFileKey;
        std::size_t ordinal = 0;  // Forget only: which of that plugin's presets
    };

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

    // --- ADD ALL PLUGINS (0.96.0) -------------------------------------------
    //
    // ONE RUN, MANY TRANSFERS, AND NOT ONE NEW CODE PATH FOR THE BYTES. The
    // store's key hands back a list of catalogue rows; this queue starts them
    // through startInstall / startUpdate one at a time, because PluginRepo has
    // a single progress/cancel pair and applies exactly one transfer at a
    // time. Everything a single FIT gets - the https rule, the byte cap, the
    // ABI test, the sha256, the manifest record, the rescan afterwards - a
    // module in this queue gets, because it IS a single FIT.
    //
    // IDENTIFIED BY CATALOGUE ID, NOT BY INDEX. A run outlives many frames and
    // a Refresh can replace catalog_ under it; an index would then name a
    // different module. An id that is no longer in the catalogue is recorded
    // as a failure with that reason rather than silently dropped.
    struct AddAllRun {
        bool active = false;
        std::vector<std::string> ids;
        std::vector<bool> isUpdate;
        std::size_t next = 0;  // the next id to start
        int installed = 0;
        int failed = 0;
        // {name, reason}, the reason verbatim and in English from whatever
        // refused it. Kept apart so the log gets the English and the panel a
        // translation of the same words (gui::trStoredReason).
        std::vector<std::pair<std::string, std::string>> failures;
        std::string currentName;  // what is moving right now
        std::size_t total = 0;
    };
    AddAllRun addAllRun_;
    // What a finished run left behind: "23 installed, 0 failed", or the names
    // that failed with the reason each gave. Kept until the next run starts.
    std::string addAllSummary_;
    bool addAllFailed_ = false;
    std::string pluginCatalogueUrl_;  // committed value (persisted)
    std::vector<cascade::core::PluginCatalogEntry> catalog_;
    std::string catalogError_;   // red: fetch/parse failure, verbatim
    std::string catalogStatus_;  // neutral: "N plugins in the catalogue"

    // WHICH CATALOGUE ROW IS SELECTED, AND WHETHER ITS NOTICE WAS TICKED, both
    // live in PluginStoreDeck now (selected, legalAck) - the store window owns
    // its own selection and clears the tick whenever that selection moves, so
    // consent given for one plugin can never be carried to the next. The two
    // members that used to hold them are gone rather than kept in step with
    // the deck: two copies of a consent flag is one copy too many.
    std::string installReport_;  // green: last successful install/remove
    std::string installError_;   // red: last failed install/remove, verbatim
    // The runner's feeding keys, reused every frame (fillPublishedState), and
    // the last sink name a try-locked read answered (keepLastGoodName).
    std::vector<std::string> feedingKeysScratch_;
    std::string sinkNameLastGood_;

    cascade::core::Scanner scanner_;
    // Readback (center + offset) right after the last scanner-commanded
    // retune. Any later frame where the live readback differs is a tune the
    // scanner did not make — a manual tune, and the user wins (scan stops).
    double scannerExpectedAbsHz_ = 0.0;
    bool scannerHasExpected_ = false;  // false until the scan's first retune
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_HPP
