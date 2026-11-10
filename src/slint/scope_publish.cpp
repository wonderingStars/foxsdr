// scope_publish.cpp - the scope modes' publisher (see scope_publish.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "scope_publish.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <vector>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/i18n.hpp"
#include "radar_compositor.hpp"
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
    foxsdr::slintkit::PluginKit& kit;
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

    // ---- the radar scope ----
    RadarScope radar;
    RadarPaintScratch rscratch;
    struct KitInfo : RadarInfoSource {
        foxsdr::slintkit::PluginKit& kit;
        explicit KitInfo(foxsdr::slintkit::PluginKit& k) : kit(k) {}
        RadarInfo lookup(const cascade::core::HostTrack& h) override {
            RadarInfo r;
            r.active = kit.trackInfo().active();
            if (!r.active) { return r; }
            const cascade::gui::TrackInfoCache::Info* i = kit.trackInfo().get(h.t.id, h.t.kind);
            if (i == nullptr) { r.pending = true; return r; }
            if (i->known) {
                r.registration = i->registration;
                r.typeCode = i->typeCode;
                r.typeName = i->typeName;
                r.operatorName = i->operatorName;
            }
            return r;
        }
    } info;
    std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> ground;
    int groundWhich = 0;
    slint::Image fan;
    int tubeW = 0, tubeH = 0;
    int lastView = 0, prevView = 0;
    bool radarWasOn = false, groundDirty = true;
    double lastGroundS = -1.0, lastGroundMs = 0.0;
    int groundSkip = 0;
    std::size_t groundPaints = 0, groundSegments = 0, panelNotes = 0;
    RadarInputs lastIn;
    int savedRange = 200;

    Impl(MainWindow& u, foxsdr::slintkit::PluginKit& k, foxsdr::slinthost::HostDuties& d, bool b) : ui(u), kit(k), duties(d), bounded(b), info(k) {}

    void load() {
        const cascade::core::AppConfig& half = duties.interfaceHalf();
        demod.state.signal = static_cast<int>(cascade::gui::scopeSignalFromIndex(half.demodScopeSignal));
        demod.state.timebase = cascade::gui::clampScopeTimebase(half.demodScopeTimebase);
        demod.state.gainIndex = cascade::gui::clampScopeGain(half.demodScopeGain);
        demod.state.autoGain = half.demodScopeAutoGain;
        demod.state.display = cascade::gui::clampScopeDisplay(half.demodScopeDisplay);
        // demodScopeOpen is saved and NOT restored (startupState clears it): the window starts closed
        radar.setRangeNm(half.scopeRangeNm);
        savedRange = radar.rangeNm();
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

// ---------------------------------------------------------------------------------------------------------------------------------
// the radar scope's tick and model

namespace {

constexpr int kSweepFanSize = 256;

slint::Color colorOf(std::uint32_t rgba) {
    // radar_logic.hpp: 0xRRGGBBAA
    return slint::Color::from_argb_uint8(static_cast<std::uint8_t>(rgba & 0xffu), static_cast<std::uint8_t>((rgba >> 24) & 0xffu), static_cast<std::uint8_t>((rgba >> 16) & 0xffu),
                                         static_cast<std::uint8_t>((rgba >> 8) & 0xffu));
}

}  // namespace

foxsdr::slintview3d::Radar3dFeed ScopePublisher::radar3dFeed() const {
    using namespace foxsdr::slintview3d;
    const Impl& d = *impl_;
    Radar3dFeed feed;
    const RadarFrame& f = d.radar.frame();
    feed.shown = ui_.get_view_index() == 2 && f.hasRx;
    if (!feed.shown) { return feed; }
    feed.rangeNm = static_cast<double>(f.rangeNm);
    feed.trailMode = d.radar.optTrail();
    feed.plottedText = f.plottedText;
    feed.rangeCorner = f.rangeCornerText;
    feed.modeText = f.modeText;
    feed.rxText = f.rxText;
    feed.emptyText = f.plotted == 0 ? f.homeEmptyTitle : std::string();
    std::map<std::string, const cascade::core::HostTrack*> byId;
    for (const cascade::core::HostTrack& h : d.kit.tracks()) { byId[h.t.id] = &h; }
    const std::map<std::string, std::vector<RadarScope::TrailPoint>>& raw = d.radar.rawTrails();
    const double rxLat = d.lastIn.rxLatDeg, rxLon = d.lastIn.rxLonDeg;
    const auto relative = [&](double lat, double lon, double& e, double& n) {
        const cascade::gui::ScopePolar p = cascade::gui::scopeRelative(rxLat, rxLon, lat, lon);
        polarToEastNorth(p.rangeNm, p.bearingDeg, e, n);
    };
    for (const RadarTarget& t : f.targets) {
        const auto it = byId.find(t.id);
        if (it == byId.end()) { continue; }
        const cascade::core::HostTrack& h = *it->second;
        Radar3dAircraft a;
        a.id = t.id;
        a.label = t.label;
        a.selected = t.selected;
        a.emergency = t.emergency;
        relative(h.t.latDeg, h.t.lonDeg, a.eastNm, a.northNm);
        a.altFt = std::isfinite(h.t.altM) ? h.t.altM * 3.28084 : std::numeric_limits<double>::quiet_NaN();
        a.courseDeg = std::isfinite(h.t.courseDeg) ? h.t.courseDeg : std::numeric_limits<double>::quiet_NaN();
        const auto tr = raw.find(t.id);
        if (tr != raw.end()) {
            a.trail.reserve(tr->second.size());
            for (const RadarScope::TrailPoint& p : tr->second) {
                Radar3dTrailPoint q;
                relative(p.lat, p.lon, q.eastNm, q.northNm);
                q.altFt = std::isfinite(p.altM) ? p.altM * 3.28084 : std::numeric_limits<double>::quiet_NaN();
                a.trail.push_back(q);
            }
        }
        feed.aircraft.push_back(std::move(a));
    }
    return feed;
}

void ScopePublisher::radarPublish(const FoxReceiverState& state, double nowS) {
    Impl& d = *impl_;
    const bool on = ui_.get_view_index() == 2;
    if (on && d.lastView != 2) {
        d.prevView = d.lastView;
        d.groundDirty = true;
        // THE USAGE REPORT'S PANEL NAME, as the classic notes it while the scope shows (aw.cpp uiNotePanel("scope"))
        (void)d.kit.submit(cascade::core::cmd::makeText(FOXAPP_OP_NOTE_PANEL, "scope"));
        ++d.panelNotes;
    }
    d.lastView = ui_.get_view_index();
    d.kit.open().radarScope = on;   // AUTO track sources wake while the scope shows (FOXAPP_PLUGIN_USE_RADAR_SCOPE)
    if (!on) {
        if (d.radarWasOn) {
            ::RadarScopeModel off;
            off.shown = false;
            ui_.set_radar_scope_model(off);
            d.radarWasOn = false;
        }
        return;
    }
    d.radarWasOn = true;
    RadarInputs in;
    in.nowS = nowS;
    in.hasRx = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
    in.rxLatDeg = state.rxLatDeg;
    in.rxLonDeg = state.rxLonDeg;
    in.running = (state.flags & FOXAPI_RX_RUNNING) != 0u;
    in.signalDb = state.signalDb;
    in.deviceOpen = (state.flags & FOXAPI_RX_DEVICE_OPEN) != 0u;
    in.deviceAgc = (state.flags & FOXAPI_RX_DEVICE_AGC) != 0u;
    if (d.taps.api != nullptr && d.taps.session != nullptr) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (d.taps.api->read_list(d.taps.session, FOXAPI_LIST_GAINS, 0, &it) > 0) {
            in.haveGain = true;
            in.gainStage = it.name;
            in.gainUnit = it.detail;
            in.gainLoDb = static_cast<float>(it.value[0]);
            in.gainHiDb = static_cast<float>(it.value[1]);
            in.gainDb = static_cast<float>(it.value[3]);
        }
    }
    const cascade::core::AppConfig& half = d.duties.interfaceHalf();
    in.aircraftIconPx = cascade::gui::clampAircraftIconPx(half.aircraftIconPx);
    in.trailWidthPx = half.mapTrailWidthPx;
    d.lastIn = in;
    d.radar.update(d.kit.tracks(), in, &d.info);
    if (d.radar.rangeNm() != d.savedRange && !d.bounded) {   // the scale is the interface half's scopeRangeNm
        cascade::core::AppConfig h = d.duties.interfaceHalf();
        h.scopeRangeNm = d.radar.rangeNm();
        if (d.duties.saveInterfaceHalf(h)) { ++d.saves; }
    }
    d.savedRange = d.radar.rangeNm();
    const RadarFrame& f = d.radar.frame();

    // THE GROUND: repainted when the picture changed and not oftener than 20 Hz, a heavy paint every other tick
    const bool sizeOk = d.tubeW >= 40 && d.tubeH >= 40;
    if (d.groundSkip > 0) { --d.groundSkip; }
    const bool due = d.groundDirty || d.radar.dirty();
    if (sizeOk && due && d.groundSkip == 0 && (nowS - d.lastGroundS >= 0.05 || d.groundDirty)) {
        auto& buf = d.ground[static_cast<std::size_t>(d.groundWhich)];
        if (buf.width() != static_cast<std::uint32_t>(d.tubeW) || buf.height() != static_cast<std::uint32_t>(d.tubeH)) {
            buf = slint::SharedPixelBuffer<slint::Rgba8Pixel>(static_cast<std::uint32_t>(d.tubeW), static_cast<std::uint32_t>(d.tubeH));
        }
        const auto t0 = std::chrono::steady_clock::now();
        d.groundSegments = paintRadarGround(reinterpret_cast<foxsdr::slinthost::Rgba*>(buf.begin()), d.tubeW, d.tubeH, d.radar, d.rscratch);
        d.lastGroundMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        d.groundSkip = d.lastGroundMs > 12.0 ? (d.lastGroundMs > 24.0 ? 2 : 1) : 0;
        ++d.groundPaints;
        d.lastGroundS = nowS;
        d.groundDirty = false;
        d.radar.markPainted();
        d.groundWhich ^= 1;
    }
    if (d.fan.size().width == 0) {
        slint::SharedPixelBuffer<slint::Rgba8Pixel> fb(kSweepFanSize, kSweepFanSize);
        paintSweepFan(reinterpret_cast<foxsdr::slinthost::Rgba*>(fb.begin()), kSweepFanSize);
        d.fan = slint::Image(fb);
    }

    ::RadarScopeModel m;
    m.shown = true;
    m.has_rx = f.hasRx;
    m.warning = ss(cascade::i18n::tr("The scope needs the receiver's position."));
    m.intro = ss(cascade::i18n::tr("Everything on a radar scope is a range and a bearing measured from the antenna, so there is nothing to draw until this receiver knows where it is. "
                                   "It is the same position the map uses, and setting it here sets it there. Either key below puts the receiver at a place the application already knows; "
                                   "the scope draws the moment one is pressed, and the exact figure can be corrected afterwards."));
    m.range_text = ss(f.rangeText);
    m.can_down = f.canStepDown;
    m.can_up = f.canStepUp;
    m.plotted_text = ss(f.plottedText);
    m.mode_text = ss(f.modeText);
    m.rx_text = ss(f.rxText);
    m.range_corner = ss(f.rangeCornerText);
    m.ground = slint::Image(d.ground[static_cast<std::size_t>(d.groundWhich ^ 1)]);
    m.fan = d.fan;
    m.sweep_deg = radarSweepDegrees(nowS);
    m.sweep_on = f.sweep;
    m.tube_w = f.tubeW;
    m.tube_h = f.tubeH;
    m.centre_x = f.centreX;
    m.centre_y = f.centreY;
    m.radius = f.radiusPx;
    m.panned = f.panned;
    m.rx_x = f.rxMarkX;
    m.rx_y = f.rxMarkY;
    std::vector<::RadarTargetItem> ts;
    for (const RadarTarget& t : f.targets) {
        ::RadarTargetItem it;
        it.id = ss(t.id);
        it.x = t.x;
        it.y = t.y;
        it.heading = t.headingDeg;
        it.color = colorOf(t.rgba);
        it.mark_r = t.markR;
        it.label = ss(t.label);
        it.emergency = t.emergency;
        it.selected = t.selected;
        it.alt_known = t.altKnown;
        ts.push_back(std::move(it));
    }
    m.targets = std::make_shared<slint::VectorModel<::RadarTargetItem>>(std::move(ts));
    const auto labels = [](const std::vector<RadarLabel>& v) {
        std::vector<::RadarLabelItem> out;
        for (const RadarLabel& l : v) { out.push_back(::RadarLabelItem{l.x, l.y, ss(l.text), l.strong}); }
        return std::make_shared<slint::VectorModel<::RadarLabelItem>>(std::move(out));
    };
    m.ring_labels = labels(f.ringLabels);
    m.bearing_labels = labels(f.bearingLabels);
    m.show_bearing_digits = f.showThreeDigitBearings;
    m.sig_lit = f.sigLit;
    m.sig_text = ss(f.sigText);
    m.alt_lit = f.altLit;
    m.alt_text = ss(f.altText);
    m.drum_caption = ss(f.drumCaption);
    m.drum_range = ss(f.drumRange);
    m.drum_tracks = ss(f.drumTracks);
    m.knob_dead = f.knobDead;
    m.knob_frac = f.knobFrac;
    m.knob_lit = f.knobLit;
    m.knob_ticks = std::make_shared<slint::VectorModel<int>>(std::vector<int>(std::begin(f.knobTicks), std::end(f.knobTicks)));
    m.knob_readout = ss(f.knobReadout);
    m.knob_note = ss(f.knobNote);
    m.power_on = f.powerOn;
    m.power_text = ss(f.powerText);
    m.lcd_shown = f.lcdShown;
    m.lcd_screen = f.lcdScreen;
    m.home_count = ss(f.homeCount);
    m.home_sub = ss(f.homeSub);
    std::vector<::RadarContactItem> cs;
    for (const RadarContact& c : f.contacts) { cs.push_back(::RadarContactItem{ss(c.id), ss(c.name), ss(c.alt), ss(c.detail), colorOf(c.rgba), c.alert}); }
    m.contacts = std::make_shared<slint::VectorModel<::RadarContactItem>>(std::move(cs));
    m.home_empty_title = ss(f.homeEmptyTitle);
    m.home_empty_body = ss(f.homeEmptyBody);
    std::vector<::RadarLineItem> ls;
    for (const RadarLine& l : f.flight) { ls.push_back(::RadarLineItem{ss(l.label), ss(l.value), l.dim, l.alert}); }
    m.flight = std::make_shared<slint::VectorModel<::RadarLineItem>>(std::move(ls));
    m.flight_title = ss(f.flightTitle);
    m.flight_body = ss(f.flightBody);
    m.flight_foot = ss(f.flightFoot);
    std::vector<::RadarOptionItem> os;
    for (const RadarOption& o : f.options) { os.push_back(::RadarOptionItem{ss(o.name), ss(o.value)}); }
    m.options = std::make_shared<slint::VectorModel<::RadarOptionItem>>(std::move(os));
    m.sys_range = ss(f.sysRange);
    m.sys_antenna = ss(f.sysAntenna);
    m.sys_help = ss(f.sysHelp);
    ui_.set_radar_scope_model(m);
}

