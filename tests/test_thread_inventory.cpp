// test_thread_inventory.cpp - the thread registry's inventory is held to the source, the way test_shutdown_budget holds the
// shutdown budget to every bounded wait it can FIND.
//
// docs/engine-stage4.md 6.27. core/thread_beats.hpp is a table of named threads and the budgets they are held to. A table
// nobody checks drifts: a new worker is added with no heartbeat, a heartbeat is left behind in a function that was deleted, a
// budget is halved because a test was flaky. This test reads src/ and fails by file name when any of those happens:
//
//   1. EVERY SPAWN IS ACCOUNTED FOR. The scan finds every `std::thread` construction and every `std::async` under src/ (comments
//      and string literals blanked: a mention is not a use) and holds each file's count to the table below. A file with a spawn
//      the table has never heard of fails by name; so does a row whose file no longer spawns what it says. Each spawn is either
//      COVERED by a registration (a `beats::Registration` in the same file - the thread beats) or EXEMPT with the reason
//      written next to it. There is no third state.
//   2. EVERY REGISTRATION IS A THREAD OF THE INVENTORY. A `Registration` whose spec is not in thread_beats.hpp's kAllThreads fails
//      to compile; one whose spec is in the table and in no source file fails here (an inventory entry nobody beats for), and a
//      source file that registers more or fewer threads than its row says fails by name.
//   3. THE INVENTORY AND THE HEADER AGREE. Every `inline constexpr ThreadSpec kThread...` declared in the header is in
//      kAllThreads, and nothing is listed twice.
//   4. THE CALLS ARE THE PRODUCT'S. The test seams (advanceClockForTest, setBudgetOverrideForTest, clearOverridesForTest,
//      resetWedgeFileCountForTest) are called from no file under src/: a budget that a product path could override is not a
//      budget.
//   5. THE WORKER MONITOR IS WIRED. HangWatchdog::threadMain calls pollThreadBeats on every wake, before the frame loop's own
//      rules (which have `continue`s that would skip it), and the wedge report is written through captureAllThreads with the
//      finding. Without that call the registry is a table nobody reads: this is the check that goes red when the monitor is
//      removed (the wedge tests in test_thread_beats.cpp go red with it, for the behavioural reason).
//   6. THE TX DEAD-MAN IS UNTOUCHED. thread_beats.cpp and hang_watchdog.cpp name nothing of the transmitter's key (no key-up,
//      no setLatched, no tick): the monitor reads one probe and that is all.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "core/thread_beats.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace beats = cascade::core::beats;

#ifndef CASCADE_SOURCE_DIR
#error "CASCADE_SOURCE_DIR must be defined by the build"
#endif

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

// Comments and string/char literals blanked to spaces (newlines kept): a mention in a comment or a log line is not a use.
std::string liveCode(const std::string& s) {
    std::string out = s;
    enum { Code, Line, Block, Str, Chr, Raw } st = Code;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = s[i];
        const char n = i + 1 < s.size() ? s[i + 1] : '\0';
        switch (st) {
            case Code:
                if (c == '/' && n == '/') { st = Line; out[i] = ' '; }
                else if (c == '/' && n == '*') { st = Block; out[i] = ' '; }
                else if (c == 'R' && n == '"') {
                    // a raw string literal: blank to the closing )delim"
                    const std::size_t open = s.find('(', i + 2);
                    if (open != std::string::npos) {
                        const std::string delim = ")" + s.substr(i + 2, open - (i + 2)) + "\"";
                        const std::size_t close = s.find(delim, open);
                        if (close != std::string::npos) {
                            for (std::size_t k = i; k < close + delim.size(); ++k) {
                                if (s[k] != '\n') { out[k] = ' '; }
                            }
                            i = close + delim.size() - 1;
                        }
                    }
                }
                else if (c == '"') { st = Str; }
                else if (c == '\'' ) {
                    // a digit separator (1'000) is not a character literal
                    const bool sep = i > 0 && (std::isalnum(static_cast<unsigned char>(s[i - 1])) != 0) && std::isalnum(static_cast<unsigned char>(n)) != 0;
                    if (!sep) { st = Chr; }
                }
                break;
            case Line:
                if (c == '\n') { st = Code; } else { out[i] = ' '; }
                break;
            case Block:
                if (c == '*' && n == '/') { out[i] = ' '; out[i + 1] = ' '; ++i; st = Code; }
                else if (c != '\n') { out[i] = ' '; }
                break;
            case Str:
                if (c == '\\') { out[i] = ' '; if (i + 1 < out.size()) { out[++i] = ' '; } }
                else if (c == '"') { st = Code; }
                else if (c != '\n') { out[i] = ' '; }
                break;
            case Chr:
                if (c == '\\') { out[i] = ' '; if (i + 1 < out.size()) { out[++i] = ' '; } }
                else if (c == '\'') { st = Code; }
                else { out[i] = ' '; }
                break;
            case Raw:
                break;
        }
    }
    return out;
}

