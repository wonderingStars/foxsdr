// Tests for the DCS block (dsp/dcs.hpp): the 83 codes, the Golay (23,12) word and its error
// correction, and the stream decoder. GENERATED SIGNALS ONLY: no recording of a real DCS
// transmission exists in the sample set, so the convention (which Golay code, which bit order) is
// derived (see the header) and proven consistent, not proven against the air.
//
// The test's own encoder is a DIFFERENT arrangement of the same code (generator
// x^11+x^10+x^6+x^5+x^4+x^2+1 = 0xC75 as given by Wikipedia and the facts report, transmitted bit
// t_j as the coefficient of x^j, data in the low 12 positions, check bits found by search), so the
// header's encoder (generator 0xAE3, t_j as the coefficient of x^(22-j)) is checked against it,
// not against itself.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/dcs.hpp"

#include "dsp/ctcss.hpp"

#include "alloc_probe.hpp"
#include "signal_block_gen.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <set>
#include <vector>

namespace {

using cascade::dsp::DcsDecoder;
using cascade::dsp::kDcsStandardCodes;
using namespace sbgen;
using Pol = DcsDecoder::Polarity;

// The RadioReference wiki Inverted Codes chart (https://wiki.radioreference.com/index.php/CDCSS, read 2026-11-11),
// the rows whose first code is one of the 83 standard codes: {code, the code an INVERTED transmission of it is read as}.
// An independent published table, not derived from anything in dsp/dcs.hpp.
constexpr unsigned kRadioReferencePairs[83][2] = {
    {0023, 0047}, {0025, 0244}, {0026, 0464}, {0031, 0627}, {0032, 0051}, {0043, 0445},
    {0047, 0023}, {0051, 0032}, {0054, 0413}, {0065, 0271}, {0071, 0306}, {0072, 0245},
    {0073, 0506}, {0074, 0174}, {0114, 0712}, {0115, 0152}, {0116, 0754}, {0125, 0365},
    {0131, 0364}, {0132, 0546}, {0134, 0223}, {0143, 0412}, {0152, 0115}, {0155, 0731},
    {0156, 0265}, {0162, 0503}, {0165, 0251}, {0172, 0036}, {0174, 0074}, {0205, 0263},
    {0223, 0134}, {0226, 0411}, {0243, 0351}, {0244, 0025}, {0245, 0072}, {0251, 0165},
    {0261, 0732}, {0263, 0205}, {0265, 0156}, {0271, 0065}, {0306, 0071}, {0311, 0664},
    {0315, 0423}, {0331, 0465}, {0343, 0532}, {0346, 0612}, {0351, 0243}, {0364, 0131},
    {0365, 0125}, {0371, 0734}, {0411, 0226}, {0412, 0143}, {0413, 0054}, {0423, 0315},
    {0431, 0723}, {0432, 0516}, {0445, 0043}, {0464, 0026}, {0465, 0331}, {0466, 0662},
    {0503, 0162}, {0506, 0073}, {0516, 0432}, {0532, 0343}, {0546, 0132}, {0565, 0703},
    {0606, 0631}, {0612, 0346}, {0624, 0632}, {0627, 0031}, {0631, 0606}, {0632, 0624},
    {0654, 0743}, {0662, 0466}, {0664, 0311}, {0703, 0565}, {0712, 0114}, {0723, 0431},
    {0731, 0155}, {0732, 0261}, {0734, 0371}, {0743, 0654}, {0754, 0116},
};

// --- the test's own encoder (independent arrangement, see the file comment) ---------------
std::uint32_t polyMod(std::uint32_t v, std::uint32_t g) {
    int gdeg = 0;
    for (int b = 31; b >= 0; --b) { if ((g >> b) & 1u) { gdeg = b; break; } }
    for (int b = 31; b >= gdeg; --b) {
        if ((v >> b) & 1u) { v ^= g << (b - gdeg); }
    }
    return v;
}

std::uint32_t refWord(unsigned code9) {
    const std::uint32_t data = (code9 & 0x1FFu) | (1u << 11);
    for (std::uint32_t p = 0; p < 2048; ++p) {
        const std::uint32_t c = data | (p << 12);  // t_j is the coefficient of x^j
        if (polyMod(c, 0xC75) == 0) { return c; }  // bit j = t_j: the same convention as dcs::encode's result
    }
    return 0;
}

// --- a DCS audio signal --------------------------------------------------------------------
// The 23-bit word repeated, NRZ at +-amp, bit rate 134.4 * (1 + clockError), shaped by two one-pole
// low-passes at 350 Hz (a transmitter's data filter: about 225 Hz overall), `startBit` bits into the word. `flip(wordIndex)`
// returns a mask of the 23 bits to invert in that repetition.
template <typename Flip>
std::vector<float> dcsAudio(std::uint32_t word, double fs, double seconds, double amp, double clockError,
                            int startBit, Flip flip) {
    const std::size_t n = static_cast<std::size_t>(seconds * fs);
    std::vector<float> v(n);
    const double bitRate = 134.4 * (1.0 + clockError);
    const double a = 1.0 - std::exp(-kTwoPi * 350.0 / fs);
    double y1 = 0, y2 = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double tb = static_cast<double>(i) * bitRate / fs + startBit;
        const std::uint64_t bi = static_cast<std::uint64_t>(tb);
        const std::uint64_t rep = bi / 23;
        const std::uint32_t w = word ^ flip(rep);
        const double x = ((w >> (bi % 23)) & 1u) ? amp : -amp;
        y1 += a * (x - y1);
        y2 += a * (y1 - y2);
        v[i] = static_cast<float>(y2);
    }
    return v;
}
std::vector<float> dcsAudio(std::uint32_t word, double fs, double seconds, double amp, double clockError = 0.0,
                            int startBit = 0) {
    return dcsAudio(word, fs, seconds, amp, clockError, startBit, [](std::uint64_t) { return 0u; });
}

