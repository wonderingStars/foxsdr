// Tests for gui/fonts.hpp's FACE CHAIN in a real ImGui atlas: which faces
// each role is built from, that English is drawn exactly as the primary face
// alone draws it, that switching language between frames rebuilds or extends
// the atlas as it should, that Japanese kanji come from a Japanese face even
// after the language list has loaded a Chinese one, and that a long CJK
// sentence with no spaces wraps inside its width.
//
// The CJK halves need a system CJK face. On a machine without one (a Linux
// desktop without fonts-noto-cjk) they are reported and counted as SKIPPED,
// never as passed - and the machine's answer to "can this be drawn" is then
// checked to be no.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/i18n.hpp"
#include "core/scripts.hpp"
#include "core/utf8_text.hpp"
#include "gui/font_probe.hpp"
#include "gui/fonts.hpp"
#include "gui/text_fit.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // ImFontCalcWordWrapPositionEx: the wrap ImGui draws with
#include "test_check.hpp"

namespace fonts = cascade::gui::fonts;
namespace fontprobe = cascade::gui::fontprobe;
namespace scripts = cascade::core::scripts;

namespace {

void skip(const char* why) {
    ++g_checksSkipped;
    std::printf("    SKIP: %s\n", why);
}

// One frame, so the atlas is exercised the way the application uses it.
void frame() {
    ImGui::NewFrame();
    ImGui::Begin("probe");
    ImGui::TextUnformatted("FoxSDR 145.800 MHz");
    ImGui::End();
    ImGui::Render();
}

void addProbe(const char* code, const char* englishName, const char* text) {
    const nlohmann::json doc = {{"code", code},
                                {"name", text},
                                {"englishName", englishName},
                                {"machine", true},
                                {"strings", {{"Band plan", text}}}};
    std::string error;
    CHECK(cascade::i18n::addCatalogue(doc.dump(), &error));
}

// Switches the chain the way applyPendingLanguage does, between frames.
bool switchTo(const char* code) {
    fonts::applyLanguage(code);
    const bool changed = fonts::applyPending();
    frame();
    return changed;
}

std::string labels() {
    std::string s;
    for (const std::string& l : fonts::uiChainLabels()) { s += (s.empty() ? "" : " > ") + l; }
    return s;
}

int indexOf(const std::string& prefix) {
    const std::vector<std::string> l = fonts::uiChainLabels();
    for (std::size_t i = 0; i < l.size(); ++i) {
        if (l[i].rfind(prefix, 0) == 0) { return static_cast<int>(i); }
    }
    return -1;
}

void testEnglishChain() {
    std::printf("  English: the primary face first, the Noto fallback behind it\n");
    const std::vector<std::string> l = fonts::uiChainLabels();
    std::printf("    UI chain: %s\n", labels().c_str());
    CHECK(l.size() == 2);
    CHECK(!l.empty() && l[0] == (fonts::usingSystemSerif() ? "Georgia" : "Saira Condensed Medium"));
    CHECK(l.size() > 1 && l[1] == "Noto Sans Condensed Medium");
    CHECK(fonts::pairInUse() == (fonts::usingSystemSerif() ? "Georgia" : "Saira Condensed"));
}

// ENGLISH IS NOT MOVED BY A PIXEL: every string below measures the same in
// the chained UI font as in a font made of the primary face alone. The
// fallback is only ever asked for a glyph the primary lacks.
void testEnglishUnchanged() {
    std::printf("  English measures exactly as the primary face alone measures it\n");
    ImFont* chained = fonts::ui();
    const fonts::Chain en = fonts::planChain("en");
    CHECK(!en.ui.empty());
    if (en.ui.empty()) { return; }
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    std::snprintf(cfg.Name, sizeof(cfg.Name), "primary alone");
    ImFont* alone = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(en.ui.front().data), static_cast<int>(en.ui.front().len),
        fonts::kUiSize, &cfg);
    CHECK(alone != nullptr);
    if (alone == nullptr) { return; }
    const char* samples[] = {
        "FUNCTION SELECT", "Band plan", "145.800 MHz  -12.5 dB", "Portugu\xC3\xAAs (Brasil)",
        "Wy\xC5\x82\xC4\x85" "cz", "C\xC3\xB4te d'Ivoire, S\xC3\xA3o Tom\xC3\xA9, \xC3\x85land",
        "The quick brown fox jumps over the lazy dog 0123456789 !?%&()[]{}<>/\\|@#$^*_+=~`'\""};
    for (const char* s : samples) {
        for (const float px : {fonts::kTinySize, fonts::kUiSize, fonts::kPanelSize}) {
            const ImVec2 a = chained->CalcTextSizeA(px, FLT_MAX, 0.0f, s);
            const ImVec2 b = alone->CalcTextSizeA(px, FLT_MAX, 0.0f, s);
            if (a.x != b.x || a.y != b.y) {
                std::printf("      \"%s\" at %.0f px: chained %.2fx%.2f, alone %.2fx%.2f\n", s, px,
                            a.x, a.y, b.x, b.y);
            }
            CHECK(a.x == b.x && a.y == b.y);
        }
    }
}

