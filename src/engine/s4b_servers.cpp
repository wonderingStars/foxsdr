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

void ServerSet::configure(const Config& c) {
    std::lock_guard<std::mutex> lk(m_);
    applyTcpAudio(c);
    cfg_ = c;
    configured_ = true;
}

bool ServerSet::active() const {
    std::lock_guard<std::mutex> lk(m_);
    return tcpAudio_ != nullptr;
}

void ServerSet::stop() {
    std::lock_guard<std::mutex> lk(m_);
    if (tcpAudio_ != nullptr) {
        tcpAudio_->stop();
        tcpAudio_.reset();
    }
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
}

std::string ServerSet::signature() const {
    std::lock_guard<std::mutex> lk(m_);
    std::string s;
    if (tcpAudio_ != nullptr) {
        const cascade::net::FanoutStats st = tcpAudio_->stats();
        s += "a" + std::to_string(st.clients) + "," + std::to_string(st.accepted) + "," + std::to_string(st.droppedSlow + st.droppedStalled + st.refusedFull) + "," + std::to_string(tcpAudio_->port());
    }
    s += "|" + tcpAudioProblem_;
    return s;
}

}  // namespace cascade::engine::s4b
