// The crash handler names code that was MAPPED AFTER its module table was last
// refreshed.
//
// The table the fault path searches (core/diag_report.cpp) is a snapshot, taken
// on the healthy path because enumerating modules from a fault handler means
// the loader lock. A fault in a DLL that arrived afterwards used to resolve to
// nothing: the report's `address:` was a bare number, its frames were bare
// numbers, its module list did not mention the DLL, and it hashed to the
// signature every unresolved fault of that code shares. That is exactly how the
// 2026-10-01 field report looked - a Native Instruments ASIO driver that
// RtAudio maps in the middle of SoapyAudio's probe, nine frames named "-" - and
// the enumeration child's one refresh (after the vendor modules load, before
// any probe runs) cannot see a DLL a probe maps itself.
//
// A real child, because the property is only visible in a report a real fault
// wrote: it arms the handlers, refreshes the table, THEN loads a DLL that no
// table has ever heard of (tests/fixtures/late_fault_dll.cpp) and faults inside
// it. A second child faults in memory that is not an image at all - the negative
// control, which must stay a bare address and must not take the handler down.
//
// Windows only: the fault-time lookup it covers is a Windows one. Elsewhere it
// SKIPs by name (SKIP_LINUX), so the suite's count does not hide that it did
// not run.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

#if defined(_WIN32)
namespace {

enum Kind { kLateDll = 1, kPrivateMemory = 2 };

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    return base / (std::string("cascade-latemod-") + tag + "-" +
                   std::to_string(::GetCurrentProcessId()));
}

std::string selfExePath() {
    char buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return std::string(buf, buf + n);
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The child: what a session has done by the time it faults - a context block,
// a refreshed table, the handlers armed - and then the load nobody told the
// table about.
int faultChild(int kind, const std::string& dir, const std::string& dllPath) {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    DiagLog::instance().configure(std::string(), false);
    DiagLog::instance().write("info", "child started");

    DiagContext ctx;
    ctx.version = "0.99.0-latemodtest";
    ctx.commit = "feedface5678";
    ctx.os = "test";
    ctx.arch = "x64";
    setDiagContext(ctx);
    refreshModuleTable();

    CrashHandlerConfig cfg;
    cfg.crashDir = dir;
    cfg.enabled = true;
    cfg.minidump = false;
    cfg.exitAfterReport = true;
    installCrashHandlers(cfg);

    if (kind == kLateDll) {
        const HMODULE dll = ::LoadLibraryA(dllPath.c_str());
        if (dll == nullptr) { return 8; }
        using Fault = void (*)();
        const Fault fault = reinterpret_cast<Fault>(
            reinterpret_cast<void*>(::GetProcAddress(dll, "lateFixtureFault")));
        if (fault == nullptr) { return 9; }
        fault();
    } else if (kind == kPrivateMemory) {
        // Code that is not in any image: a page of our own with UD2 in it.
        void* page = ::VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (page == nullptr) { return 10; }
        const unsigned char ud2[] = {0x0F, 0x0B};
        std::memcpy(page, ud2, sizeof(ud2));
        using Code = void (*)();
        reinterpret_cast<Code>(page)();
    }
    return 7;  // never reached; 7 means the fault did not happen
}

struct Caught {
    unsigned long exitCode = 0;
    bool timedOut = false;
    std::size_t reports = 0;
    std::string text;
};

Caught runChild(int kind, const std::string& tag, const std::string& dllPath) {
    Caught out;
    const fs::path dir = scratchDir(tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    std::string cmd = "\"" + selfExePath() + "\" --fault " + std::to_string(kind) + " \"" +
                      dir.string() + "\" \"" + dllPath + "\"";
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &si, &pi) == 0) {
        out.exitCode = 0xFFFFFFFFul;
        return out;
    }
    if (::WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0) {
        out.timedOut = true;
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    out.exitCode = code;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        ++out.reports;
        out.text += readFile(e.path());
    }
    return out;
}

