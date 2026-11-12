// hpsdr1_source.cpp - see hpsdr1_source.hpp and hpsdr1_frames.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hpsdr1_source.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace cascade::source {

namespace hp = hpsdr;
namespace h1 = hpsdr1;

namespace {

constexpr int kPollMs = 200;
constexpr int kRefreshMs = 100;
constexpr double kRedPitayaMaxHz = 61440000.0;  // Pavel Demin's Red Pitaya page: "from 0 Hz to 61.44 MHz"

}  // namespace

// ---------------------------------------------------------------------------------------
// the state shared with the reader thread
// ---------------------------------------------------------------------------------------

struct Hpsdr1Source::Rx {
    std::shared_ptr<UdpTransport> udp;

    std::mutex cmdMutex;
    h1::CommandState cmd;
    std::uint32_t epSeq = 0;
    std::atomic<std::uint64_t> commandPackets{0};

    std::mutex trackMutex;
    hp::SequenceTracker tracker;
    bool armed = true;
    std::chrono::steady_clock::time_point armDeadline{};
    std::atomic<std::uint64_t> bad{0};
    std::atomic<std::uint64_t> overload{0};
    int silentTicks = 0;
    int silentLimit = 10;
    std::chrono::steady_clock::time_point nextRefresh{};
    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(4096);
    std::vector<std::vector<std::complex<float>>> perRx;

    // The full set of command frames, two to a data packet. False if a send failed.
    bool sendRefresh() {
        std::vector<std::array<std::uint8_t, h1::kFrameBytes>> frames;
        {
            std::lock_guard<std::mutex> lk(cmdMutex);
            for (int a : h1::refreshAddresses(cmd)) { frames.push_back(h1::buildCommandFrame(cmd, a)); }
            if (frames.size() % 2 == 1) { frames.push_back(h1::buildCommandFrame(cmd, h1::kAddrGeneral)); }
        }
        bool ok = true;
        for (std::size_t i = 0; i + 1 < frames.size(); i += 2) {
            std::uint8_t both[2 * h1::kFrameBytes];
            std::memcpy(both, frames[i].data(), h1::kFrameBytes);
            std::memcpy(both + h1::kFrameBytes, frames[i + 1].data(), h1::kFrameBytes);
            std::uint32_t seq;
            {
                std::lock_guard<std::mutex> lk(cmdMutex);
                seq = epSeq++;
            }
            const std::vector<std::uint8_t> pkt = hp::buildDataPacket(hp::kEndpointToDevice, seq, both);
            if (!udp->sendAll(pkt.data(), pkt.size())) { ok = false; } else { commandPackets.fetch_add(1, std::memory_order_relaxed); }
        }
        return ok;
    }

