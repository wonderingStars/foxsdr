// The ADALM-Pluto's transmit half. See pluto_tx.hpp for the safety argument
// this file is arranged around, the thread ownership, and what has and has
// not been proven; the wire protocol and its provenance are in
// iiod_client.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/pluto_tx.hpp"

#include "core/diag_log.hpp"
#include "source/pluto_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace cascade::source {

namespace {

// Abandoned writer threads, process-wide. See writersAbandoned().
std::atomic<unsigned long long> g_writersAbandoned{0};

// An attribute value for an integer-valued attribute - the LO frequency and
// the sampling frequency are both whole Hz.
std::string integerText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
    return buf;
}

// THE ATTENUATION IS NOT AN INTEGER. An AD9361's transmit gain steps in
// quarter decibels and the driver rejects a value off that grid by rounding
// it itself - but "-10" and "-10.250000" are different attenuations and a
// control that could only ask for whole ones would be a quarter of a
// decibel's worth of lie on every readout. Six decimal places is what sysfs
// prints these back as.
std::string decimalText(double v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

// The phy device's transmit half: an OUTPUT channel carrying a frequency (the
// TX LO) and an OUTPUT channel carrying a hardwaregain (the attenuation).
// Found by SHAPE rather than by name, exactly as the receive driver finds its
// half, so a board whose device is called something else still opens.
//
// THE LO IS IDENTIFIED AS THE ONE THAT IS NOT THE RECEIVER'S. A Pluto's phy
// has two altvoltage outputs - altvoltage0 named RX_LO and altvoltage1 named
// TX_LO - and they are indistinguishable by shape: both are outputs with a
// frequency. So the name is used when there is one, and the fallback is
// "altvoltage1", which is the only place in this file where a name is load-
// bearing. Getting it wrong would retune the RECEIVER when the operator moved
// the transmit frequency, so it is checked in the tests against a context
// that carries both.
const iiod::Device* findPhyTx(const iiod::Context& ctx, std::string& loChannel,
                              std::string& txChannel) {
    for (const iiod::Device& d : ctx.devices) {
        const iiod::Channel* lo = nullptr;
        const iiod::Channel* tx = nullptr;
        for (const iiod::Channel& c : d.channels) {
            if (!c.output) { continue; }
            if (c.hasAttr("frequency") &&
                (c.name == "TX_LO" || (lo == nullptr && c.name.empty() && c.id == "altvoltage1"))) {
                if (c.name == "TX_LO" || lo == nullptr) { lo = &c; }
            }
            if (c.hasAttr("hardwaregain") && tx == nullptr) { tx = &c; }
        }
        if (lo != nullptr && tx != nullptr) {
            loChannel = lo->id;
            txChannel = tx->id;
            return &d;
        }
    }
    return nullptr;
}

// The DAC device: the one whose OUTPUT channels carry scan elements. The
// capture device has scan elements too, but on INPUT channels, so the
// direction test is what tells them apart rather than a substring of the
// name - the mirror image of findCaptureDevice in pluto_source.cpp.
//
// `dds` collects the OUTPUT channels that have a `raw` attribute and NO scan
// element: those are the AD9361's internal tone generators, and they have to
// be written 0 or the DAC plays them instead of whatever the buffer holds.
const iiod::Device* findDacDevice(const iiod::Context& ctx, std::vector<std::size_t>& enabled,
                                  std::vector<std::string>& dds) {
    for (const iiod::Device& d : ctx.devices) {
        std::vector<std::size_t> idx;
        std::vector<std::string> tones;
        for (std::size_t i = 0; i < d.channels.size(); ++i) {
            const iiod::Channel& c = d.channels[i];
            if (!c.output) { continue; }
            if (c.hasScanElement) {
                idx.push_back(i);
            } else if (c.hasAttr("raw")) {
                tones.push_back(c.id);
            }
        }
        if (idx.size() >= 2) {
            // I and Q, in the daemon's own channel order - which is the order
            // the samples must be interleaved in the buffer, and the order
            // the OPEN mask's bits are numbered in.
            enabled.assign(idx.begin(), idx.begin() + 2);
            dds = std::move(tones);
            return &d;
        }
    }
    return nullptr;
}

}  // namespace

