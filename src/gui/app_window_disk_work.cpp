// app_window_disk_work.cpp - the AppWindow members that hand disk work to a
// worker, so that no frame waits for a disk (0.99.64).
//
// WHAT THIS IS. docs/DIAGNOSTICS.md, "The window does no disk work", is the
// audit of what the 0.99.63 Record fix left on the thread that draws the window.
// Each member here is one answer to one finding, in the house pattern of
// gui/record_start.hpp: a worker that owns its inputs by value, one in flight, a
// result collected by a once-a-frame poll, a bounded wait at quit, the user told
// in the same words as before.
//
//   openIqFile / pollIqOpen / installOpenedIqFile   the Source section's I/Q-file
//       Open (gui/disk_job.hpp): the header is read on a worker.
//   flushBookmarkSave / flushMarkerSave / drainListSaves   the bookmark list and
//       the waterfall markers (gui/background_saver.hpp): the temp file and the
//       rename are a worker's; the last request lands within a bound at exit.
//   saveImageBmp / exportBookmarksForSdrSharp / writeShots   a picture saved as
//       a BMP, the frequency list exported for SDR#, F12's screenshot: the folder
//       and the file are a worker's (gui/disk_job.hpp).
//   pollDiskJobs   the once-a-frame collection of all of the above and of the
//       recordings list (patchListRecordings, app_window_patch_radios.cpp).
//   importBookmarkFile   (0.99.65) the Bookmarks section's Import and a list dropped
//       on the window: the file is read and parsed on a worker, the list is the GUI
//       thread's and is filled by pollDiskJobs.
//   finishTake / pollRecordFinishes / drainRecordFinishes   (0.99.65) a recording's
//       file is closed on a worker (core/record_finish.hpp): Stop detaches it from
//       the recorder without touching the disk, a Record pressed meanwhile is
//       remembered, and quit drains against the deadline the saves share.
//
// WHY A FILE OF THEIR OWN. They are one subject, and app_window.cpp is at the edge
// of what an MSVC object file can hold (error C1128, "number of sections exceeded
// object file format limit"): the first of these, written in place, took it over.
// tests/test_gui_disk_audit.cpp scans this file with the rest of app_window*.cpp
// for any file-system call on the GUI thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/freq_import.hpp"
#include "core/i18n.hpp"
#include "core/image_write.hpp"
#include "core/utf8_text.hpp"
#include "core/write_fault.hpp"
#include "core/patch_recordings.hpp"
#include "core/record_finish.hpp"
#include "gui/app_window_disk_state.hpp"
#include "gui/background_saver.hpp"
#include "gui/disk_job.hpp"

