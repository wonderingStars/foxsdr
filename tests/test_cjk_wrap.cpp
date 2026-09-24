// Tests for the FOXSDR PATCH in the vendored Dear ImGui's word wrap
// (third_party/imgui/FOXSDR-PATCHES.md): Chinese and Japanese lines break
// between characters, never start with a closing mark or end with an opening
// one, and keep a Latin word inside them whole - while English, and Korean,
// wrap exactly as upstream ImGui wraps them.
//
// WHY "EXACTLY AS UPSTREAM" CAN BE TESTED. The upstream routine is copied
// below, verbatim but for two lookups that are private to imgui_draw.cpp (the
// classifier tables, rebuilt here from the same public calls, and the advance
// lookup, which is ImFontBaked::GetCharAdvance). Every English string the
// interface draws - every catalogue key - is wrapped by both at a spread of
// widths, and every line break must land on the same byte.
//
// THE CJK CHECKS DO NOT NEED A CJK FONT. Break positions depend only on the
// advances; on a machine without a system CJK face every ideograph has the
// fallback glyph's advance, which is still a fixed width, so the rules are
// checked the same way everywhere and nothing here is skipped.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

namespace fonts = cascade::gui::fonts;

namespace {

// --- upstream Dear ImGui 1.92.8, ImFontCalcWordWrapPositionEx, unpatched ------

ImU32 gRef0000[128 / 16] = {};
ImU32 gRef3000[16 / 16] = {};

void refInit() {
    // As ImTextInitClassifiers() does it.
    ImTextClassifierClear(gRef0000, 0, 128, ImWcharClass_Other);
    ImTextClassifierSetCharClassFromStr(gRef0000, 0, 128, ImWcharClass_Blank, " \t");
    ImTextClassifierSetCharClassFromStr(gRef0000, 0, 128, ImWcharClass_Punct, ".,;!?\"");
    ImTextClassifierClear(gRef3000, 0x3000, 0x300F, ImWcharClass_Other);
    ImTextClassifierSetCharClass(gRef3000, 0x3000, 0x300F, ImWcharClass_Blank, 0x3000);
    ImTextClassifierSetCharClass(gRef3000, 0x3000, 0x300F, ImWcharClass_Punct, 0x3001);
    ImTextClassifierSetCharClass(gRef3000, 0x3000, 0x300F, ImWcharClass_Punct, 0x3002);
}

#define REF_CLASS(_BITS, _CHAR_OFFSET) ((_BITS[(_CHAR_OFFSET) >> 4] >> (((_CHAR_OFFSET) & 15) << 1)) & 0x03)

const char* refWrap(ImFont* font, float size, const char* text, const char* text_end, float wrap_width,
                    ImDrawTextFlags flags) {
    ImFontBaked* baked = font->GetFontBaked(size);
    const float scale = size / baked->Size;
    float line_width = 0.0f;
    float blank_width = 0.0f;
    wrap_width /= scale;
    const char* s = text;
    int prev_type = ImWcharClass_Other;
    const bool keep_blanks = (flags & ImDrawTextFlags_WrapKeepBlanks) != 0;
    const char* span_end = s;
    float span_width = 0.0f;
    while (s < text_end) {
        unsigned int c = (unsigned int)*s;
        const char* next_s;
        if (c < 0x80)
            next_s = s + 1;
        else
            next_s = s + ImTextCharFromUtf8(&c, s, text_end);
        if (c < 32) {
            if (c == '\n') return s;
            if (c == '\r') {
                s = next_s;
                continue;
            }
        }
        const float char_width = baked->GetCharAdvance(static_cast<ImWchar>(c));
        int curr_type;
        if (c < 128)
            curr_type = REF_CLASS(gRef0000, c);
        else if (c >= 0x3000 && c < 0x3010)
            curr_type = REF_CLASS(gRef3000, c & 15);
        else
            curr_type = ImWcharClass_Other;
        if (curr_type == ImWcharClass_Blank) {
            if (prev_type != ImWcharClass_Blank && !keep_blanks) {
                span_end = s;
                line_width += span_width;
                span_width = 0.0f;
            }
            blank_width += char_width;
        } else {
            if (prev_type == ImWcharClass_Punct && curr_type != ImWcharClass_Punct && !(c >= '0' && c <= '9')) {
                span_end = s;
                line_width += span_width + blank_width;
                span_width = blank_width = 0.0f;
            } else if (prev_type == ImWcharClass_Blank && keep_blanks) {
                span_end = s;
                line_width += span_width + blank_width;
                span_width = blank_width = 0.0f;
            }
            span_width += char_width;
        }
        if (span_width + blank_width + line_width > wrap_width) {
            if (span_width + blank_width > wrap_width) break;
            return span_end;
        }
        prev_type = curr_type;
        s = next_s;
    }
    if (s == text && text < text_end) return s + ImTextCountUtf8BytesFromChar(s, text_end);
    return s;
}

// --- line breaking, as RenderText and CalcTextSizeA walk it -------------------

using WrapFn = const char* (*)(ImFont*, float, const char*, const char*, float, ImDrawTextFlags);

const char* patchedWrap(ImFont* f, float size, const char* t, const char* e, float w, ImDrawTextFlags flags) {
    return ImFontCalcWordWrapPositionEx(f, size, t, e, w, flags);
}

// Byte offsets where each line after the first begins, and each line's
// [start, end) with trailing blanks removed.
struct Line {
    std::size_t start;
    std::size_t end;
};
std::vector<Line> lines(WrapFn fn, ImFont* f, float size, const std::string& text, float width) {
    std::vector<Line> out;
    const char* base = text.c_str();
    const char* end = base + text.size();
    const char* s = base;
    int guard = 0;
    while (s < end && ++guard < 10000) {
        const char* eol = fn(f, size, s, end, width, 0);
        if (eol == s && *s == '\n') {  // an empty line, as RenderText steps over it
            out.push_back({static_cast<std::size_t>(s - base), static_cast<std::size_t>(s - base)});
            s = ImTextCalcWordWrapNextLineStart(eol, end, 0);
            continue;
        }
        if (eol <= s) {  // never advances: a line nothing will ever leave
            out.push_back({static_cast<std::size_t>(s - base), static_cast<std::size_t>(s - base)});
            break;
        }
        out.push_back({static_cast<std::size_t>(s - base), static_cast<std::size_t>(eol - base)});
        s = ImTextCalcWordWrapNextLineStart(eol, end, 0);
    }
    return out;
}

unsigned int cpAt(const std::string& s, std::size_t i) {
    if (i >= s.size()) { return 0; }
    return cascade::core::utf8Decode(s, i);
}
unsigned int cpBefore(const std::string& s, std::size_t i) {
    if (i == 0) { return 0; }
    std::size_t k = i - 1;
    while (k > 0 && (static_cast<unsigned char>(s[k]) & 0xC0u) == 0x80u) { --k; }
    return cpAt(s, k);
}

bool closing(unsigned int c) {
    for (const unsigned int x : {0x3002u, 0xFF0Cu, 0x3001u, 0xFF0Eu, 0xFF01u, 0xFF1Fu, 0xFF09u, 0x300Du,
                                 0x300Fu, 0x3011u, 0x3009u, 0x300Bu, 0xFF1Au, 0xFF1Bu, 0x201Du, 0x2019u,
                                 unsigned(','), unsigned('.'), unsigned('!'), unsigned('?'), unsigned(')'),
                                 unsigned(';'), unsigned(':')}) {
        if (c == x) { return true; }
    }
    return false;
}
bool opening(unsigned int c) {
    for (const unsigned int x : {0xFF08u, 0x300Cu, 0x300Eu, 0x3010u, 0x3008u, 0x300Au, 0x201Cu, 0x2018u}) {
        if (c == x) { return true; }
    }
    return false;
}
bool latin(unsigned int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

float widthOf(ImFont* f, float px, const std::string& s, std::size_t a, std::size_t b) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s.c_str() + a, s.c_str() + b).x;
}

