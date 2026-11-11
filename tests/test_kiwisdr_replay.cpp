// test_kiwisdr_replay.cpp - the KiwiSDR client against what a REAL receiver sent.
//
// On 2026-11-11 one listen-only connection was made to a public KiwiSDR (tests/
// test_kiwisdr_live_probe.cpp, docs/feature-records/kiwi-client.md "Live test 2026-11-11") and
// every frame the receiver sent was kept, unmasked, in frames.bin under
// D:/foxsdr-samples/kiwi-live/ (or the folder named by the environment variable KIWILIVE_DIR).
// THOSE FRAMES ARE NOT COMMITTED: they carry somebody else's broadcast and other users'
// names. This test therefore
//
//   1. always runs a short hand-typed excerpt of the receiver's protocol text (keywords and
//      numbers only, nothing personal), and
//   2. replays the recording when the folder exists - first through the frame parser, then over a
//      real loopback socket through the whole client - and SKIPS CLEANLY (counted, never a
//      failure) when it does not.
//
// The recording is 44 s of the 77.5 kHz German time signal (DCF77) heard on a Belgian receiver in
// AM, IMA ADPCM. The carrier is dipped for 0.1 s or 0.2 s at the start of every second, so the
// decoded audio has an envelope dip once a second: that is what proves the ADPCM decoder (nibble
// order, state carried across frames) against the real server rather than against our own idea
// of the format.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "kiwisdr_fake_server.hpp"
#include "source/kiwisdr_client.hpp"
#include "source/kiwisdr_protocol.hpp"
#include "test_check.hpp"

using cascade::source::KiwiSdrClient;
namespace kiwi = cascade::source::kiwi;
using ms = std::chrono::milliseconds;

namespace {

struct RecFrame {
    bool fromServer;
    std::uint8_t opcode;
    std::string payload;
};

bool loadFrames(const std::string& path, std::vector<RecFrame>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { return false; }
    for (;;) {
        std::uint8_t h[14];
        if (std::fread(h, 1, sizeof h, f) != sizeof h) { break; }
        const std::uint32_t n = h[2] | (h[3] << 8) | (h[4] << 16) | (static_cast<std::uint32_t>(h[5]) << 24);
        if (n > (16u << 20)) { break; }
        RecFrame r;
        r.fromServer = h[0] == 1;
        r.opcode = h[1] & 0x0F;
        r.payload.resize(n);
        if (n && std::fread(&r.payload[0], 1, n, f) != n) { break; }
        out.push_back(std::move(r));
    }
    std::fclose(f);
    return !out.empty();
}

std::vector<std::uint8_t> bytesOf(const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); }

// --- 1. the hand-typed excerpt: protocol keywords and numbers only -------------------------------

