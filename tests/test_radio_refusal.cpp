// test_radio_refusal.cpp - A REFUSED RADIO CHOICE SAYS WHY (0.99.113): the words of the Source line (core/radio_refusal.hpp). No window, no engine: the engine half is tests/test_engine_api_source_refusal.cpp; the classic window's wiring is
// tests/test_refusal_popup_app.cpp; the new window's Source error line is src/slint/tests/test_slint_host_signal.cpp.
//
//   A  THE SENTENCES: the three holders' words, exactly (the Source line's); nothing for no holder; no path and no serial in any of them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstring>
#include <string>

#include "test_check.hpp"
#include "core/radio_refusal.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace rf = cascade::core::refusal;

namespace {

void testTheSentences() {
    CHECK_EQ(rf::sourceSentence(rf::kHolderPatchPart, 3, "Aerial B200"), std::string("This radio is set as the radio of the part \"Aerial B200\" on the Patch page. Change or remove that part, or start the patch to use the radio there."));
    CHECK_EQ(rf::sourceSentence(rf::kHolderPatchPart, 3, ""), std::string("This radio is set as the radio of the part \"Radio\" on the Patch page. Change or remove that part, or start the patch to use the radio there."));   // cleared caption: the kind the part still shows
    CHECK_EQ(rf::sourceSentence(rf::kHolderReceiver, 2), std::string("This radio is in use by Radio 2. Choose another radio there first."));
    CHECK_EQ(rf::sourceSentence(rf::kHolderTransmitter, 0), std::string("This radio is in use by the transmitter."));
    CHECK_EQ(rf::sourceSentence(rf::kHolderNone, 0), std::string());
    // no path and no serial in any of them
    for (int k : {rf::kHolderPatchPart, rf::kHolderReceiver, rf::kHolderTransmitter}) {
        const std::string s = rf::sourceSentence(k, 1);
        CHECK(s.find('\\') == std::string::npos && s.find('/') == std::string::npos && s.find("serial") == std::string::npos);
    }
}

}  // namespace

int main() {
    std::printf("test_radio_refusal\n");
    testTheSentences();
    return testSummary("test_radio_refusal");
}