void ScopePublisher::toggleRadar() {
    if (ui_.get_view_index() == 2) { leaveRadar(); } else { ui_.set_view_index(2); }
}

void ScopePublisher::leaveRadar() {
    if (ui_.get_view_index() != 2) { return; }
    ui_.set_view_index(impl_->prevView == 2 ? 0 : impl_->prevView);
}

bool ScopePublisher::radarOn() const { return ui_.get_view_index() == 2; }

ScopePublisher::ScopePublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, const FoxEngineApi* api, FoxSession* session, foxsdr::slinthost::HostDuties& duties, bool bounded)
    : impl_(std::make_unique<Impl>(ui, kit, duties, bounded)), ui_(ui) {
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
    // ---- the radar scope ----
    ev.on_radar_exit([this] { leaveRadar(); });
    ev.on_radar_range([d](int dir) { d->radar.stepRange(dir); });
    ev.on_radar_wheel([d](int dir) { d->radar.wheel(dir); });
    ev.on_radar_drag([d](float dx, float dy) { d->radar.dragBy(dx, dy); });
    ev.on_radar_released([d](float x, float y, bool dragged) { d->radar.releasePointer(x, y, dragged); });
    ev.on_radar_double_clicked([d] { d->radar.doubleClick(); });
    ev.on_radar_resized([d](int w, int h) {
        d->tubeW = std::clamp(w, 0, kMaxGlass);
        d->tubeH = std::clamp(h, 0, kMaxGlass);
        d->radar.setTubeSize(static_cast<float>(d->tubeW), static_cast<float>(d->tubeH));
        d->groundDirty = true;
    });
    ev.on_radar_lcd_tab([d](int screen) { d->radar.setLcdScreen(screen); });
    ev.on_radar_contact([d](slint::SharedString id) { d->radar.selectContact(std::string(id)); });
    ev.on_radar_option([d](int index) { d->radar.cycleOption(index); });
    ev.on_radar_knob([d](int steps) {
        // THE KNOB'S DETENT: the dB the classic sends with FOXAPP_OP_SET_GAIN_NO_READBACK (stage in the text, dB in num[0]); NaN = dead, nothing to send
        const double db = d->radar.knobDetent(steps, d->lastIn);
        if (std::isfinite(db) && !d->lastIn.gainStage.empty()) { (void)d->kit.submit(cascade::core::cmd::makeText(FOXAPP_OP_SET_GAIN_NO_READBACK, d->lastIn.gainStage, 0, 0, db)); }
    });
    ev.on_radar_power([d] {
        FoxCommand c = cascade::core::cmd::make(FOXAPI_OP_RUN);
        c.ival[0] = d->lastIn.running ? 0 : 1;
        (void)d->kit.submit(c);
    });
}

