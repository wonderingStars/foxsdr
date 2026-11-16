// cat_link.cpp - see cat_link.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/cat_link.hpp"

#include <algorithm>
#include <cmath>

#include "core/app_commands.hpp"
#include "foxsdr_api.h"

namespace cascade::engine::wirec {

namespace rc = cascade::source::rigcat;
namespace rig = cascade::core::rig;

namespace {

constexpr const char* kModes[] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};   // FOXAPI_DEMOD_NFM.. = 1..8

int foxModeIndex(const std::string& name) {
    for (std::size_t i = 0; i < 8; ++i) {
        if (name == kModes[i]) { return static_cast<int>(i + 1); }
    }
    return 0;
}

// OmniRig's mode names (core/omnirig_client.hpp) as the FoxSDR mode that demodulates them.
std::string foxModeForOmniRigName(const std::string& n) {
    if (n == "CW" || n == "CW-R") { return "CW"; }
    if (n == "USB" || n == "DIG-U") { return "USB"; }
    if (n == "LSB" || n == "DIG-L") { return "LSB"; }
    if (n == "AM") { return "AM"; }
    if (n == "FM") { return "NFM"; }
    return std::string();
}

}  // namespace

// ================================================================================ what may be sent
const std::vector<std::string>& catCommandsSent() {
    static const std::vector<std::string> v = {"FA;", "MD;", "MD0;"};
    return v;
}

bool isCatReadCommand(rc::Dialect d, const std::string& message) {
    if (message == "FA;") { return true; }
    return message == (d == rc::Dialect::Yaesu ? "MD0;" : "MD;");
}

bool GatedStream::write(const void* data, std::size_t n) {
    if (data == nullptr || n == 0) { return false; }   // nothing to send is not a message
    const std::string bytes(static_cast<const char*>(data), n);
    std::size_t from = 0;
    while (from < bytes.size()) {
        const std::size_t semi = bytes.find(';', from);
        if (semi == std::string::npos) {   // an unterminated message is not a message this link sends
            ++blocked_;
            lastBlocked_ = bytes.substr(from, 16);
            return false;
        }
        const std::string one = bytes.substr(from, semi - from + 1);
        if (!isCatReadCommand(dialect_, one)) {
            ++blocked_;
            lastBlocked_ = one.substr(0, 16);
            return false;
        }
        from = semi + 1;
    }
    if (!inner_.write(data, n)) { return false; }
    ++written_;
    return true;
}

// ================================================================================ the link
struct CatLink::Run {
    CatLinkConfig cfg;
    CatLinkSeams seams;
    std::atomic<bool> stopFlag{false};
    mutable std::mutex m;
    CatLinkStatus st;

    void setState(const std::string& state, const std::string& error = std::string()) {
        std::lock_guard<std::mutex> lk(m);
        st.state = state;
        if (state == "error" || !error.empty()) { st.error = error; }
    }
};

