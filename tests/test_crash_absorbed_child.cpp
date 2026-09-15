// An absorbed CHILD-PROCESS fault must not walk this process's stack - and the
// frame capture must survive being asked to.
//
// WHAT THIS IS FOR. Field report "crash cascade.exe @ captureFramesGuarded"
// (0.96.3, Windows 10.0.22631): the SoapySDR enumeration child died - the known
// contained libusb fault under an old UHD 4.0.0, which the parent survives by
// design - and the parent then died with an access violation while writing the
// report ABOUT it. reportAbsorbedFault (crash_handler.cpp:735) -> writeReport ->
// captureFramesGuarded, on the std::async worker thread of the device scan,
// with no exception context of its own, so the capture fell through to walking
// that worker's own nearly spent stack. The same shape as B9D41A8D on 0.64.0,
// which is what the __try there was added for - and a __try cannot catch a
// stack overflow, because the exception dispatcher needs stack too.
//
// TWO PROPERTIES, tested separately because they fail separately:
//
//   1. A child fault records the child's exit code, the attempt and the reason,
//      and says in words that this process has no stack to show. It never
//      walks.
//   2. captureFramesGuarded, asked for a stack it has not been given, answers
//      with no frames instead of quietly substituting the calling thread's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
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

namespace {

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return base / (std::string("cascade-absorbed-") + tag + "-" + std::to_string(pid));
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<fs::path> filesIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file()) { out.push_back(e.path()); }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 1. The report a contained child death leaves behind.
// ---------------------------------------------------------------------------
void checkChildFaultReport() {
    const fs::path dir = scratchDir("report");
    std::error_code ec;
    fs::remove_all(dir, ec);

    // LINUX-TODO(crash-capture): reportAbsorbedChildFault() and
    // installCrashHandlers() are a documented no-op off Windows
    // (core/crash_handler.cpp ~741-901), so nothing is ever written to `dir`
    // here - every assertion below is about the CONTENT of a report that
    // cannot exist yet. Skipped as one block rather than failing forever
    // against missing infrastructure a parallel branch is adding.
#ifndef _WIN32
    SKIP_LINUX(
        "core::reportAbsorbedChildFault()/installCrashHandlers() are a no-op off "
        "Windows - no report is ever written to inspect");
    fs::remove_all(dir, ec);
    return;
#endif

    DiagContext ctx;
    ctx.version = "0.96.4-childfaulttest";
    ctx.commit = "deadbeef9999";
    ctx.os = "test";
    ctx.arch = "x64";
    ctx.mode = "WFM";
    ctx.sourceKind = "generator";
    setDiagContext(ctx);
    refreshModuleTable();

    CrashHandlerConfig cfg;
    cfg.crashDir = dir.string();
    cfg.enabled = true;
    cfg.minidump = true;  // ...and a child fault must still write none: see below
    // NEVER true in this test: the report is written in-process and the
    // application is supposed to carry on, which is the whole point of an
    // ABSORBED fault.
    cfg.exitAfterReport = false;
    installCrashHandlers(cfg);

    // ON A WORKER THREAD, because that is where it happens and where it died.
    // The report must be identical wherever it is filed from, but filing it
    // from the main thread would stage the one case that never crashed.
    std::thread worker([] {
        reportAbsorbedChildFault(
            "SDR device enumeration child process died (contained: the parent "
            "survived and re-probed)",
            0xC0000005ul, 1);
    });
    worker.join();

    const std::vector<fs::path> files = filesIn(dir);
    CHECK(files.size() == 1u);
    if (files.size() != 1u) { return; }
    const std::string text = readFile(files.front());
    std::printf("--- child-fault report: %zu bytes ---\n", text.size());

    // It is still a `kind: crash` report, deliberately: core/crash_upload.cpp
    // forwards two kinds and inventing a third would 400 on every send.
    CHECK(text.find("kind: crash") != std::string::npos);
    CHECK(text.find("enumeration child process died") != std::string::npos);
    CHECK(text.find("code: 0xC0000005") != std::string::npos);

    // THE HALF THIS TEST EXISTS FOR. No frames, and a sentence saying why -
    // not the generic "could not be walked", which would read as a failed walk
    // rather than a walk that was never attempted.
    CHECK(text.find("the fault was in a child process") != std::string::npos);
    CHECK(text.find("(no frames: the stack could not be walked)") == std::string::npos);

    // NEVER WALKED, proved by content rather than by trust: a walk of this
    // process would put this test binary in the stack section, and every frame
    // line begins with two spaces and a module name. The section must hold
    // exactly the one explanatory line.
    const std::size_t stackAt = text.find("--- stack");
    const std::size_t processAt = text.find("--- process ---");
    CHECK(stackAt != std::string::npos);
    CHECK(processAt != std::string::npos && processAt > stackAt);
    if (stackAt != std::string::npos && processAt != std::string::npos && processAt > stackAt) {
        const std::string section = text.substr(stackAt, processAt - stackAt);
        CHECK(section.find(".exe+0x") == std::string::npos);
        CHECK(section.find(".dll+0x") == std::string::npos);
    }

    // The two facts the death actually carries, and the walk's own verdict.
    CHECK(text.find("child-exit-code: 0xC0000005") != std::string::npos);
    CHECK(text.find("child-attempt: 1") != std::string::npos);
    CHECK(text.find("fault-thread-own: unknown") != std::string::npos);

    // AND NO MINIDUMP, with minidumps switched ON above: a dump of the process
    // that survived cannot document the fault, and it is the most revealing
    // thing this product can write to a disk.
    for (const fs::path& f : filesIn(dir)) {
        CHECK(f.extension() != ".dmp");
    }

    // THE HEADER INVENTORY, both ways, exactly as tests/test_crash_capture.cpp
    // holds a real fault to it. The child fields are in the PROCESS block
    // rather than the header precisely so this comparison still passes for
    // every report that is not a child death.
    const std::set<std::string> declared(crashReportFieldNames().begin(),
                                         crashReportFieldNames().end());
    std::set<std::string> emitted;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t eol = text.find('\n', pos);
        const std::size_t end = (eol == std::string::npos) ? text.size() : eol;
        const std::string line = text.substr(pos, end - pos);
        if (line.compare(0, 3, "---") == 0) { break; }
        const std::size_t colon = line.find(": ");
        if (colon != std::string::npos && colon > 0) { emitted.insert(line.substr(0, colon)); }
        pos = end + 1;
    }
    CHECK(emitted == declared);

    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// 2. The frame capture's own policy.
