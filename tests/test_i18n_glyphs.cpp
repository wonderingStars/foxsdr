// Every letter a translation uses must be IN the typefaces the bench draws
// with. ImGui does not fall back from one face to another: a code point the
// face lacks is drawn as the face's fallback box, so with no ł in the font
// "Wyłącz" reads "Wy□ącz" - a translation that looks broken, on exactly the
// machines of the people it was made for.
//
// WHAT IS CHECKED: every code point in every resources/lang/*.json (names and
// translations), every country name in the English table (drawn as-is when
// English is chosen), and a fixed probe of the six first languages' special
// letters - which is what makes this test mean something before the first
// catalogue exists, and keeps meaning it after.
//
// AGAINST WHICH FACES: the three EMBEDDED faces - UI, legend and reading - by
// their bytes (gui/font_blobs.hpp), because those are what every platform can
// fall back to. And, where this machine has them, the Georgia pair fonts.cpp
// prefers on Windows (0.84.0): a letter Georgia lacks is a letter every
// Windows user sees as a box. Absent (Linux), that half is reported as
// skipped, never as passed.
//
// The lookup is ImFont::IsGlyphInFont, which asks each font source's loader
// whether the TTF's cmap maps the code point - no rasterising, no atlas.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/countries.hpp"
#include "gui/font_blobs.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

// The special letters of Portuguese, Spanish, French, German, Italian and
// Polish, plus the Spanish inverted marks.
const char* const kProbe =
    "ãõçáéíóúâêôàèùœëîïûüäößñ¿¡ąćęłńśźżŁŚŻĆ"
    "ÃÕÇÁÉÍÓÚÂÊÔÀÈÙŒËÎÏÛÜÄÖÑĄĘŃŹ";

std::string findRoot() {
    std::error_code ec;
    fs::path dir = fs::current_path(ec);
    for (int level = 0; !ec && level < 10; ++level) {
        if (fs::is_directory(dir / "resources" / "bandplans", ec)) { return dir.string(); }
        if (!dir.has_parent_path() || dir.parent_path() == dir) { break; }
        dir = dir.parent_path();
    }
    return {};
}

// Decodes UTF-8 into code points, adding each to `out` with where it came
// from (the first place seen), so a failure can say which string needs it.
void collect(const std::string& s, const std::string& where,
             std::map<unsigned int, std::string>& out) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned int cp = c;
        std::size_t len = 1;
        if (c >= 0xC0 && c < 0xE0) {
            cp = c & 0x1Fu;
            len = 2;
        } else if (c >= 0xE0 && c < 0xF0) {
            cp = c & 0x0Fu;
            len = 3;
        } else if (c >= 0xF0) {
            cp = c & 0x07u;
            len = 4;
        }
        if (i + len > s.size()) { break; }
        for (std::size_t k = 1; k < len; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        }
        i += len;
        if (cp < 0x20) { continue; }  // newlines and tabs are not drawn as glyphs
        out.emplace(cp, where);
    }
}

ImFont* addBlob(const unsigned char* data, std::size_t len, const char* name) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // constexpr storage: never free()d
    std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", name);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data),
                                                      static_cast<int>(len), 16.0f, &cfg);
}

void checkFace(ImFont* font, const char* face, const std::map<unsigned int, std::string>& cps) {
    CHECK(font != nullptr);
    if (font == nullptr) { return; }
    std::size_t missing = 0;
    for (const auto& [cp, where] : cps) {
        // ImWchar is 16 bits in this build (no IMGUI_USE_WCHAR32), so a code
        // point past the BMP cannot be drawn at all - which is itself missing.
        const bool have = cp <= 0xFFFF && font->IsGlyphInFont(static_cast<ImWchar>(cp));
        if (!have) {
            std::string ch;
            if (cp < 0x80) {
                ch.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                ch.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                ch.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                ch.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                ch.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                ch.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
            std::printf("      %s face lacks U+%04X '%s' (first used in: %s)\n", face, cp, ch.c_str(),
                        where.c_str());
            ++missing;
        }
    }
    std::printf("    %s: %zu of %zu code points present\n", face, cps.size() - missing, cps.size());
    CHECK(missing == 0);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc >= 2 ? std::string(argv[1]) : findRoot();
    if (root.empty()) {
        std::printf("cannot find the repository root (pass it as the first argument)\n");
        CHECK(false);
        return testSummary("test_i18n_glyphs");
    }

    std::map<unsigned int, std::string> cps;
    collect(kProbe, "the six languages' letter probe", cps);
    for (const cascade::core::Country& c : cascade::core::countries()) {
        collect(c.name, std::string("country name ") + c.code, cps);
    }
    std::size_t catalogues = 0;
    const fs::path dir = fs::path(root) / "resources" / "lang";
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (!e.is_regular_file() || e.path().extension() != ".json") { continue; }
            std::ifstream f(e.path(), std::ios::binary);
            const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
            const std::string file = e.path().filename().string();
            CHECK(!j.is_discarded() && j.is_object());  // test_i18n says why in detail
            if (j.is_discarded() || !j.is_object()) { continue; }
            ++catalogues;
            collect(j.value("name", std::string()), file + " name", cps);
            collect(j.value("englishName", std::string()), file + " englishName", cps);
            const auto s = j.find("strings");
            if (s == j.end() || !s->is_object()) { continue; }
            for (auto it = s->begin(); it != s->end(); ++it) {
                if (it.value().is_string()) {
                    collect(it.value().get<std::string>(), file + " \"" + it.key() + "\"", cps);
                }
            }
        }
    }
    std::printf("  %zu catalogues, %zu distinct code points to find\n", catalogues, cps.size());

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 600.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    std::printf("  the embedded faces\n");
    ImFont* ui = addBlob(cascade::gui::fonts::uiTtf(), cascade::gui::fonts::uiTtfLen(), "ui");
    ImFont* legend =
        addBlob(cascade::gui::fonts::legendTtf(), cascade::gui::fonts::legendTtfLen(), "legend");
    ImFont* reading =
        addBlob(cascade::gui::fonts::readingTtf(), cascade::gui::fonts::readingTtfLen(), "reading");
    checkFace(ui, "UI (Saira Condensed Medium)", cps);
    checkFace(legend, "legend (Saira Condensed SemiBold)", cps);
    checkFace(reading, "reading (Nova Mono)", cps);

    std::printf("  the system Georgia pair fonts.cpp prefers where it exists\n");
    for (const char* file : {"georgia.ttf", "georgiab.ttf"}) {
        const std::string path = cascade::gui::fonts::systemFontPath(file);
        if (path.empty() || !fs::is_regular_file(path)) {
            std::printf("    %s: not on this machine - skipped, not passed\n", file);
            continue;
        }
        ImFontConfig cfg;
        std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", file);
        ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), 16.0f, &cfg);
        checkFace(f, file, cps);
    }

    ImGui::DestroyContext();
    return testSummary("test_i18n_glyphs");
}
