// radio_handoff.cpp - see radio_handoff.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_handoff.hpp"

#include <chrono>
#include <cstdlib>
#include <vector>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#endif

namespace cascade::core::handoff {

std::string radioKey(const std::string& kind, const std::string& args) {
    if (kind.empty()) { return std::string(); }
    const std::string tag = "serial=";
    const std::size_t at = args.find(tag);
    if (at == std::string::npos) { return std::string(); }
    std::size_t end = args.find(',', at);
    std::string serial = args.substr(at + tag.size(), end == std::string::npos ? std::string::npos : end - at - tag.size());
    if (serial.empty()) { return std::string(); }
    return kind + "|" + serial;
}

std::string serialOfKey(const std::string& key) {
    const std::size_t bar = key.find('|');
    return bar == std::string::npos ? std::string() : key.substr(bar + 1);
}

std::string lastFour(const std::string& serial) { return serial.size() <= 4 ? serial : serial.substr(serial.size() - 4); }

std::string defaultPrefix() {
    std::string p = "FoxSDR-handoff";
    const char* hook = std::getenv("CASCADE_CONFIG_TEST");
    const char* scope = std::getenv("FOXSDR_INSTANCE_SCOPE");
    if (hook != nullptr && scope != nullptr && *scope != '\0') {
        std::string s(scope);
        for (char& c : s) {
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) { c = '_'; }
        }
        p += "-" + s.substr(0, 32);
    }
    return p;
}

std::string refusalSentence(const std::string& code, const std::string& radioName) {
    using cascade::i18n::tr;
    std::string s;
    if (code == "recording") {
        s = tr("%s is recording in the other FoxSDR window. Stop the recording there first, then try again.");
    } else {
        s = tr("The other FoxSDR window would not let %s go.");
    }
    const std::size_t at = s.find("%s");
    if (at != std::string::npos) { s.replace(at, 2, radioName); }
    return s;
}

Sheet sheetFor(const std::string& state, const std::string& model, const std::string& last4, const std::string& detail) {
    using cascade::i18n::tr;
    auto fill = [&](std::string s) {
        const std::string name = model.empty() ? std::string(tr("The radio")) : model;
        const std::string ending = last4.empty() ? std::string() : " " + std::string(tr("(serial ending")) + " " + last4 + ")";
        const std::size_t at = s.find("%s");
        if (at != std::string::npos) { s.replace(at, 2, name + ending); }
        return s;
    };
    Sheet h;
    if (state == "ask") {
        h.title = tr("Radio in use by another FoxSDR window");
        h.text = fill(tr("%s is in use by another FoxSDR window on this PC. Move it here? The other window will stop using it and show a notice."));
        h.goLabel = tr("Move it here");
        h.stayLabel = tr("Leave it there");
    } else if (state == "other") {
        h.title = tr("Radio in use");
        h.text = fill(tr("%s is in use by another program. Close that program, then pick the radio again."));
        h.stayLabel = tr("OK");
    } else if (state == "refused") {
        h.title = tr("Radio not moved");
        h.text = refusalSentence(detail, model.empty() ? std::string(tr("The radio")) : model + (last4.empty() ? std::string() : " " + std::string(tr("(serial ending")) + " " + last4 + ")"));
        h.stayLabel = tr("OK");
    } else if (state == "timeout") {
        h.title = tr("Radio not moved");
        h.text = fill(tr("The other FoxSDR window did not answer in time, so %s was not moved. Nothing was changed: it is still in use there."));
        h.stayLabel = tr("OK");
    } else if (state == "moved") {
        h.title = tr("Radio moved");
        h.text = fill(tr("%s was moved to another FoxSDR window. This window is on the signal generator now."));
        h.stayLabel = tr("OK");
    }
    return h;
}

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// "D:P(A;;GA;;;<this user's SID>)": the one account that may open the pipe is the one running this program.
bool currentUserOnlyDescriptor(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) { return false; }
    DWORD need = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &need);
    std::vector<BYTE> buf(need ? need : 1);
    bool ok = need != 0 && ::GetTokenInformation(token, TokenUser, buf.data(), need, &need) != FALSE;
    ::CloseHandle(token);
    if (!ok) { return false; }
    LPWSTR sid = nullptr;
    if (!::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) { return false; }
    const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    ::LocalFree(sid);
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) { return false; }
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return true;
}

bool overlappedWait(HANDLE h, OVERLAPPED& ov, DWORD timeoutMs, DWORD& moved, HANDLE stopEvent = nullptr) {
    HANDLE waits[2] = {ov.hEvent, stopEvent};
    const DWORD r = ::WaitForMultipleObjects(stopEvent != nullptr ? 2 : 1, waits, FALSE, timeoutMs);
    if (r == WAIT_OBJECT_0) { return ::GetOverlappedResult(h, &ov, &moved, FALSE) != FALSE; }
    ::CancelIoEx(h, &ov);
    DWORD ignored = 0;
    ::GetOverlappedResult(h, &ov, &ignored, TRUE);
    return false;
}

