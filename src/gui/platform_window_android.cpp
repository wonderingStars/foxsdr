// platform_window_android.cpp - the phone's PlatformWindow. See the header for
// the three ways an Android surface is not a desktop window; this file is
// mostly the consequences of the first of them.
//
// EVERY egl* AND ALooper* CALL THE APPLICATION MAKES IS IN THIS FILE, which is
// the same property gui/platform_window_glfw.cpp has for glfw*: app_window.cpp
// contains neither, and a grep can check it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/platform_window_android.hpp"

#include <android/configuration.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <time.h>

#include <cstdlib>

#include <imgui.h>
#include <imgui_impl_android.h>

#include "core/diag_log.hpp"
#include "gui/ui_scale.hpp"

namespace cascade::gui {
namespace {

constexpr const char* kTag = "FoxSDR";

// HOW LONG THE LOOP SLEEPS WHEN THERE IS NOTHING TO DRAW, and why it is not
// infinite. Blocking for ever is what a phone should do while it is in a
// pocket, but AppWindow's run loop also checks shouldClose() and the config
// debounce once per turn, and a loop that cannot turn cannot notice that the
// activity has been asked to finish. One second costs one ImGui frame into a
// 1x1 pbuffer per second - a rounding error next to the DSP - and keeps every
// other thing the loop is responsible for alive.
constexpr int kIdlePollMs = 1000;

void logError(const char* what) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "EGL: %s (error 0x%04x)", what,
                        static_cast<unsigned>(eglGetError()));
}

double monotonicSeconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1.0e9;
}

// The android_app callbacks are C function pointers with no user data of their
// own, so they arrive here and are forwarded through android_app::userData -
// which create() sets to the window. Deliberately NOT a file-scope pointer to
// the instance: the framework can destroy and recreate an activity inside one
// process, and a global would carry the previous one's EGL handles into the
// new one.
void appCmdThunk(android_app* app, std::int32_t cmd) {
    if (app == nullptr || app->userData == nullptr) { return; }
    static_cast<AndroidPlatformWindow*>(app->userData)->onAppCmd(cmd);
}

std::int32_t inputThunk(android_app* app, AInputEvent* event) {
    if (app == nullptr || app->userData == nullptr) { return 0; }
    return static_cast<AndroidPlatformWindow*>(app->userData)->onInputEvent(event);
}

}  // namespace

AndroidPlatformWindow::~AndroidPlatformWindow() { destroy(); }

// -- Lifetime --------------------------------------------------------------

