// wire_c_cat.cpp - see wire_c_cat.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c_cat.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "core/app_commands.hpp"
#include "engine/fox_engine_api.hpp"
#include "foxsdr_api.h"

namespace cascade::engine::wirec {

namespace {

constexpr const char* kModes[] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};   // FOXAPI_DEMOD_NFM.. = 1..8

}  // namespace

// The one session every client of the emulator shares; its calls are serialised (the emulator calls from one thread per client).
struct CatEmulation::Bridge {
    const FoxEngineApi* t = nullptr;
    FoxSession* s = nullptr;
    std::mutex m;

    ~Bridge() {
        if (t != nullptr && s != nullptr) { t->close_session(s); }
    }
    bool read(FoxReceiverState& st) {
        for (int i = 0; i < 4; ++i) {
            st = FoxReceiverState{};
            st.structSize = sizeof(st);
            const std::int32_t r = t->read_state(s, &st);
            if (r == FOXAPI_OK) { return true; }
            if (r != FOXAPI_BUSY) { return false; }   // BUSY: the seqlock read overlapped a publish; ask again
        }
        return false;
    }
    bool submit(const FoxCommand& cmd) {
        FoxCommand c = cmd;
        c.structSize = sizeof(FoxCommand);
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const std::int32_t n = t->submit(s, &c, 1, &sr);
        // drain the answers so the session's result queue never fills (the outcome of a retune is the next read's business)
        FoxCommandResult r[8];
        for (FoxCommandResult& x : r) { x.structSize = sizeof(x); }
        while (t->poll_results(s, r, 8) == 8) {}
        return n > 0 && sr.status == FOXAPI_OK;
    }
};

bool CatEmulation::configure(bool enabled, const std::string& dialect, int port, bool bindAll, const Opener& opener) {
    const bool was = running();
    stop();
    error_.clear();
    if (!enabled) { return was; }
    if (port <= 0) {
        error_ = "give the port the logging program connects to";
        return was;
    }
    if (port == 4532) {
        error_ = "4532 is the port of FoxSDR's own rigctl server; choose another";
        return was;
    }
    if (opener.table == nullptr || !opener.open) {
        error_ = "the engine is not ready";
        return was;
    }
    FoxSession* session = opener.open();
    if (session == nullptr) {
        error_ = "the engine would not open a CAT session";
        return was;
    }
    auto b = std::make_shared<Bridge>();
    b->t = opener.table;
    b->s = session;
    cascade::net::RigBackend rig;
    rig.frequencyHz = [b] {
        std::lock_guard<std::mutex> lk(b->m);
        FoxReceiverState st;
        return b->read(st) ? st.tunedHz : 0.0;
    };
    rig.setFrequencyHz = [b](double hz) {
        if (!std::isfinite(hz) || hz <= 0.0) { return false; }
        std::lock_guard<std::mutex> lk(b->m);
        // The CAT session's closed list of operations is the centre, the offset, the mode and the passband (what rigctl's F and M send). The frequency heard is the centre plus the
        // VFO offset, so the centre is moved to put the same offset on the new frequency; a frequency the receiver cannot tune is refused by the engine and the emulator answers E;.
        FoxReceiverState st;
        const double offset = b->read(st) ? st.vfoOffsetHz : 0.0;
        return b->submit(cascade::core::cmd::makeNum(FOXAPI_OP_SET_CENTRE, hz - offset));
    };
    rig.mode = [b]() -> std::string {
        std::lock_guard<std::mutex> lk(b->m);
        FoxReceiverState st;
        if (!b->read(st) || st.demodMode < 1u || st.demodMode > 8u) { return "NFM"; }
        return kModes[st.demodMode - 1u];
    };
    rig.setMode = [b](const std::string& name) {
        for (std::size_t i = 0; i < 8; ++i) {
            if (name == kModes[i]) {
                std::lock_guard<std::mutex> lk(b->m);
                return b->submit(cascade::core::cmd::makeInt(FOXAPI_OP_SET_MODE, static_cast<std::int64_t>(i + 1)));
            }
        }
        return false;
    };
    rig.signalFraction = [b] {
        std::lock_guard<std::mutex> lk(b->m);
        FoxReceiverState st;
        return b->read(st) ? std::min(1.0, std::max(0.0, st.sMeter)) : 0.0;
    };
    rig.running = [b] {
        std::lock_guard<std::mutex> lk(b->m);
        FoxReceiverState st;
        return b->read(st) && (st.flags & FOXAPI_RX_RUNNING) != 0u;
    };
    const cascade::net::CatDialect d = dialect == "yaesu-ft891" ? cascade::net::CatDialect::YaesuFT891 : cascade::net::CatDialect::KenwoodTS480;
    auto svc = std::make_unique<cascade::net::CatTcpService>(d, std::move(rig));
    cascade::net::CatEmuFact fact;
    if (!svc->start(static_cast<std::uint16_t>(port), bindAll, fact)) {
        error_ = fact.text.empty() ? "the port could not be opened" : fact.text;
        return was;
    }
    bridge_ = std::move(b);
    service_ = std::move(svc);
    return true;
}

void CatEmulation::stop() {
    if (service_) {
        service_->stop();   // a hard stop: no thread of the service runs after it returns
        service_.reset();
    }
    bridge_.reset();   // closes the session
}

bool CatEmulation::running() const { return service_ && service_->running(); }
int CatEmulation::boundPort() const { return service_ ? static_cast<int>(service_->boundPort()) : 0; }
int CatEmulation::clients() const { return service_ ? service_->clients() : 0; }
std::uint64_t CatEmulation::commands() const { return service_ ? service_->commandsServed() : 0u; }

}  // namespace cascade::engine::wirec
