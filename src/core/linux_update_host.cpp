// Implementation of core/linux_update_host.hpp. The FsUpdateHost half is portable and tested on
// Windows; the SystemUpdateHost half is POSIX only and UNPROVEN ON LINUX.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/linux_update_host.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "core/plugin_repo.hpp"

#if !defined(_WIN32)
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "core/linux_update_fetch.hpp"
#endif

namespace fs = std::filesystem;

namespace cascade::core::linuxupdate {

bool FsUpdateHost::exists(const std::string& path) {
    std::error_code ec;
    return fs::exists(fs::path(path), ec);
}

std::int64_t FsUpdateHost::fileSize(const std::string& path) {
    std::error_code ec;
    const fs::path p(path);
    if (!fs::is_regular_file(p, ec)) { return -1; }
    const auto n = fs::file_size(p, ec);
    return ec ? -1 : static_cast<std::int64_t>(n);
}

bool FsUpdateHost::makeDir(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::path(path), ec);
    return !ec && fs::is_directory(fs::path(path), ec);
}

bool FsUpdateHost::writableDir(const std::string& dir) {
    // The only honest test of "can I create a file here" is creating one.
    const std::string probe =
        dir + "/.foxsdr-write-probe-" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
    std::FILE* f = std::fopen(probe.c_str(), "wb");
    if (f == nullptr) { return false; }
    std::fclose(f);
    std::error_code ec;
    fs::remove(fs::path(probe), ec);
    return true;
}

std::uint64_t FsUpdateHost::freeSpace(const std::string& dir) {
    std::error_code ec;
    const fs::space_info si = fs::space(fs::path(dir), ec);
    return ec ? 0 : static_cast<std::uint64_t>(si.available);
}

bool FsUpdateHost::createExclusive(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "wbx");  // "x": fail if it exists (C11; MSVC and glibc)
    if (f == nullptr) { return false; }
    std::fclose(f);
    return true;
}

bool FsUpdateHost::removeAll(const std::string& path) {
    std::error_code ec;
    fs::remove_all(fs::path(path), ec);
    return !fs::exists(fs::path(path), ec);
}

bool FsUpdateHost::rename(const std::string& from, const std::string& to) {
    std::error_code ec;
    fs::rename(fs::path(from), fs::path(to), ec);  // rename(2) on POSIX: atomic within one file system
    return !ec;
}

bool FsUpdateHost::writeText(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

bool FsUpdateHost::readText(const std::string& path, std::string& text) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { return false; }
    text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool FsUpdateHost::readHead(const std::string& path, std::size_t n, std::string& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { return false; }
    bytes.assign(n, '\0');
    in.read(&bytes[0], static_cast<std::streamsize>(n));
    bytes.resize(static_cast<std::size_t>(in.gcount()));
    return true;
}

bool FsUpdateHost::sha256(const std::string& path, std::string& hexLower) {
    std::string err;
    return PluginRepo::sha256File(path, hexLower, err);
}

bool FsUpdateHost::makeExecutable(const std::string& path) {
    std::error_code ec;
    fs::permissions(fs::path(path),
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
    return !ec;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t nl = text.find('\n', i);
        if (nl == std::string::npos) { nl = text.size(); }
        std::string line = text.substr(i, nl - i);
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        out.push_back(std::move(line));
        i = nl + 1;
    }
    return out;
}

#if !defined(_WIN32)

namespace {

constexpr std::size_t kMaxProgramOutput = 4u << 20;  // a tar listing of a few thousand names

class SystemHost final : public FsUpdateHost {
public:
    FetchResult fetch(const std::string& url, const std::string& partialPath,
                      std::uint64_t resumeFrom, std::uint64_t maxBytes,
                      std::atomic<bool>* cancel) override {
        FetchResult fr;
        // https://host[:port]/path - the manifest parse has already required https and foxsdr.com.
        const std::string prefix = "https://";
        if (url.rfind(prefix, 0) != 0) { fr.error = "not an https address"; return fr; }
        const std::size_t slash = url.find('/', prefix.size());
        const std::string authority = url.substr(
            prefix.size(), slash == std::string::npos ? std::string::npos : slash - prefix.size());
        const std::string target = slash == std::string::npos ? "/" : url.substr(slash);
        std::string host = authority;
        int port = 443;
        const std::size_t colon = authority.find(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = std::atoi(authority.c_str() + colon + 1);
        }
        httplib::SSLClient cli(host, port);
        cli.enable_server_certificate_verification(true);  // left strict, as the plugin catalogue's is
        cli.set_follow_location(false);                    // a redirect could leave foxsdr.com
        cli.set_connection_timeout(10, 0);
        cli.set_read_timeout(30, 0);
        cli.set_write_timeout(30, 0);
        return rangeFetch(cli, target, partialPath, resumeFrom, maxBytes, cancel);
    }

    bool listArchive(const std::string& archive, std::vector<std::string>& names) override {
        std::string out;
        if (runProgram("tar", {"-tzf", archive}, 60000, out) != 0) { return false; }
        names = splitLines(out);
        return true;
    }

    bool extractArchive(const std::string& archive, const std::string& destDir) override {
        std::string out;
        return runProgram("tar", {"-xzf", archive, "-C", destDir, "--no-same-owner"}, 300000, out) == 0;
    }

    int runProgram(const std::string& program, const std::vector<std::string>& args, int timeoutMs,
                   std::string& output) override {
        output.clear();
        int fds[2];
        if (::pipe(fds) != 0) { return -1; }
        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return -1;
        }
        if (pid == 0) {
            ::dup2(fds[1], 1);
            ::dup2(fds[1], 2);
            ::close(fds[0]);
            ::close(fds[1]);
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(program.c_str()));
            for (const std::string& a : args) { argv.push_back(const_cast<char*>(a.c_str())); }
            argv.push_back(nullptr);
            ::execvp(program.c_str(), argv.data());  // no shell: the arguments are exactly these
            ::_exit(127);
        }
        ::close(fds[1]);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        bool timedOut = false;
        char buf[4096];
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) { timedOut = true; break; }
            pollfd p{fds[0], POLLIN, 0};
            const int pr = ::poll(&p, 1, static_cast<int>(left));
            if (pr < 0 && errno == EINTR) { continue; }
            if (pr <= 0) { timedOut = (pr == 0); break; }
            const ssize_t n = ::read(fds[0], buf, sizeof(buf));
            if (n <= 0) { break; }  // end of output: the child closed its end
            if (output.size() < kMaxProgramOutput) { output.append(buf, static_cast<std::size_t>(n)); }
        }
        ::close(fds[0]);
        if (timedOut) { ::kill(pid, SIGKILL); }  // our own child, and only it
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (timedOut || !WIFEXITED(status)) { return -1; }
        return WEXITSTATUS(status);
    }
};

