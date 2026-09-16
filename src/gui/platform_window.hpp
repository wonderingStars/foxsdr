// platform_window.hpp - everything AppWindow asks of the window system, as one
// interface.
//
// WHY THIS EXISTS. AppWindow made 60 direct glfw* calls spread over 18,600
// lines, and GLFW does not exist on Android: there is no glfwCreateWindow, no
// glfwGetTime, no second OpenGL context to share and no desktop to tear a page
// out onto. Rather than #ifdef those 60 sites, they now go through this
// interface, and the GLFW bodies live in one file (gui/platform_window_glfw.cpp)
// that an Android build simply does not compile. app_window.cpp contains no
// glfw* call and no GLFW header after this change, which is the property the
// Android port needs and the one a grep can check.
//
// WHAT IT IS NOT. It is not a portability layer for the whole application: the
// OpenGL calls in the frame loop (glViewport, glClear, glReadPixels) and the
// ImGui *renderer* backend stay where they are, because desktop GL and GLES
// share that backend and the only difference is the shader version string. Nor
// is it a general window toolkit - every method below exists because something
// in this application needs it, or because the Android shell will have to
// supply it in place of the ImGui GLFW backend that supplies it today.
//
// THE DESKTOP-ONLY GROUP, at the bottom, is the honest part of the design.
// Torn-off pages in their own operating-system windows, a probe for a second
// shared GL context, and the Windows custom title bar are desktop ideas with no
// Android meaning at all. They are not hidden behind #ifdef; they are methods an
// implementation may DECLINE - return false, return zero, do nothing - and
// AppWindow already copes with that, because a desktop that refuses a second GL
// context takes exactly the same path (see gui/viewport_policy.hpp).
//
// THREADING. Every method is called from the GUI thread, with the sole
// exception of displayErrorCount(), which reads a counter the window system may
// raise from whichever thread called into it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace cascade::gui {

// The pointer shapes a window can wear. Deliberately the small set every
// platform has: a shape nothing maps to must fall back to Arrow rather than
// leave the pointer on whatever the last request was.
enum class CursorShape {
    Arrow,
    TextInput,
    Hand,
    ResizeEW,
    ResizeNS,
    ResizeAll,
};

// The one window this application has, the events that reach it, and the clock
// the frame loop is paced and measured by.
//
// A PlatformWindow is constructed empty and owns nothing until create()
// succeeds; destroy() puts it back in that state. That two-step exists so
// main() can own the object (its lifetime must outlive AppWindow's) while
// AppWindow::run() still decides when the window appears and when it goes away,
// which is what keeps the teardown ordering in run() - config save, pipeline
// join, GL teardown, watchdog stop - exactly as it was.
class PlatformWindow {
public:
    // What create() is asked for. The minimum size is part of the request
    // rather than a separate call because BOTH minima have to be given or
    // neither is applied on Win32 (see kMinWindowW in app_window.cpp); a
    // single struct makes forgetting one impossible.
    struct CreateInfo {
        int width = 1280;
        int height = 720;
        int minWidth = 0;   // 0 = no floor
        int minHeight = 0;  // 0 = no floor
        std::string title;
    };

    virtual ~PlatformWindow() = default;

    // -- Lifetime ----------------------------------------------------------

    // Bring the window system up and create the window, ready to draw into:
    // context current, vsync on, icon applied, size floor set, and NOT YET
    // VISIBLE. The window stays hidden until show() so the frame it will keep
    // is the frame it is first seen with. Returns false having printed the
    // reason to stderr and released anything it took.
    virtual bool create(const CreateInfo& info) = 0;

    // Put the window on screen. Called once, after the ImGui backends are up
    // and the native frame (if any) has been installed.
    virtual void show() = 0;

    // Destroy the window and shut the window system down. Called once, inside
    // the shutdown budget - see the teardown in AppWindow::run().
    virtual void destroy() = 0;

    // -- The frame loop ----------------------------------------------------

    // Has the user asked for this window to close (the desktop's own close
    // button, Alt-F4, a close request from the compositor)?
    virtual bool shouldClose() const = 0;

    // Ask for it from inside the application: the rail's close key takes the
    // same path the desktop's would, so the run loop shuts the receiver down
    // cleanly rather than exiting from a draw call.
    virtual void requestClose() = 0;

