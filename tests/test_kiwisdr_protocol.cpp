// test_kiwisdr_protocol.cpp - the KiwiSDR wire format as pure functions:
// command text, frame parsing, IMA ADPCM, and the bounds of every length.
//
// WHERE THE EXPECTATIONS COME FROM, AND WHAT THEY CANNOT PROVE. Every command
// string, tag, offset and byte below is a LITERAL written by hand from the
// outline in the pre-release facts report (section 3) - never one of the
// protocol file's own constants - so a test that merely agreed with the code
// could not pass by accident. The outline says nothing about several things
// (the frequency unit, PCM byte order, ADPCM nibble order, ...); where this
// file asserts one of those it says so ("ASSUMED") and the same assumption is
// listed in docs/feature-records/kiwi-client.md. What the suite does NOT prove:
// that a real KiwiSDR sends these bytes. Nothing here has ever spoken to one.
//
// ADPCM EXPECTATIONS are computed two independent ways: a vector worked out by
// hand from the public step and index tables, and a reference ENCODER written
// in this file (the encoder tracks its own reconstruction; the decoder under
// test must reproduce it exactly, and the result must be close to the signal).
//
// BOUNDS ARE PROVEN BY A GUARD PAGE, not by hoping: each frame under test is
// copied so that it ends exactly at the end of a readable page and the next
// page is inaccessible, so any read past the end of the frame is a crash, not a
// wrong value.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "source/kiwisdr_protocol.hpp"
#include "test_check.hpp"

namespace kiwi = cascade::source::kiwi;

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- the guard page ------------------------------------------------------------

class GuardBuffer {
public:
    GuardBuffer() {
#if defined(_WIN32)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        page_ = si.dwPageSize;
        // Large enough for the biggest frame the tests hand over.
        pages_ = (kMax + page_ - 1) / page_;
        base_ = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, (pages_ + 1) * page_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        if (base_ != nullptr) { VirtualProtect(base_ + pages_ * page_, page_, PAGE_NOACCESS, &old); }
#else
        page_ = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        pages_ = (kMax + page_ - 1) / page_;
        void* p = mmap(nullptr, (pages_ + 1) * page_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        base_ = p == MAP_FAILED ? nullptr : static_cast<unsigned char*>(p);
        if (base_ != nullptr) { mprotect(base_ + pages_ * page_, page_, PROT_NONE); }
#endif
    }
    ~GuardBuffer() {
#if defined(_WIN32)
        if (base_ != nullptr) { VirtualFree(base_, 0, MEM_RELEASE); }
#else
        if (base_ != nullptr) { munmap(base_, (pages_ + 1) * page_); }
#endif
    }
    bool ok() const { return base_ != nullptr; }
    // The bytes end exactly where the inaccessible page begins.
    const std::uint8_t* place(const std::uint8_t* data, std::size_t n) {
        unsigned char* end = base_ + pages_ * page_;
        unsigned char* start = end - n;
        if (n > 0) { std::memcpy(start, data, n); }
        return start;
    }
    static constexpr std::size_t kMax = 200 * 1024;

private:
    unsigned char* base_ = nullptr;
    std::size_t page_ = 4096;
    std::size_t pages_ = 0;
};

// --- frame builders (from the outline) ---------------------------------------------

std::vector<std::uint8_t> snd(std::uint8_t flags, std::uint32_t seq, std::uint16_t smeter,
                              const std::vector<std::uint8_t>& audio) {
    std::vector<std::uint8_t> f = {'S', 'N', 'D', flags};
    for (int i = 0; i < 4; ++i) { f.push_back(static_cast<std::uint8_t>((seq >> (8 * i)) & 0xFF)); }
    f.push_back(static_cast<std::uint8_t>(smeter >> 8));
    f.push_back(static_cast<std::uint8_t>(smeter & 0xFF));
    f.insert(f.end(), audio.begin(), audio.end());
    return f;
}

std::vector<std::uint8_t> msg(const std::string& text) {
    std::vector<std::uint8_t> f = {'M', 'S', 'G', ' '};
    f.insert(f.end(), text.begin(), text.end());
    return f;
}

// --- a reference IMA ADPCM ENCODER, written for this test ----------------------------
// (the public algorithm; deliberately a different shape from the decoder: it
// searches for the nibble whose reconstruction is closest, rather than
// computing it by successive approximation)

const int kSteps[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
                        21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
                        60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
                        173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
                        494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
                        1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
                        4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
                        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
                        32767};
const int kAdj[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

struct RefEnc {
    int pred = 0;
    int idx = 0;
    // Reconstruction of nibble n from the current state (the standard).
    int recon(int n) const {
        const int step = kSteps[idx];
        int d = step >> 3;
        if (n & 1) { d += step >> 2; }
        if (n & 2) { d += step >> 1; }
        if (n & 4) { d += step; }
        int p = pred + ((n & 8) ? -d : d);
        return std::max(-32768, std::min(32767, p));
    }
    int encode(int sample) {
        int best = 0;
        int bestErr = 1 << 30;
        for (int n = 0; n < 16; ++n) {
            const int e = std::abs(recon(n) - sample);
            if (e < bestErr) {
                bestErr = e;
                best = n;
            }
        }
        pred = recon(best);
        idx = std::max(0, std::min(88, idx + kAdj[best & 7]));
        return best;
    }
};

// Two samples per byte, the FIRST sample in the LOW nibble (ASSUMED packing).
std::vector<std::uint8_t> encodeAdpcm(RefEnc& enc, const std::vector<int>& samples,
                                      std::vector<int>& reconstruction) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < samples.size(); i += 2) {
        const int a = enc.encode(samples[i]);
        reconstruction.push_back(enc.pred);
        const int b = enc.encode(samples[i + 1]);
        reconstruction.push_back(enc.pred);
        out.push_back(static_cast<std::uint8_t>(a | (b << 4)));
    }
    return out;
}

