// See rtlsdr_source.hpp for the shape, the threading contract and the link.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rtlsdr_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "core/diag_log.hpp"

namespace cascade::source {

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

// ---------------------------------------------------------------------------

// The bulk ring. 16 KB is 32 maximum-size packets, which at 2.4 MS/s is a
// buffer every 3.4 ms - small enough that the spectrum is not a slideshow,
// large enough that WinUSB is not completing a request every packet. Sixteen
// of them is about 55 ms of queue, which is what rides out a GUI stall.
constexpr std::size_t kBulkBufferBytes = 16384;
constexpr std::size_t kBulkBufferCount = 16;

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
        line = healthLineLocked(link);
        if (line.empty()) { return; }
        if (worrying || first || !nominal) { link.healthEverWritten = true; }
    }
    // Same policy and the same words as SoapySource: the first window after a
    // start is always written, so a healthy radio leaves a line proving it,
    // and after that only a window with something to say.
    if (worrying) {
        core::diagWarnf("%s", line.c_str());
    } else if (first || !nominal) {
        core::diagLogf("%s", line.c_str());
    }
}

void noteFault(RtlSdrSource::Link& link, std::string msg, const char* what) {
    {
        std::lock_guard<std::mutex> lk(link.errorMutex);
        link.lastError = std::move(msg);
        link.faulted = true;
        link.deviceDead = true;
        link.faultedWhile = what;
    }
    core::diagWarnf("source: the RTL-SDR failed while %s", what);
}

