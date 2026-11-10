// globe_publish.cpp - see globe_publish.hpp. SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "globe_publish.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/i18n.hpp"
#include "core/plugin_ui_data.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slintkit {

using namespace foxsdr::globe;

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }
template <class T> std::shared_ptr<slint::Model<T>> model(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }
std::shared_ptr<slint::Model<slint::SharedString>> strings(const std::vector<std::string>& v) {
    std::vector<slint::SharedString> out;
    for (const auto& s : v) { out.push_back(ss(s)); }
    return model(std::move(out));
}
std::string T(const char* english) { return std::string(cascade::i18n::tr(english)); }
template <class... A> std::string F(const char* english, A... a) {
    std::string out;
    cascade::core::formatUtf8(out, cascade::i18n::tr(english), a...);
    return out;
}
slint::Color colour(std::uint32_t argb) { return slint::Color::from_argb_encoded(argb); }
slint::Color labelColour(std::uint32_t rgba) { return slint::Color::from_argb_encoded(((rgba & 0xFFu) << 24) | ((rgba >> 8) & 0x00FFFFFFu)); }   // 0xRRGGBBAA -> ARGB
std::int64_t unixMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::int64_t steadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string cell(const char* c) { return std::string(c, ::strnlen(c, CASCADE_PANEL_CELL_CHARS)); }
std::string num(double v, int decimals) { char b[48]; std::snprintf(b, sizeof(b), "%.*f", decimals, v); return b; }
bool iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) { if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i]))) { return false; } }
    return true;
}

// the controls
::GlobeControl segs(int id, const std::string& label, const std::vector<std::string>& options, int value, const std::string& tip = std::string()) {
    ::GlobeControl c{};
    c.id = id; c.kind = 0; c.label = ss(label); c.options = strings(options); c.value = value; c.enabled = true; c.tooltip = ss(tip);
    return c;
}
::GlobeControl checkbox(int id, const std::string& label, bool on, const std::string& tip = std::string()) {
    ::GlobeControl c{};
    c.id = id; c.kind = 1; c.label = ss(label); c.options = strings({}); c.value = on ? 1 : 0; c.enabled = true; c.tooltip = ss(tip);
    return c;
}
::GlobeControl slider(int id, const std::string& label, int value1000, const std::string& text, const std::string& tip = std::string()) {
    ::GlobeControl c{};
    c.id = id; c.kind = 2; c.label = ss(label); c.options = strings({}); c.value = value1000; c.value_text = ss(text); c.enabled = true; c.tooltip = ss(tip);
    return c;
}
::GlobeControl keyControl(int id, const std::string& label, const std::string& tip = std::string()) {
    ::GlobeControl c{};
    c.id = id; c.kind = 3; c.label = ss(label); c.options = strings({}); c.enabled = true; c.tooltip = ss(tip);
    return c;
}
::GlobeControl readout(const std::string& label, const std::string& text) {
    ::GlobeControl c{};
    c.id = 0; c.kind = 4; c.label = ss(label); c.options = strings({}); c.value_text = ss(text); c.enabled = true;
    return c;
}
::GlobeChipModel chip(const std::string& text, std::uint32_t argb, bool on = false, bool pressable = false) {
    ::GlobeChipModel c{};
    c.text = ss(text); c.color = colour(argb); c.on = on; c.pressable = pressable;
    return c;
}

// control ids, per view
constexpr int kBand = 1, kGreyline = 2;                // propagation
constexpr int kFt8Band = 1, kFt8Window = 2;            // FT8
constexpr int kGoesSat = 1;                            // GOES
constexpr int kScale = 1, kMoon = 2, kDate = 3, kToday = 4;   // solar system
constexpr int kFootprint = 1, kTracks = 2;             // satellites

const int kPropBands[] = {0, 6, 7, 8, 9, 10};          // ALL, 20 m, 17 m, 15 m, 12 m, 10 m: the bands the NCDXF beacons are on
const int kFt8Bands[] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};   // ALL and the FT8 bands (160 m .. 2 m, not 60 m)
const int kWindows[] = {15, 60, 1440};

std::string bandName(int id) { const Band* b = bandById(id); return b != nullptr ? b->name : std::string(); }

}  // namespace

GlobePublisher::GlobePublisher(PluginKit& kit) : kit_(kit) {}
GlobePublisher::~GlobePublisher() = default;

void GlobePublisher::setViewPrefsHooks(std::function<std::string()> load, std::function<void(const std::string&)> save) {
    loadPrefs_ = std::move(load);
    savePrefs_ = std::move(save);
}

void GlobePublisher::changed(Surface& s) {
    s.touched = true;
    prefsDirty_ = true;
    prefsChangedAtMs_ = steadyMs();
}

void GlobePublisher::flushPrefs() {
    if (!prefsDirty_ || !savePrefs_ || !loadPrefs_ || steadyMs() - prefsChangedAtMs_ < 400) { return; }
    prefsDirty_ = false;
    slintview3d::ViewPrefs prefs = slintview3d::ViewPrefs::parse(loadPrefs_());   // the other views' entries are kept as they are
    for (const auto& kv : surfaces_) {
        if (kv.second->touched && kv.second->kind != ViewKind::None) { prefs.set(prefsName(kv.second->kind), toPrefs(kv.second->view, kv.second->kind)); }
    }
    const std::string text = prefs.serialize();
    if (text == lastSavedPrefs_) { return; }
    lastSavedPrefs_ = text;
    savePrefs_(text);
}

