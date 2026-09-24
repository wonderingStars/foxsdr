// sdrplay_source.hpp - FoxSDR's SDRplay driver: a DeviceSource that drives an
// RSP through the SDRplay API the user installed, with no SoapySDR module in
// the path and nothing of SDRplay's linked at build time or shipped in the
// installer.
//
// WHY THIS ONE IS SHAPED DIFFERENTLY FROM EVERY OTHER NATIVE DRIVER HERE.
// The RTL-SDR, HackRF, Airspy R2/Mini and Airspy HF+ drivers speak their
// radios' USB protocols over our own WinUSB transport: we own the handle, we
// own the reader thread, and every wait is ours to bound. An RSP cannot be
// reached that way. SDRplay publish no device protocol; the tuner is
// programmed by a Windows SERVICE that holds the USB handle, and the only
// documented way in is sdrplay_api.dll talking to that service. So:
//
//   - there is no src/usb here and no USB of ours anywhere in this file;
//   - the DLL is loaded at RUNTIME from the documented install path, falling
//     back to the loader's own search, and its entry points are resolved into
//     a table of function pointers (src/source/sdrplay_api_decl.hpp);
//   - when that DLL is absent, enumeration is empty and sdrPlayApiAdvice()
//     supplies the one sentence the Source section shows instead of a radio.
//
// Everything above the seam is the same as the other drivers: the same
// DeviceSource interface, the same ring, the same read() contract, the same
// stream-health line, the same faulted()/deviceDead() story. A reader who has
// understood hackrf_source.hpp has understood this one's outside.
//
// THE THREAD IS THEIRS, NOT OURS, AND THAT IS THE WHOLE DIFFERENCE INSIDE.
// sdrplay_api_Init hands the service two callbacks and the service calls them
// on a thread it created. We never join that thread and we cannot bound it.
// What we can do is make what it touches safe to touch: the stream callback
// converts the service's int16 pairs into our ring, the event callback raises
// faulted(), and BOTH work through a Link held by shared_ptr whose address is
// the cbContext. The API documents that Uninit returns with the callbacks
// stopped; if a callback is nevertheless still inside us when we are done
// with the device, the Link is deliberately STRANDED (kept alive in a
// process-scope graveyard) rather than freed under a thread that is in it -
// the same judgement the USB drivers make when they abandon a wedged reader,
// because a leak is survivable and a use-after-free on somebody else's thread
// is not.
//
// WHAT WE CANNOT BOUND, SAID PLAINLY. sdrplay_api_Open, LockDeviceApi,
// GetDevices, SelectDevice, GetDeviceParams, Init, Uninit, ReleaseDevice and
// Close take no timeout and offer no cancellation. If the service wedges,
// those block for as long as it takes; there is no argument we can pass and no
// handle we can close to shorten them. Our own waits (kReadWait, kUpdateWait,
// kCallbackDrainWait and, from 0.96.1, kEnumerateWait) are bounded and named
// below, and the difference between the two categories is stated again in the
// shutdown-budget notes rather than left to be discovered.
//
// sdrplay_api_Update belongs on that list too and was left off it, because
// the reference makes it inline and a healthy one returns in milliseconds. A
// wedged service took five seconds to refuse one and the window went with it
// (0.96.2) - see kControlWait, which now treats it the way kEnumerateWait
// treats a scan.
//
// AND ONE OF THOSE UNBOUNDABLE CALLS IS NO LONGER WAITED ON. ENUMERATION is
// the only one of them a user can provoke at will - the source combo scans
// every time it opens - and a wedged service turned that into a frozen
// application (0.96.1, two hang reports). We still cannot cancel the call; we
// stopped waiting on it instead, by running the vendor half on a worker that
// is ABANDONED on expiry. See kEnumerateWait; 0.96.2 did the same for every
// live CONTROL (kControlWait), which is the other thing a user can provoke at
// will on an open radio. Every other call in that list is made with a device
// already open and is still unbounded, because abandoning a thread that is
// inside SelectDevice or Init would leave the service holding a radio nothing
// in this process could ever release.
//
// ...AND THE TEARDOWN AFTER AN ABANDONED CONTROL IS NO LONGER ONE OF THEM.
// This paragraph used to end "and that includes the Uninit and ReleaseDevice
// that follow an abandoned control, which is the price of having a thread we
// cannot recall". That price was paid by the user, not by us: 0.96.4's hang
// report is a GUI thread inside stopStreamingLocked's sdrplay_api_Uninit,
// under Pipeline::quiesceSourceThreadLocked, seconds after the log recorded
// an abandoned retune and "a worker is still inside sdrplay_api_Update". One
// call at a time is what the API's own device lock means, so the teardown
// queued behind the very thread we had already given up on, and the window
// went with it - the same freeze 0.96.3 had just moved off the control.
//
// So the rule is now stated once, for the whole file: WHEN A WORKER HAS BEEN
// ABANDONED INSIDE THE VENDOR DLL, OR THE SERVICE HAS DECLARED ITSELF GONE,
// NOTHING OF OURS ENTERS THAT DLL FOR THIS DEVICE AGAIN - not Update, not
// Uninit, not ReleaseDevice, not Close. The handle is left to the thread that
// is still in there, the Link is stranded rather than freed (nothing has told
// the service to stop calling us, so there is no safe moment to free it), and
// stop() and closeDevice() return without waiting on anything at all. What
// that costs is real and is said plainly rather than discovered: the RSP
// stays selected in the service and this process's SDRplay session is
// finished until FoxSDR is restarted - which is the same restart the "restart
// the SDRplay API service, then open the radio again" sentence already asks
// for. See vendorUnreachableLocked().
//
// ...AND THE RULE ONLY WORKS IF EVERY FAILURE THAT MEANS IT GETS RECORDED.
// 0.97.1's hang report is the same freeze one call further round: a GUI
// thread inside closeDevice's sdrplay_api_ReleaseDevice, with the log's
// PREVIOUS line reading "SDRplay Uninit failed -
// sdrplay_api_ServiceNotResponding (14)" - stop()'s own Uninit, not a
// control's Update. noteIfServiceDead had only ever been wired to
// updateLocked's failures, so that Uninit answering the exact error the rule
// above is about left serviceGone_ false, and closeDevice() a few log lines
// later found vendorUnreachableLocked() still saying it was safe to call
// ReleaseDevice. It was not. Every call that can answer
// sdrplay_api_ServiceNotResponding now reports it through noteIfServiceDead,
// not only the one that first needed it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dsp/spsc_ring.hpp"
#include "source/device_source.hpp"
#include "source/sdrplay_api_decl.hpp"

