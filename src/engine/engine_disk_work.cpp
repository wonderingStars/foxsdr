// engine_disk_work.cpp - THE ENGINE'S PASS DOES NO DISK WORK (stub campaign batch B19; master 0.99.64 - 0.99.66, app_window_disk_work.cpp).
//
// WHAT THIS IS. docs/DIAGNOSTICS.md, "The window does no disk work", is the audit of what the 0.99.63 Record fix left on the thread that draws
// the window; four field freezes in 0.99.58 - 0.99.63 were one defect, a file-system call made against a disk that was slow to answer (a
// synchronised or network folder, a drive that had spun down, a scanner holding the file). Master moved each call to a worker in the house
// pattern of core/disk_job.hpp. The engine split moved the calls with the work: the I/Q file's Open, the frequency list's export and the
// patch Radio's listing of recordings are the ENGINE's now, and the 0.99.84 merge carried them as they were - inline on the pass, which is
// the thread a window frame (the Host kind) or a browser's answer (the headless kind) waits on. They are workers again, here:
//
//   openIqFile / pollIqOpen / installOpenedIqFile   SELECT_SOURCE "file:<path>": the header is read on a worker, the source is installed on
//       the pass that finds the file open. A second Open while one is out changes nothing; a result that arrives after the choice moved on is
//       dropped and its file closed off the pass; the engine's own `sourceError_` carries the reason, in the source's own words.
//   importBookmarkFile                              FOXAPP_OP_BOOKMARK_IMPORT_FILE: the file is read and parsed on a worker, and the rows join the list on
//       the pass that collects it (finishBookmarkImport, engine.cpp, is the body the synchronous import ran after its read). One at a time; one asked
//       for while one is out is remembered, the last wins. The answer to the op is that the read is under way: what it found is importLast_.
//   exportBookmarks                                 FOXAPP_OP_BOOKMARK_EXPORT: the text is made on the pass (a walk of the store, the store's FULL
//       names), the folder and the file are a worker's. ival[0] 0 is SDR#'s XML for a list of ids, 1 is one preset's CSV (the AIRBAND
//       section's Export CSV, which master wrote from the window's own copy of the list).
//   patchListRecordings                             the recordings folders listed and up to kMaxRecordingOpens headers read, on a worker that
//       owns a COPY of the probe cache and hands both back.
//   pollDiskJobs                                    once a pass, from pumpWorkers: collects what the workers have finished. NEVER BLOCKS.
//
// The slow-disk seam, diskHook(), is called by each worker immediately before it touches the disk: a test binds one that sleeps - a slow disk
// as far as the pass can tell - with the real files, the real BMP and CSV and XML writers and the real header parser behind it
// (tests/test_gui_file_jobs.cpp, tests/test_iq_open_async.cpp).
//
// The patch Radio on a recording is the patch radios' own open worker (engine_patch_radios.cpp, patchRadioPending_). The window's own disk work
// - a plugin picture saved as a BMP, the screenshot's files - is src/gui/app_window_disk_work.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/diag_log.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/patch_recordings.hpp"
#include "core/unique_file.hpp"
#include "core/utf8_text.hpp"
#include "core/write_fault.hpp"
#include "engine/engine.hpp"
#include "engine/engine_disk_state.hpp"
#include "engine/fox_engine_api.hpp"