// Feeds v in 1 ms chunks; returns the time of the first gate open (-1 if never).
double feedUntilGate(DcsDecoder& d, const std::vector<float>& v, double fs) {
    const std::size_t chunk = static_cast<std::size_t>(fs / 1000.0);
    for (std::size_t i = 0; i + chunk <= v.size(); i += chunk) {
        d.process(v.data() + i, chunk);
        if (d.gateOpen()) { return static_cast<double>(i + chunk) / fs; }
    }
    return -1.0;
}

int lockEdges(DcsDecoder& d, const std::vector<float>& v, double fs) {
    const std::size_t chunk = static_cast<std::size_t>(fs / 1000.0);
    int edges = 0;
    bool was = false;
    for (std::size_t i = 0; i + chunk <= v.size(); i += chunk) {
        d.process(v.data() + i, chunk);
        const bool now = d.detectedCode() != 0;
        if (now && !was) { ++edges; }
        was = now;
    }
    return edges;
}

void testTable() {
    CHECK(kDcsStandardCodes.size() == 83);
    std::set<unsigned> seen;
    for (unsigned c : kDcsStandardCodes) {
        CHECK(c < 512);
        // Octal digits: the three-digit number written in octal, first digit 0-7.
        CHECK(seen.insert(c).second);
    }
    CHECK(std::is_sorted(kDcsStandardCodes.begin(), kDcsStandardCodes.end()));
    CHECK(kDcsStandardCodes.front() == 023);
    CHECK(kDcsStandardCodes.back() == 0754);
    CHECK(cascade::dsp::dcsIsStandardCode(023));
    CHECK(cascade::dsp::dcsIsStandardCode(0754));
    CHECK(!cascade::dsp::dcsIsStandardCode(0));
    CHECK(!cascade::dsp::dcsIsStandardCode(024));
    CHECK(!cascade::dsp::dcsIsStandardCode(0755));
    // The octal literals in the table are what the Wikipedia list prints (spot values).
    CHECK(kDcsStandardCodes[14] == 0114);
    CHECK(kDcsStandardCodes[29] == 0205);
    CHECK(kDcsStandardCodes[40] == 0306);
}

