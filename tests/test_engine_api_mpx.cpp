// test_engine_api_mpx.cpp - THE FM MULTIPLEX TAKE THROUGH THE ENGINE (engine serial lane A, row D4): FOXAPP_OP_MPX_RECORD 0x8622.
//   A  in WFM a start writes a 32-bit float mono 192 kHz WAV named mpx_*, fact.radio.1.mpx reads 1, samples arrive, a stop finishes the file (header patched) and the fact reads 0;
//   B  refusals: not in WFM (UNSUPPORTED), a start twice and a stop with nothing running (NO_CHANGE), an absent radio;  C  a radio removed while it records ends the take (the file is closed).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "wire1_rig.hpp"

namespace fs = std::filesystem;

namespace {

FoxCommand mpx(std::uint32_t target, std::int64_t on) {
    FoxCommand c = testing::cmd(FOXAPP_OP_MPX_RECORD, 0.0, on, nullptr);
    c.target = target;
    return c;
}
FoxCommand mode(std::uint32_t target, std::int64_t m) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_MODE, 0.0, m, nullptr);
    c.target = target;
    return c;
}
std::vector<fs::path> wavs(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) { return out; }
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".wav") { out.push_back(e.path()); }
    }
    return out;
}
std::vector<unsigned char> slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::uint32_t u32(const std::vector<unsigned char>& v, std::size_t at) { return static_cast<std::uint32_t>(v[at]) | (static_cast<std::uint32_t>(v[at + 1]) << 8) | (static_cast<std::uint32_t>(v[at + 2]) << 16) | (static_cast<std::uint32_t>(v[at + 3]) << 24); }
std::uint16_t u16(const std::vector<unsigned char>& v, std::size_t at) { return static_cast<std::uint16_t>(v[at] | (v[at + 1] << 8)); }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_mpx\n");
    engine_api_test::hermetic("mpx");
    Access::installHooks();
    const fs::path rec = fs::path("mpxeng_" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(rec, ec);
    Rig r;
    Access::recordDir(r.eng, rec.string());
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_WFM)).status(), FOXAPI_OK);
    r.pump(20);

    std::printf("A: a take\n");
    CHECK_EQ(r.fact("radio.1.mpx"), std::string("0"));
    CHECK_EQ(r.send(mpx(1, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.mpx"), std::string("1"));
    CHECK(r.until([&] { return !wavs(rec).empty(); }, 5000));
    r.until([] { return false; }, 1500);
    CHECK_EQ(r.send(mpx(1, 1)).status(), FOXAPI_NO_CHANGE);
    CHECK_EQ(r.send(mpx(1, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("radio.1.mpx"), std::string("0"));
    CHECK_EQ(r.send(mpx(1, 0)).status(), FOXAPI_NO_CHANGE);
    {
        const std::vector<fs::path> w = wavs(rec);
        CHECK_EQ(w.size(), static_cast<std::size_t>(1));
        if (!w.empty()) {
            const std::vector<unsigned char> f = slurp(w[0]);
            std::printf("  %s: %zu bytes\n", w[0].filename().string().substr(0, 3).c_str(), f.size());
            CHECK(w[0].filename().string().rfind("mpx_", 0) == 0);
            CHECK(f.size() > 44 + 4 * 48000);
            if (f.size() > 44) {
                CHECK(u16(f, 20) == 3 && u16(f, 22) == 1 && u16(f, 34) == 32 && u32(f, 24) == 192000u);
                CHECK_EQ(u32(f, 40), static_cast<std::uint32_t>(f.size() - 44));   // the header was patched
            }
        }
    }

    std::printf("B: refusals\n");
    CHECK_EQ(r.send(mode(1, FOXAPI_DEMOD_AM)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(mpx(1, 1)).status(), FOXAPI_UNSUPPORTED);
    CHECK_EQ(r.fact("radio.1.mpx"), std::string("0"));
    CHECK_EQ(r.send(mpx(9, 1)).status(), FOXAPI_NO_DEVICE);

    std::printf("C: a radio removed while it records\n");
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    CHECK_EQ(r.send(mode(2, FOXAPI_DEMOD_WFM)).status(), FOXAPI_OK);
    r.until([] { return false; }, 800);
    CHECK_EQ(r.send(mpx(2, 1)).status(), FOXAPI_OK);
    r.until([] { return false; }, 800);
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 1, 0.0, 2)).status(), FOXAPI_OK);
    r.until([] { return false; }, 500);
    CHECK_EQ(r.fact("radio.count"), std::string("1"));
    for (const fs::path& p : wavs(rec)) {
        const std::vector<unsigned char> f = slurp(p);
        CHECK(f.size() >= 44);
        if (f.size() >= 44) { CHECK_EQ(u32(f, 40), static_cast<std::uint32_t>(f.size() - 44)); }   // every file is closed with its header patched
    }
    fs::remove_all(rec, ec);
    return check::finish("test_engine_api_mpx");
}
