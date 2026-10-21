// Windows' own record of the application's death (0.99.66): the parser, the
// validation, the lookup and its time bound, and - on Windows - a REAL process that
// fails fast, whose event is read back.
//
// WHAT IS PINNED HERE, and what by:
//   - the parser against event XML CAPTURED on Windows 11 22631 (2026-10-05) from a
//     real __fastfail, a real /GS failure and a real access violation, in an
//     executable and in a DLL (the one string replaced in each is the account-name
//     part of the two paths, which the parser must never read anyway), and against
//     HOSTILE variants of the same shape;
//   - the matching: process id AND creation time AND exit code, so that another
//     program's event, another instance's and an earlier run's are never attached;
//   - the bound: a source that never answers costs the caller its budget and no
//     more, and a source that throws costs nothing;
//   - the real thing: a child process that really fails fast, an event Windows
//     really wrote, read back through the Event Log API.
//
// THE REAL-CHILD TESTS SAY SKIP, WITH THE REASON, when this machine wrote no event
// for the child - a policy that disables Windows Error Reporting, a disabled or full
// Application log - and an independent witness (os_event_oracle.hpp) confirms it. If
// the witness SEES the event and the reader does not, that is a failure.
//
// WHY THE CHILD DOES NOT SET SEM_NOGPFAULTERRORBOX, which every other crash test's
// child does: measured, 2026-10-05, a process that sets it (alone or with the flag
// below) gets NO Application Error event, for a fast-fail or an access violation.
// The child sets WER_FAULT_REPORTING_NO_UI only - "fault reporting UI should not be
// shown" - which was measured to write the event and show no window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/os_crash_record.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <intrin.h>
#include <windows.h>

#include <werapi.h>

#include "os_event_oracle.hpp"
#pragma comment(lib, "wer.lib")
#endif

using namespace cascade::core;

