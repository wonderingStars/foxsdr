// test_engine_api_wireb_ftdi.cpp - THE SDR-IQ'S ROW IN THE ONE NEEDED-LIBRARY TABLE, THROUGH THE ENGINE (engine fixes 4, item 4): the operation NEEDED_LIB_CHECK 0x8930 with {"id":"ftdi-d2xx"} runs
// the SDR-IQ bridge's own check (source/sdriq_d2xx.cpp checkFtd2xx: loads the library silently, checks the eight calls, unloads) and reports it with the six states of the owner's rule.
// The machine is a STAND-IN: the ftd2xx.dll this project builds from FTDI's published programmer's guide (tests/fixtures/ftd2xx_standin.cpp), the partial and the needs-a-dependency
// libraries beside it, text and 32-bit files made here; no real FTDI driver is looked at (the PATH is held on a folder of this test's) and no device is opened.
//
//   F1  the six states through the operation: working (depth "loaded", the file, its folder, no full path), not-found, not-a-library, wrong-32-bit (when the system has a 32-bit DLL),
//       too-old (names the missing call), would-not-load; each is the row's `check`, and only the row asked is changed
//   F2  CHECK AGAIN WITHOUT A RESTART: empty folder -> not-found, the stand-in "installed" -> working, removed -> not-found, in one engine
//   F3  the PATH: with no path given the row looks on the PATH (the stand-in's folder) and says so
//   F4  the loader is asked exactly once for a library this row loads, and never for a header-only row (winradio-g39ddc beside it): the counter moves by one, then by none
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "eol_fixture.hpp"
#include "engine/engine_wire_b.hpp"
#include "source/needed_library.hpp"
#include "source/vendor_lib.hpp"
#include "wire1_rig.hpp"