double clampTxGainDb(double requested, const iiod::Range& range) {
    // The quiet end and the loud end, taken from the pair rather than from
    // which member is called min - a board that published them the other way
    // round would otherwise have its silence and its full output swapped.
    const double quiet = std::min(range.min, range.max);
    const double loud = std::max(range.min, range.max);
    // NaN first, with a negated test, so a number that is not one lands on
    // the quiet end instead of falling through every ordering comparison and
    // being written verbatim.
    if (!(requested == requested)) { return quiet; }
    if (requested < quiet) { return quiet; }
    if (requested > loud) { return quiet; }
    // ABOVE THE LOUD END IS CLAMPED TO QUIET, NOT TO LOUD, and that is the
    // whole point of this function. Every other gain control in this product
    // clamps an over-large request to the maximum, because on a receiver the
    // worst that costs is a saturated front end. Here the maximum is FULL
    // TRANSMIT POWER, so a hand-edited config, an off-by-a-sign in a caller
    // or a units mix-up would key the radio at full output - by accident, and
    // silently. A request this function cannot honour is therefore answered
    // with silence, which is the only answer that cannot hurt anyone.
    return requested;
}

PlutoTx::~PlutoTx() { close(); }

// --- the error slot ---------------------------------------------------------

void PlutoTx::setErrorOn(WriterLink& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void PlutoTx::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void PlutoTx::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
}

void PlutoTx::noteFaultOn(WriterLink& link, const char* what, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lk(link.errorMutex);
        // Do not overwrite the FIRST cause: once a connection has gone every
        // later command on it fails too, and the last message is the least
        // informative one there is.
        if (!link.faulted) {
            link.lastError = std::string("the Pluto stopped answering while ") + what +
                             (detail.empty() ? std::string() : (": " + detail)) +
                             "; transmission has stopped";
        }
        link.faulted = true;
    }
    core::diagWarnf("tx: fault while %s%s%s", what, detail.empty() ? "" : ": ", detail.c_str());
}

void PlutoTx::noteFault(const char* what, const std::string& detail) {
    noteFaultOn(*link_, what, detail);
}

bool PlutoTx::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

const char* PlutoTx::lastError() const {
    // A per-THREAD snapshot, for the reason every driver in this directory
    // keeps one: the writer thread rewrites the member while the GUI reads
    // the pointer, and handing out a pointer into a string another thread is
    // assigning is undefined behaviour, not a stale value.
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(link_->errorMutex);
        snapshot = link_->lastError;
    }
    return snapshot.c_str();
}

void PlutoTx::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* PlutoTx::name() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        snapshot = name_;
    }
    return snapshot.c_str();
}

// --- attribute plumbing -----------------------------------------------------

bool PlutoTx::writeChanAttrLocked(const std::string& device, bool output,
                                  const std::string& channel, const char* attr,
                                  const std::string& value, const char* what) {
    if (control_ == nullptr) {
        setError(std::string("no Pluto is open for transmit (") + what + ")");
        return false;
    }
    if (!control_->writeChannelAttr(device, output, channel, attr, value)) {
        // A REFUSAL IS NOT A DEAD BOARD, the same distinction the receive
        // driver draws: the daemon answering a negative errno means this
        // firmware would not take that value, which is a fact about the
        // board; only a transport failure condemns the connection.
        if (control_->lastStatus() < 0) {
            setError(control_->lastError());
            return false;
        }
        noteFault(what, control_->lastError());
        return false;
    }
    return true;
}

bool PlutoTx::readChanAttrLocked(const std::string& device, bool output,
                                 const std::string& channel, const char* attr,
                                 std::string& value, const char* what) {
    if (control_ == nullptr) {
        setError(std::string("no Pluto is open for transmit (") + what + ")");
        return false;
    }
    if (!control_->readChannelAttr(device, output, channel, attr, value)) {
        if (control_->lastStatus() < 0) {
            setError(control_->lastError());
            return false;
        }
        noteFault(what, control_->lastError());
        return false;
    }
    return true;
}

bool PlutoTx::quietenLocked(const char* what) {
    // THE ORDER IS THE SAFETY PROPERTY. Attenuation first: it is the control
    // that stops RF leaving the connector, and it works whether or not the LO
    // is up. Powering the LO down first would leave a board with its PA still
    // at whatever gain it had for as long as the second write took.
    bool ok = writeChanAttrLocked(phyDevice_, true, txChannel_, "hardwaregain",
                                  decimalText(std::min(gainRange_.min, gainRange_.max)), what);
    if (haveLoPowerdown_) {
        // Attempted even if the first write failed: two chances at silence is
        // better than one, and a board that refused the gain may still take
        // the powerdown.
        ok = writeChanAttrLocked(phyDevice_, true, txLoChannel_, "powerdown", "1", what) && ok;
    }
    return ok;
}