namespace cascade::source {

// --- the API itself -------------------------------------------------------

// The process's one table, resolved on first use and never unloaded. Empty
// (resolved == false) when the API is not installed, which is the ordinary
// case on a machine with no RSP and is not an error.
//
// THE PATH IS TRIED BEFORE THE SEARCH ORDER, deliberately. SDRplay's
// installer puts the DLL at a fixed place and does NOT put it on PATH, so the
// documented absolute path is the one that works; the bare-name fallback is
// for a user who has arranged their own copy, and it comes second so that a
// stray sdrplay_api.dll beside some other application cannot pre-empt the real
// install.
const sdrplay_abi::Api& processSdrPlayApi();

// The documented 64-bit install path, exposed so the log and the tests can
// both name the same string.
const char* sdrPlayApiDllPath();

#if !defined(_WIN32)
// The Linux SONAME dlopen() is asked for first - the versioned name
// SDRplay's own .run installer registers with ldconfig - exposed for the same
// reason sdrPlayApiDllPath() is: so this string cannot drift from what
// tests/test_sdrplay_source.cpp checks it against.
const char* sdrPlayApiSoName();
#endif

// THE SENTENCE THE SOURCE SECTION SHOWS WHEN THERE IS NO RADIO TO SHOW.
// Pure: it takes what the load found rather than looking again, so it can be
// proved without an install. Empty string when the API is present and new
// enough - i.e. when there is nothing to say.
//
// `version` is what sdrplay_api_ApiVersion answered, or 0 when the DLL was
// never loaded and the question never asked.
std::string sdrPlayApiAdvice(bool resolved, float version);

// WHY THE LAST ENUMERATION LISTED NOTHING, in the enumeration's own words.
//
// Added because the too-old-API sentence could not reach the screen. The
// version a session learns is recorded on the table only when the session
// SUCCEEDS (sessionAcquire), and a version below kMinApiVersion fails before
// that line - so the Source section, which composes its sentence from the
// table's `resolved` and `version`, was asking sdrPlayApiAdvice(true, 0.0f)
// and being answered with silence. An RSP owner running API 3.05 therefore
// got an empty Source section and no instruction at all, while the log two
// inches away carried the exact sentence telling them to update it.
//
// So enumerateSdrPlayWith records the reason it skipped - the same string it
// logs, not a paraphrase - and this hands it back. Empty when the last
// enumeration got as far as asking for the device list, which is every
// machine with a working install: a stale sentence there would send its owner
// to reinstall an API that is already fine.
//
// Process-scope and last-writer-wins, like the table it describes: there is
// one SDRplay API per process and one Source section looking at it.
std::string sdrPlayLastEnumerationSkip();

// WHAT THE SOURCE SECTION SHOWS WHEN THERE IS NO RSP ROW. Pure, so the panel's
// whole decision is one testable line: the enumeration's own reason when it
// has one, and otherwise what can be said from the load result alone (which
// is what the panel did before, and still covers a draw that happens before
// any enumeration has run). Empty means there is nothing to say.
std::string sdrPlayPanelAdvice(bool resolved, float version, const std::string& enumerationSkip);

// --- enumeration ----------------------------------------------------------

// HOW LONG A SCAN WILL WAIT FOR THE SDRPLAY SERVICE BEFORE GIVING UP ON IT.
//
// WHY THIS HAD TO EXIST (0.96.1, two hang reports from one RSP1A on API 3.15).
// The file header above says plainly that sdrplay_api_Open, LockDeviceApi and
// GetDevices take no timeout and offer no cancellation, and treats that as
// something to document rather than something to survive. A user's service
// then died with the device still open: every call answered
// sdrplay_api_ServiceNotResponding, and a SCAN from the Source section - on
// the GUI thread, which is where scanNative() runs - went into sessionAcquire
// and never came back. The hang watchdog filed it as
// "hang ntdll.dll @ cascade::source::enumerateSdrPlayWith".
//
// We still cannot cancel those calls. What we can do is stop WAITING on them:
// the vendor half runs on a worker, this is how long the caller gives it, and
// on expiry the worker is ABANDONED - detached, never joined, never spoken to
// again - and the panel is told why. Three seconds because a healthy service
// answers a device list in milliseconds and a user pressing Refresh will
// tolerate three seconds once; anything longer and the window is visibly
// stuck, which is the fault being fixed.
inline constexpr std::chrono::milliseconds kEnumerateWait{3000};

// ...AND HOW LONG THE NEXT SCANS LEAVE IT ALONE AFTER ONE ABANDONMENT.
//
// A dead service does not recover in a frame, and scanNative() runs every time
// the source combo is opened. Without this, every one of those would spend
// another three seconds of GUI thread and leak another abandoned worker into a
// service that is still wedged. During the hold-off the SDRplay step is
// skipped without touching the API at all, and the panel keeps the sentence
// that says what to do about it.
inline constexpr std::chrono::seconds kEnumerateHoldOff{60};

// WHAT THE PANEL AND THE LOG SAY WHEN THE SERVICE DID NOT ANSWER. One string,
// so the sentence a user reads is the sentence a test pins - the same rule
// sdrPlayApiAdvice follows, and for the same reason: it is the only
// instruction an RSP owner gets.
const char* sdrPlayServiceHungSentence();

// WHAT A LIVE CONTROL SAYS WHEN THE SERVICE NEVER ANSWERED IT. The same rule
// as the sentence above and the same reason: one string, pinned by a test,
// because it is the whole of what an RSP owner is told about a receiver that
// has just stopped responding to its own panel. Composed by noteFaultOn into
// "<control>: <this>", so it names the remedy and not the control.
//
// Separate from the ServiceNotResponding wording because the two are
// genuinely different events: that one is the service ANSWERING a refusal,
// this one is the service never answering at all and a thread of ours left
// inside the vendor DLL for good. See SdrPlaySource::kControlWait.
const char* sdrPlayControlHungSentence();

// WHAT A SCAN AND AN OPEN SAY ONCE THE PROCESS'S SESSION IS LOST (0.99.28).
// After a worker is abandoned inside the vendor DLL or the service answers
// sdrplay_api_ServiceNotResponding, the device's session is orphaned and can
// never be closed, so nothing in the process calls the SDRplay API again - the
// 0.99.27 crash was a scan's GetDevices through that session. One string,
// pinned by a test, like the two above; unlike the hold-off it never expires,
// so it names both restarts the user needs.
const char* sdrPlaySessionLostSentence();

// True while the hold-off above is still running, i.e. the last enumeration
// abandoned a wedged service and the next ones are skipping it.
bool sdrPlayEnumerationHeldOff();

// TESTS ONLY: forget the hold-off. Safe to call at any time because an empty
// hold-off IS the state this process starts in - unlike a registry populated
// by init(), which is why that one is never cleared.
void sdrPlayClearEnumerationHoldOffForTest();

// Every RSP the service can see, as the Source section wants them.
//
// THIS IS THE ONE ENUMERATION IN THE APPLICATION THAT IS SAFE AT ANY TIME.
// usb_device.hpp's rule 1 - enumeration never opens a device - exists because
// opening a USB radio to ask its name steals it from whatever is streaming.
// sdrplay_api_GetDevices does not touch USB at all: it asks the SERVICE for
// its list, which the service maintains whether or not anything is streaming.
// It is still taken under LockDeviceApi, because that is the API's own rule
// for reading the list consistently against another process selecting from it.
std::vector<NativeDeviceInfo> enumerateSdrPlay();

// The same, through a given table - the seam the tests enumerate through.
std::vector<NativeDeviceInfo> enumerateSdrPlayWith(const sdrplay_abi::Api& api);

// --- the pure halves ------------------------------------------------------
//
// Everything below can be proved without an RSP, an API or a service, which
// is why each is a free function rather than a method: they are the parts of
// the driver that a machine with no SDRplay hardware can still hold to
// account.

// "RSP1A", "RSPduo", "RSPdx-R2"; "RSP (hw 9)" for one we do not know, because
// a number the user can quote is worth more than the word UNKNOWN.
std::string sdrPlayModelName(unsigned char hwVer);

// What the combo shows: "SDRplay RSP1A (serial 1234567890)".
std::string sdrPlayLabel(const sdrplay_abi::DeviceT& dev);

// The antennas a model has. An RSPduo in single-tuner mode is presented as
// two antennas, "Tuner 1" and "Tuner 2", because from the Source section's
// point of view that is exactly what choosing a tuner is: choosing which
// socket the signal comes in on. Its Hi-Z port is NOT offered at this stage -
// see setAntenna.
std::vector<std::string> sdrPlayAntennas(unsigned char hwVer, sdrplay_abi::RspDuoModeT duoMode);

// How many LNA states the model has, from the reference's own per-model table
// (SoapySDRPlay3 Settings.cpp getGainRange, which states the maximum; the
// count is that plus one). These are the counts for the BROADEST band; the
// hardware has fewer in some bands and the API clamps, which is why
// setGainDb clamps to this and reports what it programmed.
int sdrPlayLnaStateCount(unsigned char hwVer);

// The output rates this driver offers, ascending. Everything at or below
// 2 MS/s is reached by decimating a 6 MHz low-IF or a higher zero-IF rate -
// the RSP's own front end does not run below 2 MS/s - and everything above is
// the ADC rate itself.
std::vector<double> sdrPlaySupportedRatesHz();

// How one output rate is actually produced: the rate to program into
// DevParams::fsFreq, the decimation to put in ControlParams, the IF the tuner
// must run at, and the channel filter that goes with it.
struct SdrPlayRatePlan {
    double fsHz = 0.0;
    unsigned int decM = 1;
    unsigned int decEnable = 0;
    unsigned int wideBandSignal = 0;
    sdrplay_abi::IfKHzT ifType = sdrplay_abi::IF_Zero;
    sdrplay_abi::BwMHzT bwType = sdrplay_abi::BW_1_536;
};

// False for a rate that is not in sdrPlaySupportedRatesHz().
bool sdrPlayRatePlan(double outputRateHz, SdrPlayRatePlan& out);

// The channel filter for an output rate, the reference's own ladder.
sdrplay_abi::BwMHzT sdrPlayBwForRate(double outputRateHz);

// --- the driver -----------------------------------------------------------

class SdrPlaySource : public DeviceSource {
public:
    SdrPlaySource() = default;
    ~SdrPlaySource() override;

