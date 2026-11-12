// test_engine_api_wire_a_recordings.cpp - THE RECORDINGS BROWSER OF WIRING LANE wire-a, held against the real engine through its API on a folder of generated recordings.
//
//   B1  SCAN, LIST, OPEN: a folder with two I/Q WAVs (float32 and 16-bit), a mono audio WAV, a damaged .wav, a text file and a sub-folder holding a third I/Q WAV is scanned on the model's worker (state scanning ->
//       done; the damaged one is counted broken, the text file ignored); the list is sorted by size and by name, filtered by name, by kind and without the broken one, and paged; a file is described by its NAME only
//       (no path in any fact); an I/Q WAV opens as the radio's source (its own sample rate arrives); an audio file, a broken one, a scan id that is not the current one and an id past the end are refused; a
//       shallow scan leaves out the sub-folder; a missing folder is reported without its path; relative path, bad sort, bad limit, bad JSON refused.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "wire_a_rig.hpp"

using namespace wire_a_rig;

namespace {

fs::path g_dir;

void le32(std::string& s, std::uint32_t v) { for (int i = 0; i < 4; ++i) { s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); } }
void le16(std::string& s, std::uint16_t v) { for (int i = 0; i < 2; ++i) { s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); } }

// A WAV of `seconds` of a tone: `channels` 1 or 2, 16-bit integer or 32-bit float.
void writeWav(const fs::path& p, std::uint32_t rate, double seconds, int channels, bool floatFmt) {
    const std::size_t n = static_cast<std::size_t>(rate * seconds);
    std::string data;
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(i) / rate;
        for (int c = 0; c < channels; ++c) {
            const double v = 0.4 * (c == 0 ? std::cos(ph) : std::sin(ph));
            if (floatFmt) {
                const float f = static_cast<float>(v);
                char b[4];
                std::memcpy(b, &f, 4);
                data.append(b, 4);
            } else {
                le16(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(v * 32767.0)));
            }
        }
    }
    const std::uint16_t bits = floatFmt ? 32 : 16;
    std::string h = "RIFF";
    le32(h, static_cast<std::uint32_t>(36 + data.size()));
    h += "WAVEfmt ";
    le32(h, 16);
    le16(h, floatFmt ? 3 : 1);
    le16(h, static_cast<std::uint16_t>(channels));
    le32(h, rate);
    le32(h, rate * channels * bits / 8);
    le16(h, static_cast<std::uint16_t>(channels * bits / 8));
    le16(h, bits);
    h += "data";
    le32(h, static_cast<std::uint32_t>(data.size()));
    std::ofstream f(p, std::ios::binary);
    f << h << data;
}

bool waitDone(Rig& r) { return r.until([&] { return r.fact("recordings.state") == "done"; }, 20000); }

std::string listJson(Rig& r, const std::string& body, int* matched = nullptr) {
    const testing::Outcome o = r.send(testing::cmd(FOXAPP_OP_RECORDINGS_LIST), &body);
    if (matched != nullptr) { *matched = static_cast<int>(o.result.applied[0]); }
    if (o.status() != FOXAPI_OK) { return std::string("refused:") + std::to_string(o.status()); }
    return r.fact("recordings.list");
}

