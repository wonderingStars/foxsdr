// The ADALM-Pluto driver itself. See pluto_source.hpp for the threading and
// ownership argument and for what a transmit path would need; the wire
// protocol and its provenance are in iiod_client.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/pluto_source.hpp"

#include "core/diag_log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace cascade::source {

namespace {

// Abandoned reader threads, process-wide. See readersAbandoned().
std::atomic<unsigned long long> g_readersAbandoned{0};

// The addresses the enumeration offers. 192.168.2.1 is the USB Ethernet
// gadget's own address out of the box; pluto.local is the mDNS name the board
// advertises. Neither is contacted to build the list.
constexpr const char* kDefaultHost = "192.168.2.1";
constexpr const char* kMdnsHost = "pluto.local";

// What a stock AD9363 Pluto covers, published by Analog Devices. Used for ONE
// thing only: deciding whether the range the board itself reported is wider
// than that, i.e. whether the AD9364 unlock has been applied. It is never
// used as a limit - every limit this driver enforces came off the board.
constexpr double kAd9363MinHz = 325.0e6;
constexpr double kAd9363MaxHz = 3.8e9;

// An attribute value for an integer-valued attribute. The AD9361's
// frequency, sampling_frequency and rf_bandwidth are all whole Hz, and its RX
// hardwaregain is whole dB; writing "2400000.000000" where the kernel wants
// an integer is a refusal waiting to happen.
std::string integerText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
    return buf;
}

bool containsWord(const std::vector<std::string>& v, const std::string& w) {
    return std::find(v.begin(), v.end(), w) != v.end();
}

// The phy device: the one carrying the RX LO and the RX gain controls. Found
// by SHAPE rather than by name, so a board whose device is called something
// else still opens - but the name is reported either way, because "which
// device did we decide to drive" is the first thing to check when a Pluto
// behaves oddly.
const iiod::Device* findPhyDevice(const iiod::Context& ctx, std::string& loChannel,
                                  std::string& rxChannel) {
    for (const iiod::Device& d : ctx.devices) {
        const iiod::Channel* lo = nullptr;
        const iiod::Channel* rx = nullptr;
        for (const iiod::Channel& c : d.channels) {
            if (c.output && c.hasAttr("frequency") &&
                (c.name == "RX_LO" || (lo == nullptr && c.id == "altvoltage0"))) {
                lo = &c;
            }
            if (!c.output && c.hasAttr("hardwaregain") && c.hasAttr("sampling_frequency")) {
                if (rx == nullptr) { rx = &c; }
            }
        }
        if (lo != nullptr && rx != nullptr) {
            loChannel = lo->id;
            rxChannel = rx->id;
            return &d;
        }
    }
    return nullptr;
}

// The capture device: the one whose INPUT channels carry scan elements. The
// transmit side (cf-ad9361-dds-core-lpc) has scan elements too, but on OUTPUT
// channels, so the direction test is what tells them apart rather than a
// substring of the name.
const iiod::Device* findCaptureDevice(const iiod::Context& ctx,
                                      std::vector<std::size_t>& enabled) {
    for (const iiod::Device& d : ctx.devices) {
        std::vector<std::size_t> idx;
        for (std::size_t i = 0; i < d.channels.size(); ++i) {
            const iiod::Channel& c = d.channels[i];
            if (!c.output && c.hasScanElement) { idx.push_back(i); }
        }
        if (idx.size() >= 2) {
            // I and Q, in the daemon's own channel order - which is the order
            // the samples are interleaved in the buffer, and the order the
            // OPEN mask's bits are numbered in (ops.c open_dev_helper walks
            // the device's channels and tests bit i of the mask for channel
            // i, so a mask bit is a POSITION in this list, not a scan index).
            enabled.assign(idx.begin(), idx.begin() + 2);
            return &d;
        }
    }
    return nullptr;
}

}  // namespace

// --- enumeration ----------------------------------------------------------

std::vector<NativeDeviceInfo> enumeratePluto() {
    std::vector<NativeDeviceInfo> out;
    for (const char* host : {kDefaultHost, kMdnsHost}) {
        NativeDeviceInfo info;
        info.driver = "pluto";
        info.args = std::string("uri=ip:") + host;
        // THE LABEL SAYS WHAT WAS ACTUALLY DONE, which is nothing. A network
        // cannot be enumerated without probing it, and probing would break
        // the rule that enumeration never opens a device - so the row is an
        // offer, and it says so rather than implying a radio was found.
        info.label = std::string("ADALM-Pluto at ip:") + host + " (not yet contacted)";
        out.push_back(std::move(info));
    }
    return out;
}

