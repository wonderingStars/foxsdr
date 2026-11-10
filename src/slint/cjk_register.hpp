// Hands the CJK faces cjk_fonts.hpp chose for a language to the window (docs 6.62). Header-only so the unit that chooses stays free of Slint (the offline glyph test links it
// without the SDK). Slint's Window::window_handle().register_font_from_path adds the file's faces to the window's font collection once per path (the face is then found by its
// family name on a machine that has no such family installed - a Linux font file in the user's folder); a face registered for a language that was left stays registered.
//
// REGISTERING IS NOT ENOUGH, and that is measured (test_slint_cjk, docs 6.62): a registered face is not a glyph fallback for the family Inter, so Han characters the fallback
// Slint picks lacks were drawn blank or as a box. What draws them is naming the face as the Text's font-family, which the Theme global's cjk-family does for every typeface of the
// window; the caller sets it from `family` (empty for a language that needs none, which puts Inter and JetBrains Mono back).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <slint.h>

#include <cstdio>
#include <set>
#include <string>

#include "cjk_fonts.hpp"

namespace foxsdr::slintfonts {

struct Registered {
    std::string labels;   // "msyh.ttc#1", for the log
    std::string family;   // the font-family the window's typefaces become ("Microsoft YaHei UI"); empty when the language needs no CJK face or the machine has none
};

// Chooses and registers the faces for `languageCode`. Safe to call again for the same code.
inline Registered registerForLanguage(slint::Window& window, const std::string& languageCode) {
    static std::set<std::string> done;
    Registered out;
    for (const Face& face : chooseFaces(languageCode)) {
        if (out.family.empty()) { out.family = familyOf(face); }   // the first face is THE face of the standard (the classic's rule); a later one only adds characters
        if (!done.insert(face.path).second) {
            out.labels += (out.labels.empty() ? "" : ", ") + face.label + " (already)";
            continue;
        }
        const auto err = window.window_handle().register_font_from_path(slint::SharedString(face.path));
        if (err) {
            std::fprintf(stderr, "foxsdr-slint: font %s not registered: %s\n", face.path.c_str(), std::string(*err).c_str());
            continue;
        }
        out.labels += (out.labels.empty() ? "" : ", ") + face.label;
    }
    return out;
}

}  // namespace foxsdr::slintfonts