    bool pump(NetLink& link) {
        std::size_t got = 0;
        if (!udp->recvSome(buf.data(), buf.size(), got)) {
            if (!link.run.load(std::memory_order_relaxed)) { return false; }
            const char* e = udp->lastError();
            if (e != nullptr && std::strstr(e, "timed out") != nullptr) {
                if (!link.streaming.load(std::memory_order_relaxed)) {
                    silentTicks = 0;
                } else if (++silentTicks >= silentLimit) {
                    link.fault("the HPSDR radio stopped sending", "no data arrived");
                    return false;
                } else if (std::chrono::steady_clock::now() >= nextRefresh) {
                    nextRefresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRefreshMs);
                    sendRefresh();
                }
                return true;
            }
            link.fault("the HPSDR network connection failed", e != nullptr ? e : "");
            return false;
        }
        silentTicks = 0;
        if (link.streaming.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() >= nextRefresh) {
            nextRefresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRefreshMs);
            sendRefresh();
        }
        hp::DataPacket p;
        if (!hp::parseDataPacket(buf.data(), got, p)) { bad.fetch_add(1, std::memory_order_relaxed); return true; }
        if (p.endpoint != hp::kEndpointIq) { return true; }  // the bandscope is not used
        {
            std::lock_guard<std::mutex> lk(trackMutex);
            if (!armed) {
                if (p.sequence == 0 || std::chrono::steady_clock::now() >= armDeadline) { armed = true; } else { return true; }
            }
            tracker.note(p.endpoint, p.sequence);
        }
        int nRx;
        {
            std::lock_guard<std::mutex> lk(cmdMutex);
            nRx = cmd.receivers;
        }
        for (int fi = 0; fi < 2; ++fi) {
            perRx.clear();
            h1::FrameStatus st;
            if (!h1::decodeRxFrame(p.frames + fi * h1::kFrameBytes, h1::kFrameBytes, nRx, perRx, st)) {
                bad.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (st.adcOverload()) { overload.fetch_add(1, std::memory_order_relaxed); }
            if (!perRx.empty() && !perRx[0].empty()) { link.pushSamples(perRx[0].data(), perRx[0].size()); }
        }
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

Hpsdr1Source::~Hpsdr1Source() { closeDevice(); }

bool Hpsdr1Source::open(const std::string& args) {
    closeDevice();
    std::lock_guard<std::mutex> lk(mutex_);
    std::string host = "255.255.255.255";
    std::uint16_t port = hp::kDevicePort;
    const std::string addr = argValue(args, "hpsdr1");
    std::string err;
    if (!addr.empty() && !parseHostPort(addr, hp::kDevicePort, host, port, err)) { setError(err); return false; }
    timeoutMs_ = 2000;
    const std::string to = argValue(args, "timeout");
    if (!to.empty()) {
        const long v = std::strtol(to.c_str(), nullptr, 10);
        if (v < 100 || v > 60000) { setError("the timeout must be 100 to 60000 ms"); return false; }
        timeoutMs_ = static_cast<int>(v);
    }
    const std::string prof = argValue(args, "profile");
    profile_ = Hpsdr1Profile::Generic;
    if (prof == "redpitaya") { profile_ = Hpsdr1Profile::RedPitaya; }
    else if (!prof.empty() && prof != "generic") { setError("the profile must be generic or redpitaya"); return false; }
    int rate = 192000;
    const std::string rt = argValue(args, "rate");
    if (!rt.empty()) {
        char* e = nullptr;
        const long v = std::strtol(rt.c_str(), &e, 10);
        if (e == rt.c_str() || *e != '\0' || h1::speedCodeForRate(static_cast<int>(v)) < 0 ||
            (profile_ == Hpsdr1Profile::RedPitaya && v == 384000)) {
            setError(profile_ == Hpsdr1Profile::RedPitaya ? "the rate must be 48000, 96000 or 192000"
                                                           : "the rate must be 48000, 96000, 192000 or 384000");
            return false;
        }
        rate = static_cast<int>(v);
    }
    std::string wantMac = argValue(args, "mac");
    for (char& c : wantMac) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

    std::vector<HpsdrDevice> found;
    if (!discoverHpsdr(host, port, std::chrono::milliseconds(timeoutMs_), found, err)) { setError(err); return false; }
    const HpsdrDevice* chosen = nullptr;
    for (const HpsdrDevice& d : found) {
        if (wantMac.empty() || d.reply.macText() == wantMac) { chosen = &d; break; }
    }
    if (chosen == nullptr) {
        setError(found.empty() ? "no HPSDR radio answered the discovery request"
                               : "no HPSDR radio with that MAC address answered (" + std::to_string(found.size()) + " other(s) did)");
        return false;
    }
    dev_ = *chosen;
    std::unique_ptr<UdpTransport> u = UdpTransport::bindLocal("0.0.0.0", 0, std::chrono::milliseconds(kPollMs), err);
    if (!u) { setError(err); return false; }
    if (!u->connectTo(dev_.ip, dev_.port)) { setError(u->lastError()); return false; }
    u->setReceiveBuffer(4 * 1024 * 1024);  // 1500 packets a second at 192 kS/s; ride out a stalled reader
    udp_ = std::shared_ptr<UdpTransport>(u.release());

    auto rx = std::make_shared<Rx>();
    rx->udp = udp_;
    rx->cmd.speedCode = h1::speedCodeForRate(rate);
    rx->cmd.receivers = 1;
    rx->silentLimit = std::max(3, timeoutMs_ / kPollMs);
    rx_ = rx;
    freqHz_ = 0.0;
    if (!beginReader([rx](NetLink& l) { return rx->pump(l); }, [rx]() { rx->udp->shutdown(); }, rx)) {
        rx_.reset();
        udp_.reset();
        return false;
    }
    markOpen(true);
    setName("HPSDR: " + dev_.name());
    return true;
}

void Hpsdr1Source::closeDevice() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_ && !isOpen()) { endReader(); return; }
    const std::string keep = lastError();
    if (udp_ && active_) {
        const std::vector<std::uint8_t> stopCmd = hp::buildStartStop(hp::kStopAll);
        udp_->sendAll(stopCmd.data(), stopCmd.size());  // best effort
    }
    active_ = false;
    markOpen(false);
    endReader();
    if (udp_) { udp_->close(); }
    udp_.reset();
    rx_.reset();
    if (!keep.empty()) { setError(keep); }
    setName("network radio: (not connected)");
}

