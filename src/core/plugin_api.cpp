// Implementation of core/plugin_api.hpp. See that header for the threading
// rule every function here is written to.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_api.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "core/maidenhead.hpp"
#include "core/plugin_run.hpp"
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

// Whole-string UTF-8 check, the standard's (RFC 3629): every lead byte followed by the right number of continuation bytes, no overlong form
// (C0, C1, E0 80..9F, F0 80..8F), no UTF-16 surrogate (ED A0..BF) and nothing above U+10FFFF (F4 90.., F5..). A settings value goes into a JSON
// config file, and JSON is UTF-8 by definition - refusing here is better than having the writer mangle it later. (The check used to refuse only
// the overlong two-byte form and the lead bytes past F4; the fuzz target `plugin_settings` found that what it let through came back from the
// file as U+FFFD - docs/engine-stage4.md 6.29.)
bool validUtf8(const char* s, std::size_t n) {
    std::size_t i = 0;
    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        unsigned char lo = 0x80u;   // the range the FIRST continuation byte may take: wider than 80..BF only for no lead byte, narrower for five
        unsigned char hi = 0xBFu;
        if (c < 0x80u) {
            len = 1;
        } else if (c >= 0xC2u && c <= 0xDFu) {
            len = 2;
        } else if (c >= 0xE0u && c <= 0xEFu) {
            len = 3;
            if (c == 0xE0u) { lo = 0xA0u; }   // not an overlong form
            if (c == 0xEDu) { hi = 0x9Fu; }   // not a surrogate
        } else if (c >= 0xF0u && c <= 0xF4u) {
            len = 4;
            if (c == 0xF0u) { lo = 0x90u; }   // not an overlong form
            if (c == 0xF4u) { hi = 0x8Fu; }   // not above U+10FFFF
        } else {
            return false;
        }
        if (i + len > n) { return false; }
        for (std::size_t k = 1; k < len; ++k) {
            const auto b = static_cast<unsigned char>(s[i + k]);
            if (k == 1 ? (b < lo || b > hi) : (b & 0xC0u) != 0x80u) { return false; }
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

PluginApiCore::PluginApiCore(std::shared_ptr<ReceiverSnapshot> snapshot)
    : snapshot_(snapshot != nullptr ? std::move(snapshot) : std::make_shared<ReceiverSnapshot>()),
      clock_(std::make_shared<StreamClock>()) {
    snapshots_[0] = snapshot_;
    clocks_[0] = clock_;
    for (std::size_t i = 1; i < kRadios; ++i) {
        snapshots_[i] = std::make_shared<ReceiverSnapshot>();
        clocks_[i] = std::make_shared<StreamClock>();
    }
}

void PluginApiCore::setClientRadio(const std::string& key, std::uint32_t radio) {
    std::lock_guard<std::mutex> lk(mutex_);
    const std::uint32_t r = radio >= 1u && radio <= kRadios ? radio : 1u;
    // Kept by key even when the plugin has no client yet (its host client is made when the plugin UI first attaches it): the client is born on the radio it was put on.
    if (r == 1u) {
        clientRadios_.erase(key);
    } else {
        clientRadios_[key] = r;
    }
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->key == key) { c->radio.store(r, std::memory_order_release); }
    }
}

std::uint32_t PluginApiCore::clientRadio(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->key == key) { return c->radio.load(std::memory_order_acquire); }
    }
    return 1u;
}

std::uint32_t PluginApiCore::clientRadio(std::size_t index) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return index < clients_.size() ? clients_[index]->radio.load(std::memory_order_acquire) : 1u;
}

PluginApiClient& PluginApiCore::client(const std::string& key, const std::string& name,
                                       std::uint32_t capabilities) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->key == key) {
            // THE NAME IS NEVER REWRITTEN, though a different build could in
            // principle arrive under the same file name with another one: a
            // plugin thread reads `name` without a lock (the settings store is
            // keyed on it), so it has to be immutable once published. A build
            // that renames itself under an unchanged file name keeps its old
            // settings bucket until the next launch, which is harmless. Same
            // rule for `capabilities`.
            (void)name;
            (void)capabilities;
            return *c;
        }
    }
    auto c = std::make_unique<PluginApiClient>();
    c->key = key;
    c->name = name;
    c->capabilities = capabilities;
    c->slot = fault::slotFor(key);
    if (const auto placed = clientRadios_.find(key); placed != clientRadios_.end()) { c->radio.store(placed->second, std::memory_order_release); }
    c->index = clients_.size();
    ClientState st;
    st.markers.reserve(CASCADE_MAX_MARKERS_PER_PLUGIN);
    st.hints.reserve(CASCADE_MAX_SCHEDULE_HINTS_PER_PLUGIN);
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
    if (!st.hints.empty()) {
        st.hints.clear();
        ++hintsSeq_;
    }
    if (st.commandCount != 0) { commandsSeq_.fetch_add(1, std::memory_order_release); }
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
    // ...and its legacy tune, if one is waiting.
    std::size_t k = 0;
    for (std::size_t i = 0; i < retuneCount_; ++i) {
        if (retunes_[i].client != index) { retunes_[k++] = retunes_[i]; }
    }
    retuneCount_ = k;
}

