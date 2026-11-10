// view3d_publish.cpp - the 3D views' publisher (see view3d_publish.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "view3d_publish.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/config.hpp"
#include "core/i18n.hpp"
#include "scene3d_slint.hpp"
#include "radar3d_compositor.hpp"
#include "survey3d_compositor.hpp"
#include "tunnel3d_compositor.hpp"
#include "view3d_prefs.hpp"
#include "waterfall3d_logic.hpp"

namespace foxsdr::slintview3d {

namespace {

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

constexpr int kDefaultW = 960, kDefaultH = 400;    // before the well has reported its size
constexpr int kMaxW = 1280, kMaxH = 800;           // a well bigger than this is painted at this and scaled up (image-fit fill)
constexpr std::int64_t kSaveSettleMs = 700;        // a camera move is saved once it has rested this long

double nowMsReal() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string numSig(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.4f|", v);
    return b;
}

// "40 ms": TIME DEPTH's readout (the unit is the same in every language)
std::string kTunnelDepthLabels(int index) {
    const double ms = kTunnelDepthsMs[static_cast<std::size_t>(std::clamp(index, 0, 15))];
    char b[24];
    std::snprintf(b, sizeof(b), "%.0f ms", ms);
    return b;
}

ViewState waterfallDefaults() {
    ViewState s;
    const scene3d::Camera c = waterfall3dDefaultCamera();
    setCamera(s, c);
    s.opt = {1, 0, 0, 0};   // HISTORY DEPTH 60 s, SHADING Height
    return s;
}

ViewState radarDefaults() {
    ViewState s;
    setCamera(s, radar3dDefaultCamera());
    s.opt = {0, 0, 0, 0};   // ALTITUDE SCALE x1
    return s;
}

ViewState tunnelDefaults() {
    ViewState s;
    setCamera(s, tunnelDefaultCamera());
    s.opt = {kTunnelDefaultDepth, 0, 0, 0};   // TIME DEPTH 40 ms, COLOUR BY Time
    return s;
}

ViewState surveyDefaults() {
    ViewState s;
    setCamera(s, survey3dDefaultCamera());
    return s;
}

std::string lowerOf(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

slint::Color colourOf(std::uint32_t rgba) {
    return slint::Color::from_argb_uint8(static_cast<std::uint8_t>(rgba & 0xffu), static_cast<std::uint8_t>(rgba >> 24), static_cast<std::uint8_t>(rgba >> 16), static_cast<std::uint8_t>(rgba >> 8));
}

}  // namespace

struct View3DPublisher::Impl {
    MainWindow& ui;
    foxsdr::slinthost::HostDuties& duties;
    bool bounded;
    foxsdr::slintkit::PluginKit& kit;
    ViewPrefs prefs;
    std::string savedText;

    // ---- the waterfall surface ----
    ViewState wf = waterfallDefaults();
    WaterfallHistory history;
    std::int64_t lastPushMs = 0;
    scene3d::SceneSurface surface;
    scene3d::Scene scene;
    std::vector<float> grid;
    std::string wfSig;
    std::int64_t wfSaveAtMs = 0;
    bool wfDirtySave = false;
    std::size_t renders = 0, lastTriangles = 0, lastPixels = 0, lastLabels = 0, saves = 0;
    double lastRenderMs = 0.0, worstRenderMs = 0.0;
    bool hasData = false;
    std::int64_t lastNowMs = 0;       // the clock of the last publish: what the history's span is measured against
    std::size_t lastRealRows = 0;     // rows of the last grid that held data
    float prevSplit = 0.6f;           // the spectrum's share of the centre before the surface took more of it
    bool splitTaken = false;
    int renderW = 0, renderH = 0;
    int appliedShading = -1;

    // ---- the radar scope's 3D airspace ----
    ViewState radar = radarDefaults();
    scene3d::SceneSurface radarSurface;
    scene3d::Scene radarScene;
    std::string radarSig;
    std::size_t radarRenders = 0, radarAircraft = 0, radarOnGround = 0, radarTrailSegs = 0, radarPixels = 0, radarLabels = 0, radarTags = 0;
    double radarLastMs = 0.0, radarWorstMs = 0.0;
    int radarW = 0, radarH = 0;
    bool radarRampSet = false;

    // ---- the demod scope's 3D tunnel ----
    ViewState tunnel = tunnelDefaults();
    scene3d::SceneSurface tunnelSurface;
    scene3d::Scene tunnelScene;
    std::string tunnelSig;
    std::size_t tunnelRenders = 0, tunnelPoints = 0, tunnelPixels = 0, tunnelLabels = 0;
    double tunnelLastMs = 0.0, tunnelWorstMs = 0.0, tunnelDepthMs = 0.0;
    int tunnelW = 0, tunnelH = 0;
    std::string depthText;

    Impl(MainWindow& u, foxsdr::slinthost::HostDuties& d, bool b, foxsdr::slintkit::PluginKit& k) : ui(u), duties(d), bounded(b), kit(k) {}

