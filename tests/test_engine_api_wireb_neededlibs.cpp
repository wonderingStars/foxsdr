// test_engine_api_wireb_neededlibs.cpp - THE NEEDED LIBRARIES AS STRUCTURED DATA (wire-b; the owner's needed-library rule; FOXAPP_OP_NEEDED_LIB_CHECK 0x8930, fact needed.libs;
// engine/wire_b_needed_libs.{hpp,cpp}). The machine is FABRICATED: a temp folder with a library that is really a system DLL copied under the maker's file name, a text file under
// that name, and a SoapySDR module folder; nothing is downloaded, opened or copied by the code under test, and no vendor code is loaded (the check reads headers).
//
//   N1  the fact is there from the start: every row carries its structure (id, kind, file names, minimum version note, 64-bit, why, page status "unverified" and NO address, steps, licence,
//       "does not include or supply it", fetch allowed "no ..."), "check" is null
//   N2  checking everything: a vendor library not on the PATH is not-found and says where it looked; the program / remote / file rows are cannot-check with a sentence; nothing is found
//   N3  a user-picked FOLDER holding a text file named G39DDCAPI.dll is unusable (not a library); the same name holding a real 64-bit DLL is found, with the folder and no full path; a
//       32-bit DLL is wrong-32-bit (when the system has one); a picked path is refused for a program row
//   N4  SoapySDR modules: a fabricated install with a module folder and a file with 'bladerf' in its name is found, 'pluto' is not-found; an empty machine is not-found with a sentence
//   N5  REMOTE sessions are DENIED
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "engine/engine_wire_b.hpp"
#include "wire1_rig.hpp"