void PluginApiCore::setLiveSet(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        const bool live = contains(keys, c->key);
        const bool was = c->live.exchange(live, std::memory_order_acq_rel);
        if (was && !live) { clearClientLocked(c->index); }
    }
}

void PluginApiCore::setLive(const std::string& key, bool live) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->key != key) { continue; }
        const bool was = c->live.exchange(live, std::memory_order_acq_rel);
        if (was && !live) { clearClientLocked(c->index); }
        return;
    }
}

void PluginApiCore::setAttached(bool attached) {
    attached_.store(attached, std::memory_order_release);
}

void PluginApiCore::refreshGrantsLocked(PluginApiClient& c) {
    std::uint32_t g = 0;
    if (contains(tuneGranted_, c.key)) { g |= CASCADE_STATE_TUNE_GRANTED; }
    if (contains(settingsGranted_, c.key)) { g |= CASCADE_STATE_SETTINGS_GRANTED; }
    if (contains(txGranted_, c.key)) { g |= CASCADE_STATE_TX_GRANTED; }
    // BY ID, like the host's own stop set: a stop survives the plugin's update.
    if (contains(stopped_, pluginRunId(c.key))) { g |= CASCADE_STATE_STOPPED; }
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

void PluginApiCore::setTxGranted(const std::string& key, bool granted) {
    std::lock_guard<std::mutex> lk(mutex_);
    setMember(txGranted_, key, granted);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) { refreshGrantsLocked(*c); }
}

bool PluginApiCore::txGranted(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return contains(txGranted_, key);
}

void PluginApiCore::publishTxState(bool armed, const std::string& keyedKey) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        std::uint32_t bits = 0;
        // ONLY A PLUGIN THAT DECLARED THE CAPABILITY IS TOLD, like receiverLocator: the
        // operator's arming window and the moments the transmitter is keyed are not
        // something every module that merely attached has any business learning.
        if ((c->capabilities & CASCADE_CAP_TX_AUDIO) != 0u) {
            if (armed) { bits |= CASCADE_STATE_TX_ARMED; }
            if (!keyedKey.empty() && c->key == keyedKey) { bits |= CASCADE_STATE_TRANSMITTING; }
        }
        c->txState.store(bits, std::memory_order_release);
    }
}

void PluginApiCore::setStopped(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lk(mutex_);
    // Reduced to ids (file names are accepted and reduced), so the match below
    // does not depend on the version in a file name.
    stopped_.clear();
    for (const std::string& k : keys) {
        const std::string id = pluginRunId(k);
        if (!id.empty() && std::find(stopped_.begin(), stopped_.end(), id) == stopped_.end()) {
            stopped_.push_back(id);
        }
    }
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        refreshGrantsLocked(*c);
        // A stopped plugin's marks and commands leave the screen at once, not
        // at the next rebuild: stopping is the user saying "not this one".
        if (contains(stopped_, pluginRunId(c->key))) { clearClientLocked(c->index); }
    }
}

void PluginApiCore::clearGrants() {
    std::lock_guard<std::mutex> lk(mutex_);
    tuneGranted_.clear();
    settingsGranted_.clear();
    txGranted_.clear();
    for (const std::unique_ptr<PluginApiClient>& c : clients_) { refreshGrantsLocked(*c); }
}

bool PluginApiCore::settingsGranted(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return contains(settingsGranted_, key);
}

bool PluginApiCore::tuneGranted(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return contains(tuneGranted_, key);
}

bool PluginApiCore::isStopped(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mutex_);
    // The stop set is ids (setStopped reduces file names), so the key is reduced the same way (0.99.73): a stop outlives an update of the module.
    return contains(stopped_, pluginRunId(key));
}

void PluginApiCore::noteTuneRequester(const std::string& key) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (std::find(tuneRequesters_.begin(), tuneRequesters_.end(), key) == tuneRequesters_.end()) {
        tuneRequesters_.push_back(key);
    }
}

void PluginApiCore::noteTuneDenied(const std::string& key) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        lastDenied_ = key;
    }
    deniedSeq_.fetch_add(1, std::memory_order_release);
}

std::vector<std::string> PluginApiCore::tuneRequesters() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return tuneRequesters_;
}

std::size_t PluginApiCore::tuneRequesterCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return tuneRequesters_.size();
}

std::string PluginApiCore::lastDeniedPlugin() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return lastDenied_;
}

void PluginApiCore::clearTuneRecords() {
    bool hadDenied = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        tuneRequesters_.clear();
        hadDenied = !lastDenied_.empty();
        lastDenied_.clear();
    }
    if (hadDenied) { deniedSeq_.fetch_add(1, std::memory_order_release); }
}

std::int32_t PluginApiCore::queueCentreRetune(const std::string& key, double centreHz) {
    if (!attached()) { return CASCADE_TUNE_FAILED; }
    // The range test the legacy path always applied, as a positive test (NaN fails it).
    if (!(centreHz > 0.0 && centreHz < 1e12)) { return CASCADE_TUNE_OUT_OF_RANGE; }
    std::lock_guard<std::mutex> lk(mutex_);
    std::size_t index = clients_.size();
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->key == key) {
            index = c->index;
            break;
        }
    }
    if (index >= clients_.size()) { return CASCADE_TUNE_FAILED; }
    // NEWEST WINS, one slot per module.
    for (std::size_t i = 0; i < retuneCount_; ++i) {
        if (retunes_[i].client == index) {
            retunes_[i].hz = centreHz;
            return CASCADE_TUNE_OK;
        }
    }
    if (retuneCount_ == kCentreRetuneQueue) { return CASCADE_TUNE_FAILED; }
    retunes_[retuneCount_++] = CentreRetune{index, centreHz};
    return CASCADE_TUNE_OK;
}

