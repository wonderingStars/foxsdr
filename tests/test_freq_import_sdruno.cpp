// Tests for the SDRuno memory-bank (.s1b) importer (core/freq_import_sdruno.hpp) and its dispatch from importFrequencyList.
//
// THE FIXTURES ARE WRITTEN BY HAND from the facts in the header (the plugin's column list and the vocabulary seen in public sample banks); no
// bank file is copied or shipped here. They prove the code against that description and its hostile-input limits, not against SDRuno's own
// files: UNVERIFIED against a bank SDRuno itself wrote (the mode codes beyond AM, FM, USB, LSB and CW were never seen).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_import_sdruno.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "core/freq_import.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::Bookmark;
using cascade::core::ForeignImportResult;
namespace sd = cascade::core::sdruno;
namespace cc = cascade::core;

// BOM, CRLF, title lines, blank comma lines, 8 columns, a heading in the mode column of a 7-field line, kilohertz filters.
const std::string kBank =
    "\xEF\xBB\xBF,,,Marine Test Bank,,,,\r\n"
    ",,,,,,,\r\n"
    ",,,Section One,,,,\r\n"
    "5980000,Y,AM,Station A,,,8000,AntA\r\n"
    "7200000,Y,USB,Net B,,,2800,AntA\r\n"
    "3600000,,LSB,\"Net, \"\"C\"\"\",,,3000,AntA\r\n"
    "14100000,Y,CW,Beacon D,,,150,AntA\r\n"
    ",,,,,,,\r\n"
    ",,Section Two,,,,\r\n"
    "146775000,Y,FM,Repeater E,,,2800,AntB\r\n"
    "473150000,Y,FM,Site F,,NFM,15K,AntB\r\n"
    "99900000,,FM,Broadcast G,,,200K,AntB\r\n"
    "162550000,Y,FM,Weather H,,WFM,,AntB\r\n";

void testDetect() {
    CHECK(sd::looksLikeS1b(kBank));
    CHECK(sd::looksLikeS1b("5980000,Y,AM,A,,,8000,AntA\n"));
    CHECK(sd::looksLikeS1b("5980000,,AM,A,,,8000,AntA,-90\n"));                         // the 9-column layout
    CHECK(sd::looksLikeS1b("Frequency,Scan,M,Station,UTC,SubM,Filter,Port,Threshold\n"));   // the column header of the plugin
    CHECK(sd::looksLikeS1b(",,,Title,,,,\n\n\n5980000,N,AM,A,,,8000,AntA\n"));
    // not an .s1b
    CHECK(!sd::looksLikeS1b(""));
    CHECK(!sd::looksLikeS1b("frequency_mhz,name,group,mode,bandwidth_hz,favourite,ticked\r\n145.5,Calling,,NFM,12500,0,1\r\n"));
    CHECK(!sd::looksLikeS1b("145500000,Calling,Ham,NFM,12500,1,0,x\n"));                 // 8 columns but the second is a name, not a Scan cell
    CHECK(!sd::looksLikeS1b("5980000,Y,AM,A,8000\n"));                                    // 5 columns: a generic CSV
    CHECK(!sd::looksLikeS1b("# Frequency ; Name ; Modulation ; Bandwidth ; Tags\n118300000 ; Tower ; AM ; 10000 ; Aircraft\n"));
    CHECK(!sd::looksLikeS1b("<MemoryEntry><Name>x</Name></MemoryEntry>"));
    CHECK(!sd::looksLikeS1b("5980.5,Y,AM,A,,,8000,AntA\n"));                              // a decimal is not hertz
    CHECK(!sd::looksLikeS1b(",,,Only a title,,,,\n"));
}