// --- the tests ---------------------------------------------------------------------------

void testBuilders() {
    // The opening commands, literal text.
    CHECK(kiwi::buildAuth("") == "SET auth t=kiwi p=");
    CHECK(kiwi::buildAuth("secret") == "SET auth t=kiwi p=secret");
    // ASSUMED: a value is percent-encoded (a space or a '&' cannot break the command).
    CHECK(kiwi::buildAuth("a b&c") == "SET auth t=kiwi p=a%20b%26c");
    CHECK(kiwi::buildAuth(std::string(129, 'x')).empty());
    CHECK(kiwi::buildIdentUser("FoxSDR") == "SET ident_user=FoxSDR");
    CHECK(kiwi::buildIdentUser("").empty());
    CHECK(kiwi::buildIdentUser(std::string(65, 'x')).empty());
    CHECK(kiwi::buildCompression(true) == "SET compression=1");
    CHECK(kiwi::buildCompression(false) == "SET compression=0");
    CHECK(kiwi::buildKeepAlive() == "SET keepalive");
    CHECK(kiwi::buildAudioRate(12000, 12000) == "SET AR OK in=12000 out=12000");
    CHECK(kiwi::buildAudioRate(0, 12000).empty());
    CHECK(kiwi::buildSoundPath(1700000000ULL) == "/1700000000/SND");

    // Tune: ASSUMED frequency unit = kHz with three places.
    CHECK(kiwi::buildTune("usb", 300, 2700, 7055000.0) == "SET mod=usb low_cut=300 high_cut=2700 freq=7055.000");
    CHECK(kiwi::buildTune("lsb", -2700, -300, 3573000.0) == "SET mod=lsb low_cut=-2700 high_cut=-300 freq=3573.000");
    CHECK(kiwi::buildTune("am", -4900, 4900, 9650500.0) == "SET mod=am low_cut=-4900 high_cut=4900 freq=9650.500");
    CHECK(kiwi::buildTune("cw", 400, 800, 0.0) == "SET mod=cw low_cut=400 high_cut=800 freq=0.000");
    CHECK(kiwi::buildTune("am", -1, 1, 1234.4) == "SET mod=am low_cut=-1 high_cut=1 freq=1.234");
    // Refusals: a mode that could carry a second command, I/Q (not built), a bad frequency.
    CHECK(kiwi::buildTune("usb SET x", 300, 2700, 7e6).empty());
    CHECK(kiwi::buildTune("usb\n", 300, 2700, 7e6).empty());
    CHECK(kiwi::buildTune("", 300, 2700, 7e6).empty());
    CHECK(kiwi::buildTune("USB", 300, 2700, 7e6).empty());
    CHECK(kiwi::buildTune("iq", 300, 2700, 7e6).empty());
    CHECK(kiwi::buildTune("usb", 300, 2700, -1.0).empty());
    CHECK(kiwi::buildTune("usb", 300, 2700, std::nan("")).empty());
    CHECK(kiwi::buildTune("usb", 300, 2700, INFINITY).empty());
    CHECK(kiwi::buildTune("usb", 300, 200000, 7e6).empty());
}

