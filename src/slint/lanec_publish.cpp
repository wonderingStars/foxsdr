// lanec_publish.cpp - see lanec_publish.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanec_publish.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gaps_logic.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace foxsdr::slintlanec {

namespace {
std::atomic<std::uint32_t> g_deltaMask{0};

std::string fmt(const char* f, double v) {   // one double into one of this unit's own literal formats (never a translated text)
    std::vector<char> b(64);
    const int n = std::snprintf(b.data(), b.size(), f, v);
    return n > 0 ? std::string(b.data(), static_cast<std::size_t>(std::min<int>(n, 63))) : std::string();
}
slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }
}  // namespace

std::uint32_t deltaMaskNow() { return g_deltaMask.load(std::memory_order_relaxed); }
int deltaRadioOf(const FoxCommand& c) { return foxsdr::slintgaps::FrequencyLock::instance().radioOf(c); }

// What each property last showed, so a tick sets only what moved.
struct LaneCPublisher::Shown {
    bool lockEnabled = false, muteScan = false, logScan = false, engineNote = false, memEnabled = false, quickStore = false, shortcutsOpen = false, recordingsOpen = false;
    std::string lockText, priority, priorityEvery, scanStats, memTitle, memSquelch, memThreshold, memOffset, memTone, memTx, clock, recordingsNote;
    std::string quickSig;
    int fieldsEpoch = 0, scanEpoch = 0;
    bool first = true;
};

LaneCPublisher::LaneCPublisher(Env env)
    : env_(std::move(env)), table_(env_.api, env_.session), shown_(std::make_unique<Shown>()),
      quickModel_(std::make_shared<slint::VectorModel<LCQuick>>()),
      shortcutModel_(std::make_shared<slint::VectorModel<LCShortcut>>()),
      recordingModel_(std::make_shared<slint::VectorModel<LCRecording>>()) {
    if (!env_.storePath.empty()) {
        std::ifstream in(std::filesystem::path(std::u8string(env_.storePath.begin(), env_.storePath.end())), std::ios::binary);
        if (in) {
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            store_ = Store::fromJson(text);
        }
    }
    g_deltaMask.store(store_.deltaMask, std::memory_order_relaxed);
    for (int i = 0; i < kQuickMemories; ++i) { quickModel_->push_back(LCQuick{ss(quickLabel(i, store_.quick[static_cast<std::size_t>(i)])), store_.quick[static_cast<std::size_t>(i)].set}); }
    if (env_.ui != nullptr) {
        auto& lc = env_.ui->global<::LaneC>();
        lc.set_quick(quickModel_);
        lc.set_shortcuts(shortcutModel_);
        lc.set_recordings(recordingModel_);
    }
}

LaneCPublisher::~LaneCPublisher() { flush(); }

void LaneCPublisher::wire() {
    env_.ui->global<::LaneC>().on_act([this](slint::SharedString id, slint::SharedString text) { act(std::string(id), std::string(text)); });
}

std::string LaneCPublisher::factOf(const char* key) {
    std::string s;
    if (!table_.fact(key, s)) { s.clear(); }
    return s;
}

void LaneCPublisher::save() {
    dirty_ = false;
    if (env_.storePath.empty()) { return; }
    namespace fs = std::filesystem;
    const fs::path path(std::u8string(env_.storePath.begin(), env_.storePath.end()));
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { return; }
        out << store_.toJson();
        if (!out) { return; }
    }
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); }
}

void LaneCPublisher::flush() { if (dirty_) { save(); } }

// ----------------------------------------------------------------------------------------------------------------------------------------- menu entries

bool LaneCPublisher::toggleStepLock() { store_.stepLock = !store_.stepLock; markDirty(); return store_.stepLock; }
void LaneCPublisher::toggleDelta(int radio) {
    if (radio < 1 || radio > 8) { return; }
    store_.deltaMask ^= 1u << (radio - 1);
    g_deltaMask.store(store_.deltaMask, std::memory_order_relaxed);
    markDirty();
}
void LaneCPublisher::toggleBwMemory() { store_.bwMemory = !store_.bwMemory; markDirty(); }

