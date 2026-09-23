// Tests for core/freq_import - frequency lists from SDR# and from Excel.
//
// The SDR# fixtures are the user's own two entries, verbatim as they posted
// them: once as SDR# writes the file, and once in the one-line-per-entry form
// their Word/Excel merge produces - including the closing tag typed as
// "/MemoryEntry>" with no '<', which is what a real 33 000-entry list of
// theirs contains. A strict XML parser would refuse that file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_import.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/freq_manager.hpp"
#include "test_check.hpp"

using cascade::core::Bookmark;
using cascade::core::FreqManager;
using cascade::core::ImportResult;

namespace {

const char* kSdrSharpFormatted = R"(<?xml version="1.0"?>
<ArrayOfMemoryEntry xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema">
<MemoryEntry>
    <IsFavourite>false</IsFavourite>
    <Name>EDGG_NOR_CTR combined EDGG_WH_CTR</Name>
    <GroupName>L- AIR DE Langen Maastricht Bremen</GroupName>
    <Frequency>128505000</Frequency>
    <DetectorType>AM</DetectorType>
    <Shift>0</Shift>
    <FilterBandwidth>8330</FilterBandwidth>
    <CenterFrequency>0</CenterFrequency>
  </MemoryEntry>
<MemoryEntry>
    <IsFavourite>false</IsFavourite>
    <Name>Vaisala Oyj (FI) RS41 (V2421202) (06-07-2023) (60mW)</Name>
    <GroupName>W- Meteo UHF 400 Sonde FI</GroupName>
    <Frequency>400200000</Frequency>
    <DetectorType>NFM</DetectorType>
    <Shift>0</Shift>
    <FilterBandwidth>6000</FilterBandwidth>
    <CenterFrequency>0</CenterFrequency>
  </MemoryEntry>
</ArrayOfMemoryEntry>
)";

const char* kSdrSharpMerged =
    "<?xml version=\"1.0\"?>\n"
    "<ArrayOfMemoryEntry xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
    "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\n"
    "<MemoryEntry><IsFavourite>false</IsFavourite><Name>EDGG_NOR_CTR combined EDGG_WH_CTR</Name>"
    "<GroupName>L- AIR DE Langen Maastricht Bremen</GroupName><Frequency>128505000</Frequency>"
    "<DetectorType>AM</DetectorType><Shift>0</Shift><FilterBandwidth>8330</FilterBandwidth>"
    "<CenterFrequency>0</CenterFrequency>/MemoryEntry> \n"
    "<MemoryEntry><IsFavourite>false</IsFavourite><Name>Vaisala Oyj (FI) RS41 (V2421202) "
    "(06-07-2023) (60mW)</Name><GroupName>W- Meteo UHF 400 Sonde FI</GroupName>"
    "<Frequency>400200000</Frequency><DetectorType>NFM</DetectorType><Shift>0</Shift>"
    "<FilterBandwidth>6000</FilterBandwidth><CenterFrequency>0</CenterFrequency></MemoryEntry> \n"
    "</ArrayOfMemoryEntry>\n";

void checkTheTwo(const ImportResult& r) {
    CHECK(r.error.empty());
    CHECK(r.entries == 2u);
    CHECK(r.skipped == 0u);
    CHECK(r.items.size() == 2u);
    if (r.items.size() != 2u) { return; }
    const Bookmark& a = r.items[0];
    CHECK(a.name == "EDGG_NOR_CTR combined EDGG_WH_CTR");
    CHECK(a.group == "L- AIR DE Langen Maastricht Bremen");
    CHECK(a.freqHz == 128505000.0);
    CHECK(a.mode == "AM");
    CHECK(a.bandwidthHz == 8330.0);
    CHECK(!a.favourite);
    const Bookmark& b = r.items[1];
    CHECK(b.name == "Vaisala Oyj (FI) RS41 (V2421202) (06-07-2023) (60mW)");
    CHECK(b.group == "W- Meteo UHF 400 Sonde FI");
    CHECK(b.freqHz == 400200000.0);
    CHECK(b.mode == "NFM");
    CHECK(b.bandwidthHz == 6000.0);
}

std::string bigXml(int n) {
    std::string s = "<?xml version=\"1.0\"?>\n<ArrayOfMemoryEntry>\n";
    char line[512];
    for (int i = 0; i < n; ++i) {
        std::snprintf(line, sizeof(line),
                      "<MemoryEntry><IsFavourite>%s</IsFavourite><Name>Channel %d</Name>"
                      "<GroupName>Group %d</GroupName><Frequency>%lld</Frequency>"
                      "<DetectorType>NFM</DetectorType><Shift>0</Shift>"
                      "<FilterBandwidth>12500</FilterBandwidth><CenterFrequency>0</CenterFrequency>"
                      "/MemoryEntry>\n",
                      i % 97 == 0 ? "true" : "false", i, i % 40,
                      static_cast<long long>(30000000LL + (static_cast<long long>(i) * 7919LL) % 1700000000LL));
        s += line;
    }
    s += "</ArrayOfMemoryEntry>\n";
    return s;
}

}  // namespace

