// safe_text_make.hpp - the ONE place a SharedString is built from a std::string_view without a validity check.
//
// slint::SharedString aborts the process on a byte that is not UTF-8 (0.99.112, crash groups 6CCFD0C0E58FC30C and
// 104F425230C2A742), so the program builds one from text it did not write only through safe_text.hpp. Text that is valid by
// construction (a key code of the toolkit, a path already converted by core::utf8OrAnsiToUtf8) goes through Make::from.
//
// tools/slint_implicit_text_probe.py compiles every src/slint file against a copy of Slint's header in which every
// std::string_view / const char* constructor and assignment is private except to this struct (a friend there), so any other
// route fails to compile. In the real header the constructor is public and this is a plain call.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef FOXSDR_SLINT_SAFE_TEXT_MAKE_HPP
#define FOXSDR_SLINT_SAFE_TEXT_MAKE_HPP

#include <slint.h>

#include <string_view>

namespace foxsdr::slintsafe {

struct Make {
    static slint::SharedString from(std::string_view s) { return slint::SharedString(s); }
};

}  // namespace foxsdr::slintsafe

#endif  // FOXSDR_SLINT_SAFE_TEXT_MAKE_HPP
