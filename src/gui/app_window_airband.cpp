// app_window_airband.cpp - the AIRBAND section: type an airport, tick what to
// hear, LISTEN. AppWindow members, kept out of app_window.cpp because they are
// one subject.
//
// THE REQUEST (a US tester, 2026-10-01), in his words: "a plug in that you
// could simply enter the airport code and it would fill out a list where you
// could select which ones to listen to. If they are close enough together have
// multiple AM demodulators running simultaneously. If they are not close
// enough together could the SDR scan across the frequencies stopping when it
// sees a peak and tuning in. When the audio stops, continue scanning." And:
// "there is no way to know which ones are more popular."
//
// WHAT EACH PART IS:
//   - the lookup: core/airband_data.hpp's table (FAA for the US, OurAirports
//     elsewhere, compiled in) puts the airport's frequencies into the
//     FREQUENCY LIST as a group, ticked - so they are also bookmarks on the
//     spectrum, clickable, exportable, and the scanner's list mode can use
//     them;
//   - the add row (0.99.66): a tester asked for a MANUAL list - "the user can
//     enter their own list of frequencies to scan through ... another column
//     in the list for the demodulator". A frequency in MHz, an optional name
//     and a mode (AM or NFM) become a ticked row of the PRESET named in the
//     field above it (the group "Manual" until that is changed) in the
//     FREQUENCY LIST, like any other row: saved with it, on the spectrum, and
//     played by the monitor. WFM is not offered - the monitor's strips run at
//     24-96 kHz and have no de-emphasis - and a WFM row is not played;
//   - presets (0.99.66): the same tester asked to "upload a file of
//     frequencies", to "enter frequencies and save them as a preset" and to
//     "export the file of frequencies". A preset IS a group of the frequency
//     list - saved with it, already in the group list above - and nothing
//     more: no new file format. By hand: the add row. IMPORT: the path of a
//     .csv or an SDR# frequencies.xml, typed or pasted (FoxSDR has no file
//     dialog; a file dropped on the window still goes in as the file says), is
//     read on a worker and every row joins the preset whatever group the file
//     gave it, the AM and NFM rows TICKED whatever tick the file gave them; a
//     row the monitor cannot play (not AM or NFM) is kept as a bookmark all the
//     same - UNTICKED, for a ticked WFM row would enter the Scanner's list mode
//     and this section could not show it to untick - and the note counts the
//     ones the monitor will play. EXPORT CSV: the preset's rows, every mode, tick kept, into the
//     recordings folder as foxsdr-<preset>-<stamp>.csv - the file Import reads -
//     with a key that opens the folder. REMOVE PRESET takes the group's rows out
//     of the list, as the Bookmarks section's "Remove this group" does, without
//     asking. The notes of all three are said in this section. Choosing a group
//     in the list below puts its name in the preset field, so the keys act on
//     the list that is showing;
//   - the monitor: core/airband_monitor.hpp cuts the ticked AM and NFM rows
//     into blocks the radio's band can hold and plays a block's channels at
//     once, mixed, each behind its own squelch; with more than one block,
//     core::Scanner in list mode walks the block centres, stops where anybody
//     is talking and moves on when the block has been quiet for the hold time;
//   - "which ones are busy": every second a channel's squelch is open while
//     the monitor plays it is added to that row's heardSeconds, saved with the
//     list, and the rows can be sorted by it;
//   - the spectrum (0.99.66): while the monitor listens, the receiver's
//     spectrum shows what is being HEARD instead of the VFO it is not
//     demodulating - each channel of the block on the air is a shaded mark
//     at its frequency and bandwidth, bright with a centre line while its
//     squelch is open and faint while it is shut, the same two states as the
//     lamp in the list; the VFO's shaded band, its line down the waterfall and
//     its "peak in passband" figure are not drawn until the monitor stops. A
//     tester asked for exactly this: the shaded area in the middle "is not
//     the frequency or bandwidth being demodulated".
//
// IT RUNS ON THE RECEIVER'S RADIO. The DSP is the pipeline's patch runner -
// the hand-off the patch page used before its radios opened devices of their
// own (pipeline.cpp, "THE PATCH TAPS THE SAME RAW BAND") - which the receiver
// view otherwise leaves idle. The patch view lends the receiver's radio to the
// patch, so LISTEN switches to the RECEIVER view first, and showing the patch
// view again stops the monitor.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/airband_marker_geometry.hpp"
#include "gui/bench_rail.hpp"
#include "gui/text_fit.hpp"
#include "gui/ui_census.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace cc = cascade::core;

