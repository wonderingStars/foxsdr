// transmit_publish.cpp - see transmit_publish.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "transmit_publish.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "core/i18n.hpp"
#include "host_duties.hpp"
#include "signatures.hpp"
#include "system_logic.hpp"
#include "transmit_logic.hpp"

namespace foxsdr::slinttransmit {

using foxsdr::slintsignal::Tone;
namespace cc = cascade::core;

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

template <class T> std::shared_ptr<slint::Model<T>> model(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }

::ChipTone tone(Tone t) {
    switch (t) {
        case Tone::Good: return ::ChipTone::Good;
        case Tone::Warn: return ::ChipTone::Warn;
        case Tone::Err: return ::ChipTone::Err;
        default: return ::ChipTone::Neutral;
    }
}

double nowSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

constexpr std::size_t kAnswerLog = 12;

}  // namespace

struct TransmitPublisher::Impl {
    TransmitPublisher& self;
    foxsdr::slinthost::HostDuties& duties;
    foxsdr::slintkit::LiveTable table;
    TransmitPage page;
    foxsdr::slinthost::Signatures sigs;
    bool openSent = false;                    // the engine believes the page open (true only once the session TOOK the command)
    std::map<std::uint32_t, int> sentByOp, withheldByOp;
    std::vector<std::string> answers;
    std::int64_t tickNo = 0;
    bool live = false;
    bool haveRadio = false;

    Impl(TransmitPublisher& p, foxsdr::slinthost::HostDuties& d) : self(p), duties(d), table(p.api_, p.session_) {}

