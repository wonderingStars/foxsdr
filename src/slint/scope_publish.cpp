// scope_publish.cpp - the scope modes' publisher (see scope_publish.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "scope_publish.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

#include "core/config.hpp"
#include "scope_compositor.hpp"

namespace foxsdr::slintscope {

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

// the table's read_scope as the seam's reader
struct LiveTaps : ScopeTapReader {
    const FoxEngineApi* api = nullptr;
    FoxSession* session = nullptr;
    std::int32_t readScope(std::uint32_t tap, FoxScopeInfo* info, float* out, std::uint32_t capFrames) override {
        if (api == nullptr || session == nullptr || api->read_scope == nullptr) { return static_cast<std::int32_t>(FOXAPI_DETACHED); }
        return api->read_scope(session, tap, info, out, capFrames);
    }
};

constexpr int kDefaultGlassW = 736, kDefaultGlassH = 376;
constexpr int kMaxGlass = 2400;   // a glass wider than this paints at this and is scaled up (image-fit fill)

}  // namespace

struct ScopePublisher::Impl {
    MainWindow& ui;
    LiveTaps taps;
    foxsdr::slinthost::HostDuties& duties;
    bool bounded;
    DemodScope demod;
    int glassW = kDefaultGlassW, glassH = kDefaultGlassH;
    std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> bufs;
    int which = 0;
    PaintScratch scratch;
    bool shownSet = false, wasOpen = false, lastLive = false;
    std::size_t segments = 0, painted = 0, saves = 0;
    double lastPaintS = -1.0;
    double lastPaintMs = 0.0;       // how long the last paint took: a heavy one (a vector figure with PERSIST) paints every other tick, never more than the window can afford
    int skipTicks = 0;
    cascade::gui::DemodScopeState savedState;
    bool savedOpen = false;

    Impl(MainWindow& u, foxsdr::slinthost::HostDuties& d, bool b) : ui(u), duties(d), bounded(b) {}

    void load() {
        const cascade::core::AppConfig& half = duties.interfaceHalf();
        demod.state.signal = static_cast<int>(cascade::gui::scopeSignalFromIndex(half.demodScopeSignal));
        demod.state.timebase = cascade::gui::clampScopeTimebase(half.demodScopeTimebase);
        demod.state.gainIndex = cascade::gui::clampScopeGain(half.demodScopeGain);
        demod.state.autoGain = half.demodScopeAutoGain;
        demod.state.display = cascade::gui::clampScopeDisplay(half.demodScopeDisplay);
        // demodScopeOpen is saved and NOT restored (startupState clears it): the window starts closed
        savedState = demod.state;
    }

    // THE SAVE: on a key of the instrument and at the close, never from a bounded run
    void save() {
        if (bounded) { return; }
        if (demod.state == savedState && demod.open == savedOpen) { return; }
        cascade::core::AppConfig half = duties.interfaceHalf();
        half.demodScopeOpen = demod.open;
        half.demodScopeSignal = demod.state.signal;
        half.demodScopeTimebase = demod.state.timebase;
        half.demodScopeGain = demod.state.gainIndex;
        half.demodScopeAutoGain = demod.state.autoGain;
        half.demodScopeDisplay = demod.state.display;
        if (duties.saveInterfaceHalf(half)) { ++saves; }
        savedState = demod.state;
        savedOpen = demod.open;
    }