// The CJK rules, for one text at one width. Returns the number of faults and
// prints each.
int cjkFaults(ImFont* f, float px, const std::string& t, float width, const char* what) {
    int faults = 0;
    auto fault = [&](const char* why, std::size_t at) {
        if (faults < 4) {
            std::printf("      %s at %.0f px: %s at byte %zu: \"%s\" | \"%s\"\n", what, width, why, at,
                        t.substr(at >= 12 ? at - 12 : 0, at >= 12 ? 12 : at).c_str(),
                        t.substr(at, 12).c_str());
        }
        ++faults;
    };
    const std::vector<Line> ls = lines(&patchedWrap, f, px, t, width);
    std::size_t covered = 0;
    for (std::size_t i = 0; i < ls.size(); ++i) {
        const Line& l = ls[i];
        if (l.end <= l.start && cpAt(t, l.start) != '\n') {
            fault("a line that does not advance", l.start);
            break;
        }
        covered = l.end;
        const float w = widthOf(f, px, t, l.start, l.end);
        if (w > width + 0.5f) {
            // Only a single unbreakable run may overhang... and ImGui cuts even
            // that, so nothing may.
            fault("a line wider than the width", l.start);
        }
        if (i > 0 && closing(cpAt(t, l.start))) { fault("a line starts with a closing mark", l.start); }
        if (i + 1 < ls.size() && opening(cpBefore(t, l.end))) {
            fault("a line ends with an opening mark", l.end);
        }
        if (i + 1 >= ls.size()) { continue; }
        // A Latin word cut in two: letters or digits on both sides of the
        // break (a dot between them counts as inside the word: foxsdr.com).
        const unsigned int before = cpBefore(t, l.end);
        const unsigned int after = cpAt(t, l.end);
        const bool inWord = (latin(before) && latin(after)) ||
                            (before == '.' && latin(after) && latin(cpBefore(t, l.end - 1)));
        if (inWord) {
            // Allowed only when the word itself is wider than a line.
            std::size_t a = l.end;
            while (a > 0 && (latin(cpBefore(t, a)) || cpBefore(t, a) == '.')) { --a; }
            std::size_t b = l.end;
            while (b < t.size() && (latin(cpAt(t, b)) || cpAt(t, b) == '.')) { ++b; }
            if (widthOf(f, px, t, a, b) <= width) { fault("a Latin word split", l.end); }
        }
        // AN EARLY END: the next unit - one character, or a whole Latin word,
        // with any closing marks that must follow it - would still have fitted.
        if (ls[i + 1].start != l.end) { continue; }  // a break at a space
        std::size_t u = l.end;
        // An opening mark cannot end a line, so it travels with what follows.
        while (u < t.size() && opening(cpAt(t, u))) { u += cascade::core::utf8CharLen(t.c_str() + u); }
        const unsigned int c0 = cpAt(t, u);
        u += cascade::core::utf8CharLen(t.c_str() + u);
        if (latin(c0)) {
            while (u < t.size() && (latin(cpAt(t, u)) || (cpAt(t, u) == '.' && latin(cpAt(t, u + 1))))) { ++u; }
        }
        while (u < t.size() && closing(cpAt(t, u))) { u += cascade::core::utf8CharLen(t.c_str() + u); }
        if (widthOf(f, px, t, l.start, u) <= width) { fault("an early line end", l.end); }
    }
    if (covered != t.size()) {
        const std::size_t lastEnd = ls.empty() ? 0 : ls.back().end;
        if (lastEnd != t.size()) { fault("text lost", lastEnd); }
    }
    return faults;
}