void PluginApiCore::takeCentreRetunes(std::vector<CentreRetune>& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (std::size_t i = 0; i < retuneCount_; ++i) { out.push_back(retunes_[i]); }
    retuneCount_ = 0;
}

bool PluginApiCore::centreRetuneStillAllowed(const CentreRetune& r) const {
    if (!attached()) { return false; }
    std::lock_guard<std::mutex> lk(mutex_);
    if (r.client >= clients_.size()) { return false; }
    const PluginApiClient& cl = *clients_[r.client];
    if (!cl.live.load(std::memory_order_acquire)) { return false; }
    const std::uint32_t g = cl.grants.load(std::memory_order_acquire);
    if ((g & CASCADE_STATE_STOPPED) != 0u) { return false; }
    return (g & CASCADE_STATE_TUNE_GRANTED) != 0u;
}

std::vector<std::string> PluginApiCore::settingsRequesters() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<std::string> out;
    for (const std::unique_ptr<PluginApiClient>& c : clients_) {
        if (c->askedSettings.load(std::memory_order_acquire)) { out.push_back(c->key); }
    }
    return out;
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
    // BACKWARD-COMPATIBILITY FLOOR, not "must equal our own sizeof": the
    // header's struct-growth rule (see the comment above CascadeReceiverState
    // in plugin_abi.h) is that the host writes min(its own sizeof,
    // out->structSize) bytes, so a plugin built against an OLDER, SMALLER
    // CascadeReceiverState (before receiverLocator was appended in 0.99.43)
    // must still get CASCADE_API_OK and a correctly truncated copy - it is
    // never entitled to fields past the end of the struct it allocated, but
    // it must not be refused outright just because this host's struct grew.
    //
    // THE FLOOR IS THE OLD STRUCT'S OWN SIZE, NOT sizeof(uint32_t). Every
    // pre-0.99.43 plugin's CascadeReceiverState is EXACTLY
    // offsetof(CascadeReceiverState, receiverLocator) bytes (192 on every
    // target this ships to) - that is the whole, real, level-1 struct such a
    // plugin actually allocates, and it is what "old plugin, not garbage"
    // means. A structSize below that was never a valid struct any real
    // plugin - old or new - ever had; accepting it as if it were a genuine
    // truncated read would turn a caller's bug (an uninitialised or corrupt
    // structSize) into a silent short read instead of the refusal that names
    // it. The floor is checked structurally with offsetof rather than a
    // literal 192 so it tracks the struct if a field before receiverLocator
    // is ever reordered.
    if (out == nullptr ||
        out->structSize < offsetof(CascadeReceiverState, receiverLocator)) {
        return CASCADE_API_BAD_ARGUMENT;
    }
    PublishedState p;
    if (!snapshotFor(c).read(p)) { return CASCADE_API_BUSY; }
    const FoxReceiverState& f = p.rx;
    CascadeReceiverState s{};
    std::uint32_t flags = 0;
    if ((f.flags & FOXAPI_RX_RUNNING) != 0u) { flags |= CASCADE_STATE_RUNNING; }
    if ((f.flags & FOXAPI_RX_DEVICE_OPEN) != 0u) { flags |= CASCADE_STATE_DEVICE_OPEN; }
    if ((f.flags & FOXAPI_RX_MUTED) != 0u) { flags |= CASCADE_STATE_MUTED; }
    if ((f.flags & FOXAPI_RX_DEVICE_AGC) != 0u) { flags |= CASCADE_STATE_DEVICE_AGC; }
    if ((f.flags & FOXAPI_RX_AGC_SUPPORTED) != 0u) { flags |= CASCADE_STATE_AGC_SUPPORTED; }
    // The level-1 rule, kept: open when the level is above the threshold, as
    // the plugin sees both (FOXAPI_RX_SQUELCH_OPEN is the DSP's own lamp).
    if (f.signalDb > f.squelchDb) { flags |= CASCADE_STATE_SQUELCH_OPEN; }
    if ((f.flags & FOXAPI_RX_STEREO_ACTIVE) != 0u) { flags |= CASCADE_STATE_STEREO; }
    flags |= c.grants.load(std::memory_order_acquire);
    // The arming window and the key, for a TX_AUDIO plugin only (publishTxState).
    flags |= c.txState.load(std::memory_order_acquire);
    // IN EVERY STATE THIS HOST FILLS, for every client, with no capability or grant
    // behind it (0.99.75): the host lists a track flagged CASCADE_TRACK_FLAG_NO_POSITION
    // (PluginUi::unplacedTracks) instead of dropping it, and a plugin may publish one
    // only when it sees this bit. A host that did not set it would be telling the ADS-B
    // plugin to go on hiding every aircraft it has heard and not yet placed.
    flags |= CASCADE_STATE_LISTS_UNPLACED_TRACKS;
    // AND THE ESTIMATES (0.99.78): a track flagged CASCADE_TRACK_FLAG_ESTIMATED is kept for
    // kTrackDropMsAircraftEstimated (180 s) instead of 60 s and drawn and listed as an estimate
    // (trackPresentation, the map's marker ring, estimatedRowNote), so an ADS-B plugin may carry a
    // fringe aircraft between its bursts instead of letting it blink off the map.
    flags |= CASCADE_STATE_HOLDS_ESTIMATED_TRACKS;
    // LIKEWISE (0.99.77): the host draws a panel row that carries CASCADE_ROW_FLAG_KEY as a key
    // and delivers its press through poll_command (drawRowTable, PluginApiCore::pressCommand), and
    // a plugin shows "press to choose" wording, and registers the commands only its row keys need,
    // only when it sees this bit. A host that did not set it would be telling the IBOC plugin to
    // go on offering HD1 to HD8 as text.
    flags |= CASCADE_STATE_ROW_KEYS;
    flags |= CASCADE_STATE_IMAGE_INPUT;
    s.flags = flags;
    s.seq = p.app.abiSeq;
    s.tuneSeq = p.app.abiTuneSeq;
    s.modeSeq = p.app.abiModeSeq;
    s.deviceSeq = p.app.abiDeviceSeq;
    s.audioSeq = p.app.abiAudioSeq;
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
    s.gainCount = std::min<std::uint32_t>(p.app.abiGainCount, kMaxPublishedGains);
    std::memcpy(s.deviceName, f.deviceName, sizeof(s.deviceName));
    s.deviceName[CASCADE_DEVICE_NAME_CHARS - 1] = '\0';
    // receiverLocator: appended field, see the header's struct-growth note.
    // Computed here rather than stored pre-formatted, because it is O(1)
    // arithmetic and this keeps exactly one place (maidenheadGrid6) that
    // knows the Maidenhead rules.
    //
    // GATED ON CASCADE_CAP_RECEIVER_LOCATOR, per client - not on whether a
    // position happens to be set. A plugin that never declared the bit reads
    // an empty locator and an unset flag whatever the receiver's position is,
    // exactly as if there were none: the field exists to answer a plugin that
    // asked for it, not to broadcast the user's location to every module that
    // merely attached (see the header's note on CASCADE_CAP_RECEIVER_LOCATOR).
    s.receiverLocator[0] = '\0';
    if (p.app.rxPositionSet && (c.capabilities & CASCADE_CAP_RECEIVER_LOCATOR) != 0u) {
        char grid[7];
        if (cascade::core::maidenheadGrid6(p.app.rxLatDeg, p.app.rxLonDeg, grid)) {
            std::memcpy(s.receiverLocator, grid, sizeof(grid));
            flags |= CASCADE_STATE_LOCATOR_KNOWN;
            s.flags = flags;
        }
    }
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
    PublishedState p;
    if (!snapshotFor(c).read(p)) { return CASCADE_API_BUSY; }
    if (index >= p.app.abiGainCount || index >= kMaxPublishedGains) {
        return CASCADE_API_NOT_FOUND;
    }
    const PublishedGain& src = p.app.gains[index];
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
    PublishedState p;
    if (!snapshotFor(c).read(p)) { return CASCADE_API_BUSY; }
    if ((p.rx.flags & FOXAPI_RX_DEVICE_OPEN) == 0u) { return 0; }
    const std::uint32_t n =
        std::min<std::uint32_t>(p.app.rateCount, static_cast<std::uint32_t>(kMaxPublishedRates));
    for (std::uint32_t i = 0; i < n && i < cap; ++i) { out[i] = p.app.rates[i]; }
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
    if (!clocks_[radioIndex(c.radio.load(std::memory_order_acquire))]->read(sf)) { return CASCADE_API_BUSY; }
    PublishedState p;
    if (!snapshotFor(c).read(p)) { return CASCADE_API_BUSY; }
    CascadeStreamInfo si{};
    si.epoch = sf.epoch;
    si.epochStartUnixMs = sf.epochStartUnixMs;
    si.iqRateHz = sf.iqRateHz;
    si.audioRateHz = sf.audioRateHz;
    si.outputRateHz = p.app.outputRateHz;
    si.iqFrames = sf.iqFrames;
    si.audioFrames = sf.audioFrames;
    si.outputFrames = p.app.outputFrames;
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
        // The first ask moves the generation the engine hashes (settingsRequestersSeq); a repeat is no change.
        if (!c.askedSettings.exchange(true, std::memory_order_acq_rel)) {
            askersSeq_.fetch_add(1, std::memory_order_release);
        }
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
        PublishedState p;
        if (!snapshotFor(c).read(p)) { return CASCADE_API_BUSY; }
        if ((p.rx.flags & FOXAPI_RX_DEVICE_OPEN) == 0u) { return CASCADE_API_NO_DEVICE; }
        if (req.kind == K::DeviceAgc && (p.rx.flags & FOXAPI_RX_AGC_SUPPORTED) == 0u) {
            return CASCADE_API_UNSUPPORTED;
        }
        if (req.kind == K::Gain) {
            const PublishedGain* found = nullptr;
            for (std::uint32_t i = 0; i < p.app.abiGainCount && i < kMaxPublishedGains; ++i) {
                if (std::strncmp(p.app.gains[i].name, req.gainName,
                                 CASCADE_GAIN_NAME_CHARS) == 0) {
                    found = &p.app.gains[i];
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
    // wire-c: a marker of the size before the credit field existed is still taken (its credit is empty)
    if (m == nullptr || m->structSize < offsetof(CascadeMarker, credit)) { return CASCADE_API_BAD_ARGUMENT; }
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
    // wire-c begin: the credit, bounded and cut back to a whole character the same way (an old-sized marker left it zero)
    {
        char credit[CASCADE_MARKER_CREDIT_CHARS];
        std::memcpy(credit, mk.credit, sizeof(credit));
        credit[CASCADE_MARKER_CREDIT_CHARS - 1] = '\0';
        std::memset(mk.credit, 0, sizeof(mk.credit));
        copyBoundedUtf8(mk.credit, sizeof(mk.credit), credit);
    }
    // wire-c end

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

std::int32_t PluginApiCore::setScheduleHint(const PluginApiClient& c, const CascadeScheduleHint* h) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (h == nullptr || h->structSize < sizeof(CascadeScheduleHint)) { return CASCADE_API_BAD_ARGUMENT; }
    // Copied FIRST: everything after this line reads the host's own copy.
    CascadeScheduleHint x{};
    std::memcpy(&x, h, std::min<std::size_t>(h->structSize, sizeof(CascadeScheduleHint)));
    x.structSize = static_cast<std::uint32_t>(sizeof(CascadeScheduleHint));
    if (!finite(x.maxElDeg) || !finite(x.freqHz) || !finite(x.bandwidthHz)) { return CASCADE_API_BAD_ARGUMENT; }
    if (x.losUnixMs <= x.aosUnixMs) { return CASCADE_API_BAD_ARGUMENT; }
    if (x.kind != CASCADE_SCHEDULE_HINT_PASS) { return CASCADE_API_OUT_OF_RANGE; }
    if (!(x.freqHz > 0.0 && x.freqHz < 6.0e9)) { return CASCADE_API_OUT_OF_RANGE; }
    if (x.losUnixMs - x.aosUnixMs > 24LL * 3600LL * 1000LL) { return CASCADE_API_OUT_OF_RANGE; }
    if (x.aosUnixMs < 1577836800000LL || x.aosUnixMs > 4102444800000LL) { return CASCADE_API_OUT_OF_RANGE; }
    if (x.maxElDeg < 0.0 || x.maxElDeg > 90.0) { return CASCADE_API_OUT_OF_RANGE; }
    if (x.bandwidthHz < 0.0 || x.bandwidthHz > 20.0e6) { return CASCADE_API_OUT_OF_RANGE; }
    if (x.demodMode > 8u) { return CASCADE_API_OUT_OF_RANGE; }
    // The texts are fixed arrays in the PLUGIN's struct, which it may have filled to the last byte: re-terminate, cut back to a whole character.
    char text[CASCADE_SCHEDULE_LABEL_CHARS];
    std::memcpy(text, x.label, sizeof(text));
    text[CASCADE_SCHEDULE_LABEL_CHARS - 1] = '\0';
    copyBoundedUtf8(x.label, sizeof(x.label), text);
    std::memcpy(text, x.decoderName, sizeof(text));
    text[CASCADE_SCHEDULE_LABEL_CHARS - 1] = '\0';
    copyBoundedUtf8(x.decoderName, sizeof(x.decoderName), text);

    std::lock_guard<std::mutex> lk(mutex_);
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }   // (as setMarker: re-checked under the lock)
    ClientState& st = state_[c.index];
    for (CascadeScheduleHint& e : st.hints) {
        if (e.id == x.id) {
            e = x;
            ++hintsSeq_;
            return CASCADE_API_OK;
        }
    }
    if (st.hints.size() >= CASCADE_MAX_SCHEDULE_HINTS_PER_PLUGIN) { return CASCADE_API_LIMIT; }
    st.hints.push_back(x);   // within the reserved capacity: no allocation
    ++hintsSeq_;
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::clearScheduleHints(const PluginApiClient& c) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    std::lock_guard<std::mutex> lk(mutex_);
    if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
    std::vector<CascadeScheduleHint>& v = state_[c.index].hints;
    if (!v.empty()) {
        v.clear();
        ++hintsSeq_;
    }
    return CASCADE_API_OK;
}

// ---- DECODE EVENTS (plugin_abi.h CascadeEvent; engine wiring W2) ------------------------------------------------------------------------------

namespace {

std::int64_t eventNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool validEventKind(const char* kind, std::size_t cap) {
    std::size_t n = 0;
    while (n < cap && kind[n] != '\0') { ++n; }
    if (n == 0 || n >= cap) { return false; }   // empty, or not terminated inside the field
    for (std::size_t i = 0; i < n; ++i) {
        const char ch = kind[i];
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) { return false; }
    }
    return true;
}

}  // namespace

