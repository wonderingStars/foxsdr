// scripts.cpp - the tables behind scripts.hpp. See the header for the
// contract; this file is the data.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/scripts.hpp"

#include <cstdlib>

#include "core/i18n.hpp"  // FOX_TR_NOOP: the reasons are translation keys

namespace cascade::core::scripts {
namespace {

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return out;
}

// The subtags of a language tag after the first, lower case: "zh-Hant-TW"
// gives {"hant", "tw"}.
std::vector<std::string> laterSubtags(const std::string& tag) {
    std::vector<std::string> out;
    std::size_t i = tag.find('-');
    while (i != std::string::npos) {
        const std::size_t j = tag.find('-', i + 1);
        out.push_back(tag.substr(i + 1, j == std::string::npos ? std::string::npos : j - i - 1));
        i = j;
    }
    return out;
}

}  // namespace

const char* cjkFaceName(CjkFace f) {
    switch (f) {
    case CjkFace::None: return "none";
    case CjkFace::ChineseSimplified: return "Chinese (Simplified)";
    case CjkFace::ChineseTraditional: return "Chinese (Traditional)";
    case CjkFace::Japanese: return "Japanese";
    case CjkFace::Korean: return "Korean";
    }
    return "none";
}

bool isHangul(unsigned int cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) ||  // jamo
           (cp >= 0x3130 && cp <= 0x318F) ||  // compatibility jamo
           (cp >= 0xA960 && cp <= 0xA97F) ||  // jamo extended-A
           (cp >= 0xAC00 && cp <= 0xD7AF) ||  // syllables
           (cp >= 0xD7B0 && cp <= 0xD7FF);    // jamo extended-B
}

bool isKana(unsigned int cp) {
    return (cp >= 0x3040 && cp <= 0x30FF) ||  // hiragana, katakana
           (cp >= 0x31F0 && cp <= 0x31FF) ||  // katakana phonetic extensions
           (cp >= 0xFF66 && cp <= 0xFF9F);    // halfwidth katakana
}

bool isHan(unsigned int cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) ||   // extension A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   // unified ideographs
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // compatibility ideographs
           (cp >= 0x20000 && cp <= 0x3FFFF);   // the supplementary planes' extensions
}

bool isLetter(unsigned int cp) {
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) { return true; }
    if (cp >= 0xC0 && cp <= 0x24F) { return cp != 0xD7 && cp != 0xF7; }  // not × ÷
    if (cp >= 0x250 && cp <= 0x2AF) { return true; }                        // IPA letters
    if (cp >= 0x370 && cp <= 0x3FF) {
        // Greek, less its numeral signs, tonos marks, question mark and
        // ano teleia, and the block's unassigned points.
        return cp != 0x374 && cp != 0x375 && cp != 0x37E && cp != 0x384 && cp != 0x385 &&
               cp != 0x387 && cp != 0x378 && cp != 0x379 && cp != 0x380 && cp != 0x381 &&
               cp != 0x382 && cp != 0x383 && cp != 0x38B && cp != 0x38D && cp != 0x3A2;
    }
    if (cp >= 0x400 && cp <= 0x52F) { return cp < 0x482 || cp > 0x489; }  // not the thousands sign, titlo
    if (cp >= 0x1E00 && cp <= 0x1FFF) { return true; }  // Latin Extended Additional, Greek Extended
    return false;
}

bool isCjk(unsigned int cp) {
    return isHangul(cp) || isKana(cp) || isHan(cp) ||
           (cp >= 0x2E80 && cp <= 0x2FFF) ||  // radicals, Kangxi, description characters
           (cp >= 0x3000 && cp <= 0x303F) ||  // symbols and punctuation: 、。「」
           (cp >= 0x3100 && cp <= 0x312F) ||  // bopomofo
           (cp >= 0x3190 && cp <= 0x31EF) ||  // kanbun, bopomofo extended, strokes
           (cp >= 0x3200 && cp <= 0x33FF) ||  // enclosed letters, compatibility
           (cp >= 0xFE10 && cp <= 0xFE1F) ||  // vertical forms
           (cp >= 0xFE30 && cp <= 0xFE4F) ||  // compatibility forms
           (cp >= 0xFF00 && cp <= 0xFFEF);    // fullwidth and halfwidth forms: ，：（）
}

