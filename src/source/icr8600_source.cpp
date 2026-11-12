#include "source/icr8600_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace cascade::source {

namespace ic = icr8600;

namespace {

constexpr int kPollMs = 100;           // plain ints: the shutdown-budget scan classifies named chrono waits per file
constexpr int kReenumerateTries = 50;  // x 100 ms: the README waits up to 5 s for 0C26:0023
constexpr int kReenumerateStepMs = 100;
constexpr std::size_t kRingBufferBytes = 64 * 1024;
constexpr std::size_t kRingBuffers = 16;

std::string hex4(unsigned v) {
    char b[8];
    std::snprintf(b, sizeof(b), "%04X", v);
    return b;
}

}  // namespace

std::vector<cascade::usb::UsbId> icr8600UsbIds() { return {{ic::kVid, ic::kPidStream}, {ic::kVid, ic::kPidLoader}}; }

// ---------------------------------------------------------------------------------------
// the connection: the device, the I/Q ring's reader state
// ---------------------------------------------------------------------------------------

struct Icr8600Source::Conn {
    std::unique_ptr<cascade::usb::UsbDevice> dev;
    bool streamBegun = false;
    ic::IqAligner aligner;
    std::mutex alignMutex;
    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(kRingBufferBytes);
    std::vector<std::complex<float>> out;
    int silentTicks = 0, silentLimit = 30;

    // Runs when the last holder lets go - the source, or an abandoned reader that has since returned. By then no
    // thread is inside readBulk (usb_device.hpp: the driver joins its reader first).
    ~Conn() {
        if (dev && streamBegun) { dev->endBulkStream(); }
    }

    bool pump(NetLink& link) {
        const int n = dev->readBulk(buf.data(), buf.size(), kPollMs);
        const bool streaming = link.streaming.load(std::memory_order_relaxed);
        if (!link.run.load(std::memory_order_relaxed)) { return false; }
        if (n < 0) {
            link.fault("the IC-R8600 I/Q port failed", "bulk read error");
            return false;
        }
        bool got = false;
        if (n > 0) {
            out.clear();
            {
                std::lock_guard<std::mutex> lk(alignMutex);
                aligner.feed(buf.data(), static_cast<std::size_t>(n), out);
            }
            if (!out.empty()) {
                got = true;
                if (streaming) { link.pushSamples(out.data(), out.size()); }
            }
        }
        if (!streaming || got) {
            silentTicks = 0;
        } else if (n == 0 && ++silentTicks >= silentLimit) {
            link.fault("the IC-R8600 stopped sending I/Q", "no I/Q data arrived");
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

Icr8600Source::~Icr8600Source() { closeDevice(); }

void Icr8600Source::setTransportForTest(EnumFn enumerate, UsbOpenFn opener) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    enumFn_ = std::move(enumerate);
    openFn_ = std::move(opener);
}

std::vector<cascade::usb::UsbDeviceInfo> Icr8600Source::enumerateAll() const {
    if (enumFn_) { return enumFn_(); }
    return cascade::usb::enumerateWinUsb(icr8600UsbIds());
}

bool Icr8600Source::failOpen(const std::string& why) {
    const std::string keep = why;
    teardown();
    setError(keep);
    return false;
}

void Icr8600Source::teardown() {
    active_ = false;
    markOpen(false);
    endReader();
    std::lock_guard<std::mutex> cl(connMutex_);
    conn_.reset();
}

bool Icr8600Source::civ(const std::vector<std::uint8_t>& frame, const char* what, bool optional) {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (c == nullptr || !c->dev) { setError("no IC-R8600 connection is open"); return false; }
    if (frame.empty()) { setError(std::string("the command for ") + what + " could not be built"); return false; }
    std::lock_guard<std::mutex> lk(civMutex_);
    const int w = c->dev->writeBulk(ic::kEpCommand, frame.data(), frame.size(), static_cast<unsigned>(ackMs_));
    if (w < 0 || static_cast<std::size_t>(w) != frame.size()) {
        setError(std::string("could not send the command for ") + what + " to the IC-R8600");
        return false;
    }
    std::uint8_t ack[64];
    const int r = c->dev->readBulkOnce(ic::kEpAnswer, ack, sizeof(ack), static_cast<unsigned>(ackMs_));
    if (r < 0) {
        setError(std::string("the IC-R8600 answer for ") + what + " could not be read");
        return false;
    }
    if (r == 0) {
        setError(std::string("the IC-R8600 did not answer the command for ") + what);
        return false;
    }
    const ic::Answer a = ic::parseAnswer(ack, static_cast<std::size_t>(r));
    if (a == ic::Answer::Ok) { return true; }
    if (a == ic::Answer::Refused) {
        if (optional) {
            ++refusedOptional_;
            return true;
        }
        setError(std::string("the IC-R8600 refused the command for ") + what);
        return false;
    }
    setError(std::string("the IC-R8600 gave an answer to the command for ") + what + " that is not an acknowledgement");
    return false;
}

bool Icr8600Source::loadFirmware(const std::string& path, const cascade::usb::UsbDeviceInfo& loader, std::string& error) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        error = "the firmware file could not be opened";
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size <= 0 || size > 4 * 1024 * 1024) {
        error = "the firmware file is empty or far too large to be the IC-R8600's";
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!f) {
        error = "the firmware file could not be read";
        return false;
    }
    std::vector<ic::FwRecord> recs;
    if (!ic::parseFirmware(bytes, recs, error)) { return false; }
    std::string openErr;
    std::unique_ptr<cascade::usb::UsbDevice> dev = openFn_ ? openFn_(loader.path, openErr) : cascade::usb::openWinUsb(loader.path, openErr);
    if (!dev) {
        error = openErr.empty() ? std::string("the IC-R8600 loader (0C26:0022) could not be opened") : openErr;
        return false;
    }
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const int r = dev->controlOut(cascade::usb::kRequestTypeVendorOut, 0xA0, recs[i].wValue, 0, recs[i].data.data(),
                                      recs[i].data.size(), 2000);
        // the last record releases the CPU, so the device drops off the bus and the transfer may report an error
        if (r < 0 && i + 1 != recs.size()) {
            error = "firmware record " + std::to_string(i + 1) + " of " + std::to_string(recs.size()) + " was not accepted by the radio";
            return false;
        }
    }
    return true;
}