std::int32_t PluginApiCore::postEvent(const PluginApiClient& c, const CascadeEvent* e) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (e == nullptr || e->structSize < offsetof(CascadeEvent, text) + CASCADE_EVENT_TEXT_CHARS) { return CASCADE_API_BAD_ARGUMENT; }
    // Copied FIRST: everything after this line reads the host's own copy (the plugin may reuse its buffer at once).
    CascadeEvent x{};
    std::memcpy(&x, e, std::min<std::size_t>(e->structSize, sizeof(CascadeEvent)));
    if (!validEventKind(x.kind, sizeof(x.kind))) { return CASCADE_API_BAD_ARGUMENT; }
    if (!std::isfinite(x.frequencyHz)) { return CASCADE_API_BAD_ARGUMENT; }
    if (x.frequencyHz < 0.0 || x.frequencyHz > 6.0e9) { return CASCADE_API_OUT_OF_RANGE; }
    // The text is a fixed array in the PLUGIN's struct, which it may have filled to the last byte: re-terminate, make control characters spaces, cut back to a
    // whole character.
    char raw[CASCADE_EVENT_TEXT_CHARS];
    std::memcpy(raw, x.text, sizeof(raw));
    raw[CASCADE_EVENT_TEXT_CHARS - 1] = '\0';
    for (char& ch : raw) {
        if (ch != '\0' && (static_cast<unsigned char>(ch) < 0x20u || static_cast<unsigned char>(ch) == 0x7Fu)) { ch = ' '; }
    }
    std::memset(x.text, 0, sizeof(x.text));
    copyBoundedUtf8(x.text, sizeof(x.text), raw);
    std::string name;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!c.live.load(std::memory_order_acquire)) { return CASCADE_API_DETACHED; }
        name = c.name;
    }
    const bool priv = (c.capabilities & CASCADE_FLAG_PRIVATE_DATA) != 0u;
    x.structSize = static_cast<std::uint32_t>(sizeof(CascadeEvent));
    x.flags = priv ? CASCADE_EVENT_FLAG_PRIVATE : 0u;
    x.unixMs = eventNowMs();
    std::memset(x.plugin, 0, sizeof(x.plugin));
    copyBoundedUtf8(x.plugin, sizeof(x.plugin), name.c_str());
    std::lock_guard<std::mutex> lk(eventsMutex_);
    x.seq = eventsNext_++;
    events_.push_back(x);
    while (events_.size() > CASCADE_EVENT_RING) { events_.pop_front(); }
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::recorderRequest(PluginApiClient& c, bool start, const char* label) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    const std::uint32_t grants = c.grants.load(std::memory_order_acquire);
    if ((grants & CASCADE_STATE_STOPPED) != 0u) { return CASCADE_API_DENIED; }
    // asking for a SETTINGS-grant function is what makes the host offer the grant key (the same rule as requestControl)
    if (!c.askedSettings.exchange(true, std::memory_order_acq_rel)) { askersSeq_.fetch_add(1, std::memory_order_release); }
    if ((grants & CASCADE_STATE_SETTINGS_GRANTED) == 0u) { return CASCADE_API_DENIED; }
    RecorderRequest req;
    req.client = c.index;
    req.start = start;
    if (start) {
        if (label == nullptr) { return CASCADE_API_BAD_ARGUMENT; }
        const std::size_t n = ::strnlen(label, CASCADE_RECORDER_LABEL_CHARS + 1);
        if (n == 0) { return CASCADE_API_BAD_ARGUMENT; }
        if (n > CASCADE_RECORDER_LABEL_CHARS - 1) { return CASCADE_API_OUT_OF_RANGE; }
        for (std::size_t i = 0; i < n; ++i) {
            const char ch = label[i];
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
            if (!ok) { return CASCADE_API_BAD_ARGUMENT; }
        }
        std::memcpy(req.label, label, n);
    }
    std::lock_guard<std::mutex> lk(recorderMutex_);
    if (recorderQueue_.size() >= kRecorderQueue) { return CASCADE_API_BUSY; }
    recorderQueue_.push_back(req);
    return CASCADE_API_OK;
}