namespace cascade::engine {

namespace cc = cascade::core;
using cascade::i18n::tr;

// --- THE OPAQUE STATE (see engine_disk_state.hpp) -----------------------------------------------------------------------------------------

void EngineDiskWorkDeleter::operator()(EngineDiskWork* p) const { delete p; }

EngineDiskWork& Engine::disk() {
    if (!diskWork_) { diskWork_.reset(new EngineDiskWork()); }
    return *diskWork_;
}

bool Engine::iqOpenPending() const { return diskWork_ != nullptr && diskWork_->iqOpen.pending(); }
bool Engine::bookmarkExportPending() const { return diskWork_ != nullptr && diskWork_->bookmarkExport.pending(); }
bool Engine::bookmarkImportPending() const { return diskWork_ != nullptr && diskWork_->bookmarkImport.pending(); }
bool Engine::recordingScanPending() const { return diskWork_ != nullptr && diskWork_->recordingScan.pending(); }

void Engine::setDiskHookForTest(std::function<void()> hook) {
    std::lock_guard<std::mutex> lk(diskHookMutex_);
    diskHook_ = std::move(hook);
}

std::function<void()> Engine::diskHook() const {
    std::lock_guard<std::mutex> lk(diskHookMutex_);
    return diskHook_;
}

// --- THE I/Q FILE'S OPEN ---------------------------------------------------------------------------------------------------------------------

void Engine::openIqFile(const std::string& path) {
    EngineDiskWork& d = disk();
    // ONE OPEN AT A TIME: a second press while the header is still being read (the key is disabled, but a browser and a key binding are not the
    // key) changes nothing.
    if (d.iqOpen.pending()) { return; }
    // The user chose a file: a saved radio that was only busy at start-up is no longer what the session is waiting for (pollSavedRadioRetry,
    // 0.99.92).
    cancelSavedRadioRetry();
    // THE DISK IS NOT TOUCHED HERE. This made an IqFileSource and called open() on it - an ifstream on the path the user typed and the RIFF
    // header walked with reads - on the pass, and a recording on a network share or a sleeping drive held every command, every answer and every
    // publish for as long as the disk took (see core/disk_job.hpp). The worker owns the path and the factory by value and hands back the opened
    // source; pollIqOpen() installs it on a later pass. The pipeline keeps its current source until then, as a failed open always left it.
    const std::uint64_t gen = sourceGen_.load();
    const int row = sourceSel_;
    const auto factory = iqFileFactory_;
    const auto hook = diskHook();
    d.iqOpen.request([path, gen, row, factory, hook]() -> IqOpenResult {
        IqOpenResult r;
        r.path = path;
        r.gen = gen;
        r.row = row;
        try {
            if (hook) { hook(); }
            auto file = factory ? factory() : std::make_unique<cascade::source::IqFileSource>();
            if (file->open(path)) {
                r.file = std::move(file);
            } else {
                r.error = file->lastError();
            }
        } catch (...) {
            r.file.reset();
            r.error = "cannot open file: " + path;
        }
        return r;
    });
}

void Engine::pollIqOpen() {
    if (diskWork_ == nullptr) { return; }
    IqOpenResult r;
    if (!diskWork_->iqOpen.poll(r)) { return; }
    if (!r.file && r.error.empty()) { r.error = "cannot open file: " + r.path; }
    // A source that was open while it was being replaced is closed by whoever lets it go, and a close is a file-system call too: not on this
    // thread.
    const auto letGo = [](std::unique_ptr<cascade::source::IqFileSource> f) {
        if (f) { std::thread([g = std::move(f)]() mutable { g.reset(); }).detach(); }
    };
    // THE CHOICE MAY HAVE MOVED ON while the header was being read (the same rule as a sound card's open): another source installed (sourceGen_
    // moved), a radio open under way, or the combo moved off the row it was asked on. The open file is then simply closed again.
    if (r.gen != sourceGen_.load() || deviceOpenPending_ || sourceSel_ != r.row) {
        cc::diagLogf("source: an I/Q file open finished after the choice moved on");
        letGo(std::move(r.file));
        return;
    }
    if (!r.file) {
        // The reason, exactly as the synchronous open gave it.
        sourceError_ = r.error;
        return;
    }
    installOpenedIqFile(std::move(r.file), r.path);
}

void Engine::installOpenedIqFile(std::unique_ptr<cascade::source::IqFileSource> file, const std::string& path) {
    // Carry the displayed frequency over: a file's center is nominal anyway, and a readout that jumps to 0 on source switch would read as a
    // tuning bug. The AIR frequency carries over; the file is told it through its own converter, which is off unless the user set one for I/Q
    // files.
    const double fileRadioHz = radioHzForSource("file", std::string(), pipeline_.activeSource().centerFrequencyHz());
    if (fileRadioHz >= 0.0) { file->setCenterFrequencyHz(fileRadioHz); }
    device_ = nullptr;  // before setSource destroys a live device
    soapyView_ = nullptr;
    deviceArgs_.clear();
    deviceModel_.clear();
    sourceError_.clear();
    ++sourceGen_;  // a device open still in flight is now stale
    installSource(std::move(file));
    sourceKind_ = "file";
    applyConverterForSource();
    iqOpenPath_ = path;
    // A file is a deliberate choice of source like any other, so a radio remembered from a failed restore is superseded here too - see
    // selectSource's generator row.
    restoreKeep_ = cascade::gui::RememberedSource{};
    restoreKeepLabel_.clear();
    followInputRate();  // DSP chain + frequency axis track the file's rate
    // The RATE, never the path: a file name is the user's own data and a report is a support artefact, not a listening record.
    cc::diagLogf("source: opened an I/Q file at %.0f S/s", pipeline_.activeSource().sampleRateHz());
}

// --- THE FREQUENCY LIST EXPORTED --------------------------------------------------------------------------------------------------------------

namespace {

// The preset's name in the file's, as far as a file name can carry it: letters, digits, '-' and '_'; anything else (a space, a slash, a letter
// outside ASCII) is '_', and a long name is cut, so the name is a name on every file system.
std::string fileSafeName(const std::string& group) {
    std::string safe;
    for (const char c : group) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        safe.push_back(ok ? c : '_');
        if (safe.size() >= 40) { break; }
    }
    return safe;
}

}  // namespace

