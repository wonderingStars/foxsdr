// Implementation of core/plugin_api.hpp. See that header for the threading
// rule every function here is written to.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_api.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/utf8_text.hpp"
// The web API's own bounds, reused rather than restated: a value a browser is
// refused is refused to a plugin too, for the same stated reason, and the two
// can never drift apart.
#include "net/web_control.hpp"

namespace cascade::core {

namespace {

thread_local int t_realtimeDepth = 0;

bool finite(double v) { return std::isfinite(v); }

// Copies at most cap-1 bytes of NUL-terminated `src` into `dst`, cut back to a
// whole UTF-8 character, and terminates. `src` is third-party memory: it is
// read no further than cap-1 bytes, so an unterminated buffer cannot walk the
// host off the end of it.
void copyBoundedUtf8(char* dst, std::size_t cap, const char* src) {
    if (cap == 0) { return; }
    std::size_t n = 0;
    if (src != nullptr) {
        while (n + 1 < cap && src[n] != '\0') {
            dst[n] = src[n];
            ++n;
        }
    }
    n = utf8Floor(dst, n);
    dst[n] = '\0';
}

// Whole-string UTF-8 check: every lead byte followed by the right number of
// continuation bytes, no overlong 2-byte form, nothing above U+10FFFF. A
// settings value goes into a JSON config file, and JSON is UTF-8 by
// definition - refusing here is better than having the writer mangle it later.
bool validUtf8(const char* s, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (c < 0x80u) {
            len = 1;
        } else if (c >= 0xC2u && c < 0xE0u) {
            len = 2;
        } else if (c >= 0xE0u && c < 0xF0u) {
            len = 3;
        } else if (c >= 0xF0u && c < 0xF5u) {
            len = 4;
        } else {
            return false;
        }
        if (i + len > n) { return false; }
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u) { return false; }
        }
        i += len;
    }
    return true;
}

// strnlen without relying on the POSIX spelling.
std::size_t boundedLen(const char* s, std::size_t cap) {
    std::size_t n = 0;
    while (n < cap && s[n] != '\0') { ++n; }
    return n;
}

bool contains(const std::vector<std::string>& v, const std::string& k) {
    return !k.empty() && std::find(v.begin(), v.end(), k) != v.end();
}

void setMember(std::vector<std::string>& v, const std::string& k, bool on) {
    if (k.empty()) { return; }
    const auto it = std::find(v.begin(), v.end(), k);
    if (on && it == v.end()) {
        v.push_back(k);
    } else if (!on && it != v.end()) {
        v.erase(it);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Real-time marker
// ---------------------------------------------------------------------------

RealtimeThreadScope::RealtimeThreadScope() { ++t_realtimeDepth; }
RealtimeThreadScope::~RealtimeThreadScope() { --t_realtimeDepth; }
bool onRealtimeThread() { return t_realtimeDepth > 0; }

// ---------------------------------------------------------------------------
// Stream clock
// ---------------------------------------------------------------------------

void StreamClock::beginEpoch(double iqRateHz, double audioRateHz, std::int64_t nowUnixMs) {
    writer_.epoch += 1;
    writer_.epochStartUnixMs = nowUnixMs;
    writer_.iqRateHz = iqRateHz;
    writer_.audioRateHz = audioRateHz;
    writer_.iqFrames = 0;
    writer_.audioFrames = 0;
    box_.store(writer_);
}

void StreamClock::addIq(std::uint64_t frames) {
    writer_.iqFrames += frames;
    box_.store(writer_);
}

void StreamClock::addAudio(std::uint64_t frames) {
    writer_.audioFrames += frames;
    box_.store(writer_);
}

// ---------------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------------

bool controlNeedsTuneGrant(PluginControl::Kind k) {
    return k == PluginControl::Kind::Frequency || k == PluginControl::Kind::VfoOffset;
}

bool validSettingKey(const char* key) {
    if (key == nullptr) { return false; }
    std::size_t n = 0;
    for (; n < CASCADE_SETTING_KEY_CHARS && key[n] != '\0'; ++n) {
        const char ch = key[n];
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) { return false; }
    }
    // 1..63: a key that fills all 64 bytes has no room for its terminator.
    return n > 0 && n < CASCADE_SETTING_KEY_CHARS;
}

PluginSettingsMap sanitisePluginSettings(const PluginSettingsMap& in) {
    PluginSettingsMap out;
    for (const auto& [plugin, kv] : in) {
        if (out.size() >= kMaxSettingsPlugins) { break; }
        if (plugin.empty() || plugin.size() > 256u) { continue; }
        std::map<std::string, std::string> clean;
        for (const auto& [k, v] : kv) {
            if (clean.size() >= CASCADE_MAX_SETTINGS_PER_PLUGIN) { break; }
            if (!validSettingKey(k.c_str()) || k.size() != std::strlen(k.c_str())) { continue; }
            if (v.size() >= CASCADE_SETTING_VALUE_BYTES) { continue; }
            if (v.find('\0') != std::string::npos) { continue; }
            if (!validUtf8(v.data(), v.size())) { continue; }
            clean.emplace(k, v);
        }
        if (!clean.empty()) { out.emplace(plugin, std::move(clean)); }
    }
    return out;
}

// ---------------------------------------------------------------------------
// PluginApiCore - host side
// ---------------------------------------------------------------------------

PluginApiCore::PluginApiCore() : clock_(std::make_shared<StreamClock>()) {}

PluginApiClient& PluginApiCore::client(const std::string& key, const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->key == key) {
            // THE NAME IS NEVER REWRITTEN, though a different build could in
            // principle arrive under the same file name with another one: a
            // plugin thread reads `name` without a lock (the settings store is
            // keyed on it), so it has to be immutable once published. A build
            // that renames itself under an unchanged file name keeps its old
            // settings bucket until the next launch, which is harmless.
            (void)name;
            return *c;
        }
    }
    auto c = std::make_unique<PluginApiClient>();
    c->key = key;
    c->name = name;
    c->index = clients_.size();
    ClientState st;
    st.markers.reserve(CASCADE_MAX_MARKERS_PER_PLUGIN);
    state_.push_back(std::move(st));
    refreshGrantsLocked(*c);
    clients_.push_back(std::move(c));
    return *clients_.back();
}