// A FALLBACK LETTER IS THE SIZE OF THE LETTERS BESIDE IT: every face behind
// the first is drawn at the em of the face English is lettered in.
void testFallbackEm() {
    std::printf("  every fallback source is drawn at the reference face's em\n");
    ImFont* f = fonts::ui();
    CHECK(f->Sources.Size >= 2);
    if (f->Sources.Size < 2) { return; }
    const ImFontConfig* first = f->Sources[0];
    const float refSpan = fontprobe::emSpan(
        {static_cast<const unsigned char*>(first->FontData), static_cast<std::size_t>(first->FontDataSize),
         static_cast<int>(first->FontNo)});
    CHECK(first->ExtraSizeScale == 1.0f);
    for (int i = 1; i < f->Sources.Size; ++i) {
        const ImFontConfig* s = f->Sources[i];
        const float span = fontprobe::emSpan(
            {static_cast<const unsigned char*>(s->FontData), static_cast<std::size_t>(s->FontDataSize),
             static_cast<int>(s->FontNo)});
        const float want = span / refSpan;
        std::printf("    %s: scale %.4f (ems %.4f against %.4f)\n", s->Name, s->ExtraSizeScale, span,
                    refSpan);
        CHECK(std::fabs(s->ExtraSizeScale - want) < 1e-4f);
    }
}

void testLatinAndCyrillicSwitches() {
    std::printf("  a language the primary pair cannot letter whole moves to a pair that can\n");
    addProbe("vi-probe", "Vietnamese", "Ti\xE1\xBA\xBFng Vi\xE1\xBB\x87t, ng\xC6\xB0\xE1\xBB\x9Di d\xC3\xB9ng, \xC6\xA1");
    addProbe("ru-probe", "Russian", "\xD0\x9D\xD0\xB0\xD1\x81\xD1\x82\xD1\x80\xD0\xBE\xD0\xB9\xD0\xBA\xD0\xB8 \xD1\x91");
    // Vietnamese: Georgia lacks its stacked vowels, Saira has them all.
    CHECK(switchTo("vi-probe") == fonts::usingSystemSerif());  // a rebuild only where Georgia was
    std::printf("    vi: %s\n", labels().c_str());
    CHECK(fonts::pairInUse() == "Saira Condensed");
    CHECK(fonts::ui()->IsGlyphInFont(0x1EC7));  // ệ
    // Russian: Georgia has Cyrillic; Saira has none, so without Georgia the
    // whole interface is lettered in Noto Sans.
    switchTo("ru-probe");
    std::printf("    ru: %s\n", labels().c_str());
    CHECK(fonts::pairInUse() == (fonts::usingSystemSerif() ? "Georgia" : "Noto Sans Condensed"));
    CHECK(fonts::ui()->IsGlyphInFont(0x0416));  // Ж
    CHECK(fonts::legend()->IsGlyphInFont(0x0416));
    CHECK(fonts::reading()->IsGlyphInFont(0x0416));
    // And back: English is exactly the chain load() made.
    switchTo("en");
    CHECK(fonts::uiChainLabels().size() == 2);
    CHECK(fonts::pairInUse() == (fonts::usingSystemSerif() ? "Georgia" : "Saira Condensed"));
}

bool haveCjk(const char* code) {
    if (fonts::canDraw(code)) { return true; }
    std::printf("    %s: %s\n", code, fonts::unavailableReason(code));
    CHECK(std::strlen(fonts::unavailableReason(code)) > 0);
    CHECK(cascade::i18n::resolveFor(code, "") == "en");  // never applied into boxes
    return false;
}

