// Tests for gui::trStoredReason and the two reason builders beside it
// (gui/plugin_store_view.hpp) - a reason kept in English, drawn translated.
//
// WHAT WENT WRONG. The plugin store's "Cannot fit:" line was translated only
// as far as its lead-in: "Nicht einbaubar: the legal notice must be
// acknowledged first". The reason is AppWindow::pluginInstallBlockedReason's
// English sentence, and it has to stay English where it is made - ADD ALL
// compares it, the log records it, the web page is handed it - so the fix is
// to translate it where it is DRAWN, and that is what these checks pin:
//
//   1. With English in force, every reason comes back BYTE FOR BYTE, and the
//      two made from a format are exactly the sentences the old string
//      concatenation produced - a log line, a web page and ADD ALL's
//      comparison see no change at all.
//   2. With a catalogue in force, a plain reason is its translation, and a
//      valued one (the ABI numbers, the platform) is re-made in the
//      translated format with the SAME values - while the English builders
//      go on making English, whatever language is chosen.
//   3. A sentence that only looks like one of the valued reasons, and one no
//      catalogue holds (PluginRepo's sha256 and I/O errors), come back exactly
//      as they went in.
//   4. The compiled-in catalogues really carry the reasons: in German and in
//      Brazilian Portuguese none of them is drawn in English.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "core/i18n.hpp"
#include "gui/plugin_store_view.hpp"
#include "test_check.hpp"

namespace {

using cascade::gui::pluginAbiMismatchReason;
using cascade::gui::pluginNoBuildReason;
using cascade::gui::trStoredReason;

// Every fixed reason the store stores in English, as AppWindow and the store
// view write them.
const std::vector<std::string> kFixed = {
    "no plugin selected",
    "a transfer is already in progress",
    "the catalogue entry declares no licence",
    "already installed",
    "the legal notice must be acknowledged first",
    "no catalogue entry with that id; fetch the catalogue and try again",
    "no longer in the catalogue",
};

const char* kTestCatalogue = R"j({"code":"qps-reason","name":"Reason Test",
  "englishName":"Reason Test","machine":true,
  "strings":{"already installed":"ya instalado",
             "the legal notice must be acknowledged first":"acepte primero el aviso",
             "not compatible with this version (built for plugin ABI %u, this build requires exactly %u)":"incompatible (ABI %u, se requiere %u)",
             "no build for %s":"sin compilación para %s"}})j";

void testEnglishIsByteIdentical() {
    std::printf("  English: every reason comes back byte for byte\n");
    CHECK(cascade::i18n::setLanguage("en") == "en");
    for (const std::string& r : kFixed) { CHECK(trStoredReason(r) == r); }
    // The exact sentences the old concatenation built, pinned by hand.
    const std::string abi = pluginAbiMismatchReason(2u, 3u);
    CHECK(abi == "not compatible with this version (built for plugin ABI " + std::to_string(2) +
                     ", this build requires exactly " + std::to_string(3) + ")");
    CHECK(trStoredReason(abi) == abi);
    const std::string noBuild = pluginNoBuildReason("windows/x64");
    CHECK(noBuild == std::string("no build for ") + "windows" + "/" + "x64");
    CHECK(trStoredReason(noBuild) == noBuild);
    CHECK(trStoredReason(std::string()).empty());
}

void testTranslatedAtTheDrawSite() {
    std::printf("  a catalogue in force: plain and valued reasons drawn translated\n");
    std::string err;
    CHECK(cascade::i18n::addCatalogue(kTestCatalogue, &err));
    CHECK(err.empty());
    CHECK(cascade::i18n::setLanguage("qps-reason") == "qps-reason");

    CHECK(trStoredReason("already installed") == "ya instalado");
    CHECK(trStoredReason("the legal notice must be acknowledged first") ==
          "acepte primero el aviso");
    // The values survive into the translated sentence, in their places.
    CHECK(trStoredReason(pluginAbiMismatchReason(2u, 3u)) == "incompatible (ABI 2, se requiere 3)");
    CHECK(trStoredReason(pluginAbiMismatchReason(41u, 1000u)) ==
          "incompatible (ABI 41, se requiere 1000)");
    CHECK(trStoredReason(pluginNoBuildReason("linux/arm64")) ==
          "sin compilación para linux/arm64");

    // THE ENGLISH IS STILL ENGLISH where it is made: what ADD ALL compares
    // and the log records does not follow the language.
    CHECK(pluginAbiMismatchReason(2u, 3u) ==
          "not compatible with this version (built for plugin ABI 2, this build requires "
          "exactly 3)");
    CHECK(pluginNoBuildReason("linux/arm64") == "no build for linux/arm64");

    // A reason this catalogue lacks is drawn in English, not dropped.
    CHECK(trStoredReason("no plugin selected") == "no plugin selected");
}

void testLookalikesAndForeignWordsUntouched() {
    std::printf("  a lookalike, and PluginRepo's own words, come back as written\n");
    CHECK(cascade::i18n::setLanguage("qps-reason") == "qps-reason");
    const std::string tail =
        "not compatible with this version (built for plugin ABI 2, this build requires "
        "exactly 3) - and more";
    CHECK(trStoredReason(tail) == tail);
    const std::string noNumber =
        "not compatible with this version (built for plugin ABI x, this build requires "
        "exactly 3)";
    CHECK(trStoredReason(noNumber) == noNumber);
    CHECK(trStoredReason("no build for ") == "no build for ");
    const std::string repo =
        "sha256 mismatch: expected 00ff..., got 1234... - the bytes were not the ones the "
        "catalogue published";
    CHECK(trStoredReason(repo) == repo);
    // A percent sign in a platform is a value, not a format.
    CHECK(trStoredReason(pluginNoBuildReason("win%s/x64")) == "sin compilación para win%s/x64");
}

void testCompiledInCataloguesCarryTheReasons() {
    std::printf("  de and pt-BR: no stored reason is drawn in English\n");
    for (const char* code : {"de", "pt-BR"}) {
        CHECK(cascade::i18n::setLanguage(code) == code);
        for (const std::string& r : kFixed) {
            const std::string shown = trStoredReason(r);
            if (shown == r) { std::printf("    %s: still English: %s\n", code, r.c_str()); }
            CHECK(shown != r);
        }
        const std::string abi = trStoredReason(pluginAbiMismatchReason(2u, 3u));
        CHECK(abi != pluginAbiMismatchReason(2u, 3u));
        CHECK(abi.find('2') != std::string::npos && abi.find('3') != std::string::npos);
        const std::string noBuild = trStoredReason(pluginNoBuildReason("windows/x64"));
        CHECK(noBuild != pluginNoBuildReason("windows/x64"));
        CHECK(noBuild.find("windows/x64") != std::string::npos);
    }
    CHECK(cascade::i18n::setLanguage("en") == "en");
}

}  // namespace

int main() {
    testEnglishIsByteIdentical();
    testTranslatedAtTheDrawSite();
    testLookalikesAndForeignWordsUntouched();
    testCompiledInCataloguesCarryTheReasons();
    return testSummary("test_stored_reason_tr");
}