bool parsePlutoUri(const std::string& args, std::string& host, std::uint16_t& port,
                   std::string& error) {
    host = kDefaultHost;
    port = iiod::kDefaultPort;
    error.clear();

    std::string text = argValue(args, "uri");
    if (text.empty()) { text = argValue(args, "host"); }
    if (text.empty()) {
        // No address at all means the board on the USB cable, which is what
        // a user who picked "ADALM-Pluto" and typed nothing meant.
        return true;
    }

    // libiio's URI spelling. Only the ip: backend can be reached from here -
    // usb: and local: would need a vendor library and a driver we do not
    // have - so anything else is refused by name rather than half-attempted.
    if (text.compare(0, 3, "ip:") == 0) {
        text = text.substr(3);
    } else if (text.find(':') != std::string::npos && text.find_first_of("0123456789") != 0) {
        const std::string scheme = text.substr(0, text.find(':'));
        if (scheme == "usb" || scheme == "local" || scheme == "serial") {
            error = "FoxSDR reaches a Pluto over the network; \"" + scheme +
                    ":\" addresses need libiio, which this driver deliberately does not use. "
                    "Use uri=ip:192.168.2.1 instead.";
            return false;
        }
    }

    // An optional :port. An IPv6 literal would also be full of colons, so the
    // port is only taken when what follows the LAST colon is entirely digits
    // and there is exactly one colon (a bare IPv6 address has several).
    const std::size_t colon = text.rfind(':');
    if (colon != std::string::npos && text.find(':') == colon && colon + 1 < text.size()) {
        const std::string tail = text.substr(colon + 1);
        bool digits = true;
        for (const char c : tail) {
            if (c < '0' || c > '9') { digits = false; }
        }
        if (digits) {
            const long n = std::strtol(tail.c_str(), nullptr, 10);
            if (n <= 0 || n > 65535) {
                error = "\"" + tail + "\" is not a port number";
                return false;
            }
            port = static_cast<std::uint16_t>(n);
            text = text.substr(0, colon);
        }
    }

    if (text.empty()) {
        error = "that address has no host in it";
        return false;
    }
    host = text;
    return true;
}

// --- construction ---------------------------------------------------------

PlutoSource::~PlutoSource() { closeDevice(); }

// --- the error slot -------------------------------------------------------

void PlutoSource::setErrorOn(ReaderLink& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void PlutoSource::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void PlutoSource::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
}

void PlutoSource::noteFaultOn(ReaderLink& link, const char* what, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lk(link.errorMutex);
        // Do not overwrite the FIRST cause. Once a connection has gone, every
        // later command on it fails too, and the last message is the least
        // informative one there is.
        if (!link.deviceDead) {
            link.lastError = std::string("the Pluto stopped answering while ") + what +
                             (detail.empty() ? std::string() : (": " + detail)) +
                             "; check the cable or the network and open it again";
            link.deadWhat = what;
        }
        link.faulted = true;
        link.deviceDead = true;
    }
    core::diagWarnf("pluto: failed while %s%s%s", what, detail.empty() ? "" : ": ",
                    detail.c_str());
}

void PlutoSource::noteFault(const char* what, const std::string& detail) {
    noteFaultOn(*link_, what, detail);
}

bool PlutoSource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool PlutoSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string PlutoSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deadWhat;
}

const char* PlutoSource::lastError() const {
    // A per-THREAD snapshot, for the same reason SoapySource keeps one: the
    // reader thread rewrites the member while the GUI reads the pointer, and
    // handing out a pointer into a string another thread is assigning is UB.
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(link_->errorMutex);
        snapshot = link_->lastError;
    }
    return snapshot.c_str();
}

void PlutoSource::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* PlutoSource::name() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        snapshot = name_;
    }
    return snapshot.c_str();
}

// --- attribute plumbing ---------------------------------------------------

