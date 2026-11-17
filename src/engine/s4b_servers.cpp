// s4b_servers.cpp - see s4b_servers.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4b_servers.hpp"

#include <chrono>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "foxsdr_api.h"

#include "engine/wire_c.hpp"
#include "net/ws_frame.hpp"

namespace cascade::engine::s4b {

std::string scopeOf(bool allAddresses) {
    return allAddresses ? "reachable from other computers on your network, without a password" : "this computer only";
}

std::string scopeOfListed(bool allAddresses, const std::string& allow) {
    return allAddresses ? "reachable only from the computers you listed (" + allow + "), without a password" : "this computer only";
}

Config configFrom(const cascade::engine::wirec::Settings& s) {
    Config c;
    c.tcpAudio = s.getBool("srv.tcpaudio.enabled");
    c.tcpAudioAll = s.getBool("srv.tcpaudio.all");
    c.tcpAudioAllow = s.get("srv.tcpaudio.allow");
    c.tcpAudioPort = static_cast<int>(s.getInt("srv.tcpaudio.port"));
    c.tcpAudioRadio = static_cast<int>(s.getInt("srv.tcpaudio.radio"));
    c.tcpAudioPacket = s.get("srv.tcpaudio.packet") == "960" ? 960 : 480;
    c.controlRadio = static_cast<int>(s.getInt("srv.control.radio"));
    c.rtl = s.getBool("srv.rtltcp.enabled");
    c.rtlAll = s.getBool("srv.rtltcp.all");
    c.rtlAllow = s.get("srv.rtltcp.allow");
    c.rtlPort = static_cast<int>(s.getInt("srv.rtltcp.port"));
    c.api = s.getBool("srv.api.enabled");
    c.apiAll = s.getBool("srv.api.all");
    c.apiPort = static_cast<int>(s.getInt("srv.api.port"));
    c.mcp = s.getBool("srv.mcp.enabled");
    c.mcpPort = static_cast<int>(s.getInt("srv.mcp.port"));
    c.mcpToken = s.get("srv.mcp.token");
    c.rotator.enabled = s.getBool("rot.enabled");
    c.rotator.serial = s.get("rot.kind") == "gs232";
    c.rotator.host = s.get("rot.host");
    c.rotator.port = static_cast<int>(s.getInt("rot.port"));
    c.rotator.comPort = s.get("rot.serial");
    c.rotator.baud = static_cast<int>(s.getInt("rot.baud"));
    c.rotator.azMaxDeg = s.get("rot.az_max") == "360" ? 360 : 450;
    c.rotator.parkAz = s.getReal("rot.park_az");
    c.rotator.parkEl = s.getReal("rot.park_el");
    return c;
}

std::int32_t ServerSet::rotatorOp(const std::string& text, std::string& why) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("action") || !j["action"].is_string()) { why = "the text is JSON with an action: turn, stop or park"; return FOXAPI_BAD_ARGUMENT; }
    const std::string action = j["action"].get<std::string>();
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& k = it.key();
        if (k != "action" && !(action == "turn" && (k == "az" || k == "el"))) { why = "an unknown key for that action"; return FOXAPI_BAD_ARGUMENT; }
    }
    RotatorResult r;
    if (action == "turn") {
        if (!j.contains("az") || !j.contains("el") || !j["az"].is_number() || !j["el"].is_number()) { why = "turn takes az and el, two numbers"; return FOXAPI_BAD_ARGUMENT; }
        r = rotator_.turn(j["az"].get<double>(), j["el"].get<double>());
        if (!r.ok) { why = r.error; return r.error.find("off") != std::string::npos ? FOXAPI_FAILED : FOXAPI_OUT_OF_RANGE; }
    } else if (action == "stop") {
        r = rotator_.stopMotion();
        if (!r.ok) { why = r.error; return FOXAPI_FAILED; }
    } else if (action == "park") {
        r = rotator_.park();
        if (!r.ok) { why = r.error; return FOXAPI_FAILED; }
    } else {
        why = "the action is turn, stop or park";
        return FOXAPI_BAD_ARGUMENT;
    }
    return FOXAPI_OK;
}