// The Chinese sentences whose wrapping the first rendered check caught
// (zoom_zh_wrap.png): a full stop alone on a line, "G" and "PS" on two lines,
// a line ended early before a long run.
const char* const kZh[] = {
    "每次启动时询问一次foxsdr.com是否有更新的版本，并且只发送您正在运行的版本号，不发送任何标识符，"
    "也不使用任何cookie。除非您按下按钮，否则不会下载或安装任何东西。这与下面的使用情况报告无关，"
    "两者不共享任何内容。",
    "这台计算机现在拥有的每一个串行端口，也就是下面GPS一行所提供的端口，集中在一个地方，"
    "而不是只能从侧栏的雷达部分找到。",
    "接收机响应的每一个快捷键。点击一个按键来更改它，然后按下您想要的按键组合，按Esc保持原样，"
    "按Backspace清除。在输入框中打字时快捷键永远不会触发。",
    "《接收机手册》（第二版）说：「请先检查天线。」然后再调整增益【重要】。",
};
const char* const kJa[] = {
    "受信機が応答するすべてのショートカット。キーをクリックして変更し、使いたいキーを押してください。"
    "「デコード中」の表示は（受信中）と同じです。Escでそのまま、Backspaceで消去します。",
    "起動するたびに一度だけfoxsdr.comに新しいバージョンがあるかを問い合わせ、実行中のバージョン番号だけを送信します。",
};
const char* const kKo[] = {
    "실행할 때마다 한 번 foxsdr.com에 새 버전이 있는지 묻고, 실행 중인 버전만 보냅니다. 식별자도 "
    "쿠키도 없습니다. 버튼을 누르지 않으면 아무것도 내려받거나 설치하지 않습니다.",
    "수신기가 응답하는 모든 단축키입니다. 키를 눌러 바꾼 다음 원하는 키를 누르세요 - Esc는 그대로 두고 "
    "Backspace는 지웁니다.",
};

