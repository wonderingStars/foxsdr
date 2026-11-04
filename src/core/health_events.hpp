// health_events.hpp - the failures that are NOT crashes, counted anonymously
// (0.99.64).
//
// WHY THIS EXISTS. Five releases once shipped in which no radio of any kind
// could be detected. Of the 49 people who took an affected build, 46 never came
// back, and nothing told us, because the application did not crash: it simply
// did not work. The usage record already says which radio a session used, how
// long it ran and whether it ended uncleanly - but not whether OPENING a radio
// failed, whether any sound came out, or whether an update or a plugin install
// failed. This is those counts, and only counts.
//
// WHAT IS COUNTED IS A FIXED VOCABULARY, never text. An event is a small
// enumerated id, optionally qualified by words from another fixed list:
//
//     reached.<milestone>         a session reached a stage of getting to work (below)
//     scan_none                   a device scan found no radio at all
//     radio_open.<driver>         a radio was opened
//     radio_data.<driver>         an opened radio's samples reached the display
//     radio_fail.<driver>.<why>   a radio would not open
//     sound_ok.<api>              the speakers were playing (once per session)
//     sound_fail.<api>.<why>      the sound output could not be opened
//     upd_check / upd_dl / upd_verify / upd_run
//                                 update: check, download, verification,
//                                 starting the installer
//     plug_cat                    the plugin catalogue could not be fetched
//     plug_inst.<class>           a plugin install failed (net, hash, write...)
//     plug_load.<class>           a plugin was refused at load
//     rec_fail                    a recording could not be started
//     setup.<kind>.<family>       what the first-run radio check found on this
//                                 computer (once per session per finding)
//     slow.<scope>.<tier>         a frame of the window took 250 ms or more, in
//                                 the part of the frame that took most of it
//     recovered.<what>            the program met a fault and carried on
//
// The driver word is the kind the diagnostics already use for a radio
// (rtlsdr, hackrf, airspy, sdrplay, soapy, pluto...); the reason is a CLASS
// derived from what the driver said (busy, driver, bind, absent, timeout,
// vendor, rate, other). Never a device name, a serial, a path, a frequency or
// any error message text: the message is read, reduced to one of eight words
// here, and dropped. A word that is not in the tables below cannot be made into
// a token at all - the builders take enums, and the parser refuses the rest.
//
// WHERE THEY LIVE. In the record the usage report sends (TelemetryReport::
// health, one string of `token=count` pairs) and, between a failure and the next
// start-up, in a small file beside config.json (HealthLedger, `telemetry-health`)
// - the same arrangement as the display-stall count (core/telemetry.hpp), and
// for the same reason: a session that crashes or is ended from the taskbar has
// still to say what failed before it died, and the record describing it is only
// sent at the NEXT start.
//
// WHAT COUNTS WHEN. Counted only while Usage reporting is on AND Diagnostics is
// on (the rule the stall count follows). Counted where the decision is made -
// the control paths that open a radio, an output, a download - never on the DSP
// or audio threads. Events that arrive before the application has read its
// config (the saved radio is opened first of all) are held in memory and decided
// when it has; if reporting is off they are dropped and nothing was ever kept.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_HEALTH_EVENTS_HPP
#define CASCADE_CORE_HEALTH_EVENTS_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cascade::core::health {

// ---------------------------------------------------------------------------
// The vocabulary
// ---------------------------------------------------------------------------

enum class RadioReason : std::uint8_t { Busy, Driver, Bind, Absent, Timeout, Vendor, Rate, Other };
enum class SoundReason : std::uint8_t { NoDevice, Busy, Format, Host, Other };
enum class InstallClass : std::uint8_t { Net, Hash, Write, Other };
enum class LoadClass : std::uint8_t { Abi, Retired, Load };
// WHAT THE RADIO-SETUP CHECK FOUND, per radio found on the USB bus (core/radio_setup.hpp
// Kind, same words): the answer to "why does this computer have no working radio",
// which a radio_fail count cannot give because the radio was never opened.
enum class SetupKind : std::uint8_t {
    Usable, WrongDriver, VendorMissing, NotVisible, NoDevice, NotChecked
};
enum class AudioApi : std::uint8_t {
    Mme, DirectSound, Wasapi, Wdmks, Asio, Alsa, Jack, Oss, CoreAudio, None, Other
};