    SdrPlaySource(const SdrPlaySource&) = delete;
    SdrPlaySource& operator=(const SdrPlaySource&) = delete;

    // --- the bounded waits, and only the bounded ones ---------------------
    //
    // Class-scope statics rather than namespace constants because
    // hackrf_source.hpp and airspy_source.hpp already declare kReadWait and
    // kStreamHealthWindow at cascade::source scope and src/gui/app_window.hpp
    // includes all of them; rtlsdr_source.hpp solved the same collision the
    // same way. tests/test_shutdown_budget.cpp finds a class-static as
    // readily as a namespace one, and each of these needs a row in its
    // kKnownWaits table.

    // How long read() waits for the service's callback to put something in
    // the ring before answering the IqSource contract's "nothing yet, retry"
    // zero. Spent on the PIPELINE's source thread, never the GUI's.
    static constexpr std::chrono::milliseconds kReadWait{20};

    // HOW LONG A LIVE CONTROL WAITS FOR sdrplay_api_Update ITSELF.
    //
    // WHY THIS HAD TO EXIST (a third hang report, the same RSP1A, this one on
    // 0.96.2 and API 3.09; bounded here in 0.96.3). 0.96.1 bounded the SCAN
    // and made the device dead on the first
    // sdrplay_api_ServiceNotResponding, and both of those worked
    // exactly as written - the log has the retune failing with (14) and the
    // released-radio line right after it. What neither of them touched is how
    // long the vendor call took to SAY (14): about five seconds, which is the
    // hang watchdog's whole frame threshold, spent on the GUI thread because
    // that is the thread a panel retune or a tuner drag calls the setter on.
    // The watchdog therefore filed a hang whose captured stack was the NEXT
    // frame's SwapBuffers, the call having returned in the meantime - a
    // graphics-looking report for a service fault.
    //
    // So this is the scan's treatment applied to every live control:
    // updateLocked runs sdrplay_api_Update on a worker, waits this long, and
    // on expiry ABANDONS it - detached, never joined, never spoken to again -
    // marks the device dead and refuses every later control for it.
    //
    // ONE SECOND. A healthy Update only QUEUES the request and returns - the
    // service reports completion separately through the changed flags that
    // kUpdateWait below waits for - so on a working install it is a
    // milliseconds-scale call, and SoapySDRPlay3 makes it inline with no
    // bound at all. A second therefore leaves better than an order of
    // magnitude for a service that is slow but alive, while the worst a
    // control can now cost the GUI thread is kControlWait + kUpdateWait =
    // 1500 ms, comfortably inside HangWatchdog::kDefaultThresholdMs's 5000
    // and well short of the five seconds that produced the report.
    //
    // The cost on the healthy path is one std::thread per live control, which
    // is tens of microseconds against a call that crosses into a Windows
    // service; and it is paid only while STREAMING, because updateLocked
    // sends nothing when the parameter block is not yet live.
    static constexpr std::chrono::milliseconds kControlWait{1000};

