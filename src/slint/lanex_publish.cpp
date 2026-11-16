// lanex_publish.cpp - see lanex_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanex_publish.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "core/dsp_benchmark.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "lanek_logic.hpp"
#include "lanex_help.hpp"

namespace foxsdr::slintlanex {

namespace {
namespace slk = foxsdr::slintlanek;
using slk::Row;

std::string tr(const char* s) { return cascade::i18n::tr(s); }
slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

KRow toK(const Row& r) {
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

std::shared_ptr<slint::VectorModel<KRow>> modelOf(const std::vector<Row>& rows) {
    std::vector<KRow> ks;
    ks.reserve(rows.size());
    for (const Row& r : rows) { ks.push_back(toK(r)); }
    return std::make_shared<slint::VectorModel<KRow>>(std::move(ks));
}

// The program's own speed test, one item at a time (core/dsp_benchmark.hpp).
BenchItem realItem(std::size_t i) {
    cascade::core::bench::BenchOptions o;
    const cascade::core::bench::BenchResult r = cascade::core::bench::runBenchItem(i, o);
    BenchItem it;
    it.group = r.group;
    it.name = r.name;
    it.ok = r.ok;
    it.error = r.error;
    it.samplesPerSec = r.samplesPerSec;
    it.referenceRateHz = r.referenceRateHz;
    it.realTimeMultiple = r.realTimeMultiple;
    return it;
}
}  // namespace

struct LaneXPublisher::Impl {
    Env env;
    // help
    bool helpOpen = false;
    std::string query;
    std::string topic;          // the id of the open topic, "" = the list
    int ctxBank = -1, ctxRow = -1;
    int shown = 0;              // how many topics the list or the results hold
    // speed test
    std::unique_ptr<BenchJob> job;
    bool ranOnce = false;
    BenchState lastState = BenchState::Idle;
    std::size_t lastDone = 0;
    long lastTick = -100;
    std::string benchSig;
    bool benchDirty = true;
    NumberWriter num;

    explicit Impl(Env e) : env(std::move(e)) {
        num = env.numbers ? env.numbers : plainNumbers();
        if (env.benchRun) {
            job = std::make_unique<BenchJob>(env.benchRun, env.benchCount);
        } else {
            job = std::make_unique<BenchJob>(realItem, cascade::core::bench::benchItemCount());
        }
    }

    // ---------------------------------------------------------------- help
    std::vector<Row> helpRows() {
        std::vector<Row> rows;
        const auto& all = helpTopics();
        const int open = topic.empty() ? -1 : topicIndex(topic);
        if (open >= 0) {
            const HelpTopic& t = all[static_cast<std::size_t>(open)];
            rows.push_back(slk::rHeading(titleOf(t)));
            for (const char* b : t.body) { rows.push_back(slk::rNote(sentence(b))); }
            rows.push_back(slk::rKey("help-back", tr("All topics")));   // not-translated: the host's action id
            shown = 1;
            return rows;
        }
        rows.push_back(slk::rField("help-search", tr("Search help"), query));
        std::vector<int> found;
        if (!query.empty()) {
            found = searchTopics(query);
            rows.push_back(slk::rHeading(tr("Results")));
            if (found.empty()) { rows.push_back(slk::rNote(tr("No topic matches what you typed."))); }
        } else {
            const int here = topicForSection(ctxBank, ctxRow);
            if (here >= 0) {
                rows.push_back(slk::rHeading(tr("Where you are")));
                rows.push_back(slk::rKey("help-topic:" + std::string(all[static_cast<std::size_t>(here)].id), titleOf(all[static_cast<std::size_t>(here)])));
            }
            rows.push_back(slk::rHeading(tr("The topics")));
            found = searchTopics("");
        }
        for (const int i : found) {
            const HelpTopic& t = all[static_cast<std::size_t>(i)];
            rows.push_back(slk::rKey("help-topic:" + std::string(t.id), titleOf(t)));
        }
        shown = static_cast<int>(found.size());
        return rows;
    }

    void publishHelp() {
        if (env.ui == nullptr) { return; }
        auto& l = env.ui->global<LaneX>();
        l.set_help_title(ss(tr("Help")));
        l.set_help_rows(modelOf(helpRows()));
    }

    void open(int bank, int row) {
        ctxBank = bank;
        ctxRow = row;
        query.clear();
        const int here = topicForSection(bank, row);
        topic = here >= 0 ? helpTopics()[static_cast<std::size_t>(here)].id : "";
        helpOpen = true;
        publishHelp();
        if (env.ui != nullptr) { env.ui->global<LaneX>().set_help_open(true); }
    }

    void close() {
        helpOpen = false;
        if (env.ui != nullptr) { env.ui->global<LaneX>().set_help_open(false); }
    }