    // Pump the window system's queue and return immediately.
    virtual void pollEvents() = 0;

    // Pump it, waiting up to `seconds` for something to arrive. Not used by
    // the frame loop, which is paced by vsync; here because a platform that
    // stops presenting when the application is in the background has no other
    // way to idle, which is the normal state of an Android activity.
    virtual void waitEventsTimeout(double seconds) = 0;

    // Present the frame just drawn.
    virtual void swapBuffers() = 0;

    // Seconds since the window system came up, monotonic. THE application's
    // clock: the frame loop, the config debounce, the present-grace window and
    // every telemetry duration are measured with it. Returns 0.0 before
    // create() and after destroy(), which is what glfwGetTime() did and what
    // the telemetry read in the constructor has always seen.
    virtual double time() const = 0;

    // -- Geometry ----------------------------------------------------------

    // The drawable, in pixels. Not the same as windowSize() on a scaled
    // display, and it is this one glViewport takes.
    virtual void framebufferSize(int& width, int& height) const = 0;

    // The window, in screen coordinates.
    virtual void windowSize(int& width, int& height) const = 0;
    virtual void windowPos(int& x, int& y) const = 0;
    virtual void setWindowPos(int x, int y) = 0;

    // Framebuffer pixels per screen coordinate. Not read by AppWindow today -
    // the ImGui GLFW backend supplies the main viewport's DpiScale - and here
    // because on Android nothing else will.
    virtual void contentScale(float& x, float& y) const = 0;

    // The usable area of the monitor this window is on, excluding the taskbar
    // or its equivalent. False, leaving the outputs untouched, where the
    // platform cannot say. AppWindow reads ImGui's platform-monitor list for
    // the page geometry instead (it has to: it needs the monitor a page's
    // CENTRE falls on, not this window's), so this serves the Android shell
    // and anything later that wants the answer without an ImGui frame.
    virtual bool monitorWorkArea(int& x, int& y, int& width, int& height) const = 0;

    // -- Window state ------------------------------------------------------

    virtual bool iconified() const = 0;
    virtual bool visible() const = 0;
    virtual bool focused() const = 0;
    virtual bool maximised() const = 0;

    virtual void iconify() = 0;
    virtual void maximise() = 0;
    virtual void restore() = 0;

    // Ask the desktop to draw attention to this window (a flashing taskbar
    // button, a bouncing dock icon). No-op where there is nothing to flash.
    virtual void requestAttention() = 0;

    // The name the desktop shows. Set once at create() today; a setter as well
    // because a title that can never change is a title that cannot say what the
    // radio is tuned to, and the Android shell has no creation-time title. The
    // getter exists so a caller can read back what it asked for - a platform
    // that shows no title anywhere still has to remember one.
    virtual std::string title() const = 0;
    virtual void setTitle(const char* title) = 0;

    // -- Clipboard, cursor, keys -------------------------------------------
    //
    // NONE OF THESE ARE CALLED BY AppWindow, and that is worth stating rather
    // than leaving a reader to wonder: on the desktop the ImGui GLFW backend
    // owns the clipboard, the pointer shape and the key names, and it does it
    // by calling GLFW itself. The Android backend (imgui_impl_android) owns
    // none of them - it has no clipboard, no cursor and no key names - so the
    // shell has to supply them, and it needs somewhere to supply them FROM.
    // They are declared here, implemented over GLFW, and tested, so the Android
    // implementation has a definition to match rather than a blank page.

    // The clipboard's text, empty when it holds none (or none convertible).
    virtual std::string clipboardText() const = 0;
    virtual void setClipboardText(const char* text) = 0;

    virtual bool cursorVisible() const = 0;
    virtual void setCursorVisible(bool visible) = 0;

    // The shape last requested. A platform with no pointer remembers the
    // request and shows nothing, so a caller can still read back what it asked
    // for - which is what makes this testable without a user.
    virtual CursorShape cursorShape() const = 0;
    virtual void setCursorShape(CursorShape shape) = 0;

    // The pointer, in client coordinates. Read by the input ledger
    // (FOXSDR_DEBUG_INPUT), which exists to put the OS's idea and ImGui's idea
    // of where the pointer is side by side in one line.
    virtual void cursorPos(double& x, double& y) const = 0;