bool PlutoSource::readAttrLocked(const std::string& device, const char* attr, std::string& value,
                                 const char* what) {
    if (control_ == nullptr) {
        setError(std::string("no Pluto is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!control_->readDeviceAttr(device, attr, value)) {
        // A REFUSAL IS NOT A DEAD BOARD. The daemon answering -ENOENT means
        // this firmware does not have that attribute, which is a fact about
        // the board rather than a failure of the link; only a transport
        // failure condemns the connection.
        if (control_->lastStatus() < 0) {
            setError(control_->lastError());
            return false;
        }
        noteFault(what, control_->lastError());
        return false;
    }
    return true;
}

bool PlutoSource::readChanAttrLocked(const std::string& device, bool output,
                                     const std::string& channel, const char* attr,
                                     std::string& value, const char* what) {
    if (control_ == nullptr) {
        setError(std::string("no Pluto is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
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

bool PlutoSource::writeChanAttrLocked(const std::string& device, bool output,
                                      const std::string& channel, const char* attr,
                                      const std::string& value, const char* what) {
    if (control_ == nullptr) {
        setError(std::string("no Pluto is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!control_->writeChannelAttr(device, output, channel, attr, value)) {
        if (control_->lastStatus() < 0) {
            setError(control_->lastError());
            return false;
        }
        noteFault(what, control_->lastError());
        return false;
    }
    return true;
}

bool PlutoSource::programFrequencyLocked(double hz) {
    if (!writeChanAttrLocked(phyDevice_, true, rxLoChannel_, "frequency", integerText(hz),
                             "setting the centre frequency")) {
        return false;
    }
    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    return true;
}

bool PlutoSource::programSampleRateLocked(double hz) {
    if (!writeChanAttrLocked(phyDevice_, false, rxChannel_, "sampling_frequency",
                             integerText(hz), "setting the sample rate")) {
        return false;
    }
    sampleRateHz_.store(hz, std::memory_order_relaxed);

    // THE FILTER FOLLOWS THE RATE. An AD9361 keeps whatever analogue
    // bandwidth it was last given, so a board another application left at
    // 200 kHz would hand us 2.5 MS/s of mostly-filtered-away signal with
    // nothing on screen to say why. The widest setting no wider than the rate
    // is the right one - the signal we can represent is exactly the span the
    // rate covers - clamped into whatever range the board published.
    if (haveBwRange_) {
        double bw = hz;
        if (bw > bwRange_.max) { bw = bwRange_.max; }
        if (bw < bwRange_.min) { bw = bwRange_.min; }
        if (!writeChanAttrLocked(phyDevice_, false, rxChannel_, "rf_bandwidth", integerText(bw),
                                 "setting the analogue bandwidth")) {
            return false;
        }
    }
    return true;
}

bool PlutoSource::programGainLocked(double db) {
    double want = db;
    if (haveGainRange_) {
        if (want < gainRange_.min) { want = gainRange_.min; }
        if (want > gainRange_.max) { want = gainRange_.max; }
    }
    if (!writeChanAttrLocked(phyDevice_, false, rxChannel_, "hardwaregain", integerText(want),
                             "setting the gain")) {
        return false;
    }
    gainDb_.store(static_cast<double>(std::llround(want)), std::memory_order_relaxed);
    return true;
}

bool PlutoSource::programGainModeLocked(bool automatic) {
    // "slow_attack" rather than "fast_attack" for the automatic case: a
    // receiver listening to a steady signal wants the AGC that settles, and
    // the fast one chases every burst, which on a waterfall reads as the
    // noise floor breathing. Both are offered by the hardware; only one is a
    // sensible default, and the mode is read back at open so what is on
    // screen is what the board is doing either way.
    const std::string mode = automatic ? "slow_attack" : "manual";
    if (!gainModes_.empty() && !containsWord(gainModes_, mode)) {
        setError("this Pluto does not offer the \"" + mode + "\" gain mode");
        return false;
    }
    if (!writeChanAttrLocked(phyDevice_, false, rxChannel_, "gain_control_mode", mode,
                             "setting the gain mode")) {
        return false;
    }
    autoGain_.store(automatic, std::memory_order_relaxed);
    return true;
}

// --- open / close ---------------------------------------------------------

bool PlutoSource::open(const std::string& args) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ != nullptr) {
        setError("this Pluto source already has a connection open");
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

    // VERSION FIRST, and it is not ceremony: something else listening on this
    // port would otherwise be discovered halfway through parsing its reply as
    // XML, and "the context is malformed" is a far worse message than "that
    // is not an IIO daemon".
    if (!control_->version(daemonVersion_)) {
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

    const iiod::Device* phy = findPhyDevice(ctx, rxLoChannel_, rxChannel_);
    if (phy == nullptr) {
        setError(
            "that IIO device has no AD9361 receiver in it (no device with an RX LO and a gain "
            "channel) - it answered, but it is not a Pluto");
        control_.reset();
        return false;
    }
    captureEnabled_.clear();
    const iiod::Device* capture = findCaptureDevice(ctx, captureEnabled_);
    // THE COUNT IS CHECKED HERE, not assumed from findCaptureDevice's own
    // rule. Everything below indexes captureEnabled_[0] and [1], and a
    // deliberate break of that rule during this file's red-green pass turned
    // into an out-of-bounds read and a silent crash with the test's output
    // still in an unflushed buffer - the same trap tests/test_hackrf_source's
    // `at()` helper exists for. An index that is checked cannot become one.
    if (capture == nullptr || captureEnabled_.size() != 2) {
        setError("that Pluto has no capture device with two input channels to stream from");
        control_.reset();
        return false;
    }
    phyDevice_ = phy->name.empty() ? phy->id : phy->name;
    captureDevice_ = capture->name.empty() ? capture->id : capture->name;
    captureChannels_ = capture->channelCount();

    // The sample layout, from the capture channels' own scan elements. Both
    // must agree: an I and a Q in different formats is not something this
    // converter can represent, and guessing which one to believe would be a
    // silently wrong picture rather than an error.
    const iiod::Channel& chI = capture->channels[captureEnabled_[0]];
    const iiod::Channel& chQ = capture->channels[captureEnabled_[1]];
    if (!iiod::parseSampleFormat(chI.scanFormat, format_)) {
        setError("this Pluto describes its samples as \"" + chI.scanFormat +
                 "\", which this driver cannot decode");
        control_.reset();
        return false;
    }
    if (chI.scanFormat != chQ.scanFormat) {
        setError("this Pluto's two capture channels disagree about their sample format (\"" +
                 chI.scanFormat + "\" and \"" + chQ.scanFormat + "\")");
        control_.reset();
        return false;
    }

    // --- what the board says about itself ------------------------------
    hwModel_ = ctx.attr("hw_model");
    hwSerial_ = ctx.attr("hw_serial");
    fwVersion_ = ctx.attr("fw_version");

    // Every read below is guarded by what the CONTEXT said the device has.
    // Asking for an attribute the board does not publish would answer -ENOENT
    // and fill the error slot with a failure that is not one, which is how a
    // perfectly good radio ends up reporting a problem in the log at open.
    std::string text;
    if (phy->hasAttr("xo_correction") &&
        readAttrLocked(phyDevice_, "xo_correction", text, "reading the crystal correction")) {
        xoCorrectionHz_ = std::strtod(text.c_str(), nullptr);
    }
    if (phy->hasAttr("rx_path_rates")) {
        readAttrLocked(phyDevice_, "rx_path_rates", rxPathRates_, "reading the path rates");
    }

    const iiod::Channel* lo = phy->findChannel(rxLoChannel_, true);
    const iiod::Channel* rx = phy->findChannel(rxChannel_, false);
    haveLoRange_ = false;
    if (lo != nullptr && lo->hasAttr("frequency_available") &&
        readChanAttrLocked(phyDevice_, true, rxLoChannel_, "frequency_available", text,
                           "reading the tuning range")) {
        haveLoRange_ = iiod::parseRange(text, loRange_);
    }
    haveRateRange_ = false;
    if (rx != nullptr && rx->hasAttr("sampling_frequency_available") &&
        readChanAttrLocked(phyDevice_, false, rxChannel_, "sampling_frequency_available", text,
                           "reading the sample-rate range")) {
        haveRateRange_ = iiod::parseRange(text, rateRange_);
    }
    haveBwRange_ = false;
    if (rx != nullptr && rx->hasAttr("rf_bandwidth_available") &&
        readChanAttrLocked(phyDevice_, false, rxChannel_, "rf_bandwidth_available", text,
                           "reading the bandwidth range")) {
        haveBwRange_ = iiod::parseRange(text, bwRange_);
    }
    haveGainRange_ = false;
    if (rx != nullptr && rx->hasAttr("hardwaregain_available") &&
        readChanAttrLocked(phyDevice_, false, rxChannel_, "hardwaregain_available", text,
                           "reading the gain range")) {
        haveGainRange_ = iiod::parseRange(text, gainRange_);
    }
    gainModes_.clear();
    if (rx != nullptr && rx->hasAttr("gain_control_mode_available") &&
        readChanAttrLocked(phyDevice_, false, rxChannel_, "gain_control_mode_available", text,
                           "reading the gain modes")) {
        gainModes_ = iiod::splitWords(text);
    }

    // --- the board's CURRENT state, which these two must answer --------
    if (!readChanAttrLocked(phyDevice_, true, rxLoChannel_, "frequency", text,
                            "reading the centre frequency")) {
        control_.reset();
        return false;
    }
    centerFrequencyHz_.store(std::strtod(text.c_str(), nullptr), std::memory_order_relaxed);

    if (!readChanAttrLocked(phyDevice_, false, rxChannel_, "sampling_frequency", text,
                            "reading the sample rate")) {
        control_.reset();
        return false;
    }
    const double rate = std::strtod(text.c_str(), nullptr);
    sampleRateHz_.store(rate, std::memory_order_relaxed);

    if (rx != nullptr && rx->hasAttr("hardwaregain") &&
        readChanAttrLocked(phyDevice_, false, rxChannel_, "hardwaregain", text,
                           "reading the gain")) {
        gainDb_.store(std::strtod(text.c_str(), nullptr), std::memory_order_relaxed);
    }
    if (rx != nullptr && rx->hasAttr("gain_control_mode") &&
        readChanAttrLocked(phyDevice_, false, rxChannel_, "gain_control_mode", text,
                           "reading the gain mode")) {
        autoGain_.store(text != "manual", std::memory_order_relaxed);
    }

    // THE ONE THING OPEN PROGRAMS. Everything else above is read and
    // reported, because a Pluto's inherited state is visible on the panel the
    // moment it is read back - but the analogue bandwidth has no control of
    // its own here, so an inherited one that does not match the rate would be
    // invisible and would quietly cost most of the span. See
    // programSampleRateLocked for the choice of width.
    if (haveBwRange_ && rate > 0.0) {
        double bw = rate;
        if (bw > bwRange_.max) { bw = bwRange_.max; }
        if (bw < bwRange_.min) { bw = bwRange_.min; }
        if (!writeChanAttrLocked(phyDevice_, false, rxChannel_, "rf_bandwidth", integerText(bw),
                                 "setting the analogue bandwidth")) {
            control_.reset();
            return false;
        }
    }

    std::string label = hwModel_.empty() ? std::string("ADALM-Pluto") : hwModel_;
    setName("Pluto: " + label + " at ip:" + host_);
    openMirror_.store(true, std::memory_order_relaxed);

    core::diagLogf(
        "pluto: opened %s at %s:%u - iiod %s, firmware \"%s\", serial %s; phy %s, capture %s "
        "(%s)",
        label.c_str(), host_.c_str(), static_cast<unsigned>(port_), daemonVersion_.c_str(),
        fwVersion_.c_str(), hwSerial_.c_str(), phyDevice_.c_str(), captureDevice_.c_str(),
        chI.scanFormat.c_str());
    if (haveLoRange_) {
        // Computed here rather than through ad9364Unlocked(), which takes
        // devMutex_ - and this whole function already holds it. A plain
        // std::mutex is not recursive, so calling the accessor would deadlock
        // every open on the success path.
        const bool unlocked =
            loRange_.min < kAd9363MinHz - 1.0 || loRange_.max > kAd9363MaxHz + 1.0;
        core::diagLogf("pluto: the board reports tuning %.3f MHz to %.3f MHz%s",
                       loRange_.min / 1e6, loRange_.max / 1e6,
                       unlocked ? " (wider than a stock AD9363 - this board has the AD9364 "
                                  "range)"
                                : " (the stock AD9363 range)");
    } else {
        core::diagLogf(
            "pluto: the board did not publish a tuning range; FoxSDR will not claim one");
    }
    if (!rxPathRates_.empty()) {
        core::diagLogf("pluto: rx path rates %s", rxPathRates_.c_str());
    }
    return true;
}

void PlutoSource::closeDevice() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
    if (control_ != nullptr) {
        control_->close();
        control_.reset();
    }
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    setName("ADALM-Pluto: (not connected)");
}

std::string PlutoSource::hardwareModel() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return hwModel_;
}

std::string PlutoSource::firmwareVersion() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return fwVersion_;
}

std::string PlutoSource::daemonVersion() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return daemonVersion_;
}

std::string PlutoSource::serialNumber() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return hwSerial_;
}

double PlutoSource::xoCorrectionHz() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return xoCorrectionHz_;
}

std::string PlutoSource::rxPathRates() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return rxPathRates_;
}

bool PlutoSource::ad9364Unlocked() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!haveLoRange_) { return false; }
    return loRange_.min < kAd9363MinHz - 1.0 || loRange_.max > kAd9363MaxHz + 1.0;
}

