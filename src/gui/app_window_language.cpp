// app_window_language.cpp - LANGUAGE & COUNTRY, the first section of the
// SYSTEM bank, and the between-frames step that puts a chosen language into
// force. AppWindow members, kept out of app_window.cpp because they are one
// subject: which language the bench is lettered in, and which country the
// user is in.
//
// THE OWNER'S REQUEST: "a language setting and a country setting so people
// from other countries can use it". The two are related but NEITHER CHANGES
// THE OTHER BY ITSELF. Choosing a country sets the band plan - that is what a
// country means to a receiver - and offers, as one key, the language most
// people there read; it does not switch to it. Somebody in Brazil who reads
// English, or a Brazilian abroad, has already said what they want by leaving
// the language where it is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "core/countries.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/list_pick.hpp"
#include "gui/text_fit.hpp"
#include "gui/theme.hpp"

namespace cascade::gui {

using cascade::i18n::tr;
using cascade::i18n::trId;

namespace {

// Stepping and capitalising UTF-8 are core/utf8_text.hpp's, shared with the
// rest of the interface; this file used to carry its own copies of both.
using cascade::core::upperLegend;
using cascade::core::utf8Append;
using cascade::core::utf8Decode;

// The base letter of every Latin-1 and Latin Extended-A letter, lower case,
// indexed from U+00C0. Enough for the six catalogues' alphabets, so that
// "Áustria" sorts among the A's and typing "osterreich" finds "Österreich".
// '*' marks the two signs in the block (multiplication, division).
constexpr char kLatinBase[] =
    "aaaaaaaceeeeiiiidnooooo*ouuuuyts"  // U+00C0-U+00DF
    "aaaaaaaceeeeiiiidnooooo*ouuuuyty"  // U+00E0-U+00FF
    "aaaaaacccccccc"                    // U+0100-U+010D
    "ddddeeeeeeeeeegggggggghhhh"        // U+010E-U+0127
    "iiiiiiiiiiiijjkkk"                 // U+0128-U+0138
    "llllllllllnnnnnnnnn"               // U+0139-U+014B
    "oooooooorrrrrrsssssssstttttt"      // U+014C-U+0167
    "uuuuuuuuuuuuwwyyyzzzzzzs";         // U+0168-U+017F
static_assert(sizeof(kLatinBase) == 1 + 0x180 - 0xC0, "one entry per code point");

// Lower case, accents off, for sorting and filtering. Not a collation - just
// what makes a list of country names read in the order a reader expects.
std::string foldKey(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned int cp = utf8Decode(s, i);
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp));
        } else if (cp >= 0xC0 && cp < 0x180 && kLatinBase[cp - 0xC0] != '*') {
            out.push_back(kLatinBase[cp - 0xC0]);
        } else {
            utf8Append(out, cp);
        }
    }
    return out;
}

// What a language is called in the Language combo: its own name, and a
// plain statement when nobody has checked the translation.
std::string languageLabel(const cascade::i18n::Language& l) {
    std::string s = l.name;
    if (l.machine) {
        s += ' ';
        s += tr("(machine translation)");
    }
    return s;
}

// The rail's hint, wrapped at the column's edge. The English lines fit the
// rail; a translation is often a third longer, and a hint cut off mid-word
// at the plate's edge reads as broken (seen on the first rendered check, in
// Portuguese).
void wrappedHint(const char* text) {
    ImGui::PushTextWrapPos(0.0f);
    railHint(text);
    ImGui::PopTextWrapPos();
}

}  // namespace

void AppWindow::applyPendingLanguage() {
    if (!languageApplyPending_) { return; }
    languageApplyPending_ = false;
    std::string setting = languageSetting_;
    // FOXSDR_LANGUAGE: a capture and test seam. It overrides the SAVED
    // setting for the first application of the run only - a choice made in
    // the section afterwards still works - and it is never written back:
    // currentConfig() saves languageSetting_, which this does not touch.
    if (!languageEnvConsumed_) {
        languageEnvConsumed_ = true;
        // FOXSDR_LANG_FILE: a translator's catalogue from disk, added before
        // anything is resolved so that it can be the language chosen - by
        // FOXSDR_LANGUAGE below, by a saved setting, or from the section.
        // Kept in the product on purpose: it is how a translation is checked
        // in place without a build. Problems go to stderr, never to a dialog.
        if (const char* file = std::getenv("FOXSDR_LANG_FILE"); file != nullptr && file[0] != '\0') {
            const std::string code = cascade::i18n::addCatalogueFile(file, stderr);
            if (!code.empty()) {
                std::fprintf(stderr, "i18n: added catalogue %s from %s\n", code.c_str(), file);
            }
        }
        const char* env = std::getenv("FOXSDR_LANGUAGE");
        if (env != nullptr && env[0] != '\0') { setting = env; }
    }
    cascade::i18n::setLanguage(setting);
}

