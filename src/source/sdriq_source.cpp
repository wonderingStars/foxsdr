#include "source/sdriq_source.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <map>

namespace cascade::source {

namespace as = ascp;
namespace sq = sdriq;

namespace {

constexpr int kPollMs = 100;  // plain ints: the shutdown-budget scan classifies named chrono waits per file

std::string tame(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (r.size() >= 40) { r += "..."; break; }
        r += (c >= 32 && c < 127) ? c : '?';
    }
    return r;
}

}  // namespace

std::unique_ptr<SdriqLink> defaultSdriqOpener(const std::string&, std::string& error) {
    error =
        "the SDR-IQ talks through FTDI's D2XX driver, which this build does not include "
        "(install FTDI's driver and use a build that has the D2XX link)";
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// the connection, shared with the reader thread
// ---------------------------------------------------------------------------------------

struct SdriqSource::Conn {
    std::shared_ptr<SdriqLink> link;
    std::mutex sendMutex;
    std::mutex mu;
    std::condition_variable cv;
    std::map<std::uint32_t, std::vector<std::uint8_t>> replies;
    std::map<std::uint32_t, std::uint64_t> replySeq;
    std::uint64_t nakSeq = 0;
    std::atomic<std::uint64_t> overload{0}, blocks{0}, other{0};
    as::StreamAssembler asmb;
    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(16384);
    std::vector<std::complex<float>> out;
    int silentTicks = 0, silentLimit = 30;

    ~Conn() {
        if (link) { link->close(); }
    }

    bool sendBytes(const std::vector<std::uint8_t>& m, std::string& err) {
        if (m.empty()) { err = "the message is too long"; return false; }
        std::lock_guard<std::mutex> lk(sendMutex);
        return link->write(m.data(), m.size(), err);
    }

    void dispatch(const as::Message& m) {
        std::lock_guard<std::mutex> lk(mu);
        if (m.nak) {
            ++nakSeq;
        } else if (m.type == as::MsgType::Set || m.type == as::MsgType::RequestRange) {
            const std::uint32_t key = (static_cast<std::uint32_t>(m.type) << 16) | m.item;
            replies[key] = m.body;
            ++replySeq[key];
        } else if (m.type == as::MsgType::Request) {  // target to host, type 1: unsolicited
            if (m.item == sq::ci::Status) {
                for (std::uint8_t b : m.body) {
                    if (b == sq::kStatusOverload) { ++overload; }
                }
            }
        }
        cv.notify_all();
    }

    bool pump(NetLink& link_) {
        const int n = link->read(buf.data(), buf.size(), kPollMs);
        const bool streaming = link_.streaming.load(std::memory_order_relaxed);
        if (!link_.run.load(std::memory_order_relaxed)) { return false; }
        if (n < 0) {
            link_.fault("the SDR-IQ connection failed", "read error");
            return false;
        }
        bool gotData = false;
        if (n > 0) {
            std::vector<as::Message> msgs;
            asmb.feed(buf.data(), static_cast<std::size_t>(n), msgs);
            for (const as::Message& m : msgs) {
                if (m.type == as::MsgType::Data0 && !m.nak) {
                    out.clear();
                    if (sq::decodeBlock(m.body, out)) {
                        ++blocks;
                        gotData = true;
                        if (streaming) { link_.pushSamples(out.data(), out.size()); }
                    } else {
                        ++other;  // a data block of the wrong size is not believed
                    }
                } else if (static_cast<unsigned>(m.type) >= 3) {
                    ++other;
                } else {
                    dispatch(m);
                }
            }
            if (!asmb.error().empty()) {
                link_.fault("the SDR-IQ sent a malformed message", asmb.error());
                return false;
            }
        }
        if (!streaming || gotData) {
            silentTicks = 0;
        } else if (n == 0 && ++silentTicks >= silentLimit) {
            link_.fault("the SDR-IQ stopped sending", "no I/Q data arrived");
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

SdriqSource::~SdriqSource() { closeDevice(); }

void SdriqSource::setOpener(SdriqOpener opener) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    opener_ = std::move(opener);
}

bool SdriqSource::failOpen(const std::string& why) {
    const std::string keep = why;
    teardown();
    setError(keep);
    return false;
}

void SdriqSource::teardown() {
    active_ = false;
    markOpen(false);
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (c && c->link) { c->link->interrupt(); }
    endReader();
    std::lock_guard<std::mutex> cl(connMutex_);
    conn_.reset();
}

bool SdriqSource::transact(as::MsgType type, std::uint16_t item, const std::vector<std::uint8_t>& params,
                           std::vector<std::uint8_t>& reply, bool& nak) {
    nak = false;
    reply.clear();
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (c == nullptr) { setError("no radio connection is open"); return false; }
    const std::uint32_t want = (static_cast<std::uint32_t>(type == as::MsgType::RequestRange ? 2 : 0) << 16) | item;
    std::uint64_t beforeSeq = 0, beforeNak = 0;
    {
        std::lock_guard<std::mutex> lk(c->mu);
        beforeSeq = c->replySeq[want];
        beforeNak = c->nakSeq;
    }
    std::string err;
    if (!c->sendBytes(as::encodeMessage(type, item, params), err)) {
        setError("could not send to the SDR-IQ" + (err.empty() ? std::string() : ": " + tame(err)));
        return false;
    }
    std::unique_lock<std::mutex> lk(c->mu);
    c->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs_),
                   [&] { return c->replySeq[want] != beforeSeq || c->nakSeq != beforeNak || faulted(); });
    if (c->replySeq[want] != beforeSeq) {
        reply = c->replies[want];
        return true;
    }
    if (c->nakSeq != beforeNak) {
        nak = true;
        return true;
    }
    if (faulted()) { setError(faultedWhile().empty() ? "the SDR-IQ connection failed" : faultedWhile()); return false; }
    setError("the SDR-IQ did not answer (receive timed out)");
    return false;
}

bool SdriqSource::transactMandatory(as::MsgType type, std::uint16_t item, const std::vector<std::uint8_t>& params,
                                    std::vector<std::uint8_t>& reply, const char* what) {
    bool nak = false;
    if (!transact(type, item, params, reply, nak)) {
        setError(std::string(what) + ": " + lastError());
        return false;
    }
    if (nak) {
        setError(std::string("the SDR-IQ does not implement ") + what);
        return false;
    }
    return true;
}

bool SdriqSource::open(const std::string& args) {
    closeDevice();
    std::lock_guard<std::mutex> lk(ctlMutex_);
    timeoutMs_ = 3000;
    const std::string to = argValue(args, "timeout");
    if (!to.empty()) {
        char* e = nullptr;
        const long v = std::strtol(to.c_str(), &e, 10);
        if (e == to.c_str() || *e != '\0' || v < 100 || v > 60000) { setError("the timeout must be 100 to 60000 ms"); return false; }
        timeoutMs_ = static_cast<int>(v);
    }
    const std::string want = argValue(args, "sdriq");
    for (char ch : want) {
        if (ch < 0x20 || ch > 0x7E) { setError("the serial number may use printable characters only"); return false; }
    }
    if (want.size() > 32) { setError("the serial number is at most 32 characters"); return false; }

    std::string err;
    std::unique_ptr<SdriqLink> raw = opener_ ? opener_(want, err) : defaultSdriqOpener(want, err);
    if (!raw) {
        setError(err.empty() ? std::string("no SDR-IQ could be opened") : err);
        return false;
    }
    auto conn = std::make_shared<Conn>();
    conn->link = std::shared_ptr<SdriqLink>(std::move(raw));
    conn->silentLimit = std::max(5, timeoutMs_ / kPollMs);
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        conn_ = conn;
    }
    if (!beginReader([conn](NetLink& l) { return conn->pump(l); }, [conn]() { conn->link->interrupt(); }, conn)) {
        std::lock_guard<std::mutex> cl(connMutex_);
        conn_.reset();
        return false;
    }

    // --- identify --------------------------------------------------------------------------------------
    std::vector<std::uint8_t> rep;
    bool nak = false;
    if (!transact(as::MsgType::Request, sq::ci::Name, {}, rep, nak)) {
        return failOpen("this does not look like an SDR-IQ: it did not answer the name request (" + std::string(lastError()) + ")");
    }
    std::string name;
    if (nak || !sq::parseCString(rep, name)) { return failOpen("this does not look like an SDR-IQ: its name answer is not a name"); }
    if (name != "SDR-IQ") {
        return failOpen("the device calls itself \"" + tame(name) + "\", not an SDR-IQ (this source drives the SDR-IQ only)");
    }
    serial_.clear();
    if (transact(as::MsgType::Request, sq::ci::Serial, {}, rep, nak) && !nak) {
        std::string s;
        if (sq::parseCString(rep, s)) { serial_ = s; }
    }
    setError("");
    if (!want.empty() && !serial_.empty() && serial_ != want) {
        return failOpen("the SDR-IQ that answered is not the one asked for (a different serial number)");
    }
    // The status: boot-loader states mean the radio is in firmware-update mode, which this source never uses.
    if (transact(as::MsgType::Request, sq::ci::Status, {}, rep, nak) && !nak && !rep.empty()) {
        for (std::uint8_t b : rep) {
            if (b == sq::kStatusBootIdle || b == sq::kStatusBootBusy || b == sq::kStatusBootError) {
                return failOpen("the SDR-IQ is in its firmware boot mode; switch it off and on again");
            }
        }
    }
    setError("");
    haveRange_ = false;
    range_ = as::FreqRange{0, 30000000};
    if (transact(as::MsgType::RequestRange, sq::ci::RxFrequency, {0x00}, rep, nak) && !nak) {
        as::FreqRange r;
        if (sq::parseRange(rep, r)) { range_ = r; haveRange_ = true; }
    }
    setError("");
    freqHz_ = 0;
    if (transact(as::MsgType::Request, sq::ci::RxFrequency, {0x00}, rep, nak) && !nak && rep.size() >= 6) {
        freqHz_ = as::getFrequency(rep.data() + 1);
    }
    setError("");
    rfGainDb_ = 0;
    if (transact(as::MsgType::Request, sq::ci::RfGain, {0x00}, rep, nak) && !nak && rep.size() >= 2 && rep[0] == 0) {
        const int g = static_cast<std::int8_t>(rep[1]);
        if (sq::validRfGainDb(g)) { rfGainDb_ = g; }
    }
    setError("");
    ifGainDb_ = 0;
    rateCode_ = sq::kPowerUpRate;
    markOpen(true);
    setName("RFspace SDR-IQ" + (serial_.empty() ? std::string() : std::string(" (serial ") + tame(serial_) + ")"));
    return true;
}

