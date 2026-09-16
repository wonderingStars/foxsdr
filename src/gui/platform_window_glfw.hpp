// platform_window_glfw.hpp - the desktop PlatformWindow: GLFW and the ImGui
// GLFW backend.
//
// The only file that names this type is main.cpp, which constructs one and
// hands it to AppWindow::run(). AppWindow itself never sees it - it holds a
// PlatformWindow& - which is what lets an Android build hand run() a different
// implementation without touching app_window.cpp.
//
// NO GLFW HEADER HERE. main.cpp is a command-line parser that happens to end in
// a window, and the tests include app_window.hpp; neither should acquire a
// windowing library through an include. GLFWwindow is forward-declared for the
// same reason app_window.hpp used to forward-declare it, and every GLFW call
// lives in the .cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

#include "gui/platform_window.hpp"

struct GLFWwindow;
struct GLFWcursor;

namespace cascade::gui {

// One per process. GLFW itself is a singleton (glfwInit/glfwTerminate are
// global), so a second live instance would fight the first over the library's
// state; nothing in this application creates one, and this note is the reason
// nothing should.
class GlfwPlatformWindow final : public PlatformWindow {
public:
    GlfwPlatformWindow() = default;
    ~GlfwPlatformWindow() override;

    GlfwPlatformWindow(const GlfwPlatformWindow&) = delete;
    GlfwPlatformWindow& operator=(const GlfwPlatformWindow&) = delete;

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

private:
    GLFWwindow* window_ = nullptr;
    bool initialised_ = false;
    // The shape last asked for, remembered rather than read back: GLFW has no
    // "what cursor is this window wearing" query, and a caller that cannot read
    // back what it set cannot be tested without a pair of eyes.
    CursorShape shape_ = CursorShape::Arrow;
    // The standard cursor currently installed, owned here. Null means the
    // window wears the platform's own default, which is what Arrow restores.
    GLFWcursor* cursor_ = nullptr;
};

}  // namespace cascade::gui
