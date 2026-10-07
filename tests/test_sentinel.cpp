// The sentinel (0.99.64), held in-process: the breadcrumb block, the decision
// table, the words, the report, what the uploader does with it, and the one
// report per death. The processes - real applications that really die, and a
// real watcher that really sees it - are in test_sentinel_proc.cpp.
//
// WHAT THIS FILE PINS THAT NOTHING ELSE CAN:
//   - the breadcrumb's layout, field by field and offset by offset, because it is
//     a contract between two processes and a silent change makes the watcher read
//     one field as another;
//   - that the hot path allocates nothing (a replaced operator new counts), and
//     that no writer ever touches the reserved bytes, so the page cannot be made
//     to carry text;
//   - the decision table, every row and every boundary, against the watchdog's own
//     thresholds read from its own constants;
//   - that every sentence a report can carry comes from a closed vocabulary and
//     fits the 200 characters the site keeps;
//   - that PRIVACY.md and the code agree, in both directions, on every line a
//     report carries and on what is sent and what is kept here;
//   - that the uploader keeps the two local classes local, spends nothing of the
//     daily five on them, and is not crowded out by them;
//   - that a death the in-process handler reported gets no second report, and that
//     a report of a fault the application SURVIVED does not hide a death.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/breadcrumb.hpp"
#include "core/crash_handler.hpp"
#include "core/crash_upload.hpp"
#include "core/diag_history.hpp"
#include "core/diag_report.hpp"
#include "core/frame_timing.hpp"
#include "core/hang_watchdog.hpp"
#include "core/sentinel.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;
using breadcrumb::Phase;

// ---------------------------------------------------------------------------
// An allocation counter, for "the hot path allocates nothing". Replacing the
// global allocation functions in this one test program counts every allocation
// the library makes too, while the flag is up.
// ---------------------------------------------------------------------------
namespace {
std::atomic<long> g_allocs{0};
std::atomic<bool> g_counting{false};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) { g_allocs.fetch_add(1); }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

fs::path scratchDir(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    const fs::path dir = base / (std::string("cascade-sentinel-") + tag + "-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// A snapshot as the watcher would read it, `silentMs` before `nowMs`.
constexpr std::uint64_t kNow = 10'000'000;
breadcrumb::Snapshot crumb(Phase p, std::uint64_t frames, std::uint64_t silentMs,
                           std::uint32_t activity = 0, std::uint32_t flags = 0) {
    breadcrumb::Snapshot s;
    s.valid = true;
    s.startedMs = 1000;
    s.phase = p;
    s.frames = frames;
    s.activity = activity;
    s.flags = flags;
    s.phaseMs = kNow - silentMs;
    s.beatMs = frames > 0 ? kNow - silentMs : 0;
    return s;
}

SentinelFacts facts(const breadcrumb::Snapshot& c, unsigned long exitCode, bool exitKnown = true) {
    SentinelFacts f;
    f.exitKnown = exitKnown;
    f.exitCode = exitCode;
    f.crumb = c;
    f.nowMs = kNow;
    return f;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// A value the compiler cannot fold, so a CHECK of a compile-time constant (the
// layout is also static_asserted in the header) is still a runtime comparison.
template <class T>
T rt(T v) {
    volatile T x = v;
    return x;
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) { ++n; }
    return n;
}

// ---------------------------------------------------------------------------
// PRIVACY.md, parsed: the sentinel section's two tables.
// ---------------------------------------------------------------------------
std::string sentinelPrivacySection() {
    const std::string doc = readFile(fs::path(CASCADE_SOURCE_DIR) / "PRIVACY.md");
    const std::size_t a = doc.find("**A sentinel report (since 0.99.64).**");
    const std::size_t b = doc.find("**A full memory dump is off by default.**", a);
    if (a == std::string::npos || b == std::string::npos) { return std::string(); }
    return doc.substr(a, b - a);
}

std::vector<std::string> cellsOf(const std::string& line) {
    std::vector<std::string> cells;
    std::size_t p = 1;
    while (p <= line.size()) {
        const std::size_t bar = line.find('|', p);
        if (bar == std::string::npos) { break; }
        std::string c = line.substr(p, bar - p);
        const std::size_t s = c.find_first_not_of(' ');
        const std::size_t e = c.find_last_not_of(' ');
        cells.push_back(s == std::string::npos ? std::string() : c.substr(s, e - s + 1));
        p = bar + 1;
    }
    return cells;
}

std::vector<std::string> backticked(const std::string& cell, bool allowHyphen) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (true) {
        const std::size_t a = cell.find('`', at);
        if (a == std::string::npos) { break; }
        const std::size_t b = cell.find('`', a + 1);
        if (b == std::string::npos) { break; }
        const std::string tok = cell.substr(a + 1, b - a - 1);
        bool ident = !tok.empty();
        for (const char c : tok) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || (allowHyphen && c == '-');
            if (!ok) { ident = false; }
        }
        if (ident) { out.push_back(tok); }
        at = b + 1;
    }
    return out;
}

// First backticked span of a cell, whatever it holds (a sentence).
std::string firstBackticked(const std::string& cell) {
    const std::size_t a = cell.find('`');
    if (a == std::string::npos) { return std::string(); }
    const std::size_t b = cell.find('`', a + 1);
    return b == std::string::npos ? std::string() : cell.substr(a + 1, b - a - 1);
}

// "name: value" lines before the first "--- " marker.
std::set<std::string> headerLines(const std::string& text) {
    std::set<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) { eol = text.size(); }
        const std::string line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.compare(0, 3, "---") == 0) { break; }
        const std::size_t colon = line.find(": ");
        if (colon != std::string::npos && colon > 0) { out.insert(line.substr(0, colon)); }
    }
    return out;
}

// "name: value" lines of one "--- <section> ..." block.
std::set<std::string> sectionLines(const std::string& text, const std::string& marker) {
    std::set<std::string> out;
    const std::size_t at = text.find(marker);
    if (at == std::string::npos) { return out; }
    std::size_t pos = text.find('\n', at);
    while (pos != std::string::npos && pos < text.size()) {
        ++pos;
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos) { eol = text.size(); }
        const std::string line = text.substr(pos, eol - pos);
        if (line.compare(0, 3, "---") == 0) { break; }
        const std::size_t colon = line.find(": ");
        if (colon != std::string::npos && colon > 0) { out.insert(line.substr(0, colon)); }
        pos = eol;
    }
    return out;
}

// One report on disk in the in-process handler's own shape: whole header, the
// reason given, a 16-digit signature.
void writeInProcessReport(const fs::path& dir, const std::string& name, const std::string& reason) {
    writeFile(dir / name,
              "kind: crash\nreason: " + reason +
                  "\ncode: 0xC0000005\naddress: cascade.exe+0x1\nsignature: A31F00112233445A\n"
                  "thread: 1\n--- context ---\nversion: 0.99.64\n--- stack (thread 1) ---\n");
}

std::vector<fs::path> reportsIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".txt") == 0) { out.push_back(e.path()); }
    }
    return out;
}

}  // namespace

