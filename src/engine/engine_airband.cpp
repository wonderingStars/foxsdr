// engine_airband.cpp - THE AIRBAND MONITOR AND THE SCANNER'S LIST MODE, in the engine (stub campaign B6, AIRBAND-A; master 0.99.60 - 0.99.71).
//
// THE REQUEST (a US tester, 2026-10-01): "a plug in that you could simply enter the airport code and it would fill out a list where you could
// select which ones to listen to. If they are close enough together have multiple AM demodulators running simultaneously. If they are not close
// enough together could the SDR scan across the frequencies stopping when it sees a peak and tuning in. When the audio stops, continue
// scanning." And: "there is no way to know which ones are more popular."
//
// WHAT IS HERE, and where master had it (src/gui_pending/app_window_airband.cpp is the window half B7 brings back; the DSP, the radio and the
// bookmark store were the ENGINE's even in master - the window only drove them, from its frame):
//   - the ROWS the monitor plays: the ticked rows of the preset whose mode is exactly AM or NFM, ONE CHANNEL PER FREQUENCY (an AM and an NFM row
//     on one frequency are one channel on the air: two strips would play it twice as loud for twice the work), named - and its heard time
//     credited - by the FIRST ticked row on it;
//   - the BLOCKS: core/airband_monitor.hpp cuts the channels into blocks the radio's band can hold (24 - 96 kHz strips off one wide capture, kept
//     off the DC spike) and builds the set that plays a block - a strip and a squelch to a channel, every channel's gated audio mixed to the
//     speakers, an NFM channel through the 50 us de-emphasis the receiver's own NFM runs;
//   - the RADIO: the pipeline's patch runner on the receiver's radio, which the patch view otherwise has. THE MONITOR AND THE PATCH SHARE ONE
//     RADIO AND ONE RUNNER, so they are never on it at once: LISTEN with the patch view showing closes it (the receiver view) and waits for the
//     patch to hand the radio back - patchWasOpen_ down, no open in flight - before it starts; the patch view showing again stops it;
//   - the SCAN: with more than one block a second core::Scanner, in list mode, walks the block centres (stops where anybody is talking, moves on
//     once the block has been quiet for the hold time);
//   - the HEARD TIME ("which ones are busy"): every second a channel's squelch is open while the monitor plays it is added to that row's
//     heardSeconds - ONE WRITER, this thread, flushed to the list every 20 s and at STOP and exit; the list is saved on a worker from a snapshot
//     taken on this thread (flushBookmarkSave), as it always was;
//   - the scanner's LIST MODE (Scanner::Params::list): the ticked rows of the list stand in for the range, each tuned with its own mode and width.
//
// WHAT THE TABLE SEES. FOXAPP_OP_AIRBAND (listen, stop, next block, above the noise, squelch, hold, tune one), FOXAPP_OP_BOOKMARK_TICK,
// FOXAPP_OP_BOOKMARK_ADD_ROWS and FOXAPP_OP_SCANNER_LIST are the commands (docs/api-0.3-additions.md 9.6); the monitor's facts are
// fact.airband.* and fact.scanner.listMode / .ticked; the channels are the live list AIRBAND_CHANNELS (51), which moves no listSeq; a row's tick
// and heard time are BOOKMARKS' FOXAPI_ITEM_TICKED and value[3].
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/diag_log.hpp"
#include "core/freq_converter.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/patch_plan.hpp"
#include "core/utf8_text.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/tune_control.hpp"

