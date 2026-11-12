// eol_needs_dep_lib.cpp - see eol_dep_lib.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
extern "C" int eol_dep_fn(int);
extern "C" __declspec(dllexport) int eol_needs_dep_fn(int x) { return eol_dep_fn(x) * 2; }