void testSndHeader() {
    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;
    // flags 0x00, sequence 0x04030201 little-endian, S-meter 0x0102 big-endian = 258.
    const auto frame = snd(0x00, 0x04030201u, 0x0102, {0x00, 0x01, 0xFF, 0xFF, 0x80, 0x00, 0x7F, 0xFF});
    CHECK(kiwi::parseFrame(frame.data(), frame.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.type == kiwi::FrameType::Snd);
    CHECK(f.snd.flags == 0x00);
    CHECK(f.snd.sequence == 0x04030201u);
    CHECK(f.snd.smeterRaw == 258);
    CHECK_NEAR(f.snd.rssiDbm, 0.1 * 258 - 127.0, 1e-9);
    CHECK_NEAR(f.snd.rssiDbm, -101.2, 1e-9);
    CHECK(!f.snd.adpcm);
    // ASSUMED: 16-bit PCM is big-endian.
    CHECK(f.snd.samples.size() == 4);
    if (f.snd.samples.size() == 4) {
        CHECK(f.snd.samples[0] == 1);
        CHECK(f.snd.samples[1] == -1);
        CHECK(f.snd.samples[2] == -32768);
        CHECK(f.snd.samples[3] == 32767);
    }
    // The RSSI formula at its ends.
    CHECK_NEAR(kiwi::rssiDbm(0), -127.0, 1e-9);
    CHECK_NEAR(kiwi::rssiDbm(1270), 0.0, 1e-9);
    CHECK_NEAR(kiwi::rssiDbm(65535), 6426.5, 1e-9);

    // The ADPCM flag is 0x10 and no other bit selects it.
    const auto a = snd(0x10, 1, 0, {0x70});
    CHECK(kiwi::parseFrame(a.data(), a.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.snd.adpcm && f.snd.samples.size() == 2);
    const auto b = snd(0xEF, 1, 0, {0x00, 0x00});  // every other bit set, not 0x10
    CHECK(kiwi::parseFrame(b.data(), b.size(), st, f) == kiwi::ParseError::None);
    CHECK(!f.snd.adpcm && f.snd.samples.size() == 1);
    CHECK(f.snd.flags == 0xEF);
    const auto c = snd(0xFF, 1, 0, {0x70});
    CHECK(kiwi::parseFrame(c.data(), c.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.snd.adpcm);

    // A header with no audio is a valid, empty frame.
    const auto e = snd(0x00, 9, 5, {});
    CHECK(kiwi::parseFrame(e.data(), e.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.snd.samples.empty());
}

void testAdpcmVectors() {
    // By hand, from the public tables. State: predictor 0, index 0.
    //   nibble 0x0: step 7;  diff = 7>>3 = 0           -> 0;     index max(0,-1) = 0
    //   nibble 0x7: step 7;  diff = 0 + 1 + 3 + 7 = 11 -> 11;    index 0+8 = 8
    //   nibble 0xF: step 16; diff = 2 + 4 + 8 + 16 = 30, negative -> -19; index 8+8 = 16
    //   nibble 0x8: step 34; diff = 4, negative         -> -23;   index 16-1 = 15
    // ASSUMED packing: the first sample of a byte is its LOW nibble, so the
    // bytes are 0x70 (0 then 7) and 0x8F (F then 8).
    kiwi::AdpcmState st;
    std::vector<std::int16_t> out;
    const std::uint8_t in[2] = {0x70, 0x8F};
    kiwi::decodeAdpcm(in, 2, st, out);
    CHECK(out.size() == 4);
    if (out.size() == 4) {
        CHECK(out[0] == 0);
        CHECK(out[1] == 11);
        CHECK(out[2] == -19);
        CHECK(out[3] == -23);
    }
    CHECK(st.predictor == -23);
    CHECK(st.index == 15);

    // Saturation up: nibble 7 over and over climbs the index to 88 and clips at 32767.
    kiwi::AdpcmState up;
    std::vector<std::int16_t> o2;
    std::vector<std::uint8_t> sevens(200, 0x77);
    kiwi::decodeAdpcm(sevens.data(), sevens.size(), up, o2);
    CHECK(o2.size() == 400);
    CHECK(up.predictor == 32767);
    CHECK(up.index == 88);
    CHECK(*std::max_element(o2.begin(), o2.end()) == 32767);
    // ... and down: nibble F clips at -32768.
    std::vector<std::uint8_t> effs(400, 0xFF);
    std::vector<std::int16_t> o3;
    kiwi::decodeAdpcm(effs.data(), effs.size(), up, o3);
    CHECK(up.predictor == -32768);
    CHECK(*std::min_element(o3.begin(), o3.end()) == -32768);
    // Index clamps at 0 from below: zeros keep it there.
    kiwi::AdpcmState z;
    std::vector<std::int16_t> o4;
    std::vector<std::uint8_t> zeros(50, 0x00);
    kiwi::decodeAdpcm(zeros.data(), zeros.size(), z, o4);
    CHECK(z.index == 0 && z.predictor == 0);
    // A state handed in out of range is brought back in range, never indexed with.
    kiwi::AdpcmState bad;
    bad.index = 5000;
    bad.predictor = 99999;
    std::vector<std::int16_t> o5;
    kiwi::decodeAdpcm(in, 2, bad, o5);
    CHECK(o5.size() == 4);
    CHECK(bad.index >= 0 && bad.index <= 88);
}

void testAdpcmAgainstReferenceEncoder() {
    // A two-tone signal, encoded by the reference encoder in frames of varying
    // size; the decoder (state carried across frames, through parseFrame) must
    // reproduce the encoder's own reconstruction EXACTLY, and that must be
    // close to the signal.
    std::vector<int> sig;
    for (int i = 0; i < 4800; ++i) {
        const double t = i / 12000.0;
        sig.push_back(static_cast<int>(std::lround(9000.0 * std::sin(2 * kPi * 440.0 * t) +
                                                   4000.0 * std::sin(2 * kPi * 1230.0 * t))));
    }
    RefEnc enc;
    std::vector<int> recon;
    std::vector<std::uint8_t> all = encodeAdpcm(enc, sig, recon);
    CHECK(recon.size() == sig.size());

    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;
    std::vector<int> got;
    const std::size_t sizes[] = {1, 7, 100, 3, 512, 64};
    std::size_t off = 0;
    std::size_t k = 0;
    std::uint32_t seq = 0;
    while (off < all.size()) {
        const std::size_t n = std::min(sizes[k++ % 6], all.size() - off);
        std::vector<std::uint8_t> audio(all.begin() + static_cast<std::ptrdiff_t>(off),
                                        all.begin() + static_cast<std::ptrdiff_t>(off + n));
        const auto frame = snd(0x10, seq++, 100, audio);
        CHECK(kiwi::parseFrame(frame.data(), frame.size(), st, f) == kiwi::ParseError::None);
        for (auto s : f.snd.samples) { got.push_back(s); }
        off += n;
    }
    CHECK(got.size() == recon.size());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < std::min(got.size(), recon.size()); ++i) {
        if (got[i] != recon[i]) { ++mismatches; }
    }
    CHECK(mismatches == 0);
    // Close to the original signal: signal-to-error above 20 dB once the step size has adapted.
    double es = 0;
    double ee = 0;
    for (std::size_t i = 200; i < got.size(); ++i) {
        es += static_cast<double>(sig[i]) * sig[i];
        const double d = static_cast<double>(sig[i]) - got[i];
        ee += d * d;
    }
    CHECK(10.0 * std::log10(es / ee) > 20.0);
}

void testMessages() {
    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;
    auto parse = [&](const std::string& text) {
        const auto fr = msg(text);
        CHECK(kiwi::parseFrame(fr.data(), fr.size(), st, f) == kiwi::ParseError::None);
        return f.message;
    };
    CHECK(parse("too_busy=4").kind == kiwi::MessageKind::TooBusy);
    CHECK(parse("too_busy=4").code == 4);
    CHECK(parse("badp=1").kind == kiwi::MessageKind::BadPassword);
    for (int c = 1; c <= 7; ++c) {
        const auto m = parse("badp=" + std::to_string(c));
        CHECK(m.kind == kiwi::MessageKind::BadPassword && m.code == c);
    }
    CHECK(parse("badp=0").kind == kiwi::MessageKind::Info);  // ASSUMED: 0 = accepted
    CHECK(parse("badp=zebra").kind == kiwi::MessageKind::BadPassword);
    CHECK(parse("badp=zebra").code == -1);
    CHECK(parse("badp").kind == kiwi::MessageKind::BadPassword);  // no value: not "accepted"
    CHECK(parse("down=1").kind == kiwi::MessageKind::Down);
    CHECK(parse("down").kind == kiwi::MessageKind::Down);
    const auto r = parse("redirect=http://example.invalid:8073/");
    CHECK(r.kind == kiwi::MessageKind::Redirect);
    CHECK(r.redirectTarget == "http://example.invalid:8073/");
    // Tolerant: other words, extra spaces, tabs, CR/LF, NULs, a word without a value.
    const auto t = parse("version_maj=1  version_min=9\tsomething  too_busy=2\r\n");
    CHECK(t.kind == kiwi::MessageKind::TooBusy && t.code == 2);
    CHECK(t.words.size() == 4);
    const auto n = parse(std::string("audio_rate=12000\0 badp=3\0", 25));
    CHECK(n.kind == kiwi::MessageKind::BadPassword && n.code == 3);
    CHECK(parse("audio_rate=12000 sample_rate=12001").kind == kiwi::MessageKind::Info);
    CHECK(parse("").kind == kiwi::MessageKind::Info);
    CHECK(parse("   ").words.empty());
    CHECK(parse("=5 =").words.empty());  // an empty key is dropped
    // Key=value splitting: only the first '=' splits.
    const auto kv = kiwi::parseKeyValues("a=b=c d= e");
    CHECK(kv.size() == 3);
    if (kv.size() == 3) {
        CHECK(kv[0].key == "a" && kv[0].value == "b=c" && kv[0].hasValue);
        CHECK(kv[1].key == "d" && kv[1].value.empty() && kv[1].hasValue);
        CHECK(kv[2].key == "e" && !kv[2].hasValue);
    }
    // A control character in a redirect target is neutralised; a long one is cut.
    const auto ctl = parse("redirect=a\x01" "b");
    CHECK(ctl.redirectTarget == "a?b");
    const auto longTarget = parse("redirect=" + std::string(2000, 'h'));
    CHECK(longTarget.redirectTarget.size() == 512);
    // A MSG without the separator space still parses.
    const std::vector<std::uint8_t> nosep = {'M', 'S', 'G', 'd', 'o', 'w', 'n'};
    CHECK(kiwi::parseFrame(nosep.data(), nosep.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.message.kind == kiwi::MessageKind::Down);
    // LIVE 2026-11-11: a real receiver sends MSG texts of 1.8 to 3 KiB (its configuration as
    // percent-encoded JSON), and the size scales with its configuration. A MSG text above
    // kMaxMessageTextBytes is therefore NOT an error: the first kMaxMessageTextBytes are parsed,
    // the rest is dropped and the frame says so. (The 64 KiB frame limit still holds.)
    const auto big = msg(std::string(5000, 'x'));
    CHECK(kiwi::parseFrame(big.data(), big.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.type == kiwi::FrameType::Msg);
    CHECK(f.messageTruncated);
    CHECK(f.message.kind == kiwi::MessageKind::Info);
    CHECK(f.message.words.size() == 1 && f.message.words[0].key.size() == kiwi::kMaxMessageTextBytes);
    // A refusal at the front of an over-long message is still seen.
    const auto busyBig = msg("too_busy=7 load_dxcfg=" + std::string(9000, 'y'));
    CHECK(kiwi::parseFrame(busyBig.data(), busyBig.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.messageTruncated && f.message.kind == kiwi::MessageKind::TooBusy && f.message.code == 7);
    // A message at exactly the limit is not truncated.
    const auto exact = msg(std::string(kiwi::kMaxMessageTextBytes, 'x'));
    CHECK(kiwi::parseFrame(exact.data(), exact.size(), st, f) == kiwi::ParseError::None);
    CHECK(!f.messageTruncated);
    // The frame limit still refuses a MSG above 64 KiB.
    const auto huge = msg(std::string(kiwi::kMaxFrameBytes, 'x'));
    CHECK(kiwi::parseFrame(huge.data(), huge.size(), st, f) == kiwi::ParseError::TooLarge);
}

void testOtherTagsAndErrors() {
    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;
    const std::vector<std::uint8_t> wf = {'W', '/', 'F', 1, 2, 3, 4};
    CHECK(kiwi::parseFrame(wf.data(), wf.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.type == kiwi::FrameType::Waterfall && f.payloadBytes == 4);
    const std::vector<std::uint8_t> ext = {'E', 'X', 'T'};
    CHECK(kiwi::parseFrame(ext.data(), ext.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.type == kiwi::FrameType::Ext && f.payloadBytes == 0);
    const std::vector<std::uint8_t> zzz = {'Z', 'Z', 'Z', 0};
    CHECK(kiwi::parseFrame(zzz.data(), zzz.size(), st, f) == kiwi::ParseError::UnknownTag);
    CHECK(kiwi::parseFrame(nullptr, 0, st, f) == kiwi::ParseError::TooShort);
    const std::uint8_t two[2] = {'S', 'N'};
    CHECK(kiwi::parseFrame(two, 2, st, f) == kiwi::ParseError::TooShort);
    // A header cut short.
    const auto full = snd(0, 1, 2, {0, 0});
    for (std::size_t n = 3; n < kiwi::kSndHeaderBytes; ++n) {
        CHECK(kiwi::parseFrame(full.data(), n, st, f) == kiwi::ParseError::TooShort);
    }
    // PCM with half a sample is refused; ADPCM of any length is not.
    const auto odd = snd(0x00, 1, 2, {1, 2, 3});
    CHECK(kiwi::parseFrame(odd.data(), odd.size(), st, f) == kiwi::ParseError::BadPayload);
    const auto oddA = snd(0x10, 1, 2, {1, 2, 3});
    CHECK(kiwi::parseFrame(oddA.data(), oddA.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.snd.samples.size() == 6);
}

void testStateOnlyAdvancesOnSuccess() {
    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;
    const auto ok = snd(0x10, 1, 0, {0x77, 0x77});
    CHECK(kiwi::parseFrame(ok.data(), ok.size(), st, f) == kiwi::ParseError::None);
    const auto before = st;
    // A refused frame (too short, too large, unknown) leaves the state alone.
    const std::vector<std::uint8_t> shortOne = {'S', 'N', 'D', 0x10, 1};
    CHECK(kiwi::parseFrame(shortOne.data(), shortOne.size(), st, f) == kiwi::ParseError::TooShort);
    CHECK(st.predictor == before.predictor && st.index == before.index);
    std::vector<std::uint8_t> huge(kiwi::kMaxFrameBytes + 1, 0x77);
    std::memcpy(huge.data(), "SND\x10", 4);
    CHECK(kiwi::parseFrame(huge.data(), huge.size(), st, f) == kiwi::ParseError::TooLarge);
    CHECK(st.predictor == before.predictor && st.index == before.index);
}

// The limit itself: the largest frame is accepted, one byte more is refused, and
// the refusal comes before any decoding (the sample vector is not touched).
void testSizeLimit() {
    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;
    std::vector<std::uint8_t> atLimit(kiwi::kMaxFrameBytes, 0x00);
    std::memcpy(atLimit.data(), "SND\x10", 4);
    CHECK(kiwi::parseFrame(atLimit.data(), atLimit.size(), st, f) == kiwi::ParseError::None);
    CHECK(f.snd.samples.size() == (kiwi::kMaxFrameBytes - kiwi::kSndHeaderBytes) * 2);
    f.snd.samples.assign(3, 42);
    std::vector<std::uint8_t> over(kiwi::kMaxFrameBytes + 1, 0x00);
    std::memcpy(over.data(), "SND\x10", 4);
    CHECK(kiwi::parseFrame(over.data(), over.size(), st, f) == kiwi::ParseError::TooLarge);
    CHECK(f.snd.samples.size() == 3);  // untouched: refused before decoding
    // A hostile 100 MB length is refused from the length alone: the pointer is never read.
    CHECK(kiwi::parseFrame(reinterpret_cast<const std::uint8_t*>(&f), 100u * 1024u * 1024u, st, f) ==
          kiwi::ParseError::TooLarge);
}

// Every prefix of every kind of frame, and garbage, handed over at the end of a
// page: a read past the end would fault.
void testNoReadPastTheEnd() {
    GuardBuffer guard;
    CHECK(guard.ok());
    if (!guard.ok()) { return; }
    kiwi::AdpcmState st;
    kiwi::ParsedFrame f;

    std::vector<std::vector<std::uint8_t>> frames;
    frames.push_back(snd(0x00, 0x01020304, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}));
    frames.push_back(snd(0x10, 0xFFFFFFFFu, 0xFFFF, {0x70, 0x8F, 0x12, 0x34}));
    frames.push_back(msg("too_busy=4 badp=2 down redirect=http://h/"));
    frames.push_back({'W', '/', 'F', 1, 2, 3});
    frames.push_back({'E', 'X', 'T', 9});
    int calls = 0;
    for (const auto& fr : frames) {
        for (std::size_t n = 0; n <= fr.size(); ++n) {
            const std::uint8_t* p = guard.place(fr.data(), n);
            (void)kiwi::parseFrame(p, n, st, f);
            ++calls;
        }
    }
    // Pseudo-random garbage of every length up to 300, tags forced to the real ones
    // half the time so the header and payload paths are reached.
    std::uint32_t rng = 12345u;
    auto next = [&] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<std::uint8_t>(rng >> 24);
    };
    const char* tags[] = {"SND", "MSG", "W/F", "EXT"};
    for (int round = 0; round < 40; ++round) {
        for (std::size_t n = 0; n <= 300; ++n) {
            std::vector<std::uint8_t> g(n);
            for (auto& b : g) { b = next(); }
            if (n >= 3 && (round & 1)) { std::memcpy(g.data(), tags[round % 4], 3); }
            const std::uint8_t* p = guard.place(g.data(), n);
            (void)kiwi::parseFrame(p, n, st, f);
            ++calls;
        }
    }
    // The largest accepted frame and the first refused one, at the page end.
    std::vector<std::uint8_t> big(kiwi::kMaxFrameBytes + 1, 0x55);
    std::memcpy(big.data(), "SND\x10", 4);
    for (std::size_t n : {kiwi::kMaxFrameBytes - 1, kiwi::kMaxFrameBytes, kiwi::kMaxFrameBytes + 1}) {
        const std::uint8_t* p = guard.place(big.data(), n);
        (void)kiwi::parseFrame(p, n, st, f);
        ++calls;
    }
    CHECK(calls > 12000);
}

}  // namespace

int main() {
    testBuilders();
    testSndHeader();
    testAdpcmVectors();
    testAdpcmAgainstReferenceEncoder();
    testMessages();
    testOtherTagsAndErrors();
    testStateOnlyAdvancesOnSuccess();
    testSizeLimit();
    testNoReadPastTheEnd();
    return testSummary("test_kiwisdr_protocol");
}