bool PlutoTx::programGainLocked(double db) {
    const double want = clampTxGainDb(db, gainRange_);
    if (!writeChanAttrLocked(phyDevice_, true, txChannel_, "hardwaregain", decimalText(want),
                             "setting the transmit power")) {
        return false;
    }
    gainDb_.store(want, std::memory_order_relaxed);
    return true;
}

// --- open / close -----------------------------------------------------------

bool PlutoTx::open(const std::string& args) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ != nullptr) {
        setError("this Pluto transmitter already has a connection open");
        return false;
    }

    std::string host;
    std::uint16_t port = iiod::kDefaultPort;
    std::string error;
    if (!parsePlutoUri(args, host, port, error)) {
        setError(error);
        return false;
    }

    std::unique_ptr<iiod::Transport> transport =
        iiod::connectTcp(host, port, iiod::kConnectWait, iiod::kReplyWait, error);
    if (transport == nullptr) {
        setError(error);
        return false;
    }
    control_.reset(new iiod::Client(std::move(transport)));
    host_ = host;
    port_ = port;
    clearError();

    std::string daemonVersion;
    if (!control_->version(daemonVersion)) {
        setError(std::string("nothing at ") + host + " answered as an IIO daemon: " +
                 control_->lastError());
        control_.reset();
        return false;
    }

    std::string xml;
    if (!control_->print(xml)) {
        setError(std::string("the Pluto would not describe itself: ") + control_->lastError());
        control_.reset();
        return false;
    }
    iiod::Context ctx;
    if (!iiod::parseContext(xml, ctx, error)) {
        setError("could not read what the Pluto sent about itself: " + error);
        control_.reset();
        return false;
    }

    const iiod::Device* phy = findPhyTx(ctx, txLoChannel_, txChannel_);
    if (phy == nullptr) {
        setError(
            "that IIO device has no AD9361 transmitter in it (no device with a TX LO and a "
            "transmit gain channel) - it answered, but FoxSDR cannot transmit through it");
        control_.reset();
        return false;
    }
    dacEnabled_.clear();
    ddsChannels_.clear();
    const iiod::Device* dac = findDacDevice(ctx, dacEnabled_, ddsChannels_);
    // The count is checked here rather than trusted from findDacDevice's own
    // rule, because everything below indexes dacEnabled_[0] and [1]. An index
    // that is checked cannot become an out-of-bounds read.
    if (dac == nullptr || dacEnabled_.size() != 2) {
        setError("that Pluto has no DAC device with two output channels to transmit through");
        control_.reset();
        return false;
    }
    phyDevice_ = phy->name.empty() ? phy->id : phy->name;
    dacDevice_ = dac->name.empty() ? dac->id : dac->name;
    dacChannels_ = dac->channelCount();

    const iiod::Channel& chI = dac->channels[dacEnabled_[0]];
    const iiod::Channel& chQ = dac->channels[dacEnabled_[1]];
    if (!iiod::parseSampleFormat(chI.scanFormat, format_)) {
        setError("this Pluto describes its transmit samples as \"" + chI.scanFormat +
                 "\", which this driver cannot encode");
        control_.reset();
        return false;
    }
    if (chI.scanFormat != chQ.scanFormat) {
        setError("this Pluto's two DAC channels disagree about their sample format (\"" +
                 chI.scanFormat + "\" and \"" + chQ.scanFormat + "\")");
        control_.reset();
        return false;
    }

    hwModel_ = ctx.attr("hw_model");

    const iiod::Channel* lo = phy->findChannel(txLoChannel_, true);
    const iiod::Channel* tx = phy->findChannel(txChannel_, true);
    haveLoPowerdown_ = lo != nullptr && lo->hasAttr("powerdown");
    // ASKED FOR ONLY WHAT THE CONTEXT SAID IS THERE. Writing an attribute the
    // board does not publish answers -ENOENT and fills the error slot with a
    // failure that is not one, which is how a perfectly good radio ends up
    // reporting a problem the first time it is keyed.
    haveTxPort_ = tx != nullptr && tx->hasAttr("rf_port_select");

    std::string text;
    haveLoRange_ = false;
    if (lo != nullptr && lo->hasAttr("frequency_available") &&
        readChanAttrLocked(phyDevice_, true, txLoChannel_, "frequency_available", text,
                           "reading the transmit tuning range")) {
        haveLoRange_ = iiod::parseRange(text, loRange_);
    }
    haveRateRange_ = false;
    if (tx != nullptr && tx->hasAttr("sampling_frequency_available") &&
        readChanAttrLocked(phyDevice_, true, txChannel_, "sampling_frequency_available", text,
                           "reading the transmit sample-rate range")) {
        haveRateRange_ = iiod::parseRange(text, rateRange_);
    }
    haveBwRange_ = false;
    if (tx != nullptr && tx->hasAttr("rf_bandwidth_available") &&
        readChanAttrLocked(phyDevice_, true, txChannel_, "rf_bandwidth_available", text,
                           "reading the transmit bandwidth range")) {
        haveBwRange_ = iiod::parseRange(text, bwRange_);
    }
    haveGainRange_ = false;
    if (tx != nullptr && tx->hasAttr("hardwaregain_available") &&
        readChanAttrLocked(phyDevice_, true, txChannel_, "hardwaregain_available", text,
                           "reading the transmit gain range")) {
        haveGainRange_ = iiod::parseRange(text, gainRange_);
    }
    // NO RANGE, NO TRANSMITTER, and this is a refusal rather than a fallback.
    // Everything this class does to keep a radio quiet is written in terms of
    // the board's OWN maximum attenuation; without one there is no number to
    // write, and the alternatives are both wrong - invent one and it may not
    // be the quiet end on this board, or skip the quietening and the safety
    // property is gone. A board that will not say how quiet it can be is a
    // board FoxSDR will not key.
    if (!haveGainRange_) {
        setError(
            "this Pluto does not publish a transmit gain range, so FoxSDR cannot tell how to "
            "make it quiet - it will not transmit through this board");
        control_.reset();
        return false;
    }

    if (!readChanAttrLocked(phyDevice_, true, txLoChannel_, "frequency", text,
                            "reading the transmit frequency")) {
        control_.reset();
        return false;
    }
    centerFrequencyHz_.store(std::strtod(text.c_str(), nullptr), std::memory_order_relaxed);

    if (!readChanAttrLocked(phyDevice_, true, txChannel_, "sampling_frequency", text,
                            "reading the transmit sample rate")) {
        control_.reset();
        return false;
    }
    sampleRateHz_.store(std::strtod(text.c_str(), nullptr), std::memory_order_relaxed);

    // THE POWER STARTS AT THE QUIET END, and it is NOT read off the board.
    // A Pluto keeps whatever the last program left in it, so adopting the
    // board's current gain would make the operator's first key-down as loud
    // as somebody else's last one. The panel persists its own setting and
    // writes it here; a fresh install starts silent and has to be turned up
    // deliberately, which is the correct default for a transmitter.
    gainDb_.store(std::min(gainRange_.min, gainRange_.max), std::memory_order_relaxed);

    // AND THE BOARD IS MADE QUIET BEFORE THIS FUNCTION RETURNS. See the file
    // header: this is the difference between "FoxSDR has not keyed it" and
    // "it is not keyed".
    if (!quietenLocked("making the board quiet at open")) {
        control_.reset();
        return false;
    }

    std::string label = hwModel_.empty() ? std::string("ADALM-Pluto") : hwModel_;
    setName("Pluto TX: " + label + " at ip:" + host_);
    openMirror_.store(true, std::memory_order_relaxed);

    core::diagLogf(
        "tx: opened %s at %s:%u for transmit - phy %s (LO %s, gain %s), DAC %s (%s), %zu DDS "
        "tone channel(s)",
        label.c_str(), host_.c_str(), static_cast<unsigned>(port_), phyDevice_.c_str(),
        txLoChannel_.c_str(), txChannel_.c_str(), dacDevice_.c_str(), chI.scanFormat.c_str(),
        ddsChannels_.size());
    core::diagLogf("tx: the board reports transmit power %.2f to %.2f dB (0 dB is full output)%s",
                   gainRange_.min, gainRange_.max,
                   haveLoPowerdown_ ? "; the TX LO can be powered down"
                                    : "; this board has no TX LO powerdown, so the attenuation "
                                      "is the only silencer");
    if (haveLoRange_) {
        core::diagLogf("tx: the board reports transmitting %.3f MHz to %.3f MHz",
                       loRange_.min / 1e6, loRange_.max / 1e6);
    } else {
        core::diagLogf("tx: the board did not publish a transmit tuning range; FoxSDR will not "
                       "claim one");
    }
    core::diagLogf("tx: the board is quiet (attenuation at its maximum%s) and stays that way "
                   "until the operator keys it",
                   haveLoPowerdown_ ? ", TX LO down" : "");
    return true;
}

