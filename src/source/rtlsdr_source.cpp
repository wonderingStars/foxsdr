// See rtlsdr_source.hpp for the shape, the threading contract and the link.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rtlsdr_source.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "core/diag_log.hpp"
#include "core/radio_events.hpp"
#include "core/health_events.hpp"
#include "core/leak_on_purpose.hpp"
#include "source/usb_stream_note.hpp"
#include "core/ppm_correction.hpp"

namespace cascade::source {
namespace rre = ::cascade::core::radioreport;

namespace {

// ---------------------------------------------------------------------------
// EVERY BOUNDED WAIT THIS DRIVER PERFORMS, in one place and named, because
// tests/test_shutdown_budget.cpp discovers them and requires a statement of
// what each costs a shutdown. Read that file before changing any of them.
// ---------------------------------------------------------------------------

// Waiting for the link's mutex. The longest anything can hold it is one bulk
// read (kBulkReadTimeout) plus one control transfer (kControlTimeout), so
// this is comfortably more than a healthy device needs and short enough that
// a GUI control which loses the race says so rather than appearing to freeze.
constexpr std::chrono::milliseconds kDeviceLockWait{750};

// Every vendor control transfer. The RTL2832U answers one in well under a
// millisecond; half a second is "this device has stopped talking".
constexpr std::chrono::milliseconds kControlTimeout{500};

// One bulk read in the reader loop. Also the longest the reader thread can
// hold the device lock, which is why it is short: a retune waits behind it.
constexpr std::chrono::milliseconds kBulkReadTimeout{50};

// stop() waiting for the reader thread to leave the device. The thread checks
// its token every kBulkReadTimeout, so a healthy exit costs a few tens of
// milliseconds; this is the bound past which the thread is ABANDONED rather
// than joined (see stop()).
constexpr std::chrono::milliseconds kReaderJoinWait{1000};

// read() polling the ring before returning the IqSource contract's "0, retry"
// answer. Spent on the pipeline's own source thread, never on the teardown
// thread.
constexpr std::chrono::milliseconds kReadPollBudget{20};

// The reader thread standing aside while a control call waits for the device
// lock (see controlLock). std::timed_mutex promises no fairness, and a reader
// that takes the lock straight back after every bulk read starved retunes:
// measured against the fake, ten in two hundred waited out kDeviceLockWait
// and were refused as "busy" on a radio that was working.
constexpr std::chrono::milliseconds kReaderStandAside{1};

// ---------------------------------------------------------------------------

// The bulk ring. 16 KB is 32 maximum-size packets, which at 2.4 MS/s is a
// buffer every 3.4 ms - small enough that the spectrum is not a slideshow,
// large enough that WinUSB is not completing a request every packet. Sixteen
// of them is about 55 ms of queue, which is what rides out a GUI stall.
constexpr std::size_t kBulkBufferBytes = 16384;
constexpr std::size_t kBulkBufferCount = 16;

// A FAILED OR SILENT STREAM IS RE-ARMED BEFORE THE RADIO IS CALLED DEAD (field faults 2 and 3, 0.99.109, 2026-11-19). Plain integers, not
// std::chrono constants: none of these is a wait on the shutdown path, and tests/test_shutdown_budget.cpp discovers chrono constants.
//
// A bulk read that FAILS is a stalled endpoint, not yet an unplugged dongle: winusb_device.cpp says in so many words that with the pipe halted
// the first read returns Windows error 31 and the SECOND delivers, and the bench saw a tuner write refused the same way under full CPU load. One
// failed read used to fault the radio for good ("it may have been unplugged"), and a real RTL-SDR that the scan listed again two seconds later
// was closed five times that way in one afternoon. So the stream is taken down and brought back up (kMaxStreamRearms times) and only a stream
// that fails again after that - or an error that says the device is gone - is a fault.
constexpr int kMaxStreamRearms = 3;
// A stream that delivers NOTHING for this long while the reader is alive (every bulk read timing out) is re-armed the same way. The chip
// delivers a buffer every few milliseconds, so three seconds of nothing is not a quiet band (Link::silenceRearmAfterMs, 3000).
// The re-arm budget is forgiven once samples have flowed for this long since the last re-arm.
constexpr int kRearmForgiveAfterMs = 60000;
// A reader that has not come round its loop for this long is stuck inside a USB call; read() says so as a fault (a reader parked in the
// kernel cannot report anything itself). Far beyond every bound the loop is built on (a 50 ms read, a 750 ms lock wait). (Link::readerStuckAfterMs, 20000.)

// The sample ring between the reader thread and read(). 2^19 samples is
// ~164 ms at the highest rate the chip supports: long enough to absorb a
// stalled consumer, short enough that latency stays under what a listener
// notices.
constexpr std::size_t kRingCapacity = 1u << 19;

// The rate and frequency a freshly opened dongle lands on, so a user who has
// not touched anything sees a spectrum rather than a flat line.
constexpr double kDefaultRateHz = 2400000.0;
constexpr double kDefaultFreqHz = 100000000.0;
// A middling manual gain. Full automatic on an R820T pumps badly across a
// wide span; zero is deaf. 29.7 dB is about the middle of the ladder.
constexpr int kDefaultGainTenthDb = 297;

// The tuning limits this driver advertises, per tuner. The R82xx's own
// specification is 24 - 1766 MHz; a Blog V4 reaches down to 500 kHz through
// its upconverter.
constexpr double kR82xxLoHz = 24000000.0;
constexpr double kR82xxHiHz = 1766000000.0;
constexpr double kBlogV4LoHz = 500000.0;

// Below this a non-V4 R82xx dongle has nothing to tune with, and the driver
// switches the demodulator to direct sampling instead. Anything it hears
// there depends on a hardware modification, which is why this is BELOW the
// advertised range rather than part of it.
constexpr double kDirectSamplingBelowHz = 24000000.0;

// The unsigned-byte to float conversion, as a table. One lookup per component
// and no arithmetic: at 3.2 MS/s this runs 6.4 million times a second.
//
// The scale is 1/128 rather than 1/127.5, so a full-scale byte maps to 0.9961
// and the result stays inside [-1, 1) as the pipeline expects.
struct ByteToFloat {
    float v[256];
    ByteToFloat() {
        for (int i = 0; i < 256; ++i) {
            v[i] = (static_cast<float>(i) - 127.5f) * (1.0f / 128.0f);
        }
    }
};
const ByteToFloat kByteToFloat;

std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) { --b; }
    return s.substr(a, b - a);
}

// "serial=00000001" / "index=0" / "". Deliberately tiny: the args string is
// our own, produced by enumerateRtlSdr().
void parseArgs(const std::string& args, std::string& serial, int& index) {
    serial.clear();
    index = -1;
    std::size_t at = 0;
    for (;;) {
        const std::size_t end = args.find(',', at);
        const std::string item =
            trim(args.substr(at, end == std::string::npos ? std::string::npos : end - at));
        const std::size_t eq = item.find('=');
        if (eq != std::string::npos) {
            const std::string key = trim(item.substr(0, eq));
            const std::string value = trim(item.substr(eq + 1));
            if (key == "serial") {
                serial = value;
            } else if (key == "index") {
                index = std::atoi(value.c_str());
            }
        }
        if (end == std::string::npos) { break; }
        at = end + 1;
    }
}

// The health tally, and the once-a-minute line. Free functions on the link
// rather than members, because the reader thread reaches them through its
// shared pointer and may outlive the RtlSdrSource.
std::string healthLineLocked(RtlSdrSource::Link& link) {
    if (!link.health.windowOpen || link.health.reads == 0) { return std::string(); }
    const auto now = std::chrono::steady_clock::now();
    // A stall still in progress at the moment of writing is part of the
    // story.
    const auto openGap =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - link.health.lastSamples)
            .count();
    if (openGap > link.health.longestGapMs) { link.health.longestGapMs = openGap; }
    const auto windowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - link.health.windowStart)
            .count();
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "source: stream health - reads %llu, with samples %llu, timeouts %llu, "
                  "overflows %llu, errors %llu, longest gap %lld ms, %llu samples in %lld s",
                  static_cast<unsigned long long>(link.health.reads),
                  static_cast<unsigned long long>(link.health.withSamples),
                  static_cast<unsigned long long>(link.health.timeouts),
                  static_cast<unsigned long long>(link.health.overflows),
                  static_cast<unsigned long long>(link.health.errors),
                  static_cast<long long>(link.health.longestGapMs),
                  static_cast<unsigned long long>(link.health.samples),
                  static_cast<long long>((windowMs + 500) / 1000));
    link.health = RtlSdrSource::Link::Health{};
    return std::string(buf);
}

void noteRead(RtlSdrSource::Link& link, int ret, std::size_t samples) {
    const auto now = std::chrono::steady_clock::now();
    bool worrying = false;
    bool first = false;
    bool nominal = false;
    rre::StreamTrouble trouble;
    std::string line;
    {
        std::lock_guard<std::mutex> lk(link.healthMutex);
        if (!link.health.windowOpen) {
            link.health.windowOpen = true;
            link.health.windowStart = now;
            link.health.lastSamples = now;
        }
        ++link.health.reads;
        if (ret > 0) {
            ++link.health.withSamples;
            link.health.samples += samples;
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - link.health.lastSamples)
                                 .count();
            if (gap > link.health.longestGapMs) { link.health.longestGapMs = gap; }
            link.health.lastSamples = now;
        } else if (ret == 0) {
            ++link.health.timeouts;
        } else {
            ++link.health.errors;
        }
        if (now - link.health.windowStart < link.healthWindow) { return; }
        nominal = link.health.timeouts == 0 && link.health.overflows == 0 &&
                  link.health.errors == 0 && link.health.longestGapMs < 250;
        first = !link.healthEverWritten;
        worrying = link.health.errors > 0 || link.health.longestGapMs >= 1000;
        trouble = {std::chrono::duration<double>(now - link.health.windowStart).count(), link.health.samples, link.health.errors, link.health.overflows,
                   link.health.timeouts, static_cast<long long>(link.health.longestGapMs)};
        line = healthLineLocked(link);
        if (line.empty()) { return; }
        if (worrying || first || !nominal) { link.healthEverWritten = true; }
    }
    // Same policy and the same words as SoapySource: the first window after a
    // start is always written, so a healthy radio leaves a line proving it,
    // and after that only a window with something to say.
    if (worrying) {
        core::diagWarnf("%s", line.c_str());
        rre::noteStreamTrouble(link.owner, "rtlsdr", trouble);
    } else if (first || !nominal) {
        core::diagLogf("%s", line.c_str());
    }
}

// `journal` false is for a stop the user asked for (decision S10): the radio is latched dead with the sentence that says what happened, but the journal's
// `radio_close` is the engine's to write, with the cause of a user's stop, not a radio fault the driver would otherwise report.
void noteFault(RtlSdrSource::Link& link, std::string msg, const char* what, bool journal = true) {
    std::string said;
    {
        std::lock_guard<std::mutex> lk(link.errorMutex);
        said = msg;
        link.lastError = std::move(msg);
        link.faulted = true;
        link.deviceDead = true;
        link.faultedWhile = what;
    }
    if (journal) {
        core::diagWarnf("source: the RTL-SDR failed while %s", what);
        rre::noteDeviceFault(link.owner, "rtlsdr", what, said);
    } else {
        core::diagLogf("source: the RTL-SDR was stopped while %s", what);   // the user's stop is not a failure: an information line, no report
    }
}

// A CONTROL CALL'S TURN AT THE DEVICE: the lock every entry point but the
// reader takes, announced first so the reader stands aside (kReaderStandAside)
// instead of winning every hand-off.
//
// WHILE THE RADIO IS BEING RE-OPENED (Link::reopening) the answer is an UNOWNED lock at once, without waiting: the device is closed, and a caller that
// waited for it would wait for the re-open. Every caller already handles an unowned lock (it means "busy"); the setters tell the two apart by reading
// Link::reopening and keep what they were asked for in Link::wanted.
std::unique_lock<std::timed_mutex> controlLock(RtlSdrSource::Link& link) {
    if (link.reopening.load(std::memory_order_acquire)) { return std::unique_lock<std::timed_mutex>(link.mutex, std::defer_lock); }
    link.controlWaiters.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::timed_mutex> lk(link.mutex, kDeviceLockWait);
    link.controlWaiters.fetch_sub(1, std::memory_order_relaxed);
    if (lk.owns_lock() && link.reopening.load(std::memory_order_acquire)) { lk.unlock(); }   // the re-open began while this call waited
    return lk;
}

std::int64_t steadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// DOES THIS TRANSPORT ERROR SAY THE DEVICE IS GONE? Windows' own words for a device that left the bus (ERROR_DEVICE_NOT_CONNECTED 1167,
// ERROR_NO_SUCH_DEVICE 433) and the transport's ("no longer present"). Anything else - error 31, a timeout, a cancelled transfer - is a
// stalled stream, and is not called "unplugged".
bool transportSaysGone(const std::string& text) {
    static const char* const kGone[] = {"Windows error 1167", "Windows error 433", "no longer present", "gone", "removed", "not connected"};
    for (const char* g : kGone) {
        if (text.find(g) != std::string::npos) { return true; }
    }
    return false;
}

// THE STREAM TAKEN DOWN AND BROUGHT BACK, on the reader's own thread under the device lock (the lock every control call takes; the reader is the
// only caller of readBulk, so nothing is inside the ring endBulkStream frees). The order is startStreamLocked's: the endpoint FIFO reset, then the
// pipe (which resets the pipe itself first). False with `why` when the device would not take it - which is how a dongle that really left says so.
//
// THREE ANSWERS, not two (diagnosis 2026-11-23, defect D1): losing the race for the device lock to a control call is not a failed re-arm and must not
// condemn the radio - it is "try again", and the caller neither spends a re-arm nor faults. Every failure keeps the Windows error of the call that failed
// (defect D2: the control write's error used to be dropped).
enum class Rearm { Back, Failed, LockLost };

Rearm rearmStream(RtlSdrSource::Link& link, std::string& why) {
    std::unique_lock<std::timed_mutex> lk(link.mutex, kDeviceLockWait);
    if (!lk.owns_lock()) {
        why = "the radio was busy with a control call";
        return Rearm::LockLost;
    }
    if (!link.transport || !link.rtl) {
        why = "no radio open";
        return Rearm::Failed;
    }
    link.transport->endBulkStream();
    link.rtl->clearError();
    if (!link.rtl->resetBuffer()) {
        // The TRANSPORT's text, not the chip object's: Rtl2832u keeps only the first error since its last clear, the transport keeps the last one, and
        // the last one is the call that just failed.
        why = "the radio would not reset its endpoint: " + link.transport->lastError();
        return Rearm::Failed;
    }
    if (!link.transport->beginBulkStream(kBulkEndpoint, kBulkBufferBytes, kBulkBufferCount)) {
        why = link.transport->lastError();
        return Rearm::Failed;
    }
    reportStreamNote(*link.transport, link.owner, "rtlsdr");
    return Rearm::Back;
}

