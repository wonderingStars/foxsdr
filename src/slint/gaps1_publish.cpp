// gaps1_publish.cpp - see gaps1_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gaps1_publish.hpp"

#include <algorithm>
#include <utility>

#include "core/i18n.hpp"
#include "core/tester_link.hpp"
#include "gui/link_request_poll.hpp"

namespace foxsdr::slintgaps1 {

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

// THE LIVE ANSWER: the site is asked on a detached worker (core::BetaLinkNameSender); a bounded run never gets this one.
class LiveNet final : public LinkNet {
public:
    void resolve(const std::string& appToken) override { sender_.send(cascade::core::betaApiBaseUrl(), appToken); }
    bool take(ResolveOutcome& outcome, std::string& name) override {
        const std::optional<cascade::core::BetaLinkResolved> r = sender_.takeResult();
        if (!r.has_value()) { return false; }
        name = r->name;
        outcome = r->outcome == cascade::core::BetaLinkOutcome::Ok        ? ResolveOutcome::Ok
                  : r->outcome == cascade::core::BetaLinkOutcome::Invalid ? ResolveOutcome::Invalid
                                                                         : ResolveOutcome::NetworkError;
        return true;
    }

private:
    cascade::core::BetaLinkNameSender sender_;
};

class CannedNet final : public LinkNet {
public:
    explicit CannedNet(std::string spec) : spec_(std::move(spec)) {}
    void resolve(const std::string&) override { ready_ = true; }
    bool take(ResolveOutcome& outcome, std::string& name) override {
        if (!ready_) { return false; }
        ready_ = false;
        name.clear();
        if (spec_ == "!invalid") { outcome = ResolveOutcome::Invalid; }
        else if (spec_ == "!network") { outcome = ResolveOutcome::NetworkError; }
        else { outcome = ResolveOutcome::Ok; name = cascade::core::sanitizeTesterName(spec_); }
        return true;
    }

private:
    std::string spec_;
    bool ready_ = false;
};

}  // namespace

std::unique_ptr<LinkNet> makeLiveLinkNet() { return std::make_unique<LiveNet>(); }
std::unique_ptr<LinkNet> makeCannedLinkNet(const std::string& spec) { return std::make_unique<CannedNet>(spec); }

struct Publisher::Impl {
    Env env;
    LinkFlow flow;
    cascade::gui::LinkRequestPoll poll;
    std::vector<FaultNotice> allFaults;
    std::vector<std::string> seenFaults;
    std::string faultSig;
    std::string linkSig;
    std::string diagStatusShown;
    bool diagOpen = false;
    int stored = 0;              // links stored (the drive scripts read it back)
    std::string storedName;
    std::string reenabled;       // the last plugin id RE-ENABLE asked for
    long lastFaultTick = -1000;
    long lastPollTick = -1000;
    explicit Impl(Env e) : env(std::move(e)) {}

    void publishFaults() {
        const std::vector<FaultNotice> shown = unseenFaults(allFaults, seenFaults);
        std::string sig;
        for (const FaultNotice& n : shown) { sig += n.id + (n.permanent ? "|1;" : "|0;"); }
        if (sig == faultSig) { return; }
        faultSig = sig;
        std::vector<FaultCard> cards;
        for (const FaultNotice& n : shown) {
            FaultCard c{};
            c.id = ss(n.id);
            c.text = ss(faultSentence(n));
            c.permanent = n.permanent;
            cards.push_back(c);
        }
        env.ui->global<Gaps1>().set_faults(std::make_shared<slint::VectorModel<FaultCard>>(std::move(cards)));
    }

    void publishLink() {
        std::string text, err;
        bool open = flow.open();
        if (flow.hasPending()) {
            text = linkQuestion(flow.pending(), env.iface ? env.iface().testerAppTokenName : std::string());
        } else if (!flow.error().empty()) {
            err = flow.error();
        }
        const std::string sig = std::string(open ? "1" : "0") + "|" + text + "|" + err;
        if (sig == linkSig) { return; }
        linkSig = sig;
        auto& g = env.ui->global<Gaps1>();
        g.set_link_text(ss(text));
        g.set_link_error(ss(err));
        g.set_link_open(open);
    }

    void publishDiagStatus() {
        if (!diagOpen || !env.diagStatus) { return; }
        const std::string s = env.diagStatus();
        if (s == diagStatusShown) { return; }
        diagStatusShown = s;
        env.ui->global<Gaps1>().set_diag_status(ss(s));
    }