bool GlobePublisher::offers(const std::string& plugin, int pageKind) const {
    switch (viewKindFor(plugin)) {
        case ViewKind::Propagation:
        case ViewKind::Goes:
        case ViewKind::SolarSystem: return pageKind == 2;
        case ViewKind::Ft8: return pageKind == 3;
        case ViewKind::Satellites: return pageKind == 5;
        default: return false;
    }
}

GlobePublisher::Surface& GlobePublisher::surfaceFor(const std::string& plugin) {
    auto it = surfaces_.find(plugin);
    if (it == surfaces_.end()) {
        it = surfaces_.emplace(plugin, std::make_unique<Surface>()).first;
        Surface& s = *it->second;
        s.kind = viewKindFor(plugin);
        s.view.camera = defaultCamera(s.kind);
        s.initial = s.view.camera;
        if (loadPrefs_) {
            // the framework's one setting: this view's entry, or its defaults when it has none (a view that is not listed is off)
            const slintview3d::ViewPrefs prefs = slintview3d::ViewPrefs::parse(loadPrefs_());
            fromPrefs(s.view, s.kind, prefs.get(prefsName(s.kind), prefsDefaults(s.kind)));
            s.touched = false;
        }
        s.shown = s.view.camera;
    }
    return *it->second;
}

const GlobePublisher::Surface* GlobePublisher::findSurface(const std::string& plugin) const {
    auto it = surfaces_.find(plugin);
    return it == surfaces_.end() ? nullptr : it->second.get();
}

bool GlobePublisher::globeOn(const std::string& plugin) const {
    const Surface* s = findSurface(plugin);
    return s != nullptr && s->view.globeOn;
}

const LandMask& GlobePublisher::land() {
    if (!land_) { land_ = std::make_unique<LandMask>(1440, 720); }
    return *land_;
}

std::vector<PanelRowText> GlobePublisher::panelRows(const std::string& plugin) const {
    std::vector<PanelRowText> out;
    for (const cascade::core::HostPanel& p : kit_.panels()) {
        if (p.plugin != plugin) { continue; }
        const std::size_t cols = std::min<std::size_t>(p.headings.size(), CASCADE_PANEL_MAX_COLUMNS);
        for (const CascadePanelRow& r : p.rows) {
            PanelRowText t;
            t.kind = static_cast<int>(r.kind);
            for (std::size_t i = 0; i < std::max<std::size_t>(cols, 1u); ++i) { t.cells.push_back(cell(r.cells[i])); }
            out.push_back(std::move(t));
        }
        break;
    }
    return out;
}

// ---- once per publish ----

void GlobePublisher::tick(const FoxReceiverState& state) {
    flushPrefs();
    if (kit_.decoderLines().empty()) { return; }
    const std::int64_t now = unixMs();
    // the receiver the stations are measured from: the plugin's own locator, else the host's position
    LatLon rx;
    bool rxKnown = false;
    {
        const std::vector<PanelRowText> rows = panelRows("FT8");
        for (const PanelRowText& r : rows) {
            if (r.kind == 0 && r.cells.size() >= 2 && r.cells[0] == "Locator") {
                const std::string loc = locatorIn(r.cells[1]);
                if (!loc.empty()) { rxKnown = locatorToLatLon(loc, rx); }
            }
        }
        if (!rxKnown && (state.flags & FOXAPI_RX_POSITION_SET) != 0u) { rx = LatLon{state.rxLatDeg, state.rxLonDeg}; rxKnown = true; }
    }
    for (const DecoderLine& l : kit_.decoderLines()) {
        if (l.id <= ft8LastId_) { continue; }
        ft8LastId_ = l.id;
        if (l.plugin != "FT8") { continue; }
        Ft8Decode d;
        if (!parseFt8Line(l.text, d)) { continue; }
        ft8_.addLine(l.text, ft8LineTimeMs(d, now), state.tunedHz / 1e6, rxKnown ? rx : LatLon{});
    }
}

// ---- the picture ----

void GlobePublisher::finish(Surface& s, const GlobeDrawList& list, const std::string& key, ::GlobeModel& m, std::string& sig) {
    const int w = std::max(64, s.w > 0 ? s.w : 640), h = std::max(64, s.h > 0 ? s.h : 480);
    const std::string full = key + "|" + std::to_string(w) + "x" + std::to_string(h);
    if (!s.composed || s.composeKey != full) {
        std::vector<scene3d::Label3D> labels;
        scene3d::Rgba* px = s.surface.begin(w, h);
        if (compositor_.paint(list, px, s.surface.width(), s.surface.height(), labels)) {
            s.image = s.surface.image();
            s.surface.flip();
            s.labels = std::move(labels);
            s.composed = true;
            s.composeKey = full;
        } else {
            s.composed = false;
        }
    }
    s.shown = list.camera;
    if (s.composed) { m.picture = s.image; m.have = true; }
    std::vector<::Scene3DLabel> labels;
    for (const scene3d::Label3D& l : s.labels) {
        ::Scene3DLabel lm{};
        lm.x = l.x; lm.y = l.y; lm.text = ss(l.text); lm.color = labelColour(l.rgba); lm.align = l.align; lm.kind = l.kind;
        labels.push_back(lm);
        sig += l.text + std::to_string(static_cast<int>(l.x * 1000)) + "," + std::to_string(static_cast<int>(l.y * 1000)) + ";";
    }
    m.labels = model(std::move(labels));
    sig += "G" + s.composeKey + (s.composed ? "c" : "n");
    if (!list.emptyNote.empty()) { m.empty_note = ss(T(list.emptyNote.c_str())); }
}