void testBrowser() {
    std::printf("B1: recordings browser\n");
    const fs::path rec = g_dir / "recs";
    fs::create_directories(rec / "sub");
    writeWav(rec / "iq_a_20260101_120000.wav", 48000, 1.0, 2, true);
    writeWav(rec / "iq_b.wav", 48000, 3.0, 2, false);
    writeWav(rec / "audio_c.wav", 16000, 2.0, 1, false);
    writeWav(rec / "sub" / "iq_d.wav", 48000, 0.5, 2, true);
    { std::ofstream f(rec / "junk.wav", std::ios::binary); f << "this is not a wav file at all"; }
    { std::ofstream f(rec / "notes.txt"); f << "notes"; }
    Rig r;
    CHECK_EQ(r.fact("recordings.state"), std::string("idle"));
    const std::string emptyObj = "{}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORDINGS_LIST), &emptyObj).status(), FOXAPI_NO_CHANGE);
    const std::string scan = "{\"folder\":\"" + rec.generic_string() + "\"}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORDINGS_SCAN), &scan).status(), FOXAPI_OK);
    CHECK(waitDone(r));
    CHECK_EQ(r.fact("recordings.count"), std::string("5"));
    CHECK_EQ(r.fact("recordings.broken"), std::string("1"));
    CHECK_EQ(r.fact("recordings.truncated"), std::string("0"));
    int n = 0;
    std::string l = listJson(r, R"({"sort":"size","ascending":false})", &n);
    CHECK_EQ(n, 5);
    CHECK(l.find("\"name\":\"iq_b.wav\"") < l.find("\"name\":\"iq_a_20260101_120000.wav\""));   // the 3 s 16-bit file is the biggest
    CHECK(l.find(g_dir.generic_string()) == std::string::npos);   // names only: no path anywhere
    CHECK(l.find("\"kind\":\"audio\"") != std::string::npos && l.find("\"kind\":\"iq\"") != std::string::npos);
    CHECK(l.find("\"rateHz\":48000.0") != std::string::npos);
    l = listJson(r, R"({"sort":"name","ascending":true,"text":"iq"})", &n);
    CHECK_EQ(n, 3);
    CHECK(l.find("audio_c") == std::string::npos);
    l = listJson(r, R"({"iq":false})", &n);
    CHECK_EQ(n, 2);   // the audio file and the damaged one (which is guessed Audio)
    l = listJson(r, R"({"hideBroken":true})", &n);
    CHECK_EQ(n, 4);
    l = listJson(r, R"({"sort":"name","limit":2,"offset":1})", &n);
    CHECK_EQ(n, 5);   // matched, of which two are listed
    {
        const nlohmann::json j = nlohmann::json::parse(l);
        CHECK_EQ(j.size(), std::size_t(2));
    }
    // refusals of the list
    CHECK(listJson(r, R"({"sort":"colour"})").find("refused:") == 0u);
    CHECK(listJson(r, R"({"limit":0})").find("refused:") == 0u);
    CHECK(listJson(r, R"({"limit":500})").find("refused:") == 0u);
    CHECK(listJson(r, "{nope").find("refused:") == 0u);
    // open: find the id of the float32 I/Q file
    l = listJson(r, R"({"text":"iq_a"})");
    const nlohmann::json one = nlohmann::json::parse(l);
    CHECK_EQ(one.size(), std::size_t(1));
    const long long id = one[0]["id"].get<long long>(), scanId = one[0]["scanId"].get<long long>();
    FoxCommand open = testing::cmd(FOXAPP_OP_RECORDINGS_OPEN, 0.0, id);
    open.ival[1] = scanId;
    CHECK_EQ(r.send(open).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.1.source") == "file" && r.local->state().sampleRateHz == 48000.0; }, 8000));
    // the others are refused
    {
        const nlohmann::json a = nlohmann::json::parse(listJson(r, R"({"text":"audio_c"})")), bk = nlohmann::json::parse(listJson(r, R"({"text":"junk"})"));
        FoxCommand oa = testing::cmd(FOXAPP_OP_RECORDINGS_OPEN, 0.0, a[0]["id"].get<long long>());
        oa.ival[1] = scanId;
        CHECK_EQ(r.send(oa).status(), FOXAPI_UNSUPPORTED);
        FoxCommand ob = testing::cmd(FOXAPP_OP_RECORDINGS_OPEN, 0.0, bk[0]["id"].get<long long>());
        ob.ival[1] = scanId;
        CHECK_EQ(r.send(ob).status(), FOXAPI_UNSUPPORTED);
        FoxCommand wrongScan = testing::cmd(FOXAPP_OP_RECORDINGS_OPEN, 0.0, id);
        wrongScan.ival[1] = scanId + 7;
        CHECK_EQ(r.send(wrongScan).status(), FOXAPI_NOT_FOUND);
        FoxCommand past = testing::cmd(FOXAPP_OP_RECORDINGS_OPEN, 0.0, 99);
        past.ival[1] = scanId;
        CHECK_EQ(r.send(past).status(), FOXAPI_NOT_FOUND);
    }
    // a shallow scan leaves the sub-folder out
    const std::string shallow = "{\"folder\":\"" + rec.generic_string() + "\",\"maxDepth\":0}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORDINGS_SCAN), &shallow).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("recordings.state") == "done" && r.fact("recordings.count") == "4"; }, 20000));
    // a missing folder is reported without its path; refusals of the scan
    const std::string missing = "{\"folder\":\"" + (g_dir / "nowhere").generic_string() + "\"}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORDINGS_SCAN), &missing).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("recordings.folderError") != "" && r.fact("recordings.folderError") != "<absent>"; }, 20000));
    CHECK(r.fact("recordings.folderError").find(g_dir.generic_string()) == std::string::npos);
    const std::string rel = "{\"folder\":\"relative\"}", badDepth = "{\"folder\":\"" + rec.generic_string() + "\",\"maxDepth\":9}", badMax = "{\"folder\":\"" + rec.generic_string() + "\",\"maxEntries\":0}";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORDINGS_SCAN), &rel).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORDINGS_SCAN), &badDepth).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORDINGS_SCAN), &badMax).status(), FOXAPI_BAD_ARGUMENT);
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_recordings");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_recordings_" + std::to_string(::_getpid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testBrowser();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_recordings");
}