void testBank() {
    const ForeignImportResult r = sd::importS1b(kBank);
    CHECK(r.error.empty() && r.format == "SDRuno memory bank (.s1b)");
    CHECK(r.entries == 8 && r.skipped == 0 && r.modeDefaulted == 0);   // title lines and blank lines are not entries
    CHECK(r.items.size() == 8);
    if (r.items.size() != 8) { return; }
    const Bookmark& a = r.items[0];
    CHECK(a.name == "Station A" && a.freqHz == 5980000.0 && a.mode == "AM" && a.bandwidthHz == 8000.0 && a.group == "Section One" && a.scan);
    const Bookmark& b = r.items[1];
    CHECK(b.name == "Net B" && b.mode == "USB" && b.bandwidthHz == 2800.0 && b.scan);
    const Bookmark& c = r.items[2];
    CHECK(c.name == "Net, \"C\"" && c.mode == "LSB" && c.bandwidthHz == 3000.0 && !c.scan);   // quoted comma, doubled quote, empty Scan
    CHECK(r.items[3].mode == "CW" && r.items[3].bandwidthHz == 150.0);
    CHECK(r.items[4].group == "Section Two" && r.items[4].mode == "NFM" && r.items[4].bandwidthHz == 2800.0);
    CHECK(r.items[5].mode == "NFM" && r.items[5].bandwidthHz == 15000.0);                      // "15K"
    CHECK(r.items[6].mode == "WFM" && r.items[6].bandwidthHz == 200000.0 && !r.items[6].scan); // FM at 200 kHz is wide
    CHECK(r.items[7].mode == "WFM" && r.items[7].bandwidthHz == 150000.0);                     // sub-mode WFM, no filter: the mode's default
    for (const Bookmark& x : r.items) { CHECK(x.heardSeconds == 0.0 && !x.favourite && x.id == 0); }
}

void testModes() {
    {   // a code with no mapping: imported as NFM, counted, the raw code kept
        const ForeignImportResult r = sd::importS1b("7100000,Y,SAM,Sync AM,,,6000,AntA\n7101000,Y,DSB,Double,,,6000,AntA\n7102000,Y,,No mode,,,6000,AntA\n");
        CHECK(r.error.empty() && r.items.size() == 3 && r.modeDefaulted == 3);
        if (r.items.size() == 3) {
            CHECK(r.items[0].mode == "NFM" && r.items[0].name == "Sync AM [SDRuno mode SAM]" && r.items[0].bandwidthHz == 6000.0);
            CHECK(r.items[1].name == "Double [SDRuno mode DSB]");
            CHECK(r.items[2].mode == "NFM" && r.items[2].name == "No mode");   // an empty cell has no raw code to keep
        }
    }
    {   // lower case codes and spaces are read; a long name keeps the raw code
        const std::string longName(200, 'n');
        const ForeignImportResult r = sd::importS1b(" 7100000 , y , usb , " + longName + " ,,, 2800 ,AntA\n7100500,Y, ecss ," + longName + ",,,2400,AntA\n");
        CHECK(r.items.size() == 2);
        if (r.items.size() == 2) {
            CHECK(r.items[0].mode == "USB" && r.items[0].scan && r.items[0].name.size() == 128);
            CHECK(r.items[1].name.size() <= 128 && r.items[1].name.find("[SDRuno mode ecss]") != std::string::npos && r.items[1].mode == "NFM");
        }
    }
    // the mode names the importer can produce are all FoxSDR demodulator names
    const std::set<std::string> valid = {"NFM", "WFM", "AM", "USB", "LSB", "CW"};
    for (const char* m : {"AM", "FM", "USB", "LSB", "CW", "SAM", "xx", "", "fm"}) {
        bool known = true;
        CHECK(valid.count(sd::modeFor(m, "", 0.0, &known)) == 1);
    }
}

