// test_engine_api_wire_a_transport.cpp - FILE_TRANSPORT 0x8826 THROUGH THE ENGINE (wire-a second pass, item 1): the speed, pause and step of a radio that plays an I/Q file.
//   A  a file radio reports speed 1, varispeed, playing; a generator radio is UNSUPPORTED;  B  pause stops the position, resume carries on from the same place, the facts follow;  C  a step moves the position by
//   exactly n blocks of 4096 frames (twice as far at 2x), a step back replays, a step while playing is DENIED;  D  speed and policy, with NO_CHANGE for the same value;  E  every refusal changes nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "wire1_rig.hpp"

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint64_t kFrames = 960000;   // twenty seconds

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

FoxCommand tr(std::uint32_t target, std::int64_t action, double speed = 0.0, std::int64_t second = 0) {
    FoxCommand c = testing::cmd(FOXAPP_OP_FILE_TRANSPORT, speed, action, nullptr);
    c.target = target;
    c.ival[1] = second;
    return c;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wire_a_transport\n");
    engine_api_test::hermetic("wire_a_transport");
    Access::installHooks();
    const std::string path = writeWav("wa_transport_20261109_134502_" + std::to_string(TEST_GETPID()) + ".wav");
    Rig r;
    CHECK_EQ(r.add(2, "file:" + path).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).fileSource == 1u; }, 30000));
    auto posFrames = [&] { return r.radio(2).filePositionSec * kRate; };
    auto settled = [&] {   // the position has not moved for a third of a second of passes
        const double a = posFrames();
        r.pump(20);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        r.pump(20);
        return std::fabs(posFrames() - a) < 0.5;
    };

    std::printf("A: the state\n");
    {
        r.pump(10);
        CHECK_EQ(r.fact("playback.2.speed"), std::string("1"));
        CHECK_EQ(r.fact("playback.2.policy"), std::string("varispeed"));
        CHECK_EQ(r.fact("playback.2.paused"), std::string("0"));
        CHECK_EQ(r.fact("playback.1.speed"), std::string("<absent>"));   // the generator radio has none
        CHECK_EQ(r.send(tr(1, 2)).status(), FOXAPI_UNSUPPORTED);
        CHECK_EQ(r.send(tr(1, 0, 2.0)).status(), FOXAPI_UNSUPPORTED);
        CHECK_EQ(r.send(tr(9, 2)).status(), FOXAPI_NO_DEVICE);
    }

    std::printf("B: pause and resume\n");
    {
        CHECK(r.until([&] { return posFrames() > 2000; }, 10000));
        CHECK_EQ(r.send(tr(2, 3)).status(), FOXAPI_NO_CHANGE);   // playing already
        CHECK_EQ(r.send(tr(2, 2)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(tr(2, 2)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.fact("playback.2.paused"), std::string("1"));
        CHECK(r.until([&] { return settled(); }, 15000));
        const double held = posFrames();
        r.pump(40);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        r.pump(40);
        std::printf("  held at frame %.0f, now %.0f\n", held, posFrames());
        CHECK(std::fabs(posFrames() - held) < 0.5);
        CHECK_EQ(r.send(tr(2, 3)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("playback.2.paused"), std::string("0"));
        CHECK(r.until([&] { return posFrames() > held + 4000; }, 10000));
    }

    std::printf("C: step\n");
    {
        CHECK_EQ(r.send(tr(2, 4, 0.0, 1)).status(), FOXAPI_DENIED);   // playing
        CHECK_EQ(r.send(tr(2, 2)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return settled(); }, 15000));
        const double p0 = posFrames();
        CHECK_EQ(r.send(tr(2, 4, 0.0, 2)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return posFrames() >= p0 + 8192 - 0.5; }, 10000));
        CHECK(r.until([&] { return settled(); }, 15000));
        std::printf("  two blocks: %.0f -> %.0f\n", p0, posFrames());
        CHECK(std::fabs(posFrames() - (p0 + 8192)) < 0.5);
        // a step back of 1 block replays one block from 1 block earlier
        const double p1 = posFrames();
        CHECK_EQ(r.send(tr(2, 4, 0.0, -1)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return settled(); }, 15000));
        std::printf("  one back: %.0f -> %.0f\n", p1, posFrames());
        CHECK(std::fabs(posFrames() - p1) < 0.5);   // back one block and played one block: the same place
        CHECK_EQ(r.send(tr(2, 4, 0.0, -2)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return settled(); }, 15000));
        CHECK(std::fabs(posFrames() - (p1 - 4096)) < 0.5);
        // at 2x a block is twice as far
        CHECK_EQ(r.send(tr(2, 0, 2.0)).status(), FOXAPI_OK);
        const double p2 = posFrames();
        CHECK_EQ(r.send(tr(2, 4, 0.0, 1)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return settled(); }, 15000));
        std::printf("  one block at 2x: %.0f -> %.0f\n", p2, posFrames());
        CHECK(std::fabs(posFrames() - (p2 + 8192)) < 6.0);   // (the interpolator holds a frame or two read ahead)
        CHECK_EQ(r.send(tr(2, 0, 1.0)).status(), FOXAPI_OK);
    }

    std::printf("D: speed and policy while playing\n");
    {
        CHECK_EQ(r.send(tr(2, 3)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(tr(2, 0, 1.0)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.send(tr(2, 0, 4.0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("playback.2.speed"), std::string("4"));
        CHECK_EQ(r.send(tr(2, 0, 0.25)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("playback.2.speed"), std::string("0.25"));
        CHECK_EQ(r.send(tr(2, 1, 0.0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("playback.2.policy"), std::string("blockskip"));
        CHECK_EQ(r.send(tr(2, 1, 0.0, 1)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.send(tr(2, 1, 0.0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("playback.2.policy"), std::string("varispeed"));
        // the file still plays at the slow speed: the position moves, and slower than real time
        const double a = posFrames();
        CHECK(r.until([&] { return posFrames() > a + 100; }, 15000));
        CHECK_EQ(r.send(tr(2, 0, 1.0)).status(), FOXAPI_OK);
    }

    std::printf("E: refusals change nothing\n");
    {
        CHECK_EQ(r.send(tr(2, 0, 0.1)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(tr(2, 0, 9.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(tr(2, 0, std::nan(""))).status(), FOXAPI_BAD_ARGUMENT);   // (the table refuses a NaN number before the engine sees it)
        CHECK_EQ(r.send(tr(2, 1, 0.0, 2)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(tr(2, 9)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(tr(2, 2)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(tr(2, 4, 0.0, 0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(tr(2, 4, 0.0, 4097)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(tr(2, 4, 0.0, -4097)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.fact("playback.2.speed"), std::string("1"));
        CHECK_EQ(r.fact("playback.2.paused"), std::string("1"));
        CHECK(r.until([&] { return settled(); }, 15000));
        CHECK_EQ(r.send(tr(2, 3)).status(), FOXAPI_OK);
    }
    r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 2));
    std::remove(path.c_str());
    return check::finish("test_engine_api_wire_a_transport");
}