// ---------------------------------------------------------------------------
// THE CHIP'S SETTINGS, as device work over the link: the one place each setter's effect on the radio is written, used by the public setters AND by the
// re-open, so a re-opened radio is put back by the very writes a user's own change would have made. All of them need the device lock held by the caller.
// ---------------------------------------------------------------------------

// What the chip-level steps need to know about where the radio is now (the source's mirrors, or the re-open's running copy of them).
struct ChipState {
    double rateHz = 0.0;
    double centerHz = 0.0;
    int ifBandwidthHz = 0;
    int directChoice = 0;
};

struct BringUp {
    bool eepromValid = false;
    bool biasForced = false;
    double rateHz = 0.0;
    std::string error;
    // What the bring-up read from the device, for the identity a re-open compares (rtlsdr_source.hpp, Link::Identity).
    std::string tunerName;
    std::string manufacturer;
    std::string product;
};

int ifBandwidthForRate(int userHz, double rateHz) { return userHz > 0 ? userHz : static_cast<int>(rateHz); }

ChipState makeChipState(double rateHz, double centerHz, int ifBandwidthHz, int directChoice) {
    ChipState st;
    st.rateHz = rateHz;
    st.centerHz = centerHz;
    st.ifBandwidthHz = ifBandwidthHz;
    st.directChoice = directChoice;
    return st;
}

// `teeForced`, when not null, replaces what the EEPROM's forced bias tee would be written as (the re-open passes false: it decides the tee itself).
bool bringUpChip(RtlSdrSource::Link& link, int ifBandwidthHz, int directChoice, BringUp& out, const bool* teeForced = nullptr);
bool retuneChip(RtlSdrSource::Link& link, double hz, int choice);

bool rangeFor(const RtlSdrSource::Link& link, int directChoice, double& loHz, double& hiHz) {
    if (!link.tuner) { return false; }
    loHz = link.tuner->blogV4() ? kBlogV4LoHz : kR82xxLoHz;
    hiHz = kR82xxHiHz;
    if ((directChoice == 2 || directChoice == 3) && !link.tuner->blogV4()) {
        loHz = RtlSdrSource::kDirectLoHz;   // a forced direct-sampling radio hears the HF band the tuner cannot (R11)
        hiHz = RtlSdrSource::kDirectHiHz;
    }
    return true;
}

double nearestSupportedRate(double hz) {
    const std::vector<double> rates = Rtl2832u::supportedRatesHz();
    double best = rates.front();
    for (const double r : rates) {
        if (std::fabs(r - hz) < std::fabs(best - hz)) { best = r; }
    }
    return best;
}

// The rate change proper, on a QUIET stream (the caller stopped it): the resampler, the ring emptied and its generation moved on, the tuner's IF filter
// and the down-converter that follows it, then the retune. `st.rateHz` is updated to what the chip settled on. False when the chip refused the rate.
bool chipRate(RtlSdrSource::Link& link, double bestHz, ChipState& st) {
    double actual = 0.0;
    if (!link.rtl->setSampleRate(static_cast<std::uint32_t>(bestHz + 0.5), actual)) { return false; }
    st.rateHz = actual;
    // Everything already read belongs to the old rate (see Link::rateEpoch).
    {
        std::lock_guard<std::mutex> rlk(link.ringMutex);
        if (link.ring) {
            std::vector<std::complex<float>> scratch(8192);
            while (link.ring->read(scratch.data(), scratch.size()) > 0) {}
        }
        link.rateEpoch.fetch_add(1, std::memory_order_relaxed);
    }
    link.rtl->setI2cRepeater(true);
    link.tuner->setBandwidthHz(ifBandwidthForRate(st.ifBandwidthHz, actual));
    link.rtl->setI2cRepeater(false);
    link.rtl->setIfFreqHz(link.tuner->ifFreqHz());
    retuneChip(link, st.centerHz, st.directChoice);
    return true;
}

// The analogue IF bandwidth (already validated). False when the tuner refused it.
bool chipIfBandwidth(RtlSdrSource::Link& link, int hz, ChipState& st) {
    st.ifBandwidthHz = hz;
    link.rtl->setI2cRepeater(true);
    const bool ok = link.tuner->setBandwidthHz(ifBandwidthForRate(hz, st.rateHz));
    link.rtl->setI2cRepeater(false);
    if (!ok) { return false; }
    link.rtl->setIfFreqHz(link.tuner->ifFreqHz());
    retuneChip(link, st.centerHz, st.directChoice);
    return true;
}

// The crystal correction, re-derived into the tuner's reference and the current frequency retuned. The result is the retune's.
bool chipPpm(RtlSdrSource::Link& link, int ppm, ChipState& st) {
    if (!link.rtl->setFreqCorrectionPpm(ppm)) { return false; }
    // The tuner's PLL reference is the same crystal on every dongle but a standard R828D.
    if (!(link.tuner->chip() == TunerR82xx::Chip::R828D && !link.tuner->blogV4())) {
        link.tuner->setXtalHz(link.rtl->correctedXtalHz());
    }
    return retuneChip(link, st.centerHz, st.directChoice);
}

// The direct-sampling choice (already validated 0..3, and a Blog V4 only 0): where the radio is now put through the rule again. A position outside what the
// new choice accepts is left to the next tune. False only when the retune was refused.
bool chipDirect(RtlSdrSource::Link& link, int choice, ChipState& st) {
    st.directChoice = choice;
    if (st.centerHz > 0.0) {
        double lo = 0.0, hi = 0.0;
        if (rangeFor(link, choice, lo, hi) && (st.centerHz < lo || st.centerHz > hi)) { return true; }
        if (!retuneChip(link, st.centerHz, choice)) { return false; }
    }
    return true;
}

// One gain: "TUNER" (the aggregate) or a stage. `known` is false for a name this radio does not have.
bool chipGain(RtlSdrSource::Link& link, const std::string& name, int tenths, bool& known) {
    known = true;
    link.rtl->setI2cRepeater(true);
    bool ok = false;
    if (name == "TUNER") {
        int lo = 0;
        int hi = 0;
        TunerR82xx::aggregateRangeTenthDb(lo, hi);
        // OUT OF RANGE IS CLAMPED, not refused - the DeviceSource contract.
        ok = link.tuner->setAggregateGainTenthDb(std::min(std::max(tenths, lo), hi));
    } else if (name == "LNA" || name == "MIXER" || name == "VGA") {
        const TunerR82xx::Stage stage = (name == "LNA")     ? TunerR82xx::Stage::Lna
                                        : (name == "MIXER") ? TunerR82xx::Stage::Mixer
                                                            : TunerR82xx::Stage::Vga;
        // A STAGE WRITTEN BY HAND LEAVES AUTOMATIC GAIN (user bug A, 2026-11-23): the part's LNA and mixer loops would overwrite the index within milliseconds, so a stage write under automatic
        // gain did nothing and nothing said so. The aggregate setter above does the same through setAggregateGainTenthDb (it takes both off their loops and clears the tuner's flag).
        ok = link.tuner->autoGain() ? link.tuner->setAutoGain(false) : true;
        ok = ok && link.tuner->setStageIndex(stage, TunerR82xx::nearestStageIndex(stage, tenths));
    } else {
        known = false;
    }
    link.rtl->setI2cRepeater(false);
    // The demodulator's digital AGC follows the tuner's (chipAutoGain turns the two on together): a manual gain turns it off with the tuner's, so the two cannot fight over the signal.
    // Written only when it is on, so a session that never used automatic gain writes nothing more than it did.
    if (known && ok && link.demodAgcOn) {
        link.rtl->setDemodAgc(false);
        link.demodAgcOn = false;
    }
    return known && ok;
}

bool chipAutoGain(RtlSdrSource::Link& link, bool on) {
    link.rtl->setI2cRepeater(true);
    const bool ok = link.tuner->setAutoGain(on);
    link.rtl->setI2cRepeater(false);
    // The demodulator's own digital AGC follows the tuner's, so the two cannot end up fighting each other over the same signal.
    link.rtl->setDemodAgc(on);
    link.demodAgcOn = on;
    return ok;
}

// Records what a setter was asked for (or got done) so a re-open can put it back; bumps the generation a running re-open watches.
template <class F>
void rememberWanted(RtlSdrSource::Link& link, F change) {
    {
        std::lock_guard<std::mutex> lk(link.wantedMutex);
        change(link.wanted);
    }
    link.wantedGen.fetch_add(1, std::memory_order_relaxed);
}

// The user's settings as the re-open was told them (Link::wanted), under the lock that guards it.
RtlSdrSource::Link::Wanted wantedCopy(RtlSdrSource::Link& link) {
    std::lock_guard<std::mutex> lk(link.wantedMutex);
    return link.wanted;
}

// ---------------------------------------------------------------------------
// THE RE-OPEN. On the reader's own thread, over the link only (never the source object: stop() may abandon this thread and the object may be gone while it
// still runs). Constants are plain integers for the reason stated at kMaxStreamRearms: none is a wait on the shutdown path, and the waits inside are
// sleep_for on THIS thread, which stop() bounds with kReaderJoinWait.
// ---------------------------------------------------------------------------
constexpr int kMaxReopensPerWindow = 2;      // then the radio keeps dropping off USB and is called dead
constexpr int kMaxReopensPerOpen = 12;
constexpr int kMaxReopenLockLosses = 3;      // the driver was busy this many times in a row: then the radio is called dead, with the sentence that says so
constexpr int kMaxSettingRounds = 3;         // rounds of putting the user's settings back, while setters keep arriving
constexpr int kReopenSliceMs = 50;           // the wait polls the stop token this often

enum class Reopen { Back, Stopped, Absent, Failed, LockLost, Mismatch };

void beat(RtlSdrSource::Link& link) { link.readerBeatNs.store(steadyNowNs(), std::memory_order_relaxed); }

