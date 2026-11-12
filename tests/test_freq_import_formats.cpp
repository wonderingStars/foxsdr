// Tests for the CHIRP CSV and SDR++ JSON bookmark readers (core/freq_import_formats.cpp), and for their detection by content in
// importFrequencyList (feat/web-bookmarks).
//
// CLEAN-ROOM: every fixture below is written by hand to the survey appendix's description of the two formats (second-feature-list-survey.md,
// "SDR++ bookmark JSON" and "CHIRP CSV"); no export of either program was available and none of their source was read. A test here proves the
// reader does what the description says, NOT that a real export reads the same way: that is the UNVERIFIED line in
// docs/feature-records/web-bookmarks.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_import.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_check.hpp"

using cascade::core::ImportResult;

namespace {

// The header as the survey's secondary record gives it.
const char* kChirpHeader =
    "Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,DtcsPolarity,RxDtcsCode,CrossMode,Mode,TStep,Skip,Power,"
    "Comment,URCALL,RPT1CALL,RPT2CALL,DVCODE\n";

std::string chirpRow(const std::string& loc, const std::string& name, const std::string& freq, const std::string& duplex,
                     const std::string& tone, const std::string& rTone, const std::string& cTone, const std::string& dtcs,
                     const std::string& mode, const std::string& comment = "") {
    return loc + "," + name + "," + freq + "," + duplex + ",0.600000," + tone + "," + rTone + "," + cTone + "," + dtcs + ",NN,023,Tone->Tone," +
           mode + ",5.00,," + "5.0W," + comment + ",,,,\n";
}

std::string chirpFile() {
    std::string t = kChirpHeader;
    t += chirpRow("0", "2m Calling", "145.500000", "", "", "88.5", "88.5", "023", "FM");
    t += chirpRow("1", "GB3WH", "145.725000", "-", "Tone", "103.5", "88.5", "023", "FM");
    t += chirpRow("2", "GB3XX", "430.800000", "-", "TSQL", "88.5", "110.9", "023", "NFM");
    t += chirpRow("3", "Digital", "145.600000", "", "DTCS", "88.5", "88.5", "754", "FM");
    t += chirpRow("4", "Tower", "118.700000", "", "", "88.5", "88.5", "023", "AM");
    t += chirpRow("5", "Odd mode", "446.100000", "", "", "88.5", "88.5", "023", "DV");
    t += chirpRow("6", "Beacon", "28.200000", "", "", "88.5", "88.5", "023", "USB");
    t += chirpRow("7", "", "7.100000", "", "", "88.5", "88.5", "023", "LSB");
    return t;
}

const cascade::core::Bookmark* find(const ImportResult& r, double hz) {
    for (const auto& b : r.items) {
        if (b.freqHz == hz) { return &b; }
    }
    return nullptr;
}

// ---- CHIRP ------------------------------------------------------------------------------------------------

void chirpHappyPath() {
    std::printf("  [C1] a CHIRP export: frequency, name, mode and tone\n");
    const ImportResult r = cascade::core::importChirpCsv(chirpFile());
    CHECK(r.error.empty());
    CHECK(r.format == "CHIRP CSV");
    CHECK(r.entries == 8u);
    CHECK(r.skipped == 0u);
    CHECK(r.items.size() == 8u);
    // FM is NFM, no tone on an empty Tone column (the rToneFreq / DtcsCode cells CHIRP always writes are not a tone by themselves).
    if (const auto* b = find(r, 145500000.0)) {
        CHECK(b->name == "2m Calling");
        CHECK(b->mode == "NFM");
        CHECK(b->bandwidthHz == 12500.0);
        CHECK(!b->favourite && !b->scan && b->group.empty());
    } else { CHECK(false); }
    // Tone: the sent tone (rToneFreq), kept in the name because a bookmark has no tone field.
    if (const auto* b = find(r, 145725000.0)) { CHECK(b->name == "GB3WH [CTCSS 103.5 Hz]"); } else { CHECK(false); }
    // TSQL: the squelch tone (cToneFreq).
    if (const auto* b = find(r, 430800000.0)) { CHECK(b->name == "GB3XX [CTCSS 110.9 Hz]"); CHECK(b->mode == "NFM"); } else { CHECK(false); }
    // DTCS: the code.
    if (const auto* b = find(r, 145600000.0)) { CHECK(b->name == "Digital [DCS 754]"); } else { CHECK(false); }
    CHECK(r.toned == 3u);
    if (const auto* b = find(r, 118700000.0)) { CHECK(b->mode == "AM"); CHECK(b->bandwidthHz == 10000.0); } else { CHECK(false); }
    // An unknown mode (DV) is NFM, and counted.
    if (const auto* b = find(r, 446100000.0)) { CHECK(b->mode == "NFM"); } else { CHECK(false); }
    CHECK(r.modeDefaulted == 1u);
    if (const auto* b = find(r, 28200000.0)) { CHECK(b->mode == "USB"); CHECK(b->bandwidthHz == 2800.0); } else { CHECK(false); }
    // An empty name is the frequency, as the other importers do.
    if (const auto* b = find(r, 7100000.0)) { CHECK(b->mode == "LSB"); CHECK(b->name == "7.1000 MHz"); } else { CHECK(false); }
}

void chirpModeTable() {
    std::printf("  [C2] the mode mapping table\n");
    struct Row { const char* chirp; const char* want; bool counted; };
    const Row rows[] = {{"FM", "NFM", false},  {"NFM", "NFM", false}, {"fm", "NFM", false}, {"WFM", "WFM", false},
                        {"AM", "AM", false},   {"NAM", "AM", false},  {"USB", "USB", false}, {"LSB", "LSB", false},
                        {"CW", "CW", false},   {"CWR", "CW", false},  {"DV", "NFM", true},   {"DMR", "NFM", true},
                        {"P25", "NFM", true},  {"Auto", "NFM", true}, {"", "NFM", false}};
    for (const Row& row : rows) {
        const std::string t = std::string(kChirpHeader) + chirpRow("0", "x", "100.000000", "", "", "", "", "", row.chirp);
        const ImportResult r = cascade::core::importChirpCsv(t);
        CHECK(r.items.size() == 1u);
        if (r.items.size() == 1u) { CHECK(r.items[0].mode == row.want); } else { std::printf("    mode %s\n", row.chirp); }
        CHECK(r.modeDefaulted == (row.counted ? 1u : 0u));
    }
}

void chirpToleratesTheShapesFilesCome() {
    std::printf("  [C3] byte-order mark, CRLF, quoted fields, empty rows, a missing optional column\n");
    // BOM + CRLF + a quoted name with a comma, a doubled quote and a line break inside + an empty row + a row of only commas.
    std::string t = "\xEF\xBB\xBF";
    t += "Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,Mode,Comment\r\n";
    t += "0,\"Hill, \"\"North\"\"\",145.575,,0.6,Tone,91.5,FM,\"two\r\nlines, here\"\r\n";
    t += "\r\n";
    t += ",,,,,,,,\r\n";
    t += "1,Plain,446.00625,,0.6,,,NFM,\r\n";
    t += "   \r\n";
    const ImportResult r = cascade::core::importChirpCsv(t);
    CHECK(r.error.empty());
    CHECK(r.items.size() == 2u);
    // The all-commas row has no frequency: seen, skipped. Fully empty rows are not rows.
    CHECK(r.entries == 3u);
    CHECK(r.skipped == 1u);
    if (r.items.size() == 2u) {
        CHECK(r.items[0].name == "Hill, \"North\" [CTCSS 91.5 Hz]");
        CHECK(r.items[0].freqHz == 145575000.0);
        CHECK(r.items[1].name == "Plain");
        CHECK(r.items[1].freqHz == 446006250.0);  // 446.00625 MHz, rounded to the hertz
    }
    // No Tone / rToneFreq / cToneFreq / DtcsCode / Mode columns at all: header Location,Name,Frequency,Duplex,Offset.
    const ImportResult small = cascade::core::importChirpCsv("Location,Name,Frequency,Duplex,Offset\n0,A,100.1,,\n1,B,100.2,,\n");
    CHECK(small.error.empty());
    CHECK(small.items.size() == 2u);
    CHECK(small.toned == 0u && small.modeDefaulted == 0u);
    if (small.items.size() == 2u) { CHECK(small.items[0].mode == "NFM"); }
    // A header in another case and column order.
    const ImportResult odd =
        cascade::core::importChirpCsv("mode,frequency,LOCATION,duplex,Name,offset\nAM,121.5,3,,Guard,\n");
    CHECK(odd.error.empty());
    CHECK(odd.items.size() == 1u);
    if (odd.items.size() == 1u) { CHECK(odd.items[0].mode == "AM"); CHECK(odd.items[0].name == "Guard"); CHECK(odd.items[0].freqHz == 121500000.0); }
    // The last row without a final line break, and a lone-CR file.
    const ImportResult noEol = cascade::core::importChirpCsv("Location,Name,Frequency,Duplex,Offset\n0,A,100.1,,");
    CHECK(noEol.items.size() == 1u);
    const ImportResult cr = cascade::core::importChirpCsv("Location,Name,Frequency,Duplex,Offset\r0,A,100.1,,\r1,B,100.2,,\r");
    CHECK(cr.items.size() == 2u);
}

void chirpSkipsBadRows() {
    std::printf("  [C4] rows with no usable frequency are skipped and counted\n");
    std::string t = kChirpHeader;
    t += chirpRow("0", "ok", "100.100000", "", "", "", "", "", "FM");
    t += chirpRow("1", "blank", "", "", "", "", "", "", "FM");
    t += chirpRow("2", "text", "abc", "", "", "", "", "", "FM");
    t += chirpRow("3", "zero", "0.000000", "", "", "", "", "", "FM");
    t += chirpRow("4", "negative", "-5.000000", "", "", "", "", "", "FM");
    t += chirpRow("5", "nan", "nan", "", "", "", "", "", "FM");
    t += chirpRow("6", "inf", "inf", "", "", "", "", "", "FM");
    t += chirpRow("7", "hertz", "146520000", "", "", "", "", "", "FM");  // can only be hertz
    const ImportResult r = cascade::core::importChirpCsv(t);
    CHECK(r.entries == 8u);
    CHECK(r.skipped == 6u);
    CHECK(r.items.size() == 2u);
    if (r.items.size() == 2u) { CHECK(r.items[1].freqHz == 146520000.0); }
    // Nothing usable at all is an error, not an empty success.
    const ImportResult none = cascade::core::importChirpCsv(std::string(kChirpHeader) + chirpRow("0", "x", "", "", "", "", "", "", "FM"));
    CHECK(!none.error.empty());
    CHECK(none.items.empty());
    // A header and nothing else.
    const ImportResult headerOnly = cascade::core::importChirpCsv(kChirpHeader);
    CHECK(!headerOnly.error.empty());
    CHECK(headerOnly.entries == 0u);
}

void chirpRefusesWhatIsNotChirp() {
    std::printf("  [C5] a file that is not CHIRP CSV is refused with a clear message\n");
    const char* notChirp[] = {
        "",
        "   \n\n",
        "frequency,name,mode\n100.1,A,FM\n",                                  // the generic CSV
        "Location,Name,Frequency\n0,A,100.1\n",                               // 3 columns: no second CHIRP column
        "Name,Duplex,Offset\nA,,\n",                                          // no Location, no Frequency
        "<MemoryEntry><Name>x</Name><Frequency>100100000</Frequency></MemoryEntry>",
        "{\"lists\":{}}",
        "just some text, with a comma\n",
    };
    for (const char* text : notChirp) {
        const ImportResult r = cascade::core::importChirpCsv(text);
        CHECK(!r.error.empty());
        CHECK(r.items.empty());
        CHECK(!cascade::core::looksLikeChirpCsv(text));
    }
    const ImportResult r = cascade::core::importChirpCsv("frequency,name,mode\n100.1,A,FM\n");
    CHECK(r.error.find("not a CHIRP CSV file") == 0u);
}

void chirpFromFileAndLarge() {
    std::printf("  [C6] from a file, and 5000 rows in well under a second\n");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "web-freq-import-formats";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "web-chirp.csv";
    {
        std::ofstream f(path, std::ios::binary);
        f << "\xEF\xBB\xBF" << chirpFile();
    }
    const ImportResult r = cascade::core::importFrequencyFile(path.string());
    CHECK(r.error.empty());
    CHECK(r.format == "CHIRP CSV");
    CHECK(r.items.size() == 8u);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    std::string big = kChirpHeader;
    for (int i = 0; i < 5000; ++i) {
        big += chirpRow(std::to_string(i), "ch" + std::to_string(i), std::to_string(144.0 + i * 0.0125), "", i % 3 == 0 ? "Tone" : "", "88.5", "88.5", "023", "FM", "c");
    }
    const auto t0 = std::chrono::steady_clock::now();
    const ImportResult b = cascade::core::importFrequencyList(big);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("    5000 rows imported in %lld ms\n", static_cast<long long>(ms));
    CHECK(b.format == "CHIRP CSV");
    CHECK(b.items.size() == 5000u);
    CHECK(ms < 1000);
}

// ---- SDR++ ------------------------------------------------------------------------------------------------

// Written to the structure the survey records: selectedList, bookmarkDisplayMode, lists{name:{showOnWaterfall, bookmarks{name:{frequency,
// bandwidth, mode}}}}.
const char* kSdrpp = R"({
  "selectedList": "General",
  "bookmarkDisplayMode": 1,
  "lists": {
    "General": {
      "showOnWaterfall": true,
      "bookmarks": {
        "BBC R4": { "frequency": 198000.0, "bandwidth": 9000.0, "mode": 2 },
        "Radio 2": { "frequency": 88100000.0, "bandwidth": 150000.0, "mode": 1 },
        "PMR 1": { "frequency": 446006250.0, "bandwidth": 12500.0, "mode": 0 }
      }
    },
    "Ham": {
      "showOnWaterfall": false,
      "bookmarks": {
        "20m SSB": { "frequency": 14200000, "bandwidth": 2700, "mode": 4 },
        "40m LSB": { "frequency": 7100000, "bandwidth": 2700, "mode": 6 },
        "CW": { "frequency": 7030000, "bandwidth": 500, "mode": 5 },
        "DSB": { "frequency": 3700000, "bandwidth": 6000, "mode": 3 },
        "IQ": { "frequency": 10000000, "bandwidth": 48000, "mode": 7 }
      }
    },
    "Empty": { "showOnWaterfall": true, "bookmarks": {} }
  }
})";