    // ---- the Survey Engine's 3D surface ----
    ViewState survey = surveyDefaults();
    scene3d::SceneSurface surveySurface;
    scene3d::Scene surveyScene;
    std::string surveySig;
    std::size_t surveyRenders = 0, surveyChannels = 0, surveyTriangles = 0, surveyPixels = 0, surveyRanked = 0, surveyLabels = 0;
    double surveyLastMs = 0.0, surveyWorstMs = 0.0;
    int surveyW = 0, surveyH = 0;
    std::string surveyFirstRow;

    // The page is on screen: a page of the kit with this 3D kind, docked and current, or torn off into a window.
    bool pageShown(int view3dKind) const {
        const auto model = ui.get_pages();
        if (!model) { return false; }
        for (std::size_t i = 0; i < model->row_count(); ++i) {
            const auto t = model->row_data(i);
            if (!t || t->view3d != view3dKind) { continue; }
            if (t->torn || ui.get_view_index() == 3 + static_cast<int>(i)) { return true; }
        }
        return false;
    }

    // The Survey Engine's panel from the kit, as channels. A panel whose title says "survey"; rows whose frequency does not read (the status rows) are skipped.
    SurveyFeed surveyFeedNow() const {
        SurveyFeed f;
        for (const cascade::core::HostPanel& p : kit.panels()) {
            if (lowerOf(p.title + " " + p.plugin).find("survey") == std::string::npos) { continue; }
            f.shown = true;
            for (const CascadePanelRow& r : p.rows) {
                std::vector<std::string> cells;
                for (std::size_t c = 0; c < p.headings.size() && c < CASCADE_PANEL_MAX_COLUMNS; ++c) { cells.emplace_back(r.cells[c]); }
                SurveyChannel ch;
                if (channelOfCells(p.headings, cells, ch)) { f.channels.push_back(std::move(ch)); }
                else if (f.status.empty() && !cells.empty()) { f.status = cells[0]; }
            }
            break;
        }
        return f;
    }

    void surveyPublish(std::int64_t nowMs) {
        const auto& g = ui.global<View3D>();
        const float az = g.get_survey_azimuth(), el = g.get_survey_elevation(), zm = g.get_survey_zoom();
        if (az != survey.azimuth || el != survey.elevation || zm != survey.zoom) {
            scene3d::Camera c = cameraOf(survey);
            c.azimuthDeg = az;
            c.elevationDeg = el;
            c.zoom = zm;
            setCamera(survey, scene3d::clamped(c));
            markSave(nowMs);
        }
        if (wfDirtySave && nowMs >= wfSaveAtMs) { wfDirtySave = false; save(); }
        if (!survey.on || !pageShown(1)) { return; }
        const SurveyFeed feed = surveyFeedNow();
        int w = g.get_survey_w(), h = g.get_survey_h();
        if (w < 64 || h < 64) { w = 640; h = 360; }
        surveyW = std::min(w, kMaxW);
        surveyH = std::min(h, kMaxH);
        char b[96];
        std::string sig = std::to_string(surveyW) + "x" + std::to_string(surveyH) + "|" + numSig(survey.azimuth) + numSig(survey.elevation) + numSig(survey.zoom) + "|";
        for (const SurveyChannel& c : feed.channels) {
            std::snprintf(b, sizeof(b), "%.0f,%.0f,%.1f,%.1f,%.1f;", c.freqHz, c.widthHz, c.peakDb, c.overDb, c.seenPct);
            sig += b;
        }
        if (sig == surveySig) { return; }
        surveySig = sig;
        SurveyInput in;
        in.channels = feed.channels;
        in.camera = cameraOf(survey);
        in.captionSeen = cascade::i18n::tr("Seen (%)");
        const double t0 = nowMsReal();
        scene3d::Rgba* mem = surveySurface.begin(surveyW, surveyH);
        const SurveyResult res = paintSurvey3d(surveyScene, mem, surveyW, surveyH, in);
        g.set_survey_picture(surveySurface.image());
        surveySurface.flip();
        surveyLastMs = nowMsReal() - t0;
        surveyWorstMs = std::max(surveyWorstMs, surveyLastMs);
        ++surveyRenders;
        surveyChannels = feed.channels.size();
        surveyTriangles = res.triangles;
        surveyPixels = surveyScene.pixelsWritten();
        surveyRanked = res.ranked;
        surveyLabels = res.labels.size();
        surveyFirstRow = feed.channels.empty() ? std::string() : feed.channels.front().raw;
        std::vector<Scene3DLabel> labels;
        labels.reserve(res.labels.size());
        for (const scene3d::Label3D& l : res.labels) { labels.push_back({l.x, l.y, ss(l.text), colourOf(l.rgba), l.align, l.kind}); }
        g.set_survey_labels(std::make_shared<slint::VectorModel<Scene3DLabel>>(std::move(labels)));
        // CHANNEL RANK: the busiest six, the panel's own cells
        std::vector<SurveyRankRow> rank;
        for (std::size_t i = 0; i < feed.channels.size() && i < 6; ++i) {
            const SurveyChannel& c = feed.channels[i];
            rank.push_back({ss(std::to_string(i + 1)), ss(c.freqText), ss(c.seenText), ss(c.band)});
        }
        g.set_survey_rank(std::make_shared<slint::VectorModel<SurveyRankRow>>(std::move(rank)));
    }

