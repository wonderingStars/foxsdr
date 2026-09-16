// egl.hpp - the phone's drawing surface: an EGL display, an OpenGL ES 3
// context and the window surface the compositor shows.
//
// WHY THIS IS ITS OWN FILE. On the desktop there is exactly one window and it
// lives as long as the process, so AppWindow can create its GL context in a
// constructor and forget about it. On Android the surface is NOT owned by the
// application: the framework takes it away whenever the activity stops (screen
// off, task switch, rotation) and hands back a different one on the way in.
// APP_CMD_INIT_WINDOW and APP_CMD_TERM_WINDOW can therefore arrive many times
// in one run, and every GL object in the process dies with the context each
// time. Keeping that lifecycle in one place is what stops it being sprinkled
// through the frame loop.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_PLATFORM_ANDROID_EGL_HPP
#define CASCADE_PLATFORM_ANDROID_EGL_HPP

#include <EGL/egl.h>
#include <android/native_window.h>

struct android_app;

namespace cascade::platform::android {

class EglContext {
public:
    EglContext() = default;
    ~EglContext();

    EglContext(const EglContext&) = delete;
    EglContext& operator=(const EglContext&) = delete;

    // Brings up display + config + context + surface for `window` and makes
    // them current. Idempotent: a second call while already up is a no-op, so
    // a duplicated APP_CMD_INIT_WINDOW cannot leak a context.
    //
    // Returns false and leaves nothing current if any stage failed; every
    // failure is logged to logcat under the "FoxSDR" tag, because on a phone
    // there is no console to print to and a silent black screen is
    // indistinguishable from a crash.
    bool init(ANativeWindow* window);

    // Releases surface, context and display, in that order. Safe to call when
    // nothing is up - which is exactly what APP_CMD_TERM_WINDOW after a failed
    // init does.
    void shutdown();

    bool valid() const { return surface_ != EGL_NO_SURFACE; }

    // Re-queries the surface dimensions. The compositor resizes under us on
    // rotation and when the system bars come and go, and EGL_WIDTH/EGL_HEIGHT
    // are the authority - ANativeWindow_getWidth can lag a frame behind.
    void refreshSize();
    int width() const { return width_; }
    int height() const { return height_; }

    // Presents the frame. Swap interval 1 is set at init, so this is what
    // paces the loop to the display's refresh rate rather than a sleep.
    void swap();

    // GL_VERSION and GL_RENDERER as reported by the live context, cached at
    // init. Never null: they read "(unknown)" if the driver refused.
    const char* glVersion() const { return glVersion_; }
    const char* glRenderer() const { return glRenderer_; }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    ANativeWindow* window_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    const char* glVersion_ = "(unknown)";
    const char* glRenderer_ = "(unknown)";
};

// The screen's density in dots per inch, from the activity's configuration.
// Returns 160 (the Android baseline, "mdpi") when the framework answers
// DEFAULT/ANY/NONE, which it does on some emulators and on any configuration
// read before the activity is fully attached.
int displayDensityDpi(android_app* app);

// The factor every logical pixel in the desktop's layout must be multiplied by
// on this screen: dpi / 160.
//
// THIS IS NOT COSMETIC AND IT IS NOT OPTIONAL. The desktop interface is
// dimensioned in raw pixels - fonts at 17/15/16/14 px, a function rail sized
// against them - and those figures were measured on a ~96 dpi monitor at
// arm's length. A modern phone is 400-500 dpi held at 300 mm, so an unscaled
// rail key comes out about 4 mm across: smaller than the 9 mm Android's own
// guidance puts on a touch target, and unusable rather than merely small.
//
// Clamped to [1, 4]: below 1 would shrink an already small interface, and
// above 4 is past any shipping screen and would mean the configuration is
// being misread.
float displayDensityScale(android_app* app);

}  // namespace cascade::platform::android

#endif  // CASCADE_PLATFORM_ANDROID_EGL_HPP
