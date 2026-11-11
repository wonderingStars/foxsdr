// Tests for the SigMF writer (source/sigmf_writer.hpp), read back through the existing reader (source/iq_readers.hpp) AND checked
// against the metadata text itself (parsed here with a strict JSON parse), so a writer and reader that agree on a wrong key
// would still fail.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/iq_readers.hpp"
#include "source/sigmf_writer.hpp"

#include "test_check.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

namespace iq = cascade::source::iqfile;
namespace fs = std::filesystem;
using Cx = std::complex<float>;
using json = nlohmann::json;

std::string base(const char* name) {
    fs::path d = fs::temp_directory_path() / "coref-sigmf";
    fs::create_directories(d);
    return (d / name).string();
}

void cleanup(const std::string& b) {
    std::error_code ec;
    fs::remove(b + ".sigmf-meta", ec);
    fs::remove(b + ".sigmf-data", ec);
    fs::remove(b + ".sigmf-meta.tmp", ec);
}

std::string slurpText(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<Cx> tone(std::size_t n, float amp = 0.6f) {
    std::vector<Cx> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 0.021 * static_cast<double>(i);
        v[i] = Cx(static_cast<float>(amp * std::cos(ph)), static_cast<float>(amp * std::sin(ph)));
    }
    return v;
}

double maxErr(const std::vector<Cx>& a, const std::vector<Cx>& b) {
    double m = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) { m = std::max({m, static_cast<double>(std::abs(a[i].real() - b[i].real())), static_cast<double>(std::abs(a[i].imag() - b[i].imag()))}); }
    return m;
}

void roundTrip(iq::SigmfType t, const char* name, const char* dt, double tol, std::size_t bytesPerSample) {
    const std::string b = base(name);
    iq::SigmfOptions o;
    o.type = t;
    o.sampleRateHz = 2400000;
    o.hasCentre = true;
    o.centreHz = 144800000;
    o.startUnixNs = 1234567890123000000LL;
    iq::SigmfWriter w;
    std::string err;
    CHECK(w.open(b, o, &err));
    const auto x = tone(40000);
    CHECK(w.write(x.data(), 25000, &err));
    CHECK(w.write(x.data() + 25000, 15000, &err));
    CHECK(w.samplesWritten() == 40000);
    CHECK(w.close(&err));
    CHECK(fs::file_size(b + ".sigmf-data") == 40000 * bytesPerSample);
    // the metadata text
    const json j = json::parse(slurpText(b + ".sigmf-meta"), nullptr, false);
    CHECK(j.is_object());
    CHECK(j["global"]["core:datatype"] == dt);
    CHECK(j["global"]["core:sample_rate"] == 2400000.0);
    CHECK(j["global"]["core:version"].is_string());
    CHECK(j["captures"].size() == 1);
    CHECK(j["captures"][0]["core:sample_start"] == 0);
    CHECK(j["captures"][0]["core:frequency"] == 144800000.0);
    CHECK(j["captures"][0]["core:datetime"] == "2009-02-13T23:31:30.123Z");
    // through the reader, by either file name
    for (const char* ext : {".sigmf-meta", ".sigmf-data"}) {
        auto r = iq::openAuto(b + ext, iq::RawParams{}, &err);
        CHECK(r != nullptr);
        if (!r) { continue; }
        CHECK(r->info().container == "sigmf");
        CHECK(r->info().format == dt);
        CHECK(r->info().sampleRateHz == 2400000.0);
        CHECK(r->info().centerKnown && r->info().centerFrequencyHz == 144800000.0);
        CHECK(r->info().totalSamples == 40000);
        CHECK(r->info().trailingBytes == 0);
        std::vector<Cx> y(40000);
        CHECK(r->read(y.data(), y.size()) == 40000);
        const double e = maxErr(x, y);
        if (std::string(ext) == ".sigmf-meta") { std::printf("  %s round trip max error %.3g\n", dt, e); }
        CHECK(e <= tol);
    }
    CHECK(!fs::exists(b + ".sigmf-meta.tmp"));
    cleanup(b);
}

