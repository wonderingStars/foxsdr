// Bit-view library: a fuzz run on every parser and decoder that takes outside bytes - the spec parser, the
// user table parser, the run-list interpreter, the line-code decoders, pattern search and framing, the
// CRC and checksum finders, field inference, the diff, the re-decode chain - plus a differential run of
// the editor against a plain model of itself.
//
// The properties are: nothing crashes or hangs, outputs respect their own bounds, accepted specs run
// over hostile run lists, and the same text always gets the same verdict. Run under an address
// sanitizer in the lane's proof.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <limits>

#include "bits_test_util.hpp"
#include "core/bits/bitedit.hpp"
#include "core/bits/bitspec.hpp"
#include "core/bits/crc.hpp"
#include "core/bits/fields.hpp"
#include "core/bits/msgdiff.hpp"
#include "core/bits/rawbuf.hpp"
#include "core/bits/sync.hpp"
#include "core/bits/whiten.hpp"
#include "test_check.hpp"

using namespace cascade::bits;
using bitstest::Rng;

namespace {

const char* kSeedSpec =
    "foxsdr-bitspec 1\nname fz\ncoding chips manchester\nstage manchester\nbit_us 976\ntolerance 30\nreset_us 5000\ninvert auto\n"
    "preamble 1111 1111 0101\npreamble_errors 1\nmin_bits 16\nmax_bits 240\nmax_violations 5\n"
    "field a at 0 16 hex nibble_lsb\nfield b at 16 8 uint\nfield c at 24 12 bcd nibble_lsb digits_lsd scale 0.1 unit C\nfield d at -8 8 int\n"
    "check nibblesum from 0 to 32 at 32 8 nibble_lsb digits_lsd\n";
const char* kSeedPwm =
    "foxsdr-bitspec 1\ncoding pwm\nshort_us 250\nlong_us 500\ntolerance 30\nreset_us 6000\nframe_bits 24\nfield x at 0 8 uint\ncheck crc from 0 to 16 at 16 8 poly 7 width 8\n";
const char* kSeedTable = "foxsdr-bittable 1\nname t\nbits 5\nplane A\nshift B = 27\n1 = x\n2 = <space>\nplane B\nshift A = 31\n1 = 1\n";

std::string mutate(const std::string& base, Rng& r) {
    std::string s = base;
    const unsigned ops = 1 + r.below(6);
    for (unsigned k = 0; k < ops; ++k) {
        switch (r.below(7)) {
            case 0: if (!s.empty()) s[r.below(static_cast<uint32_t>(s.size()))] = static_cast<char>(r.byte()); break;
            case 1: if (!s.empty()) s.erase(r.below(static_cast<uint32_t>(s.size())), 1 + r.below(8)); break;
            case 2: s.insert(r.below(static_cast<uint32_t>(s.size() + 1)), std::string(1 + r.below(5), static_cast<char>(r.byte()))); break;
            case 3: if (!s.empty()) s.resize(r.below(static_cast<uint32_t>(s.size()))); break;
            case 4: {  // duplicate a stretch
                const size_t a = s.find('\n', r.below(static_cast<uint32_t>(s.size() + 1)));
                if (a != std::string::npos) s.insert(a, s.substr(a / 2, 20));
                break;
            }
            case 5:
                if (!s.empty()) {
                    const size_t p = r.below(static_cast<uint32_t>(s.size()));
                    s.replace(p, 1, r.below(2) ? "999999999999999999999" : "-1");
                }
                break;
            default:
                if (!s.empty()) std::swap(s[r.below(static_cast<uint32_t>(s.size()))], s[r.below(static_cast<uint32_t>(s.size()))]);
                break;
        }
    }
    return s;
}

std::vector<LevelRun> randomRuns(Rng& r, size_t n) {
    std::vector<LevelRun> v;
    for (size_t i = 0; i < n; ++i) {
        LevelRun x;
        x.level = static_cast<uint8_t>(r.below(3));  // includes values over 1
        switch (r.below(10)) {
            case 0: x.duration = 0.0; break;
            case 1: x.duration = -50.0; break;
            case 2: x.duration = std::numeric_limits<double>::infinity(); break;
            case 3: x.duration = std::numeric_limits<double>::quiet_NaN(); break;
            case 4: x.duration = 1e15; break;
            case 5: x.duration = 20000 + r.uni() * 1e5; break;
            default: x.duration = 100 + r.uni() * 1200; break;
        }
        v.push_back(x);
    }
    return v;
}

}  // namespace

