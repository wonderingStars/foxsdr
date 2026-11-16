// The band plan's three palettes (window-gaps-2, rows 258, 269, 270; src/slint/bandplan_palette.hpp): Classic keeps the plan file's own colour for every service, Vivid is one saturated hue per service
// at one alpha, Mono is the skin's accent graded by alpha per service, and an unknown palette or service falls back (Classic, the faintest / light-grey class) rather than going blank.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "bandplan_palette.hpp"
#include "test_check.hpp"

using foxsdr::slintbands::bandPlanColour;

int main() {
    const char* services[] = {"broadcast", "amateur", "aviation", "marine", "mobile", "satellite", "iss", "something-new"};
    // Classic: the plan file's colour, untouched, for every service and for an unknown palette key
    for (const char* s : services) {
        CHECK(bandPlanColour("classic", s, 0x11223344u, 0xF0A840u) == 0x11223344u);
        CHECK(bandPlanColour("", s, 0x11223344u, 0xF0A840u) == 0x11223344u);
        CHECK(bandPlanColour("Neon", s, 0x11223344u, 0xF0A840u) == 0x11223344u);
    }
    // Vivid: one alpha for every class, a hue each, and the classic colour does not matter
    CHECK(bandPlanColour("vivid", "broadcast", 1u, 0u) == 0xFF8C0080u && bandPlanColour("vivid", "amateur", 2u, 0u) == 0x00B7FF80u && bandPlanColour("vivid", "iss", 3u, 0u) == 0xFFEA0080u);
    CHECK(bandPlanColour("vivid", "something-new", 4u, 0u) == 0xBDBDBD80u);
    for (const char* s : services) { CHECK((bandPlanColour("vivid", s, 0u, 0u) & 0xFFu) == 0x80u); }
    // Mono: the accent, graded by alpha; the classic colour does not matter and the accent's own alpha byte is not read
    CHECK(bandPlanColour("mono", "broadcast", 1u, 0xF0A840u) == 0xF0A84090u && bandPlanColour("mono", "iss", 1u, 0xF0A840u) == 0xF0A840A0u && bandPlanColour("mono", "something-new", 1u, 0xF0A840u) == 0xF0A84030u);
    CHECK(bandPlanColour("mono", "broadcast", 1u, 0xFFF0A840u) == 0xF0A84090u);
    // the three differ for one service, and Mono follows a different accent
    CHECK(bandPlanColour("classic", "marine", 0x01020360u, 0xF0A840u) != bandPlanColour("vivid", "marine", 0x01020360u, 0xF0A840u));
    CHECK(bandPlanColour("mono", "marine", 0u, 0xF0A840u) != bandPlanColour("mono", "marine", 0u, 0x7DF3FFu));
    return testSummary("test_slint_host_bandpalette");
}