bool readLine(HANDLE h, OVERLAPPED& ov, DWORD timeoutMs, std::string& out, HANDLE stopEvent = nullptr) {
    char buf[512];
    ::ResetEvent(ov.hEvent);
    DWORD got = 0;
    if (!::ReadFile(h, buf, sizeof(buf) - 1, &got, &ov)) {
        if (::GetLastError() != ERROR_IO_PENDING) { return false; }
        if (!overlappedWait(h, ov, timeoutMs, got, stopEvent)) { return false; }
    }
    out.assign(buf, got);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '\0')) { out.pop_back(); }
    return true;
}

bool writeLine(HANDLE h, OVERLAPPED& ov, const std::string& line, DWORD timeoutMs) {
    const std::string msg = line + "\n";
    ::ResetEvent(ov.hEvent);
    DWORD put = 0;
    if (!::WriteFile(h, msg.data(), static_cast<DWORD>(msg.size()), &put, &ov)) {
        if (::GetLastError() != ERROR_IO_PENDING) { return false; }
        if (!overlappedWait(h, ov, timeoutMs, put)) { return false; }
    }
    return true;
}

// One request, one answer, against one pipe, inside `timeoutMs` in all. False when nothing answered.
bool transact(const std::wstring& path, const std::string& request, int timeoutMs, std::string& reply, unsigned long& serverPid) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto left = [&]() {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        return ms > 0 ? static_cast<DWORD>(ms) : 0u;
    };
    HANDLE h = INVALID_HANDLE_VALUE;
    for (;;) {
        h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) { break; }
        if (::GetLastError() != ERROR_PIPE_BUSY || left() == 0) { return false; }
        ::WaitNamedPipeW(path.c_str(), left() < 200 ? left() : 200);
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    ::SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
    ULONG pid = 0;
    if (::GetNamedPipeServerProcessId(h, &pid)) { serverPid = pid; }
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const bool ok = ov.hEvent != nullptr && writeLine(h, ov, request, left() ? left() : 1) && readLine(h, ov, left(), reply);
    if (ov.hEvent != nullptr) { ::CloseHandle(ov.hEvent); }
    ::CloseHandle(h);
    return ok;
}

std::wstring pipePath(const std::string& prefix, unsigned long pid) { return L"\\\\.\\pipe\\" + widen(prefix) + L"-" + std::to_wstring(pid); }

}  // namespace

struct Server::Impl {
    std::string prefix, window;
    Hooks hooks;
    HANDLE stopEvent = nullptr;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
};

Server::Server() = default;
Server::~Server() { stop(); }

bool Server::start(const std::string& prefix, const std::string& windowWord, Hooks hooks) {
    if (running_.load()) { return true; }
    impl_ = std::make_unique<Impl>();
    Impl& I = *impl_;
    I.prefix = prefix;
    I.window = windowWord;
    I.hooks = std::move(hooks);
    if (!currentUserOnlyDescriptor(I.sa, I.sd)) {
        diagLogf("handoff: the pipe's access list could not be made; this window cannot be asked to let a radio go");
        impl_.reset();
        return false;
    }
    I.stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    I.pipe = ::CreateNamedPipeW(pipePath(prefix, ::GetCurrentProcessId()).c_str(),
                                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 512, 512, 0, &I.sa);
    if (I.pipe == INVALID_HANDLE_VALUE || I.stopEvent == nullptr) {
        diagLogf("handoff: the pipe could not be created (error %lu); this window cannot be asked to let a radio go", static_cast<unsigned long>(::GetLastError()));
        if (I.stopEvent != nullptr) { ::CloseHandle(I.stopEvent); }
        ::LocalFree(I.sd);
        impl_.reset();
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this]() {
        Impl& I2 = *impl_;
        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        while (running_.load()) {
            ::ResetEvent(ov.hEvent);
            bool connected = false;
            if (::ConnectNamedPipe(I2.pipe, &ov)) {
                connected = true;
            } else {
                const DWORD e = ::GetLastError();
                if (e == ERROR_PIPE_CONNECTED) {
                    connected = true;
                } else if (e == ERROR_IO_PENDING) {
                    DWORD moved = 0;
                    connected = overlappedWait(I2.pipe, ov, INFINITE, moved, I2.stopEvent);
                }
            }
            if (!running_.load()) { break; }
            if (connected) {
                std::string req;
                if (readLine(I2.pipe, ov, 2000, req)) {
                    std::string answer = "NO";
                    const std::size_t sp = req.find(' ');
                    const std::string verb = req.substr(0, sp);
                    const std::string key = sp == std::string::npos ? std::string() : req.substr(sp + 1);
                    if (verb == "HOLDS") {
                        answer = (!key.empty() && I2.hooks.holds && I2.hooks.holds(key)) ? "YES " + I2.window : "NO";
                    } else if (verb == "RELEASE") {
                        ReleaseResult r;
                        if (!key.empty() && I2.hooks.release) { r = I2.hooks.release(key); }
                        switch (r.result) {
                            case Release::Released: answer = "RELEASED"; break;
                            case Release::Refused: answer = "REFUSED " + (r.code.empty() ? std::string("other") : r.code); break;
                            case Release::NotHeld: answer = "NOTHELD"; break;
                            default: answer = "REFUSED busy"; break;
                        }
                    }
                    writeLine(I2.pipe, ov, answer, 2000);
                    served_.fetch_add(1);
                }
            }
            ::DisconnectNamedPipe(I2.pipe);
        }
        ::CloseHandle(ov.hEvent);
    });
    return true;
}

