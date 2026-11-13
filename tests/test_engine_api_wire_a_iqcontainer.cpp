// test_engine_api_wire_a_iqcontainer.cpp - RECORD_IQ_CONTAINER 0x8829 THROUGH THE ENGINE (wire-a second pass, item 4): baseband I/Q takes in RF64 and in SigMF.
//   A  the default is the recorder's own WAV, unchanged, and it ENDS at its size ceiling (lowered here to 1 MiB);  B  RF64: the same ceiling does not end the take, the header is promoted to RF64 with the true
//   sizes, and the take plays back through the I/Q file source;  C  SigMF: a .sigmf-data of raw frames and a .sigmf-meta with the rate, the centre and a capture segment for the retune made during the take;
//   D  the record lock lets the centre change during a SigMF take and refuses it during a WAV take;  E  the setting: facts, NO_CHANGE, OUT_OF_RANGE, the file, a new engine reads it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#define _getpid getpid
#endif

#include "check.hpp"
#include "core/recorder.hpp"
#include "source/iq_file_source.hpp"
#include "wire_a_rig.hpp"

using namespace wire_a_rig;
using nlohmann::json;

namespace {

fs::path g_dir;

std::vector<fs::path> filesUnder(const fs::path& dir, const std::string& ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) { return out; }
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ext) { out.push_back(e.path()); }
    }
    return out;
}

std::string head(const fs::path& p, std::size_t n) {
    std::ifstream f(p, std::ios::binary);
    std::string s(n, '\0');
    f.read(s.data(), static_cast<std::streamsize>(n));
    s.resize(static_cast<std::size_t>(f.gcount()));
    return s;
}

std::uint32_t le32(const std::string& s, std::size_t at) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[at])) | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 2])) << 16) | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[at + 3])) << 24);
}
std::uint64_t le64(const std::string& s, std::size_t at) { return static_cast<std::uint64_t>(le32(s, at)) | (static_cast<std::uint64_t>(le32(s, at + 4)) << 32); }

FoxCommand setContainer(std::int64_t c) { return testing::cmd(FOXAPP_OP_RECORD_IQ_CONTAINER, 0.0, c); }

