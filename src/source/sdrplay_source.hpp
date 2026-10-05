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
// finished until FoxSDR is restarted - which is the restart every sentence
// the user is shown then asks for ("restart the SDRplay API service, then
// restart FoxSDR"; until 0.99.36 two of them said "then open the radio again",
// which a lost session refuses). See vendorUnreachableLocked().
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
// ...AND "NOT YET" IS NO LONGER "NEVER" (0.99.50, the RSPdx-R2 report). The
// rule above still holds while a worker is inside the DLL, but a control that
// misses kControlWait is now LISTENED FOR until kControlGrace before the radio
// is given up, and a successful late answer gives it back. The specification
// sets no time limit on sdrplay_api_Update and says it may stop and restart
// the stream; see kControlGrace.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "dsp/spsc_ring.hpp"
#include "source/device_source.hpp"
#include "source/sdrplay_api_decl.hpp"
#include "source/sdrplay_service.hpp"

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

// WHETHER THE SDRPLAY API IS INSTALLED ON THIS MACHINE - the DLL was found and
// every entry point resolved. Free-standing rather than a one-line caller of
// processSdrPlayApi().resolved because the native Mirics driver (0.99.42's
// RSP1A/RSPdx field reports) needs the same answer without taking on a
// dependency on the whole SdrPlaySource type: an RSP's silicon is reachable
// both ways, and mirisdr_source.cpp asks this ONE question before ever
// touching the bus. Cheap after the first call - processSdrPlayApi() resolves
// the DLL once per process and every rsp_rows/enumerateSdrPlay caller has
// almost always already paid that cost by the time a device is opened.
bool sdrPlayApiPresent();

// Overrides the answer above for a test - no LoadLibrary, no real DLL needed.
// std::nullopt restores the real answer (processSdrPlayApi().resolved).
void setSdrPlayApiPresentForTest(std::optional<bool> present);

// --- ONE CHIP, ONE ROUTE: what the process knows about the API's health -------
//
// An RSP1/RSP1A/RSP2 is a Mirics chip, and FoxSDR can reach one four ways: its
// native SDRplay driver (through the API), its native Mirics driver, and
// SoapySDR's `sdrplay` and `miri` modules. The two SoapySDR modules run inside
// this process exactly as the API does - SoapySDRPlay3 calls the same vendor
// library, SoapyMiri drives the same silicon - so none of the four can be
// allowed to ignore what the others have learned. Two field reports (0.99.59,
// one Linux RSP1A): a SoapyMiri read() faulting on a radio the SDRplay service
// also owned, and a heap corruption seconds after the patch page opened the
// radio through SoapySDRPlay3 AFTER the native driver had declared the API
// session lost ("no further SDRplay API calls are made until FoxSDR is
// restarted" - and the Soapy module simply made them).
//
// This is the part of that rule that is a fact about the PROCESS, readable
// without a call, a lock or a wait (see Api::sessionLostFlag). The decision
// made from it is pure and lives in rsp_rows.hpp (miricsSoapyRouteRefusal).
struct SdrPlayApiState {
    bool installed = false;        // the API is on this machine (sdrPlayApiPresent)
    bool sessionLost = false;      // markSessionLost: this process's session is finished
    bool workerAbandoned = false;  // an enumeration worker was abandoned inside the API and
                                   // nothing since has shown the service answering again
};

// The state of one table, atomics only - NEVER the session mutex, which an
// abandoned worker may hold. `installed` is the table's own `resolved`.
SdrPlayApiState sdrPlayApiStateOf(const sdrplay_abi::Api& api);

// The process's: the test override when one is set, otherwise
// sdrPlayApiPresent() plus the process table's latches. Loads the vendor
// library on first use (one failed LoadLibrary where there is none), so ask it
// only about a driver that is in the Mirics family.
SdrPlayApiState sdrPlayApiState();

// Overrides the answer above for a test. std::nullopt restores the real one.
void setSdrPlayApiStateForTest(std::optional<SdrPlayApiState> state);