    ::DemodModel model(const slint::Image& picture) {
        const DemodReadouts r = demod.readouts();
        ::DemodModel m;
        m.shown = demod.open;
        m.live = demod.live();
        m.signal = static_cast<int>(demod.signalNow());
        m.mpx_available = demod.mpxAvailable();
        m.spectral = r.spectral;
        m.baseband = cascade::gui::scopeSignalIsBaseband(demod.signalNow());
        m.timebase_text = ss(r.timebaseKey);
        m.gain_text = ss(r.gainKey);
        m.auto_gain = demod.state.autoGain;
        m.display = demod.state.display;
        m.caption = ss(r.caption);
        m.display_word = ss(r.display);
        m.centre = ss(r.centre);
        m.gain_read = ss(r.gain);
        m.audio_from = ss(r.audioFrom);
        m.dead = ss(r.dead);
        m.picture = picture;
        std::vector<::MpxMarkItem> marks;
        for (const MpxMark& k : demod.mpxMarks()) {
            ::MpxMarkItem it;
            it.x0 = static_cast<float>(k.x0);
            it.x1 = static_cast<float>(k.x1);
            it.room = static_cast<float>(k.room);
            it.label = ss(k.label);
            it.tone = k.tone;
            it.optional = k.optional;
            marks.push_back(std::move(it));
        }
        m.marks = std::make_shared<slint::VectorModel<::MpxMarkItem>>(std::move(marks));
        return m;
    }
};

ScopePublisher::ScopePublisher(MainWindow& ui, const FoxEngineApi* api, FoxSession* session, foxsdr::slinthost::HostDuties& duties, bool bounded)
    : impl_(std::make_unique<Impl>(ui, duties, bounded)), ui_(ui) {
    impl_->taps.api = api;
    impl_->taps.session = session;
    impl_->load();
}

ScopePublisher::~ScopePublisher() = default;

bool ScopePublisher::demodOpen() const { return impl_->demod.open; }
int ScopePublisher::demodSignal() const { return static_cast<int>(impl_->demod.signalNow()); }
bool ScopePublisher::demodLive() const { return impl_->demod.live(); }

void ScopePublisher::setDemodOpen(bool open) {
    if (impl_->demod.open == open) { return; }
    impl_->demod.open = open;
    impl_->save();
}

void ScopePublisher::openDemodForCapture(int signal) {
    if (signal >= 0) { impl_->demod.state.signal = signal; }
    impl_->demod.open = true;
}

void ScopePublisher::toggleDemod() { setDemodOpen(!impl_->demod.open); }

void ScopePublisher::wire() {
    auto&& ev = ui_.global<::ScopeEvents>();
    Impl* d = impl_.get();
    ev.on_demod_signal([d](int i) { if (d->demod.setSignal(i)) { d->save(); } });
    ev.on_demod_timebase([d](int step) { if (d->demod.stepTimebase(step)) { d->save(); } });
    ev.on_demod_gain([d](int step) { if (d->demod.stepGain(step)) { d->save(); } });
    ev.on_demod_auto([d](bool on) { if (d->demod.setAutoGain(on)) { d->save(); } });
    ev.on_demod_display([d](int i) { if (d->demod.setDisplay(i)) { d->save(); } });
    ev.on_demod_resized([d](int w, int h) {
        d->glassW = std::clamp(w, 1, kMaxGlass);
        d->glassH = std::clamp(h, 1, kMaxGlass);
        d->demod.markPainted();
        d->lastPaintS = -1.0;   // repaint at the new size on the next tick
    });
    ev.on_demod_closed([d] { d->demod.open = false; d->save(); });
}

