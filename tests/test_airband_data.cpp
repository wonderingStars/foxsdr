// Tests for core/airband_data.hpp - the airport frequency table, its lookup,
// the 8.33 kHz channel rule, and the frequency-list entries an airport makes.
//
// Two kinds of test, as in test_band_plan.cpp:
//   1. RULE tests against small tables written here, with answers worked out
//      by hand (the 8.33 kHz channel arithmetic, the parser's tolerance, the
//      lookup order, the nearest-airport order).
//   2. DATA tests against the table this build SHIPS: the embedded bytes must
//      equal resources/airband/airband.tsv (the generator writes both), and
//      the airport the request was about - Chicago O'Hare - must carry every
//      frequency the tester listed by hand.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/airband_data.hpp"
#include "core/airband_assets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_check.hpp"

using namespace cascade::core;
namespace fs = std::filesystem;

namespace {

// Walk up from the CWD to the directory holding resources/airband (build
// directories live inside the source tree; see test_band_plan.cpp).
fs::path findTable() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        const fs::path candidate = dir / "resources" / "airband" / "airband.tsv";
        if (fs::is_regular_file(candidate, ec)) { return candidate; }
        const fs::path up = dir.parent_path();
        if (up == dir) { break; }
        dir = up;
    }
    return {};
}

const AirbandFreq* freqAt(const Airport& a, int khz) {
    for (const AirbandFreq& f : a.freqs) {
        if (f.channelKhz == khz) { return &f; }
    }
    return nullptr;
}

}  // namespace