    void load() {
        prefs = ViewPrefs::parse(duties.interfaceHalf().view3d);
        savedText = prefs.serialize();
        wf = prefs.get(kViewWaterfall, waterfallDefaults());
        wf.opt[0] = std::clamp(wf.opt[0], 0, 2);
        wf.opt[1] = std::clamp(wf.opt[1], 0, 1);
        radar = prefs.get(kViewRadar, radarDefaults());
        radar.opt[0] = std::clamp(radar.opt[0], 0, 2);
        tunnel = prefs.get(kViewTunnel, tunnelDefaults());
        tunnel.opt[0] = std::clamp(tunnel.opt[0], 0, 15);
        tunnel.opt[1] = std::clamp(tunnel.opt[1], 0, 1);
        survey = prefs.get(kViewSurvey, surveyDefaults());
    }

    void apply() {
        const auto& g = ui.global<View3D>();
        g.set_wf_on(wf.on);
        g.set_wf_azimuth(wf.azimuth);
        g.set_wf_elevation(wf.elevation);
        g.set_wf_zoom(wf.zoom);
        g.set_wf_depth(wf.opt[0]);
        g.set_wf_shading(wf.opt[1]);
        takeRoom(wf.on);
        wfSig.clear();
        g.set_radar_on(radar.on);
        g.set_radar_azimuth(radar.azimuth);
        g.set_radar_elevation(radar.elevation);
        g.set_radar_zoom(radar.zoom);
        g.set_radar_scale(radar.opt[0]);
        radarSig.clear();
        g.set_tunnel_on(tunnel.on);
        g.set_tunnel_azimuth(tunnel.azimuth);
        g.set_tunnel_elevation(tunnel.elevation);
        g.set_tunnel_zoom(tunnel.zoom);
        g.set_tunnel_depth_pos(sliderOfDepthIndex(tunnel.opt[0]));
        g.set_tunnel_colour(tunnel.opt[1]);
        g.set_tunnel_depth_text(ss(kTunnelDepthLabels(tunnel.opt[0])));
        tunnelSig.clear();
        g.set_survey_on(survey.on);
        g.set_survey_azimuth(survey.azimuth);
        g.set_survey_elevation(survey.elevation);
        g.set_survey_zoom(survey.zoom);
        surveySig.clear();
    }

    // THE SAVE: the text of every view, when it changed, into the interface half (a bounded run keeps it in the host's copy, which `state cfg.view3d` reads, and writes no file:
    // that file is named only by a scratch run's CASCADE_CONFIG_TEST, as for every other bank).
    void save() {
        prefs.set(kViewWaterfall, wf);
        prefs.set(kViewRadar, radar);
        prefs.set(kViewTunnel, tunnel);
        prefs.set(kViewSurvey, survey);
        const std::string text = prefs.serialize();
        if (text == savedText) { return; }
        savedText = text;
        cascade::core::AppConfig half = duties.interfaceHalf();
        half.view3d = text;
        if (duties.saveInterfaceHalf(half)) { ++saves; }
    }

    // THE SURFACE NEEDS ROOM: the page's own split gives the spectrum 60 % of the centre, which leaves the surface a strip. While the 3D view is up the spectrum takes 38 % (the design's
    // 40 %); the user's split comes back with the 2D view. A drag of the splitter in between is the user's and is not undone until then.
    void takeRoom(bool on) {
        if (on && !splitTaken) { prevSplit = ui.get_split_ratio(); splitTaken = true; ui.set_split_ratio(0.38f); }
        else if (!on && splitTaken) { ui.set_split_ratio(prevSplit); splitTaken = false; }
    }

    void markSave(std::int64_t nowMs) { wfDirtySave = true; wfSaveAtMs = nowMs + kSaveSettleMs; }

