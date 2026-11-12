// Tests for the gqrx / CubicSDR bookmark importers (core/freq_import_foreign.hpp), and for their dispatch from importFrequencyList.
//
// THE FIXTURES ARE WRITTEN BY HAND from the format descriptions in the header; no real gqrx or CubicSDR file was available, so these tests
// prove the code against that description (and its hostile-input limits), not against the programs' own files. UNVERIFIED until a real
// file of each is read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_import_foreign.hpp"

#include "core/freq_import.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {

using cascade::core::Bookmark;
using cascade::core::ForeignImportResult;
using cascade::core::importForeignBookmarks;
namespace fo = cascade::core::foreign;

const char* kGqrx =
    "# Tag name          ; color\n"
    "Untagged            ; #c0c0c0\n"
    "Aircraft            ; #ff0000\n"
    "Amateur             ; #00ff00\n"
    "2m Band             ; #0000ff\n"
    "\n"
    "# Frequency  ; Name        ; Modulation ; Bandwidth ; Tags\n"
    "   118300000 ; Tower       ; AM         ;      10000 ; Aircraft\n"
    "   145500000 ; Calling     ; Narrow FM  ;      12500 ; Amateur,Local\n"
    "    99900000 ; Radio X     ; WFM (stereo) ;   180000 ; Untagged\n"
    "     7100000 ; 40m SSB     ; LSB        ;       2400 ;\n"
    "    10000000 ; WWV         ; AM-Sync    ;          0 ; \n"
    "    14100000 ; Beacon      ; CW-U       ;        500 ; Amateur\n"
    "   162550000 ; Mystery     ; Plasma FM  ;      12500 ; \n";

ForeignImportResult gq(const std::string& s) { return cascade::core::foreign::importGqrxCsv(s); }
ForeignImportResult cu(const std::string& s) { return cascade::core::foreign::importCubicSdrXml(s); }

void testGqrx() {
    const auto r = importForeignBookmarks(kGqrx);
    CHECK(r.error.empty());
    CHECK(r.format == "gqrx CSV");
    CHECK(r.entries == 7);
    CHECK(r.skipped == 0);
    CHECK(r.items.size() == 7);
    CHECK(r.modeDefaulted == 1);                 // "Plasma FM"
    if (r.items.size() == 7) {
        const auto& a = r.items[0];
        CHECK(a.name == "Tower" && a.freqHz == 118300000 && a.mode == "AM" && a.bandwidthHz == 10000 && a.group == "Aircraft");
        const auto& b = r.items[1];
        CHECK(b.name == "Calling" && b.mode == "NFM" && b.bandwidthHz == 12500 && b.group == "Amateur");   // first tag wins
        const auto& c = r.items[2];
        CHECK(c.mode == "WFM" && c.bandwidthHz == 180000 && c.group.empty());                              // Untagged = no group
        CHECK(r.items[3].mode == "LSB" && r.items[3].group.empty());
        CHECK(r.items[4].mode == "AM" && r.items[4].bandwidthHz == 10000);       // bandwidth 0 -> the mode's default
        CHECK(r.items[5].mode == "CW");
        CHECK(r.items[6].mode == "NFM" && r.items[6].name == "Mystery");
    }
    // the tag-colour block is never taken for bookmarks
    for (const auto& b : r.items) { CHECK(b.name != "Aircraft" && b.name != "Untagged"); }
}