    void say(const std::string& line) const { std::fprintf(stderr, "foxsdr-slint: transmit: %s\n", line.c_str()); }
    static std::string hex(std::uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof(b), "%04x", static_cast<unsigned>(v));
        return b;
    }

    // Whether a command may go (a bounded run's filter); counted and said once by its op when it may not.
    bool may(const FoxCommand& c) {
        if (!self.bounded_ || boundedMaySend(c, haveRadio)) { return true; }
        ++self.withheld_;
        if (withheldByOp[c.op]++ == 0) { say("bounded run - op 0x" + hex(c.op) + " not sent"); }
        return false;
    }

    // ONE command, as the window's uiOffer sends it: taken = the session accepted it (not BUSY); the ticket 0 for a command it merged or did not take.
    bool offer(const cc::cmd::QueuedCommand& q, std::uint64_t* ticket = nullptr) {
        if (ticket != nullptr) { *ticket = 0; }
        if (self.api_ == nullptr || self.session_ == nullptr) { return false; }
        FoxCommand c = q.c;
        c.structSize = sizeof(c);
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const std::int32_t n = (q.longText.empty() || !FOXAPI_HAS(self.api_, submit_text))
                                   ? self.api_->submit(self.session_, &c, 1, &sr)
                                   : self.api_->submit_text(self.session_, &c, q.longText.data(), static_cast<std::uint32_t>(q.longText.size()), &sr);
        if (n < 0 || sr.status == FOXAPI_BUSY) { return false; }
        if (sr.status != FOXAPI_OK && sr.status != FOXAPI_NO_CHANGE) { return false; }
        if (ticket != nullptr) { *ticket = sr.ticket; }
        ++self.sent_;
        if (self.bounded_ && sentByOp[c.op]++ == 0) { say("op 0x" + hex(c.op) + " sent"); }
        return true;
    }

    void send(const Command& c) {
        if (!c.valid || !may(c.q.c)) { return; }
        (void)offer(c.q);
    }

    // THE KEY'S BATCH, ONE SUBMIT, in order (a LATCH press is "release, then press").
    void sendBatch(const std::vector<FoxCommand>& cmds) {
        if (cmds.empty() || self.api_ == nullptr || self.session_ == nullptr) { return; }
        FoxCommand batch[16];
        std::uint32_t n = 0;
        for (const FoxCommand& f : cmds) {
            if (n >= 16 || !may(f)) { continue; }
            batch[n] = f;
            batch[n].structSize = sizeof(FoxCommand);
            ++n;
        }
        if (n == 0) { return; }
        (void)self.api_->submit(self.session_, batch, n, nullptr);
        self.sent_ += n;
        for (std::uint32_t i = 0; i < n; ++i) {
            if (self.bounded_ && sentByOp[batch[i].op]++ == 0) { say("op 0x" + hex(batch[i].op) + " sent"); }
        }
    }

    std::string pttChord() const {
        const auto t = foxsdr::slintsystem::keyTableFromConfig(duties.interfaceHalf().keyBindings);
        const int i = foxsdr::slintsystem::keyActionIndexOf("transmitPtt");
        if (i < 0) { return std::string(); }
        return foxsdr::slintsystem::formatChord(t[static_cast<std::size_t>(i)]);
    }

    // The model, mapped 1:1.
    ::TransmitPageModel map(const Model& m) const {
        ::TransmitPageModel o{};
        o.address = ss(m.address);
        o.address_enabled = m.address_enabled;
        o.have = m.have;
        o.open_enabled = m.open_enabled;
        o.close_enabled = m.close_enabled;
        o.open_tip = ss(m.open_tooltip);
        o.radio_line = ss(m.radio_line);
        o.radio_err = m.radio_tone == Tone::Err;
        o.frequency = ss(m.frequency);
        o.frequency_enabled = m.frequency_enabled;
        o.split = m.split;
        o.split_enabled = m.split_enabled;
        o.following_note = ss(m.following_note);
        o.mode = m.mode;
        o.modes_enabled = m.modes_enabled;
        std::vector<::TxChoice> modes;
        for (int i = 0; i < kModeCount; ++i) { modes.push_back(::TxChoice{ss(modeName(i)), ss(modeCaption(i))}); }
        o.modes = model(std::move(modes));
        o.power = m.power;
        o.power_text = ss(m.power_text);
        o.power_enabled = m.power_enabled;
        o.power_note = ss(m.power_note);
        o.input = m.input;
        o.inputs_enabled = m.inputs_enabled;
        std::vector<::TxChoice> inputs;
        for (int i = 0; i < kInputCount; ++i) { inputs.push_back(::TxChoice{ss(inputLabel(i)), ss("")}); }
        o.inputs = model(std::move(inputs));
        o.tone_shown = m.tone_shown;
        o.tone_hz = ss(m.tone_hz);
        o.tone_enabled = m.tone_enabled;
        o.meter = m.meter;
        o.lamp_word = ss(m.lamp_word);
        o.lamp_tone = tone(m.lamp_tone);
        o.on_air = m.on_air;
        o.latched = m.latched;
        o.key_enabled = m.key_enabled;
        o.remote_armed = m.remote_armed;
        o.monitor = m.monitor;
        o.key_note = ss(m.key_note);
        o.unkey_reason = ss(m.unkey_reason);
        o.error_line = ss(m.error_line);
        o.armed = m.armed;
        o.armed_minutes = m.armed_minutes;
        o.armed_line = ss(m.armed_line);
        o.on_air_line = ss(m.on_air_line);
        o.plugin_on_air = m.plugin_on_air;
        o.not_armed_line = ss(m.not_armed_line);
        o.refusal_line = ss(m.refusal_line);
        o.reason_line = ss(m.reason_line);
        o.data_flat = m.data_flat;
        std::vector<slint::SharedString> arm;
        for (int minutes : {5, 15, 60}) { arm.push_back(ss(armLabel(minutes))); }
        o.arm_labels = model(std::move(arm));
        o.licence = ss(m.licence);
        return o;
    }

    static std::string sig(const Model& m) {
        std::string s;
        auto add = [&s](const std::string& v) { s += v; s += '\x1f'; };
        auto num = [&add](double v) { char b[32]; std::snprintf(b, sizeof(b), "%.6g", v); add(b); };
        add(m.address); num(m.address_enabled); num(m.have); num(m.open_enabled); num(m.close_enabled); add(m.open_tooltip); add(m.radio_line); num(static_cast<int>(m.radio_tone));
        add(m.frequency); num(m.frequency_enabled); num(m.split); num(m.split_enabled); add(m.following_note); num(m.mode); num(m.modes_enabled);
        num(m.power); add(m.power_text); num(m.power_enabled); add(m.power_note); num(m.input); num(m.inputs_enabled); num(m.tone_shown); add(m.tone_hz); num(m.tone_enabled); num(m.meter);
        add(m.lamp_word); num(static_cast<int>(m.lamp_tone)); num(m.on_air); num(m.latched); num(m.key_enabled); num(m.remote_armed); num(m.monitor); add(m.key_note); add(m.unkey_reason); add(m.error_line);
        num(m.armed); num(m.armed_minutes); add(m.armed_line); add(m.on_air_line); num(m.plugin_on_air); add(m.not_armed_line); add(m.refusal_line); add(m.reason_line); num(m.data_flat); add(m.licence);
        return s;
    }
};