// THE READER THREAD. A free function taking the link by shared pointer, so an
// abandoned thread reads memory that is still alive - see the header.
void readerLoop(std::shared_ptr<RtlSdrSource::Link> link) {
    std::vector<std::uint8_t> raw(kBulkBufferBytes);
    std::vector<std::complex<float>> conv(kBulkBufferBytes / 2);
    while (link->readerRun.load(std::memory_order_relaxed)) {
        int got = 0;
        {
            std::unique_lock<std::timed_mutex> lk(link->mutex, kDeviceLockWait);
            if (!lk.owns_lock()) {
                // A control call is inside the device right now. Not an error
                // and not this thread's business: come back.
                continue;
            }
            if (!link->readerRun.load(std::memory_order_relaxed)) { break; }
            if (!link->transport || !link->transport->streaming()) { break; }
            got = link->transport->readBulk(raw.data(), raw.size(),
                                            static_cast<unsigned>(kBulkReadTimeout.count()));
        }
        if (!link->readerRun.load(std::memory_order_relaxed)) { break; }
        noteRead(*link, got, got > 0 ? static_cast<std::size_t>(got) / 2u : 0u);
        if (got < 0) {
            // The device is gone, or the pipe has failed. faulted() is what
            // the pipeline's source loop polls; setting it and leaving is the
            // whole of the recovery, because there is none.
            noteFault(*link, "the radio stopped delivering samples (it may have been "
                             "unplugged)",
                      "reading samples");
            break;
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
const std::vector<usb::UsbId>& rtlSdrUsbIds() {
    static const std::vector<usb::UsbId> ids = {
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
    };
    return ids;
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

bool RtlSdrSource::open(const std::string& args) {
    std::string wantSerial;
    int wantIndex = -1;
    parseArgs(args, wantSerial, wantIndex);

    const std::vector<usb::UsbDeviceInfo> found = usb::enumerateWinUsb(rtlSdrUsbIds());
    if (found.empty()) {
        setError("no RTL-SDR is bound to WinUSB on this machine");
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
    return openWithTransport(std::move(transport), label);
}

bool RtlSdrSource::openWithTransport(std::unique_ptr<usb::UsbDevice> transport,
                                     const std::string& label) {
    if (!transport) {
        setError("openWithTransport() needs a transport");
        return false;
    }
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
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
    link_->ring = std::make_unique<dsp::SpscRing<std::complex<float>>>(kRingCapacity);
    {
        std::lock_guard<std::mutex> elk(link_->errorMutex);
        link_->name = "RTL-SDR: " + label;
    }
    if (!bringUpLocked()) {
        teardownLocked();
        return false;
    }
    openMirror_.store(true, std::memory_order_relaxed);
    core::diagLogf("source: opened %s natively, tuner %s, %.4f MS/s", label.c_str(),
                   link_->tuner->name(), sampleRateHz_.load(std::memory_order_relaxed) / 1e6);
    return true;
}

bool RtlSdrSource::bringUpLocked() {
    Rtl2832u& rtl = *link_->rtl;

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
    link_->tuner = std::make_unique<TunerR82xx>(rtl, cfg);
    TunerR82xx& tuner = *link_->tuner;

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
        setError("the tuner would not initialise: " + tuner.lastError());
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
    std::uint8_t eeprom[8] = {0};
    bool forceBiasTee = false;
    if (rtl.readEeprom(eeprom, 0, sizeof(eeprom)) && eeprom[0] == 0x28 && eeprom[1] == 0x32) {
        forceBiasTee = (eeprom[7] & 0x02) == 0;
    }
    rtl.setBiasTee(forceBiasTee);
    biasTee_.store(forceBiasTee, std::memory_order_relaxed);

    // A rate, a gain and a frequency, so the panel is not looking at a dead
    // device the moment it opens.
    double actual = 0.0;
    if (!rtl.setSampleRate(static_cast<std::uint32_t>(kDefaultRateHz), actual)) {
        setError("the resampler would not accept the default sample rate");
        return false;
    }
    sampleRateHz_.store(actual, std::memory_order_relaxed);
    rtl.setI2cRepeater(true);
    tuner.setBandwidthHz(static_cast<int>(actual));
    tuner.setAggregateGainTenthDb(kDefaultGainTenthDb);
    rtl.setI2cRepeater(false);
    rtl.setIfFreqHz(tuner.ifFreqHz());

    if (!retuneLocked(kDefaultFreqHz)) {
        setError("the tuner would not accept its default frequency");
        return false;
    }
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
    const int unlocked = unlockedTunes_.exchange(0, std::memory_order_relaxed);
    if (unlocked > 0) {
        core::diagWarnf(
            "source: the tuner's PLL failed to lock on %d tune%s while this radio was open",
            unlocked, unlocked == 1 ? "" : "s");
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
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->name = "RTL-SDR: (no device)";
}

void RtlSdrSource::closeDevice() {
    stop();
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
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
    return true;
}

void RtlSdrSource::stopStreamLocked() {
    if (link_->transport) { link_->transport->endBulkStream(); }
}

bool RtlSdrSource::start() {
    if (running_.load(std::memory_order_relaxed)) { return true; }
    {
        std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
        if (!lk.owns_lock()) {
            setError("the radio is busy; try again");
            return false;
        }
        if (!link_->transport || !link_->rtl) {
            setError("start() with no radio open");
            return false;
        }
        if (deviceDead()) {
            setError("this radio has faulted; reopen it");
            return false;
        }
        if (!startStreamLocked()) { return false; }
    }
    {
        std::lock_guard<std::mutex> lk(link_->healthMutex);
        link_->health = Link::Health{};
        link_->healthEverWritten = false;
    }
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
        std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
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

    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (lk.owns_lock()) { stopStreamLocked(); }
}

std::size_t RtlSdrSource::read(std::complex<float>* dst, std::size_t n) {
    if (!link_->ring || n == 0) { return 0; }
    // A short bounded poll rather than an immediate zero: the ring is filled
    // by our own thread at the device's rate, so on a healthy radio the first
    // attempt almost always succeeds, and on an idle one the caller gets the
    // contract's "0, retry" after kReadPollBudget rather than spinning.
    const auto deadline = std::chrono::steady_clock::now() + kReadPollBudget;
    for (;;) {
        const std::size_t got = link_->ring->read(dst, n);
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
    const std::vector<double> rates = Rtl2832u::supportedRatesHz();
    double best = rates.front();
    for (const double r : rates) {
        if (std::fabs(r - hz) < std::fabs(best - hz)) { best = r; }
    }

    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (!lk.owns_lock()) {
        setError("the radio is busy; try again");
        return false;
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

    double actual = 0.0;
    bool ok = link_->rtl->setSampleRate(static_cast<std::uint32_t>(best + 0.5), actual);
    if (ok) {
        sampleRateHz_.store(actual, std::memory_order_relaxed);
        // The tuner's IF filter follows the rate, and the IF it settles on
        // has to be fed back to the demodulator's down-converter or the whole
        // spectrum sits at the wrong offset.
        link_->rtl->setI2cRepeater(true);
        link_->tuner->setBandwidthHz(static_cast<int>(actual));
        link_->rtl->setI2cRepeater(false);
        link_->rtl->setIfFreqHz(link_->tuner->ifFreqHz());
        retuneLocked(centerFrequencyHz_.load(std::memory_order_relaxed));
    } else {
        setError("the radio refused that sample rate: " + link_->rtl->lastError());
    }
    if (wasStreaming) {
        if (!startStreamLocked()) { ok = false; }
    }
    return ok;
}

bool RtlSdrSource::retuneLocked(double hz) {
    if (!link_->rtl || !link_->tuner) { return false; }
    if (!(hz > 0.0)) { return false; }
    Rtl2832u& rtl = *link_->rtl;
    TunerR82xx& tuner = *link_->tuner;
    const std::uint32_t target = static_cast<std::uint32_t>(hz + 0.5);

    // DIRECT SAMPLING, and only where it is the right answer. A Blog V4 has
    // an upconverter for HF and must never leave the tuner; any other R82xx
    // dongle has nothing below 24 MHz but the demodulator's own ADC input,
    // which is what the usual direct-sampling modification wires an antenna
    // to.
    const bool wantDirect = !tuner.blogV4() && hz < kDirectSamplingBelowHz;
    const bool isDirect = rtl.directSampling() != 0;
    if (wantDirect != isDirect) {
        if (wantDirect) {
            rtl.setI2cRepeater(true);
            tuner.standby();
            rtl.setI2cRepeater(false);
            rtl.demodWriteReg(1, 0xb1, 0x1a, 1);
            rtl.demodWriteReg(1, 0x15, 0x00, 1);
            // Mode 2: the Q branch, which is where the modification puts it.
            rtl.setDirectSampling(2);
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
            if (unlockedTunes_.fetch_add(1, std::memory_order_relaxed) == 0) {
                core::diagWarnf("source: the tuner's PLL did not lock at %.4f MHz - %s from a "
                                "%.4f MHz reference; deaf there, though samples keep flowing",
                                hz / 1e6, tuner.name(),
                                static_cast<double>(tuner.xtalHz()) / 1e6);
                core::diagWarnf("source: if no frequency locks, close the radio and reopen it - "
                                "a PLL reference that does not match the board fails at every "
                                "frequency. Send this log if it persists.");
            }
        }
    }
    if (ok) { centerFrequencyHz_.store(hz, std::memory_order_relaxed); }
    return ok;
}

bool RtlSdrSource::setCenterFrequencyHz(double hz) {
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (!lk.owns_lock()) {
        setError("the radio is busy; try again");
        return false;
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
        setError(buf);
        return false;
    }
    if (!retuneLocked(hz)) {
        setError("the radio would not tune there: " + link_->tuner->lastError());
        return false;
    }
    return true;
}

bool RtlSdrSource::rangeLocked(double& loHz, double& hiHz) const {
    if (!link_->tuner) { return false; }
    loHz = link_->tuner->blogV4() ? kBlogV4LoHz : kR82xxLoHz;
    hiHz = kR82xxHiHz;
    return true;
}

bool RtlSdrSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (!lk.owns_lock()) { return false; }
    return rangeLocked(loHz, hiHz);
}

std::string RtlSdrSource::tunerName() const {
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
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
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (!lk.owns_lock()) {
        setError("the radio is busy; try again");
        return false;
    }
    if (!link_->rtl || !link_->tuner) {
        setError("setGainDb() with no radio open");
        return false;
    }
    if (deviceDead()) {
        setError("this radio has faulted; reopen it");
        return false;
    }
    const int tenths = static_cast<int>(std::lround(db * 10.0));
    bool ok = false;
    link_->rtl->setI2cRepeater(true);
    if (name == "TUNER") {
        int lo = 0;
        int hi = 0;
        TunerR82xx::aggregateRangeTenthDb(lo, hi);
        // OUT OF RANGE IS CLAMPED, not refused - the DeviceSource contract.
        ok = link_->tuner->setAggregateGainTenthDb(std::min(std::max(tenths, lo), hi));
    } else if (name == "LNA" || name == "MIXER" || name == "VGA") {
        const TunerR82xx::Stage stage = (name == "LNA")     ? TunerR82xx::Stage::Lna
                                        : (name == "MIXER") ? TunerR82xx::Stage::Mixer
                                                            : TunerR82xx::Stage::Vga;
        ok = link_->tuner->setStageIndex(stage, TunerR82xx::nearestStageIndex(stage, tenths));
    } else {
        link_->rtl->setI2cRepeater(false);
        setError("this radio has no gain called " + name);
        return false;
    }
    link_->rtl->setI2cRepeater(false);
    if (!ok) { setError("the tuner refused that gain: " + link_->tuner->lastError()); }
    return ok;
}

double RtlSdrSource::gainDb(const std::string& name) const {
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
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
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (!lk.owns_lock()) {
        setError("the radio is busy; try again");
        return false;
    }
    if (!link_->rtl || !link_->tuner) {
        setError("setAutoGain() with no radio open");
        return false;
    }
    if (deviceDead()) {
        setError("this radio has faulted; reopen it");
        return false;
    }
    link_->rtl->setI2cRepeater(true);
    const bool ok = link_->tuner->setAutoGain(on);
    link_->rtl->setI2cRepeater(false);
    // The demodulator's own digital AGC follows the tuner's, so the two
    // cannot end up fighting each other over the same signal.
    link_->rtl->setDemodAgc(on);
    if (!ok) { setError("the tuner refused the gain mode: " + link_->tuner->lastError()); }
    return ok;
}

bool RtlSdrSource::autoGain() const {
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    return lk.owns_lock() && link_->tuner && link_->tuner->autoGain();
}

// --- antennas ---------------------------------------------------------------

std::vector<std::string> RtlSdrSource::antennas() const {
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (lk.owns_lock() && link_->tuner && link_->tuner->blogV4()) { return {"RX", "HF"}; }
    return {"RX"};
}

std::string RtlSdrSource::antenna() const {
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (lk.owns_lock() && link_->tuner && link_->tuner->blogV4() &&
        centerFrequencyHz_.load(std::memory_order_relaxed) <= kBlogV4UpconvertHz) {
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

bool RtlSdrSource::setBiasTee(bool on) {
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (!lk.owns_lock() || !link_->rtl) {
        setError("the radio is busy; try again");
        return false;
    }
    if (!link_->rtl->setBiasTee(on)) {
        setError("the radio would not switch its bias tee");
        return false;
    }
    biasTee_.store(on, std::memory_order_relaxed);
    return true;
}

bool RtlSdrSource::setFreqCorrectionPpm(int ppm) {
    std::unique_lock<std::timed_mutex> lk(link_->mutex, kDeviceLockWait);
    if (!lk.owns_lock() || !link_->rtl || !link_->tuner) {
        setError("the radio is busy; try again");
        return false;
    }
    if (!link_->rtl->setFreqCorrectionPpm(ppm)) {
        setError("the radio would not accept a crystal correction");
        return false;
    }
    ppm_.store(ppm, std::memory_order_relaxed);
    // The tuner's PLL reference is the same crystal on every dongle but a
    // standard R828D, so the correction has to reach it too - and the current
    // frequency re-derived from the new reference, or the trim only takes
    // effect on the next tune.
    if (!(link_->tuner->chip() == TunerR82xx::Chip::R828D && !link_->tuner->blogV4())) {
        link_->tuner->setXtalHz(link_->rtl->correctedXtalHz());
    }
    return retuneLocked(centerFrequencyHz_.load(std::memory_order_relaxed));
}

// --- stream health ----------------------------------------------------------

std::string RtlSdrSource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void RtlSdrSource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

}  // namespace cascade::source
