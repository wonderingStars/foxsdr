// plugin_kit.cpp - the plugin kit's host side (plugin_kit.hpp): the port, the duties, the commands.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "plugin_kit.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "core/diag_log.hpp"
#include "core/image_write.hpp"
#include "core/plugin_calls.hpp"

namespace foxsdr::slintkit {

namespace {
constexpr std::size_t kPollBufBytes = 4096;
constexpr std::size_t kMaxPollsPerDrain = 32;
constexpr std::size_t kMaxTilesPerTick = 16;
constexpr int kTrackInfoLinesPerTick = 8;
constexpr std::size_t kTrackInfoBacklog = 500;
constexpr std::size_t kDecoderRing = 500;

void entry(FoxListItem& it) { it = FoxListItem{}; it.structSize = sizeof(it); }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
BasemapClient::~BasemapClient() { detach(); }

void BasemapClient::attach(const CascadeBasemapApi* api, const std::string& pluginKey) {
    if (api == api_) { return; }
    detach();
    if (api == nullptr) { return; }
    cascade::core::fault::Slot* const slot = cascade::core::fault::slotFor(pluginKey);
    if (!pluginKey.empty() && cascade::core::fault::isQuarantined(slot)) { return; }
    void* h = cascade::core::calls::create(slot, api);
    if (h == nullptr) { return; }
    api_ = api;
    handle_ = h;
    slot_ = slot;
    attribution_ = api->attribution != nullptr ? api->attribution : "";
    minZoom_ = api->minZoom;
    maxZoom_ = api->maxZoom;
    tileSize_ = api->tileSize;
    pollBuf_.assign(kPollBufBytes, '\0');
}

void BasemapClient::detach() {
    if (api_ != nullptr && handle_ != nullptr) { cascade::core::calls::destroy(slot_, api_, handle_); }
    api_ = nullptr;
    handle_ = nullptr;
    slot_ = nullptr;
    attribution_.clear();
    partial_.clear();
}

std::int32_t BasemapClient::rawTile(std::uint32_t z, std::uint32_t x, std::uint32_t y, std::vector<std::uint8_t>& rgb) {
    rgb.clear();
    if (!active()) { return CASCADE_TILE_MISSING; }
    CascadeTile t{};
    t.structSize = static_cast<std::uint32_t>(sizeof(CascadeTile));
    const std::int32_t got = cascade::core::calls::getTile(slot_, api_, handle_, z, x, y, &t);
    if (got == CASCADE_TILE_PENDING) { return CASCADE_TILE_PENDING; }
    if (got != CASCADE_TILE_READY) { return CASCADE_TILE_MISSING; }
    const std::size_t rowBytes = static_cast<std::size_t>(t.width) * 3u;
    const bool sane = t.pixels != nullptr && t.width == tileSize_ && t.height == tileSize_ && t.format == CASCADE_IMAGE_RGB24 &&
                      t.stride >= rowBytes && t.width <= CASCADE_TILE_SIZE_MAX;
    if (!sane) { cascade::core::fault::noteMisuse(slot_, cascade::core::fault::Misuse::BadReturn); }
    bool copied = false;
    if (sane) {
        rgb.resize(rowBytes * t.height);
        copied = cascade::core::fault::copyRowsFromPlugin(slot_, cascade::core::fault::Entry::GetTile, rgb.data(), rowBytes, t.pixels, t.stride, t.height);
        if (!copied) { rgb.clear(); }
    }
    cascade::core::calls::releaseTile(slot_, api_, handle_, &t);
    return copied ? CASCADE_TILE_READY : CASCADE_TILE_MISSING;
}

std::vector<std::string> BasemapClient::drainText() {
    std::vector<std::string> out;
    if (!active()) { return out; }
    for (std::size_t polls = 0; polls < kMaxPollsPerDrain; ++polls) {
        const std::int32_t n = cascade::core::calls::pollText(slot_, api_, handle_, pollBuf_.data(), pollBuf_.size());
        if (n <= 0) { break; }
        partial_.append(pollBuf_.data(), std::min(static_cast<std::size_t>(n), pollBuf_.size()));
        for (;;) {
            const std::size_t nl = partial_.find('\n');
            if (nl == std::string::npos) { break; }
            if (nl > 0) { out.push_back(partial_.substr(0, nl)); }
            partial_.erase(0, nl + 1);
        }
        if (partial_.size() > 4096) { out.push_back(partial_); partial_.clear(); }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------
std::int32_t LiveTable::readList(std::uint32_t list, std::uint32_t index, FoxListItem* out) {
    if (out != nullptr) { entry(*out); }
    return api_->read_list(session_, list, index, out);
}

bool LiveTable::fact(const std::string& key, std::string& out) {
    out.clear();
    char buf[512];
    std::int32_t n = api_->get_setting(session_, key.c_str(), buf, sizeof(buf));
    if (n >= static_cast<std::int32_t>(sizeof(buf))) {
        out.resize(static_cast<std::size_t>(n) + 1u);
        n = api_->get_setting(session_, key.c_str(), out.data(), out.size());
        out.resize(n >= 0 ? std::min(static_cast<std::size_t>(n), out.size() - 1u) : 0u);
    } else if (n >= 0) {
        out.assign(buf, static_cast<std::size_t>(n));
    }
    return n >= 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------
PluginKit::PluginKit(const Env& env) : env_(env), table_(env.api, env.session), port_(cascade::core::makeNullPluginUiPort()) {
    state_.structSize = sizeof(state_);
}

PluginKit::~PluginKit() {
    // The port's destructor disconnects; the basemap and track-info clients are detached first, on this thread, so nothing points into a module.
    basemap_.detach();
    trackInfo_.detach();
    port_.reset();
}

bool PluginKit::attach() {
    if (env_.api == nullptr || env_.engine == nullptr || env_.session == nullptr) { return false; }
    if (!FOXAPI_HAS(env_.api, get_local_extension)) {
        cascade::core::diagWarnf("slint kit: no plugin UI extension on this engine - there are no plugin windows");
        return false;
    }
    cascade::core::PluginUiAttach attach{};
    attach.structSize = sizeof(attach);
    attach.session = env_.session;
    attach.steps.before = [this](const cascade::core::PluginDirectiveFacts& d) { directiveBefore(d); };
    attach.steps.after = [this](const cascade::core::PluginDirectiveFacts& d) { directiveAfter(d); };
    void* out = nullptr;
    const std::int32_t rc = env_.api->get_local_extension(env_.engine, FOXAPI_EXT_PLUGIN_UI, &attach, &out);
    if (rc != FOXAPI_OK || out == nullptr) {
        cascade::core::diagWarnf("slint kit: the engine refused the plugin UI extension (%d) - there are no plugin windows", static_cast<int>(rc));
        return false;
    }
    port_.reset(static_cast<cascade::core::PluginUiPort*>(out));
    attached_ = true;
    port_->applyNewestDirective();
    return true;
}

void PluginKit::directiveBefore(const cascade::core::PluginDirectiveFacts& d) {
    (void)d;   // the map pages' geometry is not saved in this phase: nothing to fold before a release
}

void PluginKit::directiveAfter(const cascade::core::PluginDirectiveFacts& d) {
    if (d.release) {
        basemap_.detach();
        trackInfo_.detach();
        return;
    }
    basemap_.attach(d.basemap, d.basemapPlugin);
    trackInfo_.attach(d.trackInfo, d.trackInfoPlugin);
    if (d.lifeOnly) { return; }
    // DEMONSTRATION instruments (FOXSDR_DEMO_INSTRUMENT) open their windows by themselves: the one exception to "nothing opens but by your hand".
    for (const cascade::core::HostInstrument& in : port_->instruments()) {
        if (in.plugin == "Demonstration") { open_.windows.insert(windowKey("instrument", in.plugin, in.title)); }
    }
}

void PluginKit::release() {
    if (!attached_) { return; }
    port_->release();
    basemap_.detach();
    trackInfo_.detach();
}

// ---------------------------------------------------------------------------------------------------------------------------------
void PluginKit::tick(const FoxReceiverState& state, bool listSeqMoved, const std::string& tunerFace) {
    state_ = state;
    port_->applyNewestDirective();
    port_->poll();
    port_->pollImages(images_);
    if (listSeqMoved || lists_.rows.empty()) {
        ReadOptions o;
        o.fittedFacts = true;
        o.settings = true;
        o.blocked = true;
        readKitLists(table_, lists_, o);
    }
    if (state.decoderLinesTotal != linesSeenTotal_) {
        linesSeenTotal_ = state.decoderLinesTotal;
        readDecoderLinesTail(table_, linesLastId_, lines_, kDecoderRing);
    }
    syncMapPages(pages_, port_->trackPluginNames(), lists_.rows);
    // the visible counts, the satellite stickiness and the coverage, from this poll's tracks
    for (MapPage& pg : pages_) { pg.visibleCount = 0; pg.unplacedCount = 0; }
    const bool home = (state.flags & FOXAPI_RX_POSITION_SET) != 0u;
    for (const cascade::core::HostTrack& t : port_->tracks()) {
        MapPage* pg = findMapPage(pages_, t.plugin);
        const cascade::core::TrackPresentation p = cascade::core::trackPresentation(t.t);
        if (pg != nullptr) {
            if (!pg->satellite && t.t.kind == CASCADE_TRACK_SATELLITE) {
                bool every = true;
                for (const cascade::core::HostTrack& u : port_->tracks()) { if (u.plugin == t.plugin && u.t.kind != CASCADE_TRACK_SATELLITE) { every = false; break; } }
                pg->satellite = every;
            }
            if (p.visible && cascade::core::trackHasPosition(t.t)) { ++pg->visibleCount; }
        }
        if (home && p.visible && cascade::core::trackHasPosition(t.t)) {
            const cascade::gui::TrackKind kind = t.t.kind == CASCADE_TRACK_SATELLITE ? cascade::gui::TrackKind::Satellite
                                               : t.t.kind == CASCADE_TRACK_VESSEL ? cascade::gui::TrackKind::Vessel
                                               : t.t.kind == CASCADE_TRACK_AIRCRAFT ? cascade::gui::TrackKind::Aircraft : cascade::gui::TrackKind::Other;
            cascade::gui::recordCoverage(coverage_, state.rxLatDeg, state.rxLonDeg, t.t.latDeg, t.t.lonDeg, kind);
        }
    }
    for (const cascade::core::HostTrack& t : port_->unplacedTracks()) {
        MapPage* pg = findMapPage(pages_, t.plugin);
        if (pg != nullptr && cascade::core::trackPresentation(t.t).visible) { ++pg->unplacedCount; }
    }
    handFrontEnd(tunerFace);
    serveWebTiles();
    sendTrackInfoLines();
    reportPluginUse();
    pollEvents();
    pollResults();
    edits_.tick();
    for (std::string& line : basemap_.drainText()) { events_.notices.push_back("basemap: " + line); }
}

bool PluginKit::fillTrackFeed(cascade::core::TrackFeed& out) {
    out.tracks.clear();
    out.polled.clear();
    const std::vector<cascade::core::HostTrack>& tracks = port_->tracks();
    if (tracks.empty()) { return false; }
    for (const cascade::core::HostTrack& t : tracks) {
        if (cascade::core::trackPresentation(t.t).visible) { out.polled.push_back(t); }
    }
    if (trackInfo_.active()) {
        for (const cascade::core::HostTrack& t : tracks) {
            if (!cascade::core::trackPresentation(t.t).visible) { continue; }
            const std::string id = t.t.id;
            const cascade::gui::TrackInfoCache::Info* d = trackInfo_.get(id, t.t.kind);
            if (d == nullptr) { continue; }
            cascade::core::TrackFeedInfo w;
            w.id = id;
            w.kind = t.t.kind;
            w.infoState = d->known ? 1u : 2u;
            if (d->known) {
                w.registration = d->registration;
                w.acType = !d->typeName.empty() ? d->typeName : d->typeCode;
                w.acOperator = d->operatorName;
                w.acCountry = d->country;
            }
            out.tracks.push_back(std::move(w));
        }
        std::sort(out.tracks.begin(), out.tracks.end(), [](const cascade::core::TrackFeedInfo& a, const cascade::core::TrackFeedInfo& b) {
            return a.id != b.id ? a.id < b.id : a.kind < b.kind;
        });
    }
    return !(out.tracks.empty() && out.polled.empty());
}

void PluginKit::handFrontEnd(const std::string& tunerFace) {
    cascade::core::PluginUiFrontEnd& f = frontEnd_;
    f.tunerDisplayStyle = tunerFace;
    f.basemap.active = basemap_.active();
    f.basemap.minZoom = basemap_.minZoom();
    f.basemap.maxZoom = basemap_.maxZoom();
    f.basemap.tileSize = basemap_.tileSize();
    f.basemap.attribution = basemap_.attribution();
    f.hasTrackFeed = fillTrackFeed(f.trackFeed);
    f.pluginStatusReport = false;
    port_->setFrontEnd(f);
}

void PluginKit::serveWebTiles() {
    const bool active = basemap_.active();
    bool clearFirst = false;
    if (active != webTilesActive_ || (active && basemap_.attribution() != webTileAttribution_)) {
        clearFirst = true;
        webTilesActive_ = active;
        webTileAttribution_ = active ? basemap_.attribution() : std::string();
    }
    const bool listening = (state_.flags & FOXAPI_RX_WEB_LISTENING) != 0u;
    port_->serveTiles(clearFirst, active && listening, kMaxTilesPerTick, [this](const cascade::core::WebTileRequest& r, std::vector<std::uint8_t>& bmp) {
        std::vector<std::uint8_t> rgb;
        const std::int32_t got = basemap_.rawTile(r.z, r.x, r.y, rgb);
        if (got == CASCADE_TILE_PENDING) { return cascade::core::WebTileAnswer::Pending; }
        if (got != CASCADE_TILE_READY) { return cascade::core::WebTileAnswer::Missing; }
        cascade::core::HostImage img;
        img.width = basemap_.tileSize();
        img.height = basemap_.tileSize();
        img.format = CASCADE_IMAGE_RGB24;
        img.pixels = std::move(rgb);
        std::string err;
        if (!cascade::core::encodeBmp24(img, bmp, err)) { bmp.clear(); return cascade::core::WebTileAnswer::Missing; }
        return cascade::core::WebTileAnswer::Ready;
    });
}

void PluginKit::sendTrackInfoLines() {
    if (!trackInfo_.active()) { trackInfoLines_.clear(); return; }
    for (std::string& line : trackInfo_.drainText()) {
        if (trackInfoLines_.size() >= kTrackInfoBacklog) {
            trackInfoLines_.pop_front();
            if (trackInfoLinesLost_++ == 0) { cascade::core::diagWarnf("slint kit: the track-info plugin is saying lines faster than the window passes them on - the oldest are dropped"); }
        }
        trackInfoLines_.push_back(std::move(line));
    }
    for (int sent = 0; sent < kTrackInfoLinesPerTick && !trackInfoLines_.empty(); ++sent) {
        if (submit(cascade::core::cmd::makeText(FOXAPP_OP_TRACK_INFO_LINE, trackInfoLines_.front())) == 0u) { break; }
        trackInfoLines_.pop_front();
    }
}

void PluginKit::reportPluginUse() {
    const std::string text = pluginUseText(lists_.rows, open_);
    if (text == useSent_) { return; }
    // Recorded as sent only when the session took it (docs 6.32, finding 6): a BUSY answer is offered again next tick.
    if (submit(cascade::core::cmd::makeText(FOXAPP_OP_PLUGIN_USE, text)) == 0u) { return; }
    useSent_ = text;
}

void PluginKit::pollEvents() {
    FoxEvent ev[16];
    for (auto& e : ev) { e.structSize = sizeof(e); }
    const std::int32_t got = env_.api->poll_events(env_.session, ev, 16);
    for (std::int32_t i = 0; i < got; ++i) {
        if (ev[i].kind == FOXAPI_EVENT_PRESET_APPLIED) { events_.presetApplied.emplace_back(ev[i].text); }
        else if (ev[i].kind == FOXAPI_EVENT_MUTE_TUNE_AWAY) { events_.muteTuneAway = true; }
    }
}

void PluginKit::pollResults() {
    FoxCommandResult r[32];
    for (auto& x : r) { x.structSize = sizeof(x); }
    const std::int32_t got = env_.api->poll_results(env_.session, r, 32);
    for (std::int32_t i = 0; i < got; ++i) {
        if (r[i].op == FOXAPP_OP_PLUGIN_SETTING_EDIT) {
            (void)edits_.deliver(r[i].ticket, verdictFromAnswer(r[i].status, r[i].applied[0]));
            continue;
        }
        if (r[i].status != FOXAPI_OK && r[i].status != FOXAPI_NO_CHANGE) {
            std::fprintf(stderr, "foxsdr-slint: op 0x%04x answered %d: %s\n", r[i].op, r[i].status, r[i].message);
            events_.notices.push_back(std::string("refused: ") + r[i].message);
        }
    }
}

KitEvents PluginKit::takeEvents() {
    KitEvents e = std::move(events_);
    events_ = KitEvents{};
    return e;
}

std::uint64_t PluginKit::instrumentSeen(const std::string& key) const {
    const auto it = instrumentSeen_.find(key);
    return it == instrumentSeen_.end() ? ~std::uint64_t{0} : it->second;
}
void PluginKit::markInstrumentSeen(const std::string& key, std::uint32_t seq) { instrumentSeen_[key] = seq; }

const cascade::core::HostImage* PluginKit::imageOf(const std::string& plugin) const {
    for (const auto& img : images_) { if (img.plugin == plugin) { return &img; } }
    return nullptr;
}
const cascade::core::HostPanel* PluginKit::panelOf(const std::string& plugin, const std::string& title) const {
    for (const auto& p : port_->panels()) { if (p.plugin == plugin && (title.empty() || p.title == title)) { return &p; } }
    return nullptr;
}
const cascade::core::HostInstrument* PluginKit::instrumentOf(const std::string& plugin, const std::string& title) const {
    for (const auto& in : port_->instruments()) { if (in.plugin == plugin && (title.empty() || in.title == title)) { return &in; } }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------------------
std::uint64_t PluginKit::submit(const cascade::core::cmd::QueuedCommand& q) {
    FoxCommand c = q.c;
    c.structSize = sizeof(c);
    FoxSubmitResult r{};
    r.structSize = sizeof(r);
    std::int32_t rc;
    if (!q.longText.empty() && FOXAPI_HAS(env_.api, submit_text)) {
        rc = env_.api->submit_text(env_.session, &c, q.longText.data(), static_cast<std::uint32_t>(q.longText.size()), &r);
    } else {
        rc = env_.api->submit(env_.session, &c, 1, &r);
    }
    if (rc < 0) {
        std::fprintf(stderr, "foxsdr-slint: submit op 0x%04x refused by the call: %d\n", c.op, rc);
        return 0u;
    }
    if (r.status == FOXAPI_BUSY) {
        if (busyDrops_++ == 0) { std::fprintf(stderr, "foxsdr-slint: the session's queue is full - op 0x%04x dropped; later ones are counted\n", c.op); }
        return 0u;
    }
    return r.ticket;
}

void PluginKit::startStop(const std::string& file, bool stop) { submit(cascade::core::cmd::makeText(stop ? FOXAPI_OP_DECODER_STOP : FOXAPI_OP_DECODER_START, file)); }
void PluginKit::stopList(const std::vector<std::string>& files, bool endMute) {
    std::string keys;
    for (const std::string& f : files) { keys += (keys.empty() ? "" : "\n") + f; }
    if (!keys.empty()) { submit(cascade::core::cmd::makeText(FOXAPP_OP_DECODER_STOP_LIST, keys, endMute ? 1 : 0)); }
}
void PluginKit::applyPreset(const std::string& file, const Preset& p) {
    if (p.mine) { submit(cascade::core::cmd::makeText(FOXAPP_OP_USER_PRESET_APPLY, file, static_cast<std::int64_t>(p.index - 1000u))); }
    else { submit(cascade::core::cmd::makeText(FOXAPI_OP_PLUGIN_PRESET, file, static_cast<std::int64_t>(p.index))); }
}
void PluginKit::forgetPreset(const std::string& file, const Preset& p) {
    if (p.mine) { submit(cascade::core::cmd::makeText(FOXAPP_OP_USER_PRESET_FORGET_AT, file, static_cast<std::int64_t>(p.index - 1000u))); }
}
void PluginKit::savePreset(const std::string& file) { submit(cascade::core::cmd::makeText(FOXAPI_OP_USER_PRESET_SAVE, file)); }
void PluginKit::setMute(const std::string& file, bool mutes) { submit(cascade::core::cmd::makeText(FOXAPI_OP_PLUGIN_MUTE, file, mutes ? 1 : 0)); }
void PluginKit::setGrant(const std::string& file, int kind, bool on) {
    if (kind == 3) { submit(cascade::core::cmd::makeText(FOXAPP_OP_PLUGIN_TX_GRANT, file, on ? 1 : 0)); }
    else { submit(cascade::core::cmd::makeText(FOXAPI_OP_PLUGIN_GRANT, file, kind, on ? 1 : 0)); }
}
void PluginKit::pressCommand(const std::string& file, std::uint32_t id) { submit(cascade::core::cmd::makeText(FOXAPI_OP_PLUGIN_COMMAND, file, static_cast<std::int64_t>(id))); }
void PluginKit::setRunMode(const std::string& file, int mode) { submit(cascade::core::cmd::makeText(FOXAPP_OP_PLUGIN_RUN_MODE, file, mode)); }
void PluginKit::reenable(const std::string& file) { submit(cascade::core::cmd::makeText(FOXAPP_OP_PLUGIN_REENABLE, file)); }
void PluginKit::rescan() { submit(cascade::core::cmd::make(FOXAPI_OP_PLUGIN_RESCAN)); }
void PluginKit::removeBlocked(const std::string& file) { submit(cascade::core::cmd::makeText(FOXAPP_OP_STORE_REMOVE_BLOCKED, file)); }
void PluginKit::updateBlocked(const std::string& catalogueId) { submit(cascade::core::cmd::makeText(FOXAPP_OP_STORE_UPDATE, catalogueId)); }
void PluginKit::autoPreset(const std::string& file) { if (!file.empty()) { submit(cascade::core::cmd::makeText(FOXAPP_OP_PLUGIN_AUTO_PRESET, file, 1)); } }
void PluginKit::imagePress(const std::string& plugin, int x, int y) {
    if (x < 0 || y < 0) { return; }
    submit(cascade::core::cmd::makeText(FOXAPP_OP_IMAGE_PRESS, plugin, x, y));
}
void PluginKit::keepMuteRunning() { submit(cascade::core::cmd::make(FOXAPP_OP_MUTE_KEEP_RUNNING)); }
void PluginKit::setCatalogueUrl(const std::string& url) { submit(cascade::core::cmd::makeText(FOXAPP_OP_SET_CATALOGUE_URL, url)); }

void PluginKit::editSetting(const std::string& file, const SettingSpec& f, const std::string& typed) {
    const std::string text = file + '\x1F' + f.key + '\x1F' + typed;
    std::string expect = typed;
    if (f.kind == CASCADE_SETTING_BOOL) { expect = typed == "1" ? "1" : ""; }
    const std::uint64_t ticket = submit(cascade::core::cmd::makeText(FOXAPP_OP_PLUGIN_SETTING_EDIT, text));
    if (ticket == 0u) { edits_.refusedAtOnce(file, f.key); }
    else { edits_.sent(file, f.key, ticket, expect); }
}

std::string PluginKit::savePicture(const std::string& plugin) {
    const cascade::core::HostImage* img = imageOf(plugin);
    if (img == nullptr || img->width == 0u) { return "Nothing to save yet."; }
    std::filesystem::path dir = std::filesystem::path(env_.configPath).parent_path() / "pictures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string name = plugin;
    for (char& ch : name) { if (!std::isalnum(static_cast<unsigned char>(ch))) { ch = '_'; } }
    const std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
    const std::string path = (dir / (name + "-" + stamp + ".bmp")).string();
    std::string err;
    if (!cascade::core::writeBmp24(*img, path, err)) { return err.empty() ? "The picture could not be saved." : err; }
    return "Saved " + path;
}

}  // namespace foxsdr::slintkit