std::string PlutoSource::phyDeviceName() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return phyDevice_;
}

std::string PlutoSource::captureDeviceName() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return captureDevice_;
}

// --- streaming ------------------------------------------------------------

bool PlutoSource::startStreamingLocked() {
    std::string error;
    std::unique_ptr<iiod::Transport> transport =
        iiod::connectTcp(host_, port_, iiod::kConnectWait, iiod::kReplyWait, error);
    if (transport == nullptr) {
        // A SECOND CONNECTION IS NOT A SECOND RADIO. Failing to make it means
        // the board has gone since open(), not that the address is wrong, so
        // it is reported as a fault rather than as a bad argument.
        noteFault("opening a second connection for the samples", error);
        return false;
    }
    std::unique_ptr<iiod::Client> stream(new iiod::Client(std::move(transport)));

    // The daemon's own timeout on the HARDWARE, distinct from our socket
    // bound: without it a board that stops producing leaves the daemon
    // waiting for a buffer for ever and our READBUF unanswered until the
    // socket gives up. Ours is the shorter of the two by design, so the
    // failure we see is our own bound and not a silent stall.
    stream->setTimeoutMs(static_cast<long>(iiod::kReplyWait.count() * 2));

    // BUFFERS_COUNT BEFORE OPEN - see the header, and ops.c
    // create_buf_and_blocks, which reads the count only when it makes the
    // buffer, which OPEN is what triggers.
    if (!stream->setBuffersCount(captureDevice_, kBuffersCount)) {
        noteFault("setting the number of sample buffers", stream->lastError());
        return false;
    }
    const std::string mask = iiod::channelMask(captureEnabled_, captureChannels_);
    if (!stream->openBuffer(captureDevice_, kSamplesPerBuffer, mask)) {
        noteFault("opening the capture device", stream->lastError());
        return false;
    }

    const std::size_t storage = format_.storageBytes();
    link_->captureDevice = captureDevice_;
    link_->bufferBytes = kSamplesPerBuffer * captureEnabled_.size() * storage;
    link_->format = format_;
    stream_ = std::move(stream);
    link_->stream = stream_.get();
    {
        std::lock_guard<std::mutex> lk(link_->waitMutex);
        link_->exited = false;
    }
    link_->run.store(true, std::memory_order_relaxed);
    reader_ = std::thread(&PlutoSource::readerThreadBody, link_);
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void PlutoSource::stopStreamingLocked() {
    if (!reader_.joinable() && !running_.load(std::memory_order_relaxed)) {
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

    if (reader_.joinable()) {
        // A BOUNDED JOIN, not a plain one (see the file header). The reader
        // sets `exited` as its last act, so a flag still clear after
        // kReaderJoinWait means a thread parked somewhere it is not coming
        // back from - and waiting for it on the GUI thread would be the hang
        // the bounds exist to avoid. The ordinary exit costs one buffer
        // period; the worst legitimate one costs iiod::kReplyWait, which is
        // why this bound is longer than that and not shorter.
        bool exited = false;
        {
            std::unique_lock<std::mutex> lk(link_->waitMutex);
            exited = link_->waitCv.wait_for(lk, kReaderJoinWait, [this] { return link_->exited; });
        }
        if (exited) {
            reader_.join();
        } else {
            // ABANDONED. The thread keeps its copy of the link and the stream
            // pointer inside it, so NEITHER may be disturbed: the client is
            // leaked deliberately rather than destroyed under a thread still
            // inside its socket, and link_->stream is left pointing at it for
            // the same reason. This source never streams again - the fault
            // condemns it, and start() refuses on a dead device.
            g_readersAbandoned.fetch_add(1, std::memory_order_relaxed);
            noteFault("waiting for the sample reader to stop",
                      "the reader did not return; the connection is left to the operating "
                      "system and FoxSDR must be restarted to use this board again");
            reader_.detach();
            (void)stream_.release();
            running_.store(false, std::memory_order_relaxed);
            return;
        }
    }

    // Only now, with no thread of ours inside it, is the stream connection
    // safe to speak on. CLOSE is the polite half - the daemon frees the
    // buffer at once rather than when it notices the socket has gone (ops.c
    // ascii_interpreter closes every open device when the connection drops,
    // so dropping it alone would be correct, just slower) - and it is skipped
    // on a dead board, where it could only add one more timeout to a teardown
    // that has already failed.
    if (stream_ != nullptr) {
        if (!deviceDead()) { stream_->closeBuffer(captureDevice_); }
        stream_->close();
        stream_.reset();
        link_->stream = nullptr;
    }
    running_.store(false, std::memory_order_relaxed);
}

bool PlutoSource::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("start() called with no Pluto open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running_.load(std::memory_order_relaxed)) { return true; }
    clearError();
    return startStreamingLocked();
}

void PlutoSource::stop() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
}

// --- the reader thread ----------------------------------------------------

void PlutoSource::readerThreadBody(std::shared_ptr<ReaderLink> link) {
    const std::size_t storage = link->format.storageBytes();
    const std::size_t bytesPerSample = 2 * storage;  // I and Q, interleaved
    const std::size_t samplesPerBuffer =
        bytesPerSample == 0 ? 0 : link->bufferBytes / bytesPerSample;
    std::vector<std::uint8_t> raw;
    std::vector<std::complex<float>> conv(samplesPerBuffer);

    while (link->run.load(std::memory_order_relaxed)) {
        const bool ok = link->stream->readBuf(link->captureDevice, link->bufferBytes, raw);
        if (!link->run.load(std::memory_order_relaxed)) { break; }

        if (!ok) {
            // A FAILED READBUF IS THE BOARD GOING. There is nothing to retry:
            // the connection is out of step or gone, and every further read
            // would fail the same way while the source loop waited for
            // samples that cannot come. Fault, and leave - the loop polls
            // faulted() and stops with this message.
            noteRead(*link, false, 0, false);
            noteFaultOn(*link, "reading samples", link->stream->lastError());
            break;
        }

        const std::size_t samples = std::min(samplesPerBuffer, raw.size() / bytesPerSample);
        bool dropped = false;
        if (samples > 0) {
            for (std::size_t i = 0; i < samples; ++i) {
                const std::uint8_t* word = raw.data() + i * bytesPerSample;
                conv[i] = std::complex<float>(iiod::convertSample(word, link->format),
                                              iiod::convertSample(word + storage, link->format));
            }
            const std::size_t written = link->ring.write(conv.data(), samples);
            if (written != samples) {
                // The host fell behind, not the radio. Counted rather than
                // silently tolerated: a spectrum with a gap in it and no
                // number anywhere is how a slow machine looks exactly like a
                // broken one.
                dropped = true;
                link->dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        noteRead(*link, true, samples, dropped);
        if (samples == 0) { continue; }
        {
            // Taken and released so a read() that has just tested the ring
            // and is about to wait cannot miss this notify in between.
            std::lock_guard<std::mutex> lk(link->waitMutex);
        }
        link->waitCv.notify_all();
    }

    // LAST ACT, and the thing stopStreamingLocked's bounded join waits for.
    {
        std::lock_guard<std::mutex> lk(link->waitMutex);
        link->run.store(false, std::memory_order_relaxed);
        link->exited = true;
    }
    link->waitCv.notify_all();
}

void PlutoSource::noteRead(ReaderLink& link, bool ok, std::size_t samples, bool dropped) {
    const auto now = std::chrono::steady_clock::now();
    std::string line;
    bool warn = false;
    {
        std::lock_guard<std::mutex> lk(link.healthMutex);
        StreamHealth& h = link.health;
        if (!h.windowOpen) {
            h.windowOpen = true;
            h.windowStart = now;
            h.lastSamples = now;
        }
        ++h.reads;
        if (ok && samples > 0) {
            ++h.withSamples;
            h.samples += samples;
            const auto gap =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - h.lastSamples).count();
            if (gap > h.longestGapMs) { h.longestGapMs = gap; }
            h.lastSamples = now;
        } else if (ok) {
            // A buffer that carried nothing. The daemon does not answer an
            // empty one on the success path, so this is counted in the same
            // column the USB drivers count an expired bulk read in - present
            // for the format's sake, and expected to stay zero.
            ++h.timeouts;
        } else {
            ++h.errors;
        }
        if (dropped) { ++h.overflows; }

        if (now - h.windowStart < link.healthWindow) { return; }

        const bool nominal = h.timeouts == 0 && h.overflows == 0 && h.errors == 0 &&
                             h.longestGapMs < 250;
        const bool first = !link.healthEverWritten;
        warn = h.errors > 0 || h.longestGapMs >= 1000;
        line = healthLineLocked(link);
        if (line.empty()) { return; }
        if (!(warn || first || !nominal)) { return; }
        link.healthEverWritten = true;
    }
    if (warn) {
        core::diagWarnf("%s", line.c_str());
    } else {
        core::diagLogf("%s", line.c_str());
    }
}

std::string PlutoSource::healthLineLocked(ReaderLink& link) {
    StreamHealth& h = link.health;
    if (!h.windowOpen || h.reads == 0) { return std::string(); }
    const auto now = std::chrono::steady_clock::now();
    const auto openGap =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - h.lastSamples).count();
    if (openGap > h.longestGapMs) { h.longestGapMs = openGap; }
    const auto windowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - h.windowStart).count();
    char buf[192];
    // THE SAME FORMAT SoapySource::streamHealthLine WRITES, word for word. A
    // reader of the diagnostic log should not have to know which driver was
    // open to parse the line.
    std::snprintf(buf, sizeof(buf),
                  "source: stream health - reads %llu, with samples %llu, timeouts %llu, "
                  "overflows %llu, errors %llu, longest gap %lld ms, %llu samples in %lld s",
                  static_cast<unsigned long long>(h.reads),
                  static_cast<unsigned long long>(h.withSamples),
                  static_cast<unsigned long long>(h.timeouts),
                  static_cast<unsigned long long>(h.overflows),
                  static_cast<unsigned long long>(h.errors),
                  static_cast<long long>(h.longestGapMs),
                  static_cast<unsigned long long>(h.samples),
                  static_cast<long long>((windowMs + 500) / 1000));
    h = StreamHealth{};
    return std::string(buf);
}

