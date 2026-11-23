// test_stop_budget.cpp - THE CONTROL THREAD'S RECEIVER STOP IS JUDGED AGAINST ITS OWN BOUNDED WAITS (report C9E6E94B, 0.99.112).
//
// A normal stop was filed as a freeze: the control pass spent 10.8 s inside a receiver stop made of waits that are each bounded on purpose (the 3 s source-thread join among
// them), and the control watchdog judges a pass at 5 s. core/stall_context.hpp holds ONE table (kStopWaits) of those waits and the Stop phase's threshold built from it, the way
// HangWatchdog::kShutdownBoundedWaitsMs is built for the teardown. This test:
//   A. reads every constant of the table out of the source that declares it and compares the number (a changed bound without the table fails);
//   B. scans the files of the stop path for `constexpr std::chrono` declarations and fails on one that is neither in the table nor named exempt below (what is NOT covered: waits in
//      files not listed here, and waits written inline - test_shutdown_budget.cpp scans the whole tree for the teardown, not for this);
//   C. stages the watchdog: inside PassPhase::Stop a control pass that stalls past 5 s-equivalent writes nothing; outside it, the same stall is reported.
//   D. WIRING (review of fix/freezes, must-fix 4): every operation that stops the receiver or waits for its source thread on the control thread is inside a Stop phase scope and beats at its
//      end: Engine::stopReceiver, installSource (a source switch), removeReceiverSlot (a radio's removal) and the two applySourceRate calls on the pass (a rate change); and no
//      `pipeline_.stop()` / `pipeline_.setSource(` / `applySourceRate(` exists in src/engine outside those, except the named ones that are not the pass's stop (listed with the reason).
//      The sums for the rate change and the removal are computed from the constants READ OUT OF THE SOURCES in A, not from the table's numbers, so the test follows a branch whose
//      kRestartGiveBack is 1500 (integration) as well as this one's 3000.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <map>
#include <vector>
#include <iterator>
#include <set>
#include <string>
#include <thread>

#include "core/hang_watchdog.hpp"
#include "core/stall_context.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace stall = cascade::core::stall;
using cascade::core::HangWatchdog;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// "constexpr std::chrono::<unit> NAME{N}" -> milliseconds, or -1.
long declared(const std::string& text, const std::string& name) {
    std::size_t at = 0;
    while ((at = text.find(name, at)) != std::string::npos) {
        const std::size_t ls = text.rfind('\n', at);
        const std::string line = text.substr(ls == std::string::npos ? 0 : ls + 1, text.find('\n', at) - (ls == std::string::npos ? 0 : ls + 1));
        at += name.size();
        const std::size_t c = line.find("std::chrono::");
        const std::size_t b = line.find('{', line.find(name));
        if (c == std::string::npos || b == std::string::npos || line.find("constexpr") == std::string::npos) { continue; }
        const long n = std::atol(line.c_str() + b + 1);
        if (line.compare(c, 26, "std::chrono::milliseconds ") == 0) { return n; }
        if (line.compare(c, 21, "std::chrono::seconds ") == 0) { return n * 1000; }
    }
    return -1;
}

}  // namespace

