// eol_partial_lib.cpp - a stand-in library that exports ONE of the calls a bridge needs and not the rest (tests/test_g39ddc_source.cpp: the
// "missing entry points" refusal).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <windows.h>

extern "C" __declspec(dllexport) int __stdcall GetDeviceList(void*, unsigned, unsigned* c, unsigned* s) {
    if (c != nullptr) { *c = 0; }
    if (s != nullptr) { *s = 0; }
    return 1;
}