bool Icr8600Source::open(const std::string& args) {
    closeDevice();
    std::lock_guard<std::mutex> lk(ctlMutex_);
    ackMs_ = 1500;
    const std::string to = argValue(args, "timeout");
    if (!to.empty()) {
        char* e = nullptr;
        const long v = std::strtol(to.c_str(), &e, 10);
        if (e == to.c_str() || *e != '\0' || v < 100 || v > 10000) { setError("the timeout must be 100 to 10000 ms"); return false; }
        ackMs_ = static_cast<int>(v);
    }
    const std::string fwPath = argValue(args, "firmware");

    // --- find the port ---------------------------------------------------------------------------------------
    cascade::usb::UsbDeviceInfo stream, loader;
    bool haveStream = false, haveLoader = false;
    auto scan = [&]() {
        haveStream = haveLoader = false;
        for (const cascade::usb::UsbDeviceInfo& d : enumerateAll()) {
            if (d.vid != ic::kVid) { continue; }
            if (d.pid == ic::kPidStream && !haveStream) { stream = d; haveStream = true; }
            if (d.pid == ic::kPidLoader && !haveLoader) { loader = d; haveLoader = true; }
        }
    };
    scan();
    if (!haveStream && !haveLoader) {
        setError(std::string("no IC-R8600 I/Q port found (is the radio's [I/Q OUT] USB cable plugged straight into the PC, and ") +
                 cascade::usb::kBindHint + " to it?)");
        return false;
    }
    if (!haveStream) {
        // the port is waiting for its firmware
        if (fwPath.empty()) {
            setError("the IC-R8600 I/Q port is waiting for Icom's firmware (0C26:0022). Install Icom's \"USB I/Q Package for HDSDR\", "
                     "which loads it, or give firmware=<the file IC-R8600_usb_iq.spt from that package>. FoxSDR does not include or "
                     "download Icom's firmware.");
            return false;
        }
        std::string err;
        if (!loadFirmware(fwPath, loader, err)) { setError(err); return false; }
        for (int i = 0; i < kReenumerateTries && !haveStream; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kReenumerateStepMs));
            scan();
        }
        if (!haveStream) {
            setError("the IC-R8600 did not come back as 0C26:0023 within 5 seconds after its firmware was loaded "
                     "(is WinUSB bound to 0C26:0023 too?)");
            return false;
        }
    }
    std::string err;
    std::unique_ptr<cascade::usb::UsbDevice> dev = openFn_ ? openFn_(stream.path, err) : cascade::usb::openWinUsb(stream.path, err);
    if (!dev) {
        setError(err.empty() ? std::string("the IC-R8600 I/Q port could not be opened") : err);
        return false;
    }
    auto conn = std::make_shared<Conn>();
    conn->dev = std::move(dev);
    conn->silentLimit = std::max(5, ackMs_ * 2 / kPollMs);
    if (!conn->dev->beginBulkStream(ic::kEpIq, kRingBufferBytes, kRingBuffers)) {
        setError("the IC-R8600 I/Q pipe could not be started: " + conn->dev->lastError());
        return false;
    }
    conn->streamBegun = true;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        conn_ = conn;
    }
    if (!beginReader([conn](NetLink& l) { return conn->pump(l); }, []() {}, conn)) {
        std::lock_guard<std::mutex> cl(connMutex_);
        conn_.reset();
        return false;
    }
    // --- the radio answers CI-V and goes into I/Q mode, output off ---------------------------------------------
    if (!civ(ic::cmdIqOff(), "switching the I/Q output off", false) || !civ(ic::cmdIqModeOn(), "entering I/Q mode", false)) {
        return failOpen("this does not look like an IC-R8600 I/Q port: " + std::string(lastError()));
    }
    setError("");
    refusedOptional_ = 0;
    markOpen(true);
    setName("Icom IC-R8600 I/Q (USB " + hex4(ic::kVid) + ":" + hex4(ic::kPidStream) + ")");
    return true;
}

