// A freeze report's signature names the code of OURS that was waiting - not the
// kernel stub it was waiting in.
//
// WHAT WENT WRONG. The signature of a "hang" or a "stall" report was hashed from
// frame 0 of the stalled thread: module + offset of whatever instruction the
// thread was parked on. A GUI thread that is frozen is almost always parked in
// the same few wait stubs - ntdll.dll's NtWaitForSingleObject and friends,
// win32u.dll's message wait - so freezes from entirely different causes (the
// audio open, a shell call, a lock held by a worker) all hashed to ONE value.
// The uploader de-duplicates by signature for 24 hours (decideUpload,
// kDedupSeconds), so a user who met two different freezes in a day sent the
// first and the second was marked `duplicate` and never arrived. The server
// re-groups what it receives by the nearest named frame of our own code, so the
// dashboard was never the problem; the loss happened on the client, before the
// upload.
//
// WHAT IS ASSERTED, with a real HangWatchdog and real blocked threads:
//   a. two stalls parked in the SAME kernel wait, reached from two DIFFERENT
//      functions of this binary, get DIFFERENT signatures - and the precondition
//      is checked, not assumed: their frame 0 is the same module and offset, and
//      each signature is exactly the hash of the first frame in this binary;
//   b. the same stall taken twice gets the SAME signature;
//   c. a stalled thread with no frame in the main image at all falls back to the
//      old key (its top frame) and still gets a signature;
//   d. a display stall keeps its own tag: it is hashed under 'STAL', never under
//      'HANG', so the same frame cannot put a monitor being switched off in a
//      group with a deadlock;
//   e. both through the real parse and the real decideUpload with the 24-hour
//      window: (a) yields two uploads, (b) yields one;
//   f. the migration: an in-flight report written by the build before this one
//      carries an old-style signature on its own `signature:` line and is judged
//      by it, exactly as before; a new-style signature is not suppressed by an
//      old-style entry in the dedup memory, so nothing is lost across the upgrade.
//
// THE OFFSET IS BUILD-SPECIFIC. The same freeze in two builds has two signatures,
// which has always been true of a crash signature and is correct: the offsets
// differ, and so do the symbols needed to read them. (The old key was the one
// that happened to survive across builds, and that was the defect.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "core/crash_upload.hpp"
#include "core/diag_report.hpp"
#include "core/hang_watchdog.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <semaphore.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using cascade::core::crashSignature;
using cascade::core::decideUpload;
using cascade::core::HangWatchdog;
using cascade::core::noteSent;
using cascade::core::ParsedReport;
using cascade::core::UploadDecision;
using cascade::core::UploadPolicyState;

namespace {

constexpr unsigned long kHangTag = 0x48414E47ul;   // 'HANG'
constexpr unsigned long kStallTag = 0x5354414Cul;  // 'STAL'

#if defined(_MSC_VER)
#define SIG_NOINLINE __declspec(noinline)
#else
#define SIG_NOINLINE __attribute__((noinline))
#endif

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr || *tmp == '\0') { tmp = std::getenv("TMPDIR"); }
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
    return base / (std::string("cascade-hangsig-") + tag + "-" + std::to_string(pid));
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

// The file name of the running executable, as the module table spells it.
std::string exeLeaf() {
    char buf[4096] = {};
#if defined(_WIN32)
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, sizeof(buf) - 1);
    const std::string full(buf, n);
    const std::size_t at = full.find_last_of("\\/");
#else
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    const std::string full(buf, n > 0 ? static_cast<std::size_t>(n) : 0u);
    const std::size_t at = full.find_last_of('/');
#endif
    return at == std::string::npos ? full : full.substr(at + 1);
}

// ---------------------------------------------------------------------------
// A kernel wait we can park a thread in from DIFFERENT functions of this binary.
//
// The wait is a macro, not a helper: a helper would be an own frame shared by
// every caller, and the whole point is two callers that reach the SAME wait.
// ---------------------------------------------------------------------------
struct Gate {
#if defined(_WIN32)
    HANDLE h = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Gate() = default;
    ~Gate() { ::CloseHandle(h); }
    void open() { ::SetEvent(h); }
#else
    sem_t s;
    Gate() { ::sem_init(&s, 0, 0); }
    ~Gate() { ::sem_destroy(&s); }
    void open() {
        for (int i = 0; i < 8; ++i) { ::sem_post(&s); }
    }
#endif
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
};

