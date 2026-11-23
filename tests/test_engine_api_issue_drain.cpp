// test_engine_api_issue_drain.cpp - THE ISSUE REPORTS A TEARDOWN ITSELF RAISES REALLY LAND (review of fix/freezes, must-fix 1).
//
// ReaderLeak and CloseLeak only raise a bit (health::raiseRecovered: lock-free, from a thread that must not take a lock); the bit becomes a written report in health::drainRecovered(),
// which the control pass ran, and the pass has stopped by the teardown. So Engine::shutdownDrain's 250 ms issue flush found no writer in flight and landed nothing for them. CatStop files
// directly and always landed. shutdownDrain (and ~Engine's own path) now drains first. This drives a REAL Engine::shutdownDrain with each of the three words raised and reads the folder.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/crash_handler.hpp"
#include "core/health_events.hpp"
#include "core/issue_report.hpp"
#include "engine/engine.hpp"
#include "engine_api_test_env.hpp"

namespace fs = std::filesystem;
namespace cc = cascade::core;
namespace health = cascade::core::health;
namespace issue = cascade::core::issue;
using cascade::engine::Engine;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> issueTexts(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("issue-", 0) == 0 && e.path().extension() == ".txt") { out.push_back(readFile(e.path())); }
    }
    return out;
}

bool has(const std::vector<std::string>& texts, health::Recovered r) {
    const std::string want = "\nreason: " + issue::issueReason(r) + "\n";
    for (const std::string& t : texts) {
        if (t.find(want) != std::string::npos) { return true; }
    }
    return false;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_issue_drain\n");
    engine_api_test::hermetic("issuedrain");
    const fs::path dir = fs::path(std::getenv("LOCALAPPDATA") != nullptr ? std::getenv("LOCALAPPDATA") : ".") / "issuedrain_reports";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    cc::CrashHandlerConfig on;
    on.crashDir = dir.string();
    on.enabled = true;
    on.minidump = false;
    on.exitAfterReport = false;
    cc::installCrashHandlers(on);
    issue::resetIssueReportsForTest();

    {
        Engine eng;
        eng.initialise();
        // What the teardown's own code does: two words as a bit (the pass that would drain them has stopped), one filed directly.
        health::raiseRecovered(health::Recovered::ReaderLeak);
        health::raiseRecovered(health::Recovered::CloseLeak);
        (void)issue::fileIssueReportOffThread(health::Recovered::CatStop);
        eng.shutdownDrain();
        // NO waiting for the writers here: shutdownDrain's flush is what is under test.
        const std::vector<std::string> texts = issueTexts(dir);
        std::printf("    %zu issue report(s) on disk after shutdownDrain\n", texts.size());
        CHECK(has(texts, health::Recovered::ReaderLeak));
        CHECK(has(texts, health::Recovered::CloseLeak));
        CHECK(has(texts, health::Recovered::CatStop));
    }
    fs::remove_all(dir, ec);
    return check::finish("test_engine_api_issue_drain");
}