std::string PlutoSource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void PlutoSource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

std::uint64_t PlutoSource::droppedBuffers() const {
    return link_->dropped.load(std::memory_order_relaxed);
}

unsigned long long PlutoSource::readersAbandoned() {
    return g_readersAbandoned.load(std::memory_order_relaxed);
}

// --- read -----------------------------------------------------------------

std::size_t PlutoSource::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) { return 0; }
    std::size_t got = link_->ring.read(dst, n);
    if (got > 0) { return got; }
    if (faulted()) { return 0; }
    {
        // Bounded, and short. The pipeline's self-paced loop treats a zero as
        // "nothing yet" and backs off a millisecond of its own.
        std::unique_lock<std::mutex> lk(link_->waitMutex);
        link_->waitCv.wait_for(lk, kReadWait, [this] {
            return link_->ring.size() > 0 || !link_->run.load(std::memory_order_relaxed);
        });
    }
    got = link_->ring.read(dst, n);
    return got;
}

// --- rate, frequency, gains ----------------------------------------------

std::vector<double> PlutoSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!haveRateRange_) { return {}; }
    // A MENU BUILT FROM THE BOARD'S RANGE, not a table. The AD9361 takes any
    // rate between the ends, so these are the round numbers a panel can offer
    // plus the two ends themselves - the ends because they are exactly what a
    // user who wants "as wide as it goes" is looking for, and rounding them
    // away would put a rate on the menu the board would then coerce.
    const double candidates[] = {2.5e6,  3.0e6,  4.0e6,  5.0e6,  6.0e6,  8.0e6,  10.0e6,
                                 12.0e6, 15.0e6, 20.0e6, 25.0e6, 30.72e6, 40.0e6, 50.0e6,
                                 61.44e6};
    std::vector<double> out;
    out.push_back(rateRange_.min);
    for (const double c : candidates) {
        if (c > rateRange_.min && c < rateRange_.max) { out.push_back(c); }
    }
    if (rateRange_.max > rateRange_.min) { out.push_back(rateRange_.max); }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool PlutoSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("setSampleRateHz() called with no Pluto open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz > 0.0)) {  // negated compare so a NaN lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }

    double want = hz;
    if (haveRateRange_) {
        if (hz < rateRange_.min) {
            // REFUSED, not raised. Below the board's own minimum the AD9361
            // needs a FIR filter loaded through filter_fir_config, which this
            // driver does not generate - and quietly giving a caller that
            // asked for 1 MS/s two and a half times the bandwidth it wanted
            // would be a lie nothing on screen could reveal.
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "this Pluto's lowest sample rate is %.6f MS/s; %.6f MS/s was refused "
                          "(going lower needs a FIR filter written to the AD9361's "
                          "filter_fir_config, which FoxSDR does not do)",
                          rateRange_.min / 1e6, hz / 1e6);
            setError(buf);
            return false;
        }
        if (hz > rateRange_.max) {
            // Above the top the rate IS coerced, because a caller asking for
            // more bandwidth than the board has can be given all of it and
            // told so.
            want = rateRange_.max;
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "this Pluto's highest sample rate is %.6f MS/s; %.6f MS/s was coerced "
                          "to it",
                          rateRange_.max / 1e6, hz / 1e6);
            setError(buf);
        }
    }

    const bool wasRunning = running_.load(std::memory_order_relaxed);
    if (wasRunning) {
        // A QUIET RADIO for the change (see the header): the reader is joined
        // and the capture device CLOSEd before the clock underneath it moves.
        stopStreamingLocked();
    }
    if (!programSampleRateLocked(want)) {
        if (wasRunning && !deviceDead()) { startStreamingLocked(); }
        return false;
    }
    if (wasRunning) { return startStreamingLocked(); }
    return true;
}

