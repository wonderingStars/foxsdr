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

struct DiskWorkState {
    DiskJob<IqOpenResult> iqOpen{"I/Q file open"};
    DiskJob<FileOutcome> imageSave{"picture save"};
    DiskJob<FileOutcome> bookmarkExport{"frequency list export"};
    DiskJob<ShotOutcome> shotWrite{"screenshot save"};
    DiskJob<RecordingScan> recordingScan{"recordings list"};
    // A listing asked for while one was out is run when it comes back.
    bool listAgain = false;
    // The screenshot being put together by the frame that took it.
    std::vector<ShotFile> shotBatch;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_APP_WINDOW_DISK_STATE_HPP
