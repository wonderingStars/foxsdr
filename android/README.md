# FoxSDR on Android — the application shell

This directory is the Gradle project that produces an installable APK. The
native code it builds lives in `../src/platform/android/`, and the C++ it links
is the same tree the desktop build compiles.

**What this slice is.** A running app: a NativeActivity that brings up EGL and
OpenGL ES 3, runs Dear ImGui on it, maps touch onto the two-button mouse the
desktop interface expects, and draws a first screen from the portable core —
the built-in signal generator through the real spectrum estimator. It is the
floor the next slices build on, not the product.

**What this slice is not.** There is no radio, no audio, no plugin host, no
`AppWindow`. The whole interface is one window with a trace, four tone keys and
a status plate.

---

## Building

Everything below runs in WSL (Ubuntu). The toolchain is expected at:

| | |
|---|---|
| `ANDROID_HOME` | `/root/android-sdk` (platforms;android-34, build-tools;34.0.0, cmake;3.22.1, platform-tools) |
| `ANDROID_NDK_HOME` | `/root/android-sdk/ndk/27.2.12479018` (clang 18) |
| `JAVA_HOME` | JDK 17 |

`~/android-env.sh` exports all three. Gradle itself is **not** installed and
does not need to be: the wrapper downloads the pinned distribution.

```sh
. ~/android-env.sh
cd ~/fox-and-shell/android
./gradlew assembleDebug
```

The APK lands at `app/build/outputs/apk/debug/app-debug.apk`.

Static analysis (0 errors; the remaining warnings are deliberate, see
"Decisions" below):

```sh
./gradlew lint          # report: app/build/reports/lint-results-debug.{html,txt,xml}
```

A release APK is `./gradlew assembleRelease`. It is **unsigned** — there is no
signing config in this project yet, because the keystore is a release-process
decision and not a shell-slice one.

### The decoder plugins, which are inside the APK

**Google Play forbids an application downloading executable code**, so the
catalogue the desktop build fetches its decoder modules from cannot exist here.
The modules an Android user can run are the ones compiled into the signed APK,
and nothing is fetched at runtime — which is what the plugin store page says on
this platform, rather than offering a key that contacts nothing.

They are **not committed**: `app/src/main/jniLibs/` is gitignored, because a
built binary belonging to a separate repository does not belong in this one's
history. Build and stage them with the plugin tree beside you:

```sh
tools/build-android-plugins.sh --plugins ../foxsdr-plugins-dev
```

…or let Gradle run the same script:

```sh
./gradlew assembleDebug -PfoxsdrPluginsDir=../foxsdr-plugins-dev
```

Without that property the APK packages whatever is already staged, and the
configure prints how many module files that was — so "I forgot to build the
plugins" and "this APK has no decoders in it" never look the same in the log.

**WHICH modules is decided by the plugin repository, not here.** Its
`CMakeLists.txt` carries the list (`FOXSDR_ANDROID_BUNDLED`) and the reason each
excluded module is excluded: TLS the NDK cannot provide, a legal notice with no
install step to show it at, a patent-encumbered codec, or a module that is not
published yet. Today that is 14 of the tree's 27 — about 1.4 MB of module
across both ABIs.

**The APK extracts its native libraries** (`useLegacyPackaging true`, so
`android:extractNativeLibs="true"`), and that changed with this slice. The
plugin host finds modules by SCANNING a directory — the package's own
`ApplicationInfo.nativeLibraryDir`, read over JNI in
`core/android_app_info.cpp` — and under the modern packaging that directory is
created EMPTY (measured on the emulator: `ls -l …/lib/x86_64` → `total 0`), so
the product would have shipped with no decoder in it and nothing on screen to
explain why. The long comment on `packaging { jniLibs { … } }` in
`app/build.gradle` carries the measured APK and installed sizes both ways, and
the alternative to revisit before an AAB upload.

What it looks like when it worked, from a device:

```sh
adb logcat -s FoxSDR | grep -E 'plugins:|plugin: '
I FoxSDR  : plugins: bundled in this package, at /data/app/~~…/lib/x86_64
I FoxSDR  : plugin: loaded ADS-B 1.6.0
…
```

(Every `diagLogf` line reaches logcat on Android, under the `FoxSDR` tag. On a
phone the diagnostics ring is in memory and its file lives inside the sandbox,
so logcat is the only account anyone gets.)

### Regenerating the launcher icon

The mipmap PNGs are committed, so a build never needs an image tool. Re-run this
only when `resources/icon/foxsdr-256.png` changes:

```sh
bash android/tools/make-icons.sh      # needs ImageMagick (magick or convert)
```

On Windows the equivalent source of truth is `resources/icon/generate_icon.py`,
which uses Pillow under `py -3.14`. Note that `C:\Windows\system32\convert.exe`
is the FAT-to-NTFS utility and **not** ImageMagick.

---