// WHAT THE PROGRAM RECOVERED FROM (0.99.65): a fault it met and carried on from
// without telling the person. One fixed word each; a "kind" of one of these is
// another word, never a qualifier made from data. The order is the written order
// of the words in a record. Each has ONE site that counts it (the survey in
// docs/DIAGNOSTICS.md names them), held to the list by tests/test_health_paths.cpp.
enum class Recovered : std::uint8_t {
    Audio,       // the output stream died and the watchdog restarted it
    Reopen,      // a SoapySDR radio's driver faulted and the radio was reopened
    SrcThread,   // a source thread was abandoned to a driver call that never returned
    VendorCall,  // a SoapySDR vendor call was abandoned because it never returned
    RingDrop,    // the source ring was full and samples were dropped
    DspExc,      // the signal-processing thread threw, was caught, and the receiver stopped
    CfgSave,     // the settings file could not be written
    EnumChild,   // a SoapySDR enumeration child died and was contained
    PluginApi,   // a plugin's call into the host API threw, and was answered with a default
    WebRoute,    // a web handler threw and the server answered 500
    PatchLoad,   // a saved patch loaded with connections dropped
    SdrEnum,     // the SDRplay service's enumeration was abandoned
    SdrLost,     // the SDRplay API session was lost and shut for the session
    AudioLead,   // the audio buffer was deepened because this computer fell behind (0.99.73)
    Viewport,    // the driver refused a second OpenGL context mid-session; every page is kept
                 // inside the main window, and tear-off rests before it is offered
                 // again (0.99.100; rested rather than ended since 0.99.102)
    // THE SURVEY OF 2026-10-28 (0.99.100): the handled faults that were logged and never
    // counted, each now a word and therefore an issue report (core/issue_report.hpp).
    ReaderLeak,    // a native radio's reader (or Pluto's writer) thread did not stop and was let go
    CloseLeak,     // a sound device's close did not return in time and was left to finish alone
    LinkStranded,  // the SDRplay callback was still running after Uninit; its link was stranded
    StreamRestart, // an Airspy's halted bulk stream was re-armed and streaming continued
    TunerRetry,    // the RTL-SDR tuner refused an I2C transfer and accepted it on a retry
    LateCall,      // a SoapySDR or SDRplay control call answered seconds late; changes were held
    ScanLimit,     // the device search ran out of time: drivers skipped, or walked in-process
    UsbStuck,      // a USB device's sysfs node did not answer and it was left out of the scan
    RatesDefault,  // a radio's firmware did not list its rates; a built-in default list was used
    GainRefused,   // the radio refused the remembered automatic gain; manual gain stays
    PpmRefused,    // the radio refused its frequency correction; none is applied
    IqFileGone,    // the saved recording did not reopen at start-up; the generator runs instead
    CatalogueBad,  // a kept plugin catalogue was unreadable and was ignored until the next fetch
    CatStop,       // the CAT server could not drain its connections when stopped and gave up
    FileKept,      // a damaged settings, bookmarks or markers file could not be set aside
    TakeHeader,    // a recording's file could not be finalised and keeps its opening header
    SentinelGone,  // the watcher process did not start, or ended while the application ran
    DisplayWatch,  // the display on/off notification could not be registered
    // THE EARLY-WARNING PAIR (master's field-early-warning branch, folded here from there): after master's thirty-three, before the engine branch's five, so
    // master's words keep their places and the next merge from master stays a merge.
    DevLatch,      // an SDRplay stream failure (the service's DeviceFailure) latched the source as faulted
    SoftGl,        // the window is drawn by the software OpenGL renderer, not the graphics driver's
    // THE ENGINE-EXTRACTION BRANCH'S FIVE (docs/engine-stage4.md 6.27, 6.28), after master's thirty-three: the order is the vocabulary's
    // order in the Worker, PRIVACY.md and the tests, and master's words keep their places so the next merge from master stays a merge.
    PluginFault,       // a plugin's call faulted and the host absorbed it (core/plugin_fault.hpp, plugin containment P1)
    PluginQuarantine,  // the host switched a plugin off for the session after it faulted, threw or never returned
    PluginWedge,       // a plugin call never returned and the host let it go, or an unload timed out inside a plugin's call
    PluginOveruse,     // a plugin call overran its budget
    ThreadWedge, // a worker thread missed its heartbeat budget and was reported by name (core/thread_beats.hpp)
};
inline constexpr std::size_t kRecoveredCount = 40;

