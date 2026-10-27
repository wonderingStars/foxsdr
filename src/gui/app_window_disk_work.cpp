// app_window_disk_work.cpp - THE AppWindow'S OWN DISK WORK, on workers, so that no frame waits for a disk (master 0.99.64; stub campaign batch B19).
//
// WHAT THIS IS. docs/DIAGNOSTICS.md, "The window does no disk work", is the audit of what the 0.99.63 Record fix left on the thread that draws the
// window. Master's app_window_disk_work.cpp (933 lines) held eleven answers to it. THIS FILE IS NOT THAT FILE, and does not port it wholesale: since
// the engine split, most of what that file moved off the thread belongs to the thread that does the work, and is the ENGINE's
// (src/engine/engine_disk_work.cpp, engine_disk_state.hpp):
//
//   master's openIqFile / pollIqOpen / installOpenedIqFile      the engine's: SELECT_SOURCE "file:<path>" opens the header on a worker;
//   master's importBookmarkFile / the import's poll              the engine's: FOXAPP_OP_BOOKMARK_IMPORT_FILE reads the file on a worker, and the
//                                                                window's own half is the lookup of a path with no root (app_window_import_dialog.cpp)
//                                                                and the Airband preset's read (app_window_airband.cpp, which carries the preset's name,
//                                                                its tick and the file's favourites to the engine in BOOKMARK_ADD_ROWS);
//   master's exportBookmarksForSdrSharp / exportGroupCsv         the engine's: FOXAPP_OP_BOOKMARK_EXPORT writes the file on a worker, from the store (so
//                                                                a name goes out whole), and the window sends the op and words the answer;
//   master's patchListRecordings                                 the engine's: FOXAPP_OP_PATCH_LIST_RECORDINGS lists on a worker;
//   master's flushBookmarkSave / flushMarkerSave / drainListSaves, finishTake / pollRecordFinishes / drainRecordFinishes, setAsideDamagedFile,
//   noteConfigWrite / configRetryHeld                            the engine's since the 0.99.84 merge (C5): the lists, the takes and the settings are
//                                                                its files.
//
// WHAT IS LEFT, because only the window has what the work needs:
//
//   saveImageBmp   a plugin's picture saved as a BMP: the pixels are the window's copy (pluginImages_), the folder is the engine's (fact.recorder.dir).
//                  The picture's name, the note and the window the note belongs to are the GUI thread's; the folder, the unique name and the file are a
//                  worker's.
//   shotAddPicture / shotAddText / shotFlush   F12's screenshot: the GL read-back and ImGui's window list are the GUI thread's and are done in the frame
//                  that took the picture; the files are handed to a worker as finished bytes. One write at a time; a press made while one is out is
//                  skipped, and said in the log.
//   pollDiskJobs   once a frame: the picture's note, and the screenshot's job collected so the next one may start.
//
// The slow-disk seam, diskHookForTest_, is called by each worker immediately before it touches the disk. tests/test_gui_disk_audit.cpp scans this file
// with the rest of app_window*.cpp for any file-system call on the GUI thread; the calls below are inside the lambdas handed to request(), which is
// what it recognises as a worker's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/image_write.hpp"
#include "core/unique_file.hpp"
#include "core/utf8_text.hpp"
#include "gui/app_window_disk_state.hpp"
#include "core/disk_job.hpp"

namespace cascade::gui {

using cascade::i18n::tr;

// The state is made on first use, here, so that the instantiations of DiskJob stay in this file (see app_window_disk_state.hpp).
DiskWorkState& AppWindow::diskWork() {
    if (!diskWork_) { diskWork_ = std::make_shared<DiskWorkState>(); }
    return *diskWork_;
}

bool AppWindow::bookmarkExportPending() { return uiFact("fact.bookmarks.exporting") == "1"; }

bool AppWindow::iqOpenPending() { return uiFact("fact.source.iqOpening") == "1"; }

// How long the I/Q file's header has been being read, for the line that says the window is waiting on the disk and not frozen. The engine's worker is
// another thread's, so the clock is this window's: from the frame it first saw the fact.
double AppWindow::iqOpenElapsedS() {
    if (!iqOpenPending()) {
        iqOpenSinceS_ = -1.0;
        return 0.0;
    }
    const double now = ImGui::GetTime();
    if (iqOpenSinceS_ < 0.0) { iqOpenSinceS_ = now; }
    return now - iqOpenSinceS_;
}

bool AppWindow::imageSavePending() const { return diskWork_ != nullptr && diskWork_->imageSave.pending(); }

void AppWindow::saveImageBmp(const cascade::core::HostImage& im) {
    // THE WORKER'S ANSWER WILL SAY WHICH PICTURE IT WAS, so the note is drawn in that picture's window only (see pollDiskJobs). One save at a
    // time: a second press while the first is out is ignored - the key is disabled, and the note already says "Saving...".
    DiskWorkState& d = diskWork();
    if (d.imageSave.pending()) { return; }
    // Named by plugin, sequence and wall-clock time, next to the recordings rather than in the install directory. The recorder's folder is read
    // AT THE PRESS, from the session's facts (fact.recorder.dir), not cached.
    std::string recordDir;
    readFactText("fact.recorder.dir", recordDir);
    const std::filesystem::path dir = std::filesystem::path(recordDir.empty() ? "." : recordDir);
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
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) { c = '_'; }
    }
    const std::filesystem::path out = dir / (safe + "-" + stamp + ".bmp");
    // THE DISK IS NOT TOUCHED HERE. This created the recordings folder and wrote the BMP on the thread that draws the window (0.99.64). The worker
    // owns a COPY of the picture and the two paths.
    const auto hook = diskHookForTest_;
    d.imageSave.request([im, dir = dir.string(), out = out.string(), hook]() -> FileOutcome {
        if (hook) { hook(); }
        FileOutcome r;
        r.plugin = im.plugin;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        std::string err;
        // THE NAME IS TAKEN ATOMICALLY HERE (core/unique_file.hpp): two saves inside one second are two files ("...-101010.bmp",
        // "...-101010-2.bmp"), never the second over the first, and the note says the name that is on disk. A picture that fails to write is removed
        // by writeBmp24 - the reserved file with it.
        std::string used;
        bool exhausted = false;
        if (!cascade::core::reserveUnique(out, used, exhausted)) {
            r.text = "cannot open \"" + out + "\" for writing";
        } else if (cascade::core::writeBmp24(im, used, err)) {
            r.ok = true;
            r.text = used;
        } else {
            r.text = err;
        }
        return r;
    });
    imageSaveNote_ = tr("Saving...");
    imageSaveNotePlugin_ = im.plugin;
    imageSaveNoteAtS_ = ImGui::GetTime();
}