bool AndroidPlatformWindow::create(const CreateInfo& info) {
    if (app_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "create() with no android_app");
        return false;
    }
    // The size and the minimum in CreateInfo are both ignored, and that is not
    // an oversight: there is exactly one window, the compositor owns its size,
    // and nothing can be dragged smaller. The minimum is honoured in the one
    // way a phone CAN honour it - gui/ui_scale.hpp scales the layout until it
    // fits the screen it was given, rather than refusing a screen.
    title_ = info.title;

    timeBase_ = monotonicSeconds();

    app_->userData = this;
    app_->onAppCmd = &appCmdThunk;
    app_->onInputEvent = &inputThunk;

    // -- The display and the context, created ONCE for the session ----------
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        logError("eglGetDisplay returned EGL_NO_DISPLAY");
        return false;
    }
    if (eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
        logError("eglInitialize failed");
        display_ = EGL_NO_DISPLAY;
        return false;
    }

    // THE CONFIG, and every entry in it is a decision.
    //
    // EGL_RENDERABLE_TYPE = EGL_OPENGL_ES3_BIT is the one that matters:
    // without it eglChooseConfig defaults to ES1 and the ES3 context below can
    // legally be refused. It is also what makes the "#version 300 es" shaders
    // imgui_impl_opengl3 emits in this build a valid thing to ask for.
    //
    // EGL_PBUFFER_BIT as well as EGL_WINDOW_BIT, because this config has to
    // serve both: the window surface the compositor shows, and the 1x1 pbuffer
    // the context is parked on while the framework has taken the window away
    // (see the header). One config for both is what lets the SAME context stay
    // current across the swap, which is the whole point - every texture the
    // waterfall and the renderer backend own lives in it.
    //
    // 8/8/8 with no alpha: the surface is opaque and full-screen. NO DEPTH AND
    // NO STENCIL - Dear ImGui draws 2D triangles in painter's order and
    // disables depth testing itself, and a 24-bit depth buffer on a 1080p
    // panel is about 7 MB of bandwidth a frame for nothing.
    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                    EGL_RED_SIZE,        8,
                                    EGL_GREEN_SIZE,      8,
                                    EGL_BLUE_SIZE,       8,
                                    EGL_DEPTH_SIZE,      0,
                                    EGL_STENCIL_SIZE,    0,
                                    EGL_NONE};
    EGLint numConfigs = 0;
    if (eglChooseConfig(display_, configAttribs, &config_, 1, &numConfigs) != EGL_TRUE ||
        numConfigs == 0) {
        logError("eglChooseConfig found no ES3 window+pbuffer config");
        destroy();
        return false;
    }

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        logError("eglCreateContext failed for ES3");
        destroy();
        return false;
    }

    const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    pbuffer_ = eglCreatePbufferSurface(display_, config_, pbufferAttribs);
    if (pbuffer_ == EGL_NO_SURFACE) {
        // Survivable, and worth saying rather than failing over: without it a
        // frame drawn while the window is gone is drawn with no current
        // context, which Android answers with a one-line warning and a no-op
        // rather than a fault. The interface still runs; only the background
        // frames are wasted differently.
        logError("eglCreatePbufferSurface failed - frames drawn while backgrounded "
                 "will have no context");
    }

    // -- The window, waited for -------------------------------------------
    //
    // android_main is called as soon as the native thread starts, which is
    // BEFORE the activity has a surface. Everything AppWindow::run does after
    // create() - the font atlas, the ImGui backends, the first frame - needs
    // one, so this is where the wait belongs rather than in a special case
    // further up.
    while (!hasSurface_ && !closeRequested_) {
        int events = 0;
        android_poll_source* source = nullptr;
        const int id = ALooper_pollOnce(kIdlePollMs, nullptr, &events,
                                        reinterpret_cast<void**>(&source));
        if (id >= 0 && source != nullptr) { source->process(app_, source); }
        if (app_->destroyRequested != 0) { closeRequested_ = true; }
    }
    if (!hasSurface_) {
        // The activity was finished before it was ever shown. Not an error to
        // report as one: run() returns 1, android_main returns, and the glue
        // tears the activity down, which is what was asked for.
        __android_log_print(ANDROID_LOG_INFO, kTag,
                            "activity finished before it had a surface");
        destroy();
        return false;
    }

    densityDpi_ = (app_->config != nullptr)
                      ? static_cast<int>(AConfiguration_getDensity(app_->config))
                      : kAndroidBaselineDpi;
    // AConfiguration_getDensity answers DEFAULT/ANY/NONE on some emulators and
    // on any configuration read before the activity is fully attached; all of
    // them are large sentinel values or zero, and androidDensityScale's clamp
    // is what turns them into the baseline. The sentinels are mapped to 0 here
    // so the LOGGED dpi is not a nonsense number.
    if (densityDpi_ == ACONFIGURATION_DENSITY_DEFAULT ||
        densityDpi_ == ACONFIGURATION_DENSITY_ANY ||
        densityDpi_ == ACONFIGURATION_DENSITY_NONE) {
        densityDpi_ = 0;
    }
    densityScale_ = androidDensityScale(densityDpi_);
    if (densityDpi_ <= 0) { densityDpi_ = kAndroidBaselineDpi; }

    uiScale_ = fittedUiScale(width_, height_, densityScale_, std::getenv("FOXSDR_UI_SCALE"));

    input_.configure(uiScale_);

    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "window up: %dx%d, %d dpi (density x%.2f), ui scale x%.3f",
                        width_, height_, densityDpi_,
                        static_cast<double>(densityScale_), static_cast<double>(uiScale_));
    cascade::core::diagLogf("android: surface %dx%d, %d dpi, density x%.2f, ui scale x%.3f",
                            width_, height_, densityDpi_,
                            static_cast<double>(densityScale_),
                            static_cast<double>(uiScale_));
    return true;
}