void testGqrxLayouts() {
    // columns in another order, named by the header
    auto r = gq("# Name ; Frequency ; Bandwidth ; Modulation\nAlpha ; 100000000 ; 150000 ; WFM (mono)\n");
    CHECK(r.error.empty() && r.items.size() == 1);
    if (!r.items.empty()) { CHECK(r.items[0].name == "Alpha" && r.items[0].freqHz == 100e6 && r.items[0].mode == "WFM" && r.items[0].bandwidthHz == 150000); }
    // no header: the default order; CRLF and a BOM
    r = gq("\xEF\xBB\xBF" "145500000 ; Calling ; Narrow FM ; 12500 ; Ham\r\n433920000 ; ISM ; Narrow FM ; 25000 ; \r\n");
    CHECK(r.error.empty() && r.items.size() == 2 && r.items[0].group == "Ham" && r.items[1].freqHz == 433920000);
    // comma and tab separated variants
    r = gq("# Frequency, Name, Modulation, Bandwidth, Tags\n145500000, Calling, AM, 9000, X\n");
    CHECK(r.error.empty() && r.items.size() == 1 && r.items[0].mode == "AM");
    r = gq("145500000\tCalling\tAM\t9000\tX\n");
    CHECK(r.error.empty() && r.items.size() == 1 && r.items[0].bandwidthHz == 9000);
    // a short line, an empty name and a last line without a newline
    r = gq("145500000 ; ; AM\n433920000 ;");
    CHECK(r.items.size() == 2 && r.items[0].name == "145.5000 MHz" && r.items[0].bandwidthHz == 10000 && r.items[1].mode == "NFM");
    // unusable frequencies are skipped and counted, the rest imported
    r = gq("0 ; zero ; AM\n-5 ; neg ; AM\nnan ; n ; AM\n1e99 ; huge ; AM\n12x ; junk ; AM\n145500000 ; ok ; AM\n");
    CHECK(r.items.size() == 1 && r.items[0].name == "ok");
    CHECK(r.skipped >= 3);
    CHECK(r.error.empty());
}

void testGqrxHostile() {
    CHECK(!gq("").error.empty());
    CHECK(!gq("   \n \n").error.empty());
    CHECK(!gq("# only comments\n# more\n").error.empty());
    CHECK(!gq(std::string("145500000 ; a\0b ; AM\n", 21)).error.empty());   // NUL
    CHECK(gq(std::string("145500000 ; a\0b ; AM\n", 21)).items.empty());
    CHECK(!importForeignBookmarks("just some text\nwith lines\n").error.empty());
    CHECK(!importForeignBookmarks("<html><body>no</body></html>").error.empty());
    // too large
    {
        std::string big;
        while (big.size() <= fo::kMaxBytes) { big += "145500000 ; a ; AM\n"; }
        const auto q = gq(big);
        CHECK(q.items.empty() && q.error.find("16 MiB") != std::string::npos);
        const auto q2 = importForeignBookmarks(big);
        CHECK(q2.items.empty() && q2.error.find("16 MiB") != std::string::npos);
    }
    // a line over 4096 bytes is skipped, the lines around it are imported
    {
        std::string t = "145500000 ; before ; AM\n433920000 ; " + std::string(5000, 'x') + " ; AM\n162550000 ; after ; AM\n";
        const auto r = gq(t);
        CHECK(r.items.size() == 2 && r.skipped == 1);
    }
    // names: control characters out, invalid UTF-8 replaced, capped at 128 bytes on a character boundary
    {
        const auto r = gq("145500000 ; a\x01\x02" "b\x7f\xff" "c\xC3\xA9 ; AM\n");
        CHECK(r.items.size() == 1);
        if (!r.items.empty()) { CHECK(r.items[0].name == "ab?c\xC3\xA9"); }
        std::string longName;
        for (int i = 0; i < 200; ++i) { longName += "\xE2\x82\xAC"; }   // 600 bytes of three-byte characters
        const auto q = gq("145500000 ; " + longName + " ; AM\n");
        CHECK(q.items.size() == 1);
        if (!q.items.empty()) {
            CHECK(q.items[0].name.size() <= 128 && q.items[0].name.size() >= 126);
            CHECK(q.items[0].name.size() % 3 == 0);                  // never cut inside a character
        }
    }
    // an entry cap
    {
        std::string t;
        for (int i = 0; i < 200100; ++i) { t += std::to_string(1000000 + i) + " ; n ; AM\n"; }
        const auto r = gq(t);
        CHECK(r.items.size() == fo::kMaxEntries);
        CHECK(!r.error.empty());                                     // says it stopped
    }
    // random bytes do not crash and give an error or items, never a hang
    {
        std::string t;
        unsigned s = 12345;
        for (int i = 0; i < 20000; ++i) { s = s * 1103515245u + 12345u; t.push_back(static_cast<char>((s >> 16) & 0xFF)); }
        t.erase(std::remove(t.begin(), t.end(), '\0'), t.end());
        const auto r = importForeignBookmarks(t);
        CHECK(r.items.size() < 50);
    }
}

