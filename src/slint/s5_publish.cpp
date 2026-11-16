// s5_publish.cpp - see s5_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "s5_publish.hpp"

#include <string>
#include <vector>

#include "lanek_logic.hpp"
#include "plugin_kit.hpp"
#include "s5_rows.hpp"

namespace foxsdr::slints5 {

namespace {
namespace slk = foxsdr::slintlanek;

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

KRow toK(const slk::Row& r) {
    KRow k;
    k.kind = r.kind;
    k.id = ss(r.id);
    k.label = ss(r.label);
    k.value = ss(r.value);
    k.note = ss(r.note);
    k.on = r.on;
    k.enabled = r.enabled;
    std::vector<slint::SharedString> opts;
    for (const auto& o : r.options) { opts.push_back(ss(o)); }
    k.options = std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(opts));
    k.pick = r.pick;
    k.level = r.level;
    k.tone = r.tone;
    return k;
}
}  // namespace

struct S5Publisher::Impl {
    Env env;
    foxsdr::slintkit::LiveTable table;
    S5Rows rows;
    std::string sig;
    std::string lastCount;
    std::shared_ptr<slint::VectorModel<KRow>> model;
    long lastRebuildTick = -100;

    static S5Rows::Env rowsEnv(Impl* self) {
        S5Rows::Env e;
        e.submit = [self](const FoxCommand& c, std::uint32_t radio, const std::string* longText) {
            const SubmitOutcome o = self->env.submit ? self->env.submit(c, radio, longText) : SubmitOutcome{};
            return o;
        };
        e.selectedRadio = [self] { return self->env.selectedRadio ? self->env.selectedRadio() : 1; };
        e.fact = [self](const std::string& name, std::string& value) { return self->table.fact(name, value); };
        return e;
    }

    explicit Impl(Env e) : env(std::move(e)), table(env.api, env.session), rows(rowsEnv(this)) {}

    void rebuild() {
        const std::vector<slk::Row> r = rows.build();
        const std::string g = slk::signatureOf(r);
        if (g == sig && model) { return; }
        sig = g;
        lastCount = std::to_string(r.size());
        std::vector<KRow> ks;
        ks.reserve(r.size());
        for (const auto& x : r) { ks.push_back(toK(x)); }
        model = std::make_shared<slint::VectorModel<KRow>>(std::move(ks));
        if (env.ui != nullptr) { env.ui->global<LaneS5>().set_rows_source(model); }
    }
};

S5Publisher::S5Publisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
S5Publisher::~S5Publisher() = default;

void S5Publisher::wire() {
    if (p_->env.ui == nullptr) { return; }
    p_->env.ui->global<LaneS5>().on_act([this](slint::SharedString id, slint::SharedString text) {
        p_->rows.act(std::string(id.data(), id.size()), std::string(text.data(), text.size()));
        p_->rebuild();
    });
    p_->rebuild();
}

void S5Publisher::tick(long tickNo) {
    // about twice a second the rows are read again from the facts; at once after any action or answer
    if (p_->rows.dirty() || tickNo - p_->lastRebuildTick >= 15) {
        p_->lastRebuildTick = tickNo;
        p_->rebuild();
    }
}

void S5Publisher::onAnswer(const FoxCommandResult& r) { p_->rows.onAnswer(r); }

std::string S5Publisher::readState(const std::string& what, std::string& out) const {
    Impl& p = *p_;
    if (what == "sent") { out = std::to_string(p.rows.sent()); return std::string(); }
    if (what == "refused") { out = std::to_string(p.rows.refused()); return std::string(); }
    if (what == "answered") { out = std::to_string(p.rows.answered()); return std::string(); }
    if (what == "note.source") { out = p.rows.note(); return std::string(); }
    if (what == "rows.source") { out = p.lastCount; return std::string(); }
    if (what.rfind("draft.", 0) == 0) { out = p.rows.draft(what.substr(6)); return std::string(); }
    if (what.rfind("fact.", 0) == 0) { out = p.rows.factText(what.substr(5)); return std::string(); }
    return "s5.<sent|refused|answered|note.source|rows.source|draft.<id>|fact.<name>>, not " + what;
}

}  // namespace foxsdr::slints5
