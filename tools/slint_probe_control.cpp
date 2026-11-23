// slint_probe_control.cpp - the POSITIVE CONTROL of tools/slint_implicit_text_probe.py. It is DELIBERATELY UNGUARDED and never part of the
// product. The probe compiles every statement below ON ITS OWN (one translation unit per statement: the compiler reports a repeated
// identical diagnostic only once per unit, which would hide a line) inside
//
//     void probe_case(const std::string& s, const char* p, char* q) { char buf[16] = "x"; slint::SharedString b; std::vector<slint::SharedString> v; <statement> }
//
// with a real unit's flags and the patched copy of Slint's headers. A statement ending in "MUST-FLAG" builds a SharedString from
// text that has not been checked, and the compiler must refuse it with the deleted / private SharedString overload. A statement
// ending in "MUST-PASS" is authored text and must compile. The probe FAILS the ctest when a MUST-FLAG statement is not refused (the
// probe has stopped looking: wrong header, wrong flags, a Slint upgrade that changed the header) or a MUST-PASS one is refused.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <slint.h>

#include <string>
#include <string_view>
#include <vector>

// CASES-BEGIN
slint::SharedString a1{std::string_view(s)};      // MUST-FLAG
slint::SharedString a2{p};                        // MUST-FLAG
slint::SharedString a3 = slint::SharedString(q);  // MUST-FLAG
slint::SharedString a4(buf);                      // MUST-FLAG
slint::SharedString a5(s.c_str());                // MUST-FLAG
slint::SharedString a6 = std::string_view(s);     // MUST-FLAG
slint::SharedString a7 = p;                       // MUST-FLAG
slint::SharedString a8{q};                        // MUST-FLAG
b = std::string_view(s);                          // MUST-FLAG
b = p;                                            // MUST-FLAG
b = buf;                                          // MUST-FLAG
b += std::string_view(s);                         // MUST-FLAG
b += buf;                                         // MUST-FLAG
auto c1 = b + std::string_view(s);                // MUST-FLAG
auto c2 = b + buf;                                // MUST-FLAG
v.emplace_back(std::string_view(s));              // MUST-FLAG
v.push_back(p);                                   // MUST-FLAG
slint::SharedString ok1("a literal");             // MUST-PASS
b = "another literal";                            // MUST-PASS
b += "more";                                      // MUST-PASS
auto ok2 = b + "tail";                            // MUST-PASS
auto ok3 = slint::SharedString::from_number(3);   // MUST-PASS
v.emplace_back("literal row");                    // MUST-PASS