// ---- the model of a page ----

void GlobePublisher::fill(const std::string& plugin, int pageKind, const FoxReceiverState& state, ::GlobeModel& m, std::string& sig) {
    m.labels = model(std::vector<::Scene3DLabel>{});
    m.controls = model(std::vector<::GlobeControl>{});
    m.readouts = strings({});
    m.chips = model(std::vector<::GlobeChipModel>{});
    if (!offers(plugin, pageKind)) { return; }
    Surface& s = surfaceFor(plugin);
    m.offered = true;
    m.active = s.view.globeOn;
    m.kind = static_cast<int>(s.kind);
    m.flat_label = ss(T(flatViewLabel(s.kind)));
    m.globe_label = ss(T(globeViewLabel(s.kind)));
    m.view_caption = ss(T("VIEW"));
    m.reset_label = ss(T("RESET VIEW"));
    m.hint = ss(T("Drag to rotate - wheel to zoom"));
    sig += std::string("V") + (s.view.globeOn ? "1" : "0") + std::to_string(static_cast<int>(s.kind));
    if (!s.view.globeOn) { return; }
    switch (s.kind) {
        case ViewKind::Propagation: fillPropagation(s, state, m, sig); break;
        case ViewKind::Ft8: fillFt8(s, state, m, sig); break;
        case ViewKind::Goes: fillGoes(s, state, m, sig); break;
        case ViewKind::SolarSystem: fillSolar(s, state, m, sig); break;
        case ViewKind::Satellites: fillSatellites(s, plugin, state, m, sig); break;
        default: break;
    }
    // the camera, in the well's terms (ui/globe_views.slint): the host's numbers, pushed again when cam-seq moves
    const bool orbits = s.kind == ViewKind::SolarSystem;
    m.cam_az = static_cast<float>(s.shown.centreLonDeg);
    m.cam_el = static_cast<float>(s.shown.centreLatDeg);
    m.cam_zoom = static_cast<float>(s.shown.zoom);
    m.cam_seq = s.camSeq;
    m.default_az = static_cast<float>(s.initial.centreLonDeg);
    m.default_el = static_cast<float>(s.initial.centreLatDeg);
    m.default_zoom = static_cast<float>(s.initial.zoom);
    m.min_el = orbits ? 5.0f : -85.0f;
    m.max_el = 85.0f;
    m.min_zoom = static_cast<float>(kZoomMin);
    m.max_zoom = static_cast<float>(kZoomMax);
    // a globe turns with the pointer: the sphere is about 0.42 of the shorter side in radius at zoom 1 (globe_compositor.hpp)
    const double well = static_cast<double>(std::min(s.w > 0 ? s.w : 480, s.h > 0 ? s.h : 480));
    m.deg_per_pixel = orbits ? 0.45f : static_cast<float>((180.0 / kPi) / (0.42 * well * std::max(0.1, s.shown.zoom)));
    sig += "C" + std::to_string(s.camSeq) + "," + num(m.deg_per_pixel, 3);
    // the whole column's words ride the signature: a changed control, readout or chip sets the page again
    std::string cs;
    for (int i = 0; i < static_cast<int>(m.controls->row_count()); ++i) {
        const auto c = m.controls->row_data(static_cast<std::size_t>(i));
        if (c) { cs += std::string(c->label) + "/" + std::to_string(c->value) + "/" + std::string(c->value_text) + "/" + (c->enabled ? "e" : "d") + ";"; }
    }
    for (int i = 0; i < static_cast<int>(m.readouts->row_count()); ++i) { const auto r = m.readouts->row_data(static_cast<std::size_t>(i)); if (r) { cs += std::string(*r) + ";"; } }
    for (int i = 0; i < static_cast<int>(m.chips->row_count()); ++i) { const auto c = m.chips->row_data(static_cast<std::size_t>(i)); if (c) { cs += std::string(c->text) + (c->on ? "1" : "0") + ";"; } }
    sig += cs + std::string(m.empty_note);
    s.note = cs;
}

// ---- 1. propagation ----

