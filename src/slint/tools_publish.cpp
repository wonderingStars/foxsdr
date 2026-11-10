// tools_publish.cpp - the TOOLS bank's publisher (tools_publish.hpp): the host's models (tools_logic.hpp) as the window's generated structs, the window's
// gestures as the Effects tools_logic.hpp names for them, and the work the Effects hand to the publisher (the disk workers, the answers the airband section
// waits for, the paced Tick all). SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "tools_publish.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>

#include "core/diag_log.hpp"
#include "core/disk_job.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/plugin_ui_data.hpp"
#include "core/utf8_text.hpp"
#include "gui/section_reads.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <windows.h>
#include <shellapi.h>
// clang-format on
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#endif

namespace foxsdr::slinttools {

namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;
using cascade::i18n::tr;

namespace {

constexpr int kTickRowsPerFrame = 32;      // Tick all / Untick all: FOXAPP_OP_BOOKMARK_TICK rows a frame (app_window.hpp kTickRowsPerFrame)
constexpr int kTickSettleFrames = 3;       // frames after the last row went out that a press still covers every row (AirbandTickRun::settle)
constexpr std::int64_t kSquelchEveryMs = 100;   // a drag of the squelch slider reaches the engine ten times a second at most, the last position always
constexpr double kRenameWantS = 4.0;       // a group just renamed: the filter follows it for this long, then gives up

slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }

::ChipTone tone(Tone t) {
    switch (t) {
        case Tone::Good: return ::ChipTone::Good;
        case Tone::Warn: return ::ChipTone::Warn;
        case Tone::Err: return ::ChipTone::Err;
        default: return ::ChipTone::Neutral;
    }
}

std::vector<::SelectItem> items(const std::vector<SelectItem>& v) {
    std::vector<::SelectItem> out;
    out.reserve(v.size());
    for (const SelectItem& s : v) {
        ::SelectItem o{};
        o.label = ss(s.label);
        o.id = ss(s.id);
        o.enabled = s.enabled;
        o.note = ss(s.note);
        out.push_back(std::move(o));
    }
    return out;
}

// A command of the publisher's own (a Tick all row, a chunk of a preset import) as the logic's Command.
Command commandOf(const FoxCommand& f) {
    Command c;
    c.q.c = f;
    c.grant = foxsdr::slintsignal::grantFor(f.op);
    c.valid = true;
    return c;
}
Command commandOf(const cmd::QueuedCommand& q) {
    Command c;
    c.q = q;
    c.grant = foxsdr::slintsignal::grantFor(q.c.op);
    c.valid = true;
    return c;
}

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) { ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n); }
    return w;
}
#endif

// A number in a scanner field's text, for the comparison of a draft with the engine's figure.
bool sameNumber(const std::string& a, const std::string& b) {
    char* ea = nullptr;
    char* eb = nullptr;
    const double x = std::strtod(a.c_str(), &ea);
    const double y = std::strtod(b.c_str(), &eb);
    if (ea == a.c_str() || eb == b.c_str()) { return a == b; }
    return std::fabs(x - y) <= 1e-9 * std::max(1.0, std::fabs(x));
}

// The scalars the aircraft heard give: scopeTrafficCentre (gui/scope_view.hpp, a header that includes the drawing helpers - its rule is written out here, and
// tests/test_slint_host_tools.cpp holds the minimum of three to that header's text): aircraft with a real position, a plain mean of degrees.
int trafficCentre(const std::vector<cc::HostTrack>& tracks, double& lat, double& lon) {
    int n = 0;
    double sumLat = 0.0, sumLon = 0.0;
    for (const cc::HostTrack& ht : tracks) {
        if (ht.t.kind != CASCADE_TRACK_AIRCRAFT) { continue; }
        if (!std::isfinite(ht.t.latDeg) || !std::isfinite(ht.t.lonDeg)) { continue; }
        if (ht.t.latDeg < -90.0 || ht.t.latDeg > 90.0 || ht.t.lonDeg < -180.0 || ht.t.lonDeg > 180.0) { continue; }
        sumLat += ht.t.latDeg;
        sumLon += ht.t.lonDeg;
        ++n;
    }
    if (n > 0) {
        lat = sumLat / static_cast<double>(n);
        lon = sumLon / static_cast<double>(n);
    }
    return n;
}

// A list model kept between publishes and updated row by row.
template <class T>
struct Synced {
    std::shared_ptr<slint::VectorModel<T>> model = std::make_shared<slint::VectorModel<T>>();
    std::shared_ptr<slint::Model<T>> set(const std::vector<T>& rows) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i < model->row_count()) {
                if (!(*model->row_data(i) == rows[i])) { model->set_row_data(i, rows[i]); }
            } else {
                model->push_back(rows[i]);
            }
        }
        while (model->row_count() > rows.size()) { model->erase(model->row_count() - 1u); }
        return model;
    }
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
struct ToolsPublisher::Lists {
    Synced<::SelectItem> groups, filterGroups, presets, modes, airGroups, gpsPorts, gpsBauds;
    Synced<::BookmarkRow> bmRows;
    Synced<::AirportRow> airports, ambiguous;
    Synced<::AirbandRow> airRows;
    Synced<::MarkerRow> markerRows;
};

struct ToolsPublisher::Last {
    std::optional<::RadarModel> radar;
    std::optional<::BookmarksModel> bookmarks;
    std::optional<::ScannerModel> scanner;
    std::optional<::AirbandModel> airband;
    std::optional<::MarkersModel> markers;
};

// What the two disk workers hand back.
struct BookmarkLookup { std::string tried; };
struct PresetOutcome {
    std::string path, error, format;
    std::size_t found = 0, skipped = 0, shifted = 0, playable = 0;
    std::vector<std::string> chunks;
    double ms = 0.0;
};

struct ToolsPublisher::Jobs {
    cc::DiskJob<BookmarkLookup> lookup{"Bookmark import file lookup"};
    std::string lookupPath;       // what the lookup in flight was asked to find
    cc::DiskJob<PresetOutcome> preset{"Airband preset import"};
    std::string presetPath, presetName;
};

// An answer the airband section waits for (airbandSend: the ticket kept with what the answer is about).
struct ToolsPublisher::Pending {
    struct Ask {
        AskKind kind = AskKind::None;
        std::uint64_t ticket = 0;
        std::string group;
        std::size_t total = 0, ticked = 0;
    };
    std::vector<Ask> asks;
    // The Import into a preset that is out: what the file held, and the rows still to send (a long list goes in chunks).
    struct ImportRun {
        bool active = false;
        std::string group;
        std::size_t found = 0, playable = 0, skipped = 0, shifted = 0, added = 0, chunksOut = 0;
        std::vector<std::string> queue;
        std::size_t next = 0;
        bool refused = false;
    } run;
    // Tick all / Untick all: the ids still to set, paced kTickRowsPerFrame a frame; a press replaces it.
    struct TickRun {
        bool on = false;
        std::vector<std::uint64_t> ids;
        std::size_t next = 0;
        int settle = 0;
        bool active() const { return next < ids.size(); }
        bool unsettled() const { return active() || settle > 0; }
    } ticks;
};

