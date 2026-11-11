// A RECEIVER: everything that is ONE radio of the engine (multi-radio engine, stage 1; docs/multi-radio-records/engine-stages-1-3.md).
//
// The engine used to hold one receiver as about 180 loose data members: its Pipeline, its device and the open / fault / retry machinery around it, the
// settings it mirrors, its recorders, its health counters and its published state. This header is where they live now. Engine holds
// `receivers_`, an array of kMaxReceivers slots; slot 0 always exists and is the receiver every client has always used. The other slots stay empty
// until stage 4 (RADIO_ADD).
//
// THE MOVE IS PURE. Each member was moved with its declaration, its initialiser and its comment unchanged, and Engine keeps a reference of the same
// name bound to slot 0 (`float& volume_ = receivers_[0]->volume_;`), so every function of the engine, the friend classes and the tests read and write
// exactly what they did. The data has one home (here); the engine's own code is converted to name a slot only where a command or a read is
// addressed to one (stage 3 onwards). Do not add a member to Engine for a thing that is one radio's: add it here.
//
// THE ORDER OF THE MEMBERS IS THE ORDER OF DESTRUCTION: the Pipeline (and the threads under it) is declared first, so it is destroyed last, after the
// open future and the other state that reads it, exactly as it was in Engine.
//
// Nothing in this header locks or starts anything: it is data. The threads are the Pipeline's.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/airspy_settings.hpp"
#include "core/audio_open.hpp"
#include "core/bias_tee.hpp"
#include "core/noise_floor_tracker.hpp"
#include "core/pipeline.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/recorder.hpp"
#include "core/record_finish.hpp"
#include "core/retune_coalescer.hpp"
#include "core/tune_control.hpp"
#include "engine/engine_types.hpp"
#include "engine/record_start.hpp"
#include "source/device_source.hpp"
#include "source/soapy_source.hpp"

namespace cascade::engine {

// THE LIMIT OF THIS BUILD: how many receivers the engine can hold, and (the owner's rule, stage 0 measurement 2026-11-10) how many radios may be open
// in total, patch radios included. ONE constant: every structure that is per radio is sized by it, `FoxCommand.target` is a number, the facts are keyed
// by number and a client reads `fact.radio.max`. Nothing may assume four.
inline constexpr std::size_t kMaxReceivers = 8;

// WHAT A DEVICE OPEN WORKER HANDS BACK (it was Engine::DeviceOpenResult; Engine keeps the name as an alias). Off-thread SoapySDR discovery and open:
// SoapySDR::Device::enumerate()/make() do USB bus discovery and, for a B200, an FPGA/firmware load: seconds of blocking work, so they run on a worker
// and the pass polls the future each frame. The device is only ever touched by the pass once the future resolves.
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
// AN AIRSPY's REMEMBERED GAIN MODE, GAINS AND DECIMATION (0.99.41), copied
// from airspyMemory_ on the GUI thread by launchDeviceOpen so the worker
// can put them on the radio straight after open() and BEFORE it asks for
// the rate: the saved rate is a decimated one, and asked for first it is
// matched against undecimated rates and "coerced" to the wrong one.
std::optional<cascade::core::AirspySetting> airspyAtOpen;
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
// THIS OPEN IS ONE OF pollSavedRadioRetry's TRIES at the radio the start-up restore found busy (0.99.92). It travels INSIDE the result so
// that finishDeviceOpen, which sees only the answer, knows whether a failure should arm the next try; an answer the user has moved on from
// drops it with the rest.
bool savedRadioRetry = false;
};

// WHAT THE AUTOMATIC AUDIO BUFFER LAST DID (it was Engine::AudioLeadNote; Engine keeps the name as an alias).
enum class AudioLeadNote { kNone, kRaised, kRestored, kLowered };

struct Receiver {
    explicit Receiver(cascade::core::Pipeline::Config cfg) : pipeline_(cfg) {}
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // The slot table for a new engine: slot 0 built, the rest empty.
    static std::array<std::unique_ptr<Receiver>, kMaxReceivers> makeSlots(cascade::core::Pipeline::Config cfg) {
        std::array<std::unique_ptr<Receiver>, kMaxReceivers> slots;
        slots[0] = std::make_unique<Receiver>(cfg);
        return slots;
    }


