// Bit-view library on REAL recordings: the Oregon Scientific weather-sensor captures in
// D:/foxsdr-samples/weather-decoder (433.92 MHz OOK, I/Q as unsigned 8-bit pairs, 250 kS/s assumed by
// the sample folder), run through the OOK slicer and a spec written for ONE known protocol, and compared
// with the reference decodes recorded beside each file (expected.json: what an independent decoder printed
// for the same file; its source was not read).
//
// The spec was derived from the recording itself with this library: the slicer shows 488 us chips, the
// chip stream is Manchester inside Manchester (a device that sends every bit twice), the payload is
// nibbles sent least significant bit first, and a nibble-sum check closes the frame.
//
// Skipped (counted as skipped, not passed) when the sample folder is absent: the files are local data
// that is not committed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/bits/bitspec.hpp"
#include "test_check.hpp"

using namespace cascade::bits;

namespace {

const char* kRoot = "D:/foxsdr-samples/weather-decoder/";

bool readFile(const std::string& path, std::string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

size_t countLines(const std::string& s) {
    size_t n = 0;
    std::stringstream ss(s);
    std::string l;
    while (std::getline(ss, l))
        if (l.find("\"model\"") != std::string::npos) ++n;
    return n;
}

std::vector<LevelRun> sliceFile(const std::string& cu8, double fs) {
    std::vector<float> mag(cu8.size() / 2);
    for (size_t i = 0; i < mag.size(); ++i) {
        const float I = static_cast<unsigned char>(cu8[2 * i]) - 127.5f, Q = static_cast<unsigned char>(cu8[2 * i + 1]) - 127.5f;
        mag[i] = std::sqrt(I * I + Q * Q);
    }
    SliceParams p;
    p.sampleRate = fs;
    return sliceEnvelope(mag, p).runs;
}

// the spec for the THGR122N layout (positions are bits after the sync nibble)
const char* kSpec =
    "foxsdr-bitspec 1\n"
    "name oregon-v21-thgr122n\n"
    "coding chips manchester\n"
    "stage manchester\n"
    "bit_us 976\n"
    "tolerance 30\n"
    "reset_us 5000\n"
    "invert auto\n"
    "preamble 1111 1111 1111 0101\n"
    "preamble_errors 1\n"
    "min_bits 68\n"
    "max_bits 240\n"
    "max_violations 3\n"
    "field sensor_type at 0 16 hex nibble_lsb\n"
    "field channel at 16 4 uint nibble_lsb\n"
    "field rolling_id at 20 8 uint nibble_lsb digits_lsd\n"
    "field flags at 28 4 hex nibble_lsb\n"
    "field temperature at 32 12 bcd nibble_lsb digits_lsd scale 0.1 unit C\n"
    "field temp_sign at 44 4 hex nibble_lsb\n"
    "field humidity at 48 8 bcd nibble_lsb digits_lsd unit %\n"
    "check nibblesum from 0 to 60 at 60 8 nibble_lsb digits_lsd\n";

// The same device without the field layout, for the other models in the folder: counts messages that have
// the preamble and a clean line code.
const char* kSpecBare =
    "foxsdr-bitspec 1\nname oregon-any\ncoding chips manchester\nstage manchester\nbit_us 976\ntolerance 30\nreset_us 5000\ninvert auto\n"
    "preamble 1111 1111 1111 0101\npreamble_errors 1\nmin_bits 60\nmax_bits 300\nmax_violations 3\n";

// scan: does some nibble position k >= 8 hold the nibble sum of everything before it?
bool hasNibbleSumCheck(const Bits& frame) {
    const size_t nn = frame.size() / 4;
    auto nib = [&](size_t i) { Bits n = slice(frame, i * 4, 4); std::reverse(n.begin(), n.end()); return static_cast<unsigned>(getField(n, 0, 4, true)); };
    unsigned sum = 0;
    for (size_t k = 0; k + 2 <= nn; ++k) {
        if (k >= 8) {
            const unsigned stored = nib(k) | (nib(k + 1) << 4);
            if ((sum & 0xFF) == stored) return true;
        }
        sum += nib(k);
    }
    return false;
}

}  // namespace

int main() {
    std::string cu8;
    const std::string first = std::string(kRoot) + "os21-thgr122n-188-54/THGR122N_188_54_ch1.cu8";
    if (!readFile(first, &cu8)) {
        ++g_checksSkipped;
        std::printf("SKIP: %s not found (local sample data)\n", first.c_str());
        return testSummary("test_bits_real");
    }
    std::string expected;
    CHECK(readFile(std::string(kRoot) + "os21-thgr122n-188-54/expected.json", &expected));
    const size_t expectedCount = countLines(expected);
    CHECK(expectedCount == 2);

    const SpecParse sp = parseSpec(kSpec);
    CHECK(sp.ok);
    const auto runs = sliceFile(cu8, 250000.0);
    CHECK(runs.size() > 100);
    SpecStats st;
    const auto msgs = runSpec(sp.spec, runs, &st);
    std::printf("  THGR122N stats: candidates %zu, no preamble %zu, too short %zu, violations %zu, bad check %zu", st.candidates, st.noPreamble, st.tooShort, st.tooManyViolations, st.badCheck);
    std::putchar(10);
    std::printf("  THGR122N recording: spec decoded %zu message(s), reference decode lists %zu\n", msgs.size(), expectedCount);
    CHECK(msgs.size() == expectedCount);
    // the reference values for both packets: id 248, channel 1, 18.8 C, 54 %, checksum good
    for (const auto& m : msgs) {
        const auto* ch = findValue(m, "channel");
        const auto* id = findValue(m, "rolling_id");
        const auto* t = findValue(m, "temperature");
        const auto* h = findValue(m, "humidity");
        const auto* ty = findValue(m, "sensor_type");
        CHECK(ch && ch->raw == 1);
        CHECK(id && id->raw == 248);
        CHECK(t && std::fabs(t->value - 18.8) < 1e-9);
        CHECK(h && h->value == 54);
        CHECK(ty && ty->text == "0x1D20");
        CHECK(m.check == SpecCheckResult::Ok);
        CHECK(m.plausible);
    }
    // the two packets sit where the reference says: 0.057164 s and 0.297504 s (a few ms of slicer start-up slack)
    if (msgs.size() == 2) {
        CHECK(std::fabs(msgs[0].startUs / 1e6 - 0.057164) < 0.004);
        CHECK(std::fabs(msgs[1].startUs / 1e6 - 0.297504) < 0.004);
    }
    // break-it in the data: flip one decoded bit of the first frame and the nibble-sum check refuses it
    if (!msgs.empty()) {
        Bits damaged = msgs[0].frame;
        damaged[35] ^= 1u;
        CHECK(detail::runCheck(sp.spec.check, damaged) == SpecCheckResult::Bad);
        CHECK(detail::runCheck(sp.spec.check, msgs[0].frame) == SpecCheckResult::Ok);
        CHECK(hasNibbleSumCheck(msgs[0].frame));
    }

    // ---- the other recordings in the folder, bare spec: message counts against the reference ----------------------------
    const char* dirs[] = {"os21-bthr968", "os21-thgr122n-m550-30", "os21-thn132n-202", "os3-bthgn129", "os3-rtgn318", "os3-wgr800x"};
    const SpecParse bare = parseSpec(kSpecBare);
    CHECK(bare.ok);
    for (const char* d : dirs) {
        const std::string dir = std::string(kRoot) + d + "/";
        std::string exp, data;
        std::string file;
        // the .cu8 name is not the folder name: find the single .cu8 by trying the manifest's files
        std::ifstream src(dir + "SOURCE.txt");
        std::string line;
        std::string cu8Name;
        while (std::getline(src, line)) {
            const size_t p = line.find("original data: ");
            if (p != std::string::npos) {
                const size_t b = p + 15, e = line.find(" |", b);
                cu8Name = line.substr(b, e == std::string::npos ? std::string::npos : e - b);
            }
        }
        if (cu8Name.empty() || !readFile(dir + cu8Name, &data) || !readFile(dir + "expected.json", &exp)) {
            ++g_checksSkipped;
            std::printf("  SKIP %s: files not found\n", d);
            continue;
        }
        const size_t want = countLines(exp);
        const auto r = runSpec(bare.spec, sliceFile(data, 250000.0));
        size_t withCheck = 0;
        for (const auto& m : r) withCheck += hasNibbleSumCheck(m.frame) ? 1 : 0;
        std::printf("  %-24s reference decodes: %zu, preamble+line-code messages: %zu, of which a nibble-sum check closes the frame: %zu\n", d, want, r.size(), withCheck);
        // the v2.1 folders (names os21-*) must match the reference count; v3 (os3-*) is a different frame (reported, not pinned here)
        if (std::string(d).rfind("os21-", 0) == 0) {
            CHECK(r.size() == want);
            CHECK(withCheck == r.size());
        }
    }
    return testSummary("test_bits_real");
}
