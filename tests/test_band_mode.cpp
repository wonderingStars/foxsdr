// THE BAND-PLAN MODE CONVENTION (field report a09fe09722e4691e), as a table.
//
// core::conventionalModeForBand(service, hz) is pure: it names the demodulator
// a band's convention implies, or "" when no convention is confident enough to
// move the user's mode. Every row of the table in band_mode.hpp is here with
// the numbers written out, because the value of the feature is in the
// boundaries - the 60 m USB island inside the LSB half of HF, the 10 MHz
// sideband split, the FM broadcast span that stops where DAB starts.
//
// core::modeFollowsTypedBandChange is the gate: the mode moves only on a typed
// retune that CROSSES into a different band entry, never within one (FT8 on
// 7.074 MHz is USB inside a band whose phone convention is LSB), never out of
// RAW, and never without a suggestion.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/band_mode.hpp"

#include <cmath>
#include <cstring>

#include "test_check.hpp"

using cascade::core::BandEntry;
using cascade::core::conventionalModeForBand;
using cascade::core::modeFollowsTypedBandChange;

namespace {

struct Row {
    const char* service;
    double hz;
    const char* want;  // "" = no suggestion
    const char* why;
};

bool same(const char* a, const char* b) { return std::strcmp(a, b) == 0; }

}  // namespace

int main() {
    const Row rows[] = {
        // amateur: LSB below 10 MHz, the 60 m USB island, USB to 30 MHz, NFM above.
        {"amateur", 1.843e6, "LSB", "160 m phone"},
        {"amateur", 3.620e6, "LSB", "the report's own example: 80 m lands in LSB"},
        {"amateur", 7.074e6, "LSB", "40 m convention is LSB; within-band typing never reaches this"},
        {"amateur", 5.3585e6, "USB", "60 m channels are USB by licence condition"},
        {"amateur", 5.25e6, "USB", "60 m island, lower edge inclusive"},
        {"amateur", 5.45e6, "USB", "60 m island, upper edge inclusive"},
        {"amateur", 5.2499e6, "LSB", "just below the island"},
        {"amateur", 5.4501e6, "LSB", "just above the island"},
        {"amateur", 9.999999e6, "LSB", "the sideband split is at 10 MHz exactly"},
        {"amateur", 10.1e6, "USB", "30 m and everything to 30 MHz"},
        {"amateur", 14.2e6, "USB", "20 m phone"},
        {"amateur", 28.5e6, "USB", "10 m phone"},
        {"amateur", 29.999999e6, "USB", "the VHF split is at 30 MHz exactly"},
        {"amateur", 51.0e6, "NFM", "6 m FM"},
        {"amateur", 145.5e6, "NFM", "2 m FM"},
        {"amateur", 433.5e6, "NFM", "70 cm FM"},
        // broadcast: AM to 30 MHz, WFM in the FM bands, nothing above them.
        {"broadcast", 198.0e3, "AM", "LW"},
        {"broadcast", 909.0e3, "AM", "MW"},
        {"broadcast", 6.07e6, "AM", "49 m SW"},
        {"broadcast", 30.0e6, "AM", "AM span is inclusive at 30 MHz"},
        {"broadcast", 63.999e6, "", "between SW and the lowest FM band: no convention"},
        {"broadcast", 64.0e6, "WFM", "FM span, lower edge (OIRT)"},
        {"broadcast", 70.0e6, "WFM", "OIRT FM"},
        {"broadcast", 90.0e6, "WFM", "Japanese / CCIR FM"},
        {"broadcast", 108.0e6, "WFM", "FM span, upper edge inclusive"},
        {"broadcast", 108.000001e6, "", "past FM: DAB is not a demodulator"},
        {"broadcast", 225.0e6, "", "DAB band III"},
        // aviation: USB on HF, AM above.
        {"aviation", 5.598e6, "USB", "North Atlantic HF air route"},
        {"aviation", 118.1e6, "AM", "VHF airband voice (the Milan tester's band)"},
        {"aviation", 121.5e6, "AM", "guard"},
        {"aviation", 243.0e6, "AM", "UHF military guard is AM too"},
        // marine: USB on HF, NFM on the VHF channels.
        {"marine", 2.182e6, "USB", "HF distress/working"},
        {"marine", 8.291e6, "USB", "HF working channel"},
        {"marine", 156.8e6, "NFM", "VHF channel 16"},
        // iss: every downlink is FM.
        {"iss", 145.8e6, "NFM", "voice/SSTV downlink"},
        {"iss", 437.8e6, "NFM", "cross-band repeater"},
        // no confident convention: the mode stays the user's.
        {"satellite", 145.9e6, "", "linear transponders are SSB, FM birds are FM"},
        {"mobile", 446.00625e6, "", "analogue PMR beside digital trunking"},
        {"other", 27.555e6, "", "unclassified"},
        {"", 100.0e6, "", "missing service"},
        {"AMATEUR", 3.62e6, "", "service classes are exact, lower-case strings"},
    };
    for (const Row& r : rows) {
        const char* got = conventionalModeForBand(r.service, r.hz);
        if (!same(got, r.want)) {
            std::printf("FAIL %s @ %.6f MHz: want \"%s\" got \"%s\" (%s)\n", r.service,
                        r.hz / 1.0e6, r.want, got, r.why);
        }
        CHECK(same(got, r.want));
    }

    // Junk frequencies never suggest, whatever the service.
    CHECK(same(conventionalModeForBand("amateur", 0.0), ""));
    CHECK(same(conventionalModeForBand("amateur", -7.0e6), ""));
    CHECK(same(conventionalModeForBand("amateur", std::nan("")), ""));

    // --- the gate -----------------------------------------------------------
    // Two distinct entries of one plan, and the gap (null).
    BandEntry eighty;
    eighty.startHz = 3.5e6;
    eighty.endHz = 3.8e6;
    eighty.service = "amateur";
    BandEntry forty;
    forty.startHz = 7.0e6;
    forty.endHz = 7.2e6;
    forty.service = "amateur";

    // Crossing bands with a differing suggestion moves the mode.
    CHECK(modeFollowsTypedBandChange(&forty, &eighty, "USB", "LSB"));
    // In from a gap counts as a crossing.
    CHECK(modeFollowsTypedBandChange(nullptr, &eighty, "WFM", "LSB"));
    // Typing within ONE band never moves the mode - the FT8-at-7.074 case.
    CHECK(!modeFollowsTypedBandChange(&forty, &forty, "USB", "LSB"));
    // Already in the convention: nothing to do.
    CHECK(!modeFollowsTypedBandChange(&forty, &eighty, "LSB", "LSB"));
    // Out into a gap: no suggestion can exist, and none is followed.
    CHECK(!modeFollowsTypedBandChange(&eighty, nullptr, "LSB", ""));
    CHECK(!modeFollowsTypedBandChange(&eighty, nullptr, "LSB", "USB"));
    // No suggestion (a service without a convention): the mode stays.
    CHECK(!modeFollowsTypedBandChange(&forty, &eighty, "USB", ""));
    CHECK(!modeFollowsTypedBandChange(&forty, &eighty, "USB", nullptr));
    // RAW is a diagnostics hold: nothing moves it.
    CHECK(!modeFollowsTypedBandChange(&forty, &eighty, "RAW", "LSB"));
    // A null current mode is a caller error answered with "do nothing".
    CHECK(!modeFollowsTypedBandChange(&forty, &eighty, nullptr, "LSB"));

    return testSummary("test_band_mode");
}