namespace {

// The engine as the link sees it: one internal session, the focused radio.
struct Bridge {
    const FoxEngineApi* t = nullptr;
    FoxSession* s = nullptr;
    ~Bridge() {
        if (t != nullptr && s != nullptr) { t->close_session(s); }
    }
    bool read(FoxReceiverState& st) const {
        for (int i = 0; i < 4; ++i) {
            st = FoxReceiverState{};
            st.structSize = sizeof(st);
            const std::int32_t r = t->read_state(s, &st);
            if (r == FOXAPI_OK) { return true; }
            if (r != FOXAPI_BUSY) { return false; }
        }
        return false;
    }
    bool submit(const FoxCommand& cmd) const {
        FoxCommand c = cmd;
        c.structSize = sizeof(FoxCommand);
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const std::int32_t n = t->submit(s, &c, 1, &sr);
        FoxCommandResult r[8];
        for (FoxCommandResult& x : r) { x.structSize = sizeof(x); }
        while (t->poll_results(s, r, 8) == 8) {}
        return n > 0 && sr.status == FOXAPI_OK;
    }
    // The centre moves so the heard frequency (centre plus the VFO's offset) becomes `hz`; the engine refuses a frequency the receiver cannot tune.
    bool setHeardFrequency(double hz) const {
        if (!std::isfinite(hz) || hz <= 0.0) { return false; }
        FoxReceiverState st;
        const double offset = read(st) ? st.vfoOffsetHz : 0.0;
        return submit(cascade::core::cmd::makeNum(FOXAPI_OP_SET_CENTRE, hz - offset));
    }
    bool setFoxMode(const std::string& name) const {
        const int i = foxModeIndex(name);
        return i != 0 && submit(cascade::core::cmd::makeInt(FOXAPI_OP_SET_MODE, i));
    }
    double heardHz() const {
        FoxReceiverState st;
        return read(st) ? st.tunedHz : 0.0;
    }
    std::string foxMode() const {
        FoxReceiverState st;
        if (!read(st) || st.demodMode < 1u || st.demodMode > 8u) { return "NFM"; }
        return kModes[st.demodMode - 1u];
    }
};

// What the rig said last, applied to FoxSDR once per CHANGE (a person who retunes FoxSDR is not fought: nothing is re-sent until the rig moves).
struct Follower {
    Bridge& b;
    CatLink::Run* run = nullptr;
    bool haveHz = false, haveMode = false;
    double lastHz = 0.0;
    std::string lastMode;
    void apply(bool freqKnown, double hz, const std::string& foxMode);
};

}  // namespace

void Follower::apply(bool freqKnown, double hz, const std::string& foxMode) {
    if (freqKnown && run->cfg.followFrequency && (!haveHz || std::fabs(hz - lastHz) >= 1.0)) {
        haveHz = true;
        lastHz = hz;
        const bool ok = b.setHeardFrequency(hz);
        std::lock_guard<std::mutex> lk(run->m);
        if (ok) { ++run->st.applied; } else { ++run->st.refused; }
    }
    if (!foxMode.empty() && run->cfg.followMode && (!haveMode || foxMode != lastMode)) {
        haveMode = true;
        lastMode = foxMode;
        const bool ok = b.setFoxMode(foxMode);
        std::lock_guard<std::mutex> lk(run->m);
        if (ok) { ++run->st.applied; } else { ++run->st.refused; }
    }
}