void GlobePublisher::fillPropagation(Surface& s, const FoxReceiverState& st, ::GlobeModel& m, std::string& sig) {
    const bool rxKnown = (st.flags & FOXAPI_RX_POSITION_SET) != 0u;
    const LatLon rx{st.rxLatDeg, st.rxLonDeg};
    const PropagationFacts facts = readPropagationPanel(panelRows("Propagation Tools"));
    const int bandSel = std::clamp(s.view.bandId, 0, static_cast<int>(sizeof(kPropBands) / sizeof(kPropBands[0])) - 1);
    const PropagationScene scene = buildPropagationScene(facts, rxKnown, rx, kPropBands[bandSel]);
    const std::int64_t now = unixMs();
    const double jd = julianDay(now / 1000);
    const SunPosition sun = sunPosition(jd);
    const std::uint64_t bake = earthBakeKey(&sun, s.view.greyline, 1024, 512);
    if (s.earth.empty() || s.earth.key != bake) { bakeEarth(s.earth, land(), &sun, s.view.greyline); }
    s.initial = initialCamera(ViewKind::Propagation, scene.homeKnown, scene.home);
    GlobeDrawList list = drawPropagation(scene, s.view, s.earth);
    if (scene.homeKnown && scene.paths.empty()) { list.emptyNote = "The plugin's panel shows no target locator and no beacon on air in this band yet: no path to draw."; }
    std::string key = "P" + std::to_string(bake) + "b" + std::to_string(bandSel) + "g" + (s.view.greyline ? "1" : "0");
    for (const GlobePath& p : scene.paths) { key += p.label + std::to_string(p.hops); }
    key += scene.homeKnown ? latLonText(scene.home) : "-";
    key += num(list.camera.centreLonDeg, 2) + "," + num(list.camera.centreLatDeg, 2) + "," + num(list.camera.zoom, 2);
    finish(s, list, key, m, sig);

    std::vector<::GlobeControl> c;
    std::vector<std::string> bandOpts{T("ALL")};
    for (std::size_t i = 1; i < sizeof(kPropBands) / sizeof(kPropBands[0]); ++i) { bandOpts.push_back(bandName(kPropBands[i])); }
    c.push_back(segs(kBand, T("BAND"), bandOpts, bandSel, T("Shows the beacons on air on this band only")));
    c.push_back(checkbox(kGreyline, T("GREYLINE"), s.view.greyline, T("Marks the band of twilight along the day / night edge, where HF often travels furthest")));
    c.push_back(readout(T("HOME POSITION"), scene.homeKnown ? F("%s (%s)", latLonText(scene.home).c_str(), T(scene.homeSource.c_str()).c_str()) : T("not set")));
    c.push_back(readout(T("SUN OVER"), latLonText(LatLon{sun.subLatDeg, sun.subLonDeg})));
    m.controls = model(std::move(c));
    std::vector<::GlobeChipModel> chips;
    for (const GlobePath& p : scene.paths) { chips.push_back(chip(pathChipText(p), p.argb)); }
    // the legend: the colour is the number of hops, counted at 4 000 km a hop, so every one is "about"
    chips.push_back(chip(T("about 1 hop"), hopColour(1)));
    chips.push_back(chip(T("about 2 hops"), hopColour(2)));
    chips.push_back(chip(T("about 3 hops"), hopColour(3)));
    chips.push_back(chip(T("about 4 or more hops"), hopColour(4)));
    m.chips = model(std::move(chips));
}

// ---- 2. FT8 ----

void GlobePublisher::fillFt8(Surface& s, const FoxReceiverState& st, ::GlobeModel& m, std::string& sig) {
    LatLon rx;
    bool rxKnown = false;
    for (const PanelRowText& r : panelRows("FT8")) {
        if (r.kind == 0 && r.cells.size() >= 2 && r.cells[0] == "Locator") {
            const std::string loc = locatorIn(r.cells[1]);
            if (!loc.empty()) { rxKnown = locatorToLatLon(loc, rx); }
        }
    }
    if (!rxKnown && (st.flags & FOXAPI_RX_POSITION_SET) != 0u) { rx = LatLon{st.rxLatDeg, st.rxLonDeg}; rxKnown = true; }
    const int bandSel = std::clamp(s.view.bandId, 0, static_cast<int>(sizeof(kFt8Bands) / sizeof(kFt8Bands[0])) - 1);
    int winSel = 1;
    for (int i = 0; i < 3; ++i) { if (kWindows[i] == s.view.windowMinutes) { winSel = i; } }
    const std::int64_t now = unixMs();
    std::vector<Ft8Spot> spots = ft8_.visible(now, static_cast<std::int64_t>(kWindows[winSel]) * 60000LL, kFt8Bands[bandSel]);
    for (Ft8Spot& sp : spots) {
        if (rxKnown) { sp.km = distanceKm(rx, sp.at); sp.bearingDeg = bearingDeg(rx, sp.at); }
    }
    const SunPosition sun = sunPosition(julianDay(now / 1000));
    const std::uint64_t bake = earthBakeKey(&sun, false, 1024, 512);
    if (s.earth.empty() || s.earth.key != bake) { bakeEarth(s.earth, land(), &sun, false); }
    s.initial = initialCamera(ViewKind::Ft8, rxKnown, rx);
    const GlobeDrawList list = drawFt8(spots, rxKnown, rx, s.view, s.earth);
    std::string key = "F" + std::to_string(bake) + "b" + std::to_string(bandSel) + "w" + std::to_string(winSel) + "n" + std::to_string(spots.size());
    if (!spots.empty()) { key += std::to_string(spots.front().atMs) + "," + std::to_string(spots.back().atMs); }
    key += rxKnown ? latLonText(rx) : "-";
    key += num(list.camera.centreLonDeg, 2) + "," + num(list.camera.centreLatDeg, 2) + "," + num(list.camera.zoom, 2);
    finish(s, list, key, m, sig);

    const Ft8Summary sum = summariseSpots(spots);
    std::vector<::GlobeControl> c;
    std::vector<std::string> bandOpts{T("ALL")};
    for (std::size_t i = 1; i < sizeof(kFt8Bands) / sizeof(kFt8Bands[0]); ++i) { bandOpts.push_back(bandName(kFt8Bands[i])); }
    c.push_back(segs(kFt8Band, T("BAND"), bandOpts, bandSel, T("Shows the stations heard on this band only")));
    c.push_back(segs(kFt8Window, T("TIME WINDOW"), {T("15 min"), T("1 h"), T("24 h")}, winSel, T("How far back the stations heard are drawn")));
    c.push_back(readout(T("HEARD"), F(sum.stations == 1 ? "%d station on %d band" : "%d stations on %d bands", sum.stations, sum.bands)));
    c.push_back(readout(T("FURTHEST HEARD"), sum.hasFurthest ? sum.furthest.call + "  " + kmText(sum.furthest.km) + "  " + bearingText(sum.furthest.bearingDeg) : T("none yet")));
    c.push_back(readout(T("RECEIVER"), rxKnown ? latLonText(rx) : T("not set")));
    m.controls = model(std::move(c));
    std::vector<::GlobeChipModel> chips;
    std::vector<int> bandsSeen;
    for (const Ft8Spot& sp : spots) { if (std::find(bandsSeen.begin(), bandsSeen.end(), sp.bandId) == bandsSeen.end()) { bandsSeen.push_back(sp.bandId); } }
    std::sort(bandsSeen.begin(), bandsSeen.end());
    for (int b : bandsSeen) { chips.push_back(chip(b == 0 ? T("other band") : bandName(b), bandColour(b))); }
    m.chips = model(std::move(chips));
    m.readouts = strings({T("These are the stations the decoder printed, not who reported whom.")});
}