CjkFace cjkFaceFor(std::string_view languageCode, const std::vector<unsigned int>& codePoints) {
    const std::string code = lowerAscii(languageCode);
    const std::string primary = code.substr(0, code.find('-'));
    if (primary == "zh" || primary == "yue") {
        // Traditional where the script subtag says so or the region writes
        // it: Taiwan, Hong Kong, Macau. Cantonese is written in Traditional.
        if (primary == "yue") { return CjkFace::ChineseTraditional; }
        for (const std::string& sub : laterSubtags(code)) {
            if (sub == "hant" || sub == "tw" || sub == "hk" || sub == "mo") {
                return CjkFace::ChineseTraditional;
            }
            if (sub == "hans") { return CjkFace::ChineseSimplified; }
        }
        return CjkFace::ChineseSimplified;
    }
    if (primary == "ja") { return CjkFace::Japanese; }
    if (primary == "ko") { return CjkFace::Korean; }
    // A code nobody planned for: what the text is written in decides. Hangul
    // and kana are each unique to one language; Han alone is read as Chinese.
    bool hangul = false;
    bool kana = false;
    bool other = false;
    for (unsigned int cp : codePoints) {
        if (isHangul(cp)) {
            hangul = true;
        } else if (isKana(cp)) {
            kana = true;
        } else if (isCjk(cp)) {
            other = true;
        }
    }
    if (hangul) { return CjkFace::Korean; }
    if (kana) { return CjkFace::Japanese; }
    if (other) { return CjkFace::ChineseSimplified; }
    return CjkFace::None;
}

std::vector<unsigned int> probeCodePoints(CjkFace f) {
    switch (f) {
    case CjkFace::None: return {};
    // 简体 中文 设 频 率 。，
    case CjkFace::ChineseSimplified:
        return {0x7B80, 0x4F53, 0x4E2D, 0x6587, 0x8BBE, 0x9891, 0x7387, 0x3002, 0xFF0C};
    // 繁體 中文 設 頻 率 。，
    case CjkFace::ChineseTraditional:
        return {0x7E41, 0x9AD4, 0x4E2D, 0x6587, 0x8A2D, 0x983B, 0x7387, 0x3002, 0xFF0C};
    // 日本語 ひら カナー 設定 。、
    case CjkFace::Japanese:
        return {0x65E5, 0x672C, 0x8A9E, 0x3072, 0x3089, 0x30AB, 0x30CA, 0x30FC, 0x8A2D, 0x5B9A,
                0x3002, 0x3001};
    // 한국어 설정 주파수
    case CjkFace::Korean:
        return {0xD55C, 0xAD6D, 0xC5B4, 0xC124, 0xC815, 0xC8FC, 0xD30C, 0xC218};
    }
    return {};
}