    // How long a live parameter change waits for the service to CONFIRM it.
    // sdrplay_api_Update returns as soon as the request is queued; the
    // service reports completion by setting grChanged / rfChanged / fsChanged
    // in the next stream callback. The reference waits 500 ms for that
    // (SoapySDRPlay3's updateTimeout) and carries on with a warning, which is
    // the right trade: the parameter is already programmed, the flag is only
    // the acknowledgement, and blocking the GUI past half a second for an
    // acknowledgement is worse than logging that it never came.
    static constexpr std::chrono::milliseconds kUpdateWait{500};

    // After Uninit returns, how long we wait for a stream or event callback
    // that is still INSIDE us to leave before the Link is freed. The API
    // documents that Uninit returns with callbacks stopped, so on every
    // healthy path this expires zero times; when it does expire the Link is
    // stranded rather than freed (see the file header), because the thread it
    // would be freed under is the service's and we cannot join it.
    static constexpr std::chrono::milliseconds kCallbackDrainWait{250};

    // Not a wait: how much streaming is tallied before one "source: stream
    // health ..." line is written. Matches SoapySource's window exactly, so a
    // report from an RSP is comparable with one from any other radio.
    static constexpr std::chrono::milliseconds kStreamHealthWindow{60000};

    // TESTS ONLY, and per instance so two tests can never see each other's
    // API. There is no RSP and no SDRplay API on the machine this driver was
    // written on: a fake table that answers the way the service does IS the
    // proof, so the seam that admits it is part of the design.
    void setApiForTest(const sdrplay_abi::Api* api);

