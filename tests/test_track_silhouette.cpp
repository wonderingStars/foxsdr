// Tests for gui/track_silhouette.hpp — which shape the map and the radar scope
// draw for a track, given the category the aircraft broadcast about itself.
//
// The drawing itself needs an ImDrawList and is not exercised here. Everything
// that DECIDES is pure, and it is the deciding that can be wrong in a way
// nobody notices: a shape that silently falls back to the generic aeroplane
// looks exactly like an aircraft that stated no category, and the whole point
// of the feature is that those two are different.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/track_silhouette.hpp"

#include <cstring>
#include <set>
#include <vector>

#include "test_check.hpp"

using cascade::gui::kSilAirliner;
using cascade::gui::kSilhouetteMaxVertices;
using cascade::gui::Silhouette;
using cascade::gui::silhouetteForCategory;
using cascade::gui::trackCategory;

namespace {

// Two silhouettes are the same PICTURE when their points are the same, which
// is not the same question as whether they are the same table: several
// categories deliberately share one shape, and a test that compared pointers
// would pass if a future edit copied the airliner table instead of reusing it.
bool samePicture(const Silhouette& a, const Silhouette& b) {
    if (a.rotor != b.rotor) { return false; }
    if (a.rotor) { return true; }
    if (a.count != b.count) { return false; }
    for (int i = 0; i < a.count; ++i) {
        if (a.half[i][0] != b.half[i][0] || a.half[i][1] != b.half[i][1]) { return false; }
    }
    return true;
}

std::vector<std::uint32_t> everyCategory() {
    std::vector<std::uint32_t> v;
    for (std::uint32_t c = 0; c < CASCADE_AIRCRAFT_CATEGORY_COUNT; ++c) { v.push_back(c); }
    return v;
}

}  // namespace