// ---------------------------------------------------------------------------
void checkFrameCapturePolicy() {
#if defined(_WIN32)
    // A caller that forbids its own stack gets nothing, whatever it passes.
    CHECK(captureFramesForTest(nullptr, false) == 0);

    // A SUPPLIED-BUT-EMPTY CONTEXT IS NOT "NO CONTEXT". This is the shape the
    // 0.96.3 report arrived in: pointers present, nothing in them. Falling
    // through to RtlCaptureStackBackTrace here substitutes the observing
    // thread's stack for the one that was asked for - and on an exhausted
    // worker stack that substitution is what killed the process.
    EXCEPTION_POINTERS epNullContext{};
    EXCEPTION_RECORD rec{};
    epNullContext.ExceptionRecord = &rec;
    epNullContext.ContextRecord = nullptr;
    CHECK(captureFramesForTest(&epNullContext, true) == 0);
    CHECK(captureFramesForTest(&epNullContext, false) == 0);

    CONTEXT zeroed{};
    EXCEPTION_POINTERS epZeroed{};
    epZeroed.ExceptionRecord = &rec;
    epZeroed.ContextRecord = &zeroed;
    CHECK(captureFramesForTest(&epZeroed, true) == 0);

    // ...AND THE PATH THAT LEGITIMATELY WALKS THIS THREAD STILL DOES. terminate,
    // purecall, invalid parameter and abort all arrive with no context at all
    // and the handler runs on the offending thread, so its own stack IS the
    // answer. A fix that turned those into empty stacks would have traded one
    // silent report for four.
    CHECK(captureFramesForTest(nullptr, true) > 0);
#else
    // LINUX-TODO(crash-capture): captureFramesForTest()/captureFramesGuarded()
    // exercise Windows structured-exception CONTEXT records
    // (core/crash_handler.cpp); captureFramesForTest() itself is a no-op
    // returning 0 off Windows (crash_handler.cpp ~903-912), so there is no
    // policy here yet to assert.
    SKIP_LINUX(
        "captureFramesForTest()/captureFramesGuarded() are Windows CONTEXT-record "
        "machinery and a no-op off Windows");
#endif
}

}  // namespace

int main() {
    checkFrameCapturePolicy();
    checkChildFaultReport();
    return testSummary("test_crash_absorbed_child");
}