std::size_t PluginApiCore::clientCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return clients_.size();
}

void PluginApiCore::clearClientLocked(std::size_t index) {
    ClientState& st = state_[index];
    if (!st.markers.empty()) {
        st.markers.clear();
        ++markersSeq_;
    }
    st.commandCount = 0;
    st.pressHead = 0;
    st.pressCount = 0;
    // Queued requests of this client are removed, keeping the rest in order.
    std::array<PluginControl, kControlQueue> keep{};
    std::size_t kept = 0;
    for (std::size_t i = 0; i < controlCount_; ++i) {
        const PluginControl& c = controls_[(controlHead_ + i) % kControlQueue];
        if (c.client != index) { keep[kept++] = c; }
    }
    controls_ = keep;
    controlHead_ = 0;
    controlCount_ = kept;
}

void PluginApiCore::setLiveSet(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        const bool live = contains(keys, c->key);
        const bool was = c->live.exchange(live, std::memory_order_acq_rel);
        if (was && !live) { clearClientLocked(c->index); }
    }
}

void PluginApiCore::setAttached(bool attached) {
    attached_.store(attached, std::memory_order_release);
}

void PluginApiCore::refreshGrantsLocked(PluginApiClient& c) {
    std::uint32_t g = 0;
    if (contains(tuneGranted_, c.key)) { g |= CASCADE_STATE_TUNE_GRANTED; }
    if (contains(settingsGranted_, c.key)) { g |= CASCADE_STATE_SETTINGS_GRANTED; }
    if (contains(stopped_, c.key)) { g |= CASCADE_STATE_STOPPED; }
    c.grants.store(g, std::memory_order_release);
}

void PluginApiCore::setTuneGranted(const std::string& key, bool granted) {
    std::lock_guard<std::mutex> lk(mutex_);
    setMember(tuneGranted_, key, granted);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) { refreshGrantsLocked(*c); }
}

void PluginApiCore::setSettingsGranted(const std::string& key, bool granted) {
    std::lock_guard<std::mutex> lk(mutex_);
    setMember(settingsGranted_, key, granted);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) { refreshGrantsLocked(*c); }
}

void PluginApiCore::setStopped(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lk(mutex_);
    stopped_ = keys;
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        refreshGrantsLocked(*c);
        // A stopped plugin's marks and commands leave the screen at once, not
        // at the next rebuild: stopping is the user saying "not this one".
        if (contains(stopped_, c->key)) { clearClientLocked(c->index); }
    }
}

void PluginApiCore::clearGrants() {
    std::lock_guard<std::mutex> lk(mutex_);
    tuneGranted_.clear();
    settingsGranted_.clear();
    for (const std::unique_ptr<PluginApiClient>& c : clients_) { refreshGrantsLocked(*c); }
}

bool PluginApiCore::settingsGranted(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return contains(settingsGranted_, key);
}

std::vector<std::string> PluginApiCore::settingsRequesters() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> out;
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->askedSettings.load(std::memory_order_acquire)) { out.push_back(c->key); }
    }
    return out;
}