    // --- DeviceSource ----------------------------------------------------

    const char* driverKey() const override { return "sdrplay"; }

    // Takes a NativeDeviceInfo::args string: "serial=<SerNo>" picks a device
    // by the serial the service reported (case-insensitive, and a suffix
    // match so the short form a user reads off SDRuno still finds it),
    // "index=N" picks the Nth in enumeration order. An empty string takes the
    // first. "tuner=1" or "tuner=2" picks an RSPduo's tuner at open; anything
    // else ignores it.
    //
    // A successful open has connected to the service, checked the API
    // version, taken the device lock, read the device list, SELECTED the
    // device (which is what makes it ours and takes it away from SDRuno), got
    // the parameter block, and put the radio in a KNOWN STATE: 2 MS/s out of
    // the 6 MHz front end at the 1.62 MHz IF (which is how the API's own
    // reference produces 2 MS/s - the RSP's ADC does not run there), 100 MHz,
    // 1.536 MHz channel filter, IF gain reduction 40 dB, LNA state 0, AGC off,
    // DC and IQ correction on, bias tee off, notches off. Nothing streams
    // until start().
    bool open(const std::string& args) override;

    // Uninit if streaming, then ReleaseDevice. Idempotent, safe on a
    // never-opened instance, safe from the destructor. lastError() survives
    // it. Does NOT close the process's connection to the service: see
    // sdrplay_abi::Api's session note.
    //
    // ...UNLESS THE VENDOR DLL IS UNREACHABLE, in which case it makes no call
    // into it at all - no ReleaseDevice, no Close - and the session reference
    // is ORPHANED rather than released. See the file header.
    void closeDevice() override;

    bool isOpen() const override { return openMirror_.load(std::memory_order_relaxed); }

    // TWO GAINS, AND THE FIRST OF THEM COUNTS DOWNWARDS IN THE HARDWARE.
    //
    // "IF" is the tuner's IF gain REDUCTION (GainT::gRdB), 20 to 59 dB, and a
    // bigger number means LESS signal. Presenting it that way would put one
    // slider in the Source panel where right is quieter while every other
    // slider's right is louder - the inconsistency the Airspy HF+'s
    // attenuator was turned over to avoid. So "IF" IS PRESENTED AS A NEGATIVE
    // GAIN: the range is -59..-20 dB, -20 is the most signal the tuner will
    // give, -59 the least, and gainDb("IF") reports the negative of the
    // reduction actually programmed. There is exactly one place that flips the
    // sign (ifGainToReduction/reductionToIfGain in the .cpp).
    //
    // "LNA" is GainT::LNAstate, which is an INDEX into a per-model, per-band
    // table the API owns and publishes no decibel mapping for. It is reported
    // as GainUnit::Steps for the same reason the Airspy R2's gains are:
    // printing register step 7 as "7.0 dB" would be a number no instrument
    // produced in a unit the radio does not use.
    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;

    // The RSP has a real AGC in the API: AgcT::enable picks the loop and
    // setPoint_dBfs picks where it holds the signal.
    bool autoGainSupported() const override { return true; }
    bool setAutoGain(bool on) override;
    bool autoGain() const override { return autoGain_.load(std::memory_order_relaxed); }

    // Where the AGC holds the signal, in dBFS. Negative; the API's default is
    // -60. Not part of DeviceSource, so the Source section reaches it through
    // the concrete type when it grows a control.
    bool setAgcSetPointDbfs(int dbfs);
    int agcSetPointDbfs() const { return agcSetPoint_.load(std::memory_order_relaxed); }

    // Per model - see sdrPlayAntennas. On an RSPdx this is the A/B/C input
    // switch; on an RSPduo it is which tuner. A tuner change on a LIVE stream
    // goes through sdrplay_api_SwapRspDuoActiveTuner; on a stopped device it
    // is a release-and-reselect, because the tuner is chosen at SelectDevice
    // and there is no other way to move it.
    std::vector<std::string> antennas() const override;
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override;

    std::vector<double> supportedSampleRatesHz() const override;

    // 1 kHz to 2 GHz, the range the API accepts for every model but the
    // original RSP1 (which starts at 10 kHz and which this driver does not
    // treat specially - the API refuses below its own floor and we report
    // that refusal).
    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // --- IqSource --------------------------------------------------------

    // sdrplay_api_Init with our two callbacks. That call IS the start: the
    // service begins delivering the moment it returns. Idempotent while
    // running; false with lastError() when there is no device.
    bool start() override;

    // sdrplay_api_Uninit, then the bounded drain. Idempotent, safe before
    // open. Makes NO vendor call when the DLL is unreachable (see the file
    // header): the Link is stranded and it returns, because the one call that
    // could still be in flight belongs to a thread we cannot recall.
    void stop() override;

    bool running() const override { return running_.load(std::memory_order_relaxed); }

    // The radio paces this source: the pipeline must not clock it.
    bool selfPaced() const override { return true; }

    double sampleRateHz() const override { return sampleRateHz_.load(std::memory_order_relaxed); }

    // Coerced to the nearest rate in sdrPlaySupportedRatesHz(), then turned
    // into the (fsHz, decimation, IF, filter) plan that produces it and
    // applied in ONE Update carrying every reason that actually changed -
    // which is what the API wants and what stops a rate change from being
    // three separate disturbances to the stream.
    bool setSampleRateHz(double hz) override;

    double centerFrequencyHz() const override {
        return centerFrequencyHz_.load(std::memory_order_relaxed);
    }