// THE NATIVE SCAN AS IT STOOD BEFORE THE DUPLICATE RSP ROWS WERE HIDDEN - the
// API's own rows plus every native Mirics row, including the ones the Source
// list no longer shows. That list is the only record of WHICH radios the API
// manages, and it lives in the GUI; a SoapySDR open is made on a worker that
// cannot see it. Published by AppWindow::scanNative, read by
// soapyMiricsRefusal below. Process-scope and last-writer-wins, like the
// enumeration's skip reason.
void sdrPlayPublishNativeRows(std::vector<NativeDeviceInfo> rows);
std::vector<NativeDeviceInfo> sdrPlayPublishedNativeRows();
// Whether any scan has published since the process started. Until one has, the
// rows are unknown - not "empty" - and a caller that is about to open a
// SoapySDR Mirics device on its own (a saved patch) scans first.
bool sdrPlayNativeRowsPublished();

// THE ONE QUESTION EVERY SOAPY OPEN ASKS BEFORE IT MAKES A DEVICE: "may this
// SoapySDR device be opened, given everything the other routes to the same
// chip know?" Empty when it may (including for every driver outside the
// Mirics family, which it never even looks up); otherwise the sentence the
// user is shown as the reason the radio would not open. Gathers the state and
// hands it to the pure decision (rsp_rows.hpp), so SoapySource::open - the one
// place every receiver, patch, restore and fallback open of a SoapySDR device
// passes through - cannot be bypassed by a new call site.
std::string soapyMiricsRefusal(const std::string& soapyArgs);

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

// WHAT A CONTROL, A SCAN AND AN OPEN SAY WHILE AN EARLIER CONTROL IS STILL
// WAITING FOR THE SERVICE'S ANSWER (SdrPlaySource::kControlGrace, 0.99.50).
// Not a failure sentence: nothing has been given up yet, so it names no
// restart. One string, pinned by a test, like the ones above.
const char* sdrPlayControlPendingSentence();

// WHAT A SCAN AND AN OPEN SAY ONCE THE PROCESS'S SESSION IS LOST (0.99.28).
// After a worker is abandoned inside the vendor DLL or the service answers
// sdrplay_api_ServiceNotResponding, the device's session is orphaned and can
// never be closed, so nothing in the process calls the SDRplay API again - the
// 0.99.27 crash was a scan's GetDevices through that session. One string,
// pinned by a test, like the two above; unlike the hold-off it never expires,
// so it names both restarts the user needs.
const char* sdrPlaySessionLostSentence();

// WHAT THE RECEIVER SAYS WHEN THE SERVICE SIMPLY STOPS DELIVERING (0.99.36).
// Every other sentence above is the answer to a CALL; this one is for the
// stream going quiet with nothing having been asked - the 0.95.0 RSP1A,
// 0.97.0 RSPdx, 0.97.1 RSP1 and 0.99.27 RSP2 logs each show gaps of 5 to 98
// seconds during which FoxSDR still called itself running and said nothing.
// See SdrPlaySource::kStreamStallLimit. One string, pinned by a test.
const char* sdrPlayStreamStalledSentence();

// --- the service behind the API (0.99.55) ----------------------------------
//
// WHY THE LAST ATTEMPT TO REACH THE SDRPLAY SERVICE FAILED, as recorded on the
// table (sdrplay_abi::Api::serviceTrouble). None once an Open succeeds. It is
// what the Source section's RESTART SDRPLAY SERVICE key is shown for - the
// three ways the service can be "not answering" as the user sees it.
enum class SdrPlayServiceTrouble {
    None = 0,
    OpenFailed = 1,       // sdrplay_api_Open refused (the 0.99.52 RSP2 Pro report)
    EnumerationHung = 2,  // a scan's worker was abandoned (kEnumerateWait)
    SessionLost = 3       // markSessionLost: this process's session is finished
};

SdrPlayServiceTrouble sdrPlayServiceTrouble(const sdrplay_abi::Api& api);