ServerSet::~ServerSet() { stop(); }

void ServerSet::setHooks(Hooks h) {
    {
        std::lock_guard<std::mutex> lk(acctM_);
        webAccountFn_ = h.webAccount;
    }
    std::lock_guard<std::mutex> lk(m_);
    hooks_ = std::move(h);
}

void ServerSet::applyTcpAudio(const Config& c) {
    const bool changed = !configured_ || c.tcpAudio != cfg_.tcpAudio || c.tcpAudioAll != cfg_.tcpAudioAll || c.tcpAudioAllow != cfg_.tcpAudioAllow || c.tcpAudioPort != cfg_.tcpAudioPort ||
                         c.tcpAudioRadio != cfg_.tcpAudioRadio || c.tcpAudioPacket != cfg_.tcpAudioPacket;
    if (!changed) { return; }
    if (tcpAudio_ != nullptr) {
        tcpAudio_->stop();
        tcpAudio_.reset();
    }
    tcpAudioProblem_.clear();
    if (!c.tcpAudio) { return; }
    // beyond this computer the audio has no password, so only the computers the person typed may connect; with no usable list it does not start (checked first: before any radio is touched)
    cascade::net::TcpAudioConfig tc;
    if (c.tcpAudioAll) {
        std::string listWhy;
        if (!cascade::net::PeerAllow::parse(c.tcpAudioAllow, tc.allowedPeers, listWhy)) { tcpAudioProblem_ = "to let other computers connect, list the computers that may: " + listWhy; return; }
    }
    if (!hooks_.audioReader) { tcpAudioProblem_ = "the engine gave this server no audio to read"; return; }
    cascade::net::UdpAudioSender::Reader reader = hooks_.audioReader(c.tcpAudioRadio);
    if (!reader) { tcpAudioProblem_ = "the engine would not open that radio's audio"; return; }
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
    const bool changed = !configured_ || c.rtl != cfg_.rtl || c.rtlAll != cfg_.rtlAll || c.rtlAllow != cfg_.rtlAllow || c.rtlPort != cfg_.rtlPort || c.controlRadio != cfg_.controlRadio;
    if (!changed) { return; }
    releaseRtl();
    rtlProblem_.clear();
    rtlTuneWord_.store(0, std::memory_order_relaxed);
    if (!c.rtl) { return; }
    // rtl_tcp has no password and hands out the radio's samples and accepts a retune: beyond this computer only the computers the person typed may connect (checked first: before any radio is touched)
    cascade::net::RtlTcpConfig rc;
    if (c.rtlAll) {
        std::string listWhy;
        if (!cascade::net::PeerAllow::parse(c.rtlAllow, rc.allowedPeers, listWhy)) { rtlProblem_ = "to let other computers connect, list the computers that may: " + listWhy; return; }
    }
    if (!hooks_.attachIq || !hooks_.radioIdentity) { rtlProblem_ = "the engine gave this server no radio samples to send"; return; }
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

namespace {
std::int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

// The API's login: the web page's own account (the same user name and password). No password set on the web page: this computer's own programs only (the server refuses to listen
// beyond this computer then). A good login is remembered for five minutes by a digest of the header, so a program that keeps connecting does not pay the password check each time;
// wrong attempts are counted per client and past five in five minutes the client is told to wait.
cascade::net::ApiAuth ServerSet::checkApiAuth(const std::string& authorization, const std::string& client) {
    namespace na = cascade::net;
    const auto readAccount = [this]() {
        Hooks::WebAccount a;
        std::function<Hooks::WebAccount()> fn;
        {
            std::lock_guard<std::mutex> lk(acctM_);
            fn = webAccountFn_;
        }
        if (fn) { a = fn(); }
        return a;
    };
    const Hooks::WebAccount acct = readAccount();
    // beyond this computer a missing password is not "open": nobody is let in (the password was removed while the interface was on)
    if (acct.record.empty()) { return apiNeedsAccount_.load(std::memory_order_acquire) ? na::ApiAuth::Denied : na::ApiAuth::Allowed; }
    static const std::string kBasic = "Basic ";
    if (authorization.compare(0, kBasic.size(), kBasic) != 0 || authorization.size() > 600) { return na::ApiAuth::Denied; }
    const std::int64_t now = steadyMs();
    std::vector<std::uint8_t> digest;
    std::string derr;
    if (!na::sha256(reinterpret_cast<const std::uint8_t*>(authorization.data()), authorization.size(), digest, derr)) { return na::ApiAuth::Denied; }
    const std::string key(digest.begin(), digest.end());
    std::lock_guard<std::mutex> lk(authM_);
    // a changed user name or password ends every remembered login at once
    const std::string who = acct.user + std::string(1, '\x1f') + acct.record;
    if (who != verifiedFor_) { verified_.clear(); verifiedFor_ = who; }
    for (std::size_t i = 0; i < verified_.size();) {
        if (verified_[i].until <= now) { verified_.erase(verified_.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
        if (verified_[i].digest == key) { return na::ApiAuth::Allowed; }
        ++i;
    }
    const std::int64_t nowS = now / 1000;
    if (!throttle_.allow(client, nowS)) { return na::ApiAuth::TooMany; }
    std::string text;
    if (!na::ws::base64Decode(authorization.substr(kBasic.size()), text) || text.size() > 400) { throttle_.recordFailure(client, nowS); return na::ApiAuth::Denied; }
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos) { throttle_.recordFailure(client, nowS); return na::ApiAuth::Denied; }
    na::PasswordRecord rec;
    std::string perr;
    if (!na::PasswordRecord::parse(acct.record, rec, perr)) { return na::ApiAuth::Denied; }
    std::string verr;
    const bool good = na::verifyLogin(text.substr(0, colon), text.substr(colon + 1), acct.user, rec, verr);
    if (!good) { throttle_.recordFailure(client, nowS); return na::ApiAuth::Denied; }
    {   // the password may have been changed while the derivation ran: a login judged against the old account is not a login
        const Hooks::WebAccount after = readAccount();
        if (after.user != acct.user || after.record != acct.record) { return na::ApiAuth::Denied; }
    }
    throttle_.recordSuccess(client);
    if (verified_.size() >= 16) { verified_.erase(verified_.begin()); }
    verified_.push_back({key, now + 5 * 60 * 1000});
    return na::ApiAuth::Allowed;
}

void ServerSet::applyApi(const Config& c) {
    const bool changed = !configured_ || c.api != cfg_.api || c.apiAll != cfg_.apiAll || c.apiPort != cfg_.apiPort;
    if (!changed) { return; }
    if (api_ != nullptr) { api_->stop(); api_.reset(); }
    apiProblem_.clear();
    {
        std::lock_guard<std::mutex> lk(authM_);
        verified_.clear();
        verifiedFor_.clear();
    }
    apiNeedsAccount_.store(false, std::memory_order_release);
    if (!c.api) { return; }
    // beyond this computer the web page's account must exist: without it nobody could be told from anybody
    if (c.apiAll) {
        const Hooks::WebAccount acct = hooks_.webAccount ? hooks_.webAccount() : Hooks::WebAccount{};
        if (acct.record.empty()) { apiProblem_ = "set a password for the web page first: this interface uses the same account, and it will not listen beyond this computer without one"; return; }
    }
    apiNeedsAccount_.store(c.apiAll, std::memory_order_release);
    {
        const Hooks::WebAccount acct = hooks_.webAccount ? hooks_.webAccount() : Hooks::WebAccount{};
        apiAccountPrint_ = acct.user + std::string(1, '\x1f') + acct.record;
    }
    lastAccountCheckMs_ = steadyMs();
    cascade::net::ApiConfig ac;
    ac.bindAddress = !c.apiBindForTest.empty() ? c.apiBindForTest : (c.apiAll ? "0.0.0.0" : "127.0.0.1");
    ac.allowAllInterfaces = c.apiAll;
    ac.port = static_cast<std::uint16_t>(c.apiPort);
    ac.requireAuth = c.apiAll;
    auto server = std::make_unique<cascade::net::ApiServer>(ac, hub_, [this](const std::string& a, const std::string& peer) { return checkApiAuth(a, peer); });
    std::string why;
    if (!server->start(why)) { apiProblem_ = why.empty() ? std::string("the interface could not start") : why; return; }
    api_ = std::move(server);
    if (hooks_.hostNames) { api_->setHostNames(hooks_.hostNames()); }
}

void ServerSet::applyMcp(const Config& c) {
    const bool changed = !configured_ || c.mcp != cfg_.mcp || c.mcpPort != cfg_.mcpPort || c.mcpToken != cfg_.mcpToken;
    if (!changed) { return; }
    if (mcp_ != nullptr) { mcp_->stop(); mcp_.reset(); }
    mcpProblem_.clear();
    if (!c.mcp) { return; }
    auto ep = std::make_unique<McpEndpoint>();
    std::string why;
    if (!ep->start(hub_, c.mcpPort, c.mcpToken, why)) { mcpProblem_ = why.empty() ? std::string("the MCP server could not start") : why; return; }
    mcp_ = std::move(ep);
}

// Once a pass while the API or the MCP server is on: what the hub shows is brought up to date (the status four times a second, the bookmarks every two seconds, every new decoded line of a
// plugin that is not private), and the latest tuning or mode request a client made is applied through the engine's own commands.
void ServerSet::controlPass() {
    const bool on = api_ != nullptr || mcp_ != nullptr;
    if (!on) { feedLive_ = false; return; }
    const std::int64_t now = steadyMs();
    const int radio = cfg_.controlRadio;
    if (now - lastStatusMs_ >= 250) {
        lastStatusMs_ = now;
        cascade::net::HubStatus st;
        if (hooks_.readStatus && hooks_.readStatus(radio, st)) { hub_.publishStatus(st); }
        else { hub_.publishStatus(cascade::net::HubStatus{}); }
    }
    if (api_ != nullptr && hooks_.hostNames && now - lastHostNamesMs_ >= 1000) {
        lastHostNamesMs_ = now;   // the owner's "Allowed web origins" list may change while the interface runs
        api_->setHostNames(hooks_.hostNames());
    }
    if (api_ != nullptr && hooks_.webAccount && now - lastAccountCheckMs_ >= 500) {
        // the web page's user name or password changed (or was set or removed): every client signed in under the old account is ended now, and the next login is judged afresh
        lastAccountCheckMs_ = now;
        const Hooks::WebAccount acct = hooks_.webAccount();
        const std::string print = acct.user + std::string(1, '\x1f') + acct.record;
        if (print != apiAccountPrint_) {
            apiAccountPrint_ = print;   // (the remembered logins are judged against the account on every request: checkApiAuth drops those made under another)
            api_->closeAllClients();
        }
    }
    if (now - lastBookmarksMs_ >= 2000 && hooks_.readBookmarks) {
        lastBookmarksMs_ = now;
        hub_.publishBookmarks(hooks_.readBookmarks());
    }
    if (hooks_.readDecodes) {
        if (!feedLive_) {
            feedLive_ = true;
            std::uint64_t seq = 0;
            hooks_.readDecodes(seq, nullptr);   // (a null sink: only moves the cursor to the present: what was decoded before the interface was turned on is not shown)
            decodeSeq_ = seq;
        } else {
            hooks_.readDecodes(decodeSeq_, [this](double t, double f, const std::string& d, const std::string& x) { hub_.addDecode(t, f, d, x); });
        }
    }
    if (const std::optional<double> hz = hub_.takeTune()) {
        const std::string why = hooks_.tune ? hooks_.tune(radio, *hz) : std::string("the engine gave no way to tune");
        if (why.empty()) { controlTunes_.fetch_add(1, std::memory_order_relaxed); } else { controlTuneRefused_.fetch_add(1, std::memory_order_relaxed); }
    }
    if (const std::optional<std::string> mode = hub_.takeMode()) {
        const std::string why = hooks_.setMode ? hooks_.setMode(radio, *mode) : std::string("the engine gave no way to set the mode");
        if (why.empty()) { controlModes_.fetch_add(1, std::memory_order_relaxed); } else { controlTuneRefused_.fetch_add(1, std::memory_order_relaxed); }
    }
}

void ServerSet::pass() {
    std::lock_guard<std::mutex> lk(m_);
    controlPass();
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
    // THE HEADER'S TWO NUMBERS follow the radio being served, for the next client (hw-findings item 7): refreshed at most twice a second and never while a client is connected
    if (rtlRadio_ > 0 && !rtl_->clientConnected()) {
        const std::int64_t now = steadyMs();
        if (now - rtlInfoMs_ >= 500) {
            rtlInfoMs_ = now;
            std::uint32_t type = 0, gains = 0;
            if (!(hooks_.tunerInfo && hooks_.tunerInfo(radio, type, gains))) { type = 0; gains = 0; }   // not known: the documented "unknown"
            rtl_->setTunerInfo(type, gains);
        }
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

void ServerSet::setApiBindForTest(const std::string& address) {
    std::lock_guard<std::mutex> lk(m_);
    apiBindForTest_ = address;
}

void ServerSet::configure(const Config& requested) {
    std::lock_guard<std::mutex> lk(m_);
    Config c = requested;
    if (!apiBindForTest_.empty()) { c.apiBindForTest = apiBindForTest_; }   // TESTS ONLY: the engine builds its Config from the settings, which never carry this
    applyTcpAudio(c);
    applyRtl(c);
    applyApi(c);
    applyMcp(c);
    rotator_.configure(c.rotator);
    cfg_ = c;
    configured_ = true;
}

bool ServerSet::active() const {
    std::lock_guard<std::mutex> lk(m_);
    return tcpAudio_ != nullptr || rtl_ != nullptr || api_ != nullptr || mcp_ != nullptr;
}

void ServerSet::stop() {
    std::lock_guard<std::mutex> lk(m_);
    releaseRtl();
    if (tcpAudio_ != nullptr) {
        tcpAudio_->stop();
        tcpAudio_.reset();
    }
    if (api_ != nullptr) { api_->stop(); api_.reset(); }
    if (mcp_ != nullptr) { mcp_->stop(); mcp_.reset(); }
    rotator_.shutdown();
    {
        std::lock_guard<std::mutex> alk(acctM_);
        webAccountFn_ = nullptr;
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
    fact("srv.tcpaudio.dropped", tcpOn ? std::to_string(tcpAudio_->stats().droppedSlow + tcpAudio_->stats().droppedStalled + tcpAudio_->stats().refusedFull + tcpAudio_->stats().refusedPeer) : std::string("0"));
    fact("srv.tcpaudio.problem", tcpAudioProblem_);
    fact("srv.tcpaudio.scope", cfg_.tcpAudio ? scopeOfListed(cfg_.tcpAudioAll, cfg_.tcpAudioAllow) : std::string());
    const bool rtlOn = rtl_ != nullptr && rtl_->running();
    const cascade::net::RtlTcpStats rs = rtlOn ? rtl_->stats() : cascade::net::RtlTcpStats{};
    fact("srv.rtltcp.running", rtlOn ? "1" : "0");
    fact("srv.rtltcp.port", rtlOn ? std::to_string(rtl_->port()) : std::string());
    fact("srv.rtltcp.clients", std::to_string(rs.clients));
    fact("srv.rtltcp.accepted", std::to_string(rs.accepted));
    fact("srv.rtltcp.refused_busy", std::to_string(rs.refusedBusy + rs.refusedPeer + rs.refusedProtocol));   // (a second client, an address not on the list, a connection that did not speak rtl_tcp)
    fact("srv.rtltcp.samples_dropped", std::to_string(rs.samplesDropped));
    fact("srv.rtltcp.commands", std::to_string(rs.commands));
    fact("srv.rtltcp.tunes", std::to_string(rtlTunes_.load()));
    fact("srv.rtltcp.tunes_refused", std::to_string(rtlTuneRefused_.load()));
    fact("srv.rtltcp.ignored", std::to_string(rtlIgnored_.load() + rs.unknownCommands));
    fact("srv.rtltcp.problem", rtlProblem_);
    fact("srv.rtltcp.scope", cfg_.rtl ? scopeOfListed(cfg_.rtlAll, cfg_.rtlAllow) : std::string());
    const bool apiOn = api_ != nullptr && api_->running();
    const cascade::net::ApiStats as = apiOn ? api_->stats() : cascade::net::ApiStats{};
    fact("srv.api.running", apiOn ? "1" : "0");
    fact("srv.api.port", apiOn ? std::to_string(api_->port()) : std::string());
    fact("srv.api.clients", std::to_string(as.connections));
    fact("srv.api.websockets", std::to_string(as.wsOpened));
    fact("srv.api.streams", std::to_string(as.sseOpened));
    fact("srv.api.messages", std::to_string(as.messages));
    fact("srv.api.refused", std::to_string(as.refusedRequests + as.refusedFull + as.authDenied + as.errors));
    fact("srv.api.tunes", std::to_string(controlTunes_.load()));
    fact("srv.api.problem", apiProblem_);
    fact("srv.api.scope", cfg_.api ? (cfg_.apiAll ? std::string("reachable from other computers on your network; each program must sign in with the web page's account, which is sent without encryption") : scopeOf(false)) : std::string());
    const bool mcpOn = mcp_ != nullptr && mcp_->running();
    fact("srv.mcp.running", mcpOn ? "1" : "0");
    fact("srv.mcp.port", mcpOn ? std::to_string(mcp_->port()) : std::string());
    fact("srv.mcp.problem", mcpProblem_);
    fact("srv.mcp.scope", cfg_.mcp ? std::string("this computer only") : std::string());
    const RotatorStatus rs2 = rotator_.status();
    fact("rot.running", rs2.enabled ? "1" : "0");
    fact("rot.connected", rs2.connected ? "1" : "0");
    char pos[64];
    std::snprintf(pos, sizeof pos, "%.1f", rs2.az);
    fact("rot.az", rs2.haveMeasured ? std::string(pos) : std::string());
    std::snprintf(pos, sizeof pos, "%.1f", rs2.el);
    fact("rot.el", rs2.haveMeasured ? std::string(pos) : std::string());
    fact("rot.turns", std::to_string(rs2.turns));
    fact("rot.stops", std::to_string(rs2.stops));
    fact("rot.parks", std::to_string(rs2.parks));
    fact("rot.failures", std::to_string(rs2.failures));
    fact("rot.problem", rs2.problem);
    fact("rot.scope", cfg_.rotator.enabled ? (cfg_.rotator.serial ? std::string("a serial port of this computer") : (cascade::engine::s4::isLoopbackHost(cfg_.rotator.host) ? std::string("this computer only") : std::string("a computer on your network: the commands are sent without encryption or a password"))) : std::string());
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
    if (api_ != nullptr) {
        const cascade::net::ApiStats as = api_->stats();
        s += "p" + std::to_string(as.connections) + "," + std::to_string(as.wsOpened) + "," + std::to_string(as.sseOpened) + "," + std::to_string(as.messages >> 3) + "," + std::to_string(controlTunes_.load()) + "," + std::to_string(as.refusedRequests + as.authDenied);
    }
    s += "|" + apiProblem_ + "|" + mcpProblem_ + (mcp_ != nullptr ? "m" : "");
    {
        const RotatorStatus rs2 = rotator_.status();
        s += "|t" + std::string(rs2.connected ? "1" : "0") + std::to_string(rs2.turns) + "," + std::to_string(rs2.stops) + "," + std::to_string(rs2.parks) + "," + std::to_string(rs2.failures) + "," + std::to_string(static_cast<int>(rs2.az * 10)) + "," + std::to_string(static_cast<int>(rs2.el * 10)) + rs2.problem;
    }
    return s;
}

}  // namespace cascade::engine::s4b