// HOW FAR A SESSION GOT (field-early-warning): the stages between "the program started" and
// "the radio is doing its job", one fixed word each. They are the working-session metric and
// the startup funnel the website draws (a build in which no radio can be heard crashes
// nowhere and used to show up in no number at all): launched -> window -> enumerated ->
// streaming -> audio, with `webspectrum` for a session that served its spectrum to a browser
// instead of (or beside) the speakers, and `working` when a radio was open, 60 seconds of its
// samples flowed AND the output was reached (audio or the web spectrum). Counted once a
// session each, in the ordinary ledger, so a record's `reached.launched` is how many sessions
// it describes and every other word's count is how many of them got that far - which is why
// `launched` is counted at all: a record from a build that never measured the stages has NO
// `reached` token, and "not measured" must not read as "got nowhere".
//   launched    the program read its settings and began (the record exists)
//   window      the window drew its first frame
//   enumerated  at least one radio was listed (a scan or the native list found one)
//   streaming   a radio was opened (radio_open, any driver)
//   audio       the speakers played (sound_ok)
//   webspectrum a browser was served a spectrum frame
//   working     streaming + 60 s of samples + (audio or webspectrum)
// Stage names only: no device, no driver, no timing beyond the single 60 s rule.
enum class Milestone : std::uint8_t {
    Launched,
    Window,
    Enumerated,
    Streaming,
    Audio,
    WebSpectrum,
    Working,
};
inline constexpr std::size_t kMilestoneCount = 7;
// Samples seconds a session must have flowing before it is "working".
inline constexpr unsigned kWorkingSampleSeconds = 60;

// One event of the vocabulary, as documentation and the Worker see it: its
// name, the word lists of its qualifiers in order (empty for a bare event), and
// whether it counts at most once per session.
struct VocabularyEntry {
    std::string name;
    std::vector<std::vector<std::string>> qualifiers;
    bool oncePerSession = false;
};

// Every event, in the order tokens are written. The Worker's copy
// (telemetry-worker/worker.js, HEALTH_VOCABULARY) and PRIVACY.md are held to
// this list by tests/test_health_events.cpp, in both directions.
const std::vector<VocabularyEntry>& vocabulary();

// Every legal token, expanded ("radio_fail.rtlsdr.busy"), in written order.
std::vector<std::string> allTokens();

// True when `token` is exactly one legal token (no count): a known event name
// followed by one word from each of its qualifier lists, and nothing else.
bool validToken(std::string_view token);

// ---------------------------------------------------------------------------
// Building tokens - only from enums and reduced words, never from free text
// ---------------------------------------------------------------------------

// A radio's driver kind reduced to the vocabulary: "rtlsdr" stays "rtlsdr",
// anything this build does not know (or a name somebody typed) is "other".
std::string driverWord(std::string_view driverKind);

// A host API name as PortAudio reports it ("Windows WASAPI", "MME", "ALSA"...)
// reduced to the vocabulary; "other" for a name not recognised.
AudioApi audioApiFromName(std::string_view hostApiName);
const char* audioApiWord(AudioApi api);

// WHAT A DRIVER SAID, reduced to one of eight classes. The text is matched for
// the wording the drivers and the transports use (the shared sentences in
// usb/usb_device.hpp, the SDRplay service's, SoapySDR's and PortAudio's own);
// anything unrecognised is `Other` - which keeps the denominator right and is
// itself a signal that a driver said something new. `driverKind` decides only
// the fallback: an error out of the SoapySDR vendor stack that matches nothing
// else is the vendor's, not ours.
RadioReason classifyRadioOpen(std::string_view driverKind, std::string_view error);
const char* radioReasonWord(RadioReason r);
const char* soundReasonWord(SoundReason r);

// THE REASON CLASS AS A WORD, for the two log lines that say a radio did not
// open ("busy", "absent", "bind", "driver", "timeout", "vendor", "rate",
// "other"): radioReasonWord(classifyRadioOpen(kind, error)). The word is from
// the closed vocabulary above and says nothing of what the driver said - which
// quotes the device arguments, and so the serial number, back - so it is safe
// in the diagnostics ring, where the error text itself is not.
std::string radioFailWhy(std::string_view driverKind, std::string_view error);