void ScopePublisher::publish(const FoxReceiverState& state, double nowS) {
    Impl& d = *impl_;
    if (!d.demod.open) {
        if (d.shownSet || d.wasOpen) {
            ::DemodModel closed;
            closed.shown = false;
            ui_.set_demod_model(closed);
            d.shownSet = false;
            d.wasOpen = false;
        }
        return;
    }
    d.wasOpen = true;
    DemodInputs in;
    in.wfm = state.demodMode == FOXAPI_DEMOD_WFM;
    in.audioRateHz = state.audioRateHz;
    in.channelRateHz = state.channelRateHz;
    in.audioFrom = state.audioPlayingPlugin;
    in.nowS = nowS;
    d.demod.tick(d.taps, in);

    // the glass is repainted when a tap moved or a setting changed, and while a memory display is fading; a dead tube is painted once
    if (d.skipTicks > 0) { --d.skipTicks; return; }
    const bool fading = d.demod.live() && d.demod.state.display == 2;
    const bool liveChanged = d.demod.live() != d.lastLive;
    d.lastLive = d.demod.live();
    if (!(d.demod.dirty() || fading || liveChanged || d.lastPaintS < 0.0 || !d.shownSet)) { return; }
    const int w = d.glassW, h = d.glassH;
    auto& buf = d.bufs[static_cast<std::size_t>(d.which)];
    if (buf.width() != static_cast<std::uint32_t>(w) || buf.height() != static_cast<std::uint32_t>(h)) { buf = slint::SharedPixelBuffer<slint::Rgba8Pixel>(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)); }
    const auto t0 = std::chrono::steady_clock::now();
    d.segments = paintDemodFace(reinterpret_cast<foxsdr::slinthost::Rgba*>(buf.begin()), w, h, d.demod, d.scratch);
    d.lastPaintMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    d.skipTicks = d.lastPaintMs > 12.0 ? (d.lastPaintMs > 24.0 ? 2 : 1) : 0;
    ++d.painted;
    d.demod.markPainted();
    d.lastPaintS = nowS;
    ui_.set_demod_model(d.model(slint::Image(buf)));
    d.which ^= 1;
    d.shownSet = true;
}

std::string ScopePublisher::readState(const std::string& what, std::string& out) const {
    const Impl& d = *impl_;
    const DemodFeed& f = d.demod.feed();
    const DemodReadouts r = d.demod.readouts();
    const auto num = [&out](std::size_t v) { out = std::to_string(v); return std::string(); };
    if (what == "demodOpen") { return num(d.demod.open ? 1 : 0); }
    if (what == "signal") { return num(static_cast<std::size_t>(d.demod.signalNow())); }
    if (what == "live") { return num(d.demod.live() ? 1 : 0); }
    if (what == "timebase") { return num(static_cast<std::size_t>(d.demod.state.timebase)); }
    if (what == "gainIndex") { return num(static_cast<std::size_t>(d.demod.state.gainIndex)); }
    if (what == "auto") { return num(d.demod.state.autoGain ? 1 : 0); }
    if (what == "display") { return num(static_cast<std::size_t>(d.demod.state.display)); }
    if (what == "audioCount") { return num(f.audioCount); }
    if (what == "iqCount") { return num(f.iqCount); }
    if (what == "bins") { return num(f.spectrumBins); }
    if (what == "segments") { return num(d.segments); }
    if (what == "painted") { return num(d.painted); }
    if (what == "glassW") { return num(static_cast<std::size_t>(d.glassW)); }
    if (what == "glassH") { return num(static_cast<std::size_t>(d.glassH)); }
    if (what == "centre") { out = r.centre; return std::string(); }
    if (what == "gain") { out = r.gain; return std::string(); }
    if (what == "dead") { out = r.dead; return std::string(); }
    if (what == "saves") { return num(d.saves); }
    return "no scope state named " + what;
}

std::string ScopePublisher::summaryLine() const {
    const Impl& d = *impl_;
    const DemodReadouts r = d.demod.readouts();
    std::string s = "scope: demod ";
    s += d.demod.open ? "open" : "closed";
    s += " signal=" + std::to_string(static_cast<int>(d.demod.signalNow())) + " live=" + (d.demod.live() ? "yes" : "no");
    const DemodFeed& f = d.demod.feed();
    s += " audio=" + std::to_string(f.audioCount) + " iq=" + std::to_string(f.iqCount) + " bins=" + std::to_string(f.spectrumBins);
    s += " glass=" + std::to_string(d.glassW) + "x" + std::to_string(d.glassH) + " painted=" + std::to_string(d.painted) + " segments=" + std::to_string(d.segments);
    s += " paint=" + std::to_string(d.lastPaintMs).substr(0, 5) + "ms gain=" + std::to_string(d.demod.state.gainIndex) + " centre=\"" + r.centre + "\" dead=\"" + r.dead + "\" saves=" + std::to_string(d.saves);
    return s;
}

}  // namespace foxsdr::slintscope