// wire-c begin: the open requests.
bool PluginApiCore::acceptableOpenUrl(const char* url, std::int32_t& why) {
    why = CASCADE_API_BAD_ARGUMENT;
    if (url == nullptr) { return false; }
    const std::size_t n = ::strnlen(url, CASCADE_OPEN_URL_CHARS + 1);
    if (n == 0) { return false; }
    if (n > CASCADE_OPEN_URL_CHARS - 1) {
        why = CASCADE_API_OUT_OF_RANGE;
        return false;
    }
    static const char kScheme[] = "https://";
    if (n <= sizeof(kScheme) - 1 || std::memcmp(url, kScheme, sizeof(kScheme) - 1) != 0) { return false; }   // lower case https:// only
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char ch = static_cast<unsigned char>(url[i]);
        if (ch <= 0x20u || ch >= 0x7Fu || ch == '\\') { return false; }   // control, space, non-ASCII, backslash
    }
    // the authority: up to the first '/', '?' or '#'
    std::size_t end = sizeof(kScheme) - 1;
    while (end < n && url[end] != '/' && url[end] != '?' && url[end] != '#') { ++end; }
    const std::string authority(url + sizeof(kScheme) - 1, end - (sizeof(kScheme) - 1));
    if (authority.empty() || authority.find('@') != std::string::npos) { return false; }   // no user name or password
    if (authority.front() == '.' || authority.front() == ':' || authority.front() == '-') { return false; }
    why = CASCADE_API_OK;
    return true;
}