    // Live: write rfHz, one Update(Tuner_Frf), and a bounded wait for the
    // service's own acknowledgement. Refused with a reason outside the range
    // rather than clamped, because a receiver that silently listens somewhere
    // else is worse than one that says no.
    bool setCenterFrequencyHz(double hz) override;

    // Drains the ring the service's callback fills. Blocks at most kReadWait
    // when it is empty and then returns 0 - the self-paced contract's
    // "nothing yet, retry".
    std::size_t read(std::complex<float>* dst, std::size_t n) override;

    // True once the event callback has reported the device gone or the
    // service failed. The pipeline's source loop polls it and stops with
    // lastError().
    bool faulted() const override;

    const char* name() const override;
    const char* lastError() const override;

    // --- the switches that are not gains, antennas or rates ---------------
    //
    // Deliberately NOT in antennas(): a bias tee is power on a connector, not
    // a choice of where to listen, and a control that can damage whatever is
    // plugged in should never be reachable by something iterating a list of
    // port names.
    bool biasTeeSupported() const;
    bool setBiasT(bool on);
    bool biasT() const { return biasT_.load(std::memory_order_relaxed); }

    // The broadcast-FM notch (RSP1A/1B/duo/dx: rfNotchEnable) and the DAB
    // notch (rfDabNotchEnable). Both are hardware filters in front of the
    // tuner; on a model without them these answer false and change nothing.
    bool rfNotchSupported() const;
    bool setRfNotch(bool on);
    bool rfNotch() const { return rfNotch_.load(std::memory_order_relaxed); }

    bool dabNotchSupported() const;
    bool setDabNotch(bool on);
    bool dabNotch() const { return dabNotch_.load(std::memory_order_relaxed); }

    // The RSPdx's HDR mode: a different front-end path below 2 MHz with a much
    // better dynamic range. RSPdx and RSPdx-R2 only.
    bool hdrModeSupported() const;
    bool setHdrMode(bool on);
    bool hdrMode() const { return hdrMode_.load(std::memory_order_relaxed); }

    // --- what open() learned ---------------------------------------------
    unsigned char hardwareVersion() const { return hwVer_.load(std::memory_order_relaxed); }
    std::string serialNo() const;
    // What the API answered for its own version, so a problem report carries
    // it without anyone having to find the installer.
    float apiVersion() const { return apiVersion_.load(std::memory_order_relaxed); }

    // The last gain-change event's calibrated gain, in dB, as the service
    // computed it. Zero before any such event, which haveCurrentGainDb()
    // distinguishes from a real zero. Exposed because with the AGC on it is
    // the only honest answer to "what gain is the radio actually at".
    double currentGainDb() const;
    bool haveCurrentGainDb() const;

    // How many overload events the service has reported since start(). The
    // event is logged by the next read() (never on the service's thread, see
    // Link::pendingEventLogs); this is what a test can assert on.
    std::uint64_t overloadEvents() const;

    // --- stream health ---------------------------------------------------
    //
    // THE READER'S HALF of the window: what read() counts on the pipeline's
    // source thread, under healthMutex. The callback's half - blocks, samples,
    // overflows, gaps, and when the window opened - is lock-free atomics in
    // the Link (0.99.32), because the callback may not take a lock; see
    // streamCallbackA.
    struct StreamHealth {
        std::uint64_t timeouts = 0;
        std::uint64_t errors = 0;
    };

    // The line for the window so far, and the window starts again. Empty when
    // nothing has been read since the last line.
    std::string streamHealthLine();
    void setStreamHealthWindowForTest(std::chrono::milliseconds w);

    // Samples the service delivered that did not fit in the ring - the host
    // fell behind, not the radio.
    std::uint64_t droppedSamples() const;

    // Links this PROCESS has stranded because a callback was still inside one
    // when the device was released. 0 on every healthy path; the delta is what
    // a test asserts, because elapsed time alone still passes when the bound
    // is deleted.
    static unsigned long long linksStranded();

private:
    // Everything the SERVICE's thread touches, behind a shared_ptr whose
    // address is the cbContext. See the file header for why it can outlive
    // the SdrPlaySource.
    struct Link {
        explicit Link(std::size_t ringCapacity) : ring(ringCapacity) {}

        cascade::dsp::SpscRing<std::complex<float>> ring;

        // EVERYTHING THE EVENT CALLBACK NEEDS TO ANSWER AN OVERLOAD LIVES
        // HERE, not behind a pointer back to the SdrPlaySource, and that is
        // the whole reason the Link can be stranded safely: a callback that
        // arrives after this object is gone still has a valid table, a valid
        // device handle and a valid tuner, and touches nothing that has been
        // freed. Written by startStreamingLocked before Init, cleared after
        // Uninit, and never touched while the service is calling.
        const sdrplay_abi::Api* api = nullptr;
        void* dev = nullptr;
        sdrplay_abi::TunerSelectT tuner = sdrplay_abi::Tuner_Neither;

        // Raised while a callback is inside us, so closeDevice can tell "the
        // service has stopped calling" from "it has not yet".
        std::atomic<int> inCallback{0};
        // Cleared by stop(); a callback that arrives after it sees false and
        // drops the block rather than filling a ring nobody will drain.
        std::atomic<bool> accepting{false};