// A SETTER THAT ARRIVES WHILE A RE-OPEN RUNS hands its change to the re-open, under the lock that guards `wanted`, and only while the re-open is still taking
// them: the re-open clears `reopening` under the same lock, after its last look at `wanted`, so a change is either seen by that look or refused here (false: the
// re-open is over and the setter takes the device in the ordinary way). That is what makes "acknowledged" mean "will reach the chip" (decision F5).
template <class F>
bool deferWhileReopening(RtlSdrSource::Link& link, F change) {
    if (link.beforeDeferForTest) { link.beforeDeferForTest(); }
    std::lock_guard<std::mutex> lk(link.wantedMutex);
    if (!link.reopening.load(std::memory_order_acquire)) { return false; }
    change(link.wanted);
    link.wantedGen.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// The device lock after a re-open ended in the instant between a setter's two looks: the re-open's last act holds it for the length of a stream start, so a few
// tries over about fifty milliseconds (polled, not a timed wait: this is never the long wait again, and nothing on the shutdown path waits here).
// THE READER STANDS ASIDE for it, as for any control call (decision S7): the wait is announced in controlWaiters like controlLock's, or a reader that holds the lock
// for the length of every bulk read could win every try and the setter would be told "busy" on a radio that has just recovered.
std::unique_lock<std::timed_mutex> retakeAfterReopen(RtlSdrSource::Link& link) {
    struct Waiting {
        RtlSdrSource::Link& l;
        explicit Waiting(RtlSdrSource::Link& link2) : l(link2) { l.controlWaiters.fetch_add(1, std::memory_order_relaxed); }
        ~Waiting() { l.controlWaiters.fetch_sub(1, std::memory_order_relaxed); }
    } waiting(link);
    std::unique_lock<std::timed_mutex> lk(link.mutex, std::defer_lock);
    for (int i = 0; i < 50 && !lk.owns_lock(); ++i) {
        if (link.retakeProbeForTest) { link.retakeProbeForTest(link.controlWaiters.load(std::memory_order_relaxed)); }
        if (lk.try_lock()) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (lk.owns_lock() && link.reopening.load(std::memory_order_acquire)) { lk.unlock(); }
    return lk;
}

// Whether what the bring-up found is what the open found: the tuner and the two USB strings. A different dongle in the same place is refused.
// AN EMPTY STRING IS NOT A DIFFERENT ONE (decision S5): the descriptor read answers empty when one transfer fails, which at an open is not fatal. A string that comes
// back empty where the open had one is read again (up to three reads, 100 ms apart, under the device lock the caller holds), and a mismatch is declared only when BOTH
// the remembered and the new string are non-empty and different. The tuner type rule is as it was: a different tuner name is a different radio.
bool sameRadioAsOpened(RtlSdrSource::Link& link, BringUp& bu) {
    RtlSdrSource::Link::Identity id;
    {
        std::lock_guard<std::mutex> lk(link.wantedMutex);
        id = link.identity;
    }
    if (id.tuner.empty()) { return true; }   // nothing was remembered (an open that did not get as far as the tuner)
    if (id.tuner != bu.tunerName) { return false; }
    for (int read = 1;; ++read) {
        const bool contradicts = (!id.manufacturer.empty() && !bu.manufacturer.empty() && id.manufacturer != bu.manufacturer) ||
                                 (!id.product.empty() && !bu.product.empty() && id.product != bu.product);
        if (contradicts) { return false; }
        const bool missing = (!id.manufacturer.empty() && bu.manufacturer.empty()) || (!id.product.empty() && bu.product.empty());
        if (!missing || read >= 3) { return true; }
        // Not a stop's business to wait out: the caller checks the token as soon as this returns.
        for (int slice = 0; slice < 2; ++slice) {
            beat(link);
            if (!link.readerRun.load(std::memory_order_relaxed)) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!link.rtl) { return true; }
        if (bu.manufacturer.empty()) { bu.manufacturer = link.rtl->manufacturer(); }
        if (bu.product.empty()) { bu.product = link.rtl->product(); }
    }
}

// The hardware the re-open closes and reopens is the link's: every step either runs under link.mutex or touches only locals.
Reopen reopenDevice(RtlSdrSource::Link& link, std::string& why, bool& biasWithheld, bool repeatInWindow) {
    biasWithheld = false;
    beat(link);
    // STEP A, under the device lock: take the radio down. No standby or power-down writes are sent - the device is not answering.
    {
        std::unique_lock<std::timed_mutex> lk(link.mutex, kDeviceLockWait);
        if (!lk.owns_lock()) {
            why = "the radio was busy with a control call";
            return Reopen::LockLost;   // nothing was touched: the caller tries again (a bounded number of times), never faults on the first
        }
        link.reopening.store(true, std::memory_order_release);
        beat(link);
        if (link.transport) { link.transport->endBulkStream(); }
        beat(link);
        link.tuner.reset();
        link.rtl.reset();
        link.transport.reset();   // closes the WinUSB handle and the file handle
        beat(link);
    }
    struct Clear {
        RtlSdrSource::Link& l;
        ~Clear() { l.reopening.store(false, std::memory_order_release); }
    } clear{link};

    // STEP B, no lock held: wait for the same radio to be listed again and open it.
    std::unique_ptr<usb::UsbDevice> fresh;
    std::string lastErr;
    bool everPresent = false;
    bool ambiguous = false;
    std::string inUseErr;   // the LAST poll's answer when it was "in use by another program" (decision S9); empty when the last poll said anything else
    const int waitMs = link.reopenWaitMs.load(std::memory_order_relaxed);
    const int pollMs = std::max(1, link.reopenPollMs.load(std::memory_order_relaxed));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    for (;;) {
        if (!link.readerRun.load(std::memory_order_relaxed) || link.abandoned.load(std::memory_order_relaxed)) { return Reopen::Stopped; }
        bool present = false;
        std::string err;
        beat(link);
        fresh = link.reopen(err, present);
        beat(link);
        if (fresh) { break; }
        everPresent = everPresent || present;
        ambiguous = (err == kRadioAmbiguousError);
        // "IN USE" IS NOT AN INSTANT FAULT (decision S9): the other holder may be gone a moment later, or our own device may reappear under another candidate. The
        // wait goes on to its end, and the fault is made only if that is still the answer then.
        inUseErr = usb::errorSaysInUse(err) ? err : std::string();
        if (!err.empty() && !ambiguous && inUseErr.empty()) { lastErr = err; }
        if (std::chrono::steady_clock::now() >= deadline) { break; }
        // Sleep in slices, storing the heartbeat (so read()'s stuck-reader check stays quiet) and watching the stop token.
        const auto until = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(pollMs));
        while (std::chrono::steady_clock::now() < until) {
            if (!link.readerRun.load(std::memory_order_relaxed)) { return Reopen::Stopped; }
            beat(link);
            std::this_thread::sleep_for(std::chrono::milliseconds(kReopenSliceMs));
        }
    }
    if (!fresh) {
        if (!inUseErr.empty()) {
            why = inUseErr;
            return Reopen::Failed;
        }
        if (ambiguous) {
            why = kReopenWhichSentence;
            return Reopen::Mismatch;
        }
        if (everPresent) {
            why = lastErr.empty() ? std::string("the radio would not open") : lastErr;
            return Reopen::Failed;
        }
        why = "the radio is not on the USB bus";
        return Reopen::Absent;
    }
    // Stopped or abandoned meanwhile: install nothing, let the new handle go.
    if (!link.readerRun.load(std::memory_order_relaxed) || link.abandoned.load(std::memory_order_relaxed)) { return Reopen::Stopped; }

    // STEP C, under the device lock: install, bring the chip up from nothing and put the settings back.
    beat(link);
    std::unique_lock<std::timed_mutex> lk(link.mutex, kDeviceLockWait);
    if (!lk.owns_lock()) {
        why = "the radio was busy with a control call";
        return Reopen::Failed;
    }
    // WHATEVER WAY THIS LEAVES EXCEPT BACK, THE DEVICE IT INSTALLED IS TAKEN OUT AGAIN, under the lock, before `reopening` comes down: a stop() that lands in
    // the bring-up, a failed step or a radio that is not the same one must not leave a half-initialised chip for a later start() to stream from (decision F1).
    struct Drop {
        RtlSdrSource::Link& l;
        bool armed = true;
        ~Drop() {
            if (armed) {
                l.tuner.reset();
                l.rtl.reset();
                l.transport.reset();
            }
        }
    } drop{link};
    if (!link.readerRun.load(std::memory_order_relaxed) || link.abandoned.load(std::memory_order_relaxed)) { return Reopen::Stopped; }
    link.transport = std::move(fresh);
    link.rtl = std::make_unique<Rtl2832u>(*link.transport, static_cast<unsigned>(kControlTimeout.count()));
    link.demodAgcOn = false;   // a fresh chip
    link.rtl->setKeepGoing(&link.readerRun);   // a stop() now ends the bring-up within the one transfer in flight
    link.rtl->setBeat(&link.readerBeatNs);     // and a long bring-up is a live reader, not a wedged one
    link.rtl->setDeadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(link.reopenBringUpMs.load(std::memory_order_relaxed)));   // and it is bounded as a whole
    auto stopped = [&link] { return !link.readerRun.load(std::memory_order_relaxed); };
    const char* phase = "the chip bring-up";
    auto expired = [&link] { return link.rtl && link.rtl->deadlineExpired(); };
    auto fail = [&](const std::string& step) {
        if (stopped()) { why = step; return Reopen::Stopped; }
        if (expired()) {
            why = std::string(phase) + " did not finish in time";
            return Reopen::Failed;
        }
        // Every failing step says its own words and the transport's error.
        why = step + ": " + (link.rtl ? link.rtl->lastError() : std::string());
        if (link.transport && !link.transport->lastError().empty() && why.find(link.transport->lastError()) == std::string::npos) { why += " [" + link.transport->lastError() + "]"; }
        return Reopen::Failed;
    };
    // Whatever way this function leaves, the token comes off the chip object: a later teardown (stop() clears the same flag) must still be able to write
    // the tuner's standby and the demodulator's power-down.
    struct TokenOff {
        RtlSdrSource::Link& l;
        ~TokenOff() {
            if (l.rtl) {
                l.rtl->setKeepGoing(nullptr);
                l.rtl->setDeadline(std::chrono::steady_clock::time_point{});
                l.rtl->setBeat(nullptr);
            }
        }
    } tokenOff{link};
    BringUp bu;
    // THE BIAS TEE IS NOT SWITCHED ON BY THE BRING-UP'S EEPROM RULE here: a dongle whose EEPROM forces it gets it written OFF, and the settings below put it where
    // the user's wish and the repeat rule say (decision F3a, 3b). Everything else the bring-up writes is the open's.
    const bool teeOffInBringUp = false;
    // The IF bandwidth is applied AFTER the bring-up, by its own step, in the order the setters would have run (see chipIfBandwidth); the bring-up is the open's.
    if (!bringUpChip(link, 0, 0, bu, &teeOffInBringUp)) {
        if (stopped()) { why = "stopped"; return Reopen::Stopped; }
        if (expired()) {
            why = std::string(phase) + " did not finish in time";
            return Reopen::Failed;
        }
        why = bu.error + (link.rtl->lastError().empty() ? std::string() : " [" + link.rtl->lastError() + "]");
        return Reopen::Failed;
    }
    // THE SAME RADIO? What the bring-up read from the device must be what the open read from the one that left: a different tuner or different USB strings is a
    // different dongle that happens to sit where the old one did, and the user's settings, bias tee included, are not put onto it (decision F4).
    if (!sameRadioAsOpened(link, bu)) {
        why = kReopenWhichSentence;
        return Reopen::Mismatch;
    }
    if (stopped()) { why = "stopped"; return Reopen::Stopped; }
    phase = "putting the settings back";
    ChipState st;
    st.rateHz = bu.rateHz;
    st.centerHz = kDefaultFreqHz;
    // ONE PASS OF PUTTING THE USER'S SETTINGS BACK, in the order the setters are called, each by the same device work a setter does. Null on success, else the words of
    // the step that failed. A zero value is applied when the user SET it (decision S6): a correction, a bandwidth or a direct-sampling choice changed back to its
    // zero is a change like any other, which a test for non-zero would only have acknowledged.
    auto applyWanted = [&](const RtlSdrSource::Link::Wanted& w) -> const char* {
        if ((w.ppm != 0 || w.ppmSet) && !chipPpm(link, w.ppm, st)) { return "the radio would not take its crystal correction back"; }
        if (w.rateHz > 0.0 && std::fabs(nearestSupportedRate(w.rateHz) - st.rateHz) > 0.5) {
            if (!chipRate(link, nearestSupportedRate(w.rateHz), st)) { return "the radio would not take its sample rate back"; }
        }
        if ((w.ifBandwidthHz != 0 || w.ifBandwidthSet) && !chipIfBandwidth(link, w.ifBandwidthHz, st)) { return "the tuner would not take its IF bandwidth back"; }
        if ((w.directChoice != 0 || w.directChoiceSet) && !chipDirect(link, w.directChoice, st)) { return "the radio would not take its direct-sampling choice back"; }
        if (w.autoGainSet && !chipAutoGain(link, w.autoGain)) { return "the tuner would not take its gain mode back"; }
        if (w.gainSet && !(w.autoGainSet && w.autoGain)) {
            bool known = true;
            if (w.gainByStage) {
                static const char* const kNames[3] = {"LNA", "MIXER", "VGA"};
                for (int s2 = 0; s2 < 3; ++s2) {
                    if (w.stageSet[s2] && !chipGain(link, kNames[s2], w.stageTenthDb[s2], known)) { return "the tuner would not take its gain back"; }
                }
            } else if (!chipGain(link, "TUNER", w.aggregateTenthDb, known)) {
                return "the tuner would not take its gain back";
            }
        }
        if (w.freqHz > 0.0) {
            double lo = 0.0, hi = 0.0;
            const bool inRange = !rangeFor(link, st.directChoice, lo, hi) || (w.freqHz >= lo && w.freqHz <= hi);
            if (inRange) {
                if (!retuneChip(link, w.freqHz, st.directChoice)) { return "the radio would not tune back"; }
                st.centerHz = w.freqHz;
            }
        }
        // THE BIAS TEE: back where the user's latest wish has it on the first re-open of a window, and OFF on a repeat - a short or an overload on the coax is
        // itself a way to brown a dongle out of the bus, and switching 4.5 V straight back on would loop. What is on the wire is what the readback and the
        // log say: when the rule withholds an "on", the tee is switched OFF if anything switched it on, and `wanted` says off from then on.
        if (w.biasTee && repeatInWindow) {
            biasWithheld = true;
            {
                std::lock_guard<std::mutex> wlk(link.wantedMutex);
                link.wanted.biasTee = false;
            }
            if (link.biasTee.load(std::memory_order_relaxed)) {
                if (!link.rtl->setBiasTee(false)) { return "the radio would not switch its bias tee off"; }
            }
            link.biasTee.store(false, std::memory_order_relaxed);
        } else if (w.biasTee != link.biasTee.load(std::memory_order_relaxed)) {
            if (!link.rtl->setBiasTee(w.biasTee)) { return "the radio would not take its bias tee back"; }
            link.biasTee.store(w.biasTee, std::memory_order_relaxed);
        }
        return nullptr;
    };
    // A setting asked for DURING a round is picked up by the next one: after each round `wanted` is looked at again under its lock, and the re-open ends (`reopening`
    // comes down, under the same lock) only when nothing arrived since the round began (decision F5). The device lock stays held meanwhile, so a setter that arrives
    // after that moment waits for the stream start below and then reaches the chip in the ordinary way.
    bool settled = false;
    for (int round = 0; round < kMaxSettingRounds && !settled; ++round) {
        unsigned gen = 0;
        RtlSdrSource::Link::Wanted w;
        {
            std::lock_guard<std::mutex> wlk(link.wantedMutex);
            w = link.wanted;
            gen = link.wantedGen.load(std::memory_order_relaxed);
        }
        if (stopped()) { why = "stopped"; return Reopen::Stopped; }
        if (const char* bad = applyWanted(w)) { return fail(bad); }
        // THE LAST LOOK, and the end of the re-open's taking of settings: under the lock a setter hands its change over by.
        {
            std::lock_guard<std::mutex> wlk(link.wantedMutex);
            if (link.wantedGen.load(std::memory_order_relaxed) == gen) {
                link.reopening.store(false, std::memory_order_release);
                settled = true;
            }
        }
    }
    if (!settled) {
        // THREE ROUNDS AND A SETTER IS STILL ARRIVING (decision S6): `reopening` comes down and the newest wish is taken in the same hold of the lock that guards it, so a
        // change is either in the copy or made after the flag came down, by a setter that then waits for the device lock this thread holds; and one last pass puts that
        // newest wish on. Nothing the user asked for is acknowledged and not applied.
        RtlSdrSource::Link::Wanted last;
        {
            std::lock_guard<std::mutex> wlk(link.wantedMutex);
            link.reopening.store(false, std::memory_order_release);
            last = link.wanted;
        }
        core::diagWarnf("source: a setting arrived while the radio was being put back after a re-open, three times over; the newest one was put on in a last pass");
        if (stopped()) { why = "stopped"; return Reopen::Stopped; }
        if (const char* bad = applyWanted(last)) { return fail(bad); }
    }
    // THE STREAM, in startStreamLocked's order, into the same ring: the endpoint FIFO reset, then the pipe.
    phase = "the stream restart";
    if (!link.rtl->resetBuffer()) { return fail("the radio would not reset its endpoint"); }
    if (!link.transport->beginBulkStream(kBulkEndpoint, kBulkBufferBytes, kBulkBufferCount)) {
        if (stopped()) { why = "stopped"; return Reopen::Stopped; }
        why = "the radio's bulk endpoint would not start: " + link.transport->lastError();
        return Reopen::Failed;
    }
    reportStreamNote(*link.transport, link.owner, "rtlsdr");
    if (stopped()) {
        // A stop() landed while the stream was being started: the radio is closed again, as for any stop inside a re-open.
        link.transport->endBulkStream();
        why = "stopped";
        return Reopen::Stopped;
    }
    drop.armed = false;
    return Reopen::Back;
}