// Nothing to show: the compositor put the surface on screen the moment the
// framework handed it over. Kept so the ordering in run() - backends up, frame
// installed, THEN visible - reads the same on both platforms.
void AndroidPlatformWindow::show() {}

void AndroidPlatformWindow::destroy() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) { eglDestroySurface(display_, surface_); }
        if (pbuffer_ != EGL_NO_SURFACE) { eglDestroySurface(display_, pbuffer_); }
        if (context_ != EGL_NO_CONTEXT) { eglDestroyContext(display_, context_); }
        eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
    surface_ = EGL_NO_SURFACE;
    pbuffer_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
    config_ = nullptr;
    hasSurface_ = false;
    timeBase_ = 0.0;

    // The callbacks come off LAST, and they must: a command delivered between
    // the EGL teardown and this line would find a window with no context,
    // which onAppCmd copes with, whereas one delivered after app_->userData is
    // cleared is simply dropped by the thunks.
    if (app_ != nullptr && app_->userData == this) {
        app_->userData = nullptr;
        app_->onAppCmd = nullptr;
        app_->onInputEvent = nullptr;
    }
}

// -- The surface, which comes and goes -------------------------------------

bool AndroidPlatformWindow::attachSurface(ANativeWindow* window) {
    if (window == nullptr || display_ == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT) {
        return false;
    }
    if (hasSurface_) { return true; }  // a duplicated INIT_WINDOW must not leak

    // Hand the window the pixel format EGL picked. Skipping this is the classic
    // cause of a correct-looking render that the compositor shows as garbage,
    // because the buffer's format and the config's disagree.
    EGLint nativeVisual = 0;
    eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &nativeVisual);
    ANativeWindow_setBuffersGeometry(window, 0, 0, nativeVisual);

    surface_ = eglCreateWindowSurface(display_, config_, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        logError("eglCreateWindowSurface failed");
        return false;
    }
    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
        logError("eglMakeCurrent failed for the window surface");
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
        return false;
    }
    // Pace to the panel. A phone that renders faster than it can present is
    // burning battery to produce frames the compositor throws away.
    eglSwapInterval(display_, 1);
    hasSurface_ = true;
    refreshSurfaceSize();

    // THE ImGui BACKEND HOLDS THE ANativeWindow AND READS ITS SIZE EVERY
    // FRAME (imgui_impl_android.cpp, ImGui_ImplAndroid_NewFrame). A second
    // INIT_WINDOW hands us a DIFFERENT window, so the backend has to be
    // re-pointed or it reads the size of a window the framework has already
    // destroyed. Init is two assignments and a name - there is nothing to
    // leak by calling it again - so re-pointing is exactly this.
    if (imguiBackendUp_) { ImGui_ImplAndroid_Init(window); }
    return true;
}