namespace cascade::gui {

using cascade::i18n::tr;

void AppWindow::openIqFile() {
    // ONE OPEN AT A TIME: a second press while the header is still being read
    // (the key is disabled, but a test and a key binding are not the key) changes
    // nothing.
    if (disk_->iqOpen.pending()) { return; }
    // THE DISK IS NOT TOUCHED HERE. This made an IqFileSource and called open() on
    // it - an ifstream on the path the user typed and the RIFF header walked with
    // reads - on the thread that draws the window, and a recording on a network
    // share or a sleeping drive froze it for as long as the disk took (see
    // gui/disk_job.hpp). The worker owns the path and the factory by value and
    // hands back the opened source; pollIqOpen() installs it on a later frame.
    const std::string path = iqPath_;
    const std::uint64_t gen = sourceGen_;
    const auto factory = iqFileFactory_;
    disk_->iqOpen.request([path, gen, factory]() -> IqOpenResult {
        IqOpenResult r;
        r.path = path;
        r.gen = gen;
        try {
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

void AppWindow::pollIqOpen() {
    IqOpenResult r;
    if (!disk_->iqOpen.poll(r)) { return; }
    if (!r.file && r.error.empty()) { r.error = "cannot open file: " + r.path; }
    // A source that was open while it was being replaced is closed by whoever
    // lets it go, and a close is a file-system call too: not on this thread.
    const auto letGo = [](std::unique_ptr<cascade::source::IqFileSource> f) {
        if (f) { std::thread([g = std::move(f)]() mutable { g.reset(); }).detach(); }
    };
    // THE CHOICE MAY HAVE MOVED ON while the header was being read (the same rule
    // as a sound card's open, app_window_soundcard.cpp): another source installed
    // (sourceGen_ moved), a radio open under way, or the combo moved off the row.
    // The open file is then simply closed again.
    if (r.gen != sourceGen_ || deviceOpenPending_ || sourceSel_ != 1) {
        cascade::core::diagLogf("source: an I/Q file open finished after the choice moved on");
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

void AppWindow::installOpenedIqFile(std::unique_ptr<cascade::source::IqFileSource> file,
                                    const std::string& path) {
    // Carry the displayed frequency over: a file's center is
    // nominal anyway, and a readout that jumps to 0 on source
    // switch would read as a tuning bug. The AIR frequency carries
    // over; the file is told it through its own converter, which
    // is off unless the user set one for I/Q files.
    const double fileRadioHz = radioHzForSource(
        "file", std::string(), pipeline_.activeSource().centerFrequencyHz());
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
    // A file is a deliberate choice of source like any other, so
    // a radio remembered from a failed restore is superseded here
    // too - see selectSource's generator row.
    restoreKeep_ = cascade::gui::RememberedSource{};
    restoreKeepLabel_.clear();
    followInputRate();  // DSP chain + frequency axis track the file's rate
    // The RATE, never the path: a file name is the user's own data
    // and a report is a support artefact, not a listening record.
    cascade::core::diagLogf("source: opened an I/Q file at %.0f S/s",
                            pipeline_.activeSource().sampleRateHz());
}

void AppWindow::flushBookmarkSave(bool force) {
    // THE LAST SAVE'S ANSWER, whenever it has come back (0.99.64: the write is
    // on a worker, so what it says is a frame or more late). A failure is shown
    // in the words the synchronous save gave; a success clears a stale error.
    {
        bool ok = false;
        std::string err;
        if (bookmarkSaver_.poll(ok, err)) {
            if (ok) {
                bookmarkError_.clear();
            } else {
                bookmarkError_ = err;
            }
        }
    }
    if (!bookmarkSaveDirty_ || bookmarkPath_.empty()) { return; }
    if (!force && ImGui::GetCurrentContext() != nullptr && ImGui::GetTime() < bookmarkSaveDueS_) { return; }
    bookmarkSaveDirty_ = false;
    // The snapshot is taken on this thread, so the worker never reads the list
    // the user is editing; the write itself - the folder, the temp file, the
    // flush, the rename - is the worker's. NEVER WAITS FOR THE DISK.
    bookmarkSaver_.request(bookmarkPath_, freqMgr_.serialize());
}

void AppWindow::flushMarkerSave(bool force) {
    // The last save's answer first, as flushBookmarkSave does (0.99.64).
    {
        bool ok = false;
        std::string err;
        if (markerSaver_.poll(ok, err)) {
            if (ok) {
                markerError_.clear();
            } else {
                markerError_ = err;
            }
        }
    }
    if (markerPath_.empty()) { return; }  // hermetic run: never touch disk
    const unsigned v = freqMarkers_.version();
    if (v == markerSavedVersion_) {
        markerSaveDueS_ = -1.0;
        return;
    }
    const bool haveClock = ImGui::GetCurrentContext() != nullptr;
    const double now = haveClock ? ImGui::GetTime() : 0.0;
    if (!force) {
        // Half a second after a change is first seen: a note is typed a
        // character at a time, and costs at most two writes a second rather
        // than one a keystroke.
        if (markerSaveDueS_ < 0.0) {
            markerSaveDueS_ = now + 0.5;
            return;
        }
        if (now < markerSaveDueS_) { return; }
    }
    // The text is taken here; the write is the worker's (see flushBookmarkSave).
    markerSaver_.request(markerPath_, freqMarkers_.serialize());
    // Recorded either way: a save that failed is reported, not retried every
    // frame; the next change tries again.
    markerSavedVersion_ = v;
    markerSaveDueS_ = -1.0;
}

bool AppWindow::drainListSaves(std::chrono::steady_clock::time_point deadline) {
    // What is left of the one deadline, never negative: a write that has already
    // finished is still collected at zero (ConfigWriter::finishOrAbandon).
    const auto left = [&deadline] {
        const auto now = std::chrono::steady_clock::now();
        return now < deadline
                   ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                   : std::chrono::milliseconds(0);
    };
    bool landed = true;
    if (!bookmarkSaver_.finishOrAbandon(left())) {
        cascade::core::diagLogf("bookmarks: final save abandoned - the disk did not answer in time");
        landed = false;
    } else if (!bookmarkSaver_.lastOk() && bookmarkSaver_.completed() > 0) {
        bookmarkError_ = bookmarkSaver_.lastError();
        cascade::core::diagWarnf("bookmarks: the final save failed");
        landed = false;
    }
    if (!markerSaver_.finishOrAbandon(left())) {
        cascade::core::diagLogf("markers: final save abandoned - the disk did not answer in time");
        landed = false;
    } else if (!markerSaver_.lastOk() && markerSaver_.completed() > 0) {
        markerError_ = markerSaver_.lastError();
        cascade::core::diagWarnf("markers: the final save failed");
        landed = false;
    }
    return landed;
}

// --- A TAKE'S FILE IS CLOSED ON A WORKER (0.99.65, core/record_finish.hpp) ----------
//
// Recorder::stop flushed the tail, patched the header and closed the file on the
// thread that draws the window - on Stop, on a source change, on a rate change and
// at quit. Now the GUI thread only DETACHES the file from the recorder
// (Recorder::stopForFinish, no I/O) and hands it to RecordFinisher.

namespace {
// What the worker said, said once, in the log and nowhere else: the synchronous stop
// said nothing at all about a header that did not reach the file (it ignored every
// result), and this keeps the screen the same - a line the user cannot act on, about
// a file that still opens - while no longer losing the fact. Never names the file.
void collectFinish(std::shared_ptr<cascade::core::RecordFinisher::Ticket>& slot, const char* what) {
    if (!slot || !slot->done.load()) { return; }
    if (!slot->ok.load()) {
        cascade::core::diagWarnf(
            "recorder: the %s take's file could not be finalised - it keeps the header it was "
            "opened with",
            what);
    }
    slot.reset();
}
}  // namespace

void AppWindow::finishTake(bool iq) {
    cascade::core::Recorder& rec = iq ? iqRecorder_ : audioRecorder_;
    cascade::core::Recorder::FinishRequest req;
    // False when there is no open file: never started, already stopped, or ended by
    // the size limit (which closed its own file). Nothing to finish, nothing to wait for.
    if (!rec.stopForFinish(req)) { return; }
    (iq ? iqFinish_ : audioFinish_) = cascade::core::RecordFinisher::submit(std::move(req));
}

bool AppWindow::takeFinishing(bool iq) const {
    const auto& slot = iq ? iqFinish_ : audioFinish_;
    return slot && !slot->done.load();
}

void AppWindow::pollRecordFinishes() {
    const auto service = [this](bool iq) {
        collectFinish(iq ? iqFinish_ : audioFinish_, iq ? "I/Q" : "audio");
        bool& queued = iq ? iqStartQueued_ : audioStartQueued_;
        if (queued && !takeFinishing(iq)) {
            queued = false;
            if (iq) {
                startIqRecording();
            } else {
                startAudioRecording();
            }
        }
    };
    service(true);
    service(false);
}

bool AppWindow::drainRecordFinishes(std::chrono::steady_clock::time_point deadline) {
    // A Record queued behind a finish is not started at quit: the window is closing.
    iqStartQueued_ = false;
    audioStartQueued_ = false;
    const bool landed = cascade::core::RecordFinisher::drain(deadline);
    if (!landed) {
        cascade::core::diagLogf(
            "recorder: a take's file was still being closed at exit - the disk did not answer in "
            "time; it keeps the header it was opened with");
    }
    collectFinish(iqFinish_, "I/Q");
    collectFinish(audioFinish_, "audio");
    return landed;
}


// --- THE REST OF THE GUI THREAD'S DISK WORK (items 5-8 of the 0.99.64 audit) -----
//
// One pattern for all four, gui/disk_job.hpp's: the GUI thread does what only it
// can (the GL read-back, the window list, the file's name, the bookmark list's
// text, the picture's pixels - copies, never references into the window) and
// hands the finished inputs to a worker that makes the folder and writes the
// file; the answer comes back through the once-a-frame pollDiskJobs() and is
// said in the words it always was. One worker per feature at a time. The
// slow-disk seam, diskHookForTest_, is called by the worker immediately before it
// touches the disk.

void AppWindow::saveImageBmp(const cascade::core::HostImage& im) {
    // THE WORKER'S ANSWER WILL SAY WHICH PICTURE IT WAS, so the note is drawn in
    // that picture's window only (see below). One save at a time: a second press
    // while the first is out is ignored - the key is disabled, and the note
    // already says "Saving...".
    if (disk_->imageSave.pending()) { return; }
    // Named by plugin, sequence and wall-clock time, next to
    // the recordings rather than in the install directory.
    const std::filesystem::path dir =
        std::filesystem::path(recordDir_.empty() ? "." : recordDir_);
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    std::string safe = im.plugin;
    for (char& c : safe) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) { c = '_'; }
    }
    const std::filesystem::path out = dir / (safe + "-" + stamp + ".bmp");
    // THE DISK IS NOT TOUCHED HERE. This created the recordings folder and wrote
    // the BMP on the thread that draws the window (0.99.64). The worker owns a
    // COPY of the picture and the two paths.
    const auto hook = diskHookForTest_;
    disk_->imageSave.request(
        [im, dir = dir.string(), out = out.string(), hook]() -> FileOutcome {
            if (hook) { hook(); }
            FileOutcome r;
            r.plugin = im.plugin;
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(dir), ec);
            std::string err;
            if (cascade::core::writeBmp24(im, out, err)) {
                r.ok = true;
                r.text = out;
            } else {
                r.text = err;
            }
            return r;
        });
    imageSaveNote_ = tr("Saving...");
    imageSaveNotePlugin_ = im.plugin;
    imageSaveNoteAtS_ = ImGui::GetTime();
}

void AppWindow::exportBookmarksForSdrSharp() {
    if (disk_->bookmarkExport.pending()) { return; }
    std::vector<cascade::core::Bookmark> out;
    out.reserve(bookmarkView_.size());
    for (const std::uint32_t i : bookmarkView_) {
        if (i < freqMgr_.list().size()) { out.push_back(freqMgr_.list()[i]); }
    }
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char name[64];
    std::strftime(name, sizeof(name), "foxsdr-frequencies-%Y%m%d-%H%M%S.xml", &tmv);
    const std::string shown = recordDir_ + "/" + name;
    const std::filesystem::path path = std::filesystem::path(recordDir_) / name;
    // THE TEXT IS MADE HERE (it is a walk of the list); THE DISK IS THE WORKER'S -
    // the recordings folder and the file, as the handler used to make them on the
    // thread that draws the window.
    const std::string xml = cascade::core::exportSdrSharpXml(out);
    const auto hook = diskHookForTest_;
    disk_->bookmarkExport.request([xml, dir = recordDir_, path = path.string(), shown,
                             count = out.size(), hook]() -> FileOutcome {
        if (hook) { hook(); }
        FileOutcome r;
        r.count = count;
        r.text = shown;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(xml.data(), static_cast<std::streamsize>(xml.size()));
        cascade::core::writeFaultPoint("export", f);  // test seam: a write that fails part way
        f.close();
        r.ok = static_cast<bool>(f);
        return r;
    });
    bookmarkImportNote_ = tr("Saving...");
}

void AppWindow::shotAddPicture(std::string path, cascade::core::HostImage image) {
    ShotFile f;
    f.path = std::move(path);
    f.image = std::move(image);
    disk_->shotBatch.push_back(std::move(f));
}

void AppWindow::shotAddText(std::string path, std::string text) {
    ShotFile f;
    f.path = std::move(path);
    f.text = std::move(text);
    f.isText = true;
    disk_->shotBatch.push_back(std::move(f));
}

void AppWindow::shotFlush() {
    std::vector<ShotFile> files;
    files.swap(disk_->shotBatch);
    // ONE WRITE AT A TIME, and a picture is not worth holding a frame for: a
    // press made while the last is still being written is skipped, and said.
    if (disk_->shotWrite.pending()) {
        cascade::core::diagLogf(
            "shot: the previous screenshot is still being written; this one was skipped");
        return;
    }
    const auto hook = diskHookForTest_;
    disk_->shotWrite.request([files = std::move(files), hook]() -> ShotOutcome {
        if (hook) { hook(); }
        ShotOutcome r;
        for (const ShotFile& f : files) {
            if (f.isText) {
                // The window list beside a picture: best effort, as it always was
                // (nothing is said when it cannot be written).
                if (std::FILE* rf = std::fopen(f.path.c_str(), "wb")) {
                    std::fwrite(f.text.data(), 1, f.text.size(), rf);
                    std::fclose(rf);
                }
                continue;
            }
            std::string err;
            // SAID BY THE WORKER, in the words the frame loop used to say it, so
            // that a screenshot taken on the LAST frame of a bounded run - after
            // which no frame polls - still logs the path a harness looks for.
            if (cascade::core::writeBmp24(f.image, f.path, err)) {
                cascade::core::diagLogf("shot: wrote %s", f.path.c_str());
                ++r.written;
            } else {
                cascade::core::diagWarnf("shot: %s", err.c_str());
                ++r.failed;
            }
        }
        return r;
    });
}

void AppWindow::importBookmarkFile(const std::string& path) {
    std::string p = path;
    // A path pasted from Explorer's "Copy as path" arrives quoted.
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') { p = p.substr(1, p.size() - 2); }
    // ONE READ AT A TIME, and a list dropped on the window while one is out is not
    // lost: it is remembered (the last one asked for) and read when the first is
    // back (pollDiskJobs).
    if (disk_->bookmarkImport.pending()) {
        disk_->importAgain = p;
        return;
    }
    // THE DISK IS NOT TOUCHED HERE (0.99.65). The file the user typed or dropped - a
    // path that can be a network share or a sleeping drive - was read, and parsed,
    // on the thread that draws the window. The worker owns the path by value and
    // hands back what the file held; pollDiskJobs() puts it in the list.
    const auto hook = diskHookForTest_;
    disk_->bookmarkImport.request([p, hook]() -> ImportOutcome {
        const auto t0 = std::chrono::steady_clock::now();
        if (hook) { hook(); }
        ImportOutcome o;
        o.path = p;
        o.result = cascade::core::importFrequencyFile(p);
        o.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                   .count();
        return o;
    });
}

void AppWindow::pollDiskJobs() {
    pollIqOpen();

    // A FREQUENCY LIST READ: put in the bookmarks, in the words the synchronous
    // import used. The list is the GUI thread's (the worker never touched it).
    {
        ImportOutcome o;
        if (disk_->bookmarkImport.poll(o)) {
            cascade::core::ImportResult& r = o.result;
            // A worker that threw (an allocation, a corrupt file): the same sentence a
            // file that cannot be read gets, instead of a crash in the frame loop.
            if (disk_->bookmarkImport.threw() && r.error.empty()) {
                r.error = "cannot read the file";
            }
            const std::string& p = o.path;
            if (!r.error.empty() && r.items.empty()) {
                bookmarkImportNote_ =
                    cascade::core::formatText(tr("Could not import: %s"), r.error.c_str());
                // The file's KIND, never its name or path: "never the name or path of
                // a file you opened" (PRIVACY.md), and a frequency list's name is
                // usually what is on it.
                // importFrequencyFile's "cannot open" names the path; the log does not.
                std::string why = r.error;
                for (std::size_t at = why.find(p); !p.empty() && at != std::string::npos;
                     at = why.find(p, at)) {
                    why.replace(at, p.size(), "(the file)");
                }
                cascade::core::diagWarnf("bookmarks: an import (%s) failed: %s",
                                         std::filesystem::path(p).extension().string().c_str(),
                                         why.c_str());
            } else {
                const auto t0 = std::chrono::steady_clock::now();
                const std::size_t found = r.items.size();
                const std::size_t added = freqMgr_.addMany(std::move(r.items));
                const double ms =
                    o.ms + std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
                char note[320];
                cascade::core::formatUtf8(
                    note, sizeof(note), "%s: %zu entries read, %zu added%s%s%s", r.format.c_str(),
                    found, added, found > added ? " (the rest were already here)" : "",
                    r.skipped > 0 ? ", some had no usable frequency" : "",
                    r.shifted > 0 ? ", converter Shift values were not applied" : "");
                bookmarkImportNote_ = note;
                cascade::core::diagLogf(
                    "bookmarks: imported a %s file - %zu read, %zu added, %zu skipped, %zu "
                    "shifted, %.0f ms",
                    r.format.c_str(), found, added, r.skipped, r.shifted, ms);
                if (added > 0) { saveBookmarks(); }
            }
            // A list asked for while this one was being read.
            if (!disk_->importAgain.empty()) {
                std::string next;
                next.swap(disk_->importAgain);
                importBookmarkFile(next);
            }
        }
    }

    // THE RECORDINGS LIST: the list and the cache the worker grew replace the
    // ones the window held. The cache is a copy that went out and came back, so
    // the window never shared it with a thread; a request made while the scan was
    // out is run now (the Look button pressed again, a folder that changed).
    {
        RecordingScan r;
        if (disk_->recordingScan.poll(r)) {
            const std::size_t opened = r.cache.opens - patchRecordingCache_.opens;
            patchRecordings_ = std::move(r.list);
            patchRecordingCache_ = std::move(r.cache);
            cascade::core::diagLogf("patch: %zu I/Q recording(s) listed (%zu file(s) read)",
                                    patchRecordings_.size(), opened);
            if (disk_->listAgain) {
                disk_->listAgain = false;
                patchListRecordings();
            }
        }
    }

    // A PICTURE SAVED: the note, in the words it always had, in the window of the
    // picture that was saved - and its clock starts when the answer arrives.
    {
        FileOutcome r;
        if (disk_->imageSave.poll(r)) {
            // One format string per sentence, so a translation can
            // place the path or the reason where its language puts it.
            std::string note;
            if (r.ok) {
                cascade::core::formatUtf8(note, tr("Saved %s"), r.text.c_str());
            } else {
                cascade::core::formatUtf8(note, tr("Save failed: %s"), r.text.c_str());
            }
            imageSaveNote_ = note;
            // WHOSE WINDOW SAID IT, AND WHEN. Without these two the
            // note was drawn inside EVERY image window by the loop
            // below, so saving the APT picture put its filename under
            // the SSTV one as well.
            imageSaveNotePlugin_ = r.plugin;
            imageSaveNoteAtS_ = ImGui::GetTime();
        }
    }

    // THE LIST EXPORTED.
    {
        FileOutcome r;
        if (disk_->bookmarkExport.poll(r)) {
            std::string said;
            if (r.ok) {
                cascade::core::formatUtf8(said, tr("Exported %zu to %s"), r.count, r.text.c_str());
            } else {
                cascade::core::formatUtf8(said, tr("Could not write %s"), r.text.c_str());
            }
            bookmarkImportNote_ = said;
        }
    }

    // THE SCREENSHOT'S FILES are said in the log by the worker itself (shotFlush);
    // all that is collected here is the end of the job, so the next one may start.
    {
        ShotOutcome r;
        (void)disk_->shotWrite.poll(r);
    }
}

// --- THE OPAQUE STATE (see gui/app_window_disk_state.hpp) -----------------------

DiskWorkState* AppWindow::newDiskWorkState() { return new DiskWorkState(); }

void DiskWorkStateDeleter::operator()(DiskWorkState* p) const { delete p; }

bool AppWindow::iqOpenPending() const { return disk_->iqOpen.pending(); }
bool AppWindow::imageSavePending() const { return disk_->imageSave.pending(); }
bool AppWindow::bookmarkExportPending() const { return disk_->bookmarkExport.pending(); }
bool AppWindow::bookmarkImportPending() const { return disk_->bookmarkImport.pending(); }
double AppWindow::bookmarkImportElapsedS() const { return disk_->bookmarkImport.elapsedS(); }
double AppWindow::iqOpenElapsedS() const { return disk_->iqOpen.elapsedS(); }

void AppWindow::patchListRecordings() {
    // ONE LISTING AT A TIME; a request made while one is out is remembered and
    // made when it comes back (pollDiskJobs), so "Look for radios" pressed after a
    // file was saved is never lost.
    if (disk_->recordingScan.pending()) {
        disk_->listAgain = true;
        return;
    }
    std::vector<std::string> dirs{recordDir_};
    // A second folder for verification and for anyone who keeps recordings
    // elsewhere - read from the environment, never guessed.
    if (const char* s = std::getenv("FOXSDR_PATCH_SAMPLES"); s != nullptr && *s != '\0') {
        dirs.emplace_back(s);
    }
    // THE LISTING IS A WORKER'S (0.99.64). The folders are listed and up to
    // kMaxRecordingOpens recording headers opened - ON THE GUI THREAD, until now,
    // in a folder the patch's own speakers keep writing to and a recording on a
    // network share can be anywhere. The worker owns the folders and a COPY of
    // the probe cache (THROUGH THE CACHE: a file already read is not opened again
    // until it changes) and hands both back; pollDiskJobs() swaps them in.
    const auto hook = diskHookForTest_;
    disk_->recordingScan.request(
        [dirs, cache = patchRecordingCache_, hook]() mutable -> RecordingScan {
            if (hook) { hook(); }
            RecordingScan r;
            r.list = cascade::core::patch::listIqRecordings(
                dirs, cascade::core::patch::kMaxRecordingsListed, &cache);
            r.cache = std::move(cache);
            return r;
        });
}

}  // namespace cascade::gui