// WHETHER THE KEY IS SHOWN. Pure: on Windows only, only for a service Windows
// actually found (a restart must name it), and only while there is trouble to
// fix or a restart is still in progress (so its outcome stays in view).
bool sdrPlayRestartKeyShown(bool windows, const SdrPlayServiceStatus& service,
                            SdrPlayServiceTrouble trouble, SdrPlayRestartPhase phase);

// WHAT TO DO ONCE THE SERVICE HAS BEEN RESTARTED - the decision the key's
// completion turns on, pure so every branch is testable without a window.
//
//   ServiceNotRunning  Windows still does not report it running: say its
//                      state (sdrPlayServiceAdvice) and do nothing else.
//   ReopenInProcess    this process never got a session (sessions == 0, not
//                      lost, no control waiting, and the session state could
//                      be read without waiting): rescan and reopen the saved
//                      radio now, no FoxSDR restart.
//   RestartFoxSdr      anything else - above all a LOST session, where a
//                      worker of ours may still be parked inside the vendor
//                      DLL (see markSessionLost and the file header). The
//                      latch stays set; the user is told to restart FoxSDR.
enum class SdrPlayAfterRestart { ServiceNotRunning, ReopenInProcess, RestartFoxSdr };

SdrPlayAfterRestart sdrPlayAfterServiceRestart(bool serviceRunning, bool sessionStateKnown,
                                               int sessions, bool sessionLost,
                                               int controlsInFlight);

// The same decision applied to a table: reads its session state WITHOUT
// WAITING (a try_lock - an abandoned enumeration worker may hold the session
// mutex inside sdrplay_api_Open, and the GUI thread must not queue behind
// it), and for ReopenInProcess clears the enumeration's skip reason, its
// hold-off and the trouble record so the next scan asks the API again.
// NEVER clears sessionLost.
SdrPlayAfterRestart sdrPlayApplyServiceRestart(const sdrplay_abi::Api& api, bool serviceRunning);

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