// The line `key` starts in `text` (first occurrence at a line start), without
// its newline; empty when absent.
std::string lineStarting(const std::string& text, const std::string& key) {
    std::size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        if (pos == 0 || text[pos - 1] == '\n') {
            const std::size_t eol = text.find('\n', pos);
            return text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos);
        }
        pos += key.size();
    }
    return std::string();
}

// The text between two section markers ("--- stack" up to "--- process").
std::string section(const std::string& text, const std::string& from, const std::string& to) {
    const std::size_t a = text.find(from);
    if (a == std::string::npos) { return std::string(); }
    const std::size_t b = text.find(to, a);
    return text.substr(a, (b == std::string::npos ? text.size() : b) - a);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--fault") == 0) {
        return faultChild(std::atoi(argv[2]), argv[3], argv[4]);
    }
    // tests/CMakeLists.txt hands over the DLL's full path.
    CHECK(argc >= 2);
    const std::string dllPath = argc >= 2 ? fs::absolute(fs::path(argv[1])).string() : "";
    CHECK(!dllPath.empty() && fs::exists(dllPath));

    // --- A fault in a DLL mapped after the table was refreshed ---------------
    {
        const Caught c = runChild(kLateDll, "dll", dllPath);
        const std::string address = lineStarting(c.text, "address: ");
        const std::string stack = section(c.text, "--- stack", "--- process");
        const std::string modules = section(c.text, "--- modules ---", "--- log");
        std::printf("late dll: exit=0x%08lX reports=%zu %s\n", c.exitCode, c.reports,
                    address.c_str());
        CHECK(!c.timedOut);
        CHECK(c.exitCode == 0xC0000005ul);  // the fault's own code, as ever
        CHECK(c.reports == 1u);

        // Named where the report names the fault...
        CHECK(address.find("address: late_fault_fixture.dll+0x") == 0);
        // ...in the frame it was walked from...
        CHECK(stack.find("\n  late_fault_fixture.dll+0x") != std::string::npos);
        // ...in the module list a reader resolves the other frames against...
        CHECK(modules.find("  late_fault_fixture.dll base=0x") != std::string::npos);
        // ...and in the grouping, which is no longer the signature every
        // unresolved access violation shares.
        CHECK(c.text.find("signature: " + crashSignature(0xC0000005ul, "?", 0)) ==
              std::string::npos);
        // The rest of the report is as it always was.
        CHECK(c.text.find("version: 0.99.0-latemodtest") != std::string::npos);
        CHECK(c.text.find("commit: feedface5678") != std::string::npos);
        CHECK(c.text.find(".exe+0x") != std::string::npos);  // our own frames still resolve
        CHECK(c.text.find("--- process ---") != std::string::npos);
    }

    // --- NEGATIVE CONTROL: a fault in memory that is no image ---------------
    //
    // Nothing to name, so nothing may be invented: the address stays a bare
    // address, no module is added to the list for it, and the handler still
    // writes a whole report and dies with the fault's own code.
    {
        const Caught c = runChild(kPrivateMemory, "private", dllPath);
        const std::string address = lineStarting(c.text, "address: ");
        const std::string modules = section(c.text, "--- modules ---", "--- log");
        std::printf("private memory: exit=0x%08lX reports=%zu %s\n", c.exitCode, c.reports,
                    address.c_str());
        CHECK(!c.timedOut);
        CHECK(c.exitCode == 0xC000001Dul);  // illegal instruction
        CHECK(c.reports == 1u);
        CHECK(address.find("address: 0x") == 0);
        CHECK(modules.find("late_fault_fixture") == std::string::npos);
        CHECK(modules.find("unknown-image") == std::string::npos);
        CHECK(c.text.find("--- process ---") != std::string::npos);
        CHECK(c.text.find("--- log") != std::string::npos);
    }

    if (g_checksFailed == 0) {
        std::error_code ec;
        for (const char* tag : {"dll", "private"}) { fs::remove_all(scratchDir(tag), ec); }
    }
    return testSummary("test_crash_late_module");
}

#else  // !_WIN32

int main() {
    SKIP_LINUX("the fault-time module lookup this covers is Windows-only; the Linux handler's "
               "table is refreshed after dlopen only");
    return testSummary("test_crash_late_module");
}

#endif