void Icr8600Source::closeDevice() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (c) {
        // the README's shutdown: I/Q OFF, then leave I/Q mode (best effort: the cable may be gone)
        if (active_) { civ(ic::cmdIqOff(), "switching the I/Q output off", true); }
        civ(ic::cmdIqModeOff(), "leaving I/Q mode", true);
    }
    c.reset();
    teardown();
    setName("network radio: (not connected)");
}

bool Icr8600Source::start() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("start() called with no radio open"); return false; }
    if (faulted()) { return false; }
    if (active_) { return true; }
    // the README's order, captured against HDSDR: frequency, attenuator, antenna, RF gain, pre-amp, IP+, prepare, ON
    if (!civ(ic::cmdFrequency(freqHz_), "tuning", false)) { return false; }
    if (!civ(ic::cmdAttenuator(attenuatorDb_), "the attenuator", true)) { return false; }
    if (!civ(ic::cmdAntenna(antennaIndex_), "the antenna", true)) { return false; }  // NG outside HF
    if (!civ(ic::cmdRfGain(rfGain_), "the RF gain", false)) { return false; }
    if (!civ(ic::cmdPreamp(preamp_), "the pre-amp", true)) { return false; }
    if (!civ(ic::cmdIpPlusOff(), "IP+", true)) { return false; }
    if (!civ(ic::cmdIqPrepare(), "preparing the I/Q output", true)) { return false; }
    {
        std::shared_ptr<Conn> c;
        {
            std::lock_guard<std::mutex> cl(connMutex_);
            c = conn_;
        }
        if (c) {
            std::lock_guard<std::mutex> al(c->alignMutex);
            c->aligner.reset();  // a new stream starts unlocked: nothing before its first marker is trusted
        }
    }
    setError("");
    if (!startKeeping()) { return false; }
    if (!civ(ic::cmdIqOn(rateCode_), "switching the I/Q output on", false)) {
        stopKeeping();
        return false;
    }
    active_ = true;
    return true;
}

void Icr8600Source::stop() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    stopKeeping();
    if (active_) { civ(ic::cmdIqOff(), "switching the I/Q output off", true); }
    active_ = false;
}

double Icr8600Source::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    for (const ic::RateRow& r : ic::kRates) {
        if (r.code == rateCode_) { return r.hz; }
    }
    return 0.0;
}

bool Icr8600Source::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setSampleRateHz() called with no radio open"); return false; }
    if (active_) { setError("stop the radio before changing its sample rate"); return false; }
    std::uint8_t code = 0;
    if (!std::isfinite(hz) || !ic::rateCodeFor(hz, code)) {
        setError("the IC-R8600 takes 240, 480, 960 kS/s or 1.92, 3.84 or 5.12 MS/s");
        return false;
    }
    rateCode_ = code;
    return true;
}

std::vector<double> Icr8600Source::supportedSampleRatesHz() const {
    std::vector<double> v;
    for (const ic::RateRow& r : ic::kRates) { v.push_back(r.hz); }
    return v;
}

double Icr8600Source::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return static_cast<double>(freqHz_);
}

bool Icr8600Source::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setCenterFrequencyHz() called with no radio open"); return false; }
    if (!(hz >= 0.0) || !std::isfinite(hz) || hz > 9999999999.0) { setError("setCenterFrequencyHz() requires a frequency of 0 to 9999999999 Hz"); return false; }
    const std::uint64_t f = static_cast<std::uint64_t>(std::llround(hz));
    if (active_ && !civ(ic::cmdFrequency(f), "tuning", false)) { return false; }
    freqHz_ = f;
    return true;
}

