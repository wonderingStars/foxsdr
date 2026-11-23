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
#include <functional>
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

// THE SCOPE MUST BE OPEN AT THE CALL (review of fix/freezes part 3, F9). The first version asked whether the text of the function SO FAR contained the scope's declaration, so a scope that sat in a
// lambda or a block that had already ended (Engine::applyCommand, applyPluginPreset) covered every stop written after it. This reads braces: a scope opens at its declaration and ends with the
// block that holds it. `lineOk(code)` is called for each line that holds a stop-like call and says whether it is excused by name. Returns the offending lines (empty: all covered).
std::vector<std::string> uncoveredStops(const std::string& text, const std::set<std::string>& callTokens, const std::function<bool(const std::string& fnLine, const std::string& code)>& excused) {
    std::vector<std::string> bad;
    int depth = 0;
    std::vector<int> scopeDepths;   // the block depth each open Stop scope was declared at
    std::string fnLine;
    bool inBlockComment = false;
    std::size_t s0 = 0;
    while (s0 <= text.size()) {
        const std::size_t e = text.find('\n', s0);
        const std::string line = text.substr(s0, e == std::string::npos ? std::string::npos : e - s0);
        s0 = (e == std::string::npos) ? text.size() + 1 : e + 1;
        if (!line.empty() && line[0] != ' ' && line[0] != '\t' && line[0] != '}' && line[0] != '/' && line[0] != '#' && line.find('(') != std::string::npos && line.find("::") != std::string::npos) { fnLine = line; }
        // the code of the line: no comments, no string or character literals
        std::string code;
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (inBlockComment) {
                if (line.compare(i, 2, "*/") == 0) { inBlockComment = false; ++i; }
                continue;
            }
            if (line.compare(i, 2, "//") == 0) { break; }
            if (line.compare(i, 2, "/*") == 0) { inBlockComment = true; ++i; continue; }
            if (line[i] == '"' || line[i] == '\'') {
                const char q = line[i];
                for (++i; i < line.size() && line[i] != q; ++i) { if (line[i] == '\\') { ++i; } }
                continue;
            }
            code.push_back(line[i]);
        }
        if (code.find("PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop)") != std::string::npos) { scopeDepths.push_back(depth); }
        for (const std::string& tok : callTokens) {
            if (code.find(tok) == std::string::npos) { continue; }
            // a call is covered while a scope is open; the scope's own declaration line is before the call
            if (scopeDepths.empty() && !excused(fnLine, line)) { bad.push_back(fnLine + " :: " + line); }
        }
        for (const char c : code) {
            if (c == '{') { ++depth; }
            if (c == '}') {
                --depth;
                while (!scopeDepths.empty() && scopeDepths.back() > depth) { scopeDepths.pop_back(); }
            }
        }
    }
    return bad;
}