void AndroidPlatformWindow::detachSurface() {
    if (display_ == EGL_NO_DISPLAY) { return; }
    // PARKED ON THE PBUFFER RATHER THAN RELEASED. eglMakeCurrent with
    // EGL_NO_CONTEXT would leave the GUI thread with no current context at
    // all, and the frame the run loop is about to draw anyway would then make
    // GL calls Android answers with a warning and a no-op. A 1x1 pbuffer
    // keeps every call legal and every object the renderer owns alive, for one
    // pixel.
    if (pbuffer_ != EGL_NO_SURFACE) {
        eglMakeCurrent(display_, pbuffer_, pbuffer_, context_);
    } else {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    hasSurface_ = false;
    // width_/height_ are deliberately LEFT ALONE: they are the last size the
    // interface was laid out at, and the frames drawn while the window is gone
    // must be laid out at the same size or every panel's remembered geometry
    // is recomputed against a zero viewport.
}

void AndroidPlatformWindow::refreshSurfaceSize() {
    if (!hasSurface_ || surface_ == EGL_NO_SURFACE) { return; }
    // EGL_WIDTH/EGL_HEIGHT are the authority: ANativeWindow_getWidth can lag a
    // frame behind the compositor when the system bars come and go.
    EGLint w = 0;
    EGLint h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    if (w > 0 && h > 0) {
        width_ = static_cast<int>(w);
        height_ = static_cast<int>(h);
    }
}

// -- The framework's commands ----------------------------------------------

void AndroidPlatformWindow::onAppCmd(std::int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        attachSurface(app_ != nullptr ? app_->window : nullptr);
        break;

    case APP_CMD_TERM_WINDOW:
        // The surface is being taken away, and everything that touches it has
        // to be done before this returns - the glue destroys the window as
        // soon as it does.
        detachSurface();
        break;

    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
        refreshSurfaceSize();
        break;

    case APP_CMD_CONFIG_CHANGED:
        // A fold, a rotation or a display swap can change the density under a
        // running activity. The NUMBERS are re-read here; the STYLE is not
        // re-scaled, and that is a stated limitation rather than an omission -
        // ImGuiStyle::ScaleAllSizes multiplies in place, so re-scaling mid
        // session needs the style rebuilt from a default-constructed one and
        // the theme re-applied, which is AppWindow's business and a slice of
        // its own. A rotation therefore keeps the scale the activity launched
        // at, which is correct for the axis that was binding and generous or
        // mean on the other.
        if (app_ != nullptr && app_->config != nullptr) {
            const int dpi = static_cast<int>(AConfiguration_getDensity(app_->config));
            if (dpi > 0 && dpi != ACONFIGURATION_DENSITY_ANY &&
                dpi != ACONFIGURATION_DENSITY_NONE) {
                densityDpi_ = dpi;
                densityScale_ = androidDensityScale(dpi);
            }
        }
        refreshSurfaceSize();
        break;

    case APP_CMD_GAINED_FOCUS:
        hasFocus_ = true;
        break;

    case APP_CMD_LOST_FOCUS:
    case APP_CMD_PAUSE:
    case APP_CMD_STOP:
        // THREE COMMANDS RATHER THAN ONE, and the set is what visible() is
        // built on. LOST_FOCUS is the reliable signal on most devices, but a
        // phone that goes straight to sleep can deliver PAUSE/STOP without
        // it. Any of them means nobody is looking, which is what pollEvents
        // idles on and what stops the hang watchdog reporting the idle.
        hasFocus_ = false;
        break;

    default:
        break;
    }
}

std::int32_t AndroidPlatformWindow::onInputEvent(AInputEvent* event) {
    if (event == nullptr || !imguiBackendUp_) { return 0; }
    // The long-press-to-right-click and two-finger-drag-to-wheel translation,
    // unchanged from the first-screen shell: the desktop interface uses a
    // right button and a wheel, and a touchscreen has neither.
    return input_.handleEvent(event);
}

// -- The frame loop --------------------------------------------------------

bool AndroidPlatformWindow::shouldClose() const {
    return closeRequested_ || (app_ != nullptr && app_->destroyRequested != 0);
}

void AndroidPlatformWindow::requestClose() {
    closeRequested_ = true;
    // Asks the framework to finish the activity as well, so the process does
    // not sit there having returned from android_main with the task still in
    // the recents list. Safe from this thread: ANativeActivity_finish posts to
    // the UI thread.
    if (app_ != nullptr && app_->activity != nullptr) {
        ANativeActivity_finish(app_->activity);
    }
}