void testRoundTrips() {
    roundTrip(iq::SigmfType::Cf32Le, "rt_cf32", "cf32_le", 1e-7, 8);
    roundTrip(iq::SigmfType::Ci16Le, "rt_ci16", "ci16_le", 1.0 / 16000.0, 4);
    roundTrip(iq::SigmfType::Ci8, "rt_ci8", "ci8", 0.0125, 2);
    roundTrip(iq::SigmfType::Cu8, "rt_cu8", "cu8", 0.0085, 2);
}

void testDatetime() {
    using iq::sigmfdetail::isoUtc;
    CHECK(isoUtc(0) == "1970-01-01T00:00:00.000Z");
    CHECK(isoUtc(-7) == "1970-01-01T00:00:00.000Z");
    CHECK(isoUtc(1234567890123000000LL) == "2009-02-13T23:31:30.123Z");
    CHECK(isoUtc(951782400LL * 1000000000LL) == "2000-02-29T00:00:00.000Z");
    CHECK(isoUtc(4107542400LL * 1000000000LL) == "2100-03-01T00:00:00.000Z");
    CHECK(isoUtc(4107542399LL * 1000000000LL + 999999999LL) == "2100-02-28T23:59:59.999Z");
    CHECK(isoUtc(1700000000LL * 1000000000LL) == "2023-11-14T22:13:20.000Z");
}

void testCapturesAndAnnotations() {
    const std::string b = base("ann");
    iq::SigmfOptions o;
    o.sampleRateHz = 48000;
    o.hasCentre = true;
    o.centreHz = 100e6;
    iq::SigmfWriter w;
    std::string err;
    CHECK(w.open(b, o, &err));
    const auto x = tone(3000);
    CHECK(w.write(x.data(), 1000, &err));
    CHECK(w.addCapture(101.5e6, 1700000000LL * 1000000000LL, &err));   // retuned after 1000 samples
    CHECK(w.write(x.data() + 1000, 2000, &err));
    iq::SigmfAnnotation a;
    a.sampleStart = 1200;
    a.hasCount = true;
    a.sampleCount = 300;
    a.hasEdges = true;
    a.lowerHz = 101.49e6;
    a.upperHz = 101.51e6;
    a.label = "burst";
    a.comment = "a comment";
    CHECK(w.addAnnotation(a, &err));
    iq::SigmfAnnotation bare;
    bare.sampleStart = 3000;   // exactly the end is allowed
    CHECK(w.addAnnotation(bare, &err));
    CHECK(w.close(&err));
    auto r = iq::openSigmf(b + ".sigmf-meta", 0, &err);
    CHECK(r != nullptr);
    if (r) {
        const auto& in = r->info();
        CHECK(in.captures.size() == 2);
        CHECK(in.captures[0].sampleStart == 0 && in.captures[0].frequencyHz == 100e6 && in.captures[0].datetime.empty());
        CHECK(in.captures[1].sampleStart == 1000 && in.captures[1].frequencyHz == 101.5e6);
        CHECK(in.captures[1].datetime == "2023-11-14T22:13:20.000Z");
        CHECK(in.annotations.size() == 2);
        CHECK(in.annotations[0].sampleStart == 1200 && in.annotations[0].hasCount && in.annotations[0].sampleCount == 300);
        CHECK(in.annotations[0].hasFreqEdges && in.annotations[0].freqLowerHz == 101.49e6 && in.annotations[0].freqUpperHz == 101.51e6);
        CHECK(in.annotations[0].label == "burst");
        CHECK(!in.annotations[1].hasCount && !in.annotations[1].hasFreqEdges);
    }
    const json j = json::parse(slurpText(b + ".sigmf-meta"));
    CHECK(j["annotations"][0]["core:comment"] == "a comment");
    cleanup(b);

    // two retunes with no samples between replace each other instead of writing two segments at one start
    CHECK(w.open(b, o, &err));
    CHECK(w.addCapture(1e6, -1, &err));
    CHECK(w.addCapture(2e6, -1, &err));
    CHECK(w.close(&err));
    const json k = json::parse(slurpText(b + ".sigmf-meta"));
    CHECK(k["captures"].size() == 1 && k["captures"][0]["core:frequency"] == 2e6);
    cleanup(b);
}