        // read() parks here when the ring is empty; the stream callback
        // signals after every block it writes - WITHOUT taking waitMutex
        // (0.99.32). The waiters re-check on a bound of their own (kReadWait,
        // updateLocked's 1 ms poll), so a notify that races a waiter costs at
        // most that bound and never a sample.
        std::mutex waitMutex;
        std::condition_variable waitCv;

        // The service's own acknowledgements, from StreamCbParamsT. Cleared
        // before an Update and waited on after it.
        std::atomic<int> grChanged{0};
        std::atomic<int> rfChanged{0};
        std::atomic<int> fsChanged{0};

        // The error slot: written by the service's thread, read by the GUI
        // and by the pipeline's source loop. A std::string written on one
        // thread and read on another is UB, not a stale value.
        mutable std::mutex errorMutex;
        std::string lastError;
        bool faulted = false;
        bool deviceDead = false;
        std::string deadWhat;

        // THE CALLBACK'S HALF OF THE HEALTH WINDOW, LOCK-FREE (0.99.32).
        //
        // Until 0.99.32 the stream callback counted into StreamHealth under
        // healthMutex and, when a window ran out, WROTE THE LINE - the
        // diagnostic log's lock, fwrite and fflush, on the service's thread.
        // The 0.99.27 RSP2 report and the 0.97.0 RSPdx report both end with a
        // window holding exactly ten blocks after that write, and then a
        // service that never delivered again and answered Uninit with
        // sdrplay_api_ServiceNotResponding. So the callback now only adds to
        // these, and read() - on the pipeline's own thread - opens, closes
        // and writes the window. Accurate to a block at a window boundary,
        // which is the price of not locking and is invisible in a line that
        // counts a minute of them.
        //
        // Times are steady_clock nanoseconds since its epoch; 0 means "none".
        std::atomic<std::uint64_t> cbReads{0};
        std::atomic<std::uint64_t> cbWithSamples{0};
        std::atomic<std::uint64_t> cbSamples{0};
        std::atomic<std::uint64_t> cbOverflows{0};
        std::atomic<std::int64_t> cbLongestGapMs{0};
        std::atomic<std::int64_t> cbWindowStartNs{0};
        std::atomic<std::int64_t> cbLastSamplesNs{0};

        // What the EVENT callback would have logged, left for read() to log
        // (0.99.32, the same rule): one bit per kind of line, see kEventLog*.
        std::atomic<unsigned int> pendingEventLogs{0};

        mutable std::mutex healthMutex;
        StreamHealth health;
        bool healthEverWritten = false;
        std::chrono::milliseconds healthWindow = kStreamHealthWindow;
        std::atomic<std::uint64_t> dropped{0};
        std::atomic<std::uint64_t> overloads{0};

        // The last gain-change event's calibrated gain.
        std::atomic<double> currGainDb{0.0};
        std::atomic<bool> haveGainDb{false};
    };

    // NEVER REASSIGNED, hence const: a stranded callback holds this address,
    // so a source that swapped in a fresh link could have a new device behind
    // a service thread still filling the old one.
    const std::shared_ptr<Link> link_ = std::make_shared<Link>(kRingCapacitySamples());

    // A quarter of a second of signal at 10 MS/s, which is the fastest this
    // driver offers, against a pipeline that asks in 10 ms chunks.
    static constexpr std::size_t kRingCapacitySamples() { return 1u << 22; }

    // The three callbacks the service is given. Static because a member
    // function has no C signature.
    //
    // THE cbContext IS THE LINK AND NOTHING ELSE - never `this`. The API takes
    // ONE context for all three callbacks, and if that context were the
    // SdrPlaySource then a callback arriving after the object was destroyed
    // would be a use-after-free on a thread we cannot join. Everything the
    // callbacks need is in the Link, which is held by shared_ptr and stranded
    // rather than freed when a callback is still inside it.
    static void streamCallbackA(short* xi, short* xq, sdrplay_abi::StreamCbParamsT* params,
                                unsigned int numSamples, unsigned int reset, void* ctx);
    static void streamCallbackB(short* xi, short* xq, sdrplay_abi::StreamCbParamsT* params,
                                unsigned int numSamples, unsigned int reset, void* ctx);
    static void eventCallback(sdrplay_abi::EventT eventId, sdrplay_abi::TunerSelectT tuner,
                              sdrplay_abi::EventParamsT* params, void* ctx);

    // The *Locked helpers assume devMutex_ is held.
    const sdrplay_abi::Api& api() const;
    bool acquireSessionLocked(std::string& error);
    void releaseSessionLocked();

    bool selectByArgsLocked(const std::string& args);
    bool getParamsLocked();
    void applyKnownStateLocked();
    // One Update carrying every reason that changed, plus the bounded wait on
    // whichever acknowledgement flag the reasons imply. Returns false and
    // fills lastError when the API refuses; a missing acknowledgement is a
    // warning, not a failure.
    bool updateLocked(sdrplay_abi::ReasonForUpdateT reason,
                      sdrplay_abi::ReasonForUpdateExt1T ext1, const char* what);
    bool startStreamingLocked();
    void stopStreamingLocked();
    // Release and re-select the same device on the other tuner, restoring the
    // parameters we had. The only way to move an RSPduo's tuner while stopped.
    bool reselectTunerLocked(sdrplay_abi::TunerSelectT tuner);

    // The current channel's parameter block - rxChannelB when the selected
    // tuner is B, rxChannelA otherwise.
    sdrplay_abi::RxChannelParamsT* chParamsLocked() const;