namespace {

// ---------------------------------------------------------------------------
// CAPTURED EVENTS. Windows 11 22631, 2026-10-05, rendered by EvtRender (so, as the
// sentinel sees them). `C:\Users\someone\build\` replaces the profile folder of the
// machine they were captured on in the two path fields.
// ---------------------------------------------------------------------------
std::string captured(const char* provider, const char* id, const char* time, const char* appName,
                     const char* appStamp, const char* moduleName, const char* modStamp,
                     const char* code, const char* offset, const char* pid, const char* created,
                     const char* modulePath) {
    std::string x;
    x += "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System><Provider Name='";
    x += provider;
    x += "' Guid='{a0e9b465-b939-57d7-b27d-95d8e925ff57}'/><EventID>";
    x += id;
    x += "</EventID><Version>0</Version><Level>2</Level><Task>100</Task><Opcode>0</Opcode>"
         "<Keywords>0x8000000000000000</Keywords><TimeCreated SystemTime='";
    x += time;
    x += "'/><EventRecordID>2979</EventRecordID><Correlation/><Execution ProcessID='11788' "
         "ThreadID='23476'/><Channel>Application</Channel><Computer>MacbookPro</Computer>"
         "<Security UserID='S-1-5-21-1000178079-4051914422-1609082771-1001'/></System><EventData>"
         "<Data Name='AppName'>";
    x += appName;
    x += "</Data><Data Name='AppVersion'>0.0.0.0</Data><Data Name='AppTimeStamp'>";
    x += appStamp;
    x += "</Data><Data Name='ModuleName'>";
    x += moduleName;
    x += "</Data><Data Name='ModuleVersion'>0.0.0.0</Data><Data Name='ModuleTimeStamp'>";
    x += modStamp;
    x += "</Data><Data Name='ExceptionCode'>";
    x += code;
    x += "</Data><Data Name='FaultingOffset'>";
    x += offset;
    x += "</Data><Data Name='ProcessId'>";
    x += pid;
    x += "</Data><Data Name='ProcessCreationTime'>";
    x += created;
    x += "</Data><Data Name='AppPath'>C:\\Users\\someone\\build\\";
    x += appName;
    x += "</Data><Data Name='ModulePath'>";
    x += modulePath;
    x += "</Data><Data Name='IntegratorReportId'>725d8404-a57b-4e5e-ac1e-0138ed6a452b</Data>"
         "<Data Name='PackageFullName'></Data><Data Name='PackageRelativeAppId'></Data>"
         "</EventData></Event>";
    return x;
}

// A __fastfail in a DLL the executable loaded (probe_mod.dll!dll_failfast).
const std::string kFastFailInDll =
    captured("Application Error", "1000", "2026-10-05T21:10:45.1685267Z", "probe.exe", "6ac4119f",
             "probe_mod.dll", "6ac4119f", "c0000409", "0000000000001335", "0x79b4",
             "0x1dd550dfc7fb5de", "C:\\Users\\someone\\build\\probe_mod.dll");
// A __fastfail inline in the executable itself.
const std::string kFastFailInExe =
    captured("Application Error", "1000", "2026-10-05T21:10:42.6307131Z", "probe.exe", "6ac4119f",
             "probe.exe", "6ac4119f", "c0000409", "00000000000015e8", "0x733c", "0x1dd550dfaff6b8a",
             "C:\\Users\\someone\\build\\probe.exe");
// A /GS stack-cookie failure in the executable; a second /GS failure in a DIFFERENT
// function gave the same module and the same offset (docs/DIAGNOSTICS.md).
const std::string kStackCookieInExe =
    captured("Application Error", "1000", "2026-10-05T21:14:29.7217780Z", "probe.exe", "6ac41335",
             "probe.exe", "6ac41335", "c0000409", "0000000000001e31", "0x60c0", "0x1dd550e8257aeab",
             "C:\\Users\\someone\\build\\probe.exe");
// An access violation in the DLL.
const std::string kAccessViolationInDll =
    captured("Application Error", "1000", "2026-10-05T21:10:45.9975572Z", "probe.exe", "6ac4119f",
             "probe_mod.dll", "6ac4119f", "c0000005", "0000000000001350", "0xb308",
             "0x1dd550dfcfd2d25", "C:\\Users\\someone\\build\\probe_mod.dll");
// abort() inside the C runtime, in a different program (a fuzz replay, 2026-10-05 08:23).
const std::string kAbortInUcrt =
    captured("Application Error", "1000", "2026-10-05T15:23:19.5975028Z", "fuzz_corpus_replay.exe",
             "6ac3bfa0", "ucrtbase.dll", "10c46e71", "c0000409", "000000000007f6fe", "0x2fbc",
             "0x1dd54dd7387f46b", "C:\\Windows\\System32\\ucrtbase.dll");

OsCrashQuery queryOf(unsigned long pid, std::uint64_t created, unsigned long code) {
    OsCrashQuery q;
    q.pid = pid;
    q.startFileTime = created;
    q.endFileTime = created + 50'000'000ull;  // five seconds later
    q.exitCode = code;
    return q;
}
const OsCrashQuery kQueryDll = queryOf(0x79b4, 0x1dd550dfc7fb5deull, 0xC0000409ul);

// The same event with the named fields replaced, dropped or repeated.
struct Edit {
    std::string field;
    std::string value;  // replacement ("" with drop = true removes the element)
    bool drop = false;
    bool twice = false;
};
std::string edited(std::string xml, const Edit& e) {
    const std::string open = "<Data Name='" + e.field + "'>";
    const std::size_t at = xml.find(open);
    if (at == std::string::npos) { return xml; }
    const std::size_t end = xml.find("</Data>", at) + 7;
    const std::string element = open + e.value + "</Data>";
    if (e.drop) {
        xml.erase(at, end - at);
    } else if (e.twice) {
        xml.replace(at, end - at, xml.substr(at, end - at) + element);
    } else {
        xml.replace(at, end - at, element);
    }
    return xml;
}
std::string withField(const std::string& field, const std::string& value) {
    return edited(kFastFailInDll, Edit{field, value});
}
std::string withoutField(const std::string& field) {
    return edited(kFastFailInDll, Edit{field, "", true});
}

bool accepted(const std::string& xml, const OsCrashQuery& q) {
    OsCrashLocation l;
    return parseApplicationErrorEvent(xml, q, l);
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// A source that hands out a fixed list, newest first, until `each` says stop.
OsEventSource listSource(std::vector<std::string> events, std::shared_ptr<std::atomic<int>> calls,
                         std::shared_ptr<std::string> xpathSeen = nullptr) {
    return [events = std::move(events), calls, xpathSeen](
               const std::string& xpath, const std::function<bool(const std::string&)>& each,
               const std::atomic<bool>& cancel) {
        if (xpathSeen) { *xpathSeen = xpath; }
        for (const std::string& e : events) {
            if (cancel.load()) { return; }
            if (calls) { calls->fetch_add(1); }
            if (each(e)) { return; }
        }
    };
}

#if defined(_WIN32)
// ===========================================================================
// THE REAL CHILD. Plays its part when this program is started as
//   test_os_crash_record.exe --child failfast | exit
// ===========================================================================
int child(const char* what) {
    // WER_FAULT_REPORTING_NO_UI and NOT SEM_NOGPFAULTERRORBOX: see the header of this file.
    ::WerSetFlags(WER_FAULT_REPORTING_NO_UI);
    if (std::strcmp(what, "failfast") == 0) {
        __fastfail(7);  // FAST_FAIL_FATAL_APP_EXIT: what abort() ends in, and no handler runs
    }
    return 0;
}

struct ChildEnd {
    bool ok = false;
    unsigned long pid = 0;
    unsigned long exitCode = 0;
    std::uint64_t created = 0;
    std::uint64_t ended = 0;
};

std::uint64_t ticks(const FILETIME& f) {
    return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

std::wstring selfPathW() {
    std::wstring p(1024, L'\0');
    p.resize(::GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size())));
    return p;
}

// Runs this program as the child, waits for it, and reads what a handle to it says.
ChildEnd runChild(const char* what) {
    ChildEnd r;
    std::wstring cmd = L"\"" + selfPathW() + L"\" --child ";
    for (const char* c = what; *c != '\0'; ++c) { cmd.push_back(static_cast<wchar_t>(*c)); }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                          nullptr, &si, &pi)) {
        return r;
    }
    ::CloseHandle(pi.hThread);
    if (::WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) {
        FILETIME c{}, x{}, k{}, u{};
        DWORD code = 0;
        if (::GetProcessTimes(pi.hProcess, &c, &x, &k, &u) && ::GetExitCodeProcess(pi.hProcess, &code)) {
            r.ok = true;
            r.pid = pi.dwProcessId;
            r.exitCode = code;
            r.created = ticks(c);
            r.ended = ticks(x);
        }
    } else {
        ::TerminateProcess(pi.hProcess, 99);
    }
    ::CloseHandle(pi.hProcess);
    return r;
}

std::string selfFileName() {
    std::string p(1024, '\0');
    p.resize(::GetModuleFileNameA(nullptr, p.data(), static_cast<DWORD>(p.size())));
    const std::size_t cut = p.find_last_of("\\/");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}