namespace {

// How long the monitor sits on a block before judging it empty: the radio's
// retune, the first filter filling and the squelch's attack all fit inside
// it, with room left to hear the start of a call.
constexpr double kAirbandDwellMs = 350.0;
// The grace after a block's hold has run out before the next retune, so a
// reply that starts a moment after the last word is still caught.
constexpr double kAirbandResumeMs = 150.0;
// A set that has run no block this long has been dropped (a plugin rescan
// flushes the runner) or starved: publish it again.
constexpr double kAirbandWatchdogS = 2.0;
// Squelch reports from a set that has run nothing for this long are history.
constexpr double kAirbandStaleS = 0.5;
// How long after a publish reports are ignored: the samples already captured
// at the previous centre are still being worked through with the new offsets.
constexpr double kAirbandSettleS = 0.15;
// heardSeconds is written to the list this often, not every frame: the list
// saves to disk a second after any change.
constexpr double kAirbandFlushS = 20.0;

// The preset the section starts with, and the one an emptied name field means
// (0.99.66). A name in the user's list, not a label: it is stored as it is here
// in every language.
constexpr const char* kManualGroup = "Manual";

// The rows the monitor plays: AM, and since 0.99.66 NFM. Any other mode (WFM,
// SSB, CW...) is not an airband row - not shown, not ticked into the monitor.
bool playsInMonitor(const cc::Bookmark& b) {
    cc::MonitorMode m = cc::MonitorMode::Am;
    return cc::monitorModeFor(b.mode, m);
}

// "1h 02m", "12m 30s", "45s"; "-" for never.
std::string heardText(double s) {
    if (!(s >= 1.0)) { return "-"; }
    const long long t = static_cast<long long>(s);
    char buf[32];
    if (t >= 3600) {
        std::snprintf(buf, sizeof(buf), "%lldh %02lldm", t / 3600, (t / 60) % 60);
    } else if (t >= 60) {
        std::snprintf(buf, sizeof(buf), "%lldm %02llds", t / 60, t % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%llds", t);
    }
    return buf;
}

}  // namespace

// --- the scanner's list mode ---------------------------------------------------

std::vector<double> AppWindow::tickedScanList() const {
    std::vector<double> out;
    for (const cc::Bookmark& b : freqMgr_.list()) {
        if (b.scan) { out.push_back(b.freqHz); }
    }
    return out;   // already ascending: the list is kept sorted
}

void AppWindow::startTickedScan() {
    cc::Scanner::Params p;
    p.startHz = scanStartMhz_ * 1.0e6;
    p.stopHz = scanStopMhz_ * 1.0e6;
    p.stepHz = scanStepKhz_ * 1.0e3;
    p.dwellMs = scanDwellMs_;
    p.holdMs = scanHoldMs_;
    p.resumeMs = scanResumeMs_;
    p.listenMs = scanListenMs_;
    p.list = tickedScanList();
    scanListVersion_ = freqMgr_.version();
    scanList_ = p.list;
    const bool wasActive = scanner_.active();
    scanner_.configure(std::move(p));
    if (!wasActive) { scanner_.start(ImGui::GetTime() * 1000.0); }
    scannerHasExpected_ = false;
}

// --- the rows ------------------------------------------------------------------

std::vector<std::size_t> AppWindow::airbandRows() const {
    std::vector<std::size_t> rows;
    const std::vector<cc::Bookmark>& list = freqMgr_.list();
    for (std::size_t i = 0; i < list.size(); ++i) {
        const cc::Bookmark& b = list[i];
        if (!playsInMonitor(b)) { continue; }
        if (airbandGroup_.empty() ? b.scan : (b.group == airbandGroup_)) { rows.push_back(i); }
    }
    return rows;
}

std::vector<cc::MonitorChannel> AppWindow::airbandWanted(std::vector<std::string>* names) const {
    // ONE CHANNEL PER FREQUENCY. Two ticked rows on one frequency (CTAF and
    // UNICOM share 122.8 at many fields; an AM and an NFM row typed on the same
    // frequency) are one channel on the air: two strips would play it twice as
    // loud for twice the work, and a carrier is one transmitter whatever the
    // rows call it. The first ticked row (the list is sorted, ties in insertion
    // order) names it and gives it its mode and width.
    std::vector<cc::MonitorChannel> out;
    if (names != nullptr) { names->clear(); }
    for (const std::size_t i : airbandRows()) {
        const cc::Bookmark& b = freqMgr_.list()[i];
        if (!b.scan) { continue; }
        cc::MonitorChannel ch{b.freqHz, b.bandwidthHz};
        if (!cc::monitorModeFor(b.mode, ch.mode)) { continue; }
        if (!out.empty() && out.back().freqHz == b.freqHz) { continue; }
        out.push_back(ch);
        if (names != nullptr) { names->push_back(b.name); }
    }
    return out;
}

// --- the lookup ------------------------------------------------------------------

void AppWindow::airbandAddAirport(const cc::Airport& a) {
    airbandChoices_.clear();
    std::vector<cc::Bookmark> items = cc::airportBookmarks(a);
    std::size_t ticked = 0;
    for (const cc::Bookmark& b : items) { ticked += b.scan ? 1u : 0u; }
    const std::size_t total = items.size();
    const std::size_t added = freqMgr_.addMany(std::move(items));
    saveBookmarks();
    airbandGroup_ = cc::airportGroupName(a);
    testerUsage_.noteFeature("airband");
    if (added == 0) {
        airbandNote_ = cc::formatText(tr("%s is already in the frequency list (%zu frequencies)."),
                                      airbandGroup_.c_str(), total);
    } else {
        airbandNote_ = cc::formatText(
            tr("%s: %zu frequencies added to the frequency list, %zu ticked. ATIS and weather "
               "broadcasts are left unticked - they never stop talking."),
            airbandGroup_.c_str(), added, ticked);
    }
    cc::diagLogf("airband: an airport's list added (%zu frequencies, %zu new)", total, added);
}

void AppWindow::airbandLookup(const std::string& code) {
    airbandChoices_.clear();
    const std::vector<const cc::Airport*> found = cc::findAirports(cc::airbandTable(), code);
    if (found.empty()) {
        airbandNote_ = cc::formatText(tr("No airport with the code \"%s\" publishes an airband "
                                         "frequency in FoxSDR's table."),
                                      code.c_str());
        return;
    }
    if (found.size() == 1) {
        airbandAddAirport(*found.front());
        return;
    }
    airbandChoices_ = found;
    airbandNote_ = cc::formatText(tr("\"%s\" names %zu airports - pick one:"), code.c_str(),
                                  found.size());
}

// THE PRESET THE SECTION ACTS ON (0.99.66): what the name field holds, without
// the spaces round it, or "Manual" when nothing is left. ASCII spaces only, so a
// name in any script is never cut.
std::string AppWindow::airbandPresetName() const {
    const std::string t = cascade::gui::trimPresetName(airbandPresetName_);
    return t.empty() ? std::string(kManualGroup) : t;
}

// SHOW A GROUP THE SECTION HAS JUST ADDED TO (0.99.66 review): Add and the
// preset's Import put the group on show, as an airport's lookup does - but only
// when that cannot change what is being played. A monitor that is listening keeps
// its choice (the preset showing, or "every ticked"): a row added to another
// preset must not narrow "every ticked" down to it. And a group the monitor has
// nothing to play in (an import of WFM rows only, a file that added nothing) is
// not put on show: the combo would name a group the list may not even have.
void AppWindow::airbandShowGroup(const std::string& group) {
    if (airbandListening_) { return; }
    for (const cc::Bookmark& b : freqMgr_.list()) {
        if (b.group == group && playsInMonitor(b)) {
            airbandGroup_ = group;
            return;
        }
    }
}

// THE PRESET'S IMPORT KEY: the file at the typed path, read on a worker, its rows
// into the preset - in its group, the AM and NFM ones ticked, whatever the file
// said (pollDiskJobs puts them there, and says what it did in this section).
void AppWindow::airbandImportPreset() {
    if (airbandImportPath_[0] == '\0') { return; }
    airbandPresetNote_.clear();
    airbandPresetFolderKey_ = false;
    importBookmarkFile(airbandImportPath_, cc::ImportInto{airbandPresetName(), true});
}

// THE PRESET'S REMOVE KEY: its rows leave the frequency list, whatever their mode
// or tick. No confirmation, as the Bookmarks section's "Remove this group" has none.
void AppWindow::airbandRemovePreset() {
    const std::string g = airbandPresetName();
    // THE MONITOR IS PLAYING THIS PRESET (0.99.66 review): with its group gone,
    // airbandGroup_ is cleared below and the next frame would cut the blocks again
    // from "every ticked row" of the WHOLE list - a different set of channels from
    // the one the user chose - or, with none ticked, stop it with no reason. It is
    // stopped here instead, saying why, before the rows go.
    if (airbandListening_ && airbandGroup_ == g) {
        airbandStop(tr("Stopped: the preset was removed."));
    }
    const std::size_t n = freqMgr_.removeGroup(g);
    airbandPresetNote_ = cc::formatText(tr("Removed %zu from \"%s\""), n, g.c_str());
    airbandPresetFolderKey_ = false;
    if (airbandGroup_ == g) { airbandGroup_.clear(); }
    saveBookmarks();
    cc::diagLogf("airband: a preset was removed (%zu rows)", n);
}

// A FREQUENCY OF YOUR OWN (0.99.66). The add row's Add key: the typed MHz, the
// mode chosen and the optional name become one ticked row of the preset named
// in the field above (the group "Manual" until that is changed) in the
// frequency list, saved with it like any other change.
void AppWindow::airbandAddManual() {
    double hz = 0.0;
    if (!parseMhz(airbandAddMhz_, hz)) {
        airbandNote_ = cc::formatText(tr("could not read frequency \"%s\""), airbandAddMhz_);
        return;
    }
    // THE SAME RULE AS A TUNE THE RADIO REFUSES (noteTuneRefused): a
    // frequency outside what the radio publishes it covers is not accepted,
    // and the note says what it does cover. Judged at the RADIO, so a
    // converter's LO is taken out first. A radio that publishes no range (the
    // generator, an I/Q file) refuses nothing; the list is the user's.
    const cc::ConverterSetting conv = pipeline_.converter();
    double rangeLoHz = 0.0;
    double rangeHiHz = 0.0;
    const bool hasRange = device_ != nullptr && device_->frequencyRangeHz(rangeLoHz, rangeHiHz);
    if (!cascade::gui::radioCanTune(cc::radioFromAir(conv, hz), hasRange, rangeLoHz, rangeHiHz)) {
        std::string note;
        if (cc::converterActive(conv)) { note = converterTuneNote(hz, /*refused=*/true, 0.0, false); }
        if (note.empty()) {
            note = cc::formatText(tr("This radio cannot tune to %s. Its range is %s to %s."),
                                  cc::converterHzText(hz).c_str(), cc::converterHzText(rangeLoHz).c_str(),
                                  cc::converterHzText(rangeHiHz).c_str());
        }
        airbandNote_ = note;
        return;
    }
    static const char* const kModes[] = {"AM", "NFM"};
    cc::Bookmark b;
    b.mode = kModes[airbandAddMode_ == 1 ? 1 : 0];
    b.freqHz = hz;
    b.bandwidthHz = cc::defaultBandwidthForMode(b.mode);
    const std::string group = airbandPresetName();
    b.group = group;
    b.scan = true;
    b.name = airbandAddName_;
    if (b.name.empty()) {
        // A nameless row would render blank: the frequency, as "Add current"
        // names one.
        char def[32];
        std::snprintf(def, sizeof(def), "%.4f MHz", hz / 1.0e6);
        b.name = def;
    }
    freqMgr_.add(std::move(b));
    saveBookmarks();
    airbandAddMhz_[0] = '\0';
    airbandAddName_[0] = '\0';
    airbandShowGroup(group);   // show the list it joined, as an airport's lookup does - unless listening
    airbandNote_.clear();
    testerUsage_.noteFeature("airband");
    cc::diagLogf("airband: a frequency was typed into a preset");
}

// --- listening -----------------------------------------------------------------

void AppWindow::airbandStart() {
    airbandStartPending_ = false;
    std::vector<std::string> names;
    const std::vector<cc::MonitorChannel> want = airbandWanted(&names);
    if (want.empty()) {
        airbandNote_ = tr("Tick at least one AM or NFM frequency to listen to.");
        return;
    }
    // THE PATCH VIEW HAS THE RECEIVER'S RADIO: hand it back first, then start
    // once the patch has let go (airbandFrame watches for it).
    if (patchOpen_ || patchWasOpen_) {
        setMainViewPatch(false);
        airbandStartPending_ = true;
        return;
    }
    if (scanner_.active()) { scanner_.stop(); }
    if (!pipeline_.running()) { startReceiver(); }
    const double rate = pipeline_.activeSource().sampleRateHz();
    if (!(rate > 0.0) || !cc::patch::chooseChannelRate(rate).ok) {
        airbandNote_ = tr("The receiver's sample rate cannot be divided down to an audio rate - "
                          "choose another rate in the Source section.");
        return;
    }
    airbandPlanned_ = want;
    airbandBlocks_ = cc::planAirbandBlocks(want, rate, cc::patch::kUsableBandFraction,
                                           cc::monitorChannelsPerBlock(rate));
    if (airbandBlocks_.empty()) {
        airbandNote_ = tr("None of the ticked frequencies can be tuned.");
        return;
    }
    // The channel table, index for index with airbandPlanned_ (the blocks'
    // member indices point into both).
    airbandChans_.clear();
    for (std::size_t i = 0; i < want.size(); ++i) {
        AirbandChan c;
        c.freqHz = want[i].freqHz;
        c.bandwidthHz = want[i].bandwidthHz;
        c.name = names[i];
        airbandChans_.push_back(std::move(c));
    }
    airbandRateHz_ = rate;
    airbandSourceDevice_ = device_;
    airbandSourceKind_ = sourceKind_;
    airbandTuned_ = false;
    // A FRESH SESSION STARTS FROM BLOCK 0 WITH A FRESH CLOCK. Carried over
    // from the last session, the block index could be past the end of the
    // new plan and the watchdog's clock long expired: the watchdog then
    // republished a block that does not exist on every frame, the scanner
    // never ticked, and LISTEN played nothing for good (review, 2026-10-01).
    airbandBlock_ = 0;
    airbandPublishedS_ = ImGui::GetTime();
    airbandProgressS_ = airbandPublishedS_;
    airbandBlocksSeen_ = pipeline_.patchRunner().blocksRun();
    airbandListening_ = true;
    airbandNote_.clear();
    airbandLastFrameS_ = ImGui::GetTime();
    airbandFlushDueS_ = airbandLastFrameS_ + kAirbandFlushS;
    testerUsage_.noteFeature("airband-listen");
    cc::diagLogf("airband: listening to %zu channel(s) in %zu block(s)", want.size(),
                 airbandBlocks_.size());
    if (airbandBlocks_.size() == 1) {
        airbandScanner_.stop();
        airbandTuneBlock(0);
        return;
    }
    cc::Scanner::Params p;
    for (const cc::AirbandBlock& b : airbandBlocks_) { p.list.push_back(b.centreHz); }
    p.dwellMs = kAirbandDwellMs;
    p.holdMs = airbandHoldS_ * 1000.0;
    p.resumeMs = kAirbandResumeMs;
    airbandScanner_.configure(std::move(p));
    airbandScanner_.start(ImGui::GetTime() * 1000.0);
    // The first tune comes from the scanner's first tick, in airbandFrame.
}

void AppWindow::airbandStop(const std::string& why) {
    airbandStartPending_ = false;
    if (!airbandListening_) { return; }
    airbandListening_ = false;
    airbandScanner_.stop();
    pipeline_.patchRunner().clear();
    airbandFlushHeard();
    for (AirbandChan& c : airbandChans_) {
        c.open = false;
        c.levelDb = -200.0f;
    }
    airbandNote_ = why;
    cc::diagLogf("airband: stopped");
}

void AppWindow::airbandTuneBlock(std::size_t index) {
    if (index >= airbandBlocks_.size()) { return; }
    airbandBlock_ = index;
    const cc::AirbandBlock& blk = airbandBlocks_[index];
    // Applied NOW rather than through the retune coalescer: the readback
    // below is the centre the strips are offset from, and a deferred tune
    // would make it the previous block's.
    applyRetuneNow(blk.centreHz, false);
    airbandCentreHz_ = pipeline_.activeSource().centerFrequencyHz();
    airbandTuned_ = true;
    // WHERE IT LANDED, NOT WHERE IT WAS ASKED: a radio that refused the tune,
    // coerced it (a B200 asked for 7 MHz answered 30.8), or has no tuner at
    // all would leave channels outside the band - their strips would alias
    // other frequencies and credit the wrong rows with what they heard.
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
    census::note("airband:centre:", static_cast<int>(std::lround(airbandCentreHz_ / 1000.0)));
    // Fresh channel identities for every set, so a squelch report from the
    // set this one replaces can never be read as this one's.
    airbandFirstId_ = airbandNextId_;
    airbandNextId_ += static_cast<cc::patch::NodeId>(cc::kMaxBlockChannels);
    if (airbandNextId_ > 0xF0000000u) { airbandNextId_ = 0x40000000u; }
    std::shared_ptr<cc::patch::StripSet> set =
        cc::buildMonitorSet(airbandPlanned_, blk, airbandCentreHz_, airbandRateHz_,
                            airbandSquelchDb_, airbandFirstId_);
    if (!set) {
        airbandStop(tr("The receiver's sample rate cannot be divided down to an audio rate."));
        return;
    }
    pipeline_.patchRunner().publish(std::move(set));
    airbandPublishedS_ = ImGui::GetTime();
    airbandProgressS_ = airbandPublishedS_;
    for (AirbandChan& c : airbandChans_) {
        c.open = false;
        c.levelDb = -200.0f;
    }
}

void AppWindow::airbandFlushHeard() {
    bool changed = false;
    for (AirbandChan& c : airbandChans_) {
        if (!(c.pendingHeardS > 0.0)) { continue; }
        const std::vector<cc::Bookmark>& list = freqMgr_.list();
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].freqHz != c.freqHz || list[i].name != c.name) { continue; }
            cc::Bookmark b = list[i];
            b.heardSeconds += c.pendingHeardS;
            freqMgr_.updateAt(i, b);
            changed = true;
            break;
        }
        c.pendingHeardS = 0.0;
    }
    if (!changed) { return; }
    // Also called on the shutdown path, where the frame loop - and with it
    // the debounce clock saveBookmarks() reads - may already be gone; the
    // caller there forces the save straight after.
    if (ImGui::GetCurrentContext() != nullptr) {
        saveBookmarks();
    } else if (!bookmarkPath_.empty()) {
        bookmarkSaveDirty_ = true;
    }
}

