// Tests for core/countries.hpp - the country table behind the Country picker.
//
// THE SHAPE: codes unique and two capital letters, every region 1-3, every
// band plan a file that exists in resources/bandplans (a plan id that names
// nothing would load as an error the moment a user picked that country),
// every plan consistent with its region, and names present.
//
// THE REGIONS: spot checks of the cases the brief called out and the table's
// derivation (countries.cpp) settles one by one from ITU RR Article 5 - the
// big three of each region, the named Region 1 exceptions east of line A,
// Iran, Greenland, Iceland, Hawaii's United States, and the Pacific south of
// 10 N. Antarctica must be absent.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/countries.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>

#include <vector>

#include "core/band_plan.hpp"
#include "core/i18n.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::Country;
using cascade::core::countries;
using cascade::core::findCountry;

namespace {

std::string findRoot() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    for (int level = 0; !ec && level < 10; ++level) {
        if (fs::is_regular_file(dir / "resources" / "bandplans" / "uk.json", ec)) {
            return dir.string();
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) { break; }
        dir = dir.parent_path();
    }
    return {};
}

void expect(const char* code, int region, const char* plan) {
    const Country* c = findCountry(code);
    if (c == nullptr) {
        std::printf("      %s is missing\n", code);
        CHECK(false);
        return;
    }
    if (c->ituRegion != region || std::strcmp(c->bandPlan, plan) != 0) {
        std::printf("      %s: region %d plan %s, expected region %d plan %s\n", code, c->ituRegion,
                    c->bandPlan, region, plan);
    }
    CHECK(c->ituRegion == region);
    CHECK(std::strcmp(c->bandPlan, plan) == 0);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc >= 2 ? std::string(argv[1]) : findRoot();
    const fs::path plans = fs::path(root) / "resources" / "bandplans";
    if (root.empty() || !fs::is_directory(plans)) {
        std::printf("cannot find resources/bandplans (pass the repository root)\n");
        CHECK(false);
        return testSummary("test_countries");
    }

    std::printf("  the table's shape\n");
    // 249 ISO 3166-1 alpha-2 codes are assigned (ISO 3166 Maintenance Agency;
    // Debian iso-codes as shipped by pycountry 26.2.16); Antarctica is left
    // out, which leaves 248.
    CHECK(countries().size() == 248);
    std::set<std::string> codes;
    for (const Country& c : countries()) {
        const bool codeOk = c.code != nullptr && std::strlen(c.code) == 2 && c.code[0] >= 'A' &&
                            c.code[0] <= 'Z' && c.code[1] >= 'A' && c.code[1] <= 'Z';
        if (!codeOk) { std::printf("      bad code: %s\n", c.code != nullptr ? c.code : "(null)"); }
        CHECK(codeOk);
        if (!codeOk) { continue; }
        if (!codes.insert(c.code).second) { std::printf("      duplicate code: %s\n", c.code); }
        const bool regionOk = c.ituRegion >= 1 && c.ituRegion <= 3;
        if (!regionOk) { std::printf("      %s: region %d\n", c.code, c.ituRegion); }
        CHECK(regionOk);
        CHECK(c.name != nullptr && c.name[0] != '\0');
        CHECK(c.language != nullptr && c.language[0] != '\0');
        const bool planExists =
            c.bandPlan != nullptr && fs::is_regular_file(plans / (std::string(c.bandPlan) + ".json"));
        if (!planExists) {
            std::printf("      %s: band plan \"%s\" is not in resources/bandplans\n", c.code,
                        c.bandPlan != nullptr ? c.bandPlan : "(null)");
        }
        CHECK(planExists);
        // A generic plan must be the country's own region's: "itu-region1"
        // for a Region 2 country would show allocations that are wrong there.
        if (c.bandPlan != nullptr && std::strncmp(c.bandPlan, "itu-region", 10) == 0) {
            const bool agrees = c.bandPlan[10] - '0' == c.ituRegion && c.bandPlan[11] == '\0';
            if (!agrees) { std::printf("      %s: region %d, plan %s\n", c.code, c.ituRegion, c.bandPlan); }
            CHECK(agrees);
        }
    }
    CHECK(codes.size() == countries().size());
    CHECK(findCountry("AQ") == nullptr);  // Antarctica: no region, no plan
    CHECK(findCountry("") == nullptr);    // the config's "not chosen"
    CHECK(findCountry("br") == nullptr);  // codes are exact, upper case
    CHECK(findCountry("XX") == nullptr);

    std::printf("  regions and plans, as derived from ITU RR Article 5\n");
    expect("BR", 2, "itu-region2");
    expect("GB", 1, "uk");
    expect("US", 2, "us");  // Hawaii included: east of line C
    expect("CA", 2, "canada");
    expect("AU", 3, "australia");
    expect("JP", 3, "japan");
    expect("DE", 1, "itu-region1");
    expect("IN", 3, "itu-region3");
    expect("IR", 3, "itu-region3");  // named: wholly Region 3
    expect("AF", 3, "itu-region3");  // east of 60 E
    expect("SA", 1, "itu-region1");
    expect("AE", 1, "itu-region1");
    expect("OM", 1, "itu-region1");
    expect("RU", 1, "itu-region1");  // named, east of line A
    expect("KZ", 1, "itu-region1");
    expect("MN", 1, "itu-region1");
    expect("GL", 2, "itu-region2");
    expect("IS", 1, "itu-region1");
    expect("CV", 1, "itu-region1");
    expect("BM", 2, "itu-region2");
    expect("KI", 3, "itu-region3");  // south of 10 N, west of 120 W
    expect("PF", 3, "itu-region3");
    expect("PN", 3, "itu-region3");
    expect("GU", 3, "itu-region3");
    expect("NZ", 3, "itu-region3");
    expect("ZA", 1, "itu-region1");
    expect("MX", 2, "itu-region2");

    std::printf("  suggested languages for the six catalogues' countries\n");
    const struct {
        const char* code;
        const char* lang;
    } langs[] = {{"BR", "pt-BR"}, {"ES", "es"}, {"MX", "es"}, {"FR", "fr"},
                 {"DE", "de"},    {"AT", "de"}, {"IT", "it"}, {"PL", "pl"}};
    for (const auto& l : langs) {
        const Country* c = findCountry(l.code);
        CHECK(c != nullptr && std::strcmp(c->language, l.lang) == 0);
    }

    // THE PLANS' NAMES ARE TRANSLATED WHERE THEY ARE DRAWN (band_plan.hpp,
    // displayPlanName). Every language showed "World (global allocations)" in
    // English (34-language review). Every shipped plan's name must be one the
    // key extractor sees - listed in shippedPlanNames - and a joined chain is
    // translated part by part.
    std::printf("  every shipped band plan's name is a translation key\n");
    const std::vector<cascade::core::PlanInfo> shipped = cascade::core::BandPlan::available(plans.string());
    CHECK(shipped.size() >= 9);
    for (const cascade::core::PlanInfo& p : shipped) {
        bool listed = false;
        for (const char* n : cascade::core::shippedPlanNames()) { listed = listed || p.name == n; }
        if (!listed) { std::printf("      %s: \"%s\" is not in shippedPlanNames\n", p.id.c_str(), p.name.c_str()); }
        CHECK(listed);
    }
    std::string error;
    CHECK(cascade::i18n::addCatalogue(
        R"({"code": "xx-plan", "name": "Plans", "englishName": "Plans", "strings": {)"
        R"("ITU Region 1": "R\u00e9gion UIT 1", "United Kingdom": "Royaume-Uni"}})",
        &error));
    cascade::i18n::setLanguage("xx-plan");
    CHECK(cascade::core::displayPlanName("ITU Region 1 + United Kingdom") ==
          "R\xC3\xA9gion UIT 1 + Royaume-Uni");
    CHECK(cascade::core::displayPlanName("United Kingdom") == "Royaume-Uni");
    CHECK(cascade::core::displayPlanName("My own plan") == "My own plan");  // in no catalogue
    CHECK(cascade::core::displayPlanName("") == "");
    cascade::i18n::setLanguage("en");
    CHECK(cascade::core::displayPlanName("ITU Region 1 + United Kingdom") ==
          "ITU Region 1 + United Kingdom");

    return testSummary("test_countries");
}
