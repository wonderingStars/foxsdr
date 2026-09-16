// platform_window_glfw.cpp - the desktop PlatformWindow, over GLFW.
//
// EVERY glfw* CALL THIS APPLICATION MAKES IS IN THIS FILE, apart from the three
// in gui/win_frame.cpp (which needs glfwGetWin32Window to reach the HWND) and
// the GL header include in gui/waterfall_view.cpp. Before this file existed
// they were spread over 60 sites in app_window.cpp; the bodies below are those
// sites, moved rather than rewritten, with their reasoning carried across
// verbatim because the reasoning is the expensive part.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/platform_window_glfw.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <imgui_impl_glfw.h>

// After the ImGui backend: glfw3.h pulls in GL/gl.h on Windows, which the
// opengl3 backend must not see before its own embedded loader.
#include <GLFW/glfw3.h>

#include "core/diag_log.hpp"
#include "core/hang_watchdog.hpp"
// Generated window-icon pixels. Reached by a path relative to this file
// because resources/ is deliberately not on any target's include path - the
// icon is an asset, not a source root, and adding an include directory for one
// generated header would be a worse trade than a two-segment relative include.
#include "../../resources/icon/foxsdr_icon_rgba.hpp"
#include "gui/win_frame.hpp"

namespace cascade::gui {
namespace {

// GLFW reports failures through this callback *before* glfwInit/CreateWindow
// return their error codes, so printing here is what gives the user an actual
// reason instead of a bare "init failed".
// A DISPLAY-SETTINGS ERROR IS A DISPLAY CHANGE IN PROGRESS, and it is counted.
//
// GLFW error 65544 (GLFW_PLATFORM_ERROR) with "Failed to query display
// settings" is EnumDisplaySettingsW failing, which is what happens while the
// desktop is being re-configured underneath the process. It is the line
// immediately before the five-second SwapBuffers stall in field report "hang
// ntdll.dll @ cascade::gui::AppWindow::run" (0.96.3, atio6axx.dll), so it is
// the earliest warning this application gets - earlier, sometimes, than the
// WM_DISPLAYCHANGE the window procedure counts, because GLFW queries monitors
// from inside glfwPollEvents.
//
// A free counter rather than a member, because GLFW's error callback is a plain
// function pointer with no user data. Atomic because GLFW may report from
// whichever thread called in.
std::atomic<unsigned> g_glfwDisplayErrors{0};

void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "cascade: GLFW error %d: %s\n", code,
                 description ? description : "(no description)");
    // Matched on the MESSAGE as well as the code: GLFW_PLATFORM_ERROR covers
    // most of what the Win32 backend can refuse, and only the display-settings
    // one says the desktop is being reconfigured. Anything else keeps the
    // watchdog armed, which is the safe direction.
    if (code == 0x00010008 && description != nullptr &&
        std::strstr(description, "display settings") != nullptr) {
        g_glfwDisplayErrors.fetch_add(1u, std::memory_order_relaxed);
        cascade::core::diagWarnf(
            "display: GLFW could not query display settings - presentation may stall "
            "briefly; hang reports are suppressed for the next %u ms",
            cascade::core::HangWatchdog::kDisplayGraceMs);
    }
}

// Applies the FoxSDR mark to the window's title-bar, taskbar and Alt-Tab
// slots. The executable's own RT_GROUP_ICON (resources/icon/foxsdr.rc) is what
// Explorer shows; this is the separate, runtime-owned window icon, and setting
// both is what stops the shipped app ever showing the blank default.
//
// The pixels are COMPILED IN from resources/icon/foxsdr_icon_rgba.hpp rather
// than decoded from a .ico at runtime or pulled back out of the executable's
// resources with LoadImage/GetIconInfo. A raw RGBA array needs no
// image-decoding dependency (this tree's whole premise is a small,
// licence-audited dependency set), no Win32-only code path in an otherwise
// portable shell, and no GDI/DIB handle lifetime to leak. GLFW copies the
// pixel data before returning, so the arrays need no lifetime management.
//
// Best-effort by construction: glfwSetWindowIcon returns void, and any
// platform-level refusal surfaces through glfwErrorCallback as one printed
// line. Nothing here can fail the caller - a missing icon must never stop the
// app starting.
void applyWindowIcon(GLFWwindow* window) {
    if (window == nullptr) { return; }
    // GLFWimage::pixels is a non-const unsigned char*; the cast is safe
    // because GLFW only reads the buffer (it copies it during the call).
    const GLFWimage images[] = {
        {icon::kSize16, icon::kSize16, const_cast<unsigned char*>(icon::kPixels16)},
        {icon::kSize32, icon::kSize32, const_cast<unsigned char*>(icon::kPixels32)},
        {icon::kSize48, icon::kSize48, const_cast<unsigned char*>(icon::kPixels48)},
    };
    glfwSetWindowIcon(window, static_cast<int>(sizeof(images) / sizeof(images[0])),
                      images);
}