// FOXAPP_OP_BOOKMARK_EXPORT (API 0.3 8.6): ival[0] 0 - the bookmarks whose ids `text` names (decimal numbers, anything else between them), written
// whole, IN THE STORE'S ORDER (the order they are shown in), as SDR#'s frequencies.xml; ival[0] 1 - the rows of the preset (bookmark group) `text`
// names, every mode, ticked or not, as a CSV that the importer reads back whole with its tick and its favourite (the AIRBAND section's Export CSV).
// Either goes to a file under the record folder that the engine names (the sender names no path), on a worker, under a name taken atomically. The
// text is made on the pass FROM THE STORE and not from the BOOKMARKS list, because that list cuts a name at 95 bytes and a group at 159 and carries
// the mode as a code (a mode no code names reads 0): a file made from it would shorten and rewrite what the user shares.
//
// FOXAPI_OK (the write is under way: the note says "Saving..." and then where, or that it could not), BAD_ARGUMENT (nothing named), NOT_FOUND (none of
// those bookmarks exists, or the preset has no rows), NO_CHANGE (an export is already out: one is ever out).
std::int32_t Engine::exportBookmarks(const std::string& text, bool preset) {
    EngineDiskWork& d = disk();
    std::vector<cc::Bookmark> out;
    std::string text8;
    std::string namePattern;
    if (preset) {
        if (text.empty()) { return FOXAPI_BAD_ARGUMENT; }
        for (const cc::Bookmark& b : freqMgr_.list()) {
            if (b.group == text) { out.push_back(b); }
        }
        if (out.empty()) { return FOXAPI_NOT_FOUND; }
        if (d.bookmarkExport.pending()) { return FOXAPI_NO_CHANGE; }
        text8 = cc::exportCsv(out);
        namePattern = "foxsdr-" + fileSafeName(text) + "-%Y%m%d-%H%M%S.csv";
    } else {
        std::unordered_set<std::uint64_t> wanted;
        std::size_t named = 0;  // digit runs seen: zero means nothing was asked for
        for (std::size_t i = 0; i < text.size();) {
            if (text[i] < '0' || text[i] > '9') {
                ++i;
                continue;
            }
            std::uint64_t v = 0;
            bool overflow = false;
            for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                const std::uint64_t digit = static_cast<std::uint64_t>(text[i] - '0');
                if (v > (UINT64_MAX - digit) / 10u) {
                    overflow = true;
                } else {
                    v = v * 10u + digit;
                }
            }
            ++named;
            if (!overflow && v != 0u) { wanted.insert(v); }  // 0 is "not in a list"
        }
        if (named == 0) { return FOXAPI_BAD_ARGUMENT; }
        for (const cc::Bookmark& b : freqMgr_.list()) {
            if (wanted.count(b.id) != 0) { out.push_back(b); }
        }
        if (out.empty()) { return FOXAPI_NOT_FOUND; }
        if (d.bookmarkExport.pending()) { return FOXAPI_NO_CHANGE; }
        text8 = cc::exportSdrSharpXml(out);
        namePattern = "foxsdr-frequencies-%Y%m%d-%H%M%S.xml";
    }
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char name[160];
    std::strftime(name, sizeof(name), namePattern.c_str(), &tmv);
    const std::string shown = recordDir_ + "/" + name;
    const std::filesystem::path path = std::filesystem::path(recordDir_) / name;
    const std::size_t count = out.size();
    const auto hook = diskHook();
    // WHO ASKED is kept here, on the pass, for pollDiskJobs: a worker that throws returns a default outcome, which would send a preset's result
    // to the Bookmarks section and leave this one's "Saving..." up for good (master's 0.99.66 review).
    d.exportForPreset = preset;
    d.bookmarkExport.request([body = std::move(text8), dir = recordDir_, path = path.string(), shown, count, hook]() -> ExportOutcome {
        if (hook) { hook(); }
        ExportOutcome r;
        r.count = count;
        r.shown = shown;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        // THE NAME IS TAKEN ATOMICALLY HERE (core/unique_file.hpp): two exports inside one second are two files, never the second over the first,
        // and the note names the one that is on disk.
        std::string used;
        bool exhausted = false;
        int index = 1;
        if (!cc::reserveUnique(path, used, exhausted, &index)) { return r; }  // not written
        r.shown = cc::numberedPath(shown, index);
        std::ofstream f(used, std::ios::binary | std::ios::trunc);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        cc::writeFaultPoint("export", f);  // test seam: a write that fails part way
        f.close();
        r.ok = static_cast<bool>(f);
        // A list that did not reach the disk whole is not left behind (the empty file taken for it).
        if (!r.ok) { std::filesystem::remove(std::filesystem::path(used), ec); }
        return r;
    });
    // The Bookmarks section's own export says so in its own note; a preset's is said in the AIRBAND section, from the facts.
    if (!preset) { bookmarkImportNote_ = tr("Saving..."); }
    return FOXAPI_OK;
}

