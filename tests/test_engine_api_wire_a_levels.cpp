// test_engine_api_wire_a_levels.cpp - LEVEL_STORE 0x8831 THROUGH THE ENGINE (wire-a second pass, item 7): a level store for every open radio.
//   A  off by default: no rows, nothing allocated;  B  on: TWO radios are open and only radio 1 is the selected one, and both fill rows at one every 250 ms, each in its own ring of the size the cap and the bin count give (the bytes
//   are reported);  C  a retune of one radio starts a new segment in its ring and not in the other's;  D  the rows read back through WireA::levelCopy are quantised dB bytes with the time, centre and span of each row;
//   E  clear and off free the rings; the settings and every refusal.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "wire1_rig.hpp"
#include "engine/wire_a.hpp"

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint64_t kFrames = 240000;

void put32(std::vector<unsigned char>& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu)); } }
void put16(std::vector<unsigned char>& v, std::uint16_t x) { for (int i = 0; i < 2; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu)); } }

std::string writeWav(const std::string& name) {
    std::vector<unsigned char> b;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(kFrames * 8);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put32(b, 36 + dataBytes);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(b, 16);
    put16(b, 3);
    put16(b, 2);
    put32(b, kRate);
    put32(b, kRate * 8);
    put16(b, 8);
    put16(b, 32);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, dataBytes);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        const double ph = 6.283185307179586 * 5000.0 * static_cast<double>(i) / kRate;
        const float re = static_cast<float>(0.3 * std::cos(ph)), im = static_cast<float>(0.3 * std::sin(ph));
        unsigned char t[4];
        std::memcpy(t, &re, 4);
        b.insert(b.end(), t, t + 4);
        std::memcpy(t, &im, 4);
        b.insert(b.end(), t, t + 4);
    }
    std::ofstream f(name, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return name;
}

