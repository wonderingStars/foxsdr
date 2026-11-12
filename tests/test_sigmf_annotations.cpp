// SigMF annotations (source/sigmf_annotations.hpp): the keys of the SigMF v1.2.6 annotation object, sorting, untouched global and captures, kept extension
// keys, every rule, atomic file edits, the analyser-box conversion and a fuzz run. There is no second SigMF program on this PC: the round trip is against
// this header's own reader and an independent spec checker written in the test.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define SA_PID _getpid()
#else
#include <unistd.h>
#define SA_PID getpid()
#endif

#include "source/sigmf_annotations.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::source::sigmfann;

namespace {

std::string baseMeta(bool withAnnotations) {
    ojson j = ojson::object();
    j["global"] = {{"core:datatype", "cf32_le"}, {"core:sample_rate", 2400000}, {"core:version", "1.2.6"}, {"core:description", "caf\xC3\xA9 test"}, {"my:ext", {{"a", 1}}}};
    j["captures"] = ojson::array({{{"core:sample_start", 0}, {"core:frequency", 7100000}, {"core:datetime", "2026-11-11T12:00:00.000Z"}},
                                   {{"core:sample_start", 1000000}, {"core:frequency", 7200000}}});
    if (withAnnotations) {
        j["annotations"] = ojson::array({{{"core:sample_start", 500}, {"core:sample_count", 100}, {"core:label", "first"}, {"ext:keep", "me"}}});
    }
    return j.dump(2);
}

Annotation box(std::uint64_t start, std::uint64_t count, double lo, double hi, const std::string& label = "L") {
    Annotation a; a.sampleStart = start; a.hasCount = true; a.sampleCount = count; a.hasFreq = true; a.lowHz = lo; a.highHz = hi; a.label = label;
    return a;
}

// an independent reading of the spec's rules over a parsed document
bool specOk(const ojson& doc) {
    if (!doc.contains("global") || !doc["global"].is_object()) { return false; }
    if (!doc.contains("annotations")) { return true; }
    const ojson& arr = doc["annotations"];
    if (!arr.is_array()) { return false; }
    std::uint64_t prev = 0;
    for (const ojson& o : arr) {
        if (!o.is_object() || !o.contains("core:sample_start") || !o["core:sample_start"].is_number_unsigned()) { return false; }
        const std::uint64_t s = o["core:sample_start"].get<std::uint64_t>();
        if (s < prev) { return false; }
        prev = s;
        if (o.contains("core:sample_count") && !o["core:sample_count"].is_number_unsigned()) { return false; }
        if (o.contains("core:freq_lower_edge") != o.contains("core:freq_upper_edge")) { return false; }
        for (const char* k : {"core:label", "core:comment", "core:generator", "core:uuid"}) { if (o.contains(k) && !o[k].is_string()) { return false; } }
    }
    return true;
}

fs::path scratch() {
    fs::path p = fs::current_path() / ("recplay-sigmf-" + std::to_string(SA_PID));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }

void testRoundTrip() {
    std::string meta = baseMeta(true);
    const ojson before = ojson::parse(meta);
    std::vector<Annotation> list;
    std::string err;
    CHECK(readAnnotations(meta, &list, &err) && list.size() == 1);
    CHECK(list[0].sampleStart == 500 && list[0].hasCount && list[0].sampleCount == 100 && list[0].label == "first" && !list[0].hasFreq);
    CHECK(list[0].extra["ext:keep"] == "me");
    // add three out of order, with every key
    Annotation a = box(2000, 300, 7.1e6, 7.1005e6, "beacon");
    a.comment = "keyed at about 18 wpm\nline two, caf\xC3\xA9"; a.generator = "FoxSDR analyser";
    unsigned char rnd[16]; for (int i = 0; i < 16; ++i) { rnd[i] = static_cast<unsigned char>(i * 17 + 3); }
    a.uuid = uuid4FromBytes(rnd);
    Annotation b = box(100, 50, 7.0e6, 7.001e6, "early");
    Annotation c = box(500, 10, 7.2e6, 7.2001e6, "tie");        // equal start to the existing one: keeps its place after it
    list.push_back(a); list.push_back(b); list.push_back(c);
    CHECK(setAnnotations(&meta, list, 3000000, &err));
    const ojson after = ojson::parse(meta);
    CHECK(specOk(after));
    CHECK(after["global"] == before["global"] && after["captures"] == before["captures"]);       // untouched
    CHECK(after["annotations"].size() == 4);
    std::vector<Annotation> back;
    CHECK(readAnnotations(meta, &back, &err) && back.size() == 4);
    CHECK(back[0].label == "early" && back[1].label == "first" && back[2].label == "tie" && back[3].label == "beacon");
    CHECK(back[1].extra["ext:keep"] == "me");                                                    // an extension key survives
    CHECK(back[3].comment == a.comment && back[3].generator == a.generator && back[3].uuid == a.uuid && back[3].hasFreq && back[3].lowHz == 7.1e6 && back[3].highHz == 7.1005e6);
    // a second pass changes nothing
    std::string again = meta;
    CHECK(setAnnotations(&again, back, 3000000, &err) && again == meta);
    // key order of the document is kept; annotations stay last where it was added
    CHECK(meta.find("\"global\"") < meta.find("\"captures\"") && meta.find("\"captures\"") < meta.find("\"annotations\""));
    // a file with no annotations key gets one
    std::string bare = baseMeta(false);
    CHECK(readAnnotations(bare, &list, &err) && list.empty());
    CHECK(setAnnotations(&bare, {box(5, 5, 1, 2)}, 0, &err) && ojson::parse(bare).contains("annotations"));
    // an empty list writes an empty array
    CHECK(setAnnotations(&bare, {}, 0, &err) && ojson::parse(bare)["annotations"].is_array() && ojson::parse(bare)["annotations"].empty());
    // sample_count and frequency edges are optional
    Annotation minimal; minimal.sampleStart = 7;
    std::string m2 = baseMeta(false);
    CHECK(setAnnotations(&m2, {minimal}, 0, &err));
    const ojson o2 = ojson::parse(m2)["annotations"][0];
    CHECK(o2.size() == 1 && o2["core:sample_start"] == 7);
    // a large sample index survives (beyond 2^53)
    Annotation big; big.sampleStart = 18000000000000000000ull;
    std::string m3 = baseMeta(false);
    CHECK(setAnnotations(&m3, {big}, 0, &err));
    std::vector<Annotation> bb; CHECK(readAnnotations(m3, &bb, &err) && bb[0].sampleStart == 18000000000000000000ull);
}

void testRules() {
    std::string err;
    struct Row { const char* what; Annotation a; std::uint64_t total; };
    std::vector<Row> rows;
    { Annotation a = box(10, 5, 2, 1); rows.push_back({"edges reversed", a, 0}); }
    { Annotation a = box(10, 5, std::nan(""), 1); rows.push_back({"NaN edge", a, 0}); }
    { Annotation a = box(10, 5, 0, HUGE_VAL); rows.push_back({"infinite edge", a, 0}); }
    { Annotation a = box(10, 5, -2e12, 1); rows.push_back({"edge beyond 1e12", a, 0}); }
    { Annotation a = box(1001, 5, 1, 2); rows.push_back({"start past the end", a, 1000}); }
    { Annotation a = box(990, 20, 1, 2); rows.push_back({"runs past the end", a, 1000}); }
    { Annotation a = box(10, 5000, 1, 2); rows.push_back({"count past the end", a, 1000}); }
    { Annotation a = box(UINT64_MAX - 2, 10, 1, 2); rows.push_back({"overflow", a, 0}); }
    { Annotation a = box(1, 1, 1, 2, std::string(129, 'x')); rows.push_back({"label too long", a, 0}); }
    { Annotation a = box(1, 1, 1, 2, "a\x01" "b"); rows.push_back({"label control char", a, 0}); }
    { Annotation a = box(1, 1, 1, 2); a.comment = std::string(4001, 'x'); rows.push_back({"comment too long", a, 0}); }
    { Annotation a = box(1, 1, 1, 2); a.generator = "\xFF"; rows.push_back({"generator invalid UTF-8", a, 0}); }
    { Annotation a = box(1, 1, 1, 2); a.uuid = "not-a-uuid"; rows.push_back({"bad uuid", a, 0}); }
    { Annotation a = box(1, 1, 1, 2); a.uuid = "123e4567-e89b-12d3-a456-42661417400g"; rows.push_back({"uuid not hex", a, 0}); }
    { Annotation a = box(1, 1, 1, 2); a.extra["core:label"] = "shadow"; rows.push_back({"extra shadows a core key", a, 0}); }
    for (const Row& r : rows) {
        const bool ok = validate(r.a, r.total, &err);
        if (ok) { std::printf("NOT REFUSED: %s\n", r.what); }
        CHECK(!ok);
        CHECK(!err.empty());
        std::string meta = baseMeta(true);
        const std::string keep = meta;
        CHECK(!setAnnotations(&meta, {r.a}, r.total, &err) && meta == keep);                     // the text is unchanged on refusal
    }
    // boundaries that are fine
    CHECK(validate(box(990, 10, 1, 1), 1000, &err));                                              // ends exactly at the end; equal edges
    CHECK(validate(box(1000, 0, 1, 2), 1000, &err));                                              // an empty annotation at the end
    CHECK(validate(box(0, 0, -1e12, 1e12), 0, &err));
    Annotation lab = box(1, 1, 1, 2, std::string(128, 'x'));
    CHECK(validate(lab, 0, &err));
    // invalid entries in a file are refused on read
    struct Bad { const char* what; std::string text; };
    const std::vector<Bad> bad = {
        {"negative start", R"({"global":{},"annotations":[{"core:sample_start":-1}]})"},
        {"float start", R"({"global":{},"annotations":[{"core:sample_start":1.5}]})"},
        {"string start", R"({"global":{},"annotations":[{"core:sample_start":"1"}]})"},
        {"no start", R"({"global":{},"annotations":[{"core:sample_count":1}]})"},
        {"one edge", R"({"global":{},"annotations":[{"core:sample_start":1,"core:freq_lower_edge":5}]})"},
        {"edge string", R"({"global":{},"annotations":[{"core:sample_start":1,"core:freq_lower_edge":"a","core:freq_upper_edge":6}]})"},
        {"label number", R"({"global":{},"annotations":[{"core:sample_start":1,"core:label":5}]})"},
        {"entry not object", R"({"global":{},"annotations":[5]})"},
        {"annotations not array", R"({"global":{},"annotations":{}})"},
        {"no global", R"({"annotations":[]})"},
        {"global not object", R"({"global":5})"},
        {"not an object", R"([1,2])"},
        {"garbage", "not json"},
        {"empty", ""},
        {"edges reversed", R"({"global":{},"annotations":[{"core:sample_start":1,"core:freq_lower_edge":9,"core:freq_upper_edge":6}]})"},
    };
    for (const Bad& b : bad) {
        std::vector<Annotation> out;
        CHECK(!readAnnotations(b.text, &out, &err));
        CHECK(!err.empty());
    }
    // the unsorted array of a third-party file is read as it is, and written sorted
    {
        std::string t = R"({"global":{},"annotations":[{"core:sample_start":50},{"core:sample_start":10}]})";
        std::vector<Annotation> out;
        CHECK(readAnnotations(t, &out, &err) && out.size() == 2 && out[0].sampleStart == 50);
        CHECK(setAnnotations(&t, out, 0, &err));
        CHECK(specOk(ojson::parse(t)) && ojson::parse(t)["annotations"][0]["core:sample_start"] == 10);
    }
    // more than 65536
    {
        std::vector<Annotation> many(kMaxAnnotations + 1, box(1, 1, 1, 2));
        std::string t = baseMeta(false);
        CHECK(!setAnnotations(&t, many, 0, &err));
        many.pop_back();
        CHECK(setAnnotations(&t, many, 0, &err));
        std::vector<Annotation> back;
        CHECK(readAnnotations(t, &back, &err) && back.size() == kMaxAnnotations);
    }
}

void testUuid() {
    unsigned char zero[16] = {0}, ones[16]; for (auto& b : ones) { b = 0xFF; }
    const std::string a = uuid4FromBytes(zero), b = uuid4FromBytes(ones);
    CHECK(a == "00000000-0000-4000-8000-000000000000");
    CHECK(b == "ffffffff-ffff-4fff-bfff-ffffffffffff");
    Annotation x = box(1, 1, 1, 2); x.uuid = a;
    std::string err;
    CHECK(validate(x, 0, &err));
    std::mt19937 g(5);
    bool ok = true;
    for (int i = 0; i < 1000; ++i) {
        unsigned char r[16]; for (auto& v : r) { v = static_cast<unsigned char>(g()); }
        const std::string u = uuid4FromBytes(r);
        ok = ok && u.size() == 36 && u[14] == '4' && std::string("89ab").find(u[19]) != std::string::npos;
        Annotation y = box(1, 1, 1, 2); y.uuid = u;
        ok = ok && validate(y, 0, nullptr);
    }
    CHECK(ok);
}

void testFromBox() {
    Annotation a;
    CHECK(fromBox(1.0, 2.5, 7.1e6, 7.1005e6, 48000.0, "sig", "note", "FoxSDR", &a));
    CHECK(a.sampleStart == 48000 && a.hasCount && a.sampleCount == 72000 && a.hasFreq && a.lowHz == 7.1e6 && a.highHz == 7.1005e6 && a.label == "sig");
    CHECK(fromBox(0.0, 0.0, 1, 2, 1000.0, "", "", "", &a) && a.sampleStart == 0 && a.sampleCount == 0);   // an instant
    CHECK(fromBox(0.00049, 0.00151, 1, 2, 1000.0, "", "", "", &a) && a.sampleStart == 0 && a.sampleCount == 2);   // rounded, not truncated
    CHECK(!fromBox(2.0, 1.0, 1, 2, 1000.0, "", "", "", &a));
    CHECK(!fromBox(-1.0, 1.0, 1, 2, 1000.0, "", "", "", &a));
    CHECK(!fromBox(0, 1, 2, 1, 1000.0, "", "", "", &a));
    CHECK(!fromBox(0, 1, 1, 2, 0.0, "", "", "", &a));
    CHECK(!fromBox(std::nan(""), 1, 1, 2, 1000.0, "", "", "", &a));
    CHECK(!fromBox(0, 1, 1, std::nan(""), 1000.0, "", "", "", &a));
    CHECK(!fromBox(0, 1e300, 1, 2, 1000.0, "", "", "", &a));
}

void testFiles() {
    const fs::path dir = scratch();
    const fs::path f = dir / "rec.sigmf-meta";
    { std::ofstream o(f, std::ios::binary); o << baseMeta(false); }
    std::string err;
    CHECK(addToFile(f, box(500, 100, 7.1e6, 7.1001e6, "one"), 1000000, &err) && !fs::exists(f.string() + ".tmp"));
    CHECK(addToFile(f, box(10, 100, 7.1e6, 7.1001e6, "two"), 1000000, &err));
    std::vector<Annotation> v;
    CHECK(readAnnotations(slurp(f), &v, &err) && v.size() == 2 && v[0].label == "two" && v[1].label == "one");
    CHECK(specOk(ojson::parse(slurp(f))));
    // a refused add leaves the bytes alone
    const std::string before = slurp(f);
    CHECK(!addToFile(f, box(900000, 500000, 1, 2), 1000000, &err) && slurp(f) == before);       // runs past the end
    // remove
    CHECK(!removeFromFile(f, 5, &err) && err == "no such annotation" && slurp(f) == before);
    CHECK(removeFromFile(f, 0, &err) && readAnnotations(slurp(f), &v, &err) && v.size() == 1 && v[0].label == "one");
    // not a metadata file: refused, untouched
    const fs::path junk = dir / "junk.sigmf-meta";
    { std::ofstream o(junk, std::ios::binary); o << "this is not json"; }
    CHECK(!addToFile(junk, box(1, 1, 1, 2), 0, &err) && slurp(junk) == "this is not json" && !fs::exists(junk.string() + ".tmp"));
    const fs::path noGlobal = dir / "ng.sigmf-meta";
    { std::ofstream o(noGlobal, std::ios::binary); o << R"({"captures":[]})"; }
    CHECK(!addToFile(noGlobal, box(1, 1, 1, 2), 0, &err));
    CHECK(!addToFile(dir / "missing.sigmf-meta", box(1, 1, 1, 2), 0, &err));
    const fs::path huge = dir / "huge.sigmf-meta";
    { std::ofstream o(huge, std::ios::binary); o << std::string(kMaxFileBytes + 1, ' '); }
    CHECK(!addToFile(huge, box(1, 1, 1, 2), 0, &err) && err.find("16 MiB") != std::string::npos);
    // an edit that cancels
    CHECK(!editFile(f, 0, [](std::vector<Annotation>*, std::string* e) { if (e != nullptr) { *e = "cancelled"; } return false; }, &err) && err == "cancelled");
    std::error_code ec; fs::remove_all(dir, ec);
}

void testFuzz() {
    const std::string good = [] { std::string m = baseMeta(true); std::vector<Annotation> l; std::string e; readAnnotations(m, &l, &e); l.push_back(box(900, 10, 7.1e6, 7.2e6, "x")); setAnnotations(&m, l, 0, &e); return m; }();
    std::mt19937 g(2468);
    long accepted = 0, refused = 0;
    bool ok = true;
    for (int i = 0; i < 40000; ++i) {
        std::string t = good;
        const int edits = 1 + static_cast<int>(g() % 4);
        for (int e = 0; e < edits && !t.empty(); ++e) {
            const std::size_t pos = g() % t.size();
            switch (g() % 5) {
                case 0: t[pos] = static_cast<char>(g()); break;
                case 1: t.erase(pos, 1 + g() % 6); break;
                case 2: t.insert(pos, 1, static_cast<char>(g())); break;
                case 3: t.resize(pos); break;
                default: { static const char* d = "0123456789-.eE"; t[pos] = d[g() % 14]; }
            }
        }
        std::vector<Annotation> list;
        std::string err;
        if (readAnnotations(t, &list, &err)) {
            ++accepted;
            // accepted: it can be rewritten, the result obeys the spec and reads back equal
            std::string w = t;
            if (setAnnotations(&w, list, 0, &err)) {
                std::vector<Annotation> again;
                ok = ok && specOk(ojson::parse(w)) && readAnnotations(w, &again, &err) && again.size() == list.size();
            } else { ok = false; }
        } else { ++refused; ok = ok && !err.empty(); }
    }
    std::printf("fuzz: 40000 mutated metadata files, %ld accepted (all rewrite to spec-valid files), %ld refused\n", accepted, refused);
    CHECK(ok);
    CHECK(accepted > 1000 && refused > 1000);
}

}  // namespace

int main() {
    testRoundTrip();
    testRules();
    testUuid();
    testFromBox();
    testFiles();
    testFuzz();
    return testSummary("test_sigmf_annotations");
}