bool PlutoSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("setCenterFrequencyHz() called with no Pluto open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (haveLoRange_ && !(hz >= loRange_.min && hz <= loRange_.max)) {
        char buf[208];
        std::snprintf(buf, sizeof(buf),
                      "this Pluto tunes %.3f MHz to %.3f MHz; %.6f MHz is outside that",
                      loRange_.min / 1e6, loRange_.max / 1e6, hz / 1e6);
        setError(buf);
        return false;
    }
    if (!(hz > 0.0)) {
        setError("setCenterFrequencyHz() requires a positive frequency");
        return false;
    }
    return programFrequencyLocked(hz);
}

bool PlutoSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!haveLoRange_) { return false; }
    loHz = loRange_.min;
    hiHz = loRange_.max;
    return true;
}

std::vector<GainInfo> PlutoSource::gains() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!haveGainRange_) { return {}; }
    // REAL DECIBELS, read off the board. The AD9361's RX hardwaregain is a
    // published dB figure (it varies with band, which is why the range is
    // read rather than tabulated), not a register index like the Airspy
    // R2/Mini's five - so it is lettered dB, and GainUnit::Decibels is the
    // truth rather than the default.
    GainInfo g;
    g.name = "RX";
    g.minDb = gainRange_.min;
    g.maxDb = gainRange_.max;
    g.stepDb = gainRange_.step > 0.0 ? gainRange_.step : 1.0;
    g.unit = GainUnit::Decibels;
    return {g};
}