void testGolay() {
    // Every 9-bit code makes a codeword; it equals the independent encoder's word.
    for (unsigned c = 0; c < 512; ++c) {
        const std::uint32_t w = cascade::dsp::dcs::encode(c);
        CHECK(cascade::dsp::dcs::syndrome(w) == 0);
        CHECK(w == refWord(c));
        CHECK((w & 0x1FF) == c);
        CHECK(((w >> 9) & 7u) == 4u);  // the fixed 001 (t_9, t_10 = 0, t_11 = 1)
        CHECK(w <= cascade::dsp::dcs::kWordMask);
    }
    // Minimum distance of the 83 words among themselves, both polarities: at least 7.
    int minDist = 99;
    std::vector<std::uint32_t> words;
    for (unsigned c : kDcsStandardCodes) {
        words.push_back(cascade::dsp::dcs::encode(c));
        words.push_back(cascade::dsp::dcs::invert(words.back()));
    }
    for (std::size_t i = 0; i < words.size(); ++i) {
        for (std::size_t j = i + 1; j < words.size(); ++j) {
            int d = 0;
            for (std::uint32_t x = words[i] ^ words[j]; x != 0; x &= x - 1) { ++d; }
            minDist = std::min(minDist, d);
        }
    }
    std::printf("  golay: minimum distance among the 83 codes in both polarities = %d\n", minDist);
    CHECK(minDist >= 7);
    // The correction table is perfect: every one of the 2048 syndromes maps to a distinct pattern of at most 3 bits.
    const auto& tab = cascade::dsp::dcs::correctionTable();
    CHECK(tab.size() == 2048);
    std::set<std::uint32_t> pats(tab.begin(), tab.end());
    CHECK(pats.size() == 2048);
    // EVERY pattern of 1, 2 and 3 bit errors corrects back to the original (exhaustive, for 3 codes).
    for (unsigned code : {023u, 0377u & 0x1FFu, 0754u}) {
        const std::uint32_t w = cascade::dsp::dcs::encode(code);
        long patterns = 0, failures = 0;
        for (int a = 0; a < 23; ++a) {
            for (int b = a; b < 23; ++b) {
                for (int c = b; c < 23; ++c) {
                    std::uint32_t e = (1u << a);
                    if (b != a) { e ^= (1u << b); }
                    if (c != b && c != a) { e ^= (1u << c); }
                    const int weight = [&] { int k = 0; for (std::uint32_t x = e; x; x &= x - 1) { ++k; } return k; }();
                    if (weight < 1 || weight > 3) { continue; }
                    // (a<=b<=c enumeration visits each pattern more than once; harmless)
                    ++patterns;
                    const auto r = cascade::dsp::dcs::correct(w ^ e);
                    if (r.word != w || r.errors != weight) { ++failures; }
                }
            }
        }
        CHECK(patterns > 0 && failures == 0);
    }
    // No error: zero corrections.
    CHECK(cascade::dsp::dcs::correct(cascade::dsp::dcs::encode(023)).errors == 0);
    // EVERY 4-bit error pattern (8855) decodes to some OTHER codeword (the perfect code cannot return the original),
    // and most of them do not even make a marked standard word.
    {
        const std::uint32_t w = cascade::dsp::dcs::encode(0245);
        long total = 0, original = 0, accepted = 0;
        for (int a = 0; a < 23; ++a)
            for (int b = a + 1; b < 23; ++b)
                for (int c = b + 1; c < 23; ++c)
                    for (int d = c + 1; d < 23; ++d) {
                        const std::uint32_t e = (1u << a) | (1u << b) | (1u << c) | (1u << d);
                        const auto r = cascade::dsp::dcs::correct(w ^ e);
                        ++total;
                        if (r.word == w) { ++original; }
                        if (((r.word >> 9) & 7u) == 4u && cascade::dsp::dcsIsStandardCode(r.word & 0x1FFu)) { ++accepted; }
                    }
        std::printf("  golay: 4-bit errors: %ld patterns, %ld back to the original, %ld land on a marked standard word\n", total, original, accepted);
        CHECK(total == 8855);
        CHECK(original == 0);
    }
    // Inversion is an involution and stays inside 23 bits.
    CHECK(cascade::dsp::dcs::invert(cascade::dsp::dcs::invert(0x12345)) == 0x12345);
    CHECK(cascade::dsp::dcs::invert(0) == cascade::dsp::dcs::kWordMask);
}

void testAlignmentCollisionProperty() {
    // The property the convention was derived from: no cyclic rotation of a standard code's word is the marked
    // word of ANOTHER standard code (nor of itself at a shifted alignment). This is the stated reason that only
    // 83 of 512 codes are standard. With the reciprocal generator it fails (34 collisions) - see the record.
    long collisions = 0;
    for (unsigned c : kDcsStandardCodes) {
        const std::uint32_t w = cascade::dsp::dcs::encode(c);
        for (int s = 1; s < 23; ++s) {
            const std::uint32_t r = ((w >> s) | (w << (23 - s))) & cascade::dsp::dcs::kWordMask;
            if (((r >> 9) & 7u) == 4u && cascade::dsp::dcsIsStandardCode(r & 0x1FFu)) { ++collisions; }
        }
    }
    std::printf("  alignment collisions among the 83 standard words: %ld\n", collisions);
    CHECK(collisions == 0);
}