// ---- 3. GOES ----

void GlobePublisher::fillGoes(Surface& s, const FoxReceiverState& st, ::GlobeModel& m, std::string& sig) {
    const int sat = std::clamp(s.view.goesSatellite, 0, kGoesSatelliteCount - 1);
    const double lon = goesSatellite(sat).lonDeg;
    const bool rxKnown = (st.flags & FOXAPI_RX_POSITION_SET) != 0u;
    const LatLon rx{st.rxLatDeg, st.rxLonDeg};
    const cascade::core::HostImage* img = kit_.imageOf("GOES HRIT / LRIT");
    const bool have = img != nullptr && img->width > 0u && img->height > 0u;
    // the draped picture is baked when it changed or the spacecraft did, at most once a second (a bake is about 100 ms)
    if (have && (s.goesRevision != img->revision || s.goesSat != sat) && steadyMs() - s.goesAtMs >= 1000) {
        bakeGoes(s.goes, img->pixels, static_cast<int>(img->width), static_cast<int>(img->height), img->format == CASCADE_IMAGE_RGB24, lon);
        s.goesRevision = img->revision;
        s.goesSat = sat;
        s.goesAtMs = steadyMs();
    }
    const Texture* surface = &s.goes;
    if (!have || s.goes.empty()) {
        if (s.earth.empty() || s.earth.key != earthBakeKey(nullptr, false, 1024, 512)) { bakeEarth(s.earth, land(), nullptr, false); }
        surface = &s.earth;
    }
    s.initial = GlobeCamera{lon, 0.0, 1.0};
    GlobeDrawList list = drawGoes(s.view, lon, rxKnown, rx, *surface);
    if (!have) { list.emptyNote = "Waiting for the first image..."; }
    std::string key = "G" + std::to_string(sat) + (have ? "r" + std::to_string(s.goesRevision) : std::string("n")) + (rxKnown ? latLonText(rx) : "-");
    key += num(list.camera.centreLonDeg, 2) + "," + num(list.camera.centreLatDeg, 2) + "," + num(list.camera.zoom, 2);
    finish(s, list, key, m, sig);

    std::vector<::GlobeControl> c;
    c.push_back(segs(kGoesSat, T("SPACECRAFT"), {std::string(goesSatellite(0).name), std::string(goesSatellite(1).name)}, sat, T("Which spacecraft the received picture came from: GOES-East sits over 75.2 W, GOES-West over 137.0 W")));
    std::string look;
    if (!rxKnown) { look = T("Set the receiver position for the look angle"); }
    else {
        const LookAngle la = goesLook(rx, lon);
        look = la.visible ? F("el %.1f deg  az %.1f deg", la.elevationDeg, la.azimuthDeg) : T("below the horizon from the receiver");
    }
    c.push_back(readout(T("LOOK ANGLE"), look));
    c.push_back(readout(T("SUB-SATELLITE POINT"), latLonText(LatLon{0.0, lon})));
    if (have) {
        c.push_back(readout(T("PICTURE"), F(img->complete ? "%ux%u  complete  frame %llu" : "%ux%u  receiving...  frame %llu", static_cast<unsigned>(img->width), static_cast<unsigned>(img->height), static_cast<unsigned long long>(img->sequence))));
    }
    m.controls = model(std::move(c));
    m.readouts = strings({T("The picture is taken to be the ABI full-disk frame: the disc touches the picture's edges.")});
}