int main() {
    Rng rng(0xF022);
    const auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](const char* what) {
        std::printf("  [%6.1f s] %s done", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), what);
        std::putchar(10);
    };

    // ---- the spec parser: mutated and random text ----------------------------------------------------------------------------
    {
        size_t accepted = 0, refused = 0;
        std::vector<BitSpec> good;
        for (int i = 0; i < 8000; ++i) {
            std::string text;
            if (i % 8 == 0) {
                const size_t n = rng.below(400);
                for (size_t k = 0; k < n; ++k) text.push_back(static_cast<char>(rng.byte()));
            } else {
                text = mutate(i % 2 ? kSeedSpec : kSeedPwm, rng);
            }
            const SpecParse p = parseSpec(text);
            CHECK(p.errors.size() <= 32);
            CHECK(p.ok == p.errors.empty());
            if (i % 50 == 0) CHECK(parseSpec(text).ok == p.ok);  // the same text, the same verdict
            for (const auto& e : p.errors) CHECK(e.line >= 0);
            if (p.ok) {
                ++accepted;
                CHECK(p.spec.fields.size() <= 32 && p.spec.maxBits <= 8192 && p.spec.version == 1);
                if (good.size() < 200) good.push_back(p.spec);
            } else {
                ++refused;
            }
        }
        std::printf("  spec parser: %zu accepted, %zu refused of 8000 mutated/random texts\n", accepted, refused);
        CHECK(accepted > 50 && refused > 1000);
        // every accepted spec runs over hostile run lists, and every message respects its own bounds
        for (const auto& sp : good)
            for (int k = 0; k < 6; ++k) {
                const auto runs = randomRuns(rng, 20 + rng.below(300));
                SpecStats st;
                const auto msgs = runSpec(sp, runs, &st);
                CHECK(msgs.size() <= 256);
                for (const auto& m : msgs) {
                    CHECK(m.frame.size() <= sp.maxBits + 1);
                    CHECK(m.values.size() == sp.fields.size());
                    for (const auto& v : m.values) CHECK(!v.name.empty());
                }
            }
    }

    lap("spec parser + interpreter");

    // ---- the user table parser -----------------------------------------------------------------------------------------------
    {
        size_t accepted = 0;
        for (int i = 0; i < 6000; ++i) {
            std::string text;
            if (i % 8 == 0) {
                const size_t n = rng.below(300);
                for (size_t k = 0; k < n; ++k) text.push_back(static_cast<char>(rng.byte()));
            } else {
                text = mutate(kSeedTable, rng);
            }
            const TableParse p = parseTable(text);
            CHECK(p.errors.size() <= 32 && p.ok == p.errors.empty());
            if (p.ok) {
                ++accepted;
                CHECK(p.table.bits >= 1 && p.table.bits <= 8 && p.table.planes.size() <= 4 && p.table.planes.size() == p.table.shifts.size());
                const Bits b = bitstest::randomBits(rng, 200);
                const DecodedText d = decodeText(b, p.table);  // never reads outside the table
                CHECK(d.symbols <= 200);
            }
        }
        std::printf("  table parser: %zu accepted of 6000\n", accepted);
        CHECK(accepted > 50);
    }

    lap("table parser");

    // ---- line codes, whitening, pattern search and framing on random bits and random parameters --------------------------------------------
    {
        for (int i = 0; i < 3000; ++i) {
            const Bits b = bitstest::randomBits(rng, rng.below(300));
            for (size_t c = 0; c < kLineCodeCount; ++c) {
                const Decoded d = decodeLineCode(b, static_cast<LineCode>(c), rng.below(4), rng.below(3));
                CHECK(d.bits.size() <= b.size() && d.violations <= d.units && d.violationAt.size() <= 64);
            }
            const auto hyp = searchLineCodes(b, rng.uni(), 1 + rng.below(20));
            CHECK(hyp.size() <= 20);
            LfsrSpec ls{static_cast<unsigned>(rng.below(70)), rng.next()};
            const uint64_t seed = rng.next();
            const Bits w = whitenBits(b, ls, seed);
            CHECK(w.size() == b.size());
            if (lfsrValid(ls, seed)) CHECK(whitenBits(w, ls, seed) == b);
            ScramblerTaps tp{static_cast<unsigned>(rng.below(70)), static_cast<unsigned>(rng.below(70))};
            CHECK(descramble(b, tp).size() == b.size() && scramble(b, tp).size() == b.size());
            if (tapsValid(tp)) CHECK(descramble(scramble(b, tp), tp) == b);
            Bits pat = bitstest::randomBits(rng, rng.below(20));
            const auto hits = findPattern(b, pat, rng.below(6), rng.below(2) != 0, 1 + rng.below(100));
            CHECK(hits.size() <= 100);
            for (const auto& h : hits) CHECK(h.pos + pat.size() <= b.size() && h.distance <= 5);
            LengthFraming lf;
            lf.lenPos = rng.below(40);
            lf.lenBits = rng.below(40);
            lf.unitBits = rng.below(9);
            lf.adjustBits = static_cast<long>(rng.below(64)) - 32;
            lf.maxBits = rng.below(500);
            size_t rej = 0;
            for (const auto& f : frameByLength(b, hits, pat.size(), lf, &rej)) CHECK(f.bits.size() <= lf.maxBits && f.start + f.bits.size() <= b.size());
            for (const auto& f : frameFixed(b, hits, pat.size(), rng.below(100), rng.below(2) != 0)) CHECK(f.start <= b.size());
            size_t ab = 0;
            for (const auto& f : frameByFlag(b, bitstest::randomBits(rng, 1 + rng.below(9)), rng.below(2) != 0, 1 + rng.below(300), &ab)) CHECK(f.bits.size() <= 300);
            (void)measurePreamble(b, rng.below(400), rng.below(20));
        }
    }

    lap("line codes/whitening/framing");

    // ---- run lists: period fit, chips, widths, slicer, chain ------------------------------------------------------------------------------
    {
        for (int i = 0; i < 1500; ++i) {
            const auto runs = randomRuns(rng, rng.below(200));
            const double T = estimateChipPeriod(runs);
            CHECK(T >= 0.0 || std::isnan(T));
            const Bits chips = runsToChips(runs, rng.uni() * 600 - 50, 1u << 12, rng.below(2) != 0);
            CHECK(chips.size() <= (1u << 12));
            (void)classifyWidths({runs.size() ? runs[0].duration : 1.0, 3.0, 9.0});
            std::vector<PulseRun> pr;
            for (size_t k = 0; k + 1 < runs.size(); k += 2) pr.push_back({runs[k].duration, runs[k + 1].duration});
            const auto pd = decodePulseWidth(pr, rng.below(2) != 0, rng.below(2) != 0, rng.below(2) ? 0.0 : rng.uni() * 1000);
            CHECK(pd.bits.size() == pr.size());
            (void)balanceRuns(runs);
            (void)despeckle(runs, rng.uni() * 300);
            RawSlice s;
            s.runs = runs;
            ChainSettings cs;
            cs.stages.clear();
            const unsigned ns = rng.below(5);
            for (unsigned k = 0; k < ns; ++k) cs.stages.push_back(static_cast<LineCode>(rng.below(kLineCodeCount)));
            cs.phase = static_cast<int>(rng.below(4)) - 1;
            cs.invert = rng.below(2) != 0;
            cs.chipUs = rng.below(2) ? 0.0 : rng.uni() * 1000;
            cs.sync = bitstest::randomBits(rng, rng.below(12));
            cs.syncErrors = rng.below(4);
            cs.framing = static_cast<FramingKind>(rng.below(4));
            cs.fixedBits = rng.below(100);
            const ChainResult r = redecode(s, cs);
            if (r.ok) {
                CHECK(r.bits.size() <= kMaxChips && r.frames.size() <= cs.maxFrames);
                for (const auto& f : r.frames) CHECK(f.bits.size() <= std::max<size_t>(cs.maxFrameBits, r.bits.size() < 8192 ? r.bits.size() : 8192) || cs.framing != FramingKind::None || cs.sync.empty());
            }
        }
        for (int i = 0; i < 100; ++i) {  // the slicer over random envelopes
            std::vector<float> m(rng.below(3000));
            for (auto& x : m) x = static_cast<float>(rng.uni() * (rng.below(2) ? 100 : 1));
            SliceParams sp;
            sp.sampleRate = (rng.below(5) ? 250000.0 : -1.0);
            sp.smoothUs = rng.uni() * 100;
            (void)sliceEnvelope(m, sp);
        }
    }

    lap("run lists");

    // ---- CRC and checksum finders, field inference, diff ------------------------------------------------------------------------------------------
    {
        for (int i = 0; i < 300; ++i) {
            std::vector<std::vector<uint8_t>> frames;
            const size_t nf = rng.below(6);
            const size_t len = rng.below(40);
            for (size_t k = 0; k < nf; ++k) frames.push_back(bitstest::randomBytes(rng, rng.below(3) ? len : rng.below(40)));
            CrcFindOptions o;
            o.widths = {static_cast<unsigned>(8 * (1 + rng.below(9))), 16, static_cast<unsigned>(rng.below(70))};
            o.maxHeadBytes = rng.below(5);
            o.maxTailBytes = rng.below(4);
            o.maxHits = 1 + rng.below(40);
            o.maxCofactorDegree = rng.below(18);
            const auto hits = findCrc(frames, o);
            CHECK(hits.size() <= o.maxHits);
            (void)findChecksums(frames, rng.below(5), rng.below(4));
            std::vector<Bits> bf;
            for (const auto& f : frames) bf.push_back(fromBytes(f));
            InferOptions io;
            io.maxFrames = 1 + rng.below(50);
            io.maxBits = 1 + rng.below(400);
            const auto fs = inferFields(bf, io);
            size_t cursor = 0;
            for (const auto& f : fs) {
                CHECK(f.pos >= cursor && f.len > 0);
                cursor = f.pos + f.len;
            }
            (void)diffFrames(bf, 1 + rng.below(400));
            if (bf.size() >= 2) {
                const BitDiff d = diffBits(bf[0], bf[1], rng.below(2) != 0, 400);
                if (d.ok) CHECK(d.matches + d.mismatches + d.gaps == d.ops.size());
            }
        }
        for (int i = 0; i < 2000; ++i) {  // hostile CRC models through the engine
            CrcModel m{static_cast<unsigned>(rng.below(80)), rng.next(), rng.next(), rng.next(), rng.below(2) != 0, rng.below(2) != 0, ""};
            bool ok = true;
            const uint64_t v = crcCompute(m, bitstest::randomBits(rng, rng.below(100)), &ok);
            if (ok && m.width >= 1 && m.width < 64) CHECK(v <= crcMask(m.width));
        }
    }

    lap("crc/fields/diff");

    // ---- the editor against a plain model of itself (differential) -----------------------------------------------------------------------------------
    {
        BitEditor e(bitstest::randomBits(rng, 64));
        Bits model = e.bits();
        std::vector<Bits> history, future;
        for (int i = 0; i < 4000; ++i) {
            const Bits before = model;
            switch (rng.below(10)) {
                case 0: { const size_t p = rng.below(80); e.flipBit(p); if (p < model.size()) model[p] ^= 1u; break; }
                case 1: { const size_t p = rng.below(80); const unsigned v = rng.below(2); e.setBit(p, v); if (p < model.size()) model[p] = static_cast<uint8_t>(v); break; }
                case 2: {
                    const size_t p = rng.below(80);
                    const Bits x = bitstest::randomBits(rng, rng.below(12));
                    e.insertBits(p, x);
                    if (p <= model.size()) model.insert(model.begin() + static_cast<std::ptrdiff_t>(p), x.begin(), x.end());
                    break;
                }
                case 3: {
                    const size_t p = rng.below(80), n = rng.below(12);
                    e.eraseBits(p, n);
                    if (p <= model.size() && n <= model.size() - p) model.erase(model.begin() + static_cast<std::ptrdiff_t>(p), model.begin() + static_cast<std::ptrdiff_t>(p + n));
                    break;
                }
                case 4: {
                    const size_t p = rng.below(80), n = rng.below(20);
                    e.invertRange(p, n);
                    if (p <= model.size() && n <= model.size() - p) for (size_t k = 0; k < n; ++k) model[p + k] ^= 1u;
                    break;
                }
                case 5: {
                    const size_t p = rng.below(80), n = rng.below(20);
                    e.reverseRange(p, n);
                    if (p <= model.size() && n <= model.size() - p) std::reverse(model.begin() + static_cast<std::ptrdiff_t>(p), model.begin() + static_cast<std::ptrdiff_t>(p + n));
                    break;
                }
                case 6:
                    if (e.undo() && !history.empty()) { future.push_back(model); model = history.back(); history.pop_back(); }
                    CHECK(e.bits() == model);
                    continue;
                case 7:
                    if (e.redo() && !future.empty()) { history.push_back(model); model = future.back(); future.pop_back(); }
                    CHECK(e.bits() == model);
                    continue;
                default: {
                    const size_t p = rng.below(10);
                    e.setByte(p, rng.byte());
                    model = e.bits();  // (the byte write itself is checked in test_bits_rawbuf_edit)
                    break;
                }
            }
            if (model != before) { history.push_back(before); future.clear(); }
            while (history.size() > BitEditor::kMaxEdits) history.erase(history.begin());
            CHECK(e.bits() == model);
        }
    }

    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  fuzz run took %.1f s\n", secs);
    CHECK(secs < 120.0);
    return testSummary("test_bits_fuzz");
}