void PlutoTx::close() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopWritingLocked();
    if (control_ != nullptr) {
        // BELT AND BRACES, and it is not redundant. The ordinary stop path
        // silences the board from the writer thread; a writer that had to be
        // abandoned did not get that far, and this is the second chance. On a
        // board that is already quiet it costs two writes that change
        // nothing, which is the right price for the case where it matters.
        //
        // This runs from ~PlutoTx, which the application reaches from
        // ~AppWindow - after the watchdog has been stopped, so it is outside
        // the shutdown budget, exactly as ~SoapySource's own teardown is.
        quietenLocked("making the board quiet at close");
        control_->close();
        control_.reset();
    }
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    setName("ADALM-Pluto transmit: (not connected)");
}

std::string PlutoTx::hardwareModel() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return hwModel_;
}
std::string PlutoTx::phyDeviceName() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return phyDevice_;
}
std::string PlutoTx::dacDeviceName() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return dacDevice_;
}
std::string PlutoTx::txLoChannel() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return txLoChannel_;
}
std::string PlutoTx::txChannel() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return txChannel_;
}

std::uint64_t PlutoTx::underrunBuffers() const {
    return link_->underruns.load(std::memory_order_relaxed);
}

unsigned long long PlutoTx::writersAbandoned() {
    return g_writersAbandoned.load(std::memory_order_relaxed);
}