void sdrppHappyPath() {
    std::printf("  [S1] an SDR++ frequency-manager file: lists become groups, the mode index is mapped\n");
    const ImportResult r = cascade::core::importSdrppJson(kSdrpp);
    CHECK(r.error.empty());
    CHECK(r.format == "SDR++ JSON");
    CHECK(r.entries == 8u);
    CHECK(r.skipped == 0u);
    CHECK(r.items.size() == 8u);
    CHECK(r.modeDefaulted == 0u);
    struct Want { double hz; const char* name; const char* group; const char* mode; double bw; };
    const Want want[] = {
        {198000.0, "BBC R4", "General", "AM", 9000.0},      {88100000.0, "Radio 2", "General", "WFM", 150000.0},
        {446006250.0, "PMR 1", "General", "NFM", 12500.0},  {14200000.0, "20m SSB", "Ham", "USB", 2700.0},
        {7100000.0, "40m LSB", "Ham", "LSB", 2700.0},       {7030000.0, "CW", "Ham", "CW", 500.0},
        {3700000.0, "DSB", "Ham", "DSB", 6000.0},           {10000000.0, "IQ", "Ham", "RAW", 48000.0},
    };
    for (const Want& w : want) {
        const auto* b = find(r, w.hz);
        CHECK(b != nullptr);
        if (b != nullptr) {
            CHECK(b->name == w.name);
            CHECK(b->group == w.group);
            CHECK(b->mode == w.mode);
            CHECK(b->bandwidthHz == w.bw);
        }
    }
}