int main() {
    // --- the channel rule ----------------------------------------------------
    // 25 kHz names are their own frequency.
    CHECK(airbandChannelHz(121600) == 121600000.0);
    CHECK(airbandChannelHz(118050) == 118050000.0);
    CHECK(airbandChannelHz(124125) == 124125000.0);
    CHECK(airbandChannelHz(128575) == 128575000.0);
    CHECK(!airband833(121600));
    CHECK(!airband833(124125));
    // 8.33 kHz names: .x05 / .x10 / .x15 are 0, 25/3 and 50/3 kHz above the
    // 25 kHz slot - in every slot, not only the first of each 100 kHz.
    CHECK(airband833(118005));
    CHECK(airband833(118010));
    CHECK(airband833(118015));
    CHECK(airband833(121935));
    CHECK_NEAR(airbandChannelHz(118005), 118000000.0, 1e-6);
    CHECK_NEAR(airbandChannelHz(118010), 118008333.333333, 1e-3);
    CHECK_NEAR(airbandChannelHz(118015), 118016666.666667, 1e-3);
    CHECK_NEAR(airbandChannelHz(118030), 118025000.0, 1e-6);
    CHECK_NEAR(airbandChannelHz(118040), 118041666.666667, 1e-3);
    // Heathrow's departure ATIS, published as 121.935.
    CHECK_NEAR(airbandChannelHz(121935), 121933333.333333, 1e-3);
    // Off both rasters (some sources list .x20): taken literally.
    CHECK(!airband833(118020));
    CHECK(airbandChannelHz(118020) == 118020000.0);

    // --- the parser ------------------------------------------------------------
    {
        const std::string text =
            "# comment\n"
            "F\t121500\tEMERG\tbefore any airport\n"                       // skipped
            "A\tKAAA\tAAA\tAAA\tUS\t41.0\t-87.0\tAlpha Field\n"
            "F\t126900\tTWR\tRWY 22R\n"
            "F\t118050\tGND\t\n"                                            // empty desc
            "F\tnot-a-number\tTWR\tx\n"                                     // skipped
            "A\tEGBB\t\t\tGB\t52.45\t-1.75\tBirmingham\n"                   // empty IATA and local
            "F\t118305\tTWR\tTower\n"
            "A\tXBAD\t\t\tGB\tnorth\t-1.0\tBroken latitude\n"               // skipped, and
            "F\t120000\tTWR\tbelongs to the broken one\n"                   // so is its F line
            "Z\tunknown record\n"                                           // skipped
            "\n";
        std::size_t skipped = 0;
        const std::vector<Airport> t = parseAirbandTable(text, &skipped);
        CHECK(t.size() == 2);
        CHECK(skipped == 5);
        if (t.size() == 2) {
            CHECK(t[0].ident == "KAAA");
            CHECK(t[0].iata == "AAA");
            CHECK(t[0].name == "Alpha Field");
            CHECK(t[0].freqs.size() == 2);
            if (t[0].freqs.size() == 2) {
                // ascending, whatever the file order
                CHECK(t[0].freqs[0].channelKhz == 118050);
                CHECK(t[0].freqs[0].desc.empty());
                CHECK(t[0].freqs[1].channelKhz == 126900);
                CHECK(t[0].freqs[1].type == "TWR");
                CHECK(t[0].freqs[1].desc == "RWY 22R");
            }
            CHECK(t[1].ident == "EGBB");
            CHECK(t[1].iata.empty());
            CHECK(t[1].local.empty());
            CHECK(t[1].country == "GB");
            CHECK_NEAR(t[1].latDeg, 52.45, 1e-9);
            CHECK(t[1].freqs.size() == 1);
            if (!t[1].freqs.empty()) {
                CHECK(t[1].freqs[0].channelKhz == 118305);
                CHECK_NEAR(t[1].freqs[0].hz, 118300000.0, 1e-6);
            }
        }
        // CRLF line endings parse the same.
        std::string crlf;
        for (const char c : text) {
            if (c == '\n') { crlf += '\r'; }
            crlf += c;
        }
        const std::vector<Airport> t2 = parseAirbandTable(crlf);
        CHECK(t2.size() == 2);
        if (t2.size() == 2) { CHECK(t2[1].name == "Birmingham"); }
        CHECK(parseAirbandTable("").empty());
    }

    // --- lookup ------------------------------------------------------------------
    {
        const std::string text =
            "A\tKORD\tORD\tORD\tUS\t41.9786\t-87.9048\tChicago O'Hare\n"
            "F\t121900\tGND\tinbound\n"
            "A\tORD\t\t\tXX\t10.0\t10.0\tAn airport whose ident is ORD\n"
            "F\t122800\tCTAF\t\n"
            "A\tKMDW\tMDW\tMDW\tUS\t41.7860\t-87.7524\tChicago Midway\n"
            "F\t126500\tTWR\t\n"
            "A\tEGLL\tLHR\t\tGB\t51.4707\t-0.4599\tLondon Heathrow\n"
            "F\t118500\tTWR\t\n";
        const std::vector<Airport> t = parseAirbandTable(text);
        // ICAO, IATA and FAA id all find O'Hare; case and spaces do not matter.
        std::vector<const Airport*> r = findAirports(t, "kord");
        CHECK(r.size() == 1 && r[0]->ident == "KORD");
        r = findAirports(t, "  lhr ");
        CHECK(r.size() == 1 && r[0]->ident == "EGLL");
        // "ORD" is an ident AND an IATA code: the exact ident first, O'Hare
        // second, each once.
        r = findAirports(t, "ORD");
        CHECK(r.size() == 2);
        if (r.size() == 2) {
            CHECK(r[0]->ident == "ORD");
            CHECK(r[1]->ident == "KORD");
        }
        CHECK(findAirports(t, "").empty());
        CHECK(findAirports(t, "   ").empty());
        CHECK(findAirports(t, "ZZZZ").empty());

        // Nearest to a point in Chicago's Loop: Midway (~14 km) then O'Hare
        // (~25 km), then London.
        const auto near = nearestAirports(t, 41.8781, -87.6298, 3);
        CHECK(near.size() == 3);
        if (near.size() == 3) {
            CHECK(near[0].first->ident == "KMDW");
            CHECK(near[1].first->ident == "KORD");
            CHECK(near[2].first->ident == "EGLL");
            CHECK(near[0].second > 10.0 && near[0].second < 16.0);
            CHECK(near[1].second > 22.0 && near[1].second < 28.0);
        }
        CHECK(nearestAirports(t, 0.0, 0.0, 0).empty());
        CHECK(nearestAirports(t, 0.0, 0.0, 99).size() == t.size());
        // London - Chicago is about 6,350 km.
        CHECK_NEAR(airbandDistanceKm(51.4707, -0.4599, 41.9786, -87.9048), 6345.0, 30.0);
        CHECK_NEAR(airbandDistanceKm(10.0, 20.0, 10.0, 20.0), 0.0, 1e-9);
    }

    // --- which services are continuous broadcasts ------------------------
    {
        const auto f = [](const char* type) {
            AirbandFreq q;
            q.type = type;
            return q;
        };
        CHECK(airbandContinuous(f("ATIS")));
        CHECK(airbandContinuous(f("D-ATIS")));
        CHECK(airbandContinuous(f("awos")));
        CHECK(airbandContinuous(f("ASOS")));
        CHECK(airbandContinuous(f("VOLMET")));
        CHECK(!airbandContinuous(f("TWR")));
        CHECK(!airbandContinuous(f("GND")));
        CHECK(!airbandContinuous(f("APP")));
        CHECK(!airbandContinuous(f("CTAF")));
        CHECK(!airbandContinuous(f("")));
    }

    // --- a description that repeats its type loses the repeat ---------------
    {
        const auto d = [](const char* type, const char* desc) {
            AirbandFreq q;
            q.type = type;
            q.desc = desc;
            return airbandDescWithoutType(q);
        };
        CHECK(d("CLASS B", "NORTH CLASS B") == "NORTH");
        CHECK(d("EMERG", "EMERG") == "");
        CHECK(d("GND", "GND METERING") == "METERING");
        CHECK(d("gnd", "GND METERING") == "METERING");
        CHECK(d("TWR", "RWY 09L/27R LCL/P") == "RWY 09L/27R LCL/P");
        CHECK(d("TWR", "Heathrow Tower") == "Heathrow Tower");
        CHECK(d("APP", "APPROACH") == "APPROACH");     // a word that only starts with it
        CHECK(d("GND", "") == "");
        CHECK(d("", "GND METERING") == "GND METERING");
    }

    // --- the frequency-list entries an airport makes ------------------------
    {
        const std::string text =
            "A\tEGLL\tLHR\t\tGB\t51.4707\t-0.4599\tLondon Heathrow Airport\n"
            "F\t118500\tTWR\tHeathrow Tower\n"
            "F\t121935\tATIS\tHeathrow ATIS (Departure)\n"
            "F\t121975\tCLD\t\n";
        const std::vector<Airport> t = parseAirbandTable(text);
        CHECK(t.size() == 1);
        if (t.size() == 1) {
            CHECK(airportGroupName(t[0]) == "EGLL London Heathrow Airport");
            const std::vector<Bookmark> b = airportBookmarks(t[0]);
            CHECK(b.size() == 3);
            if (b.size() == 3) {
                for (const Bookmark& x : b) {
                    CHECK(x.mode == "AM");
                    CHECK(x.group == "EGLL London Heathrow Airport");
                    CHECK(!x.favourite);
                    CHECK(x.heardSeconds == 0.0);
                }
                // Ticked - except the ATIS, which never stops talking.
                CHECK(b[0].scan);
                CHECK(!b[1].scan);
                CHECK(b[2].scan);
                CHECK(b[0].name == "TWR Heathrow Tower");
                CHECK(b[0].freqHz == 118500000.0);
                CHECK(b[0].bandwidthHz == kAirbandBw25Hz);
                // 8.33: the channel NAME is in the entry's name, the true
                // frequency in its figure, and the channel is narrower.
                CHECK(b[1].name == "ATIS 121.935 Heathrow ATIS (Departure)");
                CHECK_NEAR(b[1].freqHz, 121933333.333333, 1e-3);
                CHECK(b[1].bandwidthHz == kAirbandBw833Hz);
                CHECK(b[2].name == "CLD");
                CHECK(b[2].bandwidthHz == kAirbandBw25Hz);
            }
        }
    }

    // --- the table this build ships -----------------------------------------------
    {
        const fs::path path = findTable();
        CHECK(!path.empty());
        if (!path.empty()) {
            std::ifstream f(path, std::ios::binary);
            const std::string disk((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            const bool same = disk.size() == cascade::core::airbanddata::kTableLen &&
                              std::equal(disk.begin(), disk.end(),
                                         reinterpret_cast<const char*>(cascade::core::airbanddata::kTable));
            if (!same) {
                std::printf("      resources/airband/airband.tsv and core/airband_assets.hpp differ - "
                            "re-run py -3.14 tools/make-airband-data.py\n");
            }
            CHECK(same);
        }

        const std::vector<Airport>& t = airbandTable();
        CHECK(t.size() > 5000);
        std::size_t freqCount = 0;
        bool allInBand = true;
        bool allNonEmpty = true;
        for (const Airport& a : t) {
            freqCount += a.freqs.size();
            allNonEmpty = allNonEmpty && !a.freqs.empty();
            for (const AirbandFreq& q : a.freqs) {
                allInBand = allInBand && q.hz >= 108.0e6 && q.hz < 137.0e6;
            }
        }
        CHECK(freqCount > 20000);
        CHECK(allInBand);
        CHECK(allNonEmpty);
        // The parse of the shipped file skips nothing.
        std::size_t skipped = 99;
        parseAirbandTable(std::string_view(reinterpret_cast<const char*>(cascade::core::airbanddata::kTable),
                                           cascade::core::airbanddata::kTableLen),
                          &skipped);
        CHECK(skipped == 0);

        // THE REQUEST: every frequency the tester copied by hand for O'Hare.
        const std::vector<const Airport*> ord = findAirports(t, "ORD");
        CHECK(!ord.empty());
        if (!ord.empty()) {
            const Airport& a = *ord.front();
            CHECK(a.ident == "KORD");
            const int listed[] = {135400, 121600, 121750, 121900, 124125, 118050, 128150, 120750,
                                  121150, 126900, 132700, 133000, 125700, 119000, 124350, 133625,
                                  125400, 128575, 118275, 126625, 120550, 133500, 128200};
            std::size_t found = 0;
            for (const int khz : listed) {
                if (freqAt(a, khz) != nullptr) {
                    ++found;
                } else {
                    std::printf("      KORD lacks %d.%03d MHz\n", khz / 1000, khz % 1000);
                }
            }
            // The FAA's 2026-10-01 cycle does not list two of the tester's
            // (125.400 "Departure West" and 118.275 "Departure NE Loop");
            // the other twenty-one must all be there.
            CHECK(found >= 21);
            const AirbandFreq* gnd = freqAt(a, 121900);
            CHECK(gnd != nullptr && gnd->type == "GND");
        }
        // Outside the US the lookup works by IATA code too.
        const std::vector<const Airport*> lhr = findAirports(t, "LHR");
        CHECK(!lhr.empty() && lhr.front()->ident == "EGLL");
    }

    return testSummary("test_airband_data");
}
