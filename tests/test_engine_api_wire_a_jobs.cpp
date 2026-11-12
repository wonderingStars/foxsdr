// test_engine_api_wire_a_jobs.cpp - THE BACKGROUND JOB OF WIRING LANE wire-a, held against the real engine through its API: the lane's one slot (fact.job.*), EXPORT_BOX.
//
//   J1  EXPORT_BOX: a recording made in the test (a tone 5037 Hz above the centre, noise, 48 kS/s float32 I/Q WAV) is exported through the engine: the box 4..6 kHz above the centre comes out at its own rate
//       with the tone at +37 Hz (measured from the output), the job state goes running -> done and the result carries the rate; an existing output file, a box outside the band, an unknown mode, a
//       relative path, bad JSON and a second job while one is out are refused with the right words; JOB_CANCEL with nothing running is NO_CHANGE and with one out is OK.
//
//   J2  RECORD FILES, all through the same job slot: PROJECT hash (the SHA-256 of a file of three bytes is the published vector for "abc") and verify (match, a flipped byte, a missing file, a damaged
//       project); ATTACH (a note, a PNG and a clip are added, a program renamed .png is refused, list, edit, verify, remove, the manifest is the folder's own file); SIGMF_ANNOTATE (two annotations added with
//       their uuids and a box rounded to samples, listed, one removed, a bad box and a bad index refused, the metadata's other keys untouched).
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

#include "core/analyser_project.hpp"
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

// Runs one record op as a job and waits for it: returns the job's state; the message and result are read after.
std::string runJob(Rig& r, std::uint32_t op, const std::string& body) {
    FoxCommand c = testing::cmd(op);
    const testing::Outcome o = r.send(c, &body);
    if (o.status() != FOXAPI_OK) { return "refused:" + std::to_string(o.status()); }
    r.until([&] { return r.fact("job.state") != "running"; }, 20000);
    return r.fact("job.state");
}

void spit(const fs::path& p, const std::string& s, bool binary = true) {
    std::ofstream f(p, binary ? std::ios::binary : std::ios::out);
    f << s;
}

