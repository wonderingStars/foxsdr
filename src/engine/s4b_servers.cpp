// s4b_servers.cpp - see s4b_servers.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4b_servers.hpp"

#include "engine/wire_c.hpp"

namespace cascade::engine::s4b {

std::string scopeOf(bool allAddresses) {
    return allAddresses ? "reachable from other computers on your network, without a password" : "this computer only";
}

Config configFrom(const cascade::engine::wirec::Settings& s) {
    Config c;
    c.tcpAudio = s.getBool("srv.tcpaudio.enabled");
    c.tcpAudioAll = s.getBool("srv.tcpaudio.all");
    c.tcpAudioPort = static_cast<int>(s.getInt("srv.tcpaudio.port"));
    c.tcpAudioRadio = static_cast<int>(s.getInt("srv.tcpaudio.radio"));
    c.tcpAudioPacket = s.get("srv.tcpaudio.packet") == "960" ? 960 : 480;
    c.controlRadio = static_cast<int>(s.getInt("srv.control.radio"));
    c.rtl = s.getBool("srv.rtltcp.enabled");
    c.rtlAll = s.getBool("srv.rtltcp.all");
    c.rtlPort = static_cast<int>(s.getInt("srv.rtltcp.port"));
    return c;
}

ServerSet::~ServerSet() { stop(); }

void ServerSet::setHooks(Hooks h) {
    std::lock_guard<std::mutex> lk(m_);
    hooks_ = std::move(h);
}

void ServerSet::applyTcpAudio(const Config& c) {
    const bool changed = !configured_ || c.tcpAudio != cfg_.tcpAudio || c.tcpAudioAll != cfg_.tcpAudioAll || c.tcpAudioPort != cfg_.tcpAudioPort ||
                         c.tcpAudioRadio != cfg_.tcpAudioRadio || c.tcpAudioPacket != cfg_.tcpAudioPacket;
    if (!changed) { return; }
    if (tcpAudio_ != nullptr) {
        tcpAudio_->stop();
        tcpAudio_.reset();
    }
    tcpAudioProblem_.clear();
    if (!c.tcpAudio) { return; }
    if (!hooks_.audioReader) { tcpAudioProblem_ = "the engine gave this server no audio to read"; return; }
    cascade::net::UdpAudioSender::Reader reader = hooks_.audioReader(c.tcpAudioRadio);
    if (!reader) { tcpAudioProblem_ = "the engine would not open that radio's audio"; return; }
    cascade::net::TcpAudioConfig tc;
    tc.bindAddress = c.tcpAudioAll ? "0.0.0.0" : "127.0.0.1";
    tc.allowAllInterfaces = c.tcpAudioAll;
    tc.port = c.tcpAudioPort;
    tc.packetFrames = c.tcpAudioPacket;
    auto server = std::make_unique<cascade::net::TcpAudioServer>();
    std::string why;
    if (!server->start(tc, std::move(reader), why)) {
        tcpAudioProblem_ = why.empty() ? std::string("the audio server could not start") : why;
        return;
    }
    tcpAudio_ = std::move(server);
}

void ServerSet::releaseRtl() {
    if (rtlRadio_ > 0 && hooks_.attachIq) { (void)hooks_.attachIq(rtlRadio_, nullptr); }   // once this returns no block is inside the tap
    rtlTap_.target = nullptr;
    rtlRadio_ = 0;
    rtlIdentity_ = 0;
    if (rtl_ != nullptr) {
        rtl_->stop();
        rtl_.reset();
    }
}

void ServerSet::applyRtl(const Config& c) {
    const bool changed = !configured_ || c.rtl != cfg_.rtl || c.rtlAll != cfg_.rtlAll || c.rtlPort != cfg_.rtlPort || c.controlRadio != cfg_.controlRadio;
    if (!changed) { return; }
    releaseRtl();
    rtlProblem_.clear();
    rtlTuneWord_.store(0, std::memory_order_relaxed);
    if (!c.rtl) { return; }
    if (!hooks_.attachIq || !hooks_.radioIdentity) { rtlProblem_ = "the engine gave this server no radio samples to send"; return; }
    cascade::net::RtlTcpConfig rc;
    rc.bindAddress = c.rtlAll ? "0.0.0.0" : "127.0.0.1";
    rc.allowAllInterfaces = c.rtlAll;
    rc.port = static_cast<std::uint16_t>(c.rtlPort);
    auto server = std::make_unique<cascade::net::RtlTcpServer>(rc);
    std::string why;
    // The one command a client may use is the frequency. The IO thread only records it; the engine's pass applies it.
    std::atomic<std::uint64_t>* word = &rtlTuneWord_;
    std::atomic<std::uint64_t>* ignored = &rtlIgnored_;
    if (!server->start([word, ignored](std::uint8_t cmd, std::uint32_t param) {
            if (cmd == cascade::net::kRtlCmdSetFrequency) { word->store((1ull << 63) | static_cast<std::uint64_t>(param), std::memory_order_release); }
            else { ignored->fetch_add(1, std::memory_order_relaxed); }
        }, why)) {
        rtlProblem_ = why.empty() ? std::string("the rtl_tcp server could not start") : why;
        return;
    }
    rtl_ = std::move(server);
    rtlTap_.target = rtl_.get();
    rtlRadio_ = 0;   // the pass puts the tap on the radio (and again when the radio is opened anew)
}

void ServerSet::pass() {
    std::lock_guard<std::mutex> lk(m_);
    if (rtl_ == nullptr) { return; }
    const int radio = cfg_.controlRadio;
    const std::uintptr_t id = hooks_.radioIdentity ? hooks_.radioIdentity(radio) : 0;
    if (id == 0) {
        rtlProblem_ = "the radio this server reads is not open";
        rtlRadio_ = 0;
        rtlIdentity_ = 0;
    } else if (rtlRadio_ != radio || rtlIdentity_ != id) {
        const std::string why = hooks_.attachIq(radio, &rtlTap_);
        if (why.empty()) { rtlRadio_ = radio; rtlIdentity_ = id; rtlProblem_.clear(); }
        else { rtlProblem_ = why; rtlRadio_ = 0; rtlIdentity_ = 0; }
    }
    const std::uint64_t w = rtlTuneWord_.exchange(0, std::memory_order_acquire);
    if ((w >> 63) != 0 && rtlRadio_ > 0 && hooks_.tune) {
        const double hz = static_cast<double>(w & 0xFFFFFFFFull);
        const std::string why = (hz >= 1000.0 && hz <= 7.2e9) ? hooks_.tune(radio, hz) : std::string("that is not a frequency this receiver can tune");
        if (why.empty()) { rtlTunes_.fetch_add(1, std::memory_order_relaxed); }
        else { rtlTuneRefused_.fetch_add(1, std::memory_order_relaxed); }
    }
}

bool ServerSet::holdsIq(int radio) const {
    std::lock_guard<std::mutex> lk(m_);
    return rtl_ != nullptr && cfg_.controlRadio == radio;
}

void ServerSet::configure(const Config& c) {
    std::lock_guard<std::mutex> lk(m_);
    applyTcpAudio(c);
    applyRtl(c);
    cfg_ = c;
    configured_ = true;
}

bool ServerSet::active() const {
    std::lock_guard<std::mutex> lk(m_);
    return tcpAudio_ != nullptr || rtl_ != nullptr;
}

void ServerSet::stop() {
    std::lock_guard<std::mutex> lk(m_);
    releaseRtl();
    if (tcpAudio_ != nullptr) {
        tcpAudio_->stop();
        tcpAudio_.reset();
    }
    hooks_ = Hooks{};   // stop() is the engine's teardown: the hooks hold the engine and must not be called after it
}

void ServerSet::facts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    std::lock_guard<std::mutex> lk(m_);
    const bool tcpOn = tcpAudio_ != nullptr && tcpAudio_->running();
    fact("srv.tcpaudio.running", tcpOn ? "1" : "0");
    fact("srv.tcpaudio.port", tcpOn ? std::to_string(tcpAudio_->port()) : std::string());
    fact("srv.tcpaudio.clients", tcpOn ? std::to_string(tcpAudio_->stats().clients) : std::string("0"));
    fact("srv.tcpaudio.accepted", tcpOn ? std::to_string(tcpAudio_->stats().accepted) : std::string("0"));
    fact("srv.tcpaudio.dropped", tcpOn ? std::to_string(tcpAudio_->stats().droppedSlow + tcpAudio_->stats().droppedStalled + tcpAudio_->stats().refusedFull) : std::string("0"));
    fact("srv.tcpaudio.problem", tcpAudioProblem_);
    fact("srv.tcpaudio.scope", cfg_.tcpAudio ? scopeOf(cfg_.tcpAudioAll) : std::string());
    const bool rtlOn = rtl_ != nullptr && rtl_->running();
    const cascade::net::RtlTcpStats rs = rtlOn ? rtl_->stats() : cascade::net::RtlTcpStats{};
    fact("srv.rtltcp.running", rtlOn ? "1" : "0");
    fact("srv.rtltcp.port", rtlOn ? std::to_string(rtl_->port()) : std::string());
    fact("srv.rtltcp.clients", std::to_string(rs.clients));
    fact("srv.rtltcp.accepted", std::to_string(rs.accepted));
    fact("srv.rtltcp.refused_busy", std::to_string(rs.refusedBusy));
    fact("srv.rtltcp.samples_dropped", std::to_string(rs.samplesDropped));
    fact("srv.rtltcp.commands", std::to_string(rs.commands));
    fact("srv.rtltcp.tunes", std::to_string(rtlTunes_.load()));
    fact("srv.rtltcp.tunes_refused", std::to_string(rtlTuneRefused_.load()));
    fact("srv.rtltcp.ignored", std::to_string(rtlIgnored_.load() + rs.unknownCommands));
    fact("srv.rtltcp.problem", rtlProblem_);
    fact("srv.rtltcp.scope", cfg_.rtl ? scopeOf(cfg_.rtlAll) : std::string());
}

std::string ServerSet::signature() const {
    std::lock_guard<std::mutex> lk(m_);
    std::string s;
    if (tcpAudio_ != nullptr) {
        const cascade::net::FanoutStats st = tcpAudio_->stats();
        s += "a" + std::to_string(st.clients) + "," + std::to_string(st.accepted) + "," + std::to_string(st.droppedSlow + st.droppedStalled + st.refusedFull) + "," + std::to_string(tcpAudio_->port());
    }
    s += "|" + tcpAudioProblem_;
    if (rtl_ != nullptr) {
        const cascade::net::RtlTcpStats rs = rtl_->stats();
        s += "r" + std::to_string(rs.clients) + "," + std::to_string(rs.accepted) + "," + std::to_string(rs.commands) + "," + std::to_string(rtlTunes_.load()) + "," + std::to_string(rtlTuneRefused_.load()) + "," + std::to_string(rs.samplesDropped >> 14) + "," + std::to_string(rtl_->port());
    }
    s += "|" + rtlProblem_;
    return s;
}

}  // namespace cascade::engine::s4b
