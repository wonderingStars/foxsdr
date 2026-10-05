// failing_disk.hpp - a take's disk that fails, on each platform the way that platform can really fail it.
//
// There is no unprivileged way to fill a volume or pull a drive under a test, so a take's file is made
// to refuse writes in the way the platform allows:
//
//   WINDOWS  the OS handle behind the stream is closed (CloseHandle on the handle the CRT holds): from
//            then on every write, seek and the close itself fail - what a full disk, a removed device
//            and a vanished share look like above stdio. The take's file keeps the 44-byte header the
//            opener flushed.
//   LINUX    the take's stream is /dev/full from the start: a REAL ENOSPC on every write that reaches
//            the operating system. The stdio buffer takes the first 256 KiB exactly as it does on
//            Windows before the handle is pulled, so the failure is seen at the same point; the file
//            the take NAMES is the real one the opener created and flushed its header into, and stays
//            that 44-byte header. Nothing is closed behind the stream: on POSIX a closed descriptor
//            number is handed to the next open in the process, so writes through a stale stream could
//            land in another file - that technique must never run there.
//   ELSEWHERE not reached (available() is false and a test reports NOT REACHED).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_TESTS_FAILING_DISK_HPP
#define CASCADE_TESTS_FAILING_DISK_HPP

#include <atomic>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#endif

#include "core/recorder.hpp"

namespace failing_disk {

// Whether a take's disk can be made to fail on this platform.
inline bool available() {
#if defined(_WIN32)
    return true;
#elif defined(__linux__)
    std::FILE* f = std::fopen("/dev/full", "wb");
    if (f == nullptr) { return false; }
    std::fclose(f);
    return true;
#else
    return false;
#endif
}

// The production opener, then - on Linux - the take's stream replaced by /dev/full (the real file keeps
// the header the opener flushed). `capture`, when given, receives the stream the take will write to,
// for pull() on Windows.
inline bool open(const cascade::core::Recorder::OpenRequest& req, cascade::core::Recorder::OpenedFile& out,
                 std::string& err, std::atomic<std::FILE*>* capture = nullptr) {
    if (!cascade::core::Recorder::openFile(req, out, err)) { return false; }
#if defined(__linux__)
    out.file.reset();  // closes the real file: its header is already on disk, nothing is buffered
    std::FILE* full = std::fopen("/dev/full", "wb");
    if (full == nullptr) {
        err = "recorder: cannot create \"/dev/full\"";
        return false;
    }
    // The buffer the take is budgeted on, so the failure is seen after the same 256 KiB.
    std::setvbuf(full, out.buffer.data(), _IOFBF, out.buffer.size());
    out.file.reset(full);
#endif
    if (capture != nullptr) { capture->store(out.file.get()); }
    return true;
}

// Windows: the handle behind `f` closed, so every later write, seek and close fails. A no-op elsewhere
// (Linux's failure is in place from the start; see above).
inline void pull(std::FILE* f) {
#if defined(_WIN32)
    if (f != nullptr) { ::CloseHandle(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f)))); }
#else
    (void)f;
#endif
}

}  // namespace failing_disk

#endif  // CASCADE_TESTS_FAILING_DISK_HPP