std::string tokenScanNone();
std::string tokenRadioOpen(std::string_view driverKind);
std::string tokenRadioData(std::string_view driverKind);
std::string tokenRadioFail(std::string_view driverKind, RadioReason why);
std::string tokenSoundOk(AudioApi api);
std::string tokenSoundFail(AudioApi api, SoundReason why);
std::string tokenUpdateCheck();
std::string tokenUpdateDownload();
std::string tokenUpdateVerify();
std::string tokenUpdateRun();
std::string tokenPluginCatalogue();
std::string tokenPluginInstall(InstallClass c);
std::string tokenPluginLoad(LoadClass c);
std::string tokenRecordFail();
// `family` is a radio family as core/radio_setup.hpp spells it ("rtlsdr", "airspy",
// "airspyhf", "hackrf", "sdrplay", "usrp", "lime"); "" and anything else is "none".
std::string tokenSetup(SetupKind kind, std::string_view family);

// SLOW FRAMES (0.99.65). The scope is a frame scope's own name as
// core/frame_timing.hpp spells it (hyphens included: "plugin-panels",
// "plugins-reload") - every one of them but `user-wait`, which is time a person
// set the pace of and is never counted, so it is not a word. The tier is the
// fixed width of the slowness: "250ms" (250 ms to under 1 s), "1s" (1 s to under
// 5 s), "5s" (5 s or more). A frame is one count, in the highest tier it
// reached. Tier words are `250ms`, `1s` and `5s` and nothing else: they start
// with a digit, which the grammar (lower-case letters, digits, '_', '-') carries.
const std::vector<std::string>& slowScopeWords();
const std::vector<std::string>& slowTierWords();
// The token for the frame scope with this index in core::kFrameScopeNames and
// this tier (0..2); empty for `user-wait` and for anything out of range.
std::string tokenSlowFrame(int frameScope, int tier);

// WHAT THE PROGRAM RECOVERED FROM. Words of Recovered, in written order.
const std::vector<std::string>& recoveredWords();
const char* recoveredWord(Recovered r);
std::string tokenRecovered(Recovered r);
// True when this word is counted at most once a session (it can repeat by itself:
// a watchdog's retry, a once-a-minute reopen, a ring that overflows again and
// again). The others are counted once per occurrence.
bool recoveredOncePerSession(Recovered r);

// THE MILESTONES. Words of Milestone, in written order.
const std::vector<std::string>& milestoneWords();
const char* milestoneWord(Milestone m);
std::string tokenReached(Milestone m);

// ---------------------------------------------------------------------------
// Counts, and their one-string encoding
// ---------------------------------------------------------------------------

// token -> how many times. Only legal tokens, every count 1..kMaxCount.
using Counts = std::map<std::string, std::uint32_t>;

constexpr std::uint32_t kMaxCount = 999;
// THE BOUNDS OF A RECORD, one per family of tokens. The failure families (every
// event but the last two) carry at most kMaxBaseTokens distinct tokens, of which
// at most kMaxRadioFailTokens are radio failures (driver x reason pairs, the one
// family that can be wide). `slow` carries at most kMaxSlowTokens distinct
// tokens and `recovered` at most kMaxRecoveredTokens, so a record has at most
// kMaxRecordTokens. When there are MORE than the cap in a family, the ones kept
// are the worst - see selectForRecord().
constexpr std::size_t kMaxBaseTokens = 24;
constexpr std::size_t kMaxRadioFailTokens = 8;
constexpr std::size_t kMaxSlowTokens = 8;
constexpr std::size_t kMaxRecoveredTokens = 8;
constexpr std::size_t kMaxRecordTokens = kMaxBaseTokens + kMaxSlowTokens + kMaxRecoveredTokens;
// The text of a record is never longer than this, so an older Worker - which
// refuses a longer text outright - still reads every record this build writes.
// The failure tokens come first in the written order and `slow` and `recovered`
// last, so a record that cannot hold everything loses the newest families first;
// what is left out stays in the ledger and goes with the next record.
constexpr std::size_t kMaxEncodedBytes = 832;

