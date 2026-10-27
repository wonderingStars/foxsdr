// app_window_airband.cpp - the AIRBAND section: type an airport, tick what to hear, LISTEN. AppWindow members, kept out of app_window.cpp because
// they are one subject (and because that file is at the object-file limit).
//
// THE REQUEST (a US tester, 2026-10-01), in his words: "a plug in that you could simply enter the airport code and it would fill out a list where
// you could select which ones to listen to. If they are close enough together have multiple AM demodulators running simultaneously. If they are not
// close enough together could the SDR scan across the frequencies stopping when it sees a peak and tuning in. When the audio stops, continue
// scanning." And: "there is no way to know which ones are more popular."
//
// WHAT EACH PART IS (master 0.99.60 - 0.99.71; the monitor itself is the ENGINE's since stub batch B6, src/engine/engine_airband.cpp, and this
// file is the window half that batch B7 brought back):
//   - the lookup: core/airband_data.hpp's table (FAA for the US, OurAirports elsewhere, compiled in) puts the airport's frequencies into the
//     FREQUENCY LIST as a group, ticked - so they are also bookmarks on the spectrum, clickable, exportable, and the scanner's list mode can use
//     them. The rows go to the engine as one FOXAPP_OP_BOOKMARK_ADD_ROWS (a LIST);
//   - the add row (0.99.66): a frequency in MHz, an optional name and a mode (AM or NFM) become a ticked row of the PRESET named in the field above
//     it (the group "Manual" until that is changed): FOXAPP_OP_BOOKMARK_ADD_ROWS (rows TYPED IN - the engine judges the radio's range, as a tune the
//     radio refuses is judged, and answers with the sentence). WFM is not offered - the monitor's strips run at 24-96 kHz and wide FM needs 200 kHz;
//   - presets (0.99.66): a preset IS a group of the frequency list. IMPORT reads the .csv or SDR# frequencies.xml whose path is typed on a worker
//     and every row joins the preset whatever group the file gave it, the AM and NFM rows TICKED whatever tick the file gave them, the rest kept
//     UNTICKED; EXPORT CSV writes the preset's rows - every mode, tick kept - into the recordings folder as foxsdr-<preset>-<stamp>.csv, the file
//     Import reads, with a key that opens the folder; REMOVE PRESET is FOXAPI_OP_BOOKMARK_REMOVE_GROUP (the engine stops a monitor that plays the
//     group). Choosing a group in the list puts its name in the preset field, so the keys act on the list that is showing;
//   - the monitor's keys: FOXAPP_OP_AIRBAND (listen, stop, next block, above the noise, squelch, hold, tune one). The preset the monitor plays is
//     the LISTEN's text; a lookup, an add or an import while it listens cannot change what is played (the engine re-cuts the blocks only when the
//     rows of THAT preset change), and the group on show is left alone while it listens;
//   - "which ones are busy": a row's heard time is BOOKMARKS' value[3] plus what its channel has heard since the last write (AIRBAND_CHANNELS
//     value[3]); the rows can be sorted by it;
//   - the spectrum (0.99.66): while the monitor listens the receiver's spectrum shows what is being HEARD instead of the VFO it is not demodulating:
//     each channel of the block on the air is a shaded mark at its frequency and bandwidth, bright with a centre line while its squelch is open
//     and faint while it is shut, the same two states as the lamp in the list; the VFO's shaded band, its line down the waterfall and its "peak in
//     passband" figure are not drawn until the monitor stops (drawCenterPanels reads facts().listening for that; the marks read the LIVE list).
//
// THE LIVE LIST IS NEVER CACHED ON listSeq. AIRBAND_CHANNELS carries each channel's level and open lamp, which change every pass, and it moves no
// listSeq (PATCH_LIVE's rule): airbandReadLive() takes it once an ImGui frame, for whoever draws first - the section's lamps and the spectrum's
// marks read the same copy and cannot disagree. A copy keyed on listSeq would show a lamp as it was whenever something else last moved the
// sequence. The facts (the monitor's state and sentences) and the rows (BOOKMARKS) are cached the way every other panel's are.
//
// WHAT IS NOT HERE, and where it is:
//   - the IMPORT dialog (the empty-box press, a failed read) is app_window_import_dialog.cpp's (stub campaign B8): openImportDialog() opens it;
//     a read that comes back with rows closes it (airbandPollJobs below);
//   - the tester recorder's feature notes ("airband", "airband-listen" - master's testerUsage_.noteFeature): the window's recorder is batch B15's;
//     the call sites are marked NOTEFEATURE;
//   - (the top bar's SQUELCH AUTO and the deck's squelch dial following the monitor are batch B12's, app_window_deck.cpp; the slider and "Above the noise"
//     below send the engine's actions, and the ENGINE turns AUTO off for them - master's setSquelchAuto(false) - so there is no window code for it.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/unique_file.hpp"
#include "core/utf8_text.hpp"
#include "core/write_fault.hpp"
#include "gui/airband_marker_geometry.hpp"
#include "gui/bench_rail.hpp"
#include "gui/disk_job.hpp"
#include "gui/fonts.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"
#include "gui/ui_census.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

// --- the two disk jobs (see airband_window_state.hpp) --------------------------------------------------------------------------------------

// What the worker that reads a frequency list hands back: the rows already put in the preset (applyImportInto), as the lines the engine's
// BOOKMARK_ADD_ROWS takes, cut into chunks it will accept.
struct AirbandImportOutcome {
    std::string path;                 // the last place looked (a bare name is looked for in the recordings folder too)
    std::string error;                // the core's reason when nothing could be read; "" when rows were
    std::string format;
    std::size_t found = 0;
    std::size_t playable = 0;         // of them, AM or NFM: what the monitor can play
    std::size_t skipped = 0;
    std::size_t shifted = 0;
    double ms = 0.0;
    std::vector<std::string> chunks;  // ADD_ROWS text, at most kChunkBytes each
};

struct AirbandExportOutcome {
    bool ok = false;
    std::string shown;                // the path as the note names it
    std::size_t count = 0;
};

struct AirbandJobs {
    DiskJob<AirbandImportOutcome> import{"Airband preset import"};
    DiskJob<AirbandExportOutcome> exportCsv{"Airband preset export"};
    // WHO ASKED, kept on the window's thread for the poll: the preset an import in flight was pressed for.
    std::string importGroup;
    std::string importPath;
};

namespace {

// The most one ADD_ROWS carries (FOXAPI_MAX_LONG_TEXT is 1 MiB): a longer list goes in several commands.
constexpr std::size_t kChunkBytes = 512u * 1024u;

// The preset the section starts with, and the one an emptied name field means (0.99.66). A name in the user's list, not a label: it is stored as
// it is here in every language.
constexpr const char* kManualGroup = "Manual";

// FOXAPI_DEMOD_* codes the monitor plays: exactly AM and NFM (core/airband_monitor.hpp's monitorModeFor, which this file may not include - it
// reaches the patch runner, which the fence denies). BOOKMARKS' value[1] is foxDemodFromName(mode), an exact match, so a code is a name.
bool playsCode(std::uint32_t modeCode) { return modeCode == FOXAPI_DEMOD_AM || modeCode == FOXAPI_DEMOD_NFM; }
bool playsName(const std::string& mode) { return mode == "AM" || mode == "NFM"; }

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

// A field of an ADD_ROWS line: the text with the characters that would split the line or the field made spaces (the sender's to clean).
std::string cleanField(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '\t' || c == '\r' || c == '\n') { c = ' '; }
    }
    return out;
}

// One ADD_ROWS line: "<Hz> TAB <mode> TAB <bandwidth Hz> TAB <tick 0/1> TAB <group> TAB <name>".
void appendRow(std::string& text, const cc::Bookmark& b, bool tick) {
    char head[96];
    std::snprintf(head, sizeof(head), "%.6f\t", b.freqHz);
    text += head;
    text += cleanField(b.mode);
    char bw[48];
    std::snprintf(bw, sizeof(bw), "\t%.3f\t%d\t", b.bandwidthHz, tick ? 1 : 0);
    text += bw;
    text += cleanField(b.group);
    text += '\t';
    text += cleanField(b.name);
    text += '\n';
}

// The rows as ADD_ROWS text, cut into chunks no longer than kChunkBytes at a line end.
std::vector<std::string> rowChunks(const std::vector<cc::Bookmark>& items) {
    std::vector<std::string> chunks;
    std::string cur;
    for (const cc::Bookmark& b : items) {
        std::string line;
        appendRow(line, b, b.scan);
        if (!cur.empty() && cur.size() + line.size() > kChunkBytes) {
            chunks.push_back(std::move(cur));
            cur.clear();
        }
        cur += line;
    }
    if (!cur.empty()) { chunks.push_back(std::move(cur)); }
    return chunks;
}

}  // namespace