// THE READER THREAD. A free function taking the link by shared pointer, so an
// abandoned thread reads memory that is still alive - see the header.
void readerLoop(std::shared_ptr<RtlSdrSource::Link> link) {
    std::vector<std::uint8_t> raw(kBulkBufferBytes);
    std::vector<std::complex<float>> conv(kBulkBufferBytes / 2);
    int rearms = 0;
    int reopensInWindow = 0;
    auto lastSamplesAt = std::chrono::steady_clock::now();
    auto lastRearmAt = lastSamplesAt;
    auto lastReopenAt = lastSamplesAt;
    const auto streamStartedAt = lastSamplesAt;
    // THE RECOVERY IN PROGRESS, one episode from "samples stopped" to "samples flowed again" or "the radio was given up". The outcome words are filed when it
    // ENDS (diagnosis 2026-11-23, section 6): a report that says a restart began tells nobody whether it worked.
    struct Episode {
        bool active = false;
        bool reopened = false;   // a re-open was part of it
        int restarts = 0;        // stream restarts made in it
        std::chrono::steady_clock::time_point since{};   // when the samples stopped
    } episode;
    // The one place the four outcome words are raised (one site each, held by tests/test_health_paths.cpp).
    auto raiseOutcome = [](bool ok, bool reopened) {
        if (ok) {
            core::health::raiseRecovered(reopened ? core::health::Recovered::UsbReopen : core::health::Recovered::StreamBack);
        } else {
            core::health::raiseRecovered(reopened ? core::health::Recovered::UsbReopenFail : core::health::Recovered::StreamFail);
        }
    };
    // THE DIAGNOSTIC LINE of a recovery's start (R4): which call failed with which Windows error, whether the radio is still listed on the bus (asked by its
    // path, never printed), how long this stream had run and how long it had been silent. No serial number, no path.
    auto sayStopped = [&](const char* what, const std::string& readText, const std::string& account, int restart) {
        beat(*link);
        const int listed = link->listed ? link->listed() : -1;
        beat(*link);
        const auto now2 = std::chrono::steady_clock::now();
        core::diagWarnf("source: the RTL-SDR stream stopped - %s%s%s; last stream start: %s; still listed on USB: %s; stream ran %lld s; last samples %lld ms ago; restart %d of %d", what,
                        readText.empty() ? "" : ": ", readText.c_str(), account.empty() ? "no account" : account.c_str(), listed < 0 ? "unknown" : (listed > 0 ? "yes" : "no"),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(now2 - streamStartedAt).count()),
                        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now2 - lastSamplesAt).count()), restart, kMaxStreamRearms);
    };
    std::string streamAccount;
    while (link->readerRun.load(std::memory_order_relaxed)) {
        link->readerBeatNs.store(steadyNowNs(), std::memory_order_relaxed);
        if (link->controlWaiters.load(std::memory_order_relaxed) > 0) {
            // A retune, a gain change, a stop: let it in. The transport's own
            // queue of bulk transfers keeps filling meanwhile.
            std::this_thread::sleep_for(kReaderStandAside);
            continue;
        }
        int got = 0;
        std::uint64_t epoch = 0;
        std::string readError;
        {
            std::unique_lock<std::timed_mutex> lk(link->mutex, kDeviceLockWait);
            if (!lk.owns_lock()) {
                // A control call is inside the device right now. Not an error
                // and not this thread's business: come back.
                continue;
            }
            if (!link->readerRun.load(std::memory_order_relaxed)) { break; }
            if (!link->transport || !link->transport->streaming()) { break; }
            // Under the same lock a rate change bumps it under, so these
            // bytes belong to exactly the generation noted here.
            epoch = link->rateEpoch.load(std::memory_order_relaxed);
            got = link->transport->readBulk(raw.data(), raw.size(),
                                            static_cast<unsigned>(kBulkReadTimeout.count()));
            if (got < 0) {
                readError = link->transport->lastError();
                streamAccount = link->transport->streamAccount();
            }
        }
        if (!link->readerRun.load(std::memory_order_relaxed)) { break; }
        noteRead(*link, got, got > 0 ? static_cast<std::size_t>(got) / 2u : 0u);
        const auto nowT = std::chrono::steady_clock::now();
        if (got > 0) {
            if (episode.active) {
                // THE OUTCOME, at last: samples flowed again. One log line and the success word; a stop that arrived first never gets here.
                const long long ms = static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(nowT - episode.since).count());
                if (episode.reopened) {
                    core::diagLogf("source: the RTL-SDR is back after being reopened (%lld ms without samples)", ms);
                    rre::noteVendorCallFault(link->owner, "rtlsdr", "usbreopen", "the radio stopped answering and was reopened; samples flowed again after " + std::to_string(ms) + " ms", true);
                } else {
                    core::diagLogf("source: the stream is back after a restart (%lld ms without samples)", ms);
                }
                raiseOutcome(true, episode.reopened);
                episode = Episode{};
            }
            lastSamplesAt = nowT;
            if (rearms > 0 && nowT - lastRearmAt >= std::chrono::milliseconds(kRearmForgiveAfterMs)) { rearms = 0; }
            if (reopensInWindow > 0 && nowT - lastReopenAt >= std::chrono::milliseconds(link->reopenForgiveAfterMs.load(std::memory_order_relaxed))) { reopensInWindow = 0; }
        }
        const bool silent = got == 0 && nowT - lastSamplesAt >= std::chrono::milliseconds(link->silenceRearmAfterMs.load(std::memory_order_relaxed));
        if (got < 0 || silent) {
            // A FAILED READ IS A STALLED STREAM, and so is a stream that has said nothing for seconds; neither is yet a dead radio. The stream is re-armed
            // (kMaxStreamRearms times); when that cannot work - the control write is refused at once (the handle is dead), the error says the device left
            // the bus, or three re-arms did not bring samples back - the radio is CLOSED AND OPENED AGAIN (reopenDevice), if this source can (open() arms it;
            // the tests' door does not). Only when that cannot be, or does not help, is it a fault, worded for what it is, with the transport's own text so
            // the next report says WHICH error.
            const bool canReopen = static_cast<bool>(link->reopen);
            if (!episode.active) {
                episode = Episode{};
                episode.active = true;
                episode.since = lastSamplesAt;
            }
            // The one way out of the episode that is a fault: files the outcome word for a recovery that was tried, and (for a re-open) the journal's
            // recovery event, then latches the radio dead. `outcome` is false for a radio that really left the bus: that is an unplug, not a failed recovery.
            // `reopenPath` is true for every give-up that is the re-open's: the cap, a lost race, a failed or refused re-open. Those always file `usbreopenfail`
            // and the journal's `gave-up` (never `streamfail`, never nothing), whether or not an earlier re-open belonged to this episode (decision F7).
            auto giveUp = [&](std::string msg, bool outcome, bool reopenPath = false) {
                const bool viaReopen = reopenPath || episode.reopened;
                if (outcome && viaReopen) { rre::noteVendorCallFault(link->owner, "rtlsdr", "usbreopen", msg, false); }
                noteFault(*link, msg, "reading samples");
                if (outcome && (viaReopen || episode.restarts > 0)) { raiseOutcome(false, viaReopen); }
            };
            // A STOP THAT LANDED WHILE THE RADIO WAS BEING RECONNECTED (or while the stream was failing, before the re-open's first step): the radio is latched
            // FAULTED with the sentence that says why, so a later start() is refused with those words and the window shows them (decisions F1, S3). Not when the radio
            // was abandoned: stop() has already latched its own, true, words and nothing here may touch an object that may be gone. Not journalled as a radio fault
            // (decision S10): a stop is the user's, the engine writes its close event with that cause.
            auto latchStopped = [&] {
                if (link->abandoned.load(std::memory_order_relaxed)) { return; }
                bool already = false;
                {
                    std::lock_guard<std::mutex> lk(link->errorMutex);
                    already = link->faulted;
                }
                if (!already) { noteFault(*link, kReopenStoppedSentence, "reconnecting the radio", false); }
            };
            bool wantReopen = false;
            std::string failWhy;
            if (got < 0 && transportSaysGone(readError)) {
                if (!canReopen) {
                    giveUp("the radio stopped delivering samples (it may have been unplugged): " + readError, false);
                    break;
                }
                sayStopped("read", readError, streamAccount, rearms);
                wantReopen = true;
                failWhy = readError;
            } else if (rearms >= kMaxStreamRearms) {
                char tail[64];
                std::snprintf(tail, sizeof(tail), " after %d restarts of the stream", kMaxStreamRearms);
                if (!canReopen) {
                    giveUp(got < 0 ? "the radio stopped delivering samples" + std::string(tail) + (readError.empty() ? std::string() : ": " + readError)
                                   : "the radio delivered nothing for " + std::to_string(link->silenceRearmAfterMs.load(std::memory_order_relaxed)) + " ms at a time" + std::string(tail),
                           true);
                    break;
                }
                sayStopped(got < 0 ? "read" : "silence", readError, streamAccount, rearms);
                wantReopen = true;
                failWhy = got < 0 ? "the stream failed again" + std::string(tail) : "the stream delivered nothing" + std::string(tail);
            } else {
                sayStopped(got < 0 ? "read" : "silence", readError, streamAccount, rearms + 1);
                core::diagWarnf("source: the RTL-SDR %s%s%s; restarting the stream (attempt %d of %d)", got < 0 ? "read failed" : "delivered nothing",
                                readError.empty() ? "" : ": ", readError.c_str(), rearms + 1, kMaxStreamRearms);
                std::string why;
                std::string after;
                const Rearm r = rearmStream(*link, why);
                if (r != Rearm::LockLost) { after = "restart attempted"; }
                (void)after;
                if (!link->readerRun.load(std::memory_order_relaxed)) {
                    if (r == Rearm::Failed) { latchStopped(); }   // the stream is down and the handle is not answering: not left dead and unmarked (decision S3)
                    break;
                }
                if (r == Rearm::LockLost) { continue; }   // a control call held the device: neither a spent re-arm nor a fault; look again
                ++rearms;
                ++episode.restarts;
                lastRearmAt = std::chrono::steady_clock::now();
                lastSamplesAt = lastRearmAt;
                if (r == Rearm::Back) { continue; }
                {
                    // WHICH CALL FAILED, with which Windows error, and what the pipe-level calls answered (R3/R4).
                    std::string account;
                    {
                        std::unique_lock<std::timed_mutex> lk(link->mutex, kDeviceLockWait);
                        if (lk.owns_lock() && link->transport) { account = link->transport->streamAccount(); }
                    }
                    const int listed = link->listed ? link->listed() : -1;
                    core::diagWarnf("source: the RTL-SDR stream would not restart - %s; last stream start: %s; still listed on USB: %s", why.c_str(),
                                    account.empty() ? "no account" : account.c_str(), listed < 0 ? "unknown" : (listed > 0 ? "yes" : "no"));
                }
                if (!canReopen) {
                    giveUp("the radio stopped delivering samples and the stream would not restart: " + why, true);
                    break;
                }
                wantReopen = true;
                failWhy = why;
            }
            if (wantReopen) {
                const bool repeat = reopensInWindow > 0;
                const int total = link->reopensThisOpen.load(std::memory_order_relaxed);
                if (reopensInWindow >= kMaxReopensPerWindow || total >= kMaxReopensPerOpen) {
                    giveUp("the radio keeps dropping off USB: it was reopened " + std::to_string(reopensInWindow >= kMaxReopensPerWindow ? reopensInWindow : total) +
                               " times and stopped answering again (" + failWhy + ")",
                           true, true);
                    break;
                }
                core::diagWarnf("source: the RTL-SDR stopped answering (%s); closing it and opening it again (re-open %d of %d in this window, %d of %d in this open)",
                                failWhy.c_str(), reopensInWindow + 1, kMaxReopensPerWindow, total + 1, kMaxReopensPerOpen);
                std::string why;
                bool biasWithheld = false;
                // LOSING THE RACE FOR THE DEVICE LOCK is a retry, never a way out of the loop with the stream still down (decision F2): the re-open is tried again,
                // at most kMaxReopenLockLosses times in a row, and only then is the radio called dead - with the sentence that says what happened.
                Reopen r = Reopen::LockLost;
                for (int tries = 0; tries < kMaxReopenLockLosses && r == Reopen::LockLost; ++tries) {
                    r = reopenDevice(*link, why, biasWithheld, repeat);
                    if (r == Reopen::LockLost && !link->readerRun.load(std::memory_order_relaxed)) { break; }
                }
                if (r == Reopen::LockLost) {
                    // (A stop() that arrived while the lock was being waited for touched nothing: the radio is as it was, the stream down, and the stop() ends the reader;
                    // the radio is latched with the stopped sentence, decision S3.)
                    if (link->readerRun.load(std::memory_order_relaxed) && !link->abandoned.load(std::memory_order_relaxed)) {
                        giveUp(kReopenBusySentence, true, true);
                    } else {
                        latchStopped();
                    }
                    break;
                }
                // THE STOP AND ABANDON STATE IS LOOKED AT BEFORE ANY FAULT, journal entry or log line is written (decision S4): a Failed, Absent or Mismatch that a stop (or
                // an abandonment by stop() after its bound) raced is a Stopped one. An abandoned reader writes nothing at all - at process exit it would touch the
                // function-static registry and the log after their destructors (crash signatures 929BF825 and 74EEC22D).
                if ((r == Reopen::Failed || r == Reopen::Absent || r == Reopen::Mismatch) &&
                    (!link->readerRun.load(std::memory_order_relaxed) || link->abandoned.load(std::memory_order_relaxed))) {
                    r = Reopen::Stopped;
                }
                if (r == Reopen::Stopped) {
                    // A STOP THAT LANDED INSIDE THE RE-OPEN: the radio is closed (reopenDevice took out anything it had installed) and latched FAULTED with the
                    // sentence that says why (decision F1; see latchStopped).
                    latchStopped();
                    break;
                }
                if (r == Reopen::Back && !link->readerRun.load(std::memory_order_relaxed)) { break; }   // the stream runs and the stop() that is waiting ends it
                ++reopensInWindow;
                link->reopensThisOpen.fetch_add(1, std::memory_order_relaxed);
                episode.reopened = true;
                if (r == Reopen::Absent) {
                    giveUp("the radio stopped delivering samples and left the USB bus (it may have been unplugged); it did not come back within " +
                               std::to_string(link->reopenWaitMs.load(std::memory_order_relaxed)) + " ms: " + failWhy,
                           false);
                    break;
                }
                if (r == Reopen::Mismatch) {
                    giveUp(kReopenWhichSentence, true, true);   // the sentence whole: it says what to do
                    break;
                }
                if (r == Reopen::Failed) {
                    giveUp("the radio stopped answering and reopening it did not help (" + why + ")", true, true);
                    break;
                }
                // Back: the stream runs again into the same ring. The clocks start over; a bias tee that was withheld is said out loud, once.
                rearms = 0;
                lastReopenAt = std::chrono::steady_clock::now();
                lastRearmAt = lastReopenAt;
                lastSamplesAt = lastReopenAt;
                core::diagWarnf("source: the RTL-SDR was closed and opened again; waiting for samples");
                if (biasWithheld) { core::diagWarnf("source: the radio was reconnected. Its bias tee was left off because the radio dropped out twice."); }
            }
            continue;
        }
        if (got == 0) { continue; }

        // 8-bit unsigned interleaved I/Q, one lookup per component. An odd
        // byte count cannot happen on a 512-multiple pipe, but half a pair
        // would swap I and Q for the rest of the stream, so the odd byte is
        // dropped rather than trusted.
        const std::size_t pairs = static_cast<std::size_t>(got) / 2u;
        for (std::size_t i = 0; i < pairs; ++i) {
            conv[i] = std::complex<float>(kByteToFloat.v[raw[2 * i]],
                                          kByteToFloat.v[raw[2 * i + 1]]);
        }
        if (link->ring) {
            // A buffer read before a rate change is not delivered after it:
            // it would be counted, demodulated and displayed at the new rate.
            std::lock_guard<std::mutex> rlk(link->ringMutex);
            if (link->rateEpoch.load(std::memory_order_relaxed) != epoch) { continue; }
            const std::size_t wrote = link->ring->write(conv.data(), pairs);
            if (wrote < pairs) {
                // The consumer fell behind. Counted as an overflow, which is
                // what it is, and never a reason to stop a live radio.
                std::lock_guard<std::mutex> lk(link->healthMutex);
                link->health.overflows += 1;
            }
        }
    }
}

}  // namespace