void testRows() {
    {   // missing name, bad frequencies, columns: 3 (the least), 9 (threshold)
        const ForeignImportResult r = sd::importS1b(
            "5980000,Y,AM,,,,8000,AntA\n"
            "5980.5,Y,AM,Decimal,,,8000,AntA\n"
            "abc,Y,AM,Letters,,,8000,AntA\n"
            "0,Y,AM,Zero,,,8000,AntA\n"
            "99999999999999,Y,AM,Huge,,,8000,AntA\n"
            "6000000,Y,AM\n"
            "6100000,,AM,Nine,,,5K,AntB,-80\n");
        CHECK(r.error.empty() && r.entries == 7 && r.skipped == 4 && r.items.size() == 3);
        if (r.items.size() == 3) {
            CHECK(r.items[0].name == "5.9800 MHz" && r.items[0].freqHz == 5980000.0);
            CHECK(r.items[1].freqHz == 6000000.0 && r.items[1].name == "6.0000 MHz" && r.items[1].bandwidthHz == 10000.0);   // AM default
            CHECK(r.items[2].name == "Nine" && r.items[2].bandwidthHz == 5000.0);
        }
    }
    {   // bandwidth cells that are nonsense fall back to the mode's default
        const ForeignImportResult r = sd::importS1b("7100000,Y,USB,A,,,0,AntA\n7100100,Y,USB,B,,,abc,AntA\n7100200,Y,USB,C,,,99999K,AntA\n7100300,Y,USB,D,,,-5,AntA\n");
        CHECK(r.items.size() == 4);
        for (const Bookmark& b : r.items) { CHECK(b.bandwidthHz == 2800.0); }
    }
    {   // control characters and invalid UTF-8 in a name
        const ForeignImportResult r = sd::importS1b(std::string("7100000,Y,USB,Bad\x01\x02 na\xFFme,,,2800,AntA\n"));
        CHECK(r.items.size() == 1 && r.items[0].name == "Bad na?me");
    }
    {   // an unbalanced quote is literal; the row survives
        const ForeignImportResult r = sd::importS1b("7100000,Y,USB,5\" dish,,,2800,AntA\n");
        CHECK(r.items.size() == 1 && r.items[0].name == "5\" dish");
    }
    {   // a title is cut at 64 bytes on a character boundary; headings replace one another (the nearest above wins)
        std::string t = ",,,";
        for (int i = 0; i < 40; ++i) { t += "\xC3\xA9"; }   // 80 bytes of e-acute
        const ForeignImportResult r = sd::importS1b(t + ",,,,\n7100000,Y,USB,A,,,2800,AntA\n,,,Second,,,,\n7100100,Y,USB,B,,,2800,AntA\n");
        CHECK(r.items.size() == 2 && r.items[0].group.size() == 64 && r.items[1].group == "Second");
        CHECK(r.items[0].group.find_first_not_of("\xC3\xA9") == std::string::npos);
    }
    {   // rows before any title are ungrouped
        const ForeignImportResult r = sd::importS1b("7100000,Y,USB,A,,,2800,AntA\n");
        CHECK(r.items.size() == 1 && r.items[0].group.empty());
    }
    {   // the plugin's column header row is not a row; a last line without a newline is read
        const ForeignImportResult r = sd::importS1b("Frequency,Scan,M,Station,UTC,SubM,Filter,Port\n7100000,Y,USB,A,,,2800,AntA");
        CHECK(r.entries == 1 && r.items.size() == 1);
    }
}

void testRefusals() {
    CHECK(!sd::importS1b("").error.empty());
    CHECK(!sd::importS1b(",,,Only a title,,,,\n,,,,,,,\n").error.empty());
    CHECK(!sd::importS1b("abc,Y,AM,x,,,1,A\n").error.empty());                       // nothing usable
    CHECK(sd::importS1b(std::string("7100000,Y,USB,A\0B,,,2800,AntA\n", 31)).error.find("NUL") != std::string::npos);
    {
        const std::string big(16u * 1024u * 1024u + 1u, 'x');
        const ForeignImportResult r = sd::importS1b(big);
        CHECK(r.error.find("16 MiB") != std::string::npos && r.items.empty());
    }
    {   // a line over 4096 bytes is skipped and counted, the next row is read
        const ForeignImportResult r = sd::importS1b("7100000,Y,USB," + std::string(5000, 'x') + ",,,2800,AntA\n7100100,Y,USB,B,,,2800,AntA\n");
        CHECK(r.items.size() == 1 && r.skipped == 1 && r.entries == 2 && r.items[0].name == "B");
    }
    {   // the row bound: 200001 rows read as 200000 and an error
        std::string t;
        t.reserve(200001u * 28u);
        for (int i = 0; i < 200001; ++i) { t += std::to_string(1000000 + i) + ",Y,AM,x,,,8000,AntA\n"; }
        const ForeignImportResult r = sd::importS1b(t);
        CHECK(r.items.size() == 200000 && r.error.find("200000") != std::string::npos);
    }
}

