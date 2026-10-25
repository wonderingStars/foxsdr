// software_gl.cpp - see software_gl.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/software_gl.hpp"

#include <cstdio>

#include "core/diag_log.hpp"
#include "gui/window_platform.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <vector>
#endif

namespace cascade::gui::softgl {

#if defined(_WIN32)

namespace {

std::atomic<bool> g_loaded{false};

// <folder of cascade.exe>\softgl, or empty when the executable's own path cannot be read.
std::filesystem::path folder() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) { return {}; }
        if (n < buf.size()) { return std::filesystem::path(std::wstring(buf.data(), n)).parent_path() / L"softgl"; }
        if (buf.size() >= 32768) { return {}; }
        buf.resize(buf.size() * 2);
    }
}

bool sameFile(HMODULE module, const std::filesystem::path& want) {
    std::vector<wchar_t> buf(32768);
    const DWORD n = ::GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) { return false; }
    std::error_code ec;
    return std::filesystem::equivalent(std::filesystem::path(std::wstring(buf.data(), n)), want, ec);
}

std::string galliumDriverNow() {
    wchar_t buf[64] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"GALLIUM_DRIVER", buf, 64);
    if (n == 0 || n >= 64) { return std::string(); }
    std::string out;
    for (DWORD i = 0; i < n; ++i) { out.push_back(buf[i] < 0x80 ? static_cast<char>(buf[i]) : '?'); }
    return out;
}

}  // namespace

bool present() {
    const std::filesystem::path dir = folder();
    if (dir.empty()) { return false; }
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / L"opengl32.dll", ec) &&
           std::filesystem::is_regular_file(dir / L"libgallium_wgl.dll", ec);
}

bool loaded() { return g_loaded.load(); }