// WHAT A RECORD CARRIES OF `counts`: every legal token with its count clamped to
// 1..kMaxCount, the families cut to their caps, and the whole kept within
// kMaxEncodedBytes. THE RULES, in the order they apply:
//   - `slow`, more than kMaxSlowTokens distinct: the worst are kept - the higher
//     tier first (5s, then 1s, then 250ms), then the higher count, then the earlier
//     in the written order (the scope's place in the frame, then the tier).
//   - `recovered`, more than kMaxRecoveredTokens distinct: the higher count first,
//     then the earlier in the written order.
//   - the failure families: kMaxBaseTokens, kMaxRadioFailTokens of them radio
//     failures, the earlier in the written order first (a ledger never holds more).
//   - the text: tokens are added in the written order until the next would not fit
//     in kMaxEncodedBytes.
// The start-up record's counts are settled against exactly this selection, so
// whatever it leaves out stays kept for the next record.
Counts selectForRecord(const Counts& counts);

// "token=count,token=count" in the written order of the vocabulary: the text of
// selectForRecord(counts); empty for no counts.
std::string encode(const Counts& counts);

// The strict inverse: false (and `out` empty) for text that is not exactly what
// encode() writes - an unknown token, a count outside 1..kMaxCount, a repeated
// token, a stray byte, a family over its cap, or more than kMaxEncodedBytes. The
// empty string is zero counts.
bool decode(std::string_view text, Counts& out);

// Keeps only what is legal in `text` (whatever its order or length), merging
// repeats and clamping counts; what encode() would write for that.
std::string sanitise(std::string_view text);

// The sum of the counts of every token whose event name is `eventName`
// ("radio_fail" adds up every driver and reason).
std::uint64_t sumOf(const Counts& counts, std::string_view eventName);

// ---------------------------------------------------------------------------
// The ledger
// ---------------------------------------------------------------------------

// A COUNTER THAT SURVIVES A CRASH, and is taken off only when the server has
// accepted a record that carried it - core::StallLedger's arrangement, for a
// set of counts instead of one number.
//
// IT BELONGS TO TWO SESSIONS AT ONCE, and keeping them apart is the point of the
// two sets below. What the ledger's file held when this run started describes
// EARLIER sessions (`prior`): that is what the start-up record - which describes
// the previous session - carries. What this run counts after that (and what it
// counted before it knew whether reporting was on, `held`) describes THIS
// session, and goes out with the NEXT start-up's record: attaching a failure of
// today's build to yesterday's record would put it under the wrong version,
// which is the one thing this feature must not do. Both live in the one file, so
// a session ended mid-failure loses neither.
//
// THE FILE (`telemetry-health`, beside config.json): the install id it belongs
// to on the first line - a file left by an earlier identity is ignored - then one
// `token=count` per line, and nothing else. Written by a short-lived thread of
// its own (never the caller's: the GUI thread must not wait on a disk, as with
// the config writer), through a temporary file and a rename, so a process ended
// mid-write leaves the old file or the new one. Removed when nothing is left,
// and when reporting is switched off.
//
// ONCE PER SESSION for the events that can repeat on their own (a watchdog
// retrying the sound output every second, a rescan meeting the same refused
// plugin): the second occurrence in one run is not counted, so a count of 1
// means "this session had it".
//
// Thread safety: note() is callable from any thread; one mutex, never held
// across I/O.
class HealthLedger {
public:
    static constexpr char kFileName[] = "telemetry-health";

    HealthLedger();
    ~HealthLedger();
    HealthLedger(const HealthLedger&) = delete;
    HealthLedger& operator=(const HealthLedger&) = delete;

    // Reporting is on with identity `installId`: counts are kept from now, in the
    // file `path` (empty keeps them in memory only). `loadExisting` reads the
    // previous sessions' counts back - start-up only. What was held in memory
    // before this call is this session's and is kept (unless Diagnostics is
    // off, see setAllowed). An id that is not a real install id disarms
    // instead: with no identity there is nobody to count for.
    void arm(const std::string& path, const std::string& installId, bool loadExisting);

    // Reporting is off: forget everything, keep nothing, remove the file (on a
    // thread of its own), and count nothing from now on.
    void disarm();

    // The Diagnostics switch (see the rule in the file header). Off: nothing new
    // is counted; what is already kept stays and is still sent.
    void setAllowed(bool allowed);
    // The switch as last set (true until told otherwise).
    bool allowed() const;