void PluginApiCore::publish(const ReceiverFacts& f) {
    Published& w = writer_;
    const ReceiverFacts& o = w.facts;
    const bool first = !havePublished_;
    const bool tune = first || f.centreHz != o.centreHz || f.vfoOffsetHz != o.vfoOffsetHz;
    const bool mode = first || f.demodMode != o.demodMode || f.bandwidthHz != o.bandwidthHz ||
                      f.squelchDb != o.squelchDb;
    bool gainsMoved = f.gainCount != o.gainCount;
    for (std::uint32_t i = 0; !gainsMoved && i < f.gainCount && i < kMaxPublishedGains; ++i) {
        const PublishedGain& a = f.gains[i];
        const PublishedGain& b = o.gains[i];
        gainsMoved = std::strncmp(a.name, b.name, CASCADE_GAIN_NAME_CHARS) != 0 ||
                     a.unit != b.unit || a.minDb != b.minDb || a.maxDb != b.maxDb ||
                     a.stepDb != b.stepDb || a.currentDb != b.currentDb;
    }
    bool ratesMoved = f.rateCount != o.rateCount;
    for (std::uint32_t i = 0; !ratesMoved && i < f.rateCount && i < kMaxPublishedRates; ++i) {
        ratesMoved = f.rates[i] != o.rates[i];
    }
    gainsMoved = gainsMoved || ratesMoved;
    const bool device = first || f.running != o.running || f.deviceOpen != o.deviceOpen ||
                        f.deviceAgc != o.deviceAgc || f.agcSupported != o.agcSupported ||
                        f.sampleRateHz != o.sampleRateHz ||
                        std::strncmp(f.deviceName, o.deviceName, CASCADE_DEVICE_NAME_CHARS) != 0 ||
                        gainsMoved;
    const bool audio = first || f.volume != o.volume || f.muted != o.muted;
    // The stereo flag is state too, though it belongs to no group of its
    // own: it moves the overall counter only.
    const bool other = first || f.stereo != o.stereo;
    if (tune) { ++w.tuneSeq; }
    if (mode) { ++w.modeSeq; }
    if (device) { ++w.deviceSeq; }
    if (audio) { ++w.audioSeq; }
    if (tune || mode || device || audio || other) { ++w.seq; }
    w.facts = f;
    havePublished_ = true;
    snapshot_.store(w);
}

void PluginApiCore::takeControls(std::vector<PluginControl>& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (std::size_t i = 0; i < controlCount_; ++i) {
        out.push_back(controls_[(controlHead_ + i) % kControlQueue]);
    }
    controlHead_ = 0;
    controlCount_ = 0;
}

void PluginApiCore::takeLog(std::vector<PluginLogLine>& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (std::size_t i = 0; i < logCount_; ++i) {
        const LogEntry& e = log_[(logHead_ + i) % kLogQueue];
        PluginLogLine l;
        if (e.client < clients_.size()) {
            l.key = clients_[e.client]->key;
            l.name = clients_[e.client]->name;
        }
        l.level = e.level;
        l.text = e.text;
        out.push_back(std::move(l));
    }
    logHead_ = 0;
    logCount_ = 0;
}

std::uint64_t PluginApiCore::logDropped() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return logDropped_;
}

bool PluginApiCore::controlStillAllowed(const PluginControl& c) const {
    if (!attached()) { return false; }
    std::lock_guard<std::mutex> lk(mutex_);
    if (c.client >= clients_.size()) { return false; }
    const PluginApiClient& cl = *clients_[c.client];
    if (!cl.live.load(std::memory_order_acquire)) { return false; }
    const std::uint32_t g = cl.grants.load(std::memory_order_acquire);
    if ((g & CASCADE_STATE_STOPPED) != 0u) { return false; }
    const std::uint32_t need = controlNeedsTuneGrant(c.kind) ? CASCADE_STATE_TUNE_GRANTED
                                                             : CASCADE_STATE_SETTINGS_GRANTED;
    return (g & need) != 0u;
}

std::string PluginApiCore::clientKey(std::size_t index) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return index < clients_.size() ? clients_[index]->key : std::string();
}

std::string PluginApiCore::clientName(std::size_t index) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return index < clients_.size() ? clients_[index]->name : std::string();
}

std::uint64_t PluginApiCore::markersSeq() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return markersSeq_;
}

void PluginApiCore::markers(std::vector<HostMarker>& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    out.clear();
    for (std::size_t i = 0; i < clients_.size(); ++i) {
        for (const CascadeMarker& m : state_[i].markers) {
            HostMarker h;
            h.key = clients_[i]->key;
            h.name = clients_[i]->name;
            h.m = m;
            out.push_back(std::move(h));
        }
    }
}