namespace {

namespace fs = std::filesystem;

testing::Outcome sendText(Rig& r, std::uint32_t op, const std::string& text, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, 0, nullptr);
    testing::Session& sess = who != nullptr ? *who : *r.s;
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = text.empty() ? api()->submit(sess.raw(), &c, 1, &sr) : api()->submit_text(sess.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 60 && !o.landed; ++pass) {
        r.eng.controlPass();
        FoxCommandResult res[16];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(sess.raw(), res, 16);
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

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_neededlibs\n");
    engine_api_test::hermetic("wirebneededlibs");
    Access::installHooks();
    Rig r;
    cascade::engine::WireBState& w = r.eng.wireBStateForTest();

    // the fabricated machine: no environment, a temp tree
    const fs::path tmp = fs::temp_directory_path() / ("wb_needed_" + std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    fs::create_directories(tmp / "textonly");
    fs::create_directories(tmp / "good");
    fs::create_directories(tmp / "x32");
    fs::create_directories(tmp / "conda" / "lib" / "SoapySDR" / "modules0.8");
    { std::ofstream f(tmp / "textonly" / "G39DDCAPI.dll", std::ios::binary); f << "this is a web page saved as a dll"; }
    std::error_code ec;
    const fs::path sys64 = "C:/Windows/System32/kernel32.dll", sys32 = "C:/Windows/SysWOW64/kernel32.dll";
    const bool have64 = fs::exists(sys64, ec), have32 = fs::exists(sys32, ec);
    if (have64) { fs::copy_file(sys64, tmp / "good" / "G39DDCAPI.dll", fs::copy_options::overwrite_existing, ec); }
    if (have32) { fs::copy_file(sys32, tmp / "x32" / "G39DDCAPI.dll", fs::copy_options::overwrite_existing, ec); }
    { std::ofstream f(tmp / "conda" / "lib" / "SoapySDR" / "modules0.8" / "bladeRFSupport.dll", std::ios::binary); f << "x"; }
    const std::string root = (tmp / "conda").generic_string();
    w.neededEnv = cascade::engine::wireb::defaultNeededEnv();
    bool withRoot = false;
    w.neededEnv.getenv = [&](const char* n) -> std::string { return withRoot && std::strcmp(n, "SOAPY_SDR_ROOT") == 0 ? root : std::string(); };

    std::printf("N1: the structure, from the start\n");
    {
        const json all = json::parse(rawFact(r, "needed.libs"), nullptr, false);
        CHECK(all.is_array());
        CHECK_EQ(all.size(), static_cast<std::size_t>(8));
        const json g = row(r, "winradio-g39ddc");
        CHECK(g.is_object());
        CHECK_EQ(g.value("source_kind", std::string()), std::string("g39ddc"));
        CHECK_EQ(g["file_names"][0].get<std::string>(), std::string("G39DDCAPI.dll"));
        CHECK(g["bits64"].get<bool>());
        CHECK_EQ(g.value("page_status", std::string()), std::string("unverified"));
        CHECK_EQ(g.value("official_page", std::string("?")), std::string());          // an unverified address is shown to nobody
        CHECK(g["steps"].size() >= 3);
        CHECK(g.value("not_supplied", std::string()).find("does not include or supply") != std::string::npos);
        CHECK(g.value("fetch_allowed", std::string()).rfind("no", 0) == 0);
        CHECK(g["check"].is_null());
        CHECK(!g.value("min_version_note", std::string()).empty());
        for (const json& j : all) {
            CHECK(!j.value("library_id", std::string()).empty() && !j.value("why", std::string()).empty() && !j.value("licence", std::string()).empty() && !j.value("install_kind", std::string()).empty());
            CHECK_EQ(j.value("official_page", std::string("?")), std::string());
        }
    }

    std::printf("N2: check everything on an empty machine\n");
    {
        const auto o = sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{}");
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(o.result.applied[0], 0.0);
        const json g = row(r, "winradio-g39ddc");
        std::printf("  g39ddc: %s\n", g["check"].dump().c_str());
        CHECK_EQ(g["check"].value("state", std::string()), std::string("not-found"));
        CHECK(g["check"].value("sentence", std::string()).find("G39DDCAPI.dll") != std::string::npos);
        CHECK_EQ(row(r, "sdrplay-sdrconnect")["check"].value("state", std::string()), std::string("cannot-check"));
        CHECK_EQ(row(r, "soapyremote-server")["check"].value("state", std::string()), std::string("cannot-check"));
        CHECK_EQ(row(r, "icom-icr8600-firmware")["check"].value("state", std::string()), std::string("cannot-check"));
        CHECK_EQ(row(r, "soapy-bladerf")["check"].value("state", std::string()), std::string("not-found"));
    }

    std::printf("N3: a picked folder\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"winradio-g39ddc\",\"path\":" + quoted(tmp / "textonly") + "}").status(), FOXAPI_OK);
        json c = row(r, "winradio-g39ddc")["check"];
        std::printf("  text file: %s\n", c.dump().c_str());
        CHECK_EQ(c.value("state", std::string()), std::string("unusable"));
        if (have64) {
            const auto o = sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"winradio-g39ddc\",\"path\":" + quoted(tmp / "good") + "}");
            CHECK_EQ(o.status(), FOXAPI_OK);
            CHECK_EQ(o.result.applied[0], 1.0);
            c = row(r, "winradio-g39ddc")["check"];
            std::printf("  64-bit: %s\n", c.dump().c_str());
            CHECK_EQ(c.value("state", std::string()), std::string("found"));
            CHECK_EQ(c.value("file", std::string()), std::string("G39DDCAPI.dll"));
            CHECK(c.value("found_in", std::string()).find("good") != std::string::npos);
            CHECK(c.value("found_in", std::string()).find("G39DDCAPI.dll") == std::string::npos);   // a folder, not a file path
        } else { std::printf("  (no 64-bit system DLL here: skipped, not passed)\n"); }
        if (have32) {
            CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"winradio-g39ddc\",\"path\":" + quoted(tmp / "x32") + "}").status(), FOXAPI_OK);
            CHECK_EQ(row(r, "winradio-g39ddc")["check"].value("state", std::string()), std::string("wrong-32-bit"));
        } else { std::printf("  (no 32-bit system DLL here: skipped, not passed)\n"); }
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"sdrplay-sdrconnect\",\"path\":" + quoted(tmp) + "}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"nope\"}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"winradio-g39ddc\",\"colour\":1}").status(), FOXAPI_BAD_ARGUMENT);
    }

    std::printf("N4: SoapySDR modules\n");
    {
        withRoot = true;
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"soapy-bladerf\"}").status(), FOXAPI_OK);
        json c = row(r, "soapy-bladerf")["check"];
        std::printf("  bladerf: %s\n", c.dump().c_str());
        CHECK_EQ(c.value("state", std::string()), std::string("found"));
        CHECK_EQ(c.value("file", std::string()), std::string("bladeRFSupport.dll"));
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"soapy-plutosdr\"}").status(), FOXAPI_OK);
        c = row(r, "soapy-plutosdr")["check"];
        CHECK_EQ(c.value("state", std::string()), std::string("not-found"));
        CHECK(c.value("sentence", std::string()).find("pluto") != std::string::npos);
        withRoot = false;
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{\"id\":\"soapy-bladerf\"}").status(), FOXAPI_OK);
        CHECK_EQ(row(r, "soapy-bladerf")["check"].value("state", std::string()), std::string("not-found"));
    }

    std::printf("N5: REMOTE\n");
    {
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_NEEDED_LIB_CHECK, "{}", &remote).status(), FOXAPI_DENIED);
    }
    fs::remove_all(tmp, ec);
    return check::finish("test_engine_api_wireb_neededlibs");
}
