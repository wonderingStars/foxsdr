// app_window_disk_state.hpp - the state of the AppWindow's disk workers (0.99.64).
//
// docs/DIAGNOSTICS.md, "The window does no disk work", is the audit this belongs
// to. Each worker is a gui::DiskJob (gui/disk_job.hpp); this header names what
// each hands back and gathers the jobs in one struct that AppWindow owns through
// an opaque pointer.
//
// WHY AN OPAQUE POINTER AND NOT FIVE MEMBERS. app_window.cpp is at the edge of
// what an MSVC object file can hold (error C1128, "number of sections exceeded
// object file format limit"); five DiskJob<T> members, each instantiating its
// future, its function wrapper and its destructor into that file, took it over.
// So AppWindow holds a DiskWorkState it cannot see into, and every call that
// touches a job is a member function defined in app_window_disk_work.cpp, which
// is the only translation unit that instantiates them. A test that needs to look
// inside includes this header.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_APP_WINDOW_DISK_STATE_HPP
#define CASCADE_GUI_APP_WINDOW_DISK_STATE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/freq_import.hpp"
#include "core/host_image.hpp"
#include "core/patch_recordings.hpp"
#include "gui/disk_job.hpp"
#include "source/iq_file_source.hpp"

namespace cascade::gui {

// The I/Q file Open: the opened source or the reason it would not open, stamped
// with the source generation it was asked at so a result that arrives after the
// user chose another source is dropped.
struct IqOpenResult {
    std::unique_ptr<cascade::source::IqFileSource> file;  // null on failure
    std::string error;  // the source's own sentence, when it failed
    std::string path;   // as typed when Open was pressed
    std::uint64_t gen = 0;
};

// A file saved, or why not: the plugin picture's BMP and the SDR# export.
struct FileOutcome {
    bool ok = false;
    std::string text;       // the path when `ok`, the reason when not
    std::string plugin;     // whose picture it was (the image save)
    std::size_t count = 0;  // how many entries (the export)
};

// One file of a screenshot: the picture, or - `isText` - the window list beside it.
struct ShotFile {
    std::string path;
    cascade::core::HostImage image;  // a BMP of this, unless `isText`
    std::string text;                // the file's bytes, when `isText`
    bool isText = false;
};
struct ShotOutcome {
    int written = 0;  // pictures written (each said in the log by the worker)
    int failed = 0;   // pictures that could not be (each said in the log, too)
};

// The patch Radio's recordings list, and the probe cache the worker grew.
struct RecordingScan {
    std::vector<cascade::core::patch::RecordingInfo> list;
    cascade::core::patch::RecordingProbeCache cache;
};

// A frequency list read: what the file held, or why it could not be read. The path is
// as it was typed less any quotes, so the failure line can leave it out of the log.
struct ImportOutcome {
    cascade::core::ImportResult result;
    std::string path;
    double ms = 0.0;  // how long the worker took, for the log line
};

struct DiskWorkState {
    DiskJob<IqOpenResult> iqOpen{"I/Q file open"};
    DiskJob<FileOutcome> imageSave{"picture save"};
    DiskJob<FileOutcome> bookmarkExport{"frequency list export"};
    // WHO ASKED FOR THE EXPORT NOW OUT (0.99.66): true = the AIRBAND section's Export CSV of
    // a preset, whose note is said in that section; false = the Bookmarks section's export for
    // SDR#. Set on the window's thread when the export starts and read when it comes back, as
    // importInto is, and not carried in the worker's result: a worker that threw comes back
    // default-constructed and must still say which section asked. One export is ever out.
    bool exportForPreset = false;
    // The Import button and a file dropped on the window. An import asked for while
    // one is out is remembered here (the last one wins) and run when it comes back.
    DiskJob<ImportOutcome> bookmarkImport{"frequency list import"};
    std::string importAgain;
    // WHERE THE ROWS GO (0.99.66): the preset the AIRBAND section's Import was pressed for,
    // for the read now out (importInto) and for the one remembered (importAgainInto). Empty
    // group = as the file says (the Bookmarks section, a dropped file). Kept here, on the
    // window's thread, and not in the worker's result: a read that threw comes back
    // default-constructed and must still say which section asked. What pollDiskJobs applies
    // is what was asked for, not what the preset field says by the time the file is read.
    cascade::core::ImportInto importInto;
    cascade::core::ImportInto importAgainInto;
    DiskJob<ShotOutcome> shotWrite{"screenshot save"};
    DiskJob<RecordingScan> recordingScan{"recordings list"};
    // A listing asked for while one was out is run when it comes back.
    bool listAgain = false;
    // The screenshot being put together by the frame that took it.
    std::vector<ShotFile> shotBatch;

    // THE SETTINGS SAVE'S BACK-OFF (0.99.65, AppWindow::noteConfigWrite / configRetryHeld).
    int configFailures = 0;            // writes that failed in a row
    bool configFailureFresh = false;   // one has failed and has not yet been given the clock
    double configRetryAtS = 0.0;       // the earliest the debounce may ask again
    bool configFailureLogged = false;  // this run of failures has been said in the log
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_APP_WINDOW_DISK_STATE_HPP