void LaneCPublisher::openShortcuts(const foxsdr::slintsystem::KeyTable& table) {
    const auto rows = shortcutRows(table);
    while (shortcutModel_->row_count() > 0) { shortcutModel_->erase(shortcutModel_->row_count() - 1); }
    for (const ShortcutRow& r : rows) { shortcutModel_->push_back(LCShortcut{ss(r.keys), ss(r.what)}); }
    shortcutsOpen_ = true;
    env_.ui->global<::LaneC>().set_shortcuts_open(true);
}
void LaneCPublisher::openRecordings() {
    recordingsAt_ = -1000;   // list at once
    env_.ui->global<::LaneC>().set_recordings_open(true);
}
bool LaneCPublisher::overlayOpen() const {
    auto& lc = env_.ui->global<::LaneC>();
    return lc.get_shortcuts_open() || lc.get_recordings_open();
}
void LaneCPublisher::closeOverlays() {
    auto& lc = env_.ui->global<::LaneC>();
    lc.set_shortcuts_open(false);
    lc.set_recordings_open(false);
}

// ----------------------------------------------------------------------------------------------------------------------------------------- the bookmark list

void LaneCPublisher::refreshBookmarks() {
    const std::string version = factOf("fact.bookmarks.version");
    if (!bookmarksVersion_.empty() && version == bookmarksVersion_) { return; }
    bookmarks_.clear();
    const std::int32_t n = std::max<std::int32_t>(0, table_.readList(FOXAPI_LIST_BOOKMARKS, 0, nullptr));
    for (std::int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (table_.readList(FOXAPI_LIST_BOOKMARKS, static_cast<std::uint32_t>(i), &it) <= i) { break; }
        Bookmark b;
        b.id = static_cast<int>(it.id);
        b.hz = it.value[0];
        b.name = it.name;
        b.mode = static_cast<std::uint32_t>(it.value[1]);
        b.bw = it.value[2];
        b.ticked = (it.flags & FOXAPI_ITEM_TICKED) != 0u;
        bookmarks_.push_back(std::move(b));
    }
    bookmarksVersion_ = (factOf("fact.bookmarks.version") == version) ? version : std::string();
}

const LaneCPublisher::Bookmark* LaneCPublisher::bookmarkAt(double hz) const {
    for (const Bookmark& b : bookmarks_) { if (std::fabs(b.hz - hz) < 1.0) { return &b; } }
    return nullptr;
}

// ----------------------------------------------------------------------------------------------------------------------------------------- the acts