FoxCommand lv(std::int64_t action, double mb = 0.0, std::uint32_t target = 0) {
    FoxCommand c = testing::cmd(FOXAPP_OP_LEVEL_STORE, mb, action, nullptr);
    c.target = target;
    return c;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wire_a_levels\n");
    engine_api_test::hermetic("wire_a_levels");
    Access::installHooks();
    const std::string path = writeWav("wa_levels_20261109_134502_" + std::to_string(TEST_GETPID()) + ".wav");
    Rig r;
    CHECK_EQ(r.add(2, "file:" + path).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).fileSource == 1u; }, 30000));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);   // radio 1 (the selected one) runs on the generator
    cascade::engine::WireA& wa = cascade::engine::WireA::of(r.eng);

    std::printf("A: off by default\n");
    {
        CHECK_EQ(r.fact("levels.on"), std::string("0"));
        CHECK_EQ(r.fact("levels.capMb"), std::string("16"));
        r.pump(40);
        std::vector<std::uint8_t> out;
        std::size_t bins = 0;
        CHECK_EQ(wa.levelCopy(0, 10, out, bins), static_cast<std::size_t>(0));
        CHECK_EQ(r.send(lv(0)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.send(lv(2)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.send(lv(3, 0.0, 1)).status(), FOXAPI_NO_CHANGE);   // no ring yet
    }

    std::printf("B: two radios, one ring each\n");
    {
        CHECK_EQ(r.send(lv(1, 99.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(lv(1, -1.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(lv(1, 0.0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(lv(1, 0.0)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.fact("levels.on"), std::string("1"));
        std::vector<std::uint8_t> a, b;
        std::size_t binsA = 0, binsB = 0;
        CHECK(r.until([&] { r.pump(5); return wa.levelCopy(0, 1000, a, binsA) >= 8 && wa.levelCopy(1, 1000, b, binsB) >= 8; }, 30000));
        std::printf("  rows now: radio 1 %zu, radio 2 %zu\n", wa.levelCopy(0, 1000, a, binsA), wa.levelCopy(1, 1000, b, binsB));
        // read the statistics of each radio
        const FoxCommandResult s1 = r.send(lv(3, 0.0, 1)).result;
        const FoxCommandResult s2 = r.send(lv(3, 0.0, 2)).result;
        const std::size_t rows1 = static_cast<std::size_t>(std::stoul(r.fact("levels.1.rows"))), rows2 = static_cast<std::size_t>(std::stoul(r.fact("levels.2.rows")));
        const std::size_t bins1 = static_cast<std::size_t>(std::stoul(r.fact("levels.1.bins")));
        std::printf("  radio 1: %zu rows of %zu bins, ring %.0f bytes; radio 2: %zu rows, ring %.0f bytes\n", rows1, bins1, s1.applied[0], rows2, s2.applied[0]);
        CHECK(rows1 >= 8 && rows2 >= 8);
        CHECK(bins1 > 0 && bins1 <= 1024);
        // the ring is what the cap and the bin count make it: min(14400 rows, 16 MiB / (bins + 24)) rows of (bins + 24) bytes
        const std::size_t capRows = std::min<std::size_t>(14400, (16u << 20) / (bins1 + 24));
        CHECK_EQ(static_cast<std::size_t>(s1.applied[0]), capRows * (bins1 + 24));
        std::printf("  an hour of %zu-bin rows is %zu bytes (%.1f MB) a radio\n", bins1, capRows * (bins1 + 24), static_cast<double>(capRows * (bins1 + 24)) / 1.0e6);
        CHECK_EQ(r.fact("levels.1.segments"), std::string("1"));
        CHECK_EQ(r.fact("levels.2.segments"), std::string("1"));
        // one row every 250 ms: the rows span about the time that passed, not the frame rate
        CHECK(rows1 < 200);
    }

    std::printf("C: a retune starts a segment in its own ring\n");
    {
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 101.0e6, 0)).status(), FOXAPI_OK);   // the selected radio (1)
        r.pump(40);
        r.until([] { return false; }, 900);
        CHECK_EQ(r.send(lv(3, 0.0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(lv(3, 0.0, 2)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("levels.1.segments"), std::string("2"));
        CHECK_EQ(r.fact("levels.2.segments"), std::string("1"));
    }

    std::printf("D: the rows read back\n");
    {
        std::vector<std::uint8_t> out;
        std::vector<cascade::core::wfhistory::LineMeta> meta;
        std::size_t bins = 0;
        const std::size_t got = wa.levelCopy(0, 1000, out, bins, &meta);
        CHECK(got >= 8 && meta.size() == got && out.size() == got * bins);
        bool ordered = true, centres = false;
        for (std::size_t i = 1; i < meta.size(); ++i) { ordered = ordered && meta[i].timeMs >= meta[i - 1].timeMs; }
        for (const auto& m : meta) { centres = centres || std::fabs(m.centerMhz - 101.0f) < 1e-3f; }
        CHECK(ordered);
        CHECK(centres);   // the rows after the retune carry the new centre
        // the rows are about 250 ms apart
        if (got >= 3) {
            const double span = static_cast<double>(meta.back().timeMs - meta.front().timeMs) / static_cast<double>(got - 1);
            std::printf("  %zu rows, %.0f ms apart on average\n", got, span);
            CHECK(span > 200.0 && span < 600.0);
        }
        bool energy = false;
        for (std::uint8_t v : out) { energy = energy || v > 0; }
        CHECK(energy);
        std::vector<std::uint8_t> none;
        std::size_t nb = 0;
        CHECK_EQ(wa.levelCopy(7, 10, none, nb), static_cast<std::size_t>(0));   // a radio with no ring
        CHECK_EQ(wa.levelCopy(99, 10, none, nb), static_cast<std::size_t>(0));
    }

    std::printf("E: clear, off, the settings\n");
    {
        CHECK_EQ(r.send(lv(2)).status(), FOXAPI_OK);
        std::vector<std::uint8_t> out;
        std::size_t bins = 0;
        CHECK(wa.levelCopy(0, 1000, out, bins) <= 2);   // emptied (a frame may already have started it again)
        CHECK(r.until([&] { r.pump(5); return wa.levelCopy(0, 10, out, bins) >= 1; }, 15000));   // it fills again: still on
        CHECK_EQ(r.send(lv(1, 4.0)).status(), FOXAPI_OK);   // a smaller cap: rings are made again at 4 MiB
        CHECK_EQ(r.fact("levels.capMb"), std::string("4"));
        CHECK(r.until([&] { r.pump(5); return wa.levelCopy(0, 10, out, bins) >= 1; }, 15000));
        CHECK_EQ(r.send(lv(3, 0.0, 1)).status(), FOXAPI_OK);
        CHECK(std::stoul(r.fact("levels.1.bytes")) <= (4u << 20));
        CHECK_EQ(r.send(lv(0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("levels.on"), std::string("0"));
        CHECK_EQ(wa.levelCopy(0, 10, out, bins), static_cast<std::size_t>(0));
        r.pump(40);
        CHECK_EQ(wa.levelCopy(0, 10, out, bins), static_cast<std::size_t>(0));   // nothing is stored while it is off
        CHECK_EQ(r.send(lv(9)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(lv(3, 0.0, 9)).status(), FOXAPI_NO_DEVICE);
    }
    r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 2));
    std::remove(path.c_str());
    return check::finish("test_engine_api_wire_a_levels");
}