AirbandJobs& AppWindow::airbandJobs() {
    if (!airband_.jobs) { airband_.jobs = std::make_shared<AirbandJobs>(); }
    return *airband_.jobs;
}

// --- what the engine published ---------------------------------------------------------------------------------------------------------

// fact.airband.*, read again only when listSeq has moved (they move with the monitor's state, its note and its settings).
const AirbandFacts& AppWindow::airbandFacts() {
    AirbandFacts& f = airband_.facts;
    if (uiSession_ == nullptr || f.listSeq == uiState_.listSeq) { return f; }
    f.listSeq = uiState_.listSeq;
    std::string t;
    readFactText("fact.airband.listening", t);
    f.listening = t == "1";
    readFactText("fact.airband.pending", t);
    f.pending = t == "1";
    readFactText("fact.airband.preset", f.preset);
    readFactText("fact.airband.note", f.note);
    readFactText("fact.airband.block", f.block);
    readFactText("fact.airband.blocks", t);
    f.blocks = std::atoi(t.c_str());
    readFactText("fact.airband.squelchDb", t);
    f.squelchDb = static_cast<float>(std::atof(t.c_str()));
    readFactText("fact.airband.holdS", t);
    f.holdS = std::atof(t.c_str());
    return f;
}

// AIRBAND_CHANNELS, once an ImGui frame and NEVER on listSeq (see the top of this file): the first caller of a frame reads it, the rest share
// the copy. The rows keep their room, so a frame in which the channels did not change allocates nothing.
void AppWindow::airbandReadLive() {
    AirbandWindowState& a = airband_;
    const int frame = ImGui::GetFrameCount();
    if (a.liveFrame == frame) { return; }
    a.liveFrame = frame;
    a.liveCount = 0;
    if (uiSession_ == nullptr) { return; }
    const int32_t n = std::max<int32_t>(0, engineApi_->read_list(uiSession_, FOXAPI_LIST_AIRBAND_CHANNELS, 0, nullptr));
    if (a.live.size() < static_cast<std::size_t>(n)) { a.live.resize(static_cast<std::size_t>(n)); }
    FoxListItem it{};
    for (int32_t i = 0; i < n; ++i) {
        it = FoxListItem{};
        it.structSize = sizeof(it);
        // `got <= i` is the end of a list that changed under the read: what was read stands.
        if (engineApi_->read_list(uiSession_, FOXAPI_LIST_AIRBAND_CHANNELS, static_cast<uint32_t>(i), &it) <= i) { break; }
        AirbandChannel& c = a.live[a.liveCount++];
        c.freqHz = it.value[0];
        c.bandwidthHz = it.value[1];
        c.levelDb = static_cast<float>(it.value[2]);
        c.pendingHeardS = it.value[3];
        c.open = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
        c.onAir = (it.flags & FOXAPI_ITEM_ON_AIR) != 0u;
        c.name.assign(it.name);
    }
}

// THE PRESET THE SECTION ACTS ON (0.99.66): what the name field holds, without the spaces round it, or "Manual" when nothing is left. ASCII
// spaces only, so a name in any script is never cut.
std::string AppWindow::airbandPresetName() const {
    const std::string t = cascade::gui::trimPresetName(airband_.presetName);
    return t.empty() ? std::string(kManualGroup) : t;
}

// THE ROWS THE SECTION LISTS, from the window's one copy of BOOKMARKS (tuningLists): the group on show's AM and NFM rows, or - with none on
// show - every ticked AM or NFM row; the groups the combo offers; how many rows the preset in the field has. Rebuilt only when the copy, the
// group on show or the preset changed - an unchanged frame costs three comparisons, a 33,000-row list included.
void AppWindow::airbandRebuildView() {
    const TuningListCache& c = tuningLists();
    AirbandWindowState& a = airband_;
    AirbandView& v = a.view;
    const std::string preset = airbandPresetName();
    if (v.copies == c.markerCopies && v.group == a.group && v.preset == preset) { return; }
    const std::vector<cascade::gui::BookmarkRow>& list = c.markers;
    if (v.copies != c.markerCopies) {
        std::set<std::string_view> groups;
        for (const cascade::gui::BookmarkRow& b : list) {
            if (playsCode(b.mode) && !b.group.empty()) { groups.insert(b.group); }
        }
        v.groups.assign(groups.begin(), groups.end());
    }
    // A group that has left the list (removed here, or in the Bookmarks section) is no longer on show.
    if (!a.group.empty() && std::find(v.groups.begin(), v.groups.end(), a.group) == v.groups.end()) { a.group.clear(); }
    v.rows.clear();
    v.presetRows = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const cascade::gui::BookmarkRow& b = list[i];
        if (b.group == preset) { ++v.presetRows; }
        if (!playsCode(b.mode)) { continue; }
        if (a.group.empty() ? b.ticked : (b.group == a.group)) { v.rows.push_back(static_cast<std::uint32_t>(i)); }
    }
    v.copies = c.markerCopies;
    v.group = a.group;
    v.preset = preset;
}

// THE TWO LINES THAT SAY WHAT IS BEING HEARD (0.99.71): the block on the air - the engine's sentence, "Block 2 of 6: 10 channels, 118.100-119.950
// MHz", or "All 7 channels at once, ..." - and the names of the channels whose squelch is open this frame. Built once, here, for both places that
// show them: the section in the rail (above its row list) and the caption on the spectrum (drawAirbandCaption).
bool AppWindow::airbandStatusText(std::string& block, std::string& hearing) {
    block.clear();
    hearing.clear();
    const AirbandFacts& f = airbandFacts();
    if (!f.listening || f.block.empty()) { return false; }
    airbandReadLive();
    block = f.block;
    for (std::size_t i = 0; i < airband_.liveCount; ++i) {
        const AirbandChannel& c = airband_.live[i];
        if (!c.open) { continue; }
        if (!hearing.empty()) { hearing += ", "; }
        hearing += c.name;
    }
    return true;
}

// --- sending ---------------------------------------------------------------------------------------------------------------------------

// A command whose answer this file waits for: its ticket is kept with what the answer is about (airbandOnAnswer).
std::uint64_t AppWindow::airbandSend(const cmd::QueuedCommand& q, AirbandAsk::Kind kind, const std::string& group,
                                     std::size_t total, std::size_t ticked) {
    const std::uint64_t ticket = uiSubmitTicket(q);
    if (ticket == 0u) { return 0u; }   // the session would not take it (full): nothing to wait for
    AirbandAsk ask;
    ask.kind = kind;
    ask.ticket = ticket;
    ask.group = group;
    ask.total = total;
    ask.ticked = ticked;
    airband_.asks.push_back(std::move(ask));
    return ticket;
}

// --- the lookup ------------------------------------------------------------------------------------------------------------------------

void AppWindow::airbandAddAirport(const cc::Airport& a) {
    airband_.choices.clear();
    const std::vector<cc::Bookmark> items = cc::airportBookmarks(a);
    std::string text;
    std::size_t ticked = 0;
    for (const cc::Bookmark& b : items) {
        appendRow(text, b, b.scan);
        ticked += b.scan ? 1u : 0u;
    }
    const std::string group = cc::airportGroupName(a);
    // AS A LIST (ival[0] 1): added as an import adds - a row already there, identical in name, frequency and group, is skipped - and not judged
    // against the radio's range. The note and the group on show are settled when the engine answers (airbandOnAnswer), with the count it added.
    // (A session that is full answers BUSY and nothing is sent: the lookup is asked again by pressing it again - the section has no sentence for it.)
    airbandSend(cmd::makeText(FOXAPP_OP_BOOKMARK_ADD_ROWS, text, 1), AirbandAsk::Kind::AirportRows, group, items.size(), ticked);
}

void AppWindow::airbandLookup(const std::string& code) {
    airband_.choices.clear();
    const std::vector<const cc::Airport*> found = cc::findAirports(cc::airbandTable(), code);
    if (found.empty()) {
        airband_.note = cc::formatText(tr("No airport with the code \"%s\" publishes an airband "
                                          "frequency in FoxSDR's table."),
                                       code.c_str());
        return;
    }
    if (found.size() == 1) {
        airbandAddAirport(*found.front());
        return;
    }
    airband_.choices = found;
    airband_.note = cc::formatText(tr("\"%s\" names %zu airports - pick one:"), code.c_str(), found.size());
}