void testInverseEquivalence() {
    // The derived convention against the independent RadioReference chart: for each of the 83 standard codes, the code
    // that its inverted transmission is read as.
    int wrong = 0;
    for (const auto& pr : kRadioReferencePairs) {
        if (cascade::dsp::dcs::inversePartner(pr[0]) != static_cast<int>(pr[1])) {
            ++wrong;
            std::printf("  FAIL chart: %03o inverted should be %03o, dcs::inversePartner says %d\n", pr[0], pr[1], cascade::dsp::dcs::inversePartner(pr[0]));
        }
    }
    CHECK(wrong == 0);
    // The relation is symmetric wherever both ends are standard; a code that is its own partner or has none is stated.
    int standardBoth = 0, standardOne = 0;
    for (unsigned c : kDcsStandardCodes) {
        const int p = cascade::dsp::dcs::inversePartner(c);
        CHECK(p >= 0);
        if (p >= 0 && cascade::dsp::dcsIsStandardCode(static_cast<unsigned>(p))) {
            ++standardBoth;
            CHECK(cascade::dsp::dcs::inversePartner(static_cast<unsigned>(p)) == static_cast<int>(c));
        } else {
            ++standardOne;
        }
    }
    std::printf("  inverse partners: %d standard codes have a standard partner, %d have a non-standard one\n", standardBoth, standardOne);
    CHECK(standardBoth == 82 && standardOne == 1);  // 172 reads as 036, which is not standard
    CHECK(cascade::dsp::dcs::inversePartner(023) == 047);
    CHECK(cascade::dsp::dcs::inversePartner(512) == -1);
    // On the air: 023 normal IS 047 inverted. The decoder reports the normal reading and the alternate.
    for (int inv = 0; inv < 2; ++inv) {
        std::uint32_t w = cascade::dsp::dcs::encode(inv ? 047 : 023);
        if (inv) { w = cascade::dsp::dcs::invert(w); }  // 047 inverted == 023 normal
        std::vector<float> v = dcsAudio(w, 48000.0, 6.0, 0.1, 0.0, 5);
        DcsDecoder d(48000.0);
        d.process(v.data(), 3 * 48000);
        CHECK(d.detectedCode() == 023 && d.detectedPolarity() == 1);
        CHECK(d.detectedAltCode() == 047 && d.detectedAltPolarity() == 2);
        CHECK(d.isDetected(023, Pol::Normal) && d.isDetected(047, Pol::Inverted));
        CHECK(!d.isDetected(023, Pol::Inverted) && !d.isDetected(047, Pol::Normal));
        CHECK(d.isDetected(023, Pol::Any) && d.isDetected(047, Pol::Any) && !d.isDetected(025, Pol::Any));
        // Choosing either reading opens the gate; choosing 047 NORMAL (a different stream) does not.
        d.setSelected(047, Pol::Inverted);
        d.process(v.data() + 3 * 48000, 4800);
        CHECK(d.gateOpen());
        d.setSelected(023, Pol::Normal);
        d.process(v.data() + 3 * 48000 + 4800, 4800);
        CHECK(d.gateOpen());
        d.setSelected(047, Pol::Normal);
        d.process(v.data() + 3 * 48000 + 9600, 4800);
        CHECK(!d.gateOpen());
        d.setSelected(047, Pol::Any);
        d.process(v.data() + 3 * 48000 + 14400, 4800);
        CHECK(d.gateOpen());
    }
}