void testAnnotationRefusals() {
    const std::string b = base("annbad");
    iq::SigmfOptions o;
    o.sampleRateHz = 1000;
    iq::SigmfWriter w;
    std::string err;
    CHECK(w.open(b, o, &err));
    const auto x = tone(100);
    CHECK(w.write(x.data(), x.size(), &err));
    iq::SigmfAnnotation a;
    a.sampleStart = 101;
    CHECK(!w.addAnnotation(a, &err));                      // after the last sample
    a.sampleStart = 10;
    a.hasEdges = true;
    a.lowerHz = 5;
    a.upperHz = 4;
    CHECK(!w.addAnnotation(a, &err));                      // reversed edges
    a.lowerHz = std::nan("");
    a.upperHz = 4;
    CHECK(!w.addAnnotation(a, &err));
    a.lowerHz = 1;
    a.upperHz = std::numeric_limits<double>::infinity();
    CHECK(!w.addAnnotation(a, &err));
    CHECK(!w.addCapture(std::nan(""), -1, &err));
    CHECK(w.metadata()["annotations"].empty());
    CHECK(w.close(&err));
    cleanup(b);
}

void testHostileStrings() {
    const std::string b = base("hostile");
    iq::SigmfOptions o;
    o.sampleRateHz = 8000;
    o.description = std::string("quote\" backslash\\ newline\n tab\t ctl\x01\x1f nul") + std::string(1, '\0') + "end </script> \xE2\x82\xAC \xF0\x9F\x98\x80";
    o.author = "bad utf8 \xff\xfe\xc3 tail";
    o.recorder = "FoxSDR \"test\"";
    iq::SigmfWriter w;
    std::string err;
    CHECK(w.open(b, o, &err));
    const auto x = tone(10);
    CHECK(w.write(x.data(), x.size(), &err));
    iq::SigmfAnnotation a;
    a.sampleStart = 0;
    a.label = "x\"y\\z\n{\"core:sample_start\":999}";    // an attempt to inject a key through the label
    CHECK(w.addAnnotation(a, &err));
    CHECK(w.close(&err));
    const std::string text = slurpText(b + ".sigmf-meta");
    for (char c : text) { CHECK(static_cast<unsigned char>(c) >= 0x20 || c == '\n'); }   // no raw control byte in the file
    const json j = json::parse(text, nullptr, false);
    CHECK(j.is_object());
    if (j.is_object()) {
        CHECK(j["global"]["core:description"].get<std::string>().find("quote\" backslash\\ newline\n tab\t ctl\x01\x1f nul") == 0);
        CHECK(j["global"]["core:description"].get<std::string>().find('\0') != std::string::npos);   // the NUL survives as \u0000
        CHECK(j["global"]["core:description"].get<std::string>().find("\xF0\x9F\x98\x80") != std::string::npos);
        const std::string au = j["global"]["core:author"].get<std::string>();
        CHECK(au.find("\xEF\xBF\xBD") != std::string::npos);                 // invalid bytes replaced by U+FFFD
        CHECK(au.find('\xff') == std::string::npos);
        CHECK(j["global"]["core:recorder"] == "FoxSDR \"test\"");
        CHECK(j["annotations"].size() == 1);                                 // no injected annotation
        CHECK(j["annotations"][0]["core:sample_start"] == 0);
        CHECK(j["annotations"][0]["core:label"] == "x\"y\\z\n{\"core:sample_start\":999}");
    }
    std::string e2;
    auto r = iq::openSigmf(b + ".sigmf-meta", 0, &e2);
    CHECK(r != nullptr && r->info().annotations.size() == 1);
    if (r && !r->info().annotations.empty()) { CHECK(r->info().annotations[0].label == "x\"y\\z\n{\"core:sample_start\":999}"); }
    cleanup(b);
}