int main() {
    // =======================================================================
    // 1. THE LAYOUT, pinned
    // =======================================================================
    {
        using breadcrumb::Block;
        CHECK(rt<std::size_t>(sizeof(Block)) == 64);
        CHECK(rt<std::size_t>(offsetof(Block, magic)) == 0);
        CHECK(rt<std::size_t>(offsetof(Block, layout)) == 4);
        CHECK(rt<std::size_t>(offsetof(Block, startedMs)) == 8);
        CHECK(rt<std::size_t>(offsetof(Block, beatMs)) == 16);
        CHECK(rt<std::size_t>(offsetof(Block, frames)) == 24);
        CHECK(rt<std::size_t>(offsetof(Block, phaseMs)) == 32);
        CHECK(rt<std::size_t>(offsetof(Block, phase)) == 40);
        CHECK(rt<std::size_t>(offsetof(Block, activity)) == 44);
        CHECK(rt<std::size_t>(offsetof(Block, flags)) == 48);
        CHECK(rt<std::size_t>(offsetof(Block, frameScope)) == 52);
        CHECK(rt<std::size_t>(offsetof(Block, reserved)) == 56);
        CHECK(rt(std::is_standard_layout_v<Block>) && rt(std::is_trivially_copyable_v<Block>));
        CHECK(rt(breadcrumb::kMagic) == 0x43425846u);  // "FXBC" little-endian
        CHECK(rt(breadcrumb::kLayout) == 1u);
        // The phase numbers are part of the contract too: a value the watcher
        // does not know reads as Unset, so a renumbering would silently misname.
        CHECK(rt(static_cast<unsigned>(Phase::Starting)) == 1 && rt(static_cast<unsigned>(Phase::BuildingApp)) == 2 &&
              rt(static_cast<unsigned>(Phase::CreatingWindow)) == 3 &&
              rt(static_cast<unsigned>(Phase::AwaitingFirstFrame)) == 4 &&
              rt(static_cast<unsigned>(Phase::Running)) == 5 &&
              rt(static_cast<unsigned>(Phase::ShutdownBegun)) == 6 &&
              rt(static_cast<unsigned>(Phase::ShutdownStoppingReceiver)) == 7 &&
              rt(static_cast<unsigned>(Phase::ShutdownUnloadingPlugins)) == 8 &&
              rt(static_cast<unsigned>(Phase::ShutdownWritingMarker)) == 9 &&
              rt(static_cast<unsigned>(Phase::ShutdownClosingWindow)) == 10 &&
              rt(static_cast<unsigned>(Phase::Finished)) == 11);
        CHECK(rt(breadcrumb::kLastPhase) == 11u);
        CHECK(rt<std::uint32_t>(breadcrumb::kOpeningRadio) == 1u && rt<std::uint32_t>(breadcrumb::kReloadingPlugins) == 2u);
        CHECK(rt<std::uint32_t>(breadcrumb::kUserPaced) == 4u && rt<std::uint32_t>(breadcrumb::kAllActivity) == 7u);
        // The frame scope is written as FrameScope + 1, and the watcher names it
        // from the frame timer's own table: the numbers the two share are pinned
        // here, at the two ends and at the one the decision reads.
        CHECK(rt(static_cast<int>(FrameScope::Events)) == 0 && rt(static_cast<int>(FrameScope::UserWait)) == 17 &&
              rt(static_cast<int>(FrameScope::Other)) == 18 && rt(kFrameScopeCount) == 19);
        CHECK(rt<std::uint32_t>(breadcrumb::kFlagSessionEnding) == 1u && rt<std::uint32_t>(breadcrumb::kFlagReportsOff) == 2u);
    }

    // =======================================================================
    // 2. THE WRITER AND THE READER, in one process
    // =======================================================================
    {
        alignas(64) static breadcrumb::Block blk;
        std::memset(&blk, 0, sizeof(blk));

        // A page nobody attached reads as nothing at all.
        CHECK(!breadcrumb::read(&blk).valid);
        CHECK(!breadcrumb::read(nullptr).valid);

        breadcrumb::attach(&blk);
        breadcrumb::Snapshot s = breadcrumb::read(&blk);
        CHECK(s.valid);
        CHECK(s.phase == Phase::Starting);
        CHECK(s.frames == 0 && s.beatMs == 0 && s.activity == 0 && s.flags == 0);
        CHECK(s.startedMs > 0 && s.phaseMs >= s.startedMs);

        // The first heartbeat counts; the SECOND ends the wait for the first frame.
        breadcrumb::setPhase(Phase::AwaitingFirstFrame);
        breadcrumb::beat();
        s = breadcrumb::read(&blk);
        CHECK(s.frames == 1 && s.phase == Phase::AwaitingFirstFrame && s.beatMs > 0);
        breadcrumb::beat();
        s = breadcrumb::read(&blk);
        CHECK(s.frames == 2 && s.phase == Phase::Running);
        // ...but only from that phase: a heartbeat in any other never changes it.
        breadcrumb::setPhase(Phase::ShutdownBegun);
        breadcrumb::beat();
        CHECK(breadcrumb::read(&blk).phase == Phase::ShutdownBegun);

        // Activity bits are set and cleared on their own, and a scope guard clears.
        CHECK(breadcrumb::read(&blk).activity == 0);
        breadcrumb::setActivity(breadcrumb::kOpeningRadio, true);
        CHECK(breadcrumb::read(&blk).activity == breadcrumb::kOpeningRadio);
        {
            breadcrumb::ActivityScope scope(breadcrumb::kReloadingPlugins);
            CHECK(breadcrumb::read(&blk).activity ==
                  (breadcrumb::kOpeningRadio | breadcrumb::kReloadingPlugins));
        }
        CHECK(breadcrumb::read(&blk).activity == breadcrumb::kOpeningRadio);
        breadcrumb::setActivity(breadcrumb::kOpeningRadio, false);
        CHECK(breadcrumb::read(&blk).activity == 0);

        // SOMEBODY IS HOLDING THE WINDOW: raised by the window procedure, cleared
        // by it - and cleared by the next heartbeat whatever became of the message
        // that should have, because a frame drawn to the end proves nobody is.
        breadcrumb::setActivity(breadcrumb::kUserPaced, true);
        CHECK(breadcrumb::read(&blk).activity == breadcrumb::kUserPaced);
        breadcrumb::setActivity(breadcrumb::kUserPaced, false);
        CHECK(breadcrumb::read(&blk).activity == 0);
        breadcrumb::setActivity(breadcrumb::kUserPaced, true);
        breadcrumb::setActivity(breadcrumb::kOpeningRadio, true);
        breadcrumb::beat();
        CHECK(breadcrumb::read(&blk).activity == breadcrumb::kOpeningRadio);  // only its own bit
        breadcrumb::setActivity(breadcrumb::kOpeningRadio, false);

        // The part of the frame: a number, kept as written.
        CHECK(breadcrumb::read(&blk).frameScope == 0u);
        breadcrumb::setFrameScope(static_cast<std::uint32_t>(FrameScope::Rail) + 1u);
        CHECK(breadcrumb::read(&blk).frameScope == static_cast<std::uint32_t>(FrameScope::Rail) + 1u);
        CHECK(blk.frameScope == static_cast<std::uint32_t>(FrameScope::Rail) + 1u);
        // ...and it is the PROCESS'S timer that writes it, at every change of
        // scope; a timer a test builds for itself leaves the page alone.
        {
            FrameScopeGuard g(FrameScope::Patch);
            CHECK(breadcrumb::read(&blk).frameScope == static_cast<std::uint32_t>(FrameScope::Patch) + 1u);
            {
                FrameScopeGuard inner(FrameScope::Recorder);
                CHECK(breadcrumb::read(&blk).frameScope == static_cast<std::uint32_t>(FrameScope::Recorder) + 1u);
            }
            CHECK(breadcrumb::read(&blk).frameScope == static_cast<std::uint32_t>(FrameScope::Patch) + 1u);
            FrameTimer own(&frameSteadyNanos, &frameAwakeNanos);
            own.beginFrame(0);
            own.switchTo(FrameScope::Rail);
            own.endFrame();
            CHECK(breadcrumb::read(&blk).frameScope == static_cast<std::uint32_t>(FrameScope::Patch) + 1u);
        }
        frameTimer().beginFrame(0);
        CHECK(breadcrumb::read(&blk).frameScope == static_cast<std::uint32_t>(FrameScope::Other) + 1u);
        frameTimer().userWaitBegin();
        CHECK(breadcrumb::read(&blk).frameScope == static_cast<std::uint32_t>(FrameScope::UserWait) + 1u);
        frameTimer().userWaitEnd();
        CHECK(breadcrumb::read(&blk).frameScope == static_cast<std::uint32_t>(FrameScope::Other) + 1u);
        frameTimer().resetForTest();

        // Flags.
        breadcrumb::noteSessionEnding();
        CHECK((breadcrumb::read(&blk).flags & breadcrumb::kFlagSessionEnding) != 0);
        breadcrumb::setFlag(breadcrumb::kFlagReportsOff, true);
        CHECK((breadcrumb::read(&blk).flags & breadcrumb::kFlagReportsOff) != 0);
        breadcrumb::setFlag(breadcrumb::kFlagReportsOff, false);
        CHECK((breadcrumb::read(&blk).flags & breadcrumb::kFlagReportsOff) == 0);

        // NO ROOM FOR TEXT: after every writer has run, the reserved bytes are
        // still zero, and a reader reads nothing but the ten numbers above.
        for (const std::uint32_t r : blk.reserved) { CHECK(r == 0u); }

        // A corrupt page is refused or clamped, never believed.
        const std::uint32_t savedMagic = blk.magic;
        blk.magic = 0xDEADBEEFu;
        CHECK(!breadcrumb::read(&blk).valid);
        blk.magic = savedMagic;
        const std::uint32_t savedLayout = blk.layout;
        blk.layout = 2;
        CHECK(!breadcrumb::read(&blk).valid);
        blk.layout = savedLayout;
        blk.phase = 9999;
        CHECK(breadcrumb::read(&blk).valid && breadcrumb::read(&blk).phase == Phase::Unset);
        blk.activity = 0xFFFFFFFFu;
        blk.flags = 0xFFFFFFFFu;
        const breadcrumb::Snapshot junk = breadcrumb::read(&blk);
        CHECK(junk.activity == breadcrumb::kAllActivity);
        CHECK(junk.flags == (breadcrumb::kFlagSessionEnding | breadcrumb::kFlagReportsOff));
        // A frame scope outside the list is carried as the number it is and NAMED
        // as nothing: the name comes from a table, bounds-checked, never the page.
        blk.frameScope = 0xFFFFFFFFu;
        {
            breadcrumb::Snapshot wild = breadcrumb::read(&blk);
            wild.phase = Phase::Running;
            wild.activity = 0;
            CHECK(wild.frameScope == 0xFFFFFFFFu);
            CHECK(sentinelFrameScopeName(wild) == nullptr);
            CHECK(!sentinelUserPaced(wild));
        }
        blk.frameScope = 0;

        // Detached, every writer is a no-op and nothing crashes.
        breadcrumb::attach(nullptr);
        breadcrumb::setPhase(Phase::Running);
        breadcrumb::beat();
        breadcrumb::setActivity(breadcrumb::kOpeningRadio, true);
        breadcrumb::setFlag(breadcrumb::kFlagReportsOff, true);
        breadcrumb::noteSessionEnding();
        CHECK(breadcrumb::read(&blk).phase == Phase::Unset);  // the junk page above, untouched
    }

    // =======================================================================
    // 3. THE HOT PATH allocates nothing and costs next to nothing
    // =======================================================================
    {
        alignas(64) static breadcrumb::Block blk;
        std::memset(&blk, 0, sizeof(blk));
        breadcrumb::attach(&blk);
        breadcrumb::setPhase(Phase::AwaitingFirstFrame);
        constexpr int kCalls = 2'000'000;
        g_allocs.store(0);
        g_counting.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kCalls; ++i) {
            breadcrumb::beat();
            breadcrumb::setActivity(breadcrumb::kOpeningRadio, (i & 1) != 0);
            breadcrumb::setPhase(Phase::Running);
        }
        const auto t1 = std::chrono::steady_clock::now();
        g_counting.store(false);
        const double perCallNs =
            std::chrono::duration<double, std::nano>(t1 - t0).count() / static_cast<double>(kCalls);
        std::printf("breadcrumb hot path: %.1f ns per frame (beat + activity + phase), %ld allocations\n",
                    perCallNs, g_allocs.load());
        CHECK(g_allocs.load() == 0);
        CHECK(perCallNs < 2000.0);
        CHECK(breadcrumb::read(&blk).frames == static_cast<std::uint64_t>(kCalls));
        breadcrumb::attach(nullptr);
    }

    // =======================================================================
    // 4. THE DECISION TABLE
    // =======================================================================
    {
        using C = SentinelClass;
        const unsigned long kFastFail = 0xC0000409ul;

        // --- clean: nothing is written ------------------------------------
        for (const Phase p : {Phase::ShutdownClosingWindow, Phase::Finished}) {
            CHECK(decideSentinel(facts(crumb(p, 500, 100), 0)).cls == C::None);
        }
        // ...but exit code 0 BEFORE the shutdown reached its last stage is not
        // clean, and not an outside kill either: a process that returned 0 mid-run.
        {
            const SentinelVerdict v = decideSentinel(facts(crumb(Phase::Running, 500, 100), 0));
            CHECK(v.cls == C::Crash);
            CHECK(contains(v.reason, "exit code 0 before the shutdown had finished"));
        }
        CHECK(decideSentinel(facts(crumb(Phase::ShutdownUnloadingPlugins, 500, 100), 0)).cls == C::Crash);
        CHECK(decideSentinel(facts(crumb(Phase::Starting, 0, 10), 0)).cls == C::Startup);
        // A death in the last stage still counts as clean ONLY with exit code 0;
        // a crash code there is a crash (the in-process handler or this watcher
        // reports it) - the existing "residual" is about the marker, not about codes.
        CHECK(decideSentinel(facts(crumb(Phase::ShutdownClosingWindow, 500, 100), kFastFail)).cls == C::Crash);

        // --- the four crash codes the brief names, and the unknown ----------
        struct CodeRow { unsigned long code; const char* words; };
        const CodeRow rows[] = {
            {0xC0000005ul, "access violation"},
            {0xC0000409ul, "fast-fail (abort or failed integrity check)"},
            {0xC0000374ul, "heap corruption"},
            {0xC00000FDul, "stack overflow"},
            {0xC0000142ul, "unknown exit code"},
            {0xE06D7363ul, "unknown exit code"},
        };
        for (const CodeRow& r : rows) {
            const SentinelVerdict v = decideSentinel(facts(crumb(Phase::Running, 500, 100), r.code));
            CHECK(v.cls == C::Crash);
            CHECK(v.upload());
            CHECK(v.codeKnown && v.code == r.code);
            CHECK(contains(v.reason, std::string(" - ") + r.words + "; phase running"));
            CHECK(v.signatureTag == "sentinel:crash:running");
        }
        // STATUS_CONTROL_C_EXIT is a request to end, not a fault.
        {
            const SentinelVerdict v = decideSentinel(facts(crumb(Phase::Running, 500, 100), 0xC000013Aul));
            CHECK(v.cls == C::Outside);
            CHECK(contains(v.reason, "Ctrl+C or a console close"));
        }
        CHECK(isCrashLikeExitCode(0xC0000005ul) && isCrashLikeExitCode(0xE06D7363ul) &&
              isCrashLikeExitCode(0x80000003ul));
        CHECK(!isCrashLikeExitCode(0ul) && !isCrashLikeExitCode(1ul) &&
              !isCrashLikeExitCode(0xFFFFFFFFul) && !isCrashLikeExitCode(0x40010004ul) &&
              !isCrashLikeExitCode(0xC000013Aul) && !isCrashLikeExitCode(3ul));

        // --- ended from outside while drawing: kept here -------------------
        for (const unsigned long code : {1ul, 0xFFFFFFFFul, 0x40010004ul, 3ul}) {
            const SentinelVerdict v = decideSentinel(facts(crumb(Phase::Running, 500, 120), code));
            CHECK(v.cls == C::Outside);
            CHECK(!v.upload());
            CHECK(sentinelReasonIsLocalOnly(v.reason));
            CHECK(v.signatureTag == "sentinel:outside:running");
        }
        CHECK(contains(decideSentinel(facts(crumb(Phase::Running, 500, 120), 1)).reason,
                       "ended by another process (exit code 1, as taskkill /F does)"));
        CHECK(contains(decideSentinel(facts(crumb(Phase::Running, 500, 120), 0xFFFFFFFFul)).reason,
                       "exit code -1, as Stop-Process and Process.Kill do"));
        CHECK(contains(decideSentinel(facts(crumb(Phase::Running, 500, 120), 0x40010004ul)).reason,
                       "unknown exit code"));

        // --- frozen, then ended: the watchdog's own thresholds --------------
        const std::uint64_t steady = HangWatchdog::kDefaultThresholdMs;
        CHECK(decideSentinel(facts(crumb(Phase::Running, 500, steady - 1), 1)).cls == C::Outside);
        CHECK(decideSentinel(facts(crumb(Phase::Running, 500, steady), 1)).cls == C::Outside);
        {
            const SentinelVerdict v = decideSentinel(facts(crumb(Phase::Running, 500, steady + 1), 1));
            CHECK(v.cls == C::Frozen && v.upload());
            CHECK(v.silentMs == static_cast<std::int64_t>(steady + 1));
            CHECK(contains(v.reason, "; silent 5 s"));
            CHECK(v.signatureTag == "sentinel:frozen:running");
        }
        // The start-up budget (30 s for the first 30 frames), then the steady one.
        const std::uint64_t startup = HangWatchdog::kStartupThresholdMs;
        const std::uint64_t startupFrames = HangWatchdog::kStartupFrames;
        CHECK(decideSentinel(facts(crumb(Phase::Running, startupFrames - 1, startup), 1)).cls == C::Outside);
        CHECK(decideSentinel(facts(crumb(Phase::Running, startupFrames - 1, startup + 1), 1)).cls == C::Frozen);
        CHECK(decideSentinel(facts(crumb(Phase::Running, startupFrames, steady + 1), 1)).cls == C::Frozen);
        // The teardown's budget, from the step it is in (no heartbeats there).
        const std::uint64_t shutdown = HangWatchdog::kShutdownThresholdMs;
        for (const Phase p : {Phase::ShutdownBegun, Phase::ShutdownStoppingReceiver,
                              Phase::ShutdownUnloadingPlugins, Phase::ShutdownWritingMarker,
                              Phase::ShutdownClosingWindow, Phase::Finished}) {
            CHECK(sentinelFreezeThresholdMs(crumb(p, 500, 0)) == shutdown);
            CHECK(decideSentinel(facts(crumb(p, 500, shutdown), 1)).cls == C::Outside);
            CHECK(decideSentinel(facts(crumb(p, 500, shutdown + 1), 1)).cls == C::Frozen);
        }
        // A heartbeat from before the phase changed is not the silence: the
        // silence runs from the later of the two.
        {
            breadcrumb::Snapshot s = crumb(Phase::ShutdownUnloadingPlugins, 500, 1000);
            s.beatMs = kNow - 60000;  // the last frame was a minute ago; the step began 1 s ago
            CHECK(decideSentinel(facts(s, 1)).silentMs == 1000);
            CHECK(decideSentinel(facts(s, 1)).cls == C::Outside);
        }

        // --- before the first frame: sent, whatever the code ----------------
        for (const Phase p : {Phase::Starting, Phase::BuildingApp, Phase::CreatingWindow,
                              Phase::AwaitingFirstFrame}) {
            for (const unsigned long code : {1ul, 0xC0000005ul, kFastFail}) {
                const SentinelVerdict v = decideSentinel(facts(crumb(p, p == Phase::AwaitingFirstFrame ? 1 : 0, 50), code));
                CHECK(v.cls == C::Startup && v.upload());
                CHECK(v.signatureTag == std::string("sentinel:startup:") + sentinelPhaseLabel(crumb(p, 0, 0)).id);
            }
        }

        // --- the session closing: kept here, and first -----------------------
        CHECK(decideSentinel(facts(crumb(Phase::Running, 500, 100, 0, breadcrumb::kFlagSessionEnding), 1)).cls == C::Session);
        CHECK(decideSentinel(facts(crumb(Phase::Running, 500, 100, 0, breadcrumb::kFlagSessionEnding), kFastFail)).cls == C::Session);
        CHECK(decideSentinel(facts(crumb(Phase::Running, 500, 90000, 0, breadcrumb::kFlagSessionEnding), 1)).cls == C::Session);
        CHECK(decideSentinel(facts(crumb(Phase::CreatingWindow, 0, 100, 0, breadcrumb::kFlagSessionEnding), 1)).cls == C::Session);
        {
            SentinelFacts f = facts(crumb(Phase::Running, 500, 100), 1);
            f.osSessionEnding = true;  // the operating system says so, the page does not
            const SentinelVerdict v = decideSentinel(f);
            CHECK(v.cls == C::Session && !v.upload() && sentinelReasonIsLocalOnly(v.reason));
            SentinelFacts g = facts(breadcrumb::Snapshot{}, 1);  // not even a readable page
            g.osSessionEnding = true;
            CHECK(decideSentinel(g).cls == C::Session);
        }
        // ...but a normal exit during a session close is still just a clean exit.
        CHECK(decideSentinel(facts(crumb(Phase::ShutdownClosingWindow, 500, 100, 0, breadcrumb::kFlagSessionEnding), 0)).cls == C::None);

        // --- off means off; one report per death -----------------------------
        CHECK(decideSentinel(facts(crumb(Phase::Running, 500, 100, 0, breadcrumb::kFlagReportsOff), kFastFail)).cls == C::None);
        CHECK(decideSentinel(facts(crumb(Phase::Running, 500, 100, 0, breadcrumb::kFlagReportsOff), 1)).cls == C::None);
        {
            SentinelFacts f = facts(crumb(Phase::Running, 500, 100), kFastFail);
            f.crashReportExists = true;
            CHECK(decideSentinel(f).cls == C::None);
            f.exitCode = 1;
            CHECK(decideSentinel(f).cls == C::None);
            SentinelFacts s = facts(crumb(Phase::Running, 500, 100, 0, breadcrumb::kFlagSessionEnding), 1);
            s.crashReportExists = true;
            CHECK(decideSentinel(s).cls == C::None);
        }
        // A freeze the watchdog filed covers the freeze - and the ending that
        // follows it - but not a crash code, and not a session close.
        {
            SentinelFacts f = facts(crumb(Phase::Running, 500, steady + 4000), 1);
            f.freezeReportExists = true;
            CHECK(decideSentinel(f).cls == C::None);
            f.crumb = crumb(Phase::Running, 500, 100);
            CHECK(decideSentinel(f).cls == C::None);  // outside
            f.crumb = crumb(Phase::AwaitingFirstFrame, 1, 100);
            CHECK(decideSentinel(f).cls == C::None);  // startup
            f.exitCode = kFastFail;
            f.crumb = crumb(Phase::Running, 500, steady + 4000);
            CHECK(decideSentinel(f).cls == C::Crash);
            f.exitCode = 1;
            f.crumb = crumb(Phase::Running, 500, 100, 0, breadcrumb::kFlagSessionEnding);
            CHECK(decideSentinel(f).cls == C::Session);
        }

        // --- the platform that cannot learn an exit status --------------------
        {
            // clean when the shutdown reached its last stage
            CHECK(decideSentinel(facts(crumb(Phase::ShutdownClosingWindow, 500, 100), 0, false)).cls == C::None);
            // never a crash class: there is no code to read
            const SentinelVerdict fresh = decideSentinel(facts(crumb(Phase::Running, 500, 100), 0, false));
            CHECK(fresh.cls == C::Outside && !fresh.codeKnown && !fresh.upload());
            CHECK(contains(fresh.reason, "exit status not available on this platform"));
            const SentinelVerdict frozen = decideSentinel(facts(crumb(Phase::Running, 500, steady + 1), 0, false));
            CHECK(frozen.cls == C::Frozen && frozen.upload());
            CHECK(contains(frozen.reason, "exit status not available on this platform"));
            CHECK(decideSentinel(facts(crumb(Phase::CreatingWindow, 0, 100), 0, false)).cls == C::Startup);
            CHECK(sentinelExitWords(false, 0xC0000005ul) == "exit status not available on this platform");
        }

        // --- a page that could not be read -------------------------------------
        {
            const breadcrumb::Snapshot none;
            CHECK(decideSentinel(facts(none, 0)).cls == C::None);
            const SentinelVerdict crash = decideSentinel(facts(none, 0xC0000005ul));
            CHECK(crash.cls == C::Crash && contains(crash.reason, "phase unknown"));
            CHECK(crash.silentMs == -1 && !contains(crash.reason, "silent"));
            const SentinelVerdict out = decideSentinel(facts(none, 1));
            CHECK(out.cls == C::Outside && contains(out.reason, "phase unknown"));
        }

        // --- which part of the frame: in the reason, and in what groups it ------
        auto inScope = [](breadcrumb::Snapshot s, FrameScope scope) {
            s.frameScope = static_cast<std::uint32_t>(scope) + 1u;
            return s;
        };
        {
            const SentinelVerdict v =
                decideSentinel(facts(inScope(crumb(Phase::Running, 500, steady + 1), FrameScope::Rail), 1));
            CHECK(v.cls == C::Frozen && v.upload());
            CHECK(contains(v.reason, "; phase running; in rail; silent 5 s"));
            CHECK(v.signatureTag == "sentinel:frozen:running:rail");
            const SentinelVerdict crash =
                decideSentinel(facts(inScope(crumb(Phase::Running, 500, 100), FrameScope::PluginPanels), kFastFail));
            CHECK(crash.cls == C::Crash);
            CHECK(contains(crash.reason, "; phase running; in plugin-panels; silent 0 s"));
            CHECK(crash.signatureTag == "sentinel:crash:running:plugin-panels");
            // Two parts of the frame are two groups; the same part is one.
            const SentinelVerdict other =
                decideSentinel(facts(inScope(crumb(Phase::Running, 500, steady + 1), FrameScope::Recorder), 1));
            CHECK(other.signatureTag == "sentinel:frozen:running:recorder");
            CHECK(other.signatureTag != v.signatureTag);
        }
        // Every scope has its own fixed name, and only while the frame loop turns:
        // before the first frame there is none, and after the loop has ended the
        // last one written is history.
        for (int i = 0; i < kFrameScopeCount; ++i) {
            const breadcrumb::Snapshot s = inScope(crumb(Phase::Running, 500, 100), static_cast<FrameScope>(i));
            const char* name = sentinelFrameScopeName(s);
            CHECK(name != nullptr && std::string(name) == kFrameScopeNames[i]);
        }
        CHECK(sentinelFrameScopeName(crumb(Phase::Running, 500, 100)) == nullptr);  // none written
        for (const Phase p : {Phase::Starting, Phase::AwaitingFirstFrame, Phase::ShutdownBegun,
                              Phase::ShutdownUnloadingPlugins, Phase::Finished}) {
            const breadcrumb::Snapshot s = inScope(crumb(p, 500, 100), FrameScope::Rail);
            CHECK(sentinelFrameScopeName(s) == nullptr);
            const SentinelVerdict v = decideSentinel(facts(s, kFastFail));
            CHECK(!contains(v.reason, "; in "));
            CHECK(v.signatureTag.find(":rail") == std::string::npos);
        }
        {
            breadcrumb::Snapshot s = crumb(Phase::Running, 500, 100);
            s.frameScope = static_cast<std::uint32_t>(kFrameScopeCount) + 1u;  // one past the last
            CHECK(sentinelFrameScopeName(s) == nullptr);
            CHECK(decideSentinel(facts(s, kFastFail)).signatureTag == "sentinel:crash:running");
        }

        // --- silent because somebody was HOLDING the window is not frozen -------
        //
        // A drag, a resize, an open menu (the window procedure's bit) or a prompt
        // being read (the frame timer's scope user-wait): the freeze watchdog
        // excuses each for as long as it lasts, and an application ended from
        // outside in that state was healthy. Kept on the machine, never sent.
        {
            const std::uint64_t longHold = steady + 60000;
            const SentinelVerdict drag =
                decideSentinel(facts(crumb(Phase::Running, 500, longHold, breadcrumb::kUserPaced), 1));
            CHECK(drag.cls == C::Outside && !drag.upload() && sentinelReasonIsLocalOnly(drag.reason));
            CHECK(contains(drag.reason, "; phase held by the user"));
            CHECK(drag.signatureTag == "sentinel:outside:user-held");
            const SentinelVerdict prompt =
                decideSentinel(facts(inScope(crumb(Phase::Running, 500, longHold), FrameScope::UserWait), 1));
            CHECK(prompt.cls == C::Outside && !prompt.upload());
            CHECK(contains(prompt.reason, "; phase held by the user; in user-wait"));
            // The control: the same silence with nobody holding it IS frozen...
            CHECK(decideSentinel(facts(crumb(Phase::Running, 500, longHold), 1)).cls == C::Frozen);
            CHECK(decideSentinel(facts(inScope(crumb(Phase::Running, 500, longHold), FrameScope::Events), 1)).cls == C::Frozen);
            // ...and so is a plugin reload that never came back: the application
            // set that pace, not a person.
            CHECK(decideSentinel(facts(crumb(Phase::Running, 500, longHold, breadcrumb::kReloadingPlugins), 1)).cls == C::Frozen);
            // A crash code while held is still a crash, and is sent.
            const SentinelVerdict died =
                decideSentinel(facts(crumb(Phase::Running, 500, longHold, breadcrumb::kUserPaced), kFastFail));
            CHECK(died.cls == C::Crash && died.upload());
            // Only the frame loop can be held: in a teardown step the bit means
            // nothing, and a step that overran its budget is frozen.
            CHECK(!sentinelUserPaced(crumb(Phase::ShutdownUnloadingPlugins, 500, 100, breadcrumb::kUserPaced)));
            CHECK(decideSentinel(facts(crumb(Phase::ShutdownUnloadingPlugins, 500, shutdown + 1, breadcrumb::kUserPaced), 1)).cls == C::Frozen);
            // What was going on in the program outranks who was holding the window.
            CHECK(std::string(sentinelPhaseLabel(crumb(Phase::Running, 5, 0, breadcrumb::kUserPaced | breadcrumb::kOpeningRadio)).id) == "opening-radio");
            CHECK(std::string(sentinelPhaseLabel(crumb(Phase::Running, 5, 0, breadcrumb::kUserPaced)).words) == "held by the user");
        }

        // --- what else was going on, in the phase the report names --------------
        CHECK(std::string(sentinelPhaseLabel(crumb(Phase::Running, 5, 0, breadcrumb::kOpeningRadio)).words) == "opening a radio");
        CHECK(std::string(sentinelPhaseLabel(crumb(Phase::Running, 5, 0, breadcrumb::kReloadingPlugins)).words) == "reloading plugins");
        CHECK(std::string(sentinelPhaseLabel(crumb(Phase::BuildingApp, 0, 0, breadcrumb::kReloadingPlugins)).words) == "loading plugins");
        CHECK(std::string(sentinelPhaseLabel(crumb(Phase::Running, 5, 0, breadcrumb::kOpeningRadio | breadcrumb::kReloadingPlugins)).id) == "reloading-plugins");
        CHECK(std::string(sentinelPhaseLabel(crumb(Phase::ShutdownBegun, 5, 0, breadcrumb::kOpeningRadio)).id) == "shutdown-saving");
        CHECK(std::string(sentinelPhaseLabel(crumb(Phase::Starting, 0, 0, breadcrumb::kReloadingPlugins)).id) == "starting");
        {
            const SentinelVerdict v = decideSentinel(facts(crumb(Phase::Running, 500, 100, breadcrumb::kOpeningRadio), kFastFail));
            CHECK(v.signatureTag == "sentinel:crash:opening-radio");
            CHECK(contains(v.reason, "; phase opening a radio;"));
        }
    }

    // =======================================================================
    // 5. EVERY REASON IS FROM A CLOSED VOCABULARY and fits the site
    // =======================================================================
    {
        std::set<std::string> ids;
        int written = 0;
        const unsigned long codes[] = {0ul,          1ul,          0xFFFFFFFFul, 0xC0000005ul, 0xC0000409ul,
                                       0xC0000374ul, 0xC00000FDul, 0xC000013Aul, 0x40010004ul, 0xDEADBEEFul};
        for (std::uint32_t p = 0; p <= 12; ++p) {
            for (const std::uint32_t activity : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) {
                for (const unsigned long code : codes) {
                    for (const std::uint64_t silent : {0ull, 6000ull, 40000ull, 4000000000ull}) {
                        for (const std::uint32_t flags : {0u, 1u}) {
                            for (const bool known : {true, false}) {
                              // Every scope the timer has, none, and two numbers it has not.
                              for (std::uint32_t scope = 0; scope <= static_cast<std::uint32_t>(kFrameScopeCount) + 2u; ++scope) {
                                breadcrumb::Snapshot c = crumb(static_cast<Phase>(p <= 11 ? p : 0), 500, silent, activity, flags);
                                c.frameScope = scope <= static_cast<std::uint32_t>(kFrameScopeCount) + 1u ? scope : 0xFFFFFFFFu;
                                const SentinelVerdict v = decideSentinel(facts(c, code, known));
                                if (!v.write()) { continue; }
                                ++written;
                                const std::string sentence = sentinelClassReason(v.cls);
                                CHECK(v.reason.compare(0, sentence.size(), sentence) == 0);
                                CHECK(v.reason.size() <= 200);
                                bool printable = true;
                                for (const char ch : v.reason) {
                                    if (ch < 0x20 || ch > 0x7E) { printable = false; }
                                }
                                CHECK(printable);
                                CHECK(sentinelReasonIsSentinel(v.reason));
                                CHECK(sentinelReasonIsLocalOnly(v.reason) == !v.upload());
                                CHECK(contains(v.reason, "; phase "));
                                CHECK(v.signatureTag.rfind("sentinel:", 0) == 0);
                                ids.insert(sentinelClassId(v.cls));
                                // Whole, never cut: the site keeps 200 characters
                                // and the seconds of silence are the last of them.
                                if (v.silentMs >= 0) {
                                    CHECK(v.reason.size() >= 2 && v.reason.compare(v.reason.size() - 2, 2, " s") == 0);
                                }
                              }
                            }
                        }
                    }
                }
            }
        }
        CHECK(written > 1000);
        // Every one of the five classes is reachable.
        CHECK(ids == (std::set<std::string>{"crash", "frozen", "startup", "outside", "session"}));
    }

    // =======================================================================
    // 6. THE FIVE SENTENCES, pinned, and what is sent
    // =======================================================================
    {
        CHECK(std::string(kSentinelReasonCrash) == "sentinel: crash exit code, no report from the process");
        CHECK(std::string(kSentinelReasonFrozen) == "sentinel: window had stopped drawing when it ended");
        CHECK(std::string(kSentinelReasonStartup) == "sentinel: ended before the first frame");
        CHECK(std::string(kSentinelReasonOutside) == "sentinel: ended from outside, window was drawing");
        CHECK(std::string(kSentinelReasonSession) == "sentinel: ended as the session closed");
        CHECK(sentinelClassUploads(SentinelClass::Crash) && sentinelClassUploads(SentinelClass::Frozen) &&
              sentinelClassUploads(SentinelClass::Startup));
        CHECK(!sentinelClassUploads(SentinelClass::Outside) && !sentinelClassUploads(SentinelClass::Session));
        CHECK(!sentinelClassUploads(SentinelClass::None));
        // A reason that merely contains a local-only sentence is not one.
        CHECK(!sentinelReasonIsLocalOnly(std::string("access violation")));
        CHECK(!sentinelReasonIsLocalOnly(std::string("x ") + kSentinelReasonOutside));
        CHECK(sentinelReasonIsLocalOnly(std::string(kSentinelReasonOutside) + " - anything"));
        CHECK(!sentinelReasonIsSentinel("fault in a third-party SDR module, absorbed"));
    }

    // =======================================================================
    // 7. THE REPORT: format, signature, and what the uploader makes of it
    // =======================================================================
    std::string typicalReport;
    {
        SentinelFacts f = facts(crumb(Phase::Running, 500, 120, breadcrumb::kOpeningRadio), 0xC0000409ul);
        const SentinelVerdict v = decideSentinel(f);
        SentinelReportInfo info;
        info.version = "0.99.64";
        info.commit = "abc123def456";
        info.os = "Windows 10.0.22631";
        info.arch = "x64";
        info.uptimeSec = 2731;
        info.logTotalLines = 4011;
        for (int i = 0; i < 256; ++i) { info.logLines.push_back("12:00:00.000 info line " + std::to_string(i)); }
        typicalReport = renderSentinelReport(v, info);

        CHECK(typicalReport.rfind("kind: crash\n", 0) == 0);
        CHECK(contains(typicalReport, "\nreason: sentinel: crash exit code, no report from the process - "));
        CHECK(contains(typicalReport, "\ncode: 0xC0000409\n"));
        CHECK(contains(typicalReport, "\nreceiver: not known to the sentinel\n"));
        CHECK(contains(typicalReport, "\nuptime-sec: 2731\n"));
        CHECK(contains(typicalReport, "\nfault-thread-own: unknown\n"));
        CHECK(contains(typicalReport, "\n--- log (last 256 of 4011 lines) ---\n12:00:00.000 info line 0\n"));
        CHECK(contains(typicalReport, "line 255\n"));
        CHECK(!contains(typicalReport, "--- stack"));    // nothing of one to show
        CHECK(!contains(typicalReport, "--- modules"));  // and no module list
        CHECK(!contains(typicalReport, "address:") && !contains(typicalReport, "thread:"));

        // The signature is class + code + phase, and nothing else.
        auto sigOf = [](const std::string& text) {
            const std::size_t at = text.find("\nsignature: ");
            return at == std::string::npos ? std::string() : text.substr(at + 12, 16);
        };
        const std::string sig = sigOf(typicalReport);
        CHECK(sig.size() == 16);
        auto render = [&](const SentinelFacts& ff) {
            return renderSentinelReport(decideSentinel(ff), info);
        };
        // other silence, same ending: same group
        CHECK(sigOf(render(facts(crumb(Phase::Running, 900, 7), 0xC0000409ul)))  != "" );
        CHECK(sigOf(render(facts(crumb(Phase::Running, 900, 7, breadcrumb::kOpeningRadio), 0xC0000409ul))) == sig);
        // other phase, other code, other class: other groups
        CHECK(sigOf(render(facts(crumb(Phase::Running, 900, 7), 0xC0000409ul))) != sig);
        CHECK(sigOf(render(facts(crumb(Phase::Running, 900, 7, breadcrumb::kOpeningRadio), 0xC0000005ul))) != sig);
        CHECK(sigOf(render(facts(crumb(Phase::Running, 900, 7, breadcrumb::kOpeningRadio, breadcrumb::kFlagSessionEnding), 0xC0000409ul))) != sig);

        // THE INVENTORY, BOTH DIRECTIONS, against the code's own lists...
        const std::set<std::string> header(sentinelHeaderFieldNames().begin(), sentinelHeaderFieldNames().end());
        const std::set<std::string> context(sentinelContextFieldNames().begin(), sentinelContextFieldNames().end());
        const std::set<std::string> process(sentinelProcessFieldNames().begin(), sentinelProcessFieldNames().end());
        CHECK(headerLines(typicalReport) == header);
        CHECK(sectionLines(typicalReport, "--- context ---") == context);
        CHECK(sectionLines(typicalReport, "--- process ---") == process);
        // ...and, for the same report, nothing between the markers but those.
        std::size_t sections = 0;
        for (std::size_t at = typicalReport.find("\n--- "); at != std::string::npos;
             at = typicalReport.find("\n--- ", at + 1)) {
            ++sections;
        }
        CHECK(sections == 3);  // context, process, log

        // ...AND AGAINST PRIVACY.md, both ways: the first table of the sentinel
        // section names exactly these lines.
        const std::string section = sentinelPrivacySection();
        CHECK(!section.empty());
        // 0.99.66: the plain statements about Windows' own record of the crash are in the
        // document (whitespace squeezed, because the document wraps its lines).
        {
            std::string squeezed;
            bool space = false;
            for (const char c : section) {
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                    space = true;
                } else {
                    if (space && !squeezed.empty()) { squeezed.push_back(' '); }
                    space = false;
                    squeezed.push_back(c);
                }
            }
            CHECK(contains(squeezed, "(since 0.99.66, Windows only)"));
            CHECK(contains(squeezed, "FoxSDR reads Windows' own record of FoxSDR's own crash"));
            CHECK(contains(squeezed, "keeps only three things from it: the module's file name (never its folder), "
                                     "the distance into it, and the crash code"));
            CHECK(contains(squeezed, "its process number and the time it started, both"));
            CHECK(contains(squeezed, "the full path of FoxSDR (which can contain your account name)"));
            CHECK(contains(squeezed, "none of that is read into anything FoxSDR keeps or sends"));
            CHECK(contains(squeezed, "FoxSDR changes no Windows setting for this, and nothing of the kind exists on Linux"));
            CHECK(contains(squeezed, "`address-source` is not sent"));
            CHECK(contains(squeezed, "it is FoxSDR's own file and FoxSDR's own build identifier, the same line "
                                     "every crash report already carries for it"));
            CHECK(contains(squeezed, "only the build identifier is sent, in the `buildId` field every crash report "
                                     "already has"));
            CHECK(contains(squeezed, "A report with no address has no such block"));
        }
        const std::size_t t2 = section.find("| Class |");
        CHECK(t2 != std::string::npos);
        std::set<std::string> documented;
        std::set<std::string> documentedClasses;
        std::size_t pos = 0;
        while (pos < section.size()) {
            std::size_t eol = section.find('\n', pos);
            if (eol == std::string::npos) { eol = section.size(); }
            const std::string line = section.substr(pos, eol - pos);
            const bool inFirst = pos < t2;
            pos = eol + 1;
            if (line.rfind("| `", 0) != 0) { continue; }
            const std::vector<std::string> cells = cellsOf(line);
            if (cells.size() < 3) { continue; }
            if (inFirst) {
                for (const std::string& n : backticked(cells[0], true)) { documented.insert(n); }
            } else {
                // | `crash` | `sentinel: ...` | sent |
                const std::vector<std::string> id = backticked(cells[0], false);
                CHECK(id.size() == 1);
                if (id.size() != 1) { continue; }
                documentedClasses.insert(id[0]);
                const std::string sentence = firstBackticked(cells[1]);
                SentinelClass cls = SentinelClass::None;
                for (const SentinelClass c : {SentinelClass::Crash, SentinelClass::Frozen, SentinelClass::Startup,
                                              SentinelClass::Outside, SentinelClass::Session}) {
                    if (id[0] == sentinelClassId(c)) { cls = c; }
                }
                CHECK(cls != SentinelClass::None);
                CHECK(sentence == sentinelClassReason(cls));
                CHECK((cells[2] == "sent") == sentinelClassUploads(cls));
                CHECK(cells[2] == "sent" || cells[2] == "kept here");
            }
        }
        std::set<std::string> expected = header;
        expected.insert(context.begin(), context.end());
        expected.insert(process.begin(), process.end());
        // 0.99.66: the two lines a located report adds are documented in the same table.
        expected.insert(sentinelLocationFieldNames().begin(), sentinelLocationFieldNames().end());
        // ...and the one-line module block of a location in our own executable.
        expected.insert(sentinelModuleBlockFieldNames().begin(), sentinelModuleBlockFieldNames().end());
        for (const std::string& n : documented) { std::printf("documented line: %s\n", n.c_str()); }
        CHECK(documented == expected);
        CHECK(documentedClasses == (std::set<std::string>{"crash", "frozen", "startup", "outside", "session"}));

        // WHAT THE UPLOADER MAKES OF IT: the existing parser reads the whole
        // report, and the payload has exactly the documented fields - no new one.
        ParsedReport parsed;
        CHECK(parseReportText(typicalReport, parsed));
        CHECK(parsed.kind == "crash");
        CHECK(parsed.signature == sig);
        CHECK(parsed.code == "0xC0000409");
        CHECK(parsed.reason.rfind(kSentinelReasonCrash, 0) == 0);
        CHECK(parsed.version == "0.99.64" && parsed.commit == "abc123def456");
        CHECK(parsed.os == "Windows 10.0.22631" && parsed.arch == "x64");
        CHECK(parsed.uptimeSec == 2731);
        CHECK(parsed.faultThreadOwn == "unknown");
        CHECK(parsed.log.size() == 256);
        CHECK(parsed.threads.empty() && parsed.plugins.empty() && parsed.module.empty());
        const nlohmann::json j = nlohmann::json::parse(uploadJson(parsed, "4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5"));
        std::set<std::string> keys;
        for (auto it = j.begin(); it != j.end(); ++it) { keys.insert(it.key()); }
        CHECK(keys == std::set<std::string>(uploadFieldNames().begin(), uploadFieldNames().end()));
        std::set<std::string> ctxKeys;
        for (auto it = j["context"].begin(); it != j["context"].end(); ++it) { ctxKeys.insert(it.key()); }
        CHECK(ctxKeys == std::set<std::string>(uploadContextFieldNames().begin(), uploadContextFieldNames().end()));
        CHECK(j["kind"] == "crash");
        CHECK(j["reason"].get<std::string>().size() <= 200);  // what the site keeps
        CHECK(j["code"].get<std::string>().size() <= 24);
        CHECK(j["signature"].get<std::string>().size() == 16);
        CHECK(j["context"]["uptimeSec"] == 2731);
        CHECK(j["context"]["deviceOpen"] == false && j["context"]["source"] == "");
        // ...and no field that could carry a name, a path, a frequency or an address.
        const std::string body = j.dump();
        CHECK(!contains(body, "\\\\") && !contains(body, "C:/") && !contains(body, "MHz"));

        // The report of a platform that cannot read an exit status.
        {
            const SentinelVerdict lv = decideSentinel(facts(crumb(Phase::Running, 500, 6500), 0, false));
            const std::string text = renderSentinelReport(lv, info);
            CHECK(contains(text, "\ncode: unknown\n"));
            ParsedReport lp;
            CHECK(parseReportText(text, lp) && lp.code == "unknown");
        }
    }

    // =======================================================================
    // 7b. NO LOCATION: THE REPORT IS BYTE FOR BYTE WHAT 0.99.64 AND 0.99.65 WROTE
    // (apart from the one context line 0.99.69 added, `foreign-modules`)
    // =======================================================================
    // The expected text is written out, and its three signatures were computed
    // OUTSIDE this code (a Python FNV-1a over the code, the tag and the offset),
    // so "unchanged" is not "equal to whatever the code says now".
    SentinelReportInfo plainInfo;
    plainInfo.version = "0.99.64";
    plainInfo.commit = "abc123def456";
    plainInfo.os = "Windows 10.0.22631";
    plainInfo.arch = "x64";
    plainInfo.uptimeSec = 2731;
    plainInfo.logTotalLines = 4011;
    plainInfo.logLines = {"12:00:00.000 info line a", "12:00:00.001 info line b"};
    auto golden = [](const std::string& reasonTail, const std::string& sig, const std::string& extraHeader) {
        return std::string("kind: crash\nreason: sentinel: crash exit code, no report from the process - "
                           "fast-fail (abort or failed integrity check); ") +
               reasonTail + "\ncode: 0xC0000409\n" + extraHeader + "signature: " + sig +
               "\n--- context ---\nversion: 0.99.64\ncommit: abc123def456\nos: Windows 10.0.22631\n"
               "arch: x64\nreceiver: not known to the sentinel\n"
               // 0.99.69: the one line a report gained, with the value it has when the info
               // names nothing (the default). The headers, the signatures and every other
               // line are exactly what 0.99.64 and 0.99.65 wrote.
               "foreign-modules: (not recorded)\n--- process ---\nuptime-sec: 2731\n"
               "fault-thread-own: unknown\n--- log (last 2 of 4011 lines) ---\n"
               "12:00:00.000 info line a\n12:00:00.001 info line b\n";
    };
    auto withScope = [](breadcrumb::Snapshot c, FrameScope s) {
        c.frameScope = static_cast<std::uint32_t>(s) + 1u;
        return c;
    };
    {
        const SentinelVerdict radio = decideSentinel(facts(crumb(Phase::Running, 500, 120, breadcrumb::kOpeningRadio), 0xC0000409ul));
        CHECK(renderSentinelReport(radio, plainInfo) ==
              golden("phase opening a radio; silent 0 s", "5BE2BD0EC80F9487", ""));
        const SentinelVerdict render =
            decideSentinel(facts(withScope(crumb(Phase::Running, 500, 2000), FrameScope::Render), 0xC0000409ul));
        CHECK(renderSentinelReport(render, plainInfo) ==
              golden("phase running; in render; silent 2 s", "501D3B6226CCCE88", ""));
        // a Startup-class ending, which is the other class a location can attach to
        const SentinelVerdict startup = decideSentinel(facts(crumb(Phase::CreatingWindow, 0, 100), 0xC0000409ul));
        CHECK(startup.cls == SentinelClass::Startup);
        const std::string startupText = renderSentinelReport(startup, plainInfo);
        CHECK(contains(startupText, "\nsignature: A4B54C53E287AF1D\n"));
        CHECK(!contains(startupText, "address"));
        // and the new entry points, given no location, change nothing
        CHECK(sentinelSignature(render, nullptr) == "501D3B6226CCCE88");
        SentinelReportInfo notLocated = plainInfo;
        notLocated.located = false;
        notLocated.location.module = "ucrtbase.dll";  // a stray value with `located` false is not rendered
        notLocated.location.offset = 0x7F6FE;
        CHECK(renderSentinelReport(render, notLocated) == renderSentinelReport(render, plainInfo));
    }

    // =======================================================================
    // 7c. WITH A LOCATION: two lines, a refined signature, and the uploader's fields
    // =======================================================================
    {
        const std::string kAddressLine = "address: ucrtbase.dll+0x7F6FE\n";
        const std::string kSourceLine = std::string("address-source: ") + kSentinelAddressSource + "\n";
        const SentinelVerdict render =
            decideSentinel(facts(withScope(crumb(Phase::Running, 500, 2000), FrameScope::Render), 0xC0000409ul));
        SentinelReportInfo here = plainInfo;
        here.located = true;
        here.location.module = "ucrtbase.dll";
        here.location.offset = 0x7F6FE;
        const std::string located = renderSentinelReport(render, here);
        // exactly the report above, with the two lines after `code` and a signature over the location
        CHECK(located == golden("phase running; in render; silent 2 s", "BE2193D0B411F7DA", kAddressLine + kSourceLine));
        CHECK(std::string(kSentinelAddressSource).find("not a stack") != std::string::npos);
        CHECK(std::string(kSentinelAddressSource).find("Windows") != std::string::npos);

        // THE SIGNATURE: a refinement. Computed outside this code (see 7b).
        CHECK(sentinelSignature(render, &here.location) == "BE2193D0B411F7DA");
        OsCrashLocation other = here.location;
        other.module = "nvoglv64.dll";
        CHECK(sentinelSignature(render, &other) == "03D3047D9ED7CD77");     // another module: another group
        other = here.location;
        other.offset = 0x7F6FF;
        CHECK(sentinelSignature(render, &other) == "4E92CA79A1E876CF");     // another offset: another group
        // the same fault with another silence, another frame: the same group
        const SentinelVerdict later = decideSentinel(facts(withScope(crumb(Phase::Running, 900, 7000), FrameScope::Render), 0xC0000409ul));
        CHECK(sentinelSignature(later, &here.location) == "BE2193D0B411F7DA");
        // the phase and the part of the frame stay in it: a refinement, never a merge
        const SentinelVerdict rail = decideSentinel(facts(withScope(crumb(Phase::Running, 500, 2000), FrameScope::Rail), 0xC0000409ul));
        CHECK(sentinelSignature(rail, &here.location) != "BE2193D0B411F7DA");
        const SentinelVerdict radio = decideSentinel(facts(crumb(Phase::Running, 500, 120, breadcrumb::kOpeningRadio), 0xC0000409ul));
        CHECK(sentinelSignature(radio, &here.location) != sentinelSignature(render, &here.location));
        // ...and the located signature is neither the unlocated one nor an in-process report's
        // of the same module and offset (the two kinds of report are never joined)
        CHECK(sentinelSignature(render, &here.location) != sentinelSignature(render, nullptr));
        CHECK(crashSignature(0xC0000409ul, "ucrtbase.dll", 0x7F6FE) == "992DA295BFD5DFD0");
        CHECK(sentinelSignature(render, &here.location) != crashSignature(0xC0000409ul, "ucrtbase.dll", 0x7F6FE));
        // WHATEVER FILLS THE INFO IN, nothing but a plain file name reaches the report: a path,
        // a name with a separator or a control character, a stack-hash word, and the report is
        // the unlocated one (and its signature is the unlocated one).
        for (const char* bad : {"C:\\Users\\someone\\evil.dll", "..\\evil.dll", "a/b.dll", "a b.dll", "a\nb.dll",
                                "unknown", "StackHash_0a9e", ""}) {
            SentinelReportInfo hostile = plainInfo;
            hostile.located = true;
            hostile.location.module = bad;
            hostile.location.offset = 0x7F6FE;
            CHECK(renderSentinelReport(render, hostile) == renderSentinelReport(render, plainInfo));
        }
        // Two lines, nowhere else, and in the header (before the first marker).
        CHECK(countOf(located, "address") == 2);
        CHECK(located.find("address:") < located.find("--- context ---"));
        CHECK(located.find("address:") < located.find("\nsignature:"));
        CHECK(!contains(located, "--- stack") && !contains(located, "--- modules"));

        // THE INVENTORY, both ways, for a located report: the header gains exactly the two
        // names sentinelLocationFieldNames() lists, and nothing else changes.
        std::set<std::string> expectedHeader(sentinelHeaderFieldNames().begin(), sentinelHeaderFieldNames().end());
        const std::set<std::string> locationNames(sentinelLocationFieldNames().begin(), sentinelLocationFieldNames().end());
        CHECK(locationNames == (std::set<std::string>{"address", "address-source"}));
        for (const std::string& n : locationNames) { CHECK(expectedHeader.count(n) == 0); }
        expectedHeader.insert(locationNames.begin(), locationNames.end());
        CHECK(headerLines(located) == expectedHeader);
        CHECK(sectionLines(located, "--- context ---") ==
              std::set<std::string>(sentinelContextFieldNames().begin(), sentinelContextFieldNames().end()));
        CHECK(sectionLines(located, "--- process ---") ==
              std::set<std::string>(sentinelProcessFieldNames().begin(), sentinelProcessFieldNames().end()));
        CHECK(headerLines(renderSentinelReport(render, plainInfo)) ==
              std::set<std::string>(sentinelHeaderFieldNames().begin(), sentinelHeaderFieldNames().end()));

        // WHAT THE UPLOADER MAKES OF IT: the module and the offset, in the fields the in-process
        // reports use; no field that was not already uploaded.
        ParsedReport p;
        CHECK(parseReportText(located, p));
        CHECK(p.kind == "crash" && p.code == "0xC0000409");
        CHECK(p.module == "ucrtbase.dll" && p.offset == 0x7F6FE);
        CHECK(p.signature == "BE2193D0B411F7DA");
        CHECK(p.reason.rfind(kSentinelReasonCrash, 0) == 0 && !sentinelReasonIsLocalOnly(p.reason));
        CHECK(p.threads.empty() && p.buildId.empty());  // no stack and no module table to take a build id from
        const nlohmann::json j = nlohmann::json::parse(uploadJson(p, "4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5"));
        std::set<std::string> keys;
        for (auto it = j.begin(); it != j.end(); ++it) { keys.insert(it.key()); }
        CHECK(keys == std::set<std::string>(uploadFieldNames().begin(), uploadFieldNames().end()));
        CHECK(j["module"] == "ucrtbase.dll" && j["offset"] == 0x7F6FE);
        CHECK(j["signature"] == "BE2193D0B411F7DA");
        // the line that says where it came from is not in the payload, and nor is anything like a path
        const std::string body = j.dump();
        CHECK(!contains(body, "address-source") && !contains(body, "crash record"));
        CHECK(!contains(body, "\\\\") && !contains(body, "C:/") && !contains(body, "Users"));
        // and the unlocated report still parses to NO module, as before
        ParsedReport p0;
        CHECK(parseReportText(renderSentinelReport(render, plainInfo), p0));
        CHECK(p0.module.empty() && p0.offset == 0);
        CHECK(p0.signature == "501D3B6226CCCE88");

        // WHEN IT IS ASKED FOR: the two classes that are sent and carry a crash code, no other
        CHECK(sentinelWantsOsCrashRecord(render));
        CHECK(sentinelWantsOsCrashRecord(radio));
        CHECK(sentinelWantsOsCrashRecord(decideSentinel(facts(crumb(Phase::CreatingWindow, 0, 100), 0xC0000409ul))));
        CHECK(sentinelWantsOsCrashRecord(decideSentinel(facts(crumb(Phase::Running, 500, 20), 0xC0000005ul))));
        CHECK(!sentinelWantsOsCrashRecord(decideSentinel(facts(crumb(Phase::Running, 500, 20), 1))));            // outside
        CHECK(!sentinelWantsOsCrashRecord(decideSentinel(facts(crumb(Phase::Running, 500, 20), 0xFFFFFFFFul)))); // outside
        CHECK(!sentinelWantsOsCrashRecord(decideSentinel(facts(crumb(Phase::Running, 500, 6500), 1))));          // a freeze
        CHECK(!sentinelWantsOsCrashRecord(decideSentinel(facts(crumb(Phase::Running, 500, 20, 0, breadcrumb::kFlagSessionEnding), 0xC0000409ul))));  // session
        CHECK(!sentinelWantsOsCrashRecord(decideSentinel(facts(crumb(Phase::Running, 500, 20), 0, true))));      // exit 0 before shutdown ended
        CHECK(!sentinelWantsOsCrashRecord(decideSentinel(facts(crumb(Phase::Running, 500, 6500), 0, false))));   // Linux: no exit code
        CHECK(!sentinelWantsOsCrashRecord(decideSentinel(facts(crumb(Phase::Running, 500, 20), 0xC000013Aul)))); // Ctrl+C is not a fault
        CHECK(!sentinelWantsOsCrashRecord(SentinelVerdict()));                                                   // no verdict at all
    }

    // =======================================================================
    // 7d. A LOCATION IN OUR OWN EXECUTABLE carries its one module-table line (0.99.66)
    // =======================================================================
    // The sentinel is a copy of the application's own executable, so it knows the
    // application's build id; the report says it in the crash writer's own line format,
    // between the context and the process block, so that ONE parser reads both and the
    // reader has a build id for the offset.
    {
        const SentinelVerdict v =
            decideSentinel(facts(withScope(crumb(Phase::Running, 500, 2000), FrameScope::Render), 0xC0000409ul));
        constexpr const char* kId = "0123456789ABCDEF0123456789ABCDEF1";
        SentinelReportInfo own = plainInfo;
        own.located = true;
        own.location.module = "cascade.exe";
        own.location.offset = 0x1A2B;
        SentinelReportInfo bare = own;  // the same location, no module line
        own.hasModule = true;
        own.moduleBase = 0x00007FF612340000ull;
        own.moduleSize = 0x25000;
        own.modulePdb = "cascade.pdb";
        own.moduleBuildId = kId;

        const std::string line = std::string("  cascade.exe base=0x00007FF612340000 size=0x25000 "
                                             "pdb=cascade.pdb build=") + kId + "\n";
        // Exactly the located report with the block between the context and the process block.
        std::string expected = renderSentinelReport(v, bare);
        const std::size_t at = expected.find("--- process ---\n");
        CHECK(at != std::string::npos);
        expected.insert(at, "--- modules ---\n" + line);
        const std::string text = renderSentinelReport(v, own);
        CHECK(text == expected);
        CHECK(!contains(renderSentinelReport(v, bare), "--- modules"));

        // THE UPLOADER'S PARSER reads it: the build id, for the module the address names.
        ParsedReport p;
        CHECK(parseReportText(text, p));
        CHECK(p.module == "cascade.exe" && p.offset == 0x1A2B);
        CHECK(p.buildId == kId);
        CHECK(p.modules.size() == 1 && p.modules[0].first == "cascade.exe" && p.modules[0].second == kId);
        // ...and the sections after it still read: the process block, the log
        CHECK(p.uptimeSec == 2731 && p.log.size() == 2 && p.faultThreadOwn == "unknown");
        // only the build id goes up: no load address, no size, no PDB name
        const std::string body = nlohmann::json::parse(uploadJson(p, "4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5")).dump();
        CHECK(contains(body, std::string("\"buildId\":\"") + kId + "\""));
        CHECK(!contains(body, "7FF612340000") && !contains(body, "25000") && !contains(body, "cascade.pdb") &&
              !contains(body, "base="));
        // the signature does not depend on any of it
        CHECK(p.signature == sentinelSignature(v, &own.location));

        // THE INVENTORY, both ways: four sections now, and the line's keys are the documented ones
        std::size_t sections = 0;
        for (std::size_t s = text.find("\n--- "); s != std::string::npos; s = text.find("\n--- ", s + 1)) { ++sections; }
        CHECK(sections == 4);  // context, modules, process, log
        std::set<std::string> keys = {"modules"};
        std::size_t pos = 0;
        while ((pos = line.find('=', pos)) != std::string::npos) {
            std::size_t b = pos;
            while (b > 0 && line[b - 1] != ' ') { --b; }
            keys.insert(line.substr(b, pos - b));
            ++pos;
        }
        CHECK(keys == std::set<std::string>(sentinelModuleBlockFieldNames().begin(), sentinelModuleBlockFieldNames().end()));
        CHECK(sectionLines(text, "--- context ---") ==
              std::set<std::string>(sentinelContextFieldNames().begin(), sentinelContextFieldNames().end()));
        CHECK(sectionLines(text, "--- process ---") ==
              std::set<std::string>(sentinelProcessFieldNames().begin(), sentinelProcessFieldNames().end()));

        // WITHOUT A LOCATION THERE IS NO BLOCK, whatever else the info says: the report is
        // the unlocated one, byte for byte.
        SentinelReportInfo notLocated = own;
        notLocated.located = false;
        CHECK(renderSentinelReport(v, notLocated) == renderSentinelReport(v, plainInfo));
        CHECK(!contains(renderSentinelReport(v, plainInfo), "--- modules"));

        // WHATEVER FILLS IT IN, the line is made of validated parts: a PDB that is not a
        // plain file name and a build id that is not hex digits are `(none)`, and then the
        // uploader has no build id (never a made-up one).
        for (const char* badPdb : {"C:\\x\\cascade.pdb", "a b.pdb", "..\\a.pdb", "", "unknown"}) {
            SentinelReportInfo h = own;
            h.modulePdb = badPdb;
            const std::string t = renderSentinelReport(v, h);
            CHECK(contains(t, " pdb=(none) build=" + std::string(kId) + "\n"));
        }
        for (const std::string& badId : {std::string(), std::string("xyz"), std::string("12 34"), std::string("0x12"),
                                         std::string(48, 'A'), std::string("AB\nCD"), std::string("C:\\id")}) {
            SentinelReportInfo h = own;
            h.moduleBuildId = badId;
            const std::string t = renderSentinelReport(v, h);
            CHECK(contains(t, " pdb=cascade.pdb build=(none)\n"));
            ParsedReport hp;
            CHECK(parseReportText(t, hp) && hp.buildId.empty() && hp.module == "cascade.exe");
        }
        // a build id the image does not carry is `(none)` too, as the crash writer says it
        SentinelReportInfo none = own;
        none.moduleBuildId.clear();
        none.modulePdb.clear();
        CHECK(contains(renderSentinelReport(v, none), "pdb=(none) build=(none)\n"));
    }

    // =======================================================================
    // 8. THE UPLOADER keeps the two local classes local, and is not crowded out
    // =======================================================================
    {
        const fs::path dir = scratchDir("sweep");
        SentinelReportInfo info;
        info.version = "0.99.64";
        info.commit = "abc123def456";
        info.os = "Windows 10.0.22631";
        info.arch = "x64";
        info.uptimeSec = 100;
        info.logLines = {"12:00:00.000 info FoxSDR 0.99.64 (abc123def456) starting"};
        info.logTotalLines = 1;
        auto put = [&](const char* name, const SentinelFacts& f) {
            writeFile(dir / name, renderSentinelReport(decideSentinel(f), info));
        };
        // The oldest by name is the one that can be sent; five newer are not.
        put("crash-20260101-000000-900-999999.txt", facts(crumb(Phase::Running, 500, 100), 0xC0000409ul));
        for (int i = 1; i <= 5; ++i) {
            const std::string name = "crash-20260102-00000" + std::to_string(i) + "-90" + std::to_string(i) + "-999999.txt";
            put(name.c_str(), facts(crumb(Phase::Running, 500, 100), 1));  // outside, kept here
        }

        SweepParams params;
        params.crashDir = dir.string();
        params.url = "http://127.0.0.1:1/api/crash";  // nothing listens: an attempt FAILS, it does not hang
        params.enabled = true;
        params.nowEpoch = static_cast<std::uint64_t>(std::time(nullptr));
        const auto cancel = std::make_shared<UploadCancel>();

        auto status = [&](const char* name) {
            const std::string text = readFile(dir / (std::string(name) + ".upload"));
            const std::size_t at = text.find("status: ");
            if (at == std::string::npos) { return std::string("(none)"); }
            return text.substr(at + 8, text.find('\n', at) - at - 8);
        };

        // START 1: the four newest are examined; all are local-only and none is an attempt.
        SweepOutcome o1 = sweepCrashDir(params, cancel);
        std::printf("sweep 1: considered %d, refused %d, failed %d, sent %d\n", o1.considered, o1.refused,
                    o1.failed, o1.sent);
        CHECK(o1.considered == 4 && o1.refused == 4 && o1.failed == 0 && o1.sent == 0);
        CHECK(status("crash-20260102-000005-905-999999.txt") == "local-only");
        CHECK(status("crash-20260102-000002-902-999999.txt") == "local-only");
        CHECK(status("crash-20260101-000000-900-999999.txt") == "(none)");
        const std::string sidecar = readFile(dir / "crash-20260102-000005-905-999999.txt.upload");
        CHECK(contains(sidecar, "not a fault in FoxSDR"));
        // Nothing of the daily five was spent, and nothing was remembered as sent.
        CHECK(o1.state.windowCount == 0 && o1.state.recent.empty());

        // START 2: they are not looked at again, so the one that CAN be sent is
        // reached - and is attempted (and fails here: nothing is listening).
        params.state = o1.state;
        SweepOutcome o2 = sweepCrashDir(params, cancel);
        std::printf("sweep 2: considered %d, refused %d, failed %d\n", o2.considered, o2.refused, o2.failed);
        CHECK(o2.considered == 2 && o2.refused == 1 && o2.failed == 1);
        CHECK(status("crash-20260101-000000-900-999999.txt") == "failed");
        CHECK(status("crash-20260102-000001-901-999999.txt") == "local-only");

        // START 3: the local ones stay out of it; only the retry remains.
        SweepOutcome o3 = sweepCrashDir(params, cancel);
        CHECK(o3.considered == 1 && o3.refused == 0);

        // THE BUNDLE'S LIST still shows them, with their class in the reason.
        const ReportListing listing = listRecentReports(dir.string(), std::time(nullptr));
        CHECK(listing.total == 6);
        int localOnly = 0;
        for (const ReportSummary& r : listing.newest) {
            const std::string line = reportSummaryLine(r);
            if (r.upload == "local-only") {
                ++localOnly;
                CHECK(contains(line, "sentinel: ended from outside, window was drawing"));
                CHECK(contains(line, "upload local-only"));
                CHECK(r.kind == "crash" && r.code == "0x00000001" && r.version == "0.99.64");
            }
        }
        CHECK(localOnly == 5);
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // 9. ONE REPORT PER DEATH: what counts as a report of THIS death
    // =======================================================================
    {
        const fs::path dir = scratchDir("onedeath");
        const auto longAgo = std::chrono::system_clock::now() - std::chrono::hours(24);
        const auto recent = std::chrono::system_clock::now() - std::chrono::minutes(5);

        writeInProcessReport(dir, "crash-20260105-120000-4242-1.txt", "access violation");
        CHECK(crashReportWrittenByProcess(dir.string(), 4242, recent));
        CHECK(!crashReportWrittenByProcess(dir.string(), 424, recent));      // whole field, not a prefix
        CHECK(!crashReportWrittenByProcess(dir.string(), 14242, recent));
        // Written before the process began: a reused process id, not this death.
        fs::last_write_time(dir / "crash-20260105-120000-4242-1.txt",
                            fs::file_time_type::clock::now() - std::chrono::hours(48));
        CHECK(!crashReportWrittenByProcess(dir.string(), 4242, std::chrono::system_clock::now() - std::chrono::minutes(1)));
        CHECK(crashReportWrittenByProcess(dir.string(), 4242, longAgo - std::chrono::hours(48)));

        // A FAULT THE PROCESS SURVIVED is not the report of its death: the vendor
        // guard's absorbed fault, and the parent's report of an enumeration child's death.
        writeInProcessReport(dir, "crash-20260105-120000-4343-1.txt",
                             std::string(kAbsorbedFaultReasonPrefix) + " - something");
        CHECK(!crashReportWrittenByProcess(dir.string(), 4343, recent));
        writeInProcessReport(dir, "crash-20260105-120000-4444-1.txt",
                             std::string(kChildDeathReasonPrefix) + " probing driver=uhd (contained: x)");
        CHECK(!crashReportWrittenByProcess(dir.string(), 4444, recent));
        writeInProcessReport(dir, "crash-20260105-120000-4545-1.txt", "child process fault (contained)");
        CHECK(!crashReportWrittenByProcess(dir.string(), 4545, recent));
        // ...and the child's OWN report of a fault the parent survived is still a
        // report of the child's death (this is the case the function was made for).
        writeInProcessReport(dir, "crash-20260105-120000-4646-1.txt",
                             "access violation - enumeration child, driver=uhd, attempt 1 (contained)");
        CHECK(crashReportWrittenByProcess(dir.string(), 4646, recent));

        // A sentinel report counts, so asking twice is answered the same.
        writeFile(dir / sentinelReportFileName(4747, std::chrono::system_clock::now()),
                  renderSentinelReport(decideSentinel(facts(crumb(Phase::Running, 500, 100), 1)),
                                       SentinelReportInfo{}));
        CHECK(crashReportWrittenByProcess(dir.string(), 4747, recent));
        const std::string nm = sentinelReportFileName(4747, std::chrono::system_clock::now());
        CHECK(nm.rfind("crash-", 0) == 0 && contains(nm, "-4747-999999.txt"));
        CHECK(rt<unsigned long>(kSentinelReportSeq) == 999999ul);

        // FREEZE REPORTS (hang-<pid>-<n>.txt): the pid, the time, and the kind.
#if defined(_WIN32)
        const std::string pidField = "5151";
#else
        const std::string pidField = "0";  // hang_watchdog.cpp names a Linux report with pid 0
#endif
        const auto since = std::chrono::system_clock::now() - std::chrono::seconds(30);
        writeFile(dir / ("hang-" + pidField + "-1.txt"), "kind: hang\nnote: x\nstalled-ms: 7000\n");
        CHECK(freezeReportWrittenSince(dir.string(), 5151, since));
        CHECK(!freezeReportWrittenSince(dir.string(), 5151, std::chrono::system_clock::now() + std::chrono::minutes(1)));
        writeFile(dir / ("hang-" + pidField + "-2.txt"), "kind: stall\nnote: x\n");
        fs::remove(dir / ("hang-" + pidField + "-1.txt"));
        CHECK(freezeReportWrittenSince(dir.string(), 5151, since));  // a stall is a freeze report too
        writeFile(dir / ("hang-" + pidField + "-2.txt"), "not a report\n");
        CHECK(!freezeReportWrittenSince(dir.string(), 5151, since));
        writeFile(dir / "hang-abc-2.txt", "kind: hang\n");
        CHECK(!freezeReportWrittenSince(dir.string(), 5151, since));
#if defined(_WIN32)
        writeFile(dir / "hang-5252-1.txt", "kind: hang\n");
        CHECK(!freezeReportWrittenSince(dir.string(), 5151, since));  // another process's freeze
        CHECK(freezeReportWrittenSince(dir.string(), 5252, since));
#endif
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // 10. THE END OF THE SESSION'S LOG, read back from the files
    // =======================================================================
    {
        const fs::path dir = scratchDir("logtail");
        auto startLine = [](const char* v) { return std::string("12:00:00.000 info FoxSDR ") + v + " (abc123def456) starting"; };
        std::string old2 = startLine("0.99.1") + "\n12:00:01.000 info old a\n12:00:02.000 info old b\n";
        std::string old1 = startLine("0.99.2") + "\n12:00:03.000 info older c\n";
        std::string live = "12:00:04.000 info older d\n" + startLine("0.99.64") + "\n";
        for (int i = 0; i < 600; ++i) { live += "12:01:00.000 info newest " + std::to_string(i) + "\n"; }
        writeFile(dir / "foxsdr.2.log", old2);
        writeFile(dir / "foxsdr.1.log", old1);
        writeFile(dir / "foxsdr.log", live);

        const SessionLogTail t = readNewestSessionLogTail(dir.string(), 256);
        CHECK(t.found);
        CHECK(t.sessionLines == 601);  // the start line and 600 lines
        CHECK(t.lines.size() == 256);
        CHECK(t.lines.back() == "12:01:00.000 info newest 599");
        CHECK(t.lines.front() == "12:01:00.000 info newest 344");
        // nothing of an earlier session is carried
        for (const std::string& l : t.lines) { CHECK(!contains(l, "old") && !contains(l, "older")); }

        const SessionLogTail all = readNewestSessionLogTail(dir.string(), 100000);
        CHECK(all.lines.size() == 601 && all.lines.front() == startLine("0.99.64"));

        // No start line (diagnostics came on part-way through): the tail is the end
        // of what there is, and says it did not find the start.
        writeFile(dir / "foxsdr.log", "12:00:00.000 info a\n12:00:01.000 info b\n12:00:02.000 info c\n");
        fs::remove(dir / "foxsdr.1.log");
        fs::remove(dir / "foxsdr.2.log");
        const SessionLogTail n = readNewestSessionLogTail(dir.string(), 2);
        CHECK(!n.found && n.lines.size() == 2 && n.lines.back() == "12:00:02.000 info c" && n.sessionLines == 3);

        CHECK(readNewestSessionLogTail((dir / "nope").string(), 10).lines.empty());
        CHECK(readNewestSessionLogTail(std::string(), 10).lines.empty());
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // 11. THE LAST ACT, in-process: finishSentinelWatch
    // =======================================================================
    {
        const fs::path dir = scratchDir("finish");
        const fs::path logs = scratchDir("finishlogs");
        writeFile(logs / "foxsdr.log",
                  "12:00:00.000 info FoxSDR 0.99.64 (abc123def456) starting\n"
                  "12:00:01.000 info source: opened\n12:00:02.000 info the last line\n");

        auto endOf = [&](unsigned long pid, unsigned long code, const breadcrumb::Snapshot& c) {
            SentinelEnd e;
            e.appPid = pid;
            e.exitKnown = true;
            e.exitCode = code;
            e.crumb = c;
            e.crashDir = dir.string();
            e.logDir = logs.string();
            e.uptimeSec = 77;
            return e;
        };
        auto freshCrumb = [](Phase p, std::uint64_t silentMs, std::uint32_t flags = 0) {
            breadcrumb::Snapshot s;
            s.valid = true;
            const std::uint64_t now = breadcrumb::nowMs();
            s.startedMs = now - 60000;
            s.phase = p;
            s.frames = 600;
            s.beatMs = now - silentMs;
            s.phaseMs = now - 50000;
            s.flags = flags;
            return s;
        };

        // A fast-fail the handler never saw: one report, named for the application.
        const SentinelOutcome a = finishSentinelWatch(endOf(6001, 0xC0000409ul, freshCrumb(Phase::Running, 10)));
        CHECK(a.verdict.cls == SentinelClass::Crash);
        CHECK(!a.reportPath.empty() && fs::exists(a.reportPath));
        CHECK(a.reportPath.find("-6001-999999.txt") != std::string::npos);
        const std::string text = readFile(a.reportPath);
        CHECK(contains(text, "code: 0xC0000409") && contains(text, "uptime-sec: 77"));
        CHECK(contains(text, "12:00:02.000 info the last line\n") && contains(text, "(last 3 of 3 lines)"));
        // ASKED TWICE, answered the same: the second call finds the first's report.
        const SentinelOutcome again = finishSentinelWatch(endOf(6001, 0xC0000409ul, freshCrumb(Phase::Running, 10)));
        CHECK(again.verdict.cls == SentinelClass::None && again.reportPath.empty());
        CHECK(reportsIn(dir).size() == 1);

        // The in-process handler reported this death: nothing more.
        writeInProcessReport(dir, "crash-20260105-120000-6002-1.txt", "access violation");
        CHECK(finishSentinelWatch(endOf(6002, 0xC0000005ul, freshCrumb(Phase::Running, 10))).reportPath.empty());
        // ...while a fault it only SURVIVED hides nothing.
        writeInProcessReport(dir, "crash-20260105-120000-6003-1.txt",
                             std::string(kChildDeathReasonPrefix) + " (contained: the parent survived and re-probed)");
        CHECK(!finishSentinelWatch(endOf(6003, 0xC0000409ul, freshCrumb(Phase::Running, 10))).reportPath.empty());

        // Clean: nothing. Ended from outside: a local-only report.
        CHECK(finishSentinelWatch(endOf(6004, 0, freshCrumb(Phase::ShutdownClosingWindow, 10))).reportPath.empty());
        const SentinelOutcome o = finishSentinelWatch(endOf(6005, 1, freshCrumb(Phase::Running, 10)));
        CHECK(o.verdict.cls == SentinelClass::Outside && !o.reportPath.empty());
        CHECK(sentinelReasonIsLocalOnly(o.verdict.reason));

        // A freeze the watchdog filed AFTER the last heartbeat covers the ending...
        writeFile(dir / ("hang-" +
#if defined(_WIN32)
                         std::string("6006")
#else
                         std::string("0")
#endif
                         + "-1.txt"),
                  "kind: hang\nnote: x\n");
        CHECK(finishSentinelWatch(endOf(6006, 1, freshCrumb(Phase::Running, 8000))).reportPath.empty());
        // ...but a freeze report from BEFORE the last sign of life (the window
        // recovered, then ended another way) does not.
        {
            const fs::path old = dir / ("hang-" +
#if defined(_WIN32)
                                        std::string("6007")
#else
                                        std::string("0")
#endif
                                        + "-1.txt");
            writeFile(old, "kind: hang\nnote: x\n");
            fs::last_write_time(old, fs::file_time_type::clock::now() - std::chrono::minutes(10));
#if defined(_WIN32)
            CHECK(!finishSentinelWatch(endOf(6007, 1, freshCrumb(Phase::Running, 8000))).reportPath.empty());
#endif
        }

        // Off means off: the flag stops it whatever happened.
        CHECK(finishSentinelWatch(endOf(6008, 0xC0000409ul,
                                        freshCrumb(Phase::Running, 10, breadcrumb::kFlagReportsOff))).reportPath.empty());

        // NO FOLDER, NO REPORT, AND NO FOLDER IS MADE.
        SentinelEnd missing = endOf(6009, 0xC0000409ul, freshCrumb(Phase::Running, 10));
        missing.crashDir = (dir / "not-there").string();
        CHECK(finishSentinelWatch(missing).reportPath.empty());
        CHECK(!fs::exists(dir / "not-there"));

        std::error_code ec;
        if (g_checksFailed == 0) {
            fs::remove_all(dir, ec);
            fs::remove_all(logs, ec);
        }
    }

    // =======================================================================
    // 11a. THE OTHER SOFTWARE'S DLLs IN A SENTINEL REPORT (0.99.69): the application logs
    // the file names of every module that is neither Windows' nor its own, once at start
    // and once per arrival (core/foreign_modules.hpp), and the sentinel - which reads none of
    // the application's memory - carries them in its report's context block, rebuilt from
    // the log, because the report's own log tail is 256 lines and a long session's start
    // line is far behind them
    // =======================================================================
    {
        const fs::path dir = scratchDir("foreign");
        auto stamped = [](const std::string& m) { return "12:00:00.000 info " + m + "\n"; };
        const std::string session = stamped("FoxSDR 0.99.69 (abc123def456) starting");

        // One sentinel report from a log of `logText`, as a crash the process never saw.
        auto reportFor = [&](const char* tag, unsigned long pid, const std::string& logText) {
            const fs::path logs = dir / tag / "logs";
            const fs::path crashes = dir / tag / "crashes";
            std::error_code mk;
            fs::create_directories(logs, mk);
            fs::create_directories(crashes, mk);
            writeFile(logs / "foxsdr.log", logText);
            SentinelEnd e;
            e.appPid = pid;
            e.exitKnown = true;
            e.exitCode = 0xC0000409ul;
            e.crashDir = crashes.string();
            e.logDir = logs.string();
            e.uptimeSec = 3000;
            breadcrumb::Snapshot s;
            s.valid = true;
            const std::uint64_t now = breadcrumb::nowMs();
            s.startedMs = now - 60000;
            s.phase = Phase::Running;
            s.frames = 600;
            s.beatMs = now - 10;
            s.phaseMs = now - 50000;
            e.crumb = s;
            const SentinelOutcome o = finishSentinelWatch(e);
            CHECK(!o.reportPath.empty());
            return o.reportPath.empty() ? std::string() : readFile(o.reportPath);
        };
        // The text of the context line, and of the report's own log section.
        auto lineOf = [](const std::string& text, const std::string& key) {
            const std::size_t at = text.find("\n" + key);
            if (at == std::string::npos) { return std::string("(absent)"); }
            const std::size_t end = text.find('\n', at + 1);
            return text.substr(at + 1, end - at - 1);
        };
        auto logSection = [](const std::string& text) {
            const std::size_t at = text.find("--- log (last ");
            return at == std::string::npos ? std::string() : text.substr(at);
        };

        // A SESSION'S START, A MODULES LINE AND A LATER ARRIVAL: the report names all three, in
        // the context block, and its log tail carries the start line too.
        {
            const std::string text = reportFor(
                "short", 8101,
                session + stamped("modules: 2 foreign - NahimicOSD.dll, RTSSHooks64.dll") + stamped("source: opened") +
                    stamped("module arrived: ZetaOverlay.dll (12.3 s)"));
            CHECK(contains(text, "\nforeign-modules: NahimicOSD.dll, RTSSHooks64.dll, ZetaOverlay.dll\n"));
            CHECK(contains(logSection(text), "modules: 2 foreign - NahimicOSD.dll, RTSSHooks64.dll"));
            // The line is in the context block, which is where the reader looks.
            CHECK(text.find("foreign-modules: ") < text.find("--- process ---"));
            CHECK(text.find("receiver: ") < text.find("foreign-modules: "));
            // What the uploader reads out of it, and what it would send.
            ParsedReport p;
            CHECK(parseReportText(text, p));
            CHECK(p.foreignModules == "NahimicOSD.dll, RTSSHooks64.dll, ZetaOverlay.dll");
            const nlohmann::json j = nlohmann::json::parse(uploadJson(p, std::string()), nullptr, false);
            CHECK(j.is_object() && j["context"].value("foreignModules", std::string("x")) ==
                                       "NahimicOSD.dll, RTSSHooks64.dll, ZetaOverlay.dll");
        }

        // A LONG SESSION: six hundred lines after the start line push the `modules:` line out
        // of the 256-line tail - which is exactly why the report has its own field. The arrival
        // late in the session is in the tail and in the field; the start line is in the field
        // only.
        {
            std::string log = session + stamped("modules: 2 foreign - NahimicOSD.dll, RTSSHooks64.dll");
            for (int i = 0; i < 600; ++i) { log += stamped("source: tuned " + std::to_string(i)); }
            log += stamped("module arrived: ZetaOverlay.dll (2999.0 s)");
            const std::string text = reportFor("long", 8102, log);
            CHECK(!contains(logSection(text), "modules: 2 foreign"));   // the tail has moved past it...
            CHECK(contains(logSection(text), "module arrived: ZetaOverlay.dll"));
            CHECK(contains(text, "\nforeign-modules: NahimicOSD.dll, RTSSHooks64.dll, ZetaOverlay.dll\n"));  // ...the field has not
        }

        // NO MODULE LINE AT ALL (diagnostics came on part-way through the session): the report
        // says so, and the uploader sends nothing for it.
        {
            const std::string text = reportFor("none-logged", 8103, session + stamped("source: opened"));
            CHECK(lineOf(text, "foreign-modules: ") == "foreign-modules: (not recorded)");
            ParsedReport p;
            CHECK(parseReportText(text, p));
            CHECK(p.foreignModules.empty());
            const nlohmann::json j = nlohmann::json::parse(uploadJson(p, std::string()), nullptr, false);
            CHECK(j.is_object() && j["context"].contains("foreignModules") &&
                  j["context"]["foreignModules"].get<std::string>().empty());
        }

        // NONE FOREIGN is a statement, not an absence.
        {
            const std::string text = reportFor("nothing-foreign", 8104, session + stamped("modules: none foreign"));
            CHECK(lineOf(text, "foreign-modules: ") == "foreign-modules: (none)");
            ParsedReport p;
            CHECK(parseReportText(text, p));
            CHECK(p.foreignModules == "(none)");
        }

        // THE START LINE'S OWN "+K more" is carried, because those names are not in the log.
        {
            const std::string text = reportFor("capped", 8105,
                                               session + stamped("modules: 9 foreign - a.dll, b.dll, +7 more"));
            CHECK(lineOf(text, "foreign-modules: ") == "foreign-modules: a.dll, b.dll, +7 more");
        }

        // AN EARLIER SESSION'S LINES ARE NOT THIS SESSION'S: only what follows the newest
        // start line counts.
        {
            const std::string text = reportFor(
                "earlier", 8106,
                stamped("FoxSDR 0.99.68 (111111111111) starting") + stamped("modules: 1 foreign - OldOverlay.dll") +
                    session + stamped("source: opened"));
            CHECK(lineOf(text, "foreign-modules: ") == "foreign-modules: (not recorded)");
            CHECK(!contains(lineOf(text, "foreign-modules: "), "OldOverlay"));
        }

        // A LINE THAT IS NOT NAMES-ONLY is not carried: a hand-edited log that put a path in
        // the line does not get the path into the report's context block or the upload.
        {
            const std::string text = reportFor(
                "edited", 8107,
                session + stamped("modules: 2 foreign - C:\\Users\\steve\\Overwolf\\evil.dll, good.dll") +
                    stamped("module arrived: D:\\x\\worse.dll (1.0 s)"));
            const std::string field = lineOf(text, "foreign-modules: ");
            CHECK(field == "foreign-modules: good.dll");
            CHECK(!contains(field, "evil") && !contains(field, "worse") && !contains(field, "\\"));
        }

        // Another process's lines that only look like ours are not read as ours.
        {
            const std::string text = reportFor(
                "lookalike", 8108,
                session + stamped("vendor: modules: 1 foreign - evil.dll") + "modules: 1 foreign - evil.dll\n");
            CHECK(lineOf(text, "foreign-modules: ") == "foreign-modules: (not recorded)");
        }

        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // =======================================================================
    // 11b. THE LAST ACT, with Windows' crash record (0.99.66): found, not found, not
    // asked for, and a reader that never answers
    // =======================================================================
    {
        const fs::path dir = scratchDir("located");
        const fs::path logs = scratchDir("locatedlogs");
        writeFile(logs / "foxsdr.log", "12:00:00.000 info FoxSDR 0.99.66 (abc123def456) starting\n12:00:02.000 info the last line\n");

        constexpr std::uint64_t kCreated = 0x1dd550dfc7fb5deull;
        auto eventFor = [](unsigned long pid, std::uint64_t created, unsigned long code, const std::string& module,
                           const std::string& offset) {
            char buf[40];
            std::string x =
                "<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'><System>"
                "<Provider Name='Application Error' Guid='{a0e9b465-b939-57d7-b27d-95d8e925ff57}'/>"
                "<EventID>1000</EventID></System><EventData><Data Name='AppName'>cascade.exe</Data>"
                "<Data Name='ModuleName'>" + module + "</Data><Data Name='ExceptionCode'>";
            std::snprintf(buf, sizeof(buf), "%lx", code);
            x += buf;
            x += "</Data><Data Name='FaultingOffset'>" + offset + "</Data><Data Name='ProcessId'>0x";
            std::snprintf(buf, sizeof(buf), "%lx", pid);
            x += buf;
            x += "</Data><Data Name='ProcessCreationTime'>0x";
            std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(created));
            x += buf;
            x += "</Data><Data Name='AppPath'>C:\\Users\\someone\\FoxSDR\\cascade.exe</Data></EventData></Event>";
            return x;
        };
        auto source = [](std::vector<std::string> events, std::shared_ptr<std::atomic<int>> asked) {
            OsEventSource s = [events = std::move(events), asked](const std::string&,
                                                                  const std::function<bool(const std::string&)>& each,
                                                                  const std::atomic<bool>&) {
                if (asked) { asked->fetch_add(1); }
                for (const std::string& e : events) {
                    if (each(e)) { return; }
                }
            };
            return s;
        };
        auto endOf = [&](unsigned long pid, unsigned long code, const breadcrumb::Snapshot& c, OsEventSource src) {
            SentinelEnd e;
            e.appPid = pid;
            e.exitKnown = true;
            e.exitCode = code;
            e.crumb = c;
            e.crashDir = dir.string();
            e.logDir = logs.string();
            e.uptimeSec = 77;
            e.appStartFileTime = kCreated;
            e.appEndFileTime = kCreated + 600'000'000ull;
            e.osEventSource = std::move(src);
            // A short wait, so that the cases where nothing matches do not each spend the
            // watcher's whole budget; the two that test the budget itself say otherwise.
            e.osRecordBudget = std::chrono::milliseconds(80);
            return e;
        };
        auto fresh = [](Phase p) {
            breadcrumb::Snapshot s;
            s.valid = true;
            const std::uint64_t now = breadcrumb::nowMs();
            s.startedMs = now - 60000;
            s.phase = p;
            s.frames = p >= Phase::Running ? 600 : 0;
            s.beatMs = p >= Phase::Running ? now - 10 : 0;
            s.phaseMs = now - 50000;
            return s;
        };
        // The report as it is written WITHOUT any location, for the others to equal.
        const SentinelOutcome plain =
            finishSentinelWatch(endOf(7000, 0xC0000409ul, fresh(Phase::Running), source({}, nullptr)));
        CHECK(plain.verdict.cls == SentinelClass::Crash && !plain.reportPath.empty());
        const std::string plainText = readFile(plain.reportPath);
        CHECK(!contains(plainText, "address"));

        // FOUND: this death's event, among others that are not.
        auto asked = std::make_shared<std::atomic<int>>(0);
        const SentinelOutcome found = finishSentinelWatch(endOf(
            7001, 0xC0000409ul, fresh(Phase::Running),
            source({eventFor(9999, kCreated, 0xC0000409ul, "nvoglv64.dll", "0000000000000010"),   // another process
                    eventFor(7001, kCreated - 1, 0xC0000409ul, "old.dll", "0000000000000020"),     // the same id, an earlier run
                    eventFor(7001, kCreated, 0xC0000409ul, "ucrtbase.dll", "000000000007f6fe")},   // ours
                   asked)));
        CHECK(found.verdict.cls == SentinelClass::Crash && !found.reportPath.empty() && found.located);
        CHECK(asked->load() == 1);
        const std::string text = readFile(found.reportPath);
        CHECK(!contains(text, "--- modules"));  // a fault in the C runtime gets no module line of ours
        CHECK(contains(text, "\naddress: ucrtbase.dll+0x7F6FE\n"));
        CHECK(contains(text, std::string("\naddress-source: ") + kSentinelAddressSource + "\n"));
        CHECK(!contains(text, "someone") && !contains(text, "Users") && !contains(text, "nvoglv64") &&
              !contains(text, "old.dll"));
        ParsedReport pr;
        CHECK(parseReportText(text, pr));
        CHECK(pr.module == "ucrtbase.dll" && pr.offset == 0x7F6FE);
        CHECK(pr.signature != sentinelSignature(found.verdict, nullptr));
        OsCrashLocation expectedLocation;
        expectedLocation.module = "ucrtbase.dll";
        expectedLocation.offset = 0x7F6FE;
        CHECK(pr.signature == sentinelSignature(found.verdict, &expectedLocation));
        // it is the same report as the unlocated one, plus exactly the three changed lines
        {
            std::string expected = plainText;
            const std::size_t sigAt = expected.find("signature: ");
            const std::size_t sigEnd = expected.find('\n', sigAt);
            expected.replace(sigAt, sigEnd - sigAt,
                             std::string("address: ucrtbase.dll+0x7F6FE\naddress-source: ") + kSentinelAddressSource +
                                 "\nsignature: " + pr.signature);
            CHECK(text == expected);
        }

        // THE STARTUP CLASS carries one too (a crash code before the first frame).
        const SentinelOutcome startup = finishSentinelWatch(endOf(
            7002, 0xC0000005ul, fresh(Phase::CreatingWindow),
            source({eventFor(7002, kCreated, 0xC0000005ul, "nvoglv64.dll", "0000000000001234")}, nullptr)));
        CHECK(startup.verdict.cls == SentinelClass::Startup);
        CHECK(contains(readFile(startup.reportPath), "\naddress: nvoglv64.dll+0x1234\n"));
        CHECK(!contains(readFile(startup.reportPath), "--- modules"));  // a vendor DLL: no line of ours

#if defined(_WIN32)
        // A LOCATION IN OUR OWN EXECUTABLE (0.99.66). In this test the "application" is this
        // very program and the sentinel is the calling process, a copy of the same file: the
        // report gets the module line, with the build id of the file. The expected id is read
        // from the FILE ON DISK (peBuildId) - another route than the mapped image the report's
        // is read from.
        {
            std::string path(1024, '\0');
            path.resize(::GetModuleFileNameA(nullptr, path.data(), static_cast<DWORD>(path.size())));
            const std::size_t cut = path.find_last_of("\\/");
            const std::string leaf = cut == std::string::npos ? path : path.substr(cut + 1);
            std::string fileId, filePdb;
            CHECK(peBuildId(path, fileId, filePdb));
            CHECK(!fileId.empty() && !filePdb.empty());

            DiagModule m;
            CHECK(describeMainModule(m));
            CHECK(leaf.size() == std::strlen(m.name) && _stricmp(leaf.c_str(), m.name) == 0);
            CHECK(fileId == m.buildId && filePdb == m.pdb);
            CHECK(m.base == reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr)) && m.size > 0x1000);

            const SentinelOutcome own = finishSentinelWatch(endOf(
                7050, 0xC0000409ul, fresh(Phase::Running),
                source({eventFor(7050, kCreated, 0xC0000409ul, leaf, "000000000000beef")}, nullptr)));
            CHECK(own.located);
            const std::string ownText = readFile(own.reportPath);
            CHECK(contains(ownText, "\naddress: " + leaf + "+0xBEEF\n"));
            CHECK(contains(ownText, "\n--- modules ---\n  " + leaf + " base=0x"));
            CHECK(contains(ownText, " pdb=" + filePdb + " build=" + fileId + "\n"));
            CHECK(countOf(ownText, "\n  " + leaf + " base=") == 1);  // ONE line
            ParsedReport op;
            CHECK(parseReportText(ownText, op));
            CHECK(op.module == leaf && op.offset == 0xBEEF);
            CHECK(op.buildId == fileId);

            // The event may spell the file name in another case; the line uses the event's
            // spelling, so the parser's exact lookup still finds it.
            std::string upper = leaf;
            for (char& c : upper) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
            const SentinelOutcome shouted = finishSentinelWatch(endOf(
                7051, 0xC0000409ul, fresh(Phase::Running),
                source({eventFor(7051, kCreated, 0xC0000409ul, upper, "000000000000beef")}, nullptr)));
            ParsedReport sp;
            CHECK(shouted.located && parseReportText(readFile(shouted.reportPath), sp));
            CHECK(sp.module == upper && sp.buildId == fileId);

            // Another module with a similar name is not ours.
            const SentinelOutcome other = finishSentinelWatch(endOf(
                7052, 0xC0000409ul, fresh(Phase::Running),
                source({eventFor(7052, kCreated, 0xC0000409ul, "x" + leaf, "000000000000beef")}, nullptr)));
            CHECK(other.located && !contains(readFile(other.reportPath), "--- modules"));
        }
#endif

        // NOT FOUND, for every reason an event can be wrong: the report is the unlocated one, to the byte.
        auto sameAsPlain = [&](unsigned long pid, const OsEventSource& s, unsigned long code = 0xC0000409ul) {
            const SentinelOutcome o = finishSentinelWatch(endOf(pid, code, fresh(Phase::Running), s));
            return !o.reportPath.empty() && !o.located && readFile(o.reportPath) == plainText;
        };
        CHECK(sameAsPlain(7010, source({}, nullptr)));  // an empty log
        CHECK(sameAsPlain(7011, source({eventFor(7011, kCreated, 0xC0000409ul, "C:\\Windows\\x.dll", "000000000007f6fe")}, nullptr)));
        CHECK(sameAsPlain(7012, source({eventFor(7012, kCreated, 0xC0000409ul, "x.dll", "zz")}, nullptr)));
        CHECK(sameAsPlain(7013, source({eventFor(7014, kCreated, 0xC0000409ul, "x.dll", "10")}, nullptr)));          // another pid
        CHECK(sameAsPlain(7015, source({eventFor(7015, kCreated + 5, 0xC0000409ul, "x.dll", "10")}, nullptr)));      // an earlier run
        CHECK(sameAsPlain(7016, source({eventFor(7016, kCreated, 0xC0000005ul, "x.dll", "10")}, nullptr)));          // another exception
        CHECK(sameAsPlain(7017, source({"<Event>garbage"}, nullptr)));
        {
            const OsEventSource thrower = [](const std::string&, const std::function<bool(const std::string&)>&,
                                             const std::atomic<bool>&) { throw std::runtime_error("no log"); };
            CHECK(sameAsPlain(7018, thrower));
        }

        // NOT ASKED FOR: an ending from outside, a freeze, a closing session, a death whose
        // start time is not known - the source is never called, however well it would match.
        auto never = std::make_shared<std::atomic<int>>(0);
        const OsEventSource matching = source({eventFor(7020, kCreated, 1, "x.dll", "10"),
                                               eventFor(7020, kCreated, 0xC0000409ul, "x.dll", "10")}, never);
        const SentinelOutcome outside = finishSentinelWatch(endOf(7020, 1, fresh(Phase::Running), matching));
        CHECK(outside.verdict.cls == SentinelClass::Outside && !contains(readFile(outside.reportPath), "address"));
        breadcrumb::Snapshot closing = fresh(Phase::Running);
        closing.flags = breadcrumb::kFlagSessionEnding;
        const SentinelOutcome session = finishSentinelWatch(endOf(7021, 0xC0000409ul, closing, matching));
        CHECK(session.verdict.cls == SentinelClass::Session && !contains(readFile(session.reportPath), "address"));
        SentinelEnd noStart = endOf(7022, 0xC0000409ul, fresh(Phase::Running), matching);
        noStart.appStartFileTime = 0;
        const SentinelOutcome unknownStart = finishSentinelWatch(noStart);
        CHECK(unknownStart.verdict.cls == SentinelClass::Crash && readFile(unknownStart.reportPath) == plainText);
        SentinelEnd noCode = endOf(7023, 0, fresh(Phase::Running), matching);
        noCode.exitKnown = false;  // the Linux watcher: no exit code at all
        CHECK(!contains(readFile(finishSentinelWatch(noCode).reportPath), "address"));
        CHECK(never->load() == 0);

        // THE BOUND. A log reader that never answers: the last act returns within its budget
        // plus the work of writing the report, and the report is written, unlocated.
        {
            struct Stuck {
                std::mutex m;
                std::condition_variable cv;
                bool open = false;
                std::atomic<bool> entered{false};
                std::atomic<bool> left{false};
            };
            auto stuck = std::make_shared<Stuck>();
            auto reportWasThere = std::make_shared<std::atomic<bool>>(false);
            const std::string reportDir = dir.string();
            const OsEventSource never_answers = [stuck, reportWasThere, reportDir](
                                                    const std::string&, const std::function<bool(const std::string&)>&,
                                                    const std::atomic<bool>&) {
                // THE REPORT IS ALREADY ON DISK when the first question is asked.
                std::error_code lec;
                for (const auto& f : fs::directory_iterator(reportDir, lec)) {
                    if (f.path().filename().string().find("-7030-999999.txt") != std::string::npos) {
                        reportWasThere->store(true);
                    }
                }
                stuck->entered = true;
                std::unique_lock<std::mutex> lk(stuck->m);
                stuck->cv.wait(lk, [&] { return stuck->open; });
                stuck->left = true;
            };
            SentinelEnd stuckEnd = endOf(7030, 0xC0000409ul, fresh(Phase::Running), never_answers);
            stuckEnd.osRecordBudget = std::chrono::milliseconds(-1);  // the watcher's own, kSentinelOsRecordBudget
            const auto t0 = std::chrono::steady_clock::now();
            const SentinelOutcome o = finishSentinelWatch(stuckEnd);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("a log reader that never answers: the last act took %.0f ms (budget %lld ms, deadline %lld ms)\n", ms,
                        static_cast<long long>(kSentinelOsRecordBudget.count()),
                        static_cast<long long>(kSentinelExitDeadline.count()));
            CHECK(stuck->entered.load());
            CHECK(!stuck->left.load());
            CHECK(reportWasThere->load());
            CHECK(o.verdict.cls == SentinelClass::Crash && !o.reportPath.empty() && !o.located);
            CHECK(readFile(o.reportPath) == plainText);
            CHECK(ms >= static_cast<double>(kSentinelOsRecordBudget.count()) - 20.0);  // it did wait its budget
            CHECK(ms < static_cast<double>(kSentinelOsRecordBudget.count()) + 120.0);  // and no longer
            CHECK(ms < static_cast<double>(kSentinelExitDeadline.count()));            // inside the deadline
            {
                std::lock_guard<std::mutex> lk(stuck->m);
                stuck->open = true;
            }
            stuck->cv.notify_all();
            for (int i = 0; i < 200 && !stuck->left.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        }

        // ASKED AGAIN UNTIL IT SHOWS. The event is stamped before the process object is
        // signalled and is not always readable when the watcher wakes (measured: not at the
        // first query in 24 of 40 real deaths): the watcher asks again, and when it finds the
        // event the report is REPLACED by one that carries it - one report, nothing beside it.
        {
            auto asks = std::make_shared<std::atomic<int>>(0);
            const std::string ev = eventFor(7040, kCreated, 0xC0000409ul, "ucrtbase.dll", "000000000007f6fe");
            const OsEventSource showsOnFourth = [asks, ev](const std::string&,
                                                           const std::function<bool(const std::string&)>& each,
                                                           const std::atomic<bool>&) {
                if (asks->fetch_add(1) + 1 >= 4) { each(ev); }
            };
            SentinelEnd later = endOf(7040, 0xC0000409ul, fresh(Phase::Running), showsOnFourth);
            later.osRecordBudget = std::chrono::milliseconds(2000);
            const auto t0 = std::chrono::steady_clock::now();
            const SentinelOutcome o = finishSentinelWatch(later);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            CHECK(o.located && o.verdict.cls == SentinelClass::Crash);
            CHECK(asks->load() == 4);
            CHECK(ms >= 3.0 * static_cast<double>(kOsCrashRetryInterval.count()) - 5.0);  // it did wait between the asks
            CHECK(contains(readFile(o.reportPath), "\naddress: ucrtbase.dll+0x7F6FE\n"));
            std::size_t mine = 0, leftovers = 0;
            std::error_code lec;
            for (const auto& f : fs::directory_iterator(dir, lec)) {
                const std::string n = f.path().filename().string();
                if (n.find("-7040-") != std::string::npos) { ++mine; }
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".tmp") == 0) { ++leftovers; }
            }
            CHECK(mine == 1);       // one report for the death
            CHECK(leftovers == 0);  // and no file written beside it is left behind
            // an event that only shows after the budget is not waited for
            auto slowAsks = std::make_shared<std::atomic<int>>(0);
            const OsEventSource neverShows = [slowAsks](const std::string&, const std::function<bool(const std::string&)>&,
                                                        const std::atomic<bool>&) { slowAsks->fetch_add(1); };
            const SentinelOutcome none = finishSentinelWatch(endOf(7041, 0xC0000409ul, fresh(Phase::Running), neverShows));
            CHECK(!none.located && readFile(none.reportPath) == plainText);
            CHECK(slowAsks->load() >= 2);  // it did ask again, within the 80 ms it was given
        }

        std::error_code ec;
        if (g_checksFailed == 0) {
            fs::remove_all(dir, ec);
            fs::remove_all(logs, ec);
        }
    }

    // =======================================================================
    // 12. THE APPLICATION NEVER WAITS FOR THE SENTINEL, read off the source
    // =======================================================================
    {
        // The application's side of each host is everything before runSentinelMain;
        // the watcher's own wait is inside it. Every call that waits on a process or a
        // child, in the application's half, must be a non-blocking test.
        for (const char* file : {"src/core/sentinel_host_win.cpp", "src/core/sentinel_host_posix.cpp"}) {
            const std::string src = readFile(fs::path(CASCADE_SOURCE_DIR) / file);
            CHECK(!src.empty());
            const std::size_t watcher = src.find("int runSentinelMain(");
            CHECK(watcher != std::string::npos);
            const std::string appSide = src.substr(0, watcher);
            const std::string watcherSide = src.substr(watcher);
            std::size_t waits = 0;
            for (const char* call : {"WaitForSingleObject(", "waitpid(", "WaitForMultipleObjects("}) {
                for (std::size_t at = appSide.find(call); at != std::string::npos; at = appSide.find(call, at + 1)) {
                    ++waits;
                    const std::size_t eol = appSide.find(';', at);
                    const std::string stmt = appSide.substr(at, eol - at);
                    // a zero timeout, or WNOHANG: never a wait
                    CHECK(stmt.find(", 0)") != std::string::npos || stmt.find("WNOHANG") != std::string::npos);
                }
            }
            CHECK(waits >= 1);
            // ...and the watcher's side blocks exactly once, on the application.
#if defined(_WIN32)
            if (std::string(file).find("_win") != std::string::npos) {
                CHECK(watcherSide.find("WaitForSingleObject(app, INFINITE)") != std::string::npos);
            }
#endif
            // the application's side never joins, terminates-and-waits, or sleeps for it
            CHECK(appSide.find("sleep_for") == std::string::npos && appSide.find("sleep(") == std::string::npos);
        }
        // AppWindow talks to it in three places only: the switch, the poll, the phases.
        const std::string appWindow = readFile(fs::path(CASCADE_SOURCE_DIR) / "src/gui/app_window.cpp");
        CHECK(countOf(appWindow, "sentinelSetEnabled(") == 1);
        CHECK(countOf(appWindow, "sentinelPoll(") == 1);
    }

    // =======================================================================
    // 13. THE APPLICATION KEEPS WINDOWS' CRASH RECORD FOR ITS OWN DEATHS (0.99.66)
    // =======================================================================
    // A process that sets SEM_NOGPFAULTERRORBOX gets no Application Error event (measured,
    // docs/DIAGNOSTICS.md), and the sentinel's location comes from that event. So the
    // application must never set it, and nor may anything that loads into it. In `src` the
    // only places that do are the two helper processes - the sentinel's own entry point and
    // the enumeration child - which are not the application. Read off the source, comments
    // left out: a third place is a decision to make, not a thing to find out later.
    {
        std::set<std::string> users;
        std::error_code ec;
        const fs::path root = fs::path(CASCADE_SOURCE_DIR);
        std::size_t scanned = 0;
        for (const auto& entry : fs::recursive_directory_iterator(root / "src", ec)) {
            if (!entry.is_regular_file()) { continue; }
            const std::string ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc" && ext != ".c") { continue; }
            ++scanned;
            std::ifstream in(entry.path(), std::ios::binary);
            std::string line;
            while (std::getline(in, line)) {
                const std::size_t slashes = line.find("//");
                const std::string code = slashes == std::string::npos ? line : line.substr(0, slashes);
                if (contains(code, "SEM_NOGPFAULTERRORBOX") || contains(code, "WerSetFlags") ||
                    contains(code, "WerAddExcludedApplication")) {
                    users.insert(fs::relative(entry.path(), root).generic_string());
                }
            }
        }
        CHECK(scanned > 200);  // it really walked the source tree
        CHECK(users == (std::set<std::string>{"src/core/sentinel_host_win.cpp", "src/source/soapy_enum_proc.cpp"}));
        // and the first of those two is the watcher's entry point, never the application's
        const std::string host = readFile(root / "src/core/sentinel_host_win.cpp");
        const std::size_t main = host.find("int runSentinelMain(");
        const std::size_t sem = host.find("SEM_NOGPFAULTERRORBOX");
        CHECK(main != std::string::npos && sem != std::string::npos && sem > main);
    }

    return testSummary("test_sentinel");
}