int main() {
    // [X1] SDR#'s own layout
    checkTheTwo(cascade::core::importSdrSharpXml(kSdrSharpFormatted));
    // [X2] the merged one-line form with the "/MemoryEntry>" typo
    checkTheTwo(cascade::core::importSdrSharpXml(kSdrSharpMerged));
    // [X3] auto-detection picks SDR# for both
    CHECK(cascade::core::importFrequencyList(kSdrSharpMerged).format == "SDR# XML");

    // [X4] entities, a self-closing empty group, a favourite, a converter
    // shift that is counted and NOT applied, a missing bandwidth that gets the
    // mode's default, and an entry with no frequency that is skipped
    {
        const char* t =
            "<MemoryEntry><IsFavourite>true</IsFavourite><Name>Tom &amp; Jerry &lt;TX&gt; &#233;</Name>"
            "<GroupName /><Frequency>145500000</Frequency><DetectorType>USB</DetectorType>"
            "<Shift>-100000000</Shift></MemoryEntry>"
            "<MemoryEntry><Name>no frequency</Name></MemoryEntry>"
            "<MemoryEntry><Name>garbage</Name><Frequency>12abc</Frequency></MemoryEntry>";
        const ImportResult r = cascade::core::importSdrSharpXml(t);
        CHECK(r.entries == 3u);
        CHECK(r.skipped == 2u);
        CHECK(r.shifted == 1u);
        CHECK(r.items.size() == 1u);
        if (!r.items.empty()) {
            CHECK(r.items[0].name == "Tom & Jerry <TX> \xC3\xA9");
            CHECK(r.items[0].group.empty());
            CHECK(r.items[0].favourite);
            CHECK(r.items[0].freqHz == 145500000.0);  // shift not applied
            CHECK(r.items[0].bandwidthHz == 2800.0);  // USB default
        }
    }
    // [X5] not a frequency list at all
    CHECK(!cascade::core::importSdrSharpXml("<html><body>hello</body></html>").error.empty());

    // [C1] CSV with a header in its own order, MHz in the header
    {
        const ImportResult r = cascade::core::importCsv(
            "Name,Frequency (MHz),Mode,Group,Bandwidth,Favourite\n"
            "\"Tower, main\",118.700,AM,Airport,8330,yes\n"
            "Marine 16,156.8,FM,Marine,,no\n");
        CHECK(r.error.empty());
        CHECK(r.items.size() == 2u);
        if (r.items.size() == 2u) {
            CHECK(r.items[0].name == "Tower, main");
            CHECK(r.items[0].freqHz == 118700000.0);
            CHECK(r.items[0].mode == "AM");
            CHECK(r.items[0].group == "Airport");
            CHECK(r.items[0].bandwidthHz == 8330.0);
            CHECK(r.items[0].favourite);
            CHECK(r.items[1].mode == "NFM");  // "FM" read as narrow FM
            CHECK(r.items[1].bandwidthHz == 12500.0);
            CHECK(!r.items[1].favourite);
        }
    }
    // [C2] European Excel: semicolons, decimal commas, a BOM, no header
    {
        const ImportResult r = cascade::core::importCsv(
            "\xEF\xBB\xBF" "446,00625;PMR 1;PMR;NFM;12500\r\n433,92;ISM;;;\r\n");
        CHECK(r.items.size() == 2u);
        if (r.items.size() == 2u) {
            CHECK(r.items[0].freqHz == 446006250.0);
            CHECK(r.items[0].name == "PMR 1");
            CHECK(r.items[0].group == "PMR");
            CHECK(r.items[1].freqHz == 433920000.0);
        }
    }
    // [C3] Hz without a header, and a line that is not a frequency
    {
        const ImportResult r = cascade::core::importCsv("145500000,Calling\nhello,world\n");
        CHECK(r.items.size() == 1u);
        CHECK(r.skipped == 1u);
        if (!r.items.empty()) { CHECK(r.items[0].freqHz == 145500000.0); }
    }

    // [BIG] 33 000 entries: read, added, re-imported, all quickly
    {
        const int n = 33000;
        const std::string xml = bigXml(n);
        const auto t0 = std::chrono::steady_clock::now();
        ImportResult r = cascade::core::importFrequencyList(xml);
        const auto t1 = std::chrono::steady_clock::now();
        FreqManager fm;
        const std::size_t added = fm.addMany(r.items);
        const auto t2 = std::chrono::steady_clock::now();
        const std::size_t again = fm.addMany(cascade::core::importFrequencyList(xml).items);
        const double parseMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double addMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
        std::printf("[BIG] parse %.1f ms, add %.1f ms\n", parseMs, addMs);
        CHECK(r.items.size() == static_cast<std::size_t>(n));
        CHECK(added == static_cast<std::size_t>(n));
        CHECK(again == 0u);
        CHECK(fm.list().size() == static_cast<std::size_t>(n));
        CHECK(parseMs < 1000.0);
        CHECK(addMs < 1000.0);
        bool sorted = true;
        for (std::size_t i = 1; i < fm.list().size(); ++i) {
            if (fm.list()[i - 1].freqHz > fm.list()[i].freqHz) { sorted = false; }
        }
        CHECK(sorted);

        // [RANGE] the binary search agrees with counting by hand
        // Inside the generated spread (30 to ~290 MHz).
        const double lo = 100.0e6, hi = 106.0e6;
        const auto rg = fm.range(lo, hi);
        std::size_t brute = 0, firstBrute = fm.list().size();
        for (std::size_t i = 0; i < fm.list().size(); ++i) {
            const double f = fm.list()[i].freqHz;
            if (f >= lo && f <= hi) {
                ++brute;
                if (firstBrute == fm.list().size()) { firstBrute = i; }
            }
        }
        CHECK(rg.second - rg.first == brute);
        CHECK(brute > 0u);
        CHECK(rg.first == firstBrute);
        // The whole list, both ends included.
        const auto whole = fm.range(0.0, 1.0e12);
        CHECK(whole.first == 0u && whole.second == fm.list().size());
        const double top = fm.list().back().freqHz;
        const auto last = fm.range(top, top);
        CHECK(last.second == fm.list().size() && last.first < last.second);
        const auto none = fm.range(1.0, 2.0);
        CHECK(none.first == none.second);

        // [IO] the bookmark file at this size: saved and loaded back whole,
        // and the time each takes printed - the app saves after every change
        {
            std::string err;
            const auto s0 = std::chrono::steady_clock::now();
            CHECK(fm.save("test_freq_import_big.json", err));
            const auto s1 = std::chrono::steady_clock::now();
            FreqManager back;
            CHECK(back.load("test_freq_import_big.json", err));
            const auto s2 = std::chrono::steady_clock::now();
            std::printf("[IO] save %.1f ms, load %.1f ms\n",
                        std::chrono::duration<double, std::milli>(s1 - s0).count(),
                        std::chrono::duration<double, std::milli>(s2 - s1).count());
            CHECK(back.list().size() == fm.list().size());
            std::remove("test_freq_import_big.json");
        }

        // [GROUP] a group goes as one
        const unsigned v0 = fm.version();
        const std::size_t gone = fm.removeGroup("Group 7");
        CHECK(gone > 0u);
        CHECK(fm.list().size() == static_cast<std::size_t>(n) - gone);
        CHECK(fm.version() != v0);
    }

    // [WEB] what the browser is sent: the whole of a small list; of a big one,
    // every favourite (up to the cap) and then the nearest to the tuned
    // frequency, in list order, each index once - the browser's row numbers
    // are mapped back through this, so a wrong index deletes the wrong entry
    {
        FreqManager small;
        std::vector<Bookmark> few;
        for (int i = 0; i < 10; ++i) {
            Bookmark b;
            b.name = "s" + std::to_string(i);
            b.freqHz = 100e6 + i * 1e3;
            few.push_back(b);
        }
        small.addMany(few);
        const std::vector<std::size_t> all = small.nearestSubset(0.0, 300, 100);
        CHECK(all.size() == 10u);
        CHECK(all.size() == 10u && all.front() == 0u && all.back() == 9u);

        FreqManager big;
        std::vector<Bookmark> many;
        for (int i = 0; i < 5000; ++i) {
            Bookmark b;
            b.name = "b" + std::to_string(i);
            b.freqHz = 50e6 + i * 10e3;  // 50 MHz .. 100 MHz in 10 kHz steps
            b.favourite = (i % 500) == 0;  // ten favourites, far and wide
            many.push_back(b);
        }
        big.addMany(many);
        const double here = 75e6;
        const std::vector<std::size_t> sub = big.nearestSubset(here, 300, 100);
        CHECK(sub.size() == 300u);
        bool ordered = true, unique = true;
        for (std::size_t k = 1; k < sub.size(); ++k) {
            if (sub[k - 1] > sub[k]) { ordered = false; }
            if (sub[k - 1] == sub[k]) { unique = false; }
        }
        CHECK(ordered);
        CHECK(unique);
        int favs = 0;
        double farthest = 0.0;
        for (const std::size_t i : sub) {
            CHECK(i < big.list().size());
            if (i >= big.list().size()) { continue; }
            if (big.list()[i].favourite) {
                ++favs;
            } else {
                farthest = std::max(farthest, std::fabs(big.list()[i].freqHz - here));
            }
        }
        CHECK(favs == 10);
        // 290 non-favourite neighbours of 75 MHz at 10 kHz spacing reach
        // about 145 entries each way: within 1.5 MHz.
        CHECK(farthest <= 1.5e6);
        // Every entry within 1 MHz of here is in.
        const auto near = big.range(here - 1.0e6, here + 1.0e6);
        bool allNear = true;
        for (std::size_t i = near.first; i < near.second; ++i) {
            if (!std::binary_search(sub.begin(), sub.end(), i)) { allNear = false; }
        }
        CHECK(allNear);
    }

    // [RT] export for SDR# and import back: everything that SDR# carries
    {
        std::vector<Bookmark> v;
        Bookmark a;
        a.name = "A & B <x>";
        a.group = "G";
        a.freqHz = 118700000.0;
        a.mode = "AM";
        a.bandwidthHz = 8330.0;
        a.favourite = true;
        v.push_back(a);
        Bookmark b;
        b.name = "Sonde";
        b.freqHz = 401600000.0;
        b.mode = "NFM";
        b.bandwidthHz = 6000.0;
        v.push_back(b);
        const ImportResult r = cascade::core::importFrequencyList(cascade::core::exportSdrSharpXml(v));
        CHECK(r.items.size() == 2u);
        if (r.items.size() == 2u) {
            CHECK(r.items[0].name == a.name && r.items[0].group == a.group);
            CHECK(r.items[0].freqHz == a.freqHz && r.items[0].mode == a.mode);
            CHECK(r.items[0].bandwidthHz == a.bandwidthHz && r.items[0].favourite);
            CHECK(r.items[1].name == b.name && r.items[1].group.empty() && !r.items[1].favourite);
        }
    }

    // [SAVE] group and favourite survive the bookmark file; a plain bookmark
    // is saved without either key, exactly as before
    {
        const std::string dir = "test_freq_import_tmp";
        std::string err;
        FreqManager fm;
        Bookmark a;
        a.name = "grouped";
        a.freqHz = 100e6;
        a.group = "Broadcast";
        a.favourite = true;
        Bookmark b;
        b.name = "plain";
        b.freqHz = 101e6;
        fm.addMany({a, b});
        const std::string path = dir + "/bm.json";
        CHECK(fm.save(path, err));
        FreqManager back;
        CHECK(back.load(path, err));
        CHECK(back.list().size() == 2u);
        if (back.list().size() == 2u) {
            CHECK(back.list()[0].group == "Broadcast" && back.list()[0].favourite);
            CHECK(back.list()[1].group.empty() && !back.list()[1].favourite);
        }
        std::FILE* f = std::fopen(path.c_str(), "rb");
        std::string text;
        if (f != nullptr) {
            char buf[4096];
            std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { text.append(buf, n); }
            std::fclose(f);
        }
        const std::size_t plainAt = text.find("\"plain\"");
        CHECK(plainAt != std::string::npos);
        // The plain entry's own object - the writer sorts keys, so "group"
        // would come BEFORE "name" within it, not after.
        const std::size_t open = text.rfind('{', plainAt);
        const std::size_t close = text.find('}', plainAt);
        const std::string entry =
            (open != std::string::npos && close != std::string::npos) ? text.substr(open, close - open) : text;
        CHECK(entry.find("\"group\"") == std::string::npos);
        CHECK(entry.find("\"favourite\"") == std::string::npos);
        // ...and the grouped one does carry both.
        const std::size_t gAt = text.find("\"grouped\"");
        const std::size_t gOpen = gAt == std::string::npos ? 0 : text.rfind('{', gAt);
        const std::string gEntry = gAt == std::string::npos ? std::string() : text.substr(gOpen, text.find('}', gAt) - gOpen);
        CHECK(gEntry.find("\"group\"") != std::string::npos);
        CHECK(gEntry.find("\"favourite\"") != std::string::npos);
        std::remove(path.c_str());
        std::remove(dir.c_str());
    }

    return testSummary("test_freq_import");
}