// The dongles this driver opens. Every one is an RTL2832U; the list is the
// set of USB ids those chips ship under, because Windows binds a driver per
// id and an id that is not here can never have been bound to WinUSB.
//
// NEVER DESTROYED (0.99.89, crash signature 74EEC22DE4163D64): the radio-setup
// probe reads this from a DETACHED worker (core/radio_setup.cpp, Probe::start)
// that can still be scanning when main returns, and the C runtime runs the
// static destructors while every other thread is still alive. As a plain
// function-local static it was freed under that worker, whose next
// ids[matched] read address zero. A heap object nothing deletes outlives
// exit; tests/test_radio_setup_exit.cpp holds it.
const std::vector<usb::UsbId>& rtlSdrUsbIds() {
    static const std::vector<usb::UsbId>* const ids =
        cascade::core::leakOnPurpose(new std::vector<usb::UsbId>{
        {0x0bda, 0x2832},  // Realtek RTL2832U, generic
        {0x0bda, 0x2838},  // Realtek RTL2832U OEM - the commonest by far
        {0x0413, 0x6680}, {0x0413, 0x6f0f}, {0x0458, 0x707f},
        {0x0ccd, 0x00a9}, {0x0ccd, 0x00b3}, {0x0ccd, 0x00b4}, {0x0ccd, 0x00b5},
        {0x0ccd, 0x00b7}, {0x0ccd, 0x00b8}, {0x0ccd, 0x00b9}, {0x0ccd, 0x00c0},
        {0x0ccd, 0x00c6}, {0x0ccd, 0x00d3}, {0x0ccd, 0x00d7}, {0x0ccd, 0x00e0},
        {0x1554, 0x5020}, {0x15f4, 0x0131}, {0x15f4, 0x0133},
        {0x185b, 0x0620}, {0x185b, 0x0650}, {0x185b, 0x0680},
        {0x1b80, 0xd393}, {0x1b80, 0xd394}, {0x1b80, 0xd395}, {0x1b80, 0xd397},
        {0x1b80, 0xd398}, {0x1b80, 0xd39d}, {0x1b80, 0xd3a4}, {0x1b80, 0xd3a8},
        {0x1b80, 0xd3af}, {0x1b80, 0xd3b0},
        {0x1d19, 0x1101}, {0x1d19, 0x1102}, {0x1d19, 0x1103}, {0x1d19, 0x1104},
        {0x1f4d, 0xa803}, {0x1f4d, 0xb803}, {0x1f4d, 0xc803}, {0x1f4d, 0xd286},
        {0x1f4d, 0xd803},
    });
    return *ids;
}

// THE SAME PHYSICAL RADIO, found again on the bus after it dropped off (the re-open), and NEVER BY GUESSING (decision F4). In this order: the exact interface path
// (accepted only when the remembered location and the candidate's do not contradict each other, decision S2); else the same hub-and-port location; else the same serial, when that serial was the only
// one of its kind at open and is the only one now. A listed device whose serial differs from a non-empty remembered one is a different dongle and is never taken
// for it, whatever port it sits on. When the serial points at devices but proves nothing (two dongles both say "00000001", before or now) the answer is
// Ambiguous and the caller opens none of them. Nothing is opened here.
RadioLocate locateRadio(const std::vector<usb::UsbDeviceInfo>& found, const RadioIdentity& id, std::string& foundPath) {
    auto lower = [](std::string s) {
        for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return s;
    };
    // The same dongle as far as the serial can say: either side without one cannot contradict, two serials that differ do.
    auto serialAgrees = [&id](const usb::UsbDeviceInfo& d) { return id.serial.empty() || d.serial.empty() || d.serial == id.serial; };
    // THE SAME PORT, as far as the location can say (decision S2): a location known on BOTH sides that differs is a different dongle. For a dongle with a serial the
    // interface path follows the serial, not the port, and two dongles that say the same serial can come back from a drop holding each other's paths; the path alone must
    // not take the other one. An unknown location on either side proves nothing, so the path then stands.
    auto locationAgrees = [&id](const usb::UsbDeviceInfo& d) { return id.location.empty() || d.location.empty() || d.location == id.location; };
    if (!id.path.empty()) {
        const std::string want = lower(id.path);
        for (const usb::UsbDeviceInfo& d : found) {
            if (lower(d.path) == want && serialAgrees(d) && locationAgrees(d)) {
                foundPath = d.path;
                return RadioLocate::Found;
            }
        }
    }
    if (!id.location.empty()) {
        const usb::UsbDeviceInfo* only = nullptr;
        int n = 0;
        for (const usb::UsbDeviceInfo& d : found) {
            if (d.location == id.location && serialAgrees(d)) { only = &d; ++n; }
        }
        if (n == 1) {
            foundPath = only->path;
            return RadioLocate::Found;
        }
    }
    if (!id.serial.empty()) {
        const usb::UsbDeviceInfo* only = nullptr;
        int n = 0;
        for (const usb::UsbDeviceInfo& d : found) {
            if (d.serial == id.serial) { only = &d; ++n; }
        }
        if (n == 1 && id.serialUnique) {
            foundPath = only->path;
            return RadioLocate::Found;
        }
        if (n >= 1) { return RadioLocate::Ambiguous; }
    }
    return RadioLocate::Absent;
}

std::function<std::unique_ptr<usb::UsbDevice>(std::string& error, bool& present)> makeRadioReopenFn(RadioIdentity id, RadioListFn list, RadioOpenFn open) {
    return [id = std::move(id), list = std::move(list), open = std::move(open)](std::string& error, bool& present) -> std::unique_ptr<usb::UsbDevice> {
        std::string where;
        switch (locateRadio(list(), id, where)) {
            case RadioLocate::Found:
                present = true;
                return open(where, error);
            case RadioLocate::Ambiguous:
                present = true;
                error = kRadioAmbiguousError;
                return nullptr;
            case RadioLocate::Absent:
                break;
        }
        present = false;
        return nullptr;
    };
}

std::function<int()> makeRadioListedFn(RadioIdentity id, RadioListFn list) {
    return [id = std::move(id), list = std::move(list)]() -> int {
        std::string where;
        return locateRadio(list(), id, where) == RadioLocate::Absent ? 0 : 1;
    };
}

std::vector<NativeDeviceInfo> enumerateRtlSdr() {
    std::vector<NativeDeviceInfo> out;
    const std::vector<usb::UsbDeviceInfo> found = usb::enumerateWinUsb(rtlSdrUsbIds());
    int index = 0;
    for (const usb::UsbDeviceInfo& d : found) {
        NativeDeviceInfo info;
        info.driver = "rtlsdr";
        // The bus-reported description is what the dongle calls itself
        // ("RTL2838UHIDIR"); fall back to something recognisable when the
        // device does not offer one.
        const std::string what = d.description.empty() ? std::string("RTL-SDR") : d.description;
        if (d.serial.empty()) {
            info.label = what;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "index=%d", index);
            info.args = buf;
        } else {
            info.label = what + " (serial " + d.serial + ")";
            info.args = "serial=" + d.serial;
        }
        out.push_back(std::move(info));
        ++index;
    }
    return out;
}

// --- error slot -------------------------------------------------------------

RtlSdrSource::~RtlSdrSource() { closeDevice(); }

void RtlSdrSource::setError(std::string msg) {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    // A LATCHED FAULT KEEPS ITS OWN WORDS. Every later call on a dead radio is refused with "this radio has faulted; reopen it", and that refusal
    // used to replace what the fault said: the journal's close event for a radio read "this radio has faulted; reopen it" and nothing of why it
    // faulted (field fix A). clearError() lifts the latch when the radio is opened again.
    if (link_->faulted) { return; }
    link_->lastError = std::move(msg);
}

void RtlSdrSource::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
    link_->faultedWhile.clear();
}

bool RtlSdrSource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool RtlSdrSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string RtlSdrSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faultedWhile;
}

const char* RtlSdrSource::name() const {
    // Per-thread snapshot, exactly as SoapySource does it: open() can rewrite
    // the name while the GUI draws it, so the pointer handed out must not
    // alias the member.
    thread_local std::string snapshot;
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    snapshot = link_->name;
    return snapshot.c_str();
}

const char* RtlSdrSource::lastError() const {
    thread_local std::string snapshot;
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    snapshot = link_->lastError;
    return snapshot.c_str();
}

// --- open -------------------------------------------------------------------

RtlBusView shippedRtlBusView() {
    RtlBusView v;
    v.bound = [] { return usb::enumerateWinUsb(rtlSdrUsbIds()); };
    v.unbound = [] { return usb::enumerateUnbound(rtlSdrUsbIds()); };
    v.present = [](std::vector<usb::UsbId>& out) { return usb::presentUsbIds(out); };
    return v;
}

cascade::core::rtlopen::Case noRtlRadioCase(const RtlBusView& bus) {
    // THE UNBOUND LIST FIRST: a dongle on the bus with no WinUSB interface is the answer whatever else the bus holds (the engine's own scan uses the same call, engine.cpp
    // scanNative). Then the whole bus listing, which says "not plugged in" only when it could be taken and holds no RTL-SDR id.
    const bool anyUnbound = bus.unbound && !bus.unbound().empty();
    std::vector<usb::UsbId> present;
    const bool listed = bus.present && bus.present(present);
    bool rtlOnBus = false;
    if (listed) {
        for (const usb::UsbId& p : present) {
            for (const usb::UsbId& w : rtlSdrUsbIds()) {
                if (p.vid == w.vid && p.pid == w.pid) { rtlOnBus = true; }
            }
        }
    }
    return cascade::core::rtlopen::decide(listed, rtlOnBus, anyUnbound);
}

bool RtlSdrSource::open(const std::string& args) { return openOver(args, shippedRtlBusView()); }

bool RtlSdrSource::openOver(const std::string& args, const RtlBusView& bus) {
    std::string wantSerial;
    int wantIndex = -1;
    parseArgs(args, wantSerial, wantIndex);

    const std::vector<usb::UsbDeviceInfo> found = bus.bound();
    if (found.empty()) {
        // THREE DIFFERENT THINGS, three sentences (core/rtl_open_case.hpp): the dongle is on the wrong Windows driver, it is not plugged in, or the bus cannot say.
        // This sentence once said "no RTL-SDR is bound to WinUSB" for all three.
        setError(cascade::core::rtlopen::sentence(noRtlRadioCase(bus)));
        return false;
    }
    const usb::UsbDeviceInfo* pick = nullptr;
    if (!wantSerial.empty()) {
        for (const usb::UsbDeviceInfo& d : found) {
            if (d.serial == wantSerial) {
                pick = &d;
                break;
            }
        }
        if (pick == nullptr) {
            setError("no RTL-SDR with serial " + wantSerial + " is present");
            return false;
        }
    } else if (wantIndex >= 0) {
        if (static_cast<std::size_t>(wantIndex) >= found.size()) {
            setError("there is no RTL-SDR at that index");
            return false;
        }
        pick = &found[static_cast<std::size_t>(wantIndex)];
    } else {
        pick = &found[0];
    }

    std::string error;
    std::unique_ptr<usb::UsbDevice> transport = usb::openWinUsb(pick->path, error);
    if (!transport) {
        setError(error.empty() ? std::string("the radio would not open") : error);
        return false;
    }
    const std::string what = pick->description.empty() ? std::string("RTL-SDR") : pick->description;
    const std::string label =
        pick->serial.empty() ? what : (what + " (serial " + pick->serial + ")");
    const bool opened = openWithTransport(std::move(transport), label);
    if (opened) {
        // ARM THE RE-OPEN for THIS physical radio, by what identifies it: the interface path, the hub-and-port location, and the serial (trusted only when it was
        // the only one of its kind at this open). The lambdas hold copies and nothing of this object (the reader thread outlives it if stop() abandons it). The
        // reader is not running yet.
        RadioIdentity id;
        id.path = pick->path;
        id.location = pick->location;
        id.serial = pick->serial;
        int sameSerial = 0;
        for (const usb::UsbDeviceInfo& d : found) {
            if (!pick->serial.empty() && d.serial == pick->serial) { ++sameSerial; }
        }
        id.serialUnique = !pick->serial.empty() && sameSerial == 1;
        {
            std::lock_guard<std::mutex> wlk(link_->wantedMutex);
            link_->identity.radio = id;
        }
        const RadioListFn list = [] { return usb::enumerateWinUsb(rtlSdrUsbIds()); };
        const RadioOpenFn openFn = [](const std::string& path, std::string& error) { return usb::openWinUsb(path, error); };
        link_->reopen = makeRadioReopenFn(id, list, openFn);
        link_->listed = makeRadioListedFn(id, list);
    }
    return opened;
}

bool RtlSdrSource::openWithTransport(std::unique_ptr<usb::UsbDevice> transport,
                                     const std::string& label) {
    link_->owner = this;
    if (!transport) {
        setError("openWithTransport() needs a transport");
        return false;
    }
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        setError("the radio is busy; try again");
        return false;
    }
    if (deviceDead()) {
        setError("this source has already lost its radio; open a fresh one");
        return false;
    }
    teardownLocked();
    clearError();

    link_->transport = std::move(transport);
    link_->rtl =
        std::make_unique<Rtl2832u>(*link_->transport, static_cast<unsigned>(kControlTimeout.count()));
    link_->demodAgcOn = false;   // a fresh chip
    link_->ring = std::make_unique<dsp::SpscRing<std::complex<float>>>(kRingCapacity);
    {
        std::lock_guard<std::mutex> elk(link_->errorMutex);
        link_->name = "RTL-SDR: " + label;
    }
    if (!bringUpLocked()) {
        teardownLocked();
        return false;
    }
    // WHAT A RE-OPEN WOULD PUT BACK starts from what this open landed on; each setter that succeeds moves its own field.
    link_->reopensThisOpen.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> wlk(link_->wantedMutex);
        link_->wanted = Link::Wanted{};
        link_->wanted.biasTee = link_->biasTee.load(std::memory_order_relaxed);
        link_->wanted.directChoice = directChoice_.load(std::memory_order_relaxed);
        link_->wanted.ifBandwidthHz = ifBandwidthHz_.load(std::memory_order_relaxed);
        link_->wanted.ppm = ppm_.load(std::memory_order_relaxed);
        link_->wantedGen.fetch_add(1, std::memory_order_relaxed);
    }
    openMirror_.store(true, std::memory_order_relaxed);
    core::diagLogf("source: opened %s natively, tuner %s, %.4f MS/s", label.c_str(),
                   link_->tuner->name(), sampleRateHz_.load(std::memory_order_relaxed) / 1e6);
    return true;
}

