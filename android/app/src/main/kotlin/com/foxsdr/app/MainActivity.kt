// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
package com.foxsdr.app

import android.app.NativeActivity

/**
 * The activity FoxSDR runs in.
 *
 * IT IS EMPTY, AND THAT IS THE POINT. Everything the user sees is drawn by
 * libfoxsdr.so through Dear ImGui on an EGL surface; this class exists so that
 * the parts of Android that have no native API have somewhere to live when
 * they are needed.
 *
 * `android:hasCode="false"` with a bare `android.app.NativeActivity` would
 * work today and would have to be undone almost immediately:
 *
 *  - USB. `UsbManager.requestPermission` puts a dialog in front of the user
 *    before an application may open a device. There is no NDK equivalent; the
 *    file descriptor has to be obtained on the Java side and handed down. That
 *    is the next slice, and it needs a class here to be a JNI counterpart to.
 *
 *  - The soft keyboard. `AInputEvent` carries no Unicode character and there
 *    is no native way to raise the on-screen keyboard, so any text entry in
 *    the interface (a frequency typed in, a plugin search) needs a
 *    `showSoftInput`/`pollUnicodeChar` pair here, called from native code the
 *    way Dear ImGui's own Android example does.
 *
 * Neither is implemented yet, and neither is pretended to be: this slice's
 * first screen has no text entry and no radio.
 */
class MainActivity : NativeActivity()