std::size_t countRegex(const std::string& text, const std::regex& re) {
    return static_cast<std::size_t>(std::distance(std::sregex_iterator(text.begin(), text.end(), re), std::sregex_iterator()));
}

// ---------------------------------------------------------------------------------------------------------------------------
// THE TABLE. One row per file under src/ that creates a thread or an async job. `covered` is how many of that file's spawns
// are covered by a registration; the others are exempt, and `why` says why in a sentence (required when any are).
//   regs     - `beats::Registration` constructions in the file (the spec identifiers are in `specs`, comma separated)
//   threads  - `std::thread` constructions (a temporary, `std::thread t(...)`, or an assignment from one)
//   asyncs   - `std::async` calls
// ---------------------------------------------------------------------------------------------------------------------------
struct Row {
    const char* file;
    int threads;
    int asyncs;
    int regs;
    int covered;
    const char* specs;
    const char* why;
};

#include "thread_inventory_rows.inc"

struct Found {
    int threads = 0;
    int asyncs = 0;
    int regs = 0;
    std::set<std::string> specs;
};

}  // namespace

int main(int argc, char** argv) {
    const fs::path root(CASCADE_SOURCE_DIR);
    const bool list = argc > 1 && std::strcmp(argv[1], "--list") == 0;

    // ---- 3. the header's own table ------------------------------------------------------------------------------------
    std::set<std::string> declared;  // kThread identifiers declared
    std::map<std::string, std::string> nameOf;
    {
        const std::string h = readFile(root / "src/core/thread_beats.hpp");
        const std::regex decl(R"(inline constexpr ThreadSpec (kThread\w+)\{\"([a-z0-9-]+)\", Kind::(\w+)\};)");
        for (auto it = std::sregex_iterator(h.begin(), h.end(), decl); it != std::sregex_iterator(); ++it) {
            CHECK(declared.insert((*it)[1]).second);
            nameOf[(*it)[1]] = (*it)[2];
        }
        std::set<std::string> listed;
        const std::size_t at = h.find("kAllThreads[] = {");
        CHECK(at != std::string::npos);
        const std::size_t end = h.find("};", at);
        const std::string body = h.substr(at, end - at);
        const std::regex ref(R"(&(kThread\w+))");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), ref); it != std::sregex_iterator(); ++it) {
            if (!listed.insert((*it)[1]).second) { std::printf("FAIL %s is listed twice in kAllThreads\n", (*it)[1].str().c_str()); }
            CHECK(listed.size() > 0);
        }
        if (listed != declared) { std::printf("FAIL kAllThreads and the declared specs differ (%zu listed, %zu declared)\n", listed.size(), declared.size()); }
        CHECK(listed == declared);
        CHECK(declared.size() == beats::kThreadCount);
    }

    // ---- 1 + 2. the scan ------------------------------------------------------------------------------------------------
    const std::regex threadRe(R"(std::thread\s*\(|std::thread\s+[A-Za-z_]\w*\s*[({])");
    const std::regex asyncRe(R"(std::async\s*\()");
    const std::regex regRe(R"(beats::Registration\s+\w+\s*[({]\s*(?:cascade::core::beats::|beats::)?(kThread\w+))");
    std::map<std::string, Found> found;
    for (const auto& e : fs::recursive_directory_iterator(root / "src")) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".inl") { continue; }
        const std::string rel = fs::relative(e.path(), root).generic_string();
        const std::string code = liveCode(readFile(e.path()));
        Found f;
        f.threads = static_cast<int>(countRegex(code, threadRe));
        f.asyncs = static_cast<int>(countRegex(code, asyncRe));
        for (auto it = std::sregex_iterator(code.begin(), code.end(), regRe); it != std::sregex_iterator(); ++it) {
            ++f.regs;
            f.specs.insert((*it)[1]);
        }
        if (f.threads != 0 || f.asyncs != 0 || f.regs != 0) { found[rel] = f; }
    }
    // the registry's own file mentions Registration in its definitions, not as a use
    found.erase("src/core/thread_beats.hpp");
    found.erase("src/core/thread_beats.cpp");

    if (list) {
        for (const auto& kv : found) {
            std::string specs;
            for (const std::string& s : kv.second.specs) { specs += (specs.empty() ? "" : ",") + s; }
            std::printf("{\"%s\", %d, %d, %d, %d, \"%s\", \"\"},\n", kv.first.c_str(), kv.second.threads, kv.second.asyncs,
                        kv.second.regs, kv.second.regs, specs.c_str());
        }
        return 0;
    }

    std::map<std::string, const Row*> rows;
    for (const Row& r : kRows) {
        CHECK(rows.emplace(r.file, &r).second);  // one row per file
    }
    int spawns = 0;
    int covered = 0;
    int exempt = 0;
    std::set<std::string> specsUsed;
    for (const auto& kv : found) {
        const std::string& file = kv.first;
        const Found& f = kv.second;
        spawns += f.threads + f.asyncs;
        const auto it = rows.find(file);
        if (it == rows.end()) {
            std::printf("*** UNACCOUNTED THREADS: %s creates %d std::thread and %d std::async and registers %d heartbeat(s), "
                        "and tests/thread_inventory_rows.inc has never heard of it.\n"
                        "*** Give the thread a heartbeat (core/thread_beats.hpp: a ThreadSpec, a Registration on the thread,\n"
                        "*** beat() at its natural cadence, wait() for a declared wait) or add a row with the reason it is exempt.\n",
                        file.c_str(), f.threads, f.asyncs, f.regs);
            CHECK(false);
            continue;
        }
        const Row& r = *it->second;
        if (r.threads != f.threads || r.asyncs != f.asyncs || r.regs != f.regs) {
            std::printf("*** %s: the table says %d thread(s), %d async, %d registration(s); the source has %d, %d, %d\n", file.c_str(),
                        r.threads, r.asyncs, r.regs, f.threads, f.asyncs, f.regs);
        }
        CHECK(r.threads == f.threads);
        CHECK(r.asyncs == f.asyncs);
        CHECK(r.regs == f.regs);
        // the specs the row names are the specs the file registers
        std::set<std::string> want;
        {
            std::string s = r.specs;
            std::size_t at = 0;
            while (at < s.size()) {
                const std::size_t comma = s.find(',', at);
                const std::string one = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!one.empty()) { want.insert(one); }
                if (comma == std::string::npos) { break; }
                at = comma + 1;
            }
        }
        if (want != f.specs) { std::printf("*** %s: the row names a different set of ThreadSpecs than the file registers\n", file.c_str()); }
        CHECK(want == f.specs);
        for (const std::string& s : f.specs) {
            specsUsed.insert(s);
            if (declared.count(s) == 0) { std::printf("*** %s registers %s, which thread_beats.hpp does not declare\n", file.c_str(), s.c_str()); }
            CHECK(declared.count(s) == 1);
        }
        const int total = f.threads + f.asyncs;
        CHECK(r.covered >= 0 && r.covered <= total);
        covered += r.covered;
        const int unregistered = total - r.covered;
        exempt += unregistered;
        if (unregistered > 0 && std::strlen(r.why) < 25) {
            std::printf("*** %s: %d spawn(s) with no heartbeat and no reason written down\n", file.c_str(), unregistered);
            CHECK(false);
        }
        if (unregistered == 0 && std::strlen(r.why) != 0) {
            std::printf("*** %s: a reason for an exemption nothing needs\n", file.c_str());
            CHECK(false);
        }
        // a registration needs a spawn in the same file to beat on... except where the registered function is run by another
        // file's spawn (none today); that would need a row note, and is refused until it exists
        if (f.regs > 0) { CHECK(total > 0); }
    }
    for (const Row& r : kRows) {
        if (found.count(r.file) == 0) {
            std::printf("*** A ROW WITH NO FILE ACTIVITY: %s is in tests/thread_inventory_rows.inc and no longer spawns or registers.\n", r.file);
            CHECK(false);
        }
    }
    // every thread in the inventory is registered by some file
    for (const std::string& d : declared) {
        if (specsUsed.count(d) == 0) { std::printf("*** %s (%s) is in the inventory and no file registers it\n", d.c_str(), nameOf[d].c_str()); }
        CHECK(specsUsed.count(d) == 1);
    }
    std::printf("thread inventory: %zu files spawn threads or jobs, %d spawn site(s), %d covered by a heartbeat, %d exempt with a reason; "
                "%zu inventory threads, all registered\n",
                found.size(), spawns, covered, exempt, declared.size());

    // ---- 3b. every phase a thread says is a plain word --------------------------------------------------------------
    // A phase is written into a report (`thread-phase:`) after reportWord() has checked it; a phase that fails the check reads as
    // "unknown", which is a report that names nothing. The literals in `beat.beat("...")`, `dspBeat("...")` and the like are
    // held to the same rule here, so the failure is a red test and not a mute report.
    {
        const std::regex phaseRe(R"((?:beat|dspBeat)\s*(?:\.beat)?\s*\(\s*\"([^\"]*)\"\s*\))");
        std::size_t phases = 0;
        for (const auto& e : fs::recursive_directory_iterator(root / "src")) {
            if (!e.is_regular_file()) { continue; }
            const std::string ext = e.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp") { continue; }
            const std::string rel = fs::relative(e.path(), root).generic_string();
            if (rel == "src/core/thread_beats.hpp" || rel == "src/core/thread_beats.cpp") { continue; }
            // (the whole text: the literal is what is being read; a mention in a comment is a plain word too, or a finding)
            const std::string text = readFile(e.path());
            for (auto it = std::sregex_iterator(text.begin(), text.end(), phaseRe); it != std::sregex_iterator(); ++it) {
                const std::string phase = (*it)[1];
                ++phases;
                if (beats::reportWord(phase.c_str()) != phase) {
                    std::printf("*** %s says the phase \"%s\", which is not a plain word\n", rel.c_str(), phase.c_str());
                }
                CHECK(beats::reportWord(phase.c_str()) == phase);
            }
        }
        std::printf("thread inventory: %zu phase literal(s), all plain words\n", phases);
        CHECK(phases >= 40);
    }

    // ---- 4. the test seams are the tests' ----------------------------------------------------------------------------
    for (const auto& e : fs::recursive_directory_iterator(root / "src")) {
        if (!e.is_regular_file()) { continue; }
        const std::string rel = fs::relative(e.path(), root).generic_string();
        if (rel == "src/core/thread_beats.hpp" || rel == "src/core/thread_beats.cpp" || rel == "src/core/hang_watchdog.hpp" ||
            rel == "src/core/hang_watchdog.cpp") {
            continue;
        }
        const std::string code = liveCode(readFile(e.path()));
        for (const char* seam : {"advanceClockForTest", "setBudgetOverrideForTest", "clearOverridesForTest", "resetWedgeFileCountForTest", "setBeatsOvershootMsForTest", "freezeClockForTest"}) {
            if (code.find(seam) != std::string::npos) { std::printf("*** %s calls the test seam %s\n", rel.c_str(), seam); }
            CHECK(code.find(seam) == std::string::npos);
        }
    }
    {
        const std::string wd = liveCode(readFile(root / "src/core/hang_watchdog.cpp"));
        CHECK(wd.find("resetWedgeFileCountForTest() {") != std::string::npos);  // defined here...
        // ...and called by no product path in this file
        std::size_t uses = 0;
        for (std::size_t at = wd.find("resetWedgeFileCountForTest"); at != std::string::npos; at = wd.find("resetWedgeFileCountForTest", at + 1)) { ++uses; }
        CHECK(uses == 1);
    }

    // ---- 5. the monitor is wired -------------------------------------------------------------------------------------
    {
        const std::string wd = liveCode(readFile(root / "src/core/hang_watchdog.cpp"));
        const std::size_t tm = wd.find("void HangWatchdog::threadMain()");
        CHECK(tm != std::string::npos);
        const std::size_t poll = wd.find("pollThreadBeats(pollInterval);", tm);
        CHECK(poll != std::string::npos);
        // before the first of the frame loop's `continue`s
        const std::size_t firstContinue = wd.find("continue;", tm);
        CHECK(poll != std::string::npos && firstContinue != std::string::npos && poll < firstContinue);
        // the report is written through the shared capture, with the finding
        CHECK(wd.find("captureAllThreads(dir + name, static_cast<double>(f.stalledMs), &f)") != std::string::npos);
        // and the poll reads the registry
        CHECK(wd.find("reg.poll()") != std::string::npos);
    }

    // ---- 6. the dead-man is the transmitter's own ------------------------------------------------------------------
    for (const char* file : {"src/core/thread_beats.cpp", "src/core/thread_beats.hpp", "src/core/hang_watchdog.cpp"}) {
        const std::string code = liveCode(readFile(root / file));
        for (const char* key : {"setLatched", "setPttHeld", "keyUp", "keyDown", "frontEndAlive", "unkey", "Transmitter"}) {
            if (code.find(key) != std::string::npos) { std::printf("*** %s names %s: the monitor must not touch the transmitter's key\n", file, key); }
            CHECK(code.find(key) == std::string::npos);
        }
    }
    // the one place the engine hands the monitor anything is a READ of the transmitter's flag
    {
        const std::string eng = liveCode(readFile(root / "src/engine/engine.cpp"));
        CHECK(eng.find("setKeyedProbe(this, [this] { return transmitter_.transmitting(); })") != std::string::npos);
        CHECK(eng.find("clearKeyedProbe(this)") != std::string::npos);
    }

    return testSummary("test_thread_inventory");
}
