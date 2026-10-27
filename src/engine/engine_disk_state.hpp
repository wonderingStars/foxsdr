// engine_disk_state.hpp - the state of the ENGINE's disk workers (0.99.64 / 0.99.65; re-expressed by stub campaign batch B19).
//
// docs/DIAGNOSTICS.md, "The window does no disk work", is the audit this belongs to. In master the thread that drew the window did the disk's
// calls and the audit moved each one to a gui::DiskJob worker. Since the engine split the thread that DOES what the window asks is the
// engine's pass - and in the Host kind it is the thread the front end waits on, in the headless kind it is the only thread that answers a
// browser - so a file-system call on the pass is the same defect under another name. The engine therefore owns the workers its own work
// needs, in the house pattern of core/disk_job.hpp (one in flight, the worker owns what it touches by value, a throw contained, abandoned at
// quit rather than joined, collected by a once-a-pass poll that never blocks):
//
//   iqOpen          the Source section's I/Q-file Open (SELECT_SOURCE "file:<path>"): the header is read on a worker;
//   bookmarkExport  the frequency list exported (FOXAPP_OP_BOOKMARK_EXPORT): SDR#'s XML for a list of ids, or one preset's CSV;
//   bookmarkImport  the frequency list imported (FOXAPP_OP_BOOKMARK_IMPORT_FILE): the file is read and parsed, the rows join the list on the pass;
//   recordingScan   the patch Radio's list of I/Q recordings (FOXAPP_OP_PATCH_LIST_RECORDINGS).
//
// A patch Radio on an I/Q recording opens through the patch radios' own open worker (patchRadioPending_, engine_patch_radios.cpp), as master's
// patchReconcile did since 0.99.65; the bookmark list and the markers are saved by core::BackgroundSaver and a take's file by
// engine/record_start.hpp and core::RecordFinisher, as they have been since the 0.99.84 merge.
//
// WHY AN OPAQUE POINTER AND NOT MEMBERS (the window's reason, src/gui/app_window_disk_state.hpp): every DiskJob<T>
// instantiates its future, its function wrapper and its destructor into the translation unit that names the member, and engine.hpp is
// included by most of the product. The Engine holds an EngineDiskWork it cannot see into, and every call that touches a job is a member
// function defined in engine_disk_work.cpp, the one translation unit that instantiates them. A test that needs to look inside includes this
// header.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_ENGINE_DISK_STATE_HPP
#define CASCADE_ENGINE_ENGINE_DISK_STATE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/freq_import.hpp"
#include "core/patch_recordings.hpp"
#include "core/disk_job.hpp"
#include "source/iq_file_source.hpp"

namespace cascade::engine {

// The I/Q file Open: the opened source or the reason it would not open, stamped with the source generation (and the combo's row) it was asked at
// so a result that arrives after the choice moved on is dropped.
struct IqOpenResult {
    std::unique_ptr<cascade::source::IqFileSource> file;  // null on failure
    std::string error;                                    // the source's own sentence, when it failed
    std::string path;                                     // as it was asked for
    std::uint64_t gen = 0;
    int row = 0;
};

// A frequency list written, or why not.
struct ExportOutcome {
    bool ok = false;
    std::string shown;      // the path as the note names it: the folder, then the name the file took (a second export in one second is "-2")
    std::size_t count = 0;  // how many rows
};

// A frequency list read: what the file held, or why it could not be read. The path is as the op gave it (the quotes of "Copy as path" taken off), so the
// sentence that says the file "could not be read" can name it even for a worker that threw and came back with nothing.
struct ImportOutcome {
    cascade::core::ImportResult result;
    std::string path;
    double ms = 0.0;  // how long the worker took, for the log line
};

// The patch Radio's recordings list, and the probe cache the worker grew.
struct RecordingScan {
    std::vector<cascade::core::patch::RecordingInfo> list;
    cascade::core::patch::RecordingProbeCache cache;
};

struct EngineDiskWork {
    cascade::core::DiskJob<IqOpenResult> iqOpen{"I/Q file open"};
    cascade::core::DiskJob<ExportOutcome> bookmarkExport{"frequency list export"};
    // WHO ASKED FOR THE EXPORT NOW OUT (master 0.99.66): true = one PRESET's CSV (the AIRBAND section's Export CSV), whose result the window says in that
    // section; false = the Bookmarks section's export for SDR#, whose result is the engine's bookmark note. Set on the pass when the export starts and
    // read when it comes back, and not carried in the worker's result: a worker that threw comes back default-constructed and must still say which
    // section asked. One export is ever out.
    bool exportForPreset = false;
    // FOXAPP_OP_BOOKMARK_IMPORT_FILE: the file is read and parsed on a worker, the rows join the list on the pass that collects it. An import asked for
    // while one is out is remembered (the LAST one asked for wins) and run when it comes back, so a list dropped on the window is not lost.
    cascade::core::DiskJob<ImportOutcome> bookmarkImport{"frequency list import"};
    std::string importAgain;
    bool importAgainSet = false;
    // The path the read now out was asked for, kept on the pass for the same reason as the export's destination: a read that threw comes back with no
    // path, and the sentence that says the file "could not be read" must still name it.
    std::string importPath;
    cascade::core::DiskJob<RecordingScan> recordingScan{"recordings list"};
    // A listing asked for while one was out is run when it comes back.
    bool listAgain = false;
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_ENGINE_DISK_STATE_HPP
