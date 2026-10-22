// config_lock.cpp - see config_lock.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/config_lock.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

// "role=<name> pid=<n>\n": what a holder writes, and the only shape a reader accepts. A role is a
// short run of [a-z-]; anything else in the file is somebody's edit and is ignored.
std::string holderText(const std::string& role, unsigned long pid) {
    return "role=" + role + " pid=" + std::to_string(pid) + "\n";
}

ConfigLock::Holder parseHolder(const std::string& text) {
    ConfigLock::Holder h;
    const std::size_t roleAt = text.find("role=");
    const std::size_t pidAt = text.find(" pid=");
    if (roleAt != 0 || pidAt == std::string::npos) { return h; }
    const std::string role = text.substr(5, pidAt - 5);
    if (role.empty() || role.size() > 32) { return h; }
    for (const char c : role) {
        if (!((c >= 'a' && c <= 'z') || c == '-')) { return h; }
    }
    char* end = nullptr;
    const unsigned long pid = std::strtoul(text.c_str() + pidAt + 5, &end, 10);
    h.role = role;
    h.pid = pid;
    return h;
}

}  // namespace

std::string ConfigLock::lockPathFor(const std::string& configPath) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::path(configPath), ec);
    if (ec || p.empty()) {
        ec.clear();
        p = fs::absolute(fs::path(configPath), ec);
        if (ec) { p = fs::path(configPath); }
        p = p.lexically_normal();
    }
    return p.string() + ".lock";
}

#if defined(_WIN32)

ConfigLock::State ConfigLock::acquire(const std::string& configPath, const std::string& role) {
    if (state_ == State::Held) { return state_; }
    holder_ = Holder{};
    error_.clear();
    lockPath_ = lockPathFor(configPath);
    const fs::path lock(lockPath_);

    std::error_code ec;
    if (lock.has_parent_path()) { fs::create_directories(lock.parent_path(), ec); }

    // NO WRITE SHARING: a second CreateFileW for writing finds this handle and fails. Read sharing is
    // left on so the program that is refused can read who holds it.
    HANDLE h = ::CreateFileW(lock.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
            HANDLE r = ::CreateFileW(lock.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (r != INVALID_HANDLE_VALUE) {
                char buf[128] = {0};
                DWORD got = 0;
                if (::ReadFile(r, buf, sizeof(buf) - 1, &got, nullptr)) { holder_ = parseHolder(std::string(buf, got)); }
                ::CloseHandle(r);
            }
            state_ = State::Contended;
            return state_;
        }
        error_ = std::system_category().message(static_cast<int>(err));
        state_ = State::Unavailable;
        return state_;
    }
    handle_ = h;
    // Who holds it: from the start of the file, replacing whatever the last holder left.
    const std::string text = holderText(role, static_cast<unsigned long>(::GetCurrentProcessId()));
    ::SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    DWORD wrote = 0;
    ::WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
    ::SetEndOfFile(h);
    state_ = State::Held;
    return state_;
}

void ConfigLock::release() {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    if (state_ == State::Held) { state_ = State::None; }
}

#else  // POSIX

ConfigLock::State ConfigLock::acquire(const std::string& configPath, const std::string& role) {
    if (state_ == State::Held) { return state_; }
    holder_ = Holder{};
    error_.clear();
    lockPath_ = lockPathFor(configPath);
    const fs::path lock(lockPath_);

    std::error_code ec;
    if (lock.has_parent_path()) { fs::create_directories(lock.parent_path(), ec); }

    // O_CLOEXEC: the enumeration helper and the sentinel are this program re-run as a child; the lock
    // must die with THIS process, not live on in a child that outlasts it.
    const int fd = ::open(lockPath_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        error_ = std::strerror(errno);
        state_ = State::Unavailable;
        return state_;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        if (err == EWOULDBLOCK || err == EAGAIN) {
            char buf[128] = {0};
            const ssize_t got = ::pread(fd, buf, sizeof(buf) - 1, 0);
            if (got > 0) { holder_ = parseHolder(std::string(buf, static_cast<std::size_t>(got))); }
            ::close(fd);
            state_ = State::Contended;
            return state_;
        }
        error_ = std::strerror(err);
        ::close(fd);
        state_ = State::Unavailable;
        return state_;
    }
    fd_ = fd;
    const std::string text = holderText(role, static_cast<unsigned long>(::getpid()));
    if (::ftruncate(fd, 0) == 0) {
        const ssize_t wrote = ::pwrite(fd, text.data(), text.size(), 0);
        (void)wrote;  // the holder text is for the refused program's sentence; the lock is already held
    }
    state_ = State::Held;
    return state_;
}

void ConfigLock::release() {
    if (fd_ >= 0) {
        ::close(fd_);  // closing the descriptor drops the flock
        fd_ = -1;
    }
    if (state_ == State::Held) { state_ = State::None; }
}

#endif

}  // namespace cascade::core