void sdrppOddEntries() {
    std::printf("  [S2] a missing bandwidth, an unknown or missing mode, and entries with no usable frequency\n");
    const char* t = R"({"lists":{"L":{"bookmarks":{
        "no bw":      {"frequency": 145500000, "mode": 0},
        "mode 9":     {"frequency": 100000000, "bandwidth": 15000, "mode": 9},
        "mode -1":    {"frequency": 101000000, "bandwidth": 15000, "mode": -1},
        "mode text":  {"frequency": 102000000, "bandwidth": 15000, "mode": "AM"},
        "no mode":    {"frequency": 103000000, "bandwidth": 15000},
        "bw zero":    {"frequency": 104000000, "bandwidth": 0, "mode": 2},
        "no freq":    {"bandwidth": 15000, "mode": 0},
        "str freq":   {"frequency": "145500000", "mode": 0},
        "neg freq":   {"frequency": -5, "mode": 0},
        "zero freq":  {"frequency": 0, "mode": 0},
        "not object": 5,
        "":           {"frequency": 105000000, "mode": 0}
    }}}})";
    const ImportResult r = cascade::core::importSdrppJson(t);
    CHECK(r.error.empty());
    CHECK(r.entries == 12u);
    CHECK(r.skipped == 5u);
    CHECK(r.items.size() == 7u);
    CHECK(r.modeDefaulted == 4u);  // mode 9, mode -1, mode "AM" (not an index), no mode
    if (const auto* b = find(r, 145500000.0)) { CHECK(b->mode == "NFM"); CHECK(b->bandwidthHz == 12500.0); } else { CHECK(false); }
    if (const auto* b = find(r, 104000000.0)) { CHECK(b->mode == "AM"); CHECK(b->bandwidthHz == 10000.0); } else { CHECK(false); }
    if (const auto* b = find(r, 100000000.0)) { CHECK(b->mode == "NFM"); CHECK(b->bandwidthHz == 15000.0); } else { CHECK(false); }
    if (const auto* b = find(r, 105000000.0)) { CHECK(b->name == "105.0000 MHz"); } else { CHECK(false); }  // an empty key
}