int standardCursorFor(CursorShape shape) {
    switch (shape) {
        case CursorShape::Arrow: return GLFW_ARROW_CURSOR;
        case CursorShape::TextInput: return GLFW_IBEAM_CURSOR;
        case CursorShape::Hand: return GLFW_POINTING_HAND_CURSOR;
        case CursorShape::ResizeEW: return GLFW_RESIZE_EW_CURSOR;
        case CursorShape::ResizeNS: return GLFW_RESIZE_NS_CURSOR;
        case CursorShape::ResizeAll: return GLFW_RESIZE_ALL_CURSOR;
    }
    return GLFW_ARROW_CURSOR;
}

}  // namespace

GlfwPlatformWindow::~GlfwPlatformWindow() { destroy(); }

// -- Lifetime --------------------------------------------------------------

bool GlfwPlatformWindow::create(const CreateInfo& info) {
    glfwSetErrorCallback(&glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "cascade: glfwInit failed\n");
        return false;
    }
    initialised_ = true;

    // HIDDEN UNTIL ITS FRAME IS SETTLED. The title bar comes off the window's
    // style after creation (frame::install), and a window that has already
    // been shown - and presented once - with the caption on has given every
    // layer beneath it a first look at a client area that is about to change.
    // A tester's 0.84.1 opened with the top of the picture off the top of the
    // window and every click landing below its control, until a resize made
    // the layers agree again. Nothing sees this window until the style it will
    // keep is the style it has.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window_ = glfwCreateWindow(info.width, info.height, info.title.c_str(), nullptr, nullptr);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (window_ == nullptr) {
        std::fprintf(stderr, "cascade: glfwCreateWindow failed\n");
        glfwTerminate();
        initialised_ = false;
        return false;
    }
    // A FLOOR ON THE WIDTH, because the top bar has one and could not keep it
    // alone: narrower than this and the volume dial is drawn outside the bar's
    // own child and clipped away, leaving no volume control at all.
    //
    // BOTH MINIMA HAVE TO BE GIVEN OR NEITHER IS APPLIED. GLFW's Win32 backend
    // fills ptMinTrackSize only when minwidth AND minheight are both set
    // (win32_window.c, WM_GETMINMAXINFO), so passing GLFW_DONT_CARE for the
    // height silently threw the width limit away too - which is exactly what
    // the first version of this line did, and it read as a working fix. The
    // height chosen is the modest one that keeps the bar and the head of the
    // rail on screen together; nothing on this face disappears below it, it
    // only gets less room to scroll in.
    if (info.minWidth > 0 && info.minHeight > 0) {
        glfwSetWindowSizeLimits(window_, info.minWidth, info.minHeight, GLFW_DONT_CARE,
                                GLFW_DONT_CARE);
    }
    // Before the context is made current: purely a window-manager property,
    // independent of GL, so even a run that fails at backend init has already
    // shown the right icon.
    applyWindowIcon(window_);
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);  // vsync: the GUI thread paces itself off the display
    return true;
}

void GlfwPlatformWindow::show() {
    if (window_ != nullptr) { glfwShowWindow(window_); }
}

void GlfwPlatformWindow::destroy() {
    if (cursor_ != nullptr) {
        glfwDestroyCursor(cursor_);
        cursor_ = nullptr;
    }
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    if (initialised_) {
        glfwTerminate();
        initialised_ = false;
    }
}

// -- The frame loop --------------------------------------------------------

bool GlfwPlatformWindow::shouldClose() const {
    return window_ != nullptr && glfwWindowShouldClose(window_) != 0;
}

void GlfwPlatformWindow::requestClose() {
    if (window_ != nullptr) { glfwSetWindowShouldClose(window_, GLFW_TRUE); }
}

void GlfwPlatformWindow::pollEvents() { glfwPollEvents(); }

void GlfwPlatformWindow::waitEventsTimeout(double seconds) { glfwWaitEventsTimeout(seconds); }