// ---- 4. the solar system ----

void GlobePublisher::fillSolar(Surface& s, const FoxReceiverState&, ::GlobeModel& m, std::string& sig) {
    const double todayJd = julianDay(unixMs() / 1000);
    const double jd = dateOfOffset(todayJd, s.view.dayOffset);
    const OrbitScene scene = buildOrbitScene(jd, s.view.logScale, s.view.showMoon);
    s.initial = GlobeCamera{20.0, 55.0, 1.0};
    const GlobeDrawList list = drawOrbits(scene, s.view);
    std::string key = "S" + dateText(jd) + (s.view.logScale ? "L" : "N") + (s.view.showMoon ? "m" : "-") + num(s.view.spinDeg, 1) + "," + num(s.view.tiltDeg, 1) + "," + num(list.camera.zoom, 2);
    finish(s, list, key, m, sig);

    std::vector<::GlobeControl> c;
    c.push_back(segs(kScale, T("SCALE"), {T("LOG"), T("LINEAR")}, s.view.logScale ? 0 : 1, T("LOG keeps the inner planets off the Sun (knee 0.1 AU); LINEAR is true to scale")));
    c.push_back(checkbox(kMoon, T("SHOW MOON"), s.view.showMoon, T("The Moon on a ring about the Earth, not to scale")));
    c.push_back(slider(kDate, T("DATE"), (s.view.dayOffset + kDayOffsetLimit) * 1000 / (2 * kDayOffsetLimit), dateText(jd) + (s.view.dayOffset == 0 ? "  " + T("TODAY") : std::string()),
                       T("Where the planets are on another date, within a year of today")));
    c.push_back(keyControl(kToday, T("TODAY"), T("Back to today's date")));
    c.push_back(readout(T("TILT / SPIN / ZOOM"), F("%d deg  %d deg  %.1fx", static_cast<int>(std::lround(s.view.tiltDeg)), static_cast<int>(std::lround(s.view.spinDeg)), list.camera.zoom)));
    // the Moon: the phase from the host's own figures
    c.push_back(readout(T("MOON"), F("%.0f km  %.0f %% lit", scene.moon.distanceKm, scene.moon.illuminatedFraction * 100.0)));
    // the check of the host's planets against the plugin's own panel for today
    if (s.view.dayOffset == 0) {
        const SolarPanelFacts pf = readSolarPanel([this] { std::vector<PanelRowText> r = panelRows("Solar System View"); return r; }());
        if (!pf.planets.empty()) {
            const PanelCheck ck = checkAgainstPanel(pf, todayJd);
            c.push_back(readout(T("CHECK"), ck.ok ? T("The positions agree with the plugin's own panel.") : F("These figures differ from the plugin's panel: %s", ck.worst.c_str())));
        }
    }
    m.controls = model(std::move(c));
    std::vector<::GlobeChipModel> chips;
    for (const BodyView& b : scene.bodies) { chips.push_back(chip(planetChipText(b), b.argb)); }
    chips.push_back(chip(F("Earth %.3f AU from the Sun", scene.bodies.empty() ? 0.0 : solar::earthSunDistanceAu(jd)), 0xFF4DA3FFu));
    m.chips = model(std::move(chips));
}

// ---- 5. satellites ----