// --- the limits -------------------------------------------------------------

bool PlutoTx::frequencyRangeHz(double& loHz, double& hiHz) const {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!haveLoRange_) { return false; }
    loHz = loRange_.min;
    hiHz = loRange_.max;
    return true;
}

bool PlutoTx::sampleRateRangeHz(double& loHz, double& hiHz) const {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!haveRateRange_) { return false; }
    loHz = rateRange_.min;
    hiHz = rateRange_.max;
    return true;
}

bool PlutoTx::gainRangeDb(double& loDb, double& hiDb) const {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!haveGainRange_) { return false; }
    loDb = std::min(gainRange_.min, gainRange_.max);
    hiDb = std::max(gainRange_.min, gainRange_.max);
    return true;
}

// --- the controls -----------------------------------------------------------

bool PlutoTx::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("no Pluto is open for transmit");
        return false;
    }
    if (haveLoRange_ && (hz < loRange_.min || hz > loRange_.max)) {
        // REFUSED WITH A REASON rather than clamped, the same rule the
        // receive driver follows: a transmission that silently lands
        // somewhere else is worse than one that does not happen, and on this
        // side of the radio it is somebody else's band.
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "this board transmits between %.3f MHz and %.3f MHz; %.3f MHz is outside "
                      "that",
                      loRange_.min / 1e6, loRange_.max / 1e6, hz / 1e6);
        setError(buf);
        return false;
    }
    if (!writeChanAttrLocked(phyDevice_, true, txLoChannel_, "frequency", integerText(hz),
                             "setting the transmit frequency")) {
        return false;
    }
    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    return true;
}

bool PlutoTx::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("no Pluto is open for transmit");
        return false;
    }
    double want = hz;
    if (haveRateRange_) {
        if (want > rateRange_.max) { want = rateRange_.max; }
        if (want < rateRange_.min) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "this board will not transmit below %.0f S/s without a FIR filter "
                          "written into filter_fir_config, which FoxSDR does not generate",
                          rateRange_.min);
            setError(buf);
            return false;
        }
    }
    if (!writeChanAttrLocked(phyDevice_, true, txChannel_, "sampling_frequency",
                             integerText(want), "setting the transmit sample rate")) {
        return false;
    }
    sampleRateHz_.store(want, std::memory_order_relaxed);
    // THE FILTER FOLLOWS THE RATE, for the same reason the receive driver
    // makes it follow: an AD9361 keeps whatever analogue bandwidth it was
    // last given, and a transmitter left at a 200 kHz filter while its DAC
    // runs at 2.5 MS/s is one whose modulation is being cut without anything
    // on screen to say so.
    if (haveBwRange_) {
        double bw = want;
        if (bw > bwRange_.max) { bw = bwRange_.max; }
        if (bw < bwRange_.min) { bw = bwRange_.min; }
        if (!writeChanAttrLocked(phyDevice_, true, txChannel_, "rf_bandwidth", integerText(bw),
                                 "setting the transmit analogue bandwidth")) {
            return false;
        }
    }
    return true;
}

