// i18n.hpp - the interface language: every word the bench shows a user goes
// through tr(), and tr() answers from the chosen language's catalogue.
//
// THE KEY IS THE ENGLISH TEXT ITSELF. A call site reads
//     ImGui::TextUnformatted(tr("Band plan"));
// and the catalogue maps "Band plan" to "Plano de banda". There are no
// symbolic ids to keep in step with the prose, a missing translation simply
// shows the English, and ENGLISH IS NOT A CATALOGUE: with English selected
// tr() hands back its own argument, so the English interface is byte for byte
// what it was before this file existed.
//
// THE CATALOGUES ARE COMPILED IN. resources/lang/<code>.json is the source;
// tools/embed-lang.py turns them into core/lang_assets.hpp, exactly as the
// fonts are embedded and for the same reason: a translation shipped as a loose
// file is one more thing each of the five packagers (Inno, MSIX, AppImage,
// Android, the CI zip) can leave out, and a missing catalogue fails silently
// into English. Compiled in, it either exists or the build does not.
//
// POINTER LIFETIME. tr() returns a pointer that stays valid for the life of
// the process, whatever language is chosen later: every catalogue ever loaded
// is kept, and switching language only moves which one is read. That is what
// makes it safe to call tr() while building a table that outlives the frame.
//
// FORMAT STRINGS are translated whole - tr("%s (%d bands)") - and every
// translation must carry the same conversion specifications in the same
// order (test_i18n enforces it; a mismatch is a crash, not a typo).
//
// WHAT IS NOT TRANSLATED: config keys, ImGui ids, anything sent to the site
// (reports, telemetry), log lines, file names, and the universal technical
// vocabulary of radio - Hz, dB, USB, AM, FM, WFM, CW, I/Q, ADS-B, the names
// of modes and protocols.
//
// TRYING A CATALOGUE WITHOUT A BUILD: FOXSDR_LANG_FILE=<path to a .json>
// adds that file at start-up (addCatalogueFile, problems on stderr) before
// the language is applied, replacing a compiled-in catalogue of the same
// code; FOXSDR_LANGUAGE=<its code> then selects it. That is how a translator
// sees their work in place, and how the pseudo-language check is run.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_I18N_HPP
#define CASCADE_CORE_I18N_HPP

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

// Marks a literal for translation WITHOUT translating it where it stands -
// for tables of labels built at static-initialisation time, before any
// language is chosen. The literal is picked up by the catalogue check, and
// the site that DRAWS it calls tr() on the stored pointer.
#define FOX_TR_NOOP(s) s