int main() {
    // --- the generic aeroplane is EXACTLY what it always was ----------------
    //
    // This is the compatibility promise of the whole feature: an aircraft that
    // broadcasts no category - and one in twenty-five does not, measured off
    // real air - must draw the shape this application has drawn since before
    // categories existed. Pinned coordinate by coordinate, because a silent
    // re-style of the default shape would change every map in the field while
    // every other test here still passed.
    {
        const Silhouette s = silhouetteForCategory(CASCADE_AIRCRAFT_NONE);
        CHECK(!s.rotor);
        CHECK(s.count == 11);
        const float want[11][2] = {
            {0.00f, -1.00f}, {0.13f, -0.70f}, {0.13f, -0.26f}, {0.98f, 0.16f},
            {0.98f, 0.38f},  {0.13f, 0.20f},  {0.13f, 0.62f},  {0.48f, 0.88f},
            {0.48f, 1.02f},  {0.08f, 0.94f},  {0.00f, 0.96f},
        };
        int wrong = 0;
        for (int i = 0; i < 11 && i < s.count; ++i) {
            if (s.half[i][0] != want[i][0] || s.half[i][1] != want[i][1]) { ++wrong; }
        }
        CHECK(wrong == 0);
    }

    // --- the shapes the owner asked for are genuinely DIFFERENT -------------
    //
    // "Light aircraft, microlights, jumbos" was the request. If any two of
    // those resolve to the same outline the feature does not exist, however
    // correctly the category was decoded.
    {
        const Silhouette light = silhouetteForCategory(CASCADE_AIRCRAFT_LIGHT);
        const Silhouette heavy = silhouetteForCategory(CASCADE_AIRCRAFT_HEAVY);
        const Silhouette micro = silhouetteForCategory(CASCADE_AIRCRAFT_ULTRALIGHT);
        const Silhouette glider = silhouetteForCategory(CASCADE_AIRCRAFT_GLIDER);
        const Silhouette generic = silhouetteForCategory(CASCADE_AIRCRAFT_NONE);
        CHECK(!samePicture(light, heavy));
        CHECK(!samePicture(light, micro));
        CHECK(!samePicture(heavy, micro));
        CHECK(!samePicture(glider, light));
        CHECK(!samePicture(glider, micro));
        // ...and none of them is the generic shape, which is what would happen
        // if a case label were dropped from the switch.
        CHECK(!samePicture(light, generic));
        CHECK(!samePicture(heavy, generic));
        CHECK(!samePicture(micro, generic));
        CHECK(!samePicture(glider, generic));
    }

    // --- a helicopter is not a winged shape at all --------------------------
    {
        CHECK(silhouetteForCategory(CASCADE_AIRCRAFT_ROTORCRAFT).rotor);
        // And nothing else is, or the disc stops meaning "helicopter".
        int rotors = 0;
        for (std::uint32_t c : everyCategory()) {
            if (silhouetteForCategory(c).rotor) { ++rotors; }
        }
        CHECK(rotors == 1);
    }

    // --- the deliberate sharing is deliberate -------------------------------
    //
    // Medium 1, Medium 2 and high vortex are all airliners; the shape is the
    // generic one on purpose. Asserted so that "they look the same" is a
    // recorded decision rather than something to be rediscovered as a bug.
    {
        const Silhouette generic = silhouetteForCategory(CASCADE_AIRCRAFT_NONE);
        CHECK(samePicture(silhouetteForCategory(CASCADE_AIRCRAFT_MEDIUM1), generic));
        CHECK(samePicture(silhouetteForCategory(CASCADE_AIRCRAFT_MEDIUM2), generic));
        CHECK(samePicture(silhouetteForCategory(CASCADE_AIRCRAFT_HIGH_VORTEX), generic));
        const Silhouette balloon = silhouetteForCategory(CASCADE_AIRCRAFT_LIGHTER_THAN_AIR);
        CHECK(samePicture(silhouetteForCategory(CASCADE_AIRCRAFT_PARACHUTIST), balloon));
        const Silhouette surf = silhouetteForCategory(CASCADE_AIRCRAFT_SURFACE_SERVICE);
        CHECK(samePicture(silhouetteForCategory(CASCADE_AIRCRAFT_SURFACE_EMERGENCY), surf));
        CHECK(samePicture(silhouetteForCategory(CASCADE_AIRCRAFT_GROUND_OBSTRUCTION), surf));
        // The things on the ground must NOT look like the things in the air.
        CHECK(!samePicture(surf, generic));
        CHECK(!samePicture(balloon, generic));
        CHECK(!samePicture(surf, balloon));
    }

    // --- a category this build has never heard of is safe -------------------
    //
    // A newer plugin may report a value added after this host shipped. It must
    // draw the generic aeroplane rather than reading off the end of anything.
    {
        const Silhouette generic = silhouetteForCategory(CASCADE_AIRCRAFT_NONE);
        CHECK(samePicture(silhouetteForCategory(CASCADE_AIRCRAFT_CATEGORY_COUNT), generic));
        CHECK(samePicture(silhouetteForCategory(31u), generic));
        CHECK(samePicture(silhouetteForCategory(999u), generic));
    }

    // --- every shape fits the buffer it is drawn through --------------------
    //
    // addTrackSymbol mirrors the half outline into a fixed array; a table that
    // overran it would be a stack write, and the count is the only thing that
    // bounds it.
    {
        int worst = 0;
        for (std::uint32_t c : everyCategory()) {
            const Silhouette s = silhouetteForCategory(c);
            if (s.rotor) { continue; }
            CHECK(s.half != nullptr);
            CHECK(s.count >= 3);
            const int mirrored = 2 * s.count - 2;
            if (mirrored > worst) { worst = mirrored; }
        }
        CHECK(worst <= kSilhouetteMaxVertices);
        // And the headroom is real rather than accidental - if this ever
        // reads equal, the next shape added will silently clip.
        CHECK(worst < kSilhouetteMaxVertices);
    }

    // --- the bits only mean anything for aircraft ---------------------------
    //
    // The ABI leaves the category bits undefined for other kinds. A ship whose
    // plugin happens to set a bit in that range must not acquire wings.
    {
        const std::uint32_t flags = CASCADE_TRACK_WITH_CATEGORY(CASCADE_AIRCRAFT_HEAVY);
        CHECK(trackCategory(CASCADE_TRACK_AIRCRAFT, flags) == CASCADE_AIRCRAFT_HEAVY);
        CHECK(trackCategory(CASCADE_TRACK_VESSEL, flags) == CASCADE_AIRCRAFT_NONE);
        CHECK(trackCategory(CASCADE_TRACK_SATELLITE, flags) == CASCADE_AIRCRAFT_NONE);
        CHECK(trackCategory(CASCADE_TRACK_STATION, flags) == CASCADE_AIRCRAFT_NONE);
        CHECK(trackCategory(CASCADE_TRACK_UNKNOWN, flags) == CASCADE_AIRCRAFT_NONE);
    }

    // --- packing and unpacking round-trips, and stays in its own bits -------
    {
        for (std::uint32_t c = 0; c < CASCADE_AIRCRAFT_CATEGORY_COUNT; ++c) {
            const std::uint32_t f = CASCADE_TRACK_WITH_CATEGORY(c);
            CHECK(CASCADE_TRACK_CATEGORY(f) == c);
            // It must not collide with the flags that were there first.
            CHECK((f & CASCADE_TRACK_FLAG_EMERGENCY) == 0u);
            CHECK((f & CASCADE_TRACK_FLAG_SELECTED) == 0u);
        }
        // An emergency heavy is both, and reading one does not disturb the
        // other - this is the combination the renderer actually meets.
        const std::uint32_t both =
            CASCADE_TRACK_FLAG_EMERGENCY | CASCADE_TRACK_WITH_CATEGORY(CASCADE_AIRCRAFT_HEAVY);
        CHECK((both & CASCADE_TRACK_FLAG_EMERGENCY) != 0u);
        CHECK(CASCADE_TRACK_CATEGORY(both) == CASCADE_AIRCRAFT_HEAVY);
        // A value too wide for the field is truncated to the field rather than
        // leaking into the neighbouring bits.
        CHECK((CASCADE_TRACK_WITH_CATEGORY(0xFFFFu) & ~CASCADE_TRACK_CATEGORY_MASK) == 0u);
    }

    return testSummary("test_track_silhouette");
}