bool Hpsdr1Source::start() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen() || !rx_) { setError("start() called with no HPSDR radio open"); return false; }
    if (faulted()) { return false; }
    if (active_) { return true; }
    {
        std::lock_guard<std::mutex> tl(rx_->trackMutex);
        rx_->tracker.reset();
        rx_->armed = false;
        rx_->armDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    }
    {
        std::lock_guard<std::mutex> cl(rx_->cmdMutex);
        rx_->epSeq = 0;  // the PC's sequence restarts at each Start (Metis guide)
    }
    rx_->nextRefresh = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRefreshMs);
    // settings first, then Start (the document asks for command frames before data on boards that
    // select their clock from them)
    if (!rx_->sendRefresh()) {
        setError(std::string("could not send the radio's settings: ") + udp_->lastError());
        return false;
    }
    const std::vector<std::uint8_t> cmd = hp::buildStartStop(hp::kStartIq);
    if (!udp_->sendAll(cmd.data(), cmd.size())) {
        setError(std::string("could not send the start command: ") + udp_->lastError());
        return false;
    }
    active_ = true;
    return startKeeping();
}

void Hpsdr1Source::stop() {
    std::lock_guard<std::mutex> lk(mutex_);
    stopKeeping();
    if (active_ && udp_) {
        const std::vector<std::uint8_t> cmd = hp::buildStartStop(hp::kStopAll);
        udp_->sendAll(cmd.data(), cmd.size());
    }
    active_ = false;
}

double Hpsdr1Source::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_) { return 0.0; }
    std::lock_guard<std::mutex> cl(rx_->cmdMutex);
    return h1::rateForSpeedCode(rx_->cmd.speedCode);
}

std::vector<double> Hpsdr1Source::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen()) { return {}; }
    std::vector<double> v{48000.0, 96000.0, 192000.0};
    if (profile_ == Hpsdr1Profile::Generic) { v.push_back(384000.0); }
    return v;
}

bool Hpsdr1Source::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen() || !rx_) { setError("setSampleRateHz() called with no HPSDR radio open"); return false; }
    const int code = std::isfinite(hz) && hz == std::floor(hz) && hz > 0 && hz < 1e7 ? h1::speedCodeForRate(static_cast<int>(hz)) : -1;
    if (code < 0 || (profile_ == Hpsdr1Profile::RedPitaya && code == 3)) {
        setError(profile_ == Hpsdr1Profile::RedPitaya ? "this radio takes 48000, 96000 or 192000 samples per second"
                                                       : "an HPSDR radio takes 48000, 96000, 192000 or 384000 samples per second");
        return false;
    }
    {
        std::lock_guard<std::mutex> cl(rx_->cmdMutex);
        rx_->cmd.speedCode = code;
    }
    if (active_ && !rx_->sendRefresh()) { setError("could not send the new rate to the radio"); return false; }
    return true;
}