namespace cascade::engine {

namespace cc = cascade::core;
using cascade::i18n::tr;

namespace {

// How long the monitor sits on a block before judging it empty: the radio's retune, the first filter filling and the squelch's attack all fit
// inside it, with room left to hear the start of a call.
constexpr double kAirbandDwellMs = 350.0;
// The grace after a block's hold has run out before the next retune, so a reply that starts a moment after the last word is still caught.
constexpr double kAirbandResumeMs = 150.0;
// A set that has run no block this long has been dropped (a plugin rescan flushes the runner) or starved: publish it again.
constexpr double kAirbandWatchdogS = 2.0;
// Squelch reports from a set that has run nothing for this long are history.
constexpr double kAirbandStaleS = 0.5;
// How long after a publish reports are ignored: the samples already captured at the previous centre are still being worked through with the new
// offsets, and a hold made on them would be a hold on noise.
constexpr double kAirbandSettleS = 0.15;
// heardSeconds is written to the list this often, not every pass: the list saves to disk a second after any change.
constexpr double kAirbandFlushS = 20.0;

// The rows the monitor plays: AM, and since 0.99.66 NFM. Any other mode (WFM, SSB, CW...) is not an airband row: it is not played, and a ticked
// WFM row (a broadcast station) is a row of the scanner's list mode and nothing else. EXACTLY "AM" and "NFM" - not a near miss rounded to one.
bool playsInMonitor(const cc::Bookmark& b) {
    cc::MonitorMode m = cc::MonitorMode::Am;
    return cc::monitorModeFor(b.mode, m);
}

// A line's tab-separated fields, at most `maxFields`; the last takes the rest of the line, so a name keeps whatever is in it.
std::vector<std::string> tabFields(const std::string& line, std::size_t maxFields) {
    std::vector<std::string> out;
    std::size_t from = 0;
    while (out.size() + 1 < maxFields) {
        const std::size_t at = line.find('\t', from);
        if (at == std::string::npos) { break; }
        out.push_back(line.substr(from, at - from));
        from = at + 1;
    }
    out.push_back(line.substr(from));
    return out;
}

}  // namespace

// ============================================================================================================================
// THE SCANNER'S LIST MODE
// ============================================================================================================================

std::vector<double> Engine::tickedScanList() const {
    std::vector<double> out;
    for (const cc::Bookmark& b : freqMgr_.list()) {
        if (b.scan) { out.push_back(b.freqHz); }
    }
    return out;   // already ascending: the list is kept sorted
}

std::size_t Engine::tickedScanCount() const {
    if (tickedCountVersion_ != freqMgr_.version()) {
        std::size_t n = 0;
        for (const cc::Bookmark& b : freqMgr_.list()) { n += b.scan ? 1u : 0u; }
        tickedCount_ = n;
        tickedCountVersion_ = freqMgr_.version();
    }
    return tickedCount_;
}

void Engine::configureScanner() {
    cc::Scanner::Params p = scannerParams();
    if (scanTicked_) {
        scanListVersion_ = freqMgr_.version();
        scanList_ = p.list;
    }
    scanner_.configure(std::move(p));
    // The reconfigure re-emits a first tune on the next tick; the user-tune baseline re-arms from that retune's readback.
    scannerHasExpected_ = false;
}

std::int32_t Engine::applyScannerListOp(const FoxCommand& c, std::string& why) {
    switch (c.ival[0]) {
        case 0:
        case 1:
            // THE CHECKBOX: the range fields stand aside in list mode. Either way a running scan stops - what it was scanning is no longer what the
            // scanner is set to.
            scanTicked_ = c.ival[0] == 1;
            if (scanner_.active()) { scanner_.stop(); }
            return FOXAPI_OK;
        case 2: {
            if (tickedScanCount() == 0) {
                why = "no frequency is ticked";
                return FOXAPI_NO_CHANGE;
            }
            scanTicked_ = true;
            // One radio, one plan: the Airband monitor stands down.
            if (airbandListening_ || airbandStartPending_) { airbandStop(std::string()); }
            const bool wasActive = scanner_.active();
            configureScanner();
            if (!wasActive) { scanner_.start(clockS() * 1000.0); }
            return FOXAPI_OK;
        }
        default:
            why = "ival[0] is 0 (range mode), 1 (list mode) or 2 (start a list scan)";
            return FOXAPI_OUT_OF_RANGE;
    }
}

// ============================================================================================================================
// THE FREQUENCY LIST'S TICK AND THE ROWS TYPED IN
// ============================================================================================================================

std::int32_t Engine::applyBookmarkTick(const FoxCommand& c, std::string& why) {
    // BY ID, re-found in the LIVE list: the list may have moved since the command was made (an import, a browser's add).
    const std::size_t i = freqMgr_.indexOfId(static_cast<std::uint64_t>(c.ival[0]));
    if (i >= freqMgr_.list().size()) {
        why = "no such bookmark";
        return FOXAPI_NOT_FOUND;
    }
    const bool want = c.ival[1] != 0;
    if (freqMgr_.list()[i].scan == want) { return FOXAPI_OK; }   // as asked already: nothing changes, nothing is written
    cc::Bookmark b = freqMgr_.list()[i];
    b.scan = want;
    freqMgr_.updateAt(i, b);   // (the frequency is the same: the row stays where it is, ahead of any peer on it)
    saveBookmarks();
    return FOXAPI_OK;
}

namespace {
// A group name as typed, less the blanks around it: "  LUGANO " and "LUGANO" are one group, and a name of blanks alone is no group (master's
// trimmedGroupName, 97ea658c, which the window did before the engine owned the list).
std::string trimmedGroupName(const std::string& typed) {
    const auto blank = [](char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
    std::size_t lo = 0;
    std::size_t hi = typed.size();
    while (hi > lo && blank(typed[hi - 1])) { --hi; }
    while (lo < hi && blank(typed[lo])) { ++lo; }
    return typed.substr(lo, hi - lo);
}
}  // namespace

std::int32_t Engine::applyBookmarkGroup(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    const auto split = [&text](std::string& a, std::string& b) {
        const std::size_t at = text.find('\x1F');
        if (at == std::string::npos) { return false; }
        a = text.substr(0, at);
        b = text.substr(at + 1);
        return true;
    };
    switch (c.ival[0]) {
        case 0: {   // ADD CURRENT IN A GROUP
            std::string group;
            std::string name;
            if (!split(group, name)) {
                why = "text is \"<group>\\x1F<name>\"";
                return FOXAPI_BAD_ARGUMENT;
            }
            res.applied[0] = static_cast<double>(addBookmarkHere(name, trimmedGroupName(group)));
            return FOXAPI_OK;
        }
        case 1: {   // MOVE ONE BOOKMARK
            // BY ID, re-found in the LIVE list, as every row op is.
            const std::size_t i = freqMgr_.indexOfId(static_cast<std::uint64_t>(c.ival[1]));
            if (i >= freqMgr_.list().size()) {
                why = "no such bookmark";
                return FOXAPI_NOT_FOUND;
            }
            const std::string to = trimmedGroupName(text);
            if (freqMgr_.list()[i].group == to) { return FOXAPI_OK; }   // already there: nothing changes, nothing is written
            cc::Bookmark b = freqMgr_.list()[i];
            b.group = to;
            freqMgr_.updateAt(i, b);   // (the frequency is the same: the row stays where it is)
            saveBookmarks();
            return FOXAPI_OK;
        }
        case 2: {   // RENAME A GROUP
            std::string from;
            std::string rawTo;
            if (!split(from, rawTo)) {
                why = "text is \"<from>\\x1F<to>\"";
                return FOXAPI_BAD_ARGUMENT;
            }
            const std::string to = trimmedGroupName(rawTo);
            if (from.empty() || to.empty()) {
                why = "a group needs a name (the row menu's \"No group\" is how a group is emptied)";
                return FOXAPI_BAD_ARGUMENT;
            }
            if (from == to) { return FOXAPI_OK; }   // the same name is a no-op
            const std::size_t n = freqMgr_.renameGroup(from, to);
            if (n == 0) {
                why = "no bookmark is in that group";
                return FOXAPI_NOT_FOUND;
            }
            // THE PRESET THE MONITOR IS PLAYING KEEPS PLAYING under its new name (airbandRows cuts the plan by group name): a rename must not turn
            // "this preset" into "every ticked row" at the next block. Master's window had the same hazard; the engine closes it.
            if (airbandGroup_ == from) { airbandGroup_ = to; }
            bookmarkImportNote_ = cc::formatText(tr("Renamed \"%s\" to \"%s\" (%zu)"), from.c_str(), to.c_str(), n);
            saveBookmarks();
            res.applied[0] = static_cast<double>(n);
            return FOXAPI_OK;
        }
        default:
            why = "ival[0] is 0 (add current in a group), 1 (move one) or 2 (rename a group)";
            return FOXAPI_OUT_OF_RANGE;
    }
}

std::int32_t Engine::applyBookmarkAddRows(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    if (c.ival[0] != 0 && c.ival[0] != 1) {
        why = "ival[0] is 0 (rows typed in) or 1 (a list)";
        return FOXAPI_OUT_OF_RANGE;
    }
    const bool typed = c.ival[0] == 0;
    std::vector<cc::Bookmark> items;
    std::size_t from = 0;
    while (from <= text.size()) {
        std::size_t to = text.find('\n', from);
        if (to == std::string::npos) { to = text.size(); }
        std::string line = text.substr(from, to - from);
        from = to + 1;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.empty()) { continue; }
        const std::vector<std::string> f = tabFields(line, 6);
        if (f.size() < 4) { continue; }   // a row names at least its frequency, mode, width and flags (tick, favourite)
        char* end = nullptr;
        const double hz = std::strtod(f[0].c_str(), &end);
        if (end == f[0].c_str() || !std::isfinite(hz) || !(hz > 0.0)) { continue; }
        cc::Bookmark b;
        b.freqHz = hz;
        b.mode = f[1].empty() ? std::string("AM") : f[1];
        const double bw = std::strtod(f[2].c_str(), &end);
        b.bandwidthHz = (end != f[2].c_str() && std::isfinite(bw) && bw > 0.0) ? bw : cc::defaultBandwidthForMode(b.mode);
        // THE FOURTH FIELD IS A WORD OF FLAGS (stub campaign B19): bit 0 ticked, bit 1 favourite. "0" and "1" are what every sender wrote before the
        // favourite was carried, and mean what they meant; a file's favourite column therefore survives an import into a preset.
        const long flags = std::strtol(f[3].c_str(), &end, 10);
        const bool flagsOk = end != f[3].c_str() && flags >= 0 && flags <= 3;
        b.scan = flagsOk && (flags & 1) != 0;
        b.favourite = flagsOk && (flags & 2) != 0;
        if (f.size() > 4) { b.group = f[4]; }
        if (f.size() > 5) { b.name = f[5]; }
        if (typed && b.name.empty()) {
            // A nameless row would render blank: the frequency, as "Add current" names one.
            char def[32];
            std::snprintf(def, sizeof(def), "%.4f MHz", hz / 1.0e6);
            b.name = def;
        }
        items.push_back(std::move(b));
    }
    if (items.empty()) {
        why = "no row in the text";
        return FOXAPI_BAD_ARGUMENT;
    }
    if (typed) {
        // THE SAME RULE AS A TUNE THE RADIO REFUSES (noteTuneRefused): a frequency outside what the radio publishes it covers is not accepted, and
        // the note says what it does cover. Judged at the RADIO, so a converter's LO is taken out first. A radio that publishes no range (the
        // generator, an I/Q file) refuses nothing; the list is the user's. Every row is judged before any is added.
        const cc::ConverterSetting conv = pipeline_.converter();
        double rangeLoHz = 0.0;
        double rangeHiHz = 0.0;
        const bool hasRange = device_ != nullptr && device_->frequencyRangeHz(rangeLoHz, rangeHiHz);
        for (const cc::Bookmark& b : items) {
            if (cascade::gui::radioCanTune(cc::radioFromAir(conv, b.freqHz), hasRange, rangeLoHz, rangeHiHz)) { continue; }
            std::string note;
            if (cc::converterActive(conv)) { note = converterTuneNote(b.freqHz, /*refused=*/true, 0.0, false); }
            if (note.empty()) {
                note = cc::formatText(tr("This radio cannot tune to %s. Its range is %s to %s."), cc::converterHzText(b.freqHz).c_str(),
                                      cc::converterHzText(rangeLoHz).c_str(), cc::converterHzText(rangeHiHz).c_str());
            }
            why = note;
            return FOXAPI_OUT_OF_RANGE;
        }
    }
    std::size_t added = 0;
    if (typed) {
        for (cc::Bookmark& b : items) {
            freqMgr_.add(std::move(b));
            ++added;
        }
    } else {
        added = freqMgr_.addMany(std::move(items));
    }
    if (added > 0) { saveBookmarks(); }
    res.applied[0] = static_cast<double>(added);
    if (typed) {
        cc::diagLogf("airband: a frequency was typed into a preset");
    } else {
        cc::diagLogf("airband: a list of frequencies was added (%zu new)", added);
    }
    return FOXAPI_OK;
}

// ============================================================================================================================
// THE ROWS THE MONITOR PLAYS
// ============================================================================================================================

std::vector<cc::MonitorChannel> Engine::airbandWanted(std::vector<AirbandRow>* rows) const {
    // ONE CHANNEL PER FREQUENCY. Two ticked rows on one frequency (CTAF and UNICOM share 122.8 at many fields; an AM and an NFM row typed on the
    // same frequency) are one channel on the air: two strips would play it twice as loud for twice the work, and a carrier is one transmitter
    // whatever the rows call it. The first ticked row (the list is sorted, ties in insertion order) names it and gives it its mode and width -
    // and is the row its heard time is credited to.
    std::vector<cc::MonitorChannel> out;
    if (rows != nullptr) { rows->clear(); }
    for (const cc::Bookmark& b : freqMgr_.list()) {
        if (!b.scan || !playsInMonitor(b)) { continue; }
        if (!airbandGroup_.empty() && b.group != airbandGroup_) { continue; }
        cc::MonitorChannel ch{b.freqHz, b.bandwidthHz};
        if (!cc::monitorModeFor(b.mode, ch.mode)) { continue; }
        if (!out.empty() && out.back().freqHz == b.freqHz) { continue; }
        out.push_back(ch);
        if (rows != nullptr) { rows->push_back({b.name, b.id}); }
    }
    return out;
}

std::string Engine::airbandBlockText() const {
    if (!airbandListening_ || airbandBlock_ >= airbandBlocks_.size()) { return std::string(); }
    const cc::AirbandBlock& blk = airbandBlocks_[airbandBlock_];
    double lo = 0.0;
    double hi = 0.0;
    if (!blk.members.empty() && blk.members.back() < airbandPlanned_.size()) {
        lo = airbandPlanned_[blk.members.front()].freqHz;
        hi = airbandPlanned_[blk.members.back()].freqHz;
    }
    if (airbandBlocks_.size() > 1) {
        return cc::formatText(tr("Block %zu of %zu: %zu channels, %.3f-%.3f MHz"), airbandBlock_ + 1, airbandBlocks_.size(),
                              blk.members.size(), lo / 1e6, hi / 1e6);
    }
    return cc::formatText(tr("All %zu channels at once, %.3f-%.3f MHz"), blk.members.size(), lo / 1e6, hi / 1e6);
}

// ============================================================================================================================
// THE MONITOR
// ============================================================================================================================

int Engine::airbandStart() {
    airbandStartPending_ = false;
    std::vector<AirbandRow> rows;
    const std::vector<cc::MonitorChannel> want = airbandWanted(&rows);
    if (want.empty()) {
        airbandNote_ = tr("Tick at least one AM or NFM frequency to listen to.");
        return 0;
    }
    // THE PATCH VIEW HAS THE RECEIVER'S RADIO: hand it back first, then start once the patch has let go (airbandFrame watches for it). LISTEN
    // SWITCHES TO THE RECEIVER VIEW, which is what closing the patch view is (pumpPatch's close rule: the patch stops, the receiver gets its radio
    // back); a window follows the engine's wish (fact.airband.pending). A radio that is being opened is between sources too: the same wait.
    if (patchPageOpen_ || patchWasOpen_ || deviceOpenPending_) {
        patchPageOpen_ = false;
        airbandStartPending_ = true;
        airbandNote_.clear();
        return 2;
    }
    if (scanner_.active()) { scanner_.stop(); }
    if (!pipeline_.running()) { startReceiver(); }
    const double rate = pipeline_.activeSource().sampleRateHz();
    if (!(rate > 0.0) || !cc::patch::chooseChannelRate(rate).ok) {
        airbandNote_ = tr("The receiver's sample rate cannot be divided down to an audio rate - choose another rate in the Source section.");
        return 0;
    }
    airbandPlanned_ = want;
    airbandBlocks_ = cc::planAirbandBlocks(want, rate, cc::patch::kUsableBandFraction, cc::monitorChannelsPerBlock(rate));
    if (airbandBlocks_.empty()) {
        airbandNote_ = tr("None of the ticked frequencies can be tuned.");
        return 0;
    }
    // The channel table, index for index with airbandPlanned_ (the blocks' member indices point into both).
    airbandChans_.clear();
    for (std::size_t i = 0; i < want.size(); ++i) {
        AirbandChan c;
        c.freqHz = want[i].freqHz;
        c.bandwidthHz = want[i].bandwidthHz;
        c.name = rows[i].name;
        c.rowId = rows[i].id;
        airbandChans_.push_back(std::move(c));
    }
    airbandRateHz_ = rate;
    airbandSourceDevice_ = device_;
    airbandSourceKind_ = sourceKind_;
    airbandTuned_ = false;
    // A FRESH SESSION STARTS FROM BLOCK 0 WITH A FRESH CLOCK. Carried over from the last session, the block index could be past the end of the new
    // plan and the watchdog's clock long expired: the watchdog then republished a block that does not exist on every pass, the scanner never
    // ticked, and LISTEN played nothing for good (review, 2026-10-01).
    airbandBlock_ = 0;
    airbandPublishedS_ = clockS();
    airbandProgressS_ = airbandPublishedS_;
    airbandBlocksSeen_ = pipeline_.patchRunner().blocksRun();
    airbandListening_ = true;
    airbandNote_.clear();
    airbandListVersion_ = freqMgr_.version();
    airbandLastFrameS_ = clockS();
    airbandFlushDueS_ = airbandLastFrameS_ + kAirbandFlushS;
    cc::diagLogf("airband: listening to %zu channel(s) in %zu block(s)", want.size(), airbandBlocks_.size());
    if (airbandBlocks_.size() == 1) {
        airbandScanner_.stop();
        airbandTuneBlock(0);
        return airbandListening_ ? 1 : 0;   // (a block the radio would not tune to has already stopped it, and said why)
    }
    cc::Scanner::Params p;
    for (const cc::AirbandBlock& b : airbandBlocks_) { p.list.push_back(b.centreHz); }
    p.dwellMs = kAirbandDwellMs;
    p.holdMs = airbandHoldS_ * 1000.0;
    p.resumeMs = kAirbandResumeMs;
    airbandScanner_.configure(std::move(p));
    airbandScanner_.start(clockS() * 1000.0);
    // The first tune comes from the scanner's first tick, in airbandFrame.
    return 1;
}

void Engine::airbandStop(const std::string& why) {
    const bool wasPending = airbandStartPending_;
    airbandStartPending_ = false;
    if (!airbandListening_) {
        // A LISTEN that was still waiting for the patch is cancelled, and says why.
        if (wasPending) { airbandNote_ = why; }
        return;
    }
    airbandListening_ = false;
    airbandScanner_.stop();
    pipeline_.patchRunner().clear();
    airbandFlushHeard();
    for (AirbandChan& c : airbandChans_) {
        c.open = false;
        c.levelDb = -200.0f;
    }
    airbandBuilt_.clear();
    airbandNote_ = why;
    cc::diagLogf("airband: stopped");
}

void Engine::airbandTuneBlock(std::size_t index) {
    if (index >= airbandBlocks_.size()) { return; }
    airbandBlock_ = index;
    const cc::AirbandBlock& blk = airbandBlocks_[index];
    // Applied NOW rather than through the retune coalescer: the readback below is the centre the strips are offset from, and a deferred tune
    // would make it the previous block's.
    applyRetuneNow(blk.centreHz, false);
    airbandCentreHz_ = pipeline_.activeSource().centerFrequencyHz();
    airbandTuned_ = true;
    // WHERE IT LANDED, NOT WHERE IT WAS ASKED: a radio that refused the tune, coerced it (a B200 asked for 7 MHz answered 30.8), or has no tuner at
    // all would leave channels outside the band - their strips would alias other frequencies and credit the wrong rows with what they heard.
    {
        const double half = 0.5 * cc::patch::kUsableBandFraction * airbandRateHz_;
        for (const std::size_t m : blk.members) {
            const cc::MonitorChannel& c = airbandPlanned_[m];
            if (std::fabs(c.freqHz - airbandCentreHz_) + 0.5 * c.bandwidthHz > half + 1.0) {
                airbandStop(tr("Stopped: the radio could not tune to where the ticked frequencies are."));
                return;
            }
        }
    }
    // Fresh channel identities for every set, so a squelch report from the set this one replaces can never be read as this one's.
    airbandFirstId_ = airbandNextId_;
    airbandNextId_ += static_cast<cc::patch::NodeId>(cc::kMaxBlockChannels);
    if (airbandNextId_ > 0xF0000000u) { airbandNextId_ = 0x40000000u; }
    std::shared_ptr<cc::patch::StripSet> set =
        cc::buildMonitorSet(airbandPlanned_, blk, airbandCentreHz_, airbandRateHz_, airbandSquelchDb_, airbandFirstId_);
    if (!set) {
        airbandStop(tr("The receiver's sample rate cannot be divided down to an audio rate."));
        return;
    }
    // WHAT WAS BUILT, read off the set as built, before it is on the air.
    airbandBuilt_.clear();
    for (std::size_t i = 0; i < set->channels.size() && i < blk.members.size(); ++i) {
        const cc::patch::RunningChannel& ch = set->channels[i];
        AirbandBuilt b;
        b.freqHz = airbandPlanned_[blk.members[i]].freqHz;
        b.nfm = ch.mode == cc::patch::Demod::Fm;
        b.deemphasisUs = ch.strip.deemphasisUs();
        b.mixLevel = static_cast<double>(ch.mixLevel);
        b.squelchDb = ch.squelchDb;
        b.node = ch.node;
        airbandBuilt_.push_back(b);
    }
    pipeline_.patchRunner().publish(std::move(set));
    airbandPublishedS_ = clockS();
    airbandProgressS_ = airbandPublishedS_;
    for (AirbandChan& c : airbandChans_) {
        c.open = false;
        c.levelDb = -200.0f;
    }
}

// THE MONITOR'S "ABOVE THE NOISE" RULE, the one place it is written: the Airband section's key calls it once, and the top bar's SQUELCH AUTO calls it
// every quarter second while it is lit and the monitor listens (0.99.74, stub campaign B12). 8 dB over the MEDIAN channel level: most channels are
// quiet most of the time, so the median is the noise, and a talker sits well above. Sets airbandSquelchDb_ and rebuilds the block's set with it
// (airbandTuneBlock - the runner's live squelch slots belong to the patch page). False, and nothing done, when no channel has reported a level yet or
// the result is within `minMoveDb` of where the threshold is: a rebuild restarts the block's channels, which AUTO must not do for a wobble of a tenth
// of a decibel.
bool Engine::airbandAboveTheNoise(float minMoveDb) {
    std::vector<float> lv;
    for (const AirbandChan& c : airbandChans_) {
        if (c.levelDb > -199.0f) { lv.push_back(c.levelDb); }
    }
    if (lv.empty()) { return false; }
    std::nth_element(lv.begin(), lv.begin() + static_cast<std::ptrdiff_t>(lv.size() / 2), lv.end());
    const float db = std::clamp(lv[lv.size() / 2] + 8.0f, -110.0f, 0.0f);
    if (std::fabs(db - airbandSquelchDb_) < minMoveDb) { return false; }
    airbandSquelchDb_ = db;
    airbandTuneBlock(airbandBlock_);
    return true;
}

// THE HEARD TIME, into the list. Each channel's goes to ITS row - the first ticked row on its frequency, held by id - and to no other: a row that
// shares the frequency, or the name, is another row. A row that has left the list takes its heard time with it. The one writer of heardSeconds.
void Engine::airbandFlushHeard() {
    bool changed = false;
    for (AirbandChan& c : airbandChans_) {
        if (!(c.pendingHeardS > 0.0)) { continue; }
        const std::size_t i = freqMgr_.indexOfId(c.rowId);
        if (i < freqMgr_.list().size()) {
            cc::Bookmark b = freqMgr_.list()[i];
            b.heardSeconds += c.pendingHeardS;
            freqMgr_.updateAt(i, b);
            changed = true;
        }
        c.pendingHeardS = 0.0;
    }
    if (changed) { saveBookmarks(); }
}

void Engine::airbandShutdown() { airbandFlushHeard(); }

void Engine::airbandFrame() {
    const double nowS = clockS();
    if (airbandStartPending_) {
        // The patch view was shown again before it let go: the user's hand wins, and the monitor says so rather than take the radio back.
        if (patchPageOpen_) {
            airbandStop(tr("Stopped: the patch view took the radio."));
            return;
        }
        // The patch has let go of the radio AND the receiver has it open again (the hand-back reopens it on a worker).
        if (!patchWasOpen_ && !deviceOpenPending_) { airbandStart(); }
        return;
    }
    if (!airbandListening_) { return; }
    // THE PATCH VIEW TOOK THE RADIO: the monitor stops (the patch view and the monitor never share the runner).
    if (patchPageOpen_) {
        airbandStop(tr("Stopped: the patch view took the radio."));
        return;
    }
    // A radio being opened is between sources: judge nothing until it lands.
    if (deviceOpenPending_) { return; }
    // A different radio (a reopen, a source switch): the blocks were cut for the old one's rate and the strips are offset from its centre.
    if (device_ != airbandSourceDevice_ || sourceKind_ != airbandSourceKind_) {
        airbandStop(std::string());
        airbandStart();
        return;
    }
    if (scanner_.active()) {
        airbandStop(tr("Stopped: the scanner is running."));
        return;
    }
    if (!pipeline_.running()) {
        airbandStop(tr("Stopped: the receiver stopped."));
        return;
    }
    // USER WINS, as with the scanner: a centre the monitor did not tune to is the user's hand on the dial, and the strips' offsets no longer mean
    // anything. (airbandTuned_ is false until the first block is tuned: with several blocks that waits for the scanner's first tick.)
    if (airbandTuned_ && std::fabs(pipeline_.activeSource().centerFrequencyHz() - airbandCentreHz_) > 1.0) {
        airbandStop(tr("Stopped: the receiver was retuned."));
        return;
    }
    // A new sample rate, or a different set of ticked rows: cut the blocks again.
    if (pipeline_.activeSource().sampleRateHz() != airbandRateHz_) {
        airbandStop(std::string());
        airbandStart();
        return;
    }
    // The rows are looked at again only when the LIST moved (a tick, an add, a removal, a heard time written): a list of tens of thousands is not
    // walked on every pass for nothing.
    if (freqMgr_.version() != airbandListVersion_) {
        airbandListVersion_ = freqMgr_.version();
        const std::vector<cc::MonitorChannel> want = airbandWanted();
        bool same = want.size() == airbandPlanned_.size();
        for (std::size_t i = 0; same && i < want.size(); ++i) {
            same = want[i].freqHz == airbandPlanned_[i].freqHz && want[i].bandwidthHz == airbandPlanned_[i].bandwidthHz &&
                   want[i].mode == airbandPlanned_[i].mode;
        }
        if (!same) {
            airbandStop(std::string());
            if (!want.empty()) { airbandStart(); }
            return;
        }
    }

    // What every channel of the block on the air is doing.
    const double dt = std::max(0.0, std::min(1.0, nowS - airbandLastFrameS_));
    airbandLastFrameS_ = nowS;
    // IS THE SET RUNNING? The runner counts the blocks it processes; a count that has not moved is a set that is not running - flushed by a plugin
    // rescan, or a stalled radio - and its last squelch reports are history, not news. Reports are also ignored for a moment after each publish:
    // samples captured at the previous centre are still being worked through with the new offsets, and a hold made on them would be a hold on
    // noise.
    const std::uint64_t ran = pipeline_.patchRunner().blocksRun();
    if (ran != airbandBlocksSeen_) {
        airbandBlocksSeen_ = ran;
        airbandProgressS_ = nowS;
    }
    const bool live = nowS - airbandProgressS_ < kAirbandStaleS;
    const bool settled = nowS - airbandPublishedS_ >= kAirbandSettleS;
    bool anyOpen = false;
    if (airbandBlock_ < airbandBlocks_.size()) {
        const cc::AirbandBlock& blk = airbandBlocks_[airbandBlock_];
        for (std::size_t m = 0; m < blk.members.size(); ++m) {
            float lvl = -200.0f;
            bool open = false;
            const bool reported = live && settled &&
                                  pipeline_.patchRunner().squelchState(airbandFirstId_ + static_cast<cc::patch::NodeId>(m), lvl, open);
            const std::size_t ci = blk.members[m];
            if (ci >= airbandChans_.size()) { continue; }
            AirbandChan& c = airbandChans_[ci];
            c.open = reported && open;
            c.levelDb = reported ? lvl : -200.0f;
            if (c.open) {
                anyOpen = true;
                c.pendingHeardS += dt;
            }
        }
    }
    // THE WATCHDOG: a set that has run no block for this long has been dropped (a plugin rescan flushes the runner) or starved - publish this block
    // again. Only once a block has been tuned: before that, with several blocks, the scanner's first tick is what tunes one.
    if (airbandTuned_ && nowS - airbandProgressS_ > kAirbandWatchdogS) {
        airbandTuneBlock(airbandBlock_);
        return;
    }
    if (airbandBlocks_.size() > 1) {
        const std::optional<double> retune = airbandScanner_.tick(nowS * 1000.0, anyOpen);
        if (retune.has_value()) { airbandTuneBlock(static_cast<std::size_t>(airbandScanner_.index()) % airbandBlocks_.size()); }
    }
    if (nowS >= airbandFlushDueS_) {
        airbandFlushHeard();
        airbandFlushDueS_ = nowS + kAirbandFlushS;
    }
}

// ============================================================================================================================
// THE OP
// ============================================================================================================================

std::int32_t Engine::applyAirbandOp(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    const auto state = [this] { return airbandListening_ ? 1.0 : (airbandStartPending_ ? 2.0 : 0.0); };
    switch (c.ival[0]) {
        case 0:   // STOP
            if (!airbandListening_ && !airbandStartPending_) {
                why = "the monitor is not listening";
                return FOXAPI_NO_CHANGE;
            }
            airbandStop(std::string());
            res.applied[0] = state();
            return FOXAPI_OK;
        case 1: {  // LISTEN, on the preset in the text ("" = every ticked AM or NFM row)
            if ((airbandListening_ || airbandStartPending_) && text == airbandGroup_) {
                res.applied[0] = state();   // already so
                return FOXAPI_OK;
            }
            // A change of preset is the monitor starting again on the new one.
            if (airbandListening_ || airbandStartPending_) { airbandStop(std::string()); }
            airbandGroup_ = text;
            const int st = airbandStart();
            res.applied[0] = state();
            if (st == 0) {
                why = airbandNote_;
                // Nothing to play is a request that changes nothing; the rest are the radio's refusals.
                return airbandWanted().empty() ? FOXAPI_NO_CHANGE : FOXAPI_FAILED;
            }
            return FOXAPI_OK;
        }
        case 2:  // NEXT BLOCK
            if (!airbandListening_ || airbandBlocks_.size() < 2) {
                why = "there is no other block to move to";
                return FOXAPI_NO_CHANGE;
            }
            airbandScanner_.skip();
            res.applied[0] = state();
            return FOXAPI_OK;
        case 3:  // ABOVE THE NOISE
            if (!airbandListening_) {
                why = "the monitor is not listening";
                return FOXAPI_NO_CHANGE;
            }
            // Pressed by hand it is a choice of its own: AUTOMATIC squelch (which re-applies this same rule for as long as it is lit) goes off (stub campaign B12) - whether or
            // not the rule finds a level to work from. Master turned it off on the press, and a press that lands just after AUTO rebuilt the block (every channel's level is
            // forgotten until its next report) must not leave AUTO lit over a threshold the hand chose to take over.
            squelchAutoOff("Airband Above the noise");
            if (!airbandAboveTheNoise(0.0f)) {
                why = "no channel has reported a level yet";
                return FOXAPI_NO_CHANGE;
            }
            res.applied[0] = state();
            res.applied[1] = static_cast<double>(airbandSquelchDb_);
            return FOXAPI_OK;
        case 4: {  // SQUELCH
            const float want = std::clamp(static_cast<float>(c.num[0]), -110.0f, 0.0f);
            if (static_cast<double>(want) != c.num[0]) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            const bool moved = want != airbandSquelchDb_;
            airbandSquelchDb_ = want;
            // A hand on the monitor's threshold is a manual choice too: the top bar's AUTO (one switch for the receiver's squelch and this one) goes off (stub campaign B12).
            squelchAutoOff("Airband squelch");
            // A new threshold rebuilds the block's set (the runner's live squelch slots belong to the patch page); a value already in force
            // restarts nothing.
            if (moved && airbandListening_) { airbandTuneBlock(airbandBlock_); }
            res.applied[0] = state();
            res.applied[1] = static_cast<double>(airbandSquelchDb_);
            return FOXAPI_OK;
        }
        case 5: {  // HOLD
            const double want = std::clamp(c.num[0], 0.0, 60.0);
            if (want != c.num[0]) { res.flags |= FOXAPI_RESULT_CLAMPED; }
            const bool moved = want != airbandHoldS_;
            airbandHoldS_ = want;
            // Only a scan between blocks uses it: that one starts again, with the new hold.
            if (moved && airbandListening_ && airbandBlocks_.size() > 1) {
                airbandStop(std::string());
                airbandStart();
            }
            res.applied[0] = state();
            res.applied[1] = airbandHoldS_;
            return FOXAPI_OK;
        }
        case 6: {  // TUNE ONE: the monitor stops, because the receiver is now somewhere the monitor did not put it
            const std::size_t i = freqMgr_.indexOfId(static_cast<std::uint64_t>(c.ival[1]));
            if (i >= freqMgr_.list().size()) {
                why = "no such bookmark";
                return FOXAPI_NOT_FOUND;
            }
            const cc::Bookmark b = freqMgr_.list()[i];
            if (airbandListening_ || airbandStartPending_) { airbandStop(cc::formatText(tr("Tuned to %s alone."), b.name.c_str())); }
            tuneToBookmark(b);
            res.applied[0] = state();
            return FOXAPI_OK;
        }
        default:
            why = "ival[0] is 0 (stop) to 6 (tune one)";
            return FOXAPI_OUT_OF_RANGE;
    }
}

// ============================================================================================================================
// WHAT THE TABLE SEES
// ============================================================================================================================

void Engine::buildApiListsAirband(ApiListBlock& b) {
    auto fact = [&b](const std::string& key, std::string value, bool localOnly = false) {
        b.facts[key] = ApiListBlock::Fact{std::move(value), localOnly};
    };
    char num[40];
    fact("airband.listening", airbandListening_ ? "1" : "0");
    fact("airband.pending", airbandStartPending_ ? "1" : "0");
    fact("airband.preset", airbandGroup_);
    fact("airband.note", airbandNote_);
    fact("airband.block", airbandBlockText());
    fact("airband.blocks", std::to_string(airbandListening_ ? airbandBlocks_.size() : 0u));
    std::snprintf(num, sizeof(num), "%.1f", static_cast<double>(airbandSquelchDb_));
    fact("airband.squelchDb", num);
    std::snprintf(num, sizeof(num), "%.1f", airbandHoldS_);
    fact("airband.holdS", num);
    fact("scanner.listMode", scanTicked_ ? "1" : "0");
    fact("scanner.ticked", std::to_string(tickedScanCount()));
}

}  // namespace cascade::engine