std::vector<float> widths(float px) {
    std::vector<float> out;
    for (float w = px * 5.0f; w <= px * 26.0f; w += px * 0.37f) { out.push_back(w); }
    return out;
}

void testCjk(const char* code, const char* const* texts, std::size_t n, const char* what) {
    fonts::applyLanguage(fonts::canDraw(code) ? code : "en");
    fonts::applyPending();
    ImFont* f = fonts::ui();
    const float px = fonts::kUiSize;
    int faults = 0;
    int checked = 0;
    for (std::size_t k = 0; k < n; ++k) {
        for (const float w : widths(px)) {
            faults += cjkFaults(f, px, texts[k], w, what);
            ++checked;
        }
    }
    std::printf("    %s: %d text/width pairs, %d faults%s\n", what, checked, faults,
                fonts::canDraw(code) ? "" : " (no CJK face here: fallback-glyph advances)");
    CHECK(faults == 0);
}

// Same breaks as upstream, byte for byte.
int compareToUpstream(ImFont* f, float px, const std::string& t, float w) {
    const std::vector<Line> a = lines(&refWrap, f, px, t, w);
    const std::vector<Line> b = lines(&patchedWrap, f, px, t, w);
    if (a.size() != b.size()) { return 1; }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].start != b[i].start || a[i].end != b[i].end) { return 1; }
    }
    return 0;
}

void testUpstreamWhereNoCjk() {
    std::printf("  English and Korean wrap exactly as upstream ImGui wraps them\n");
    fonts::applyLanguage("en");
    fonts::applyPending();
    ImFont* f = fonts::ui();
    const float px = fonts::kUiSize;
    // Every English string the interface draws: the keys of a shipped
    // catalogue.
    std::vector<std::string> english;
    for (const auto& file : cascade::i18n::embeddedCatalogueFiles()) {
        const nlohmann::json j = nlohmann::json::parse(
            std::string(reinterpret_cast<const char*>(file.bytes), file.len), nullptr, false);
        if (!j.is_object() || !j.contains("strings")) { continue; }
        for (auto it = j["strings"].begin(); it != j["strings"].end(); ++it) { english.push_back(it.key()); }
        break;
    }
    CHECK(english.size() > 1000);
    // And paragraphs chosen for the corners: punctuation runs, a dotted name,
    // curly quotes, brackets, a word longer than the line, several spaces.
    english.push_back("Asks foxsdr.com once per launch whether a newer version exists - no identifier, "
                      "no cookie. \xE2\x80\x9CQuoted,\xE2\x80\x9D he said; (bracketed) text!? Yes... "
                      "e.g. v0.99.27, 145.800 MHz, 1,234,567 Hz.");
    english.push_back("Supercalifragilisticexpialidociousantidisestablishmentarianism is long.   "
                      "Three spaces, then \"quotes\", then an em \xE2\x80\x94 dash and a tab\there.");
    english.push_back("Wy\xC5\x82\xC4\x85" "cz odbiornik, \xC5\xBC" "eby zmieni\xC4\x87 cz\xC4\x99stotliwo\xC5\x9B\xC4\x87.");
    int differ = 0;
    std::size_t pairs = 0;
    for (const std::string& s : english) {
        for (const float w : {px * 3.0f, px * 7.5f, px * 13.0f, px * 21.0f}) {
            if (compareToUpstream(f, px, s, w) != 0) {
                if (differ < 5) { std::printf("      differs at %.0f px: \"%s\"\n", w, s.c_str()); }
                ++differ;
            }
            ++pairs;
        }
    }
    std::printf("    English: %zu strings, %zu string/width pairs, %d differ\n", english.size(), pairs, differ);
    CHECK(differ == 0);
    // Korean breaks at its spaces, as upstream does - the patch leaves Hangul alone.
    int koDiffer = 0;
    for (const char* s : kKo) {
        for (const float w : widths(px)) { koDiffer += compareToUpstream(f, px, s, w); }
    }
    std::printf("    Korean: %d differ\n", koDiffer);
    CHECK(koDiffer == 0);
}

