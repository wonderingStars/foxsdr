// Attachments on a signal record (core/record_attachments.hpp): screenshots, clips and notes copied or kept in a record folder, magic-byte checks,
// limits, safe names, hash verification, manifest round trip and hostile manifests, privacy (no echo, no output) and nothing outside the folder.
#include <cstdio>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define RA_PID _getpid()
#else
#include <unistd.h>
#define RA_PID getpid()
#endif

#include "core/record_attachments.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::core::attach;

namespace {

fs::path scratch() {
    fs::path p = fs::current_path() / ("recplay-attach-" + std::to_string(RA_PID));
    fs::remove_all(p);
    fs::create_directories(p / "record");
    fs::create_directories(p / "inbox");
    return p;
}
void writeFile(const fs::path& p, const std::string& bytes) { std::ofstream o(p, std::ios::binary | std::ios::trunc); o.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }

std::string png(std::size_t extra = 100) { return std::string("\x89PNG\r\n\x1a\n", 8) + std::string(extra, 'p'); }
std::string jpg(std::size_t extra = 100) { return std::string("\xFF\xD8\xFF\xE0", 4) + std::string(extra, 'j'); }
std::string wav(std::size_t extra = 100) { return std::string("RIFF\x00\x00\x00\x00WAVE", 12) + std::string(extra, 'w'); }

std::set<std::string> names(const fs::path& dir) { std::set<std::string> s; for (const auto& e : fs::directory_iterator(dir)) { s.insert(e.path().filename().string()); } return s; }

void testAddListVerify() {
    const fs::path base = scratch();
    AttachmentStore st;
    std::string err;
    CHECK(!st.open(base / "nope", &err));
    CHECK(!st.addNote("t", "n", 0, &err) && err == "no record folder is open");
    CHECK(st.open(base / "record", &err) && st.items().empty());
    writeFile(base / "inbox" / "shot one.png", png());
    writeFile(base / "inbox" / "clip.wav", wav(5000));
    writeFile(base / "inbox" / "photo.JPG", jpg());
    const std::uint32_t a = st.addFile(base / "inbox" / "shot one.png", Kind::Screenshot, "The signal at 14:02", 1000, &err);
    const std::uint32_t b = st.addFile(base / "inbox" / "clip.wav", Kind::Clip, "audio", 2000, &err);
    const std::uint32_t c = st.addFile(base / "inbox" / "photo.JPG", Kind::Screenshot, "", 3000, &err);
    const std::uint32_t d = st.addNote("Observation", "Heard a weak carrier\nsecond line caf\xC3\xA9", 4000, &err);
    CHECK(a == 1 && b == 2 && c == 3 && d == 4);
    CHECK(st.items().size() == 4);
    CHECK(st.items()[0].storedName == "1-shot one.png" && st.items()[1].storedName == "2-clip.wav" && st.items()[2].storedName == "3-photo.jpg");
    CHECK(slurp(base / "record" / "1-shot one.png") == png());                                    // copied byte for byte
    CHECK(fs::exists(base / "inbox" / "shot one.png"));                                           // the original stays
    CHECK(st.items()[3].kind == Kind::Note && st.items()[3].text.find("second line") != std::string::npos);
    CHECK(st.totalBytes() == png().size() + wav(5000).size() + jpg().size() + st.items()[3].text.size());
    for (const VerifyEntry& v : st.verify()) { CHECK(v.state == FileState::Ok); }
    CHECK(st.verify().size() == 3);
    // a second store opens the same record
    AttachmentStore st2;
    CHECK(st2.open(base / "record", &err) && st2.items().size() == 4 && st2.manifestText() == st.manifestText());
    CHECK(st2.items()[1].sha256 == st.items()[1].sha256 && st2.items()[3].title == "Observation");
    // a changed, truncated and missing file are told apart
    writeFile(base / "record" / "1-shot one.png", png() + "X");
    writeFile(base / "record" / "2-clip.wav", wav(5000).substr(0, 3000));
    fs::remove(base / "record" / "3-photo.jpg");
    std::string one = png(); one[50] ^= 1;
    writeFile(base / "record" / "1-shot one.png", one);
    const auto vr = st.verify();
    CHECK(vr.size() == 3 && vr[0].state == FileState::HashChanged && vr[1].state == FileState::SizeChanged && vr[2].state == FileState::Missing);
    // edit, remove
    CHECK(st.edit(d, "Renamed", nullptr, &err) && st.items()[3].title == "Renamed");
    const std::string newText = "changed text";
    CHECK(st.edit(d, "Renamed", &newText, &err) && st.items()[3].text == newText && st.items()[3].sizeBytes == newText.size());
    CHECK(!st.edit(a, "x", &newText, &err));                                                       // text on a file item
    CHECK(!st.edit(99, "x", nullptr, &err) && err == "no such attachment");
    CHECK(!st.edit(d, std::string(65, 'x'), nullptr, &err));
    CHECK(st.remove(b, &err) && !fs::exists(base / "record" / "2-clip.wav") && st.items().size() == 3);
    CHECK(!st.remove(b, &err));
    CHECK(st.remove(d, &err) && st.items().size() == 2);
    // ids are not reused
    writeFile(base / "inbox" / "again.png", png());
    CHECK(st.addFile(base / "inbox" / "again.png", Kind::Screenshot, "again", 5, &err) == 5);
    // the same file name twice gets two stored names
    writeFile(base / "inbox" / "dup.png", png());
    const std::uint32_t d1 = st.addFile(base / "inbox" / "dup.png", Kind::Screenshot, "", 6, &err), d2 = st.addFile(base / "inbox" / "dup.png", Kind::Screenshot, "", 7, &err);
    CHECK(d1 == 6 && d2 == 7 && st.items()[st.items().size() - 1].storedName == "7-dup.png");
    std::error_code ec; fs::remove_all(base, ec);
}

void testRefusals() {
    const fs::path base = scratch();
    AttachmentStore st;
    std::string err;
    CHECK(st.open(base / "record", &err));
    const auto recordBefore = [&] { return names(base / "record"); };
    // wrong content for the kind, or disguised
    writeFile(base / "inbox" / "prog.png", std::string("MZ\x90\x00", 4) + std::string(100, 'x'));
    CHECK(st.addFile(base / "inbox" / "prog.png", Kind::Screenshot, "", 0, &err) == 0 && err.find("PNG or JPEG") != std::string::npos);
    writeFile(base / "inbox" / "clip.png", wav());
    CHECK(st.addFile(base / "inbox" / "clip.png", Kind::Screenshot, "", 0, &err) == 0);           // a WAV is not a screenshot
    writeFile(base / "inbox" / "pic.wav", png());
    CHECK(st.addFile(base / "inbox" / "pic.wav", Kind::Clip, "", 0, &err) == 0);                  // and a PNG is not a clip
    writeFile(base / "inbox" / "script.png", "#!/bin/sh\nrm -rf x\n");
    CHECK(st.addFile(base / "inbox" / "script.png", Kind::Screenshot, "", 0, &err) == 0);
    // the stored extension comes from the CONTENT: a JPEG named .png is stored as .jpg
    writeFile(base / "inbox" / "j.png", jpg());
    CHECK(st.addFile(base / "inbox" / "j.png", Kind::Screenshot, "", 0, &err) == 1 && st.items()[0].storedName == "1-j.jpg");
    // nothing, empty, a folder, a missing file
    CHECK(st.addFile(base / "inbox" / "missing.png", Kind::Screenshot, "", 0, &err) == 0);
    writeFile(base / "inbox" / "empty.png", "");
    CHECK(st.addFile(base / "inbox" / "empty.png", Kind::Screenshot, "", 0, &err) == 0);
    CHECK(st.addFile(base / "inbox", Kind::Screenshot, "", 0, &err) == 0);
    CHECK(st.addFile(base / "inbox" / "j.png", Kind::Note, "", 0, &err) == 0);
    // titles and notes
    writeFile(base / "inbox" / "ok.png", png());
    CHECK(st.addFile(base / "inbox" / "ok.png", Kind::Screenshot, std::string(65, 'x'), 0, &err) == 0);
    CHECK(st.addFile(base / "inbox" / "ok.png", Kind::Screenshot, "bad\x01title", 0, &err) == 0);
    CHECK(st.addNote("t", "", 0, &err) == 0 && st.addNote("t", std::string(8001, 'x'), 0, &err) == 0 && st.addNote("t", std::string("a\0b", 3), 0, &err) == 0 && st.addNote("t", "\xFF", 0, &err) == 0);
    CHECK(st.addNote("t", std::string(8000, 'x'), 0, &err) == 2);
    // hostile source names make safe stored names
    for (const char* nm : {"..png", "a<b>c.png", "con.png", " lead.png", "trail .png", "x:y.png"}) {
        writeFile(base / "inbox" / nm, png());
        const std::uint32_t id = st.addFile(base / "inbox" / nm, Kind::Screenshot, "", 0, &err);
        if (id != 0) {
            const std::string stored = st.items().back().storedName;
            CHECK(stored.find('/') == std::string::npos && stored.find('\\') == std::string::npos && stored.find(':') == std::string::npos && stored.find("..") == std::string::npos);
            CHECK(fs::exists(base / "record" / stored));
        }
    }
    // the name builder directly (Windows cannot even create most of these source names)
    CHECK(detail::cleanStem("a<b>:c|d?e*f/g\\h") == "a_b__c_d_e_f_g_h");
    CHECK(detail::cleanStem("") == "file" && detail::cleanStem("..") == "file" && detail::cleanStem("...") == "file" && detail::cleanStem("  ") == "file");
    CHECK(detail::cleanStem(std::string(100, 'x')).size() == 40);
    CHECK(detail::cleanStem("trail. ") == "trail" && detail::cleanStem(" lead") == "lead");
    CHECK(detail::cleanStem("caf\xC3\xA9") == "caf\xC3\xA9" && detail::cleanStem("bad\xFF") == "bad_");
    for (const char* ok : {"1-a.png", "12-x y.wav", "3-caf\xC3\xA9.jpg"}) { CHECK(detail::plainName(ok)); }
    for (const char* bad : {"", ".", "..", "a/b", "a\b", "C:x", "a.", "a ", "a<b", "a|b", "a\x01" "b"}) { CHECK(!detail::plainName(bad)); }
    CHECK(!detail::plainName(std::string(81, 'a')));
    // nothing but the manifest and the stored files in the record folder, nothing outside it
    for (const std::string& n : recordBefore()) { CHECK(n == "attachments.json" || (n.size() > 2 && std::isdigit(static_cast<unsigned char>(n[0])))); }
    CHECK(names(base) == (std::set<std::string>{"record", "inbox"}));
    std::error_code ec; fs::remove_all(base, ec);
}

void testLimits() {
    const fs::path base = scratch();
    Limits lim; lim.maxScreenshotBytes = 1000; lim.maxClipBytes = 3000; lim.maxTotalBytes = 5000; lim.maxItems = 6; lim.maxTitle = 10; lim.maxNote = 50;
    AttachmentStore st(lim);
    std::string err;
    CHECK(st.open(base / "record", &err));
    writeFile(base / "inbox" / "big.png", png(2000));
    CHECK(st.addFile(base / "inbox" / "big.png", Kind::Screenshot, "", 0, &err) == 0 && err.find("larger") != std::string::npos);
    writeFile(base / "inbox" / "edge.png", png(1000 - 8));                                         // exactly the limit
    CHECK(st.addFile(base / "inbox" / "edge.png", Kind::Screenshot, "", 0, &err) == 1);
    writeFile(base / "inbox" / "c.wav", wav(2988));                                                // exactly 3000
    CHECK(st.addFile(base / "inbox" / "c.wav", Kind::Clip, "", 0, &err) == 2);
    CHECK(st.addFile(base / "inbox" / "c.wav", Kind::Clip, "", 0, &err) == 0 && err.find("total size") != std::string::npos);   // 1000 + 3000 + 3000 > 5000
    CHECK(!fs::exists(base / "record" / "3-c.wav"));
    CHECK(st.addNote("0123456789", std::string(50, 'x'), 0, &err) == 3);
    CHECK(st.addNote("01234567890", "x", 0, &err) == 0);
    CHECK(st.addNote("t", std::string(51, 'x'), 0, &err) == 0);
    CHECK(st.addNote("a", "x", 0, &err) == 4 && st.addNote("b", "x", 0, &err) == 5 && st.addNote("c", "x", 0, &err) == 6);
    CHECK(st.addNote("d", "x", 0, &err) == 0 && err.find("too many") != std::string::npos && st.items().size() == 6);
    std::error_code ec; fs::remove_all(base, ec);
}

void testManifestHostile() {
    const fs::path base = scratch();
    AttachmentStore good;
    std::string err;
    CHECK(good.open(base / "record", &err));
    writeFile(base / "inbox" / "a.png", png());
    good.addFile(base / "inbox" / "a.png", Kind::Screenshot, "t", 1, &err);
    good.addNote("n", "text", 2, &err);
    const std::string text = good.manifestText();
    using nlohmann::json;
    struct Row { const char* what; std::function<void(json&)> edit; };
    const std::vector<Row> rows = {
        {"format", [](json& j) { j["format"] = "x"; }},
        {"version 2", [](json& j) { j["version"] = 2; }},
        {"nextId 0", [](json& j) { j["nextId"] = 0; }},
        {"nextId low", [](json& j) { j["nextId"] = 1; }},
        {"items not array", [](json& j) { j["items"] = 5; }},
        {"id 0", [](json& j) { j["items"][0]["id"] = 0; }},
        {"id repeated", [](json& j) { j["items"][1]["id"] = j["items"][0]["id"]; }},
        {"kind", [](json& j) { j["items"][0]["kind"] = "movie"; }},
        {"title number", [](json& j) { j["items"][0]["title"] = 5; }},
        {"file with ..", [](json& j) { j["items"][0]["file"] = "..\\x.png"; }},
        {"file with slash", [](json& j) { j["items"][0]["file"] = "a/b.png"; }},
        {"file absolute", [](json& j) { j["items"][0]["file"] = "C:\\Windows\\x.png"; }},
        {"file dotdot only", [](json& j) { j["items"][0]["file"] = ".."; }},
        {"file missing", [](json& j) { j["items"][0].erase("file"); }},
        {"hash short", [](json& j) { j["items"][0]["sha256"] = "ab"; }},
        {"hash upper", [](json& j) { j["items"][0]["sha256"] = std::string(64, 'A'); }},
        {"note text empty", [](json& j) { j["items"][1]["text"] = ""; }},
        {"size negative", [](json& j) { j["items"][0]["sizeBytes"] = -1; }},
        {"too many items", [](json& j) { for (int i = 0; i < 600; ++i) { json o = j["items"][1]; o["id"] = 10 + i; j["items"].push_back(o); } j["nextId"] = 1000; }},
    };
    int n = 0;
    for (const Row& r : rows) {
        json j = json::parse(text);
        r.edit(j);
        writeFile(base / "record" / "attachments.json", j.dump());
        AttachmentStore s;
        const bool ok = s.open(base / "record", &err);
        if (ok) { std::printf("NOT REFUSED: %s\n", r.what); }
        CHECK(!ok);
        CHECK(s.items().empty());
        ++n;
    }
    std::printf("hostile manifest rows refused: %d\n", n);
    // a refused manifest is not overwritten by an add (open failed, so nothing is open)
    writeFile(base / "record" / "attachments.json", "garbage");
    AttachmentStore s2;
    CHECK(!s2.open(base / "record", &err));
    CHECK(s2.addNote("t", "x", 0, &err) == 0 && slurp(base / "record" / "attachments.json") == "garbage");
    // fuzz: mutated manifests either open with valid content or refuse; an opened one saves and reopens equal
    std::mt19937 g(99);
    long acc = 0, ref = 0;
    bool consistent = true;
    for (int i = 0; i < 4000; ++i) {
        std::string t = text;
        for (int e = 0, k = 1 + static_cast<int>(g() % 3); e < k; ++e) {
            const std::size_t pos = g() % t.size();
            switch (g() % 4) { case 0: t[pos] = static_cast<char>(g()); break; case 1: t.erase(pos, 1 + g() % 5); break; case 2: t.insert(pos, 1, static_cast<char>(g())); break; default: t.resize(pos); }
            if (t.empty()) { t = "x"; }
        }
        writeFile(base / "record" / "attachments.json", t);
        AttachmentStore s;
        if (s.open(base / "record", &err)) {
            ++acc;
            const std::string m = s.manifestText();
            writeFile(base / "record" / "attachments.json", m);
            AttachmentStore s3;
            consistent = consistent && s3.open(base / "record", &err) && s3.manifestText() == m;
            for (const Item& it : s.items()) { consistent = consistent && (it.kind == Kind::Note || (it.storedName.find('/') == std::string::npos && it.storedName.find('\\') == std::string::npos && it.storedName.find("..") == std::string::npos)); }
        } else { ++ref; consistent = consistent && !err.empty(); }
    }
    std::printf("manifest fuzz: 4000 inputs, %ld accepted, %ld refused\n", acc, ref);
    CHECK(consistent && ref > 2000);
    std::error_code ec; fs::remove_all(base, ec);
}

void testPrivacy() {
    const fs::path base = scratch();
    AttachmentStore st;
    std::string err;
    st.open(base / "record", &err);
    writeFile(base / "inbox" / "SECRET-FILE-77.png", std::string("MZ", 2) + std::string(50, 'x'));
    CHECK(st.addFile(base / "inbox" / "SECRET-FILE-77.png", Kind::Screenshot, "SECRET-TITLE-12", 0, &err) == 0);
    CHECK(err.find("SECRET") == std::string::npos);
    CHECK(st.addNote("SECRET-TITLE-12", std::string(9000, 'S') + "SECRET-NOTE", 0, &err) == 0 && err.find("SECRET") == std::string::npos);
    CHECK(st.addNote(std::string(100, 'T') + "SECRET-T", "x", 0, &err) == 0 && err.find("SECRET") == std::string::npos);
    CHECK(!st.edit(99, "SECRET-TITLE-12", nullptr, &err) && err.find("SECRET") == std::string::npos);
    // a note and a title are in the manifest only (no other file in the record folder holds them)
    CHECK(st.addNote("SECRET-TITLE-OK", "SECRET-NOTE-OK", 0, &err) == 1);
    for (const auto& e : fs::directory_iterator(base / "record")) { CHECK(e.path().filename() == "attachments.json"); }
    fs::path hdr = fs::path(__FILE__).parent_path().parent_path() / "src" / "core" / "record_attachments.hpp";
    std::ifstream in(hdr, std::ios::binary);
    if (in) {
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const char* banned : {"printf", "std::cout", "std::cerr", "std::clog", "puts(", "fputs", "OutputDebugString", "perror("}) { CHECK(src.find(banned) == std::string::npos); }
        CHECK(src.size() > 1000);
    } else { SKIP_LINUX("record_attachments.hpp not found from __FILE__ for the source scan"); }
    std::error_code ec; fs::remove_all(base, ec);
}

}  // namespace

int main() {
    testAddListVerify();
    testRefusals();
    testLimits();
    testManifestHostile();
    testPrivacy();
    return testSummary("test_record_attachments");
}