const char* kCubic =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>\n"
    "<cubicsdr_bookmarks version=\"0.2.1\">\n"
    "  <modems>\n"
    "    <group name=\"Aviation &amp; More\" expanded=\"1\">\n"
    "      <modem>\n"
    "        <bandwidth>10000</bandwidth>\n"
    "        <frequency>118300000</frequency>\n"
    "        <type>AM</type>\n"
    "        <user_label>Tower &lt;1&gt; caf&#233; &#x1F600;</user_label>\n"
    "      </modem>\n"
    "      <modem><bandwidth>12500</bandwidth><frequency>121500000</frequency><type>FM</type><user_label>Guard</user_label></modem>\n"
    "    </group>\n"
    "    <group name=\"Broadcast\">\n"
    "      <modem><bandwidth>200000</bandwidth><frequency>99900000</frequency><type>FMS</type><user_label><![CDATA[Radio <X>]]></user_label></modem>\n"
    "      <modem><frequency>7100000</frequency><type>LSB</type></modem>\n"
    "      <modem><type>AM</type><user_label>no frequency</user_label></modem>\n"
    "      <modem><frequency>162550000</frequency><type>Plasma</type><user_label>odd</user_label></modem>\n"
    "    </group>\n"
    "  </modems>\n"
    "  <ranges>\n"
    "    <range><label>band</label><start>88000000</start><end>108000000</end><frequency>1</frequency></range>\n"
    "  </ranges>\n"
    "</cubicsdr_bookmarks>\n";

void testCubic() {
    const auto r = importForeignBookmarks(kCubic);
    CHECK(r.error.empty());
    CHECK(r.format == "CubicSDR XML");
    CHECK(r.entries == 6);
    CHECK(r.skipped == 1);
    CHECK(r.items.size() == 5);
    CHECK(r.modeDefaulted == 1);
    if (r.items.size() == 5) {
        CHECK(r.items[0].name == "Tower <1> caf\xC3\xA9 \xF0\x9F\x98\x80" && r.items[0].freqHz == 118300000 && r.items[0].mode == "AM" && r.items[0].bandwidthHz == 10000);
        CHECK(r.items[0].group == "Aviation & More");
        CHECK(r.items[1].mode == "NFM" && r.items[1].name == "Guard" && r.items[1].group == "Aviation & More");
        CHECK(r.items[2].mode == "WFM" && r.items[2].bandwidthHz == 200000 && r.items[2].name == "Radio <X>" && r.items[2].group == "Broadcast");
        CHECK(r.items[3].mode == "LSB" && r.items[3].bandwidthHz == 2800 && r.items[3].name == "7.1000 MHz");
        CHECK(r.items[4].mode == "NFM");
    }
    for (const auto& b : r.items) { CHECK(b.name != "band"); }   // the range is not a bookmark
}