void SdriqSource::closeDevice() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    if (c && active_) {
        std::string e;
        c->sendBytes(as::encodeMessage(as::MsgType::Set, sq::ci::RxState, sq::runParams(false)), e);  // best effort
    }
    c.reset();
    teardown();
    setName("network radio: (not connected)");
}

bool SdriqSource::start() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("start() called with no radio open"); return false; }
    if (faulted()) { return false; }
    if (active_) { return true; }
    std::vector<std::uint8_t> rep;
    // rate first, while idle (the document: not while running); the radio echoes the value it took
    if (!transactMandatory(as::MsgType::Set, sq::ci::OutSampleRate, sq::rateParams(rateCode_), rep, "setting the sample rate")) { return false; }
    if (rep.size() < 5 || as::getLe(rep.data() + 1, 4) != rateCode_) {
        setError("the SDR-IQ did not accept that sample rate");
        return false;
    }
    if (!transactMandatory(as::MsgType::Set, sq::ci::RfGain, sq::rfGainParams(rfGainDb_), rep, "setting the RF gain")) { return false; }
    if (!transactMandatory(as::MsgType::Set, sq::ci::IfGain, sq::ifGainParams(ifGainDb_), rep, "setting the IF gain")) { return false; }
    setError("");
    if (!startKeeping()) { return false; }
    if (!transactMandatory(as::MsgType::Set, sq::ci::RxState, sq::runParams(true), rep, "starting the receiver")) {
        stopKeeping();
        return false;
    }
    active_ = true;
    return true;
}