// SHOW A GROUP THE SECTION HAS JUST ADDED TO (0.99.66 review): Add and the preset's Import put the group on show - as an airport's lookup does,
// since 0.99.69 - but only when that cannot change what is being played. The group on show is what a LISTEN plays and what choosing in the
// combo restarts a listening monitor on, so a monitor that is listening (or about to) keeps its choice: a row added to another preset must not
// narrow "every ticked" down to it. And a group the monitor has nothing to play in (an import of WFM rows only, a file that added nothing) is not
// put on show: the combo would name a group the list may not even have.
void AppWindow::airbandShowGroup(const std::string& group) {
    const AirbandFacts& f = airbandFacts();
    if (f.listening || f.pending) { return; }
    for (const cascade::gui::BookmarkRow& b : tuningLists().markers) {
        if (b.group == group && playsCode(b.mode)) {
            airband_.group = group;
            return;
        }
    }
}

// A FREQUENCY OF YOUR OWN (0.99.66). The add row's Add key: the typed MHz, the mode chosen and the optional name become one ticked row of the
// preset named in the field above (the group "Manual" until that is changed) in the frequency list. The engine adds it - and, when the open radio
// publishes a range, refuses a frequency outside it with the sentence that says what it does cover; the fields are cleared when it has.
void AppWindow::airbandAddManual() {
    double hz = 0.0;
    if (!cascade::gui::parseMhz(airband_.addMhz, hz)) {
        airband_.note = cc::formatText(tr("could not read frequency \"%s\""), airband_.addMhz);
        return;
    }
    cc::Bookmark b;
    b.mode = airband_.addMode == 1 ? "NFM" : "AM";
    b.freqHz = hz;
    b.bandwidthHz = cc::defaultBandwidthForMode(b.mode);
    b.group = airbandPresetName();
    b.name = airband_.addName;   // empty: the engine names the row by its frequency, as "Add current" does
    std::string text;
    appendRow(text, b, true);
    // (A session that is full answers BUSY and nothing is sent; the typed text stays, and pressing Add again sends it.)
    airbandSend(cmd::makeText(FOXAPP_OP_BOOKMARK_ADD_ROWS, text, 0), AirbandAsk::Kind::TypedRow, b.group, 1, 1);
}

// --- presets ---------------------------------------------------------------------------------------------------------------------------

// THE PRESET'S IMPORT KEY: the file at the typed path, read on a worker, its rows into the preset - in its group, the AM and NFM ones ticked,
// whatever the file said. With the box EMPTY (0.99.80) it opens the "Import a frequency list" window for this preset instead (batch B8).
void AppWindow::airbandImportPreset() {
    if (airband_.importPath[0] == '\0') {
        openImportDialog(cc::ImportInto{airbandPresetName(), true}, std::string());
        return;
    }
    airband_.presetNote.clear();
    airband_.presetFolderKey = false;
    airbandImportFile(airband_.importPath);
}

// THE DISK IS NOT TOUCHED HERE (0.99.65): the file the user typed - a path that can be a network share or a sleeping drive - is read and parsed
// on a worker (gui/disk_job.hpp), which owns everything it touches by value; airbandPollJobs() collects it. ONE READ AT A TIME: a press made
// while one is out is remembered (the last one) and read when the first is back.
void AppWindow::airbandImportFile(const std::string& path) {
    // What was typed or pasted, as a path (cleanImportPath, 0.99.82): the quotes Explorer's "Copy as path" puts round it, the Enter or the space a
    // paste leaves after it, a file:// address.
    const std::string p = cc::cleanImportPath(path);
    AirbandJobs& j = airbandJobs();
    if (j.import.pending()) {
        airband_.importAgain = p;
        return;
    }
    const std::string group = airbandPresetName();
    j.importGroup = group;
    j.importPath = p;
    std::string dir;
    readFactText("fact.recorder.dir", dir);
    j.import.request([p, group, dir]() -> AirbandImportOutcome {
        const auto t0 = std::chrono::steady_clock::now();
        AirbandImportOutcome o;
        o.path = p;
        // A RELATIVE PATH (0.99.80 for a bare name, 0.99.82 for any): "frequencies.xml", as Export showed it, opens relative to the process's
        // working directory first and, when that does not open, in the recordings folder where Export wrote it.
        cc::ImportResult r = cc::importFrequencyFileIn(p, dir, o.path);
        o.format = r.format;
        if (!r.error.empty() && r.items.empty()) {
            o.error = r.error;
            return o;
        }
        o.found = r.items.size();
        o.skipped = r.skipped;
        o.shifted = r.shifted;
        // THE PRESET'S NAME AND TICK, put on every row here, after the file was read and before the rows join the list (0.99.66): whatever group
        // and tick the file gave them, they are the preset's, ticked when the monitor can play them (AM, NFM) and not when it cannot.
        cc::applyImportInto(r.items, cc::ImportInto{group, true});
        for (const cc::Bookmark& b : r.items) { o.playable += playsName(b.mode) ? 1u : 0u; }
        o.chunks = rowChunks(r.items);
        o.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return o;
    });
}

// A finished read is put in the list; a finished write says where it went. Once a frame (airbandFrame), whether the section is open or not.
void AppWindow::airbandPollJobs() {
    if (!airband_.jobs) { return; }
    AirbandJobs& j = *airband_.jobs;
    AirbandImportOutcome o;
    if (j.import.poll(o)) {
        AirbandImportRun& run = airband_.import;
        run = AirbandImportRun{};
        const std::string group = j.importGroup;
        // A worker that threw (an allocation, a corrupt file): the same sentence a file that cannot be read gets, instead of a crash in the frame.
        if (j.import.threw() && o.error.empty()) { o.error = "cannot read the file"; }
        const std::string& p = o.path.empty() ? j.importPath : o.path;
        if (!o.error.empty()) {
            // THE REASON IN THE USER'S LANGUAGE (0.99.80): the core's three reasons are English sentences, which put an English tail on "Could
            // not import: ..."; the log below keeps the core's own words. Anything else passes through as it was.
            std::string reason = o.error;
            if (o.error.rfind("cannot open", 0) == 0) {
                reason = cc::formatText(tr("the file \"%s\" could not be opened"), p.c_str());
            } else if (o.error.rfind("cannot read", 0) == 0) {
                reason = cc::formatText(tr("the file \"%s\" could not be read"), p.c_str());
            } else if (o.error.rfind("no line had a usable frequency", 0) == 0) {
                reason = tr("no line of the file had a usable frequency");
            }
            airband_.presetNote = cc::formatText(tr("Could not import: %s"), reason.c_str());
            // The file's KIND, never its name or path: "never the name or path of a file you opened" (PRIVACY.md).
            std::string why = o.error;
            for (std::size_t at = why.find(p); !p.empty() && at != std::string::npos; at = why.find(p, at)) {
                why.replace(at, p.size(), "(the file)");
            }
            const auto extension = std::filesystem::path(std::u8string(p.begin(), p.end())).extension().u8string();
            const std::string kind(extension.begin(), extension.end());
            cc::diagWarnf("bookmarks: an import (%s) failed: %s", kind.c_str(), why.c_str());
            // THE "Import a frequency list" WINDOW opens on any import that came back with no rows, with the sentence above and the target of the
            // import that failed (batch B8).
            openImportDialog(cc::ImportInto{group, true}, airband_.presetNote);
        } else {
            // An import the window was waiting for came back WITH rows: it has done its job, and the "Import a frequency list" window - open
            // if the import began there (a Browse, a dropped file) - closes (batch B8).
            importDialogOpen_ = false;
            importDialogQueued_ = false;
            importDialogReason_.clear();
            run.active = true;
            run.group = group;
            run.found = o.found;
            run.playable = o.playable;
            run.skipped = o.skipped;
            run.shifted = o.shifted;
            run.queue = std::move(o.chunks);
            cc::diagLogf("bookmarks: read a %s file for a preset - %zu read, %.0f ms", o.format.c_str(), o.found, o.ms);
            // A file whose rows were all unusable has nothing to send: the note is said at once, with nothing new.
            if (run.queue.empty()) { airbandFinishImport(); }
        }
        if (!airband_.importAgain.empty()) {
            std::string next;
            next.swap(airband_.importAgain);
            airbandImportFile(next);
        }
    }
    AirbandExportOutcome e;
    if (j.exportCsv.poll(e)) {
        std::string said;
        if (j.exportCsv.threw() && !e.ok && e.shown.empty()) { e.shown = "(the file)"; }
        if (e.ok) {
            said = cc::formatText(tr("Exported %zu to %s"), e.count, e.shown.c_str());
        } else {
            said = cc::formatText(tr("Could not write %s"), e.shown.c_str());
        }
        airband_.presetNote = said;
        airband_.presetFolderKey = e.ok;   // a file that was written offers to open its folder
    }
}