double Hpsdr1Source::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return freqHz_;
}

bool Hpsdr1Source::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen() || !rx_) { setError("setCenterFrequencyHz() called with no HPSDR radio open"); return false; }
    if (!(hz >= 0.0) || !std::isfinite(hz)) { setError("setCenterFrequencyHz() requires a frequency"); return false; }
    if (profile_ == Hpsdr1Profile::RedPitaya && hz > kRedPitayaMaxHz) {
        setError("the Red Pitaya receiver tunes 0 to 61.44 MHz");
        return false;
    }
    if (hz > 4294967295.0) { setError("the frequency field is 32 bits: 4.29 GHz at most"); return false; }
    {
        std::lock_guard<std::mutex> cl(rx_->cmdMutex);
        rx_->cmd.rxHz[0] = static_cast<std::uint32_t>(std::llround(hz));
    }
    freqHz_ = static_cast<double>(static_cast<std::uint32_t>(std::llround(hz)));
    if (active_ && !rx_->sendRefresh()) { setError("could not send the new frequency to the radio"); return false; }
    return true;
}

bool Hpsdr1Source::frequencyRangeHz(double& lo, double& hi) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (profile_ != Hpsdr1Profile::RedPitaya) { return false; }
    lo = 0.0;
    hi = kRedPitayaMaxHz;
    return true;
}

std::vector<GainInfo> Hpsdr1Source::gains() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<GainInfo> v;
    if (isOpen() && profile_ == Hpsdr1Profile::Generic) {
        GainInfo g;
        g.name = "Attenuator";
        g.minDb = 0.0;
        g.maxDb = 31.0;
        g.stepDb = 1.0;
        v.push_back(g);
    }
    return v;
}

bool Hpsdr1Source::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!isOpen() || !rx_) { setError("setGainDb() called with no HPSDR radio open"); return false; }
    if (profile_ != Hpsdr1Profile::Generic || name != "Attenuator") {
        setError(profile_ == Hpsdr1Profile::RedPitaya ? "this radio has no gain control" : "this radio has one gain, \"Attenuator\"");
        return false;
    }
    if (!(db >= 0.0 && db <= 31.0) || db != std::floor(db)) { setError("the attenuator takes whole decibels, 0 to 31"); return false; }
    {
        std::lock_guard<std::mutex> cl(rx_->cmdMutex);
        rx_->cmd.attenDb = static_cast<std::uint8_t>(db);
        rx_->cmd.attenEnable = true;
    }
    if (active_ && !rx_->sendRefresh()) { setError("could not send the new attenuation to the radio"); return false; }
    return true;
}

double Hpsdr1Source::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_ || name != "Attenuator") { return 0.0; }
    std::lock_guard<std::mutex> cl(rx_->cmdMutex);
    return rx_->cmd.attenDb;
}

HpsdrDevice Hpsdr1Source::device() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return dev_;
}

Hpsdr1Profile Hpsdr1Source::profile() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return profile_;
}

std::uint64_t Hpsdr1Source::packetsLost() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_) { return 0; }
    std::lock_guard<std::mutex> tl(rx_->trackMutex);
    return rx_->tracker.lost();
}

std::uint64_t Hpsdr1Source::packetsLate() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!rx_) { return 0; }
    std::lock_guard<std::mutex> tl(rx_->trackMutex);
    return rx_->tracker.late();
}

std::uint64_t Hpsdr1Source::badPackets() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->bad.load() : 0;
}

std::uint64_t Hpsdr1Source::overloadFrames() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->overload.load() : 0;
}

std::uint64_t Hpsdr1Source::commandPacketsSent() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return rx_ ? rx_->commandPackets.load() : 0;
}

}  // namespace cascade::source