// THE CHIP BRING-UP, over the link, so the open and the re-open run the very same code (the re-open is on the reader's thread and may not touch the source
// object). The caller holds the device lock and has put the transport and the demodulator object on the link. `out.error` carries the failing step's words.
namespace {
bool bringUpChip(RtlSdrSource::Link& link, int ifBandwidthHz, int directChoice, BringUp& out, const bool* teeForced) {
    Rtl2832u& rtl = *link.rtl;
    auto setError = [&out](std::string msg) { out.error = std::move(msg); };

    // A write that costs nothing and proves the device is answering before
    // the long initialisation sequence starts. A dongle unplugged since
    // enumeration fails HERE, with a clear message, instead of half way
    // through the FIR table.
    if (!rtl.writeReg(RtlBlock::Usb, kRegUsbSysCtl, 0x09, 1)) {
        setError("the radio did not answer its first register write");
        return false;
    }
    if (!rtl.initBaseband()) {
        setError("the demodulator would not initialise: " + rtl.lastError());
        return false;
    }

    // WHO MADE IT. The only thing that distinguishes an RTL-SDR Blog V4 from
    // any other R828D dongle is its USB manufacturer and product strings, and
    // the difference matters enormously: a V4 has an upconverter in front of
    // the tuner and hears nothing below 24 MHz unless it is switched in.
    const std::string manufacturer = rtl.manufacturer();
    const std::string product = rtl.product();
    const bool blogV4 = (manufacturer == "RTLSDRBlog" && product == "Blog V4");

    if (!rtl.setI2cRepeater(true)) {
        setError("the demodulator's I2C repeater would not switch on");
        return false;
    }
    TunerR82xx::Chip chip = TunerR82xx::Chip::R820T;
    std::uint8_t i2cAddr = kR820tI2cAddr;
    if (!TunerR82xx::detect(rtl, chip, i2cAddr)) {
        rtl.setI2cRepeater(false);
        setError("no R820T or R828D tuner answered; this dongle's tuner is not one this "
                 "driver supports yet");
        return false;
    }

    TunerR82xx::Config cfg;
    cfg.chip = chip;
    cfg.i2cAddr = i2cAddr;
    cfg.blogV4 = blogV4;
    // A standard R828D clocks its tuner from its own 16 MHz crystal; a Blog
    // V4 shares the demodulator's 28.8 MHz one. Getting this wrong tunes to
    // the wrong frequency by nearly a factor of two.
    cfg.xtalHz =
        (chip == TunerR82xx::Chip::R828D && !blogV4) ? kR828dXtalHz : rtl.correctedXtalHz();
    // THE IDENTITY, IN THE LOG, because it is the one thing a field report of
    // "the tuner will not lock" cannot be diagnosed without. A Blog V4 is
    // recognised by these two strings and by nothing else, and getting that
    // wrong drives a 28.8 MHz part from a 16 MHz reference and fails every
    // tune. Empty strings are a finding in their own right: they mean the
    // descriptor read gave nothing, not that the dongle is anonymous.
    core::diagLogf("source: usb strings manufacturer \"%s\", product \"%s\"%s; tuner %s from a "
                   "%.4f MHz reference",
                   manufacturer.c_str(), product.c_str(), blogV4 ? " - an RTL-SDR Blog V4" : "",
                   TunerR82xx::chipName(chip), static_cast<double>(cfg.xtalHz) / 1e6);
    link.tuner = std::make_unique<TunerR82xx>(rtl, cfg);
    TunerR82xx& tuner = *link.tuner;
    out.tunerName = tuner.name();
    out.manufacturer = manufacturer;
    out.product = product;
    {
        std::lock_guard<std::mutex> wlk(link.wantedMutex);
        link.tunerNameCached = tuner.name();
        link.blogV4Cached = blogV4;
    }

    // THE R82xx IS NOT A ZERO-IF TUNER. It delivers a 3.57 MHz intermediate
    // frequency, so the demodulator's zero-IF input is switched off, one ADC
    // is used, the down-converter is pointed at the IF, and the spectrum
    // inversion the mixing introduces is undone.
    bool ok = true;
    ok = rtl.demodWriteReg(1, 0xb1, 0x1a, 1) && ok;
    ok = rtl.demodWriteReg(0, 0x08, 0x4d, 1) && ok;
    ok = rtl.setIfFreqHz(kR82xxIfFreqHz) && ok;
    ok = rtl.demodWriteReg(1, 0x15, 0x01, 1) && ok;
    if (!ok) {
        rtl.setI2cRepeater(false);
        setError("the demodulator would not accept its intermediate-frequency settings");
        return false;
    }

    if (!tuner.init()) {
        rtl.setI2cRepeater(false);
        setError("the tuner would not initialise: " + tuner.lastError() + " [" + rtl.lastError() +
                 "]");
        return false;
    }
    rtl.setI2cRepeater(false);

    // THE BIAS TEE, off unless this dongle is wired to force it on. Byte 7
    // bit 1 of the configuration EEPROM clear means the manufacturer tied the
    // bias tee permanently on; anything else and a fresh open leaves 4.5 V
    // off the coax, because a user who does not know it is on can damage a
    // receiver that is not expecting it.
    //
    // AND THE HEADER IS CHECKED FIRST, which the reference driver does not
    // do. An RTL2832U EEPROM begins 0x28 0x32; a dongle with no EEPROM at all
    // answers every byte as zero, and zero has that bit CLEAR - so trusting
    // the byte unconditionally switches 4.5 V onto the antenna of exactly the
    // cheap dongles least likely to survive it.
    //
    // Both facts are kept for the Source panel, which decides whether a
    // remembered "on" may be put back after this open (core/bias_tee.hpp):
    // a dongle with no EEPROM never gets one back.
    std::uint8_t eeprom[8] = {0};
    bool forceBiasTee = false;
    const bool eepromValid =
        rtl.readEeprom(eeprom, 0, sizeof(eeprom)) && eeprom[0] == 0x28 && eeprom[1] == 0x32;
    if (eepromValid) { forceBiasTee = (eeprom[7] & 0x02) == 0; }
    out.eepromValid = eepromValid;
    out.biasForced = forceBiasTee;
    // THE READBACK FOLLOWS THE WRITE, as it does in setBiasT: a dongle that
    // refused the GPIO writes has not switched anything on.
    // (A re-open replaces a FORCED tee's "on" with its own answer, and leaves the bytes of every other dongle exactly as they are.)
    const bool tee = (forceBiasTee && teeForced != nullptr) ? *teeForced : forceBiasTee;
    const bool wrote = rtl.setBiasTee(tee);
    link.biasTee.store(wrote ? tee : false, std::memory_order_relaxed);

    // A rate, a gain and a frequency, so the panel is not looking at a dead
    // device the moment it opens.
    double actual = 0.0;
    if (!rtl.setSampleRate(static_cast<std::uint32_t>(kDefaultRateHz), actual)) {
        setError("the resampler would not accept the default sample rate");
        return false;
    }
    out.rateHz = actual;
    rtl.setI2cRepeater(true);
    tuner.setBandwidthHz(ifBandwidthForRate(ifBandwidthHz, actual));
    tuner.setAggregateGainTenthDb(kDefaultGainTenthDb);
    rtl.setI2cRepeater(false);
    rtl.setIfFreqHz(tuner.ifFreqHz());

    if (!retuneChip(link, kDefaultFreqHz, directChoice)) {
        setError("the tuner would not accept its default frequency");
        return false;
    }
    return true;
}
}  // namespace

bool RtlSdrSource::bringUpLocked() {
    BringUp out;
    const bool ok = bringUpChip(*link_, ifBandwidthHz_.load(std::memory_order_relaxed), directChoice_.load(std::memory_order_relaxed), out);
    if (!ok) {
        setError(out.error);
        return false;
    }
    {
        // WHAT THIS RADIO IS, as read from the device (the path, location and serial are put beside it by open()): a re-open compares against it.
        std::lock_guard<std::mutex> wlk(link_->wantedMutex);
        link_->identity = Link::Identity{};
        link_->identity.tuner = out.tunerName;
        link_->identity.manufacturer = out.manufacturer;
        link_->identity.product = out.product;
    }
    eepromValid_.store(out.eepromValid, std::memory_order_relaxed);
    biasTForced_.store(out.biasForced, std::memory_order_relaxed);
    sampleRateHz_.store(out.rateHz, std::memory_order_relaxed);
    centerFrequencyHz_.store(kDefaultFreqHz, std::memory_order_relaxed);
    return true;
}

void RtlSdrSource::teardownLocked() noexcept {
    if (link_->abandoned.load(std::memory_order_relaxed)) {
        // A reader thread is still inside this device. Nothing may be called
        // and nothing may be freed; the handles leak for the life of the
        // process, deliberately, exactly as SoapySource leaks a wedged vendor
        // device rather than unmaking it under a stranded caller.
        openMirror_.store(false, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
        return;
    }
    stopStreamLocked();
    // The count the per-retune warning no longer prints. Emitted here so that
    // "it would not lock" and "it would not lock 47 times" are distinguishable
    // in a log, which is the whole point of counting instead of repeating.
    const int unlocked = link_->unlockedTunes.exchange(0, std::memory_order_relaxed);
    if (unlocked > 0) {
        core::diagWarnf(
            "source: the tuner's PLL failed to lock on %d tune%s while this radio was open",
            unlocked, unlocked == 1 ? "" : "s");
        rre::noteVendorCallFault(this, "rtlsdr", "pllunlocked", "the tuner's PLL failed to lock on " + std::to_string(unlocked) + " tunes while this radio was open", false);
    }
    if (link_->rtl && link_->tuner) {
        // Power the analogue blocks down on the way out, so a dongle left
        // plugged in is not warming the desk for nothing.
        link_->rtl->setI2cRepeater(true);
        link_->tuner->standby();
        link_->rtl->setI2cRepeater(false);
    }
    if (link_->rtl) { link_->rtl->deinitBaseband(); }
    link_->tuner.reset();
    link_->rtl.reset();
    link_->transport.reset();
    link_->ring.reset();
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    // A closed radio has no bias tee to report on, and a stale "on" here
    // would be read by the next panel that asked.
    link_->biasTee.store(false, std::memory_order_relaxed);
    eepromValid_.store(false, std::memory_order_relaxed);
    biasTForced_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->name = "RTL-SDR: (no device)";
}

void RtlSdrSource::closeDevice() {
    stop();
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        // Once the reader thread is gone, the only thing that can hold this
        // lock is a control call on another thread, and every one of those is
        // bounded. Losing the race means the caller is closing while a setter
        // runs; the device is condemned rather than torn down under it.
        std::lock_guard<std::mutex> elk(link_->errorMutex);
        link_->deviceDead = true;
        link_->faultedWhile = "closing the radio";
        return;
    }
    teardownLocked();
}

// --- streaming --------------------------------------------------------------

bool RtlSdrSource::startStreamLocked() {
    if (!link_->transport || !link_->rtl) { return false; }
    // THE FIFO RESET, and it must be the last thing before the pipe opens.
    // The endpoint FIFO holds whatever the demodulator produced since the
    // last stream; reading it would deliver a buffer that begins halfway
    // through an I/Q pair, and every sample after it would have I and Q
    // swapped.
    if (!link_->rtl->resetBuffer()) {
        setError("the radio would not reset its endpoint before streaming");
        return false;
    }
    if (!link_->transport->beginBulkStream(kBulkEndpoint, kBulkBufferBytes, kBulkBufferCount)) {
        setError("the radio's bulk endpoint would not start: " + link_->transport->lastError());
        return false;
    }
    reportStreamNote(*link_->transport, link_->owner, "rtlsdr");
    return true;
}

void RtlSdrSource::stopStreamLocked() {
    if (link_->transport) { link_->transport->endBulkStream(); }
}