void LaneCPublisher::act(const std::string& id, const std::string& text) {
    const double now = env_.nowMs ? env_.nowMs() : 0.0;
    if (id == "lock-temp" || id == "lock-perm") {
        const double forMs = id == "lock-temp" ? 600000.0 : 0.0;
        const double hz = scanHz_;
        if (scanState_ == FOXAPI_SCANNER_IDLE || hz <= 0.0) { return; }
        refreshBookmarks();
        const Bookmark* b = factOf("fact.scanner.listMode") == "1" ? bookmarkAt(hz) : nullptr;
        if (b != nullptr) {
            lockouts_.add(b->id, hz, now, forMs);
            FoxCommand c{};
            c.op = FOXAPP_OP_BOOKMARK_TICK;
            c.ival[0] = b->id;
            c.ival[1] = 0;
            send(c);
            bookmarksVersion_.clear();
        }   // a range scan has no row to untick: the engine's scanner takes a lockout with Params::lockedHz (the hand-off), so the keys are not offered there
    } else if (id == "lock-clear") {
        for (const int bid : lockouts_.clear()) {
            FoxCommand c{};
            c.op = FOXAPP_OP_BOOKMARK_TICK;
            c.ival[0] = bid;
            c.ival[1] = 1;
            send(c);
        }
        bookmarksVersion_.clear();
    } else if (id == "priority") {
        store_.priorityHz = parsePriorityList(text);
        ++fieldsEpoch_;
        markDirty();
    } else if (id == "priority-every") {
        if (const auto v = parsePriorityEvery(text)) { store_.priorityEveryS = *v; markDirty(); }
        ++fieldsEpoch_;
    } else if (id == "mute-scan") {
        store_.muteScan = text == "1";
        markDirty();
    } else if (id == "log-scan") {
        store_.logScan = text == "1";
        markDirty();
    } else if (id == "quick-store") {
        quickStore_ = text == "1";
    } else if (id == "quick") {
        const int i = std::atoi(text.c_str());
        if (i < 0 || i >= kQuickMemories) { return; }
        QuickMemory& q = store_.quick[static_cast<std::size_t>(i)];
        if (quickStore_) {
            q.set = true;
            q.hz = tunedHz_;
            q.mode = mode_;
            q.bandwidthHz = bandwidthHz_;
            quickStore_ = false;
            markDirty();
        } else {
            for (const FoxCommand& c : recallCommands(q)) { send(c); }
        }
    } else if (id == "mem-squelch" || id == "mem-threshold" || id == "mem-offset" || id == "mem-tone") {
        if (memoryBookmark_ < 0 || memoryBookmark_ >= static_cast<int>(bookmarks_.size())) { ++fieldsEpoch_; return; }
        const Bookmark& b = bookmarks_[static_cast<std::size_t>(memoryBookmark_)];
        const std::string key = memoryKey(b.name, b.hz);
        Memory m;
        if (const auto it = store_.memories.find(key); it != store_.memories.end()) { m = it->second; }
        bool unset = false;
        bool ok = true;
        if (id == "mem-squelch") { if (const auto v = parseDbField(text, unset)) { m.squelchDb = *v; } else { ok = false; } }
        else if (id == "mem-threshold") { if (const auto v = parseDbField(text, unset)) { m.thresholdDb = *v; } else { ok = false; } }
        else if (id == "mem-offset") { if (const auto v = parseKhzField(text)) { m.offsetKhz = *v; } else { ok = false; } }
        else { if (const auto v = parseToneField(text)) { m.toneHz = *v; } else { ok = false; } }
        if (ok) {
            if (m.empty()) { store_.memories.erase(key); } else { store_.memories[key] = m; }
            markDirty();
        }
        ++fieldsEpoch_;   // the field shows what is stored (an unreadable entry goes back)
    } else if (id == "clock-cycle") {
        store_.clockMode = store_.clockMode == 1 ? 2 : store_.clockMode == 2 ? 0 : 1;   // UTC -> local -> off -> UTC
        lastClockSecond_ = -1;
        markDirty();
    } else if (id == "rec-folder") {
        const std::string dir = env_.recorderDir ? env_.recorderDir() : std::string();
        lastOpened_ = dir;
#if defined(_WIN32)
        if (!env_.bounded && !dir.empty()) {
            const std::filesystem::path p{std::u8string(dir.begin(), dir.end())};
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            ::ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
#endif
    } else if (id == "rec-open") {
        const int i = std::atoi(text.c_str());
        if (i < 0 || i >= static_cast<int>(recordings_.size())) { return; }
        const RecordingFile& r = recordings_[static_cast<std::size_t>(i)];
        lastOpened_ = r.path;
        if (r.kind == "I/Q" && isIqWav(r.path)) {
            if (env_.openIq) { env_.openIq(r.path); }   // the I/Q file source takes it, as a dropped recording does
            env_.ui->global<::LaneC>().set_recordings_open(false);
        }
#if defined(_WIN32)
        else if (!env_.bounded) {
            const std::filesystem::path p{std::u8string(r.path.begin(), r.path.end())};
            ::ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);   // the system's own player or viewer
        }
#endif
    }
}

