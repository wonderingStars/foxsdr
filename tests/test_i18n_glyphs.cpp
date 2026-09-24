// Every character a translation uses must be DRAWABLE by the typefaces the
// bench letters it in on this machine. A code point no face in the chain has
// is drawn as the face's fallback box, so with no ł the Polish "Wyłącz" reads
// "Wy□ącz", and with no Cyrillic a Russian interface is a screen of boxes -
// on exactly the machines of the people the translation was made for.
//
// THE FACES ARE gui/fonts.hpp's CHAIN, asked through fonts::planChain() - the
// same function that loads the atlas - so this test and the application
// cannot disagree about which faces there are. For every catalogue:
//
//   1. EVERYTHING BUT CJK IS COVERED BY THE COMPILED-IN FACES, in every role:
//      Saira Condensed (Medium / SemiBold) and Nova Mono, with the Noto Sans
//      Condensed subset behind them. Checked against the embedded bytes, not
//      against whatever this machine has, so the Linux run - where there is
//      no Georgia and no system CJK face - proves that a Russian, Greek or
//      Vietnamese catalogue draws on a machine with nothing installed.
//   2. THE PAIR planChain CHOOSES HAS EVERY LETTER ITSELF, so no word is
//      assembled from two typefaces (a Georgia "Vi" and a Noto "ệ").
//   3. A CJK CATALOGUE IS DRAWN BY THE SYSTEM FACE planChain finds for it -
//      every Han, kana and Hangul character it uses. When this machine has
//      no such face (a Linux desktop without fonts-noto-cjk), the catalogue
//      is REPORTED UNAVAILABLE - which is what the application does with it -
//      and counted as skipped, never as passed and never as failed.
//   4. A NON-CJK CATALOGUE NEEDS NO CJK FACE: one stray "，" in a Russian
//      catalogue would make the language unavailable on half the machines.
//
// WHAT IS CHECKED: every resources/lang/*.json, plus a PROBE CATALOGUE per
// language being translated - a sentence of real interface words and that
// alphabet's special letters - so the rule holds before those catalogues
// exist, and keeps holding (twice over) after they land. And the English
// country names, drawn as-is when English is in force.
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
#include "core/i18n.hpp"
#include "core/scripts.hpp"
#include "core/utf8_text.hpp"
#include "gui/font_blobs.hpp"
#include "gui/font_probe.hpp"
#include "gui/fonts.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace scripts = cascade::core::scripts;
namespace fonts = cascade::gui::fonts;
namespace fontprobe = cascade::gui::fontprobe;