// The checker held to what it exists for: a scope that has ended covers nothing after it.
void checkTheWiringCheckerItself() {
    const std::set<std::string> tokens = {"pipeline_.stop()"};
    const auto none = [](const std::string&, const std::string&) { return false; };
    // a stop inside its scope: covered
    CHECK(uncoveredStops("void Engine::a() {\n    const cascade::core::stall::PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop);\n    rx().pipeline_.stop();\n}\n", tokens, none).empty());
    // no scope at all: caught
    CHECK(uncoveredStops("void Engine::a() {\n    rx().pipeline_.stop();\n}\n", tokens, none).size() == 1u);
    // a call BEFORE the scope: caught
    CHECK(uncoveredStops("void Engine::a() {\n    rx().pipeline_.stop();\n    const cascade::core::stall::PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop);\n}\n", tokens, none).size() == 1u);
    // THE FAULT THE REVIEW FOUND: the scope sits in a lambda that has ended, and a later stop is not covered by it
    CHECK(uncoveredStops("void Engine::a() {\n    auto f = [&] {\n        const cascade::core::stall::PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop);\n        x();\n    };\n    rx().pipeline_.stop();\n}\n", tokens, none).size() == 1u);
    // ...and the same in a plain block
    CHECK(uncoveredStops("void Engine::a() {\n    {\n        const cascade::core::stall::PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop);\n        x();\n    }\n    rx().pipeline_.stop();\n}\n", tokens, none).size() == 1u);
    // a stop inside the lambda's own scope: covered; a scope that is still open in the enclosing block covers a call inside a nested block
    CHECK(uncoveredStops("void Engine::a() {\n    {\n        const cascade::core::stall::PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop);\n        if (y) {\n            rx().pipeline_.stop();\n        }\n    }\n}\n", tokens, none).empty());
    // a call in a comment or a string is not a call
    CHECK(uncoveredStops("void Engine::a() {\n    // rx().pipeline_.stop();\n    log(\"pipeline_.stop()\");\n}\n", tokens, none).empty());
    // an excused function is excused
    CHECK(uncoveredStops("void Engine::a() {\n    rx().pipeline_.stop();\n}\n", tokens, [](const std::string& fn, const std::string&) { return fn.find("Engine::a(") != std::string::npos; }).empty());
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
        // Every call that stops the pipeline or changes the rate of a live source, in every engine file: with a Stop phase scope OPEN at the call (see uncoveredStops), or named here with the reason
        // it is not the pass's stop. An exemption is a function (or, with `lineHas`, one call inside it), never a whole file.
        struct Exempt { const char* file; const char* sig; const char* lineHas; const char* why; };
        const Exempt notThePass[] = {
            {"engine.cpp", "std::unique_ptr<cascade::source::DeviceSource> Engine::openDeviceSync(", "", "opens a device that is not installed yet (no stream to stop)"},
            {"engine.cpp", "void Engine::launchDeviceOpen(", "", "the device-open worker applies the requested rate to a device that is not installed yet (off the pass)"},
            {"engine.cpp", "void Engine::shutdownStop(", "", "the teardown: the shutdown budget (HangWatchdog::kShutdownBoundedWaitsMs) judges it, and the control watchdog is stopped"},
            {"engine_patch_radios.cpp", "void Engine::patchReconcile(", "\"open\")", "the patch radio's open worker (applySourceRate with \"open\"): a device not yet installed, off the pass"},
        };
        const std::set<std::string> callTokens = {"pipeline_.stop()", "pipeline_.setSource(", "applySourceRate("};
        for (const auto& entry : fs::directory_iterator(root / "src/engine")) {
            const std::string name = entry.path().filename().string();
            if (entry.path().extension() != ".cpp") { continue; }
            const std::vector<std::string> bad = uncoveredStops(strip(readFile(entry.path())), callTokens, [&](const std::string& fnLine, const std::string& line) {
                for (const Exempt& x : notThePass) {
                    if (name == x.file && fnLine.find(x.sig) != std::string::npos && (x.lineHas[0] == '\0' || line.find(x.lineHas) != std::string::npos)) { return true; }
                }
                return false;
            });
            for (const std::string& b1 : bad) { std::printf("FAIL %s: a stop-like call with no Stop phase scope open: %s\n", name.c_str(), b1.c_str()); }
            CHECK(bad.empty());
        }
        // THE PATCH RADIOS' STOPS (engine_patch_radios.cpp, patchReconcile and patchStopAll) are on the pass and make the same bounded waits (PatchRadio::stop(): the source's stop, then the reader's join of
        // PatchRadio::kStopWaitMs = 3000): inside the scope, one radio per beat.
        for (const char* sig : {"void Engine::patchReconcile(", "void Engine::patchStopAll("}) {
            const std::string body = bodyOf(strip(readFile(root / "src/engine/engine_patch_radios.cpp")), sig);
            std::printf("  wiring %-32s scope %d beat %d\n", sig, body.find("PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop)") != std::string::npos ? 1 : 0,
                        body.find("controlWatchdog_.heartbeat()") != std::string::npos ? 1 : 0);
            CHECK(!body.empty());
            CHECK(body.find("PassPhaseScope inStop(cascade::core::stall::PassPhase::Stop)") != std::string::npos);
            CHECK(body.find("controlWatchdog_.heartbeat()") != std::string::npos);
        }
        checkTheWiringCheckerItself();
        // THE PATCH RADIO'S WORST BETWEEN TWO BEATS, from the constants READ OUT OF THE SOURCES in A: the source's stop (lock wait + vendor call + the restart give-back) + the reader's join
        // (PatchRadio::kStopWaitMs, read from patch_radio.hpp) + the close in the destructor (lock wait + give-back + vendor call), as the radio's removal does; it must fit under the Stop threshold.
        {
            const long lock = declared(readFile(root / "src/source/soapy_source.cpp"), "kControlLockWait");
            const long vendor = declared(readFile(root / "src/source/soapy_source.cpp"), "kVendorCallWait");
            const long giveBack = declared(readFile(root / "src/source/soapy_source.hpp"), "kRestartGiveBack");
            const std::string pr = readFile(root / "src/core/patch_radio.hpp");
            const std::size_t k = pr.find("kStopWaitMs = ");
            const long join = k == std::string::npos ? -1 : std::atol(pr.c_str() + k + 14);
            const long oneRadio = lock + vendor + giveBack + join + lock + giveBack + vendor;
            std::printf("  a patch radio's removal: stop %ld + join %ld + close %ld = %ld ms (threshold %u ms)\n", lock + vendor + giveBack, join, lock + giveBack + vendor, oneRadio, stall::kStopThresholdMs);
            CHECK(lock > 0 && vendor > 0 && giveBack > 0 && join > 0);
            CHECK(static_cast<unsigned>(oneRadio) + stall::kStopReserveMs < stall::kStopThresholdMs);
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