void testCrashSurvival() {
    const std::string b = base("crash");
    iq::SigmfOptions o;
    o.sampleRateHz = 250000;
    o.hasCentre = true;
    o.centreHz = 433.92e6;
    iq::SigmfWriter w;
    std::string err;
    CHECK(w.open(b, o, &err));
    // right after open the pair is already a readable (empty) recording
    fs::copy_file(b + ".sigmf-meta", b + "-c0.sigmf-meta", fs::copy_options::overwrite_existing);
    fs::copy_file(b + ".sigmf-data", b + "-c0.sigmf-data", fs::copy_options::overwrite_existing);
    {
        auto r = iq::openSigmf(b + "-c0.sigmf-meta", 0, &err);
        CHECK(r != nullptr && r->info().totalSamples == 0);
    }
    const auto x = tone(5000);
    CHECK(w.write(x.data(), x.size(), &err));
    iq::SigmfAnnotation a;
    a.sampleStart = 100;
    a.label = "kept";
    CHECK(w.addAnnotation(a, &err));
    CHECK(w.flushMetadata(&err));
    // copy while the writer is still open: this is what a crash would leave
    fs::copy_file(b + ".sigmf-meta", b + "-c1.sigmf-meta", fs::copy_options::overwrite_existing);
    fs::copy_file(b + ".sigmf-data", b + "-c1.sigmf-data", fs::copy_options::overwrite_existing);
    auto r = iq::openSigmf(b + "-c1.sigmf-meta", 0, &err);
    CHECK(r != nullptr);
    if (r) {
        CHECK(r->info().totalSamples == 5000);
        CHECK(r->info().annotations.size() == 1 && r->info().annotations[0].label == "kept");
    }
    CHECK(w.close(&err));
    cleanup(b);
    cleanup(b + "-c0");
    cleanup(b + "-c1");
}

void testConditioningAndRefusals() {
    const std::string b = base("cond");
    cleanup(b);   // a run that failed earlier may have left files
    iq::SigmfWriter w;
    std::string err;
    iq::SigmfOptions o;
    CHECK(!w.open(b, o, &err) && !err.empty());           // rate 0
    o.sampleRateHz = -1;
    CHECK(!w.open(b, o, &err));
    o.sampleRateHz = std::nan("");
    CHECK(!w.open(b, o, &err));
    o.sampleRateHz = std::numeric_limits<double>::infinity();
    CHECK(!w.open(b, o, &err));
    o.sampleRateHz = 1000;
    o.hasCentre = true;
    o.centreHz = std::nan("");
    CHECK(!w.open(b, o, &err));
    o.hasCentre = false;
    CHECK(!w.open("", o, &err));
    CHECK(!w.open((fs::path(b).parent_path() / "no-such-dir" / "x").string(), o, &err));
    CHECK(err.find("cannot create") != std::string::npos);
    Cx s(0, 0);
    CHECK(!w.write(&s, 1, &err));                          // not open
    CHECK(!w.flushMetadata(&err));
    CHECK(!fs::exists(b + ".sigmf-data"));                 // nothing was created by the refused opens
    // clipping and non-finite samples
    o.type = iq::SigmfType::Ci16Le;
    CHECK(w.open(b + ".sigmf-data", o, &err));             // a suffixed name is accepted
    const float inf = std::numeric_limits<float>::infinity();
    const std::vector<Cx> x = {Cx(4.0f, -4.0f), Cx(std::nanf(""), inf), Cx(-inf, 0.5f)};
    CHECK(w.write(x.data(), x.size(), &err));
    CHECK(w.write(nullptr, 0, &err));
    CHECK(w.close(&err));
    CHECK(w.close(&err));
    auto r = iq::openSigmf(b + ".sigmf-meta", 0, &err);
    CHECK(r != nullptr);
    if (r) {
        Cx y[3];
        CHECK(r->read(y, 3) == 3);
        CHECK(std::abs(y[0].real() - 32767.0f / 32768.0f) < 1e-5f && std::abs(y[0].imag() + 32767.0f / 32768.0f) < 1e-5f);
        CHECK(y[1] == Cx(0, 0));
        CHECK(y[2].real() == 0.0f && std::abs(y[2].imag() - 0.5f) < 1e-4f);
    }
    cleanup(b);
    // an empty recording is a valid recording
    CHECK(w.open(b, o, &err));
    CHECK(w.close(&err));
    auto e = iq::openSigmf(b + ".sigmf-meta", 0, &err);
    CHECK(e != nullptr && e->info().totalSamples == 0);
    cleanup(b);
}

}  // namespace

int main() {
    std::printf("sigmf writer\n");
    testRoundTrips();
    testDatetime();
    testCapturesAndAnnotations();
    testAnnotationRefusals();
    testHostileStrings();
    testCrashSurvival();
    testConditioningAndRefusals();
    return testSummary("test_sigmf_writer");
}