    // True once reporting has been decided either way (arm or disarm).
    bool decided() const;
    bool armed() const;

    // One event. A token that is not in the vocabulary is ignored. Before the
    // decision: held in memory, bounded. After it: counted only when armed and
    // allowed.
    void note(const std::string& token);
    // `times` occurrences at once (a poll that finds a counter has moved by
    // that much). A token counted once a session is counted once however many.
    void note(const std::string& token, std::uint32_t times);

    // A hook the WRITER THREAD calls just before it writes the file, with no lock
    // of the ledger held: for the tests, to make a slow disk. Null (the default)
    // is no hook. Process-wide.
    static void setWriteHookForTest(void (*hook)());

    // Everything kept (earlier sessions and this one), and the part that
    // describes EARLIER sessions only (what the start-up record carries).
    Counts counts() const;
    Counts priorCounts() const;
    // encode(counts()), or empty when not armed.
    std::string encoded() const;

    // A record that carried `carried` was accepted: forget exactly that.
    void settle(const Counts& carried);

    // Waits (bounded) for the writer thread to be idle - for the tests, which
    // read the file. True when idle.
    bool flush(unsigned timeoutMs = 5000) const;

    // Back to "nothing decided, nothing kept" - the state of a process that has
    // not read its config yet. For the tests, which share one process.
    void reset();

    // The file's text for `counts` under `installId`, and its inverse (false
    // unless the first line is exactly `installId`; lines that are not a legal
    // `token=count` are skipped, bounds as for decode). Exposed for the tests.
    static std::string fileText(const std::string& installId, const Counts& counts);
    static bool parseFileText(const std::string& text, const std::string& installId, Counts& out);
    static std::string pathIn(const std::string& configDir);
    static void removeFile(const std::string& path);

    // The state the writer thread shares with the ledger (defined in the .cpp;
    // public only so that thread's function can name it).
    struct Shared;

private:
    std::shared_ptr<Shared> s_;
};

// THE LEDGER THE PRODUCT COUNTS INTO: one per process, created on first use and
// never destroyed (a writer thread may still be finishing when the process ends,
// and nothing here is worth a static-destruction race). Every call site goes
// through the note functions below, which are no-ops until it has been armed.
std::shared_ptr<HealthLedger> globalLedger();

// ---------------------------------------------------------------------------
// Call-site helpers - each reduces what it is given to the vocabulary
// ---------------------------------------------------------------------------

void note(const std::string& token);
void noteScanNone();
void noteRadioOpen(std::string_view driverKind);
void noteRadioData(std::string_view driverKind);
// `error` is what the driver said; it is classified and dropped.
void noteRadioFail(std::string_view driverKind, std::string_view error);
void noteSoundOk(std::string_view hostApiName);
void noteSoundFail(AudioApi api, SoundReason why);
void noteUpdateCheckFailed();
void noteUpdateDownloadFailed();
void noteUpdateVerifyFailed();
void noteUpdateRunFailed();
void notePluginCatalogueFailed();
void notePluginInstallFailed(InstallClass c);
void notePluginLoadRefused(LoadClass c);
void noteRecordFailed();
// One finding of the radio-setup check. `kindWord` is core::radiosetup::kindName()
// ("usable", "wrong-driver", "vendor-missing", "not-visible", "no-device",
// "not-checked"); a word that is none of those counts nothing. `family` is reduced
// as for tokenSetup. Counted once a session for each finding.
void noteSetupFinding(std::string_view kindWord, std::string_view family);

// One slow frame (core::FrameTimer::commit, GUI thread): `frameScope` is the
// scope's index in core::kFrameScopeNames and `tier` 0..2. Nothing for
// `user-wait` or anything out of range.
void noteSlowFrame(int frameScope, int tier);

// WHAT THE PROGRAM RECOVERED FROM. noteRecovered() counts at once: for the
// control path (the window's thread, a scan or web worker) and never for a DSP,
// source or audio thread, because it takes the ledger's lock. raiseRecovered() is
// for the places a signal thread or a plugin's thread meets the fault: ONE
// relaxed atomic OR, no lock, no allocation, nothing else; drainRecovered() - run
// by a poll the window already makes every frame - turns what was raised into
// counts. Both are no-ops until the ledger is armed (the raise is simply drained
// into a ledger that ignores it).
void noteRecovered(Recovered r);
void raiseRecovered(Recovered r);
void drainRecovered();

