// The other software's DLLs in the process, named in the diagnostic log (0.99.69).
//
// Two field deaths were fast-fails "in present" and "in render" - the hand-off to the
// graphics driver, where overlay DLLs that other programs inject into every process
// hook in - and the reports said nothing about what ELSE was loaded. These tests hold
// the four things that have to be true for the new lines to be worth reading:
//
//   - the CLASSIFIER calls a module foreign exactly when it is under none of the roots
//     (the program's folder, Windows', the SDR vendors', the C runtime's own files), on
//     paths that exist on no machine of ours, in every spelling Windows gives a path;
//   - what reaches a report is a FILE NAME and nothing else, in the exact line formats the
//     documents promise, within the widths the log and the report allow;
//   - an ARRIVAL is caught by the REAL loader notification in THIS process: a DLL that is
//     not under the test's folder or Windows' is copied to a scratch folder and loaded,
//     and exactly that name is written; a Windows DLL loaded the same way writes nothing;
//     an unload writes nothing; the second load of a name writes nothing; the queue's two
//     limits (its size and the session's arrival lines) say so when they bite;
//   - the FALLBACK, taken when the registration fails, finds the same arrival by rescanning.
//
// The classifier and the words are portable and run everywhere; everything that loads a
// DLL is Windows only and SKIPs by name elsewhere. The stand-in for another program's
// overlay is tests/fixtures/late_fault_dll.cpp, a DLL that is mapped only when something
// loads it and does nothing when it is - which is all an injected hook has to be here.
// The path of the built fixture arrives as argv[1].
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/foreign_modules.hpp"
#include "core/plugin_host.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The ring's lines (the log's own memory of what was written, oldest first) that carry `key`.
std::vector<std::string> ringLinesWith(const std::string& key) {
    std::vector<std::string> out;
    for (const std::string& l : DiagLog::instance().ringSnapshot()) {
        if (contains(l, key)) { out.push_back(l); }
    }
    return out;
}