#if defined(_WIN32)
#define BLOCK_ON(g) ::WaitForSingleObject((g).h, INFINITE)
#else
#define BLOCK_ON(g)                                     \
    while (::sem_wait(&(g).s) != 0 && errno == EINTR) { \
    }
#endif

// Different bodies (a different counter each), NOINLINE, so neither the
// optimiser nor the linker's identical-code folding can make them one function:
// the premise of block a is that they are two places in the code.
std::atomic<int> g_touchedA{0};
std::atomic<int> g_touchedB{0};

SIG_NOINLINE void stallFromFunctionA(Gate& g) {
    g_touchedA.fetch_add(1);
    BLOCK_ON(g);
    g_touchedA.fetch_add(10);
}

SIG_NOINLINE void stallFromFunctionB(Gate& g) {
    g_touchedB.fetch_add(2);
    BLOCK_ON(g);
    g_touchedB.fetch_add(20);
}

// ---------------------------------------------------------------------------
// Reading a real report back
// ---------------------------------------------------------------------------
struct Frame {
    std::string module;  // empty for a bare address
    unsigned long long offset = 0;
};

// The frames of the section the writer labels "(gui, stalled)", top first.
std::vector<Frame> stalledFrames(const std::string& text) {
    std::vector<Frame> out;
    const std::string marker = "(gui, stalled) ---\n";
    std::size_t at = text.find(marker);
    if (at == std::string::npos) { return out; }
    at += marker.size();
    while (at < text.size()) {
        std::size_t eol = text.find('\n', at);
        if (eol == std::string::npos) { eol = text.size(); }
        const std::string line = text.substr(at, eol - at);
        if (line.compare(0, 2, "  ") != 0) { break; }  // the next section
        Frame f;
        const std::size_t plus = line.find("+0x");
        if (plus != std::string::npos) {
            f.module = line.substr(2, plus - 2);
            f.offset = std::strtoull(line.c_str() + plus + 3, nullptr, 16);
        }
        out.push_back(f);
        at = eol + 1;
    }
    return out;
}

std::string headerValue(const std::string& text, const std::string& key) {
    const std::string want = key + ": ";
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t eol = text.find('\n', at);
        if (eol == std::string::npos) { eol = text.size(); }
        if (text.compare(at, 3, "---") == 0) { break; }
        if (text.compare(at, want.size(), want) == 0) {
            return text.substr(at + want.size(), eol - at - want.size());
        }
        at = eol + 1;
    }
    return std::string();
}

bool hasOwnFrame(const std::vector<Frame>& frames, const std::string& exe) {
    for (const Frame& f : frames) {
        if (lower(f.module) == lower(exe)) { return true; }
    }
    return false;
}

// What the signature MUST be, worked out from the report's own frames: the
// first frame in the main executable, or - when the stack has none - frame 0,
// exactly as the old key was. Hashed with the same function the writer uses.
std::string expectedSignature(unsigned long tag, const std::vector<Frame>& frames,
                              const std::string& exe) {
    for (const Frame& f : frames) {
        if (lower(f.module) == lower(exe)) {
            return crashSignature(tag, f.module.c_str(), static_cast<std::uintptr_t>(f.offset));
        }
    }
    if (frames.empty()) { return std::string(); }
    const Frame& top = frames.front();
    return crashSignature(tag, top.module.empty() ? "?" : top.module.c_str(),
                          static_cast<std::uintptr_t>(top.offset));
}

// The signature the OLD key would have produced: tag + frame 0, whatever it is.
std::string oldStyleSignature(unsigned long tag, const std::vector<Frame>& frames) {
    if (frames.empty()) { return std::string(); }
    const Frame& top = frames.front();
    return crashSignature(tag, top.module.empty() ? "?" : top.module.c_str(),
                          static_cast<std::uintptr_t>(top.offset));
}

