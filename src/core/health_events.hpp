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
};
inline constexpr std::size_t kRecoveredCount = 13;

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

// THE POLL FOR WHAT ONLY A COUNTER CAN SAY. Four recoveries happen inside a
// thread this code must not count from (the pipeline's source and DSP threads,
// a vendor call's worker); each leaves a monotonic counter the control path can
// read. A poll gives this the readings; the FIRST poll only takes the baseline
// (a process that has already abandoned a call before the poll started is not
// news), and every later poll counts what moved.
struct RecoveryReadings {
    unsigned long long sourceThreadsAbandoned = 0;  // Pipeline::abandonedSourceThreads()
    unsigned long long vendorCallsAbandoned = 0;    // SoapySource::driverCallsAbandoned()
    unsigned long long ringDroppedSamples = 0;      // Pipeline::ringDroppedSamples()
    unsigned long long dspThreadExceptions = 0;     // Pipeline::dspThreadExceptions()
};
class RecoveryWatch {
public:
    void poll(const RecoveryReadings& now);

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