void testRecords() {
    std::printf("J2: project, attachments, SigMF annotations\n");
    Rig r;
    // ---- PROJECT hash / verify
    const fs::path abc = g_dir / "abc.bin";
    spit(abc, "abc");
    CHECK_EQ(runJob(r, FOXAPP_OP_PROJECT, "{\"action\":\"hash\",\"recording\":\"" + abc.generic_string() + "\"}"), std::string("done"));
    CHECK(r.fact("job.result").find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != std::string::npos);
    CHECK(r.fact("job.result").find("\"sizeBytes\":3") != std::string::npos);
    CHECK_EQ(runJob(r, FOXAPP_OP_PROJECT, "{\"action\":\"hash\",\"recording\":\"" + (g_dir / "none.bin").generic_string() + "\"}"), std::string("failed"));
    CHECK_EQ(runJob(r, FOXAPP_OP_PROJECT, "{\"action\":\"nope\"}"), std::string("refused:") + std::to_string(FOXAPI_BAD_ARGUMENT));
    CHECK_EQ(runJob(r, FOXAPP_OP_PROJECT, "{\"action\":\"hash\",\"recording\":\"rel.bin\"}"), std::string("refused:") + std::to_string(FOXAPI_BAD_ARGUMENT));
    {
        namespace an = cascade::core::analyser;
        an::Project p;
        p.recording.path = abc.string();
        p.recording.sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        p.recording.sizeBytes = 3;
        p.recording.sampleRateHz = 48000.0;
        p.recording.centerHz = 1.0e8;
        p.recording.durationSec = 1.0;
        std::string err;
        const fs::path proj = g_dir / "t.fxproj";
        CHECK(an::saveFile(proj, p, &err));
        const std::string pj = proj.generic_string();
        CHECK_EQ(runJob(r, FOXAPP_OP_PROJECT, "{\"action\":\"verify\",\"project\":\"" + pj + "\"}"), std::string("done"));
        CHECK(r.fact("job.result").find("\"state\":\"match\"") != std::string::npos);
        spit(abc, "abd");   // one byte differs, the size is the same
        CHECK_EQ(runJob(r, FOXAPP_OP_PROJECT, "{\"action\":\"verify\",\"project\":\"" + pj + "\"}"), std::string("done"));
        CHECK(r.fact("job.result").find("\"state\":\"hashDiffers\"") != std::string::npos);
        const fs::path moved = g_dir / "moved.bin";
        spit(moved, "abc");   // the recording was moved: a path given overrides the project's
        CHECK_EQ(runJob(r, FOXAPP_OP_PROJECT, "{\"action\":\"verify\",\"project\":\"" + pj + "\",\"recording\":\"" + moved.generic_string() + "\"}"), std::string("done"));
        CHECK(r.fact("job.result").find("\"state\":\"match\"") != std::string::npos);
        CHECK_EQ(runJob(r, FOXAPP_OP_PROJECT, "{\"action\":\"verify\",\"project\":\"" + pj + "\",\"recording\":\"" + (g_dir / "gone.bin").generic_string() + "\"}"), std::string("done"));
        CHECK(r.fact("job.result").find("\"state\":\"missing\"") != std::string::npos);
        spit(g_dir / "bad.fxproj", "{\"format\":\"foxsdr-analyser-project\",\"version\":9}");
        CHECK_EQ(runJob(r, FOXAPP_OP_PROJECT, "{\"action\":\"verify\",\"project\":\"" + (g_dir / "bad.fxproj").generic_string() + "\"}"), std::string("failed"));
        CHECK(r.fact("job.message").find(g_dir.generic_string()) == std::string::npos);
    }
    // ---- ATTACH
    const fs::path folder = g_dir / "record1";
    fs::create_directories(folder);
    const std::string fj = folder.generic_string();
    const fs::path png = g_dir / "shot.png";
    spit(png, std::string("\x89PNG\r\n\x1a\n", 8) + std::string(64, 'x'));
    const fs::path fake = g_dir / "evil.png";
    spit(fake, std::string("MZ") + std::string(64, 'x'));
    const fs::path clip = g_dir / "rec.wav";   // written by J1 (a WAV)
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + fj + "\",\"action\":\"add\",\"kind\":\"note\",\"title\":\"First\",\"text\":\"heard at 18:00\"}"), std::string("done"));
    CHECK(r.fact("job.result").find("heard at 18:00") != std::string::npos);
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + fj + "\",\"action\":\"add\",\"kind\":\"screenshot\",\"path\":\"" + png.generic_string() + "\",\"title\":\"Waterfall\"}"), std::string("done"));
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + fj + "\",\"action\":\"add\",\"kind\":\"screenshot\",\"path\":\"" + fake.generic_string() + "\",\"title\":\"x\"}"), std::string("failed"));
    CHECK(r.fact("job.message").find(g_dir.generic_string()) == std::string::npos);
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + fj + "\",\"action\":\"add\",\"kind\":\"clip\",\"path\":\"" + clip.generic_string() + "\",\"title\":\"Audio\"}"), std::string("done"));
    CHECK(fs::exists(folder / "attachments.json"));
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + fj + "\",\"action\":\"verify\"}"), std::string("done"));
    {
        const std::string res = r.fact("job.result");
        std::size_t ok = 0, at = 0;
        while ((at = res.find("\"state\":\"ok\"", at)) != std::string::npos) { ++ok; ++at; }
        CHECK_EQ(ok, std::size_t(2));   // the two files; the note has no file
    }
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + fj + "\",\"action\":\"edit\",\"id\":1,\"title\":\"First, renamed\",\"text\":\"heard at 18:05\"}"), std::string("done"));
    CHECK(r.fact("job.result").find("First, renamed") != std::string::npos);
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + fj + "\",\"action\":\"remove\",\"id\":2}"), std::string("done"));
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + fj + "\",\"action\":\"remove\",\"id\":99}"), std::string("failed"));
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + (g_dir / "nofolder").generic_string() + "\",\"action\":\"list\"}"), std::string("failed"));
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"rel\",\"action\":\"list\"}"), std::string("refused:") + std::to_string(FOXAPI_BAD_ARGUMENT));
    CHECK_EQ(runJob(r, FOXAPP_OP_ATTACH, "{\"folder\":\"" + fj + "\",\"action\":\"add\",\"kind\":\"gif\"}"), std::string("refused:") + std::to_string(FOXAPI_BAD_ARGUMENT));
    // ---- SIGMF_ANNOTATE
    const fs::path meta = g_dir / "cap.sigmf-meta";
    spit(meta, R"({"global":{"core:datatype":"cf32_le","core:sample_rate":48000,"core:version":"1.0.0","x:keep":"me"},"captures":[{"core:sample_start":0,"core:frequency":100000000}],"annotations":[]})");
    const std::string mj = meta.generic_string();
    const std::string a1 = "{\"meta\":\"" + mj + "\",\"action\":\"add\",\"startSec\":0.5,\"endSec\":0.75,\"loHz\":100004000,\"hiHz\":100006000,\"sampleRateHz\":48000,\"totalSamples\":48000,\"label\":\"burst\",\"comment\":\"first\"}";
    const std::string a2 = "{\"meta\":\"" + mj + "\",\"action\":\"add\",\"startSec\":0.1,\"endSec\":0.2,\"loHz\":100001000,\"hiHz\":100002000,\"sampleRateHz\":48000,\"totalSamples\":48000,\"label\":\"early\"}";
    CHECK_EQ(runJob(r, FOXAPP_OP_SIGMF_ANNOTATE, a1), std::string("done"));
    CHECK_EQ(runJob(r, FOXAPP_OP_SIGMF_ANNOTATE, a2), std::string("done"));
    CHECK(r.fact("job.result").find("\"annotations\":2") != std::string::npos);
    {
        std::ifstream f(meta, std::ios::binary);
        std::ostringstream o;
        o << f.rdbuf();
        const std::string t = o.str();
        CHECK(t.find("\"x:keep\"") != std::string::npos);                       // the metadata's other keys are kept
        CHECK(t.find("\"core:sample_start\": 24000") != std::string::npos || t.find("\"core:sample_start\":24000") != std::string::npos);   // 0.5 s at 48 kS/s
        CHECK(t.find("core:uuid") != std::string::npos);
        CHECK(t.find("FoxSDR") != std::string::npos);
        CHECK(t.find("\"core:sample_start\": 4800") < t.find("\"core:sample_start\": 24000") || t.find("\"core:sample_start\":4800") < t.find("\"core:sample_start\":24000"));   // sorted by start
    }
    CHECK_EQ(runJob(r, FOXAPP_OP_SIGMF_ANNOTATE, "{\"meta\":\"" + mj + "\",\"action\":\"remove\",\"index\":0}"), std::string("done"));
    CHECK(r.fact("job.result").find("\"annotations\":1") != std::string::npos);
    CHECK_EQ(runJob(r, FOXAPP_OP_SIGMF_ANNOTATE, "{\"meta\":\"" + mj + "\",\"action\":\"remove\",\"index\":7}"), std::string("failed"));
    const std::string backwards = "{\"meta\":\"" + mj + "\",\"action\":\"add\",\"startSec\":0.9,\"endSec\":0.5,\"loHz\":1,\"hiHz\":2,\"sampleRateHz\":48000}";
    CHECK_EQ(runJob(r, FOXAPP_OP_SIGMF_ANNOTATE, backwards), std::string("failed"));
    const std::string beyond = "{\"meta\":\"" + mj + "\",\"action\":\"add\",\"startSec\":0.5,\"endSec\":9.5,\"loHz\":1,\"hiHz\":2,\"sampleRateHz\":48000,\"totalSamples\":48000}";
    CHECK_EQ(runJob(r, FOXAPP_OP_SIGMF_ANNOTATE, beyond), std::string("failed"));
    CHECK_EQ(runJob(r, FOXAPP_OP_SIGMF_ANNOTATE, "{\"meta\":\"" + (g_dir / "nometa.sigmf-meta").generic_string() + "\",\"action\":\"list\"}"), std::string("failed"));
    CHECK_EQ(runJob(r, FOXAPP_OP_SIGMF_ANNOTATE, "{\"meta\":\"" + mj + "\",\"action\":\"add\"}"), std::string("refused:") + std::to_string(FOXAPI_BAD_ARGUMENT));
    CHECK_EQ(runJob(r, FOXAPP_OP_SIGMF_ANNOTATE, "{\"meta\":\"" + mj + "\",\"action\":\"explode\"}"), std::string("refused:") + std::to_string(FOXAPI_BAD_ARGUMENT));
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
    testRecords();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_jobs");
}
