// test_engine_api_wire_c.cpp - the settings op of wiring lane C (FOXAPP_OP_WIREC_SETTING 0x8A00, src/engine/engine_wire_c.cpp, wire_c.{hpp,cpp}), held against the
// real engine through the C table:
//   W1  every key of the registry reads back its default as fact.wirec.<key>; reading creates no file
//   W2  a change is OK, saved in wirec.json, published, and survives a new engine on the same folder; the same value again is NO_CHANGE
//   W3  every refusal: no separator, an unknown key, a switch that is not 1/0, a number out of range, a choice that is not offered, a long text, a control character
//   W4  setting a key back to its default removes its entry (and the file when it is empty); a damaged file means defaults and a sentence in fact.wirec.error
//   W5  a REMOTE session cannot send the op
//   W6  config.json is not touched (no file of that name appears in the folder)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "wire_c_rig.hpp"

#include "engine/wire_c.hpp"

using namespace wirec_test;

namespace {

void testDefaults(const fs::path& dir) {
    Rig r(dir);
    std::size_t n = 0;
    const cascade::engine::wirec::Spec* specs = cascade::engine::wirec::specs(n);
    CHECK(n > 0);
    for (std::size_t i = 0; i < n; ++i) { CHECK(r.fact(std::string("wirec.") + specs[i].key) == specs[i].def); }
    CHECK(r.fact("wirec.error").empty());
    CHECK(!fs::exists(dir / "wirec.json"));
    CHECK(!fs::exists(dir / "config.json"));
}

void testChangeAndKeep(const fs::path& dir) {
    {
        Rig r(dir);
        CHECK(r.status(setting("access.units", "metric")) == FOXAPI_OK);
        CHECK(r.fact("wirec.access.units") == "metric");
        CHECK(r.status(setting("access.units", "metric")) == FOXAPI_NO_CHANGE);
        CHECK(r.status(setting("sys.backup_keep", "25")) == FOXAPI_OK);
        CHECK(r.status(setting("web.cors_origins", "https://a.example\nhttps://b.example")) == FOXAPI_OK);
        CHECK(fs::exists(dir / "wirec.json"));
        const std::string file = readAll(dir / "wirec.json");
        CHECK(file.find("metric") != std::string::npos);
        CHECK(file.find("sys.resume_reopen") == std::string::npos);   // only what differs from a default is kept
        CHECK(!fs::exists(dir / "config.json"));
    }
    {
        Rig again(dir);   // a new engine, the same folder
        CHECK(again.fact("wirec.access.units") == "metric");
        CHECK(again.fact("wirec.sys.backup_keep") == "25");
        CHECK(again.fact("wirec.web.cors_origins") == "https://a.example\nhttps://b.example");
        // back to the defaults: entries go, and the file when nothing is left
        CHECK(again.status(setting("access.units", "automatic")) == FOXAPI_OK);
        CHECK(again.status(setting("sys.backup_keep", "10")) == FOXAPI_OK);
        CHECK(again.status(setting("web.cors_origins", "")) == FOXAPI_OK);
        CHECK(!fs::exists(dir / "wirec.json"));
    }
}

void testRefusals(const fs::path& dir) {
    Rig r(dir);
    CHECK(r.status(textOp(FOXAPP_OP_WIREC_SETTING, "nokey")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textOp(FOXAPP_OP_WIREC_SETTING, std::string("\x1F") + "1")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(setting("no.such.key", "1")) == FOXAPI_NOT_FOUND);
    CHECK(r.status(setting("sys.resume_reopen", "maybe")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(setting("sys.backup_keep", "0")) == FOXAPI_OUT_OF_RANGE);
    CHECK(r.status(setting("sys.backup_keep", "51")) == FOXAPI_OUT_OF_RANGE);
    CHECK(r.status(setting("sys.backup_keep", "2.5")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(setting("sys.backup_keep", "abc")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(setting("sys.backup_keep", "nan")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(setting("access.units", "furlongs")) == FOXAPI_OUT_OF_RANGE);
    CHECK(r.status(setting("access.units", "metric|imperial")) == FOXAPI_OUT_OF_RANGE);
    {   // (a command's text holds 256 bytes, so the length rule is held on the registry directly)
        std::string out, why;
        cascade::engine::wirec::SetResult bad = cascade::engine::wirec::SetResult::Changed;
        const cascade::engine::wirec::Spec* s = cascade::engine::wirec::findSpec("sys.sync_folder");
        CHECK(s != nullptr);
        if (s != nullptr) {
            CHECK(!cascade::engine::wirec::normalise(*s, std::string(261, 'x'), out, bad, why));
            CHECK(bad == cascade::engine::wirec::SetResult::BadValue);
            CHECK(cascade::engine::wirec::normalise(*s, std::string(260, 'x'), out, bad, why));
        }
    }
    CHECK(r.status(setting("sys.sync_folder", std::string("a") + '\x01' + "b")) == FOXAPI_BAD_ARGUMENT);
    // nothing of the above changed anything
    CHECK(r.fact("wirec.sys.backup_keep") == "10");
    CHECK(r.fact("wirec.access.units") == "automatic");
    CHECK(!fs::exists(dir / "wirec.json"));
}

void testDamagedFile(const fs::path& dir) {
    {
        std::ofstream f(dir / "wirec.json", std::ios::binary);
        f << "{ this is not json";
    }
    {
        Rig r(dir);
        CHECK(r.fact("wirec.access.units") == "automatic");
        CHECK(!r.fact("wirec.error").empty());
        CHECK(r.status(setting("access.units", "imperial")) == FOXAPI_OK);   // a good value replaces the damaged file
        CHECK(r.fact("wirec.access.units") == "imperial");
    }
    {
        std::ofstream f(dir / "wirec.json", std::ios::binary | std::ios::trunc);
        f << "{\"future.key\":\"x\",\"sys.backup_keep\":\"9999\",\"access.units\":\"nautical\"}";
    }
    Rig r2(dir);   // an unknown key is ignored, a bad value for a known key is its default
    CHECK(r2.fact("wirec.sys.backup_keep") == "10");
    CHECK(r2.fact("wirec.access.units") == "nautical");
}

void testRemote(const fs::path& dir) {
    Rig r(dir);
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    const testing::Outcome o = r.sendOn(remote, setting("access.units", "metric"));
    CHECK_EQ(o.status(), FOXAPI_DENIED);
    CHECK(r.fact("wirec.access.units") == "automatic");
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("settings");
    int n = 0;
    for (auto fn : {testDefaults, testChangeAndKeep, testRefusals, testDamagedFile, testRemote}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_wire_c");
}