    // ---- pipeline ----
    // DSP pipeline plus the two live display widgets it feeds. The views are
    // held by unique_ptr for two reasons: the forward declarations above, and
    // the waterfall's GL texture, whose deletion needs the creating GL context
    // current - run() tears the view down explicitly before destroying the
    // context, because AppWindow itself outlives it (destroyed in main()).
    cascade::core::Pipeline pipeline_;
    // ---- device, open, fault and retry ----
    bool faultSeen_ = false;  // endTakesOnFault's edge: the latch as last frame saw it
    // The fault sentence the list block carries, WHOLE (the pipeline's, or the test stand-in's): the state's
    // faultMessage cuts it at 127 bytes. Kept so the per-pass fingerprint hashes it without a copy.
    std::string apiFaultText_;
    // --- Reopening after an absorbed driver fault (0.90.1) -----------------
    // When the automatic reopen was last attempted, in ImGui::GetTime()
    // seconds; negative = never. gui::autoReopenDue holds the next attempt
    // off for kSoapyReopenHoldoffSec after this, so a radio that is really
    // gone is tried once, not in a loop.
    double soapyReopenAttemptSec_ = -1.0;
    // --- Trying a busy saved radio again (0.99.92; engine/saved_radio_retry.hpp) ---
    // Tries made so far (0..4), when the last failure happened in retryClockS() seconds, and whether that failure was BUSY - the only class that
    // is retried. Armed by applyConfig's restore failure; cleared when the user chooses another source or a try fails any other way. ALL OF IT IS
    // THE PASS'S: the restore (before the control thread starts, or on the pass), finishDeviceOpen, selectSource, openIqFile and
    // launchSoundCardOpen are all the pass's, like the rest of the open machinery. (Master kept these in the window, stamped in the interface's
    // clock; the engine has its own, clockS(), and the headless engine retries a busy saved radio exactly as the window's does.)
    int savedRadioRetryAttempts_ = 0;
    double savedRadioRetryLastFailSec_ = -1.0;
    bool savedRadioRetryBusy_ = false;
    // TESTS ONLY (tests/test_saved_radio_retry_app.cpp): when it is not negative the retry reads THIS as the time, so a test places the stamps and
    // the polls at exact seconds, as master's test did with ImGui's clock. Never set in the product.
    double savedRadioRetryClockS_ = -1.0;
    std::future<DeviceOpenResult> deviceOpenFuture_;
    bool deviceOpenPending_ = false;
    std::string deviceBusyLabel_;  // device name shown while an open is in flight
    // Paces hardware retunes - see retuneSourceHz. 50 ms: invisible against
    // the wheel gesture, one apply per notch burst instead of one per frame.
    cascade::core::RetuneCoalescer retuneCoalescer_{50.0};
    // The isPluginPreset a retune was requested with, carried across the
    // coalescer alongside its frequency - see retuneSourceHz. Overwritten on
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
    // Atomic since stage 3b: the control side moves it, and a submit on the
    // front end's thread stamps each queued command with it.
    std::atomic<std::uint64_t> sourceGen_{0};
    std::uint64_t deviceOpenReqGen_ = 0;
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
    // needed a rule of its own for (core/bias_tee.hpp, rtlBiasTeeAtOpen).
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
    // The ACTIVE source's kind as the config store spells it. Tracked at each
    // successful switch because the pipeline does not expose source identity.
    // "siggen"|"file"|"soapy"|"rtlsdr"|"hackrf"|"airspy"|"airspyhf"|
    // "sdrplay"|"mirisdr"|"rx888"|"pluto"|"soundcard"
    std::string sourceKind_ = "siggen";
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
    // ---- settings mirrors ----
    bool vfoBandwidthDragged_ = false;
    // THE USER'S OWN MUTE, kept apart from the plugin mute (mutedBy_ and the
    // rest). They are different things with different lifetimes: a plugin's
    // mute is recomputed from the tuning every frame in updateAudioMute, and
    // one that also cleared a user's mute would make the Mute key stop working
    // the moment a decoder was running. The pipeline is told the OR of the two.
    bool userMuted_ = false;
    // Display range for both the spectrum axis and the waterfall colormap.
    float dbMin_ = -110.0f;
    float dbMax_ = 0.0f;
    // Radio/Sinks control state. The pipeline owns the live DSP values; these
    // mirrors exist because ImGui widgets edit by pointer. Defaults match the
    // pipeline's own defaults (WFM, 150 kHz bandwidth, -50 dB squelch) except
    // the VFO offset, which the constructor pushes to +300 kHz so the demo
    // tone 0 sits on the VFO - near-silent in WFM (an unmodulated carrier
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
    int deemphIndex_ = 0;  // index into kDeemphUs; 0 = 50 us (global default)
    // Last REQUESTED VFO bandwidth (Hz): combo presets and band-edge drags
    // both land here, and this is what the overlay and the config store use.
    // (The combo keeps showing its last preset after an edge drag - the combo
    // is a preset picker, not a readback; the overlay is the truth.)
    double vfoBandwidthHz_ = 150000.0;
    // --- P7 feature state -----------------------------------------------------
    // Panel mirrors for the pipeline's stereo / NR / notch settings (ImGui
    // edits by pointer). Defaults match AppConfig's, which match the
    // pipeline's own construction defaults, so the three can never disagree
    // before the first user click.
    bool stereoEnabled_ = true;
    bool nrEnabled_ = false;
    float nrStrength_ = 0.5f;
    // AGC choice and noise blanker (lane C): the engine's mirrors, FOXAPI_AGC_* and dB. agcFixedGainDb_ is the value last set or loaded; while the
    // mode is Off the pipeline's own (the gain frozen at the switch) is the truth and is what is published and saved (agcFixedGainShown, engine.cpp).
    int agcMode_ = 0;
    float agcFixedGainDb_ = 0.0f;
    bool nbEnabled_ = false;
    float nbThresholdDb_ = 15.0f;
    // Tone squelch (engine wiring lane 1, group B): FOXAPI_TONE_*, the CTCSS tone (Hz, 0 any), the DCS code as digits (0 any) and its polarity (0 any), and the tone
    // remover. Off, any, any, any, off by default: the audio as it always was.
    int toneKind_ = 0;
    double toneHz_ = 0.0;
    int toneCode_ = 0;
    int tonePol_ = 0;
    bool toneRemove_ = false;
    // S-meter display units (engine wiring lane 1): FOXAPI_METER_*, dBFS by default.
    int meterUnits_ = 0;
    // Noise-reduction method (engine wiring lane 1): FOXAPI_NR_*, standard by default.
    int nrMethod_ = 0;
    bool noiseSquelchOn_ = false;           // the FM noise squelch (engine wiring lane 1)
    double noiseSquelchThresholdDb_ = -30.0;
    double noiseSquelchTailMs_ = 100.0;
    double cwPitchHz_ = 700.0;   // the CW sidetone pitch (engine wiring lane 1)
    // The manual notch bank (engine wiring lane 1): ten slots, each off at 1000 Hz and 50 Hz wide until set.
    struct ManualNotchSlot {
        double hz = 1000.0;
        double widthHz = 50.0;
        bool on = false;
    };
    std::array<ManualNotchSlot, 10> manualNotch_{};
    bool notchEnabled_ = false;
    float notchFreqHz_ = 1000.0f;
    float notchQ_ = 30.0f;
    bool autoNotch_ = false;
    // AUTOMATIC SQUELCH (stub campaign B12, engine_squelch_auto.cpp; 0.99.74): the top bar's AUTO key under the SQUELCH dial, the engine's so that a headless engine,
    // a web page and a CAT client honour it too. While squelchAuto_ is lit the pass keeps the receiver's threshold 8 dB over the noise floor measured under the
    // channel (noiseFloor_: the quietest of the last ten seconds, a carrier that holds the gate open never teaching it) or, while the Airband monitor listens,
    // re-applies the monitor's own "above the noise" rule. A hand on any threshold - an accepted SET_SQUELCH, the monitor's squelch or its above-the-noise key - turns
    // it off (squelchAutoOff); a threshold restored from the config does not.
    bool squelchAuto_ = false;
    cascade::core::NoiseFloorTracker noiseFloor_;
    double squelchAutoSetS_ = -1.0e9;       // when the rule last moved a threshold (it is judged four times a second)
    double squelchAutoAbsHz_ = 0.0;         // the channel the floor was measured on: another one is another floor
    double squelchAutoBwHz_ = 0.0;
    int squelchAutoMode_ = -1;
    bool squelchAutoOnAirband_ = false;     // the rule was last applied to the monitor's squelch
    // ---- recorders ----
    // THE DISK IS NOT TOUCHED ON THE PASS (0.99.63 / 0.99.65; gui/record_start.hpp, core/record_finish.hpp). Recorder::start's create_directories
    // and fopen waited for the disk on the control thread - whose key the transmitter judges by its heartbeat - and a slow one froze the pass.
    // A start is PREPARED here (the cheap half: a refusal that needs no disk is reported at once), its file opened by RecordStart's worker and
    // ARMED when it arrives (pollRecordStarts); a stop detaches the file without touching the disk and RecordFinisher closes it on a worker.
    // A Record pressed while the previous take's file is still being closed is remembered (iqStartQueued_ / audioStartQueued_) and starts the
    // moment that file is closed, so a Stop followed by a Record in one breath still gives the take that was asked for and an open never
    // lands beside a file still being finalised (a file name has a one-second resolution).
    cascade::gui::RecordStart iqStart_;
    cascade::gui::RecordStart audioStart_;
    bool iqStartQueued_ = false;
    bool audioStartQueued_ = false;
    std::shared_ptr<cascade::core::RecordFinisher::Ticket> iqFinish_;
    std::shared_ptr<cascade::core::RecordFinisher::Ticket> audioFinish_;
    // --- Recorder state (P6) --------------------------------------------------
    // Two independent Recorder instances so IQ and audio takes can run
    // simultaneously (each records ONE kind at a time by its contract). The
    // pipeline holds non-owning pointers to them only while a take is live;
    // stop*Recording clears the pointer before stopping the recorder.
    cascade::core::Recorder iqRecorder_;
    cascade::core::Recorder audioRecorder_;
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
    // ---- health counters ----
    // Output-stream watchdog (see pollAudioHealth). The note is shown in the
    // Sinks panel: a stream that had to be restarted is something the user
    // should be told about, because the alternative reading of the same
    // event - audio that stopped and came back on its own - is indistinguish-
    // able from a fault in their radio.
    double lastAudioProbeSec_ = 0.0;
    int audioRecoveries_ = 0;
    std::string audioHealthNote_;
    int audioBufferAutoMs_ = 0;
    AudioLeadNote audioLeadNote_ = AudioLeadNote::kNone;
    int audioLeadNoteMs_ = 0;
    int audioLeadNoteCount_ = 0;
    int audioLeadQuietMinutes_ = 0;                                    // consecutive quiet, played minutes (the way back down)
    // THE DEVICE OPEN, OFF THIS THREAD. Field report "hang ntdll.dll @
    // InitializeWaveHandles" (0.96.4): picking an output device put the GUI
    // thread inside waveOutOpen for 57 seconds. See core/audio_open.hpp for the
    // whole argument; what matters here is that nothing on this thread may
    // query the sink while inFlight() is true.
    cascade::gui::AudioOpen audioOpen_;
    // Once-a-minute starvation digest (see pollAudioHealth). Sampled every
    // frame - not gated behind the 1 Hz watchdog above - because a ring can
    // dip and recover well inside a second at 48 kHz, and a low-water mark
    // read only once a second would miss most of the dips it exists to
    // report. SIZE_MAX so the very first frame's real reading always beats
    // the sentinel instead of needing a separate "have we sampled yet" flag.
    double lastAudioLogSec_ = 0.0;
    std::size_t audioRingLowWaterFrames_ = SIZE_MAX;
    std::uint64_t audioUnderrunsAtLogStart_ = 0;
    std::uint64_t audioPrimingAtLogStart_ = 0;
    // A MINUTE THAT RELOADED PLUGINS IS NOT JUDGED (master 0.99.102, 8c0a4f41; the 0.99.104 merge). The 2026-10-29 field report: 27 starved
    // callbacks during nine store installs in one minute raised the buffer to 960 ms - a deliberate stall of the thread that reloads, read as
    // "this computer fell behind", costing a second of audio latency for the next quarter hour. pluginScans_ (declared further down, with the
    // lists' fingerprint) already counts every rescanPlugins() before any work, early return included - master's pluginRescans_ is this
    // counter, so there is no second one; the mark is where it stood when the minute opened. The first load (Engine::initialise) is consumed
    // into the mark right after it, so the session's first minute is still judged on its own figures.
    std::uint64_t audioRescansAtLogStart_ = 0;
    // A USER'S STOP IS NOT THE MACHINE FALLING BEHIND (master 0.99.102, bde914f6): each stop of a running receiver starves the sink's last
    // callback once, and three stops in a minute met the raise threshold on their own. receiverStops_ counts them (stopReceiver, running
    // stops only); the mark is where the count stood when the minute opened. closeAudioMinute subtracts the minute's stops from its starved
    // callbacks before judging a raise - one callback per stop, never below zero. The pass thread's, like closeAudioMinute.
    std::uint64_t receiverStops_ = 0;
    std::uint64_t audioStopsAtLogStart_ = 0;
    // The pipeline ring's minute (pollDspMinute / closeDspMinute): its own clock, and the monotonic pipeline counters as they stood when
    // it last closed (the drop and read counts, and - master 0.99.101 - the writes that were cut).
    double lastDspLogSec_ = 0.0;
    std::uint64_t dspDroppedAtMinute_ = 0;
    std::uint64_t dspReadAtMinute_ = 0;
    std::uint64_t dspCutAtMinute_ = 0;
    // The session's maxima of the two marks closeDspMinute takes each minute (the ring's high water, the longest DSP block), for the
    // bundle's dsp-ring line; publishRingFigures() adds the minute still open to them and publishes (core::setRingFigures).
    std::size_t dspRingHighWaterSession_ = 0;
    std::uint64_t dspBlockLongestNsSession_ = 0;
    // ---- the slot's own life (multi-radio stage 4) ----
    // Which radio this is, from 1 (slot + 1); set once when the slot is built.
    std::uint32_t radioNumber_ = 1;
    // WHAT THE ENGINE-WIDE MIRRORS THAT ARE ONE RADIO'S SAY WHILE THIS SLOT IS THE ONE BEING WORKED ON (Engine::RadioWork): the Source list's selected
    // row, the I/Q file's path and the two saved-config device spellings live in Engine (the window's and the saved settings' one set), so a command
    // addressed to a radio other than the first swaps them for this slot's for the length of the call and puts the first radio's back. Slot 0 never uses
    // them (its are the engine's).
    int sourceSel_wide_ = 0;
    // ...and the three more that are the open radio's panel and memory: its bias tee panel (present / shown / remembered), the gains it had before auto gain
    // switched on, and the crystal correction last applied to it.
    cascade::gui::BiasTeePanel biasTeePanel_wide_;
    std::vector<std::pair<std::string, float>> manualGainsBeforeAgc_wide_;
    double ppmRadioApplied_wide_ = 0.0;
    std::string iqOpenPath_wide_;
    std::string cfgSoapyArgs_wide_;
    std::string cfgNativeArgs_wide_;
    // WHERE THIS RADIO'S AUDIO GOES (RADIO_AUDIO: AudioOut::kRoute*), and whether the solo rule holds it silent (RADIO_SOLO). The route is the user's
    // choice and survives a solo; the silence is the engine's and is recomputed from which radio is solo.
    int audioRoute_ = 0;
    // A test radio's tone, Hz from the centre ("" device: the signal generator), for the label and for the tests; 0 = none set.
    double testToneOffsetHz_ = 0.0;
    // The device key this radio asked the ledger for when it was added ("" for the generator), kept for the refusal and the facts.
    std::string claimedKey_;
    // The words fact.radio.<n>.refusal carries when the radio's device could not be had (the ledger's sentence); cleared by the next good open.
    std::string refusal_;
    // A radio added on a file, a Pluto or an rtl_tcp server opens on a worker (as the first radio's panel does): until the open lands, the generator stands in, and when it fails the
    // reason is the radio's refusal. True from the add until the pass has seen the open end either way (multi-radio engine stage 7).
    bool openWatch_ = false;
    // The open device's tunable range (fact.radio.<n>.range), asked of the device once it is open (a driver may not answer the first time) and kept: rangeKnown_ false = none yet.
    // Pass thread only. The fingerprint of the lists block hashes it, so the fact appears when it becomes known.
    bool rangeKnown_ = false;
    double rangeLoHz_ = 0.0;
    double rangeHiHz_ = 0.0;
    // When the radio asked for a stop of its own accord (RADIO_REMOVE is in progress), so the pass does not start it again. Pass thread only.
    bool removing_ = false;
    // ---- published state ----
    // THE ONE RECEIVER SNAPSHOT (engine stage 2, core/receiver_snapshot.hpp),
    // declared first so the API core can be built on it: publishReceiverState
    // fills it once a frame, and the plugin host API, the web server and CAT
    // all answer from it. Shared, because plugin bridges outlive this window.
    std::shared_ptr<cascade::core::ReceiverSnapshot> receiverSnapshot_ =
        std::make_shared<cascade::core::ReceiverSnapshot>();
};  // struct Receiver

}  // namespace cascade::engine
