// test_engine_api_wire_c_sys.cpp - the SYSTEM operations of wiring lane C in the real engine (src/engine/wire_c_sys.{hpp,cpp}, engine_wire_c.cpp; models core/sys_bundle,
// sys_reset, sys_first_run), over a scratch settings folder named foxsdr.
//
//   (Y1 to Y4, the export, import, backup and reset of the engine, are removed with their code: owner decision 74, engine lane E1; the SYSTEM page's Settings and backup section is the one copy.)
//   Y5  first-run wizard: needed until done; a step without its choice is refused; the chosen set of stations is added; usage reports changes ONLY if the person chose it; the done marker;
//       skip from the start; a choice that does not fit is refused
//   Y6  a REMOTE session cannot send any of them
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>

#include "core/config.hpp"
#include "engine/wire_c_portable.hpp"
#include "engine/wire_c_sys.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

void writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

FoxCommand textCmd(std::uint32_t op, const std::string& text, std::int64_t a = 0, std::int64_t b = 0) {
    FoxCommand c = testing::cmd(op, 0.0, a, text.c_str());
    c.ival[1] = b;
    return c;
}

const char* kConfig = "{\"volume\":0.5,\"sourceKind\":\"rtl\",\"webPassword\":\"SECRET-DO-NOT-EXPORT\",\"installId\":\"abcdef0123456789\"}";
const char* kBookmarks = "{\"version\":1,\"bookmarks\":[]}";

fs::path settingsDir(const char* tag) {
    const fs::path root = scratch(tag);
    const fs::path d = root / "foxsdr";   // the settings folder must be named foxsdr (the reset's guard)
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

int bookmarkCount(Rig& r) { return api()->read_list(r.local->raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr); }

// Y5
void testFirstRun() {
    const fs::path dir = settingsDir("sysF");
    Rig r(dir);
    CHECK(r.fact("sys.firstrun.needed") == "1");   // no config.json, no marker
    CHECK(r.fact("sys.firstrun.step") == "welcome");
    CHECK(r.fact("sys.firstrun.radios").find("RTL-SDR dongle") != std::string::npos);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.step") == "radio");
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_BAD_ARGUMENT);   // no radio chosen yet
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "radio:99")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "bogus")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "radio:1")) == FOXAPI_OK);   // the second choice (an RTL-SDR dongle)
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.step") == "preset");
    const std::string presets = r.fact("sys.firstrun.presets");
    CHECK(!presets.empty());
    const std::string firstId = presets.substr(0, presets.find('\t'));
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "preset:not-a-set")) == FOXAPI_BAD_ARGUMENT);
    const int before = bookmarkCount(r);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "preset:" + firstId)) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.step") == "privacy");
    CHECK(r.fact("sys.firstrun.done") == "0");
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "telemetry:0")) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.step") == "done" && r.fact("sys.firstrun.done") == "1");
    CHECK(bookmarkCount(r) > before);   // the chosen set of stations was added
    CHECK(fs::exists(dir / cascade::core::sys::kFirstRunMarker));
    CHECK(r.fact("sys.firstrun.needed") == "0");
}

void testFirstRunSkip() {
    const fs::path dir = settingsDir("sysG");
    Rig r(dir);
    const int before = bookmarkCount(r);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "skip")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.done") == "1");
    CHECK(bookmarkCount(r) == before);   // a skip adds nothing
    CHECK(fs::exists(dir / cascade::core::sys::kFirstRunMarker));
    CHECK(r.fact("sys.firstrun.needed") == "0");
}

// Y6
void testRemote() {
    const fs::path dir = settingsDir("sysH");
    writeText(dir / "config.json", kConfig);
    Rig r(dir);
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    {
        const testing::Outcome o = r.sendOn(remote, textCmd(FOXAPP_OP_SYS_FIRSTRUN, "skip"));
        CHECK_EQ(o.status(), FOXAPI_DENIED);
    }
    // THE RETIRED OPS (owner decision 74): numbers 0x8A03..0x8A09 answer nothing here: the engine does not know them
    for (const std::uint32_t op : {0x8A03u, 0x8A04u, 0x8A05u, 0x8A06u, 0x8A07u, 0x8A08u, 0x8A09u}) {
        const testing::Outcome o = r.send(textCmd(op, "x"));
        CHECK(o.status() != FOXAPI_OK);
    }
    CHECK(r.fact("sys.firstrun.done") == "0");
    CHECK(!fs::exists(dir / "backups"));
    // the facts of the removed copy are gone with it
    for (const char* gone : {"sys.export.last", "sys.import.plan", "sys.import.token", "sys.import.notes", "sys.reset.plan", "sys.reset.token", "sys.backup.last", "sys.lastboot", "sys.pending"}) {
        CHECK(r.fact(gone) == "<absent>");
    }
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c_sys");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testFirstRun();
    testFirstRunSkip();
    testRemote();
    return check::finish("test_engine_api_wire_c_sys");
}