bool AndroidPlatformWindow::drainEvents() {
    if (app_ == nullptr) { return true; }
    int events = 0;
    android_poll_source* source = nullptr;
    while (ALooper_pollOnce(0, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
        if (source != nullptr) { source->process(app_, source); }
        if (app_->destroyRequested != 0) {
            closeRequested_ = true;
            return true;
        }
    }
    return false;
}

void AndroidPlatformWindow::pollEvents() {
    if (app_ == nullptr) { return; }
    if (drainEvents()) { return; }
    if (drawable()) {
        refreshSurfaceSize();
        return;
    }

    // NOTHING TO DRAW: SLEEP IN THE KERNEL. This is the entire idle-power
    // behaviour of the application on a phone, and it is also what keeps the
    // hang watchdog quiet - AppWindow's PresentGrace pauses it while
    // visible() is false, which is exactly this state, and the pause has no
    // timeout. The block is bounded (see kIdlePollMs) so the run loop still
    // turns and can still see a close request.
    int events = 0;
    android_poll_source* source = nullptr;
    const int id =
        ALooper_pollOnce(kIdlePollMs, nullptr, &events, reinterpret_cast<void**>(&source));
    if (id >= 0 && source != nullptr) { source->process(app_, source); }
    drainEvents();
    if (drawable()) { refreshSurfaceSize(); }
}

void AndroidPlatformWindow::waitEventsTimeout(double seconds) {
    if (app_ == nullptr) { return; }
    int ms = static_cast<int>(seconds * 1000.0);
    if (ms < 0) { ms = -1; }  // negative means "block until something arrives"
    int events = 0;
    android_poll_source* source = nullptr;
    const int id = ALooper_pollOnce(ms, nullptr, &events, reinterpret_cast<void**>(&source));
    if (id >= 0 && source != nullptr) { source->process(app_, source); }
    drainEvents();
}

void AndroidPlatformWindow::swapBuffers() {
    if (display_ == EGL_NO_DISPLAY) { return; }
    if (!hasSurface_ || surface_ == EGL_NO_SURFACE) {
        // The pbuffer needs no present, and asking for one on it is an error
        // rather than a no-op. The frame just drawn is thrown away, which is
        // what a frame drawn with no window on screen is worth.
        return;
    }
    if (eglSwapBuffers(display_, surface_) != EGL_TRUE) {
        // EGL_BAD_SURFACE here means the framework pulled the window out from
        // under us between the draw and the present, which is a normal thing
        // to happen on a task switch. APP_CMD_TERM_WINDOW is already on its
        // way; there is nothing to do but not draw into it again.
        logError("eglSwapBuffers failed");
    }
}

double AndroidPlatformWindow::time() const {
    // 0.0 before create() and after destroy(), which is the contract
    // glfwGetTime() set and which the telemetry in AppWindow's constructor has
    // always seen.
    if (timeBase_ == 0.0) { return 0.0; }
    return monotonicSeconds() - timeBase_;
}

// -- Geometry --------------------------------------------------------------

void AndroidPlatformWindow::framebufferSize(int& width, int& height) const {
    width = width_;
    height = height_;
}

// THE SAME NUMBERS AS THE FRAMEBUFFER, and deliberately. On a desktop these
// differ because the window is measured in scaled screen coordinates and the
// drawable in pixels; on Android there is one surface measured in pixels and
// nothing else to report. The interface is scaled by gui/ui_scale.hpp instead,
// which is a decision about the layout rather than a property of the window.
void AndroidPlatformWindow::windowSize(int& width, int& height) const {
    width = width_;
    height = height_;
}

void AndroidPlatformWindow::windowPos(int& x, int& y) const {
    x = 0;
    y = 0;
}

// A window that fills the screen cannot be moved. AppWindow calls this when it
// restores a remembered main-window position, and doing nothing is the right
// answer rather than a silent failure - the position it remembered was this
// one.
void AndroidPlatformWindow::setWindowPos(int, int) {}

void AndroidPlatformWindow::contentScale(float& x, float& y) const {
    x = densityScale_;
    y = densityScale_;
}

bool AndroidPlatformWindow::monitorWorkArea(int& x, int& y, int& width, int& height) const {
    if (width_ <= 0 || height_ <= 0) { return false; }
    x = 0;
    y = 0;
    width = width_;
    height = height_;
    return true;
}

// -- Window state ----------------------------------------------------------

// ICONIFIED AND VISIBLE ARE THE SAME QUESTION HERE, and the answer is what
// makes a backgrounded phone not look like a hung application: AppWindow's
// PresentGrace holds the hang watchdog's pause for as long as this says the
// window is not presenting, which is exactly as long as pollEvents blocks.
bool AndroidPlatformWindow::iconified() const { return !drawable(); }
bool AndroidPlatformWindow::visible() const { return drawable(); }
bool AndroidPlatformWindow::focused() const { return hasFocus_; }

// An Android activity IS maximised - it fills the screen and cannot do
// otherwise - so the cabinet rail's maximise key correctly reads as already
// maximised and its three keys have nothing to do but the close.
bool AndroidPlatformWindow::maximised() const { return true; }

void AndroidPlatformWindow::iconify() {}
void AndroidPlatformWindow::maximise() {}
void AndroidPlatformWindow::restore() {}
void AndroidPlatformWindow::requestAttention() {}

std::string AndroidPlatformWindow::title() const { return title_; }

// Remembered, and shown nowhere: an Android activity's label comes from the
// manifest and the task switcher, neither of which a native surface can
// change. The getter still has to answer what was asked for - see the
// PlatformWindow contract.
void AndroidPlatformWindow::setTitle(const char* title) {
    if (title != nullptr) { title_ = title; }
}

// -- Clipboard, cursor, keys -----------------------------------------------

std::string AndroidPlatformWindow::clipboardText() const { return clipboard_.get(); }

void AndroidPlatformWindow::setClipboardText(const char* text) { clipboard_.set(text); }

// THERE IS NO POINTER TO SHOW OR HIDE. Both are answered honestly rather than
// pretended: nothing is visible, and a request for a shape is remembered so a
// caller can read back what it asked for (which is what makes the pair
// testable at all - see gui/android_window_logic.hpp for the same argument
// about the clipboard).
bool AndroidPlatformWindow::cursorVisible() const { return false; }
void AndroidPlatformWindow::setCursorVisible(bool) {}
CursorShape AndroidPlatformWindow::cursorShape() const { return shape_; }
void AndroidPlatformWindow::setCursorShape(CursorShape shape) { shape_ = shape; }

// ImGui's own idea of where the last touch was, which on a touchscreen is the
// only idea there is - there is no OS cursor to compare it against. Read by
// the input ledger (FOXSDR_DEBUG_INPUT), whose whole purpose on the desktop is
// to put the two side by side; on a phone it reports one number twice, and
// that is the true thing to report.
void AndroidPlatformWindow::cursorPos(double& x, double& y) const {
    const ImVec2 p = ImGui::GetIO().MousePos;
    x = static_cast<double>(p.x);
    y = static_cast<double>(p.y);
}

// NO NAMES. The NDK has no keycode-to-printable-character API at all
// (AKeyEvent_* reports the code, the action and the metastate and stops
// there); the mapping lives in Java, in KeyCharacterMap, and reaching it needs
// the JNI slice the clipboard note describes. Null is the documented "this
// platform cannot say", which is the same answer GLFW gives for a key with no
// printable form.
const char* AndroidPlatformWindow::keyName(int, int) const { return nullptr; }

// -- Display health --------------------------------------------------------

// Android has no equivalent notion: there is no display-settings query to
// fail, and a configuration change arrives as a command rather than as an
// error. Always 0, which is what the contract asks of a platform with no such
// notion - and it keeps AppWindow's display-change grace armed only by the
// Windows side that really has one.
unsigned AndroidPlatformWindow::displayErrorCount() const { return 0u; }

// -- The ImGui PLATFORM backend --------------------------------------------

bool AndroidPlatformWindow::imguiBackendInit() {
    if (app_ == nullptr || app_->window == nullptr) { return false; }
    // NO MOUSE CURSOR CHANGES: there is no cursor to change and the Android
    // backend does not implement the capability, so asking for it would have
    // ImGui request shapes nothing can supply.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (!ImGui_ImplAndroid_Init(app_->window)) { return false; }
    imguiBackendUp_ = true;
    return true;
}

void AndroidPlatformWindow::imguiBackendNewFrame() {
    // A long press is a TIMEOUT, not an event - nothing arrives from the
    // system when a finger stops moving - so the press that becomes a right
    // click has to be noticed once a frame, and before ImGui::NewFrame so the
    // button is seen in the same frame it was decided.
    input_.newFrame();

    if (hasSurface_) {
        ImGui_ImplAndroid_NewFrame();
        return;
    }

    // THE BACKEND MUST NOT BE CALLED WITH NO WINDOW. Its NewFrame reads
    // ANativeWindow_getWidth of the window it was initialised with, and
    // between APP_CMD_TERM_WINDOW and the next APP_CMD_INIT_WINDOW that window
    // has been destroyed by the framework - reading it is a use-after-free.
    // The two things it supplies are supplied here instead, from the last size
    // the interface was laid out at and from our own clock.
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width_ > 0 ? width_ : 1),
                            static_cast<float>(height_ > 0 ? height_ : 1));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    const double now = monotonicSeconds();
    io.DeltaTime = (lastIdleFrame_ > 0.0)
                       ? static_cast<float>(now - lastIdleFrame_)
                       : (1.0f / 60.0f);
    // ImGui refuses a non-positive delta; a bounded idle poll can land on the
    // same microsecond twice on a coarse clock.
    if (!(io.DeltaTime > 0.0f)) { io.DeltaTime = 1.0f / 60.0f; }
    lastIdleFrame_ = now;
}