void AppWindow::shotAddPicture(std::string path, cascade::core::HostImage image) {
    ShotFile f;
    f.path = std::move(path);
    f.image = std::move(image);
    diskWork().shotBatch.push_back(std::move(f));
}

void AppWindow::shotAddText(std::string path, std::string text) {
    ShotFile f;
    f.path = std::move(path);
    f.text = std::move(text);
    f.isText = true;
    diskWork().shotBatch.push_back(std::move(f));
}

void AppWindow::shotFlush() {
    DiskWorkState& d = diskWork();
    std::vector<ShotFile> files;
    files.swap(d.shotBatch);
    // ONE WRITE AT A TIME, and a picture is not worth holding a frame for: a press made while the last is still being written is skipped, and said.
    if (d.shotWrite.pending()) {
        cascade::core::diagLogf("shot: the previous screenshot is still being written; this one was skipped");
        return;
    }
    const auto hook = diskHookForTest_;
    d.shotWrite.request([files = std::move(files), hook]() -> ShotOutcome {
        if (hook) { hook(); }
        ShotOutcome r;
        for (const ShotFile& f : files) {
            if (f.isText) {
                // The window list beside a picture: best effort, as it always was (nothing is said when it cannot be written).
                if (std::FILE* rf = std::fopen(f.path.c_str(), "wb")) {
                    std::fwrite(f.text.data(), 1, f.text.size(), rf);
                    std::fclose(rf);
                }
                continue;
            }
            std::string err;
            cascade::core::BmpFailure why = cascade::core::BmpFailure::None;
            // SAID BY THE WORKER, in the words the frame loop used to say it, so that a screenshot taken on the LAST frame of a bounded run - after
            // which no frame polls - still logs the path a harness looks for.
            if (cascade::core::writeBmp24(f.image, f.path, err, &why)) {
                cascade::core::diagLogf("shot: wrote %s", f.path.c_str());
                ++r.written;
            } else {
                // FROM THE CAUSE, NOT FROM THE WRITER'S TEXT (0.99.65): that text names the file, and a log line never names a path (it goes into
                // every report). The picture that failed is removed by writeBmp24.
                cascade::core::diagWarnf("shot: a screenshot could not be written (%s)", cascade::core::bmpFailureWords(why));
                ++r.failed;
            }
        }
        return r;
    });
}

// ONCE A FRAME (drawUi): a picture saved shows its note, in the window of the picture that was saved; a finished screenshot frees the next.
void AppWindow::pollDiskJobs() {
    if (!diskWork_) { return; }
    DiskWorkState& d = *diskWork_;

    // A PICTURE SAVED: the note, in the words it always had, in the window of the picture that was saved - and its clock starts when the answer
    // arrives.
    {
        FileOutcome r;
        if (d.imageSave.poll(r)) {
            // One format string per sentence, so a translation can place the path or the reason where its language puts it.
            std::string note;
            if (r.ok) {
                cascade::core::formatUtf8(note, tr("Saved %s"), r.text.c_str());
            } else {
                cascade::core::formatUtf8(note, tr("Save failed: %s"), r.text.c_str());
            }
            imageSaveNote_ = note;
            // WHOSE WINDOW SAID IT, AND WHEN. Without these two the note was drawn inside EVERY image window by the loop below, so saving the APT
            // picture put its filename under the SSTV one as well.
            imageSaveNotePlugin_ = r.plugin;
            imageSaveNoteAtS_ = ImGui::GetTime();
        }
    }

    // THE SCREENSHOT'S FILES are said in the log by the worker itself (shotFlush); all that is collected here is the end of the job, so the next one
    // may start.
    {
        ShotOutcome r;
        (void)d.shotWrite.poll(r);
    }
}

}  // namespace cascade::gui