void testRoundTripAllCodes() {
    // Every one of the 83 codes, normal and inverted, through a generated signal: locked, right code,
    // right polarity, gate open when chosen, closed when another code or the other polarity is chosen.
    for (double fs : {48000.0, 24000.0}) {
        double worstLock = 0;
        int bad = 0;
        std::mt19937 rng(5);
        for (unsigned code : kDcsStandardCodes) {
            for (int inv = 0; inv < 2; ++inv) {
                std::uint32_t w = cascade::dsp::dcs::encode(code);
                if (inv) { w = cascade::dsp::dcs::invert(w); }
                const int start = static_cast<int>(rng() % 23);
                const double clk = (static_cast<int>(rng() % 21) - 10) / 1000.0;  // +-1 %
                std::vector<float> v = dcsAudio(w, fs, 3.0, 0.1, clk, start);
                DcsDecoder d(fs);
                d.setSelected(code, inv ? Pol::Inverted : Pol::Normal);
                const double t = feedUntilGate(d, v, fs);
                // The stream is `code` with the polarity sent, under one of its two readings (see dsp/dcs.hpp,
                // ON-AIR EQUIVALENCE); the canonical reading is the normal-polarity one when there is one.
                if (t < 0 || !d.isDetected(code, inv ? Pol::Inverted : Pol::Normal)) {
                    ++bad;
                    std::printf("  FAIL code %03o inv %d fs %.0f t=%.2f got %03o pol %d alt %03o\n", code, inv, fs, t, d.detectedCode(), d.detectedPolarity(), d.detectedAltCode());
                } else {
                    worstLock = std::max(worstLock, t);
                }
                // The wrong polarity chosen: the gate must stay shut though the code is locked (the other
                // polarity of the same number is a different stream, except for the equivalence pairs).
                DcsDecoder d2(fs);
                d2.setSelected(code, inv ? Pol::Normal : Pol::Inverted);
                const double t2 = feedUntilGate(d2, v, fs);
                if (t2 >= 0) { ++bad; std::printf("  FAIL wrong-polarity gate opened for %03o inv %d\n", code, inv); }
            }
        }
        std::printf("  dcs round trip fs=%.0f: 166 signals, bad=%d, worst lock time %.3f s\n", fs, bad, worstLock);
        CHECK(bad == 0);
        CHECK(worstLock <= 1.0);
    }
    // A different code chosen: closed. Any code (none chosen): open.
    {
        std::vector<float> v = dcsAudio(cascade::dsp::dcs::encode(023), 48000.0, 3.0, 0.1);
        DcsDecoder d(48000.0);
        d.setSelected(025, Pol::Any);
        CHECK(feedUntilGate(d, v, 48000.0) < 0);
        CHECK(d.detectedCode() == 023);
        DcsDecoder e(48000.0);
        CHECK(feedUntilGate(e, v, 48000.0) > 0);
        e.setSelected(023, Pol::Any);
        e.process(v.data(), 4800);
        CHECK(e.gateOpen());
    }
}

void testWithNoiseSpeechAndOffsets() {
    const double fs = 48000.0;
    cascade::dsp::CtcssHighpass txHp(fs);
    int bad = 0;
    for (unsigned code : {023u, 0132u, 0245u, 0411u, 0754u}) {
        for (int inv = 0; inv < 2; ++inv) {
            std::uint32_t w = cascade::dsp::dcs::encode(code);
            if (inv) { w = cascade::dsp::dcs::invert(w); }
            const std::size_t n = static_cast<std::size_t>(4.0 * fs);
            std::vector<float> v = dcsAudio(w, fs, 4.0, 0.1, 0.004, 7);
            std::vector<float> sp = speechLike(n, fs, 0.25, 77 + code), spf(n);
            txHp.reset();
            txHp.process(sp.data(), spf.data(), n);
            std::vector<float> noise = discriminatorNoise(n, 0.02, 99 + code);
            for (std::size_t i = 0; i < n; ++i) { v[i] += spf[i] + noise[i] + 0.15f; }  // speech + noise + DC offset
            DcsDecoder d(fs);
            const double t = feedUntilGate(d, v, fs);
            if (t < 0 || !d.isDetected(code, inv ? Pol::Inverted : Pol::Normal)) { ++bad; std::printf("  FAIL noisy %03o inv %d\n", code, inv); }
        }
    }
    CHECK(bad == 0);
    // Weak data: 10 dB less amplitude over the same noise still decodes.
    {
        std::vector<float> v = dcsAudio(cascade::dsp::dcs::encode(0245), fs, 6.0, 0.02);
        std::vector<float> noise = discriminatorNoise(v.size(), 0.01, 3);
        for (std::size_t i = 0; i < v.size(); ++i) { v[i] += noise[i]; }
        DcsDecoder d(fs);
        CHECK(feedUntilGate(d, v, fs) > 0 && d.isDetected(0245, Pol::Normal));
    }
}

