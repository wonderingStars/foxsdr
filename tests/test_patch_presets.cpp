// Tests for core/patch_presets.hpp - the patch page's named presets - and for
// their round trip through config.json.
//
// THE STORE: save, overwrite only when told, rename, delete, names matched
// without regard to case, every name rule (control characters, trimming, the
// 64-byte cut on a character boundary, empty, the reserved slot's name), both
// caps (100 presets, 256 KB a text), a text that is not a patch refused, and
// the previous-patch slot kept apart from the list and its count.
//
// THE CONFIG: presets survive ConfigStore::save and load exactly; a malformed
// entry is dropped with one line in the log and neither the rest of the list
// nor the rest of the config goes with it; a text parse() does not read is
// dropped at load.
//
// Hermetic: every file is written under a scratch folder of its own.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_io.hpp"
#include "core/patch_presets.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;
using cascade::core::PatchPreset;
using cascade::core::PatchPresetStatus;
using cascade::core::PatchPresetStore;

namespace {

std::string g_root;
std::string p(const char* name) { return (fs::path(g_root) / name).string(); }

// A real patch document, as the page would save it: a radio wired to a
// channel. `hz` makes each one different.
std::string patchText(double hz) {
    namespace pc = cascade::core::patch;
    pc::Graph g;
    const pc::NodeId r = g.addNode(pc::NodeKind::Radio, "Radio");
    const pc::NodeId c = g.addNode(pc::NodeKind::Channel, "Channel", pc::PortType::Iq, 200.0f, 0.0f);
    if (pc::Node* n = g.mutableNode(r)) { n->device = "rtlsdr|serial=00000001"; }
    if (pc::Node* n = g.mutableNode(c)) { n->freqHz = hz; }
    g.connect(r, 0, c, 0);
    return pc::serialise(g, 10.0f, 20.0f, 1.5f);
}

// How many lines the application log gained that mention `what`.
std::size_t logLinesWith(const char* what) {
    std::size_t n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(what) != std::string::npos) { ++n; }
    }
    return n;
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

void testNames() {
    std::printf("  names: cleaned, trimmed, cut on a character, matched without case\n");
    using cascade::core::cleanPatchPresetName;
    using cascade::core::patchPresetNamesMatch;
    CHECK(cleanPatchPresetName("  Airband  ") == "Airband");
    CHECK(cleanPatchPresetName("Air\tband\n\x7F") == "Airband");
    // A C1 control (U+0085, NEXT LINE) is a control character too.
    CHECK(cleanPatchPresetName("A\xC2\x85" "B") == "AB");
    // Bytes that are no whole character go; whole ones stay.
    CHECK(cleanPatchPresetName("A\xFF\xC3" "B") == "AB");
    CHECK(cleanPatchPresetName("Łódź") == "Łódź");
    CHECK(cleanPatchPresetName("   ").empty());
    CHECK(cleanPatchPresetName("\n\t").empty());

    // The 64-byte cap, never splitting a character: 63 ASCII bytes and a
    // two-byte letter would be 65, so the letter goes whole.
    const std::string ascii63(63, 'a');
    const std::string cut = cleanPatchPresetName(ascii63 + "ł" + "tail");
    CHECK(cut == ascii63);
    CHECK(cut.size() <= cascade::core::kMaxPatchPresetNameBytes);
    const std::string exact(64, 'b');
    CHECK(cleanPatchPresetName(exact + "c") == exact);
    // A cut that lands after a space leaves no trailing space.
    CHECK(cleanPatchPresetName(std::string(63, 'd') + " efg") == std::string(63, 'd'));
    // Twenty-two three-byte characters are 66 bytes: 21 fit.
    std::string han;
    for (int i = 0; i < 22; ++i) { han += "中"; }
    const std::string hanCut = cleanPatchPresetName(han);
    CHECK(hanCut.size() == 63u);

    CHECK(patchPresetNamesMatch("Airband", "AIRBAND"));
    CHECK(patchPresetNamesMatch("  airband ", "AirBand"));
    CHECK(patchPresetNamesMatch("ŁÓDŹ", "łódź"));
    CHECK(patchPresetNamesMatch("ПРИВЕТ", "привет"));
    CHECK(!patchPresetNamesMatch("Cafe", "Café"));
    CHECK(!patchPresetNamesMatch("Airband", "Airband 2"));
}

