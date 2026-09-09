// Tests for gui/tune_control.hpp - the two pure decisions behind the two
// tuning bugs fixed alongside this file: a band preset zeroing the VFO
// offset it should have zeroed for an I/Q decoder but keeping for an audio
// one, and a radio that lands somewhere other than it was asked with
// nothing anywhere saying so.
//
// Both live in a header with no ImGui, no pipeline and no live source
// because that is the only way either is checkable at all: the first is a
// one-line decision AppWindow used to make inline, and the second used to be
// unobservable outside a live retune against real (or absent) hardware.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "core/plugin_abi.h"
#include "gui/tune_control.hpp"
#include "test_check.hpp"

using cascade::gui::autoPresetIndexOnStart;
using cascade::gui::autoPresetTriggersOnWindowClick;
using cascade::gui::digitPlaceHz;
using cascade::gui::freqCellLeftX;
using cascade::gui::FreqRect;
using cascade::gui::kFreqBezelH;
using cascade::gui::kFreqBezelPadX;
using cascade::gui::kFreqBezelPadY;
using cascade::gui::kFreqBezelW;
using cascade::gui::kFreqBezelY;
using cascade::gui::kFreqCellW;
using cascade::gui::kFreqDigitCells;
using cascade::gui::kFreqPlateH;
using cascade::gui::kFreqPlatePadX;
using cascade::gui::kFreqPlateW;
using cascade::gui::kFreqSwitchH;
using cascade::gui::kFreqSwitchHalfH;
using cascade::gui::kFreqTubeH;
using cascade::gui::kFreqTubeSwitchGap;
using cascade::gui::switchRectForCell;
using cascade::gui::tubeRectForCell;
using cascade::gui::kTuneMismatchToleranceHz;
using cascade::gui::presetVfoOffsetHz;
using cascade::gui::stepDigit;
using cascade::gui::tuneMismatchMessage;
using cascade::gui::kDeckCoreW;
using cascade::gui::kDeckMinWindowW;
using cascade::gui::kFirstLaunchBarW;
using cascade::gui::kMeterCoreClearance;
using cascade::gui::kMeterGap;
using cascade::gui::kMeterRightMargin;
using cascade::gui::kMeterW;
using cascade::gui::kMuteBannerMinW;
using cascade::gui::meter1XOnBar;
using cascade::gui::meter2XOnBar;
using cascade::gui::metersFitOnBar;
using cascade::gui::muteBannerMiddleW;
using cascade::gui::muteBannerTakesTheMiddle;

namespace {
// A minimal audio preset (no CASCADE_PRESET_DEVICE_CENTRE): frequencyHz and
// bandwidthHz are all autoPresetIndexOnStart reads for this kind.
CascadePreset audioPreset(double freqHz, double bwHz) {
    CascadePreset ps{};
    ps.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
    ps.frequencyHz = freqHz;
    ps.bandwidthHz = bwHz;
    ps.flags = 0u;
    return ps;
}

// A minimal band preset (CASCADE_PRESET_DEVICE_CENTRE set): only frequencyHz
// and the flag matter to this function.
CascadePreset bandPreset(double freqHz) {
    CascadePreset ps{};
    ps.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
    ps.frequencyHz = freqHz;
    ps.flags = CASCADE_PRESET_DEVICE_CENTRE;
    return ps;
}
}  // namespace

namespace {
// An identity, read through a function so MSVC does not fold the comparison
// below into a compile-time constant and flag it C4127 - see the same trick
// in test_scope_view.cpp.
template <typename T>
T asValue(T v) {
    return v;
}
}  // namespace

