// radio_move_host.hpp - THE WINDOW'S SIDE OF "THIS RADIO IS IN USE BY ANOTHER FOXSDR WINDOW" (field fix A2, 2026-11-19; the engine's half is engine/engine_handoff.cpp, the words are core/radio_handoff.hpp's sheetFor()).
//
// The engine publishes two facts: fact.radio.handoff ("<seq>\t<state>\t<model>\t<last four of the serial>\t<detail>", empty when no question is open) and fact.radio.handoffMoved (the same shape, for the window that
// LET a radio go). poll() shows the Handover sheet ONCE per sequence number - a fact that stays set does not reopen a sheet the person has answered - and the sheet's two keys answer through submit():
// "Move it here" (go) sends 1, "Leave it there" (stay) sends 0, and the one-key notices ("OK") send 0, which only puts the question away. Plain C++ over four std::functions, so a test holds it with no window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdlib>
#include <functional>
#include <string>

#include "core/radio_handoff.hpp"

namespace foxsdr::slinthost {

class RadioMoveHost {
public:
    struct Hooks {
        std::function<void(const cascade::core::handoff::Sheet&)> show;
        std::function<void()> hide;
        std::function<void(bool move)> submit;   // FOXAPP_OP_RADIO_HANDOFF
    };
    explicit RadioMoveHost(Hooks hooks) : hooks_(std::move(hooks)) {}

    // Called every tick with the two facts' current values ("" when absent).
    void poll(const std::string& handoffFact, const std::string& movedFact) {
        Parsed q;
        if (parse(handoffFact, q)) {
            if (q.seq != lastSeq_) {
                lastSeq_ = q.seq;
                present(q, /*answerable=*/true);
            }
        } else if (open_ && answerable_) {
            hide();   // the engine put the question away (the move began, or the holder let go): the sheet goes with it
        }
        Parsed m;
        if (parse(movedFact, m) && m.seq != lastMovedSeq_) {
            lastMovedSeq_ = m.seq;
            present(m, /*answerable=*/false);
        }
    }
    // The sheet's keys.
    void confirm() {
        if (!open_) { return; }
        const bool move = state_ == "ask";
        close();
        if (move && hooks_.submit) { hooks_.submit(true); }
        else if (answerable_ && hooks_.submit) { hooks_.submit(false); }
    }
    void cancel() {
        if (!open_) { return; }
        const bool answerable = answerable_;
        close();
        if (answerable && hooks_.submit) { hooks_.submit(false); }
    }
    bool open() const { return open_; }
    // How many sheets this host has shown (a test counts them: once per failed open).
    int shown() const { return shown_; }

private:
    struct Parsed {
        unsigned seq = 0;
        std::string state, model, last4, detail;
    };
    static bool parse(const std::string& v, Parsed& out) {
        if (v.empty()) { return false; }
        std::string f[5];
        std::size_t at = 0;
        for (int i = 0; i < 5; ++i) {
            const std::size_t t = v.find('\t', at);
            f[i] = v.substr(at, t == std::string::npos ? std::string::npos : t - at);
            if (t == std::string::npos) { break; }
            at = t + 1;
        }
        out.seq = static_cast<unsigned>(std::strtoul(f[0].c_str(), nullptr, 10));
        out.state = f[1];
        out.model = f[2];
        out.last4 = f[3];
        out.detail = f[4];
        return out.seq != 0 && !out.state.empty();
    }
    void present(const Parsed& p, bool answerable) {
        const cascade::core::handoff::Sheet s = cascade::core::handoff::sheetFor(p.state, p.model, p.last4, p.detail);
        if (!s.shown()) { return; }
        state_ = p.state;
        answerable_ = answerable;
        open_ = true;
        ++shown_;
        if (hooks_.show) { hooks_.show(s); }
    }
    void hide() { close(); }
    void close() {
        open_ = false;
        if (hooks_.hide) { hooks_.hide(); }
    }

    Hooks hooks_;
    unsigned lastSeq_ = 0, lastMovedSeq_ = 0;
    bool open_ = false, answerable_ = false;
    std::string state_;
    int shown_ = 0;
};

}  // namespace foxsdr::slinthost