void ScopePublisher::publish(const FoxReceiverState& state, double nowS) {
    Impl& d = *impl_;
    radarPublish(state, nowS);
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
    // ---- the radar scope ----
    const RadarFrame& rf = d.radar.frame();
    if (what == "radarOn") { return num(ui_.get_view_index() == 2 ? 1 : 0); }
    if (what == "hasRx") { return num(rf.hasRx ? 1 : 0); }
    if (what == "targets") { return num(rf.targets.size()); }
    if (what == "plotted") { return num(static_cast<std::size_t>(rf.plotted)); }
    if (what == "range") { return num(static_cast<std::size_t>(d.radar.rangeNm())); }
    if (what == "panned") { return num(d.radar.panned() ? 1 : 0); }
    if (what == "selected") { out = d.radar.selectedId(); return std::string(); }
    if (what == "lcd") { return num(static_cast<std::size_t>(d.radar.lcdScreen())); }
    if (what == "phosphor") { return num(d.radar.optPhosphor() ? 1 : 0); }
    if (what == "sweep") { return num(d.radar.optSweep() ? 1 : 0); }
    if (what == "trail") { return num(static_cast<std::size_t>(d.radar.optTrail())); }
    if (what == "filter") { return num(static_cast<std::size_t>(d.radar.optFilter())); }
    if (what == "tubeW") { return num(static_cast<std::size_t>(d.tubeW)); }
    if (what == "groundPaints") { return num(d.groundPaints); }
    if (what == "groundSegments") { return num(d.groundSegments); }
    if (what == "knobDead") { return num(rf.knobDead ? 1 : 0); }
    if (what == "contacts") { return num(rf.contacts.size()); }
    if (what == "drumTracks") { out = rf.drumTracks; return std::string(); }
    if (what == "drumRange") { out = rf.drumRange; return std::string(); }
    if (what == "powerText") { out = rf.powerText; return std::string(); }
    if (what == "notes") { return num(d.panelNotes); }
    if (what == "flight") {
        out.clear();
        for (const RadarLine& l : rf.flight) { out += l.label + "=" + l.value + ";"; }
        return std::string();
    }
    if (what == "trailPoints") { return num(d.radar.trailPointCount()); }
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