// ----------------------------------------------------------------------------------------------------------------------------------------- the tick

void LaneCPublisher::tick(const FoxReceiverState& state, long tickNo) {
    tickNo_ = tickNo;
    auto& lc = env_.ui->global<::LaneC>();
    Shown& sh = *shown_;
    const double now = env_.nowMs ? env_.nowMs() : 0.0;
    const std::int64_t unixNow = env_.unixNow ? env_.unixNow() : 0;
    tunedHz_ = state.tunedHz;
    mode_ = state.demodMode;
    bandwidthHz_ = state.bandwidthHz;
    scanState_ = state.scannerState;
    scanHz_ = state.scanCurrentHz;

    if (tickNo - bookmarksAt_ >= 15) { bookmarksAt_ = tickNo; refreshBookmarks(); }

    // C3: a temporary lockout that has run out ticks its row again
    for (const int bid : lockouts_.due(now)) {
        FoxCommand c{};
        c.op = FOXAPP_OP_BOOKMARK_TICK;
        c.ival[0] = bid;
        c.ival[1] = 1;
        send(c);
        bookmarksVersion_.clear();
    }

    // C5: silence while the scan steps
    const bool userMuted = (state.flags & FOXAPI_RX_MUTED) != 0u;
    if (const std::optional<int> m = mute_.update(store_.muteScan, state.scannerState, userMuted)) {
        FoxCommand c{};
        c.op = FOXAPI_OP_SET_MUTED;
        c.ival[0] = *m;
        send(c);
    }

    // C4
    if (state.scannerState != FOXAPI_SCANNER_IDLE && lastScanState_ == FOXAPI_SCANNER_IDLE) { watcher_.reset(); }
    const std::string line = watcher_.observe(now, state.scannerState, state.scanCurrentHz, unixNow);
    if (!line.empty() && store_.logScan && !env_.scanLogPath.empty()) {
        namespace fs = std::filesystem;
        const fs::path p(std::u8string(env_.scanLogPath.begin(), env_.scanLogPath.end()));
        std::error_code ec;
        const bool fresh = !fs::exists(p, ec);
        std::ofstream out(p, std::ios::binary | std::ios::app);
        if (out) {
            if (fresh) { out << ScanWatcher::csvHeader() << "\n"; }
            out << line << "\n";
            ++logLines_;
        }
    }
    lastScanState_ = state.scannerState;

    // F7: the width each mode was last used at
    {
        bool changed = false;
        if (const std::optional<double> bw = bandwidth_.update(store_.bwMemory, state.demodMode, state.bandwidthHz, tickNo, store_.bwByMode, changed)) {
            FoxCommand c{};
            c.op = FOXAPI_OP_SET_BANDWIDTH;
            c.num[0] = *bw;
            send(c);
        }
        if (changed) { markDirty(); }
    }

    publishMemory(state);

    // the quick memories' labels
    publishQuick();
    if (sh.quickStore != quickStore_) { sh.quickStore = quickStore_; lc.set_quick_store(quickStore_); }

    // the scanner block
    {
        const bool enabled = state.scannerState != FOXAPI_SCANNER_IDLE && factOf("fact.scanner.listMode") == "1";   // only a scan of the ticked rows has a row to untick
        if (sh.first || sh.lockEnabled != enabled) { sh.lockEnabled = enabled; lc.set_lock_enabled(enabled); }
        const std::size_t locks = lockouts_.size();
        const std::string lt = locks == 0 ? std::string() : cascade::core::formatText(cascade::i18n::tr("%d locked out"), static_cast<int>(locks));
        if (sh.first || sh.lockText != lt) { sh.lockText = lt; lc.set_lock_text(ss(lt)); }
        const std::string pr = priorityText(store_.priorityHz);
        const std::string pe = fmt("%.0f", store_.priorityEveryS);
        const bool again = sh.scanEpoch != fieldsEpoch_;   // a commit: the field shows what is stored, even when that is the text it already holds
        sh.scanEpoch = fieldsEpoch_;
        if (again) { sh.priority = pr + " "; lc.set_priority_text(ss(sh.priority)); sh.priorityEvery = pe + " "; lc.set_priority_every_text(ss(sh.priorityEvery)); }
        else {
            if (sh.first || sh.priority != pr) { sh.priority = pr; lc.set_priority_text(ss(pr)); }
            if (sh.first || sh.priorityEvery != pe) { sh.priorityEvery = pe; lc.set_priority_every_text(ss(pe)); }
        }
        if (sh.first || sh.muteScan != store_.muteScan) { sh.muteScan = store_.muteScan; lc.set_mute_scan(store_.muteScan); }
        if (sh.first || sh.logScan != store_.logScan) { sh.logScan = store_.logScan; lc.set_log_scan(store_.logScan); }
        std::string stats;
        if (watcher_.found() > 0) {
            const ScanHit* b = watcher_.busiest();
            stats = cascade::core::formatText(cascade::i18n::tr("Found %d on %zu frequencies; most heard %s (%d)"), watcher_.found(), watcher_.hits().size(), fmt("%.4f MHz", b != nullptr ? b->hz / 1e6 : 0.0).c_str(), b != nullptr ? b->count : 0);
        }
        if (sh.first || sh.scanStats != stats) { sh.scanStats = stats; lc.set_scan_stats(ss(stats)); }
        bool anyThreshold = false;
        for (const auto& [k, m] : store_.memories) { if (!std::isnan(m.thresholdDb)) { anyThreshold = true; } }
        const bool note = (!store_.priorityHz.empty() || anyThreshold) && factOf("fact.scanner.extras") != "1";
        if (sh.first || sh.engineNote != note) { sh.engineNote = note; lc.set_scan_engine_note(note); }
    }

    // T12
    {
        const std::int64_t sec = unixNow;
        if (sec != lastClockSecond_) {
            lastClockSecond_ = sec;
            std::string c = clockText(store_.clockMode, sec, store_.clockMode == 2 ? localOffsetSecondsNow(sec) : 0);
            if (c.empty()) { c = "--:--:--"; }   // the clock is off: the place stays, so a press brings it back
            if (sh.first || sh.clock != c) { sh.clock = c; lc.set_clock_text(ss(c)); }
        }
    }

    // P11: the recorder's folder, listed every second while the sheet is open
    if (lc.get_recordings_open()) {
        if (tickNo - recordingsAt_ >= 30) {
            recordingsAt_ = tickNo;
            const std::string dir = env_.recorderDir ? env_.recorderDir() : std::string();
            std::vector<RecordingFile> list = listRecordings(dir, 200);
            std::string sig;
            for (const RecordingFile& r : list) { sig += r.name + "|" + std::to_string(r.bytes) + "|" + std::to_string(r.unixSeconds) + ";"; }
            if (sig != recordingsSig_) {
                recordingsSig_ = sig;
                recordings_ = std::move(list);
                while (recordingModel_->row_count() > 0) { recordingModel_->erase(recordingModel_->row_count() - 1); }
                int index = 0;
                for (const RecordingFile& r : recordings_) {
                    recordingModel_->push_back(LCRecording{ss(r.name), ss(r.kind), ss(sizeText(r.bytes)), ss(dateText(r.unixSeconds)), r.kind == "I/Q" && isIqWav(r.path), index++});
                }
            }
            const std::string note = recordings_.empty() ? cascade::i18n::tr("No recordings in this folder") : std::string();
            if (sh.recordingsNote != note) { sh.recordingsNote = note; lc.set_recordings_note(ss(note)); }
        }
    }

    // save a moment after the last change
    if (dirty_ && tickNo - dirtySince_ >= 15) { save(); }
    sh.first = false;
}