bool PlutoTx::setGainDb(double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("no Pluto is open for transmit");
        return false;
    }
    const double want = clampTxGainDb(db, gainRange_);
    if (!running_.load(std::memory_order_relaxed)) {
        // NOT WRITTEN TO A BOARD THAT IS NOT KEYED. While the sink is stopped
        // the board is deliberately sitting at its maximum attenuation (see
        // the file header), and writing the operator's chosen power into it
        // now would undo exactly that. The number is remembered and start()
        // writes it - as its last act, after everything else is set up.
        gainDb_.store(want, std::memory_order_relaxed);
        return true;
    }
    return programGainLocked(want);
}

// --- keying -----------------------------------------------------------------

bool PlutoTx::startWritingLocked() {
    std::string error;
    std::unique_ptr<iiod::Transport> transport =
        iiod::connectTcp(host_, port_, iiod::kConnectWait, iiod::kReplyWait, error);
    if (transport == nullptr) {
        noteFault("opening a second connection for the transmit samples", error);
        return false;
    }
    std::unique_ptr<iiod::Client> stream(new iiod::Client(std::move(transport)));
    stream->setTimeoutMs(static_cast<long>(iiod::kReplyWait.count() * 2));

    // --- everything below here happens while the board is still quiet ------

    // The port. One transmit port is brought out on a Pluto and it is the A
    // pair; the AD9361 publishes more and they go nowhere on this board.
    // Written rather than assumed, because a board another program left on a
    // different port would transmit into a connector that is not there.
    if (haveTxPort_ &&
        !writeChanAttrLocked(phyDevice_, true, txChannel_, "rf_port_select", "A",
                             "setting the transmit port")) {
        quietenLocked("making the board quiet after a failed start");
        return false;
    }

    // THE DDS TONE GENERATORS OFF. This is the step pluto_source.hpp's note
    // did not know about, and without it a Pluto transmits its own internal
    // test tones and ignores every sample handed to it - which looks exactly
    // like a driver that is not writing anything. Each generator is written
    // 0 through `raw`; a board that publishes none (the context said so) gets
    // nothing written, because there is nothing to turn off.
    for (const std::string& dds : ddsChannels_) {
        if (!writeChanAttrLocked(dacDevice_, true, dds, "raw", "0",
                                 "switching off the board's own test tones")) {
            quietenLocked("making the board quiet after a failed start");
            return false;
        }
    }

    if (haveLoPowerdown_ &&
        !writeChanAttrLocked(phyDevice_, true, txLoChannel_, "powerdown", "0",
                             "powering the transmit oscillator up")) {
        quietenLocked("making the board quiet after a failed start");
        return false;
    }

    // BUFFERS_COUNT BEFORE OPEN, the same ops.c rule the receive side obeys:
    // the count is stashed on the device and only read when the buffer is
    // created, which is what OPEN triggers.
    if (!stream->setBuffersCount(dacDevice_, kTxBuffersCount)) {
        noteFault("setting the number of transmit buffers", stream->lastError());
        quietenLocked("making the board quiet after a failed start");
        return false;
    }
    const std::string mask = iiod::channelMask(dacEnabled_, dacChannels_);
    if (!stream->openBuffer(dacDevice_, kTxSamplesPerBuffer, mask)) {
        noteFault("opening the DAC device", stream->lastError());
        quietenLocked("making the board quiet after a failed start");
        return false;
    }

    // --- and THIS is the step that keys the radio --------------------------
    if (!programGainLocked(gainDb_.load(std::memory_order_relaxed))) {
        quietenLocked("making the board quiet after a failed start");
        return false;
    }

    link_->dacDevice = dacDevice_;
    link_->phyDevice = phyDevice_;
    link_->txChannel = txChannel_;
    link_->txLoChannel = txLoChannel_;
    link_->haveLoPowerdown = haveLoPowerdown_;
    link_->quietDb = std::min(gainRange_.min, gainRange_.max);
    link_->bufferSamples = kTxSamplesPerBuffer;
    link_->format = format_;
    // A stale ring from the last transmission is stale MODULATION, and it
    // would go on the air before anything the operator is saying now. Drained
    // here rather than in the writer because here is the one moment nothing
    // else touches it: the previous writer has been joined and the next has
    // not been spawned, so this thread is the only reader AND the only
    // writer. (AudioOut::open drains its own ring at the same point, for the
    // same reason.)
    {
        std::complex<float> scratch[256];
        while (link_->ring.read(scratch, sizeof(scratch) / sizeof(scratch[0])) != 0) {}
    }
    link_->silenced.store(false, std::memory_order_relaxed);
    stream_ = std::move(stream);
    link_->stream = stream_.get();
    {
        std::lock_guard<std::mutex> lk(link_->waitMutex);
        link_->exited = false;
    }
    link_->run.store(true, std::memory_order_relaxed);
    writer_ = std::thread(&PlutoTx::writerThreadBody, link_);
    running_.store(true, std::memory_order_relaxed);
    return true;
}