void sdrppRefusesWhatIsNotSdrpp() {
    std::printf("  [S3] anything that is not an SDR++ frequency-manager file is refused with a clear message\n");
    const char* notSdrpp[] = {
        "",
        "   \n",
        "not json at all",
        "{\"lists\":",                                           // truncated
        "[1,2,3]",
        "{}",
        "{\"selectedList\":\"x\"}",                              // no lists
        "{\"lists\":[]}",                                        // lists not an object
        "{\"lists\":{\"A\":{\"bookmarks\":{}}}}",                // lists, but nothing in them
        "{\"lists\":{\"A\":5}}",
        "Location,Name,Frequency,Duplex,Offset\n0,A,100.1,,\n",
        "<MemoryEntry><Frequency>100100000</Frequency></MemoryEntry>",
    };
    for (const char* text : notSdrpp) {
        const ImportResult r = cascade::core::importSdrppJson(text);
        CHECK(!r.error.empty());
        CHECK(r.items.empty());
    }
    CHECK(cascade::core::importSdrppJson("not json").error.find("not an SDR++ frequency-manager file") == 0u);
    CHECK(!cascade::core::looksLikeSdrppJson("{\"selectedList\":\"x\"}"));
    CHECK(!cascade::core::looksLikeSdrppJson("[1]"));
    CHECK(!cascade::core::looksLikeSdrppJson(chirpFile()));
    CHECK(cascade::core::looksLikeSdrppJson(kSdrpp));
    CHECK(cascade::core::looksLikeSdrppJson(std::string("\xEF\xBB\xBF  \n") + kSdrpp));
}