void AppWindow::drawLanguageSection() {
    const std::string& inForce = cascade::i18n::current();
    std::string chip = upperLegend(inForce);
    // VERIFICATION ONLY, the house rule FOXSDR_OPEN_SERIAL_PORTS follows: the
    // row's open state is ImGui's own storage, so nothing in a config file can
    // open it for a headless self-capture. ImGuiCond_Once, so a session that
    // closes it again is not fought.
    if (std::getenv("FOXSDR_OPEN_LANGUAGE") != nullptr) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    }
    if (!railSection(trId("Language & country###language"), false, chip.c_str(),
                     cascade::gui::theme::kPhosphor, false)) {
        return;
    }

    // --- LANGUAGE -------------------------------------------------------------
    //
    // The operating system's language is read once: it does not change under
    // a running application, and the combo's preview is drawn every frame.
    static const std::string kSystemLocale = cascade::i18n::systemLocale();
    const std::string autoCode = cascade::i18n::resolveFor("auto", kSystemLocale);
    const cascade::i18n::Language* autoLang = cascade::i18n::findLanguage(autoCode);
    // THE LANGUAGE "AUTOMATIC" LANDS ON, BY ITS OWN NAME - "Automatisch
    // (Deutsch)" - because that is how a reader finds their language in a
    // list. Except English: English is not a catalogue, its entry is named in
    // English, and "Automatique (English)" read as a word left untranslated
    // in the middle of a French row. It is the one name said in the language
    // in force - "Automatique (Anglais)" - which is also how the row reads to
    // somebody who does not know the English word for their own language.
    char autoLabel[160];
    const char* autoName = (autoLang == nullptr || autoLang->code == "en")
                               ? tr("English")
                               : autoLang->name.c_str();
    cascade::core::formatUtf8(autoLabel, sizeof(autoLabel), tr("Automatic (%s)"), autoName);

    const std::vector<cascade::i18n::Language>& langs = cascade::i18n::languages();
    std::string preview;
    const std::string setting = languageSetting_.empty() ? "auto" : languageSetting_;
    // resolveFor treats "auto" without regard to case; so must the combo, or a
    // hand-edited "Auto" would show a language that is not the one in force.
    const bool followsSystem = foldKey(setting) == "auto";
    const std::string chosenCode =
        followsSystem ? std::string() : cascade::i18n::resolveFor(setting, std::string());
    if (followsSystem) {
        preview = autoLabel;
    } else {
        const cascade::i18n::Language* l = cascade::i18n::findLanguage(chosenCode);
        preview = l != nullptr ? languageLabel(*l) : std::string("English");
    }
    // Rows: "auto", then languages() - English first, then each catalogue.
    // The pick is recorded here and applied after EndCombo (list_pick.hpp).
    std::string picked;
    // Label above the combo only when a translation will not fit beside it on
    // the rail (gui/text_fit.hpp); the English row is as it was.
    if (ImGui::BeginCombo(cascade::gui::labelAboveIfNeeded(trId("Language")), preview.c_str())) {
        const bool autoSelected = followsSystem;
        if (ImGui::Selectable(autoLabel, autoSelected) && !autoSelected) { picked = "auto"; }
        if (autoSelected) { ImGui::SetItemDefaultFocus(); }
        const std::size_t pick = cascade::gui::pickFromList(
            langs, [&](const cascade::i18n::Language& l, std::size_t i) {
                const bool selected = !autoSelected && l.code == chosenCode;
                ImGui::PushID(static_cast<int>(i));
                const bool hit = ImGui::Selectable(languageLabel(l).c_str(), selected);
                ImGui::PopID();
                if (selected) { ImGui::SetItemDefaultFocus(); }
                return hit && !selected;
            });
        ImGui::EndCombo();
        if (pick != cascade::gui::kNoPick) { picked = langs[pick].code; }
    }
    if (!picked.empty()) {
        // Saved at once through currentConfig; APPLIED before the next frame.
        languageSetting_ = picked;
        languageApplyPending_ = true;
    }

    const cascade::i18n::Language* active = cascade::i18n::findLanguage(inForce);
    if (active != nullptr && active->machine) {
        wrappedHint(tr("This language was translated by machine and may read oddly."));
        if (ImGui::Button(trId("SUGGEST A BETTER TRANSLATION"))) {
            // The existing REPORT A BUG / DISLIKE page, with its box started
            // for them. The prefix is English on purpose: it is read by
            // whoever triages the site's reports, not by the user. A draft
            // already in the box is theirs and is left alone.
            problemReportOpen_ = true;
            if (problemReportText_.empty()) {
                problemReportText_ = "Translation (" + inForce + "): ";
            }
        }
    }

    ImGui::Spacing();

    // --- COUNTRY --------------------------------------------------------------
    //
    // Sorted by the name as it is SHOWN, in the language in force, so a list
    // drawn in Portuguese reads in Portuguese order. Rebuilt only when the
    // language changes.
    const std::span<const cascade::core::Country> all = cascade::core::countries();
    if (countryOrderLanguage_ != inForce || countryOrder_.size() != all.size()) {
        countryOrder_.resize(all.size());
        for (std::size_t i = 0; i < all.size(); ++i) { countryOrder_[i] = i; }
        std::vector<std::string> keys(all.size());
        for (std::size_t i = 0; i < all.size(); ++i) { keys[i] = foldKey(tr(all[i].name)); }
        std::stable_sort(countryOrder_.begin(), countryOrder_.end(),
                         [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
        countryOrderLanguage_ = inForce;
    }

    const cascade::core::Country* chosen = cascade::core::findCountry(countrySetting_);
    bool pickedNone = false;
    std::size_t pickedCountry = cascade::gui::kNoPick;
    if (ImGui::BeginCombo(cascade::gui::labelAboveIfNeeded(trId("Country")),
                          chosen != nullptr ? tr(chosen->name) : tr("Not set"),
                          ImGuiComboFlags_HeightLarge)) {
        // TYPE TO NARROW. Two hundred and forty-eight names is a list nobody
        // should have to scroll; the box takes the keyboard as the list opens,
        // and matches the name as shown, the English name, or the code.
        if (ImGui::IsWindowAppearing()) {
            countryFilter_.clear();
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##countryfilter", tr("Type to narrow the list"),
                                 &countryFilter_);
        const std::string needle = foldKey(countryFilter_);
        if (needle.empty()) {
            if (ImGui::Selectable(tr("Not set"), chosen == nullptr) && chosen != nullptr) {
                pickedNone = true;
            }
        }
        const std::size_t pick = cascade::gui::pickFromList(
            countryOrder_, [&](std::size_t idx, std::size_t) {
                const cascade::core::Country& c = all[idx];
                const char* shown = tr(c.name);
                if (!needle.empty() && foldKey(shown).find(needle) == std::string::npos &&
                    foldKey(c.name).find(needle) == std::string::npos &&
                    foldKey(c.code) != needle) {
                    return false;
                }
                const bool selected = (&c == chosen);
                ImGui::PushID(c.code);
                const bool hit = ImGui::Selectable(shown, selected);
                ImGui::PopID();
                if (selected) { ImGui::SetItemDefaultFocus(); }
                return hit && !selected;
            });
        ImGui::EndCombo();
        if (pick != cascade::gui::kNoPick) { pickedCountry = countryOrder_[pick]; }
    }
    if (pickedNone) {
        // "Not set" forgets the country and leaves the band plan alone: the
        // plan in force was chosen for a reason, and removing the country is
        // not a reason to change it.
        countrySetting_.clear();
        chosen = nullptr;
    } else if (pickedCountry != cascade::gui::kNoPick) {
        chosen = &all[pickedCountry];
        countrySetting_ = chosen->code;
        // The same two lines the Region picker in Display runs, and after
        // EndCombo for the same reason (list_pick.hpp): loadBandPlan()
        // rebuilds bandPlanChoices_.
        if (bandPlanSelection_ != chosen->bandPlan) {
            bandPlanSelection_ = chosen->bandPlan;
            loadBandPlan();
        }
    }

    if (chosen != nullptr) {
        // Which plan the country chose, by the name the Region picker shows.
        const char* planName = chosen->bandPlan;
        for (const cascade::core::PlanInfo& p : bandPlanChoices_) {
            if (p.id == chosen->bandPlan) { planName = p.name.c_str(); }
        }
        char line[200];
        cascade::core::formatUtf8(line, sizeof(line), tr("Band plan: %s"), planName);
        wrappedHint(line);
        if (bandPlanSelection_ != chosen->bandPlan) {
            wrappedHint(tr("Display has since been set to a different band plan."));
        }

        // ONE KEY, NEVER AN AUTOMATIC SWITCH: the language of the country,
        // offered when this build carries it and it is not already in force.
        const std::string suggested = cascade::i18n::matchCatalogue(chosen->language);
        if (!suggested.empty() && suggested != inForce) {
            const cascade::i18n::Language* l = cascade::i18n::findLanguage(suggested);
            if (l != nullptr) {
                char key[200];
                cascade::core::formatUtf8(key, sizeof(key), tr("USE %s"), upperLegend(l->name).c_str());
                std::string label = key;
                label += "###uselanguage";
                if (ImGui::Button(label.c_str())) {
                    languageSetting_ = suggested;
                    languageApplyPending_ = true;
                }
            }
        }
    }
}

}  // namespace cascade::gui