std::vector<GainInfo> Icr8600Source::gains() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    std::vector<GainInfo> v;
    if (!isOpen()) { return v; }
    GainInfo rf;  // the radio's own 0..255 code: not decibels
    rf.name = "RF gain";
    rf.minDb = 0.0;
    rf.maxDb = 255.0;
    rf.stepDb = 1.0;
    rf.unit = GainUnit::Steps;
    v.push_back(rf);
    GainInfo att;
    att.name = "Attenuator";
    att.minDb = -30.0;
    att.maxDb = 0.0;
    att.stepDb = 10.0;
    v.push_back(att);
    GainInfo pre;
    pre.name = "Pre-amp";
    pre.minDb = 0.0;
    pre.maxDb = 1.0;
    pre.stepDb = 1.0;
    pre.unit = GainUnit::Steps;
    v.push_back(pre);
    return v;
}

bool Icr8600Source::setGainDb(const std::string& name, double v) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (name != "RF gain" && name != "Attenuator" && name != "Pre-amp") {
        setError("this radio has three gains: \"RF gain\", \"Attenuator\" and \"Pre-amp\"");
        return false;
    }
    if (!isOpen()) { setError("setGainDb() called with no radio open"); return false; }
    if (!std::isfinite(v)) { setError("that is not a gain"); return false; }
    const long n = std::lround(v);
    if (std::fabs(v - static_cast<double>(n)) > 0.01) { setError("that gain must be a whole number"); return false; }
    if (name == "RF gain") {
        if (n < 0 || n > 255) { setError("the RF gain code is 0 to 255"); return false; }
        if (active_ && !civ(ic::cmdRfGain(static_cast<unsigned>(n)), "the RF gain", false)) { return false; }
        rfGain_ = static_cast<unsigned>(n);
    } else if (name == "Attenuator") {
        if (n != 0 && n != -10 && n != -20 && n != -30) { setError("the attenuator takes 0, -10, -20 or -30 dB"); return false; }
        if (active_ && !civ(ic::cmdAttenuator(static_cast<unsigned>(-n)), "the attenuator", false)) { return false; }
        attenuatorDb_ = static_cast<unsigned>(-n);
    } else {
        if (n != 0 && n != 1) { setError("the pre-amp is 0 (off) or 1 (on)"); return false; }
        if (active_ && !civ(ic::cmdPreamp(n == 1), "the pre-amp", false)) { return false; }
        preamp_ = n == 1;
    }
    return true;
}

double Icr8600Source::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (name == "RF gain") { return rfGain_; }
    if (name == "Attenuator") { return -static_cast<double>(attenuatorDb_); }
    if (name == "Pre-amp") { return preamp_ ? 1.0 : 0.0; }
    return 0.0;
}

std::vector<std::string> Icr8600Source::antennas() const { return {"ANT 1", "ANT 2", "ANT 3"}; }

bool Icr8600Source::setAntenna(const std::string& name) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    unsigned idx = 99;
    if (name == "ANT 1") { idx = 0; }
    else if (name == "ANT 2") { idx = 1; }
    else if (name == "ANT 3") { idx = 2; }
    if (idx > 2) { setError("the antenna is ANT 1, ANT 2 or ANT 3"); return false; }
    if (!isOpen()) { setError("setAntenna() called with no radio open"); return false; }
    // SoapyICR8600: the antenna command only works on HF (the radio answers NG elsewhere, ANT 1 being automatic)
    if (freqHz_ >= 30000000ull && idx != 0) {
        setError("ANT 2 and ANT 3 can be chosen on HF only (below 30 MHz); above it the radio uses ANT 1");
        return false;
    }
    if (active_ && !civ(ic::cmdAntenna(idx), "the antenna", false)) { return false; }
    antennaIndex_ = idx;
    return true;
}

std::string Icr8600Source::antenna() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return std::string("ANT ") + std::to_string(antennaIndex_ + 1);
}

std::uint64_t Icr8600Source::syncMarkers() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (!c) { return 0; }
    std::lock_guard<std::mutex> lk(c->alignMutex);
    return c->aligner.markers();
}
std::uint64_t Icr8600Source::realignments() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (!c) { return 0; }
    std::lock_guard<std::mutex> lk(c->alignMutex);
    return c->aligner.realigned();
}
std::uint64_t Icr8600Source::refusedOptional() const { return refusedOptional_.load(); }

}  // namespace cascade::source