namespace {

namespace fs = std::filesystem;

testing::Outcome sendText(Rig& r, std::uint32_t op, const std::string& text) {
    FoxCommand c = testing::cmd(op, 0.0, 0, nullptr);
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = text.empty() ? api()->submit(r.s->raw(), &c, 1, &sr) : api()->submit_text(r.s->raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 60 && !o.landed; ++pass) {
        r.eng.controlPass();
        FoxCommandResult res[16];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(r.s->raw(), res, 16);
        for (int32_t i = 0; i < got; ++i) { if (res[i].ticket == sr.ticket) { o.landed = true; o.result = res[i]; } }
    }
    return o;
}

std::string rawFact(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    return n >= 0 ? std::string(buf.c_str()) : std::string("<absent>");
}

json row(Rig& r, const std::string& id) {
    const json all = json::parse(rawFact(r, "needed.libs"), nullptr, false);
    if (all.is_array()) { for (const json& j : all) { if (j.value("library_id", std::string()) == id) { return j; } } }
    return json();
}

std::string quoted(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

void setPath(const std::string& v) {
    _putenv_s("PATH", v.c_str());
    SetEnvironmentVariableA("PATH", v.c_str());
}

std::string state(Rig& r, const std::string& id) { return row(r, id)["check"].value("state", std::string("<none>")); }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_ftdi\n");
    engine_api_test::hermetic("wirebftdi");
    Access::installHooks();
    Rig r;
    namespace vl = cascade::source::vendorlib;
    const fs::path fx = eol::fixtureDir();
    const fs::path tmp = eol::scratch("wbftdi");
    std::error_code ec;
    const char* oldPath = std::getenv("PATH");
    const std::string savedPath = oldPath != nullptr ? oldPath : "";
    fs::create_directories(tmp / "empty");
    setPath((tmp / "empty").string());   // no real FTDI driver is looked at: restored before the end
    CHECK(fs::exists(fx / "ftd2xx.dll", ec));
    CHECK(fs::exists(fx / "eol_partial.dll", ec));

    std::printf("F1: the six states\n");
    {
        CHECK(row(r, "ftdi-d2xx")["check"].is_null());   // never checked yet
        const std::uint64_t loads0 = vl::Library::silentLoadCount();
        const auto o = sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(fx) + "}");
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(o.result.applied[0], 1.0);
        json c = row(r, "ftdi-d2xx")["check"];
        std::printf("  working: %s\n", c.dump().c_str());
        CHECK_EQ(c.value("state", std::string()), std::string("working"));
        CHECK_EQ(c.value("depth", std::string()), std::string("loaded"));
        CHECK_EQ(c.value("file", std::string()), std::string("ftd2xx.dll"));
        CHECK(!c.value("found_in", std::string()).empty());
        CHECK(c.value("found_in", std::string()).find("ftd2xx.dll") == std::string::npos);   // a folder, not a file path
        CHECK(c.value("sentence", std::string()).find("Found and working") != std::string::npos);
        CHECK(c.value("missing", std::string("?")).empty());
        CHECK_EQ(vl::Library::silentLoadCount(), loads0 + 1);   // this row loads the stand-in once (and unloads it)
        CHECK_EQ(row(r, "winradio-g39ddc")["check"].is_null(), true);   // only the row asked was checked

        fs::create_directories(tmp / "none");
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(tmp / "none") + "}").status(), FOXAPI_OK);
        CHECK_EQ(state(r, "ftdi-d2xx"), std::string("not-found"));

        fs::create_directories(tmp / "text");
        { std::ofstream f(tmp / "text" / "ftd2xx.dll", std::ios::binary); f << "a web page saved as a dll"; }
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(tmp / "text") + "}").status(), FOXAPI_OK);
        CHECK_EQ(state(r, "ftdi-d2xx"), std::string("not-a-library"));

        const fs::path sys32 = "C:/Windows/SysWOW64/kernel32.dll";
        if (fs::exists(sys32, ec)) {
            fs::create_directories(tmp / "x32");
            fs::copy_file(sys32, tmp / "x32" / "ftd2xx.dll", fs::copy_options::overwrite_existing, ec);
            CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(tmp / "x32") + "}").status(), FOXAPI_OK);
            CHECK_EQ(state(r, "ftdi-d2xx"), std::string("wrong-32-bit"));
            CHECK_EQ(row(r, "ftdi-d2xx")["check"].value("depth", std::string()), std::string("headers"));   // refused from its bytes: the loader was not asked
        } else { std::printf("  (no 32-bit system DLL here: skipped, not passed)\n"); }

        fs::create_directories(tmp / "partial");
        fs::copy_file(fx / "eol_partial.dll", tmp / "partial" / "ftd2xx.dll", fs::copy_options::overwrite_existing, ec);
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(tmp / "partial") + "}").status(), FOXAPI_OK);
        c = row(r, "ftdi-d2xx")["check"];
        std::printf("  too old: %s\n", c.dump().c_str());
        CHECK_EQ(c.value("state", std::string()), std::string("too-old"));
        CHECK(c.value("missing", std::string()).find("FT_ListDevices") != std::string::npos);

        fs::create_directories(tmp / "needsdep");
        fs::copy_file(fx / "eol_needs_dep.dll", tmp / "needsdep" / "ftd2xx.dll", fs::copy_options::overwrite_existing, ec);
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(tmp / "needsdep") + "}").status(), FOXAPI_OK);
        c = row(r, "ftdi-d2xx")["check"];
        std::printf("  would not load: %s\n", c.dump().c_str());
        CHECK_EQ(c.value("state", std::string()), std::string("would-not-load"));
        CHECK_EQ(c.value("depth", std::string()), std::string("loaded"));   // the loader was asked (silently) and refused
    }

    std::printf("F2: check again without a restart\n");
    {
        fs::create_directories(tmp / "later");
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(tmp / "later") + "}").status(), FOXAPI_OK);
        CHECK_EQ(state(r, "ftdi-d2xx"), std::string("not-found"));
        fs::copy_file(fx / "ftd2xx.dll", tmp / "later" / "ftd2xx.dll", fs::copy_options::overwrite_existing, ec);   // "installed"
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(tmp / "later") + "}").status(), FOXAPI_OK);
        CHECK_EQ(state(r, "ftdi-d2xx"), std::string("working"));
        fs::remove(tmp / "later" / "ftd2xx.dll", ec);   // the check let go of the file: nothing keeps it loaded
        CHECK(!ec);
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(tmp / "later") + "}").status(), FOXAPI_OK);
        CHECK_EQ(state(r, "ftdi-d2xx"), std::string("not-found"));
    }

    std::printf("F3: the PATH\n");
    {
        setPath(fx.string());
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\"}").status(), FOXAPI_OK);
        const json c = row(r, "ftdi-d2xx")["check"];
        std::printf("  PATH: %s\n", c.dump().c_str());
        CHECK_EQ(c.value("state", std::string()), std::string("working"));
        CHECK(c.value("looked_in", std::string()).find("PATH") != std::string::npos);
        setPath((tmp / "empty").string());
    }

    std::printf("F4: what loads and what does not\n");
    {
        fs::create_directories(tmp / "g39");
        const fs::path sys64 = "C:/Windows/System32/kernel32.dll";
        if (fs::exists(sys64, ec)) {
            fs::copy_file(sys64, tmp / "g39" / "G39DDCAPI.dll", fs::copy_options::overwrite_existing, ec);
            const std::uint64_t loads0 = vl::Library::silentLoadCount();
            CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"winradio-g39ddc\",\"path\":" + quoted(tmp / "g39") + "}").status(), FOXAPI_OK);
            CHECK_EQ(state(r, "winradio-g39ddc"), std::string("working"));
            CHECK_EQ(vl::Library::silentLoadCount(), loads0);   // header-only: the vendor's file is never mapped
            CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"ftdi-d2xx\",\"path\":" + quoted(fx) + "}").status(), FOXAPI_OK);
            CHECK_EQ(vl::Library::silentLoadCount(), loads0 + 1);   // the one row that loads
        } else { std::printf("  (no 64-bit system DLL here: skipped, not passed)\n"); }
    }

    setPath(savedPath);
    fs::remove_all(tmp, ec);
    return check::finish("test_engine_api_wireb_ftdi");
}