int main() {
    // --- presetVfoOffsetHz ----------------------------------------------------
    {
        // THE ABI FLAG ITSELF, pinned: presetVfoOffsetHz keys on this exact
        // bit, and a plugin compiled against a different value would silently
        // stop zeroing the offset it asked to have zeroed. RED WHEN version 3
        // of the preset descriptor renumbers it.
        CHECK(asValue<std::uint32_t>(CASCADE_PRESET_DEVICE_CENTRE) == 0x00000001u);

        // AN I/Q DECODER'S PRESET: the offset is zeroed regardless of what the
        // user had it set to - this is the ADS-B measurement itself (a
        // remembered -12 kHz walked the device 12 kHz off 1090 MHz).
        CHECK(presetVfoOffsetHz(CASCADE_PRESET_DEVICE_CENTRE, -12000.0) == 0.0);
        CHECK(presetVfoOffsetHz(CASCADE_PRESET_DEVICE_CENTRE, 45000.0) == 0.0);
        CHECK(presetVfoOffsetHz(CASCADE_PRESET_DEVICE_CENTRE, 0.0) == 0.0);

        // AN AUDIO DECODER'S PRESET (no flag): the user's offset survives
        // untouched, because the VFO is where their station actually is.
        CHECK(presetVfoOffsetHz(0u, -12000.0) == -12000.0);
        CHECK(presetVfoOffsetHz(0u, 0.0) == 0.0);

        // A flag set ALONGSIDE others still zeroes the offset - this is an OR
        // of bits, not an exact-match test.
        CHECK(presetVfoOffsetHz(CASCADE_PRESET_DEVICE_CENTRE | 0x00000002u, 5000.0) == 0.0);
    }

    // --- tuneMismatchMessage: within tolerance ---------------------------------
    {
        // EXACTLY ON THE MARK.
        CHECK(tuneMismatchMessage(1090.0e6, 1090.0e6, true, 70.0e6, 6.0e9, false).empty());
        // A COERCED FEW HERTZ - what a real synthesiser's step size actually
        // costs, and the case this must never annotate.
        CHECK(tuneMismatchMessage(1090.0e6, 1090.0e6 + 3.0, true, 70.0e6, 6.0e9, false).empty());
        CHECK(tuneMismatchMessage(1090.0e6, 1090.0e6 - 3.0, false, 0.0, 0.0, false).empty());
        // No range and no preset: still empty when it landed on the mark.
        CHECK(tuneMismatchMessage(100.0e6, 100.0e6, false, 0.0, 0.0, false).empty());
    }

    // --- the tolerance boundary -------------------------------------------------
    {
        // EXACTLY AT THE NAMED CONSTANT counts as "coerced", not "refused" -
        // "more than" is the documented rule, so a difference equal to the
        // tolerance must not trigger. RED WHEN the comparison becomes >=.
        CHECK(tuneMismatchMessage(100.0e6, 100.0e6 + kTuneMismatchToleranceHz, true, 70.0e6,
                                   6.0e9, false)
                  .empty());
        CHECK(tuneMismatchMessage(100.0e6, 100.0e6 - kTuneMismatchToleranceHz, true, 70.0e6,
                                   6.0e9, false)
                  .empty());
        // One Hz past it fires, either side.
        CHECK(!tuneMismatchMessage(100.0e6, 100.0e6 + kTuneMismatchToleranceHz + 1.0, true,
                                    70.0e6, 6.0e9, false)
                   .empty());
        CHECK(!tuneMismatchMessage(100.0e6, 100.0e6 - kTuneMismatchToleranceHz - 1.0, true,
                                    70.0e6, 6.0e9, false)
                   .empty());
    }

    // --- the exact wording, with a known range ---------------------------------
    // THE MEASURED CASE ITSELF: a B200 (70 MHz - 6 GHz) asked for 7.000 MHz and
    // answered 30.800 MHz. RED WHEN the wording drifts from what was measured
    // and specified - this is the sentence a user actually has to read and act
    // on, not a paraphrase of it.
    {
        const std::string msg =
            tuneMismatchMessage(7.0e6, 30.8e6, true, 70.0e6, 6.0e9, false);
        CHECK(msg ==
              "This radio cannot tune to 7.000 MHz - it answered 30.800 MHz. "
              "Its range is 70.000 MHz to 6.000 GHz.");
    }

    // --- without a range: the shorter wording -----------------------------------
    // The signal generator and the IQ file never call this at all (no source
    // owns a Soapy device to ask), and a Soapy driver that answers no range
    // takes the same path: the range sentence is dropped rather than printing
    // a lie built from a sentinel.
    {
        const std::string msg = tuneMismatchMessage(7.0e6, 30.8e6, false, 0.0, 0.0, false);
        CHECK(msg == "This radio cannot tune to 7.000 MHz - it answered 30.800 MHz.");
    }

    // --- a plugin preset outside the device's range -----------------------------
    // THE WEFAX MEASUREMENT: a preset for 3.853 MHz answered 34.000 MHz. The
    // preset-specific sentence is appended AFTER the range sentence, and only
    // when a mismatch was already found - a preset that lands exactly where it
    // asked gets no lecture about hardware it did not need.
    {
        const std::string msg =
            tuneMismatchMessage(3.853e6, 34.0e6, true, 70.0e6, 6.0e9, true);
        CHECK(msg ==
              "This radio cannot tune to 3.853 MHz - it answered 34.000 MHz. "
              "Its range is 70.000 MHz to 6.000 GHz. "
              "This preset needs a receiver that covers that band.");
        CHECK(tuneMismatchMessage(3.853e6, 3.853e6, true, 70.0e6, 6.0e9, true).empty());
    }

    // --- a plugin preset with no known range ------------------------------------
    {
        const std::string msg = tuneMismatchMessage(3.853e6, 34.0e6, false, 0.0, 0.0, true);
        CHECK(msg ==
              "This radio cannot tune to 3.853 MHz - it answered 34.000 MHz. "
              "This preset needs a receiver that covers that band.");
    }

    // --- rounding of the MHz figures ---------------------------------------------
    // %.3f rounds, so a figure a whisker under a round number must still read
    // as the round number rather than printing a false extra digit's worth of
    // precision the receiver never had.
    {
        const std::string msg =
            tuneMismatchMessage(6999999999.6, 6999999999.6 + 600.0, true, 70.0e6, 6.0e9, false);
        CHECK(msg.find("7.000 GHz") != std::string::npos);
        // Just under the GHz switchover: still three MHz digits, not four.
        const std::string near =
            tuneMismatchMessage(999999999.6, 999999999.6 + 600.0, false, 0.0, 0.0, false);
        CHECK(near.find("1000.000 MHz") != std::string::npos);
    }

    // --- autoPresetIndexOnStart: "we want the user to have to do nothing" -----
    {
        // NO PRESETS AT ALL: nothing to apply, whatever the receiver is doing.
        CHECK(autoPresetIndexOnStart({}, 100.0e6, 0.0, 2.0e6) == -1);
    }
    {
        // ALREADY INSIDE AN AUDIO PRESET: tuned freq (centre + offset) sits
        // inside max(bw/2, 5kHz) of the preset - nothing to do. 162.400 MHz
        // tuned (100.000 MHz device + 62.400 MHz offset - an exaggerated
        // offset only to keep the arithmetic obvious) against a 25 kHz-wide
        // preset AT 162.400 MHz.
        const std::vector<CascadePreset> presets = {audioPreset(162.4e6, 25000.0)};
        CHECK(autoPresetIndexOnStart(presets, 100.0e6, 62.4e6, 2.0e6) == -1);
    }
    {
        // JUST OUTSIDE an audio preset's tolerance: apply preset 0. Tolerance
        // is max(25000/2, 5000) = 12500 Hz; 20 kHz away is outside it.
        const std::vector<CascadePreset> presets = {audioPreset(162.4e6, 25000.0)};
        CHECK(autoPresetIndexOnStart(presets, 100.0e6, 62.42e6, 2.0e6) == 0);
    }
    {
        // A BANDWIDTH OF ZERO ("no preference") still floors at 5 kHz rather
        // than collapsing the tolerance to zero - 4 kHz away counts as
        // inside, 6 kHz away does not.
        const std::vector<CascadePreset> presets = {audioPreset(100.0e6, 0.0)};
        CHECK(autoPresetIndexOnStart(presets, 0.0, 100.004e6, 0.0) == -1);
        CHECK(autoPresetIndexOnStart(presets, 0.0, 100.006e6, 0.0) == 0);
    }
    {
        // INSIDE A BAND PRESET: device centre within half the sample rate of
        // 1090 MHz at 2 MS/s (tolerance 1 MHz) - the ADS-B case itself.
        const std::vector<CascadePreset> presets = {bandPreset(1090.0e6)};
        CHECK(autoPresetIndexOnStart(presets, 1090.5e6, 0.0, 2.0e6) == -1);
    }
    {
        // OUTSIDE a band preset's tolerance: apply preset 0. Device parked at
        // 100 MHz, 1090 MHz band preset at 2 MS/s (tolerance 1 MHz) - 990 MHz
        // away is nowhere near it.
        const std::vector<CascadePreset> presets = {bandPreset(1090.0e6)};
        CHECK(autoPresetIndexOnStart(presets, 100.0e6, 0.0, 2.0e6) == 0);
    }
    {
        // SEVERAL PRESETS, receiver inside the THIRD (index 2): still -1, not
        // "outside the first two so apply preset 0" - any match means the
        // decoder is already listening for this plugin.
        const std::vector<CascadePreset> presets = {
            audioPreset(10.0e6, 10000.0),
            audioPreset(20.0e6, 10000.0),
            audioPreset(30.0e6, 10000.0),
        };
        CHECK(autoPresetIndexOnStart(presets, 30.0e6, 0.0, 0.0) == -1);
        // And truly outside all three: preset 0, not preset 2.
        CHECK(autoPresetIndexOnStart(presets, 99.0e6, 0.0, 0.0) == 0);
    }
    {
        // ZERO DEVICE RATE against a band preset: no source open (or one that
        // has not answered a rate yet) collapses the tolerance to zero rather
        // than crashing or accepting anything as "inside" - so this must
        // still say "apply preset 0" rather than -1, and only an EXACT
        // landing on the preset's own frequency reads as already there.
        const std::vector<CascadePreset> presets = {bandPreset(1090.0e6)};
        CHECK(autoPresetIndexOnStart(presets, 100.0e6, 0.0, 0.0) == 0);
        CHECK(autoPresetIndexOnStart(presets, 1090.0e6, 0.0, 0.0) == -1);
    }

    // --- autoPresetTriggersOnWindowClick: the DECODE-row gesture ---------------
    {
        // SHOWN BY CLICK: a row pressed while its window was hidden - the
        // gesture this feature exists to catch.
        CHECK(autoPresetTriggersOnWindowClick(true, false) == true);
        // HIDDEN BY CLICK: the same row, pressed while already shown - a
        // close, not a "give me this plugin". RED WHEN this collapses to a
        // bare `clicked`, which would re-apply a preset on every close too.
        CHECK(autoPresetTriggersOnWindowClick(true, true) == false);
        // NO CLICK THIS FRAME, window currently hidden - an ordinary frame
        // nobody touched the row on.
        CHECK(autoPresetTriggersOnWindowClick(false, false) == false);
        // NO CLICK THIS FRAME, window currently shown - covers a
        // restored-at-startup window too: nothing ever calls this without a
        // click, but if it were, "already shown, nobody clicked" must still
        // say no rather than retuning on every frame a window stays open.
        CHECK(autoPresetTriggersOnWindowClick(false, true) == false);
    }

    // --- digitPlaceHz: the ten cells, most significant first ------------------
    {
        CHECK(kFreqDigitCells == 10);
        CHECK(digitPlaceHz(0) == 1.0e9);   // leftmost cell: 1 GHz
        CHECK(digitPlaceHz(1) == 1.0e8);
        CHECK(digitPlaceHz(2) == 1.0e7);
        CHECK(digitPlaceHz(3) == 1.0e6);
        CHECK(digitPlaceHz(4) == 1.0e5);
        CHECK(digitPlaceHz(5) == 1.0e4);
        CHECK(digitPlaceHz(6) == 1.0e3);
        CHECK(digitPlaceHz(7) == 1.0e2);
        CHECK(digitPlaceHz(8) == 1.0e1);
        CHECK(digitPlaceHz(9) == 1.0e0);   // rightmost cell: 1 Hz
        // A decade ladder, most to least significant.
        for (int i = 1; i < kFreqDigitCells; ++i) {
            CHECK(digitPlaceHz(i) == digitPlaceHz(i - 1) / 10.0);
        }
    }

    // --- stepDigit: one step, either way, clamped at 0 Hz ----------------------
    {
        // AN ORDINARY STEP, most and least significant cell alike.
        CHECK(stepDigit(1000.0, 0, true) == 1000.0 + 1.0e9);
        CHECK(stepDigit(1000.0, 9, true) == 1001.0);
        CHECK(stepDigit(1000.0, 9, false) == 999.0);

        // A STEP DOWN BELOW ZERO CLAMPS TO 0 - a tune may never ask the
        // source for a negative centre. RED WHEN this goes negative instead.
        CHECK(stepDigit(0.0, 9, false) == 0.0);
        CHECK(stepDigit(5.0, 6, false) == 0.0);  // 5 Hz down a whole kHz place

        // A STEP UP FROM 999,999,999 INTO THE 1 GHz CELL: the rightmost
        // cell's own step still carries into the next figure - this is plain
        // double arithmetic, not per-digit carrying, so it works the same as
        // any other addition.
        CHECK(stepDigit(999999999.0, 9, true) == 1000000000.0);

        // A STEP UP AT THE TOP CELL has nowhere to clamp - it simply grows.
        CHECK(stepDigit(9000000000.0, 0, true) == 10000000000.0);
    }

    // --- the plate's own pinned size -----------------------------------------
    // Ten 28-unit tubes with nine 6-unit gaps (280 + 54) inside a bezel padded
    // 5 a side, on a plate padded 10 a side: 364 wide - the compact cut the
    // owner asked for ("we don't want to affect the size of the top bar -
    // it's perfect the way we have it"), which app_window.cpp places its
    // second divider from. Top to bottom: 5 of padding, the 12-unit name
    // plate strip, a 4 gap, the bezel (4 + 40 tube + 4 + 30 switch + 4 =
    // 82), a 4 gap, the 9-unit footer and 5 of padding: 121 tall, inside the
    // 160-unit bar the deck has always had. drawToolbar's static_asserts
    // check the plate against its divider and the bar from these two
    // numbers, so a change here shows up here first.
    {
        CHECK_NEAR(kFreqTubeH, 40.0f, 1.0e-4f);
        CHECK_NEAR(kFreqBezelW, 344.0f, 1.0e-4f);
        CHECK_NEAR(kFreqBezelH, 82.0f, 1.0e-4f);
        CHECK_NEAR(kFreqPlateW, 364.0f, 1.0e-4f);
        CHECK_NEAR(kFreqPlateH, 121.0f, 1.0e-4f);
    }

    // --- freqCellLeftX: one column, shared by the tube and its switch ---------
    {
        // Cell 0 sits one plate padding and one bezel padding in from the
        // plate's own left edge: 10 + 5.
        CHECK_NEAR(freqCellLeftX(0.0f, 0, 1.0f), 15.0f, 1.0e-4f);
        // Cell 9 (the last), at scale 1: 15 + 9 * (28 + 6) = 321.
        CHECK_NEAR(freqCellLeftX(0.0f, 9, 1.0f), 321.0f, 1.0e-4f);
        // The whole row translates with the plate's own origin.
        CHECK_NEAR(freqCellLeftX(100.0f, 0, 1.0f), 115.0f, 1.0e-4f);
        // And scales with the bar - every term above times 0.8.
        CHECK_NEAR(freqCellLeftX(0.0f, 9, 0.8f), 256.8f, 1.0e-3f);
        // The last tube ends one bezel padding short of the bezel's right
        // edge, which is one plate padding short of the plate's - the row
        // and the plate agree on where the plate ends.
        CHECK_NEAR(freqCellLeftX(0.0f, 9, 1.0f) + kFreqCellW + kFreqBezelPadX + kFreqPlatePadX,
                   kFreqPlateW, 1.0e-4f);
    }

    // --- tubeRectForCell: the glass, in its column ---------------------------
    {
        // CELL 0 at scale 1: the bezel's top-left (10, 21) plus its own
        // padding (5, 4), kFreqCellW wide and kFreqTubeH tall.
        const FreqRect t0 = tubeRectForCell(0.0f, 0.0f, 0, 1.0f);
        CHECK_NEAR(t0.x0, 15.0f, 1.0e-4f);
        CHECK_NEAR(t0.y0, 25.0f, 1.0e-4f);
        CHECK_NEAR(t0.x1 - t0.x0, kFreqCellW, 1.0e-4f);
        CHECK_NEAR(t0.y1 - t0.y0, kFreqTubeH, 1.0e-4f);
        // CELL 9: the same row, the last column.
        const FreqRect t9 = tubeRectForCell(0.0f, 0.0f, 9, 1.0f);
        CHECK_NEAR(t9.x0, 321.0f, 1.0e-4f);
        CHECK_NEAR(t9.y0, t0.y0, 1.0e-4f);
        // At scale 0.8 everything scales together.
        const FreqRect s9 = tubeRectForCell(0.0f, 0.0f, 9, 0.8f);
        CHECK_NEAR(s9.x0, 256.8f, 1.0e-3f);
        CHECK_NEAR(s9.y0, 25.0f * 0.8f, 1.0e-3f);
        CHECK_NEAR(s9.x1 - s9.x0, kFreqCellW * 0.8f, 1.0e-4f);
        CHECK_NEAR(s9.y1 - s9.y0, kFreqTubeH * 0.8f, 1.0e-4f);
        // The plate's own origin carries through unchanged (396, 20 is where
        // drawToolbar actually puts it at scale 1).
        const FreqRect o0 = tubeRectForCell(396.0f, 20.0f, 0, 1.0f);
        CHECK_NEAR(o0.x0, 396.0f + 15.0f, 1.0e-4f);
        CHECK_NEAR(o0.y0, 20.0f + 25.0f, 1.0e-4f);
    }

    // --- switchRectForCell: the layout fact behind the toggle switches --------
    //
    // An earlier cut of the digit keys got exactly this wrong once (a click on
    // the visible key did nothing; a click elsewhere fired a different cell's
    // key), because the button and the ink beside it were two separate
    // calculations that had drifted apart. switchRectForCell is ONE
    // calculation, called for both the InvisibleButton and the drawing that
    // follows it, so a half's ink and its hit box cannot disagree.
    {
        // CELL 0, UPPER HALF, scale 1: kFreqTubeSwitchGap below the tube's own
        // bottom edge, the full cell width, kFreqSwitchHalfH tall.
        const FreqRect t0 = tubeRectForCell(0.0f, 0.0f, 0, 1.0f);
        const FreqRect r0u = switchRectForCell(0.0f, 0.0f, 0, true, 1.0f);
        CHECK_NEAR(r0u.x0, t0.x0, 1.0e-4f);  // the SAME column as its tube
        CHECK_NEAR(r0u.x1, t0.x1, 1.0e-4f);
        CHECK_NEAR(r0u.y0, t0.y1 + kFreqTubeSwitchGap, 1.0e-4f);
        CHECK_NEAR(r0u.y0, 69.0f, 1.0e-4f);  // 25 + 40 + 4
        CHECK_NEAR(r0u.y1, r0u.y0 + kFreqSwitchHalfH, 1.0e-4f);

        // CELL 0, LOWER HALF: same column, directly beneath - the two tile the
        // whole switch area with NO GAP and NO OVERLAP: the lower half's top
        // edge is exactly the upper half's bottom edge.
        const FreqRect r0l = switchRectForCell(0.0f, 0.0f, 0, false, 1.0f);
        CHECK_NEAR(r0l.x0, r0u.x0, 1.0e-4f);
        CHECK_NEAR(r0l.x1, r0u.x1, 1.0e-4f);
        CHECK_NEAR(r0l.y0, r0u.y1, 1.0e-4f);  // RED WHEN a gap or overlap opens up
        CHECK_NEAR(r0l.y1, r0u.y1 + kFreqSwitchHalfH, 1.0e-4f);
        // The whole switch (both halves) is exactly kFreqSwitchH tall.
        CHECK_NEAR(r0l.y1 - r0u.y0, kFreqSwitchH, 1.0e-4f);
        // ...and ends one bezel padding above the bezel's own foot, which is
        // the plate's foot less the footer and its gap and the bottom padding.
        CHECK_NEAR(r0l.y1 + kFreqBezelPadY, kFreqBezelY + kFreqBezelH, 1.0e-4f);

        // TUBE AND SWITCH SHARE ONE CENTRE LINE: the lever stands directly
        // under the digit it steps.
        CHECK_NEAR((r0u.x0 + r0u.x1) * 0.5f, (t0.x0 + t0.x1) * 0.5f, 1.0e-4f);

        // CELL 9 (the last; "the 10 kHz cell" is cell 5 between these two):
        // same Y as cell 0's halves (the whole row runs at one Y), but the
        // LAST column's X, centred on ITS tube.
        const FreqRect t9 = tubeRectForCell(0.0f, 0.0f, 9, 1.0f);
        const FreqRect r9u = switchRectForCell(0.0f, 0.0f, 9, true, 1.0f);
        const FreqRect r9l = switchRectForCell(0.0f, 0.0f, 9, false, 1.0f);
        CHECK_NEAR(r9u.x0, 321.0f, 1.0e-4f);
        CHECK_NEAR((r9u.x0 + r9u.x1) * 0.5f, (t9.x0 + t9.x1) * 0.5f, 1.0e-4f);
        CHECK_NEAR(r9u.y0, r0u.y0, 1.0e-4f);  // same upper row
        CHECK_NEAR(r9l.y0, r0l.y0, 1.0e-4f);  // same lower row
        CHECK(r9u.x0 > r0u.x0);               // a different column from cell 0
        CHECK_NEAR(r9l.y0, r9u.y1, 1.0e-4f);  // tiles here too - no gap, no overlap

        // AT SCALE 0.8: every one of the above scales together - width,
        // height and both halves' Y all move by the same factor, so a shrunk
        // bar cannot separate a switch's ink from its own hit box either, and
        // the halves still tile with no gap.
        const FreqRect s0u = switchRectForCell(0.0f, 0.0f, 0, true, 0.8f);
        const FreqRect s0l = switchRectForCell(0.0f, 0.0f, 0, false, 0.8f);
        CHECK_NEAR(s0u.x1 - s0u.x0, kFreqCellW * 0.8f, 1.0e-4f);
        CHECK_NEAR(s0u.y1 - s0u.y0, kFreqSwitchHalfH * 0.8f, 1.0e-4f);
        CHECK_NEAR(s0u.y0, 69.0f * 0.8f, 1.0e-4f);
        CHECK_NEAR(s0l.y0, s0u.y1, 1.0e-4f);  // still tiles at scale 0.8
        CHECK_NEAR(s0l.y1 - s0u.y0, kFreqSwitchH * 0.8f, 1.0e-4f);

        // THE PLATE'S OWN ORIGIN CARRIES THROUGH UNCHANGED - a switch on a
        // plate drawn away from (0,0) sits at the plate's origin plus the same
        // offsets measured above, never at those offsets alone.
        const FreqRect o0u = switchRectForCell(396.0f, 20.0f, 0, true, 1.0f);
        CHECK_NEAR(o0u.x0, 396.0f + 15.0f, 1.0e-4f);
        CHECK_NEAR(o0u.y0, 20.0f + 69.0f, 1.0e-4f);
    }

    // --- the meters are on the deck at first launch ---------------------------
    // The owner's complaint on 0.89.0: "on first launch you don't see the
    // sample rate or the frame time meters". The bar a fresh install opens
    // with is kFirstLaunchBarW (1282 client, 25 of cabinet a side), and the
    // old rule wanted kCoreW + 2 meters + 110 of slack - 1320 with the knob's
    // 958-unit cluster - so the meters were dropped until the window was
    // widened. RED WHEN the slack comes back: put the rule's 110 back and
    // the first assertion fails.
    {
        std::printf("  the two meters fit the bar a fresh install opens with\n");
        CHECK_NEAR(kDeckCoreW, 888.0f, 1.0e-4f);
        CHECK(kDeckMinWindowW == 624);
        CHECK_NEAR(kFirstLaunchBarW, 1232.0f, 1.0e-4f);
        CHECK(metersFitOnBar(kFirstLaunchBarW, kDeckCoreW));

        // ...and not at the narrowest window run() allows, where the bar is at
        // most the client less the cabinet's minimum inset: the meters would
        // have to stand on the volume dial.
        const float minBarW = static_cast<float>(kDeckMinWindowW) - 2.0f * 25.0f;
        CHECK(!metersFitOnBar(minBarW, kDeckCoreW));
        CHECK(!metersFitOnBar(static_cast<float>(kDeckMinWindowW), kDeckCoreW));

        // THE EXACT THRESHOLD: the narrowest bar that shows them is the one
        // where the first meter's left edge clears the cluster by exactly
        // kMeterCoreClearance; one unit narrower and they go.
        const float threshold =
            kDeckCoreW + kMeterCoreClearance + 2.0f * kMeterW + kMeterGap + kMeterRightMargin;
        CHECK_NEAR(threshold, 1202.0f, 1.0e-4f);
        CHECK(metersFitOnBar(threshold, kDeckCoreW));
        CHECK(!metersFitOnBar(threshold - 1.0f, kDeckCoreW));
        CHECK(threshold <= kFirstLaunchBarW);

        // NO OVERLAP WITH THE VOLUME DIAL at that narrowest showing width: the
        // first meter starts at or past the cluster's end plus its clearance,
        // and the second meter keeps the right margin behind it.
        CHECK(meter1XOnBar(threshold) >= kDeckCoreW + kMeterCoreClearance);
        CHECK_NEAR(meter1XOnBar(threshold), kDeckCoreW + kMeterCoreClearance, 1.0e-4f);
        CHECK_NEAR(meter2XOnBar(threshold) - meter1XOnBar(threshold), kMeterW + kMeterGap,
                   1.0e-4f);
        CHECK_NEAR(threshold - (meter2XOnBar(threshold) + kMeterW), kMeterRightMargin, 1.0e-4f);
        // At the first-launch width the meters sit further right, never closer.
        CHECK(meter1XOnBar(kFirstLaunchBarW) >= kDeckCoreW + kMeterCoreClearance);
    }

    // --- the mute banner: the middle when there is room, under the counter otherwise
    // Same terms as the meters rule, so the banner cannot be told the middle
    // is free while the meters are standing in it.
    {
        std::printf("  the mute banner takes the middle only when 220 units of it are free\n");
        CHECK_NEAR(kMuteBannerMinW, 220.0f, 1.0e-4f);
        // At first launch the meters are on the bar and the middle between
        // the cluster and the first meter is only a few units wide: the
        // banner falls back under the counter rather than onto a meter.
        CHECK(muteBannerMiddleW(kFirstLaunchBarW, kDeckCoreW) < kMuteBannerMinW);
        CHECK(!muteBannerTakesTheMiddle(kFirstLaunchBarW, kDeckCoreW));
        // Just under the meters threshold there are no meters and the middle
        // runs to the bar's edge: 1201 - 12 - 888 - 12 = 289, enough.
        CHECK(muteBannerTakesTheMiddle(1201.0f, kDeckCoreW));
        CHECK_NEAR(muteBannerMiddleW(1201.0f, kDeckCoreW), 289.0f, 1.0e-4f);
        // A wide bar with the meters on it: the strip runs from the cluster
        // plus 12 (900) to the first meter less one meter gap (barW - 318),
        // and takes the middle again once that reaches 220 - at 1438 exactly.
        CHECK(!muteBannerTakesTheMiddle(1437.0f, kDeckCoreW));
        CHECK(muteBannerTakesTheMiddle(1438.0f, kDeckCoreW));
        CHECK_NEAR(muteBannerMiddleW(1438.0f, kDeckCoreW), 220.0f, 1.0e-4f);
        CHECK(metersFitOnBar(1438.0f, kDeckCoreW));
        // The narrowest window: no meters, no room - under the counter.
        CHECK(!muteBannerTakesTheMiddle(static_cast<float>(kDeckMinWindowW) - 50.0f,
                                        kDeckCoreW));
    }

    return testSummary("test_tune_control");
}
