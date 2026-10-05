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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <iterator>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/breadcrumb.hpp"
#include "core/crash_handler.hpp"
#include "core/crash_upload.hpp"
#include "core/diag_history.hpp"
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
        CHECK(rt<std::size_t>(offsetof(Block, reserved)) == 52);
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

        // Flags.
        breadcrumb::noteSessionEnding();
        CHECK((breadcrumb::read(&blk).flags & breadcrumb::kFlagSessionEnding) != 0);
        breadcrumb::setFlag(breadcrumb::kFlagReportsOff, true);
        CHECK((breadcrumb::read(&blk).flags & breadcrumb::kFlagReportsOff) != 0);
        breadcrumb::setFlag(breadcrumb::kFlagReportsOff, false);
        CHECK((breadcrumb::read(&blk).flags & breadcrumb::kFlagReportsOff) == 0);

        // NO ROOM FOR TEXT: after every writer has run, the reserved bytes are
        // still zero, and a reader reads nothing but the nine numbers above.
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
        CHECK(junk.activity == (breadcrumb::kOpeningRadio | breadcrumb::kReloadingPlugins));
        CHECK(junk.flags == (breadcrumb::kFlagSessionEnding | breadcrumb::kFlagReportsOff));

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
            for (const std::uint32_t activity : {0u, 1u, 2u, 3u}) {
                for (const unsigned long code : codes) {
                    for (const std::uint64_t silent : {0ull, 6000ull, 40000ull}) {
                        for (const std::uint32_t flags : {0u, 1u}) {
                            for (const bool known : {true, false}) {
                                breadcrumb::Snapshot c = crumb(static_cast<Phase>(p <= 11 ? p : 0), 500, silent, activity, flags);
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

    return testSummary("test_sentinel");
}