bool PlutoSource::setGainDb(const std::string& gainName, double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("setGainDb() called with no Pluto open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (gainName != "RX") {
        setError("the Pluto has no gain called \"" + gainName + "\"");
        return false;
    }
    // A HAND-SET GAIN TURNS THE AGC OFF FIRST, in that order and
    // deliberately. The AD9361 ignores hardwaregain while it is in an attack
    // mode, so a slider moved with the AGC on would move on screen, change
    // nothing, and read back as whatever the AGC had chosen - which looks
    // exactly like a broken slider. Refusing instead would be defensible but
    // worse: the user's intent when they move a gain control is manual gain.
    if (autoGain_.load(std::memory_order_relaxed)) {
        if (!programGainModeLocked(false)) { return false; }
        core::diagLogf("pluto: automatic gain control switched off because the gain was set by "
                       "hand");
    }
    return programGainLocked(db);
}

double PlutoSource::gainDb(const std::string& gainName) const {
    if (gainName == "RX") { return gainDb_.load(std::memory_order_relaxed); }
    return 0.0;
}

bool PlutoSource::setAutoGain(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("setAutoGain() called with no Pluto open");
        return false;
    }
    if (deviceDead()) { return false; }
    return programGainModeLocked(on);
}

bool PlutoSource::setAntenna(const std::string& antennaName) {
    if (antennaName == "A_BALANCED") { return true; }
    setError("the Pluto has one receive port, \"A_BALANCED\"");
    return false;
}

}  // namespace cascade::source