    static void setErrorOn(Link& link, std::string msg);
    static void noteFaultOn(Link& link, const char* what, const std::string& detail);

    // TRUE WHEN NO THREAD OF OURS MAY ENTER THE VENDOR DLL FOR THIS DEVICE
    // AGAIN - the whole of the rule the file header states, in one place so
    // that updateLocked, stopStreamingLocked and closeDevice cannot drift
    // apart about it. devMutex_ held, like every other *Locked helper.
    //
    // DELIBERATELY NOT deviceDead(). That is raised by an UNPLUGGED radio too
    // (eventCallback's DeviceRemoved), and an unplugged radio leaves a healthy
    // service that answers Uninit and ReleaseDevice in microseconds - calls
    // which are exactly what lets the next RSP be opened. Skipping them there
    // would trade a hang nobody has reported for a receiver that cannot be
    // re-plugged without restarting the application. Only the two conditions
    // below mean the SERVICE is gone.
    bool vendorUnreachableLocked() const { return controlAbandoned_ || serviceGone_; }

    // A SERVICE THAT HAS STOPPED ANSWERING IS A DEAD DEVICE, NOT A FAILED CALL.
    //
    // sdrplay_api_ServiceNotResponding (14) means the thing holding the USB
    // handle is gone; nothing we send afterwards can succeed. Treated as an
    // ordinary refusal it produced the 0.95.0 report's second half: a retune,
    // an LNA change, an AGC change and finally Uninit each answered 14, the
    // pipeline kept reading, and the stream-health line recorded 1910 timeouts
    // over fifty seconds while the user watched a dead receiver. Raising
    // faulted()/deviceDead() instead puts it through the path a removed radio
    // already takes - Pipeline's source thread latches the fault, stops, and
    // the Source section says what happened. True when the error was that one.
    bool noteIfServiceDead(sdrplay_abi::ErrT err, const char* what);

    void setError(std::string msg);
    void clearError();

    void setName(std::string n);
    static std::string healthLineLocked(Link& link);   // link.healthMutex held
    // Service thread: atomics only. Never a lock, never the log.
    static void noteBlock(Link& link, std::size_t samples, bool dropped);
    // The pipeline's source thread (read()): closes a window that has run
    // out and writes its line, and logs what the event callback left.
    static void maybeWriteHealth(Link& link);
    static void drainEventLogs(Link& link);

    // The bits of Link::pendingEventLogs.
    static constexpr unsigned int kEventLogOverload = 1u;
    static constexpr unsigned int kEventLogOverloadCorrected = 2u;
    static constexpr unsigned int kEventLogRemoved = 4u;
    static constexpr unsigned int kEventLogFailure = 8u;
    static constexpr unsigned int kEventLogMasterLost = 16u;

    // Serialises API calls against each other and against
    // open/start/stop/close. The service's callbacks do NOT take it: they
    // only touch the Link.
    mutable std::mutex devMutex_;

    const sdrplay_abi::Api* api_ = nullptr;  // null means the process table
    bool sessionHeld_ = false;

    sdrplay_abi::DeviceT device_{};
    sdrplay_abi::DeviceParamsT* deviceParams_ = nullptr;
    bool selected_ = false;
    bool initialised_ = false;

    // TRUE ONCE A CONTROL'S WORKER HAS BEEN ABANDONED INSIDE THE VENDOR DLL,
    // and from then on no control touches the API for this device again.
    // There is a thread of ours parked in sdrplay_api_Update that nothing can
    // recall; a second control would park a second one, and a panel's sliders
    // are not short of clicks. Guarded by devMutex_ like everything else the
    // *Locked helpers read, and cleared by open() - a fresh session is a
    // fresh device, the same judgement clearError() makes about deviceDead.
    bool controlAbandoned_ = false;

    // TRUE ONCE THE SERVICE HAS ANSWERED sdrplay_api_ServiceNotResponding for
    // this device. The other half of vendorUnreachableLocked(), and separate
    // from controlAbandoned_ because the two are different facts: that one
    // says a thread of ours is parked in the DLL, this one says the thing
    // holding the USB handle has told us it is gone. Either makes a further
    // vendor call pointless at best - 0.95.0's report is four of them in a row
    // answering (14) - and, on the evidence of 0.96.4's stack, a frozen window
    // at worst. Set under devMutex_ by noteIfServiceDead, cleared by open()
    // for the same reason controlAbandoned_ is: a fresh session is a fresh
    // device.
    bool serviceGone_ = false;

    // Lock-free mirrors, so per-frame GUI readouts never wait behind an API
    // call in flight.
    std::atomic<bool> openMirror_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> sampleRateHz_{0.0};
    std::atomic<double> centerFrequencyHz_{0.0};
    std::atomic<int> ifReductionDb_{40};
    std::atomic<int> lnaState_{0};
    std::atomic<bool> autoGain_{false};
    std::atomic<int> agcSetPoint_{-60};
    std::atomic<bool> biasT_{false};
    std::atomic<bool> rfNotch_{false};
    std::atomic<bool> dabNotch_{false};
    std::atomic<bool> hdrMode_{false};
    std::atomic<unsigned char> hwVer_{0};
    std::atomic<float> apiVersion_{0.0f};

    mutable std::mutex nameMutex_;
    std::string name_ = "SDRplay: (no device)";
    std::string serial_;
    std::string antenna_ = "RX";
};

}  // namespace cascade::source