void GlfwPlatformWindow::swapBuffers() {
    if (window_ != nullptr) { glfwSwapBuffers(window_); }
}

double GlfwPlatformWindow::time() const { return glfwGetTime(); }

// -- Geometry --------------------------------------------------------------

void GlfwPlatformWindow::framebufferSize(int& width, int& height) const {
    width = 0;
    height = 0;
    if (window_ != nullptr) { glfwGetFramebufferSize(window_, &width, &height); }
}

void GlfwPlatformWindow::windowSize(int& width, int& height) const {
    width = 0;
    height = 0;
    if (window_ != nullptr) { glfwGetWindowSize(window_, &width, &height); }
}

void GlfwPlatformWindow::windowPos(int& x, int& y) const {
    x = 0;
    y = 0;
    if (window_ != nullptr) { glfwGetWindowPos(window_, &x, &y); }
}

void GlfwPlatformWindow::setWindowPos(int x, int y) {
    if (window_ != nullptr) { glfwSetWindowPos(window_, x, y); }
}

void GlfwPlatformWindow::contentScale(float& x, float& y) const {
    x = 1.0f;
    y = 1.0f;
    if (window_ != nullptr) { glfwGetWindowContentScale(window_, &x, &y); }
}

bool GlfwPlatformWindow::monitorWorkArea(int& x, int& y, int& width, int& height) const {
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor == nullptr) { return false; }
    int mx = 0, my = 0, mw = 0, mh = 0;
    glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
    if (mw <= 0 || mh <= 0) { return false; }
    x = mx;
    y = my;
    width = mw;
    height = mh;
    return true;
}

// -- Window state ----------------------------------------------------------

bool GlfwPlatformWindow::iconified() const {
    return window_ != nullptr && glfwGetWindowAttrib(window_, GLFW_ICONIFIED) != 0;
}

bool GlfwPlatformWindow::visible() const {
    return window_ != nullptr && glfwGetWindowAttrib(window_, GLFW_VISIBLE) != 0;
}

bool GlfwPlatformWindow::focused() const {
    return window_ != nullptr && glfwGetWindowAttrib(window_, GLFW_FOCUSED) != 0;
}

bool GlfwPlatformWindow::maximised() const {
    return window_ != nullptr && glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) != 0;
}

void GlfwPlatformWindow::iconify() {
    if (window_ != nullptr) { glfwIconifyWindow(window_); }
}

void GlfwPlatformWindow::maximise() {
    if (window_ != nullptr) { glfwMaximizeWindow(window_); }
}

void GlfwPlatformWindow::restore() {
    if (window_ != nullptr) { glfwRestoreWindow(window_); }
}

void GlfwPlatformWindow::requestAttention() {
    if (window_ != nullptr) { glfwRequestWindowAttention(window_); }
}

std::string GlfwPlatformWindow::title() const {
    if (window_ == nullptr) { return std::string(); }
    const char* t = glfwGetWindowTitle(window_);
    return t != nullptr ? std::string(t) : std::string();
}

void GlfwPlatformWindow::setTitle(const char* title) {
    if (window_ != nullptr && title != nullptr) { glfwSetWindowTitle(window_, title); }
}

// -- Clipboard, cursor, keys -----------------------------------------------

std::string GlfwPlatformWindow::clipboardText() const {
    if (window_ == nullptr) { return std::string(); }
    // Null when the clipboard is empty or holds something that will not
    // convert to text, which is not an error and must not read as one.
    const char* text = glfwGetClipboardString(window_);
    return text != nullptr ? std::string(text) : std::string();
}

void GlfwPlatformWindow::setClipboardText(const char* text) {
    if (window_ != nullptr && text != nullptr) { glfwSetClipboardString(window_, text); }
}

bool GlfwPlatformWindow::cursorVisible() const {
    return window_ != nullptr && glfwGetInputMode(window_, GLFW_CURSOR) != GLFW_CURSOR_HIDDEN;
}