namespace {

// The languages the first six were written in, and the twenty-seven being
// translated now. Each is a line of the kind the interface says, plus the
// alphabet's letters a sentence might not happen to use, plus its quotation
// marks. They are catalogues in their own right ("<code>-probe"), run
// through exactly what a real catalogue is.
struct Probe {
    const char* code;
    const char* englishName;
    const char* text;
};
const Probe kProbes[] = {
    {"pt-probe", "Portuguese", "Configurações do receptor: frequência, ganho. ãõçáéíóúâêôà ÃÕÇÁÉÍÓÚÂÊÔÀ “”"},
    {"es-probe", "Spanish", "Ajustes del receptor: frecuencia, ganancia. ¿Qué? ¡Sí! ñÑ áéíóúü «»"},
    {"fr-probe", "French", "Réglages du récepteur : fréquence. œŒ ëîïûüùèàâç ÉÈÊÀÇÎÏÛÙ « »"},
    {"de-probe", "German", "Empfängereinstellungen: Frequenz, Verstärkung. äöüß ÄÖÜ „“"},
    {"it-probe", "Italian", "Impostazioni del ricevitore: frequenza. àèéìòù ÀÈÉÌÒÙ «»"},
    {"pl-probe", "Polish", "Ustawienia odbiornika: częstotliwość. ąćęłńśźż ĄĆĘŁŃŚŹŻ „”"},
    {"nl-probe", "Dutch", "Ontvangerinstellingen: frequentie, versterking. ĳ Ĳ éëïöü ‘’"},
    {"sv-probe", "Swedish", "Mottagarinställningar: frekvens, förstärkning. åäö ÅÄÖ ”"},
    {"da-probe", "Danish", "Modtagerindstillinger: frekvens. æøå ÆØÅ »«"},
    {"nb-probe", "Norwegian Bokmål", "Mottakerinnstillinger: frekvens, forsterkning. æøå ÆØÅ «»"},
    {"fi-probe", "Finnish", "Vastaanottimen asetukset: taajuus, vahvistus. äöå ÄÖÅ šž ”"},
    {"cs-probe", "Czech", "Nastavení přijímače: kmitočet, zesílení. ěščřžýáíéůúďťň ĚŠČŘŽÝÁÍÉŮÚĎŤŇ „“"},
    {"sk-probe", "Slovak", "Nastavenie prijímača: frekvencia, zosilnenie. äľĺŕôčďťň ÄĽĹŔÔČĎŤŇ „“"},
    {"hu-probe", "Hungarian", "A vevő beállításai: frekvencia, erősítés. őűáéíóöúü ŐŰÁÉÍÓÖÚÜ „”"},
    {"ro-probe", "Romanian", "Setările receptorului: frecvență, amplificare. ăâîșț ĂÂÎȘȚ „”"},
    {"hr-probe", "Croatian", "Postavke prijamnika: frekvencija, pojačanje. čćđšž ČĆĐŠŽ „“"},
    {"sl-probe", "Slovenian", "Nastavitve sprejemnika: frekvenca, ojačanje. čšž ČŠŽ „“"},
    {"tr-probe", "Turkish", "Alıcı ayarları: frekans, kazanç, ses düzeyi. ıİğĞşŞçÇöÖüÜ “”"},
    {"id-probe", "Indonesian", "Pengaturan penerima: frekuensi, penguatan, volume."},
    {"vi-probe", "Vietnamese",
     "Cài đặt máy thu: tần số, độ lợi, âm lượng. Tiếng Việt, người dùng, "
     "ạảấầẩẫậắằẳẵặẹẻẽếềểễệỉịọỏốồổỗộớờởỡợụủứừửữựỳỵỷỹ ơư ĂÂĐÊÔƠƯ ẠẢẤẦẨẪẬẮẰẲẴẶẸẺẼẾỀỂỄỆỈỊỌỎỐỒỔỖỘỚỜỞỠỢỤỦỨỪỬỮỰỲỴỶỸ ₫"},
    {"ca-probe", "Catalan", "Configuració del receptor: freqüència, guany. àçéèíïòóúü l·l ŀĿ «»"},
    {"pt-PT-probe", "Portuguese (Portugal)", "Definições do recetor: frequência, ganho. ãõçáéíóúâêô «»"},
    {"lt-probe", "Lithuanian", "Imtuvo nustatymai: dažnis, stiprinimas. ąčęėįšųūž ĄČĘĖĮŠŲŪŽ „“"},
    {"lv-probe", "Latvian", "Uztvērēja iestatījumi: frekvence, pastiprinājums. āčēģīķļņšūž ĀČĒĢĪĶĻŅŠŪŽ «»"},
    {"et-probe", "Estonian", "Vastuvõtja seaded: sagedus, võimendus. õäöüšž ÕÄÖÜŠŽ „“"},
    {"ru-probe", "Russian",
     "Настройки приёмника: частота, усиление, громкость. «Ёлка» № 5 — "
     "абвгдеёжзийклмнопрстуфхцчшщъыьэюя АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"},
    {"uk-probe", "Ukrainian", "Налаштування приймача: частота, підсилення. ґєії ҐЄІЇ м’ясо «»"},
    {"bg-probe", "Bulgarian", "Настройки на приемника: честота, усилване. ѝ ъщ ЪЩ „“"},
    {"el-probe", "Greek",
     "Ρυθμίσεις δέκτη: συχνότητα, ενίσχυση; «Έξοδος» · "
     "αβγδεζηθικλμνξοπρσςτυφχψω ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ άέήίόύώ ΆΈΉΊΌΎΏ ϊϋΐΰ ΪΫ"},
    {"zh-CN-probe", "Chinese (Simplified)", "接收机设置：频率、增益、音量。简体中文，正在解码……"},
    {"zh-TW-probe", "Chinese (Traditional)", "接收機設定：頻率、增益、音量。繁體中文，正在解碼……"},
    {"ja-probe", "Japanese", "受信機の設定：周波数、ゲイン、音量。日本語、ひらがな、カタカナー「デコード中」"},
    {"ko-probe", "Korean", "수신기 설정: 주파수, 이득, 음량. 한국어 「디코딩 중」"},
};

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

std::string utf8Of(unsigned int cp) {
    std::string s;
    cascade::core::utf8Append(s, cp);
    return s;
}

// The code points of `cps` no face in `faces` has - the per-glyph rule ImGui
// draws by. ImWchar is 16 bits in this build (no IMGUI_USE_WCHAR32), so a
// code point past the BMP cannot be drawn at all: it is always missing.
std::vector<unsigned int> uncovered(const std::vector<fonts::ChainFace>& faces,
                                    const std::vector<unsigned int>& cps) {
    std::vector<unsigned int> out;
    for (const unsigned int cp : cps) {
        bool have = false;
        for (const fonts::ChainFace& f : faces) {
            if (cp <= 0xFFFF && fontprobe::has({f.data, f.len, f.index}, cp)) {
                have = true;
                break;
            }
        }
        if (!have) { out.push_back(cp); }
    }
    return out;
}

// Prints what is missing and returns how much.
std::size_t report(const char* what, const std::string& code,
                   const std::vector<unsigned int>& missing) {
    if (missing.empty()) { return 0; }
    std::printf("      %s: %s lacks %zu:", code.c_str(), what, missing.size());
    for (std::size_t i = 0; i < missing.size() && i < 12; ++i) {
        std::printf(" U+%04X '%s'", missing[i], utf8Of(missing[i]).c_str());
    }
    std::printf("%s\n", missing.size() > 12 ? " ..." : "");
    return missing.size();
}

void checkCatalogue(const std::string& code) {
    const std::vector<unsigned int> cps = cascade::i18n::codePoints(code);
    std::vector<unsigned int> letters;
    std::vector<unsigned int> cjk;
    for (const unsigned int cp : cps) { (scripts::isCjk(cp) ? cjk : letters).push_back(cp); }
    const fonts::Chain plan = fonts::planChain(code);
    const auto face = static_cast<scripts::CjkFace>(plan.cjk);

    // 1. The compiled-in faces, role by role.
    const fonts::ChainFace notoUi{"Noto Medium", fonts::fallbackUiTtf(), fonts::fallbackUiTtfLen(), 0};
    const fonts::ChainFace notoLegend{"Noto SemiBold", fonts::fallbackLegendTtf(),
                                      fonts::fallbackLegendTtfLen(), 0};
    const std::vector<fonts::ChainFace> embeddedUi{
        {"Saira Medium", fonts::uiTtf(), fonts::uiTtfLen(), 0}, notoUi};
    const std::vector<fonts::ChainFace> embeddedLegend{
        {"Saira SemiBold", fonts::legendTtf(), fonts::legendTtfLen(), 0}, notoLegend};
    const std::vector<fonts::ChainFace> embeddedReading{
        {"Nova Mono", fonts::readingTtf(), fonts::readingTtfLen(), 0}, notoUi};
    CHECK(report("the embedded UI chain", code, uncovered(embeddedUi, letters)) == 0);
    CHECK(report("the embedded legend chain", code, uncovered(embeddedLegend, letters)) == 0);
    CHECK(report("the embedded reading chain", code, uncovered(embeddedReading, letters)) == 0);

    // 2. One pair for the whole alphabet - its LETTERS; a dong sign or a
    // guillemet may come from the fallback without moving the language.
    std::vector<unsigned int> alphabet;
    for (const unsigned int cp : letters) {
        if (scripts::isLetter(cp)) { alphabet.push_back(cp); }
    }
    CHECK(!plan.ui.empty() && !plan.legend.empty());
    if (!plan.ui.empty() && !plan.legend.empty()) {
        const std::size_t split =
            report(("the chosen pair (" + plan.pair + ")").c_str(), code,
                   uncovered({plan.ui.front()}, alphabet)) +
            report(("the chosen pair's legend (" + plan.pair + ")").c_str(), code,
                   uncovered({plan.legend.front()}, alphabet));
        CHECK(split == 0);
    }

    // 4. A non-CJK catalogue needs no CJK face.
    if (face == scripts::CjkFace::None) {
        CHECK(cjk.empty());
        std::printf("    %-12s %4zu code points, lettered in %s\n", code.c_str(), cps.size(),
                    plan.pair.c_str());
        return;
    }

    // 3. A CJK catalogue: this machine's face for it, or honestly unavailable.
    if (!plan.cjkFound) {
        std::printf("    %-12s UNAVAILABLE here (%s): %s - skipped, not passed\n", code.c_str(),
                    scripts::cjkFaceName(face), fonts::unavailableReason(code));
        CHECK(!fonts::canDraw(code));  // and the application agrees
        // Counted in the summary line as skipped (test_check.hpp), so "0
        // failed" and "not checked here" can never be confused.
        ++g_checksSkipped;
        return;
    }
    CHECK(fonts::canDraw(code));
    std::vector<fonts::ChainFace> sys;
    for (const fonts::ChainFace& f : plan.ui) {
        if (f.label.find('#') != std::string::npos) { sys.push_back(f); }
    }
    std::string faces;
    for (const fonts::ChainFace& f : sys) { faces += (faces.empty() ? "" : ", ") + f.label; }
    std::printf("    %-12s %4zu code points, lettered in %s + %s (%s)\n", code.c_str(), cps.size(),
                plan.pair.c_str(), faces.c_str(), scripts::cjkFaceName(face));
    CHECK(!sys.empty());
    CHECK(report("the system CJK face", code, uncovered(sys, cjk)) == 0);
    // Every role carries it, so a legend or a reading in that language draws.
    CHECK(plan.legend.size() >= sys.size() && plan.reading.size() >= sys.size());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc >= 2 ? std::string(argv[1]) : findRoot();
    if (root.empty()) {
        std::printf("cannot find the repository root (pass it as the first argument)\n");
        CHECK(false);
        return testSummary("test_i18n_glyphs");
    }

    // The shipped catalogues, from disk - test_i18n proves the compiled-in
    // bytes are these files; reading them here also checks a catalogue added
    // to resources/lang before anyone has regenerated lang_assets.hpp.
    std::vector<std::string> codes;
    const fs::path dir = fs::path(root) / "resources" / "lang";
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".json") { files.push_back(e.path()); }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& p : files) {
            const std::string code = cascade::i18n::addCatalogueFile(p.string(), stdout);
            CHECK(!code.empty());  // test_i18n says why in detail
            if (!code.empty()) { codes.push_back(code); }
        }
    }
    std::printf("  %zu catalogues in resources/lang\n", codes.size());
    for (const std::string& code : codes) { checkCatalogue(code); }

    std::printf("  the probe catalogues, one per language being translated\n");
    for (const Probe& p : kProbes) {
        const nlohmann::json doc = {{"code", p.code},
                                    {"name", p.text},
                                    {"englishName", p.englishName},
                                    {"machine", true},
                                    {"strings", {{"Band plan", p.text}}}};
        std::string error;
        CHECK(cascade::i18n::addCatalogue(doc.dump(), &error));
        if (!error.empty()) { std::printf("      %s: %s\n", p.code, error.c_str()); }
        checkCatalogue(p.code);
    }

    // The country table's English names, drawn as they are when English is in
    // force: in the English chain, glyph by glyph.
    std::printf("  the English country names, in the English chain\n");
    std::vector<unsigned int> countryCps;
    std::set<unsigned int> seen;
    for (const cascade::core::Country& c : cascade::core::countries()) {
        const std::string_view name(c.name);
        for (std::size_t i = 0; i < name.size();) {
            const unsigned int cp = cascade::core::utf8Decode(name, i);
            if (cp >= 0x20 && seen.insert(cp).second) { countryCps.push_back(cp); }
        }
    }
    const fonts::Chain en = fonts::planChain("en");
    CHECK(report("the English UI chain", "en", uncovered(en.ui, countryCps)) == 0);
    std::printf("    %zu code points; English is lettered in %s\n", countryCps.size(), en.pair.c_str());

    return testSummary("test_i18n_glyphs");
}
