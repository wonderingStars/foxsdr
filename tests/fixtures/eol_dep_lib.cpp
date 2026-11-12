// eol_dep_lib.cpp - a stand-in DEPENDENCY: eol_needs_dep.dll imports eol_dep_fn from this library. The test copies the first library to a folder
// that does not hold this one, so loading it fails with the loader's "missing dependency" error (126), which must be an answer and never a box.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
extern "C" __declspec(dllexport) int eol_dep_fn(int x) { return x + 1; }