## Installing it on a phone

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.foxsdr/com.foxsdr.app.MainActivity
```

It also appears in the launcher as **FoxSDR**. Android will warn that the app
came from an unknown source; the debug APK is signed with the local debug key,
which is expected.

### What you should see

The phone turns to **landscape** (the manifest forces it) and shows one dark
panel:

- a caption reading `SIGNAL GENERATOR 2.048 MS/s 1024-POINT BLACKMAN-HARRIS`;
- a black-green well holding a live spectrum trace in phosphor green: a noise
  floor around −100 dB with **four tone peaks** at −620, −150, +310 and
  +745 kHz, the tallest near −10 dB. The trace moves continuously — the noise
  floor is being re-estimated about 2000 times a second;
- a dB ladder down the left of the well (0 to −120 in 20 dB steps) and vertical
  rules every 500 kHz;
- **four brass keys** reading `ON -620 kHz` … `ON +745 kHz`. Tapping one mutes
  that tone: the key goes grey and reads `OFF`, and the corresponding peak
  disappears from the trace. Tapping again brings it back;
- a plate underneath: a running block count, the frame time, the surface size,
  the screen density, the ABI, the GL version and renderer strings, and the
  FoxSDR version.

The lettering is Saira Condensed and Nova Mono — the product's own faces,
embedded in the binary — over the bench palette from `src/gui/theme.hpp`. If it
comes up in ImGui's default blue with a terminal font, `fonts::load()` failed
and logcat will say so.

### If nothing appears

```sh
adb logcat -d | grep -iE "FoxSDR|AndroidRuntime|DEBUG|libEGL"
```

The shell logs two lines under the tag `FoxSDR` when it comes up:

```
I FoxSDR  : EGL up: 2264x1080, OpenGL ES 3.0 …, <renderer>
I FoxSDR  : shell up: x86_64, 420 dpi (x2.62), FoxSDR 0.97.0
```

No `EGL up` line means the ES3 context was refused; the message above it says
which EGL call failed. No `android_main` line at all means the activity could
not load `libfoxsdr.so` — check that `android.app.lib_name` in the manifest
still matches the CMake target name.

---

## Touch, and what it maps to

The desktop interface assumes a mouse with two buttons and a wheel. A
touchscreen has none of those, so `src/platform/android/input.cpp` supplies
them:

| gesture | what ImGui sees |
|---|---|
| tap, drag | left button, as the stock ImGui Android backend does it |
| **long press** (one finger, held 450 ms within 8 dp) | left button released, **right button** pressed; released when the finger lifts |
| **two-finger drag** | **wheel**, both axes, content following the fingers |
| physical mouse / stylus over USB or Bluetooth | passed straight through untouched |

Both gestures are the platform conventions rather than inventions. A tap is
*not* delayed to wait out the long-press window — that would put 450 ms of lag
on every touch in the application — so a long press does send a short left
click before the right button arrives, exactly as holding the left mouse button
before deciding to use the right one does on the desktop.

---

## Decisions in this slice

**EGL config.** `EGL_OPENGL_ES3_BIT` renderable type (without it
`eglChooseConfig` defaults to ES1 and the ES3 context can legally be refused),
8/8/8 with no alpha, **no depth and no stencil** — Dear ImGui draws 2D
triangles in painter's order and a full-screen depth buffer would cost about
7 MB of bandwidth a frame for nothing. `eglSwapInterval(1)`, so the loop is
paced by the panel rather than by a sleep.

**Density scaling.** `AConfiguration_getDensity` / 160, clamped to [1, 4], is
applied once through `ImGuiStyle::ScaleAllSizes` and `FontScaleDpi`. Call sites
push the desktop's own 17/15/16/14 px font sizes unscaled and get the right
physical size for free. This is not cosmetic: the desktop layout was measured
at ~96 dpi, and unscaled on a 420 dpi phone a rail key is about 4 mm across —
under half Android's own touch-target guidance.

**Idle behaviour.** The loop blocks in `ALooper_pollOnce(-1)` whenever it has no
window or no focus, and the DSP worker is stopped in both cases. `APP_CMD_PAUSE`
and `APP_CMD_STOP` are treated like `APP_CMD_LOST_FOCUS`, because a phone that
sleeps straight from the foreground can deliver those without a focus change.

**No `INTERNET` permission.** Crash upload, the usage report and the plugin
catalogue are later slices, and none of them may ship before `PRIVACY.md`
describes what an Android build sends. With the permission absent, nothing can
leak by accident. See the comment at the top of `AndroidManifest.xml`.

**64-bit only** (`arm64-v8a`, `x86_64`). Play has required a 64-bit build since
2019 and accepts a 64-bit-only upload; `armeabi-v7a` would double the APK for
phones that have not shipped in years.

**`ANDROID_STL=c++_shared`.** Decoder plugins will be separate `.so` files in
this process, and two copies of a static libc++ in one process means two sets of
`type_info`, two `operator new`/`delete` pairs and exceptions that cannot cross
the boundary. One 1.3 MB library now, instead of an unfixable ABI problem later.

**Three lint warnings are left standing, deliberately:**
`NonResizeableActivity` and two `DiscouragedApi` warnings, all of them about
`screenOrientation="landscape"` and `resizeableActivity="false"`. The interface
is a landscape bench panel and every hard-coded width in it was measured that
way; a portrait or free-form window would fold the layout into a column nothing
in the code knows how to draw. Revisit when the layout is responsive, not
before.

---

## Where the CMake lives

`app/build.gradle` points `externalNativeBuild` at the **repository's root
`CMakeLists.txt`** with `-DCASCADE_ANDROID=ON`. That file takes an early return
into `src/platform/android/CMakeLists.txt` and never configures the desktop
half — which needs GLFW, PortAudio, SoapySDR, desktop OpenGL and libunwind,
none of which exist on an NDK sysroot.

Until the portable core lands as `cascade_core`, the shell links a placeholder
target, `cascade_core_min`, that compiles only what the first screen needs: all
of `src/dsp`, the signal generator and its `IqSource` adapter, and
`src/gui/theme.cpp` + `src/gui/fonts.cpp`. **Swapping in the real core is one
line** — the `target_link_libraries(foxsdr PRIVATE cascade_core_min)` at the
bottom of that file.