unsigned long selfImageSize() {
    const auto* base = reinterpret_cast<const unsigned char*>(::GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

bool tokenElevated() {
    HANDLE t = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &t)) { return false; }
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = ::GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n) != 0;
    ::CloseHandle(t);
    return ok && e.TokenIsElevated != 0;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && ::_stricmp(a.c_str(), b.c_str()) == 0;
}
#endif  // _WIN32

[[maybe_unused]] void skipNote(const char* why) {
    ++g_checksSkipped;
    std::printf("SKIP: %s\n", why);
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc >= 3 && std::strcmp(argv[1], "--child") == 0) { return child(argv[2]); }
#else
    (void)argc;
    (void)argv;
#endif

    // =======================================================================
    // 1. THE MODULE NAME: a plain file name from a conservative set, or nothing
    // =======================================================================
    {
        for (const char* ok : {"cascade.exe", "probe.exe", "probe_mod.dll", "ucrtbase.dll",
                               "KERNELBASE.dll", "nvoglv64.dll", "libusb-1.0.dll", "Qt6Core.dll",
                               "vcruntime140_1.dll", "a", "_x", "7z.dll", "gl+.dll"}) {
            CHECK(osCrashModuleNameOk(ok));
        }
        // 63 characters is the longest; 64 is one too many.
        CHECK(osCrashModuleNameOk(std::string(59, 'a') + ".dll"));
        CHECK(!osCrashModuleNameOk(std::string(60, 'a') + ".dll"));
        CHECK(!osCrashModuleNameOk(std::string(300, 'a')));
        volatile std::size_t nameMax = kOsCrashModuleNameMax;  // volatile, so the comparison is a runtime one
        CHECK(nameMax == 63);
        for (const char* bad : {
                 "",
                 "C:\\Windows\\System32\\ucrtbase.dll",
                 "\\ucrtbase.dll",
                 "System32\\ucrtbase.dll",
                 "/usr/lib/libc.so",
                 "..\\..\\evil.dll",
                 "..",
                 ".",
                 ".hidden",
                 "-dash.dll",
                 "a b.dll",              // a space is outside the conservative set
                 "a(b).dll",
                 "a;b.dll",
                 "a&amp;b.dll",
                 "a<b>.dll",
                 "a'b.dll",
                 "a\"b.dll",
                 "a:b.dll",
                 "a*.dll",
                 "caf\xC3\xA9.dll",      // UTF-8 for e acute
                 "caf\xE9.dll",          // the same letter in Windows-1252
                 "unknown",
                 "UNKNOWN",
                 "Unknown",
                 "StackHash_0a9e",
                 "stackhash_0a9e",
             }) {
            CHECK(!osCrashModuleNameOk(bad));
        }
        // control characters of every kind
        for (int c = 0; c < 32; ++c) {
            CHECK(!osCrashModuleNameOk(std::string("a") + static_cast<char>(c) + "b.dll"));
        }
        CHECK(!osCrashModuleNameOk(std::string("a\0b.dll", 7)));
        CHECK(!osCrashModuleNameOk("a\x7f.dll"));
        // a name that merely CONTAINS one of the two refused words is a name
        CHECK(osCrashModuleNameOk("unknown_driver.dll"));
        CHECK(osCrashModuleNameOk("MyStackHash_0.dll"));
    }

    // =======================================================================
    // 2. THE OFFSET: hex digits, a module's worth of them
    // =======================================================================
    {
        std::uint64_t v = 99;
        CHECK(osCrashOffsetOk("000000000007f6fe", v) && v == 0x7f6feull);
        CHECK(osCrashOffsetOk("7F6FE", v) && v == 0x7f6feull);
        CHECK(osCrashOffsetOk("0", v) && v == 0);
        CHECK(osCrashOffsetOk("00000000ffffffff", v) && v == 0xffffffffull);
        // a raw address (what Windows writes when no module holds the fault), or too long
        CHECK(!osCrashOffsetOk("0000000100000000", v));
        CHECK(!osCrashOffsetOk("00007ff612345678", v));
        CHECK(!osCrashOffsetOk("00000000000000000", v));  // 17 digits
        for (const char* bad : {"", "0x7f6fe", "0X7F6FE", "zz12", "12zz", "-5", "+5", "12 34", " 12",
                                "12 ", "1.5", "\t1", "12\n", "\xEF\xBC\x91"}) {
            CHECK(!osCrashOffsetOk(bad, v));
        }
        CHECK(!osCrashOffsetOk(std::string("12\0", 3), v));
        // a rejected offset leaves nothing behind
        v = 1234;
        CHECK(!osCrashOffsetOk("nope", v));
        CHECK(v == 1234);
    }

    // =======================================================================
    // 3. THE PARSER, against captured events
    // =======================================================================
    {
        OsCrashLocation l;
        // The fast-fail in the DLL: the DLL's file name, and the offset in it.
        CHECK(parseApplicationErrorEvent(kFastFailInDll, kQueryDll, l));
        CHECK(l.module == "probe_mod.dll" && l.offset == 0x1335);
        // The fast-fail inline in the executable: the executable's.
        l = OsCrashLocation();
        CHECK(parseApplicationErrorEvent(kFastFailInExe, queryOf(0x733c, 0x1dd550dfaff6b8aull, 0xC0000409ul), l));
        CHECK(l.module == "probe.exe" && l.offset == 0x15e8);
        // A /GS failure: the executable, and the offset of the shared failure stub.
        l = OsCrashLocation();
        CHECK(parseApplicationErrorEvent(kStackCookieInExe, queryOf(0x60c0, 0x1dd550e8257aeabull, 0xC0000409ul), l));
        CHECK(l.module == "probe.exe" && l.offset == 0x1e31);
        // An access violation, with ITS code.
        l = OsCrashLocation();
        CHECK(parseApplicationErrorEvent(kAccessViolationInDll, queryOf(0xb308, 0x1dd550dfcfd2d25ull, 0xC0000005ul), l));
        CHECK(l.module == "probe_mod.dll" && l.offset == 0x1350);
        // The CRT's own abort, a system DLL.
        l = OsCrashLocation();
        CHECK(parseApplicationErrorEvent(kAbortInUcrt, queryOf(0x2fbc, 0x1dd54dd7387f46bull, 0xC0000409ul), l));
        CHECK(l.module == "ucrtbase.dll" && l.offset == 0x7f6fe);

        // NOTHING OF A PATH IS KEPT: the result is a name and a number.
        CHECK(l.module.find('\\') == std::string::npos && !contains(l.module, "Users") &&
              !contains(l.module, "someone"));

        // A rejected event leaves the output untouched.
        OsCrashLocation keep;
        keep.module = "kept.dll";
        keep.offset = 7;
        CHECK(!parseApplicationErrorEvent(kFastFailInDll, queryOf(0x1, 0x2, 0xC0000409ul), keep));
        CHECK(keep.module == "kept.dll" && keep.offset == 7);
    }

    // =======================================================================
    // 4. WHICH DEATH: an event about anything else is never attached
    // =======================================================================
    {
        // another program's event (different process id), and another's creation time
        CHECK(accepted(kFastFailInDll, kQueryDll));
        CHECK(!accepted(kFastFailInDll, queryOf(0x79b5, 0x1dd550dfc7fb5deull, 0xC0000409ul)));
        CHECK(!accepted(kFastFailInDll, queryOf(0x79b4 + 0x10000, 0x1dd550dfc7fb5deull, 0xC0000409ul)));
        // THE SAME PROCESS ID, AN EARLIER RUN: ids are reused, creation times are not
        CHECK(!accepted(kFastFailInDll, queryOf(0x79b4, 0x1dd550dfc7fb5ddull, 0xC0000409ul)));
        CHECK(!accepted(kFastFailInDll, queryOf(0x79b4, 0x1dd550dfc7fb5dfull, 0xC0000409ul)));
        CHECK(!accepted(kFastFailInDll, queryOf(0x79b4, 0x1dd550dfc7fb5deull + 10'000'000ull, 0xC0000409ul)));
        // a different exception than the process exited with
        CHECK(!accepted(kFastFailInDll, queryOf(0x79b4, 0x1dd550dfc7fb5deull, 0xC0000005ul)));
        CHECK(!accepted(kFastFailInDll, queryOf(0x79b4, 0x1dd550dfc7fb5deull, 0)));
        // nothing known about the death: nothing is attached, not even on the pid alone
        CHECK(!accepted(kFastFailInDll, queryOf(0x79b4, 0, 0xC0000409ul)));
        CHECK(!accepted(kFastFailInDll, queryOf(0, 0x1dd550dfc7fb5deull, 0xC0000409ul)));
        CHECK(!accepted(kFastFailInDll, OsCrashQuery()));
        // an event for ANOTHER EXECUTABLE entirely, asked about with its own numbers it
        // is a good event; asked about with ours it is not
        CHECK(accepted(kAbortInUcrt, queryOf(0x2fbc, 0x1dd54dd7387f46bull, 0xC0000409ul)));
        CHECK(!accepted(kAbortInUcrt, kQueryDll));
        // the id and the creation time compared as NUMBERS, not as text
        CHECK(accepted(withField("ProcessId", "0x079B4"), kQueryDll));
        CHECK(accepted(withField("ProcessId", "0X79B4"), kQueryDll));
        CHECK(accepted(withField("ProcessCreationTime", "0x01DD550DFC7FB5DE"), kQueryDll));
        CHECK(accepted(withField("ExceptionCode", "C0000409"), kQueryDll));
        CHECK(accepted(withField("ExceptionCode", "0xC0000409"), kQueryDll));
        // ...and not by prefix or by substring
        CHECK(!accepted(withField("ProcessId", "0x79b4x"), kQueryDll));
        CHECK(!accepted(withField("ProcessId", "79b4"), kQueryDll));        // no prefix
        CHECK(!accepted(withField("ProcessId", "31156"), kQueryDll));       // decimal is not hex
        CHECK(!accepted(withField("ProcessId", "0x79b4 0x1"), kQueryDll));
        CHECK(!accepted(withField("ProcessCreationTime", "0x1dd550dfc7fb5de0"), kQueryDll));
        CHECK(!accepted(withField("ProcessCreationTime", "1dd550dfc7fb5de"), kQueryDll));
        CHECK(!accepted(withField("ExceptionCode", "c00004090"), kQueryDll));
        CHECK(!accepted(withField("ExceptionCode", ""), kQueryDll));

        // another provider, another id: not this event, however well the rest matches
        CHECK(!accepted(captured("Windows Error Reporting", "1000", "2026-10-05T21:10:45.1685267Z",
                                 "probe.exe", "6ac4119f", "probe_mod.dll", "6ac4119f", "c0000409",
                                 "0000000000001335", "0x79b4", "0x1dd550dfc7fb5de", "x"),
                        kQueryDll));
        CHECK(!accepted(captured("Application Error", "1001", "2026-10-05T21:10:45.1685267Z",
                                 "probe.exe", "6ac4119f", "probe_mod.dll", "6ac4119f", "c0000409",
                                 "0000000000001335", "0x79b4", "0x1dd550dfc7fb5de", "x"),
                        kQueryDll));
        CHECK(!accepted(captured("Application Error", "10000", "2026-10-05T21:10:45.1685267Z",
                                 "probe.exe", "6ac4119f", "probe_mod.dll", "6ac4119f", "c0000409",
                                 "0000000000001335", "0x79b4", "0x1dd550dfc7fb5de", "x"),
                        kQueryDll));
        CHECK(!accepted(captured("Application Hang", "1000", "2026-10-05T21:10:45.1685267Z",
                                 "probe.exe", "6ac4119f", "probe_mod.dll", "6ac4119f", "c0000409",
                                 "0000000000001335", "0x79b4", "0x1dd550dfc7fb5de", "x"),
                        kQueryDll));
    }

    // =======================================================================
    // 5. HOSTILE EVENTS: what is kept is validated, and anything else is dropped
    // =======================================================================
    {
        // a PATH where a file name should be
        CHECK(!accepted(withField("ModuleName", "C:\\Windows\\System32\\ucrtbase.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "C:\\Users\\someone\\build\\probe.exe"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "\\\\server\\share\\x.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "..\\..\\evil.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "../evil.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "sub/dir.dll"), kQueryDll));
        // separators, markup and escapes in a name
        CHECK(!accepted(withField("ModuleName", "a&amp;b.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "a&lt;b&gt;.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "a&#92;b.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "a&#x1;b.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "a b.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "name.dll "), kQueryDll));
        CHECK(!accepted(withField("ModuleName", " name.dll"), kQueryDll));
        // control characters, raw in the text
        CHECK(!accepted(withField("ModuleName", std::string("a\x01" "b.dll")), kQueryDll));
        CHECK(!accepted(withField("ModuleName", std::string("a\r\nb.dll")), kQueryDll));
        CHECK(!accepted(withField("ModuleName", std::string("a\tb.dll")), kQueryDll));
        CHECK(!accepted(withField("ModuleName", std::string("a\0b.dll", 7)), kQueryDll));
        // over-long
        CHECK(accepted(withField("ModuleName", std::string(59, 'a') + ".dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", std::string(60, 'a') + ".dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", std::string(5000, 'a') + ".dll"), kQueryDll));
        // empty, and the two words Windows writes when it could not name the module
        CHECK(!accepted(withField("ModuleName", ""), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "unknown"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "StackHash_0a9e"), kQueryDll));
        // not ASCII
        CHECK(!accepted(withField("ModuleName", "caf\xC3\xA9.dll"), kQueryDll));
        CHECK(!accepted(withField("ModuleName", "\xE2\x80\xAEgnp.dll"), kQueryDll));  // right-to-left override

        // non-hex offsets, a prefix, a sign, a space, a raw address, a blank
        for (const char* bad : {"zz12", "12zz", "0x1335", "-1335", "+1335", "13 35", "1335 ", " 1335",
                                "", "13.35", "00007ff612345678", "0000000100000000",
                                "00000000000000001335", "&#49;335"}) {
            CHECK(!accepted(withField("FaultingOffset", bad), kQueryDll));
        }
        // a path is not an offset either
        CHECK(!accepted(withField("FaultingOffset", "C:\\x"), kQueryDll));

        // a MISSING field: each of the five is needed
        for (const char* gone : {"ModuleName", "FaultingOffset", "ExceptionCode", "ProcessId",
                                 "ProcessCreationTime"}) {
            CHECK(!accepted(withoutField(gone), kQueryDll));
        }
        // ...and the ones that are not needed are not
        for (const char* gone : {"AppName", "AppVersion", "AppTimeStamp", "ModuleVersion",
                                 "ModuleTimeStamp", "AppPath", "ModulePath", "IntegratorReportId",
                                 "PackageFullName", "PackageRelativeAppId"}) {
            CHECK(accepted(withoutField(gone), kQueryDll));
        }
        // a field named TWICE is ambiguous, whichever of the two is the right one
        for (const char* dup : {"ModuleName", "FaultingOffset", "ExceptionCode", "ProcessId",
                                "ProcessCreationTime"}) {
            const std::string good = dup == std::string("ModuleName")           ? "probe_mod.dll"
                                     : dup == std::string("FaultingOffset")     ? "0000000000001335"
                                     : dup == std::string("ExceptionCode")      ? "c0000409"
                                     : dup == std::string("ProcessId")          ? "0x79b4"
                                                                                : "0x1dd550dfc7fb5de";
            CHECK(!accepted(edited(kFastFailInDll, Edit{dup, good, false, true}), kQueryDll));
        }
        // No Name attributes (an older layout): nothing is guessed from position.
        {
            std::string unnamed = kFastFailInDll;
            for (std::size_t at = unnamed.find("<Data Name='"); at != std::string::npos;
                 at = unnamed.find("<Data Name='", at)) {
                const std::size_t close = unnamed.find("'>", at);
                unnamed.replace(at, close + 2 - at, "<Data>");
            }
            CHECK(!accepted(unnamed, kQueryDll));
        }
        // The path fields can say anything, including something that looks like a field:
        // they are never read, so they can neither supply a value nor spoil one.
        {
            const std::string decoy = withField(
                "AppPath", "C:\\x\\&lt;Data Name='ModuleName'&gt;evil.dll&lt;/Data&gt;"
                           "&lt;Data Name='FaultingOffset'&gt;1&lt;/Data&gt;");
            OsCrashLocation l;
            CHECK(parseApplicationErrorEvent(decoy, kQueryDll, l));
            CHECK(l.module == "probe_mod.dll" && l.offset == 0x1335);
            const std::string longPath = withField("AppPath", std::string(200000, 'p'));
            CHECK(accepted(longPath, kQueryDll));
        }
        // a truncated event, an empty one, one that is not an event at all
        const std::size_t cut = kFastFailInDll.find("<Data Name='FaultingOffset'>");
        CHECK(cut != std::string::npos);
        CHECK(!accepted(kFastFailInDll.substr(0, cut), kQueryDll));
        CHECK(!accepted(kFastFailInDll.substr(0, cut + 30), kQueryDll));
        CHECK(!accepted(kFastFailInDll.substr(0, kFastFailInDll.size() / 2), kQueryDll));
        CHECK(!accepted(std::string(), kQueryDll));
        CHECK(!accepted("not xml at all", kQueryDll));
        CHECK(!accepted("<Event></Event>", kQueryDll));
        CHECK(!accepted(std::string(1000000, '<'), kQueryDll));
        // an element left open must not run on into the next one
        CHECK(!accepted(edited(kFastFailInDll, Edit{"ModuleName", "probe_mod.dll<Data Name='X'>"}), kQueryDll));
        // double quotes round the attributes are fine
        {
            std::string dq = kFastFailInDll;
            for (std::size_t at = dq.find("Name='"); at != std::string::npos; at = dq.find("Name='", at)) {
                const std::size_t close = dq.find('\'', at + 6);
                dq[at + 5] = '"';
                dq[close] = '"';
                at = close;
            }
            // (the Provider's attribute is a Name too, and was turned over with the rest)
            CHECK(accepted(dq, kQueryDll));
        }
    }

    // =======================================================================
    // 6. THE QUERY: this provider, this event id, a window - and nothing about us
    // =======================================================================
    {
        CHECK(osCrashEventXPath(60000) ==
              "*[System[Provider[@Name='Application Error'] and EventID=1000 and "
              "TimeCreated[timediff(@SystemTime) <= 60000]]]");
        CHECK(osCrashEventXPath(1) !=  osCrashEventXPath(2));

        constexpr std::uint64_t kSec = 10'000'000ull;
        const std::uint64_t now = 1'000'000ull * kSec;
        OsCrashQuery q;
        q.pid = 4242;
        q.startFileTime = now - 5 * kSec;
        q.endFileTime = now - 1 * kSec;
        // from the start to now, and a second of slack
        CHECK(osCrashWindowMs(q, now) == 5000 + 1000);
        // no end known: it ends now
        q.endFileTime = 0;
        q.startFileTime = now - 3 * kSec;
        CHECK(osCrashWindowMs(q, now) == 3000 + 1000);
        // an application that ran for two days is asked about the last half hour before its end
        q.startFileTime = now - 2 * 24 * 3600 * kSec;
        q.endFileTime = now - 1 * kSec;
        CHECK(osCrashWindowMs(q, now) == kOsCrashWindowMaxMs + 1000 + 1000);
        // a clock that put the start after now: the slack alone, never a wrapped number
        q.startFileTime = now + 100 * kSec;
        q.endFileTime = now + 101 * kSec;
        CHECK(osCrashWindowMs(q, now) == 1000);
        // an application that ran for under half an hour is asked about all of it
        q.startFileTime = now - 600 * kSec;
        q.endFileTime = now - 2 * kSec;
        CHECK(osCrashWindowMs(q, now) == 600'000 + 1000);
    }

    // =======================================================================
    // 7. THE LOOKUP, against a fixture source
    // =======================================================================
    {
        // The matching event is the third of four; the fourth is never asked for.
        auto calls = std::make_shared<std::atomic<int>>(0);
        auto seen = std::make_shared<std::string>();
        const OsEventSource src = listSource(
            {kAbortInUcrt,                                                    // another program's
             edited(kFastFailInDll, Edit{"ProcessCreationTime", "0x1dd54000000000a"}),  // same id, an earlier run
             kFastFailInDll,                                                  // ours
             kStackCookieInExe},                                              // newer-looking, not reached
            calls, seen);
        const OsCrashLookup r = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000), src);
        CHECK(r.found && !r.timedOut);
        CHECK(r.location.module == "probe_mod.dll" && r.location.offset == 0x1335);
        CHECK(calls->load() == 3);
        // what the source was asked: this provider and id and a window; nothing of ours
        CHECK(seen->rfind("*[System[Provider[@Name='Application Error'] and EventID=1000 and "
                          "TimeCreated[timediff(@SystemTime) <= ", 0) == 0);
        CHECK(!contains(*seen, "79b4") && !contains(*seen, "31156") && !contains(*seen, "ProcessId") &&
              !contains(*seen, "probe"));

        // Nothing matches: not found, and the source was not read to the end of a long log.
        {
            std::vector<std::string> many(200, kAbortInUcrt);
            auto n = std::make_shared<std::atomic<int>>(0);
            const OsCrashLookup none = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000),
                                                         listSource(many, n));
            CHECK(!none.found && !none.timedOut);
            volatile int cap = kOsCrashMaxEventsExamined;
            CHECK(n->load() == cap);
            CHECK(cap == 64);
        }
        // The matching event is past the cap: not attached (it is among the newest, or it is not there).
        {
            std::vector<std::string> many(kOsCrashMaxEventsExamined, kAbortInUcrt);
            many.push_back(kFastFailInDll);
            const OsCrashLookup late = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000),
                                                         listSource(many, nullptr));
            CHECK(!late.found);
        }
        // An empty log, and an event that cannot be read.
        CHECK(!findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000), listSource({}, nullptr)).found);
        CHECK(!findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000),
                                 listSource({"", "garbage", "<Event/>"}, nullptr)).found);
        // Nothing known about the death: the source is not even asked.
        {
            auto n = std::make_shared<std::atomic<int>>(0);
            OsCrashQuery unknown = kQueryDll;
            unknown.startFileTime = 0;
            CHECK(!findOsCrashRecord(unknown, std::chrono::milliseconds(2000), listSource({kFastFailInDll}, n)).found);
            unknown = kQueryDll;
            unknown.pid = 0;
            CHECK(!findOsCrashRecord(unknown, std::chrono::milliseconds(2000), listSource({kFastFailInDll}, n)).found);
            CHECK(!findOsCrashRecord(kQueryDll, std::chrono::milliseconds(0), listSource({kFastFailInDll}, n)).found);
            CHECK(n->load() == 0);
        }
        // A source that throws: not found, and the program carries on.
        {
            const OsEventSource thrower = [](const std::string&, const std::function<bool(const std::string&)>&,
                                             const std::atomic<bool>&) { throw std::runtime_error("log gone"); };
            const OsCrashLookup t = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000), thrower);
            CHECK(!t.found && !t.timedOut);
        }
        // A callback that throws is the source's problem and the lookup's silence.
        {
            const OsEventSource badEach = [](const std::string&, const std::function<bool(const std::string&)>& each,
                                             const std::atomic<bool>&) {
                each(std::string(10, '\xff'));
                throw 5;
            };
            CHECK(!findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000), badEach).found);
        }
    }

    // =======================================================================
    // 8. THE TIME BOUND: a log reader that never answers costs its budget, no more
    // =======================================================================
    {
        struct Gate {
            std::mutex m;
            std::condition_variable cv;
            bool open = false;
            std::atomic<bool> entered{false};
            std::atomic<bool> sawCancel{false};
            std::atomic<bool> left{false};
        };
        auto gate = std::make_shared<Gate>();
        // A source that is stuck in a call that cannot be interrupted, and ignores `cancel`.
        const OsEventSource stuck = [gate](const std::string&, const std::function<bool(const std::string&)>&,
                                           const std::atomic<bool>&) {
            gate->entered = true;
            std::unique_lock<std::mutex> lk(gate->m);
            gate->cv.wait(lk, [&] { return gate->open; });
            gate->left = true;
        };
        const auto t0 = Clock::now();
        const OsCrashLookup r = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(200), stuck);
        const double took = msSince(t0);
        std::printf("stalled source: the caller returned after %.0f ms (budget 200)\n", took);
        CHECK(!r.found);
        CHECK(r.timedOut);
        CHECK(took >= 180.0);   // it did wait for its budget
        CHECK(took < 700.0);    // and not for the source
        CHECK(gate->entered.load());
        CHECK(!gate->left.load());  // the source really was still stuck when the caller came back
        // let the stuck thread go, so the process ends tidily
        {
            std::lock_guard<std::mutex> lk(gate->m);
            gate->open = true;
        }
        gate->cv.notify_all();
        for (int i = 0; i < 200 && !gate->left.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK(gate->left.load());

        // A source that does listen for cancel is told, once the budget has run out.
        auto g2 = std::make_shared<Gate>();
        const OsEventSource polite = [g2](const std::string&, const std::function<bool(const std::string&)>&,
                                          const std::atomic<bool>& cancel) {
            g2->entered = true;
            while (!cancel.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            g2->sawCancel = true;
        };
        const auto t1 = Clock::now();
        const OsCrashLookup r2 = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(150), polite);
        CHECK(!r2.found && r2.timedOut);
        CHECK(msSince(t1) < 700.0);
        for (int i = 0; i < 200 && !g2->sawCancel.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        CHECK(g2->sawCancel.load());

        // A slow source that DOES finish inside the budget is waited for and used.
        const OsEventSource slow = [](const std::string&, const std::function<bool(const std::string&)>& each,
                                      const std::atomic<bool>&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            each(kFastFailInDll);
        };
        const OsCrashLookup r3 = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000), slow);
        CHECK(r3.found && !r3.timedOut);
        // ...and one that finishes only AFTER the budget is not used, even though it found it.
        const OsEventSource late = [](const std::string&, const std::function<bool(const std::string&)>& each,
                                      const std::atomic<bool>&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            each(kFastFailInDll);
        };
        const OsCrashLookup r4 = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(100), late);
        CHECK(!r4.found && r4.timedOut);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let it finish before the process ends
    }

    // =======================================================================
    // 8b. RETRIES: the event is not always readable at the first query
    // =======================================================================
    // Measured on the real log: Windows stamps the event BEFORE the process object is
    // signalled, but in 24 of 40 real deaths a query made the instant the watcher woke
    // found nothing, and the event appeared 40-372 ms later.
    {
        auto asks = std::make_shared<std::atomic<int>>(0);
        const OsEventSource onFourth = [asks](const std::string&, const std::function<bool(const std::string&)>& each,
                                              const std::atomic<bool>&) {
            if (asks->fetch_add(1) + 1 >= 4) { each(kFastFailInDll); }
        };
        // With a retry interval: asked again until it shows.
        const auto t0 = Clock::now();
        const OsCrashLookup r = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000), onFourth,
                                                  std::chrono::milliseconds(20));
        CHECK(r.found && !r.timedOut);
        CHECK(r.attempts == 4);
        CHECK(asks->load() == 4);
        CHECK(msSince(t0) >= 3 * 20 - 5.0);  // it waited between the asks...
        CHECK(msSince(t0) < 1000.0);         // ...and not for the budget
        CHECK(r.location.module == "probe_mod.dll" && r.location.offset == 0x1335);
        // Without one (the default): asked once.
        asks->store(0);
        const OsCrashLookup once = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000), onFourth);
        CHECK(!once.found && !once.timedOut && once.attempts == 1 && asks->load() == 1);
        // Never there: asked again and again until the budget, then not at all.
        auto n = std::make_shared<std::atomic<int>>(0);
        const OsEventSource nothing = [n](const std::string&, const std::function<bool(const std::string&)>&,
                                          const std::atomic<bool>&) { n->fetch_add(1); };
        const auto t1 = Clock::now();
        const OsCrashLookup none = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(200), nothing,
                                                     std::chrono::milliseconds(20));
        const double took = msSince(t1);
        CHECK(!none.found && none.timedOut);
        CHECK(took >= 190.0 && took < 600.0);
        CHECK(none.attempts >= 4 && none.attempts <= 11);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const int settled = n->load();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(n->load() == settled);  // the worker stopped asking once the caller gave up
        // A wrong event on every attempt never turns into a right one.
        auto w = std::make_shared<std::atomic<int>>(0);
        const OsEventSource wrong = [w](const std::string&, const std::function<bool(const std::string&)>& each,
                                        const std::atomic<bool>&) {
            w->fetch_add(1);
            each(kAbortInUcrt);
        };
        const OsCrashLookup nope = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(120), wrong,
                                                     std::chrono::milliseconds(20));
        CHECK(!nope.found && w->load() >= 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

#if defined(_WIN32)
    // =======================================================================
    // 9. THE REAL THING: a child that really fails fast, and the event Windows wrote
    // =======================================================================
    {
        std::printf("this test process is %s\n", tokenElevated() ? "ELEVATED (the unelevated read is NOT shown by this run)"
                                                                 : "not elevated (an ordinary, unelevated read)");

        // A child that exits normally: no event, nothing found, and no wait for one.
        const ChildEnd calm = runChild("exit");
        CHECK(calm.ok && calm.exitCode == 0);
        if (calm.ok) {
            const auto t0 = Clock::now();
            const OsCrashLookup none = findOsCrashRecord(queryOf(calm.pid, calm.created, 0), std::chrono::milliseconds(3000));
            std::printf("a normal exit: lookup took %.0f ms\n", msSince(t0));
            CHECK(!none.found && !none.timedOut);
            // not even by pretending it crashed
            CHECK(!findOsCrashRecord(queryOf(calm.pid, calm.created, 0xC0000409ul), std::chrono::milliseconds(3000)).found);
        }

        const ChildEnd bad = runChild("failfast");
        CHECK(bad.ok);
        std::printf("the child failed fast: pid %lu, exit code 0x%08lX\n", bad.pid, bad.exitCode);
        CHECK(bad.exitCode == 0xC0000409ul);
        if (bad.ok) {
            OsCrashQuery q;
            q.pid = bad.pid;
            q.startFileTime = bad.created;
            q.endFileTime = bad.ended;
            q.exitCode = bad.exitCode;
            // THE SENTINEL'S MOMENT: asked the instant the child is gone, with the retry the
            // sentinel uses, and with no witness's help.
            const auto t0 = Clock::now();
            const OsCrashLookup r = findOsCrashRecord(q, std::chrono::milliseconds(3000), OsEventSource(),
                                                      kOsCrashRetryInterval);
            const double lookupMs = msSince(t0);
            // THE WITNESS, asked only if the reader found nothing: did this machine write an
            // event at all?
            const bool witnessed = r.found || oracle::applicationErrorEventFor(bad.pid, 20000);
            std::printf("the death's event: found=%d after %.0f ms (%d asks), witnessed=%d\n", r.found ? 1 : 0,
                        lookupMs, r.attempts, witnessed ? 1 : 0);
            if (!witnessed) {
                skipNote("Windows wrote no Application Error event for the child that failed fast within "
                         "20 s (Windows Error Reporting disabled by a policy, or the Application log "
                         "disabled or full): the reader cannot be exercised on this machine");
                CHECK(!r.found);  // and it must then not invent one
            } else {
                CHECK(r.found);   // Windows wrote it, so the reader must have found it
                CHECK(!r.timedOut);
                CHECK(iequals(r.location.module, selfFileName()));
                std::printf("the location: %s+0x%llX (image size 0x%lX)\n", r.location.module.c_str(),
                            static_cast<unsigned long long>(r.location.offset), selfImageSize());
                // PLAUSIBLE: inside the image of the very program that failed
                CHECK(r.location.offset > 0 && r.location.offset < selfImageSize());

                // THE SAME EVENT, ASKED ABOUT SOMEBODY ELSE'S DEATH, is not attached:
                OsCrashQuery other = q;
                other.pid = bad.pid + 4;  // another process id
                CHECK(!findOsCrashRecord(other, std::chrono::milliseconds(3000)).found);
                other = q;
                other.startFileTime = q.startFileTime - 1;  // the same id, an earlier run
                CHECK(!findOsCrashRecord(other, std::chrono::milliseconds(3000)).found);
                other = q;
                other.exitCode = 0xC0000005ul;  // a different exception
                CHECK(!findOsCrashRecord(other, std::chrono::milliseconds(3000)).found);
                // the calm child's numbers: that death has no event
                if (calm.ok) {
                    OsCrashQuery c = queryOf(calm.pid, calm.created, 0xC0000409ul);
                    CHECK(!findOsCrashRecord(c, std::chrono::milliseconds(3000)).found);
                }
            }
        }
    }
#else
    SKIP_LINUX("Windows' crash record is Windows-only; findOsCrashRecord answers 'not known' here");
    {
        // The stub: nothing is read, nothing is found, and nothing waits.
        const OsCrashLookup r = findOsCrashRecord(kQueryDll, std::chrono::milliseconds(2000));
        CHECK(!r.found && !r.timedOut);
        CHECK(!windowsApplicationLogSource());
    }
#endif

    return testSummary("test_os_crash_record");
}