// The output rates this driver offers, ascending. Everything below 2 MS/s is
// reached by decimating a zero-IF ADC rate of at least 2 MS/s (2 MS/s itself
// for the binary fractions, 3.072 MS/s for the audio rates), 2 MS/s is the
// API's own default ADC rate undecimated, and everything above is the ADC rate
// itself. No plan uses the low-IF front end since 0.99.44 - see
// sdrPlayRatePlan in the .cpp for the field logs that retired it.
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
    // updateLocked runs sdrplay_api_Update on a worker and waits this long
    // for it ON THE CALLER'S THREAD. On expiry the caller is released and the
    // worker is left to answer late - see kControlGrace for what happens then.
    //
    // ONE SECOND IS THE GUI'S BUDGET, NOT THE SERVICE'S. Until 0.99.50 this
    // comment said "a healthy Update only QUEUES the request and returns",
    // and the one second was justified by that. SDRplay API Specification
    // 3.15 says no such thing: sdrplay_api_Update (section 3.17, p27) carries
    // no time bound at all, and "if required it will stop the stream, change
    // the values and then start the stream again, otherwise it will make the
    // changes directly". The 0.99.46 RSPdx-R2 report is what that cost: an
    // LNA change given up at one second while the service - on the report's
    // own stream-health arithmetic - was still delivering, i.e. still working
    // on it, 1.28 s after it was sent. What one second IS good for is the
    // thread that waits: the worst a control can cost the GUI thread stays
    // kControlWait + kUpdateWait = 1500 ms, comfortably inside
    // HangWatchdog::kDefaultThresholdMs's 5000.
    //
    // The cost on the healthy path is one std::thread per live control, which
    // is tens of microseconds against a call that crosses into a Windows
    // service; and it is paid only while STREAMING, because updateLocked
    // sends nothing when the parameter block is not yet live.
    static constexpr std::chrono::milliseconds kControlWait{1000};

    // Not a wait: HOW LONG A CONTROL THE SERVICE HAS NOT YET ANSWERED IS
    // GIVEN TO ANSWER LATE, counted from when it was sent (0.99.50).
    //
    // WHY. Through 0.99.49 a control not answered within kControlWait was
    // ABANDONED on the spot: the device dead, the process's session lost, and
    // the user told to restart the service and FoxSDR - even if the call came
    // back successfully a moment later, which nothing ever looked at. Now the
    // worker is left in the DLL for up to this long, and NOTHING of ours
    // enters the DLL meanwhile - no control (refused at once with
    // sdrPlayControlPendingSentence()), no scan or open (Api::controlsInFlight),
    // no overload acknowledgement, and no teardown: a stop() in the grace is
    // treated exactly as a stop after an abandonment, the 0.96.4 hang report's
    // rule. When the answer arrives (seen by the next read() on the
    // pipeline's source thread, or by the next control):
    //
    //   - Success: the radio is given back, the readbacks follow the block;
    //   - any other refusal: the call is over and the service alive, so the
    //     block is put back (BlockRollback) and the radio kept;
    //   - sdrplay_api_ServiceNotResponding: the service is gone, and said so.
    //
    // If it has not arrived when this runs out, the radio is given up exactly
    // as it was at kControlWait before. Nothing WAITS on this - it is a
    // deadline read by the reader and the next control - so the GUI's budget
    // and the teardown's are unchanged.
    //
    // TEN SECONDS. The specification gives no bound. The longest a service in
    // the field reports has taken to answer at all is the 0.96.2 report's
    // five seconds (an answer of ServiceNotResponding, which this now names
    // correctly instead of calling it a hang); twice that is the margin.
    // While a control is waiting the stall rule (kStreamStallLimit) stands
    // aside, because an Update that stops and restarts the stream is silent
    // by design.
    static constexpr std::chrono::milliseconds kControlGrace{10000};
    // TESTS ONLY: a shorter grace. Zero (or anything up to kControlWait)
    // gives the radio up at kControlWait, the pre-0.99.50 behaviour, for the
    // tests that are about what happens once it has been given up.
    void setControlGraceForTest(std::chrono::milliseconds g);

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

    // Not a wait either: HOW LONG A RUNNING STREAM MAY GO WITHOUT A SINGLE
    // CALLBACK before the service is treated as gone (0.99.36).
    //
    // WHY. A service that stops delivering raises nothing: no call is in
    // flight to answer ServiceNotResponding, so faulted() stayed false and the
    // receiver called itself running on an empty ring until the user happened
    // to press something - 49 s in the 0.95.0 RSP1A log, 51 s in the 0.99.27
    // RSP1 log, 85 s in the 0.99.27 RSP2 log, 99 s in the 0.97.0 RSPdx log,
    // every one of them ending in a service that answered the next call with
    // sdrplay_api_ServiceNotResponding. A healthy RSP's longest gap in those
    // same logs is 45 to 55 ms, and a rate change's reset is a fraction of a
    // second, so five seconds is two orders of magnitude clear of anything a
    // working service does.
    //
    // CHECKED IN read(), on the pipeline's source thread, and costs nothing
    // there: one clock read on a read that came back empty. On expiry the
    // stream is treated exactly as a service that answered (14): the fault is
    // raised with sdrPlayStreamStalledSentence(), the session is marked lost,
    // and the teardown makes no call into the vendor DLL (the 0.96.4 and
    // 0.97.0 hang reports are a teardown entering a wedged service).
    static constexpr std::chrono::milliseconds kStreamStallLimit{5000};

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
    // the parameter block, and put the radio in a KNOWN STATE: 2 MS/s zero-IF
    // with nothing decimated (the API's own default ADC rate; until 0.99.44
    // this was the 6 MHz front end at the 1.62 MHz IF, which two RSP2 field
    // logs show delivering 6 MS/s), 100 MHz,
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
    // TESTS ONLY: a shorter kStreamStallLimit, so a test can prove the stall
    // is named without waiting five seconds for it.
    void setStreamStallLimitForTest(std::chrono::milliseconds w);

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
        // Uninit, and never touched while the service is calling. `tuner` is
        // the tuner active AT INIT and only a fallback: an RSPduo's tuner can
        // be swapped live without this being rewritten, so the acknowledgement
        // goes to the tuner the service names in the event itself.
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

        // THE STALL CLOCK (0.99.36), separate from the health window's because
        // that one is reset every time a window closes. lastCallbackNs is
        // written by EVERY stream callback, samples or not - a callback at all
        // proves the service is still calling us; streamStartNs is set just
        // before Init so a service that never delivers a first block is caught
        // too. Both steady_clock ns, 0 meaning "none". See kStreamStallLimit.
        std::atomic<std::int64_t> lastCallbackNs{0};
        std::atomic<std::int64_t> streamStartNs{0};
        // THE READER'S HALF (the 0c59853 review): when the current run of
        // back-to-back empty reads began, and when the last empty read was.
        // A stall needs BOTH clocks past the limit, so a whole-process freeze
        // (sleep/resume, a paused VM, a debugger) - which stops the reader as
        // well as the callbacks - does not look like a silent service.
        std::atomic<std::int64_t> emptySinceNs{0};
        std::atomic<std::int64_t> lastEmptyReadNs{0};
        std::atomic<std::int64_t> stallLimitNs{
            std::chrono::duration_cast<std::chrono::nanoseconds>(kStreamStallLimit).count()};

        // What the EVENT callback would have logged, left for read() to log
        // (0.99.32, the same rule): one bit per kind of line, see kEventLog*.
        std::atomic<unsigned int> pendingEventLogs{0};

        // THE OVERLOAD ACKNOWLEDGEMENT, THE SAME RULE ONE LEVEL FURTHER
        // (the repair-round review, 2026-09-28): the event callback runs on
        // the SERVICE'S OWN THREAD, and it used to call sdrplay_api_Update
        // directly from there to acknowledge an overload - a second entry
        // into the same vendor call every OTHER control on this object goes
        // through updateLocked's single bounded worker for, from a thread we
        // do not own and cannot bound.
        //
        // THIS IS A DEFENSIVE CHOICE, NOT A SPEC VIOLATION BEING FIXED - the
        // vendor's SDRplay API Specification v3.15 says nothing about
        // calling sdrplay_api_Update from inside an event callback, and the
        // vendor's OWN example (sdrplay_api_example.c) does exactly that:
        // its EventCallback's PowerOverloadChange case calls
        // sdrplay_api_Update(..., Update_Ctrl_OverloadMsgAck, ...) directly,
        // concurrently with whatever Tuner_Gr updates the rest of the
        // example issues elsewhere. So the vendor's own reference code takes
        // the same risk this file used to. What changed here is not "the
        // spec requires serialisation" - it does not say either way - but
        // that THIS codebase's whole SDRplay driver is already built on the
        // rule that nothing of ours enters that DLL except through one
        // bounded, mutex-serialised path, and the event callback was the one
        // exception to a rule everything else here follows. Bringing it into
        // line is defensive hardening against an unproven risk (this file's
        // own bounded-wait philosophy applied consistently), not a fix for a
        // confirmed defect - there is no evidence, from the field reports or
        // otherwise, that this exact race caused anything.
        //
        // So the callback only LATCHES which tuner needs acknowledging;
        // drainPendingOverloadAcks() (called from read(), the same thread
        // drainEventLogs already runs on, never the callback's) issues the
        // actual Update through ackOverloadLocked(), which TRIES devMutex_
        // (never waits for it - read() must not queue behind a GUI-thread
        // control in flight; a busy mutex leaves these flags set for the
        // next read() to retry) and is bounded exactly like every other
        // control once it has the lock.
        //
        // CLEARED IN startStreamingLocked() (round 3, measured): a stop()
        // between an overload firing and the next read() left the flag set,
        // and the NEXT session's first read() sent an acknowledgement for an
        // overload that never happened this time.
        std::atomic<bool> pendingOverloadAckA{false};
        std::atomic<bool> pendingOverloadAckB{false};

        // THE RATE THE RADIO WAS SET FOR, as sampleRateHz() reports it,
        // copied here by startStreamingLocked and by an accepted rate change
        // (0.99.44, GitHub issue 5). The health window compares what the
        // service actually delivered against it and says so when the two are
        // far apart: the 0.99.27 and 0.99.43 RSP2 logs each show 6.0 MS/s
        // arriving at a radio FoxSDR called 2 MS/s, and neither said it in
        // words. 0 means "not streaming".
        std::atomic<double> setRateHz{0.0};
        // Raised by a live rate change: the window it lands in holds samples
        // at two rates, so that one window is not judged.
        std::atomic<bool> rateChangedInWindow{false};

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
    // `serviceAdvice`: see sessionAcquire in the .cpp - the SDRplay API
    // Service sentence when Open itself failed, empty otherwise.
    bool acquireSessionLocked(std::string& error, std::string* serviceAdvice = nullptr);
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
    // --- the late answer (kControlGrace, 0.99.50) -------------------------
    //
    // One control at most can be waiting: while it is, controlAbandoned_ is
    // true, so every other vendor call is refused before it is made and no
    // second worker can join the first. Defined in the .cpp.
    struct LateControl;
    std::shared_ptr<LateControl> lateControl_;  // devMutex_
    // Mirrors lateControl_ != nullptr for the two readers that must not take
    // devMutex_ to ask: read()'s drain and checkForStallFromRead.
    std::atomic<bool> lateControlPending_{false};
    std::atomic<long long> controlGraceMs_{kControlGrace.count()};
    // Set by reapLateControlLocked just before a give-up for an expired grace,
    // so the log says "did not answer within N ms" only when that is true
    // (a stop or close inside the grace says so instead). devMutex_.
    bool lateControlGraceRanOut_ = false;

    // Called with the future of a worker that did not answer within
    // kControlWait: records it and leaves it to answer. `table` is the one it
    // was called through; `undo` is set later by undoRefusedLocked.
    void beginLateControlLocked(std::shared_ptr<LateControl> late);
    // THE ONE PLACE A LATE ANSWER IS LOOKED AT, at the head of every entry
    // that takes devMutex_ (and from read() when the lock is free): acts on an
    // answer that has arrived, gives the radio up when the grace has run out,
    // and otherwise does nothing.
    void reapLateControlLocked();
    // Gives up on a control that has not answered, for good - the grace ran
    // out, or the radio is being stopped or closed with the worker still in.
    void giveUpLateControlLocked(const char* why);
    // Forgets the late control and returns the process table's count.
    void endLateControlLocked();
    // After a late Success: the readback mirrors follow what the block now
    // holds for the reasons that control carried, because the setter that
    // sent it returned false before it could store them.
    void syncMirrorsAfterLateSuccessLocked(sdrplay_abi::ReasonForUpdateT reason,
                                           sdrplay_abi::ReasonForUpdateExt1T ext1);
    // A setter's refused Update: put the block back - unless a worker of
    // ours may still be reading it, in which case the undo is kept for the
    // late answer to apply (a refusal) or discard (a success), or dropped
    // after a give-up.
    void undoRefusedLocked(std::function<void()> undo);
    // read(): the late answer, TRIED under devMutex_ and never waited for.
    void drainLateControl();

    // The overload acknowledgement, bounded exactly like updateLocked (a
    // worker, kControlWait, abandon on timeout) but for an EXPLICIT tuner
    // rather than device_.tuner - an RSPduo's overload can be on either
    // tuner independently of which one Init selected (the 0.99.34 fix this
    // preserves). No acknowledgement flag to wait for afterwards: unlike a
    // retune or a rate change, OverloadMsgAck sets none of grChanged/
    // rfChanged/fsChanged.
    void ackOverloadLocked(sdrplay_abi::TunerSelectT tuner);
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
    // that updateLocked, stopStreamingLocked, closeDevice and every setter
    // cannot drift apart about it. devMutex_ held, like every other *Locked
    // helper.
    //
    // DELIBERATELY NOT deviceDead(). That is raised by an UNPLUGGED radio too
    // (eventCallback's DeviceRemoved), and an unplugged radio leaves a healthy
    // service that answers Uninit and ReleaseDevice in microseconds - calls
    // which are exactly what lets the next RSP be opened. Skipping them there
    // would trade a hang nobody has reported for a receiver that cannot be
    // re-plugged without restarting the application. Only the two conditions
    // below mean the SERVICE is gone.
    bool vendorUnreachableLocked() const {
        return controlAbandoned_ || serviceGone_ || streamStalled_.load(std::memory_order_acquire);
    }

    // ...AND THE TEARDOWN'S QUESTION, which is wider: this device's own flags
    // OR THE PROCESS'S SESSION, lost by any radio (0.99.44, GitHub issue 5).
    // stop(), closeDevice() and the overload acknowledgement ask this one.
    // Until 0.99.44 they asked vendorUnreachableLocked() alone, so a second
    // radio still streaming when the first lost the session went into
    // Uninit, ReleaseDevice and Close on the dead service - each of which the
    // 0.99.43 report shows taking five seconds on the GUI thread to answer
    // ServiceNotResponding. start(), open(), the scan and every setter
    // already asked the session. devMutex_ held.
    bool teardownMustSkipVendorLocked() const;

    // THE FIRST LINE OF EVERY SETTER (the review of 6e308c3). True - with
    // lastError "<what> refused: <the sentence for why>" - when the vendor DLL
    // is unreachable, and then the setter returns false having touched
    // NOTHING: not the parameter block, not a readback mirror, not the API.
    // It has to come before the setter's "nothing changed" shortcut as well
    // as before its writes: after an abandoned control the block still holds
    // the abandoned request, so the shortcut answered "already there" for a
    // frequency the radio never reached, and a different frequency was
    // written into a block a worker of ours may still be reading.
    bool refuseIfVendorUnreachableLocked(const char* what);

    // The source thread's half of kStreamStallLimit: called by read() when it
    // came back empty. Raises the fault once and never takes devMutex_ - read()
    // must not queue behind a GUI-thread control in flight.
    void checkForStallFromRead();

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
    // link.healthMutex held. `rateWarning`, when given, receives the line
    // that says the window's samples arrived at a rate far from the one the
    // radio was set for (empty when they did not) - see Link::setRateHz.
    static std::string healthLineLocked(Link& link, std::string* rateWarning = nullptr);
    // Service thread: atomics only. Never a lock, never the log.
    static void noteBlock(Link& link, std::size_t samples, bool dropped);
    // The pipeline's source thread (read()): closes a window that has run
    // out and writes its line, and logs what the event callback left.
    static void maybeWriteHealth(Link& link);
    static void drainEventLogs(Link& link);
    // The pipeline's source thread (read()): issues any overload
    // acknowledgement the event callback latched (Link::pendingOverloadAckA/
    // B), through ackOverloadLocked - the one bounded, serialised path every
    // other control uses. TRIES devMutex_ and never waits for it (round 3 of
    // the same review - measured a blocking lock here costing read() 2453 ms
    // behind one slow GUI-thread control): read() must never queue behind a
    // control in flight, the same rule checkForStallFromRead already follows.
    // A busy mutex leaves the flags set for the next read() to retry.
    void drainPendingOverloadAcks();

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

    // TRUE ONCE THE STREAM HAS GONE kStreamStallLimit WITHOUT A CALLBACK
    // (0.99.36). The third half of vendorUnreachableLocked(), and ATOMIC where
    // the other two are plain bools: it is raised by read() on the pipeline's
    // source thread, which does not hold devMutex_ and must not wait for it.
    // Cleared by open(), like the other two.
    std::atomic<bool> streamStalled_{false};

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