std::vector<HostCommand> PluginApiCore::commands(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<HostCommand> out;
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->key != key) { continue; }
        const ClientState& st = state_[c->index];
        for (std::size_t i = 0; i < st.commandCount; ++i) {
            out.push_back(HostCommand{st.commands[i].id, st.commands[i].label});
        }
        break;
    }
    return out;
}

bool PluginApiCore::pressCommand(const std::string& key, std::uint32_t id) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->key != key) { continue; }
        if (!c->live.load(std::memory_order_acquire)) { return false; }
        ClientState& st = state_[c->index];
        bool known = false;
        for (std::size_t i = 0; i < st.commandCount; ++i) {
            if (st.commands[i].id == id) { known = true; }
        }
        if (!known) { return false; }
        if (st.pressCount == kPressQueue) {
            // Oldest dropped: the newest press is the one the user just made.
            st.pressHead = (st.pressHead + 1) % kPressQueue;
            --st.pressCount;
        }
        st.presses[(st.pressHead + st.pressCount) % kPressQueue] = id;
        ++st.pressCount;
        return true;
    }
    return false;
}

void PluginApiCore::loadSettings(const PluginSettingsMap& settings) {
    std::lock_guard<std::mutex> lk(settingsMutex_);
    settings_ = sanitisePluginSettings(settings);
    // NOT a generation bump: loading is the store being told what is already
    // on disk, and a bump would have the host write it straight back.
}

std::uint64_t PluginApiCore::settingsGeneration() const {
    std::lock_guard<std::mutex> lk(settingsMutex_);
    return settingsGeneration_;
}

PluginSettingsMap PluginApiCore::settingsSnapshot() const {
    std::lock_guard<std::mutex> lk(settingsMutex_);
    return settings_;
}

// ---------------------------------------------------------------------------
// PluginApiCore - plugin side
// ---------------------------------------------------------------------------