LoadResult load() {
    LoadResult r;
    const std::filesystem::path dir = folder();
    if (dir.empty() || !present()) { return r; }
    const std::filesystem::path gl = dir / L"opengl32.dll";
    // ALREADY ONE: ours (a second call) or the system's (nothing may be changed under it).
    if (HMODULE existing = ::GetModuleHandleW(L"opengl32.dll"); existing != nullptr) {
        if (sameFile(existing, gl)) {
            g_loaded = true;
            r.status = LoadStatus::Loaded;
            r.galliumDriver = galliumDriverNow();
        } else {
            r.status = LoadStatus::SystemStillLoaded;
        }
        return r;
    }
    if (galliumDriverNow().empty()) { ::SetEnvironmentVariableW(L"GALLIUM_DRIVER", L"llvmpipe"); }
    // The renderer first, by its full path, so that opengl32.dll's import of it is answered from
    // the loaded-module list rather than from a search that would never look in softgl\. Both
    // handles are kept for the life of the process: OpenGL is never unloaded under a window.
    const std::filesystem::path gallium = dir / L"libgallium_wgl.dll";
    if (::LoadLibraryExW(gallium.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr ||
        ::LoadLibraryExW(gl.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) == nullptr) {
        r.status = LoadStatus::Failed;
        r.error = ::GetLastError();
        return r;
    }
    g_loaded = true;
    r.status = LoadStatus::Loaded;
    r.galliumDriver = galliumDriverNow();
    return r;
}

RelaunchResult relaunchAndWait() {
    RelaunchResult r;
    std::vector<wchar_t> exe(32768);
    const DWORD n = ::GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (n == 0 || n >= exe.size()) {
        r.error = ::GetLastError();
        return r;
    }
    // The same command line, word for word: whatever the first process was asked to do - a
    // bounded run, a link, a file - the second is asked the same.
    std::wstring cmd = ::GetCommandLineW();
    ::SetEnvironmentVariableW(L"FOXSDR_GL", L"software");
    const std::wstring parent = std::to_wstring(::GetCurrentProcessId());
    ::SetEnvironmentVariableW(L"FOXSDR_SOFTGL_RELAUNCH", parent.c_str());

    // REDIRECTED STANDARD HANDLES ARE PASSED ON, AND NOTHING ELSE. A pipe or a file (a test, a
    // script) is handed to the second process explicitly; a console is not a handle to pass - the
    // second process attaches to it as the first did (core/console_owner.cpp) - and no other
    // inheritable handle of this process (the sentinel's, say) leaks into it: the attribute list
    // names exactly the handles it may have.
    std::vector<HANDLE> pass;
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    HANDLE* slots[] = {&si.StartupInfo.hStdInput, &si.StartupInfo.hStdOutput, &si.StartupInfo.hStdError};
    const DWORD ids[] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (int i = 0; i < 3; ++i) {
        HANDLE h = ::GetStdHandle(ids[i]);
        const DWORD type = (h != nullptr && h != INVALID_HANDLE_VALUE) ? ::GetFileType(h) : FILE_TYPE_UNKNOWN;
        if (type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE) { continue; }
        ::SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        *slots[i] = h;
        bool seen = false;
        for (const HANDLE p : pass) { seen = seen || p == h; }
        if (!seen) { pass.push_back(h); }
    }
    std::vector<unsigned char> attrBuf;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = nullptr;
    DWORD flags = 0;
    if (!pass.empty()) {
        SIZE_T size = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attrBuf.resize(size);
        attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        if (::InitializeProcThreadAttributeList(attrs, 1, 0, &size) &&
            ::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, pass.data(),
                                        pass.size() * sizeof(HANDLE), nullptr, nullptr)) {
            si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
            si.lpAttributeList = attrs;
            flags |= EXTENDED_STARTUPINFO_PRESENT;
        } else {
            attrs = nullptr;
        }
    }
    PROCESS_INFORMATION pi{};
    const BOOL ok = ::CreateProcessW(exe.data(), cmd.data(), nullptr, nullptr, attrs != nullptr ? TRUE : FALSE,
                                     flags, nullptr, nullptr, &si.StartupInfo, &pi);
    if (attrs != nullptr) { ::DeleteProcThreadAttributeList(attrs); }
    if (!ok) {
        r.error = ::GetLastError();
        return r;
    }
    ::CloseHandle(pi.hThread);
    r.started = true;
    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hProcess);
    r.exitCode = code;
    return r;
}

void tellUserRelaunchFailed(unsigned long error, int frames) {
    namespace wp = cascade::gui::windowplatform;
    cascade::core::diagWarnf("window: the second FoxSDR process could not be started (Windows error %lu)", error);
    const std::string text = wp::failureMessage(cascade::core::DiagLog::instance().filePath(), true);
    std::fprintf(stderr, "cascade: the software renderer could not be started\n%s\n", text.c_str());
    if (!wp::errorDialogAllowed(frames)) {
        cascade::core::diagLogf("window: error dialog suppressed for a flagged run");
        return;
    }
    cascade::core::diagLogf("window: error dialog shown");
    ::MessageBoxA(nullptr, text.c_str(), "FoxSDR", MB_OK | MB_ICONERROR);
}

bool takeRelaunchMarker(std::string& parent) {
    const char* v = std::getenv(kRelaunchEnv);
    if (v == nullptr || *v == '\0') { return false; }
    parent.clear();
    for (const char* p = v; *p != '\0' && parent.size() < 10; ++p) {
        if (*p >= '0' && *p <= '9') { parent.push_back(*p); }
    }
    _putenv_s(kRelaunchEnv, "");  // removes it from both the C runtime's copy and the process's
    return true;
}

#else

bool present() { return false; }
bool loaded() { return false; }
LoadResult load() { return LoadResult{}; }
RelaunchResult relaunchAndWait() { return RelaunchResult{}; }
void tellUserRelaunchFailed(unsigned long, int) {}
bool takeRelaunchMarker(std::string&) { return false; }

#endif

}  // namespace cascade::gui::softgl
