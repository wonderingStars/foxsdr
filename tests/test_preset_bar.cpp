// Tests for the 0.99.0 preset bars: one key per valid preset on a plugin's
// own window (map page, image window, panel, instrument window), and the
// grouped bar in the shared Decoder output window for text decoders that
// have no window of their own.
//
// EVERYTHING HERE IS PURE - no ImGui, no plugin ABI call, no live pipeline -
// and that is not a convenience, it is the only way any of this is checkable
// at all. applyPluginPreset ends by rebuilding the very lists
// AppWindow::drawPluginWindows is iterating while it draws a bar
// (gui/tune_control.hpp's own comment on PendingPresetRequest explains the
// hazard, the same shape 0.96.1 fixed in gui/list_pick.hpp), so the state
// machine that keeps a key press from applying mid-iteration has to live
// somewhere with no frame to corrupt, and this is the header that is.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_ui.hpp"
#include "gui/tune_control.hpp"
#include "test_check.hpp"

using cascade::core::MutePreset;
using cascade::gui::cappedPresetCount;
using cascade::gui::PendingPresetRequest;
using cascade::gui::presetBarKeys;
using cascade::gui::PresetBarKey;
using cascade::gui::PresetBarRequest;
using cascade::gui::presetIsValid;
using cascade::gui::presetKeyFitsOnRow;
using cascade::gui::presetLabel;
using cascade::gui::presetRequestStillValid;

namespace {

CascadePreset presetAt(double freqHz) {
    CascadePreset ps{};
    ps.structSize = static_cast<std::uint32_t>(sizeof(CascadePreset));
    ps.frequencyHz = freqHz;
    return ps;
}

MutePreset mutePreset(double freqHz, double bwHz, bool deviceCentre, std::uint32_t index,
                      const std::string& label) {
    MutePreset mp;
    mp.frequencyHz = freqHz;
    mp.bandwidthHz = bwHz;
    mp.deviceCentre = deviceCentre;
    mp.index = index;
    mp.label = label;
    return mp;
}

// A BOUNDS-SAFE INDEX, for reading a vector back after a CHECK of its size.
// CHECK records a failure and continues rather than stopping the test (see
// test_check.hpp), so a plain v[i] right after "CHECK(v.size() == N)" is an
// out-of-bounds read the moment that CHECK is the one that fails - the exact
// trap the mayhem-b200 ADS-B decimator lesson names. Returning a default-
// constructed element instead just fails whatever CHECK reads it next,
// which is the correct outcome: the size was already wrong.
template <typename T>
T at(const std::vector<T>& v, std::size_t i) {
    return i < v.size() ? v[i] : T{};
}

}  // namespace