void testCubicVariants() {
    // <bookmark> elements, single-quoted group name, an empty group, a numeric char reference that is not allowed
    auto r = cu("<cubicsdr_bookmarks><group name='G1'/><group name='G2'><bookmark><frequency>100000000</frequency><label>a&#0;b&#xD800;c</label></bookmark></group></cubicsdr_bookmarks>");
    CHECK(r.error.empty() && r.items.size() == 1);
    if (!r.items.empty()) { CHECK(r.items[0].group == "G2" && r.items[0].name == "a?b?c"); }
    // a modem outside any group has no group
    r = cu("<cubicsdr_bookmarks><modems><modem><frequency>100000000</frequency></modem></modems></cubicsdr_bookmarks>");
    CHECK(r.items.size() == 1 && r.items[0].group.empty() && r.items[0].mode == "NFM" && r.modeDefaulted == 0);
    // upper-case tags are read
    r = cu("<CubicSDR_Bookmarks><MODEM><FREQUENCY>100000000</FREQUENCY></MODEM></CubicSDR_Bookmarks>");
    CHECK(r.items.size() == 1);
    // unknown entities stay as written
    r = cu("<cubicsdr_bookmarks><modem><frequency>1e8</frequency><user_label>x &foo; y &amp</user_label></modem></cubicsdr_bookmarks>");
    CHECK(r.items.size() == 1 && r.items[0].name == "x &foo; y &amp");
}

void testCubicHostile() {
    CHECK(!cu("").error.empty());
    CHECK(!cu("<other><modem/></other>").error.empty());
    CHECK(!cu(std::string("<cubicsdr_bookmarks><modem><frequency>1\0" "0</frequency></modem></cubicsdr_bookmarks>", 80)).error.empty());
    // entity expansion attack: refused, never expanded
    const std::string lol =
        "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol \"lol\"><!ENTITY lol2 \"&lol;&lol;&lol;&lol;\">]>"
        "<cubicsdr_bookmarks><modem><frequency>100000000</frequency><user_label>&lol2;</user_label></modem></cubicsdr_bookmarks>";
    auto r = cu(lol);
    CHECK(r.error.find("ENTITY") != std::string::npos && r.items.empty());
    CHECK(!importForeignBookmarks(lol).error.empty());
    r = cu("<!doctype x><cubicsdr_bookmarks><modem><frequency>1e8</frequency></modem></cubicsdr_bookmarks>");
    CHECK(!r.error.empty());
    // nesting depth
    {
        std::string t = "<cubicsdr_bookmarks>";
        for (int i = 0; i < 100; ++i) { t += "<g>"; }
        for (int i = 0; i < 100; ++i) { t += "</g>"; }
        t += "</cubicsdr_bookmarks>";
        CHECK(cu(t).error.find("nested") != std::string::npos);
    }
    // truncated and unbalanced input: what was complete is kept, the error says the rest was not read
    {
        const std::string t = kCubic;
        const auto cut = cu(t.substr(0, t.find("<modem><bandwidth>200000")));
        CHECK(cut.items.size() == 2);                                  // the two complete modems before the cut
        CHECK(!cut.error.empty());
        const auto bad = cu("<cubicsdr_bookmarks><modem><frequency>1e8</frequency></wrong></cubicsdr_bookmarks>");
        CHECK(bad.items.empty() && !bad.error.empty());
        const auto trunc = cu("<cubicsdr_bookmarks><modem><frequency>1e8</freq");
        CHECK(trunc.items.empty() && !trunc.error.empty());
    }
    // too large
    {
        std::string big = "<cubicsdr_bookmarks>";
        while (big.size() <= fo::kMaxBytes) { big += "<modem><frequency>100000000</frequency></modem>"; }
        big += "</cubicsdr_bookmarks>";
        const auto q = cu(big);
        CHECK(q.items.empty() && q.error.find("16 MiB") != std::string::npos);
    }
    // many entries: capped
    {
        std::string t = "<cubicsdr_bookmarks>";
        for (int i = 0; i < 200100; ++i) { t += "<modem><frequency>" + std::to_string(1000000 + i) + "</frequency></modem>"; }
        t += "</cubicsdr_bookmarks>";
        const auto r2 = cu(t);
        CHECK(r2.items.size() == fo::kMaxEntries && !r2.error.empty());
    }
    // a frequency that is hostile
    r = cu("<cubicsdr_bookmarks><modem><frequency>-5</frequency></modem><modem><frequency>nan</frequency></modem><modem><frequency>1e300</frequency></modem><modem><frequency>0x10</frequency></modem></cubicsdr_bookmarks>");
    CHECK(r.items.empty() && r.skipped == 4);
}