// The rows of a read go to the engine a chunk a frame, in order, until every one is taken (a session that is full answers BUSY: the chunk is
// offered again next frame).
void AppWindow::airbandSendChunks() {
    AirbandImportRun& run = airband_.import;
    if (!run.active) { return; }
    while (run.next < run.queue.size()) {
        if (airbandSend(cmd::makeText(FOXAPP_OP_BOOKMARK_ADD_ROWS, run.queue[run.next], 1), AirbandAsk::Kind::ImportRows, run.group, 0, 0) ==
            0u) {
            return;
        }
        ++run.chunksOut;
        ++run.next;
    }
}

// An import whose every chunk has been answered: the note, the group on show, the log.
void AppWindow::airbandFinishImport() {
    AirbandImportRun& run = airband_.import;
    std::string said = cc::formatText(
        tr("Preset \"%s\": %zu read, %zu new; the monitor can play %zu of the rows read (AM or NFM)."), run.group.c_str(), run.found,
        run.added, run.playable);
    if (run.skipped > 0) {
        said += " ";
        said += tr("Some rows of the file had no usable frequency.");
    }
    // THE CONVERTER'S SHIFT is not applied (importSdrSharpXml): the Bookmarks section's note says so, and a preset's must too - an SDR# file with
    // Shift values would otherwise land at the wrong frequencies without a word.
    if (run.shifted > 0) {
        said += " ";
        said += tr("Converter Shift values were not applied.");
    }
    if (run.refused) { said = airband_.note; }
    airband_.presetNote = said;
    airband_.presetFolderKey = false;
    // Show the preset the rows joined, as the lookup of an airport does - unless the monitor is listening, or the preset has nothing it can play.
    if (!run.refused) { airbandShowGroup(run.group); }
    // NOTEFEATURE: testerUsage_.noteFeature("airband") - master noted the feature here; the window's tester recorder is batch B15's.
    cc::diagLogf("bookmarks: a preset import added %zu of %zu rows", run.added, run.found);
    run = AirbandImportRun{};
}

// THE PRESET'S REMOVE KEY: its rows leave the frequency list, whatever their mode or tick. No confirmation, as the Bookmarks section's "Remove this
// group" has none. A monitor that is playing this preset is stopped by the ENGINE ("Stopped: the preset was removed.", before the rows go - the
// heard time goes into them first), so it is never cut again from "every ticked row" of the rest of the list.
void AppWindow::airbandRemovePreset() {
    const std::string g = airbandPresetName();
    airbandSend(cmd::makeText(FOXAPI_OP_BOOKMARK_REMOVE_GROUP, g), AirbandAsk::Kind::RemoveGroup, g, 0, 0);
    airband_.presetFolderKey = false;
}

// THE PRESET'S EXPORT KEY (0.99.66): the rows of one group of the list, every mode, ticked or not, as a CSV that importCsv reads back whole, with
// its tick - into the recordings folder, as foxsdr-<preset>-<stamp>.csv, written on a worker under a name taken atomically (two exports in one
// second are two files).
//
// BUILT FROM THE WINDOW'S COPY OF BOOKMARKS (tuningLists), so a name or a group the table cut at its item size (95 and 159 bytes) is exported cut
// - the one thing this key does not do as master's did (which read the list itself); no figure of a row is lost otherwise (frequency, mode,
// bandwidth, favourite and tick all travel). A mode no code names (the table's 0) is written empty: the importer reads it as the default.
void AppWindow::airbandExportPreset() {
    AirbandJobs& j = airbandJobs();
    if (j.exportCsv.pending()) { return; }
    const std::string group = airbandPresetName();
    static const char* const kCsvModeNames[] = {"", "NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};   // by FOXAPI_DEMOD_*
    std::vector<cc::Bookmark> out;
    for (const cascade::gui::BookmarkRow& r : tuningLists().markers) {
        if (r.group != group) { continue; }
        cc::Bookmark b;
        b.name = r.name;
        b.freqHz = r.hz;
        b.mode = r.mode <= 8u ? kCsvModeNames[r.mode] : "";
        b.bandwidthHz = r.bandwidthHz;
        b.group = r.group;
        b.favourite = r.favourite;
        b.scan = r.ticked;
        out.push_back(std::move(b));
    }
    airband_.presetFolderKey = false;
    if (out.empty()) {
        airband_.presetNote = cc::formatText(tr("Nothing to export: the preset \"%s\" has no rows."), group.c_str());
        return;
    }
    // THE PRESET'S NAME IN THE FILE'S, as far as a file name can carry it: letters, digits, '-' and '_'; anything else (a space, a slash, a letter
    // outside ASCII) is '_', and a long name is cut, so the name is a name on every file system.
    std::string safe;
    for (const char c : group) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        safe.push_back(ok ? c : '_');
        if (safe.size() >= 40) { break; }
    }
    std::string dir;
    readFactText("fact.recorder.dir", dir);
    if (dir.empty()) { dir = "."; }
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char name[128];
    std::strftime(name, sizeof(name), ("foxsdr-" + safe + "-%Y%m%d-%H%M%S.csv").c_str(), &tmv);
    const std::string shown = dir + "/" + name;
    const std::string path = (std::filesystem::path(dir) / name).string();
    std::string text = cc::exportCsv(out);
    const std::size_t count = out.size();
    j.exportCsv.request([text = std::move(text), dir, path, shown, count]() -> AirbandExportOutcome {
        AirbandExportOutcome r;
        r.count = count;
        r.shown = shown;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        // THE NAME IS TAKEN ATOMICALLY HERE (0.99.65, core/unique_file.hpp): two exports inside one second are two files, and the note names the
        // one on disk.
        std::string used;
        bool exhausted = false;
        int index = 1;
        if (!cc::reserveUnique(path, used, exhausted, &index)) { return r; }   // not written
        r.shown = cc::numberedPath(shown, index);
        std::ofstream f(used, std::ios::binary | std::ios::trunc);
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        cc::writeFaultPoint("export", f);   // test seam: a write that fails part way
        f.close();
        r.ok = static_cast<bool>(f);
        // A list that did not reach the disk whole is not left behind (the empty file taken for it).
        if (!r.ok) { std::filesystem::remove(std::filesystem::path(used), ec); }
        return r;
    });
    airband_.presetNote = tr("Saving...");
}

// --- listening -------------------------------------------------------------------------------------------------------------------------

// LISTEN on the group on show ("" = every ticked AM or NFM row). The engine does the rest: with the patch view showing it closes it and waits
// for the radio (fact.airband.pending; airbandFrame follows with the window's own view), and it says why when it would not start. The same
// preset while listening is already so; another is the monitor starting again on it.
void AppWindow::airbandListen() {
    airbandSend(cmd::makeText(FOXAPP_OP_AIRBAND, airband_.group, 1), AirbandAsk::Kind::Listen, airband_.group, 0, 0);
    // NOTEFEATURE: testerUsage_.noteFeature("airband-listen") - batch B15.
}

// --- the answers -----------------------------------------------------------------------------------------------------------------------

