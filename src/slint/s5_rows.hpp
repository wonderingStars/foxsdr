// s5_rows.hpp - SCREEN LANE S5's ROWS (wave 8; docs/multi-radio-records/screen-s5-sources.md): what the Source section's extra panel shows and what each of its controls sends, with no Slint and no engine
// library: the facts come in through `Env::fact` and the commands leave through `Env::submit`, so a test can stand in for both and read back exactly what the engine would have been sent.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "foxsdr_api.h"
#include "lanek_logic.hpp"

namespace foxsdr::slints5 {

// What one submit came to.
struct SubmitOutcome {
    std::uint64_t ticket = 0;      // 0 = the engine never took it
    int status = FOXAPI_OK;        // what submit answered
    std::string message;           // the host's own words when the call refused it
};

class S5Rows {
public:
    struct Env {
        // One command aimed at radio `radio` (1-based; 0 = the selected radio). `longText` is the command's text when it is longer than the struct's slot.
        std::function<SubmitOutcome(const FoxCommand&, std::uint32_t radio, const std::string* longText)> submit;
        std::function<int()> selectedRadio;                                    // 1-based
        std::function<bool(const std::string& name, std::string& value)> fact;  // "fact.<name>"'s text; false when the engine has none
    };
    explicit S5Rows(Env env) : env_(std::move(env)) {}

    // The rows of the Source section's panel as they stand now (the handlers are registered as a side effect).
    std::vector<foxsdr::slintlanek::Row> build();
    // A control was used (its row id and the text it carries: a field's text, a choice's index, a tick's "1" or "0"). False when no row has that id.
    bool act(const std::string& id, const std::string& text);
    // The engine's answer to a command this object sent (others are ignored).
    void onAnswer(const FoxCommandResult& r);

    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    long sent() const { return sent_; }
    long refused() const { return refused_; }
    long answered() const { return answered_; }
    const std::string& note() const { return note_; }
    std::string draft(const std::string& id) const { const auto it = draft_.find(id); return it == draft_.end() ? std::string() : it->second; }
    std::string factText(const std::string& name) const;

private:
    struct Cmd { std::uint32_t op = 0; long long i0 = 0, i1 = 0; double n0 = 0, n1 = 0; std::string text; };
    enum Kind { kPlain = 0, kClaim };
    struct Pending { std::string what; int kind = kPlain; };

    std::string fact(const std::string& name) const;
    int sel() const;
    std::string rn() const { return std::to_string(sel()); }
    std::string dv(const std::string& id, const std::string& def = std::string()) const;
    int pv(const std::string& id, int def) const;
    bool bv(const std::string& id, bool def) const;
    void setNote(const std::string& t, int tone) { note_ = t; noteTone_ = tone; dirty_ = true; }
    void send(const std::string& what, const Cmd& k, int kind = kPlain, std::uint32_t radio = 0);
    void refuseHere(const std::string& what, const std::string& why);

    void field(std::vector<foxsdr::slintlanek::Row>& v, const std::string& id, const std::string& label, const std::string& def, const std::string& tip = std::string());
    void key(std::vector<foxsdr::slintlanek::Row>& v, const std::string& id, const std::string& label, std::function<void()> fn, bool enabled = true, const std::string& tip = std::string());
    void tick(std::vector<foxsdr::slintlanek::Row>& v, const std::string& id, const std::string& label, bool on, std::function<void(bool)> fn, const std::string& tip = std::string());
    void choice(std::vector<foxsdr::slintlanek::Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int pick, std::function<void(int)> fn, const std::string& tip = std::string());
    void choiceD(std::vector<foxsdr::slintlanek::Row>& v, const std::string& id, const std::string& label, const std::vector<std::string>& opts, int def);
    void tickD(std::vector<foxsdr::slintlanek::Row>& v, const std::string& id, const std::string& label, bool def, const std::string& tip = std::string());

    void buildRawIq(std::vector<foxsdr::slintlanek::Row>& v);
    void buildPpm(std::vector<foxsdr::slintlanek::Row>& v);
    void buildPool(std::vector<foxsdr::slintlanek::Row>& v);
    void buildDirect(std::vector<foxsdr::slintlanek::Row>& v);

    Env env_;
    std::map<std::string, std::function<void(const std::string&)>> handlers_;
    std::map<std::string, std::string> draft_;    // what the person typed or chose and has not sent (never saved)
    std::map<std::uint64_t, Pending> pending_;    // ticket -> what it was
    std::string note_;
    int noteTone_ = 0;
    std::string armed_;                           // the id of a key pressed once that asks for a second press
    long sent_ = 0, refused_ = 0, answered_ = 0;
    bool dirty_ = true;
};

}  // namespace foxsdr::slints5