// One I/Q take of `ms` milliseconds: start, wait, stop, and wait for the file to be finished.
void takeFor(Rig& r, int ms) {
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
    r.settle(ms);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
    r.settle(900);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wire_a_iqcontainer\n");
    engine_api_test::hermetic("wire_a_iqcontainer");
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_iqc_" + std::to_string(_getpid()));
    { std::error_code ec; fs::remove_all(g_dir, ec); fs::create_directories(g_dir, ec); }

    std::printf("A + B: the WAV take ends at its ceiling, the RF64 take does not\n");
    {
        const fs::path rec = g_dir / "recAB", user = g_dir / "userAB";
        fs::create_directories(rec);
        cascade::core::Recorder::setRf64PlainLimitForTest(1u << 20);
        Rig r(user, rec);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.iqcontainer"), std::string("wav"));
        Access::iqWavLimit(r.eng, 1u << 20);
        takeFor(r, 1200);
        std::vector<fs::path> w = filesUnder(rec, ".wav");
        CHECK_EQ(w.size(), static_cast<std::size_t>(1));
        if (!w.empty()) {
            const std::string h = head(w[0], 44);
            CHECK(h.compare(0, 4, "RIFF") == 0 && h.compare(36, 4, "data") == 0);
            const std::uint64_t sz = fs::file_size(w[0]);
            std::printf("  WAV: %llu bytes (the ceiling was 1 MiB)\n", static_cast<unsigned long long>(sz));
            CHECK(sz <= (1u << 20) + 44u);   // the take ended at the ceiling
            CHECK_EQ(le32(h, 40), static_cast<std::uint32_t>(sz - 44));
        }
        // RF64: the same 1 MiB limit, now the point at which the header is promoted: the take goes on
        CHECK_EQ(r.send(setContainer(1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(setContainer(1)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.fact("record.iqcontainer"), std::string("rf64"));
        takeFor(r, 1200);
        w = filesUnder(rec, ".wav");
        CHECK_EQ(w.size(), static_cast<std::size_t>(2));
        fs::path rf;
        for (const fs::path& p : w) { if (head(p, 4) == "RF64") { rf = p; } }
        CHECK(!rf.empty());
        if (!rf.empty()) {
            const std::string h = head(rf, 80);
            const std::uint64_t sz = fs::file_size(rf);
            const std::uint64_t dataBytes = le64(h, 20 + 8);   // ds64: riff size, data size, sample count
            const std::uint64_t frames = le64(h, 20 + 16);
            std::printf("  RF64: %llu bytes, ds64 data size %llu, %llu frames\n", static_cast<unsigned long long>(sz), static_cast<unsigned long long>(dataBytes), static_cast<unsigned long long>(frames));
            CHECK(sz > (1u << 20) + 80u);   // well past the ceiling the WAV take stopped at
            CHECK_EQ(h.compare(12, 4, "ds64"), 0);
            CHECK_EQ(dataBytes, sz - 80);
            CHECK_EQ(frames * 8u, dataBytes);
            // and it plays back through the I/Q file source as it was recorded
            cascade::source::IqFileSource src;
            const bool opened = src.open(rf.string());
            if (!opened) { std::printf("  IqFileSource: %s\n", src.lastError()); }
            CHECK(opened);
            if (opened) {
                CHECK_EQ(src.totalFrames(), frames);
                CHECK(src.start());
                std::vector<std::complex<float>> b(4096);
                CHECK_EQ(src.read(b.data(), b.size()), b.size());
                double e = 0.0;
                for (const auto& s : b) { e += static_cast<double>(std::norm(s)); }
                CHECK(e > 1e-6);
            }
        }
        // 16-bit float of the same: the sample-format setting reaches the container
        CHECK_EQ(r.send(setContainer(0)).status(), FOXAPI_OK);
    }
    cascade::core::Recorder::setRf64PlainLimitForTest(0);

    std::printf("C + D: SigMF logs the retune, and the lock allows it\n");
    {
        const fs::path rec = g_dir / "recC", user = g_dir / "userC";
        fs::create_directories(rec);
        Rig r(user, rec);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 1)).status(), FOXAPI_OK);
        // the lock refuses a centre change during a WAV take ...
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
        r.settle(500);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 101.0e6, 0)).status(), FOXAPI_DENIED);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
        r.settle(900);
        // ... and lets it through during a SigMF take
        CHECK_EQ(r.send(setContainer(2)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.iqcontainer"), std::string("sigmf"));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
        r.settle(700);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 101.0e6, 0)).status(), FOXAPI_OK);
        r.settle(700);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 102.5e6, 0)).status(), FOXAPI_OK);
        r.settle(500);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
        r.settle(900);
        const std::vector<fs::path> data = filesUnder(rec, ".sigmf-data"), meta = filesUnder(rec, ".sigmf-meta");
        CHECK_EQ(data.size(), static_cast<std::size_t>(1));
        CHECK_EQ(meta.size(), static_cast<std::size_t>(1));
        if (!data.empty() && !meta.empty()) {
            std::ifstream mf(meta[0]);
            const json m = json::parse(std::string((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>()), nullptr, false);
            CHECK(!m.is_discarded());
            if (!m.is_discarded()) {
                const std::uint64_t bytes = fs::file_size(data[0]);
                std::printf("  data %llu bytes (%llu frames); datatype %s, rate %.0f\n", static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(bytes / 8), m["global"].value("core:datatype", "").c_str(), m["global"].value("core:sample_rate", 0.0));
                CHECK_EQ(m["global"].value("core:datatype", std::string()), std::string("cf32_le"));
                CHECK(m["global"].value("core:sample_rate", 0.0) > 1000.0);
                CHECK_EQ(bytes % 8u, static_cast<std::uint64_t>(0));
                CHECK(bytes > 8u * 1000u);
                const json& caps = m["captures"];
                CHECK(caps.is_array() && caps.size() >= 3);
                if (caps.is_array() && caps.size() >= 3) {
                    std::printf("  captures: %.0f Hz at %llu, %.0f Hz at %llu, %.0f Hz at %llu\n", caps[0].value("core:frequency", 0.0), static_cast<unsigned long long>(caps[0].value("core:sample_start", 0ull)),
                                caps[1].value("core:frequency", 0.0), static_cast<unsigned long long>(caps[1].value("core:sample_start", 0ull)), caps[2].value("core:frequency", 0.0), static_cast<unsigned long long>(caps[2].value("core:sample_start", 0ull)));
                    CHECK_EQ(caps[0].value("core:frequency", 0.0), 100.0e6);
                    CHECK_EQ(caps[1].value("core:frequency", 0.0), 101.0e6);
                    CHECK_EQ(caps[2].value("core:frequency", 0.0), 102.5e6);
                    CHECK(caps[1].value("core:sample_start", 0ull) > caps[0].value("core:sample_start", 0ull));
                    CHECK(caps[2].value("core:sample_start", 0ull) > caps[1].value("core:sample_start", 0ull));
                    CHECK(caps[2].value("core:sample_start", 0ull) < bytes / 8);
                }
            }
        }
    }

    std::printf("E: the setting\n");
    {
        const fs::path user = g_dir / "userE";
        {
            Rig r(user);
            CHECK_EQ(r.send(setContainer(3)).status(), FOXAPI_OUT_OF_RANGE);
            CHECK_EQ(r.send(setContainer(-1)).status(), FOXAPI_OUT_OF_RANGE);
            CHECK_EQ(r.fact("record.iqcontainer"), std::string("wav"));
            CHECK_EQ(r.send(setContainer(0)).status(), FOXAPI_NO_CHANGE);
            CHECK_EQ(r.send(setContainer(2)).status(), FOXAPI_OK);
            CHECK(r.until([&] {
                std::ifstream f(user / "record-iq-container.json");
                std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                return t.find("\"container\":2") != std::string::npos;
            }, 6000));
        }
        {
            Rig r(user);
            r.settle(300);
            CHECK_EQ(r.fact("record.iqcontainer"), std::string("sigmf"));   // a new engine reads it
        }
    }
    { std::error_code ec; fs::remove_all(g_dir, ec); }
    return check::finish("test_engine_api_wire_a_iqcontainer");
}
