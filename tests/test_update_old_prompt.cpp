// Tests for the update answer that prompts a desktop FoxSDR older than 0.99.64 (engine lane E1, owner decision 14). Those installs cannot be changed: only the
// answer the site gives them can. docs/multi-radio-records/engine-e1.md section "Hand-off for the site lane" says exactly what to send; this test holds that answer
// to BOTH readers: the parser of v0.99.63 (reproduced below from `git show v0.99.63:src/core/updater.cpp`, lines 168-240, the parts that read the answer) and the
// parser of this tree. The answer is a ONE-LINE prompt: one note with one line, marked critical so the banner is the red "Important update" one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/updater.hpp"
#include "test_check.hpp"

using nlohmann::json;
using cascade::core::compareVersions;
using cascade::core::parseUpdateManifest;
using cascade::core::UpdateInfo;
using cascade::core::wellFormedVersion;

namespace {

// THE ANSWER THE SITE SENDS to a request whose ?v= is older than 0.99.64.
std::string siteAnswer(const std::string& version, const std::string& line) {
    json note = {{"version", version}, {"date", "2026-11-16"}, {"critical", true}, {"notes", json::array({line})}};
    json j = {{"version", version},
              {"url", "https://foxsdr.com/downloads/foxsdr-setup-" + version + ".exe"},
              {"sha256", std::string(64, 'a')},
              {"size", 123456789u},
              {"notes", json::array({note})}};
    return j.dump();
}

// v0.99.63's reading of the answer, as it was (updater.cpp parseUpdateManifest): value() with a default THROWS type_error 302 for a field that is present with another type.
struct Old {
    bool ok = false;
    bool threw = false;
    std::string version, url, sha256, error;
    std::uint64_t size = 0;
    bool newer = false, critical = false;
    std::vector<std::vector<std::string>> noteLines;
};

Old oldParse(const std::string& text, const std::string& current) {
    Old o;
    try {
        json j = json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { o.error = "not JSON"; return o; }
        o.version = j.value("version", std::string());
        if (o.version.empty()) { o.error = "no version"; return o; }
        o.url = j.value("url", std::string());
        o.sha256 = j.value("sha256", std::string());
        if (!wellFormedVersion(o.version)) { o.error = "version"; return o; }
        if (o.sha256.size() != 64) { o.error = "sha"; return o; }
        const std::string pre = "https://";
        const std::size_t slash = o.url.find('/', pre.size());
        const std::string host = o.url.rfind(pre, 0) == 0 ? o.url.substr(pre.size(), slash == std::string::npos ? std::string::npos : slash - pre.size()) : std::string();
        if (!(host == "foxsdr.com" || (host.size() > 11 && host.compare(host.size() - 11, 11, ".foxsdr.com") == 0))) { o.error = "host"; return o; }
        if (j.contains("size") && j["size"].is_number_unsigned()) { o.size = j["size"].get<std::uint64_t>(); }
        o.newer = compareVersions(o.version, current) > 0;
        if (j.contains("notes") && j["notes"].is_array()) {
            for (const json& n : j["notes"]) {
                if (!n.is_object()) { continue; }
                const std::string v = n.value("version", std::string());
                (void)n.value("date", std::string());
                const bool crit = n.value("critical", false);
                std::vector<std::string> lines;
                if (n.contains("notes") && n["notes"].is_array()) { for (const json& l : n["notes"]) { if (l.is_string()) { lines.push_back(l.get<std::string>()); } } }
                if (!v.empty() && !lines.empty()) { o.noteLines.push_back(lines); if (crit && o.newer) { o.critical = true; } }
            }
        }
        o.ok = true;
    } catch (const std::exception& e) {
        o.threw = true;
        o.error = e.what();
    }
    return o;
}

void testAnswerOldAndNew() {
    const std::string line = "A newer FoxSDR fixes problems found in this version. Please update.";
    const std::string answer = siteAnswer("0.99.109", line);
    CHECK(answer.size() < 256 * 1024);   // under the old reader's manifest cap
    for (const char* running : {"0.55.0", "0.64.0", "0.75.0", "0.99.1", "0.99.50", "0.99.63"}) {
        const Old o = oldParse(answer, running);
        CHECK(o.ok);
        CHECK(!o.threw);
        CHECK(o.newer);                         // the banner is drawn
        CHECK(o.critical);                      // the red "Important update: FoxSDR 0.99.109"
        CHECK(o.noteLines.size() == 1);
        CHECK(o.noteLines.size() == 1 && o.noteLines[0].size() == 1 && o.noteLines[0][0] == line);   // ONE line, one bullet
        CHECK(o.size == 123456789u);
        // the same answer through this tree's parser
        UpdateInfo ui;
        std::string err;
        CHECK(parseUpdateManifest(answer, running, ui, err));
        CHECK(ui.newer && ui.critical && ui.version == "0.99.109");
        CHECK(ui.notes.size() == 1 && ui.notes[0].notes.size() == 1 && ui.notes[0].notes[0] == line);
    }
    // an install that is already current is not nagged by the same answer
    CHECK(!oldParse(answer, "0.99.109").newer);
    CHECK(!oldParse(answer, "0.99.110").newer);
}

void testWhatMakesTheOldReaderRefuseOrThrow() {
    // each is what the SITE MUST NOT SEND to an old install
    json base = json::parse(siteAnswer("0.99.109", "x"));
    {   // a version that is not a string: the old reader THROWS (an uncaught type_error on its worker thread)
        json j = base; j["version"] = 109;
        CHECK(oldParse(j.dump(), "0.99.63").threw);
    }
    {   // a note whose critical is not a boolean
        json j = base; j["notes"][0]["critical"] = "yes";
        CHECK(oldParse(j.dump(), "0.99.63").threw);
    }
    {   // a note date that is not a string
        json j = base; j["notes"][0]["date"] = 20261116;
        CHECK(oldParse(j.dump(), "0.99.63").threw);
    }
    {   // a url off foxsdr.com is refused
        json j = base; j["url"] = "https://example.invalid/foxsdr-setup.exe";
        const Old o = oldParse(j.dump(), "0.99.63");
        CHECK(!o.ok && o.error == "host");
    }
    {   // plain http is refused
        json j = base; j["url"] = "http://foxsdr.com/foxsdr-setup.exe";
        CHECK(!oldParse(j.dump(), "0.99.63").ok);
    }
    {   // a checksum that is not 64 characters is refused
        json j = base; j["sha256"] = "abc";
        CHECK(!oldParse(j.dump(), "0.99.63").ok);
    }
    {   // a note with no version or no lines is dropped, so the banner would show no line at all
        json j = base; j["notes"][0]["notes"] = json::array();
        const Old o = oldParse(j.dump(), "0.99.63");
        CHECK(o.ok && o.noteLines.empty());
    }
    {   // a pre-release suffix still parses, but only a release number compares as newer than the running one
        CHECK(compareVersions("0.99.109", "0.99.108") > 0);
        CHECK(compareVersions("0.99.109", "0.99.63") > 0);
    }
}

}  // namespace

int main() {
    testAnswerOldAndNew();
    testWhatMakesTheOldReaderRefuseOrThrow();
    return testSummary("test_update_old_prompt");
}