    // THE TUNNEL: the camera read back, the picture repainted while the demod window is open on a baseband input, in 3D, and the samples or the choices moved.
    void tunnelPublish(const TunnelFeed& feed, std::int64_t nowMs) {
        const auto& g = ui.global<View3D>();
        const float az = g.get_tunnel_azimuth(), el = g.get_tunnel_elevation(), zm = g.get_tunnel_zoom();
        if (az != tunnel.azimuth || el != tunnel.elevation || zm != tunnel.zoom) {
            scene3d::Camera c = cameraOf(tunnel);
            c.azimuthDeg = az;
            c.elevationDeg = el;
            c.zoom = zm;
            setCamera(tunnel, scene3d::clamped(c));
            markSave(nowMs);
        }
        if (wfDirtySave && nowMs >= wfSaveAtMs) { wfDirtySave = false; save(); }
        if (!tunnel.on || !feed.shown) { return; }
        int w = g.get_tunnel_w(), h = g.get_tunnel_h();
        if (w < 64 || h < 64) { w = 700; h = 300; }
        tunnelW = std::min(w, kMaxW);
        tunnelH = std::min(h, kMaxH);
        const double avail = availableMs(feed.i.size(), feed.rateHz);
        tunnelDepthMs = effectiveDepthMs(tunnel.opt[0], avail, feed.rateHz);
        const std::size_t take = samplesFor(tunnelDepthMs, feed.rateHz, feed.i.size());
        // the signature: the choices, the size, the camera and the newest samples (a hash of the ones drawn, so a steady carrier does not repaint at the tick rate)
        std::uint64_t hsh = 1469598103934665603ull;
        for (std::size_t k = feed.i.size() - take; k < feed.i.size(); k += std::max<std::size_t>(1, take / 64)) {
            std::uint32_t a, b2;
            std::memcpy(&a, &feed.i[k], sizeof(a));
            std::memcpy(&b2, &feed.q[k], sizeof(b2));
            hsh = (hsh ^ a) * 1099511628211ull;
            hsh = (hsh ^ b2) * 1099511628211ull;
        }
        std::string sig = std::to_string(tunnelW) + "x" + std::to_string(tunnelH) + "|" + numSig(tunnel.azimuth) + numSig(tunnel.elevation) + numSig(tunnel.zoom) + std::to_string(tunnel.opt[0]) + "|" +
                          std::to_string(tunnel.opt[1]) + "|" + std::to_string(take) + "|" + std::to_string(hsh) + "|" + (feed.live ? "L" : "-") + feed.dead;
        if (sig == tunnelSig) { return; }
        tunnelSig = sig;
        TunnelInput in;
        if (take > 0) {
            in.i = feed.i.data() + (feed.i.size() - take);
            in.q = feed.q.data() + (feed.q.size() - take);
        }
        in.n = take;
        in.rateHz = feed.rateHz;
        in.colourBy = tunnel.opt[1];
        in.camera = cameraOf(tunnel);
        const double t0 = nowMsReal();
        scene3d::Rgba* mem = tunnelSurface.begin(tunnelW, tunnelH);
        const TunnelResult res = paintTunnel(tunnelScene, mem, tunnelW, tunnelH, in);
        g.set_tunnel_picture(tunnelSurface.image());
        tunnelSurface.flip();
        tunnelLastMs = nowMsReal() - t0;
        tunnelWorstMs = std::max(tunnelWorstMs, tunnelLastMs);
        ++tunnelRenders;
        tunnelPoints = res.points;
        tunnelPixels = tunnelScene.pixelsWritten();
        tunnelLabels = res.labels.size();
        std::vector<Scene3DLabel> labels;
        labels.reserve(res.labels.size());
        for (const scene3d::Label3D& l : res.labels) { labels.push_back({l.x, l.y, ss(l.text), colourOf(l.rgba), l.align, l.kind}); }
        g.set_tunnel_labels(std::make_shared<slint::VectorModel<Scene3DLabel>>(std::move(labels)));
        // the honest empty state: the scope's own sentence while no samples arrive
        g.set_tunnel_empty(ss(take == 0 || !feed.live ? feed.dead : std::string()));
    }