struct Rng {
    std::uint64_t s;
    std::uint32_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(s >> 33);
    }
};

void testFuzz() {
    Rng rng{20261112};
    const std::set<std::string> valid = {"NFM", "WFM", "AM", "USB", "LSB", "CW"};
    const char alphabet[] = "0123456789,,,,\"\r\n YyNnAMFSBLCUWK.-;<\x01\xC3\xA9\xFF";
    for (int i = 0; i < 4000; ++i) {
        std::string t = i % 2 ? "\xEF\xBB\xBF" : "";
        const std::size_t n = rng.next() % 600;
        for (std::size_t k = 0; k < n; ++k) { t.push_back(alphabet[rng.next() % (sizeof alphabet - 1)]); }
        if (i % 3 == 0) { t = "5980000,Y,AM,A,,,8000,AntA\n" + t; }
        const bool looks = sd::looksLikeS1b(t);
        const ForeignImportResult r = sd::importS1b(t);
        (void)looks;
        CHECK(r.items.size() <= r.entries);
        for (const Bookmark& b : r.items) {
            CHECK(b.freqHz > 0.0 && b.freqHz <= 1.0e12 && b.name.size() <= 128 && !b.name.empty() && valid.count(b.mode) == 1);
            CHECK(b.bandwidthHz >= 1.0 && b.bandwidthHz <= 20.0e6 && b.group.size() <= 64);
            for (char c : b.name) { CHECK(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) != 0x7F); }
        }
        cc::importFrequencyList(t);   // the dispatch never crashes either
    }
}

void testDispatch() {
    {
        const cc::ImportResult r = cc::importFrequencyList(kBank);
        CHECK(r.error.empty() && r.format == "SDRuno memory bank (.s1b)" && r.items.size() == 8 && r.entries == 8 && r.skipped == 0);
        CHECK(!r.items.empty() && r.items[0].name == "Station A" && r.items[0].scan);
    }
    {   // the generic CSV still takes what it took before: this exported list has a name in the second column
        std::vector<Bookmark> list(2);
        list[0].name = "Calling";
        list[0].freqHz = 145500000.0;
        list[0].mode = "NFM";
        list[0].bandwidthHz = 12500;
        list[1].name = "Tower";
        list[1].freqHz = 118300000.0;
        list[1].mode = "AM";
        list[1].bandwidthHz = 10000;
        const cc::ImportResult r = cc::importFrequencyList(cc::exportCsv(list));
        CHECK(r.error.empty() && r.format == "CSV" && r.items.size() == 2);
    }
    {   // a file on disk, through the path the Import box uses
        const std::filesystem::path p = std::filesystem::temp_directory_path() / ("eon-bank-" + std::to_string(static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count())) + ".s1b");
        {
            std::ofstream o(p, std::ios::binary);
            o << kBank;
        }
        const cc::ImportResult r = cc::importFrequencyFile(p.generic_string());
        CHECK(r.error.empty() && r.format == "SDRuno memory bank (.s1b)" && r.items.size() == 8);
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
}

}  // namespace

int main() {
    testDetect();
    testBank();
    testModes();
    testRows();
    testRefusals();
    testFuzz();
    testDispatch();
    return testSummary("test_freq_import_sdruno");
}
