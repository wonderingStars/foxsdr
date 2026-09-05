// The FUNCTION SELECT rail's banks and their motion - see gui/rail_banks.hpp
// for why the rail has banks at all.
//
// Three things are pinned. A drawer's progress reaches exactly 1 and exactly
// 0, never overshoots, and lands on the target from ANY frame length - a
// stalled frame must not leave a section part-open, which is the one visible
// way this arithmetic could fail. The easing is a real smoothstep with no
// motion at either end. And every bank has a name, a caption and a function
// key, in the order the rail always drew its captions.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstring>

#include "gui/rail_banks.hpp"
#include "test_check.hpp"

using cascade::gui::RailBank;
using cascade::gui::kRailBankCount;
using cascade::gui::kRailDrawerSeconds;
using cascade::gui::railBankCaption;
using cascade::gui::railBankForFunctionKey;
using cascade::gui::railBankFromIndex;
using cascade::gui::railBankLabel;
using cascade::gui::railDrawerAdvance;
using cascade::gui::railDrawerVisible;
using cascade::gui::railEase;

int main() {
    // --- the banks are named, captioned and ordered ------------------------
    {
        int banks = kRailBankCount;  // a variable, or MSVC calls the check constant
        CHECK(banks == 5);
        const RailBank order[] = {RailBank::SignalPath, RailBank::Decode, RailBank::View,
                                  RailBank::Extend, RailBank::System};
        for (int i = 0; i < kRailBankCount; ++i) {
            CHECK(static_cast<int>(order[i]) == i);
            CHECK(std::strlen(railBankLabel(order[i])) > 0);
            CHECK(std::strlen(railBankCaption(order[i])) > 0);
            // The key is the short form of the caption, never longer than it.
            CHECK(std::strlen(railBankLabel(order[i])) <= std::strlen(railBankCaption(order[i])));
            CHECK(railBankForFunctionKey(i) == i);
        }
        CHECK(std::strcmp(railBankCaption(RailBank::SignalPath), "SIGNAL PATH") == 0);
        CHECK(std::strcmp(railBankLabel(RailBank::SignalPath), "SIGNAL") == 0);
        CHECK(std::strcmp(railBankLabel(RailBank::System), "SYSTEM") == 0);
        // Keys past the fifth, and no key at all, select nothing.
        CHECK(railBankForFunctionKey(5) == -1);
        CHECK(railBankForFunctionKey(-1) == -1);
        CHECK(railBankForFunctionKey(11) == -1);
    }

    // --- a saved bank index is clamped, never trusted ----------------------
    {
        CHECK(railBankFromIndex(0) == RailBank::SignalPath);
        CHECK(railBankFromIndex(4) == RailBank::System);
        CHECK(railBankFromIndex(-3) == RailBank::SignalPath);
        CHECK(railBankFromIndex(99) == RailBank::System);
    }

    // --- the easing is a smoothstep ------------------------------------------
    {
        CHECK(railEase(0.0f) == 0.0f);
        CHECK(railEase(1.0f) == 1.0f);
        CHECK_NEAR(railEase(0.5f), 0.5, 1e-6);
        CHECK(railEase(-0.5f) == 0.0f);
        CHECK(railEase(1.5f) == 1.0f);
        // Monotonic: no drawer ever moves backwards while opening.
        float prev = 0.0f;
        bool monotonic = true;
        for (int i = 1; i <= 100; ++i) {
            const float e = railEase(static_cast<float>(i) / 100.0f);
            if (e < prev) { monotonic = false; }
            prev = e;
        }
        CHECK(monotonic);
        // Slow at both ends, fast in the middle.
        CHECK(railEase(0.1f) - railEase(0.0f) < railEase(0.55f) - railEase(0.45f));
        CHECK(railEase(1.0f) - railEase(0.9f) < railEase(0.55f) - railEase(0.45f));
    }

    // --- a drawer unfolds in kRailDrawerSeconds, at any frame rate ---------
    {
        // 60 frames a second: lands on exactly 1, and takes the advertised
        // time to get there, give or take one frame.
        float p = 0.0f;
        int frames = 0;
        while (p < 1.0f && frames < 1000) {
            p = railDrawerAdvance(p, true, 1.0f / 60.0f);
            ++frames;
        }
        CHECK(p == 1.0f);
        const int expected = static_cast<int>(kRailDrawerSeconds * 60.0f + 0.999f);
        CHECK(frames >= expected - 1 && frames <= expected + 1);

        // Folding back is symmetric and lands on exactly 0.
        frames = 0;
        while (p > 0.0f && frames < 1000) {
            p = railDrawerAdvance(p, false, 1.0f / 60.0f);
            ++frames;
        }
        CHECK(p == 0.0f);
        CHECK(frames >= expected - 1 && frames <= expected + 1);
    }

    // --- a stalled frame lands on the target rather than overshooting ------
    {
        CHECK(railDrawerAdvance(0.0f, true, 5.0f) == 1.0f);
        CHECK(railDrawerAdvance(1.0f, false, 5.0f) == 0.0f);
        CHECK(railDrawerAdvance(0.3f, true, 100.0f) == 1.0f);
        // A zero or negative dt changes nothing - a paused clock is not a
        // reason to move.
        CHECK(railDrawerAdvance(0.3f, true, 0.0f) == 0.3f);
        CHECK(railDrawerAdvance(0.3f, true, -1.0f) == 0.3f);
        // Garbage in is clamped: a NaN or an overshot progress comes back sane.
        CHECK(railDrawerAdvance(1.7f, true, 0.0f) == 1.0f);
        CHECK(railDrawerAdvance(-0.4f, false, 0.0f) == 0.0f);
    }

    // --- visibility: drawn until the last pixel has gone -------------------
    {
        CHECK(!railDrawerVisible(0.0f));
        CHECK(railDrawerVisible(0.01f));
        CHECK(railDrawerVisible(1.0f));
        // Halfway through folding shut, the drawer is still on screen.
        CHECK(railDrawerVisible(railDrawerAdvance(1.0f, false, kRailDrawerSeconds * 0.5f)));
    }

    return testSummary("test_rail_banks");
}