void testFiles() {
    namespace fs = std::filesystem;
    const fs::path d = fs::temp_directory_path() / "coref-import";
    fs::create_directories(d);
    const fs::path g = d / "bookmarks.csv", c = d / "bookmarks.xml", big = d / "big.csv";
    { std::ofstream f(g, std::ios::binary); f << kGqrx; }
    { std::ofstream f(c, std::ios::binary); f << kCubic; }
    CHECK(cascade::core::importForeignFile(g.string()).items.size() == 7);
    CHECK(cascade::core::importForeignFile(c.string()).items.size() == 5);
    CHECK(!cascade::core::importForeignFile((d / "missing.csv").string()).error.empty());
    CHECK(!cascade::core::importForeignFile(d.string()).error.empty());                 // a directory
    {
        std::ofstream f(big, std::ios::binary);
        std::string chunk(1 << 20, 'a');
        for (int i = 0; i < 17; ++i) { f << chunk; }
    }
    const auto r = cascade::core::importForeignFile(big.string());
    CHECK(!r.error.empty() && r.error.find("16 MiB") != std::string::npos);
    std::error_code ec;
    fs::remove_all(d, ec);
}

// ---- wired into importFrequencyList (engine wiring W2): the one entry point every import path uses -------------------------------------------

void testDispatch() {
    // gqrx and CubicSDR files, by content, through the entry point the import op (FOXAPP_OP_BOOKMARK_IMPORT_FILE) and the window both call.
    const cascade::core::ImportResult g = cascade::core::importFrequencyList(kGqrx);
    CHECK(g.error.empty());
    CHECK(g.format == "gqrx CSV");
    CHECK(g.entries == 7 && g.items.size() == 7 && g.skipped == 0 && g.modeDefaulted == 1);
    if (!g.items.empty()) { CHECK(g.items[0].name == "Tower" && g.items[0].freqHz == 118300000 && g.items[0].mode == "AM" && g.items[0].group == "Aircraft"); }
    const cascade::core::ImportResult gb = cascade::core::importFrequencyList(std::string("\xEF\xBB\xBF") + kGqrx);   // a byte-order mark
    CHECK(gb.format == "gqrx CSV" && gb.items.size() == 7);
    const cascade::core::ImportResult c = cascade::core::importFrequencyList(kCubic);
    CHECK(c.error.empty());
    CHECK(c.format == "CubicSDR XML");
    CHECK(!c.items.empty());
    CHECK(cascade::core::importFrequencyList(std::string("\xEF\xBB\xBF") + kCubic).format == "CubicSDR XML");
    // The same count as the foreign reader itself: nothing is lost in the fold.
    CHECK(c.items.size() == importForeignBookmarks(kCubic).items.size());
    // What was there before is untouched: a generic CSV (comma, no header comment) and SDR#'s XML are still read as they were.
    CHECK(cascade::core::importFrequencyList("frequency,name,mode\n100.1,A,FM\n").format == "CSV");
    CHECK(cascade::core::importFrequencyList("Name,Frequency (MHz),Mode\nA,100.1,FM\n").format == "CSV");
    CHECK(cascade::core::importFrequencyList("<MemoryEntry><Name>x</Name><Frequency>100000000</Frequency></MemoryEntry>").format == "SDR# XML");
    // A refusal from the foreign reader reaches the caller as the ImportResult error (a gqrx-looking file with no usable line).
    const cascade::core::ImportResult none = cascade::core::importFrequencyList("# Frequency ; Name ; Modulation\n");
    CHECK(!none.error.empty() && none.items.empty());
}

}  // namespace

int main() {
    std::printf("foreign bookmark import\n");
    testGqrx();
    testGqrxLayouts();
    testGqrxHostile();
    testCubic();
    testCubicVariants();
    testCubicHostile();
    testFiles();
    testDispatch();
    return testSummary("test_freq_import_foreign");
}