std::int32_t PluginApiCore::gate(const PluginApiClient& c) const {
    if (!attached_.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::getState(const PluginApiClient& c,
                                     CascadeReceiverState* out) const {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (out == nullptr || out->structSize < sizeof(CascadeReceiverState)) {
        return CASCADE_API_BAD_ARGUMENT;
    }
    Published p;
    if (!snapshot_.load(p)) { return CASCADE_API_BUSY; }
    const ReceiverFacts& f = p.facts;
    CascadeReceiverState s{};
    std::uint32_t flags = 0;
    if (f.running) { flags |= CASCADE_STATE_RUNNING; }
    if (f.deviceOpen) { flags |= CASCADE_STATE_DEVICE_OPEN; }
    if (f.muted) { flags |= CASCADE_STATE_MUTED; }
    if (f.deviceAgc) { flags |= CASCADE_STATE_DEVICE_AGC; }
    if (f.agcSupported) { flags |= CASCADE_STATE_AGC_SUPPORTED; }
    if (f.signalDb > f.squelchDb) { flags |= CASCADE_STATE_SQUELCH_OPEN; }
    if (f.stereo) { flags |= CASCADE_STATE_STEREO; }
    flags |= c.grants.load(std::memory_order_acquire);
    s.flags = flags;
    s.seq = p.seq;
    s.tuneSeq = p.tuneSeq;
    s.modeSeq = p.modeSeq;
    s.deviceSeq = p.deviceSeq;
    s.audioSeq = p.audioSeq;
    s.centreHz = f.centreHz;
    s.vfoOffsetHz = f.vfoOffsetHz;
    s.tunedHz = f.centreHz + f.vfoOffsetHz;
    s.sampleRateHz = f.sampleRateHz;
    s.bandwidthHz = f.bandwidthHz;
    s.squelchDb = f.squelchDb;
    s.volume = f.volume;
    s.signalDb = f.signalDb;
    // The host's own S-meter mapping (drawRadioSection): [-120, 0] dB.
    s.sMeter = std::clamp((f.signalDb + 120.0) / 120.0, 0.0, 1.0);
    s.demodMode = f.demodMode;
    s.gainCount = std::min<std::uint32_t>(f.gainCount, kMaxPublishedGains);
    std::memcpy(s.deviceName, f.deviceName, sizeof(s.deviceName));
    s.deviceName[CASCADE_DEVICE_NAME_CHARS - 1] = '\0';
    // Only as many bytes as BOTH sides know about - see the header's note on
    // in/out structs. Today they are equal; the min is for the day they are
    // not.
    const std::size_t n = std::min<std::size_t>(out->structSize, sizeof(CascadeReceiverState));
    s.structSize = static_cast<std::uint32_t>(n);
    std::memcpy(out, &s, n);
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::getGain(const PluginApiClient& c, std::uint32_t index,
                                    CascadeGainInfo* out) const {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (out == nullptr || out->structSize < sizeof(CascadeGainInfo)) {
        return CASCADE_API_BAD_ARGUMENT;
    }
    Published p;
    if (!snapshot_.load(p)) { return CASCADE_API_BUSY; }
    if (index >= p.facts.gainCount || index >= kMaxPublishedGains) {
        return CASCADE_API_NOT_FOUND;
    }
    const PublishedGain& src = p.facts.gains[index];
    CascadeGainInfo gi{};
    gi.unit = src.unit;
    std::memcpy(gi.name, src.name, sizeof(gi.name));
    gi.name[CASCADE_GAIN_NAME_CHARS - 1] = '\0';
    gi.minDb = src.minDb;
    gi.maxDb = src.maxDb;
    gi.stepDb = src.stepDb;
    gi.currentDb = src.currentDb;
    const std::size_t n = std::min<std::size_t>(out->structSize, sizeof(CascadeGainInfo));
    gi.structSize = static_cast<std::uint32_t>(n);
    std::memcpy(out, &gi, n);
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::getSampleRates(const PluginApiClient& c, double* out,
                                           std::uint32_t cap) const {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (out == nullptr && cap != 0u) { return CASCADE_API_BAD_ARGUMENT; }
    Published p;
    if (!snapshot_.load(p)) { return CASCADE_API_BUSY; }
    if (!p.facts.deviceOpen) { return 0; }
    const std::uint32_t n =
        std::min<std::uint32_t>(p.facts.rateCount, static_cast<std::uint32_t>(kMaxPublishedRates));
    for (std::uint32_t i = 0; i < n && i < cap; ++i) { out[i] = p.facts.rates[i]; }
    return static_cast<std::int32_t>(n);
}

std::int32_t PluginApiCore::getStreamInfo(const PluginApiClient& c,
                                          CascadeStreamInfo* out) const {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (out == nullptr || out->structSize < sizeof(CascadeStreamInfo)) {
        return CASCADE_API_BAD_ARGUMENT;
    }
    StreamFacts sf;
    if (!clock_->read(sf)) { return CASCADE_API_BUSY; }
    Published p;
    if (!snapshot_.load(p)) { return CASCADE_API_BUSY; }
    CascadeStreamInfo si{};
    si.epoch = sf.epoch;
    si.epochStartUnixMs = sf.epochStartUnixMs;
    si.iqRateHz = sf.iqRateHz;
    si.audioRateHz = sf.audioRateHz;
    si.outputRateHz = p.facts.outputRateHz;
    si.iqFrames = sf.iqFrames;
    si.audioFrames = sf.audioFrames;
    si.outputFrames = p.facts.outputFrames;
    const std::size_t n = std::min<std::size_t>(out->structSize, sizeof(CascadeStreamInfo));
    si.structSize = static_cast<std::uint32_t>(n);
    std::memcpy(out, &si, n);
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::requestControl(PluginApiClient& c, const PluginControl& reqIn) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    PluginControl req = reqIn;
    req.client = c.index;
    const std::uint32_t grants = c.grants.load(std::memory_order_acquire);

    // A STOPPED plugin is refused before it is even recorded as asking: a key
    // inviting the user to grant settings to a plugin they switched off would
    // be an odd thing to offer (the tune path's rule, for the same reason).
    if ((grants & CASCADE_STATE_STOPPED) != 0u) { return CASCADE_API_DENIED; }
    const bool tune = controlNeedsTuneGrant(req.kind);
    if (tune) {
        c.askedTune.store(true, std::memory_order_release);
    } else {
        c.askedSettings.store(true, std::memory_order_release);
    }
    const std::uint32_t need = tune ? CASCADE_STATE_TUNE_GRANTED : CASCADE_STATE_SETTINGS_GRANTED;
    if ((grants & need) == 0u) { return CASCADE_API_DENIED; }

    // The value, against the same bounds the web API applies (net/web_control).
    // NaN and infinity are a malformed ARGUMENT, not a value out of range: no
    // clamp can repair them.
    using K = PluginControl::Kind;
    switch (req.kind) {
        case K::Frequency:
            if (!finite(req.value)) { return CASCADE_API_BAD_ARGUMENT; }
            if (!(req.value > 0.0 && req.value < 1e12)) { return CASCADE_API_OUT_OF_RANGE; }
            break;
        case K::VfoOffset:
            if (!finite(req.value)) { return CASCADE_API_BAD_ARGUMENT; }
            if (!(std::fabs(req.value) < cascade::net::kMaxVfoOffsetHz)) {
                return CASCADE_API_OUT_OF_RANGE;
            }
            break;
        case K::Mode:
            if (req.mode < CASCADE_DEMOD_NFM || req.mode > CASCADE_DEMOD_RAW) {
                return CASCADE_API_OUT_OF_RANGE;
            }
            break;
        case K::Bandwidth:
            if (!finite(req.value)) { return CASCADE_API_BAD_ARGUMENT; }
            if (!(req.value >= cascade::net::kMinBandwidthHz &&
                  req.value <= cascade::net::kMaxBandwidthHz)) {
                return CASCADE_API_OUT_OF_RANGE;
            }
            break;
        case K::Squelch:
            if (!finite(req.value)) { return CASCADE_API_BAD_ARGUMENT; }
            if (!(req.value >= cascade::net::kMinSquelchDb &&
                  req.value <= cascade::net::kMaxSquelchDb)) {
                return CASCADE_API_OUT_OF_RANGE;
            }
            break;
        case K::SampleRate:
            if (!finite(req.value)) { return CASCADE_API_BAD_ARGUMENT; }
            if (!(req.value >= CASCADE_IQ_RATE_MIN_HZ && req.value <= CASCADE_IQ_RATE_MAX_HZ)) {
                return CASCADE_API_OUT_OF_RANGE;
            }
            break;
        case K::Gain:
            if (!finite(req.value)) { return CASCADE_API_BAD_ARGUMENT; }
            if (req.gainName[0] == '\0') { return CASCADE_API_BAD_ARGUMENT; }
            break;
        case K::Volume:
            if (!finite(req.value)) { return CASCADE_API_BAD_ARGUMENT; }
            if (!(req.value >= 0.0 && req.value <= 1.0)) { return CASCADE_API_OUT_OF_RANGE; }
            break;
        case K::DeviceAgc:
        case K::Running:
        case K::Muted:
            break;
    }

    // What needs a radio, against the snapshot the plugin could itself read.
    if (req.kind == K::SampleRate || req.kind == K::Gain || req.kind == K::DeviceAgc) {
        Published p;
        if (!snapshot_.load(p)) { return CASCADE_API_BUSY; }
        if (!p.facts.deviceOpen) { return CASCADE_API_NO_DEVICE; }
        if (req.kind == K::DeviceAgc && !p.facts.agcSupported) {
            return CASCADE_API_UNSUPPORTED;
        }
        if (req.kind == K::Gain) {
            const PublishedGain* found = nullptr;
            for (std::uint32_t i = 0; i < p.facts.gainCount && i < kMaxPublishedGains; ++i) {
                if (std::strncmp(p.facts.gains[i].name, req.gainName,
                                 CASCADE_GAIN_NAME_CHARS) == 0) {
                    found = &p.facts.gains[i];
                    break;
                }
            }
            if (found == nullptr) { return CASCADE_API_UNSUPPORTED; }
            // Half a step of slack either side: a plugin that computed
            // max from min + n*step lands a rounding error past it.
            const double slack = 0.5 * std::max(found->stepDb, 1e-9);
            if (req.value < found->minDb - slack || req.value > found->maxDb + slack) {
                return CASCADE_API_OUT_OF_RANGE;
            }
        }
    }

    std::lock_guard<std::mutex> lk(mutex_);
    // RE-CHECKED UNDER THE LOCK: setLiveSet flips `live` holding this same
    // mutex, so a plugin detached between gate() above and here is refused
    // rather than leaving something behind for a plugin that is gone.
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    // And the stop, for the same reason: setStopped removes a stopped
    // plugin's queued requests under this lock, and one arriving just after
    // must not slip in behind it.
    if ((c.grants.load(std::memory_order_acquire) & CASCADE_STATE_STOPPED) != 0u) {
        return CASCADE_API_DENIED;
    }
    if (controlCount_ == kControlQueue) { return CASCADE_API_BUSY; }
    controls_[(controlHead_ + controlCount_) % kControlQueue] = req;
    ++controlCount_;
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::setMarker(const PluginApiClient& c, const CascadeMarker* m) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (m == nullptr || m->structSize < sizeof(CascadeMarker)) { return CASCADE_API_BAD_ARGUMENT; }
    // Copied FIRST, and only as much as both sides know: everything after
    // this line reads the host's own copy, never the plugin's memory again.
    CascadeMarker mk{};
    std::memcpy(&mk, m, std::min<std::size_t>(m->structSize, sizeof(CascadeMarker)));
    mk.structSize = static_cast<std::uint32_t>(sizeof(CascadeMarker));
    if (mk.kind != CASCADE_MARKER_POINT && mk.kind != CASCADE_MARKER_SPAN) {
        return CASCADE_API_OUT_OF_RANGE;
    }
    if (!finite(mk.freqHz)) { return CASCADE_API_BAD_ARGUMENT; }
    if (!(mk.freqHz > 0.0 && mk.freqHz < 1e12)) { return CASCADE_API_OUT_OF_RANGE; }
    if (mk.kind == CASCADE_MARKER_SPAN) {
        if (!finite(mk.widthHz)) { return CASCADE_API_BAD_ARGUMENT; }
        if (!(mk.widthHz > 0.0 && mk.widthHz <= 1e10)) { return CASCADE_API_OUT_OF_RANGE; }
    } else {
        mk.widthHz = 0.0;
    }
    mk.reserved = 0;
    // The label is a fixed array in the PLUGIN's struct, which it may have
    // filled to the last byte: re-terminate, cut back to a whole character.
    char label[CASCADE_MARKER_LABEL_CHARS];
    std::memcpy(label, mk.label, sizeof(label));
    label[CASCADE_MARKER_LABEL_CHARS - 1] = '\0';
    copyBoundedUtf8(mk.label, sizeof(mk.label), label);

    std::lock_guard<std::mutex> lk(mutex_);
    // RE-CHECKED UNDER THE LOCK: setLiveSet flips `live` holding this same
    // mutex, so a plugin detached between gate() above and here is refused
    // rather than leaving something behind for a plugin that is gone.
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    ClientState& st = state_[c.index];
    for (CascadeMarker& e : st.markers) {
        if (e.id == mk.id) {
            e = mk;
            ++markersSeq_;
            return CASCADE_API_OK;
        }
    }
    if (st.markers.size() >= CASCADE_MAX_MARKERS_PER_PLUGIN) { return CASCADE_API_LIMIT; }
    st.markers.push_back(mk);  // within the reserved capacity: no allocation
    ++markersSeq_;
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::removeMarker(const PluginApiClient& c, std::uint32_t id) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    std::lock_guard<std::mutex> lk(mutex_);
    // RE-CHECKED UNDER THE LOCK: setLiveSet flips `live` holding this same
    // mutex, so a plugin detached between gate() above and here is refused
    // rather than leaving something behind for a plugin that is gone.
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    std::vector<CascadeMarker>& v = state_[c.index].markers;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->id == id) {
            v.erase(it);
            ++markersSeq_;
            return CASCADE_API_OK;
        }
    }
    return CASCADE_API_NOT_FOUND;
}

std::int32_t PluginApiCore::clearMarkers(const PluginApiClient& c) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    std::lock_guard<std::mutex> lk(mutex_);
    // RE-CHECKED UNDER THE LOCK: setLiveSet flips `live` holding this same
    // mutex, so a plugin detached between gate() above and here is refused
    // rather than leaving something behind for a plugin that is gone.
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    std::vector<CascadeMarker>& v = state_[c.index].markers;
    if (!v.empty()) {
        v.clear();
        ++markersSeq_;
    }
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::settingsGet(const PluginApiClient& c, const char* key, char* buf,
                                        std::size_t cap) const {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (onRealtimeThread()) { return CASCADE_API_WRONG_THREAD; }
    if (!validSettingKey(key)) { return CASCADE_API_BAD_ARGUMENT; }
    if (buf == nullptr && cap != 0) { return CASCADE_API_BAD_ARGUMENT; }
    std::lock_guard<std::mutex> lk(settingsMutex_);
    const auto pit = settings_.find(c.name);
    if (pit == settings_.end()) { return CASCADE_API_NOT_FOUND; }
    const auto kit = pit->second.find(key);
    if (kit == pit->second.end()) { return CASCADE_API_NOT_FOUND; }
    const std::string& v = kit->second;
    if (cap > 0) {
        std::size_t n = std::min(v.size(), cap - 1);
        std::memcpy(buf, v.data(), n);
        n = utf8Floor(buf, n);
        buf[n] = '\0';
    }
    return static_cast<std::int32_t>(v.size());
}

std::int32_t PluginApiCore::settingsSet(const PluginApiClient& c, const char* key,
                                        const char* value) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (onRealtimeThread()) { return CASCADE_API_WRONG_THREAD; }
    if (!validSettingKey(key)) { return CASCADE_API_BAD_ARGUMENT; }
    std::string v;
    if (value != nullptr) {
        // Measured against the bound FIRST, reading no further than it: a
        // plugin's unterminated buffer must not walk the host off its end.
        const std::size_t n = boundedLen(value, CASCADE_SETTING_VALUE_BYTES);
        if (n >= CASCADE_SETTING_VALUE_BYTES) { return CASCADE_API_OUT_OF_RANGE; }
        if (!validUtf8(value, n)) { return CASCADE_API_BAD_ARGUMENT; }
        v.assign(value, n);
    }
    std::lock_guard<std::mutex> lk(settingsMutex_);
    if (value == nullptr) {
        const auto pit = settings_.find(c.name);
        if (pit == settings_.end() || pit->second.erase(key) == 0u) {
            return CASCADE_API_NOT_FOUND;
        }
        if (pit->second.empty()) { settings_.erase(pit); }
        ++settingsGeneration_;
        return CASCADE_API_OK;
    }
    std::map<std::string, std::string>& kv = settings_[c.name];
    const auto it = kv.find(key);
    if (it == kv.end() && kv.size() >= CASCADE_MAX_SETTINGS_PER_PLUGIN) {
        return CASCADE_API_LIMIT;
    }
    if (it != kv.end() && it->second == v) { return CASCADE_API_OK; }  // no change, no save
    kv[key] = std::move(v);
    ++settingsGeneration_;
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::log(const PluginApiClient& c, std::uint32_t level,
                                const char* text) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (text == nullptr) { return CASCADE_API_BAD_ARGUMENT; }
    if (level > CASCADE_LOG_ERROR) { return CASCADE_API_OUT_OF_RANGE; }
    // DEBUG is accepted and dropped: a plugin may leave its debug calls in a
    // release build without this host showing them to the user.
    if (level == CASCADE_LOG_DEBUG) { return CASCADE_API_OK; }
    std::lock_guard<std::mutex> lk(mutex_);
    // RE-CHECKED UNDER THE LOCK: setLiveSet flips `live` holding this same
    // mutex, so a plugin detached between gate() above and here is refused
    // rather than leaving something behind for a plugin that is gone.
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    if (logCount_ == kLogQueue) {
        ++logDropped_;
        return CASCADE_API_BUSY;
    }
    LogEntry& e = log_[(logHead_ + logCount_) % kLogQueue];
    e.client = c.index;
    e.level = level;
    copyBoundedUtf8(e.text, sizeof(e.text), text);
    ++logCount_;
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::addCommand(const PluginApiClient& c, std::uint32_t id,
                                       const char* label) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (id == 0u || label == nullptr || label[0] == '\0') { return CASCADE_API_BAD_ARGUMENT; }
    CommandSlot slot;
    slot.id = id;
    copyBoundedUtf8(slot.label, sizeof(slot.label), label);
    std::lock_guard<std::mutex> lk(mutex_);
    // RE-CHECKED UNDER THE LOCK: setLiveSet flips `live` holding this same
    // mutex, so a plugin detached between gate() above and here is refused
    // rather than leaving something behind for a plugin that is gone.
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    ClientState& st = state_[c.index];
    for (std::size_t i = 0; i < st.commandCount; ++i) {
        if (st.commands[i].id == id) {
            st.commands[i] = slot;
            return CASCADE_API_OK;
        }
    }
    if (st.commandCount >= CASCADE_MAX_COMMANDS_PER_PLUGIN) { return CASCADE_API_LIMIT; }
    st.commands[st.commandCount++] = slot;
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::removeCommand(const PluginApiClient& c, std::uint32_t id) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    std::lock_guard<std::mutex> lk(mutex_);
    // RE-CHECKED UNDER THE LOCK: setLiveSet flips `live` holding this same
    // mutex, so a plugin detached between gate() above and here is refused
    // rather than leaving something behind for a plugin that is gone.
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    ClientState& st = state_[c.index];
    for (std::size_t i = 0; i < st.commandCount; ++i) {
        if (st.commands[i].id != id) { continue; }
        for (std::size_t k = i + 1; k < st.commandCount; ++k) {
            st.commands[k - 1] = st.commands[k];
        }
        --st.commandCount;
        // A press of a command that no longer exists is not delivered.
        std::array<std::uint32_t, kPressQueue> keep{};
        std::size_t kept = 0;
        for (std::size_t p = 0; p < st.pressCount; ++p) {
            const std::uint32_t pid = st.presses[(st.pressHead + p) % kPressQueue];
            if (pid != id) { keep[kept++] = pid; }
        }
        st.presses = keep;
        st.pressHead = 0;
        st.pressCount = kept;
        return CASCADE_API_OK;
    }
    return CASCADE_API_NOT_FOUND;
}

std::int32_t PluginApiCore::pollCommand(const PluginApiClient& c, std::uint32_t* id) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (id == nullptr) { return CASCADE_API_BAD_ARGUMENT; }
    std::lock_guard<std::mutex> lk(mutex_);
    // RE-CHECKED UNDER THE LOCK: setLiveSet flips `live` holding this same
    // mutex, so a plugin detached between gate() above and here is refused
    // rather than leaving something behind for a plugin that is gone.
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    ClientState& st = state_[c.index];
    if (st.pressCount == 0) { return 0; }
    *id = st.presses[st.pressHead];
    st.pressHead = (st.pressHead + 1) % kPressQueue;
    --st.pressCount;
    return 1;
}

}  // namespace cascade::core
