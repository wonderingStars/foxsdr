// test_engine_api_wire_a_jobs.cpp - THE BACKGROUND JOB OF WIRING LANE wire-a, held against the real engine through its API: the lane's one slot (fact.job.*), EXPORT_BOX.
//
//   J1  EXPORT_BOX: a recording made in the test (a tone 5037 Hz above the centre, noise, 48 kS/s float32 I/Q WAV) is exported through the engine: the box 4..6 kHz above the centre comes out at its own rate
//       with the tone at +37 Hz (measured from the output), the job state goes running -> done and the result carries the rate; an existing output file, a box outside the band, an unknown mode, a
//       relative path, bad JSON and a second job while one is out are refused with the right words; JOB_CANCEL with nothing running is NO_CHANGE and with one out is OK.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "wire_a_rig.hpp"

using namespace wire_a_rig;

namespace {

fs::path g_dir;

void le32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) { s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); } }
void le16(std::string& s, std::uint16_t v) { for (int i = 0; i < 2; ++i) { s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); } }

// A float32 two-channel (I, Q) WAV: a tone `toneHz` above the centre at amplitude 0.5, noise 0.01.
void writeRecording(const fs::path& p, double rate, double seconds, double toneHz) {
    const std::size_t n = static_cast<std::size_t>(rate * seconds);
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.01f);
    std::string data;
    data.reserve(n * 8);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * 3.14159265358979323846 * toneHz * static_cast<double>(i) / rate;
        const float re = static_cast<float>(0.5 * std::cos(ph)) + noise(rng), im = static_cast<float>(0.5 * std::sin(ph)) + noise(rng);
        char b[8];
        std::memcpy(b, &re, 4);
        std::memcpy(b + 4, &im, 4);
        data.append(b, 8);
    }
    std::string h = "RIFF";
    le32(h, static_cast<std::uint32_t>(36 + data.size()));
    h += "WAVEfmt ";
    le32(h, 16);
    le16(h, 3);   // IEEE float
    le16(h, 2);
    le32(h, static_cast<std::uint32_t>(rate));
    le32(h, static_cast<std::uint32_t>(rate * 8));
    le16(h, 8);
    le16(h, 32);
    h += "data";
    le32(h, static_cast<std::uint32_t>(data.size()));
    std::ofstream f(p, std::ios::binary);
    f << h << data;
}

// Reads a float32 WAV written by the export: its rate and its frames.
bool readOut(const fs::path& p, int& channels, double& rate, std::vector<float>& samples) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    const std::string s = o.str();
    if (s.size() < 44 || s.compare(0, 4, "RIFF") != 0) { return false; }
    std::uint16_t ch = 0;
    std::uint32_t r = 0;
    std::memcpy(&ch, s.data() + 22, 2);
    std::memcpy(&r, s.data() + 24, 4);
    channels = ch;
    rate = r;
    const std::size_t at = s.find("data");
    if (at == std::string::npos) { return false; }
    std::uint32_t bytes = 0;
    std::memcpy(&bytes, s.data() + at + 4, 4);
    samples.resize(bytes / 4);
    std::memcpy(samples.data(), s.data() + at + 8, samples.size() * 4);
    return true;
}

std::string job(const std::string& in, const std::string& out, const std::string& extra = std::string()) {
    return "{\"input\":\"" + in + "\",\"output\":\"" + out + "\",\"t0\":0.2,\"t1\":0.7,\"loHz\":100004000,\"hiHz\":100006000,\"centerHz\":100000000" + extra + "}";
}