namespace {

// OmniRig's port as the link hands it to the client: reads pass through, BOTH writes are refused here and counted (the link never asks for one; this is the second lock on the door).
class ReadOnlyOmniPort final : public rig::OmniRigRigPort {
public:
    explicit ReadOnlyOmniPort(rig::OmniRigRigPort& inner) : inner_(inner) {}
    std::int32_t status() override { return inner_.status(); }
    std::int32_t freq() override { return inner_.freq(); }
    std::int32_t freqA() override { return inner_.freqA(); }
    std::int32_t freqB() override { return inner_.freqB(); }
    std::uint32_t mode() override { return inner_.mode(); }
    std::uint32_t vfo() override { return inner_.vfo(); }
    std::uint32_t split() override { return inner_.split(); }
    std::uint32_t txState() override { return inner_.txState(); }
    bool putFreq(std::int32_t) override { ++refusedWrites; return false; }
    bool putMode(std::uint32_t) override { ++refusedWrites; return false; }
    std::uint64_t refusedWrites = 0;

private:
    rig::OmniRigRigPort& inner_;
};

const char* omniStatusWords(std::int32_t s) {
    switch (s) {
        case rig::ST_NOTCONFIGURED: return "no rig is set up in OmniRig for this slot: open OmniRig and choose the rig and its port";
        case rig::ST_DISABLED: return "the rig is switched off in OmniRig";
        case rig::ST_PORTBUSY: return "OmniRig's serial port is in use by another program";
        case rig::ST_NOTRESPONDING: return "the rig does not answer OmniRig";
        default: return "";
    }
}

void runFollow(CatLink::Run& run, Bridge& bridge, rc::ByteStream& stream) {
    const rc::Dialect d = run.cfg.dialect == "yaesu" ? rc::Dialect::Yaesu : rc::Dialect::Kenwood;
    GatedStream gate(stream, d);
    rc::RigFollower f(d, gate);
    Follower fol{bridge, &run};
    run.setState("following");
    while (!run.stopFlag.load()) {
        const bool ok = f.poll(std::chrono::milliseconds(600));
        {
            std::lock_guard<std::mutex> lk(run.m);
            ++run.st.polls;
            run.st.rigErrors = f.errorsFromRig();
            run.st.blocked = gate.blocked();
            if (ok) { ++run.st.answers; }
            run.st.rigKnown = f.state().frequencyKnown;
            run.st.rigHz = f.state().frequencyHz;
            run.st.rigMode = f.state().modeKnown ? rc::rigModeName(f.state().mode) : "";
        }
        if (!ok && (f.lastError().rfind("the rig connection failed", 0) == 0 || f.lastError().rfind("could not write to the rig", 0) == 0)) {
            run.setState("error", "the port stopped answering: " + f.lastError());
            return;
        }
        if (ok) { fol.apply(f.state().frequencyKnown, f.state().frequencyHz, f.state().modeKnown ? rc::foxModeForRigMode(f.state().mode) : std::string()); }
        const auto until = std::chrono::steady_clock::now() + run.seams.pollEvery;
        while (!run.stopFlag.load() && std::chrono::steady_clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
}

void runEmulate(CatLink::Run& run, Bridge& bridge, rc::ByteStream& stream) {
    const rc::Dialect d = run.cfg.dialect == "yaesu" ? rc::Dialect::Yaesu : rc::Dialect::Kenwood;
    rc::EmulatorHost host;
    host.frequencyHz = [&bridge] { return bridge.heardHz(); };
    host.foxMode = [&bridge] { return bridge.foxMode(); };
    host.setFrequencyHz = [&bridge](double hz) { return bridge.setHeardFrequency(hz); };
    host.setFoxMode = [&bridge](const std::string& m) { return bridge.setFoxMode(m); };
    rc::RigEmulator em(d, std::move(host));
    run.setState("emulating");
    std::uint8_t buf[256];
    while (!run.stopFlag.load()) {
        std::size_t got = 0;
        if (!stream.read(buf, sizeof(buf), got, std::chrono::milliseconds(50))) {
            run.setState("error", std::string("the port stopped answering: ") + stream.lastError());
            return;
        }
        if (got == 0) { continue; }
        const std::string answer = em.feed(buf, got);
        if (!answer.empty() && !stream.write(answer.data(), answer.size())) {
            run.setState("error", std::string("the port would not take the answer: ") + stream.lastError());
            return;
        }
        std::lock_guard<std::mutex> lk(run.m);
        run.st.served = em.commandsHandled();
        run.st.refused = em.commandsRefused();
    }
}

void runOmniRig(CatLink::Run& run, Bridge& bridge, rig::OmniRigRigPort& rawPort) {
    ReadOnlyOmniPort port(rawPort);
    rig::OmniRigClient client(&port);
    Follower fol{bridge, &run};
    run.setState("following");
    while (!run.stopFlag.load()) {
        const rig::RigSnapshot s = client.snapshot();
        {
            std::lock_guard<std::mutex> lk(run.m);
            ++run.st.polls;
            run.st.blocked = port.refusedWrites;
            if (s.online) {
                ++run.st.answers;
                run.st.error.clear();
                run.st.rigKnown = true;
                run.st.rigHz = s.freqHz;
                run.st.rigMode = s.modeText;
            } else {
                run.st.rigKnown = false;
                run.st.rigMode.clear();
                run.st.error = omniStatusWords(s.status);
            }
        }
        if (s.online && !s.transmitting) { fol.apply(true, s.freqHz, foxModeForOmniRigName(s.modeText)); }
        const auto until = std::chrono::steady_clock::now() + run.seams.pollEvery;
        while (!run.stopFlag.load() && std::chrono::steady_clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
}

void threadMain(std::shared_ptr<CatLink::Run> runp) {
    CatLink::Run& run = *runp;
    Bridge bridge;
    bridge.t = run.seams.table;
    if (run.seams.openSession) { bridge.s = run.seams.openSession(); }
    if (bridge.t == nullptr || bridge.s == nullptr) {
        run.setState("error", "the engine would not open a CAT session");
        return;
    }
    std::string err;
    if (run.cfg.mode == "omnirig") {
        std::unique_ptr<rig::OmniRigRigPort> port = run.seams.openOmniRig ? run.seams.openOmniRig(run.cfg.rig, err) : openOmniRigCom(run.cfg.rig, err);
        if (!port) {
            run.setState("error", err.empty() ? CatLink::omniRigNeededSentence() : err);
            return;
        }
        runOmniRig(run, bridge, *port);
        return;
    }
    rc::SerialSettings ss;
    ss.baud = run.cfg.baud;
    ss.stopBits = run.cfg.stopBits;
    std::unique_ptr<rc::ByteStream> stream = run.seams.openSerial ? run.seams.openSerial(run.cfg.port, ss, err) : rc::openSerialByteStream(run.cfg.port, ss, err);
    if (!stream) {
        run.setState("error", err.empty() ? std::string("the port could not be opened") : err);
        return;
    }
    if (run.cfg.mode == "emulate") { runEmulate(run, bridge, *stream); } else { runFollow(run, bridge, *stream); }
    stream->close();
}

}  // namespace

std::string CatLink::omniRigNeededSentence() {
    return "OmniRig is a free program by Alex Shovkoplyas, VE3NEA (Afreet Software) that lets other programs read and set a transceiver. It is not installed on this computer. "
           "Get it from https://www.dxatlas.com/OmniRig/ , run its installer, open OmniRig and choose your rig and its serial port, then choose OmniRig here. FoxSDR does not download it.";
}

void CatLink::configure(const CatLinkConfig& cfg, const CatLinkSeams& seams) {
    stop();
    auto r = std::make_shared<Run>();
    r->cfg = cfg;
    r->seams = seams;
    r->st.kind = cfg.mode;
    if (cfg.mode == "off") {
        run_ = r;
        return;
    }
    if (cfg.mode != "follow" && cfg.mode != "emulate" && cfg.mode != "omnirig") {
        r->st.state = "error";
        r->st.error = "no such CAT mode";
        run_ = r;
        return;
    }
    if (cfg.mode != "omnirig") {
        if (cfg.port.empty()) { r->st.state = "error"; r->st.error = "give the serial port (for example COM3)"; run_ = r; return; }
        if (cfg.baud < 300 || cfg.baud > 4000000) { r->st.state = "error"; r->st.error = "give the baud rate the rig is set to (300 to 4000000)"; run_ = r; return; }
    }
    if (cfg.mode == "omnirig" && (cfg.rig < 1 || cfg.rig > 2)) { r->st.state = "error"; r->st.error = "OmniRig has rig 1 and rig 2"; run_ = r; return; }
    r->st.state = "opening";
    run_ = r;
    thread_ = std::thread([r] { threadMain(r); });
}

void CatLink::stop() {
    if (run_) { run_->stopFlag.store(true); }
    if (thread_.joinable()) { thread_.join(); }
    run_.reset();
}

bool CatLink::running() const {
    if (!run_) { return false; }
    std::lock_guard<std::mutex> lk(run_->m);
    return run_->st.state == "following" || run_->st.state == "emulating" || run_->st.state == "opening";
}

CatLinkStatus CatLink::status() const {
    if (!run_) { return CatLinkStatus{}; }
    std::lock_guard<std::mutex> lk(run_->m);
    return run_->st;
}

}  // namespace cascade::engine::wirec
