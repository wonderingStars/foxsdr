// app_window_disk_state.hpp - the state of the AppWindow's own disk workers (0.99.64; re-expressed by stub campaign batch B19).
//
// docs/DIAGNOSTICS.md, "The window does no disk work", is the audit this belongs to. Each worker is a gui::DiskJob (core/disk_job.hpp); this
// header names what each hands back and gathers the jobs in one struct that AppWindow owns through a shared_ptr it cannot see into.
//
// WHAT IS HERE AND WHAT IS NOT. Master's DiskWorkState held eight jobs. Since the engine split most of that work is the ENGINE's, because the
// engine owns what the work touches: the I/Q file's open, the frequency list's import and export and the patch Radio's recordings list are
// workers in src/engine/engine_disk_state.hpp, and the recorder's finish, the bookmark list's save and the settings' write were moved in the
// 0.99.84 merge (C5). What stays the WINDOW's is what only the window has: a plugin's picture, whose pixels the window holds, saved as a BMP
// (imageSave), and the screenshot, whose pixels are the GL framebuffer and whose window list is ImGui's (shotWrite). The Airband section's import
// (the file read and put in a preset: app_window_airband.cpp) and the import window's lookup of a path with no root (app_window_import_dialog.cpp)
// are window workers too, each with its state beside its code.
//
// WHY AN OPAQUE POINTER AND NOT MEMBERS. app_window.cpp is at the edge of what an MSVC object file can hold (error C1128, "number of sections
// exceeded object file format limit"); each DiskJob<T> member instantiates its future, its function wrapper and its destructor into the file that
// names it. So AppWindow holds a DiskWorkState it cannot see into, and every call that touches a job is a member function defined in
// app_window_disk_work.cpp, which is the only translation unit that instantiates them. A test that needs to look inside includes this header.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_APP_WINDOW_DISK_STATE_HPP
#define CASCADE_GUI_APP_WINDOW_DISK_STATE_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "core/host_image.hpp"
#include "core/disk_job.hpp"

namespace cascade::gui {

using cascade::core::DiskJob;

// A picture saved, or why not: the plugin picture's BMP.
struct FileOutcome {
    bool ok = false;
    std::string text;    // the path when `ok`, the reason when not
    std::string plugin;  // whose picture it was
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

struct DiskWorkState {
    DiskJob<FileOutcome> imageSave{"picture save"};
    DiskJob<ShotOutcome> shotWrite{"screenshot save"};
    // The screenshot being put together by the frame that took it.
    std::vector<ShotFile> shotBatch;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_APP_WINDOW_DISK_STATE_HPP