int main() {
    // --- presetIsValid: the shared filter, third-party garbage refused -----
    {
        CHECK(presetIsValid(presetAt(1090.0e6)));
        // ZERO: not a place. Accepting it would send the radio to 0 Hz.
        CHECK(!presetIsValid(presetAt(0.0)));
        CHECK(!presetIsValid(presetAt(-1090.0e6)));
        // AT AND JUST BELOW THE UPPER BOUND: exactly at 1e12 is refused,
        // one hertz under it is accepted.
        CHECK(!presetIsValid(presetAt(1.0e12)));
        CHECK(presetIsValid(presetAt(1.0e12 - 1.0)));
        // NaN. Written as a positive test (`> 0.0 && < 1e12`) for exactly
        // this reason: every direct comparison against NaN is false, so
        // `!(NaN > 0.0 && NaN < 1e12)` correctly refuses it, while a
        // negated-comparison rewrite (`f <= 0.0 || f >= 1e12`) would let it
        // through, because those comparisons are ALSO false for NaN. RED
        // WHEN presetIsValid is rewritten that way.
        CHECK(!presetIsValid(presetAt(std::nan(""))));
    }

    // --- cappedPresetCount: the shared cap ----------------------------------
    {
        CHECK(cappedPresetCount(0u, 16u) == 0u);
        CHECK(cappedPresetCount(5u, 16u) == 5u);
        CHECK(cappedPresetCount(16u, 16u) == 16u);
        // COUNT ABOVE THE CAP: capped, not refused outright - the first 16
        // presets are still a menu, however oddly a plugin behaves past
        // that. RED WHEN this stops capping and simply echoes rawCount.
        CHECK(cappedPresetCount(20u, 16u) == 16u);
    }

    // --- presetLabel: bounded, with the plugin-name fallback ---------------
    {
        char label[CASCADE_PRESET_LABEL_CHARS] = "WEFAX region 1";
        CHECK(presetLabel(label, "WEFAX") == "WEFAX region 1");

        // EMPTY: falls back to the plugin's own name - most presets letter
        // nothing of their own.
        char empty[CASCADE_PRESET_LABEL_CHARS] = "";
        CHECK(presetLabel(empty, "ADS-B") == "ADS-B");

        // UNTERMINATED, 100% FULL: every one of CASCADE_PRESET_LABEL_CHARS
        // bytes is a letter and none of them is NUL. strlen() would run
        // past the end of this array looking for a terminator that is not
        // there; strnlen (what presetLabel must call) stops at the bound.
        // `sentinel` sits immediately after `buf` with no padding possible
        // between two char members, so it is exactly the byte one-past-the-
        // end that an unbounded scan would read next - set to a printable,
        // non-NUL value so a scan that reaches it changes the result this
        // test checks, rather than silently reading whatever the stack
        // otherwise held.
        struct Guarded {
            char buf[CASCADE_PRESET_LABEL_CHARS];
            char sentinel;
        } guarded;
        std::memset(guarded.buf, 'A', sizeof(guarded.buf));
        guarded.sentinel = 'Z';
        const std::string got = presetLabel(guarded.buf, "fallback");
        CHECK(got.size() == static_cast<std::size_t>(CASCADE_PRESET_LABEL_CHARS));
        CHECK(got == std::string(static_cast<std::size_t>(CASCADE_PRESET_LABEL_CHARS), 'A'));
    }

    // --- presetBarKeys: empty table -> no bar -------------------------------
    {
        CHECK(presetBarKeys({}, 1090.0e6, 0.0, 2.0e6).empty());
    }

    // --- presetBarKeys: the lit decision, a DEVICE_CENTRE preset -----------
    {
        // ADS-B's OWN SHAPE: lit when the device centre sits within half the
        // sample rate of the preset - the VFO offset must NOT matter here,
        // which is the whole reason CASCADE_PRESET_DEVICE_CENTRE exists.
        const std::vector<MutePreset> band = {mutePreset(1090.0e6, 0.0, true, 7u, "ADS-B")};

        const std::vector<PresetBarKey> lit = presetBarKeys(band, 1090.5e6, 999999.0, 2.0e6);
        CHECK(lit.size() == 1u);
        CHECK(at(lit, 0).lit);
        CHECK(at(lit, 0).index == 7u);
        CHECK(at(lit, 0).label == "ADS-B");

        const std::vector<PresetBarKey> dark = presetBarKeys(band, 100.0e6, 0.0, 2.0e6);
        CHECK(dark.size() == 1u);
        CHECK(!at(dark, 0).lit);
    }

    // --- presetBarKeys: the lit decision, an audio preset -------------------
    {
        // APRS's OWN SHAPE: lit when the TUNED frequency (device centre plus
        // VFO offset) sits within max(bandwidth/2, 5 kHz) - here the offset
        // is exactly what decides it, the mirror image of the band case.
        const std::vector<MutePreset> audio = {
            mutePreset(144.8e6, 25000.0, false, 3u, "APRS")};

        // 144.7 MHz device + 100 kHz offset = 144.8 MHz tuned: exactly on it.
        const std::vector<PresetBarKey> lit = presetBarKeys(audio, 144.7e6, 100000.0, 0.0);
        CHECK(lit.size() == 1u);
        CHECK(at(lit, 0).lit);
        CHECK(at(lit, 0).index == 3u);

        // SAME DEVICE CENTRE, NO OFFSET: 144.7 MHz tuned is 100 kHz off a
        // preset whose tolerance is max(12500, 5000) = 12500 Hz - not lit.
        const std::vector<PresetBarKey> dark = presetBarKeys(audio, 144.7e6, 0.0, 0.0);
        CHECK(dark.size() == 1u);
        CHECK(!at(dark, 0).lit);
    }

    // --- presetKeyFitsOnRow: the wrap, at the exact boundary ----------------
    {
        // EXACTLY ON THE BOUNDARY FITS (<=, not <) - the same "coerced, not
        // refused" convention this header's kTuneMismatchToleranceHz uses.
        // RED WHEN the comparison becomes strict (<).
        CHECK(presetKeyFitsOnRow(100.0f, 100.0f));
        // One hundredth of a pixel over: wraps.
        CHECK(!presetKeyFitsOnRow(100.0f, 100.01f));
        CHECK(presetKeyFitsOnRow(200.0f, 100.0f));
        // NOTHING LEFT ON THE ROW: only a zero-width key still fits, and
        // that is the boundary case above, not a special one.
        CHECK(!presetKeyFitsOnRow(0.0f, 1.0f));
        CHECK(presetKeyFitsOnRow(0.0f, 0.0f));
    }

    // --- PendingPresetRequest: record now, apply after the loop -------------
    {
        PendingPresetRequest req;
        // NOTHING PRESSED YET.
        CHECK(!req.hasPending());
        CHECK(!req.take().has_value());

        req.record("adsb.dll", 2u);
        CHECK(req.hasPending());

        // TWO PRESSES IN ONE FRAME: the SECOND replaces the first entirely -
        // "the last thing you pressed" is the whole rule, never a queue of
        // two. RED WHEN record() appends instead of replacing.
        req.record("pocsag.dll", 5u);
        CHECK(req.hasPending());
        const std::optional<PresetBarRequest> got = req.take();
        CHECK(got.has_value());
        CHECK(got.value_or(PresetBarRequest{}).pluginKey == "pocsag.dll");
        CHECK(got.value_or(PresetBarRequest{}).presetIndex == 5u);

        // CONSUMED ONCE: a second take() in the same frame finds nothing,
        // exactly as if nothing had been pressed - this is what makes the
        // safe point idempotent if it were ever (mis)called twice.
        CHECK(!req.hasPending());
        CHECK(!req.take().has_value());
    }

    // --- presetRequestStillValid: the resolve, at the safe point -----------
    {
        // THE ORDINARY CASE: found, index in range, fetched, valid.
        CHECK(presetRequestStillValid(/*pluginFound=*/true, /*count=*/4u, /*maxPresets=*/16u,
                                       /*presetIndex=*/2u, /*fetchedOk=*/true,
                                       presetAt(1090.0e6)));

        // UNKNOWN / UNLOADED PLUGIN KEY: nothing in the current plugin list
        // answers to it any more - a silent no-op, whatever the index says.
        CHECK(!presetRequestStillValid(/*pluginFound=*/false, 4u, 16u, 0u, true,
                                        presetAt(1090.0e6)));

        // A STALE INDEX: the plugin is found, but the index recorded when the
        // key was pressed no longer names a preset in its CURRENT table.
        CHECK(!presetRequestStillValid(true, /*count=*/2u, 16u, /*presetIndex=*/5u, true,
                                        presetAt(1090.0e6)));

        // THE CAP IS ENFORCED HERE TOO, even when the plugin's own count()
        // would allow the index - the same cap the bar itself never drew a
        // key past.
        CHECK(!presetRequestStillValid(true, /*count=*/20u, /*maxPresets=*/4u,
                                        /*presetIndex=*/4u, true, presetAt(1090.0e6)));

        // get() ITSELF FAILED (fetchedOk=false): the plugin answered "no",
        // which must be treated exactly as a stale index is.
        CHECK(!presetRequestStillValid(true, 4u, 16u, 2u, /*fetchedOk=*/false,
                                        presetAt(1090.0e6)));

        // FETCHED, BUT NOT A VALID PRESET: the table changed to garbage
        // between the press and this safe point.
        CHECK(!presetRequestStillValid(true, 4u, 16u, 2u, true, presetAt(0.0)));
    }

    return testSummary("test_preset_bar");
}