namespace cascade::i18n {

struct Language {
    std::string code;         // BCP 47: "en", "pt-BR", "de" ...
    std::string name;         // in its own language: "Português (Brasil)"
    std::string englishName;  // "Portuguese (Brazil)"
    bool machine = false;     // translated by machine, not yet by a person
    std::size_t translated = 0;  // entries in the catalogue
};

// Every language the build carries. English first, then the catalogues in
// order of englishName. Never empty.
const std::vector<Language>& languages();

// The setting as stored in the config: "auto" (or empty) follows the
// operating system's language when a catalogue exists for it, otherwise
// English; anything else is a code from languages(). An unknown code
// resolves to English.
std::string resolve(const std::string& setting);

// Make `setting` (resolved as above) the active language. Returns the code
// actually in force. Call from the UI thread, between frames.
std::string setLanguage(const std::string& setting);

// The code in force ("en" until setLanguage says otherwise).
const std::string& current();

// The translation of `english` in the active language, or `english` itself
// when there is none (English selected, or the entry is missing). Never null
// for a non-null argument; null in, null out.
const char* tr(const char* english);

// For ImGui WIDGET LABELS, whose visible text is also their id. Returns the
// translated text followed by "###" and the ORIGINAL label, so the widget's
// id - and anything ImGui keyed on it, open state, focus, ini position -
// does not change with the language. With English active it returns `label`
// unchanged. A label that already carries "##suffix" keeps it: only the part
// before "##" is looked up.
const char* trId(const char* label);

// The hemisphere letter after a latitude (N/S) or a longitude (E/W), in the
// language in force: "W" is "O" in Spanish, Portuguese, French and Italian,
// "E" is "O" in German, and Polish keeps N/S/E/W (its own W would mean east).
// `positive` is north / east. Every place that prints a
// position uses this, so a position reads the same way everywhere on screen.
inline const char* hemisphereLetter(bool latitude, bool positive) {
    if (latitude) { return positive ? tr("N") : tr("S"); }
    return positive ? tr("E") : tr("W");
}

// The operating system's preferred UI language as a BCP 47 tag ("pt-BR"),
// or "" when it cannot be read.
std::string systemLocale();

// --- Below: the engine's own surface, for the settings page and the tests.

// resolve() with the system locale passed in rather than read, so the rule
// can be tested without changing the machine: "auto"/"" matches `locale`
// against the catalogues - the exact tag first (case-insensitive), then the
// primary language subtag ("pt-PT" finds "pt-BR" when that is the only
// Portuguese) - and falls back to "en". Any other setting is a code from
// languages(), matched case-insensitively and returned in its canonical
// spelling, or "en" when no catalogue has it. Either way a catalogue that is
// not drawable() here resolves to "en".
std::string resolveFor(const std::string& setting, const std::string& locale);

// The catalogue a language tag would pick by the auto rule above, or "" when
// none would (English included: "en-GB" answers ""). The settings page uses
// it to ask "is there a catalogue for the chosen country's language". A
// catalogue that is not drawable() here is not offered.
std::string matchCatalogue(const std::string& tag);

// The entry in languages() for `code` (exact spelling), or null.
const Language* findLanguage(const std::string& code);

// --- Whether this machine can DRAW a language --------------------------------
//
// A catalogue can exist and still be unreadable here: Chinese, Japanese and
// Korean are drawn in a typeface from the operating system (core/scripts.hpp
// says why), and a Linux desktop without one would letter the whole interface
// in boxes. So the font layer installs a predicate, and the language rules
// above consult it: "auto" never resolves to a language this machine cannot
// draw - it falls back to English - and neither does a saved choice, which
// is KEPT (the fonts may be installed tomorrow) but not applied.
//
// With no predicate installed every catalogue is drawable - the state the
// tests of the rules above run in, and what a build with no fonts layer gets.
using DrawablePredicate = bool (*)(const std::string& code);
void setDrawablePredicate(DrawablePredicate p);
bool drawable(const std::string& code);

// Every distinct code point (>= U+0020) the catalogue for `code` draws - its
// name and every translation - in ascending order. Empty for English and for
// a code with no catalogue. What the font layer checks a typeface against.
std::vector<unsigned int> codePoints(const std::string& code);

// Adds a catalogue from the text of a resources/lang/<code>.json file:
//     {"code":"pt-BR","name":"Português (Brasil)","englishName":"Portuguese
//      (Brazil)","machine":true,"strings":{"English key":"translation",...}}
// Returns false with a sentence in *error for input that is not such a
// document; nothing is added then. An entry whose translation is empty, or
// carries a different sequence of printf conversions from its key, or holds
// "##", is left out (English shows instead) and named in *warnings when
// given. A catalogue for a code already present REPLACES it for every later
// lookup, and the one it replaced is kept alive, so no pointer tr() has
// returned is ever invalidated. "en" is refused: English is not a catalogue.
bool addCatalogue(const std::string& json, std::string* error,
                  std::vector<std::string>* warnings = nullptr);

// addCatalogue() on the contents of the file at `path` - the FOXSDR_LANG_FILE
// seam above. Returns the catalogue's code, or "" when the file cannot be read
// or is refused. Every problem, and every entry left in English, is written to
// `diag` (stderr in the application) as a line naming the file, because the
// person reading it is a translator who has just saved that file and wants to
// know what in it did not take. A UTF-8 byte-order mark is skipped: editors on
// Windows still write one, and it is not the translator's mistake.
std::string addCatalogueFile(const std::string& path, std::FILE* diag);

// The printf conversion specifications in `s`, in order, each as its full
// text ("%-5.2f", "%zu", "%s"). "%%" is a literal and is not listed. A '%'
// that does not start a well-formed specification is listed as "%?" plus
// what followed it, so a stray percent sign has to match a stray percent
// sign. This is the rule addCatalogue and test_i18n hold translations to.
std::vector<std::string> formatSpecs(const char* s);

// "pt_BR.UTF-8" -> "pt-BR", "de_DE@euro" -> "de-DE", "C"/"POSIX"/"" -> "".
// The POSIX half of systemLocale(), separate so it can be tested.
std::string localeFromPosix(const std::string& value);

// The compiled-in catalogue files (see tools/embed-lang.py), for the test
// that proves they are the files in resources/lang.
struct EmbeddedFile {
    const char* name;
    const unsigned char* bytes;
    std::size_t len;
};
std::vector<EmbeddedFile> embeddedCatalogueFiles();

}  // namespace cascade::i18n

#endif  // CASCADE_CORE_I18N_HPP
