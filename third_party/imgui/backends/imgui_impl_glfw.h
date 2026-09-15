// dear imgui: Platform Backend for GLFW
// This needs to be used along with a Renderer (e.g. OpenGL3, Vulkan, WebGPU..)
// (Info: GLFW is a cross-platform general purpose library for handling windows, inputs, OpenGL/Vulkan graphics context creation, etc.)
// (Requires: GLFW 3.0+. Prefer GLFW 3.3+/3.4+ for full feature support.)

// Implemented features:
//  [X] Platform: Clipboard support.
//  [X] Platform: Mouse support. Can discriminate Mouse/TouchScreen/Pen (Windows only).
//  [X] Platform: Keyboard support. Since 1.87 we are using the io.AddKeyEvent() function. Pass ImGuiKey values to all key functions e.g. ImGui::IsKeyPressed(ImGuiKey_Space). [Legacy GLFW_KEY_* values are obsolete since 1.87 and not supported since 1.91.5]
//  [X] Platform: Gamepad support. Enable with 'io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad'.
//  [X] Platform: Mouse cursor shape and visibility (ImGuiBackendFlags_HasMouseCursors) with GLFW 3.1+. Resizing cursors requires GLFW 3.4+! Disable with 'io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange'.
//  [X] Platform: Multi-viewport support (multiple windows). Enable with 'io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable'.
//  [X] Multiple Dear ImGui contexts support.
// Missing features or Issues:
//  [ ] Platform: Touch events are only correctly identified as Touch on Windows. This create issues with some interactions. GLFW doesn't provide a way to identify touch inputs from mouse inputs, we cannot call io.AddMouseSourceEvent() to identify the source. We provide a Windows-specific workaround.
//  [ ] Platform: Missing ImGuiMouseCursor_Wait and ImGuiMouseCursor_Progress cursors.
//  [ ] Platform: Multi-viewport: Missing ImGuiBackendFlags_HasParentViewport support. The viewport->ParentViewportID field is ignored, and therefore io.ConfigViewportsNoDefaultParent has no effect either.

// You can use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

#pragma once
#include "imgui.h"      // IMGUI_IMPL_API
#ifndef IMGUI_DISABLE

struct GLFWwindow;
struct GLFWmonitor;

// Follow "Getting Started" link and check examples/ folder to learn about using backends!
IMGUI_IMPL_API bool     ImGui_ImplGlfw_InitForOpenGL(GLFWwindow* window, bool install_callbacks);
IMGUI_IMPL_API bool     ImGui_ImplGlfw_InitForVulkan(GLFWwindow* window, bool install_callbacks);
IMGUI_IMPL_API bool     ImGui_ImplGlfw_InitForOther(GLFWwindow* window, bool install_callbacks);
IMGUI_IMPL_API void     ImGui_ImplGlfw_Shutdown();
IMGUI_IMPL_API void     ImGui_ImplGlfw_NewFrame();

// Emscripten related initialization phase methods (call after ImGui_ImplGlfw_InitForOpenGL)
#ifdef __EMSCRIPTEN__
IMGUI_IMPL_API void     ImGui_ImplGlfw_InstallEmscriptenCallbacks(GLFWwindow* window, const char* canvas_selector);
//static inline void    ImGui_ImplGlfw_InstallEmscriptenCanvasResizeCallback(const char* canvas_selector) { ImGui_ImplGlfw_InstallEmscriptenCallbacks(nullptr, canvas_selector); } } // Renamed in 1.91.0
#endif

// GLFW callbacks install
// - When calling Init with 'install_callbacks=true': ImGui_ImplGlfw_InstallCallbacks() is called. GLFW callbacks will be installed for you. They will chain-call user's previously installed callbacks, if any.
// - When calling Init with 'install_callbacks=false': GLFW callbacks won't be installed. You will need to call individual function yourself from your own GLFW callbacks.
IMGUI_IMPL_API void     ImGui_ImplGlfw_InstallCallbacks(GLFWwindow* window);
IMGUI_IMPL_API void     ImGui_ImplGlfw_RestoreCallbacks(GLFWwindow* window);

// GLFW callbacks options:
// - Set 'chain_for_all_windows=true' to enable chaining callbacks for all windows (including secondary viewports created by backends or by user)
IMGUI_IMPL_API void     ImGui_ImplGlfw_SetCallbacksChainForAllWindows(bool chain_for_all_windows);

// GLFW callbacks (individual callbacks to call yourself if you didn't install callbacks)
IMGUI_IMPL_API void     ImGui_ImplGlfw_WindowFocusCallback(GLFWwindow* window, int focused);        // Since 1.84
IMGUI_IMPL_API void     ImGui_ImplGlfw_CursorEnterCallback(GLFWwindow* window, int entered);        // Since 1.84
IMGUI_IMPL_API void     ImGui_ImplGlfw_CursorPosCallback(GLFWwindow* window, double x, double y);   // Since 1.87
IMGUI_IMPL_API void     ImGui_ImplGlfw_MouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
IMGUI_IMPL_API void     ImGui_ImplGlfw_ScrollCallback(GLFWwindow* window, double xoffset, double yoffset);
IMGUI_IMPL_API void     ImGui_ImplGlfw_KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
IMGUI_IMPL_API void     ImGui_ImplGlfw_CharCallback(GLFWwindow* window, unsigned int c);
IMGUI_IMPL_API void     ImGui_ImplGlfw_MonitorCallback(GLFWmonitor* monitor, int event);

// --- FoxSDR ADDITION: A SECOND OPENGL CONTEXT THE DRIVER WOULD NOT GIVE -----
//
// Upstream's ImGui_ImplGlfw_CreateWindow calls glfwCreateWindow and then uses
// the result unconditionally. On a machine whose driver refuses to share a
// second GL context that result is nullptr, GLFW logs "65543: WGL: Failed to
// create OpenGL context", and the very next line - glfwGetWin32Window(nullptr)
// - takes the access violation FoxSDR received as
// "crash cascade.exe @ glfwGetWin32Window" (0.95.0, Windows 10.0.22621).
//
// The backend has no logger of its own, so it counts instead. The application
// reads the counter after ImGui::UpdatePlatformWindows(), and a rise means
// "this display cannot do torn-off windows" - see AppWindow::run, which clears
// ImGuiConfigFlags_ViewportsEnable for the rest of the session and says so in
// the log, so the page is merged back into the main window rather than the
// process dying. Every other platform callback in the backend now tolerates a
// viewport whose window is null, because between the failure and the
// application noticing, ImGui will still call them on it.
IMGUI_IMPL_API int      ImGui_ImplGlfw_ViewportWindowCreationFailures();

// TESTS ONLY: make the next secondary-viewport creation fail as if the driver
// had refused it. There is no way to provoke the real refusal on a machine
// whose driver is willing, and a guard nothing has ever exercised is a guard
// nobody knows works. Resets itself after one use.
IMGUI_IMPL_API void     ImGui_ImplGlfw_FailNextViewportWindowForTest();

// GLFW helpers
IMGUI_IMPL_API void     ImGui_ImplGlfw_Sleep(int milliseconds);
IMGUI_IMPL_API float    ImGui_ImplGlfw_GetContentScaleForWindow(GLFWwindow* window);
IMGUI_IMPL_API float    ImGui_ImplGlfw_GetContentScaleForMonitor(GLFWmonitor* monitor);


#endif // #ifndef IMGUI_DISABLE