// A MILESTONE REACHED. Callable from any thread that may take the ledger's lock (the GUI thread,
// a web worker): never from the DSP, source or audio threads. The process remembers which it has
// reached (reachedMask), so a frame loop can ask `reached()` for the price of one atomic load
// before it counts anything, and replayReached() puts them into a ledger armed AFTER they were
// reached (the person switched usage reporting on mid-session, which arms a fresh ledger).
void noteReached(Milestone m);
bool reached(Milestone m);
void replayReached();
// Test seam: forget what the process has reached.
void resetReachedForTest();

// WHEN A SESSION COUNTS AS WORKING. Pure, so the rule is testable without a window: told
// once a second (wall clock) whether a radio is open, whether its samples are flowing, and
// whether the output has been reached, it says - exactly once - when the session has had
// kWorkingSampleSeconds seconds of flowing samples with a radio open and the output reached.
// The seconds are cumulative, not consecutive: a radio that is retuned or briefly stalls
// does not start the minute over. Time with no radio open, or with the samples stopped, adds
// nothing.
class WorkingClock {
public:
    // True on the one call that makes the session working; false before and after.
    bool second(bool radioOpen, bool samplesFlowing, bool outputReached);
    unsigned flowingSeconds() const { return flowing_; }
    bool working() const { return working_; }
    // Test seam: a window test cannot wait a minute for real.
    void seedFlowingSecondsForTest(unsigned seconds) { flowing_ = seconds; }

private:
    unsigned flowing_ = 0;
    bool working_ = false;
};

// THE POLL FOR WHAT ONLY A COUNTER CAN SAY. Five recoveries happen inside a
// thread this code must not count from (the pipeline's source and DSP threads,
// a vendor call's worker, the watchdog's own thread); each leaves a monotonic
// counter the control path can read. A poll gives this the readings; the FIRST poll only takes the baseline
// (a process that has already abandoned a call before the poll started is not
// news), and every later poll counts what moved.
struct RecoveryReadings {
    unsigned long long sourceThreadsAbandoned = 0;  // Pipeline::abandonedSourceThreads()
    unsigned long long vendorCallsAbandoned = 0;    // SoapySource::driverCallsAbandoned()
    unsigned long long ringDroppedSamples = 0;      // Pipeline::ringDroppedSamples()
    unsigned long long dspThreadExceptions = 0;     // Pipeline::dspThreadExceptions()
    unsigned long long threadWedges = 0;            // beats::Registry::wedgesFound(): workers the watchdog reported late (6.27)
    // The plugin containment's four (core/plugin_fault.hpp totals(), 6.28): absorbed faults, plugins switched off, calls that never
    // returned (and unloads that timed out), and calls that overran their budget.
    unsigned long long pluginFaults = 0;
    unsigned long long pluginQuarantines = 0;
    unsigned long long pluginWedges = 0;
    unsigned long long pluginOveruses = 0;
};
class RecoveryWatch {
public:
    void poll(const RecoveryReadings& now);
    // THE BASELINE, TAKEN BY THE OWNER WHEN IT STARTS rather than by whatever poll comes first (deep review of engine/stage4, batch A; docs/engine-stage4.md
    // 6.31). The first poll as the baseline was written for four counters that cannot move before the first frame. The plugin containment's four and the
    // per-thread watchdog's can: the engine's first pass runs its plugin phase (a plugin that faults in create on every launch is absorbed THERE) before the
    // health poll at the end of the same pass, so the first poll's baseline swallowed exactly the fault most worth reporting. An owner that baselines at
    // construction counts everything that moves after; one that never does keeps the old rule (its first poll is the baseline).
    void baseline(const RecoveryReadings& r);

private:
    bool started_ = false;
    RecoveryReadings last_;
};

// ---------------------------------------------------------------------------
// The usage record's `health` member
// ---------------------------------------------------------------------------

// `recordJson` with its `health` member set to `encoded` (already legal; it is
// passed through sanitise() again here). A record that is not a JSON object is
// returned unchanged.
std::string withHealth(const std::string& recordJson, const std::string& encoded);

}  // namespace cascade::core::health

#endif  // CASCADE_CORE_HEALTH_EVENTS_HPP