    // THE RADAR AIRSPACE: the camera read back (TILT, a drag, a key), the picture repainted while the scope is up, in 3D and something moved.
    void radarPublish(const Radar3dFeed& feed, std::int64_t nowMs) {
        const auto& g = ui.global<View3D>();
        const float az = g.get_radar_azimuth(), el = g.get_radar_elevation(), zm = g.get_radar_zoom();
        if (az != radar.azimuth || el != radar.elevation || zm != radar.zoom) {
            scene3d::Camera c = cameraOf(radar);
            c.azimuthDeg = az;
            c.elevationDeg = el;
            c.zoom = zm;
            setCamera(radar, scene3d::clamped(c));
            markSave(nowMs);
        }
        if (wfDirtySave && nowMs >= wfSaveAtMs) { wfDirtySave = false; save(); }
        if (!radar.on || !feed.shown) { return; }
        int w = g.get_radar_w(), h = g.get_radar_h();
        if (w < 64 || h < 64) { w = 700; h = 480; }
        radarW = std::min(w, kMaxW);
        radarH = std::min(h, kMaxH);
        if (!radarRampSet) {
            g.set_radar_key_ramp(scene3d::rampImage(radar3dRamp(), true, 96));
            radarRampSet = true;
        }
        char b[64];
        std::string sig = std::to_string(radarW) + "x" + std::to_string(radarH) + "|" + numSig(radar.azimuth) + numSig(radar.elevation) + numSig(radar.zoom) + std::to_string(radar.opt[0]) + "|" +
                          std::to_string(feed.trailMode) + "|" + numSig(feed.rangeNm) + "|" + feed.emptyText + "|";
        for (const Radar3dAircraft& a : feed.aircraft) {
            std::snprintf(b, sizeof(b), "%.2f,%.2f,%.0f,%.0f,%d%d,%zu;", a.eastNm, a.northNm, a.altFt, a.courseDeg, a.selected ? 1 : 0, a.emergency ? 1 : 0, a.trail.size());
            sig += a.id + a.label + b;
        }
        if (sig == radarSig) { return; }
        radarSig = sig;
        Radar3dInput in;
        in.rangeNm = feed.rangeNm;
        in.aircraft = feed.aircraft;
        in.scaleIndex = radar.opt[0];
        in.trails = feed.trailMode;
        in.camera = cameraOf(radar);
        in.noAlt = cascade::i18n::tr("NO ALT");
        const double t0 = nowMsReal();
        scene3d::Rgba* mem = radarSurface.begin(radarW, radarH);
        const Radar3dResult res = paintRadar3d(radarScene, mem, radarW, radarH, in);
        g.set_radar_picture(radarSurface.image());
        radarSurface.flip();
        radarLastMs = nowMsReal() - t0;
        radarWorstMs = std::max(radarWorstMs, radarLastMs);
        ++radarRenders;
        radarAircraft = res.aircraft;
        radarOnGround = res.onGround;
        radarTrailSegs = res.trailSegments;
        radarPixels = radarScene.pixelsWritten();
        radarLabels = res.labels.size();
        radarTags = 0;
        for (const scene3d::Label3D& l : res.labels) { radarTags += l.kind == 2 ? 1 : 0; }
        std::vector<Scene3DLabel> labels;
        labels.reserve(res.labels.size());
        for (const scene3d::Label3D& l : res.labels) { labels.push_back({l.x, l.y, ss(l.text), colourOf(l.rgba), l.align, l.kind}); }
        g.set_radar_labels(std::make_shared<slint::VectorModel<Scene3DLabel>>(std::move(labels)));
        g.set_radar_empty(ss(feed.emptyText));
    }