std::int32_t PluginApiCore::openRequest(const PluginApiClient& c, bool folder, const char* url) {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if ((c.grants.load(std::memory_order_acquire) & CASCADE_STATE_STOPPED) != 0u) { return CASCADE_API_DENIED; }
    std::string held;
    if (!folder) {
        std::int32_t why = CASCADE_API_BAD_ARGUMENT;
        if (!acceptableOpenUrl(url, why)) { return why; }
        held = url;
    }
    std::lock_guard<std::mutex> lk(openMutex_);
    openRequests_[{c.index, folder ? 1 : 0}] = held;
    openSeq_.fetch_add(1, std::memory_order_release);
    return CASCADE_API_OK;
}

std::vector<PluginApiCore::OpenRequest> PluginApiCore::openRequests() const {
    std::vector<OpenRequest> out;
    std::lock_guard<std::mutex> lk(mutex_);
    std::lock_guard<std::mutex> lk2(openMutex_);
    for (const auto& kv : openRequests_) {
        if (kv.first.first >= clients_.size()) { continue; }
        const PluginApiClient& cl = *clients_[kv.first.first];
        if (!cl.live.load(std::memory_order_acquire) || (cl.grants.load(std::memory_order_acquire) & CASCADE_STATE_STOPPED) != 0u) { continue; }
        OpenRequest r;
        r.key = cl.key;
        r.name = cl.name;
        r.folder = kv.first.second == 1;
        r.url = kv.second;
        out.push_back(std::move(r));
    }
    return out;
}