void LaneCPublisher::publishMemory(const FoxReceiverState& state) {
    auto& lc = env_.ui->global<::LaneC>();
    Shown& sh = *shown_;
    const Bookmark* b = bookmarkAt(state.tunedHz);
    memoryBookmark_ = b != nullptr ? static_cast<int>(b - bookmarks_.data()) : -1;
    const bool enabled = b != nullptr;
    Memory m;
    std::string title, tx;
    if (b != nullptr) {
        const auto it = store_.memories.find(memoryKey(b->name, b->hz));
        if (it != store_.memories.end()) { m = it->second; }
        title = fmt("%.4f MHz", b->hz / 1e6) + "  " + b->name;
        const std::string t = offsetText(b->hz, m);
        if (!t.empty()) { tx = cascade::core::formatText(cascade::i18n::tr("Transmit %s MHz"), t.c_str()); }
        // C12: the receiver has just come onto this bookmark and no scan is running: its squelch goes with it
        if (b->id != memoryId_ && state.scannerState == FOXAPI_SCANNER_IDLE) {
            for (const FoxCommand& c : memoryRecallCommands(m)) { send(c); }
        }
        memoryId_ = b->id;
    } else {
        title = cascade::i18n::tr("Tune to a bookmark to give it a memory");
        memoryId_ = 0;
    }
    lastTunedHz_ = state.tunedHz;
    const std::string sq = dbFieldText(m.squelchDb), th = dbFieldText(m.thresholdDb);
    const std::string off = m.offsetKhz == 0.0 ? std::string() : fmt("%g", m.offsetKhz), tone = m.toneHz == 0.0 ? std::string() : fmt("%g", m.toneHz);
    if (sh.first || sh.memEnabled != enabled) { sh.memEnabled = enabled; lc.set_mem_enabled(enabled); }
    if (sh.first || sh.memTitle != title) { sh.memTitle = title; lc.set_mem_title(ss(title)); }
    // a field the user has just committed is set again even when the stored text is the same, so an unreadable entry goes back to what is stored
    const bool force = sh.fieldsEpoch != fieldsEpoch_;
    sh.fieldsEpoch = fieldsEpoch_;
    // (a field that was typed into keeps its typed text when the same value is set again, so the value is first set to something else and put back a tick later by the comparison below)
    const auto field = [&](std::string& shown, const std::string& want, auto&& setter) {
        if (force) { shown = want + " "; setter(ss(shown)); }
        else if (sh.first || shown != want) { shown = want; setter(ss(want)); }
    };
    field(sh.memSquelch, sq, [&](const slint::SharedString& v) { lc.set_mem_squelch(v); });
    field(sh.memThreshold, th, [&](const slint::SharedString& v) { lc.set_mem_threshold(v); });
    field(sh.memOffset, off, [&](const slint::SharedString& v) { lc.set_mem_offset(v); });
    field(sh.memTone, tone, [&](const slint::SharedString& v) { lc.set_mem_tone(v); });
    if (sh.first || sh.memTx != tx) { sh.memTx = tx; lc.set_mem_tx(ss(tx)); }
}