    void waterfall(const Wf3dContext& ctx) {
        lastNowMs = ctx.nowMs;
        const auto& g = ui.global<View3D>();
        // what the window says now: the camera (a drag, a key, a slider) is read back and remembered
        const float az = g.get_wf_azimuth(), el = g.get_wf_elevation(), zm = g.get_wf_zoom();
        if (az != wf.azimuth || el != wf.elevation || zm != wf.zoom) {
            scene3d::Camera c = cameraOf(wf);
            c.azimuthDeg = az;
            c.elevationDeg = el;
            c.zoom = zm;
            setCamera(wf, scene3d::clamped(c));
            markSave(ctx.nowMs);
        }
        if (wfDirtySave && ctx.nowMs >= wfSaveAtMs) { wfDirtySave = false; save(); }
        if (!wf.on || !ctx.visible) { return; }

        int w = g.get_wf_w(), h = g.get_wf_h();
        if (w < 64 || h < 64) { w = kDefaultW; h = kDefaultH; }
        renderW = std::min(w, kMaxW);
        renderH = std::min(h, kMaxH);
        if (appliedShading != wf.opt[1]) {
            g.set_wf_key_ramp(scene3d::rampImage(waterfall3dRamp(wf.opt[1]), false, 96));
            appliedShading = wf.opt[1];
        }
        // the signature: everything the picture is made of. The age of the newest row matters only while frames arrive (a stopped receiver is refreshed once a second).
        const bool fresh = ctx.nowMs - lastPushMs < 3000;
        std::string sig = std::to_string(renderW) + "x" + std::to_string(renderH) + "|" + numSig(wf.azimuth) + numSig(wf.elevation) + numSig(wf.zoom) + std::to_string(wf.opt[0]) + "|" +
                          std::to_string(wf.opt[1]) + "|" + numSig(ctx.dbMin) + numSig(ctx.dbMax) + numSig(ctx.centreHz) + numSig(ctx.spanHz) + numSig(ctx.tunedHz) + "|" +
                          std::to_string(history.size()) + "|" + std::to_string(lastPushMs) + "|" + std::to_string(fresh ? 0 : ctx.nowMs / 1000) + "|" + (ctx.running ? "R" : "-");
        for (const Wf3dMarker& m : ctx.markers) { sig += numSig(m.hz) + m.label + ";"; }
        if (sig == wfSig) { return; }
        wfSig = sig;

        int rows = 0, cols = 0;
        gridSizeFor(renderW, renderH, rows, cols);
        const double depthS = kWf3dDepthsS[static_cast<std::size_t>(std::clamp(wf.opt[0], 0, 2))];
        lastRealRows = history.grid(ctx.nowMs, depthS, rows, cols, grid);
        Wf3dInput in;
        in.grid = grid.data();
        in.rows = rows;
        in.cols = cols;
        in.centreHz = ctx.centreHz;
        in.spanHz = ctx.spanHz;
        in.tunedHz = ctx.tunedHz;
        in.dbMin = ctx.dbMin;
        in.dbMax = ctx.dbMax;
        in.depthS = depthS;
        in.markers = ctx.markers;
        in.shading = wf.opt[1];
        in.camera = cameraOf(wf);
        in.captionAge = cascade::i18n::tr("Age (s)");
        const double t0 = nowMsReal();
        scene3d::Rgba* mem = surface.begin(renderW, renderH);
        const Wf3dResult res = paintWaterfall3d(scene, mem, renderW, renderH, in);
        g.set_wf_picture(surface.image());
        surface.flip();
        lastRenderMs = nowMsReal() - t0;
        worstRenderMs = std::max(worstRenderMs, lastRenderMs);
        ++renders;
        lastTriangles = res.triangles;
        lastPixels = scene.pixelsWritten();
        lastLabels = res.labels.size();
        hasData = res.hasData;
        std::vector<Scene3DLabel> labels;
        labels.reserve(res.labels.size());
        for (const scene3d::Label3D& l : res.labels) { labels.push_back({l.x, l.y, ss(l.text), colourOf(l.rgba), l.align, l.kind}); }
        g.set_wf_labels(std::make_shared<slint::VectorModel<Scene3DLabel>>(std::move(labels)));
        // the honest empty state: no row of the history is on the grid. A running receiver is about to have one; a stopped one is said so, in the page's own words.
        g.set_wf_empty(ss(!res.hasData && !ctx.running ? std::string(cascade::i18n::tr("No spectrum yet - its radio is not running.")) : std::string()));
    }
};

View3DPublisher::View3DPublisher(MainWindow& ui, foxsdr::slinthost::HostDuties& duties, bool bounded, foxsdr::slintkit::PluginKit& kit) : impl_(std::make_unique<Impl>(ui, duties, bounded, kit)), ui_(ui) {
    impl_->load();
}
View3DPublisher::~View3DPublisher() = default;

void View3DPublisher::wire() {
    Impl& m = *impl_;
    m.apply();
    const auto& g = ui_.global<View3D>();
    g.on_wf_view_selected([this](bool on) {
        Impl& i = *impl_;
        i.wf.on = on;
        ui_.global<View3D>().set_wf_on(on);
        i.takeRoom(on);
        i.wfSig.clear();
        i.save();
    });
    g.on_wf_depth_selected([this](int d) {
        Impl& i = *impl_;
        i.wf.opt[0] = std::clamp(d, 0, 2);
        ui_.global<View3D>().set_wf_depth(i.wf.opt[0]);
        i.wfSig.clear();
        i.save();
    });
    g.on_wf_shading_selected([this](int s) {
        Impl& i = *impl_;
        i.wf.opt[1] = std::clamp(s, 0, 1);
        ui_.global<View3D>().set_wf_shading(i.wf.opt[1]);
        i.wfSig.clear();
        i.save();
    });
    g.on_survey_view_selected([this](bool on) {
        Impl& i = *impl_;
        i.survey.on = on;
        ui_.global<View3D>().set_survey_on(on);
        i.surveySig.clear();
        i.save();
    });
    g.on_survey_reset_view([this] {
        Impl& i = *impl_;
        const ViewState d = surveyDefaults();
        i.survey.azimuth = d.azimuth;
        i.survey.elevation = d.elevation;
        i.survey.zoom = d.zoom;
        i.surveySig.clear();
        i.save();
    });
    g.on_tunnel_view_selected([this](bool on) {
        Impl& i = *impl_;
        i.tunnel.on = on;
        ui_.global<View3D>().set_tunnel_on(on);
        i.tunnelSig.clear();
        i.save();
    });
    g.on_tunnel_depth_moved([this](float v) {
        Impl& i = *impl_;
        i.tunnel.opt[0] = depthIndexOfSlider(v);
        const auto& gl = ui_.global<View3D>();
        gl.set_tunnel_depth_pos(sliderOfDepthIndex(i.tunnel.opt[0]));
        gl.set_tunnel_depth_text(ss(kTunnelDepthLabels(i.tunnel.opt[0])));
        i.tunnelSig.clear();
        i.save();
    });
    g.on_tunnel_colour_selected([this](int c) {
        Impl& i = *impl_;
        i.tunnel.opt[1] = std::clamp(c, 0, 1);
        ui_.global<View3D>().set_tunnel_colour(i.tunnel.opt[1]);
        i.tunnelSig.clear();
        i.save();
    });
    g.on_tunnel_reset_view([this] {
        Impl& i = *impl_;
        const ViewState d = tunnelDefaults();
        i.tunnel.azimuth = d.azimuth;
        i.tunnel.elevation = d.elevation;
        i.tunnel.zoom = d.zoom;
        i.tunnelSig.clear();
        i.save();
    });
    g.on_radar_view_selected([this](bool on) {
        Impl& i = *impl_;
        i.radar.on = on;
        ui_.global<View3D>().set_radar_on(on);
        i.radarSig.clear();
        i.save();
    });
    g.on_radar_scale_selected([this](int s) {
        Impl& i = *impl_;
        i.radar.opt[0] = std::clamp(s, 0, 2);
        ui_.global<View3D>().set_radar_scale(i.radar.opt[0]);
        i.radarSig.clear();
        i.save();
    });
    g.on_radar_reset_view([this] {
        Impl& i = *impl_;
        const ViewState d = radarDefaults();
        i.radar.azimuth = d.azimuth;
        i.radar.elevation = d.elevation;
        i.radar.zoom = d.zoom;
        i.radarSig.clear();
        i.save();
    });
    g.on_wf_reset_view([this] {
        Impl& i = *impl_;
        const ViewState d = waterfallDefaults();
        i.wf.azimuth = d.azimuth;
        i.wf.elevation = d.elevation;
        i.wf.zoom = d.zoom;
        i.wfSig.clear();
        i.save();
    });
}

void View3DPublisher::pushWaterfallFrame(std::int64_t tMs, const float* bins, std::size_t n, std::uint64_t windowKey) {
    impl_->history.push(tMs, bins, n, windowKey);
    impl_->lastPushMs = tMs;
}

void View3DPublisher::publish(const Wf3dContext& wf) { impl_->waterfall(wf); }
void View3DPublisher::publishRadar(const Radar3dFeed& feed, std::int64_t nowMs) { impl_->radarPublish(feed, nowMs); }
void View3DPublisher::publishTunnel(const TunnelFeed& feed, std::int64_t nowMs) { impl_->tunnelPublish(feed, nowMs); }
void View3DPublisher::publishSurvey(std::int64_t nowMs) { impl_->surveyPublish(nowMs); }

int View3DPublisher::detaches() const { return impl_->surface.detaches() + impl_->radarSurface.detaches() + impl_->tunnelSurface.detaches() + impl_->surveySurface.detaches(); }

std::string View3DPublisher::readState(const std::string& what, std::string& out) const {
    const Impl& m = *impl_;
    char b[64];
    const auto num = [&](double v) { std::snprintf(b, sizeof(b), "%.2f", v); out = b; return std::string(); };
    if (what == "wf.on") { out = m.wf.on ? "1" : "0"; return std::string(); }
    if (what == "wf.azimuth") { return num(m.wf.azimuth); }
    if (what == "wf.elevation") { return num(m.wf.elevation); }
    if (what == "wf.zoom") { return num(m.wf.zoom); }
    if (what == "wf.depth") { out = std::to_string(m.wf.opt[0]); return std::string(); }
    if (what == "wf.shading") { out = std::to_string(m.wf.opt[1]); return std::string(); }
    if (what == "wf.renders") { out = std::to_string(m.renders); return std::string(); }
    if (what == "wf.triangles") { out = std::to_string(m.lastTriangles); return std::string(); }
    if (what == "wf.pixels") { out = std::to_string(m.lastPixels); return std::string(); }
    if (what == "wf.labels") { out = std::to_string(m.lastLabels); return std::string(); }
    if (what == "wf.span") { return num(m.history.spanSeconds(m.lastNowMs)); }
    if (what == "wf.realrows") { out = std::to_string(m.lastRealRows); return std::string(); }
    if (what == "wf.rows") { out = std::to_string(m.history.size()); return std::string(); }
    if (what == "wf.data") { out = m.hasData ? "1" : "0"; return std::string(); }
    if (what == "wf.w") { out = std::to_string(m.renderW); return std::string(); }
    if (what == "wf.h") { out = std::to_string(m.renderH); return std::string(); }
    if (what == "survey.on") { out = m.survey.on ? "1" : "0"; return std::string(); }
    if (what == "survey.azimuth") { return num(m.survey.azimuth); }
    if (what == "survey.elevation") { return num(m.survey.elevation); }
    if (what == "survey.zoom") { return num(m.survey.zoom); }
    if (what == "survey.renders") { out = std::to_string(m.surveyRenders); return std::string(); }
    if (what == "survey.channels") { out = std::to_string(m.surveyChannels); return std::string(); }
    if (what == "survey.triangles") { out = std::to_string(m.surveyTriangles); return std::string(); }
    if (what == "survey.pixels") { out = std::to_string(m.surveyPixels); return std::string(); }
    if (what == "survey.ranked") { out = std::to_string(m.surveyRanked); return std::string(); }
    if (what == "survey.labels") { out = std::to_string(m.surveyLabels); return std::string(); }
    if (what == "survey.first") { out = m.surveyFirstRow; return std::string(); }
    if (what == "survey.w") { out = std::to_string(m.surveyW); return std::string(); }
    if (what == "survey.h") { out = std::to_string(m.surveyH); return std::string(); }
    if (what == "tunnel.on") { out = m.tunnel.on ? "1" : "0"; return std::string(); }
    if (what == "tunnel.azimuth") { return num(m.tunnel.azimuth); }
    if (what == "tunnel.elevation") { return num(m.tunnel.elevation); }
    if (what == "tunnel.zoom") { return num(m.tunnel.zoom); }
    if (what == "tunnel.depth") { out = std::to_string(m.tunnel.opt[0]); return std::string(); }
    if (what == "tunnel.depthms") { return num(m.tunnelDepthMs); }
    if (what == "tunnel.colour") { out = std::to_string(m.tunnel.opt[1]); return std::string(); }
    if (what == "tunnel.renders") { out = std::to_string(m.tunnelRenders); return std::string(); }
    if (what == "tunnel.points") { out = std::to_string(m.tunnelPoints); return std::string(); }
    if (what == "tunnel.pixels") { out = std::to_string(m.tunnelPixels); return std::string(); }
    if (what == "tunnel.labels") { out = std::to_string(m.tunnelLabels); return std::string(); }
    if (what == "tunnel.w") { out = std::to_string(m.tunnelW); return std::string(); }
    if (what == "tunnel.h") { out = std::to_string(m.tunnelH); return std::string(); }
    if (what == "radar.on") { out = m.radar.on ? "1" : "0"; return std::string(); }
    if (what == "radar.azimuth") { return num(m.radar.azimuth); }
    if (what == "radar.tilt" || what == "radar.elevation") { return num(m.radar.elevation); }
    if (what == "radar.zoom") { return num(m.radar.zoom); }
    if (what == "radar.scale") { out = std::to_string(m.radar.opt[0]); return std::string(); }
    if (what == "radar.renders") { out = std::to_string(m.radarRenders); return std::string(); }
    if (what == "radar.aircraft") { out = std::to_string(m.radarAircraft); return std::string(); }
    if (what == "radar.onground") { out = std::to_string(m.radarOnGround); return std::string(); }
    if (what == "radar.trails") { out = std::to_string(m.radarTrailSegs); return std::string(); }
    if (what == "radar.pixels") { out = std::to_string(m.radarPixels); return std::string(); }
    if (what == "radar.tags") { out = std::to_string(m.radarTags); return std::string(); }
    if (what == "radar.labels") { out = std::to_string(m.radarLabels); return std::string(); }
    if (what == "radar.w") { out = std::to_string(m.radarW); return std::string(); }
    if (what == "radar.h") { out = std::to_string(m.radarH); return std::string(); }
    return "view3d.wf.on|azimuth|elevation|zoom|depth|shading|renders|triangles|pixels|labels|span|realrows|rows|data|w|h, or view3d.radar.on|azimuth|tilt|zoom|scale|renders|aircraft|onground|trails|pixels|labels|tags|w|h, or view3d.tunnel.on|azimuth|elevation|zoom|depth|depthms|colour|renders|points|pixels|labels|w|h, or view3d.survey.on|azimuth|elevation|zoom|renders|channels|triangles|pixels|ranked|labels|first|w|h, not view3d." + what;
}

std::string View3DPublisher::summaryLine() const {
    const Impl& m = *impl_;
    char b[1200];
    std::snprintf(b, sizeof(b), "view3d: waterfall on=%d, %zu renders, last %.2f ms, worst %.2f ms, %zu triangles, %zu rows kept, %zu saves, %d buffer detaches; radar on=%d, %zu renders, last %.2f ms, worst %.2f ms, %zu aircraft (%zu without altitude), %zu trail segments, %d buffer detaches; tunnel on=%d, %zu renders, last %.2f ms, worst %.2f ms, %zu points, %d buffer detaches; survey on=%d, %zu renders, last %.2f ms, worst %.2f ms, %zu channels, %zu triangles, %d buffer detaches", m.wf.on ? 1 : 0, m.renders, m.lastRenderMs,
                  m.worstRenderMs, m.lastTriangles, m.history.size(), m.saves, m.surface.detaches(), m.radar.on ? 1 : 0, m.radarRenders, m.radarLastMs, m.radarWorstMs, m.radarAircraft, m.radarOnGround, m.radarTrailSegs, m.radarSurface.detaches(), m.tunnel.on ? 1 : 0, m.tunnelRenders, m.tunnelLastMs, m.tunnelWorstMs, m.tunnelPoints, m.tunnelSurface.detaches(), m.survey.on ? 1 : 0, m.surveyRenders, m.surveyLastMs, m.surveyWorstMs, m.surveyChannels, m.surveyTriangles, m.surveySurface.detaches());
    return b;
}

}  // namespace foxsdr::slintview3d