TransmitPublisher::TransmitPublisher(MainWindow& ui, foxsdr::slintkit::KitPublisher& pages, foxsdr::slinthost::HostDuties& duties, const FoxEngineApi* api, FoxSession* session,
                                     bool bounded)
    : ui_(ui), pages_(pages), api_(api), session_(session), bounded_(bounded) {
    impl_ = std::make_unique<Impl>(*this, duties);
}

TransmitPublisher::~TransmitPublisher() = default;

bool TransmitPublisher::pageOpen() const { return pages_.transmitView() != foxsdr::slintkit::KitPublisher::TransmitView::Closed; }

void TransmitPublisher::togglePage() {
    if (pageOpen()) { pages_.closeTransmitPage(); } else { pages_.openTransmitPage(); }
}

void TransmitPublisher::onAnswer(const FoxCommandResult& r) {
    Impl& m = *impl_;
    m.page.onAnswer(r);
    const bool ours = r.op == FOXAPI_OP_TX_LATCH || r.op == FOXAPI_OP_TX_REMOTE_ARM || r.op == FOXAPP_OP_TX_PLUGIN_ARM || r.op == FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN ||
                      r.op == FOXAPI_OP_TX_PTT;
    if (!ours) { return; }
    // The lease's beat (-1) answers every tick: not logged (it would be the whole log).
    ++answered_;
    char line[200];
    std::snprintf(line, sizeof(line), "op 0x%04x ticket %llu status %d applied [%g, %g]%s%s", r.op, static_cast<unsigned long long>(r.ticket), r.status, r.applied[0], r.applied[1],
                  r.message[0] != '\0' ? " - " : "", r.message);
    if (r.op == FOXAPP_OP_TX_PLUGIN_ARM && (r.status == FOXAPI_OK || r.status == FOXAPI_NO_CHANGE)) { return; }
    m.answers.push_back(line);
    if (m.answers.size() > kAnswerLog) { m.answers.erase(m.answers.begin()); }
    if (bounded_) { m.say(std::string("answer: ") + line); }
}

void TransmitPublisher::publish(const FoxReceiverState& state) {
    Impl& m = *impl_;
    ++m.tickNo;
    using View = foxsdr::slintkit::KitPublisher::TransmitView;
    const View view = pages_.transmitView();
    const bool open = view != View::Closed;
    m.live = pages_.transmitLive();
    m.page.read(m.table, state);
    m.haveRadio = m.page.reading().have;
    // THE ENGINE IS TOLD WHETHER THE PAGE IS OPEN, and the window believes it has told it only when the session TOOK the command (the classic's syncTransmitPageOpen).
    if (open != m.openSent) {
        cc::cmd::QueuedCommand q;
        q.c = cc::cmd::makeInt(FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN, open ? 1 : 0);
        if (m.offer(q)) { m.openSent = open; }
    }
    // THE KEY, once a tick, whether or not the page is open: the first tick after it went away is the one that lets go.
    m.sendBatch(m.page.tick(m.live, nowSeconds()));
    if (open) {
        const Model mm = m.page.model();
        if (m.sigs.changed("page", Impl::sig(mm))) {
            pages_.setTransmitModel(m.map(mm), mm.lamp_word, tone(mm.lamp_tone));
        }
    }
}

std::string TransmitPublisher::readState(const std::string& name, std::string& out) const {
    const Impl& m = *impl_;
    using View = foxsdr::slintkit::KitPublisher::TransmitView;
    const std::string key = name.rfind("transmit.", 0) == 0 ? name.substr(9) : name;
    const auto flag = [&out](bool v) { out = v ? "1" : "0"; return std::string(); };
    if (key == "page") {
        const View v = pages_.transmitView();
        out = v == View::Closed ? "closed" : v == View::Docked ? "docked" : "shown";
        return std::string();
    }
    if (key == "live") { return flag(m.live); }
    if (key == "ptt") { return flag(m.page.pttHeld()); }
    if (key == "consent") { return flag(m.page.consentHeld()); }
    if (key == "latchPending") { return flag(m.page.latchPending()); }
    if (key == "sent") { out = std::to_string(sent_); return std::string(); }
    if (key == "withheld") { out = std::to_string(withheld_); return std::string(); }
    if (key == "answers") { out = std::to_string(answered_); return std::string(); }
    if (key == "refusal") { out = std::to_string(m.page.reading().plugin.refusal); return std::string(); }
    if (key == "keyNote") { out = m.page.keyNote(); return std::string(); }
    const auto count = [&](const std::map<std::uint32_t, int>& by, const std::string& hexOp) {
        const std::uint32_t op = static_cast<std::uint32_t>(std::strtoul(hexOp.c_str(), nullptr, 16));
        const auto it = by.find(op);
        out = std::to_string(it == by.end() ? 0 : it->second);
        return std::string();
    };
    if (key.rfind("sent.", 0) == 0) { return count(m.sentByOp, key.substr(5)); }
    if (key.rfind("withheld.", 0) == 0) { return count(m.withheldByOp, key.substr(9)); }
    return "transmit.page|live|ptt|consent|latchPending|sent|withheld|answers|refusal|keyNote|sent.<op>|withheld.<op>, not " + name;
}

