/*
 * THE ANDROID PlatformWindow'S PURE HALF.
 *
 * gui/platform_window_android.cpp cannot be built on this machine - EGL,
 * ANativeWindow, android_native_app_glue and imgui_impl_android are all NDK
 * sysroot headers - so what is checked here is what a wrong answer would be
 * VISIBLE in, pulled out into gui/android_window_logic.hpp for exactly that
 * reason: the screen's density, which decides how big the whole interface is
 * drawn, and the clipboard, which is a documented stub rather than the
 * system's and therefore has a contract that can be got wrong silently.
 *
 * WHAT IS NOT CHECKED HERE, stated so an absence is not mistaken for
 * coverage: the EGL bring-up, the surface coming and going with
 * APP_CMD_INIT_WINDOW / TERM_WINDOW, the ALooper pump and the touch
 * translation are all verified on the emulator instead (a screenshot of the
 * real interface, and a tap that opens a rail drawer), because none of them
 * can be expressed without a device.
 *
 * BREAK-IT CHECKS, both run while writing this:
 *   - removing the `if (dpi <= 0)` guard from androidDensityScale makes the
 *     unattached-activity case return 0 and fails 2 checks;
 *   - making LocalClipboard::set ignore a null pointer (the obvious "be
 *     defensive" reading) fails the clear-on-null check, which is the one
 *     that matters: a paste finding the PREVIOUS text after a failed copy is
 *     worse than finding nothing.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include "gui/android_window_logic.hpp"
#include "test_check.hpp"

using cascade::gui::androidDensityScale;
using cascade::gui::LocalClipboard;

int main() {
    // -- DENSITY ------------------------------------------------------------
    // The baseline is exactly 1:1.
    CHECK_NEAR(androidDensityScale(160), 1.0f, 1e-6);
    // The emulator this port is first seen on: 420 dpi.
    CHECK_NEAR(androidDensityScale(420), 2.625f, 1e-6);
    // The common buckets, so a bucket that stops being a multiple of 160 is
    // still handled by arithmetic rather than by a table.
    CHECK_NEAR(androidDensityScale(320), 2.0f, 1e-6);   // xhdpi
    CHECK_NEAR(androidDensityScale(480), 3.0f, 1e-6);   // xxhdpi
    CHECK_NEAR(androidDensityScale(560), 3.5f, 1e-6);
    // Below the baseline (ldpi/mdpi tablets) is not shrunk: the interface is
    // already small at 1:1.
    CHECK_NEAR(androidDensityScale(120), 1.0f, 1e-6);
    // An activity whose configuration is not attached yet, and the
    // DEFAULT/ANY/NONE answers, all arrive here as a non-positive number.
    CHECK_NEAR(androidDensityScale(0), 1.0f, 1e-6);
    CHECK_NEAR(androidDensityScale(-1), 1.0f, 1e-6);
    // Past any shipping screen: clamped, so a misread configuration cannot
    // magnify the interface off the display.
    CHECK_NEAR(androidDensityScale(1600), 4.0f, 1e-6);

    // -- CLIPBOARD ----------------------------------------------------------
    LocalClipboard clip;
    // Empty before anything has been copied - the same answer a system
    // clipboard holding nothing gives, which is what PlatformWindow promises.
    CHECK(clip.get().empty());
    clip.set("14.230 MHz");
    CHECK(clip.get() == "14.230 MHz");
    // Reads are not destructive: a second paste gets the same text.
    CHECK(clip.get() == "14.230 MHz");
    clip.set("https://foxsdr.com/plugins/index.json");
    CHECK(clip.get() == "https://foxsdr.com/plugins/index.json");
    // Null CLEARS. A caller that failed to build its string must not leave
    // the last copy behind for a paste to find.
    clip.set(nullptr);
    CHECK(clip.get().empty());
    // An empty string is a legal thing to copy and is not a failure.
    clip.set("");
    CHECK(clip.get().empty());

    return testSummary("test_android_window");
}