struct Captured {
    std::string text;
    std::string signature;       // the report's own `signature:` line
    std::vector<Frame> frames;   // the stalled thread, top first
    bool complete = false;       // the file reached its log section
};

using Launcher = std::function<void(HangWatchdog&, Gate&, std::vector<std::thread>&)>;

// Starts a REAL watchdog on a scratch directory, lets `launch` park a thread
// that has beaten once (which is what makes it "the GUI thread"), waits for the
// COMPLETE report - the stacks are written after the header, so the header
// alone is not enough to read the stalled thread - and releases everything.
Captured capture(const std::string& tag, const Launcher& launch) {
    Captured c;
    const fs::path dir = scratchDir(tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // The snapshot the capture resolves frames against. Taken here, after
    // everything this test will have loaded, so a module mapped late is named.
    cascade::core::refreshModuleTable();

    Gate gate;
    std::vector<std::thread> threads;
    {
        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 800);
        launch(w, gate, threads);

        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < until) {
            const std::string p = w.lastReportPath();
            if (!p.empty()) {
                c.text = readFile(p);
                if (c.text.find("--- log (last") != std::string::npos) {
                    c.complete = true;
                    break;
                }
            }
            sleepMs(50);
        }
        gate.open();
        for (std::thread& t : threads) { t.join(); }
        w.stop();
    }
    c.signature = headerValue(c.text, "signature");
    c.frames = stalledFrames(c.text);
    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    return c;
}

Launcher guiThreadIn(void (*stall)(Gate&)) {
    return [stall](HangWatchdog& w, Gate& g, std::vector<std::thread>& threads) {
        threads.emplace_back([&w, &g, stall] {
            w.heartbeat();  // from here on this thread IS the GUI thread
            stall(g);
        });
    };
}

// How many of `sigs` the real policy lets through, one minute apart.
int uploadsFor(const std::vector<std::string>& sigs) {
    UploadPolicyState state;
    std::uint64_t now = 1'800'000'000ull;
    int sent = 0;
    for (const std::string& s : sigs) {
        if (decideUpload(state, true, s, now) == UploadDecision::Send) {
            noteSent(state, s, now);
            ++sent;
        }
        now += 60;
    }
    return sent;
}

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// A GUI thread with NO frame of this binary on its stack.
//
// heartbeat() registers the thread that CALLS it, and any thread that calls it
// has a frame of this binary beneath the call - until that frame returns. A
// thread-pool worker does exactly that: the work item beats once and returns,
// and the worker goes back to idling inside ntdll, with nothing of ours left on
// its stack. It stays registered as the GUI thread, stops beating, and is
// reported as stalled: a real thread, a real wait, no own frame.
// ---------------------------------------------------------------------------
DWORD WINAPI beatOnceOnAPoolThread(PVOID p) {
    static_cast<HangWatchdog*>(p)->heartbeat();
    return 0;
}

Launcher guiThreadWithNoFrameOfOurs() {
    return [](HangWatchdog& w, Gate&, std::vector<std::thread>&) {
        ::QueueUserWorkItem(&beatOnceOnAPoolThread, &w, WT_EXECUTEDEFAULT);
    };
}

// ---------------------------------------------------------------------------
// A DISPLAY stall: a wait with the graphics stack beneath it. GDI's font
// enumeration (gdi32full.dll) calls back into the application, and the callback
// blocks - so the stalled thread is parked in a kernel wait, our callback is
// above gdi32full.dll, and gdi32full.dll is one of the modules
// HangWatchdog::isDisplayPresentationStall counts as the display stack.
// ---------------------------------------------------------------------------
Gate* g_fontGate = nullptr;

int CALLBACK blockInsideGdiCallback(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM) {
    if (g_fontGate != nullptr) { BLOCK_ON(*g_fontGate); }
    return 0;  // stop the enumeration
}

SIG_NOINLINE void stallInsideGdi(Gate& g) {
    g_fontGate = &g;
    HDC dc = ::GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    ::EnumFontFamiliesExW(dc, &lf, &blockInsideGdiCallback, 0, 0);
    ::ReleaseDC(nullptr, dc);
}
#endif

}  // namespace

