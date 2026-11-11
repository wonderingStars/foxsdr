// linux_update_host.hpp - the real UpdateHost (core/linux_update.hpp).
//
// FsUpdateHost does everything that needs only the C++ file system library and is therefore also
// compiled and exercised on Windows (tests/test_linux_update.cpp runs the swap and the rollback
// against real files in a temp directory). What it cannot do portably (fetch, archives, running a
// program) stays pure virtual.
//
// SystemUpdateHost (non-Windows only) supplies those from POSIX: an https download with
// httplib::SSLClient, `tar` through fork/exec with fixed arguments, a program run with a timeout.
// UNPROVEN ON LINUX: written from the POSIX, tar(1) and cpp-httplib documentation and never run.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_LINUX_UPDATE_HOST_HPP
#define CASCADE_CORE_LINUX_UPDATE_HOST_HPP

#include <memory>
#include <string>
#include <vector>

#include "core/linux_update.hpp"

namespace cascade::core::linuxupdate {

class FsUpdateHost : public UpdateHost {
public:
    bool exists(const std::string& path) override;
    std::int64_t fileSize(const std::string& path) override;
    bool makeDir(const std::string& path) override;
    bool writableDir(const std::string& dir) override;
    std::uint64_t freeSpace(const std::string& dir) override;
    bool createExclusive(const std::string& path) override;
    bool removeAll(const std::string& path) override;
    bool rename(const std::string& from, const std::string& to) override;
    bool writeText(const std::string& path, const std::string& text) override;
    bool readText(const std::string& path, std::string& text) override;
    bool readHead(const std::string& path, std::size_t n, std::string& bytes) override;
    bool sha256(const std::string& path, std::string& hexLower) override;
    bool makeExecutable(const std::string& path) override;
};

// Every operation answers "no" / "nothing". A base for tests (and for the udev installer's tests) that
// override only what they exercise.
class NullUpdateHost : public UpdateHost {
public:
    bool exists(const std::string&) override { return false; }
    std::int64_t fileSize(const std::string&) override { return -1; }
    bool makeDir(const std::string&) override { return false; }
    bool writableDir(const std::string&) override { return false; }
    std::uint64_t freeSpace(const std::string&) override { return 0; }
    bool createExclusive(const std::string&) override { return false; }
    bool removeAll(const std::string&) override { return false; }
    bool rename(const std::string&, const std::string&) override { return false; }
    bool writeText(const std::string&, const std::string&) override { return false; }
    bool readText(const std::string&, std::string&) override { return false; }
    bool readHead(const std::string&, std::size_t, std::string&) override { return false; }
    bool sha256(const std::string&, std::string&) override { return false; }
    bool makeExecutable(const std::string&) override { return false; }
    FetchResult fetch(const std::string&, const std::string&, std::uint64_t, std::uint64_t,
                      std::atomic<bool>*) override { return FetchResult{}; }
    bool listArchive(const std::string&, std::vector<std::string>&) override { return false; }
    bool extractArchive(const std::string&, const std::string&) override { return false; }
    int runProgram(const std::string&, const std::vector<std::string>&, int, std::string&) override { return -1; }
};

// Splits a block of text into lines (\n or \r\n), dropping a trailing empty one. Pure.
std::vector<std::string> splitLines(const std::string& text);

#if !defined(_WIN32)
// Reads $APPIMAGE, $SNAP, /.flatpak-info, /.dockerenv, /proc/self/exe and asks dpkg / rpm who
// owns the program. UNPROVEN ON LINUX.
InstallProbe probeThisInstall(UpdateHost& host);
// The path applyUpdate replaces: the AppImage file, or the directory holding the program.
std::string targetFor(const InstallProbe& probe, InstallKind kind);
std::unique_ptr<UpdateHost> makeSystemHost();
#endif

}  // namespace cascade::core::linuxupdate

#endif  // CASCADE_CORE_LINUX_UPDATE_HOST_HPP
