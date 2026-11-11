// Bit-view library: the user-described decoder (bitspec.hpp) and the OOK slicer (ookslicer.hpp).
//
// Parser: every refusal asserted WITH ITS LINE NUMBER, the resource limits, and a good spec. Interpreter:
// a pulse-width device and a Manchester-inside-Manchester device are generated with jitter, clock error
// and noise, and decoded through the slicer and the spec; the decode rate is pinned, a damaged
// checksum is refused (never mis-decoded), every field type is read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/bits/bitspec.hpp"

#include <cstdio>
#include <string>

#include "bits_test_util.hpp"
#include "test_check.hpp"

using namespace cascade::bits;
using bitstest::Rng;

namespace {

const char* kPwmSpec =
    "foxsdr-bitspec 1\n"
    "name test-pwm\n"
    "coding pwm\n"
    "short_us 250\n"
    "long_us 500\n"
    "tolerance 30\n"
    "reset_us 6000\n"
    "invert no\n"
    "preamble 1010 1010\n"
    "min_bits 40\n"
    "max_bits 200\n"
    "field id at 0 8 uint\n"
    "field temp at 8 12 int scale 0.1 unit C\n"
    "field battery at 20 1 flag\n"
    "field count at 24 8 uint min 0 max 250\n"
    "field tag at 32 8 hex\n"
    "field raw at 0 4 bits\n"
    "check crc from 0 to 40 at 40 16 poly 0x1021 init 0xFFFF xorout 0 width 16\n";

std::vector<LevelRun> pwmRuns(const Bits& bits, double clockErr, double jitterFrac, Rng& rng, double gapUs = 12000) {
    std::vector<LevelRun> r;
    for (uint8_t b : bits) {
        const double mark = (b ? 500.0 : 250.0) * (1.0 + clockErr) * (1.0 + jitterFrac * rng.sym());
        const double space = 300.0 * (1.0 + clockErr) * (1.0 + jitterFrac * rng.sym());
        r.push_back({1, mark});
        r.push_back({0, space});
    }
    r.push_back({1, 250.0});
    r.push_back({0, gapUs});
    return r;
}

Bits pwmFrame(uint8_t id, int tempTenths, bool batt, uint8_t count, uint8_t tag) {
    Bits f = fromString("1010 1010");  // preamble
    Bits body;
    auto put = [&](uint64_t v, unsigned n) { for (int i = static_cast<int>(n) - 1; i >= 0; --i) body.push_back(static_cast<uint8_t>((v >> i) & 1u)); };
    put(id, 8);
    put(static_cast<uint64_t>(tempTenths) & 0xFFF, 12);
    put(batt ? 1 : 0, 1);
    put(0, 3);
    put(count, 8);
    put(tag, 8);
    CrcModel m{16, 0x1021, 0xFFFF, 0, false, false, ""};
    const uint64_t c = crcCompute(m, body);
    put(c, 16);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

// ---- Manchester inside Manchester (a device that sends each bit twice) -----------------------------------------
Bits oregonLikeBits(const std::vector<uint8_t>& nibbles /* values, sent LSB first */) {
    Bits payload = fromString("1111 1111 1111 1111 0101");  // preamble then the sync nibble
    for (uint8_t n : nibbles)
        for (int i = 0; i < 4; ++i) payload.push_back(static_cast<uint8_t>((n >> i) & 1u));  // LSB first
    return payload;
}
std::vector<LevelRun> chipRuns(const Bits& chips, double T, double clockErr, double jitter, Rng& rng, double biasUs) {
    auto runs = bitstest::chipsToRuns(chips, T, clockErr, jitter, rng);
    for (auto& r : runs) r.duration += r.level ? -biasUs : biasUs;  // the slicer's mark/space bias
    return runs;
}

}  // namespace

int main() {
    Rng rng(0x5BEC);

    // ---- a good spec parses, with every statement read ---------------------------------------------------------------
    const SpecParse good = parseSpec(kPwmSpec);
    CHECK(good.ok && good.errors.empty());
    CHECK(good.spec.name == "test-pwm" && good.spec.coding == SpecCoding::Pwm && good.spec.shortUs == 250 && good.spec.longUs == 500);
    CHECK(good.spec.fields.size() == 6 && good.spec.fields[1].type == SpecType::Int && good.spec.fields[1].scale == 0.1 && good.spec.fields[1].unit == "C");
    CHECK(good.spec.check.kind == SpecCheckKind::Crc && good.spec.check.crc.poly == 0x1021 && good.spec.check.crc.init == 0xFFFF);
    CHECK(good.spec.preamble == fromString("10101010"));

    // ---- refusals WITH LINE NUMBERS ---------------------------------------------------------------------------------
    struct Bad { const char* text; int line; const char* part; };
    const Bad bad[] = {
        {"", 0, "empty"},
        {"hello\n", 1, "first statement"},
        {"foxsdr-bitspec 2\n", 1, "unsupported spec version"},
        {"foxsdr-bitspec\n", 1, "version number"},
        {"foxsdr-bitspec 1\nwobble 3\n", 2, "unknown statement"},
        {"foxsdr-bitspec 1\ncoding chips klingon\n", 2, "unknown line code"},
        {"foxsdr-bitspec 1\ncoding fm\n", 2, "expected"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us -5\n", 3, "outside"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us abc\n", 3, "not a number"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 100 200\n", 3, "takes 1"},
        {"foxsdr-bitspec 1\ncoding chips manchester\ntolerance 99\n", 3, "outside"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield a at 0 8 uint\nfield a at 8 8 uint\n", 5, "already defined on line 4"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield a at 0 8 float\n", 4, "unknown field type"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield a at 0 0 uint\n", 4, "field length"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield a at 0 65 uint\n", 4, "field length"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield a at 0 6 bcd\n", 4, "multiple of 4"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield a at 0 8 flag\n", 4, "one bit"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield a at 0 8 uint sparkle\n", 4, "unknown field attribute"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield a at 0 8 uint scale\n", 4, "needs a number"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield bad-name at 0 8 uint\n", 4, "field name"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nfield a 0 8 uint\n", 4, "expected 'field NAME at"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\npreamble 10x1\n", 4, "preamble"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\npreamble_errors 99\n", 4, "outside"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\ncheck crc from 0 to 8 at 8 16 poly 0x1021\n", 4, "needs 'width'"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\ncheck crc from 0 to 8 at 8 8 poly 0x1021 width 16\n", 4, "must be equal"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\ncheck wobble from 0 to 8 at 8 8\n", 4, "unknown check kind"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\ncheck sum8 from 0 to 8 at 8 8\ncheck xor8 from 0 to 8 at 8 8\n", 5, "only one"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\ncheck nibblesum from 0 to 8 at 8 6\n", 4, "multiple of 4"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nmin_bits 100\nmax_bits 50\n", 0, "min_bits is larger"},
        {"foxsdr-bitspec 1\ncoding chips manchester\n", 0, "bit_us"},
        {"foxsdr-bitspec 1\ncoding pwm\nshort_us 100\n", 0, "short_us' and 'long_us"},
        {"foxsdr-bitspec 1\ncoding pwm\nshort_us 500\nlong_us 250\n", 0, "smaller"},
        {"foxsdr-bitspec 1\nname x\n", 0, "no 'coding'"},
        {"foxsdr-bitspec 1\ncoding pwm\nshort_us 100\nlong_us 200\nstage manchester\n", 0, "needs 'coding chips'"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nstage manchester\nstage manchester\nstage manchester\n", 6, "at most two"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nframe_bits 40\nfield late at 36 8 uint\n", 5, "beyond the frame"},
        {"foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nname has space\n", 4, "takes 1"},
    };
    for (const auto& b : bad) {
        const SpecParse p = parseSpec(b.text);
        bool found = false;
        for (const auto& e : p.errors) found |= e.line == b.line && e.message.find(b.part) != std::string::npos;
        if (!found) {
            std::printf("  refusal not as expected for spec:\n%s  wanted line %d containing '%s'; got:", b.text, b.line, b.part);
            for (const auto& e : p.errors) std::printf(" [%d: %s]", e.line, e.message.c_str());
            std::printf("\n");
        }
        CHECK(!p.ok);
        CHECK(found);
    }
    // comments and blank lines do not move the line numbers
    {
        const SpecParse p = parseSpec("# a comment\n\nfoxsdr-bitspec 1   # version\ncoding pwm\n\n  wobble\n");
        bool at6 = false;
        for (const auto& e : p.errors) at6 |= e.line == 6 && e.message.find("unknown statement") != std::string::npos;
        CHECK(at6);
    }
    // resource limits
    {
        CHECK(!parseSpec(std::string(17 * 1024, ' ')).ok);
        std::string many = "foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\n";
        for (int i = 0; i < 40; ++i) many += "field f" + std::to_string(i) + " at " + std::to_string(i) + " 1 flag\n";
        const SpecParse p = parseSpec(many);
        bool tooMany = false;
        for (const auto& e : p.errors) tooMany |= e.message.find("more than 32 fields") != std::string::npos;
        CHECK(!p.ok && tooMany);
        std::string longLines = "foxsdr-bitspec 1\n";
        for (int i = 0; i < 250; ++i) longLines += "# x\n";
        bool lines = false;
        for (const auto& e : parseSpec(longLines).errors) lines |= e.message.find("200 lines") != std::string::npos;
        CHECK(lines);
        bool longLine = false;
        for (const auto& e : parseSpec("foxsdr-bitspec 1\n" + std::string(300, 'x') + "\n").errors) longLine |= e.message.find("200 characters") != std::string::npos;
        CHECK(longLine);
        CHECK(parseSpec("foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\npreamble " + std::string(65, '1') + "\n").errors.size() >= 1);
        CHECK(parseSpec("foxsdr-bitspec 1\ncoding chips manchester\nbit_us 976\nmax_bits 9000\n").errors.size() >= 1);
        // not more than 32 errors are collected
        std::string noise = "foxsdr-bitspec 1\n";
        for (int i = 0; i < 100; ++i) noise += "bogus\n";
        CHECK(parseSpec(noise).errors.size() <= 32);
    }

    // ---- the pulse-width device, through jitter and clock error: the decode rate is pinned ------------------------------
    {
        const BitSpec& sp = good.spec;
        auto trial = [&](double clockErr, double jitter, int n, int* accepted, int* wrong) {
            int ok = 0;
            *accepted = 0;
            *wrong = 0;
            for (int t = 0; t < n; ++t) {
                const uint8_t id = rng.byte(), count = static_cast<uint8_t>(rng.below(250)), tag = rng.byte();
                const int temp = static_cast<int>(rng.below(1200)) - 400;
                const bool batt = rng.below(2) != 0;
                const Bits frame = pwmFrame(id, temp, batt, count, tag);
                auto runs = pwmRuns(frame, clockErr, jitter, rng);
                // three repeats like a real sensor
                std::vector<LevelRun> all(runs);
                all.insert(all.end(), runs.begin(), runs.end());
                all.insert(all.end(), runs.begin(), runs.end());
                const auto msgs = runSpec(sp, all);
                for (const auto& m : msgs) {
                    ++*accepted;
                    const auto* vid = findValue(m, "id");
                    const auto* vt = findValue(m, "temp");
                    const auto* vb = findValue(m, "battery");
                    const auto* vc = findValue(m, "count");
                    const bool right = vid && vid->raw == id && vt && std::fabs(vt->value - temp / 10.0) < 1e-9 && vb && vb->raw == (batt ? 1u : 0u) && vc && vc->raw == count;
                    if (!right) ++*wrong;
                    CHECK(m.check == SpecCheckResult::Ok);
                }
                ok += msgs.size() == 3 ? 1 : 0;
            }
            return ok;
        };
        int acc = 0, wrong = 0;
        int full = trial(0.0, 0.0, 60, &acc, &wrong);
        CHECK(full == 60 && acc == 180 && wrong == 0);
        full = trial(0.08, 0.15, 150, &acc, &wrong);
        std::printf("  pwm device, clock +8%%, width jitter +-15%%: all three repeats decoded in %d/150 sets (%d accepted, %d wrong)\n", full, acc, wrong);
        CHECK(full == 150 && wrong == 0);
        full = trial(-0.1, 0.25, 150, &acc, &wrong);
        std::printf("  pwm device, clock -10%%, width jitter +-25%%: all three repeats decoded in %d/150 sets (%d accepted, %d wrong)\n", full, acc, wrong);
        CHECK(wrong == 0);   // the CRC refuses the damaged ones; nothing is mis-decoded
        CHECK(full > 0 && full < 150);  // beyond the 30 % class tolerance most frames are refused (measured 16/150)
        full = trial(0.0, 0.9, 100, &acc, &wrong);
        std::printf("  pwm device, width jitter +-90%% (hopeless): %d/100 sets complete, %d accepted, %d wrong\n", full, acc, wrong);
        CHECK(wrong == 0 && acc < 150);
    }

    // ---- refusals on the interpreter side -------------------------------------------------------------------------------------
    {
        const BitSpec& sp = good.spec;
        Rng r2(11);
        Bits frame = pwmFrame(0x42, 215, true, 17, 0xA5);
        // 1. damaged check: refused and counted
        Bits dam = frame;
        dam[8 + 10] ^= 1u;
        SpecStats st;
        CHECK(runSpec(sp, pwmRuns(dam, 0, 0, r2), &st).empty() && st.badCheck == 1);
        // 2. wrong preamble
        Bits wp = frame;
        wp[2] ^= 1u;
        wp[5] ^= 1u;
        CHECK(runSpec(sp, pwmRuns(wp, 0, 0, r2), &st).empty() && st.noPreamble == 1);
        // 3. truncated: fewer than min_bits
        const Bits cut(frame.begin(), frame.begin() + 30);
        CHECK(runSpec(sp, pwmRuns(cut, 0, 0, r2), &st).empty() && st.tooShort >= 1);
        // 4. empty and degenerate input
        CHECK(runSpec(sp, {}).empty());
        CHECK(runSpec(sp, {{1, 100}}).empty());
        CHECK(runSpec(BitSpec{}, pwmRuns(frame, 0, 0, r2)).empty());  // version 0: a spec that was never parsed
        // 5. widths nowhere near either class count as violations; over the allowance the message is refused
        std::vector<LevelRun> weird;
        for (int i = 0; i < 60; ++i) { weird.push_back({1, 1000.0}); weird.push_back({0, 300.0}); }
        weird.push_back({0, 20000});
        CHECK(runSpec(sp, weird, &st).empty() && st.tooManyViolations == 1);
        // 6. a good frame is found, accepted, and the field text is right
        const auto ok = runSpec(sp, pwmRuns(frame, 0, 0, r2));
        CHECK(ok.size() == 1);
        if (!ok.empty()) {
            CHECK(findValue(ok[0], "id")->raw == 0x42);
            CHECK(findValue(ok[0], "temp")->text == "21.5 C");
            CHECK(findValue(ok[0], "battery")->text == "1");
            CHECK(findValue(ok[0], "tag")->text == "0xA5");
            CHECK(findValue(ok[0], "raw")->text == "0100");
            CHECK(findValue(ok[0], "nosuch") == nullptr);
            CHECK(ok[0].plausible);
        }
        // 7. negative temperature (12-bit two's complement) and a value over max marks the message implausible
        const auto neg = runSpec(sp, pwmRuns(pwmFrame(1, -123, false, 251, 0), 0, 0, r2));
        CHECK(neg.size() == 1 && !neg.empty() && findValue(neg[0], "temp")->value == -12.3 && !neg[0].plausible && !findValue(neg[0], "count")->valid);
        // 8. two messages in one run list, started where they start
        std::vector<LevelRun> two = pwmRuns(frame, 0, 0, r2, 20000);
        const auto second = pwmRuns(pwmFrame(0x43, 100, false, 1, 1), 0, 0, r2);
        two.insert(two.end(), second.begin(), second.end());
        const auto m2 = runSpec(sp, two);
        CHECK(m2.size() == 2 && m2[1].startUs > m2[0].startUs + 20000);
        // 9. maxMessages bounds the output
        CHECK(runSpec(sp, two, nullptr, 1).size() == 1);
    }

    // ---- ppm, invert, frame_bits, negative positions, sum8/xor8/nibblesum checks ----------------------------------------------------
    {
        const char* ppmSpec =
            "foxsdr-bitspec 1\ncoding ppm\nshort_us 400\nlong_us 800\nreset_us 5000\ninvert yes\nframe_bits 24\n"
            "field a at 0 8 uint\nfield b at -16 8 uint\ncheck sum8 from 0 to 16 at 16 8\n";
        const SpecParse p = parseSpec(ppmSpec);
        CHECK(p.ok);
        Rng r3(5);
        auto frameRuns = [&](const Bits& bits) {
            std::vector<LevelRun> r;
            for (uint8_t b : bits) { r.push_back({1, 200.0}); r.push_back({0, (b ? 800.0 : 400.0) * (1.0 + 0.1 * r3.sym())}); }
            r.push_back({1, 200.0});  // the closing mark, then the gap
            r.push_back({0, 9000});
            return r;
        };
        // invert yes: the wire carries long = 0
        auto sendBits = [&](uint8_t a, uint8_t b) {
            Bits f;
            for (int i = 7; i >= 0; --i) f.push_back(static_cast<uint8_t>((a >> i) & 1u));
            for (int i = 7; i >= 0; --i) f.push_back(static_cast<uint8_t>((b >> i) & 1u));
            const uint8_t s = static_cast<uint8_t>(a + b);
            for (int i = 7; i >= 0; --i) f.push_back(static_cast<uint8_t>((s >> i) & 1u));
            return inverted(f);
        };
        const auto m = runSpec(p.spec, frameRuns(sendBits(0x12, 0x34)));
        CHECK(m.size() == 1 && !m.empty() && findValue(m[0], "a")->raw == 0x12 && findValue(m[0], "b")->raw == 0x34 && m[0].frame.size() == 24);
        // a wrong sum is refused
        Bits badSum = sendBits(0x12, 0x34);
        badSum[20] ^= 1u;
        CHECK(runSpec(p.spec, frameRuns(badSum)).empty());
        // xor8 and nibblesum on a chips device
        const SpecParse px = parseSpec("foxsdr-bitspec 1\ncoding chips nrzl\nbit_us 100\nframe_bits 24\nreset_us 1000\ninvert no\ncheck xor8 from 0 to 16 at 16 8\n");
        CHECK(px.ok);
        Bits xf = fromBytes({0xA5, 0x0F, static_cast<uint8_t>(0xA5 ^ 0x0F)});
        // NRZ-L chips at 100 us, framed by low gaps
        std::vector<LevelRun> xr;
        xr.push_back({0, 3000});
        xr.push_back({1, 100});  // a leading 1 chip that is part of the data: bit 0 of 0xA5 is 1
        size_t i = 1;
        uint8_t lvl = 1;
        size_t runLen = 1;
        for (; i < xf.size(); ++i) {
            if (xf[i] == lvl) ++runLen;
            else { xr.back().duration = runLen * 100.0; xr.push_back({static_cast<uint8_t>(xf[i]), 100.0}); lvl = xf[i]; runLen = 1; }
        }
        xr.back().duration = runLen * 100.0;
        xr.push_back({0, 3000});
        const auto mx = runSpec(px.spec, xr);
        CHECK(mx.size() == 1 && !mx.empty() && mx[0].check == SpecCheckResult::Ok);
    }

    // ---- Manchester inside Manchester with the slicer's bias, jitter and clock error ---------------------------------------------------------------
    {
        const char* spec =
            "foxsdr-bitspec 1\nname twice\ncoding chips manchester\nstage manchester\nbit_us 976\ntolerance 30\nreset_us 5000\ninvert auto\n"
            "preamble 1111 1111 1111 0101\npreamble_errors 1\nmin_bits 60\nmax_bits 200\n"
            "field type at 0 16 hex nibble_lsb\nfield channel at 16 4 uint nibble_lsb\nfield id at 20 8 uint nibble_lsb digits_lsd\n"
            "field temp at 32 12 bcd nibble_lsb digits_lsd scale 0.1 unit C\nfield hum at 48 8 bcd nibble_lsb digits_lsd unit %\n"
            "check nibblesum from 0 to 56 at 56 8 nibble_lsb digits_lsd\n";
        const SpecParse p = parseSpec(spec);
        CHECK(p.ok);
        int ok = 0, wrong = 0;
        const int n = 80;
        for (int t = 0; t < n; ++t) {
            const uint8_t id = rng.byte();
            const int tenths = static_cast<int>(rng.below(999));
            const unsigned hum = rng.below(100);
            std::vector<uint8_t> nib;
            nib.reserve(16);
            for (uint8_t v : {0x1, 0xD, 0x2, 0x0, 0x1}) nib.push_back(v);
            nib.push_back(static_cast<uint8_t>(id & 15));
            nib.push_back(static_cast<uint8_t>(id >> 4));
            nib.push_back(0x0);
            nib.push_back(static_cast<uint8_t>(tenths % 10));
            nib.push_back(static_cast<uint8_t>((tenths / 10) % 10));
            nib.push_back(static_cast<uint8_t>(tenths / 100));
            nib.push_back(0x0);
            nib.push_back(static_cast<uint8_t>(hum % 10));
            nib.push_back(static_cast<uint8_t>(hum / 10));
            unsigned sum = 0;
            for (uint8_t v : nib) sum += v;
            nib.push_back(static_cast<uint8_t>(sum & 15));
            nib.push_back(static_cast<uint8_t>((sum >> 4) & 15));
            // frame: preamble + sync + nibbles, doubled by two Manchester layers
            const Bits bits = oregonLikeBits(nib);
            const Bits l1 = encodeLineCode(bits, LineCode::ManchesterIeee);
            const Bits chips = encodeLineCode(l1, LineCode::ManchesterIeee);
            auto runs = chipRuns(chips, 488.0, (rng.sym()) * 0.02, 0.12, rng, 110.0);
            runs.insert(runs.begin(), LevelRun{0, 20000});
            runs.push_back({0, 20000});
            SpecStats st2;
            const auto msgs = runSpec(p.spec, runs, &st2);
            if (msgs.empty() && t < 3) std::printf("  twice: no message (cand %zu, no preamble %zu, short %zu, violations %zu, bad check %zu)\n", st2.candidates, st2.noPreamble, st2.tooShort, st2.tooManyViolations, st2.badCheck);
            if (msgs.size() == 1) {
                ++ok;
                const auto& m = msgs[0];
                const bool right = findValue(m, "type")->raw == 0x1D20 && findValue(m, "channel")->raw == 1 && findValue(m, "id")->raw == id &&
                                   std::fabs(findValue(m, "temp")->value - tenths / 10.0) < 1e-9 && findValue(m, "hum")->value == hum && m.check == SpecCheckResult::Ok;
                if (!right) ++wrong;
            }
        }
        std::printf("  Manchester x2 device with 110 us bias, +-12%% edge jitter, +-2%% clock: %d/%d decoded, %d wrong\n", ok, n, wrong);
        CHECK(ok >= n * 90 / 100);
        CHECK(wrong == 0);
    }

    // ---- the slicer ---------------------------------------------------------------------------------------------------------------------------------
    {
        // OOK burst: 20 ms of noise, then marks and spaces at a known pattern, then noise
        const double fs = 250000.0;
        std::vector<float> mag;
        auto noise = [&](size_t n) { for (size_t i = 0; i < n; ++i) mag.push_back(static_cast<float>(10.0 + 3.0 * rng.sym())); };
        noise(5000);
        const int widths[] = {400, 800, 400, 400, 1200, 800, 400, 400, 800};  // microseconds
        bool mark = true;
        for (int w : widths) {
            const size_t n = static_cast<size_t>(w * fs / 1e6);
            for (size_t i = 0; i < n; ++i) mag.push_back(static_cast<float>(mark ? 100.0 + 8.0 * rng.sym() : 10.0 + 3.0 * rng.sym()));
            mark = !mark;
        }
        noise(5000);
        SliceParams sp;
        sp.sampleRate = fs;
        const SliceResult r = sliceEnvelope(mag, sp);
        CHECK(r.signal && r.runs.size() >= 9);
        // find the first mark run and compare the next runs to the pattern, within a sample or two plus the smoothing
        size_t first = 0;
        while (first < r.runs.size() && r.runs[first].level == 0) ++first;
        bool good = first + 9 <= r.runs.size();
        for (size_t k = 1; k < 8 && good; ++k) good &= std::fabs(r.runs[first + k].duration - widths[k]) < 60.0;
        CHECK(good);
        // pure noise has no signal
        std::vector<float> flat;
        for (int i = 0; i < 20000; ++i) flat.push_back(static_cast<float>(10.0 + 3.0 * rng.sym()));
        CHECK(!sliceEnvelope(flat, sp).signal);
        CHECK(!sliceEnvelope({}, sp).signal);
        CHECK(!sliceEnvelope({1, 2, 3}, sp).signal);
        SliceParams bad;
        bad.sampleRate = 0;
        CHECK(!sliceEnvelope(mag, bad).signal);
        SliceParams capped = sp;
        capped.maxRuns = 3;
        CHECK(sliceEnvelope(mag, capped).runs.size() <= 3);
        // balanceRuns: marks 100 us short and spaces 100 us long for one chip, restored
        std::vector<LevelRun> biased;
        for (int i = 0; i < 30; ++i) { biased.push_back({1, 288.0 + (i % 3 == 0 ? 488.0 : 0.0)}); biased.push_back({0, 588.0 + (i % 5 == 0 ? 488.0 : 0.0)}); }
        double b = 0;
        const auto fixed = balanceRuns(biased, &b);
        CHECK(std::fabs(b - 150.0) < 1.0);
        CHECK(std::fabs(fixed[0].duration - 926.0) < 1.0 && std::fabs(fixed[1].duration - 1026.0 + 150.0 + 0.0) < 800.0);
        double b0 = 1;
        CHECK(balanceRuns({{1, 10}, {0, 10}}, &b0).size() == 2 && b0 == 0.0);
        // despeckle: a one-sample dropout inside a mark is absorbed
        std::vector<LevelRun> sp1 = {{0, 1000}, {1, 400}, {0, 4}, {1, 396}, {0, 1000}};
        const auto ds = despeckle(sp1, 10.0);
        CHECK(ds.size() == 3 && std::fabs(ds[1].duration - 800.0) < 1e-9);
        // iqMagnitude
        CHECK(iqMagnitude({3, 4, 0, 1}).size() == 2 && std::fabs(iqMagnitude({3, 4})[0] - 5.0f) < 1e-6f);
    }
    return testSummary("test_bits_spec");
}