int main() {
    const std::string exe = exeLeaf();
    std::printf("main image: %s\n", exe.c_str());

    // --- the stalls ----------------------------------------------------------
    const Captured a1 = capture("a1", guiThreadIn(&stallFromFunctionA));
    const Captured b1 = capture("b1", guiThreadIn(&stallFromFunctionB));
    const Captured a2 = capture("a2", guiThreadIn(&stallFromFunctionA));

    for (const Captured* c : {&a1, &b1, &a2}) {
        CHECK(c->complete);
        CHECK(headerValue(c->text, "kind") == "hang");
        CHECK(c->signature.size() == 16u);
        CHECK(!c->frames.empty());
    }

    // --- a. two different callers of one wait ---------------------------------
    {
        const Frame topA = a1.frames.empty() ? Frame{} : a1.frames.front();
        const Frame topB = b1.frames.empty() ? Frame{} : b1.frames.front();
        std::printf("A top frame %s+0x%llX, signature %s\n", topA.module.c_str(), topA.offset,
                    a1.signature.c_str());
        std::printf("B top frame %s+0x%llX, signature %s\n", topB.module.c_str(), topB.offset,
                    b1.signature.c_str());

        // THE PRECONDITION, so the red run is red for the right reason: the two
        // stalls really are parked on the very same instruction in the very same
        // system module. Under the old key that made them one group.
        CHECK(!topA.module.empty());
        CHECK(topA.module == topB.module);
        CHECK(topA.offset == topB.offset);
        CHECK(!hasOwnFrame({topA}, exe));  // the top frame is a stub, not ours
        // ...and both stacks do contain code of ours, at two different places.
        CHECK(hasOwnFrame(a1.frames, exe));
        CHECK(hasOwnFrame(b1.frames, exe));
        CHECK(expectedSignature(kHangTag, a1.frames, exe) !=
              expectedSignature(kHangTag, b1.frames, exe));

        // THE PROPERTY.
        CHECK(a1.signature != b1.signature);
        // And it is the hash of the first frame in OUR image, not merely
        // "something that differs": value-for-value against the report's frames.
        CHECK(a1.signature == expectedSignature(kHangTag, a1.frames, exe));
        CHECK(b1.signature == expectedSignature(kHangTag, b1.frames, exe));
    }

    // --- b. the same stall twice ---------------------------------------------
    {
        std::printf("A again, signature %s\n", a2.signature.c_str());
        CHECK(!a1.signature.empty());
        CHECK(a1.signature == a2.signature);
        CHECK(a2.signature == expectedSignature(kHangTag, a2.frames, exe));
    }

    // --- e. through the real parse and the real decideUpload -------------------
    {
        ParsedReport pa1, pb1, pa2;
        CHECK(cascade::core::parseReportText(a1.text, pa1));
        CHECK(cascade::core::parseReportText(b1.text, pb1));
        CHECK(cascade::core::parseReportText(a2.text, pa2));
        CHECK(pa1.kind == "hang");
        // The uploader judges the signature line of the file, not a recomputation.
        CHECK(pa1.signature == a1.signature);

        // (a) two different freezes inside one 24-hour window: BOTH are sent.
        const int differentFreezes = uploadsFor({pa1.signature, pb1.signature});
        std::printf("uploads for two different freezes: %d\n", differentFreezes);
        CHECK(differentFreezes == 2);

        // (b) the same freeze twice: one is sent, the repeat is a duplicate.
        const int sameFreeze = uploadsFor({pa1.signature, pa2.signature});
        std::printf("uploads for the same freeze twice: %d\n", sameFreeze);
        CHECK(sameFreeze == 1);

        // The window is 24 h and no longer: the same freeze a day and a minute
        // later is new information again.
        UploadPolicyState st;
        const std::uint64_t t0 = 1'800'000'000ull;
        CHECK(decideUpload(st, true, pa1.signature, t0) == UploadDecision::Send);
        noteSent(st, pa1.signature, t0);
        CHECK(decideUpload(st, true, pa1.signature, t0 + 3600) == UploadDecision::Duplicate);
        CHECK(decideUpload(st, true, pb1.signature, t0 + 3600) == UploadDecision::Send);
        CHECK(decideUpload(st, true, pa1.signature, t0 + cascade::core::kDedupSeconds + 60) ==
              UploadDecision::Send);
    }

    // --- f. the migration -------------------------------------------------------
    {
        // The signature the build BEFORE this one wrote for stall A: tag + frame 0.
        const std::string oldA = oldStyleSignature(kHangTag, a1.frames);
        CHECK(!oldA.empty());
        CHECK(oldA != a1.signature);  // the key really did change for this freeze

        const std::uint64_t t0 = 1'800'000'000ull;
        UploadPolicyState st;
        noteSent(st, oldA, t0);  // sent by the old build, remembered in the config

        // An in-flight report written by the old build and swept by this one
        // carries `signature: <oldA>` and is judged by exactly that, as before.
        CHECK(decideUpload(st, true, oldA, t0 + 600) == UploadDecision::Duplicate);
        // A freeze written by THIS build is a different string: the old entry
        // cannot suppress it, so nothing is lost across the upgrade. (It can send
        // the same freeze once more, at most once per new signature per 24 h.)
        CHECK(decideUpload(st, true, a1.signature, t0 + 600) == UploadDecision::Send);
        // The stored memory is read as it always was: 16 hex digits and a time.
        CHECK(cascade::core::decodePolicyState(cascade::core::encodePolicyRecent(st), 0, 0, 0)
                  .recent.size() == 1u);
    }

    // --- c. no frame of ours: the old key, and still a signature ---------------
#if defined(_WIN32)
    {
        const Captured p = capture("pool", guiThreadWithNoFrameOfOurs());
        CHECK(p.complete);
        CHECK(headerValue(p.text, "kind") == "hang");
        CHECK(p.text.find("(gui, stalled)") != std::string::npos);
        CHECK(!p.frames.empty());
        // The precondition: this stack really has nothing of ours on it.
        CHECK(!hasOwnFrame(p.frames, exe));
        std::printf("no-own-frame stall: top %s+0x%llX, signature %s\n",
                    p.frames.empty() ? "?" : p.frames.front().module.c_str(),
                    p.frames.empty() ? 0ull : p.frames.front().offset, p.signature.c_str());
        CHECK(p.signature.size() == 16u);
        // The fallback IS today's key: tag + module + offset of frame 0.
        CHECK(p.signature == oldStyleSignature(kHangTag, p.frames));
        CHECK(p.signature == expectedSignature(kHangTag, p.frames, exe));
    }
#else
    SKIP_LINUX("the no-own-frame stall needs a Windows thread-pool worker");
#endif

    // --- d. a display stall keeps its own tag -----------------------------------
#if defined(_WIN32)
    {
        const Captured s = capture("display", guiThreadIn(&stallInsideGdi));
        CHECK(s.complete);
        const std::string kind = headerValue(s.text, "kind");
        std::printf("display stall: kind %s, signature %s\n", kind.c_str(), s.signature.c_str());
        CHECK(kind == "stall");
        CHECK(hasOwnFrame(s.frames, exe));
        // Hashed under 'STAL' from the first frame of ours...
        CHECK(s.signature == expectedSignature(kStallTag, s.frames, exe));
        // ...and NOT under 'HANG' from that same frame: the two tags can never
        // meet, whatever the frame. Compared against the exact hang-tagged hash
        // of the same module and offset.
        const std::string asHang = expectedSignature(kHangTag, s.frames, exe);
        CHECK(!asHang.empty());
        CHECK(s.signature != asHang);
        // A stall is kept on the machine by the uploader, whatever its signature.
        ParsedReport ps;
        CHECK(cascade::core::parseReportText(s.text, ps));
        CHECK(ps.kind == "stall");
    }
#else
    SKIP_LINUX("the display-stall block parks a thread inside GDI");
#endif

    return testSummary("test_hang_signature");
}