void testHandTypedExcerpt() {
    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;
    // The receiver's first batch of MSG lines after the upgrade (values are numbers; the
    // server-specific ones are left out).
    const char* lines[] = {"rx_chans=50",           "chan_no_pwd=0",        "chan_no_pwd_true=0", "max_camp=50",
                           "badp=0",                "center_freq=15000000", "bandwidth=30000000", "adc_clk_nom=66666600",
                           "sample_rate=12000.000000", "rx_chan=7",         "cfg_loaded",         "audio_init=0 audio_rate=12000"};
    for (const char* l : lines) {
        const auto b = bytesOf(std::string("MSG ") + l);
        CHECK(kiwi::parseFrame(b.data(), b.size(), st, f) == kiwi::ParseError::None);
        CHECK(f.type == kiwi::FrameType::Msg);
        CHECK(f.message.kind == kiwi::MessageKind::Info);
        CHECK(!f.messageTruncated);
    }
    // The answer to the auth command with no password on a receiver that wants none: badp=0, a
    // plain Info (accepted); the sample rate and audio rate keys carry the numbers the client uses.
    auto b = bytesOf("MSG sample_rate=12000.000000");
    CHECK(kiwi::parseFrame(b.data(), b.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.message.words.size() == 1 && f.message.words[0].key == "sample_rate" &&
          f.message.words[0].value == "12000.000000");
    b = bytesOf("MSG audio_init=0 audio_rate=12000");
    CHECK(kiwi::parseFrame(b.data(), b.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.message.words.size() == 2 && f.message.words[1].key == "audio_rate" && f.message.words[1].value == "12000");
    // A real SND header: flags 0x10 (ADPCM), sequence 1 little-endian, S-meter 0x01F3 big-endian
    // (499 -> -77.1 dBm), then 120 payload bytes = 240 samples.
    const std::string snd = std::string("SND") + std::string("\x10\x01\x00\x00\x00\x01\xF3", 7) + std::string(120, '\x77');
    const auto sb = bytesOf(snd);
    CHECK(kiwi::parseFrame(sb.data(), sb.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.snd.adpcm && f.snd.sequence == 1 && f.snd.smeterRaw == 499 && f.snd.samples.size() == 240);
    CHECK_NEAR(f.snd.rssiDbm, -77.1, 1e-9);
}

// --- 2. the recording ---------------------------------------------------------------------------

std::vector<double> blockRms(const std::vector<std::int16_t>& x, std::size_t blk) {
    std::vector<double> e;
    for (std::size_t b = 0; (b + 1) * blk <= x.size(); ++b) {
        double s = 0;
        for (std::size_t k = 0; k < blk; ++k) { const double v = x[b * blk + k]; s += v * v; }
        e.push_back(std::sqrt(s / static_cast<double>(blk)));
    }
    return e;
}

void testRecordedFrames(const std::vector<RecFrame>& rec) {
    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;
    std::vector<std::int16_t> audio;
    std::size_t snds = 0, msgs = 0, other = 0;
    std::uint32_t expect = 0;
    bool contiguous = true, allAdpcm = true, all240 = true;
    for (const auto& r : rec) {
        if (!r.fromServer || r.opcode != 2) { continue; }
        const auto b = bytesOf(r.payload);
        const auto e = kiwi::parseFrame(b.data(), b.size(), st, f);
        CHECK(e == kiwi::ParseError::None);
        if (e != kiwi::ParseError::None) { continue; }
        if (f.type == kiwi::FrameType::Snd) {
            ++snds;
            contiguous = contiguous && f.snd.sequence == expect;
            expect = f.snd.sequence + 1;
            allAdpcm = allAdpcm && f.snd.adpcm && f.snd.flags == 0x10;
            all240 = all240 && f.snd.samples.size() == 240;
            audio.insert(audio.end(), f.snd.samples.begin(), f.snd.samples.end());
        } else if (f.type == kiwi::FrameType::Msg) {
            ++msgs;
            CHECK(f.message.kind == kiwi::MessageKind::Info);  // no refusal, no down, no redirect
        } else {
            ++other;
        }
    }
    std::printf("replay: %zu SND, %zu MSG, %zu other frames, %zu samples\n", snds, msgs, other, audio.size());
    CHECK(snds > 1000);
    CHECK(contiguous && allAdpcm && all240);
    CHECK(other == 0);
    CHECK(audio.size() == snds * 240);

    // THE PROOF: once a second the carrier dips (DCF77 second marks). Blocks of 10 ms at 12 kHz.
    const auto env = blockRms(audio, 120);
    std::vector<double> sorted = env;
    std::sort(sorted.begin(), sorted.end());
    const double med = sorted[sorted.size() / 2];
    std::vector<std::size_t> starts;
    bool in = false;
    for (std::size_t b = 0; b < env.size(); ++b) {
        const bool low = env[b] < 0.6 * med;
        if (low && !in) { starts.push_back(b); }
        in = low;
    }
    // The first dip is the start-up transient; judge the rest.
    int onSecond = 0, spaced = 0;
    for (std::size_t i = 2; i < starts.size(); ++i) {
        ++spaced;
        const long d = static_cast<long>(starts[i] - starts[i - 1]);
        if (d >= 99 && d <= 101) { ++onSecond; }
    }
    std::printf("replay: %zu envelope dips, %d of %d spaced 1.00 s +- 10 ms\n", starts.size(), onSecond, spaced);
    CHECK(starts.size() >= 35);
    CHECK(spaced > 0 && onSecond * 10 >= spaced * 9);
    // The carrier is a large positive offset (AM envelope, not DC-blocked); no clipping.
    long long sum = 0;
    int clipped = 0;
    for (auto v : audio) { sum += v; clipped += (v == 32767 || v == -32768); }
    CHECK(clipped == 0);
    CHECK(sum / static_cast<long long>(audio.size()) > 500);
}

void testRecordedThroughTheClient(const std::vector<RecFrame>& rec) {
    // What the parser makes of the recording, for the client to be held against.
    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;
    std::vector<std::int16_t> want;
    std::size_t msgCount = 0;
    for (const auto& r : rec) {
        if (!r.fromServer || r.opcode != 2) { continue; }
        const auto b = bytesOf(r.payload);
        if (kiwi::parseFrame(b.data(), b.size(), st, f) != kiwi::ParseError::None) { continue; }
        if (f.type == kiwi::FrameType::Snd) { want.insert(want.end(), f.snd.samples.begin(), f.snd.samples.end()); }
        if (f.type == kiwi::FrameType::Msg) { ++msgCount; }
    }

    kiwifake::FakeKiwi srv([&rec](kiwifake::Conn& c, int) {
        if (!c.upgrade()) { return; }
        int n = 0;
        for (const auto& r : rec) {
            if (!r.fromServer || r.opcode != 2) { continue; }
            if (!c.sendBinary(r.payload)) { return; }
            if (r.payload.compare(0, 3, "SND") == 0 && (++n % 4) == 0 && !c.sleep(1)) { return; }
        }
        while (c.sleep(10)) {}
    });
    CHECK(srv.ok());
    KiwiSdrClient client;
    KiwiSdrClient::Options o;
    o.host = "127.0.0.1";
    o.port = srv.port();
    o.connectWait = ms(2000);
    o.stallWait = ms(5000);
    CHECK(client.open(o));
    std::vector<std::int16_t> got;
    std::vector<std::int16_t> buf(4096);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (got.size() < want.size() && std::chrono::steady_clock::now() < end) {
        const std::size_t n = client.read(buf.data(), buf.size(), ms(50));
        got.insert(got.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
    }
    CHECK(client.state() == KiwiSdrClient::State::Playing);
    CHECK(got == want);
    CHECK(client.droppedSamples() == 0);
    CHECK(client.sequenceGaps() == 0);
    CHECK(client.ignoredFrames() == msgCount);
    CHECK(client.haveSmeter());
    client.close();
}

}  // namespace

int main() {
    testHandTypedExcerpt();

    const char* env = std::getenv("KIWILIVE_DIR");
    const std::string dir = env && *env ? env : "D:/foxsdr-samples/kiwi-live";
    std::vector<RecFrame> rec;
    if (!loadFrames(dir + "/frames.bin", rec)) {
        ++g_checksSkipped;
        std::printf("SKIP: no recording at %s/frames.bin (local data, never committed)\n", dir.c_str());
        return testSummary("test_kiwisdr_replay");
    }
    testRecordedFrames(rec);
    testRecordedThroughTheClient(rec);
    return testSummary("test_kiwisdr_replay");
}