// The roots the synthetic paths below are judged against.
ForeignRoots fakeRoots() {
    ForeignRoots r;
    r.programFolder = L"C:\\Program Files\\FoxSDR";
    r.windowsFolder = L"C:\\WINDOWS";
    r.vendorFolders = {L"C:\\Users\\steve\\radioconda\\Library\\lib\\SoapySDR\\modules0.8",
                       L"C:\\Users\\steve\\radioconda\\Library\\bin"};
    r.runtimeFiles = {L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Redist\\vcruntime140.dll"};
    return r;
}

// ---------------------------------------------------------------------------
// The classifier
// ---------------------------------------------------------------------------
void classifierCases() {
    const ForeignRoots r = fakeRoots();
    auto foreign = [&r](const wchar_t* p) { return isForeignModule(p, r); };

    // WINDOWS' OWN, in every folder it keeps code in, in the case Windows writes and in others.
    CHECK(!foreign(L"C:\\WINDOWS\\System32\\kernel32.dll"));
    CHECK(!foreign(L"C:\\Windows\\SysWOW64\\wow64.dll"));
    CHECK(!foreign(L"C:\\WINDOWS\\WinSxS\\amd64_microsoft.windows.common-controls_6595b64144ccf1df_6.0.22621.4036_none_270f70857386168e\\comctl32.dll"));
    CHECK(!foreign(L"c:\\windows\\system32\\ntdll.dll"));
    CHECK(!foreign(L"C:\\WINDOWS\\SYSTEM32\\USER32.DLL"));

    // OURS: the executable, a plugin in a subfolder, a vcpkg DLL beside it, whatever its case.
    CHECK(!foreign(L"C:\\Program Files\\FoxSDR\\cascade.exe"));
    CHECK(!foreign(L"C:\\Program Files\\FoxSDR\\plugins\\adsb\\adsb.dll"));
    CHECK(!foreign(L"C:\\Program Files\\FoxSDR\\SoapySDR.dll"));
    CHECK(!foreign(L"c:\\PROGRAM FILES\\foxsdr\\CASCADE.EXE"));

    // THE SDR VENDORS': their module folder and the bin folder their modules' DLLs live in.
    CHECK(!foreign(L"C:\\Users\\steve\\radioconda\\Library\\lib\\SoapySDR\\modules0.8\\rtlsdrSupport.dll"));
    CHECK(!foreign(L"C:\\Users\\steve\\radioconda\\Library\\bin\\libusb-1.0.dll"));
    // ...and nothing beside them: a sibling folder whose name merely starts the same.
    CHECK(foreign(L"C:\\Users\\steve\\radioconda\\Library\\bin2\\hook.dll"));

    // THE C RUNTIME, by the exact path the loader reports - a file of that name elsewhere is not it.
    CHECK(!foreign(L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Redist\\vcruntime140.dll"));
    CHECK(foreign(L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Redist\\evil.dll"));
    CHECK(foreign(L"C:\\Elsewhere\\vcruntime140.dll"));

    // FOREIGN: the motivating case, and a path under a user's profile.
    CHECK(foreign(L"C:\\Program Files\\Nahimic\\NahimicOSD.dll"));
    CHECK(foreign(L"C:\\Users\\steve\\AppData\\Local\\Overwolf\\Extensions\\hook64.dll"));
    CHECK(foreign(L"D:\\WINDOWS\\System32\\kernel32.dll"));  // another drive is not Windows'

    // THE SPELLINGS A PATH COMES IN. \\?\ and \??\ and forward slashes name the same file...
    CHECK(!foreign(L"\\\\?\\C:\\WINDOWS\\system32\\ntdll.dll"));
    CHECK(!foreign(L"\\??\\C:\\WINDOWS\\system32\\ntdll.dll"));
    CHECK(!foreign(L"C:/WINDOWS/system32/ntdll.dll"));
    CHECK(!foreign(L"\\\\?\\c:\\program files\\foxsdr\\plugins\\x.dll"));
    CHECK(foreign(L"\\\\?\\C:\\Program Files\\Nahimic\\NahimicOSD.dll"));
    // ...and a `..` cannot be used to walk out of, or into, a root.
    CHECK(foreign(L"C:\\Program Files\\FoxSDR\\..\\Nahimic\\x.dll"));
    CHECK(!foreign(L"C:\\Program Files\\FoxSDR\\..\\FoxSDR\\plugins\\x.dll"));
    CHECK(!foreign(L"C:\\Program Files\\Nahimic\\..\\FoxSDR\\x.dll"));
    CHECK(!foreign(L"C:\\WINDOWS\\.\\system32\\\\x.dll"));  // `.` and a doubled separator
    // A folder that merely begins with a root's name is not beneath it.
    CHECK(foreign(L"C:\\Program Files\\FoxSDR2\\x.dll"));
    CHECK(foreign(L"C:\\WINDOWS2\\x.dll"));
    // A network share is its own root, so it is foreign.
    CHECK(foreign(L"\\\\?\\UNC\\server\\share\\x.dll"));
    CHECK(foreign(L"\\\\server\\share\\x.dll"));

    // WHAT A STRING CANNOT KNOW. An 8.3 short name is not expanded here (the Windows half
    // expands a path that carries a tilde, with the file system, before it asks) - so the
    // pure classifier says foreign for the short spelling of a folder that is ours. This
    // is the stated limit, pinned so that nobody mistakes the classifier for the whole job.
    CHECK(foreign(L"C:\\PROGRA~1\\FoxSDR\\x.dll"));

    // NOT A MODULE, and NOT SOMETHING TO TRUST.
    CHECK(!foreign(L""));              // nothing to name
    CHECK(foreign(L"NahimicOSD.dll"));  // not absolute: it cannot be shown to be ours
    CHECK(foreign(L"..\\x.dll"));

    // A root that names a whole drive would turn the classifier off: it names nothing.
    ForeignRoots drive = r;
    drive.programFolder = L"C:\\";
    CHECK(isForeignModule(L"C:\\Nahimic\\x.dll", drive));
    drive.programFolder = L"C:";
    CHECK(isForeignModule(L"C:\\Nahimic\\x.dll", drive));
    drive.programFolder = L"\\\\server\\share";
    CHECK(isForeignModule(L"\\\\server\\share\\x.dll", drive));
    ForeignRoots empty;  // no roots at all: nothing can be shown to be ours
    CHECK(isForeignModule(L"C:\\WINDOWS\\System32\\kernel32.dll", empty));

    // The names the snapshot lists: only the foreign ones, each once, alphabetical.
    const std::vector<std::wstring> loaded = {
        L"C:\\Program Files\\FoxSDR\\cascade.exe",
        L"C:\\WINDOWS\\System32\\kernel32.dll",
        L"C:\\Program Files\\RTSS\\RTSSHooks64.dll",
        L"C:\\Program Files\\Nahimic\\NahimicOSD.dll",
        L"D:\\Other\\nahimicosd.dll",  // the same name from another folder: listed once
        L"C:\\Users\\steve\\radioconda\\Library\\bin\\libusb-1.0.dll"};
    const std::vector<std::string> names = foreignNamesOf(loaded, r);
    CHECK(names.size() == 2);
    CHECK(names.size() == 2 && names[0] == "NahimicOSD.dll" && names[1] == "RTSSHooks64.dll");
}

// ---------------------------------------------------------------------------
// A name, and only a name
// ---------------------------------------------------------------------------
void nameCases() {
    CHECK(fileNameOnly(L"C:\\Program Files\\Nahimic\\NahimicOSD.dll") == "NahimicOSD.dll");
    CHECK(fileNameOnly(L"\\\\?\\C:\\x\\y.dll") == "y.dll");
    CHECK(fileNameOnly(L"C:/a/b.dll") == "b.dll");
    CHECK(fileNameOnly(L"bare.dll") == "bare.dll");
    CHECK(fileNameOnly(L"C:\\Program Files\\Nahimic\\").empty());  // names nothing
    CHECK(fileNameOnly(L"").empty());
    CHECK(fileNameOnly(L"C:\\x\\..").empty());  // a folder reference is not a file
    CHECK(fileNameOnly(L"C:\\x\\.").empty());
    // A character that is not a plain file-name character is a '?', one per code point
    // (a surrogate pair is one), and a control character is one too.
    CHECK(fileNameOnly(L"D\u00e9j\u00e0.dll") == "D?j?.dll");
    CHECK(fileNameOnly(L"x\U0001F600y.dll") == "x?y.dll");
    CHECK(fileNameOnly(L"a\tb.dll") == "a?b.dll");
    CHECK(fileNameOnly(L"a,b.dll") == "a?b.dll");  // the list's own separator
    CHECK(fileNameOnly(L"it's.dll") == "it?s.dll");  // a quote would be rewritten by the log's scrub
    CHECK(fileNameOnly(L"  padded.dll ") == "padded.dll");

    // The cut: at most 63 bytes, however long the name.
    const std::wstring longName(200, L'a');
    CHECK(fileNameOnly(L"C:\\x\\" + longName + L".dll").size() == kForeignNameMaxBytes);

    // THE PROPERTY, over inputs chosen to break it: whatever goes in, no separator comes out,
    // and what comes out passes the reader's own validator.
    const wchar_t* nasty[] = {L"C:\\a\\b\\c.dll",   L"\\\\?\\UNC\\s\\sh\\n.dll", L"a\\\\b.dll",
                              L"/",                 L"\\",                       L"x/y\\z.dll",
                              L"C:\\folder\\..\\..", L"\u202Eevil.dll",          L"a:b.dll",
                              L"a|b<c>d.dll",       L"a\"b.dll",                 L"..",
                              L"."};
    for (const wchar_t* n : nasty) {
        const std::string out = fileNameOnly(n);
        CHECK(out.find('\\') == std::string::npos);
        CHECK(out.find('/') == std::string::npos);
        CHECK(out.empty() || isPlainModuleName(out));
    }

    CHECK(isPlainModuleName("NahimicOSD.dll"));
    CHECK(isPlainModuleName("libusb-1.0.dll"));
    CHECK(isPlainModuleName("My Overlay (x64).dll"));
    CHECK(!isPlainModuleName(""));
    CHECK(!isPlainModuleName(" lead.dll"));
    CHECK(!isPlainModuleName("trail.dll "));
    CHECK(!isPlainModuleName("a\\b.dll"));
    CHECK(!isPlainModuleName("C:\\x.dll"));
    CHECK(!isPlainModuleName("a,b.dll"));
    CHECK(!isPlainModuleName("a\nb.dll"));
    CHECK(!isPlainModuleName(std::string(kForeignNameMaxBytes + 1, 'a')));
    CHECK(isPlainModuleName(std::string(kForeignNameMaxBytes, 'a')));
}

// ---------------------------------------------------------------------------
// The words
// ---------------------------------------------------------------------------
std::vector<std::string> manyNames(std::size_t n, std::size_t width) {
    std::vector<std::string> v;
    for (std::size_t i = 0; i < n; ++i) {
        char idx[16];
        std::snprintf(idx, sizeof(idx), "%03zu", i);
        std::string name = "Mod" + std::string(idx);
        while (name.size() + 4 < width) { name.push_back('x'); }
        v.push_back(name + ".dll");
    }
    return v;
}

void wordCases() {
    // The order and the folding.
    const std::vector<std::string> sorted = sortedUniqueNames({"b.dll", "A.dll", "a.DLL", "C.dll"});
    CHECK(sorted.size() == 3);
    CHECK(sorted.size() == 3 && sorted[0] == "A.dll" && sorted[1] == "b.dll" && sorted[2] == "C.dll");

    // THE START LINE, in the exact form PRIVACY.md and the documentation show.
    CHECK(foreignStartLineText({}) == "modules: none foreign");
    CHECK(foreignStartLineText({"NahimicOSD.dll", "RTSSHooks64.dll"}) ==
          "modules: 2 foreign - NahimicOSD.dll, RTSSHooks64.dll");
    CHECK(foreignStartLineText({"One.dll"}) == "modules: 1 foreign - One.dll");

    // ...and its cap. Forty names of thirty characters do not fit in 150: as many as do,
    // alphabetical from the front, then ", +K more", and the line never passes the cap.
    {
        const std::vector<std::string> names = manyNames(40, 30);
        const std::string line = foreignStartLineText(names);
        CHECK(line.size() <= kForeignStartLineMaxChars);
        CHECK(line.rfind("modules: 40 foreign - ", 0) == 0);
        const std::size_t more = line.rfind(", +");
        CHECK(more != std::string::npos);
        // The listed names are the alphabetical front of the list, and the K that is left
        // out is exactly the ones not listed. (No name is a part of another: they are the
        // same width and differ in their number.)
        const std::string head = line.substr(0, more);
        std::size_t listed = 0;
        while (listed < names.size() && contains(head, names[listed])) { ++listed; }
        for (std::size_t i = listed; i < names.size(); ++i) { CHECK(!contains(head, names[i])); }
        CHECK(listed > 0 && listed < names.size());
        // (Not substr(npos): a line with no tail is a failed check, not an exception that ends the run.)
        CHECK((more == std::string::npos ? std::string() : line.substr(more)) ==
              ", +" + std::to_string(names.size() - listed) + " more");
        CHECK(contains(line, names[0]));
        CHECK(!contains(line, names.back()));
    }
    // The boundary: a list that exactly fits has no "+K more"; one name more does.
    {
        // "modules: 3 foreign - " is 21 characters; three names of 43, 43 and 39 fill 150.
        std::vector<std::string> fit = {std::string(39, 'a') + ".dll", std::string(39, 'b') + ".dll",
                                        std::string(35, 'c') + ".dll"};
        const std::string line = foreignStartLineText(fit);
        CHECK(line.size() == 21 + 43 + 2 + 43 + 2 + 39);
        CHECK(line.size() == kForeignStartLineMaxChars);
        CHECK(!contains(line, "more"));
        fit[2] = std::string(36, 'c') + ".dll";  // one character more: the third no longer fits
        const std::string over = foreignStartLineText(fit);
        CHECK(over.size() <= kForeignStartLineMaxChars);
        CHECK(contains(over, ", +1 more"));
        CHECK(contains(over, "modules: 3 foreign - "));
        CHECK(!contains(over, "ccc"));
    }
    // The widest single name still lists itself.
    {
        const std::string wide = foreignStartLineText({std::string(kForeignNameMaxBytes, 'w')});
        CHECK(wide == "modules: 1 foreign - " + std::string(kForeignNameMaxBytes, 'w'));
        CHECK(wide.size() <= kForeignStartLineMaxChars);
    }

    // THE ARRIVAL LINE.
    CHECK(foreignArrivalLineText("NahimicOSD.dll", 12.34) == "module arrived: NahimicOSD.dll (12.3 s)");
    CHECK(foreignArrivalLineText("x.dll", 0.04) == "module arrived: x.dll (0.0 s)");
    CHECK(foreignArrivalLineText("x.dll", 3600.96) == "module arrived: x.dll (3601.0 s)");
    CHECK(foreignArrivalLineText("x.dll", -5.0) == "module arrived: x.dll (0.0 s)");

    // THE CONTEXT VALUE: the names, alphabetical, and its own cap.
    CHECK(foreignFieldText({}) == "(none)");
    CHECK(foreignFieldText({"a.dll", "b.dll"}) == "a.dll, b.dll");
    CHECK(foreignFieldText({"a.dll"}, 3) == "a.dll, +3 more");
    CHECK(foreignFieldText({}, 2) == "+2 more");
    {
        const std::vector<std::string> names = manyNames(60, 30);
        const std::string v = foreignFieldText(names);
        CHECK(v.size() <= kForeignFieldMaxChars);
        CHECK(contains(v, names[0]));
        CHECK(v.find(", +") != std::string::npos);
        CHECK(v.size() > kForeignStartLineMaxChars);  // the report carries more than a log line can
    }

    // WHAT A READER CARRIES ON. A list is re-rendered from the names that are plain file
    // names; anything else in it is dropped, not repaired; the sentences that say nothing
    // about the modules become EMPTY; "(none)" is the one that says "none".
    CHECK(normaliseForeignField("NahimicOSD.dll, RTSSHooks64.dll") == "NahimicOSD.dll, RTSSHooks64.dll");
    CHECK(normaliseForeignField("RTSSHooks64.dll, NahimicOSD.dll, nahimicosd.dll") ==
          "NahimicOSD.dll, RTSSHooks64.dll");  // sorted, unique
    CHECK(normaliseForeignField("a.dll, +4 more") == "a.dll, +4 more");
    CHECK(normaliseForeignField("(none)") == "(none)");
    CHECK(normaliseForeignField("(not recorded)").empty());
    CHECK(normaliseForeignField("(not scanned yet)").empty());
    CHECK(normaliseForeignField("(not applicable)").empty());
    CHECK(normaliseForeignField("").empty());
    CHECK(normaliseForeignField("C:\\Users\\steve\\evil.dll, good.dll") == "good.dll");  // the path is dropped
    CHECK(normaliseForeignField("C:\\Users\\steve\\evil.dll").empty());
    CHECK(normaliseForeignField("a\nb.dll, ok.dll") == "ok.dll");
    CHECK(normaliseForeignField("+7 more") == "+7 more");

    // THE LOG, READ BACK. The start line, then arrivals, as the log file holds them.
    auto stamped = [](const std::string& msg) { return "12:34:56.789 info " + msg; };
    {
        const std::vector<std::string> log = {
            stamped("FoxSDR 0.99.69 (abc123def456) starting"),
            stamped("modules: 2 foreign - NahimicOSD.dll, RTSSHooks64.dll"),
            stamped("source opened"),
            stamped("module arrived: ZetaOverlay.dll (12.3 s)"),
            stamped("module arrived: ZetaOverlay.dll (99.0 s)"),  // the same name twice
            stamped("module arrived: rtsshooks64.DLL (99.0 s)")};  // the same name in another case
        const ForeignList l = foreignListFromLog(log);
        CHECK(l.known);
        CHECK(l.names.size() == 3);
        CHECK(l.names.size() == 3 && l.names[0] == "NahimicOSD.dll" && l.names[1] == "RTSSHooks64.dll" &&
              l.names[2] == "ZetaOverlay.dll");
        CHECK(foreignFieldFromLog(log) == "NahimicOSD.dll, RTSSHooks64.dll, ZetaOverlay.dll");
    }
    CHECK(foreignFieldFromLog({stamped("modules: none foreign")}) == "(none)");
    CHECK(foreignFieldFromLog({stamped("modules: none foreign"), stamped("module arrived: A.dll (1.0 s)")}) == "A.dll");
    // The start line's own "+K more" is carried, because those names are not in the log.
    CHECK(foreignFieldFromLog({stamped("modules: 9 foreign - a.dll, b.dll, +7 more")}) == "a.dll, b.dll, +7 more");
    // A log with neither line says so; lines not in the log's own shape, or that only look
    // like ours, are not read as ours (a driver's stderr line is "vendor: ...").
    CHECK(foreignFieldFromLog({}) == "(not recorded)");
    CHECK(foreignFieldFromLog({stamped("source opened")}) == "(not recorded)");
    CHECK(foreignFieldFromLog({stamped("vendor: modules: 1 foreign - evil.dll")}) == "(not recorded)");
    CHECK(foreignFieldFromLog({"modules: 1 foreign - evil.dll"}) == "(not recorded)");
    // A name that is not a plain file name is dropped, never carried: a path in the start line
    // (a hand-edited log) does not reach the report.
    CHECK(foreignFieldFromLog({stamped("modules: 1 foreign - C:\\x\\evil.dll")}) == "(none)");
    CHECK(!contains(foreignFieldFromLog({stamped("module arrived: C:\\x\\evil.dll (1.0 s)")}), "evil"));
    CHECK(isForeignModulesLogLine(stamped("modules: none foreign")));
    CHECK(isForeignModulesLogLine(stamped("module arrived: A.dll (1.0 s)")));
    CHECK(!isForeignModulesLogLine(stamped("module watch: the loader notification is not available")));
    CHECK(!isForeignModulesLogLine(stamped("module arrivals missed: 3 (x)")));
    CHECK(!isForeignModulesLogLine(stamped("module arrivals: 32 logged, the rest are listed")));
    CHECK(!isForeignModulesLogLine(stamped("plugin: loaded modules: 3")));
    CHECK(!isForeignModulesLogLine("modules: none foreign"));

    // THE LINES SURVIVE THE UPLOAD SCRUB UNCHANGED, whatever digits the names carry: the
    // log's rule 8 masks numbers on a line that mentions a frequency or is cut at the ring's
    // width, and neither is true of these. (The lines are the ones the documents quote.)
    {
        const std::vector<std::string> names = {"Nahimic2UILauncher.dll", "NahimicOSD.dll", "RTSSHooks64.dll",
                                                "WavesLib64.dll",        "nvspcap64.dll",   "GameOverlayRenderer64.dll"};
        const std::string start = stamped(foreignStartLineText(sortedUniqueNames(names)));
        CHECK(scrubUploadLine(start) == start);
        const std::string arrived = stamped(foreignArrivalLineText("ToolsetHook64.dll", 4321.5));
        CHECK(scrubUploadLine(arrived) == arrived);
        // At the widest the message can be, the whole line is still under the ring's width,
        // which is what keeps it from being treated as a cut line (every number masked).
        const std::string widest = stamped(foreignStartLineText(manyNames(60, 40)));
        CHECK(widest.size() < static_cast<std::size_t>(DiagLog::kLineBytes) - 1u);
        CHECK(scrubUploadLine(widest) == widest);
    }

    // ...AND THEY REACH THE LOG WHOLE: through the real log, at the widest, no cut.
    {
        DiagLog::instance().resetForTest();
        const std::string msg = foreignStartLineText(manyNames(60, 40));
        DiagLog::instance().write("info", msg.c_str());
        const std::vector<std::string> ring = DiagLog::instance().ringSnapshot();
        CHECK(ring.size() == 1);
        CHECK(ring.size() == 1 && ring[0].size() > 18 && ring[0].substr(18) == msg);
        CHECK(ring.size() == 1 && isForeignModulesLogLine(ring[0]));
        DiagLog::instance().resetForTest();
    }
}

#if defined(_WIN32)

fs::path scratchDir(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    const fs::path dir = base / (std::string("cascade-foreign-") + tag + "-" +
                                 std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Polls the watch the way the frame loop does until `done()` or the time is up.
template <class Done>
bool pollUntil(ForeignModuleWatch& w, unsigned timeoutMs, Done done) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        w.poll();
        if (done()) { return true; }
        sleepMs(10);
    }
    w.poll();
    return done();
}

bool waitScanned(const ForeignModuleWatch& w, unsigned timeoutMs = 20000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < until) {
        if (w.scanned()) { return true; }
        sleepMs(5);
    }
    return w.scanned();
}

// A system DLL this process has not loaded, to load for the Windows-folder case.
std::wstring unloadedSystemDll() {
    static const wchar_t* const kCandidates[] = {L"msftedit.dll", L"mpr.dll",   L"wldap32.dll",
                                                 L"cabinet.dll",  L"tapi32.dll", L"wintrust.dll",
                                                 L"dbghelp.dll",  L"slc.dll"};
    wchar_t sys[MAX_PATH] = {};
    ::GetSystemDirectoryW(sys, MAX_PATH);
    for (const wchar_t* c : kCandidates) {
        if (::GetModuleHandleW(c) == nullptr) { return std::wstring(sys) + L"\\" + c; }
    }
    return std::wstring();
}

std::vector<std::string> arrivalNames() {
    std::vector<std::string> out;
    for (const std::string& l : ringLinesWith("module arrived: ")) {
        const std::size_t at = l.find("module arrived: ");
        const std::size_t end = l.rfind(" (");
        if (at != std::string::npos && end != std::string::npos && end > at + 16) {
            out.push_back(l.substr(at + 16, end - at - 16));
        }
    }
    return out;
}

void realProcessCases(const std::string& fixture) {
    // THE REAL PROCESS, through the real Windows calls: what is loaded, and the roots.
    {
        const std::vector<std::wstring> paths = loadedModulePaths();
        CHECK(paths.size() > 5);
        const ForeignRoots roots = currentForeignRoots();
        CHECK(!roots.programFolder.empty());
        CHECK(!roots.windowsFolder.empty());

        wchar_t exe[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        CHECK(!isForeignModule(exe, roots));  // this test program is ours
        wchar_t k32[MAX_PATH] = {};
        ::GetModuleFileNameW(::GetModuleHandleW(L"kernel32.dll"), k32, MAX_PATH);
        CHECK(!isForeignModule(k32, roots));  // and kernel32 is Windows'

        // What is foreign in this process, whatever that is on this machine: printed, and
        // never one of the modules every process has.
        const std::vector<std::string> foreign = snapshotForeignModules();
        std::string joined;
        for (const std::string& n : foreign) { joined += (joined.empty() ? "" : ", ") + n; }
        std::printf("this test process's own foreign modules: %s\n", foreign.empty() ? "(none)" : joined.c_str());
        CHECK(std::find(foreign.begin(), foreign.end(), "kernel32.dll") == foreign.end());
        CHECK(std::find(foreign.begin(), foreign.end(), "ntdll.dll") == foreign.end());
    }

    if (fixture.empty() || !fs::exists(fixture)) {
        std::printf("SKIP: no fixture DLL was given (argv[1]); the real arrival cases did not run\n");
        ++g_checksSkipped;
        return;
    }

    const fs::path scratch = scratchDir("arrive");
    std::error_code ec;
    fs::copy_file(fixture, scratch / "ForeignProbe.dll", fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    const std::wstring probePath = (scratch / "ForeignProbe.dll").wstring();

    // THE START SCAN SEES A MODULE THAT WAS ALREADY LOADED: a foreign DLL loaded BEFORE the
    // watch starts is in the start line, through the real EnumProcessModulesEx.
    {
        fs::copy_file(fixture, scratch / "ForeignEarly.dll", fs::copy_options::overwrite_existing, ec);
        HMODULE early = ::LoadLibraryW((scratch / "ForeignEarly.dll").wstring().c_str());
        CHECK(early != nullptr);
        const std::vector<std::string> snap = snapshotForeignModules();
        CHECK(std::find(snap.begin(), snap.end(), "ForeignEarly.dll") != snap.end());

        DiagLog::instance().resetForTest();
        ForeignModuleWatch watch;
        const auto t0 = std::chrono::steady_clock::now();
        watch.start();
        const double startReturnedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(waitScanned(watch));
        const double scannedMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        // What start-up pays: start() itself returns at once (the scan is on a thread of its
        // own), and the whole scan - the registration, the module list, the classification
        // and the log line - is a few milliseconds beside it.
        std::printf("start(): returned in %.2f ms; the scan and its line were done %.1f ms after\n",
                    startReturnedMs, scannedMs);
        CHECK(startReturnedMs < 250.0);
        const std::vector<std::string> start = ringLinesWith("modules: ");
        CHECK(start.size() == 1);
        if (start.size() == 1) {
            std::printf("start line: %s\n", start[0].c_str());
            CHECK(contains(start[0], "ForeignEarly.dll"));
            CHECK(contains(start[0], " foreign - "));
            CHECK(isForeignModulesLogLine(start[0]));
        }
        CHECK(contains(watch.contextValue(), "ForeignEarly.dll"));
        CHECK(watch.contextValue() != "(not scanned yet)");
        // It is NOT an arrival: the notification was registered before the scan, the module
        // loaded before both, and the drain never writes what the scan already listed.
        CHECK(watch.usingNotification());
        for (int i = 0; i < 10; ++i) { watch.poll(); sleepMs(5); }
        CHECK(ringLinesWith("module arrived: ").empty());
        if (early != nullptr) { ::FreeLibrary(early); }
    }

    // THE REAL ARRIVAL, by the real loader notification.
    {
        DiagLog::instance().resetForTest();
        ForeignModuleWatch watch;
        watch.start();
        CHECK(waitScanned(watch));
        CHECK(watch.usingNotification());
        CHECK(ringLinesWith("module arrived: ").empty());

        HMODULE h = ::LoadLibraryW(probePath.c_str());
        CHECK(h != nullptr);
        const bool saw = pollUntil(watch, 5000, [] { return !arrivalNames().empty(); });
        CHECK(saw);
        // EXACTLY THAT NAME, once, written after the start line.
        const std::vector<std::string> arrived = arrivalNames();
        CHECK(arrived.size() == 1);
        CHECK(arrived.size() == 1 && arrived[0] == "ForeignProbe.dll");
        const std::vector<std::string> lines = ringLinesWith("module arrived: ");
        if (lines.size() == 1) {
            std::printf("arrival line: %s\n", lines[0].c_str());
            const std::size_t open = lines[0].rfind(" (");
            const double secs = std::atof(lines[0].c_str() + open + 2);
            CHECK(secs >= 0.0 && secs < 600.0);
            CHECK(lines[0].size() > 4 && lines[0].compare(lines[0].size() - 3, 3, " s)") == 0);
        }
        // The start line is first in the log.
        {
            const std::vector<std::string> ring = DiagLog::instance().ringSnapshot();
            std::size_t startAt = ring.size();
            std::size_t arrivedAt = ring.size();
            for (std::size_t i = 0; i < ring.size(); ++i) {
                if (startAt == ring.size() && contains(ring[i], "modules: ")) { startAt = i; }
                if (arrivedAt == ring.size() && contains(ring[i], "module arrived: ")) { arrivedAt = i; }
            }
            CHECK(startAt < arrivedAt);
        }
        // The context value says it, and keeps saying it after the module is gone.
        CHECK(contains(watch.contextValue(), "ForeignProbe.dll"));

        // AN UNLOAD WRITES NOTHING.
        const std::size_t before = DiagLog::instance().linesWritten();
        if (h != nullptr) { CHECK(::FreeLibrary(h) != 0); }
        for (int i = 0; i < 20; ++i) { watch.poll(); sleepMs(10); }
        CHECK(DiagLog::instance().linesWritten() == before);
        CHECK(contains(watch.contextValue(), "ForeignProbe.dll"));

        // A SECOND LOAD OF A NAME WE HAVE SEEN WRITES NOTHING: once a session.
        h = ::LoadLibraryW(probePath.c_str());
        CHECK(h != nullptr);
        for (int i = 0; i < 20; ++i) { watch.poll(); sleepMs(10); }
        CHECK(arrivalNames().size() == 1);
        if (h != nullptr) { ::FreeLibrary(h); }

        // A WINDOWS DLL, loaded the same way, WRITES NOTHING AND IS NOT LISTED.
        const std::wstring sys = unloadedSystemDll();
        if (sys.empty()) {
            std::printf("SKIP: every candidate system DLL was already loaded; the Windows-folder case did not run\n");
            ++g_checksSkipped;
        } else {
            HMODULE sh = ::LoadLibraryW(sys.c_str());
            CHECK(sh != nullptr);
            const std::size_t beforeSys = DiagLog::instance().linesWritten();
            for (int i = 0; i < 30; ++i) { watch.poll(); sleepMs(10); }
            CHECK(DiagLog::instance().linesWritten() == beforeSys);
            CHECK(!contains(watch.contextValue(), fileNameOnly(sys)));
            if (sh != nullptr) { ::FreeLibrary(sh); }
        }
    }

    // A DLL UNDER A FOLDER THE APPLICATION REGISTERED AS ITS OWN (an SDR vendor's, the
    // plugins') WRITES NOTHING, though it is outside the program's folder.
    {
        const fs::path own = scratch / "own";
        fs::create_directories(own, ec);
        fs::copy_file(fixture, own / "VendorSupport.dll", fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        DiagLog::instance().resetForTest();
        ForeignModuleWatch watch;
        watch.start();
        CHECK(waitScanned(watch));
        registerOwnModuleFolder(own.string());
        registerOwnModuleFolder(own.string());  // idempotent
        const ForeignRoots roots = currentForeignRoots();
        std::size_t registered = 0;
        for (const std::wstring& v : roots.vendorFolders) {
            if (v.size() >= own.wstring().size() &&
                _wcsicmp(v.c_str(), own.wstring().c_str()) == 0) {
                ++registered;
            }
        }
        CHECK(registered == 1);
        HMODULE h = ::LoadLibraryW((own / "VendorSupport.dll").wstring().c_str());
        CHECK(h != nullptr);
        for (int i = 0; i < 30; ++i) { watch.poll(); sleepMs(10); }
        CHECK(arrivalNames().empty());
        CHECK(!contains(watch.contextValue(), "VendorSupport.dll"));
        if (h != nullptr) { ::FreeLibrary(h); }
    }

    // A PLUGIN SCAN REGISTERS ITS FOLDER AS OURS: the per-user plugins folder is not under the
    // program's, and without this every plugin (and every DLL it brings) would be written to
    // the log as another program's component. An empty folder is enough - the scan registers
    // before it maps anything - and a plugin DLL in it is not loaded here.
    {
        const fs::path plugins = scratch / "plugins-folder";
        fs::create_directories(plugins, ec);
        const auto registeredCount = [&plugins] {
            std::size_t n = 0;
            for (const std::wstring& v : currentForeignRoots().vendorFolders) {
                if (_wcsicmp(v.c_str(), plugins.wstring().c_str()) == 0) { ++n; }
            }
            return n;
        };
        CHECK(registeredCount() == 0);
        PluginHost host;
        host.scan(plugins.string());
        CHECK(registeredCount() == 1);
        host.scan(plugins.string());  // a rescan does not register it twice
        CHECK(registeredCount() == 1);
        CHECK(!isForeignModule((plugins / "adsb" / "adsb.dll").wstring(), currentForeignRoots()));
    }

    // THE TWO LIMITS, SAID WHEN THEY BITE. A hundred and forty modules arrive while nothing
    // polls: the queue holds 128 and the rest are COUNTED, not lost silently; of those that
    // are read, the log writes the first 32 and then says it has stopped, once.
    {
        DiagLog::instance().resetForTest();
        ForeignModuleWatch watch;
        watch.start();
        CHECK(waitScanned(watch));
        const fs::path bulk = scratch / "bulk";
        fs::create_directories(bulk, ec);
        std::vector<HMODULE> loaded;
        for (int i = 0; i < 140; ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "ForeignBulk%03d.dll", i);
            fs::copy_file(fixture, bulk / name, fs::copy_options::overwrite_existing, ec);
            HMODULE h = ::LoadLibraryW((bulk / name).wstring().c_str());
            if (h != nullptr) { loaded.push_back(h); }
        }
        CHECK(loaded.size() == 140);
        for (int i = 0; i < 30; ++i) { watch.poll(); sleepMs(10); }
        const std::vector<std::string> missed = ringLinesWith("module arrivals missed: ");
        CHECK(missed.size() == 1);
        if (missed.size() == 1) { std::printf("queue full: %s\n", missed[0].c_str()); }
        CHECK(arrivalNames().size() == kForeignMaxArrivalLines);
        const std::vector<std::string> capped = ringLinesWith("module arrivals: ");
        CHECK(capped.size() == 1);
        if (capped.size() == 1) { std::printf("arrival-line cap: %s\n", capped[0].c_str()); }
        // The context carries what the log stopped writing.
        CHECK(watch.names().size() > kForeignMaxArrivalLines);
        for (HMODULE h : loaded) { ::FreeLibrary(h); }
    }

    // THE FALLBACK: the notification is not registered (what a refusal does) and the same
    // arrival is found by rescanning, by the same drain, written once.
    {
        DiagLog::instance().resetForTest();
        ForeignWatchOptions opts;
        opts.notification = false;
        opts.rescanMs = 100;
        ForeignModuleWatch watch(opts);
        watch.start();
        CHECK(waitScanned(watch));
        CHECK(!watch.usingNotification());
        CHECK(ringLinesWith("module watch: the loader notification is not available").size() == 1);
        fs::copy_file(fixture, scratch / "ForeignRescan.dll", fs::copy_options::overwrite_existing, ec);
        HMODULE h = ::LoadLibraryW((scratch / "ForeignRescan.dll").wstring().c_str());
        CHECK(h != nullptr);
        const bool saw = pollUntil(watch, 8000, [] { return !arrivalNames().empty(); });
        CHECK(saw);
        for (int i = 0; i < 30; ++i) { watch.poll(); sleepMs(10); }  // more rescans: still once
        CHECK(arrivalNames().size() == 1);
        CHECK(arrivalNames().size() == 1 && arrivalNames()[0] == "ForeignRescan.dll");
        CHECK(contains(watch.contextValue(), "ForeignRescan.dll"));
        if (h != nullptr) { ::FreeLibrary(h); }
    }

    // THE SHORT NAME. A module the loader reports by its 8.3 spelling is classified by what
    // it IS: the file system expands it before the roots are asked. Loaded from a copy beside
    // this program through the short spelling of its folder. Where the volume keeps no short
    // names, or the loader reports the long spelling anyway, this proves nothing about the
    // expansion and says so.
    {
        wchar_t exe[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const fs::path exeDir = fs::path(exe).parent_path();
        wchar_t shortDir[MAX_PATH] = {};
        const DWORD n = ::GetShortPathNameW(exeDir.wstring().c_str(), shortDir, MAX_PATH);
        const fs::path beside = exeDir / "ForeignShort.dll";
        fs::copy_file(fixture, beside, fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        if (n == 0 || n >= MAX_PATH || std::wstring(shortDir) == exeDir.wstring()) {
            std::printf("SKIP: this folder has no distinct 8.3 spelling; the short-name case did not run\n");
            ++g_checksSkipped;
        } else {
            DiagLog::instance().resetForTest();
            ForeignModuleWatch watch;
            watch.start();
            CHECK(waitScanned(watch));
            const std::wstring viaShort = std::wstring(shortDir) + L"\\ForeignShort.dll";
            HMODULE h = ::LoadLibraryW(viaShort.c_str());
            CHECK(h != nullptr);
            wchar_t reported[MAX_PATH] = {};
            ::GetModuleFileNameW(h, reported, MAX_PATH);
            std::printf("short-name load: the loader reports %ls\n", reported);
            for (int i = 0; i < 30; ++i) { watch.poll(); sleepMs(10); }
            CHECK(arrivalNames().empty());  // beside the program: ours, whichever spelling
            CHECK(!contains(watch.contextValue(), "ForeignShort.dll"));
            if (h != nullptr) { ::FreeLibrary(h); }
        }
        fs::remove(beside, ec);
    }

    // THE SHUTDOWN: a watch destroyed at once, a hundred times over, and a module loaded
    // after the last of them is gone does not find a callback behind it that points at
    // freed memory (the process is still alive to say so).
    for (int i = 0; i < 100; ++i) {
        ForeignModuleWatch watch;
        watch.start();
        if (i % 3 == 0) { sleepMs(1); }
    }
    {
        HMODULE h = ::LoadLibraryW(probePath.c_str());
        CHECK(h != nullptr);
        sleepMs(50);
        if (h != nullptr) { ::FreeLibrary(h); }
    }

    if (g_checksFailed == 0) { fs::remove_all(scratch, ec); }
}

#endif  // _WIN32

}  // namespace

int main(int argc, char** argv) {
    classifierCases();
    nameCases();
    wordCases();
#if defined(_WIN32)
    realProcessCases(argc > 1 ? std::string(argv[1]) : std::string());
#else
    (void)argc;
    (void)argv;
    SKIP_LINUX("the loader notification, the module snapshot and the real arrival cases are Windows only");
#endif
    return testSummary("test_foreign_modules");
}