bool RtlSdrSource::start() {
    if (running_.load(std::memory_order_relaxed)) { return true; }
    {
        std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
        // (A radio closed by a stop that landed inside a re-open is latched faulted, so setError below leaves the fault's own words in place: the refusal says
        // "was being reconnected when it was stopped; pick the radio again", not "no radio open" - decision F1.)
        if (!link_->transport || !link_->rtl) {
            setError("start() with no radio open");
            return false;
        }
        if (deviceDead()) {
            setError("this radio has faulted; reopen it");
            return false;
        }
        if (!startStreamLocked()) {
            // A START THAT FAILS ON A DEAD HANDLE IS A FAULT, not only a refusal (decision S3): latched with the words startStreamLocked set, so the window shows them
            // and picking the radio again opens it afresh. (Before this a refused start() left the radio neither working nor marked dead, and the pick did nothing.)
            noteFault(*link_, std::string(lastError()), "starting the stream");
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(link_->healthMutex);
        link_->health = Link::Health{};
        link_->healthEverWritten = false;
    }
    link_->readerBeatNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);   // a stale beat from an earlier stream is not a stuck reader
    link_->readerRun.store(true, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    // The promise the thread satisfies as its last act is what gives stop() a
    // BOUNDED join; std::thread offers none.
    auto exited = std::make_shared<std::promise<void>>();
    readerExit_ = exited->get_future();
    reader_ = std::thread([link = link_, exited] {
        readerLoop(link);
        exited->set_value();
    });
    return true;
}

void RtlSdrSource::stop() {
    if (link_->abandoned.load(std::memory_order_relaxed)) {
        // A reader thread is still inside this device and always will be.
        // Ending its stream from here would abort pipes it is reading and
        // free a ring it is writing; the promise made when it was abandoned
        // was that nothing would touch any of that again.
        running_.store(false, std::memory_order_relaxed);
        return;
    }
    if (!reader_.joinable()) {
        running_.store(false, std::memory_order_relaxed);
        std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
        if (lk.owns_lock()) { stopStreamLocked(); }
        return;
    }
    // THE ORDER MATTERS. The token first and WITHOUT the device lock: the
    // reader thread may be inside a bulk read holding that lock right now,
    // and taking it here before asking the thread to leave would deadlock
    // this call against the very thread it is waiting for.
    link_->readerRun.store(false, std::memory_order_relaxed);

    const bool exited = readerExit_.valid() &&
                        readerExit_.wait_for(kReaderJoinWait) == std::future_status::ready;
    if (exited) {
        reader_.join();
    } else {
        // ABANDONED, and said out loud. The thread is left running inside a
        // transport this object will therefore never touch again: it holds
        // its own copy of the link's shared pointer, so everything it is
        // still reading stays alive, and the link is marked so teardown never
        // frees it. The radio is condemned for the life of the process.
        core::health::raiseRecovered(core::health::Recovered::ReaderLeak);
        core::diagWarnf("source: the RTL-SDR reader thread did not exit within its bound; "
                        "it is abandoned and the radio is condemned");
        reader_.detach();
        {
            std::lock_guard<std::mutex> lk(link_->errorMutex);
            link_->deviceDead = true;
            link_->faulted = true;
            link_->faultedWhile = "stopping the reader thread";
            link_->lastError = "the radio's reader thread would not stop; restart FoxSDR to "
                               "use this radio again";
        }
        link_->abandoned.store(true, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
        return;
    }
    running_.store(false, std::memory_order_relaxed);

    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (lk.owns_lock()) { stopStreamLocked(); }
}

std::size_t RtlSdrSource::read(std::complex<float>* dst, std::size_t n) {
    if (!link_->ring || n == 0) { return 0; }
    // A READER THAT IS PARKED INSIDE A USB CALL FOR GOOD cannot say so; the pipeline's thread, which still comes here every few milliseconds, can.
    // Without this the ring simply ran dry and the radio looked open and fine while it delivered nothing.
    if (running_.load(std::memory_order_relaxed)) {
        const std::int64_t beat = link_->readerBeatNs.load(std::memory_order_relaxed);
        if (beat != 0 && steadyNowNs() - beat >= static_cast<std::int64_t>(link_->readerStuckAfterMs.load(std::memory_order_relaxed)) * 1000000 && !faulted()) {
            noteFault(*link_, "the radio's USB transfer did not return for " + std::to_string(link_->readerStuckAfterMs.load(std::memory_order_relaxed)) + " ms", "reading samples");
        }
    }
    // A short bounded poll rather than an immediate zero: the ring is filled
    // by our own thread at the device's rate, so on a healthy radio the first
    // attempt almost always succeeds, and on an idle one the caller gets the
    // contract's "0, retry" after kReadPollBudget rather than spinning.
    const auto deadline = std::chrono::steady_clock::now() + kReadPollBudget;
    for (;;) {
        std::size_t got = 0;
        {
            // Shared only with a rate change emptying the ring (see
            // Link::ringMutex), so uncontended on every other read.
            std::lock_guard<std::mutex> rlk(link_->ringMutex);
            got = link_->ring->read(dst, n);
        }
        if (got > 0) { return got; }
        if (!running_.load(std::memory_order_relaxed)) { return 0; }
        if (std::chrono::steady_clock::now() >= deadline) { return 0; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// --- rate, frequency, gain --------------------------------------------------

std::vector<double> RtlSdrSource::supportedSampleRatesHz() const {
    return Rtl2832u::supportedRatesHz();
}

bool RtlSdrSource::setSampleRateHz(double hz) {
    if (!(hz > 0.0)) {
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    // COERCE TO THE NEAREST SUPPORTED RATE, as the Soapy path does, rather
    // than refusing: a preset that asks for 2 MS/s should get 2.048, not an
    // error dialogue.
    const double best = nearestSupportedRate(hz);

    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        if (deferWhileReopening(*link_, [best](Link::Wanted& w) { w.rateHz = best; })) {   // the radio is being reopened: kept, and put on with the rest
            sampleRateHz_.store(best, std::memory_order_relaxed);
            return true;
        }
        lk = retakeAfterReopen(*link_);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
    }
    if (!link_->rtl || !link_->tuner) {
        setError("setSampleRateHz() with no radio open");
        return false;
    }
    if (deviceDead()) {
        setError("this radio has faulted; reopen it");
        return false;
    }

    // ON A QUIET STREAM. The resampler is soft-reset as part of the change,
    // and a bulk stream running across that reset delivers a buffer that is
    // half one rate and half the other. The reader thread is meanwhile parked
    // on this very lock, so nothing else is inside the device.
    const bool wasStreaming = link_->transport && link_->transport->streaming();
    if (wasStreaming) { stopStreamLocked(); }

    ChipState st = makeChipState(sampleRateHz_.load(std::memory_order_relaxed), centerFrequencyHz_.load(std::memory_order_relaxed),
                                 ifBandwidthHz_.load(std::memory_order_relaxed), directChoice_.load(std::memory_order_relaxed));
    bool ok = chipRate(*link_, best, st);
    if (ok) {
        sampleRateHz_.store(st.rateHz, std::memory_order_relaxed);
        rememberWanted(*link_, [&st](Link::Wanted& w) { w.rateHz = st.rateHz; });
    } else {
        setError(rre::noteControlRefusedText(this, "rtlsdr", rre::Control::Rate, "the radio refused that sample rate: " + link_->rtl->lastError()));
    }
    if (wasStreaming && !startStreamLocked()) {
        // A STREAM THAT WILL NOT COME BACK IS A FAULT, not merely a failed
        // call. The reader thread wakes to a closed pipe and leaves; without
        // this the source would say running() and not faulted() while
        // delivering nothing, and faulted() is the one thing the pipeline's
        // source loop polls.
        noteFault(*link_, lastError(), "restarting the stream after a rate change");
        ok = false;
    }
    return ok;
}

// THE RETUNE, over the link (shared by the setters and the re-open). `choice` is the user's direct-sampling choice. Does not store the centre-frequency
// mirror: that is the source's, and the caller stores it.
namespace {
bool retuneChip(RtlSdrSource::Link& link, double hz, int choice) {
    if (!link.rtl || !link.tuner) { return false; }
    if (!(hz > 0.0)) { return false; }
    Rtl2832u& rtl = *link.rtl;
    TunerR82xx& tuner = *link.tuner;
    const std::uint32_t target = static_cast<std::uint32_t>(hz + 0.5);

    // DIRECT SAMPLING, and only where it is the right answer. A Blog V4 has
    // an upconverter for HF and must never leave the tuner; any other R82xx
    // dongle has nothing below 24 MHz but the demodulator's own ADC input,
    // which is what the usual direct-sampling modification wires an antenna
    // to.
    // The mode wanted: the user's choice (R11) over the automatic rule. 0 off, 1 the I branch, 2 the Q branch.
    const int wantMode = tuner.blogV4() ? 0 : (choice == 1 ? 0 : (choice == 2 ? 1 : (choice == 3 ? 2 : (hz < kDirectSamplingBelowHz ? 2 : 0))));
    const bool wantDirect = wantMode != 0;
    const bool isDirect = rtl.directSampling() != 0;
    if (wantDirect && isDirect && rtl.directSampling() != wantMode) {
        rtl.setDirectSampling(wantMode);   // the other branch: the tuner is already asleep and the demodulator already set up
    } else if (wantDirect != isDirect) {
        if (wantDirect) {
            rtl.setI2cRepeater(true);
            tuner.standby();
            rtl.setI2cRepeater(false);
            rtl.demodWriteReg(1, 0xb1, 0x1a, 1);
            rtl.demodWriteReg(1, 0x15, 0x00, 1);
            // Mode 2 (the Q branch, where the modification puts it) unless the user chose the I branch.
            rtl.setDirectSampling(wantMode);
        } else {
            rtl.setI2cRepeater(true);
            tuner.init();
            rtl.setI2cRepeater(false);
            rtl.setDirectSampling(0);
            rtl.demodWriteReg(1, 0xb1, 0x1a, 1);
            rtl.demodWriteReg(0, 0x08, 0x4d, 1);
            rtl.setIfFreqHz(tuner.ifFreqHz());
            rtl.demodWriteReg(1, 0x15, 0x01, 1);
        }
    }

    bool ok = true;
    if (rtl.directSampling() != 0) {
        // No mixer in this path: the down-converter IS the tuning.
        ok = rtl.setIfFreqHz(target);
    } else {
        ok = rtl.setI2cRepeater(true) && ok;
        ok = tuner.setFreqHz(target) && ok;
        rtl.setI2cRepeater(false);
        if (ok && !tuner.locked()) {
            // ONCE PER OPEN, WITH A COUNT (see unlockedTunes_). A PLL driven
            // from the wrong reference fails at EVERY frequency, so a line
            // per retune fills the log with the same fact and says nothing
            // about how many - which is exactly what the field report that
            // found this looked like. The one line carries the two facts that
            // separate the two causes: which tuner this is, and what it is
            // being clocked from.
            //
            // TWO LINES, because the log truncates at 192 bytes and the facts
            // must survive that: the identity and the reference first, the
            // advice second.
            if (link.unlockedTunes.fetch_add(1, std::memory_order_relaxed) == 0) {
                // Not WHERE it failed to lock: that is the frequency the user
                // tuned to, which no report may carry (PRIVACY.md). The tuner
                // and the reference are what diagnose it.
                core::diagWarnf("source: the tuner's PLL did not lock at the requested frequency - "
                                "%s from a %.4f MHz reference; deaf there, though samples keep "
                                "flowing",
                                tuner.name(), static_cast<double>(tuner.xtalHz()) / 1e6);
                core::diagWarnf("source: if no frequency locks, close the radio and reopen it - "
                                "a PLL reference that does not match the board fails at every "
                                "frequency. Send this log if it persists.");
                rre::noteVendorCallFault(link.owner, "rtlsdr", "pllunlocked", std::string("the tuner's PLL did not lock at the requested frequency; ") + tuner.name() + " is deaf there", false);
            }
        }
    }
    return ok;
}
}  // namespace

bool RtlSdrSource::retuneLocked(double hz) {
    const bool ok = retuneChip(*link_, hz, directChoice_.load(std::memory_order_relaxed));
    if (ok) { centerFrequencyHz_.store(hz, std::memory_order_relaxed); }
    return ok;
}

bool RtlSdrSource::setCenterFrequencyHz(double hz) {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        if (hz > 0.0 && deferWhileReopening(*link_, [hz](Link::Wanted& w) { w.freqHz = hz; })) {   // the radio is being reopened: kept, and tuned to when it is back
            centerFrequencyHz_.store(hz, std::memory_order_relaxed);
            return true;
        }
        lk = retakeAfterReopen(*link_);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
    }
    if (!link_->rtl || !link_->tuner) {
        setError("setCenterFrequencyHz() with no radio open");
        return false;
    }
    if (deviceDead()) {
        setError("this radio has faulted; reopen it");
        return false;
    }
    double lo = 0.0;
    double hi = 0.0;
    if (rangeLocked(lo, hi) && (hz < lo || hz > hi)) {
        char buf[176];
        std::snprintf(buf, sizeof(buf),
                      "this radio tunes %.3f - %.3f MHz; %.3f MHz is outside that", lo / 1e6,
                      hi / 1e6, hz / 1e6);
        rre::noteControlRefused(this, "rtlsdr", rre::Control::Frequency, "outside the range this radio can tune", /*userRange=*/true);
        setError(buf);
        return false;
    }
    if (!retuneLocked(hz)) {
        setError(rre::noteControlRefusedText(this, "rtlsdr", rre::Control::Frequency, "the radio would not tune there: " + link_->tuner->lastError() + " [" +
                 link_->rtl->lastError() + "]"));
        return false;
    }
    rememberWanted(*link_, [hz](Link::Wanted& w) { w.freqHz = hz; });
    return true;
}

// THE ANALOGUE IF BANDWIDTH (G1-14). 0 is automatic: the filter follows the sample rate exactly as it always did (ifBandwidthFor(rate) is then the rate itself, the value every call site passed before this setter existed).
int RtlSdrSource::ifBandwidthFor(double rateHz) const {
    const int user = ifBandwidthHz_.load(std::memory_order_relaxed);
    return user > 0 ? user : static_cast<int>(rateHz);
}

bool RtlSdrSource::setIfBandwidthHz(int hz) {
    if (hz != 0 && (hz < kIfBandwidthMinHz || hz > kIfBandwidthMaxHz)) {
        setError(rre::noteControlRefusedText(this, "rtlsdr", rre::Control::Bandwidth, "the analogue IF bandwidth is 0 (automatic) or 300 kHz to 8 MHz", /*userRange=*/true));
        return false;
    }
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        if (deferWhileReopening(*link_, [hz](Link::Wanted& w) { w.ifBandwidthHz = hz; w.ifBandwidthSet = true; })) {   // the radio is being reopened: kept, and put on with the rest
            ifBandwidthHz_.store(hz, std::memory_order_relaxed);
            return true;
        }
        lk = retakeAfterReopen(*link_);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
    }
    if (!link_->rtl || !link_->tuner) {
        setError("setIfBandwidthHz() with no radio open");
        return false;
    }
    if (deviceDead()) {
        setError("this radio has faulted; reopen it");
        return false;
    }
    ifBandwidthHz_.store(hz, std::memory_order_relaxed);
    // The IF the filter settles on moves with the bandwidth, and the demodulator's down-converter has to follow it (as after a rate change), then the tuner is put through its tune again.
    ChipState st = makeChipState(sampleRateHz_.load(std::memory_order_relaxed), centerFrequencyHz_.load(std::memory_order_relaxed), hz, directChoice_.load(std::memory_order_relaxed));
    if (!chipIfBandwidth(*link_, hz, st)) {
        setError(rre::noteControlRefusedText(this, "rtlsdr", rre::Control::Bandwidth, "the tuner would not accept that IF bandwidth: " + link_->tuner->lastError()));
        return false;
    }
    rememberWanted(*link_, [hz](Link::Wanted& w) { w.ifBandwidthHz = hz; w.ifBandwidthSet = true; });
    return true;
}

std::uint32_t RtlSdrSource::rtlTcpTunerCode() const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock() || !link_->tuner) { return 0; }
    switch (link_->tuner->chip()) {
        case TunerR82xx::Chip::R820T: return 5;
        case TunerR82xx::Chip::R828D: return 6;
    }
    return 0;
}

std::uint32_t RtlSdrSource::tunerGainSteps() const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock() || !link_->tuner) { return 0; }
    // THE NUMBER AN rtl_tcp SERVER REPORTS FOR AN R820T OR R828D: 29 (the vendor library's gain table for these tuners; `rtl_test -t` on the real R820T of the 2026-11-17 check
    // listed 29 values, 0.0 to 49.6 dB). The driver's own aggregate ladder (TunerR82xx::aggregateLadderTenthDb) has 30 rungs because it also counts its own starting point, so it is not what a
    // client that sizes a gain list by this number expects.
    return 29;
}

bool RtlSdrSource::directSamplingSupported() const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    return lk.owns_lock() && link_->tuner && !link_->tuner->blogV4();
}

int RtlSdrSource::directSamplingMode() const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock() && link_->reopening.load(std::memory_order_acquire)) {
        const int c = wantedCopy(*link_).directChoice;
        return c == 2 ? 1 : (c == 3 ? 2 : 0);
    }
    return (lk.owns_lock() && link_->rtl) ? link_->rtl->directSampling() : 0;
}

bool RtlSdrSource::setDirectSamplingChoice(int choice) {
    if (choice < 0 || choice > 3) { return false; }
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        // the radio is being reopened: kept, and put on with the rest (a Blog V4 never leaves its tuner, so it is refused here as it is on an open radio)
        bool v4 = false;
        {
            std::lock_guard<std::mutex> wlk(link_->wantedMutex);
            v4 = link_->blogV4Cached;
        }
        if (choice != 0 && v4 && link_->reopening.load(std::memory_order_acquire)) { return false; }
        if (deferWhileReopening(*link_, [choice](Link::Wanted& w) { w.directChoice = choice; w.directChoiceSet = true; })) {
            directChoice_.store(choice, std::memory_order_relaxed);
            return true;
        }
        lk = retakeAfterReopen(*link_);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
    }
    if (!link_->rtl || !link_->tuner) {
        setError("setDirectSamplingChoice() with no radio open");
        return false;
    }
    if (choice != 0 && link_->tuner->blogV4()) {
        setError("this radio has an upconverter for HF and never leaves its tuner: direct sampling is not for it");
        return false;
    }
    directChoice_.store(choice, std::memory_order_relaxed);
    rememberWanted(*link_, [choice](Link::Wanted& w) { w.directChoice = choice; w.directChoiceSet = true; });
    // Where the radio is now, put through the rule again: forced on, it goes direct (when that is inside the first Nyquist zone); off or automatic, back to the tuner.
    ChipState st = makeChipState(sampleRateHz_.load(std::memory_order_relaxed), centerFrequencyHz_.load(std::memory_order_relaxed), ifBandwidthHz_.load(std::memory_order_relaxed), 0);
    if (!chipDirect(*link_, choice, st)) { setError(rre::noteControlRefusedText(this, "rtlsdr", rre::Control::DirectSampling, "the radio would not retune after the direct-sampling change")); return false; }
    return true;
}