// Called from actOnUiAnswers for the ops this file sends, AFTER the frame's state was read: the facts and the rows read now are at least as new
// as the snapshot that raised the answer.
void AppWindow::airbandOnAnswer(const FoxCommandResult& r) {
    AirbandWindowState& a = airband_;
    const auto it = std::find_if(a.asks.begin(), a.asks.end(), [&](const AirbandAsk& k) { return k.ticket == r.ticket; });
    if (it == a.asks.end()) { return; }
    const AirbandAsk ask = *it;
    a.asks.erase(it);
    const bool ok = r.status == FOXAPI_OK;
    switch (ask.kind) {
        case AirbandAsk::Kind::TypedRow:
            if (ok) {
                a.addMhz[0] = '\0';
                a.addName[0] = '\0';
                airbandShowGroup(ask.group);   // show the list it joined, as an airport's lookup does - unless listening
                a.note.clear();
                // NOTEFEATURE: testerUsage_.noteFeature("airband") - batch B15.
            } else {
                // The engine's sentence: a frequency outside what the open radio covers says what it does cover. The typed text stays.
                a.note = r.message;
            }
            break;
        case AirbandAsk::Kind::AirportRows: {
            if (!ok) {
                a.note = r.message;
                break;
            }
            const std::size_t added = static_cast<std::size_t>(r.applied[0]);
            // PUT THE AIRPORT ON SHOW - unless the monitor is listening (0.99.69). The group on show is what the monitor is restarted on when it
            // is changed, and a lookup used to replace the preset being heard with the airport's rows, or narrow "every ticked" down to them,
            // under the listener's ear. The rows are added either way, as Add and Import do.
            airbandShowGroup(ask.group);
            if (added == 0) {
                a.note = cc::formatText(tr("%s is already in the frequency list (%zu frequencies)."), ask.group.c_str(), ask.total);
            } else {
                a.note = cc::formatText(
                    tr("%s: %zu frequencies added to the frequency list, %zu ticked. ATIS and weather "
                       "broadcasts are left unticked - they never stop talking."),
                    ask.group.c_str(), added, ask.ticked);
            }
            // A monitor playing ANOTHER preset does not play these rows, and a note that says "ticked" would read as though it did: say what it
            // is still playing and where the choice is. ("Every ticked" plays them - they are ticked - and the airport's own list needs no
            // telling.)
            const AirbandFacts& f = airbandFacts();
            if (f.listening && !f.preset.empty() && f.preset != ask.group) {
                a.note += " " + cc::formatText(
                                    tr("The monitor keeps playing %s; choose %s in the group list to "
                                       "hear it."),
                                    f.preset.c_str(), ask.group.c_str());
            }
            // NOTEFEATURE: testerUsage_.noteFeature("airband") - batch B15.
            cc::diagLogf("airband: an airport's list added (%zu frequencies, %zu new)", ask.total, added);
            break;
        }
        case AirbandAsk::Kind::ImportRows: {
            AirbandImportRun& run = a.import;
            if (run.chunksOut > 0) { --run.chunksOut; }
            if (ok) {
                run.added += static_cast<std::size_t>(r.applied[0]);
            } else {
                run.refused = true;
                a.note = r.message;
            }
            if (run.active && run.chunksOut == 0 && run.next >= run.queue.size()) { airbandFinishImport(); }
            break;
        }
        case AirbandAsk::Kind::RemoveGroup:
            if (ok) {
                a.presetNote = cc::formatText(tr("Removed %zu from \"%s\""), static_cast<std::size_t>(r.applied[0]), ask.group.c_str());
                cc::diagLogf("airband: a preset was removed (%zu rows)", static_cast<std::size_t>(r.applied[0]));
            }
            break;
        case AirbandAsk::Kind::Listen:
            // A LISTEN the engine would not start says why, in the language in force when it said it (fact.airband.note, whole - the answer's
            // message is cut at 127 bytes).
            if (!ok) { a.note = airbandFacts().note.empty() ? std::string(r.message) : airbandFacts().note; }
            break;
    }
}

// --- the frame -------------------------------------------------------------------------------------------------------------------------

// ONCE A FRAME, whether the section is drawn or not (drawUi, with the polls): collects the disk jobs, sends the rows still to go, follows the
// engine's wish to switch to the receiver view, takes over the monitor's sentences and tells the census.
void AppWindow::airbandFrame() {
    if (uiSession_ == nullptr) { return; }
    airbandPollJobs();
    airbandSendChunks();
    const AirbandFacts& f = airbandFacts();

    // THE PATCH VIEW HAS THE RECEIVER'S RADIO. LISTEN with it showing makes the engine close it and wait for the radio (fact.airband.pending); the
    // window follows by showing the receiver view, ONCE, on the rising edge of "wants to listen" - not every frame it is true, which would fight
    // a hand that clicks PATCH again while the engine is still saying so (the engine cancels a wait, and stops a monitor, for that click).
    const bool wanting = f.pending || f.listening;
    if (wanting && !airband_.wasWanting && patchOpen_) { setMainViewPatch(false); }
    airband_.wasWanting = wanting;

    // What the monitor last said (a stop's reason, a refused LISTEN) replaces what the section said, when it changes.
    if (f.note != airband_.noteSeen) {
        airband_.noteSeen = f.note;
        airband_.note = f.note;
    }

    // The settings a hand was on are the engine's again once its figure has caught up.
    if (airband_.squelchHeld && std::fabs(f.squelchDb - airband_.squelchEdit) < 0.05f) { airband_.squelchHeld = false; }
    if (airband_.holdHeld && std::fabs(f.holdS - airband_.holdEdit) < 0.05) { airband_.holdHeld = false; }

    if (f.listening) {
        airbandReadLive();
        cascade::gui::census::note("airband:listening");
        cascade::gui::census::note("airband:blocks:", f.blocks);
        cascade::gui::census::note("airband:channels:", static_cast<int>(airband_.liveCount));
        // WHERE THE BLOCK LANDED: the receiver's centre once a block is on the air (the engine tunes it, and reads back where it landed).
        for (std::size_t i = 0; i < airband_.liveCount; ++i) {
            if (airband_.live[i].onAir) {
                cascade::gui::census::note("airband:centre:", static_cast<int>(std::lround(uiState_.centreHz / 1000.0)));
                break;
            }
        }
    }
}

// --- the spectrum ----------------------------------------------------------------------------------------------------------------------

// WHAT THE SPECTRUM SAYS WHILE THE MONITOR LISTENS (0.99.66). The strips tap the raw band at each channel's own offset and the VFO's audio is
// faded out for their mix, so the VFO's shaded band - which the receiver view draws as "what is being demodulated" - names nothing that is heard.
// drawCenterPanels leaves it, its waterfall line and its passband peak off while the monitor listens, and this draws what IS heard in their
// place: one mark per channel of the block on the air, in the two states the list's lamp has.
//
// Fed from the LIVE channels (airbandReadLive: this frame's read, never a copy keyed on listSeq), so the open channel's mark is bright exactly
// when its lamp in the list is lit. Called after the bookmark markers and before the gridlines, so the axis furniture stays on top; clipped to the
// panel, because a mark at the edge of the view must not paint the header or the waterfall under it. No names: the bookmark markers already
// carry them, and a mark that also lettered itself would hide the trace it is meant to sit on.
//
// The census gets "airband:mark:<kHz>" for a channel whose squelch is open (the same spelling as the list's "airband:open:<kHz>") and the
// rectangle of every mark drawn, so the test can hold the marks to where the channels are.
void AppWindow::drawAirbandMarkers(float x0, float y0, float width, float height) {
    airbandReadLive();
    const AirbandWindowState& a = airband_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, y0 + height), true);
    const ImU32 openFill = cascade::gui::theme::withAlpha(cascade::gui::theme::kPhosphor, 0.30f);
    const ImU32 openLine = cascade::gui::theme::kPhosphor;
    const ImU32 shutFill = cascade::gui::theme::withAlpha(cascade::gui::theme::kPhosphor, 0.08f);
    const ImU32 shutLine = cascade::gui::theme::withAlpha(cascade::gui::theme::kPhosphorDim, 0.6f);
    for (std::size_t i = 0; i < a.liveCount; ++i) {
        const AirbandChannel& c = a.live[i];
        if (!c.onAir) { continue; }
        cascade::gui::AirbandMarkExtent e;
        if (!cascade::gui::airbandMarkExtent(scale_, c.freqHz, c.bandwidthHz, x0, width, e)) { continue; }
        dl->AddRectFilled(ImVec2(e.xLo, y0), ImVec2(e.xHi, y0 + height), c.open ? openFill : shutFill);
        dl->AddLine(ImVec2(e.xCentre, y0), ImVec2(e.xCentre, y0 + height), c.open ? openLine : shutLine, 1.0f);
        const int kHz = static_cast<int>(std::lround(c.freqHz / 1000.0));
        if (c.open) { cascade::gui::census::note("airband:mark:", kHz); }
        cascade::gui::census::rect("airband:mk:", kHz, e.xLo, y0, e.xHi, y0 + height);
    }
    dl->PopClipRect();
}