void SdriqSource::stop() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    stopKeeping();
    if (active_) {
        std::vector<std::uint8_t> rep;
        bool nak = false;
        transact(as::MsgType::Set, sq::ci::RxState, sq::runParams(false), rep, nak);
    }
    active_ = false;
}

double SdriqSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return sq::rateHzFor(rateCode_);
}

bool SdriqSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setSampleRateHz() called with no radio open"); return false; }
    if (active_) { setError("stop the radio before changing its sample rate"); return false; }
    std::uint32_t code = 0;
    if (!std::isfinite(hz) || !sq::rateCodeFor(hz, code)) {
        setError("the SDR-IQ takes 8138, 16276, 37793, 55556, 111111, 158730 or 196078 samples per second");
        return false;
    }
    rateCode_ = code;
    return true;
}

std::vector<double> SdriqSource::supportedSampleRatesHz() const {
    std::vector<double> v;
    for (const sq::RateCode& r : sq::kRates) { v.push_back(sq::kAdClockHz / static_cast<double>(r.divisor)); }
    return v;
}

double SdriqSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return static_cast<double>(freqHz_);
}

bool SdriqSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setCenterFrequencyHz() called with no radio open"); return false; }
    if (!(hz >= 0.0) || !std::isfinite(hz) || hz > 1.0e12) { setError("setCenterFrequencyHz() requires a frequency"); return false; }
    const std::uint64_t f = static_cast<std::uint64_t>(std::llround(hz));
    if (f < range_.minHz || f > range_.maxHz || f > sq::kMaxFrequencyHz) {
        setError("the SDR-IQ tunes " + std::to_string(range_.minHz) + " to " + std::to_string(range_.maxHz) + " Hz; " + std::to_string(f) + " Hz is outside that");
        return false;
    }
    std::vector<std::uint8_t> rep;
    if (!transactMandatory(as::MsgType::Set, sq::ci::RxFrequency, sq::frequencyParams(f), rep, "tuning")) { return false; }
    freqHz_ = rep.size() >= 6 ? as::getFrequency(rep.data() + 1) : f;
    return true;
}

