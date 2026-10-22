// config_lock.hpp - one program at a time may CHANGE a settings file (engine extraction stage 7c).
//
// WHY. The headless engine (foxsdr-engine) and the window application (cascade) share ONE settings
// file by design (survey decision O3: a desktop user who "runs headless" gets the settings they set
// on the desktop). Two programs saving one file lose each other's changes: each saves what it holds,
// the last writer wins, and a window that was open beside a service would put the service's snapshot
// of the window's own fields back over what the window had just changed. So a program that is going
// to read and later rewrite a settings file takes a LOCK first, and a second program that finds it
// taken says so instead of running beside it.
//
// THE LOCK IS THE OPERATING SYSTEM'S, NOT A FILE'S EXISTENCE. A "lock file" whose mere presence
// meant "taken" would be stale after every crash and every kill - the next start would refuse for ever
// - so the file beside the settings ("<settings file>.lock") is only the thing the lock is TAKEN ON:
//   Windows  the file is opened with no write sharing (CreateFileW, FILE_SHARE_READ): a second open
//            for writing fails with a sharing violation for as long as the first handle lives;
//   POSIX    flock(LOCK_EX | LOCK_NB) on a descriptor that is close-on-exec (a child this process
//            starts - the enumeration helper, the sentinel - must not inherit it and outlive it).
// Both are released by the system when the process ends however it ends (a kill included), so
// there is no stale lock to clean up and nothing to time out. The file itself is left in place:
// it is a few bytes, and removing it would race with the next taker.
//
// WHO HOLDS IT is written into the file ("role=<name> pid=<n>") by the holder, and read back by a
// program that finds the lock taken, so its refusal can say WHICH program is in the way. The read is
// best effort: the sentence is better with it and correct without it.
//
// WHAT IT IS NOT. It is not a single-instance guard for the window application (a second window has
// always been allowed to start - core/tester_link.hpp, claimPrimaryInstance); the host takes this lock
// for the engine's sake and carries on when it cannot (src/main.cpp). It does not protect the other
// files beside the settings (bookmarks, markers): those are per-program-lifetime lists the engine
// loads too, and sharing them is the same decision as sharing the settings.
//
// POSIX half: written on a machine that cannot run it (stage 7c); UNVERIFIED until the Linux jobs do.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace cascade::core {

class ConfigLock {
public:
    enum class State {
        None,         // nothing taken yet (or released)
        Held,         // this object holds the lock
        Contended,    // another process holds it: holder() says which, when it could be read
        Unavailable,  // the lock could not be asked for at all (a folder that takes no file): error() says why
    };

    // Who wrote the lock file last, as the holder wrote it; empty fields when it could not be read.
    struct Holder {
        std::string role;           // "cascade" (the window application) or "foxsdr-engine"
        unsigned long pid = 0;
    };

    ConfigLock() = default;
    ~ConfigLock() { release(); }
    ConfigLock(const ConfigLock&) = delete;
    ConfigLock& operator=(const ConfigLock&) = delete;

    // Take the lock for the settings file at `configPath`, as `role`. Creates the settings folder if it
    // is not there (the first save would). Idempotent while held. Never throws.
    State acquire(const std::string& configPath, const std::string& role);

    // Let go (the process ending does it too).
    void release();

    State state() const { return state_; }
    const Holder& holder() const { return holder_; }      // meaningful when Contended
    const std::string& error() const { return error_; }   // meaningful when Unavailable
    const std::string& lockPath() const { return lockPath_; }

    // The file the lock is taken on: the settings file's absolute, lexically normal path plus ".lock".
    // Both programs derive it from the path they were given, so two spellings of one file name one lock
    // (an existing file is resolved through links and case; a file that does not exist yet keeps the
    // spelling of its name).
    static std::string lockPathFor(const std::string& configPath);

private:
    State state_ = State::None;
    Holder holder_;
    std::string error_;
    std::string lockPath_;
#if defined(_WIN32)
    void* handle_ = nullptr;  // HANDLE, kept out of this header so it needs no windows.h
#else
    int fd_ = -1;
#endif
};

}  // namespace cascade::core