void testStore() {
    std::printf("  the store: save, overwrite only when told, rename, delete\n");
    PatchPresetStore s;
    const std::string a = patchText(118.0e6);
    const std::string b = patchText(121.5e6);

    CHECK(s.save("Airband", a, false) == PatchPresetStatus::Saved);
    CHECK(s.list().size() == 1u);
    CHECK(s.list()[0].name == "Airband");
    CHECK(s.list()[0].text == a);

    // The same name in another case is the same preset: refused unless told.
    CHECK(s.save("AIRBAND", b, false) == PatchPresetStatus::Exists);
    CHECK(s.list().size() == 1u);
    CHECK(s.list()[0].text == a);
    CHECK(s.save("  AIRBAND ", b, true) == PatchPresetStatus::Overwritten);
    CHECK(s.list().size() == 1u);
    CHECK(s.list()[0].text == b);
    CHECK(s.list()[0].name == "AIRBAND");  // the spelling last chosen
    CHECK(s.find("airband") == 0);

    // Sorted by name without regard to case.
    CHECK(s.save("marine", a, false) == PatchPresetStatus::Saved);
    CHECK(s.save("Broadcast", a, false) == PatchPresetStatus::Saved);
    CHECK(s.list().size() == 3u);
    CHECK(s.list()[0].name == "AIRBAND");
    CHECK(s.list()[1].name == "Broadcast");
    CHECK(s.list()[2].name == "marine");

    // Refusals, each leaving the list alone.
    CHECK(s.save("   ", a, false) == PatchPresetStatus::EmptyName);
    CHECK(s.save("(Previous Patch)", a, true) == PatchPresetStatus::ReservedName);
    CHECK(s.save("junk", "not a patch at all", false) == PatchPresetStatus::NotAPatch);
    CHECK(s.save("junk", "", false) == PatchPresetStatus::NotAPatch);
    CHECK(s.list().size() == 3u);

    // Rename: a case change alone is a rename; another preset's name is not
    // an overwrite.
    CHECK(s.rename("marine", "Marine") == PatchPresetStatus::Renamed);
    CHECK(s.find("MARINE") >= 0);
    CHECK(s.list()[static_cast<std::size_t>(s.find("marine"))].name == "Marine");
    CHECK(s.rename("Marine", "broadcast") == PatchPresetStatus::Exists);
    CHECK(s.rename("Marine", " ") == PatchPresetStatus::EmptyName);
    CHECK(s.rename("Marine", "(previous patch)") == PatchPresetStatus::ReservedName);
    CHECK(s.rename("nothing", "x") == PatchPresetStatus::NotFound);
    CHECK(s.rename("Marine", "Sea") == PatchPresetStatus::Renamed);
    CHECK(s.find("Marine") < 0);
    CHECK(s.find("sea") >= 0);
    CHECK(s.list().size() == 3u);

    // Delete.
    CHECK(s.remove("SEA") == PatchPresetStatus::Deleted);
    CHECK(s.find("Sea") < 0);
    CHECK(s.list().size() == 2u);
    CHECK(s.remove("Sea") == PatchPresetStatus::NotFound);
}

void testCaps() {
    std::printf("  the caps: %zu presets, %zu bytes a text\n", cascade::core::kMaxPatchPresets,
                cascade::core::kMaxPatchPresetTextBytes);
    PatchPresetStore s;
    const std::string t = patchText(100.0e6);
    for (std::size_t i = 0; i < cascade::core::kMaxPatchPresets; ++i) {
        CHECK(s.save("Preset " + std::to_string(i), t, false) == PatchPresetStatus::Saved);
    }
    CHECK(s.list().size() == cascade::core::kMaxPatchPresets);
    CHECK(s.save("One too many", t, false) == PatchPresetStatus::ListFull);
    CHECK(s.list().size() == cascade::core::kMaxPatchPresets);
    // A full list still takes an overwrite: it adds nothing.
    CHECK(s.save("preset 7", patchText(1.0e6), true) == PatchPresetStatus::Overwritten);
    // The previous-patch slot is not one of the hundred.
    CHECK(s.keepPrevious(t));
    CHECK(s.previous() == t);
    CHECK(s.list().size() == cascade::core::kMaxPatchPresets);

    // The text cap: a comment line of padding keeps it a patch, so only the
    // size can be what refuses it. Exactly at the cap is allowed.
    PatchPresetStore big;
    std::string atCap = t;
    atCap += "# ";
    atCap += std::string(cascade::core::kMaxPatchPresetTextBytes - atCap.size() - 1, 'x');
    atCap += "\n";
    CHECK(atCap.size() == cascade::core::kMaxPatchPresetTextBytes);
    CHECK(big.save("At the cap", atCap, false) == PatchPresetStatus::Saved);
    const std::string overCap = atCap + "x";
    CHECK(big.save("Over the cap", overCap, false) == PatchPresetStatus::TooLarge);
    CHECK(big.list().size() == 1u);
    CHECK(!big.keepPrevious(overCap));
    CHECK(!big.keepPrevious("not a patch"));
    CHECK(big.previous().empty());
}

void testConfigRoundTrip() {
    std::printf("  presets and the previous slot survive a config save and load\n");
    AppConfig in;
    in.patchPresets = {PatchPreset{"Airband", patchText(118.0e6)},
                       PatchPreset{"Łódź nights", patchText(145.5e6)},
                       PatchPreset{"marine", patchText(156.8e6)}};
    in.patchPresetPrevious = patchText(99.9e6);
    std::string err;
    const std::string path = p("round_trip.json");
    CHECK(ConfigStore::save(path, in, err));
    AppConfig out;
    CHECK(ConfigStore::load(path, out, err));
    CHECK(err.empty());
    CHECK(out.patchPresets == in.patchPresets);
    CHECK(out.patchPresetPrevious == in.patchPresetPrevious);

    // A config with no presets at all loads as none.
    AppConfig none;
    CHECK(ConfigStore::save(p("none.json"), none, err));
    AppConfig outNone;
    CHECK(ConfigStore::load(p("none.json"), outNone, err));
    CHECK(outNone.patchPresets.empty());
    CHECK(outNone.patchPresetPrevious.empty());
}