void testExport() {
    std::printf("J1: box export\n");
    const fs::path rec = g_dir / "rec.wav";
    writeRecording(rec, 48000.0, 1.0, 5037.0);
    const std::string in = rec.generic_string();
    Rig r;
    CHECK_EQ(r.fact("job.state"), std::string("idle"));
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_JOB_CANCEL)).status(), FOXAPI_NO_CHANGE);
    const std::string out1 = (g_dir / "box.wav").generic_string();
    const std::string j1 = job(in, out1);
    testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_EXPORT_BOX), &j1);
    CHECK_EQ(o.status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("job.state") == "done" || r.fact("job.state") == "failed"; }, 20000));
    CHECK_EQ(r.fact("job.state"), std::string("done"));
    CHECK_EQ(r.fact("job.name"), std::string("box export"));
    CHECK(r.fact("job.result").find("\"outRateHz\"") != std::string::npos);
    int ch = 0;
    double rate = 0.0;
    std::vector<float> s;
    CHECK(readOut(out1, ch, rate, s));
    CHECK_EQ(ch, 2);
    CHECK(rate > 2400.0 && rate < 4000.0);
    const std::size_t frames = s.size() / 2;
    CHECK(frames > static_cast<std::size_t>(0.45 * rate) && frames < static_cast<std::size_t>(0.55 * rate));
    // the tone is at +37 Hz in the output: the phase step between frames, over the middle of the box
    double acc = 0.0;
    std::complex<double> sum(0.0, 0.0);
    for (std::size_t k = frames / 4; k + 1 < 3 * frames / 4; ++k) {
        const std::complex<double> a(s[2 * k], s[2 * k + 1]), b(s[2 * k + 2], s[2 * k + 3]);
        sum += b * std::conj(a);
        acc += std::abs(a);
    }
    const double hz = std::arg(sum) * rate / (2.0 * 3.14159265358979323846);
    std::printf("  the tone lands at %.2f Hz (expect 37), output rate %.1f S/s, %zu frames, mean amplitude %.3f\n", hz, rate, frames, acc / static_cast<double>(frames / 2));
    CHECK(std::fabs(hz - 37.0) < 2.0);
    CHECK(acc / static_cast<double>(frames / 2) > 0.4 && acc / static_cast<double>(frames / 2) < 0.6);
    // refusals of the op itself
    const std::string nope = "{nope";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_EXPORT_BOX), &nope).status(), FOXAPI_BAD_ARGUMENT);
    const std::string rel = job("rec.wav", out1), badMode = job(in, (g_dir / "x.wav").generic_string(), ",\"mode\":\"qam\""), badFmt = job(in, (g_dir / "x.wav").generic_string(), ",\"format\":\"mp3\"");
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_EXPORT_BOX), &rel).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_EXPORT_BOX), &badMode).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_EXPORT_BOX), &badFmt).status(), FOXAPI_BAD_ARGUMENT);
    std::string noNum = "{\"input\":\"" + in + "\",\"output\":\"" + (g_dir / "x.wav").generic_string() + "\",\"t0\":0.2}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_EXPORT_BOX), &noNum).status(), FOXAPI_BAD_ARGUMENT);
    // failures that arrive as a failed job: an existing output, a box outside the band, a missing recording
    auto failsWith = [&](const std::string& body, const char* word) {
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_EXPORT_BOX), &body).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("job.state") != "running"; }, 20000));
        CHECK_EQ(r.fact("job.state"), std::string("failed"));
        CHECK(r.fact("job.message").find(word) != std::string::npos);
        CHECK(r.fact("job.message").find(g_dir.generic_string()) == std::string::npos);   // never a path
    };
    failsWith(j1, "exists already");
    std::string outside = job(in, (g_dir / "o.wav").generic_string());
    outside.replace(outside.find("100004000"), 9, "100500000");
    outside.replace(outside.find("100006000"), 9, "100506000");
    failsWith(outside, "");
    failsWith(job((g_dir / "missing.wav").generic_string(), (g_dir / "m.wav").generic_string()), "could not be opened");
    // a second job while one is out is BUSY: both submitted before any pass
    {
        const std::string a = job(in, (g_dir / "b1.wav").generic_string()), b = job(in, (g_dir / "b2.wav").generic_string());
        FoxCommand c1 = testing::cmd(FOXAPP_OP_EXPORT_BOX), c2 = testing::cmd(FOXAPP_OP_EXPORT_BOX), c3 = testing::cmd(FOXAPP_OP_JOB_CANCEL);
        FoxSubmitResult sr1{}, sr2{}, sr3{};
        sr1.structSize = sr2.structSize = sr3.structSize = sizeof(sr1);
        CHECK(api()->submit_text(r.local->raw(), &c1, a.data(), static_cast<uint32_t>(a.size()), &sr1) >= 0);
        CHECK(api()->submit_text(r.local->raw(), &c2, b.data(), static_cast<uint32_t>(b.size()), &sr2) >= 0);
        CHECK(api()->submit(r.local->raw(), &c3, 1, &sr3) >= 0);
        int st1 = -99, st2 = -99, st3 = -99;
        for (int i = 0; i < 20 && (st1 == -99 || st2 == -99 || st3 == -99); ++i) {
            r.eng.controlPass();
            FoxCommandResult res[8];
            for (auto& x : res) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(r.local->raw(), res, 8);
            for (int32_t k = 0; k < got; ++k) {
                if (res[k].ticket == sr1.ticket) { st1 = res[k].status; }
                if (res[k].ticket == sr2.ticket) { st2 = res[k].status; }
                if (res[k].ticket == sr3.ticket) { st3 = res[k].status; }
            }
        }
        CHECK_EQ(st1, FOXAPI_OK);
        CHECK_EQ(st2, FOXAPI_BUSY);
        CHECK_EQ(st3, FOXAPI_OK);   // the cancel reaches the job that is out
        CHECK(r.until([&] { return r.fact("job.state") != "running"; }, 20000));
        const std::string st = r.fact("job.state");
        CHECK(st == "done" || st == "cancelled");
        CHECK(!fs::exists(g_dir / "b2.wav"));
    }
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_jobs");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_jobs_" + std::to_string(::_getpid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testExport();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_jobs");
}