bool PluginApiCore::takeOpenRequest(const std::string& key, bool folder, OpenRequest& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::lock_guard<std::mutex> lk2(openMutex_);
    for (std::size_t i = 0; i < clients_.size(); ++i) {
        const PluginApiClient& cl = *clients_[i];
        if (cl.key != key || !cl.live.load(std::memory_order_acquire)) { continue; }
        const auto it = openRequests_.find({i, folder ? 1 : 0});
        if (it == openRequests_.end()) { return false; }
        out.key = cl.key;
        out.name = cl.name;
        out.folder = folder;
        out.url = it->second;
        openRequests_.erase(it);
        openSeq_.fetch_add(1, std::memory_order_release);
        return true;
    }
    return false;
}
// wire-c end

void PluginApiCore::takeRecorderRequests(std::vector<RecorderRequest>& out) {
    std::lock_guard<std::mutex> lk(recorderMutex_);
    for (const RecorderRequest& r : recorderQueue_) { out.push_back(r); }
    recorderQueue_.clear();
}

bool PluginApiCore::recorderRequestStillAllowed(const RecorderRequest& r) const {
    if (!attached()) { return false; }
    std::lock_guard<std::mutex> lk(mutex_);
    if (r.client >= clients_.size()) { return false; }
    const PluginApiClient& cl = *clients_[r.client];
    if (!cl.live.load(std::memory_order_acquire)) { return false; }
    const std::uint32_t g = cl.grants.load(std::memory_order_acquire);
    if ((g & CASCADE_STATE_STOPPED) != 0u) { return false; }
    return r.start ? (g & CASCADE_STATE_SETTINGS_GRANTED) != 0u : true;
}

void PluginApiCore::hostPostEvent(const std::string& plugin, bool privateData, const std::string& kind, const std::string& text, double frequencyHz,
                                  std::int64_t unixMs) {
    CascadeEvent x{};
    x.structSize = static_cast<std::uint32_t>(sizeof(CascadeEvent));
    x.flags = privateData ? CASCADE_EVENT_FLAG_PRIVATE : 0u;
    x.unixMs = unixMs != 0 ? unixMs : eventNowMs();
    x.frequencyHz = (std::isfinite(frequencyHz) && frequencyHz >= 0.0 && frequencyHz <= 6.0e9) ? frequencyHz : 0.0;
    copyBoundedUtf8(x.plugin, sizeof(x.plugin), plugin.c_str());
    copyBoundedUtf8(x.kind, sizeof(x.kind), kind.c_str());
    std::string clean = text;
    for (char& ch : clean) {
        if (static_cast<unsigned char>(ch) < 0x20u || static_cast<unsigned char>(ch) == 0x7Fu) { ch = ' '; }
    }
    copyBoundedUtf8(x.text, sizeof(x.text), clean.c_str());
    std::lock_guard<std::mutex> lk(eventsMutex_);
    x.seq = eventsNext_++;
    events_.push_back(x);
    while (events_.size() > CASCADE_EVENT_RING) { events_.pop_front(); }
}