// ---- both, by content --------------------------------------------------------------------------------------

void detectionByContent() {
    std::printf("  [D1] importFrequencyList picks the reader from the content, not the extension\n");
    CHECK(cascade::core::importFrequencyList(chirpFile()).format == "CHIRP CSV");
    CHECK(cascade::core::importFrequencyList(std::string("\xEF\xBB\xBF") + chirpFile()).format == "CHIRP CSV");
    CHECK(cascade::core::importFrequencyList(kSdrpp).format == "SDR++ JSON");
    CHECK(cascade::core::importFrequencyList(kSdrpp).items.size() == 8u);
    // The older two are untouched: a generic CSV (even one with a Location column), and SDR#'s XML.
    CHECK(cascade::core::importFrequencyList("frequency,name,mode\n100.1,A,FM\n").format == "CSV");
    CHECK(cascade::core::importFrequencyList("Name,Frequency (MHz),Mode\nA,100.1,FM\n").format == "CSV");
    CHECK(cascade::core::importFrequencyList("Location,Name,Frequency\n0,A,100.1\n").format == "CSV");
    CHECK(cascade::core::importFrequencyList("<MemoryEntry><Name>x</Name><Frequency>100100000</Frequency></MemoryEntry>").format == "SDR# XML");
    // A JSON file that is not SDR++'s falls through to the generic CSV reader and is refused there, in its words.
    const ImportResult other = cascade::core::importFrequencyList("{\"hello\": 1}");
    CHECK(other.format == "CSV");
    CHECK(!other.error.empty());
    // Through a file, with a name that says nothing (a ".txt" and a ".dat").
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "web-freq-import-formats";
    std::filesystem::create_directories(dir);
    const std::filesystem::path a = dir / "web-sdrpp.dat";
    const std::filesystem::path b = dir / "web-chirp.txt";
    { std::ofstream f(a, std::ios::binary); f << kSdrpp; }
    { std::ofstream f(b, std::ios::binary); f << chirpFile(); }
    CHECK(cascade::core::importFrequencyFile(a.string()).format == "SDR++ JSON");
    CHECK(cascade::core::importFrequencyFile(b.string()).format == "CHIRP CSV");
    std::error_code ec;
    std::filesystem::remove(a, ec);
    std::filesystem::remove(b, ec);
}