// --- THE FREQUENCY LIST IMPORTED ---------------------------------------------------------------------------------------------------------------

void Engine::importBookmarkFile(const std::string& path) {
    EngineDiskWork& d = disk();
    std::string p = path;
    // A path pasted from Explorer's "Copy as path" arrives quoted.
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') { p = p.substr(1, p.size() - 2); }
    // ONE READ AT A TIME, and a list dropped on the window while one is out is not lost: it is remembered (the last one asked for) and read when
    // the first is back (pollDiskJobs).
    if (d.bookmarkImport.pending()) {
        d.importAgain = p;
        d.importAgainSet = true;
        return;
    }
    // THE DISK IS NOT TOUCHED HERE (master 0.99.65; stub campaign B19). The file the user typed or dropped - a path that can be a network share or a
    // sleeping drive - was read, and parsed, on the pass. The worker owns the path by value and hands back what the file held; pollDiskJobs() puts it
    // in the list.
    const auto hook = diskHook();
    d.importPath = p;
    d.bookmarkImport.request([p, hook]() -> ImportOutcome {
        const auto t0 = std::chrono::steady_clock::now();
        if (hook) { hook(); }
        ImportOutcome o;
        o.path = p;
        o.result = cc::importFrequencyFile(p);
        o.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return o;
    });
}

// --- THE PATCH RADIO'S RECORDINGS LIST --------------------------------------------------------------------------------------------------------