std::int32_t PluginApiCore::readEvents(const PluginApiClient& c, std::uint64_t afterSeq, CascadeEvent* out, std::uint32_t cap) const {
    const std::int32_t g = gate(c);
    if (g != CASCADE_API_OK) { return g; }
    if (cap == 0u) { return 0; }
    if (out == nullptr) { return CASCADE_API_BAD_ARGUMENT; }
    std::string self;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        self = c.name;
    }
    std::uint32_t n = 0;
    std::lock_guard<std::mutex> lk(eventsMutex_);
    for (const CascadeEvent& e : events_) {
        if (e.seq <= afterSeq) { continue; }
        // A private plugin's events are its own and the host's windows': never another plugin's.
        if ((e.flags & CASCADE_EVENT_FLAG_PRIVATE) != 0u && self != e.plugin) { continue; }
        const std::uint32_t keep = out[n].structSize;
        const std::size_t want = std::min<std::size_t>(keep, sizeof(CascadeEvent));
        if (want < offsetof(CascadeEvent, text) + CASCADE_EVENT_TEXT_CHARS) { return n > 0 ? static_cast<std::int32_t>(n) : CASCADE_API_BAD_ARGUMENT; }
        std::memcpy(&out[n], &e, want);
        out[n].structSize = keep;
        if (++n >= cap) { break; }
    }
    return static_cast<std::int32_t>(n);
}

std::vector<CascadeEvent> PluginApiCore::hostEvents(std::uint64_t afterSeq, std::size_t max) const {
    std::vector<CascadeEvent> out;
    std::lock_guard<std::mutex> lk(eventsMutex_);
    for (const CascadeEvent& e : events_) {
        if (e.seq <= afterSeq) { continue; }
        out.push_back(e);
        if (out.size() >= max) { break; }
    }
    return out;
}

std::uint64_t PluginApiCore::eventsSeq() const {
    std::lock_guard<std::mutex> lk(eventsMutex_);
    return eventsNext_ - 1u;
}

std::uint64_t PluginApiCore::scheduleHintsSeq() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return hintsSeq_;
}

void PluginApiCore::scheduleHints(std::vector<HostScheduleHint>& out) const {
    out.clear();
    std::lock_guard<std::mutex> lk(mutex_);
    for (std::size_t i = 0; i < clients_.size() && i < state_.size(); ++i) {
        for (const CascadeScheduleHint& h : state_[i].hints) { out.push_back(HostScheduleHint{clients_[i]->key, h}); }
    }
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

std::int32_t PluginApiCore::settingsUiSet(const std::string& pluginName, const char* key,
                                          const char* value) {
    if (!validSettingKey(key)) { return CASCADE_API_BAD_ARGUMENT; }
    std::string v;
    if (value != nullptr) {
        const std::size_t n = boundedLen(value, CASCADE_SETTING_VALUE_BYTES);
        if (n >= CASCADE_SETTING_VALUE_BYTES) { return CASCADE_API_OUT_OF_RANGE; }
        if (!validUtf8(value, n)) { return CASCADE_API_BAD_ARGUMENT; }
        v.assign(value, n);
    }
    std::lock_guard<std::mutex> lk(settingsMutex_);
    if (value == nullptr) {
        const auto pit = settings_.find(pluginName);
        if (pit == settings_.end() || pit->second.erase(key) == 0u) {
            return CASCADE_API_NOT_FOUND;
        }
        if (pit->second.empty()) { settings_.erase(pit); }
        ++settingsGeneration_;
        std::uint64_t& s = settingsUiSeq_[pluginName];
        s = (s == 0u ? 2u : s + 1u);
        return CASCADE_API_OK;
    }
    std::map<std::string, std::string>& kv = settings_[pluginName];
    const auto it = kv.find(key);
    if (it == kv.end() && kv.size() >= CASCADE_MAX_SETTINGS_PER_PLUGIN) {
        return CASCADE_API_LIMIT;
    }
    if (it != kv.end() && it->second == v) { return CASCADE_API_OK; }  // no change, no bump
    kv[key] = std::move(v);
    ++settingsGeneration_;
    std::uint64_t& s = settingsUiSeq_[pluginName];
    s = (s == 0u ? 2u : s + 1u);
    return CASCADE_API_OK;
}

std::int32_t PluginApiCore::settingsUiGet(const std::string& pluginName, const char* key,
                                          std::string& out) const {
    out.clear();
    if (!validSettingKey(key)) { return CASCADE_API_BAD_ARGUMENT; }
    std::lock_guard<std::mutex> lk(settingsMutex_);
    const auto pit = settings_.find(pluginName);
    if (pit == settings_.end()) { return CASCADE_API_NOT_FOUND; }
    const auto kit = pit->second.find(key);
    if (kit == pit->second.end()) { return CASCADE_API_NOT_FOUND; }
    out = kit->second;
    return CASCADE_API_OK;
}

std::uint64_t PluginApiCore::settingsUiSeq(const std::string& pluginName) const {
    std::lock_guard<std::mutex> lk(settingsMutex_);
    const auto it = settingsUiSeq_.find(pluginName);
    return it == settingsUiSeq_.end() ? 1u : it->second;
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
            // The same key said again is no change; a new label is one (commandsSeq).
            if (std::strncmp(st.commands[i].label, slot.label, sizeof(slot.label)) != 0) {
                st.commands[i] = slot;
                commandsSeq_.fetch_add(1, std::memory_order_release);
            }
            return CASCADE_API_OK;
        }
    }
    if (st.commandCount >= CASCADE_MAX_COMMANDS_PER_PLUGIN) { return CASCADE_API_LIMIT; }
    st.commands[st.commandCount++] = slot;
    commandsSeq_.fetch_add(1, std::memory_order_release);
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
        commandsSeq_.fetch_add(1, std::memory_order_release);
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