void AppWindow::airbandFrame() {
    const double nowS = ImGui::GetTime();
    if (airbandStartPending_) {
        // The patch has let go of the radio AND the receiver has it open
        // again (the hand-back reopens it on a worker).
        if (!patchOpen_ && !patchWasOpen_ && !deviceOpenPending_) { airbandStart(); }
        return;
    }
    if (!airbandListening_) { return; }
    // A radio being opened is between sources: judge nothing until it lands.
    if (deviceOpenPending_) { return; }
    // A different radio (a reopen, a source switch): the blocks were cut for
    // the old one's rate and the strips are offset from its centre.
    if (device_ != airbandSourceDevice_ || sourceKind_ != airbandSourceKind_) {
        airbandStop("");
        airbandStart();
        return;
    }

    if (patchOpen_) {
        airbandStop(tr("Stopped: the patch view took the radio."));
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
    // USER WINS, as with the scanner: a centre the monitor did not tune to is
    // the user's hand on the dial, and the strips' offsets no longer mean
    // anything.
    // (airbandTuned_ is false until the first block is tuned: with several
    // blocks that waits for the scanner's first tick.)
    if (airbandTuned_ && std::fabs(pipeline_.activeSource().centerFrequencyHz() - airbandCentreHz_) > 1.0) {
        airbandStop(tr("Stopped: the receiver was retuned."));
        return;
    }
    // A new sample rate, or a different set of ticked rows: cut the blocks
    // again.
    if (pipeline_.activeSource().sampleRateHz() != airbandRateHz_) {
        airbandStop("");
        airbandStart();
        return;
    }
    {
        const std::vector<cc::MonitorChannel> want = airbandWanted();
        bool same = want.size() == airbandPlanned_.size();
        for (std::size_t i = 0; same && i < want.size(); ++i) {
            same = want[i].freqHz == airbandPlanned_[i].freqHz &&
                   want[i].bandwidthHz == airbandPlanned_[i].bandwidthHz &&
                   want[i].mode == airbandPlanned_[i].mode;
        }
        if (!same) {
            airbandStop("");
            if (!want.empty()) { airbandStart(); }
            return;
        }
    }

    census::note("airband:listening");
    census::note("airband:blocks:", static_cast<int>(airbandBlocks_.size()));
    census::note("airband:channels:", static_cast<int>(airbandChans_.size()));
    // What every channel of the block on the air is doing.
    const double dt = std::max(0.0, std::min(1.0, nowS - airbandLastFrameS_));
    airbandLastFrameS_ = nowS;
    // IS THE SET RUNNING? The runner counts the blocks it processes; a count
    // that has not moved is a set that is not running - flushed by a plugin
    // rescan, or a stalled radio - and its last squelch reports are history,
    // not news. Reports are also ignored for a moment after each publish:
    // samples captured at the previous centre are still being worked through
    // with the new offsets, and a hold made on them would be a hold on noise.
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
                                  pipeline_.patchRunner().squelchState(
                                      airbandFirstId_ + static_cast<cc::patch::NodeId>(m), lvl, open);
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
    // THE WATCHDOG: a set that has run no block for this long has been
    // dropped (a plugin rescan flushes the runner) or starved - publish this
    // block again. Only once a block has been tuned: before that, with
    // several blocks, the scanner's first tick is what tunes one.
    if (airbandTuned_ && nowS - airbandProgressS_ > kAirbandWatchdogS) {
        airbandTuneBlock(airbandBlock_);
        return;
    }

    if (airbandBlocks_.size() > 1) {
        const std::optional<double> retune = airbandScanner_.tick(nowS * 1000.0, anyOpen);
        if (retune.has_value()) {
            airbandTuneBlock(static_cast<std::size_t>(airbandScanner_.index()) % airbandBlocks_.size());
        }
    }
    if (nowS >= airbandFlushDueS_) {
        airbandFlushHeard();
        airbandFlushDueS_ = nowS + kAirbandFlushS;
    }
}

// --- the spectrum --------------------------------------------------------------

// WHAT THE SPECTRUM SAYS WHILE THE MONITOR LISTENS (0.99.66). The strips tap
// the raw band at each channel's own offset and the VFO's audio is faded out
// for their mix, so the VFO's shaded band - which the receiver view draws as
// "what is being demodulated" - names nothing that is heard. drawCenterPanels
// leaves it, its waterfall line and its passband peak off while the monitor
// listens, and this draws what IS heard in their place: one mark per channel
// of the block on the air, in the two states the list's lamp has.
//
// Called after the bookmark markers and before the gridlines, so the axis
// furniture stays on top; clipped to the panel, because a mark at the edge of
// the view must not paint the header or the waterfall under it. No names: the
// bookmark markers already carry them, and a mark that also lettered itself
// would hide the trace it is meant to sit on.
//
// The census gets "airband:mark:<kHz>" for a channel whose squelch is open (the
// same spelling as the list's "airband:open:<kHz>") and the rectangle of every
// mark drawn, so the test can hold the marks to where the channels are.
void AppWindow::drawAirbandMarkers(float x0, float y0, float width, float height) {
    if (airbandBlock_ >= airbandBlocks_.size()) { return; }
    const cc::AirbandBlock& blk = airbandBlocks_[airbandBlock_];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, y0 + height), true);
    const ImU32 openFill = cascade::gui::theme::withAlpha(cascade::gui::theme::kPhosphor, 0.30f);
    const ImU32 openLine = cascade::gui::theme::kPhosphor;
    const ImU32 shutFill = cascade::gui::theme::withAlpha(cascade::gui::theme::kPhosphor, 0.08f);
    const ImU32 shutLine = cascade::gui::theme::withAlpha(cascade::gui::theme::kPhosphorDim, 0.6f);
    for (const std::size_t ci : blk.members) {
        if (ci >= airbandChans_.size()) { continue; }
        const AirbandChan& c = airbandChans_[ci];
        cascade::gui::AirbandMarkExtent e;
        if (!cascade::gui::airbandMarkExtent(scale_, c.freqHz, c.bandwidthHz, x0, width, e)) { continue; }
        dl->AddRectFilled(ImVec2(e.xLo, y0), ImVec2(e.xHi, y0 + height), c.open ? openFill : shutFill);
        dl->AddLine(ImVec2(e.xCentre, y0), ImVec2(e.xCentre, y0 + height), c.open ? openLine : shutLine, 1.0f);
        const int kHz = static_cast<int>(std::lround(c.freqHz / 1000.0));
        if (c.open) { census::note("airband:mark:", kHz); }
        census::rect("airband:mk:", kHz, e.xLo, y0, e.xHi, y0 + height);
    }
    dl->PopClipRect();
}