// THE CAPTION ON THE SPECTRUM (0.99.71). A tester listening to O'Hare, whose twenty-nine rows make the section taller than the rail, had to hold
// the rail scrolled to its foot to read which frequency was talking: the status lines were the last thing the section drew, under the keys. They
// now sit above the row list in the rail, and are lettered here as well, at the left under the spectrum's header - the row the trace-mode label
// uses (one row lower while PEAK HOLD or AVERAGE is showing, so the two never cross: stub campaign B11), over the channel marks drawAirbandMarkers has
// just drawn - so the message is on the picture the listener is watching, whatever the rail shows. The block's line in the rail's green, then the
// names in its amber, on ONE line: a long list of names is cut at the panel's edge rather than wrapped down over the trace. Drawn after the
// gridlines as the panel's topmost lettering.
//
// The census gets the caption's rectangle ("airband:caption") and a note ("airband:caption:hearing") when names were lettered, so the test can
// hold it to the top of the spectrum and know it named the open channel.
void AppWindow::drawAirbandCaption(float x0, float y0, float width, float yBottom) {
    std::string block, hearing;
    if (!airbandStatusText(block, hearing)) { return; }
    ImFont* font = cascade::gui::fonts::ui();
    const float px = ImGui::GetFontSize() * 0.8f;
    float y = y0 + 2.0f;
    if (spectrumTraceMode_ != cc::TraceMode::Normal) { y += px + 2.0f; }
    if (y + px > yBottom) { return; }
    const float x = x0 + 6.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x0 + width, yBottom), true);
    dl->AddText(font, px, ImVec2(x, y), cascade::gui::theme::kPhosphor, block.c_str());
    float xEnd = x + font->CalcTextSizeA(px, FLT_MAX, 0.0f, block.c_str()).x;
    if (!hearing.empty()) {
        const std::string said = cc::formatText(tr("Hearing: %s"), hearing.c_str());
        const float gap = px;
        dl->AddText(font, px, ImVec2(xEnd + gap, y), cascade::gui::theme::kAmber, said.c_str());
        xEnd += gap + font->CalcTextSizeA(px, FLT_MAX, 0.0f, said.c_str()).x;
        cascade::gui::census::note("airband:caption:hearing");
    }
    dl->PopClipRect();
    cascade::gui::census::rect("airband:caption", x, y, std::min(xEnd, x0 + width), y + px);
}

// --- the section -----------------------------------------------------------------------------------------------------------------------