void Server::stop() {
    if (!impl_) { return; }
    running_.store(false);
    if (impl_->stopEvent != nullptr) { ::SetEvent(impl_->stopEvent); }
    if (thread_.joinable()) { thread_.join(); }
    if (impl_->pipe != INVALID_HANDLE_VALUE) { ::CloseHandle(impl_->pipe); }
    if (impl_->stopEvent != nullptr) { ::CloseHandle(impl_->stopEvent); }
    if (impl_->sd != nullptr) { ::LocalFree(impl_->sd); }
    impl_.reset();
}

bool findHolder(const std::string& prefix, const std::string& key, int budgetMs, Holder& out, unsigned long selfPid) {
    if (selfPid == 0) { selfPid = ::GetCurrentProcessId(); }
    if (key.empty()) { return false; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    const std::wstring pattern = L"\\\\.\\pipe\\" + widen(prefix) + L"-*";
    WIN32_FIND_DATAW fd{};
    HANDLE f = ::FindFirstFileW(pattern.c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) { return false; }
    std::vector<std::wstring> names;
    do { names.push_back(fd.cFileName); } while (::FindNextFileW(f, &fd));
    ::FindClose(f);
    const std::wstring stem = widen(prefix) + L"-";
    for (const std::wstring& n : names) {
        if (n.size() <= stem.size() || n.compare(0, stem.size(), stem) != 0) { continue; }
        const std::wstring tail = n.substr(stem.size());
        if (tail.find_first_not_of(L"0123456789") != std::wstring::npos) { continue; }
        const unsigned long pid = std::wcstoul(tail.c_str(), nullptr, 10);
        if (pid == selfPid) { continue; }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (ms <= 0) { break; }
        std::string reply;
        unsigned long serverPid = 0;
        if (!transact(L"\\\\.\\pipe\\" + n, "HOLDS " + key, static_cast<int>(ms), reply, serverPid)) { continue; }
        if (reply.rfind("YES", 0) == 0 && serverPid == pid) {
            out.pipe.clear();
            for (const wchar_t c : n) { out.pipe.push_back(static_cast<char>(c)); }   // a pipe name here is ASCII (prefix, '-', digits)
            out.pid = pid;
            out.window = reply.size() > 4 ? reply.substr(4) : std::string();
            return true;
        }
    }
    return false;
}

ReleaseResult requestRelease(const Holder& holder, const std::string& key, int timeoutMs) {
    ReleaseResult r;
    std::string reply;
    unsigned long pid = 0;
    std::wstring path = L"\\\\.\\pipe\\";
    for (const char c : holder.pipe) { path.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c))); }
    if (!transact(path, "RELEASE " + key, timeoutMs, reply, pid)) { return r; }   // NoAnswer
    if (reply == "RELEASED") {
        r.result = Release::Released;
    } else if (reply.rfind("REFUSED", 0) == 0) {
        r.result = Release::Refused;
        r.code = reply.size() > 8 ? reply.substr(8) : std::string("other");
    } else if (reply == "NOTHELD") {
        r.result = Release::NotHeld;
    }
    return r;
}

#else  // !_WIN32: no pipe, so no holder is ever found and no window is ever asked

struct Server::Impl {};
Server::Server() = default;
Server::~Server() {}
bool Server::start(const std::string&, const std::string&, Hooks) { return false; }
void Server::stop() {}
bool findHolder(const std::string&, const std::string&, int, Holder&, unsigned long) { return false; }
ReleaseResult requestRelease(const Holder&, const std::string&, int) { return ReleaseResult{}; }

#endif

}  // namespace cascade::core::handoff