void testCjkLazyAndOrdered() {
    std::printf("  CJK: loaded only when asked for, and the language's own face first\n");
    addProbe("zh-CN-probe", "Chinese (Simplified)", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87");  // 简体中文
    addProbe("zh-TW-probe", "Chinese (Traditional)", "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87");  // 繁體中文
    addProbe("ja-probe", "Japanese", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");                 // 日本語
    addProbe("ko-probe", "Korean", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4");                 // 한국어
    // English carries none of them until something asks.
    CHECK(fonts::uiChainLabels().size() == 2);
    CHECK(!fonts::ui()->IsGlyphInFont(0x65E5));  // 日
    // Each part runs for the standards this machine has a face for, and says
    // so for the rest: a Linux box may have a Chinese face and no Korean one.
    const bool ja = haveCjk("ja-probe");
    const bool sc = haveCjk("zh-CN-probe");
    const bool tc = haveCjk("zh-TW-probe");
    const bool ko = haveCjk("ko-probe");
    if (!ja && !sc && !tc && !ko) {
        skip("no system CJK face on this machine - the CJK atlas half was not run");
        return;
    }
    // The language list on screen: the names' faces are merged BEHIND the
    // English chain, and the English primary still comes first.
    fonts::requestLanguageNames();
    CHECK(fonts::applyPending());
    frame();
    std::printf("    en + names: %s\n", labels().c_str());
    CHECK(indexOf(fonts::usingSystemSerif() ? "Georgia" : "Saira") == 0);
    if (ja) { CHECK(fonts::ui()->IsGlyphInFont(0x65E5)); }  // 日, for the name 日本語
    if (ko) { CHECK(fonts::ui()->IsGlyphInFont(0xD55C)); }  // 한, for 한국어
    // Japanese after that: its own face must come AHEAD of the Chinese one
    // the names brought in, or its kanji would be drawn in Chinese forms.
    std::string jaFace;
    if (ja) {
        switchTo("ja-probe");
        std::printf("    ja: %s\n", labels().c_str());
        for (const auto& f : fonts::planChain("ja-probe").ui) {
            if (f.label.find('#') != std::string::npos) {
                jaFace = f.label;
                break;
            }
        }
        CHECK(!jaFace.empty());
        const int jaAt = indexOf(jaFace);
        CHECK(jaAt == 2);  // primary, Noto, then the Japanese face
        for (const std::string& l : fonts::uiChainLabels()) {
            if (l.find("(names)") != std::string::npos) { CHECK(indexOf(l) > jaAt); }
        }
    } else {
        skip("no Japanese face - the Japanese-ahead-of-Chinese order was not run");
    }
    // Traditional Chinese: its own face first.
    if (tc) {
        switchTo("zh-TW-probe");
        std::printf("    zh-TW: %s\n", labels().c_str());
        const auto tw = fonts::planChain("zh-TW-probe");
        CHECK(tw.ui.size() >= 3 && indexOf(tw.ui[2].label) == 2);
    } else {
        skip("no Traditional Chinese face");
    }
    // Korean: Hangul in every role.
    if (ko) {
        switchTo("ko-probe");
        std::printf("    ko: %s\n", labels().c_str());
        CHECK(fonts::ui()->IsGlyphInFont(0xD55C));
        CHECK(fonts::legend()->IsGlyphInFont(0xD55C));
        CHECK(fonts::reading()->IsGlyphInFont(0xD55C));
    } else {
        skip("no Korean face");
    }
    // Back to English: the CJK faces leave the chain except as names faces
    // (which come back only while the list asks for them).
    switchTo("en");
    std::printf("    en: %s\n", labels().c_str());
    CHECK(indexOf(fonts::usingSystemSerif() ? "Georgia" : "Saira") == 0);
    for (const std::string& l : fonts::uiChainLabels()) {
        if (l.find('#') != std::string::npos) { CHECK(l.find("(names)") != std::string::npos); }
    }
}

// ImGui breaks a line at a space or after punctuation; Chinese and Japanese
// have neither between words. What it does with a run of characters too long
// for the line is cut it at the character that no longer fits - so a spaceless
// sentence still fills each line and never overhangs it. This proves that for
// the CJK face in force, the way TextWrapped and CalcTextSize with a wrap
// width do it.
void testCjkWraps() {
    std::printf("  a long CJK sentence with no spaces wraps inside its width\n");
    if (!fonts::canDraw("zh-CN-probe")) {
        skip("no system CJK face - wrapping measured on boxes would prove nothing");
        return;
    }
    switchTo("zh-CN-probe");
    ImFont* f = fonts::ui();
    const float px = fonts::kUiSize;
    // 接收机正在对整个频段进行解码并且把每一条消息都显示在输出窗口里面以便用户阅读和保存
    const char* text =
        "\xE6\x8E\xA5\xE6\x94\xB6\xE6\x9C\xBA\xE6\xAD\xA3\xE5\x9C\xA8\xE5\xAF\xB9\xE6\x95\xB4"
        "\xE4\xB8\xAA\xE9\xA2\x91\xE6\xAE\xB5\xE8\xBF\x9B\xE8\xA1\x8C\xE8\xA7\xA3\xE7\xA0\x81"
        "\xE5\xB9\xB6\xE4\xB8\x94\xE6\x8A\x8A\xE6\xAF\x8F\xE4\xB8\x80\xE6\x9D\xA1\xE6\xB6\x88"
        "\xE6\x81\xAF\xE9\x83\xBD\xE6\x98\xBE\xE7\xA4\xBA\xE5\x9C\xA8\xE8\xBE\x93\xE5\x87\xBA"
        "\xE7\xAA\x97\xE5\x8F\xA3\xE9\x87\x8C\xE9\x9D\xA2\xE4\xBB\xA5\xE4\xBE\xBF\xE7\x94\xA8"
        "\xE6\x88\xB7\xE9\x98\x85\xE8\xAF\xBB\xE5\x92\x8C\xE4\xBF\x9D\xE5\xAD\x98";
    CHECK(f->IsGlyphInFont(0x63A5));  // 接: drawn, not a box
    // The width is taken from the sentence, not fixed: the same sentence is a
    // third shorter on Linux, where the em is Saira's rather than Georgia's,
    // and a fixed width that made four lines here made three there. A little
    // over a fifth of the whole is room for four full lines and a short one.
    const float whole = f->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
    const float width = std::floor(whole / 4.6f);
    CHECK(whole > width * 4.0f);  // the premise: it cannot fit on four lines
    const char* end = text + std::strlen(text);
    int lines = 0;
    std::size_t chars = 0;
    bool advanced = true;
    for (const char* s = text; s < end;) {
        const char* eol = ImFontCalcWordWrapPositionEx(f, px, s, end, width);
        // A wrap that does not move on is a line ImGui's own text loops would
        // never leave: stop here rather than hang in them below.
        advanced = eol > s;
        CHECK(advanced);
        if (!advanced) { break; }
        const float w = f->CalcTextSizeA(px, FLT_MAX, 0.0f, s, eol).x;
        if (w > width + 0.5f) { std::printf("      line %d is %.1f px in %.0f\n", lines, w, width); }
        CHECK(w <= width + 0.5f);
        // Every line but the last is FULL - within one character of the width
        // - which is what separates cutting at the edge from giving up early.
        if (eol < end) {
            const float next = f->CalcTextSizeA(px, FLT_MAX, 0.0f, eol, cascade::core::utf8Next(eol)).x;
            CHECK(w + next > width);
        }
        CHECK((static_cast<unsigned char>(*eol) & 0xC0u) != 0x80u || eol == end);  // whole characters
        chars += cascade::core::utf8Count(std::string(s, eol).c_str());
        s = eol;
        ++lines;
    }
    std::printf("    %zu characters, %.0f px long, in %d lines of %.0f px\n", chars, whole, lines, width);
    CHECK(lines >= 5);
    CHECK(chars == cascade::core::utf8Count(text));
    // The whole-block measurement ImGui makes for TextWrapped agrees.
    if (advanced) {
        const ImVec2 block = f->CalcTextSizeA(px, FLT_MAX, width, text);
        CHECK(block.x <= width + 0.5f);
        CHECK(std::fabs(block.y - static_cast<float>(lines) * px) < 1.0f);
    }

    // A tracked caption steps a CJK character (three bytes) as ONE glyph.
    const char* caption = "\xE9\xA2\x91\xE7\x8E\x87\xE8\xAE\xBE\xE7\xBD\xAE";  // 频率设置
    const float plain = cascade::gui::trackedWidth(f, px, caption, 0.0f);
    CHECK(std::fabs(plain - f->CalcTextSizeA(px, FLT_MAX, 0.0f, caption).x) < 0.5f);
    CHECK(std::fabs(cascade::gui::trackedWidth(f, px, caption, 2.0f) - plain - 6.0f) < 0.01f);
    switchTo("en");
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    const bool loaded = fonts::load();
    CHECK(loaded);
    if (loaded) {
        frame();
        testEnglishChain();
        testFallbackEm();
        testLatinAndCyrillicSwitches();
        testCjkLazyAndOrdered();
        testCjkWraps();
        testEnglishUnchanged();  // last: it adds a font of its own to the atlas
        frame();
    }
    ImGui::DestroyContext();
    return testSummary("test_font_chain");
}