bool SdriqSource::frequencyRangeHz(double& lo, double& hi) const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { return false; }
    lo = static_cast<double>(range_.minHz);
    hi = static_cast<double>(range_.maxHz);
    return true;
}

std::vector<GainInfo> SdriqSource::gains() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    std::vector<GainInfo> v;
    if (isOpen()) {
        GainInfo rf;
        rf.name = "RF gain";
        rf.minDb = -30.0;
        rf.maxDb = 0.0;
        rf.stepDb = 10.0;
        v.push_back(rf);
        GainInfo ifg;
        ifg.name = "IF gain";
        ifg.minDb = 0.0;
        ifg.maxDb = 24.0;
        ifg.stepDb = 6.0;
        v.push_back(ifg);
    }
    return v;
}

bool SdriqSource::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (name != "RF gain" && name != "IF gain") { setError("this radio has two gains, \"RF gain\" and \"IF gain\""); return false; }
    if (!isOpen()) { setError("setGainDb() called with no radio open"); return false; }
    const long v = std::isfinite(db) ? std::lround(db) : 0;
    const bool whole = std::isfinite(db) && std::fabs(db - static_cast<double>(v)) <= 0.01;
    std::vector<std::uint8_t> rep;
    if (name == "RF gain") {
        if (!whole || !sq::validRfGainDb(static_cast<int>(v))) { setError("the RF gain takes 0, -10, -20 or -30 dB"); return false; }
        if (!transactMandatory(as::MsgType::Set, sq::ci::RfGain, sq::rfGainParams(static_cast<int>(v)), rep, "setting the RF gain")) { return false; }
        rfGainDb_ = static_cast<int>(v);
    } else {
        if (!whole || !sq::validIfGainDb(static_cast<int>(v))) { setError("the IF gain takes 0, 6, 12, 18 or 24 dB"); return false; }
        if (!transactMandatory(as::MsgType::Set, sq::ci::IfGain, sq::ifGainParams(static_cast<int>(v)), rep, "setting the IF gain")) { return false; }
        ifGainDb_ = static_cast<int>(v);
    }
    return true;
}

double SdriqSource::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (name == "RF gain") { return rfGainDb_; }
    if (name == "IF gain") { return ifGainDb_; }
    return 0.0;
}

std::string SdriqSource::serialNumber() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return serial_;
}

std::uint64_t SdriqSource::dataBlocks() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    return c ? c->blocks.load() : 0;
}
std::uint64_t SdriqSource::otherBlocks() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    return c ? c->other.load() : 0;
}
std::uint64_t SdriqSource::overloadReports() const {
    std::shared_ptr<Conn> c;
    {
        std::lock_guard<std::mutex> cl(connMutex_);
        c = conn_;
    }
    return c ? c->overload.load() : 0;
}

}  // namespace cascade::source