// --- the section ---------------------------------------------------------------

void AppWindow::drawAirbandSection() {
    const char* chip = airbandListening_ ? tr("LISTEN") : tr("IDLE");
    if (!railSection(trId("Airband###airband"), false, chip, cascade::gui::theme::kPhosphor,
                     airbandListening_)) {
        return;
    }
    telemetryNotePanel("airband");

    // --- the airport -------------------------------------------------------
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float lookW = std::max(82.0f, cascade::gui::buttonWidth(trId("Look up")));
    const bool beside = cascade::gui::fitsBeside(90.0f, spacing, lookW, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(beside ? -(lookW + spacing) : -1.0f);
    const bool entered = cascade::gui::inputTextWithFittedHint(
        "##airband_code", tr("airport code - KORD, ORD, EGLL, LHR"), airbandCode_, sizeof(airbandCode_),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
    census::rect("airband:code", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                 ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    if (beside) { ImGui::SameLine(); }
    ImGui::BeginDisabled(airbandCode_[0] == '\0');
    if ((ImGui::Button(trId("Look up"), ImVec2(lookW, 0.0f)) || entered) && airbandCode_[0] != '\0') {
        airbandLookup(airbandCode_);
    }
    census::rect("airband:lookup", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                 ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr("The ICAO code, the three-letter code on a luggage tag, or in the US\n"
                                   "the FAA's own id. Adds the airport's frequencies to the frequency\n"
                                   "list as a group. US lists come from the FAA, the rest of the world's\n"
                                   "from OurAirports; both are built into FoxSDR, so this needs no\n"
                                   "internet connection."));
    }
    // NEAREST: from the receiver position the map and the range rings use.
    ImGui::BeginDisabled(!rxSet_);
    if (ImGui::SmallButton(trId("Nearest airports"))) {
        airbandNearest_ = cc::nearestAirports(cc::airbandTable(), rxLat_, rxLon_, 6);
        airbandChoices_.clear();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", rxSet_ ? tr("The airports closest to the receiver position.")
                                       : tr("Set the receiver position first: Radar section,\n"
                                            "\"Receiver position\" - or \"Set RX here\" on a map."));
    }
    if (!airbandNearest_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(trId("Hide##airband_near"))) { airbandNearest_.clear(); }
        const cc::Airport* pick = nullptr;
        for (std::size_t i = 0; i < airbandNearest_.size(); ++i) {
            const cc::Airport* a = airbandNearest_[i].first;
            ImGui::PushID(static_cast<int>(i));
            char label[192];
            cc::formatUtf8(label, sizeof(label), "%s  %s  %.0f km", a->ident.c_str(), a->name.c_str(),
                           airbandNearest_[i].second);
            if (ImGui::Selectable(label)) { pick = a; }
            ImGui::PopID();
        }
        if (pick != nullptr) {
            airbandNearest_.clear();
            airbandAddAirport(*pick);
        }
    }
    if (!airbandChoices_.empty()) {
        const cc::Airport* pick = nullptr;
        for (std::size_t i = 0; i < airbandChoices_.size(); ++i) {
            const cc::Airport* a = airbandChoices_[i];
            ImGui::PushID(static_cast<int>(1000 + i));
            char label[192];
            cc::formatUtf8(label, sizeof(label), "%s  %s  (%s)", a->ident.c_str(), a->name.c_str(),
                           a->country.c_str());
            if (ImGui::Selectable(label)) { pick = a; }
            ImGui::PopID();
        }
        if (pick != nullptr) { airbandAddAirport(*pick); }
    }

    // --- a PRESET, and a frequency of your own for it (0.99.66) ----------------
    // THE PRESET'S NAME first: the group of the frequency list that Add, Import,
    // Export CSV and Remove preset all act on. The caption sits beside the field
    // when the rail holds both, above it when it does not (text_fit.hpp's rule),
    // and the hover on either says what a preset is.
    {
        const char* const presetHelp =
            tr("A preset is a named list of frequencies - a group of the frequency list.\n"
               "Add puts a frequency in it. Import reads a .csv or an SDR# frequencies.xml\n"
               "into it, the AM and NFM rows ticked. Export CSV writes it to the recordings folder.\n"
               "Remove preset takes its rows out of the list.");
        const float capW = ImGui::CalcTextSize(tr("Preset")).x;
        const bool capBeside = cascade::gui::fitsBeside(90.0f, spacing, capW, ImGui::GetContentRegionAvail().x);
        if (capBeside) { ImGui::AlignTextToFramePadding(); }
        ImGui::TextUnformatted(tr("Preset"));
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", presetHelp); }
        if (capBeside) { ImGui::SameLine(); }
        ImGui::SetNextItemWidth(-1.0f);
        cascade::gui::inputTextWithFittedHint("##airband_preset", tr("Preset"), airbandPresetName_,
                                              sizeof(airbandPresetName_));
        census::rect("airband:preset", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", presetHelp); }
    }
    // The frequency in MHz beside the mode, then an optional name beside Add;
    // each pair stacks when the rail is too narrow to hold it (text_fit.hpp's
    // rule). The mode is AM or NFM only: WFM is not something the monitor plays.
    {
        static const char* const kAddModes[] = {"AM", "NFM"};
        const ImGuiStyle& st = ImGui::GetStyle();
        const float modeW = std::max(64.0f, ImGui::CalcTextSize("NFM").x + st.FramePadding.x * 2.0f +
                                                ImGui::GetFrameHeight());
        const bool modeBeside = cascade::gui::fitsBeside(90.0f, spacing, modeW, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(modeBeside ? -(modeW + spacing) : -1.0f);
        cascade::gui::inputTextWithFittedHint("##airband_add_mhz", tr("Frequency (MHz)"), airbandAddMhz_,
                                              sizeof(airbandAddMhz_));
        census::rect("airband:addfreq", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        if (modeBeside) { ImGui::SameLine(); }
        ImGui::SetNextItemWidth(modeBeside ? modeW : -1.0f);
        const bool modeOpen = ImGui::BeginCombo("##airband_add_mode", kAddModes[airbandAddMode_ == 1 ? 1 : 0]);
        census::rect("airband:mode", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        if (modeOpen) {
            for (int m = 0; m < 2; ++m) {
                if (ImGui::Selectable(kAddModes[m], airbandAddMode_ == m)) { airbandAddMode_ = m; }
                census::rect("airband:mode:", m, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                             ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
            }
            ImGui::EndCombo();
        }
        const float addW = std::max(60.0f, cascade::gui::buttonWidth(trId("Add##airband_add")));
        const bool addBeside = cascade::gui::fitsBeside(90.0f, spacing, addW, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(addBeside ? -(addW + spacing) : -1.0f);
        cascade::gui::inputTextWithFittedHint("##airband_add_name", tr("name"), airbandAddName_,
                                              sizeof(airbandAddName_));
        census::rect("airband:addname", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        if (addBeside) { ImGui::SameLine(); }
        ImGui::BeginDisabled(airbandAddMhz_[0] == '\0');
        if (ImGui::Button(trId("Add##airband_add"), ImVec2(addBeside ? addW : -FLT_MIN, 0.0f))) {
            airbandAddManual();
        }
        census::rect("airband:add", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tr("Adds the frequency to the preset named above, ticked. With no name\n"
                                       "it is named by its frequency.\n"
                                       "AM is for aircraft; NFM is narrow FM - marine, business and\n"
                                       "public-service radio."));
        }
    }
    if (!airbandNote_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
        ImGui::TextWrapped("%s", airbandNote_.c_str());
        ImGui::PopStyleColor();
    }

    // THE PRESET'S FILE KEYS (0.99.66): the path of a file to read into it and
    // Import beside it, then Export CSV and Remove preset. The path is typed or
    // pasted - FoxSDR has no file dialog - like the Bookmarks section's, and a
    // file dropped on the window still goes in as the file says.
    {
        const float importW = std::max(82.0f, cascade::gui::buttonWidth(trId("Import##airband_import")));
        const bool importBeside = cascade::gui::fitsBeside(90.0f, spacing, importW, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(importBeside ? -(importW + spacing) : -1.0f);
        cascade::gui::inputTextWithFittedHint("##airband_import_path", tr("SDR# frequencies.xml or .csv - its path"),
                                              airbandImportPath_, sizeof(airbandImportPath_));
        census::rect("airband:importpath", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        if (importBeside) { ImGui::SameLine(); }
        // Disabled while a read is out (the Bookmarks section's rule): the file named
        // is being read, and a second press would only be remembered.
        ImGui::BeginDisabled(airbandImportPath_[0] == '\0' || bookmarkImportPending());
        if (ImGui::Button(trId("Import##airband_import"), ImVec2(importBeside ? importW : -FLT_MIN, 0.0f))) {
            airbandImportPreset();
        }
        census::rect("airband:import", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();

        // The preset's rows, whatever their mode or tick: what Export has to write and
        // Remove has to take out.
        const std::string preset = airbandPresetName();
        std::size_t presetRows = 0;
        for (const cc::Bookmark& b : freqMgr_.list()) { presetRows += (b.group == preset) ? 1u : 0u; }
        const float avail = ImGui::GetContentRegionAvail().x;
        const float half = (avail - spacing) * 0.5f;
        const bool keysBeside = cascade::gui::buttonWidth(trId("Export CSV##airband_export")) <= half &&
                                cascade::gui::buttonWidth(trId("Remove preset##airband_remove")) <= half;
        const ImVec2 keySize(keysBeside ? half : -FLT_MIN, 0.0f);
        ImGui::BeginDisabled(presetRows == 0 || bookmarkExportPending());
        if (ImGui::Button(trId("Export CSV##airband_export"), keySize)) { exportGroupCsv(preset); }
        census::rect("airband:export", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();
        if (keysBeside) { ImGui::SameLine(); }
        ImGui::BeginDisabled(presetRows == 0);
        if (ImGui::Button(trId("Remove preset##airband_remove"), keySize)) { airbandRemovePreset(); }
        census::rect("airband:remove", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();
        if (!airbandPresetNote_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
            ImGui::TextWrapped("%s", airbandPresetNote_.c_str());
            ImGui::PopStyleColor();
        }
        // A read that is taking long says so, in the Bookmarks section's sentence.
        if (bookmarkImportPending() && bookmarkImportElapsedS() >= 1.0) {
            ImGui::TextDisabled(tr("Waiting for the disk to open the file: %.0f s"), bookmarkImportElapsedS());
        }
        // A file that was written: the key that shows where. The folder is the
        // recordings folder, which the export has made by now.
        if (airbandPresetFolderKey_) {
            if (ImGui::SmallButton(trId("Open the folder##airband_folder"))) { shellOpen(recordDir_); }
            census::rect("airband:openfolder", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                         ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        }
    }

    // --- which rows: one group (an airport's, or "Manual"), or every ticked
    //     AM or NFM row -----------------------------------------------------
    std::vector<std::string> groups;
    for (const cc::Bookmark& b : freqMgr_.list()) {
        if (playsInMonitor(b) && !b.group.empty() &&
            std::find(groups.begin(), groups.end(), b.group) == groups.end()) {
            groups.push_back(b.group);
        }
    }
    std::sort(groups.begin(), groups.end());
    if (!airbandGroup_.empty() && std::find(groups.begin(), groups.end(), airbandGroup_) == groups.end()) {
        airbandGroup_.clear();   // the group was removed from the list
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##airband_group",
                          airbandGroup_.empty() ? tr("Every ticked AM or NFM frequency") : airbandGroup_.c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(trId("Every ticked AM or NFM frequency"), airbandGroup_.empty())) {
            airbandGroup_.clear();
        }
        for (std::size_t g = 0; g < groups.size(); ++g) {
            ImGui::PushID(static_cast<int>(g));
            if (ImGui::Selectable(groups[g].c_str(), airbandGroup_ == groups[g])) {
                airbandGroup_ = groups[g];
                // THE GROUP SHOWN IS THE PRESET ACTED ON (0.99.66): Add, Export CSV and
                // Remove preset take the name from the field above, so choosing a group
                // here puts its name there - a group chosen and a preset named that were
                // two different things would export the wrong list. The name goes in
                // TRIMMED, as the keys read it (a group stored with spaces round it
                // would otherwise leave them dead), and one that still does not fit is
                // not copied cut short - or left as the OLD name, which Export CSV and
                // Remove preset would then act on, unconfirmed: the field is cleared.
                const std::string field = cascade::gui::presetFieldText(groups[g], sizeof(airbandPresetName_));
                std::snprintf(airbandPresetName_, sizeof(airbandPresetName_), "%s", field.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    std::vector<std::size_t> rows = airbandRows();
    const std::vector<cc::Bookmark>& list = freqMgr_.list();
    // What has been heard but not yet written to the list, so the column
    // moves while the monitor plays rather than every twenty seconds.
    const auto pendingFor = [&](const cc::Bookmark& b) {
        for (const AirbandChan& c : airbandChans_) {
            if (c.freqHz == b.freqHz && c.name == b.name) { return c.pendingHeardS; }
        }
        return 0.0;
    };
    const auto chanFor = [&](const cc::Bookmark& b) -> const AirbandChan* {
        if (!airbandListening_) { return nullptr; }
        for (const AirbandChan& c : airbandChans_) {
            if (c.freqHz == b.freqHz) { return &c; }
        }
        return nullptr;
    };
    if (airbandBusyFirst_) {
        std::stable_sort(rows.begin(), rows.end(), [&](std::size_t x, std::size_t y) {
            return list[x].heardSeconds + pendingFor(list[x]) > list[y].heardSeconds + pendingFor(list[y]);
        });
    }

    std::size_t tickedCount = 0;
    for (const std::size_t i : rows) { tickedCount += list[i].scan ? 1u : 0u; }
    census::note("airband:rows:", static_cast<int>(rows.size()));
    census::note("airband:ticked:", static_cast<int>(tickedCount));
    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
    ImGui::Text(tr("%zu ticked of %zu"), tickedCount, rows.size());
    ImGui::PopStyleColor();
    int setAll = -1;
    if (!rows.empty() && !airbandGroup_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(trId("Tick all"))) { setAll = 1; }
        ImGui::SameLine();
        if (ImGui::SmallButton(trId("Untick all"))) { setAll = 0; }
    }
    ImGui::Checkbox(trId("Busiest first"), &airbandBusyFirst_);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Sorts by how long each frequency has been heard talking while\n"
                                   "FoxSDR listened - measured here, at your aerial, and kept."));
    }

    // Rows: tick, live lamp, frequency and name (click to tune the receiver
    // to that one alone), time heard.
    int toggleIdx = -1;
    int tuneIdx = -1;
    const float rowsH = std::min(14.0f, static_cast<float>(rows.size())) * ImGui::GetFrameHeightWithSpacing() + 4.0f;
    if (!rows.empty() && ImGui::BeginChild("##airband_rows", ImVec2(-1.0f, rowsH), ImGuiChildFlags_None)) {
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(rows.size()));
        const float heardW = ImGui::CalcTextSize("00h 00m").x;
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const std::size_t i = rows[static_cast<std::size_t>(r)];
                const cc::Bookmark& b = list[i];
                ImGui::PushID(static_cast<int>(i));
                bool t = b.scan;
                if (ImGui::Checkbox("##tick", &t)) { toggleIdx = static_cast<int>(i); }
                ImGui::SameLine();
                // THE LAMP: lit while this channel's squelch is open.
                const AirbandChan* live = chanFor(b);
                const bool lit = live != nullptr && live->open;
                if (lit) { census::note("airband:open:", static_cast<int>(std::lround(b.freqHz / 1000.0))); }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float d = ImGui::GetFrameHeight();
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(p.x + d * 0.35f, p.y + d * 0.5f), d * 0.22f,
                    lit ? static_cast<ImU32>(cascade::gui::theme::kPhosphor)
                        : cascade::gui::theme::withAlpha(cascade::gui::theme::kInkFaint, 0.5f));
                ImGui::Dummy(ImVec2(d * 0.7f, d));
                ImGui::SameLine();
                char label[256];
                cc::formatUtf8(label, sizeof(label), "%.4f  %s", b.freqHz / 1.0e6, b.name.c_str());
                const float nameW = ImGui::GetContentRegionAvail().x - heardW - spacing;
                if (ImGui::Selectable(label, lit, ImGuiSelectableFlags_None, ImVec2(std::max(40.0f, nameW), 0.0f))) {
                    tuneIdx = static_cast<int>(i);
                }
                if (ImGui::IsItemHovered() && live != nullptr) {
                    ImGui::SetTooltip(tr("Level %.0f dB (squelch %.0f dB)"), static_cast<double>(live->levelDb),
                                      static_cast<double>(airbandSquelchDb_));
                }
                ImGui::SameLine();
                const std::string h = heardText(b.heardSeconds + pendingFor(b));
                ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
                ImGui::TextUnformatted(h.c_str());
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
    }
    if (!rows.empty()) { ImGui::EndChild(); }
    if (setAll >= 0) {
        // Every row of the group, found again BY WHAT IT IS before each
        // update: an index taken before the first update must not be trusted
        // by the second (until 0.99.66 updateAt re-inserted a row after the
        // others on its frequency - CTAF and UNICOM on one frequency - and it
        // still does when the frequency itself changes).
        struct Id {
            double freqHz;
            std::string name;
            std::string group;
        };
        std::vector<Id> ids;
        for (const std::size_t i : rows) { ids.push_back({list[i].freqHz, list[i].name, list[i].group}); }
        for (const Id& id : ids) {
            const std::vector<cc::Bookmark>& now = freqMgr_.list();
            for (std::size_t i = 0; i < now.size(); ++i) {
                if (now[i].freqHz != id.freqHz || now[i].name != id.name || now[i].group != id.group) {
                    continue;
                }
                if (now[i].scan != (setAll == 1)) {
                    cc::Bookmark b = now[i];
                    b.scan = setAll == 1;
                    freqMgr_.updateAt(i, b);
                }
                break;
            }
        }
        saveBookmarks();
    }
    if (toggleIdx >= 0 && toggleIdx < static_cast<int>(list.size())) {
        cc::Bookmark b = list[static_cast<std::size_t>(toggleIdx)];
        b.scan = !b.scan;
        freqMgr_.updateAt(static_cast<std::size_t>(toggleIdx), b);
        saveBookmarks();
    }
    if (tuneIdx >= 0 && tuneIdx < static_cast<int>(list.size())) {
        // One channel, on the receiver: the monitor stops, because the
        // receiver is now somewhere the monitor did not put it.
        const cc::Bookmark b = list[static_cast<std::size_t>(tuneIdx)];
        airbandStop(cc::formatText(tr("Tuned to %s alone."), b.name.c_str()));
        tuneToBookmark(b);
    }

    // --- listening ------------------------------------------------------------
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##airband_sq", &airbandSquelchDb_, -110.0f, 0.0f, tr("squelch %.0f dB"));
    if (ImGui::IsItemDeactivatedAfterEdit() && airbandListening_) {
        // A new threshold rebuilds the block's set (the runner's live squelch
        // slots belong to the patch page); on release, not every frame of a
        // drag, so the channels are not restarted sixty times a second.
        airbandTuneBlock(airbandBlock_);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("A channel is heard while its level is above this. Hover a row\n"
                                   "while listening to see its level; \"Above the noise\" sets it\n"
                                   "for you from the quiet channels."));
    }
    ImGui::BeginDisabled(!airbandListening_);
    if (ImGui::SmallButton(trId("Above the noise"))) {
        // 8 dB over the MEDIAN channel level: most channels are quiet most of
        // the time, so the median is the noise, and a talker sits well above.
        std::vector<float> lv;
        for (const AirbandChan& c : airbandChans_) {
            if (c.levelDb > -199.0f) { lv.push_back(c.levelDb); }
        }
        if (!lv.empty()) {
            std::nth_element(lv.begin(), lv.begin() + static_cast<std::ptrdiff_t>(lv.size() / 2), lv.end());
            airbandSquelchDb_ = std::clamp(lv[lv.size() / 2] + 8.0f, -110.0f, 0.0f);
            airbandTuneBlock(airbandBlock_);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x));
    double hold = airbandHoldS_;
    if (ImGui::InputDouble("##airband_hold", &hold, 0.0, 0.0, tr("hold %.1f s"))) {
        airbandHoldS_ = std::clamp(hold, 0.0, 60.0);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && airbandListening_ && airbandBlocks_.size() > 1) {
        airbandStop("");
        airbandStart();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("How long a block stays after its last word before the scan moves\n"
                                   "on. Only used when the ticked frequencies need more than one block."));
    }

    if (!airbandListening_ && !airbandStartPending_) {
        if (ImGui::Button(trId("LISTEN"), ImVec2(-FLT_MIN, 0.0f))) { airbandStart(); }
        census::rect("airband:listen", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                     ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Plays every ticked AM or NFM frequency the radio's band can hold at\n"
                                       "once, mixed; when they do not all fit, scans between blocks of\n"
                                       "them and stops wherever someone is talking. Uses the receiver's\n"
                                       "radio, so it switches to the RECEIVER view."));
        }
    } else {
        const bool scanning = airbandBlocks_.size() > 1;
        const float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        if (ImGui::Button(trId("STOP##airband"), ImVec2(scanning ? half : -FLT_MIN, 0.0f))) {
            airbandStop("");
        }
        if (scanning) {
            ImGui::SameLine();
            if (ImGui::Button(trId("Next block"), ImVec2(-FLT_MIN, 0.0f))) { airbandScanner_.skip(); }
        }
    }

    if (airbandStartPending_) {
        ImGui::TextWrapped("%s", tr("Handing the radio back from the patch view..."));
    } else if (airbandListening_ && airbandBlock_ < airbandBlocks_.size()) {
        const cc::AirbandBlock& blk = airbandBlocks_[airbandBlock_];
        double lo = 0.0, hi = 0.0;
        if (!blk.members.empty() && blk.members.back() < airbandPlanned_.size()) {
            lo = airbandPlanned_[blk.members.front()].freqHz;
            hi = airbandPlanned_[blk.members.back()].freqHz;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
        if (airbandBlocks_.size() > 1) {
            ImGui::TextWrapped(tr("Block %zu of %zu: %zu channels, %.3f-%.3f MHz"), airbandBlock_ + 1,
                               airbandBlocks_.size(), blk.members.size(), lo / 1e6, hi / 1e6);
        } else {
            ImGui::TextWrapped(tr("All %zu channels at once, %.3f-%.3f MHz"), blk.members.size(), lo / 1e6,
                               hi / 1e6);
        }
        ImGui::PopStyleColor();
        std::string hearing;
        for (const AirbandChan& c : airbandChans_) {
            if (!c.open) { continue; }
            if (!hearing.empty()) { hearing += ", "; }
            hearing += c.name;
        }
        if (!hearing.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
            ImGui::TextWrapped(tr("Hearing: %s"), hearing.c_str());
            ImGui::PopStyleColor();
        }
    }
}

}  // namespace cascade::gui