    // ---------------------------------------------------------------- the speed test
    std::vector<Row> benchRows() {
        std::vector<Row> rows;
        rows.push_back(slk::rHeading(tr("Speed test")));
        rows.push_back(slk::rNote(tr("Times the signal processing on this computer, one block after another: the spectrum transform, the channel filter, each demodulator and the resamplers. It uses one processor core for about ten seconds and changes no setting.")));
        const bool running = job->running();
        rows.push_back(slk::rKey("bench-run", tr("Run the speed test"), !running));
        if (running) {
            rows.push_back(slk::rBar(tr("Progress"), job->total() ? static_cast<float>(job->done()) / static_cast<float>(job->total()) : 0.0f, progressText(job->done(), job->total())));
            rows.push_back(slk::rKey("bench-cancel", tr("Cancel the speed test")));
        }
        const std::vector<BenchItem> items = job->results();
        for (const BenchItem& it : items) { rows.push_back(slk::rReadout(it.group + " " + it.name, itemText(it, num))); }
        if (!running && ranOnce) {
            if (job->state() == BenchState::Cancelled) {
                rows.push_back(slk::rNote(cascade::core::formatText(tr("Cancelled after %d of %d items. The results so far are shown."), static_cast<int>(job->done()), static_cast<int>(job->total())), slk::kWarn));
            }
            const std::string s = summaryText(items, num);
            if (!s.empty()) { rows.push_back(slk::rNote(s, slk::kGood)); }
        }
        return rows;
    }

    void publishBench(bool force) {
        if (env.ui == nullptr) { return; }
        std::vector<Row> rows = benchRows();
        const std::string sig = slk::signatureOf(rows);
        if (!force && sig == benchSig) { return; }
        benchSig = sig;
        env.ui->global<LaneX>().set_rows_bench(modelOf(rows));
    }

    void act(const std::string& id, const std::string& text) {
        if (id == "help-close") { close(); return; }
        if (id == "help-search") { query = text; topic.clear(); publishHelp(); return; }
        if (id == "help-back") { topic.clear(); publishHelp(); return; }
        if (id.rfind("help-topic:", 0) == 0) {
            const std::string t = id.substr(11);
            if (topicIndex(t) >= 0) { topic = t; publishHelp(); }
            return;
        }
        if (id == "bench-run") { if (job->start()) { ranOnce = true; } publishBench(true); return; }
        if (id == "bench-cancel") { job->cancel(); publishBench(true); return; }
    }
};

LaneXPublisher::LaneXPublisher(Env env) : p_(std::make_unique<Impl>(std::move(env))) {}
LaneXPublisher::~LaneXPublisher() = default;

void LaneXPublisher::wire() {
    if (p_->env.ui == nullptr) { return; }
    p_->env.ui->global<LaneX>().on_act([this](slint::SharedString id, slint::SharedString text) { p_->act(std::string(id.data(), id.size()), std::string(text.data(), text.size())); });
    p_->publishBench(true);
}

void LaneXPublisher::tick(long tickNo) {
    // the speed test's rows follow its progress about four times a second while it runs, and once more when it ends
    const BenchState st = p_->job->state();
    const std::size_t done = p_->job->done();
    if (st != p_->lastState || done != p_->lastDone || (st == BenchState::Running && tickNo - p_->lastTick >= 8)) {
        p_->lastState = st;
        p_->lastDone = done;
        p_->lastTick = tickNo;
        p_->publishBench(false);
    }
}

void LaneXPublisher::openHelp() {
    const std::pair<int, int> w = p_->env.where ? p_->env.where() : std::make_pair(-1, -1);
    p_->open(w.first, w.second);
}
void LaneXPublisher::openHelpAt(int bank, int row) { p_->open(bank, row); }
void LaneXPublisher::closeHelp() { p_->close(); }
bool LaneXPublisher::helpOpen() const { return p_->helpOpen; }

std::string LaneXPublisher::readState(const std::string& what, std::string& out) const {
    Impl& p = *p_;
    if (what == "help.open") { out = p.helpOpen ? "1" : "0"; return std::string(); }
    if (what == "help.topic") { out = p.topic; return std::string(); }
    if (what == "help.query") { out = p.query; return std::string(); }
    if (what == "help.shown") { out = std::to_string(p.shown); return std::string(); }
    if (what == "help.topics") { out = std::to_string(helpTopics().size()); return std::string(); }
    if (what == "bench.state") {
        switch (p.job->state()) {
            case BenchState::Idle: out = "idle"; break;
            case BenchState::Running: out = "running"; break;
            case BenchState::Done: out = "done"; break;
            case BenchState::Cancelled: out = "cancelled"; break;
        }
        return std::string();
    }
    if (what == "bench.done") { out = std::to_string(p.job->done()); return std::string(); }
    if (what == "bench.total") { out = std::to_string(p.job->total()); return std::string(); }
    if (what == "bench.failed") { int n = 0; for (const BenchItem& it : p.job->results()) { if (!it.ok) { ++n; } } out = std::to_string(n); return std::string(); }
    return "lx.<help.open|help.topic|help.query|help.shown|help.topics|bench.state|bench.done|bench.total|bench.failed>, not " + what;
}

}  // namespace foxsdr::slintlanex
