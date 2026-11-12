// test_engine_api_recformat.cpp - THE SAMPLE FORMAT OF A RECORDING THROUGH THE ENGINE (engine serial lane A, row P7): SET_RECORD_FORMAT 0x0121, per radio: baseband I/Q 0 float32 / 1 int16, audio 0 int16 / 1 int24 / 2 float32.
//   A  default, each radio its own, refusals change nothing;  B  real takes on the generator: the header of each file says what was asked (bits, channels, PCM or float) and the data fits it;
//   C  saved per radio and restored in an in-process restart, junk in a file lands on the default.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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

FoxCommand fmt(std::uint32_t target, std::int64_t iq, std::int64_t audio) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_RECORD_FORMAT, 0.0, iq, nullptr);
    c.target = target;
    c.ival[1] = audio;
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
std::uint16_t u16(const std::vector<unsigned char>& v, std::size_t at) { return static_cast<std::uint16_t>(v[at] | (v[at + 1] << 8)); }

void testA() {
    std::printf("A: default, per radio, refusals\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    CHECK_EQ(r.radio(1).recordIqFormat, 0u);
    CHECK_EQ(r.radio(1).recordAudioFormat, 0u);
    CHECK_EQ(r.send(fmt(2, 1, 2)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).recordIqFormat == 1u && r.radio(2).recordAudioFormat == 2u; }, 5000));
    CHECK_EQ(r.radio(1).recordIqFormat, 0u);
    for (const std::int64_t bad : {-1, 2, 9}) { CHECK_EQ(r.send(fmt(1, bad, 0)).status(), FOXAPI_OUT_OF_RANGE); }
    for (const std::int64_t bad : {-1, 3, 9}) { CHECK_EQ(r.send(fmt(1, 0, bad)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(fmt(9, 0, 0)).status(), FOXAPI_NO_DEVICE);
    const FoxCommandResult res = r.eng.applyCommand(fmt(1, 5, 5), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).recordIqFormat, 0u);
}

// One take on radio 1 (the generator) of the kind asked, with the format set before it; the file's header is read back.
std::vector<unsigned char> oneTake(Rig& r, const fs::path& rec, bool iq, std::int64_t iqFmt, std::int64_t audioFmt) {
    std::error_code ec;
    fs::remove_all(rec, ec);
    CHECK_EQ(r.send(fmt(1, iqFmt, audioFmt)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(iq ? FOXAPI_OP_RECORD_IQ : FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return !wavs(rec).empty(); }, 15000));
    r.until([] { return false; }, 600);   // a little data
    CHECK_EQ(r.send(testing::cmd(iq ? FOXAPI_OP_RECORD_IQ : FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
    r.until([] { return false; }, 900);   // let the finish settle (the next start waits for it)
    const std::vector<fs::path> w = wavs(rec);
    if (w.empty()) { return {}; }
    return slurp(w[0]);
}

void testB() {
    std::printf("B: real takes carry the format asked\n");
    const fs::path rec = fs::path("recfmt_" + std::to_string(TEST_GETPID()));
    Rig r;
    Access::recordDir(r.eng, rec.string());
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    r.pump(20);
    struct Want { bool iq; std::int64_t iqFmt, audioFmt; std::uint16_t tag, channels, bits; };
    const Want cases[] = {
        {true, 0, 0, 3, 2, 32}, {true, 1, 0, 1, 2, 16},
        {false, 0, 0, 1, 1, 16}, {false, 0, 1, 1, 1, 24}, {false, 0, 2, 3, 1, 32},
    };
    for (const Want& w : cases) {
        const std::vector<unsigned char> f = oneTake(r, rec, w.iq, w.iqFmt, w.audioFmt);
        CHECK(f.size() >= 44);
        if (f.size() < 44) { continue; }
        std::printf("  %s iq=%lld audio=%lld: tag %u, %u channel(s), %u bits, %zu bytes\n", w.iq ? "I/Q  " : "audio", static_cast<long long>(w.iqFmt), static_cast<long long>(w.audioFmt), u16(f, 20), u16(f, 22), u16(f, 34), f.size());
        CHECK(u16(f, 20) == w.tag && u16(f, 22) == w.channels && u16(f, 34) == w.bits);
        CHECK(f.size() > 44 && (f.size() - 44) % (w.channels * w.bits / 8u) == 0);   // whole frames of that size
    }
    std::error_code ec;
    fs::remove_all(rec, ec);
}

void testC() {
    std::printf("C: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(fmt(1, 1, 1)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(fmt(2, 0, 2)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.recordIqFormat, 1);
        CHECK_EQ(file.recordAudioFormat, 1);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].recordAudioFormat, 2);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).recordIqFormat, 1u);
        CHECK_EQ(b.radio(2).recordAudioFormat, 2u);
    }
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"recordIqFormat\":7,\"recordAudioFormat\":-2}", "test", back, error));
    CHECK_EQ(back.recordIqFormat, 0);
    CHECK_EQ(back.recordAudioFormat, 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_recformat\n");
    engine_api_test::hermetic("recformat");
    Access::installHooks();
    testA();
    testB();
    testC();
    return check::finish("test_engine_api_recformat");
}
