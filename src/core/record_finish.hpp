// record_finish.hpp - closing a recording's file without making anyone wait for
// the disk (0.99.65).
//
// WHAT THIS IS FOR. 0.99.63 took the START of a recording off the GUI thread
// (gui/record_start.hpp): the open, which creates a folder and a file and writes
// a header, waits for the disk. The FINISH waits for it too, and was left where it
// was: Recorder::stop() flushes the tail of the take, seeks back and patches the
// header's two size fields, and closes the file - on the thread that draws the
// window, on the Stop key, on a source change, at quit and (for a patch speaker)
// when the set that holds its file is retired. docs/DIAGNOSTICS.md, "The window
// does no disk work", listed it as STILL OPEN; this closes it.
//
// THE SHAPE. Recorder::stopForFinish() moves the file, its stdio buffer and the
// byte count off the recorder without touching the disk; submit() hands that to a
// worker of its own and returns at once with a Ticket; the worker runs
// Recorder::finishFile (flush, patch, close) and marks the ticket done. Nothing
// here waits, and the worker owns everything it touches by value, so it can be
// abandoned at quit like every other worker in this tree.
//
// WHAT THE CALLER GETS TO KNOW. A ticket says whether the finish is still out
// (done()) and how it went (ok()): false means the header patch did not reach the
// file, which leaves the zero-length header the opener flushed - a husk a
// chunk-walking reader parses, the same file a crash leaves. The synchronous code
// said nothing about that; neither does this. (docs/DIAGNOSTICS.md lists it.)
//
// QUIT. drain(deadline) waits, bounded, for every finish still out. AppWindow
// calls it once, in the teardown, against the SAME deadline the config's and the
// lists' last saves share, so the shutdown budget is unchanged
// (tests/test_shutdown_budget.cpp). A finish that does not land by then is left to
// finish by itself or die with the process: its file keeps the husk header, which
// is what the synchronous code would have left had the process been killed in the
// same call - and what it would have waited for forever.
//
// ONE PROCESS-WIDE QUEUE, deliberately: a patch speaker's destructor has no
// window to ask, and the quit drain has to find every finish whoever started it.
// The state is leaked on purpose (a worker may still be finishing when the process
// ends, and nothing here is worth a static-destruction race).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RECORD_FINISH_HPP
#define CASCADE_CORE_RECORD_FINISH_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>

#include "core/recorder.hpp"

namespace cascade::core {

class RecordFinisher {
public:
    // One finish's state, shared between the worker and whoever asked.
    struct Ticket {
        std::atomic<bool> done{false};
        std::atomic<bool> ok{true};
    };

    // Hands `req` to a worker and returns at once. NEVER BLOCKS. The ticket is
    // done when the file is closed; an empty request (no file) is done already.
    static std::shared_ptr<Ticket> submit(Recorder::FinishRequest&& req);

    // Waits until every finish submitted so far has completed, or `deadline`.
    // True when none is left. A zero or past deadline still collects what has
    // already finished.
    static bool drain(std::chrono::steady_clock::time_point deadline);

    // How many finishes are out.
    static std::size_t inFlight();

    // A FINISH THAT IS NOT A Recorder's: a worker that is closing a file of its own after
    // its owner went (the patch speaker's MP3 encoder, core/patch_audio.cpp). It brackets
    // its work with these so that drain() gives it the same bounded chance at quit. Every
    // externalBegin() must be paired with one externalEnd(), on any thread.
    static void externalBegin();
    static void externalEnd();
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_RECORD_FINISH_HPP