std::string TransmitPublisher::summaryLine() const {
    const Impl& m = *impl_;
    using View = foxsdr::slintkit::KitPublisher::TransmitView;
    const View v = pages_.transmitView();
    const Reading& r = m.page.reading();
    std::string s = std::string("transmit: page=") + (v == View::Closed ? "closed" : v == View::Docked ? "docked" : "shown") + " live=" + (m.live ? "1" : "0") + " radio=" +
                    (r.have ? "open" : "none") + " keyed=" + (r.on_air ? "1" : "0") + " ptt=" + (m.page.pttHeld() ? "1" : "0") + " consent=" + (m.page.consentHeld() ? "1" : "0") +
                    " armed=" + (r.plugin.armed ? "1" : "0") + " refusal=" + std::to_string(r.plugin.refusal) + " key note=\"" + m.page.keyNote() + "\"";
    s += "; commands sent=" + std::to_string(sent_) + " withheld=" + std::to_string(withheld_) + " answers=" + std::to_string(answered_);
    if (!m.answers.empty()) {
        s += " [";
        for (std::size_t i = 0; i < m.answers.size(); ++i) { s += (i ? "; " : "") + m.answers[i]; }
        s += "]";
    }
    return s;
}

void TransmitPublisher::wire() {
    Impl* m = impl_.get();
    auto&& ev = ui_.global<::TransmitEvents>();
    TransmitPage& page = m->page;
    ev.on_address_edited([m](slint::SharedString t) { m->send(m->page.addressEdited(std::string(t))); });
    ev.on_address_focus([&page](bool f) { page.addressFocus(f); });
    ev.on_open_clicked([m] { m->send(m->page.open()); });
    ev.on_close_clicked([m] { m->send(m->page.close()); });
    ev.on_frequency_committed([m](slint::SharedString t) { m->send(m->page.frequencyCommitted(std::string(t))); });
    ev.on_frequency_stepped([m](int d) { m->send(m->page.frequencyStepped(d)); });
    ev.on_split_clicked([m] { m->send(m->page.splitPressed()); });
    ev.on_mode_clicked([m](int i) { m->send(m->page.modeSelected(i)); });
    ev.on_power_changed([m](float v) { m->send(m->page.powerChanged(v)); });
    ev.on_input_clicked([m](int i) { m->send(m->page.inputSelected(i)); });
    ev.on_tone_committed([m](slint::SharedString t) { m->send(m->page.toneCommitted(std::string(t))); });
    // THE KEY: levels and presses, turned into the engine's hold by the tick. The pointer is the PTT key's TouchArea; the chord is compared with the bound one here.
    ev.on_ptt_pointer([&page](bool down) { page.pttPointer(down); });
    ev.on_ptt_chord([m](slint::SharedString text, bool ctrl, bool shift, bool alt, bool down) {
        if (!down) {
            // Any key coming up (or the focus leaving, which sends an empty text) lets the chord go: a release the key did not name still ends the hold.
            m->page.pttChord(false);
            return;
        }
        m->page.pttChord(chordMatches(m->pttChord(), std::string(text), ctrl, shift, alt));
    });
    ev.on_latch_clicked([&page] { page.latchPressed(); });
    ev.on_remote_toggled([&page](bool on) { page.remoteArmToggled(on); });
    ev.on_monitor_toggled([m](bool on) { m->send(m->page.monitorToggled(on)); });
    ev.on_arm_clicked([&page](int minutes) { page.pluginArmPressed(minutes); });
    ev.on_data_flat_toggled([m](bool on) { m->send(m->page.dataFlatToggled(on)); });
}

}  // namespace foxsdr::slinttransmit