    void storeLink(const LinkPending& p) {
        if (!env.iface || !env.saveIface) { return; }
        // setTesterAppToken: the portal-token identity is superseded, not kept beside the new one.
        cascade::core::AppConfig half = env.iface();
        half.testerAppToken = p.token;
        half.testerAppTokenName = p.name;
        half.testerToken.clear();
        half.testerTokenInvalid = false;
        env.saveIface(half);
        ++stored;
        storedName = p.name;
        // THE TOKEN IS A CREDENTIAL: nothing of it is said.
        if (env.say) { env.say("tester: this FoxSDR is now linked (the link was confirmed by name and the tester pressed Link)"); }
    }
};

Publisher::Publisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
Publisher::~Publisher() = default;

void Publisher::wire() {
    auto& g = p_->env.ui->global<Gaps1>();
    Impl* p = p_.get();
    g.set_diag_text(ss(diagOfferText()));
    g.on_diag_copy([p] { if (p->env.copyDiagnostics) { p->env.copyDiagnostics(); } });
    g.on_diag_reports([p] { if (p->env.openReports) { p->env.openReports(); } });
    g.on_diag_not_now([p] { p->diagOpen = false; p->env.ui->global<Gaps1>().set_diag_open(false); });
    g.on_link_yes([p] {
        if (const std::optional<LinkPending> lp = p->flow.accept()) { p->storeLink(*lp); }
        p->publishLink();
    });
    g.on_link_no([p] { p->flow.dismiss(); p->publishLink(); });
    g.on_fault_reenable([p](slint::SharedString id) {
        const std::string name(id);
        p->reenabled = name;
        if (p->env.reenable) { p->env.reenable(name); }   // the engine forgets the strike's hold, rescans and builds the plugin afresh; the card is put away with it
        p->seenFaults.push_back(name);
        p->publishFaults();
    });
    g.on_fault_seen([p](slint::SharedString id) { p->seenFaults.push_back(std::string(id)); p->publishFaults(); });
}

void Publisher::offerDiagnostics(bool due) {
    p_->diagOpen = due;
    p_->env.ui->global<Gaps1>().set_diag_open(due);
}

void Publisher::linkRequested(const std::string& appToken) { p_->flow.arrive(appToken); }

void Publisher::tick(long tickNo) {
    Impl& p = *p_;
    // the plugin fault cards: the engine's fact, read about once a second
    if (p.env.fact && tickNo - p.lastFaultTick >= 30) {
        p.lastFaultTick = tickNo;
        std::string text;
        (void)p.env.fact("fact.plugins.fault.notices", text);
        std::vector<FaultNotice> now = parseFaultNotices(text);
        if (!(now == p.allFaults)) { p.allFaults = std::move(now); }
        p.publishFaults();
    }
    // THE ONE-SHOT LINK-REQUEST FILE, about once a second, on a worker (a hung disk must not stop a frame)
    std::string token;
    if (p.poll.poll(token) && !token.empty()) { p.flow.arrive(token); }
    if (!p.env.configDir.empty() && tickNo - p.lastPollTick >= 30) {
        p.lastPollTick = tickNo;
        p.poll.request(p.env.configDir);
    }
    // the site's answer, then a lookup for the token waiting
    ResolveOutcome outcome = ResolveOutcome::NetworkError;
    std::string name;
    if (p.env.net && p.env.net->take(outcome, name)) {
        const cascade::core::AppConfig cfg = p.env.iface ? p.env.iface() : cascade::core::AppConfig{};
        p.flow.resolved(outcome, name, !cfg.testerAppToken.empty(), !cfg.testerToken.empty());
    }
    std::string wanted;
    if (p.env.net && p.flow.takeLookup(wanted)) { p.env.net->resolve(wanted); }
    p.publishLink();
    p.publishDiagStatus();
}

std::string Publisher::readState(const std::string& what, std::string& out) const {
    const Impl& p = *p_;
    if (what == "diag") { out = p.diagOpen ? "1" : "0"; return std::string(); }
    if (what == "link") { out = p.flow.hasPending() ? "pending" : (!p.flow.error().empty() ? "error" : (p.flow.lookingUp() ? "looking" : "none")); return std::string(); }
    if (what == "linkname") { out = p.flow.hasPending() ? p.flow.pending().name : std::string(); return std::string(); }
    if (what == "linkerr") { out = p.flow.error(); return std::string(); }
    if (what == "faults") { out = std::to_string(unseenFaults(p.allFaults, p.seenFaults).size()); return std::string(); }
    if (what == "held") { out = p.env.iface ? p.env.iface().testerAppTokenName : std::string(); return std::string(); }   // the NAME of the link the settings hold (never the token)
    if (what == "reenabled") { out = p.reenabled; return std::string(); }
    if (what == "stored") { out = std::to_string(p.stored); return std::string(); }
    if (what == "storedname") { out = p.storedName; return std::string(); }
    return "diag|link|linkname|linkerr|faults|held|reenabled|stored|storedname, not " + what;
}

}  // namespace foxsdr::slintgaps1
