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
};  // struct Receiver

}  // namespace cascade::engine
