// platform_window_android.hpp - the phone's PlatformWindow: EGL, a
// NativeActivity's ALooper, and the ImGui Android backend.
//
// THE COUNTERPART OF gui/platform_window_glfw.hpp, and the reason that file's
// interface was extracted at all. AppWindow holds a PlatformWindow& and never
// learns which implementation it got, so the SAME AppWindow::run() - the
// function rail, the spectrum, the waterfall, the pages, all 18,500 lines of
// it - drives a phone when handed one of these.
//
// THE THREE THINGS THAT ARE GENUINELY DIFFERENT FROM A DESKTOP, because they
// are what the bodies in the .cpp are mostly about:
//
//   1. THE SURFACE IS NOT OURS. The framework takes the drawing surface away
//      whenever the activity stops (screen off, task switch, rotation) and
//      hands back a different one on the way in - APP_CMD_INIT_WINDOW and
//      APP_CMD_TERM_WINDOW arrive many times in one run. AppWindow's run loop
//      has no state for that and must not need one, so this object absorbs it:
//      the EGL DISPLAY and CONTEXT are created once and live for the whole
//      session (so every texture, buffer and shader the renderer backend owns
//      survives), and only the window SURFACE comes and goes. Between them the
//      context is kept current on a 1x1 pbuffer, so a frame drawn while the
//      activity is in the background is drawn into a throwaway pixel rather
//      than into no context at all.
//
//   2. THE LOOP MUST BE ABLE TO IDLE. An Android activity spends most of its
//      life not being looked at, and a frame drawn then is a frame nobody
//      sees. pollEvents() therefore BLOCKS in ALooper_pollOnce while the
//      activity has no surface or no focus - bounded, so the loop still turns
//      about once a second and can still notice a close request - and
//      visible()/iconified() report that state, which is what makes
//      AppWindow's PresentGrace hold the hang watchdog's pause across it. A
//      backgrounded phone is not a hung application and must never be reported
//      as one.
//
//   3. THERE IS ONE SURFACE AND NO DESKTOP. Every method in
//      PlatformWindow's DESKTOP ONLY group is declined - supportsMultiViewport
//      false first of all, which sends gui/viewport_policy.hpp down the
//      single-viewport path that a desktop with a driver refusing a second GL
//      context already takes and already ships.
//
// ONE PER PROCESS, like the GLFW one: it installs itself as the android_app's
// userData and command/input callbacks, and a second instance would fight the
// first for them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PLATFORM_WINDOW_ANDROID_HPP
#define CASCADE_GUI_PLATFORM_WINDOW_ANDROID_HPP

#include <EGL/egl.h>

#include <cstdint>
#include <string>

#include "gui/android_window_logic.hpp"
#include "gui/platform_window.hpp"
#include "platform/android/input.hpp"

struct android_app;
struct ANativeWindow;
struct AInputEvent;

namespace cascade::gui {

class AndroidPlatformWindow final : public PlatformWindow {
public:
    // The android_app the NDK's glue handed android_main. Held, not owned.
    explicit AndroidPlatformWindow(android_app* app) : app_(app) {}
    ~AndroidPlatformWindow() override;

    AndroidPlatformWindow(const AndroidPlatformWindow&) = delete;
    AndroidPlatformWindow& operator=(const AndroidPlatformWindow&) = delete;

    bool create(const CreateInfo& info) override;
    void show() override;
    void destroy() override;

    bool shouldClose() const override;
    void requestClose() override;
    void pollEvents() override;
    void waitEventsTimeout(double seconds) override;
    void swapBuffers() override;
    double time() const override;

    void framebufferSize(int& width, int& height) const override;
    void windowSize(int& width, int& height) const override;
    void windowPos(int& x, int& y) const override;
    void setWindowPos(int x, int y) override;
    void contentScale(float& x, float& y) const override;
    bool monitorWorkArea(int& x, int& y, int& width, int& height) const override;

    bool iconified() const override;
    bool visible() const override;
    bool focused() const override;
    bool maximised() const override;
    void iconify() override;
    void maximise() override;
    void restore() override;
    void requestAttention() override;
    std::string title() const override;
    void setTitle(const char* title) override;

    std::string clipboardText() const override;
    void setClipboardText(const char* text) override;
    bool cursorVisible() const override;
    void setCursorVisible(bool visible) override;
    CursorShape cursorShape() const override;
    void setCursorShape(CursorShape shape) override;
    void cursorPos(double& x, double& y) const override;
    const char* keyName(int key, int scancode) const override;

    unsigned displayErrorCount() const override;

    bool imguiBackendInit() override;
    void imguiBackendNewFrame() override;
    void imguiBackendShutdown() override;

    // -- DESKTOP ONLY: every one of these is declined; see the .cpp ---------
    bool supportsMultiViewport() const override;
    bool probeSecondRenderContext() override;
    int viewportWindowCreationFailures() const override;
    void* currentRenderContext() const override;
    void setCurrentRenderContext(void* context) override;
    bool viewportFramebufferSize(void* handle, int& width, int& height) const override;
    void iconifyViewport(void* handle) override;
    void setViewportVisible(void* handle, bool visible) override;
    bool isMainWindowHandle(const void* handle) const override;
    bool installNativeFrame() override;

    // -- Called by the android_app callbacks, which are free functions -----
    //
    // Public because android_native_app_glue's onAppCmd/onInputEvent are C
    // function pointers with no user data beyond android_app::userData, so the
    // free functions that receive them have to be able to reach these. Not
    // part of PlatformWindow, and nothing in src/gui calls them.
    void onAppCmd(std::int32_t cmd);
    std::int32_t onInputEvent(AInputEvent* event);

    // The scale the interface is drawn at on THIS screen: the largest factor
    // at which the desktop's 1280 x 720 layout fits the framebuffer, capped at
    // the density, overridable with FOXSDR_UI_SCALE. See gui/ui_scale.hpp for
    // the rule and why it is not simply the density. Zero until create().
    float uiScale() const { return uiScale_; }

private:
    // The window surface, created when the framework gives us a window and
    // destroyed when it takes one away. The context stays current on
    // pbuffer_ in between, which is what keeps every GL object alive.
    bool attachSurface(ANativeWindow* window);
    void detachSurface();
    // Pump whatever the looper already has, without blocking. Returns true if
    // the activity asked to go away.
    bool drainEvents();
    bool drawable() const { return hasSurface_ && hasFocus_; }
    void refreshSurfaceSize();

    android_app* app_ = nullptr;

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;   // the window's, or EGL_NO_SURFACE
    EGLSurface pbuffer_ = EGL_NO_SURFACE;   // the 1x1 stand-in; see the header

    bool hasSurface_ = false;
    bool hasFocus_ = false;
    bool closeRequested_ = false;
    bool imguiBackendUp_ = false;

    int width_ = 0;
    int height_ = 0;
    int densityDpi_ = kAndroidBaselineDpi;
    float densityScale_ = 1.0f;
    float uiScale_ = 0.0f;

    double timeBase_ = 0.0;
    // The clock imguiBackendNewFrame() derives io.DeltaTime from while there
    // is no window, because the ImGui backend that normally supplies it
    // cannot be called then. Zero means "no idle frame yet".
    double lastIdleFrame_ = 0.0;
    std::string title_;
    LocalClipboard clipboard_;
    CursorShape shape_ = CursorShape::Arrow;
    platform::android::TouchInput input_;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLATFORM_WINDOW_ANDROID_HPP