void GlobePublisher::fillSatellites(Surface& s, const std::string& plugin, const FoxReceiverState& st, ::GlobeModel& m, std::string& sig) {
    const bool rxKnown = (st.flags & FOXAPI_RX_POSITION_SET) != 0u;
    const LatLon rx{st.rxLatDeg, st.rxLonDeg};
    SatSceneIn in;
    in.rxKnown = rxKnown;
    in.rx = rx;
    in.footprints = s.view.footprints;
    in.groundTracks = s.view.groundTracks;
    for (const cascade::core::HostTrack& tr : kit_.tracks()) {
        if (tr.plugin != plugin || !cascade::core::trackHasPosition(tr.t)) { continue; }
        SatIn sat;
        sat.id = tr.t.id;
        sat.label = tr.t.label;
        sat.at = LatLon{tr.t.latDeg, tr.t.lonDeg};
        sat.hasAlt = std::isfinite(tr.t.altM);
        sat.altKm = sat.hasAlt ? tr.t.altM / 1000.0 : 0.0;
        in.sats.push_back(std::move(sat));
    }
    for (const cascade::core::HostPath& p : kit_.paths()) {
        if (p.plugin != plugin) { continue; }
        SatPathIn sp;
        sp.id = p.id;
        sp.dashed = (p.flags & CASCADE_PATH_FLAG_DASHED) != 0u;
        sp.closed = (p.flags & CASCADE_PATH_FLAG_CLOSED) != 0u;
        for (const CascadePathPoint& q : p.points) { sp.points.push_back(LatLon{q.latDeg, q.lonDeg}); }
        in.paths.push_back(std::move(sp));
    }
    // the selection is the map page's own (a click on the map or its list), else the first satellite
    MapPage* pg = findMapPage(kit_.mapPages(), plugin);
    std::string selected = pg != nullptr ? pg->selectedId : std::string();
    bool found = false;
    for (const SatIn& sat : in.sats) { if (sat.id == selected) { found = true; } }
    if (!found) { selected = in.sats.empty() ? std::string() : in.sats.front().id; }
    in.selectedId = selected;
    const std::int64_t now = unixMs();
    const SunPosition sun = sunPosition(julianDay(now / 1000));
    const std::uint64_t bake = earthBakeKey(&sun, false, 1024, 512);
    if (s.earth.empty() || s.earth.key != bake) { bakeEarth(s.earth, land(), &sun, false); }
    s.initial = initialCamera(ViewKind::Satellites, rxKnown, rx);
    const GlobeDrawList list = drawSatellites(in, s.view, s.earth);
    std::string key = "T" + std::to_string(bake) + selected + (s.view.footprints ? "f" : "-") + (s.view.groundTracks ? "t" : "-") + (rxKnown ? latLonText(rx) : "-");
    for (const SatIn& sat : in.sats) { key += sat.id + num(sat.at.lat, 2) + "," + num(sat.at.lon, 2) + ";"; }
    for (const SatPathIn& p : in.paths) { key += p.id + std::to_string(p.points.size()) + (p.points.empty() ? "" : num(p.points.front().lat, 1) + num(p.points.front().lon, 1)); }
    key += num(list.camera.centreLonDeg, 2) + "," + num(list.camera.centreLatDeg, 2) + "," + num(list.camera.zoom, 2);
    finish(s, list, key, m, sig);

    std::vector<::GlobeControl> c;
    c.push_back(checkbox(kFootprint, T("FOOTPRINT"), s.view.footprints, T("The circle on the ground the selected satellite can see")));
    c.push_back(checkbox(kTracks, T("GROUND TRACKS"), s.view.groundTracks, T("The ground tracks the plugin predicts")));
    const SatIn* sel = nullptr;
    for (const SatIn& sat : in.sats) { if (sat.id == selected) { sel = &sat; } }
    if (sel != nullptr) {
        const SatReadout r = satReadout(*sel, rxKnown, rx);
        c.push_back(readout(T("SELECTED"), r.label + (r.hasAlt ? "  " + num(r.altKm, 0) + " km" : std::string())));
        c.push_back(readout(T("SUB-POINT"), latLonText(r.sub)));
        if (r.hasLook) {
            c.push_back(readout(T("FROM THE RECEIVER"), F("%s  el %.0f deg  az %.0f deg", kmText(r.look.slantRangeKm).c_str(), r.look.elevationDeg, r.look.azimuthDeg) + (r.look.visible ? std::string() : "  " + T("below the horizon"))));
        } else {
            c.push_back(readout(T("FROM THE RECEIVER"), rxKnown ? T("needs the satellite's altitude") : T("Set the receiver position")));
        }
    }
    m.controls = model(std::move(c));
    s.chipIds.clear();
    std::vector<::GlobeChipModel> chips;
    for (const SatIn& sat : in.sats) {
        s.chipIds.push_back(sat.id);
        chips.push_back(chip(sat.label.empty() ? sat.id : sat.label, sat.hasAlt ? orbitClassColour(sat.altKm) : 0xFFFFFFFFu, sat.id == selected, true));
    }
    m.chips = model(std::move(chips));
}

// ---- the gestures ----

void GlobePublisher::gesture(int page, const std::function<void(Surface&)>& f) {
    const std::string plugin = pluginOfPage_ ? pluginOfPage_(page) : std::string();
    if (plugin.empty()) { return; }
    f(surfaceFor(plugin));
}