void AndroidPlatformWindow::imguiBackendShutdown() {
    if (!imguiBackendUp_) { return; }
    ImGui_ImplAndroid_Shutdown();
    imguiBackendUp_ = false;
}

// -- DESKTOP ONLY ----------------------------------------------------------
//
// EVERY ONE OF THESE IS DECLINED, and PlatformWindow says in as many words
// that an implementation which declines them all is a correct one: AppWindow
// takes the same path a desktop whose driver refuses a second shared GL
// context takes, which already ships and is already tested
// (tests/test_viewports.cpp). There is one surface on a phone and no desktop
// to put a second one on, so a torn-off page has nowhere to go; every page
// stays inside the main window, which is what supportsMultiViewport() == false
// tells gui/viewport_policy.hpp, and the log says so once.

bool AndroidPlatformWindow::supportsMultiViewport() const { return false; }

// Never reached: run() only probes when supportsMultiViewport() is true. False
// anyway, so a future caller that forgets that gets the safe answer.
bool AndroidPlatformWindow::probeSecondRenderContext() { return false; }

int AndroidPlatformWindow::viewportWindowCreationFailures() const { return 0; }

// The frame loop saves and restores the current render context around drawing
// the torn-off windows. With viewports off it never runs, and there is nothing
// to save: the one EGL context is current for the whole session.
void* AndroidPlatformWindow::currentRenderContext() const { return nullptr; }
void AndroidPlatformWindow::setCurrentRenderContext(void*) {}

bool AndroidPlatformWindow::viewportFramebufferSize(void*, int& width, int& height) const {
    width = 0;
    height = 0;
    return false;
}

void AndroidPlatformWindow::iconifyViewport(void*) {}
void AndroidPlatformWindow::setViewportVisible(void*, bool) {}

// Scope mode hides every torn-off page and must not hide the one it is drawing
// itself in. There are no torn-off pages, so the only handle that can reach
// this is the main viewport's - and on Android ImGui's main viewport carries no
// platform handle at all, so a null handle IS the main window here. Answering
// true for it keeps scope mode from hiding the interface it is drawn in.
bool AndroidPlatformWindow::isMainWindowHandle(const void*) const { return true; }

// There is no title bar to take off: an Android activity's surface arrives
// with no chrome of any kind. False means drawCabinetRail draws no minimise/
// maximise/close keys of its own... which is exactly right for a platform
// where the system's back gesture is how an activity is left.
bool AndroidPlatformWindow::installNativeFrame() { return false; }

}  // namespace cascade::gui