    // The printable name of a key on THIS keyboard layout, or null. Platform
    // key code and scancode, exactly as the platform numbers them.
    virtual const char* keyName(int key, int scancode) const = 0;

    // -- Display health ----------------------------------------------------

    // HOW MANY TIMES THE WINDOW SYSTEM HAS FAILED TO READ THE DISPLAY,
    // monotonic, readable from any thread.
    //
    // This is not a diagnostic curiosity. A display-settings query failing is
    // the earliest warning this application gets that the desktop is being
    // reconfigured underneath it, and the present call that follows can stall
    // for seconds (0.96.3, an AMD stack, five seconds inside atio6axx.dll).
    // The frame loop sums it with the window procedure's own WM_DISPLAYCHANGE
    // count and pauses hang reporting across the pair. Always 0 where the
    // platform has no such notion.
    virtual unsigned displayErrorCount() const = 0;

    // -- The ImGui PLATFORM backend ----------------------------------------
    //
    // Which backend pairs with this window is the implementation's business,
    // not AppWindow's: imgui_impl_glfw here, imgui_impl_android there. The
    // RENDERER backend (imgui_impl_opengl3) stays in AppWindow, because both
    // platforms use it.

    virtual bool imguiBackendInit() = 0;
    virtual void imguiBackendNewFrame() = 0;
    virtual void imguiBackendShutdown() = 0;

    // -- DESKTOP ONLY ------------------------------------------------------
    //
    // Everything below may be declined. An implementation that declines them
    // all is a correct implementation; AppWindow takes the same path it takes
    // on a desktop whose driver refuses a second GL context, which is a path
    // that already ships and is already tested (tests/test_viewports.cpp).

    // Can a page be torn out into its own operating-system window AT ALL on
    // this platform? False on Android, where there is one surface and no
    // desktop to put a second one on. False here also suppresses the probe and
    // the failure count below, so a declining implementation need do nothing
    // else.
    virtual bool supportsMultiViewport() const = 0;

    // ASK THE DRIVER FOR A SECOND SHARED RENDER CONTEXT, and throw it away.
    // The only honest way to find out whether torn-off pages can work on this
    // machine - a tester's driver answered "WGL: Failed to create OpenGL
    // context" and 0.95.0 died on the first drag. False means the feature is
    // turned off with a reason in the log instead.
    virtual bool probeSecondRenderContext() = 0;

    // How many times the platform backend has been refused a window for a
    // torn-off page. Monotonic within a session; a driver can start refusing
    // after the first frame (a GPU reset, an adapter switch), which the startup
    // probe cannot see.
    virtual int viewportWindowCreationFailures() const = 0;

    // The current render context, and how to put it back. Opaque: the frame
    // loop saves it, lets ImGui draw every torn-off window (which leaves
    // whichever it drew last current), and restores it - otherwise the next
    // frame's clear would paint the main UI into a torn-off page. On GLFW the
    // context handle and the window handle are the same object, which is why
    // the same setter also selects a viewport's context below.
    virtual void* currentRenderContext() const = 0;
    virtual void setCurrentRenderContext(void* context) = 0;

    // A torn-off page's window, addressed by the handle ImGui holds for it
    // (ImGuiViewport::PlatformHandle). Null handles are tolerated and do
    // nothing: a refused window leaves exactly that behind.
    virtual bool viewportFramebufferSize(void* handle, int& width, int& height) const = 0;
    virtual void iconifyViewport(void* handle) = 0;
    virtual void setViewportVisible(void* handle, bool visible) = 0;

    // Is this handle the MAIN window? Scope mode hides every torn-off page and
    // must not hide the one it is drawing itself in.
    virtual bool isMainWindowHandle(const void* handle) const = 0;

    // TAKE THE TITLE BAR OFF, keeping the desktop's own move, resize, snap,
    // maximise and system menu. True only on Windows, where gui/win_frame.cpp
    // subclasses the window procedure; the draw code asks frame::installed()
    // and draws the three keys only where it succeeded.
    virtual bool installNativeFrame() = 0;
};

}  // namespace cascade::gui