bool PlutoTx::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("start() called with no Pluto open for transmit");
        return false;
    }
    if (faulted()) { return false; }
    if (running_.load(std::memory_order_relaxed)) { return true; }
    clearError();
    const bool ok = startWritingLocked();
    if (ok) {
        // Never the frequency (see Transmitter::key's log line).
        core::diagLogf("tx: started - %.0f S/s, power %.2f dB",
                       sampleRateHz_.load(std::memory_order_relaxed),
                       gainDb_.load(std::memory_order_relaxed));
    }
    return ok;
}

void PlutoTx::stopWritingLocked() {
    if (!writer_.joinable() && !running_.load(std::memory_order_relaxed)) {
        if (stream_ != nullptr) {
            stream_->close();
            stream_.reset();
            link_->stream = nullptr;
        }
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    link_->run.store(false, std::memory_order_relaxed);
    link_->waitCv.notify_all();

    if (writer_.joinable()) {
        // A BOUNDED JOIN, not a plain one. The writer sets `exited` as its
        // last act - after it has silenced the board - so a flag still clear
        // after kWriterJoinWait means a thread that is not coming back, and
        // waiting for it on the GUI thread would be the hang the bounds exist
        // to avoid.
        bool exited = false;
        {
            std::unique_lock<std::mutex> lk(link_->waitMutex);
            exited = link_->waitCv.wait_for(lk, kWriterJoinWait, [this] { return link_->exited; });
        }
        if (exited) {
            writer_.join();
        } else {
            // ABANDONED - and it keeps trying. The thread holds its own copy
            // of the link and the stream pointer inside it, so NEITHER may be
            // disturbed: the client is leaked deliberately rather than
            // destroyed under a thread still inside its socket, which also
            // means the stranded writer still has a live connection to
            // silence the board through if the board comes back.
            g_writersAbandoned.fetch_add(1, std::memory_order_relaxed);
            noteFault("waiting for the transmit writer to stop",
                      "the writer did not return; FoxSDR could not confirm the board was made "
                      "quiet - check the radio");
            core::diagWarnf(
                "tx: the writer thread did not return within %lld ms and was abandoned; the "
                "board may still be transmitting",
                static_cast<long long>(kWriterJoinWait.count()));
            writer_.detach();
            (void)stream_.release();
            link_->stream = nullptr;
            running_.store(false, std::memory_order_relaxed);
            return;
        }
    }

    if (stream_ != nullptr) {
        // Dropping the socket is what closes the DAC buffer: the daemon frees
        // every device a connection held when its read returns 0 (ops.c
        // ascii_interpreter). No CLOSE command is sent, for the reason
        // pluto_source.cpp's stopStreamingLocked gives at length - it would
        // cost one iiod::kReplyWait on the teardown path for a politeness the
        // daemon does not need.
        stream_->close();
        stream_.reset();
        link_->stream = nullptr;
    }
    running_.store(false, std::memory_order_relaxed);
}

void PlutoTx::stop() {
    std::lock_guard<std::mutex> lk(devMutex_);
    const bool was = running_.load(std::memory_order_relaxed);
    stopWritingLocked();
    if (was) {
        core::diagLogf("tx: stopped - the board was told to go quiet (%s)",
                       link_->silenced.load(std::memory_order_relaxed)
                           ? "confirmed"
                           : "NOT CONFIRMED - the board did not answer");
    }
}

// --- the samples ------------------------------------------------------------

std::size_t PlutoTx::write(const std::complex<float>* samples, std::size_t n) {
    if (samples == nullptr || n == 0) { return 0; }
    if (!running_.load(std::memory_order_relaxed)) { return 0; }
    if (faulted()) { return 0; }

    std::size_t done = link_->ring.write(samples, n);
    if (done == n) { return done; }

    // The ring is full: the radio is not taking modulation as fast as it is
    // being made. Wait once, BOUNDED, and then report short - the caller is
    // also the thread that notices the PTT has been released, so it may not
    // be parked here indefinitely (tx_sink.hpp, point 3).
    {
        std::unique_lock<std::mutex> lk(link_->waitMutex);
        link_->waitCv.wait_for(lk, kWriteWait, [this] {
            return link_->ring.freeSpace() > 0 || !link_->run.load(std::memory_order_relaxed);
        });
    }
    if (!running_.load(std::memory_order_relaxed)) { return done; }
    done += link_->ring.write(samples + done, n - done);
    return done;
}

// --- the writer thread ------------------------------------------------------

void PlutoTx::silenceOn(WriterLink& link) {
    if (link.stream == nullptr) { return; }
    // THE ORDER AGAIN, and it is the same order quietenLocked uses on the
    // control connection: attenuation first, oscillator second. Both are
    // attempted even if the first fails.
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.6f", link.quietDb);
    bool ok = link.stream->writeChannelAttr(link.phyDevice, true, link.txChannel,
                                            "hardwaregain", buf);
    if (link.haveLoPowerdown) {
        ok = link.stream->writeChannelAttr(link.phyDevice, true, link.txLoChannel, "powerdown",
                                           "1") &&
             ok;
    }
    link.silenced.store(ok, std::memory_order_relaxed);
    if (!ok) {
        // DOES NOT OVERWRITE A FAULT ALREADY REPORTED. When the board
        // vanishes mid-transmission this write fails too, and "it would not
        // confirm it had gone quiet" is the second thing that happened -
        // reporting it over "it stopped answering while handing over a
        // transmit buffer" would replace the cause with its consequence. It
        // was caught exactly that way: the fake daemon dropped the socket
        // after three buffers and the message the test printed named the
        // silencing rather than the stream.
        std::lock_guard<std::mutex> lk(link.errorMutex);
        if (!link.faulted) {
            link.lastError = std::string("the Pluto did not confirm it had gone quiet: ") +
                             link.stream->lastError();
        }
    }
}

void PlutoTx::writerThreadBody(std::shared_ptr<WriterLink> link) {
    const std::size_t storage = link->format.storageBytes();
    const std::size_t bytesPerSample = 2 * storage;  // I and Q, interleaved
    const std::size_t samples = link->bufferSamples;
    std::vector<std::complex<float>> block(samples);
    std::vector<std::uint8_t> raw(samples * bytesPerSample);

    while (link->run.load(std::memory_order_relaxed)) {
        // A WHOLE BUFFER OR NOTHING - WRITEBUF states a byte count and sends
        // exactly that many, so a partial one is not on offer. Wait briefly
        // for the modulator to catch up; if it has not, pad the rest with
        // silence rather than stalling, because a gap in the modulation is
        // recoverable and a writer that stops noticing `run` is not.
        std::size_t got = link->ring.read(block.data(), samples);
        if (got < samples) {
            std::unique_lock<std::mutex> lk(link->waitMutex);
            link->waitCv.wait_for(lk, kWriteWait, [&link, samples] {
                return link->ring.size() >= samples ||
                       !link->run.load(std::memory_order_relaxed);
            });
            lk.unlock();
            got += link->ring.read(block.data() + got, samples - got);
        }
        if (!link->run.load(std::memory_order_relaxed)) { break; }
        if (got < samples) {
            std::fill(block.begin() + static_cast<std::ptrdiff_t>(got), block.end(),
                      std::complex<float>(0.0f, 0.0f));
            link->underruns.fetch_add(1, std::memory_order_relaxed);
        }

        for (std::size_t i = 0; i < samples; ++i) {
            std::uint8_t* word = raw.data() + i * bytesPerSample;
            iiod::packSample(block[i].real(), link->format, word);
            iiod::packSample(block[i].imag(), link->format, word + storage);
        }

        if (!link->stream->writeBuf(link->dacDevice, raw.data(), raw.size())) {
            // A FAILED WRITEBUF IS THE BOARD GOING. There is nothing to
            // retry: the connection is out of step or gone, and every further
            // write would fail the same way while a keyed radio sat there. So
            // leave the loop - which runs the silencing below, on the one
            // chance that the connection recovers enough to carry it.
            noteFaultOn(*link, "handing the Pluto a transmit buffer",
                        link->stream->lastError());
            break;
        }

        {
            // Taken and released so a write() that has just found the ring
            // full and is about to wait cannot miss this notify in between.
            std::lock_guard<std::mutex> lk(link->waitMutex);
        }
        link->waitCv.notify_all();
    }

    // THE LAST ACT BUT ONE: make the radio quiet. On the ordinary path this
    // is what stop()'s bounded join is waiting for.
    silenceOn(*link);

    {
        std::lock_guard<std::mutex> lk(link->waitMutex);
        link->run.store(false, std::memory_order_relaxed);
        link->exited = true;
    }
    link->waitCv.notify_all();
}

}  // namespace cascade::source