int main() {
    const fs::path root(CASCADE_SOURCE_DIR);
    // A. the table against the source
    std::set<std::string> known;
    std::map<std::string, long> fromSource;   // what the sources say, by constant (the sums below are built from these)
    long sourceSum = 0;
    for (const stall::StopWait& w : stall::kStopWaits) {
        const long ms = declared(readFile(root / w.file), w.name);
        std::printf("  %-34s %-18s table %5u ms  source %5ld ms\n", w.file, w.name, w.ms, ms);
        CHECK(ms == static_cast<long>(w.ms));
        known.insert(w.name);
        fromSource[w.name] = ms;
        sourceSum += ms;
    }
    // THE SUMS, from the sources' own numbers: the stop's, the rate change's and the removal's (stall_context.hpp states what each is made of).
    const long srcStop = sourceSum;
    const long srcRate = fromSource["kControlLockWait"] + 3 * fromSource["kVendorCallWait"];
    const long srcRemoval = srcStop + fromSource["kControlLockWait"] + fromSource["kRestartGiveBack"] + fromSource["kVendorCallWait"];
    std::printf("sums from the sources: stop/source switch %ld ms, rate change %ld ms, radio removal %ld ms; threshold %u ms\n", srcStop, srcRate, srcRemoval, stall::kStopThresholdMs);
    CHECK(static_cast<long>(stall::stopBoundedWaitsMs()) == srcStop);
    CHECK(static_cast<long>(stall::kRateChangeWorstMs()) == srcRate);
    CHECK(static_cast<long>(stall::kRemovalWorstMs()) == srcRemoval);
    CHECK(srcRemoval + static_cast<long>(stall::kStopReserveMs) < static_cast<long>(stall::kStopThresholdMs));   // the longest operation in the phase fits under its threshold
    CHECK(srcRate <= srcStop);                                                                                   // a rate change is inside the stop's own sum
    std::printf("stop's bounded waits %u ms + reserve %u ms; the Stop phase's threshold %u ms (the watchdog's own: %u ms)\n", stall::stopBoundedWaitsMs(), stall::kStopReserveMs,
                stall::kStopThresholdMs, HangWatchdog::kDefaultThresholdMs);
    CHECK(stall::kStopThresholdMs == 2u * (stall::stopBoundedWaitsMs() + stall::kStopReserveMs));
    CHECK(stall::stopBoundedWaitsMs() > HangWatchdog::kDefaultThresholdMs);   // the defect, in one line: the sum does not fit the pass's threshold
    // B. no unlisted chrono constant in the stop path's files
    const std::set<std::string> exempt = {"kStreamHealthWindow", "kStallWindow", "kLateCallGrace", "kSilenceStep"};   // detector thresholds and grace times: nothing waits on them in a stop
    for (const char* f : {"src/core/pipeline.cpp", "src/source/soapy_source.cpp", "src/source/soapy_source.hpp"}) {
        const std::string text = readFile(root / f);
        std::size_t at = 0;
        while ((at = text.find("constexpr std::chrono::", at)) != std::string::npos) {
            const std::size_t s = text.find(' ', at + 23);
            const std::size_t e = text.find_first_of("{= ;", s + 1);
            const std::string name = text.substr(s + 1, e - s - 1);
            at += 23;
            if (name.empty() || name[0] != 'k') { continue; }
            if (known.count(name) == 0 && exempt.count(name) == 0) { std::printf("FAIL %s declares %s: add it to stall::kStopWaits or to the exempt list\n", f, name.c_str()); }
            CHECK(known.count(name) == 1 || exempt.count(name) == 1);
        }
    }
    // D. the wiring: the operations that stop the receiver on the pass are inside the Stop phase and beat at the end
    {
        const auto strip = [](std::string s) { s.erase(std::remove(s.begin(), s.end(), '\r'), s.end()); return s; };
        // The body of the function whose signature starts at `sig` (up to its closing brace at column 0).
        const auto bodyOf = [](const std::string& text, const std::string& sig) -> std::string {
            const std::size_t at = text.find(sig);
            if (at == std::string::npos) { return std::string(); }
            const std::size_t end = text.find("\n}\n", at);
            return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        };
        struct Fn { const char* file; const char* sig; };
        const Fn wired[] = {
            {"src/engine/engine.cpp", "void Engine::stopReceiver("},
            {"src/engine/engine.cpp", "void Engine::installSource("},
            {"src/engine/engine_radios.cpp", "void Engine::removeReceiverSlot("},
        };
        for (const Fn& f : wired) {
            const std::string body = bodyOf(strip(readFile(root / f.file)), f.sig);
            std::printf("  wiring %-32s scope %d beat %d\n", f.sig, body.find("PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop)") != std::string::npos ? 1 : 0,
                        body.find("controlWatchdog_.heartbeat()") != std::string::npos ? 1 : 0);
            CHECK(!body.empty());
            CHECK(body.find("PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop)") != std::string::npos);
            CHECK(body.find("controlWatchdog_.heartbeat()") != std::string::npos);
        }
        // stopReceiver beats LAST: after the receiver-stop counter, still inside the scope (nothing but the closing brace after it)
        {
            const std::string body = bodyOf(strip(readFile(root / "src/engine/engine.cpp")), "void Engine::stopReceiver(");
            const std::size_t beat = body.rfind("controlWatchdog_.heartbeat();");
            CHECK(beat != std::string::npos && body.find('}', beat) == std::string::npos && body.find("receiverStops_", 0) < beat);
        }
        // Every call that stops the pipeline or changes the rate of a live source, in every engine file: inside a function above, or named here with the reason it is not the pass's stop.
        struct Exempt { const char* file; const char* sig; const char* why; };
        const Exempt notThePass[] = {
            {"src/engine/engine.cpp", "std::unique_ptr<cascade::source::DeviceSource> Engine::openDeviceSync(", "opens a device that is not installed yet (no stream to stop)"},
            {"src/engine/engine.cpp", "void Engine::launchDeviceOpen(", "the device-open worker applies the requested rate to a device that is not installed yet (off the pass)"},
            {"src/engine/engine.cpp", "void Engine::shutdownStop(", "the teardown: the shutdown budget (HangWatchdog::kShutdownBoundedWaitsMs) judges it, and the control watchdog is stopped"},
            {"src/engine/engine_patch_radios.cpp", "", "the patch radio's open worker (applySourceRate with \"open\"): a device not yet installed, off the pass"},
        };
        const std::set<std::string> callTokens = {"pipeline_.stop()", "pipeline_.setSource(", "applySourceRate("};
        for (const auto& entry : fs::directory_iterator(root / "src/engine")) {
            const std::string name = entry.path().filename().string();
            if (entry.path().extension() != ".cpp") { continue; }
            const std::string text = strip(readFile(entry.path()));
            std::size_t pos = 0;
            std::vector<std::string> lines;
            {
                std::size_t s = 0;
                while (s <= text.size()) {
                    const std::size_t e = text.find('\n', s);
                    lines.push_back(text.substr(s, e == std::string::npos ? std::string::npos : e - s));
                    if (e == std::string::npos) { break; }
                    s = e + 1;
                }
            }
            (void)pos;
            std::string fnLine;   // the last line at column 0 that looks like a function head
            std::string fnBody;   // the text of the current function so far
            for (const std::string& line : lines) {
                if (!line.empty() && line[0] != ' ' && line[0] != '\t' && line[0] != '}' && line[0] != '/' && line[0] != '#' && line.find('(') != std::string::npos && line.find("::") != std::string::npos) {
                    fnLine = line;
                    fnBody.clear();
                }
                fnBody += line + "\n";
                const std::size_t c = line.find("//");
                const std::string code = c == std::string::npos ? line : line.substr(0, c);
                for (const std::string& tok : callTokens) {
                    if (code.find(tok) == std::string::npos) { continue; }
                    bool ok = fnBody.find("PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop)") != std::string::npos;
                    if (!ok) {
                        for (const Exempt& x : notThePass) {
                            if (name == fs::path(x.file).filename().string() && (x.sig[0] == '\0' || fnLine.find(x.sig) != std::string::npos)) { ok = true; }
                        }
                    }
                    if (!ok) { std::printf("FAIL %s: `%s` outside a Stop phase scope, in: %s\n", name.c_str(), tok.c_str(), fnLine.c_str()); }
                    CHECK(ok);
                }
            }
        }
    }
    // C. the watchdog
    const fs::path dir = fs::temp_directory_path() / ("foxsdr-stopbudget-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    HangWatchdog w;
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.setRole(HangWatchdog::Role::Control);
    w.start(dir.string(), 700);
    w.heartbeat();
    {
        const stall::PassPhaseScope inStop(stall::PassPhase::Stop);
        std::this_thread::sleep_for(std::chrono::milliseconds(2200));   // a stop pass that spends 3x the threshold in bounded waits
        CHECK(w.reportsWritten() == 0u);                                  // no freeze report
    }
    w.heartbeat();
    std::this_thread::sleep_for(std::chrono::milliseconds(2200));        // the same stall in any other phase
    CHECK(w.reportsWritten() >= 1u);                                       // IS reported
    w.stop();
    fs::remove_all(dir, ec);
    return testSummary("test_stop_budget");
}