ToolsPublisher::ToolsPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, const FoxEngineApi* api, FoxSession* session, bool bounded, Hooks hooks)
    : ui_(ui), kit_(kit), api_(api), session_(session), bounded_(bounded), hooks_(std::move(hooks)), table_(api, session), lists_(std::make_unique<Lists>()),
      jobs_(std::make_unique<Jobs>()), pending_(std::make_unique<Pending>()), lastSet_(std::make_unique<Last>()) {
    opts_.cache = &cache_;
    // the saved spectrum options (docs 6.52): the boxes open as the last session left them, as the classic window's do
    opts_.on_spectrum = hooks_.onSpectrum;
    opts_.stack_names = hooks_.stackNames;
}

ToolsPublisher::~ToolsPublisher() = default;

double ToolsPublisher::nowS() const { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string ToolsPublisher::fact(const std::string& key) {
    std::string out;
    if (!table_.fact(key, out)) { out.clear(); }
    return out;
}

void ToolsPublisher::say(const char* line) const { std::fprintf(stderr, "foxsdr-slint: tools: %s\n", line); }

// One command through the kit's submit (a long text goes by its long-text channel); the ticket, 0 when the session would not take it.
std::uint64_t ToolsPublisher::send(const Command& c) {
    forceRead_ = true;
    if (!c.valid) { return 0u; }
    const std::uint64_t ticket = kit_.submit(c.q);
    if (ticket != 0u) { ++sent_; }
    return ticket;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the shell and the clipboard. A bounded run touches neither.

void ToolsPublisher::openFolder(const std::string& path) {
    if (path.empty()) { return; }
    if (bounded_) {
        cc::diagLogf("tools: bounded run - not opening a folder");
        return;
    }
    const std::filesystem::path dir{std::u8string(path.begin(), path.end())};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#if defined(_WIN32)
    ::ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void ToolsPublisher::setClipboard(const std::string& text) {
    if (bounded_) {
        cc::diagLogf("tools: bounded run - not writing the clipboard (%zu bytes)", text.size());
        std::fprintf(stderr, "foxsdr-slint: tools: markers not copied (bounded run), %zu bytes\n", text.size());
        return;
    }
#if defined(_WIN32)
    const std::wstring w = widen(text);
    const std::size_t bytes = (w.size() + 1u) * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem == nullptr) { return; }
    void* p = ::GlobalLock(mem);
    if (p == nullptr) {
        ::GlobalFree(mem);
        return;
    }
    std::memcpy(p, w.c_str(), bytes);
    ::GlobalUnlock(mem);
    if (!::OpenClipboard(nullptr)) {
        ::GlobalFree(mem);
        say("the clipboard could not be opened; nothing copied");
        return;
    }
    ::EmptyClipboard();
    if (::SetClipboardData(CF_UNICODETEXT, mem) == nullptr) {
        ::GlobalFree(mem);   // the clipboard did not take it: still ours
    }
    ::CloseClipboard();
#else
    (void)text;
    say("the clipboard is Windows-only here; nothing copied");
#endif
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the disk workers (app_window_airband.cpp airbandImportFile / airbandPollJobs; app_window_import_dialog.cpp importBookmarkFile)

// A bookmark import of a path with no root: looked for where an import box looks (the working directory, then the recordings folder) on a worker, one at a
// time, and the last one asked for while one is out is sent when it is back. The result goes to the engine (FOXAPP_OP_BOOKMARK_IMPORT_FILE) with the full path.
void ToolsPublisher::startBookmarkLookup(const std::string& path) {
    importNote_.clear();
    Jobs& j = *jobs_;
    if (j.lookup.pending()) {
        againPath_ = path;
        again_ = true;
        return;
    }
    const std::string dir = fact("fact.recorder.dir");
    j.lookupPath = path;
    j.lookup.request([path, dir]() -> BookmarkLookup {
        BookmarkLookup o;
        (void)cc::importFrequencyFileIn(path, dir, o.tried);
        return o;
    });
}

// THE PRESET'S IMPORT: the file read and parsed on a worker, the preset's name and tick put on every row, the rows cut into chunks (the AM and NFM ones ticked,
// whatever the file said). One read at a time; a press made while one is out is remembered (the last one, with the preset it was pressed for).
void ToolsPublisher::importIntoPreset(const std::string& path, const std::string& preset) { exec(applyAirbandImportInto(path, preset), "airband-import-dialog"); }

void ToolsPublisher::startPresetImport(const std::string& path, const std::string& preset) {
    Jobs& j = *jobs_;
    if (j.preset.pending()) {
        againPresetPath_ = path;
        againPresetName_ = preset;
        againPreset_ = true;
        return;
    }
    j.presetPath = path;
    j.presetName = preset;
    const std::string dir = fact("fact.recorder.dir");
    j.preset.request([path, preset, dir]() -> PresetOutcome {
        const auto t0 = std::chrono::steady_clock::now();
        PresetOutcome o;
        o.path = path;
        cc::ImportResult r = cc::importFrequencyFileIn(path, dir, o.path);
        o.format = r.format;
        if (!r.error.empty() && r.items.empty()) {
            o.error = r.error;
            return o;
        }
        o.found = r.items.size();
        o.skipped = r.skipped;
        o.shifted = r.shifted;
        cc::applyImportInto(r.items, cc::ImportInto{preset, true});
        for (const cc::Bookmark& b : r.items) { o.playable += (b.mode == "AM" || b.mode == "NFM") ? 1u : 0u; }
        o.chunks = bookmarkRowChunks(r.items);
        o.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return o;
    });
}

void ToolsPublisher::pollJobs() {
    Jobs& j = *jobs_;
    {
        BookmarkLookup o;
        if (j.lookup.poll(o)) {
            if (j.lookup.threw() || o.tried.empty()) {
                const std::string reason = cc::formatText(tr("the file \"%s\" could not be read"), j.lookupPath.c_str());
                importNote_ = cc::formatText(tr("Could not import: %s"), reason.c_str());
                forceRead_ = true;
            } else {
                send(foxsdr::slintsignal::opImportPath(o.tried, 0));
            }
            if (again_) {
                again_ = false;
                std::string next;
                next.swap(againPath_);
                startBookmarkLookup(next);
            }
        }
    }
    {
        PresetOutcome o;
        if (j.preset.poll(o)) {
            forceRead_ = true;
            Pending::ImportRun& run = pending_->run;
            run = Pending::ImportRun{};
            if (j.preset.threw() && o.error.empty()) { o.error = "cannot read the file"; }
            const std::string& p = o.path.empty() ? j.presetPath : o.path;
            if (!o.error.empty()) {
                // THE REASON IN THE USER'S LANGUAGE: the core's three reasons are English sentences.
                std::string reason = o.error;
                if (o.error.rfind("cannot open", 0) == 0) {
                    reason = cc::formatText(tr("the file \"%s\" could not be opened"), p.c_str());
                } else if (o.error.rfind("cannot read", 0) == 0) {
                    reason = cc::formatText(tr("the file \"%s\" could not be read"), p.c_str());
                } else if (o.error.rfind("no line had a usable frequency", 0) == 0) {
                    reason = tr("no line of the file had a usable frequency");
                }
                opts_.air_preset_note = cc::formatText(tr("Could not import: %s"), reason.c_str());
                // The file's KIND, never its name or path (PRIVACY.md).
                const auto extension = std::filesystem::path(std::u8string(p.begin(), p.end())).extension().u8string();
                const std::string kind(extension.begin(), extension.end());
                cc::diagWarnf("bookmarks: an import (%s) failed", kind.c_str());
                if (hooks_.openImportDialog) { hooks_.openImportDialog(std::string()); }   // "Import a frequency list" opens on a read with no rows
            } else {
                run.active = true;
                run.group = j.presetName;
                run.found = o.found;
                run.playable = o.playable;
                run.skipped = o.skipped;
                run.shifted = o.shifted;
                run.queue = std::move(o.chunks);
                cc::diagLogf("bookmarks: read a %s file for a preset - %zu read, %.0f ms", o.format.c_str(), o.found, o.ms);
                if (run.queue.empty()) { finishPresetImport(); }
            }
            if (againPreset_) {
                againPreset_ = false;
                std::string next, into;
                next.swap(againPresetPath_);
                into.swap(againPresetName_);
                startPresetImport(next, into);
            }
        }
    }
}

// The rows of a read go to the engine a chunk a call, in order, until every one is taken (a session that is full is not an answer: the chunk is offered again).
void ToolsPublisher::sendChunks() {
    Pending::ImportRun& run = pending_->run;
    if (!run.active) { return; }
    while (run.next < run.queue.size()) {
        const std::uint64_t ticket = send(commandOf(cmd::makeText(FOXAPP_OP_BOOKMARK_ADD_ROWS, run.queue[run.next], 1)));
        if (ticket == 0u) { return; }
        Pending::Ask a;
        a.kind = AskKind::ImportRows;
        a.ticket = ticket;
        a.group = run.group;
        pending_->asks.push_back(std::move(a));
        ++run.chunksOut;
        ++run.next;
    }
}

// The run's next rows, once a frame: at most kTickRowsPerFrame, in order, the first refusal ends the frame's share (the row stays next).
void ToolsPublisher::sendTicks() {
    Pending::TickRun& run = pending_->ticks;
    if (run.ids.empty()) {
        if (run.settle > 0) { --run.settle; }
        return;
    }
    int sent = 0;
    while (run.active() && sent < kTickRowsPerFrame) {
        if (send(commandOf(cmd::makeInt(FOXAPP_OP_BOOKMARK_TICK, static_cast<std::int64_t>(run.ids[run.next]), run.on ? 1 : 0))) == 0u) { return; }
        ++run.next;
        ++sent;
    }
    if (!run.active()) {
        run.ids.clear();
        run.next = 0;
        run.settle = kTickSettleFrames;
    }
}

// An import whose every chunk has been answered: the note, the group on show.
void ToolsPublisher::finishPresetImport() {
    Pending::ImportRun& run = pending_->run;
    std::string said = cc::formatText(tr("Preset \"%s\": %zu read, %zu new; the monitor can play %zu of the rows read (AM or NFM)."), run.group.c_str(), run.found,
                                      run.added, run.playable);
    if (run.skipped > 0) {
        said += " ";
        said += tr("Some rows of the file had no usable frequency.");
    }
    if (run.shifted > 0) {
        said += " ";
        said += tr("Converter Shift values were not applied.");
    }
    if (run.refused) { said = opts_.air_note; }
    opts_.air_preset_note = said;
    opts_.air_folder_key = false;
    if (!run.refused) { airbandShowGroup(run.group); }
    cc::diagLogf("bookmarks: a preset import added %zu of %zu rows", run.added, run.found);
    run = Pending::ImportRun{};
    forceRead_ = true;
}

// SHOW A GROUP THE SECTION HAS JUST ADDED TO, but only when that cannot change what is being played: a monitor that is listening (or about to) keeps its choice,
// and a group the monitor has nothing to play in is not put on show (the combo would name a group the list may not even have).
void ToolsPublisher::airbandShowGroup(const std::string& group) {
    if (fact("fact.airband.listening") == "1" || fact("fact.airband.pending") == "1") { return; }
    refreshBookmarks(table_, cache_.bookmarks);
    for (const cascade::gui::BookmarkRow& b : cache_.bookmarks.rows) {
        if (b.group == group && airbandPlays(b.mode)) {
            opts_.air_group = group;
            return;
        }
    }
}

// An answer polled by main.cpp: the ones the airband section waits for (airbandOnAnswer).
void ToolsPublisher::onAnswer(const FoxCommandResult& r) {
    auto& asks = pending_->asks;
    const auto it = std::find_if(asks.begin(), asks.end(), [&](const Pending::Ask& k) { return k.ticket == r.ticket; });
    if (it == asks.end()) { return; }
    const Pending::Ask ask = *it;
    asks.erase(it);
    forceRead_ = true;
    const bool ok = r.status == FOXAPI_OK;
    switch (ask.kind) {
        case AskKind::TypedRow:
            if (ok) {
                opts_.air_freq.clear();
                opts_.air_name.clear();
                airbandShowGroup(ask.group);   // show the list it joined, as an airport's lookup does - unless listening
                opts_.air_note.clear();
            } else {
                opts_.air_note = r.message;   // the engine's sentence: a frequency outside what the open radio covers says what it does cover
            }
            break;
        case AskKind::AirportRows: {
            if (!ok) {
                opts_.air_note = r.message;
                break;
            }
            const std::size_t added = static_cast<std::size_t>(r.applied[0]);
            airbandShowGroup(ask.group);
            if (added == 0) {
                opts_.air_note = cc::formatText(tr("%s is already in the frequency list (%zu frequencies)."), ask.group.c_str(), ask.total);
            } else {
                opts_.air_note = cc::formatText(
                    tr("%s: %zu frequencies added to the frequency list, %zu ticked. ATIS and weather "
                       "broadcasts are left unticked - they never stop talking."),
                    ask.group.c_str(), added, ask.ticked);
            }
            // A monitor playing ANOTHER preset does not play these rows: say what it is still playing and where the choice is.
            const std::string playing = fact("fact.airband.preset");
            if (fact("fact.airband.listening") == "1" && !playing.empty() && playing != ask.group) {
                opts_.air_note += " " + cc::formatText(tr("The monitor keeps playing %s; choose %s in the group list to "
                                                          "hear it."),
                                                       playing.c_str(), ask.group.c_str());
            }
            cc::diagLogf("airband: an airport's list added (%zu frequencies, %zu new)", ask.total, added);
            break;
        }
        case AskKind::ImportRows: {
            Pending::ImportRun& run = pending_->run;
            if (run.chunksOut > 0) { --run.chunksOut; }
            if (ok) {
                run.added += static_cast<std::size_t>(r.applied[0]);
            } else {
                run.refused = true;
                opts_.air_note = r.message;
            }
            if (run.active && run.chunksOut == 0 && run.next >= run.queue.size()) { finishPresetImport(); }
            break;
        }
        case AskKind::RemoveGroup:
            // A preset with no rows is NOT_FOUND to the table (a group nothing is in changes nothing) and "Removed 0" to this section.
            if (ok || r.status == FOXAPI_NOT_FOUND) {
                const std::size_t n = ok ? static_cast<std::size_t>(r.applied[0]) : 0u;
                opts_.air_preset_note = cc::formatText(tr("Removed %zu from \"%s\""), n, ask.group.c_str());
                if (opts_.air_group == ask.group) { opts_.air_group.clear(); }
                cc::diagLogf("airband: a preset was removed (%zu rows)", n);
            }
            break;
        case AskKind::ExportPreset:
            if (ok) {
                opts_.air_export_waiting = true;   // the engine has taken it: the worker is writing; the result is a fact (pollExport)
                opts_.air_preset_note = tr("Saving...");
            } else if (r.status == FOXAPI_NOT_FOUND) {
                opts_.air_preset_note = cc::formatText(tr("Nothing to export: the preset \"%s\" has no rows."), ask.group.c_str());
            } else {
                opts_.air_preset_note = r.message;
            }
            break;
        case AskKind::Listen:
            // A LISTEN the engine would not start says why, in the language in force when it said it (fact.airband.note, whole - the answer's message is cut).
            if (!ok) {
                const std::string whole = fact("fact.airband.note");
                opts_.air_note = whole.empty() ? std::string(r.message) : whole;
            }
            break;
        case AskKind::None: break;
    }
}

// The export this section asked for has been written, or has failed (airbandPollExport): only a preset's export is said here.
void ToolsPublisher::pollExport() {
    const std::string seqText = fact("fact.bookmarks.export.seq");
    if (seqText.empty()) { return; }
    const std::uint64_t seq = std::strtoull(seqText.c_str(), nullptr, 10);
    if (!exportSeqKnown_) {   // whatever was written before this window looked is not news
        exportSeqKnown_ = true;
        exportSeqSeen_ = seq;
        return;
    }
    if (seq == exportSeqSeen_) { return; }
    exportSeqSeen_ = seq;
    if (!opts_.air_export_waiting || fact("fact.bookmarks.export.preset") != "1") { return; }
    opts_.air_export_waiting = false;
    const bool ok = fact("fact.bookmarks.export.ok") == "1";
    const std::string shown = fact("fact.bookmarks.export.path");
    if (ok) {
        const std::size_t count = static_cast<std::size_t>(std::strtoull(fact("fact.bookmarks.export.count").c_str(), nullptr, 10));
        opts_.air_preset_note = cc::formatText(tr("Exported %zu to %s"), count, shown.c_str());
    } else {
        opts_.air_preset_note = cc::formatText(tr("Could not write %s"), shown.c_str());
    }
    opts_.air_folder_key = ok;   // a file that was written offers to open its folder
}

// Once a read (airbandFrame): what the monitor last said replaces what the section said, and the settings a hand was on are the engine's again once its figure
// has caught up. (The ImGui frame also follows the patch view away; this window has no patch view to leave.)
void ToolsPublisher::airbandFrame() {
    pollExport();
    const std::string note = fact("fact.airband.note");
    if (note != airNoteSeen_) {
        airNoteSeen_ = note;
        opts_.air_note = note;
    }
    const std::string sq = fact("fact.airband.squelchDb");
    const double sqDb = sq.empty() ? -55.0 : std::atof(sq.c_str());
    if (opts_.squelch_held && !squelchPending_ && std::fabs(sqDb - static_cast<double>(opts_.squelch_edit)) < 0.05) { opts_.squelch_held = false; }
    const std::string hold = fact("fact.airband.holdS");
    const double holdS = hold.empty() ? 2.0 : std::atof(hold.c_str());
    if (opts_.hold_held && std::fabs(holdS - opts_.hold_edit) < 0.05) { opts_.hold_held = false; }
}

// A drag of the squelch slider: the figure on show follows the hand at once, the command goes ten times a second at most and the last position always.
void ToolsPublisher::flushSquelch() {
    if (!squelchPending_) { return; }
    const std::int64_t now = static_cast<std::int64_t>(nowS() * 1000.0);
    if (now - squelchSentMs_ < kSquelchEveryMs) { return; }
    squelchSentMs_ = now;
    squelchPending_ = false;
    exec(squelchEffect_, "airband-squelch");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// executing an Effect

void ToolsPublisher::applyLocal(const Effect& e) {
    switch (e.local) {
        case Local::None: break;
        case Local::Deferred:
            std::fprintf(stderr, "foxsdr-slint: tools: deferred: the scope mode / the radar scope - next allowance (%s)\n", e.text.c_str());
            break;
        case Local::PositionLat: opts_.lat_text = e.text; break;
        case Local::PositionLon: opts_.lon_text = e.text; break;
        case Local::BookmarkName: opts_.name_draft = e.text; break;
        case Local::BookmarkGroup: opts_.group_draft = e.text; break;
        case Local::BookmarkImportPath:
            opts_.import_path = e.text;
            importNote_.clear();
            break;
        case Local::BookmarkSearch: opts_.search = e.text; break;
        case Local::BookmarkFilter:
            opts_.filter_group = e.text;
            opts_.rename_want.clear();
            opts_.rename_draft = e.text;   // the rename field opens with the group's own name, as the ImGui key seeds it
            break;
        case Local::BookmarkFavourites: opts_.favourites_only = e.flag; break;
        case Local::BookmarkOnSpectrum:
            opts_.on_spectrum = e.flag;
            if (hooks_.saveBookmarkOptions) { hooks_.saveBookmarkOptions(opts_.on_spectrum, opts_.stack_names); }   // bookmarkMarkers (docs 6.52)
            break;
        case Local::BookmarkStack:
            opts_.stack_names = e.flag;
            if (hooks_.saveBookmarkOptions) { hooks_.saveBookmarkOptions(opts_.on_spectrum, opts_.stack_names); }   // bookmarkStackNames (docs 6.52)
            break;
        case Local::BookmarkRenameDraft: opts_.rename_draft = e.text; break;
        case Local::BookmarkRenameOpen:
            opts_.rename_open = e.flag;
            opts_.rename_draft = e.text;
            break;
        case Local::BookmarkRenamed:
            // The filter follows the group to its new name once the list has it (bookmarkRenameWant_); the field closes.
            opts_.rename_want = e.text;
            opts_.filter_group.clear();
            opts_.rename_draft = e.text;
            opts_.rename_open = false;
            renameWantAt_ = nowS();
            break;
        case Local::BookmarkMenu: opts_.menu_row = e.number; break;
        case Local::OpenImportDialog:
            if (hooks_.openImportDialog) { hooks_.openImportDialog(e.flag ? e.text : std::string()); }   // an Airband Import over an empty box names its preset (docs 6.61)
            break;
        case Local::ImportLookup: startBookmarkLookup(e.text); break;
        case Local::PresetImportLookup: startPresetImport(e.text, e.text2); break;
        case Local::ScannerDraft: {
            const std::size_t f = static_cast<std::size_t>(std::clamp(e.number, 0, kScannerFieldCount - 1));
            opts_.scan_draft[f] = e.text;
            opts_.scan_draft_bits |= 1u << f;
            scanDraftAt_[f] = nowS();
            break;
        }
        case Local::AirbandCode: opts_.air_code = e.text; break;
        case Local::AirbandPresetName: opts_.air_preset = e.text; break;
        case Local::AirbandFreq: opts_.air_freq = e.text; break;
        case Local::AirbandName: opts_.air_name = e.text; break;
        case Local::AirbandImportPath: opts_.air_import_path = e.text; break;
        case Local::AirbandMode: opts_.air_mode = e.number; break;
        case Local::AirbandGroup:
            opts_.air_group = e.text;
            if (e.flag) { opts_.air_preset = e.text2; }
            break;
        case Local::AirbandBusiest: opts_.air_busiest = e.flag; break;
        case Local::OpenFolder: openFolder(e.text); break;
        case Local::TickRun: {
            Pending::TickRun& run = pending_->ticks;
            run.on = e.flag;
            run.ids = e.ids;
            run.next = 0;
            run.settle = 0;
            break;
        }
        case Local::SquelchHeld:
            opts_.squelch_held = true;
            opts_.squelch_edit = static_cast<float>(e.value);
            break;
        case Local::HoldHeld:
            opts_.hold_held = true;
            opts_.hold_edit = e.value;
            break;
        case Local::MarkersOpen: opts_.markers_shown = true; break;
        case Local::MarkersClosed: opts_.markers_shown = false; break;
        case Local::MarkerNoteHold:
            opts_.note_hold.number = e.number;
            opts_.note_hold.text = e.text;
            opts_.note_hold.frames = kNoteHoldFrames;
            break;
        case Local::Clipboard: setClipboard(e.text); break;
    }
}

void ToolsPublisher::exec(const Effect& e, const char* what) {
    forceRead_ = true;
    if (e.empty()) { return; }
    // The Airband section's lists and notes: what goes first, then what is put (a lookup clears the choices, then offers its own).
    if (e.clear_choices) { opts_.choices.clear(); }
    if (e.clear_nearest) { opts_.nearest.clear(); }
    if (e.clear_folder_key) { opts_.air_folder_key = false; }
    if (e.clear_preset_note) { opts_.air_preset_note.clear(); }
    if (e.set_choices) { opts_.choices = e.choices; }
    if (e.set_nearest) { opts_.nearest = e.nearest; }
    if (e.set_note) { opts_.air_note = e.note; }
    for (const Command& c : e.commands) {
        const std::uint64_t ticket = send(c);
        if (ticket != 0u && e.ask.kind != AskKind::None) {
            Pending::Ask a;
            a.kind = e.ask.kind;
            a.ticket = ticket;
            a.group = e.ask.group;
            a.total = e.ask.total;
            a.ticked = e.ask.ticked;
            pending_->asks.push_back(std::move(a));
        }
    }
    if (e.coverage_reset) { kit_.coverage().reset(); }   // the position was set: the coverage starts again (submitReceiverPosition)
    if (e.local != Local::None) { applyLocal(e); }
    (void)what;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the models

::RadarModel ToolsPublisher::mapRadar(const RadarModel& m, const foxsdr::slintsystem::GpsRow& gps) {
    ::RadarModel o{};
    o.warning = ss(m.warning);
    const PositionOffers& p = m.offers;
    ::PositionOffers f{};
    f.known = p.known;
    f.known_text = ss(p.known_text);
    f.heard_label = ss(p.heard_label);
    f.heard_enabled = p.heard_enabled;
    f.map_label = ss(p.map_label);
    f.map_enabled = p.map_enabled;
    f.hint = ss(p.hint);
    f.lat_text = ss(p.lat_text);
    f.lon_text = ss(p.lon_text);
    f.error = ss(p.error);
    f.set_label = ss(p.set_label);
    f.set_disabled = p.set_disabled;
    f.set_tooltip = ss(p.set_tooltip);
    f.gps_shown = p.gps_shown;
    f.gps_hint = ss(p.gps_hint);
    ::GpsRow g{};
    g.ports = lists_->gpsPorts.set(items(gps.ports));
    g.port_index = gps.port_index;
    g.bauds = lists_->gpsBauds.set(items(gps.bauds));
    g.baud_index = gps.baud_index;
    g.listening = gps.listening;
    g.key_label = ss(gps.key_label);
    g.status = ss(gps.status);
    g.status_tone = tone(gps.status_tone);
    g.satellites = ss(gps.satellites);
    g.key_enabled = gps.key_enabled;
    g.refusal = ss(gps.refusal);
    f.gps = std::move(g);
    f.footnote = ss(p.footnote);
    o.offers = std::move(f);
    o.scope_label = ss(m.scope_label);
    o.scope_on = m.scope_on;
    o.scope_note = ss(m.scope_note);
    return o;
}

::BookmarksModel ToolsPublisher::mapBookmarks(const BookmarksModel& m) {
    ::BookmarksModel o{};
    o.name_draft = ss(m.name_draft);
    o.add_label = ss(m.add_label);
    o.group_draft = ss(m.group_draft);
    o.groups = lists_->groups.set(items(m.groups));
    o.import_path = ss(m.import_path);
    o.import_label = ss(m.import_label);
    o.import_note = ss(m.import_note);
    o.disk_wait = ss(m.disk_wait);
    o.search = ss(m.search);
    o.filter_groups = lists_->filterGroups.set(items(m.filter_groups));
    o.filter_index = m.filter_index;
    o.favourites_only = m.favourites_only;
    o.on_spectrum = m.on_spectrum;
    o.stack_names = m.stack_names;
    o.stack_enabled = m.stack_enabled;
    o.count_line = ss(m.count_line);
    o.export_label = ss(m.export_label);
    o.remove_group_label = ss(m.remove_group_label);
    o.rename_group_label = ss(m.rename_group_label);
    o.rename_draft = ss(m.rename_draft);
    std::vector<::BookmarkRow> rows;
    rows.reserve(m.rows.size());
    for (const BookmarkRow& r : m.rows) {
        ::BookmarkRow x{};
        x.id = r.id;
        x.ticked = r.ticked;
        x.favourite = r.favourite;
        x.text = ss(r.text);
        x.group = ss(r.group);
        x.hz_text = ss(r.hz_text);
        x.mode = ss(r.mode);
        x.name = ss(r.name);
        rows.push_back(std::move(x));
    }
    o.rows = lists_->bmRows.set(rows);
    o.menu_row = m.menu_row;
    o.error = ss(m.error);
    o.import_busy = m.import_busy;
    o.export_busy = m.export_busy;
    return o;
}

::ScannerModel ToolsPublisher::mapScanner(const ScannerModel& m) {
    ::ScannerModel o{};
    o.ticked_mode = m.ticked_mode;
    o.ticked_label = ss(m.ticked_label);
    o.start_mhz = ss(m.start_mhz);
    o.stop_mhz = ss(m.stop_mhz);
    o.step_khz = ss(m.step_khz);
    o.dwell_ms = ss(m.dwell_ms);
    o.hold_ms = ss(m.hold_ms);
    o.resume_ms = ss(m.resume_ms);
    o.listen_ms = ss(m.listen_ms);
    o.range_enabled = m.range_enabled;
    o.scanning = m.scanning;
    o.start_label = ss(m.start_label);
    o.stop_label = ss(m.stop_label);
    o.skip_label = ss(m.skip_label);
    o.status = ss(m.status);
    o.status_tone = tone(m.status_tone);
    o.start_disabled = m.start_disabled;
    return o;
}

::AirbandModel ToolsPublisher::mapAirband(const AirbandModel& m) {
    ::AirbandModel o{};
    o.code_draft = ss(m.code_draft);
    o.lookup_label = ss(m.lookup_label);
    o.nearest_label = ss(m.nearest_label);
    o.hide_label = ss(m.hide_label);
    const auto airports = [](const std::vector<AirportRow>& v) {
        std::vector<::AirportRow> out;
        out.reserve(v.size());
        for (const AirportRow& r : v) {
            ::AirportRow x{};
            x.code = ss(r.code);
            x.name = ss(r.name);
            x.distance = ss(r.distance);
            x.note = ss(r.note);
            out.push_back(std::move(x));
        }
        return out;
    };
    o.airports = lists_->airports.set(airports(m.airports));
    o.ambiguous = lists_->ambiguous.set(airports(m.ambiguous));
    o.preset_name = ss(m.preset_name);
    o.presets = lists_->presets.set(items(m.presets));
    o.preset_index = m.preset_index;
    o.freq_draft = ss(m.freq_draft);
    o.modes = lists_->modes.set(items(m.modes));
    o.mode_index = m.mode_index;
    o.name_draft = ss(m.name_draft);
    o.add_label = ss(m.add_label);
    o.note = ss(m.note);
    o.import_path = ss(m.import_path);
    // The "Waiting for the disk..." line has no field of its own in the UI: it follows the preset's note in the one line.
    o.import_label = ss(m.import_label);
    o.export_label = ss(m.export_label);
    o.remove_preset_label = ss(m.remove_preset_label);
    std::string presetNote = m.preset_note;
    if (!m.disk_wait.empty()) { presetNote += presetNote.empty() ? m.disk_wait : "\n" + m.disk_wait; }
    o.preset_note = ss(presetNote);
    o.folder_label = ss(m.folder_label);
    o.groups = lists_->airGroups.set(items(m.groups));
    o.group_index = m.group_index;
    o.count_line = ss(m.count_line);
    o.tick_all_label = ss(m.tick_all_label);
    o.untick_all_label = ss(m.untick_all_label);
    o.busiest_first = m.busiest_first;
    o.hearing = ss(m.hearing);
    std::vector<::AirbandRow> rows;
    rows.reserve(m.rows.size());
    for (const AirbandRow& r : m.rows) {
        ::AirbandRow x{};
        x.id = r.id;
        x.ticked = r.ticked;
        x.heard = r.heard;
        x.text = ss(r.text);
        x.heard_time = ss(r.heard_time);
        x.on_air = r.on_air;
        rows.push_back(std::move(x));
    }
    o.rows = lists_->airRows.set(rows);
    o.squelch_norm = m.squelch_norm;
    o.squelch_text = ss(m.squelch_text);
    o.above_noise_label = ss(m.above_noise_label);
    o.hold_text = ss(m.hold_text);
    o.listen_label = ss(m.listen_label);
    o.listening = m.listening;
    o.next_label = ss(m.next_label);
    o.nearest_disabled = m.nearest_disabled;
    o.import_busy = m.import_busy;
    o.export_busy = m.export_busy;
    return o;
}

::MarkersModel ToolsPublisher::mapMarkers(const MarkersModel& m) {
    ::MarkersModel o{};
    o.shown = m.shown;
    std::vector<::MarkerRow> rows;
    rows.reserve(m.rows.size());
    for (const MarkerRow& r : m.rows) {
        ::MarkerRow x{};
        x.number = r.number;
        x.hz_text = ss(r.hz_text);
        x.note = ss(r.note);
        x.dropped = ss(r.dropped);
        rows.push_back(std::move(x));
    }
    o.rows = lists_->markerRows.set(rows);
    o.error = ss(m.error);
    o.clear_label = ss(m.clear_label);
    o.empty_line = ss(m.empty_line);
    o.clear_yes_label = ss(m.clear_yes_label);
    o.copy_label = ss(m.copy_label);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------------------
ReadOptions ToolsPublisher::buildOptions(const FoxReceiverState& state, const foxsdr::slintsystem::GpsRow& gps) {
    (void)state;
    opts_.cache = &cache_;
    // the Radar fold: the heard aircraft's middle, the map page's centre, the track sources' census, the SYSTEM bank's GPS row
    opts_.traffic_heard = trafficCentre(kit_.tracks(), opts_.traffic_lat, opts_.traffic_lon);
    opts_.track_census = trackCensus(kit_.lists().rows);
    opts_.gps = gps;
    // The open page first (the one being looked at), else any page whose view is known (a page never fitted sits on its default, which is no view).
    const foxsdr::slintkit::MapPage* page = nullptr;
    for (const foxsdr::slintkit::MapPage& p : kit_.mapPages()) {
        if (!p.viewFitted) { continue; }
        if (page == nullptr || (p.open && !page->open)) { page = &p; }
    }
    opts_.map_have = page != nullptr;
    if (page != nullptr) {
        opts_.map_plugin = page->plugin;
        opts_.map_lat = page->centreLat;
        opts_.map_lon = page->centreLon;
    }
    // the two disk waits
    const double now = nowS();
    const bool importOut = jobs_->lookup.pending() || fact("fact.bookmarks.importing") == "1";
    if (!importOut) {
        importSince_ = -1.0;
    } else if (importSince_ < 0.0) {
        importSince_ = now;
    }
    opts_.import_lookup_pending = jobs_->lookup.pending();
    opts_.import_elapsed_s = importSince_ < 0.0 ? 0.0 : now - importSince_;
    opts_.air_import_pending = jobs_->preset.pending();
    opts_.air_import_elapsed_s = jobs_->preset.elapsedS();
    // the scanner's drafts: a field shows what was typed until the engine's figure is the typed one (or a second has gone)
    const std::array<std::string, kScannerFieldCount> texts = scannerFieldTexts(state);
    for (int i = 0; i < kScannerFieldCount; ++i) {
        const unsigned bit = 1u << i;
        if ((opts_.scan_draft_bits & bit) == 0u) { continue; }
        if (sameNumber(texts[static_cast<std::size_t>(i)], opts_.scan_draft[static_cast<std::size_t>(i)]) || now - scanDraftAt_[i] > 1.0) { opts_.scan_draft_bits &= ~bit; }
    }
    return opts_;
}

void ToolsPublisher::publish(const FoxReceiverState& state, const foxsdr::slintsystem::GpsRow& gps) {
    state_ = state;
    gps_ = gps;
    const std::int64_t nowMs = static_cast<std::int64_t>(nowS() * 1000.0);
    // the duties of every tick (airbandFrame's): the disk workers, the chunks of an import, the Tick all run, the held note and the held squelch
    pollJobs();
    sendChunks();
    sendTicks();
    flushSquelch();
    if (opts_.note_hold.frames > 0 && --opts_.note_hold.frames == 0) { forceRead_ = true; }
    if (followPosition(state, seen_, opts_.lat_text, opts_.lon_text)) { forceRead_ = true; }
    // The lists and facts are read at 10 Hz (and at once after a gesture): the ImGui window reads them every frame, this one does not need a thirtieth of a second.
    if (!forceRead_ && haveModels_ && nowMs - lastReadMs_ < 100) { return; }
    forceRead_ = false;
    lastReadMs_ = nowMs;
    airbandFrame();
    if (!opts_.rename_want.empty() && renameWantAt_ >= 0.0 && nowS() - renameWantAt_ > kRenameWantS) { opts_.rename_want.clear(); }
    const ReadOptions options = buildOptions(state, gps);
    ToolsModels m = readTools(table_, state, options);
    // A group just renamed: the filter follows it once the list has it.
    if (!opts_.rename_want.empty() && m.bookmarks.filter_group == opts_.rename_want) {
        opts_.filter_group = opts_.rename_want;
        opts_.rename_want.clear();
    }
    // A bookmark import's own failure (the worker found no file) says itself where the engine's note would.
    if (!importNote_.empty()) { m.bookmarks.import_note = importNote_; }
    last_ = m;
    haveModels_ = true;

    const auto setIf = [](auto& slot, auto&& value, auto&& setter) {
        if (!slot.has_value() || !(*slot == value)) {
            slot = value;
            setter(value);
        }
    };
    setIf(lastSet_->radar, mapRadar(m.radar, gps), [&](const ::RadarModel& v) { ui_.set_radar_model(v); });
    setIf(lastSet_->bookmarks, mapBookmarks(m.bookmarks), [&](const ::BookmarksModel& v) { ui_.set_bookmarks_model(v); });
    setIf(lastSet_->scanner, mapScanner(m.scanner), [&](const ::ScannerModel& v) { ui_.set_scanner_model(v); });
    setIf(lastSet_->airband, mapAirband(m.airband), [&](const ::AirbandModel& v) { ui_.set_airband_model(v); });
    setIf(lastSet_->markers, mapMarkers(m.markers), [&](const ::MarkersModel& v) { ui_.set_markers_model(v); });
}

ToolsPublisher::RowFace ToolsPublisher::face(int row) const {
    RowFace f;
    if (!haveModels_ || row < 0 || row >= kToolsRowCount) { return f; }
    const RowChip& c = last_.chips[static_cast<std::size_t>(row)];
    f.chip = c.word;
    f.valid = !c.word.empty();
    switch (row) {
        case kRowScanner: f.tone = tone(scannerTone(state_.scannerState)); break;   // scanning and paused good, holding warn, idle neutral
        case kRowDemodScope:
        case kRowAirband: f.tone = c.lit ? ::ChipTone::Good : ::ChipTone::Neutral; break;
        default: f.tone = ::ChipTone::Neutral; break;
    }
    return f;
}

void ToolsPublisher::openMarkers() {
    opts_.markers_shown = true;
    forceRead_ = true;
}

std::string ToolsPublisher::summaryLine() const {
    if (!haveModels_) { return "tools: no models read"; }
    std::string hearing = last_.airband.hearing;
    std::replace(hearing.begin(), hearing.end(), '\n', '|');
    std::string chips;
    for (int r = 0; r < kToolsRowCount; ++r) { chips += (chips.empty() ? "" : ", ") + std::to_string(r) + "=" + last_.chips[static_cast<std::size_t>(r)].word; }
    return "tools: bookmarks=" + std::to_string(last_.bookmarks.total) + " shown=" + std::to_string(last_.bookmarks.rows.size()) + " count line=\"" +
           last_.bookmarks.count_line + "\" groups=" + std::to_string(last_.bookmarks.groups.size()) + "; scanner status=\"" + last_.scanner.status + "\" chip=" +
           last_.scanner.chip + " ticked=" + std::to_string(last_.scanner.ticked_count) + "; airband hearing=\"" + hearing + "\" rows=" + std::to_string(last_.airband.rows.size()) +
           " count line=\"" + last_.airband.count_line + "\" chip=" + last_.airband.chip + "; markers=" + std::to_string(last_.markers.rows.size()) + " shown=" +
           (last_.markers.shown ? "yes" : "no") + "; radar position " + (last_.radar.offers.known ? "known" : "not set") + " offers \"" + last_.radar.offers.heard_label +
           "\"; chips " + chips + "; commands sent=" + std::to_string(sent_);
}

// ---------------------------------------------------------------------------------------------------------------------------------
void ToolsPublisher::wire() {
    auto&& ev = ui_.global<::ToolsEvents>();
    // The preset the keys act on, as the field holds it NOW (the last read may be a tick behind a commit that came just before the press).
    const auto presetNow = [this] {
        const std::string t = cascade::gui::trimPresetName(opts_.air_preset);
        return t.empty() ? std::string("Manual") : t;
    };
    // ---- Demod scope and Radar: the two scope windows are deferred (the effects log it) ----
    ev.on_scope_clicked([this] { exec(applyScopeClicked(), "scope-clicked"); });
    ev.on_radar_scope_clicked([this] { exec(applyRadarScopeClicked(), "radar-scope-clicked"); });
    // ---- the receiver position offers (the GPS row's three callbacks are the SystemEvents', the SYSTEM publisher's) ----
    ev.on_position_heard([this] { exec(applyPositionHeard(opts_), "position-heard"); });
    ev.on_position_map([this] { exec(applyPositionMap(opts_), "position-map"); });
    ev.on_position_lat([this](slint::SharedString t) { exec(applyPositionLat(std::string(t)), "position-lat"); });
    ev.on_position_lon([this](slint::SharedString t) { exec(applyPositionLon(std::string(t)), "position-lon"); });
    ev.on_position_set([this] { exec(applyPositionSet(opts_.lat_text, opts_.lon_text), "position-set"); });

    // ---- Bookmarks ----
    ev.on_bookmark_name([this](slint::SharedString t) { exec(applyBookmarkName(std::string(t)), "bookmark-name"); });
    ev.on_bookmark_add([this] { exec(applyBookmarkAdd(opts_.name_draft, opts_.group_draft, state_.tunedHz), "bookmark-add"); });
    ev.on_bookmark_group([this](slint::SharedString t) { exec(applyBookmarkGroup(std::string(t)), "bookmark-group"); });
    ev.on_bookmark_import_path([this](slint::SharedString t) { exec(applyBookmarkImportPath(std::string(t)), "bookmark-import-path"); });
    ev.on_bookmark_import([this] {
        BookmarksModel m = last_.bookmarks;   // the box as it is now (the key flushes the field first)
        m.import_path = opts_.import_path;
        exec(applyBookmarkImport(m), "bookmark-import");
    });
    ev.on_bookmark_search([this](slint::SharedString t) { exec(applyBookmarkSearch(std::string(t)), "bookmark-search"); });
    ev.on_bookmark_filter([this](int i) { exec(applyBookmarkFilter(last_.bookmarks, i), "bookmark-filter"); });
    ev.on_bookmark_favourites([this](bool on) { exec(applyBookmarkFavourites(on), "bookmark-favourites"); });
    ev.on_bookmark_on_spectrum([this](bool on) { exec(applyBookmarkOnSpectrum(on), "bookmark-on-spectrum"); });
    ev.on_bookmark_stack([this](bool on) { exec(applyBookmarkStack(last_.bookmarks, on), "bookmark-stack"); });
    ev.on_bookmark_export([this] { exec(applyBookmarkExport(last_.bookmarks), "bookmark-export"); });
    ev.on_bookmark_remove_group([this] { exec(applyBookmarkRemoveGroup(last_.bookmarks), "bookmark-remove-group"); });
    ev.on_bookmark_rename_draft([this](slint::SharedString t) { exec(applyBookmarkRenameDraft(std::string(t)), "bookmark-rename-draft"); });
    // The UI's "Rename this group" key shows and hides the field itself and tells the host which (docs 6.61): applyBookmarkRenameOpen seeds the draft with the group's own
    // name every time, as the ImGui key does, so a name typed and abandoned does not come back. The field's own Rename is applyBookmarkRename.
    ev.on_bookmark_rename_open([this](bool open) {
        BookmarksModel m = last_.bookmarks;
        m.rename_open = !open;   // the effect is the key's TOGGLE: it lands on what the field now is
        exec(applyBookmarkRenameOpen(m), "bookmark-rename-open");
    });
    ev.on_bookmark_rename([this] {
        BookmarksModel m = last_.bookmarks;
        m.rename_draft = opts_.rename_draft;
        exec(applyBookmarkRename(m), "bookmark-rename");
    });
    ev.on_bookmark_tick([this](int id, bool on) { exec(applyBookmarkTick(last_.bookmarks, id, on), "bookmark-tick"); });
    ev.on_bookmark_favourite([this](int id, bool on) { exec(applyBookmarkFavourite(last_.bookmarks, id, on), "bookmark-favourite"); });
    ev.on_bookmark_tune([this](int id) { exec(applyBookmarkTune(last_.bookmarks, id), "bookmark-tune"); });
    ev.on_bookmark_delete([this](int id) { exec(applyBookmarkDelete(last_.bookmarks, id), "bookmark-delete"); });
    ev.on_bookmark_menu([this](int id) { exec(applyBookmarkMenu(id), "bookmark-menu"); });
    ev.on_bookmark_move([this](int id, slint::SharedString g) { exec(applyBookmarkMove(last_.bookmarks, id, std::string(g)), "bookmark-move"); });

    // ---- Scanner ----
    ev.on_scanner_ticked([this](bool on) { exec(applyScannerTicked(on), "scanner-ticked"); });
    ev.on_scanner_start([this](slint::SharedString t) { exec(applyScannerField(last_.scanner, state_, kFieldStart, std::string(t)), "scanner-start"); });
    ev.on_scanner_stop([this](slint::SharedString t) { exec(applyScannerField(last_.scanner, state_, kFieldStop, std::string(t)), "scanner-stop"); });
    ev.on_scanner_step([this](slint::SharedString t) { exec(applyScannerField(last_.scanner, state_, kFieldStep, std::string(t)), "scanner-step"); });
    ev.on_scanner_dwell([this](slint::SharedString t) { exec(applyScannerField(last_.scanner, state_, kFieldDwell, std::string(t)), "scanner-dwell"); });
    ev.on_scanner_hold([this](slint::SharedString t) { exec(applyScannerField(last_.scanner, state_, kFieldHold, std::string(t)), "scanner-hold"); });
    ev.on_scanner_resume([this](slint::SharedString t) { exec(applyScannerField(last_.scanner, state_, kFieldResume, std::string(t)), "scanner-resume"); });
    ev.on_scanner_listen([this](slint::SharedString t) { exec(applyScannerField(last_.scanner, state_, kFieldListen, std::string(t)), "scanner-listen"); });
    ev.on_scanner_run([this] { exec(applyScannerRun(last_.scanner, state_), "scanner-run"); });
    ev.on_scanner_halt([this] { exec(applyScannerHalt(last_.scanner), "scanner-halt"); });
    ev.on_scanner_skip([this] { exec(applyScannerSkip(last_.scanner), "scanner-skip"); });

    // ---- Airband ----
    ev.on_airband_code([this](slint::SharedString t) { exec(applyAirbandCode(std::string(t)), "airband-code"); });
    ev.on_airband_lookup([this] { exec(applyAirbandLookup(opts_.air_code), "airband-lookup"); });
    ev.on_airband_nearest([this] { exec(applyAirbandNearest(state_), "airband-nearest"); });
    ev.on_airband_hide([this] { exec(applyAirbandHide(), "airband-hide"); });
    ev.on_airband_airport([this](int i) { exec(applyAirbandAirport(opts_, i), "airband-airport"); });
    ev.on_airband_ambiguous([this](int i) { exec(applyAirbandAirport(opts_, kChoiceBase + i), "airband-ambiguous"); });
    ev.on_airband_preset([this](int i) { exec(applyAirbandPreset(last_.airband, i), "airband-preset"); });
    ev.on_airband_preset_name([this](slint::SharedString t) { exec(applyAirbandPresetName(std::string(t)), "airband-preset-name"); });
    ev.on_airband_freq([this](slint::SharedString t) { exec(applyAirbandFreq(std::string(t)), "airband-freq"); });
    ev.on_airband_mode([this](int i) { exec(applyAirbandMode(i), "airband-mode"); });
    ev.on_airband_name([this](slint::SharedString t) { exec(applyAirbandName(std::string(t)), "airband-name"); });
    ev.on_airband_add([this, presetNow] {
        AirbandModel m = last_.airband;   // the fields as they are now (the key flushes them first)
        m.freq_draft = opts_.air_freq;
        m.name_draft = opts_.air_name;
        m.mode_index = opts_.air_mode;
        m.preset = presetNow();
        exec(applyAirbandAdd(m), "airband-add");
    });
    ev.on_airband_import_path([this](slint::SharedString t) { exec(applyAirbandImportPath(std::string(t)), "airband-import-path"); });
    ev.on_airband_import([this, presetNow] {
        AirbandModel m = last_.airband;
        m.import_path = opts_.air_import_path;
        m.preset = presetNow();
        exec(applyAirbandImport(m), "airband-import");
    });
    ev.on_airband_export([this, presetNow] {
        AirbandModel m = last_.airband;
        m.preset = presetNow();
        exec(applyAirbandExport(m), "airband-export");
    });
    ev.on_airband_remove_preset([this, presetNow] {
        AirbandModel m = last_.airband;
        m.preset = presetNow();
        exec(applyAirbandRemovePreset(m), "airband-remove-preset");
    });
    ev.on_airband_folder([this] { exec(applyAirbandFolder(table_), "airband-folder"); });
    ev.on_airband_group([this](int i) { exec(applyAirbandGroup(last_.airband, i), "airband-group"); });
    ev.on_airband_tick_all([this] { exec(applyAirbandTickAll(last_.airband, true, pending_->ticks.unsettled()), "airband-tick-all"); });
    ev.on_airband_untick_all([this] { exec(applyAirbandTickAll(last_.airband, false, pending_->ticks.unsettled()), "airband-untick-all"); });
    ev.on_airband_busiest([this](bool on) { exec(applyAirbandBusiest(on), "airband-busiest"); });
    ev.on_airband_tick([this](int id, bool on) { exec(applyAirbandTick(id, on), "airband-tick"); });
    ev.on_airband_tune([this](int id) { exec(applyAirbandTune(id), "airband-tune"); });
    // The slider's figure follows the hand at once; the command goes at most ten times a second (and the last position always), not on every pixel of the drag.
    ev.on_airband_squelch([this](float n) {
        Effect e = applyAirbandSquelch(n);
        if (e.empty()) { return; }
        opts_.squelch_held = true;
        opts_.squelch_edit = static_cast<float>(e.value);
        squelchEffect_ = std::move(e);
        squelchPending_ = true;
        forceRead_ = true;
    });
    ev.on_airband_above_noise([this] { exec(applyAirbandAboveNoise(last_.airband), "airband-above-noise"); });
    ev.on_airband_hold([this](slint::SharedString t) { exec(applyAirbandHold(std::string(t)), "airband-hold"); });
    ev.on_airband_listen([this] { exec(applyAirbandListen(last_.airband), "airband-listen"); });
    ev.on_airband_next([this] { exec(applyAirbandNext(last_.airband), "airband-next"); });

    // ---- the Markers sheet ----
    ev.on_markers_open([this] { exec(applyMarkersOpen(), "markers-open"); });
    ev.on_markers_note([this](int n, slint::SharedString t) { exec(applyMarkerNote(n, std::string(t)), "markers-note"); });
    ev.on_markers_remove([this](int n) { exec(applyMarkerRemove(n), "markers-remove"); });
    ev.on_markers_clear([this] { exec(applyMarkersClear(last_.markers), "markers-clear"); });
    ev.on_markers_tune([this](int n) { exec(applyMarkerTune(last_.markers, n), "markers-tune"); });
    ev.on_markers_bookmark([this](int n) { exec(applyMarkerBookmark(last_.markers, n, state_), "markers-bookmark"); });
    ev.on_markers_copy([this] { exec(applyMarkersCopy(last_.markers), "markers-copy"); });
    ev.on_markers_closed([this] { exec(applyMarkersClosed(), "markers-closed"); });
}

}  // namespace foxsdr::slinttools