void testBitErrors() {
    // 1, 2 and 3 bit errors in EVERY repetition (different positions each time) still decode; with
    // maxCorrectedBits = 2 three-bit errors are refused; 4-bit errors never produce the right code.
    const double fs = 48000.0;
    const unsigned code = 0245;
    const std::uint32_t w = cascade::dsp::dcs::encode(code);
    auto burst = [&](int k) {
        return [k](std::uint64_t rep) {
            std::uint32_t m = 0;
            std::mt19937 g(static_cast<std::uint32_t>(rep * 7919 + 13));
            std::set<int> pos;
            while (static_cast<int>(pos.size()) < k) { pos.insert(static_cast<int>(g() % 23)); }
            for (int p : pos) { m |= 1u << p; }
            return m;
        };
    };
    for (int k = 1; k <= 3; ++k) {
        std::vector<float> v = dcsAudio(w, fs, 5.0, 0.1, 0.0, 3, burst(k));
        DcsDecoder d(fs);
        const double t = feedUntilGate(d, v, fs);
        std::printf("  %d bit error(s) per word: lock at %.2f s code %03o corrected %d\n", k, t, d.detectedCode(), d.lastCorrectedBits());
        CHECK(t > 0 && d.isDetected(code, Pol::Normal));
    }
    {
        std::vector<float> v = dcsAudio(w, fs, 5.0, 0.1, 0.0, 3, burst(3));
        DcsDecoder d(fs);
        d.setMaxCorrectedBits(2);
        CHECK(feedUntilGate(d, v, fs) < 0);  // 3-bit errors every word, only 2 allowed: no lock
        CHECK(d.maxCorrectedBits() == 2);
        d.setMaxCorrectedBits(9);
        CHECK(d.maxCorrectedBits() == 3);
        d.setMaxCorrectedBits(-4);
        CHECK(d.maxCorrectedBits() == 0);
    }
    {
        std::vector<float> v = dcsAudio(w, fs, 8.0, 0.1, 0.0, 3, burst(4));
        DcsDecoder d(fs);
        feedUntilGate(d, v, fs);
        CHECK(!d.isDetected(code, Pol::Normal));  // 4 errors can never come back as the original
    }
    // One word in four ruined by 6 bit errors: the lock holds (the release needs two misses in a row at the alignment).
    {
        std::vector<float> v = dcsAudio(w, fs, 6.0, 0.1, 0.0, 3, [](std::uint64_t rep) { return (rep % 4 == 1) ? 0x2A5A5Au : 0u; });
        DcsDecoder d(fs);
        bool dropped = false, locked = false;
        const std::size_t chunk = 480;
        for (std::size_t i = 0; i + chunk <= v.size(); i += chunk) {
            d.process(v.data() + i, chunk);
            if (d.gateOpen()) { locked = true; }
            if (locked && !d.gateOpen()) { dropped = true; }
        }
        CHECK(locked);
        CHECK(!dropped);
    }
}

void testReleaseAndSilence() {
    const double fs = 48000.0;
    const std::size_t on = static_cast<std::size_t>(3.0 * fs), total = static_cast<std::size_t>(6.0 * fs);
    std::vector<float> v = discriminatorNoise(total, 0.02, 1);
    std::vector<float> d0 = dcsAudio(cascade::dsp::dcs::encode(0245), fs, 3.0, 0.1);
    for (std::size_t i = 0; i < on; ++i) { v[i] += d0[i]; }
    DcsDecoder d(fs);
    double tOpen = -1, tClose = -1;
    for (std::size_t i = 0; i + 480 <= total; i += 480) {
        d.process(v.data() + i, 480);
        if (tOpen < 0 && d.gateOpen()) { tOpen = static_cast<double>(i + 480) / fs; }
        if (tOpen >= 0 && tClose < 0 && i >= on && !d.gateOpen()) { tClose = static_cast<double>(i + 480 - on) / fs; }
    }
    std::printf("  dcs: lock at %.2f s, release %.2f s after the data stops\n", tOpen, tClose);
    CHECK(tOpen > 0 && tOpen <= 1.0);
    CHECK(tClose > 0 && tClose <= 1.0);
    // Digital silence and DC: nothing.
    DcsDecoder s(fs);
    std::vector<float> z(static_cast<std::size_t>(fs), 0.0f), dc(static_cast<std::size_t>(fs), 0.3f);
    s.process(z.data(), z.size());
    s.process(dc.data(), dc.size());
    CHECK(s.detectedCode() == 0 && !s.gateOpen());
}

