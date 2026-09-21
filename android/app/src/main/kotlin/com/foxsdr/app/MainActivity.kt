// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
package com.foxsdr.app

import android.app.NativeActivity
import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController

/**
 * The activity FoxSDR runs in.
 *
 * ALMOST EVERYTHING THE USER SEES IS DRAWN BY libfoxsdr.so through Dear ImGui
 * on an EGL surface. This class exists for the parts of Android that have no
 * native API - and, as of the slice that put the real desktop interface on
 * screen, for one of them: getting the system bars out of the way.
 *
 * WHY IMMERSIVE MODE IS NOT A PREFERENCE. The interface is a bench cabinet:
 * brass sides, four screws, a rail down one edge and panels sunk into the
 * metal, drawn to the very edge of the surface it is given. The theme already
 * asks for a fullscreen window (res/values/themes.xml), and on a phone that is
 * enough. On an Android 13+ TABLET it is not: the taskbar is a separate,
 * always-on strip that sits OVER the bottom of the window, and on the Pixel
 * Tablet it covered the bottom of the cabinet - the status plate and the foot
 * of the function rail - while the application correctly believed it had the
 * whole 2560 x 1600 surface, because it does. Hiding the bars is the only
 * thing that gives the picture back, and it has to be re-applied on every
 * focus gain because any system gesture brings them back transiently.
 *
 * USB IS THE OTHER THING THIS CLASS EXISTS FOR, and almost none of it is in
 * this file. `UsbManager`, its permission dialog and the file descriptor it
 * hands out are Java-only APIs with no NDK equivalent, so the whole sequence
 * lives in `Usb.java` beside this file and reaches native code through the C
 * ABI in `src/usb/usb_android_bridge.h`. What this activity owes it is two
 * lifecycle calls and nothing more - the scan is NOT started from `onCreate`,
 * but from native code (`androidUsbInit`) once the native methods are bound,
 * because until that binding has happened every call Java makes into native
 * would throw `UnsatisfiedLinkError`. See `Usb.onNativeReady`.
 *
 * WHAT IS STILL NOT IMPLEMENTED, and is not pretended to be:
 *
 *  - The soft keyboard. `AInputEvent` carries no Unicode character and there
 *    is no native way to raise the on-screen keyboard, so any text entry in
 *    the interface (a frequency typed in, a plugin search) needs a
 *    `showSoftInput`/`pollUnicodeChar` pair here, called from native code the
 *    way Dear ImGui's own Android example does.
 *
 *  - The clipboard, and opening a link or a folder: both are Java objects and
 *    intents, and both are refused honestly in native code today. See
 *    gui/android_window_logic.hpp and gui/shell_open.hpp.
 */
class MainActivity : NativeActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        goImmersive()
    }

    // Applied again on every focus gain: a swipe from an edge, the
    // notification shade, or returning from another app all put the bars
    // back. A single call in onCreate would work exactly once.
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) { goImmersive() }
    }

    // THE CASE ANDROID'S OWN BROADCASTS DO NOT COVER. A dongle plugged in
    // while FoxSDR was in the background, or permission granted from the
    // system's own dialog as part of a launch, both leave a radio attached
    // that no attach broadcast will ever arrive for. Usb.onResume rescans;
    // it is idempotent (a device already open is skipped, one already asked
    // about is not asked twice) and does nothing at all before native code
    // has armed it.
    override fun onResume() {
        super.onResume()
        Usb.onResume(this)
    }

    // THE ONE PERMISSION PROMPT THIS APPLICATION RAISES, and the only place
    // its answer can arrive. Android delivers the result to the activity, so
    // Loc cannot hear it on its own; without this forwarding a user who
    // tapped "Allow" would watch the control do nothing at all, because the
    // request that was waiting for the grant would never be resumed.
    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        Loc.onPermissionResult(requestCode, permissions as Array<String>, grantResults)
    }

    // BEFORE super.onDestroy(), deliberately. NativeActivity's own onDestroy
    // is what tears the native side down and waits for its thread, so this is
    // the last moment at which the library is certainly still loaded and the
    // native methods Usb.stop calls are certainly still bound. Closing a
    // UsbDeviceConnection here does not cut a native driver off mid-transfer:
    // the transport dup()s the descriptor when it opens a radio, and a usbfs
    // interface claim lives as long as the last descriptor sharing it.
    override fun onDestroy() {
        Usb.stop(this)
        // A location request outstanding at the moment the activity goes is a
        // GNSS chip left powered on. Native code cancels its own request on
        // the way down too; this is the belt for the case where the process
        // is torn down without that path running.
        Loc.stop()
        super.onDestroy()
    }

    @Suppress("DEPRECATION")
    private fun goImmersive() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // The modern route. BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE is the
            // "sticky" half: a swipe from an edge shows the bars OVER the
            // picture for a few seconds and then takes them away again,
            // rather than resizing the window under a running GL surface.
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let { controller ->
                controller.hide(WindowInsets.Type.systemBars())
                controller.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            // minSdk is 26, so this path is real: it is what an Android 8 or 9
            // device takes. IMMERSIVE_STICKY is the same behaviour the modern
            // call describes; the LAYOUT_* flags are what stop the window
            // being resized as the bars come and go.
            window.decorView.systemUiVisibility =
                (View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY)
        }
    }
}