// Every prefix of both fixtures, and a few damaged ones, through every reader: never a crash, never an exception, and a result that is either
// items or an error.
void truncatedAndDamagedInput() {
    std::printf("  [D2] every prefix of both fixtures is read without a crash\n");
    for (const std::string& whole : {chirpFile(), std::string(kSdrpp)}) {
        for (std::size_t n = 0; n <= whole.size(); n += 3) {
            const std::string part = whole.substr(0, n);
            const ImportResult c = cascade::core::importChirpCsv(part);
            const ImportResult s = cascade::core::importSdrppJson(part);
            const ImportResult a = cascade::core::importFrequencyList(part);
            CHECK(!c.items.empty() || !c.error.empty());
            CHECK(!s.items.empty() || !s.error.empty());
            CHECK(!a.items.empty() || !a.error.empty());
        }
    }
    std::string noise;
    for (int i = 0; i < 4000; ++i) { noise.push_back(static_cast<char>((i * 131 + 7) & 0xFF)); }
    CHECK(!cascade::core::importFrequencyList(noise).error.empty());
    CHECK(!cascade::core::importChirpCsv(std::string("Location,Name,Frequency,Duplex\n") + std::string(10000, '"')).items.size());
}

}  // namespace

int main() {
    chirpHappyPath();
    chirpModeTable();
    chirpToleratesTheShapesFilesCome();
    chirpSkipsBadRows();
    chirpRefusesWhatIsNotChirp();
    chirpFromFileAndLarge();
    sdrppHappyPath();
    sdrppOddEntries();
    sdrppRefusesWhatIsNotSdrpp();
    detectionByContent();
    truncatedAndDamagedInput();
    return testSummary("test_freq_import_formats");
}