void GlfwPlatformWindow::setCursorVisible(bool visible) {
    if (window_ == nullptr) { return; }
    glfwSetInputMode(window_, GLFW_CURSOR,
                     visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
}

CursorShape GlfwPlatformWindow::cursorShape() const { return shape_; }

void GlfwPlatformWindow::setCursorShape(CursorShape shape) {
    if (window_ == nullptr) { return; }
    // The request is remembered whatever the platform does with it, so a
    // caller can read back what it asked for.
    shape_ = shape;
    GLFWcursor* next =
        (shape == CursorShape::Arrow) ? nullptr : glfwCreateStandardCursor(standardCursorFor(shape));
    // Installed BEFORE the old one is destroyed: a window pointing at a
    // destroyed cursor is undefined, and glfwSetCursor(nullptr) is how the
    // platform's own default comes back.
    glfwSetCursor(window_, next);
    if (cursor_ != nullptr) { glfwDestroyCursor(cursor_); }
    cursor_ = next;
}

void GlfwPlatformWindow::cursorPos(double& x, double& y) const {
    x = 0.0;
    y = 0.0;
    if (window_ != nullptr) { glfwGetCursorPos(window_, &x, &y); }
}

const char* GlfwPlatformWindow::keyName(int key, int scancode) const {
    return glfwGetKeyName(key, scancode);
}

// -- Display health --------------------------------------------------------

unsigned GlfwPlatformWindow::displayErrorCount() const {
    return g_glfwDisplayErrors.load(std::memory_order_relaxed);
}

// -- The ImGui PLATFORM backend --------------------------------------------

bool GlfwPlatformWindow::imguiBackendInit() {
    return window_ != nullptr && ImGui_ImplGlfw_InitForOpenGL(window_, true);
}

void GlfwPlatformWindow::imguiBackendNewFrame() { ImGui_ImplGlfw_NewFrame(); }

void GlfwPlatformWindow::imguiBackendShutdown() { ImGui_ImplGlfw_Shutdown(); }

// -- DESKTOP ONLY ----------------------------------------------------------

bool GlfwPlatformWindow::supportsMultiViewport() const { return true; }

// CAN THIS DISPLAY MAKE A SECOND OPENGL CONTEXT AT ALL?
//
// The only honest way to answer is to ask for one, so this makes a 1x1 hidden
// window sharing the main one and throws it away again. It is exactly the call
// ImGui_ImplGlfw_CreateWindow makes for every torn-off page, which is the
// point: a machine whose driver answers "WGL: Failed to create OpenGL context"
// (GLFW 65543) answers it here, at startup, where the application can turn the
// feature off and say why - rather than on the first drag, where 0.95.0 took
// an access violation instead.
//
// The hint and the current context are both put back. GLFW window hints are
// sticky and the ImGui backend sets its own before every creation, but the
// main window's GLFW_VISIBLE=TRUE is restored by create() and this must not
// leave a different value behind for it.
bool GlfwPlatformWindow::probeSecondRenderContext() {
    if (window_ == nullptr) { return false; }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* probe = glfwCreateWindow(1, 1, "foxsdr viewport probe", nullptr, window_);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
    if (probe != nullptr) { glfwDestroyWindow(probe); }
    // glfwDestroyWindow clears the current context if it was the destroyed
    // one's; the creation never makes its own current, but restoring is one
    // call and a run that lost the main context would draw nothing at all.
    glfwMakeContextCurrent(window_);
    return probe != nullptr;
}

int GlfwPlatformWindow::viewportWindowCreationFailures() const {
    return ImGui_ImplGlfw_ViewportWindowCreationFailures();
}

void* GlfwPlatformWindow::currentRenderContext() const { return glfwGetCurrentContext(); }

void GlfwPlatformWindow::setCurrentRenderContext(void* context) {
    glfwMakeContextCurrent(static_cast<GLFWwindow*>(context));
}

bool GlfwPlatformWindow::viewportFramebufferSize(void* handle, int& width, int& height) const {
    width = 0;
    height = 0;
    if (handle == nullptr) { return false; }
    glfwGetFramebufferSize(static_cast<GLFWwindow*>(handle), &width, &height);
    return true;
}

void GlfwPlatformWindow::iconifyViewport(void* handle) {
    if (handle != nullptr) { glfwIconifyWindow(static_cast<GLFWwindow*>(handle)); }
}

void GlfwPlatformWindow::setViewportVisible(void* handle, bool visible) {
    if (handle == nullptr) { return; }
    GLFWwindow* w = static_cast<GLFWwindow*>(handle);
    if (visible) {
        glfwShowWindow(w);
    } else {
        glfwHideWindow(w);
    }
}

bool GlfwPlatformWindow::isMainWindowHandle(const void* handle) const {
    return handle != nullptr && handle == static_cast<const void*>(window_);
}

bool GlfwPlatformWindow::installNativeFrame() {
    return window_ != nullptr && cascade::gui::frame::install(window_);
}

}  // namespace cascade::gui