void LaneCPublisher::publishQuick() {
    std::string sig;
    for (int i = 0; i < kQuickMemories; ++i) { const QuickMemory& q = store_.quick[static_cast<std::size_t>(i)]; sig += quickLabel(i, q) + (q.set ? "+" : "-") + ";"; }
    if (sig == shown_->quickSig) { return; }
    shown_->quickSig = sig;
    for (int i = 0; i < kQuickMemories; ++i) {
        const QuickMemory& q = store_.quick[static_cast<std::size_t>(i)];
        quickModel_->set_row_data(static_cast<std::size_t>(i), LCQuick{ss(quickLabel(i, q)), q.set});
    }
}

// ----------------------------------------------------------------------------------------------------------------------------------------- the drive scripts' reads

std::string LaneCPublisher::readState(const std::string& w, std::string& out) const {
    const auto num = [](double v) { return fmt("%.6f", v); };
    if (w == "steplock") { out = store_.stepLock ? "1" : "0"; }
    else if (w == "deltamask") { out = std::to_string(store_.deltaMask); }
    else if (w == "deltarefusals") { out = std::to_string(deltaRefusals()); }
    else if (w == "deltaconversions") { out = std::to_string(deltaConversions()); }
    else if (w == "bwmemory") { out = store_.bwMemory ? "1" : "0"; }
    else if (w.rfind("bw.", 0) == 0) { const auto it = store_.bwByMode.find(static_cast<std::uint32_t>(std::atoi(w.c_str() + 3))); out = it == store_.bwByMode.end() ? "0" : fmt("%.0f", it->second); }
    else if (w == "priority") { out = priorityText(store_.priorityHz); }
    else if (w == "priorityevery") { out = fmt("%.0f", store_.priorityEveryS); }
    else if (w == "mutescan") { out = store_.muteScan ? "1" : "0"; }
    else if (w == "mutedbyus") { out = mute_.weMuted() ? "1" : "0"; }
    else if (w == "logscan") { out = store_.logScan ? "1" : "0"; }
    else if (w == "loglines") { out = std::to_string(logLines_); }
    else if (w == "clockmode") { out = std::to_string(store_.clockMode); }
    else if (w == "clock") { out = shown_->clock; }
    else if (w.rfind("quick.", 0) == 0) { const int i = std::atoi(w.c_str() + 6); out = i >= 0 && i < kQuickMemories && store_.quick[static_cast<std::size_t>(i)].set ? num(store_.quick[static_cast<std::size_t>(i)].hz) : "0"; }
    else if (w == "quickstore") { out = quickStore_ ? "1" : "0"; }
    else if (w == "memenabled") { out = shown_->memEnabled ? "1" : "0"; }
    else if (w == "memtitle") { out = shown_->memTitle; }
    else if (w == "memsquelch") { out = shown_->memSquelch; }
    else if (w == "memthreshold") { out = shown_->memThreshold; }
    else if (w == "memoffset") { out = shown_->memOffset; }
    else if (w == "memtone") { out = shown_->memTone; }
    else if (w == "memtx") { out = shown_->memTx; }
    else if (w == "memories") { out = std::to_string(store_.memories.size()); }
    else if (w == "lockouts") { out = std::to_string(lockouts_.size()); }
    else if (w == "scanstate") { out = std::to_string(scanState_); }
    else if (w == "scanhz") { out = num(scanHz_); }
    else if (w == "locktext") { out = shown_->lockText; }
    else if (w == "scanfound") { out = std::to_string(watcher_.found()); }
    else if (w == "scanstats") { out = shown_->scanStats; }
    else if (w == "enginenote") { out = shown_->engineNote ? "1" : "0"; }
    else if (w == "shortcuts") { out = std::to_string(shortcutModel_->row_count()); }
    else if (w == "shortcutsopen") { out = env_.ui->global<::LaneC>().get_shortcuts_open() ? "1" : "0"; }
    else if (w == "recordings") { out = std::to_string(recordings_.size()); }
    else if (w == "recordingsopen") { out = env_.ui->global<::LaneC>().get_recordings_open() ? "1" : "0"; }
    else if (w == "opened") { out = lastOpened_; }
    else if (w == "saved") { out = dirty_ ? "0" : "1"; }
    else { return "lanec.steplock|deltamask|deltarefusals|deltaconversions|bwmemory|bw.<mode>|priority|priorityevery|mutescan|mutedbyus|logscan|loglines|clockmode|clock|quick.<n>|quickstore|memenabled|memtitle|memsquelch|memthreshold|memoffset|memtone|memtx|memories|lockouts|scanstate|scanhz|locktext|scanfound|scanstats|enginenote|shortcuts|shortcutsopen|recordings|recordingsopen|opened|saved, not " + w; }
    return std::string();
}

}  // namespace foxsdr::slintlanec