bool RtlSdrSource::rangeLocked(double& loHz, double& hiHz) const {
    if (!link_->tuner) { return false; }
    loHz = link_->tuner->blogV4() ? kBlogV4LoHz : kR82xxLoHz;
    hiHz = kR82xxHiHz;
    const int choice = directChoice_.load(std::memory_order_relaxed);
    if ((choice == 2 || choice == 3) && !link_->tuner->blogV4()) {
        loHz = kDirectLoHz;   // a forced direct-sampling radio hears the HF band the tuner cannot (R11)
        hiHz = kDirectHiHz;
    }
    return true;
}

bool RtlSdrSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) { return false; }
    return rangeLocked(loHz, hiHz);
}

std::string RtlSdrSource::tunerName() const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock() && link_->reopening.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> wlk(link_->wantedMutex);
        return link_->tunerNameCached;
    }
    if (!lk.owns_lock() || !link_->tuner) { return std::string(); }
    return link_->tuner->name();
}

std::vector<GainInfo> RtlSdrSource::gains() const {
    std::vector<GainInfo> out;
    int lo = 0;
    int hi = 0;
    TunerR82xx::aggregateRangeTenthDb(lo, hi);
    out.push_back({"TUNER", lo / 10.0, hi / 10.0, 0.1});
    const std::pair<const char*, TunerR82xx::Stage> stages[] = {
        {"LNA", TunerR82xx::Stage::Lna},
        {"MIXER", TunerR82xx::Stage::Mixer},
        {"VGA", TunerR82xx::Stage::Vga},
    };
    for (const auto& s : stages) {
        TunerR82xx::stageRangeTenthDb(s.second, lo, hi);
        out.push_back({s.first, lo / 10.0, hi / 10.0, 0.1});
    }
    return out;
}

bool RtlSdrSource::setGainDb(const std::string& name, double db) {
    const int tenths = static_cast<int>(std::lround(db * 10.0));
    // Which entry of Link::Wanted a gain name is: 0 aggregate, 1 to 3 the stages; -1 for a name this radio does not have.
    const int slot = name == "TUNER" ? 0 : name == "LNA" ? 1 : name == "MIXER" ? 2 : name == "VGA" ? 3 : -1;
    auto remember = [this, slot, tenths](bool measured) {
        // A stage gain is remembered with the OTHER stages as they stand (read off the tuner under the lock), so putting it back reproduces the whole ladder.
        int stageNow[3] = {tenths, tenths, tenths};
        if (measured && slot > 0 && link_->tuner) {
            const TunerR82xx::Stage stages[3] = {TunerR82xx::Stage::Lna, TunerR82xx::Stage::Mixer, TunerR82xx::Stage::Vga};
            for (int s = 0; s < 3; ++s) { stageNow[s] = TunerR82xx::stageGainTenthDb(stages[s], link_->tuner->stageIndex(stages[s])); }
        }
        rememberWanted(*link_, [slot, tenths, measured, stageNow](Link::Wanted& w) {
            w.gainSet = true;
            if (slot == 0) {
                int lo = 0;
                int hi = 0;
                TunerR82xx::aggregateRangeTenthDb(lo, hi);
                w.gainByStage = false;
                w.aggregateTenthDb = std::min(std::max(tenths, lo), hi);
            } else {
                w.gainByStage = true;
                if (measured) {
                    for (int s = 0; s < 3; ++s) {
                        w.stageTenthDb[s] = stageNow[s];
                        w.stageSet[s] = true;
                    }
                } else {
                    w.stageTenthDb[slot - 1] = tenths;
                    w.stageSet[slot - 1] = true;
                }
            }
        });
    };
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        if (slot >= 0 && deferWhileReopening(*link_, [slot, tenths](Link::Wanted& w) {   // the radio is being reopened: kept, and put on with the rest
                w.gainSet = true;
                if (w.autoGain) { w.autoGainSet = true; w.autoGain = false; }   // a manual write ends automatic gain here too (review A2): else the re-open put automatic gain back and skipped this gain (applyWanted)
                if (slot == 0) {
                    int lo = 0;
                    int hi = 0;
                    TunerR82xx::aggregateRangeTenthDb(lo, hi);
                    w.gainByStage = false;
                    w.aggregateTenthDb = std::min(std::max(tenths, lo), hi);
                } else {
                    w.gainByStage = true;
                    w.stageTenthDb[slot - 1] = tenths;
                    w.stageSet[slot - 1] = true;
                }
            })) {
            return true;
        }
        lk = retakeAfterReopen(*link_);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
    }
    if (!link_->rtl || !link_->tuner) {
        setError("setGainDb() with no radio open");
        return false;
    }
    if (deviceDead()) {
        setError("this radio has faulted; reopen it");
        return false;
    }
    // Automatic gain in force before this write: a manual write ends it (chipGain takes the tuner and the demodulator off automatic), and what the re-open puts back must say manual too.
    const bool wasAuto = link_->tuner->autoGain() || link_->demodAgcOn;
    bool known = true;
    const bool ok = chipGain(*link_, name, tenths, known);
    if (!known) {
        setError("this radio has no gain called " + name);
        return false;
    }
    if (ok) {
        remember(true);
        if (wasAuto) { rememberWanted(*link_, [](Link::Wanted& w) { w.autoGainSet = true; w.autoGain = false; }); }
    }
    if (!ok) { setError(rre::noteControlRefusedText(this, "rtlsdr", rre::Control::Gain, "the tuner refused that gain: " + link_->tuner->lastError())); }
    return ok;
}

double RtlSdrSource::gainDb(const std::string& name) const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock() && link_->reopening.load(std::memory_order_acquire)) {
        // THE RADIO IS BEING REOPENED: answer with what it will be put back to, not with a closed radio's zero (a panel that read zero would write it back).
        const Link::Wanted w = wantedCopy(*link_);
        if (name == "TUNER") { return (w.gainSet && !w.gainByStage ? w.aggregateTenthDb : kDefaultGainTenthDb) / 10.0; }
        const int s = name == "LNA" ? 0 : name == "MIXER" ? 1 : name == "VGA" ? 2 : -1;
        return (s >= 0 && w.stageSet[s]) ? w.stageTenthDb[s] / 10.0 : 0.0;
    }
    if (!lk.owns_lock() || !link_->tuner) { return 0.0; }
    const TunerR82xx& tuner = *link_->tuner;
    if (name == "TUNER") { return tuner.aggregateGainTenthDb() / 10.0; }
    if (name == "LNA") {
        return TunerR82xx::stageGainTenthDb(TunerR82xx::Stage::Lna,
                                            tuner.stageIndex(TunerR82xx::Stage::Lna)) /
               10.0;
    }
    if (name == "MIXER") {
        return TunerR82xx::stageGainTenthDb(TunerR82xx::Stage::Mixer,
                                            tuner.stageIndex(TunerR82xx::Stage::Mixer)) /
               10.0;
    }
    if (name == "VGA") {
        return TunerR82xx::stageGainTenthDb(TunerR82xx::Stage::Vga,
                                            tuner.stageIndex(TunerR82xx::Stage::Vga)) /
               10.0;
    }
    return 0.0;
}

bool RtlSdrSource::setAutoGain(bool on) {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        if (deferWhileReopening(*link_, [on](Link::Wanted& w) { w.autoGainSet = true; w.autoGain = on; })) { return true; }   // the radio is being reopened: kept, and put on with the rest
        lk = retakeAfterReopen(*link_);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
    }
    if (!link_->rtl || !link_->tuner) {
        setError("setAutoGain() with no radio open");
        return false;
    }
    if (deviceDead()) {
        setError("this radio has faulted; reopen it");
        return false;
    }
    const bool ok = chipAutoGain(*link_, on);
    if (ok) { rememberWanted(*link_, [on](Link::Wanted& w) { w.autoGainSet = true; w.autoGain = on; }); }
    if (!ok) { setError(rre::noteControlRefusedText(this, "rtlsdr", rre::Control::AutoGain, "the tuner refused the gain mode: " + link_->tuner->lastError())); }
    return ok;
}

bool RtlSdrSource::autoGain() const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock() && link_->reopening.load(std::memory_order_acquire)) { return wantedCopy(*link_).autoGain; }
    return lk.owns_lock() && link_->tuner && link_->tuner->autoGain();
}

// --- antennas ---------------------------------------------------------------

std::vector<std::string> RtlSdrSource::antennas() const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock() && link_->reopening.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> wlk(link_->wantedMutex);
        if (link_->blogV4Cached) { return {"RX", "HF"}; }
        return {"RX"};
    }
    if (lk.owns_lock() && link_->tuner && link_->tuner->blogV4()) { return {"RX", "HF"}; }
    return {"RX"};
}

std::string RtlSdrSource::antenna() const {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    bool v4 = lk.owns_lock() && link_->tuner && link_->tuner->blogV4();
    if (!lk.owns_lock() && link_->reopening.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> wlk(link_->wantedMutex);
        v4 = link_->blogV4Cached;
    }
    if (v4 && centerFrequencyHz_.load(std::memory_order_relaxed) <= kBlogV4UpconvertHz) {
        return "HF";
    }
    return "RX";
}

bool RtlSdrSource::setAntenna(const std::string& name) {
    // A READOUT WEARING A SETTER, and it says so rather than pretending. The
    // V4's input switch follows the frequency - there is no register that
    // selects HF independently of tuning below 28.8 MHz - so accepting "HF"
    // while tuned to 100 MHz would be a control that silently does nothing.
    const std::vector<std::string> list = antennas();
    if (std::find(list.begin(), list.end(), name) == list.end()) {
        setError("this radio has no antenna port called " + name);
        return false;
    }
    if (name == antenna()) { return true; }
    setError("on this radio the antenna port follows the frequency: tune below 28.8 MHz for "
             "the HF input");
    return false;
}

// --- extras -----------------------------------------------------------------

bool RtlSdrSource::biasT() const {
    if (link_->reopening.load(std::memory_order_acquire)) { return wantedCopy(*link_).biasTee; }
    return link_->biasTee.load(std::memory_order_relaxed);
}

bool RtlSdrSource::setBiasT(bool on) {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        // the radio is being reopened: kept, and switched when it is back (the re-open may withhold an ON, and says so); the readback answers from it meanwhile
        if (deferWhileReopening(*link_, [on](Link::Wanted& w) { w.biasTee = on; })) { return true; }
        lk = retakeAfterReopen(*link_);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
    }
    if (!link_->rtl) {
        setError("the radio is busy; try again");
        return false;
    }
    if (!link_->rtl->setBiasTee(on)) {
        setError(rre::noteControlRefusedText(this, "rtlsdr", rre::Control::BiasTee, "the radio would not switch its bias tee"));
        return false;
    }
    link_->biasTee.store(on, std::memory_order_relaxed);
    rememberWanted(*link_, [on](Link::Wanted& w) { w.biasTee = on; });
    return true;
}

bool RtlSdrSource::setFreqCorrectionPpm(int ppm) {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock()) {
        if (deferWhileReopening(*link_, [ppm](Link::Wanted& w) { w.ppm = ppm; w.ppmSet = true; })) {   // the radio is being reopened: kept, and put on with the rest
            ppm_.store(ppm, std::memory_order_relaxed);
            return true;
        }
        lk = retakeAfterReopen(*link_);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
    }
    if (!link_->rtl || !link_->tuner) {
        setError("the radio is busy; try again");
        return false;
    }
    // The crystal correction reaches the chip's reference and the tuner's (a standard R828D keeps its own crystal), and the current frequency is
    // re-derived from it, or the trim only takes effect on the next tune.
    ChipState st = makeChipState(sampleRateHz_.load(std::memory_order_relaxed), centerFrequencyHz_.load(std::memory_order_relaxed), ifBandwidthHz_.load(std::memory_order_relaxed), directChoice_.load(std::memory_order_relaxed));
    if (!link_->rtl->setFreqCorrectionPpm(ppm)) {
        setError(rre::noteControlRefusedText(this, "rtlsdr", rre::Control::Ppm, "the radio would not accept a crystal correction"));
        return false;
    }
    ppm_.store(ppm, std::memory_order_relaxed);
    rememberWanted(*link_, [ppm](Link::Wanted& w) { w.ppm = ppm; w.ppmSet = true; });
    if (!(link_->tuner->chip() == TunerR82xx::Chip::R828D && !link_->tuner->blogV4())) {
        link_->tuner->setXtalHz(link_->rtl->correctedXtalHz());
    }
    const bool ok = retuneChip(*link_, st.centerHz, st.directChoice);
    if (ok) { centerFrequencyHz_.store(st.centerHz, std::memory_order_relaxed); }
    return ok;
}

bool RtlSdrSource::setFrequencyCorrectionPpm(double ppm) {
    return setFreqCorrectionPpm(cascade::core::ppmWholeForRadio(ppm));
}

// --- stream health ----------------------------------------------------------

std::string RtlSdrSource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void RtlSdrSource::setStallBoundsForTest(int silenceRearmAfterMs, int readerStuckAfterMs) {
    link_->silenceRearmAfterMs.store(silenceRearmAfterMs, std::memory_order_relaxed);
    link_->readerStuckAfterMs.store(readerStuckAfterMs, std::memory_order_relaxed);
}

void RtlSdrSource::setReopenForTest(ReopenFn fn, std::function<int()> listed) {
    link_->reopen = std::move(fn);
    link_->listed = std::move(listed);
}

void RtlSdrSource::setBeforeDeferForTest(std::function<void()> fn) { link_->beforeDeferForTest = std::move(fn); }

void RtlSdrSource::setRetakeProbeForTest(std::function<void(int)> fn) { link_->retakeProbeForTest = std::move(fn); }

void RtlSdrSource::setReopenBoundsForTest(int waitMs, int pollMs, int forgiveMs, int bringUpMs) {
    link_->reopenForgiveAfterMs.store(forgiveMs, std::memory_order_relaxed);
    link_->reopenBringUpMs.store(bringUpMs, std::memory_order_relaxed);
    link_->reopenWaitMs.store(waitMs, std::memory_order_relaxed);
    link_->reopenPollMs.store(pollMs, std::memory_order_relaxed);
}

bool RtlSdrSource::invalidateTransportForTest() {
    std::unique_lock<std::timed_mutex> lk = controlLock(*link_);
    if (!lk.owns_lock() || !link_->transport) { return false; }
    return link_->transport->invalidateForTest();
}

void RtlSdrSource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

}  // namespace cascade::source
