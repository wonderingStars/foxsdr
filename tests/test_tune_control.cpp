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
using cascade::gui::kTuneMismatchToleranceHz;
using cascade::gui::presetVfoOffsetHz;
using cascade::gui::tuneMismatchMessage;

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

    return testSummary("test_tune_control");
}