std::string parentDir(const std::string& p) {
    const std::size_t s = p.find_last_of('/');
    return s == std::string::npos ? "." : (s == 0 ? "/" : p.substr(0, s));
}

}  // namespace

std::unique_ptr<UpdateHost> makeSystemHost() { return std::make_unique<SystemHost>(); }

InstallProbe probeThisInstall(UpdateHost& host) {
    InstallProbe p;
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { p.exePath.assign(buf, static_cast<std::size_t>(n)); }
    if (const char* a = std::getenv("APPIMAGE")) { p.appImageEnv = a; }
    p.appImageFileExists = !p.appImageEnv.empty() && host.exists(p.appImageEnv);
    p.snapEnv = std::getenv("SNAP") != nullptr;
    const char* flatpakId = std::getenv("FLATPAK_ID");
    p.flatpakInfo = (flatpakId != nullptr && *flatpakId != '\0') || host.exists("/.flatpak-info");
    p.containerEnv = host.exists("/.dockerenv");
    for (const char* prefix : {"/usr/bin/", "/usr/sbin/", "/usr/lib/", "/usr/libexec/", "/bin/", "/sbin/"}) {
        if (p.exePath.rfind(prefix, 0) == 0) { p.underSystemPrefix = true; }
    }
    if (!p.exePath.empty()) {
        std::string out;
        // Fixed argv, the path as ONE argument: nothing a user types reaches a shell.
        if (host.runProgram("dpkg", {"-S", p.exePath}, 5000, out) == 0) {
            p.ownedByPackage = true;
        } else if (host.runProgram("rpm", {"-qf", p.exePath}, 5000, out) == 0) {
            p.ownedByPackage = true;
        }
    }
    // The AppImage file is replaced inside its own folder; an unpacked tree is replaced inside the
    // folder that holds its root (the program sits at the root), so that is the folder to test.
    const std::string where =
        p.appImageFileExists ? parentDir(p.appImageEnv) : parentDir(parentDir(p.exePath));
    p.dirWritable = !p.exePath.empty() && host.writableDir(where);
    return p;
}

std::string targetFor(const InstallProbe& probe, InstallKind kind) {
    if (kind == InstallKind::AppImage) { return probe.appImageEnv; }
    if (kind == InstallKind::Tree) { return parentDir(probe.exePath); }
    return std::string();
}

#endif  // !_WIN32

}  // namespace cascade::core::linuxupdate