// EVERY SHIPPED CHINESE AND JAPANESE STRING, wrapped at a spread of widths:
// no line may end with an opening mark. The hand-picked sentences above never
// met the case the 34-language screenshot review did (a zh-CN line ending in
// an opening bracket), because every bracket in them was a FULLWIDTH one. The
// catalogues also write the ASCII "(" and "[" straight before a CJK character
// - "(重要)", "(赤道处)" - and the patch allowed a break after them, since
// what follows is CJK. An ASCII opening bracket counts as an opening mark here.
bool openingOrAscii(unsigned int c) { return opening(c) || c == '(' || c == '[' || c == '{'; }

void testShippedCatalogueMarks() {
    std::printf("  every zh-CN / zh-TW / ja catalogue value: no opening mark ends a line\n");
    for (const auto& file : cascade::i18n::embeddedCatalogueFiles()) {
        const nlohmann::json j = nlohmann::json::parse(
            std::string(reinterpret_cast<const char*>(file.bytes), file.len), nullptr, false);
        if (!j.is_object() || !j.contains("strings") || !j.contains("code")) { continue; }
        const std::string code = j["code"].get<std::string>();
        if (code != "zh-CN" && code != "zh-TW" && code != "ja") { continue; }
        fonts::applyLanguage(fonts::canDraw(code.c_str()) ? code.c_str() : "en");
        fonts::applyPending();
        ImFont* f = fonts::ui();
        const float px = fonts::kUiSize;
        int ends = 0;
        std::size_t pairs = 0;
        for (auto it = j["strings"].begin(); it != j["strings"].end(); ++it) {
            if (!it.value().is_string()) { continue; }
            const std::string t = it.value().get<std::string>();
            for (float w = px * 5.0f; w <= px * 24.0f; w += px * 0.53f) {
                ++pairs;
                const std::vector<Line> ls = lines(&patchedWrap, f, px, t, w);
                for (std::size_t i = 0; i + 1 < ls.size(); ++i) {
                    if (!openingOrAscii(cpBefore(t, ls[i].end))) { continue; }
                    if (ends < 6) {
                        std::printf("      %s at %.0f px: a line ends with an opening mark: \"%s\" | \"%s\"\n",
                                    code.c_str(), w, t.substr(ls[i].start, ls[i].end - ls[i].start).c_str(),
                                    t.substr(ls[i].end, 24).c_str());
                    }
                    ++ends;
                }
            }
        }
        std::printf("    %s: %zu value/width pairs, %d end on an opening mark\n", code.c_str(), pairs, ends);
        CHECK(ends == 0);
    }
}

void addProbe(const char* code, const char* text) {
    const nlohmann::json doc = {{"code", code}, {"name", text}, {"englishName", code},
                                {"strings", {{"Band plan", text}}}};
    std::string error;
    CHECK(cascade::i18n::addCatalogue(doc.dump(), &error));
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
    refInit();
    const bool loaded = fonts::load();
    CHECK(loaded);
    if (loaded) {
        ImGui::NewFrame();  // the classifiers and the atlas, as the application has them
        ImGui::Render();
        addProbe("zh-CN-wrap", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87");
        addProbe("ja-wrap", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
        testUpstreamWhereNoCjk();
        std::printf("  Chinese and Japanese: break between characters, marks in their place\n");
        testCjk("zh-CN-wrap", kZh, sizeof(kZh) / sizeof(kZh[0]), "zh");
        testCjk("ja-wrap", kJa, sizeof(kJa) / sizeof(kJa[0]), "ja");
        testShippedCatalogueMarks();
    }
    ImGui::DestroyContext();
    return testSummary("test_cjk_wrap");
}
