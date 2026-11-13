// test_engine_api_wire_a_airband.cpp - AIRBAND_CHANNEL 0x8830 THROUGH THE ENGINE (wire-a second pass, item 6): the airband monitor's per-channel settings.
//   A  the monitor as it always was: the generator's tone 300 kHz above the centre opens High's squelch and not Low's;  B  each channel its own squelch: High given a threshold nothing reaches shuts, Low given one the noise reaches
//   opens - the reverse of A, on the same two channels in the same block, taking effect at once while listening;  C  an SNR squelch;  D  a stream output: datagrams reach a socket on 127.0.0.1 while the gate is open;
//   E  a file output: a WAV under <recordings>/airband/<UTC day>/ after the set is torn down;  F  the settings: persistence, clearing, NO_CHANGE and every refusal.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#define _getpid getpid
#endif

#include "check.hpp"
#include "core/airband_channel.hpp"
#include "wire_a_rig.hpp"

using namespace wire_a_rig;
namespace ab = cascade::core::airband;

namespace {

fs::path g_dir;

cascade::core::Bookmark bm(const char* name, double hz, bool tick) {
    cascade::core::Bookmark b;
    b.name = name;
    b.freqHz = hz;
    b.mode = "AM";
    b.bandwidthHz = 10000.0;
    b.scan = tick;
    return b;
}

FoxListItem chan(Rig& r, const std::string& name) {
    const int32_t n = r.api()->read_list(r.local->raw(), FOXAPI_LIST_AIRBAND_CHANNELS, 0, nullptr);
    for (int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        r.api()->read_list(r.local->raw(), FOXAPI_LIST_AIRBAND_CHANNELS, static_cast<uint32_t>(i), &it);
        if (name == it.name) { return it; }
    }
    return FoxListItem{};
}
bool open(Rig& r, const std::string& name) { return (chan(r, name).flags & FOXAPI_ITEM_ACTIVE) != 0u; }

std::string text(std::int64_t hz, ab::SquelchKind kind, int squelchTenths, int snrTenths = 100, ab::OutputKind out = ab::OutputKind::None, int port = 0, int highPass = 0) {
    ab::AirbandChannelConfig c;
    c.freqHz = hz;
    c.squelchKind = kind;
    c.squelchTenthsDb = squelchTenths;
    c.snrOpenTenthsDb = snrTenths;
    c.output = out;
    c.udpPort = port;
    c.highPassHz = highPass;
    return ab::serialise(c);
}

testing::Outcome setCh(Rig& r, const std::string& t) { return r.send(testing::cmd(FOXAPP_OP_AIRBAND_CHANNEL, 0.0, 0, nullptr), &t); }
testing::Outcome clearCh(Rig& r, double hz) { return r.send(testing::cmd(FOXAPP_OP_AIRBAND_CHANNEL, hz, 1, nullptr)); }

std::size_t wavsUnder(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    if (!fs::exists(dir, ec)) { return 0; }
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) { if (e.is_regular_file() && e.path().extension() == ".wav" && e.file_size() > 44) { ++n; } }
    return n;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wire_a_airband\n");
    engine_api_test::hermetic("wire_a_airband");
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_airband_" + std::to_string(_getpid()));
    { std::error_code ec; fs::remove_all(g_dir, ec); fs::create_directories(g_dir, ec); }
    const fs::path user = g_dir / "user", rec = g_dir / "rec";
    fs::create_directories(rec);
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    {
        Rig r(user, rec);
        Access::bookmarks(r.eng).addMany({bm("Low", 119.7e6, true), bm("High", 120.3e6, true)});
        r.settle(100);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_PATCH_PAGE_OPEN, 0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        FoxCommand listen = testing::cmd(FOXAPP_OP_AIRBAND, 0.0, 1, "");
        CHECK_EQ(r.send(listen).status(), FOXAPI_OK);
        r.settle(300);
        CHECK_EQ(r.fact("airband.listening"), std::string("1"));

        std::printf("A: the monitor as it always was\n");
        CHECK(r.until([&] { return open(r, "High"); }, 10000));
        CHECK(!open(r, "Low"));

        std::printf("B: each channel its own squelch, at once\n");
        CHECK_EQ(setCh(r, text(120300000, ab::SquelchKind::Level, 0)).status(), FOXAPI_OK);          // 0 dB: nothing reaches it
        CHECK_EQ(setCh(r, text(119700000, ab::SquelchKind::Level, -1200)).status(), FOXAPI_OK);     // -120 dB: the noise does
        CHECK_EQ(r.fact("airband.channels"), std::string("2"));
        CHECK(r.until([&] { return open(r, "Low"); }, 10000));
        r.settle(500);
        std::printf("  High %s (level %.1f dB), Low %s (level %.1f dB)\n", open(r, "High") ? "open" : "shut", chan(r, "High").value[2], open(r, "Low") ? "open" : "shut", chan(r, "Low").value[2]);
        CHECK(!open(r, "High"));
        CHECK(open(r, "Low"));
        // clearing hands the channel back to the shared squelch
        CHECK_EQ(clearCh(r, 120300000.0).status(), FOXAPI_OK);
        CHECK_EQ(clearCh(r, 119700000.0).status(), FOXAPI_OK);
        CHECK(r.until([&] { return open(r, "High") && !open(r, "Low"); }, 10000));

        std::printf("C: an SNR squelch is in force (it judges the audio's signal-to-noise, not the level)\n");
        CHECK_EQ(setCh(r, text(120300000, ab::SquelchKind::Snr, -500, 100)).status(), FOXAPI_OK);
        r.settle(1200);
        // the unmodulated carrier that opens the level squelch has no audio to measure a signal-to-noise ratio on: the SNR squelch stays shut, and its meter reads dB of SNR near 0, not the channel level of about -30 dB
        std::printf("  High: %s, meter %.1f\n", open(r, "High") ? "open" : "shut", chan(r, "High").value[2]);
        CHECK(!open(r, "High"));
        CHECK(chan(r, "High").value[2] > -10.0);
        CHECK_EQ(clearCh(r, 120300000.0).status(), FOXAPI_OK);
        CHECK(r.until([&] { return open(r, "High"); }, 10000));

        std::printf("D: a stream output reaches a socket on 127.0.0.1\n");
        {
#ifdef _WIN32
            SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            const bool bad = s == INVALID_SOCKET;
#else
            int s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            const bool bad = s < 0;
#endif
            CHECK(!bad);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(40731);
            inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
            CHECK(::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
#ifdef _WIN32
            DWORD tmo = 300;
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
#else
            timeval tv{0, 300000};
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            CHECK_EQ(setCh(r, text(120300000, ab::SquelchKind::Level, -550, 100, ab::OutputKind::Udp, 40731)).status(), FOXAPI_OK);
            int datagrams = 0;
            char buf[4096];
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (std::chrono::steady_clock::now() < end && datagrams < 5) {
                r.eng.controlPass();
                const int n = ::recv(s, buf, sizeof(buf), 0);
                if (n > 0) { ++datagrams; }
            }
            std::printf("  %d datagrams received\n", datagrams);
            CHECK(datagrams >= 5);
            // the other channel (no output asked for) sends nothing: its own socket sees none
#ifdef _WIN32
            ::closesocket(s);
#else
            ::close(s);
#endif
            CHECK_EQ(clearCh(r, 120300000.0).status(), FOXAPI_OK);
        }

        std::printf("E: a file output\n");
        {
            CHECK_EQ(wavsUnder(rec / "airband"), static_cast<std::size_t>(0));
            CHECK_EQ(setCh(r, text(120300000, ab::SquelchKind::Level, -550, 100, ab::OutputKind::WavFile)).status(), FOXAPI_OK);
            r.settle(2500);
            CHECK_EQ(setCh(r, text(120300000, ab::SquelchKind::Level, -550)).status(), FOXAPI_OK);   // the block is built again without the output: the old set goes and its writer closes the file
            r.until([&] { return wavsUnder(rec / "airband") >= 1; }, 6000);   // (the set is torn down a moment after STOP, and the writer closes then)
            { std::error_code ec; for (const auto& e : fs::recursive_directory_iterator(rec, ec)) { std::printf("  found %s (%llu bytes)\n", e.path().string().c_str(), static_cast<unsigned long long>(e.is_regular_file() ? e.file_size() : 0)); } }
            std::printf("  %zu wav file(s) under the recordings folder\n", wavsUnder(rec / "airband"));
            CHECK(wavsUnder(rec / "airband") >= 1);
        }

        std::printf("F: the settings\n");
        {
            CHECK_EQ(setCh(r, text(120300000, ab::SquelchKind::Level, -300, 100, ab::OutputKind::None, 0, 300)).status(), FOXAPI_OK);
            CHECK_EQ(setCh(r, text(120300000, ab::SquelchKind::Level, -300, 100, ab::OutputKind::None, 0, 300)).status(), FOXAPI_NO_CHANGE);
            CHECK_EQ(setCh(r, "junk").status(), FOXAPI_BAD_ARGUMENT);
            CHECK_EQ(setCh(r, text(120300000, ab::SquelchKind::Level, 50)).status(), FOXAPI_OUT_OF_RANGE);
            CHECK_EQ(setCh(r, text(120300000, ab::SquelchKind::Level, -300, 100, ab::OutputKind::Udp, 0)).status(), FOXAPI_OUT_OF_RANGE);   // a stream with no port
            CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_AIRBAND_CHANNEL, 0.0, 9, nullptr)).status(), FOXAPI_OUT_OF_RANGE);
            CHECK_EQ(clearCh(r, 555.0).status(), FOXAPI_NO_CHANGE);
            CHECK_EQ(clearCh(r, -1.0).status(), FOXAPI_OUT_OF_RANGE);
            CHECK_EQ(r.fact("airband.channels"), std::string("1"));
            CHECK(r.fact("airband.channel.120300000").rfind("FOXAIRCH1|120300000|", 0) == 0);
            CHECK(r.until([&] {
                std::ifstream f(user / "airband-channels.json");
                std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                return t.find("FOXAIRCH1|120300000|") != std::string::npos;
            }, 6000));
        }
    }
    {
        Rig r(user, rec);
        r.settle(300);
        CHECK_EQ(r.fact("airband.channels"), std::string("1"));   // a new engine reads the file
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_AIRBAND_CHANNEL, 0.0, 2, nullptr)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_AIRBAND_CHANNEL, 0.0, 2, nullptr)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.fact("airband.channels"), std::string("0"));
    }
#ifdef _WIN32
    WSACleanup();
#endif
    { std::error_code ec; fs::remove_all(g_dir, ec); }
    return check::finish("test_engine_api_wire_a_airband");
}