void Engine::patchListRecordings() {
    EngineDiskWork& d = disk();
    // ONE LISTING AT A TIME; a request made while one is out is remembered and made when it comes back (pollDiskJobs), so "Look for radios" pressed
    // after a file was saved is never lost.
    if (d.recordingScan.pending()) {
        d.listAgain = true;
        return;
    }
    std::vector<std::string> dirs{recordDir_};
    // A second folder for verification and for anyone who keeps recordings elsewhere - read from the environment, never guessed.
    if (const char* s = std::getenv("FOXSDR_PATCH_SAMPLES"); s != nullptr && *s != '\0') { dirs.emplace_back(s); }
    // THE LISTING IS A WORKER'S (master 0.99.64). The folders are listed and up to kMaxRecordingOpens recording headers opened - in a folder the
    // patch's own speakers keep writing to and a recording on a network share can be anywhere. The worker owns the folders and a COPY of the probe
    // cache (THROUGH THE CACHE: a file already read is not opened again until it changes) and hands both back; pollDiskJobs() swaps them in.
    const auto hook = diskHook();
    d.recordingScan.request([dirs, cache = patchRecordingCache_, hook]() mutable -> RecordingScan {
        if (hook) { hook(); }
        RecordingScan r;
        r.list = cc::patch::listIqRecordings(dirs, cc::patch::kMaxRecordingsListed, &cache);
        r.cache = std::move(cache);
        return r;
    });
}

// --- ONCE A PASS ------------------------------------------------------------------------------------------------------------------------------

void Engine::pollDiskJobs() {
    if (diskWork_ == nullptr) { return; }
    EngineDiskWork& d = *diskWork_;
    pollIqOpen();

    // A FREQUENCY LIST READ: put in the bookmarks, in the words the synchronous import used. The list is the pass's (the worker never touched it).
    {
        ImportOutcome o;
        if (d.bookmarkImport.poll(o)) {
            // A worker that threw (an allocation, a corrupt file): the same sentence a file that cannot be read gets, instead of a crash in the pass.
            if (d.bookmarkImport.threw() && o.result.error.empty()) { o.result.error = "cannot read the file"; }
            // The path the reason names: the one asked for, for a worker that threw and left none.
            const std::string p = o.path.empty() ? d.importPath : o.path;
            const bool ok = finishBookmarkImport(std::move(o.result), p, o.ms);
            importLast_.ok = ok;
            importLast_.path = p;
            importLast_.note = bookmarkImportNote_;
            ++importLast_.seq;
            // A list asked for while this one was being read.
            if (d.importAgainSet) {
                d.importAgainSet = false;
                std::string next;
                next.swap(d.importAgain);
                importBookmarkFile(next);
            }
        }
    }

    // THE LIST EXPORTED. What it says is the Bookmarks section's note (SDR#), or a fact the AIRBAND section words in its own language (a preset's).
    {
        ExportOutcome r;
        if (d.bookmarkExport.poll(r)) {
            if (d.bookmarkExport.threw() && !r.ok && r.shown.empty()) { r.shown = "(the file)"; }
            if (!d.exportForPreset) {
                if (r.ok) {
                    bookmarkImportNote_ = cc::formatText(tr("Exported %zu to %s"), r.count, r.shown.c_str());
                } else {
                    bookmarkImportNote_ = cc::formatText(tr("Could not write %s"), r.shown.c_str());
                }
            }
            exportLast_.ok = r.ok;
            exportLast_.preset = d.exportForPreset;
            exportLast_.count = r.count;
            exportLast_.shown = r.shown;
            ++exportLast_.seq;
        }
    }

    // THE RECORDINGS LIST: the list and the cache the worker grew replace the ones the engine held. The cache is a copy that went out and came
    // back, so the engine never shared it with a thread; a request made while the scan was out is run now.
    {
        RecordingScan r;
        if (d.recordingScan.poll(r)) {
            const std::size_t opened = r.cache.opens - patchRecordingCache_.opens;
            patchRecordings_ = std::move(r.list);
            patchRecordingCache_ = std::move(r.cache);
            cc::diagLogf("patch: %zu I/Q recording(s) listed (%zu file(s) read)", patchRecordings_.size(), opened);
            if (d.listAgain) {
                d.listAgain = false;
                patchListRecordings();
            }
        }
    }
}

}  // namespace cascade::engine