void testMalformedEntries() {
    std::printf("  a malformed or unreadable entry is dropped, logged, and nothing else is\n");
    cascade::core::DiagLog::instance().resetForTest();
    const std::string good = patchText(118.0e6);
    // JSON-escaped by hand: the patch text holds newlines.
    std::string esc;
    for (const char c : good) {
        if (c == '\n') {
            esc += "\\n";
        } else {
            esc.push_back(c);
        }
    }
    const std::string doc =
        "{\n"
        "  \"volume\": 0.25,\n"
        "  \"patchPresets\": [\n"
        "    5,\n"
        "    \"just a string\",\n"
        "    {\"name\": 1, \"text\": \"" + esc + "\"},\n"
        "    {\"name\": \"no text\"},\n"
        "    {\"name\": \"Good\", \"text\": \"" + esc + "\"},\n"
        "    {\"name\": \"Not a patch\", \"text\": \"hello world\\nnode 1 2 3\"},\n"
        "    {\"name\": \"GOOD\", \"text\": \"" + esc + "\"},\n"
        "    {\"name\": \"  \\t \", \"text\": \"" + esc + "\"},\n"
        "    {\"name\": \"(previous patch)\", \"text\": \"" + esc + "\"},\n"
        "    {\"name\": \"  Second\\u0007 \", \"text\": \"" + esc + "\"}\n"
        "  ],\n"
        "  \"patchPresetPrevious\": \"garbage\",\n"
        "  \"mainView\": \"receiver\"\n"
        "}\n";
    writeFile(p("malformed.json"), doc);
    AppConfig out;
    std::string err;
    CHECK(ConfigStore::load(p("malformed.json"), out, err));
    CHECK(err.empty());
    // Only the two good entries, the second's name cleaned.
    CHECK(out.patchPresets.size() == 2u);
    if (out.patchPresets.size() == 2u) {
        CHECK(out.patchPresets[0].name == "Good");
        CHECK(out.patchPresets[0].text == good);
        CHECK(out.patchPresets[1].name == "Second");
    }
    CHECK(out.patchPresetPrevious.empty());
    // The rest of the config loaded around them.
    CHECK(out.volume == 0.25f);
    CHECK(out.mainView == "receiver");
    // One line per dropped entry (eight) and one for the previous slot.
    CHECK(logLinesWith("patch preset") == 8u);
    CHECK(logLinesWith("previous-patch slot") == 1u);
    CHECK(logLinesWith("patch preset 5 dropped - its text is not a patch") == 1u);

    // A list that is not a list at all: ignored, logged, config still loads.
    cascade::core::DiagLog::instance().resetForTest();
    writeFile(p("not_a_list.json"), "{\"patchPresets\": {\"a\": 1}, \"volume\": 0.5}\n");
    AppConfig out2;
    CHECK(ConfigStore::load(p("not_a_list.json"), out2, err));
    CHECK(out2.patchPresets.empty());
    CHECK(out2.volume == 0.5f);
    CHECK(logLinesWith("patchPresets is not a list") == 1u);

    // More than the cap in the file: the first hundred are kept.
    std::vector<PatchPreset> many;
    for (int i = 0; i < 105; ++i) { many.push_back(PatchPreset{"P" + std::to_string(i), good}); }
    const std::vector<PatchPreset> kept = cascade::core::sanitisePatchPresets(many);
    CHECK(kept.size() == cascade::core::kMaxPatchPresets);
    CHECK(kept.back().name == "P99");
}

void testUnparseableDropped() {
    std::printf("  a preset whose text parse() cannot read is dropped at load\n");
    cascade::core::DiagLog::instance().resetForTest();
    AppConfig in;
    in.patchPresets = {PatchPreset{"Fine", patchText(1.0e6)},
                       PatchPreset{"Broken", "foxsdr-notapatch 6\nnode 1\n"},
                       PatchPreset{"Empty", ""}};
    std::string err;
    CHECK(ConfigStore::save(p("unparseable.json"), in, err));
    AppConfig out;
    CHECK(ConfigStore::load(p("unparseable.json"), out, err));
    CHECK(out.patchPresets.size() == 1u);
    CHECK(!out.patchPresets.empty() && out.patchPresets[0].name == "Fine");
    CHECK(logLinesWith("is not a patch") == 2u);
}

}  // namespace

int main() {
    g_root = (fs::temp_directory_path() / ("patch_presets_test_" + std::to_string(TEST_GETPID())))
                 .string();
    fs::remove_all(g_root);
    fs::create_directory(g_root);

    testNames();
    testStore();
    testCaps();
    testConfigRoundTrip();
    testMalformedEntries();
    testUnparseableDropped();

    fs::remove_all(g_root);
    return testSummary("test_patch_presets");
}