void AppWindow::drawAirbandSection() {
    const AirbandFacts& facts = airbandFacts();
    const char* chip = facts.listening ? tr("LISTEN") : tr("IDLE");
    if (!railSection(trId("Airband###airband"), false, chip, cascade::gui::theme::kPhosphor, facts.listening)) { return; }
    uiNotePanel("airband");
    AirbandWindowState& a = airband_;

    // --- the airport -------------------------------------------------------
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float lookW = std::max(82.0f, cascade::gui::buttonWidth(trId("Look up")));
    const bool beside = cascade::gui::fitsBeside(90.0f, spacing, lookW, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(beside ? -(lookW + spacing) : -1.0f);
    const bool entered = cascade::gui::inputTextWithFittedHint(
        "##airband_code", tr("airport code - KORD, ORD, EGLL, LHR"), a.code, sizeof(a.code),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase);
    cascade::gui::census::rect("airband:code", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                               ImGui::GetItemRectMax().y);
    if (beside) { ImGui::SameLine(); }
    ImGui::BeginDisabled(a.code[0] == '\0');
    if ((ImGui::Button(trId("Look up"), ImVec2(lookW, 0.0f)) || entered) && a.code[0] != '\0') { airbandLookup(a.code); }
    cascade::gui::census::rect("airband:lookup", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                               ImGui::GetItemRectMax().y);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tr("The ICAO code, the three-letter code on a luggage tag, or in the US\n"
                                   "the FAA's own id. Adds the airport's frequencies to the frequency\n"
                                   "list as a group. US lists come from the FAA, the rest of the world's\n"
                                   "from OurAirports; both are built into FoxSDR, so this needs no\n"
                                   "internet connection."));
    }
    // NEAREST: from the receiver position the map and the range rings use.
    const bool rxSet = receiverPositionKnown();
    ImGui::BeginDisabled(!rxSet);
    if (ImGui::SmallButton(trId("Nearest airports"))) {
        a.nearest = cc::nearestAirports(cc::airbandTable(), uiState_.rxLatDeg, uiState_.rxLonDeg, 6);
        a.choices.clear();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", rxSet ? tr("The airports closest to the receiver position.")
                                      : tr("Set the receiver position first: Radar section,\n"
                                           "\"Receiver position\" - or \"Set RX here\" on a map."));
    }
    if (!a.nearest.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(trId("Hide##airband_near"))) { a.nearest.clear(); }
        const cc::Airport* pick = nullptr;
        for (std::size_t i = 0; i < a.nearest.size(); ++i) {
            const cc::Airport* ap = a.nearest[i].first;
            ImGui::PushID(static_cast<int>(i));
            char label[192];
            cc::formatUtf8(label, sizeof(label), "%s  %s  %.0f km", ap->ident.c_str(), ap->name.c_str(), a.nearest[i].second);
            if (ImGui::Selectable(label)) { pick = ap; }
            ImGui::PopID();
        }
        if (pick != nullptr) {
            a.nearest.clear();
            airbandAddAirport(*pick);
        }
    }
    if (!a.choices.empty()) {
        const cc::Airport* pick = nullptr;
        for (std::size_t i = 0; i < a.choices.size(); ++i) {
            const cc::Airport* ap = a.choices[i];
            ImGui::PushID(static_cast<int>(1000 + i));
            char label[192];
            cc::formatUtf8(label, sizeof(label), "%s  %s  (%s)", ap->ident.c_str(), ap->name.c_str(), ap->country.c_str());
            if (ImGui::Selectable(label)) { pick = ap; }
            ImGui::PopID();
        }
        if (pick != nullptr) { airbandAddAirport(*pick); }
    }

    // --- a PRESET, and a frequency of your own for it (0.99.66) ----------------
    // THE PRESET'S NAME first: the group of the frequency list that Add, Import, Export CSV and Remove preset all act on. The caption sits beside
    // the field when the rail holds both, above it when it does not (text_fit.hpp's rule), and the hover on either says what a preset is.
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
        cascade::gui::inputTextWithFittedHint("##airband_preset", tr("Preset"), a.presetName, sizeof(a.presetName));
        cascade::gui::census::rect("airband:preset", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", presetHelp); }
    }
    // The frequency in MHz beside the mode, then an optional name beside Add; each pair stacks when the rail is too narrow to hold it
    // (text_fit.hpp's rule). The mode is AM or NFM only: WFM is not something the monitor plays.
    {
        static const char* const kAddModes[] = {"AM", "NFM"};
        const ImGuiStyle& st = ImGui::GetStyle();
        const float modeW = std::max(64.0f, ImGui::CalcTextSize("NFM").x + st.FramePadding.x * 2.0f + ImGui::GetFrameHeight());
        const bool modeBeside = cascade::gui::fitsBeside(90.0f, spacing, modeW, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(modeBeside ? -(modeW + spacing) : -1.0f);
        cascade::gui::inputTextWithFittedHint("##airband_add_mhz", tr("Frequency (MHz)"), a.addMhz, sizeof(a.addMhz));
        cascade::gui::census::rect("airband:addfreq", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        if (modeBeside) { ImGui::SameLine(); }
        ImGui::SetNextItemWidth(modeBeside ? modeW : -1.0f);
        const bool modeOpen = ImGui::BeginCombo("##airband_add_mode", kAddModes[a.addMode == 1 ? 1 : 0]);
        cascade::gui::census::rect("airband:mode", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        if (modeOpen) {
            for (int m = 0; m < 2; ++m) {
                if (ImGui::Selectable(kAddModes[m], a.addMode == m)) { a.addMode = m; }
                cascade::gui::census::rect("airband:mode:", m, ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                                           ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y);
            }
            ImGui::EndCombo();
        }
        const float addW = std::max(60.0f, cascade::gui::buttonWidth(trId("Add##airband_add")));
        const bool addBeside = cascade::gui::fitsBeside(90.0f, spacing, addW, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(addBeside ? -(addW + spacing) : -1.0f);
        cascade::gui::inputTextWithFittedHint("##airband_add_name", tr("name"), a.addName, sizeof(a.addName));
        cascade::gui::census::rect("airband:addname", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        if (addBeside) { ImGui::SameLine(); }
        ImGui::BeginDisabled(a.addMhz[0] == '\0');
        if (ImGui::Button(trId("Add##airband_add"), ImVec2(addBeside ? addW : -FLT_MIN, 0.0f))) { airbandAddManual(); }
        cascade::gui::census::rect("airband:add", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tr("Adds the frequency to the preset named above, ticked. With no name\n"
                                       "it is named by its frequency.\n"
                                       "AM is for aircraft; NFM is narrow FM - marine, business and\n"
                                       "public-service radio."));
        }
    }
    if (!a.note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
        ImGui::TextWrapped("%s", a.note.c_str());
        ImGui::PopStyleColor();
    }

    // THE PRESET'S FILE KEYS (0.99.66): the path of a file to read into it and Import beside it, then Export CSV and Remove preset. The path is
    // typed or pasted, like the Bookmarks section's; Import pressed with the box empty opens the "Import a frequency list" window (0.99.80: a drop
    // zone and a Browse key, for this preset - batch B8), and a file dropped on the window with no such window open still goes in as the file says.
    AirbandJobs& jobs = airbandJobs();
    airbandRebuildView();
    {
        const float importW = std::max(82.0f, cascade::gui::buttonWidth(trId("Import##airband_import")));
        const bool importBeside = cascade::gui::fitsBeside(90.0f, spacing, importW, ImGui::GetContentRegionAvail().x);
        ImGui::SetNextItemWidth(importBeside ? -(importW + spacing) : -1.0f);
        cascade::gui::inputTextWithFittedHint("##airband_import_path", tr("SDR# frequencies.xml or .csv - its path"), a.importPath,
                                              sizeof(a.importPath));
        cascade::gui::census::rect("airband:importpath", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        if (importBeside) { ImGui::SameLine(); }
        // Disabled while a read is out (the Bookmarks section's rule): the file named is being read, and a second press would only be remembered.
        // An empty box does not disable it: pressed empty, it opens the Import window.
        ImGui::BeginDisabled(jobs.import.pending());
        if (ImGui::Button(trId("Import##airband_import"), ImVec2(importBeside ? importW : -FLT_MIN, 0.0f))) { airbandImportPreset(); }
        cascade::gui::census::rect("airband:import", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();

        // The preset's rows, whatever their mode or tick: what Export has to write and Remove has to take out.
        const std::size_t presetRows = a.view.presetRows;
        const float avail = ImGui::GetContentRegionAvail().x;
        const float half = (avail - spacing) * 0.5f;
        const bool keysBeside = cascade::gui::buttonWidth(trId("Export CSV##airband_export")) <= half &&
                                cascade::gui::buttonWidth(trId("Remove preset##airband_remove")) <= half;
        const ImVec2 keySize(keysBeside ? half : -FLT_MIN, 0.0f);
        ImGui::BeginDisabled(presetRows == 0 || jobs.exportCsv.pending());
        if (ImGui::Button(trId("Export CSV##airband_export"), keySize)) { airbandExportPreset(); }
        cascade::gui::census::rect("airband:export", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();
        if (keysBeside) { ImGui::SameLine(); }
        ImGui::BeginDisabled(presetRows == 0);
        if (ImGui::Button(trId("Remove preset##airband_remove"), keySize)) { airbandRemovePreset(); }
        cascade::gui::census::rect("airband:remove", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        ImGui::EndDisabled();
        if (!a.presetNote.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
            ImGui::TextWrapped("%s", a.presetNote.c_str());
            ImGui::PopStyleColor();
        }
        // A read that is taking long says so, in the Bookmarks section's sentence.
        if (jobs.import.pending() && jobs.import.elapsedS() >= 1.0) {
            ImGui::TextDisabled(tr("Waiting for the disk to open the file: %.0f s"), jobs.import.elapsedS());
        }
        // A file that was written: the key that shows where. The folder is the recordings folder, which the export has made by now.
        if (a.presetFolderKey) {
            if (ImGui::SmallButton(trId("Open the folder##airband_folder"))) {
                std::string dir;
                readFactText("fact.recorder.dir", dir);
                shellOpen(dir);
            }
            cascade::gui::census::rect("airband:openfolder", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                       ImGui::GetItemRectMax().y);
        }
    }

    // --- which rows: one group (an airport's, or "Manual"), or every ticked AM or NFM row ----------------------------------------------
    // A choice made while the monitor listens is the monitor starting again on it (it plays the LISTEN's preset; the group on show is only what
    // the next LISTEN, or this choice, sends).
    const auto chooseGroup = [&](const std::string& g) {
        const bool changed = a.group != g;
        a.group = g;
        if (changed && (facts.listening || facts.pending)) { airbandListen(); }
    };
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##airband_group", a.group.empty() ? tr("Every ticked AM or NFM frequency") : a.group.c_str(),
                          ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(trId("Every ticked AM or NFM frequency"), a.group.empty())) { chooseGroup(std::string()); }
        for (std::size_t g = 0; g < a.view.groups.size(); ++g) {
            ImGui::PushID(static_cast<int>(g));
            if (ImGui::Selectable(a.view.groups[g].c_str(), a.group == a.view.groups[g])) {
                const std::string chosen = a.view.groups[g];
                chooseGroup(chosen);
                // THE GROUP SHOWN IS THE PRESET ACTED ON (0.99.66): Add, Export CSV and Remove preset take the name from the field above, so
                // choosing a group here puts its name there - a group chosen and a preset named that were two different things would export the
                // wrong list. The name goes in TRIMMED, as the keys read it (a group stored with spaces round it would otherwise leave them
                // dead), and one that still does not fit is not copied cut short - or left as the OLD name, which Export CSV and Remove preset
                // would then act on, unconfirmed: the field is cleared.
                const std::string field = cascade::gui::presetFieldText(chosen, sizeof(a.presetName));
                std::snprintf(a.presetName, sizeof(a.presetName), "%s", field.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // (A choice rebuilds the rows; so does the group that was removed from the list.)
    airbandRebuildView();

    const std::vector<cascade::gui::BookmarkRow>& list = tuningLists().markers;
    airbandReadLive();
    // What has been heard but not yet written to the list, so the column moves while the monitor plays rather than every twenty seconds.
    const auto pendingFor = [&](const cascade::gui::BookmarkRow& b) {
        for (std::size_t i = 0; i < a.liveCount; ++i) {
            if (a.live[i].freqHz == b.hz && a.live[i].name == b.name) { return a.live[i].pendingHeardS; }
        }
        return 0.0;
    };
    const auto chanFor = [&](const cascade::gui::BookmarkRow& b) -> const AirbandChannel* {
        if (!facts.listening) { return nullptr; }
        for (std::size_t i = 0; i < a.liveCount; ++i) {
            if (a.live[i].freqHz == b.hz) { return &a.live[i]; }
        }
        return nullptr;
    };
    std::vector<std::uint32_t> sortedRows;
    const std::vector<std::uint32_t>* rowsOf = &a.view.rows;
    if (a.busyFirst) {
        sortedRows = a.view.rows;
        std::stable_sort(sortedRows.begin(), sortedRows.end(), [&](std::uint32_t x, std::uint32_t y) {
            return list[x].heardS + pendingFor(list[x]) > list[y].heardS + pendingFor(list[y]);
        });
        rowsOf = &sortedRows;
    }
    const std::vector<std::uint32_t>& rows = *rowsOf;

    std::size_t tickedCount = 0;
    for (const std::uint32_t i : rows) { tickedCount += list[i].ticked ? 1u : 0u; }
    cascade::gui::census::note("airband:rows:", static_cast<int>(rows.size()));
    cascade::gui::census::note("airband:ticked:", static_cast<int>(tickedCount));
    ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kInkMuted));
    ImGui::Text(tr("%zu ticked of %zu"), tickedCount, rows.size());
    ImGui::PopStyleColor();
    int setAll = -1;
    if (!rows.empty() && !a.group.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(trId("Tick all"))) { setAll = 1; }
        ImGui::SameLine();
        if (ImGui::SmallButton(trId("Untick all"))) { setAll = 0; }
    }
    ImGui::Checkbox(trId("Busiest first"), &a.busyFirst);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("Sorts by how long each frequency has been heard talking while\n"
                                   "FoxSDR listened - measured here, at your aerial, and kept."));
    }

    // WHAT IS BEING HEARD, ABOVE THE LIST (0.99.71): the block on the air and the names talking in it were the last lines of the section, under
    // the keys - and a tester listening to O'Hare, whose twenty-nine rows make the section taller than the rail, had to hold the rail scrolled to
    // its foot to read them. They sit here now, between the list's heading and its rows, and the spectrum carries them too (drawAirbandCaption).
    // ALWAYS TWO LINES TALL: "Hearing: -" while every squelch is shut, and the same two lines left empty while the monitor is idle - so a talker
    // starting does not push the rows and the keys down a line under the hand about to press them, and STOP is drawn exactly where LISTEN was (the
    // stop scenario of tests/test_airband_app presses the same spot; the first build of this change had moved the key two lines down and the press
    // missed it).
    {
        std::string block, hearing;
        const float slotH = 2.0f * ImGui::GetTextLineHeightWithSpacing();
        if (facts.pending) {
            ImGui::TextWrapped("%s", tr("Handing the radio back from the patch view..."));
            ImGui::Dummy(ImVec2(0.0f, slotH - ImGui::GetTextLineHeightWithSpacing()));
        } else if (!airbandStatusText(block, hearing)) {
            ImGui::Dummy(ImVec2(0.0f, slotH));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kPhosphor));
            ImGui::TextWrapped("%s", block.c_str());
            ImGui::PopStyleColor();
            cascade::gui::census::rect("airband:status", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                       ImGui::GetItemRectMax().y);
            const bool talking = !hearing.empty();
            ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(talking ? cascade::gui::theme::kAmber : cascade::gui::theme::kInkMuted));
            ImGui::TextWrapped(tr("Hearing: %s"), talking ? hearing.c_str() : "-");
            ImGui::PopStyleColor();
            cascade::gui::census::rect("airband:hearing", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                       ImGui::GetItemRectMax().y);
        }
    }

    // Rows: tick, live lamp, frequency and name (click to tune the receiver to that one alone), time heard.
    std::int64_t toggleId = -1;
    bool toggleTo = false;
    std::int64_t tuneId = -1;
    const float rowsH = std::min(14.0f, static_cast<float>(rows.size())) * ImGui::GetFrameHeightWithSpacing() + 4.0f;
    if (!rows.empty() && ImGui::BeginChild("##airband_rows", ImVec2(-1.0f, rowsH), ImGuiChildFlags_None)) {
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(rows.size()));
        const float heardW = ImGui::CalcTextSize("00h 00m").x;
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const std::uint32_t i = rows[static_cast<std::size_t>(r)];
                if (i >= list.size()) { continue; }
                const cascade::gui::BookmarkRow& b = list[i];
                ImGui::PushID(static_cast<int>(i));
                bool t = b.ticked;
                if (ImGui::Checkbox("##tick", &t)) {
                    toggleId = static_cast<std::int64_t>(b.id);
                    toggleTo = t;
                }
                ImGui::SameLine();
                // THE LAMP: lit while this channel's squelch is open - from the LIVE list read this frame, never from a copy that waited for
                // listSeq.
                const AirbandChannel* live = chanFor(b);
                const bool lit = live != nullptr && live->open;
                if (lit) { cascade::gui::census::note("airband:open:", static_cast<int>(std::lround(b.hz / 1000.0))); }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float d = ImGui::GetFrameHeight();
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(p.x + d * 0.35f, p.y + d * 0.5f), d * 0.22f,
                    lit ? static_cast<ImU32>(cascade::gui::theme::kPhosphor) : cascade::gui::theme::withAlpha(cascade::gui::theme::kInkFaint, 0.5f));
                ImGui::Dummy(ImVec2(d * 0.7f, d));
                ImGui::SameLine();
                char label[256];
                cc::formatUtf8(label, sizeof(label), "%.4f  %s", b.hz / 1.0e6, b.name.c_str());
                const float nameW = ImGui::GetContentRegionAvail().x - heardW - spacing;
                if (ImGui::Selectable(label, lit, ImGuiSelectableFlags_None, ImVec2(std::max(40.0f, nameW), 0.0f))) {
                    tuneId = static_cast<std::int64_t>(b.id);
                }
                if (ImGui::IsItemHovered() && live != nullptr) {
                    ImGui::SetTooltip(tr("Level %.0f dB (squelch %.0f dB)"), static_cast<double>(live->levelDb),
                                      static_cast<double>(facts.squelchDb));
                }
                ImGui::SameLine();
                const std::string h = heardText(b.heardS + pendingFor(b));
                ImGui::PushStyleColor(ImGuiCol_Text, cascade::gui::theme::vec(cascade::gui::theme::kAmber));
                ImGui::TextUnformatted(h.c_str());
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
        }
    }
    if (!rows.empty()) {
        ImGui::EndChild();
        // Where the list is, so a test can hold the status lines above it.
        cascade::gui::census::rect("airband:rowlist", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
    }
    // Commands by id, landing at the engine's next pass - never while the clipper above still walks the list.
    if (setAll >= 0) {
        for (const std::uint32_t i : rows) {
            if (i < list.size() && list[i].ticked != (setAll == 1)) {
                uiSubmit(cmd::makeInt(FOXAPP_OP_BOOKMARK_TICK, static_cast<std::int64_t>(list[i].id), setAll));
            }
        }
    }
    if (toggleId >= 0) { uiSubmit(cmd::makeInt(FOXAPP_OP_BOOKMARK_TICK, toggleId, toggleTo ? 1 : 0)); }
    if (tuneId >= 0) {
        // One channel, on the receiver: the monitor stops ("Tuned to <name> alone."), because the receiver is now somewhere the monitor did not put
        // it.
        uiSubmit(cmd::makeInt(FOXAPP_OP_AIRBAND, 6, tuneId));
    }

    // --- listening ------------------------------------------------------------
    // The squelch the monitor judges by: the engine's figure, or the one under the hand until the engine's has caught up. A new threshold
    // rebuilds the block's set (the runner's live squelch slots belong to the patch page), so it is sent on release, not every frame of a drag:
    // the channels are not restarted sixty times a second.
    ImGui::SetNextItemWidth(-1.0f);
    float squelch = a.squelchHeld ? a.squelchEdit : facts.squelchDb;
    if (ImGui::SliderFloat("##airband_sq", &squelch, -110.0f, 0.0f, tr("squelch %.0f dB"))) {
        a.squelchEdit = squelch;
        a.squelchHeld = true;
    }
    cascade::gui::census::rect("airband:squelch", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                               ImGui::GetItemRectMax().y);
    // (A hand on this slider turns the top bar's AUTO key off, one switch for the receiver's squelch and this one: the engine does it, for this op - stub campaign B12.)
    if (ImGui::IsItemDeactivatedAfterEdit()) { uiSubmit(cmd::makeNumInt(FOXAPP_OP_AIRBAND, static_cast<double>(a.squelchEdit), 4)); }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("A channel is heard while its level is above this. Hover a row\n"
                                   "while listening to see its level; \"Above the noise\" sets it\n"
                                   "for you from the quiet channels."));
    }
    ImGui::BeginDisabled(!facts.listening);
    const bool aboveNoisePressed = ImGui::SmallButton(trId("Above the noise"));
    cascade::gui::census::rect("airband:abovenoise", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                               ImGui::GetItemRectMax().y);
    // The rule is written once, in the engine (8 dB over the median channel level; nothing when no channel has reported yet).
    if (aboveNoisePressed) { uiSubmit(cmd::makeInt(FOXAPP_OP_AIRBAND, 3)); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x));
    double hold = a.holdHeld ? a.holdEdit : facts.holdS;
    if (ImGui::InputDouble("##airband_hold", &hold, 0.0, 0.0, tr("hold %.1f s"))) {
        a.holdEdit = std::clamp(hold, 0.0, 60.0);
        a.holdHeld = true;
    }
    // A hold that is changed while several blocks are being scanned starts the scan again, in the engine.
    if (ImGui::IsItemDeactivatedAfterEdit()) { uiSubmit(cmd::makeNumInt(FOXAPP_OP_AIRBAND, a.holdEdit, 5)); }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tr("How long a block stays after its last word before the scan moves\n"
                                   "on. Only used when the ticked frequencies need more than one block."));
    }

    if (!facts.listening && !facts.pending) {
        if (ImGui::Button(trId("LISTEN"), ImVec2(-FLT_MIN, 0.0f))) { airbandListen(); }
        cascade::gui::census::rect("airband:listen", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", tr("Plays every ticked AM or NFM frequency the radio's band can hold at\n"
                                       "once, mixed; when they do not all fit, scans between blocks of\n"
                                       "them and stops wherever someone is talking. Uses the receiver's\n"
                                       "radio, so it switches to the RECEIVER view."));
        }
    } else {
        const bool scanning = facts.blocks > 1;
        const float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        if (ImGui::Button(trId("STOP##airband"), ImVec2(scanning ? half : -FLT_MIN, 0.0f))) { uiSubmit(cmd::makeInt(FOXAPP_OP_AIRBAND, 0)); }
        cascade::gui::census::rect("airband:stop", ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y, ImGui::GetItemRectMax().x,
                                   ImGui::GetItemRectMax().y);
        if (scanning) {
            ImGui::SameLine();
            if (ImGui::Button(trId("Next block"), ImVec2(-FLT_MIN, 0.0f))) { uiSubmit(cmd::makeInt(FOXAPP_OP_AIRBAND, 2)); }
        }
    }
    // The block on the air and who is talking in it are said above the row list (0.99.71), and on the spectrum - not here, under the keys, where
    // a long list put them below the fold.
}

}  // namespace cascade::gui