void GlobePublisher::wire(::MainWindow& ui, std::function<std::string(int)> pluginOfPage) {
    pluginOfPage_ = std::move(pluginOfPage);
    auto& ev = ui.global<::GlobeEvents>();
    ev.on_view_chosen([this](int p, bool on) { gesture(p, [this, on](Surface& s) { s.view.globeOn = on; changed(s); }); });
    ev.on_segment_chosen([this](int p, int id, int idx) {
        gesture(p, [this, id, idx](Surface& s) {
            switch (s.kind) {
                case ViewKind::Propagation: if (id == kBand) { s.view.bandId = idx; } break;
                case ViewKind::Ft8:
                    if (id == kFt8Band) { s.view.bandId = idx; }
                    else if (id == kFt8Window && idx >= 0 && idx < 3) { s.view.windowMinutes = kWindows[idx]; }
                    break;
                case ViewKind::Goes:
                    if (id == kGoesSat) { s.view.goesSatellite = std::clamp(idx, 0, kGoesSatelliteCount - 1); s.view.cameraSet = false; ++s.camSeq; }
                    break;
                case ViewKind::SolarSystem: if (id == kScale) { s.view.logScale = idx == 0; } break;
                default: break;
            }
            changed(s);
        });
    });
    ev.on_toggled([this](int p, int id, bool on) {
        gesture(p, [this, id, on](Surface& s) {
            switch (s.kind) {
                case ViewKind::Propagation: if (id == kGreyline) { s.view.greyline = on; } break;
                case ViewKind::SolarSystem: if (id == kMoon) { s.view.showMoon = on; } break;
                case ViewKind::Satellites:
                    if (id == kFootprint) { s.view.footprints = on; }
                    else if (id == kTracks) { s.view.groundTracks = on; }
                    break;
                default: break;
            }
            changed(s);
        });
    });
    ev.on_slid([this](int p, int id, float v) {
        gesture(p, [id, v](Surface& s) {
            if (s.kind == ViewKind::SolarSystem && id == kDate) {
                s.view.dayOffset = std::clamp(static_cast<int>(std::lround(v * 2.0f * kDayOffsetLimit)) - kDayOffsetLimit, -kDayOffsetLimit, kDayOffsetLimit);
            }
        });
    });
    ev.on_key_pressed([this](int p, int id) {
        gesture(p, [id](Surface& s) { if (s.kind == ViewKind::SolarSystem && id == kToday) { s.view.dayOffset = 0; } });
    });
    ev.on_chip_pressed([this](int p, int idx) {
        gesture(p, [this, idx](Surface& s) {
            if (s.kind == ViewKind::Satellites && idx >= 0 && idx < static_cast<int>(s.chipIds.size())) {
                MapPage* pg = findMapPage(kit_.mapPages(), "Satellites");
                if (pg != nullptr) { pg->selectedId = s.chipIds[static_cast<std::size_t>(idx)]; }
            }
        });
    });
    // the well's own move of the camera (a drag, the wheel, a key): the host takes its numbers
    ev.on_camera_moved([this](int p, float az, float el, float zoom) {
        gesture(p, [this, az, el, zoom](Surface& s) {
            const double z = std::clamp(static_cast<double>(zoom), kZoomMin, kZoomMax);
            if (s.kind == ViewKind::SolarSystem) {
                s.view.spinDeg = std::fmod(static_cast<double>(az) + 720.0, 360.0);
                s.view.tiltDeg = std::clamp(static_cast<double>(el), 5.0, 85.0);
                s.view.camera.zoom = z;
            } else {
                s.view.camera = GlobeCamera{normalizeLon(az), std::clamp(static_cast<double>(el), -85.0, 85.0), z};
            }
            s.view.cameraSet = true;
            changed(s);
        });
    });
    ev.on_reset([this](int p) {
        gesture(p, [this](Surface& s) {
            s.view.cameraSet = false;
            s.view.camera = defaultCamera(s.kind);
            s.view.spinDeg = 20.0;
            s.view.tiltDeg = 55.0;
            ++s.camSeq;
            changed(s);
        });
    });
    ev.on_resized([this](int p, int w, int h) {
        gesture(p, [w, h](Surface& s) { s.w = std::max(64, w); s.h = std::max(64, h); });
    });
}

bool GlobePublisher::takeCommand(const std::string& plugin, const std::string& label) {
    if (viewKindFor(plugin) != ViewKind::SolarSystem) { return false; }
    Surface& s = surfaceFor(plugin);
    if (!s.view.globeOn) { return false; }
    if (iequals(label, "ROTATE LEFT")) { s.view.spinDeg = std::fmod(s.view.spinDeg - 10.0 + 360.0, 360.0); }
    else if (iequals(label, "ROTATE RIGHT")) { s.view.spinDeg = std::fmod(s.view.spinDeg + 10.0, 360.0); }
    else if (iequals(label, "TILT")) { s.view.tiltDeg = s.view.tiltDeg + 10.0 > 85.0 ? 10.0 : s.view.tiltDeg + 10.0; }
    else if (iequals(label, "ZOOM IN")) { GlobeCamera c = s.view.camera; zoomCamera(c, 1.0); s.view.camera.zoom = c.zoom; }
    else if (iequals(label, "ZOOM OUT")) { GlobeCamera c = s.view.camera; zoomCamera(c, -1.0); s.view.camera.zoom = c.zoom; }
    else { return false; }
    s.view.cameraSet = true;
    ++s.camSeq;
    changed(s);
    return true;
}

std::string GlobePublisher::statusLine() const {
    std::string out;
    for (const auto& kv : surfaces_) {
        if (!kv.second->view.globeOn) { continue; }
        if (!out.empty()) { out += "; "; }
        out += kv.first + " 3D" + (kv.second->composed ? "" : " (no picture)");
    }
    return out;
}

std::string GlobePublisher::stateText(const std::string& plugin) const {
    const Surface* s = findSurface(plugin);
    if (s == nullptr) { return "none"; }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "globe=%d lon=%.1f lat=%.1f zoom=%.2f spin=%.0f tilt=%.0f band=%d window=%d log=%d moon=%d day=%d sat=%d greyline=%d composed=%d", s->view.globeOn ? 1 : 0,
                  s->shown.centreLonDeg, s->shown.centreLatDeg, s->shown.zoom, s->view.spinDeg, s->view.tiltDeg, s->view.bandId, s->view.windowMinutes, s->view.logScale ? 1 : 0,
                  s->view.showMoon ? 1 : 0, s->view.dayOffset, s->view.goesSatellite, s->view.greyline ? 1 : 0, s->composed ? 1 : 0);
    return buf;
}

}  // namespace foxsdr::slintkit