std::vector<SystemFace> systemCandidates(CjkFace f, bool windows) {
    if (f == CjkFace::None) { return {}; }
    if (windows) {
        // The UI variants of the Chinese faces are the ones Windows letters its
        // own interface in; Yu Gothic MEDIUM rather than the UI face in the
        // same file, whose regular weight is visibly thinner than the Latin
        // beside it. The later entries are older or optional faces, tried when
        // an install has had the first removed.
        switch (f) {
        case CjkFace::ChineseSimplified:
            return {{"msyh.ttc", "Microsoft YaHei UI"}, {"msyh.ttf", "Microsoft YaHei"},
                    {"simsun.ttc", "SimSun"}, {"msjh.ttc", "Microsoft JhengHei UI"}};
        case CjkFace::ChineseTraditional:
            return {{"msjh.ttc", "Microsoft JhengHei UI"}, {"msjh.ttf", "Microsoft JhengHei"},
                    {"mingliu.ttc", "PMingLiU"}, {"msyh.ttc", "Microsoft YaHei UI"}};
        case CjkFace::Japanese:
            return {{"YuGothM.ttc", "Yu Gothic Medium"}, {"meiryo.ttc", "Meiryo UI"},
                    {"msgothic.ttc", "MS UI Gothic"}};
        case CjkFace::Korean:
            return {{"malgun.ttf", "Malgun Gothic"}, {"gulim.ttc", "Gulim"}};
        case CjkFace::None: break;
        }
        return {};
    }
    // Everywhere else: the usual install locations, most likely first. The
    // Noto Sans CJK collection holds every standard as a separate face (the
    // hint picks it); WenQuanYi and Droid Sans Fallback are single faces that
    // cover all four, with Chinese glyph forms, so they are the last resort.
    const char* noto = nullptr;
    const char* sourceHan = nullptr;
    switch (f) {
    case CjkFace::ChineseSimplified: noto = "Noto Sans CJK SC"; sourceHan = "Source Han Sans SC"; break;
    case CjkFace::ChineseTraditional: noto = "Noto Sans CJK TC"; sourceHan = "Source Han Sans TC"; break;
    case CjkFace::Japanese: noto = "Noto Sans CJK JP"; sourceHan = "Source Han Sans"; break;
    case CjkFace::Korean: noto = "Noto Sans CJK KR"; sourceHan = "Source Han Sans K"; break;
    case CjkFace::None: return {};
    }
    std::vector<SystemFace> out;
    for (const char* dir : {"/usr/share/fonts/opentype/noto",            // Debian, Ubuntu
                            "/usr/share/fonts/noto-cjk",                 // Arch
                            "/usr/share/fonts/google-noto-sans-cjk-fonts",  // Fedora
                            "/usr/share/fonts/google-noto-cjk",          // older Fedora
                            "/usr/share/fonts/truetype/noto",
                            "/system/fonts"}) {                          // Android
        out.push_back({std::string(dir) + "/NotoSansCJK-Regular.ttc", noto});
    }
    // A user's own font folder, where a desktop's "install font" puts a file.
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        out.push_back({std::string(home) + "/.local/share/fonts/NotoSansCJK-Regular.ttc", noto});
        out.push_back({std::string(home) + "/.fonts/NotoSansCJK-Regular.ttc", noto});
    }
    out.push_back({"/usr/share/fonts/adobe-source-han-sans/SourceHanSans.ttc", sourceHan});
    out.push_back({"/usr/share/fonts/opentype/source-han-sans/SourceHanSans.ttc", sourceHan});
    if (f == CjkFace::Japanese) {
        out.push_back({"/usr/share/fonts/opentype/ipaexfont-gothic/ipaexg.ttf", "IPAexGothic"});
        out.push_back({"/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf", "IPAGothic"});
        out.push_back({"/usr/share/fonts/truetype/takao-gothic/TakaoPGothic.ttf", "TakaoPGothic"});
    }
    if (f == CjkFace::Korean) {
        out.push_back({"/usr/share/fonts/truetype/nanum/NanumGothic.ttf", "NanumGothic"});
        out.push_back({"/usr/share/fonts/truetype/unfonts-core/UnDotum.ttf", "UnDotum"});
    }
    out.push_back({"/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", "WenQuanYi Micro Hei"});
    out.push_back({"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", "WenQuanYi Zen Hei"});
    out.push_back({"/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf", "Droid Sans Fallback"});
    return out;
}

const char* missingFontReason(CjkFace f, bool windows) {
    // Whole sentences, one per case, rather than "needs a %s font": the word
    // for the language changes its ending with the sentence around it in half
    // the catalogues, and a translator can only get that right with the
    // sentence in front of them.
    if (windows) {
        switch (f) {
        case CjkFace::ChineseSimplified:
        case CjkFace::ChineseTraditional:
            return FOX_TR_NOOP("needs a Chinese font, which this computer does not have");
        case CjkFace::Japanese:
            return FOX_TR_NOOP("needs a Japanese font, which this computer does not have");
        case CjkFace::Korean:
            return FOX_TR_NOOP("needs a Korean font, which this computer does not have");
        case CjkFace::None: break;
        }
        return "";
    }
    switch (f) {
    case CjkFace::ChineseSimplified:
    case CjkFace::ChineseTraditional:
        return FOX_TR_NOOP("needs a Chinese font, e.g. install fonts-noto-cjk");
    case CjkFace::Japanese: return FOX_TR_NOOP("needs a Japanese font, e.g. install fonts-noto-cjk");
    case CjkFace::Korean: return FOX_TR_NOOP("needs a Korean font, e.g. install fonts-noto-cjk");
    case CjkFace::None: break;
    }
    return "";
}

}  // namespace cascade::core::scripts