void testNoFalseDecode() {
    // 10 minutes of each of: white noise, discriminator-shaped noise with DC, speech-like audio after a 300 Hz
    // high-pass and raw. Locks are counted: ZERO required for all four.
    const double fs = 48000.0;
    const std::size_t n600 = static_cast<std::size_t>(600.0 * fs);
    DcsDecoder d(fs);
    const int w = lockEdges(d, whiteNoise(n600, 0.1, 21), fs);
    d.reset();
    std::vector<float> dn = discriminatorNoise(n600, 0.1, 22);
    for (auto& x : dn) { x += 0.2f; }
    const int dd = lockEdges(d, dn, fs);
    d.reset();
    const int sp = lockEdges(d, speechLike(n600, fs, 0.5, 23), fs);
    d.reset();
    cascade::dsp::CtcssHighpass hp(fs);
    std::vector<float> s2 = speechLike(n600, fs, 0.5, 24), s2f(n600);
    hp.process(s2.data(), s2f.data(), n600);
    const int spf = lockEdges(d, s2f, fs);
    std::printf("  dcs false locks in 10 min: white %d, discriminator+DC %d, raw speech %d, speech after 300 Hz HP %d\n", w, dd, sp, spf);
    CHECK(w == 0);
    CHECK(dd == 0);
    CHECK(sp == 0);
    CHECK(spf == 0);
    // A CTCSS tone alone (a repeater's other squelch) is not a DCS code.
    {
        std::vector<float> t = discriminatorNoise(static_cast<std::size_t>(30 * fs), 0.02, 25);
        addTone(t, fs, 100.0, 0.1, 0, t.size());
        DcsDecoder e(fs);
        CHECK(lockEdges(e, t, fs) == 0);
    }
}

void testEdges() {
    DcsDecoder d;
    CHECK(!d.configure(1000.0));
    CHECK(!d.configure(std::numeric_limits<double>::quiet_NaN()));
    float x[8] = {};
    d.process(x, 8);
    CHECK(d.detectedCode() == 0 && !d.gateOpen());
    CHECK(d.configure(48000.0));
    CHECK(d.configured());
    CHECK(!d.setSelected(024));      // not a standard code
    CHECK(!d.setSelected(0777));
    CHECK(d.setSelected(023));
    CHECK(d.setSelected(0));         // none
    d.process(x, 0);                 // empty
    // Non-finite input does not poison the decoder.
    std::vector<float> junk(4800, std::numeric_limits<float>::quiet_NaN());
    junk[5] = std::numeric_limits<float>::infinity();
    d.process(junk.data(), junk.size());
    std::vector<float> v = dcsAudio(cascade::dsp::dcs::encode(0131), 48000.0, 3.0, 0.1);
    d.process(v.data(), v.size());
    CHECK(d.isDetected(0131, Pol::Normal));
    // Chunk size does not change the result.
    DcsDecoder a(48000.0), b(48000.0);
    a.process(v.data(), v.size());
    for (std::size_t i = 0; i < v.size(); ++i) { b.process(v.data() + i, 1); }
    CHECK(a.detectedCode() == b.detectedCode() && a.isDetected(0131, Pol::Normal));
    // Other rates configure and decode.
    for (double r : {8000.0, 16000.0, 44100.0, 96000.0}) {
        DcsDecoder e(r);
        CHECK(e.configured());
        std::vector<float> s = dcsAudio(cascade::dsp::dcs::encode(0343), r, 3.0, 0.1);
        e.process(s.data(), s.size());
        CHECK(e.isDetected(0343, Pol::Normal));
    }
    // reset() forgets the lock but keeps the selection.
    a.setSelected(0131);
    a.reset();
    CHECK(a.detectedCode() == 0 && !a.gateOpen());
    a.process(v.data(), v.size());
    CHECK(a.gateOpen());
    // No allocation in process().
    DcsDecoder f(48000.0);
    AllocProbe probe;
    for (int i = 0; i < 5; ++i) { f.process(v.data(), v.size()); }
    CHECK(probe.news() == 0);
}

}  // namespace

int main() {
    testTable();
    testGolay();
    testAlignmentCollisionProperty();
    testInverseEquivalence();
    testRoundTripAllCodes();
    testWithNoiseSpeechAndOffsets();
    testBitErrors();
    testReleaseAndSilence();
    testNoFalseDecode();
    testEdges();
    return testSummary("test_dcs");
}
