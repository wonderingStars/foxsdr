// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "frame.hpp"

#include <GLES3/gl3.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <chrono>
#include <string>

#include "egl.hpp"
#include "gui/fonts.hpp"
#include "gui/theme.hpp"
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include "input.hpp"
#include "screen_first.hpp"

namespace cascade::platform::android {
namespace {

constexpr const char* kTag = "FoxSDR";

const char* buildAbi() {
#if defined(__aarch64__)
    return "arm64-v8a";
#elif defined(__x86_64__)
    return "x86_64";
#elif defined(__arm__)
    return "armeabi-v7a";
#elif defined(__i386__)
    return "x86";
#else
    return "(unknown ABI)";
#endif
}

const char* appVersion() {
#ifdef CASCADE_VERSION_STRING
    return CASCADE_VERSION_STRING;
#else
    return "(unversioned)";
#endif
}

// Everything the shell owns, in one place, hung off android_app::userData.
//
// NOT a file-scope singleton, deliberately. The framework can destroy and
// recreate the activity within one process (rotation without a configChanges
// declaration does exactly that), and a global would carry the previous
// activity's EGL handles into the new one. Tying the state to the android_app
// it belongs to makes that impossible to get wrong.
struct Shell {
    EglContext egl;
    TouchInput input;
    FirstScreen screen;

    bool imguiUp = false;
    bool hasFocus = false;
    float densityScale = 1.0f;
    int densityDpi = 160;
    std::string iniPath;

    std::chrono::steady_clock::time_point lastFrame{};
    float frameMs = 0.0f;

    bool readyToDraw() const { return imguiUp && hasFocus && egl.valid(); }
};

Shell* shellOf(android_app* app) {
    return app != nullptr ? static_cast<Shell*>(app->userData) : nullptr;
}

// The product's theme, sized for this screen.
//
// THE ORDER IS LOAD-BEARING AND THE RESET IS PART OF IT. ImGuiStyle::
// ScaleAllSizes multiplies the style's existing metrics in place, so calling it
// twice - which a configuration change does - would square the density. Going
// back to a default-constructed ImGuiStyle first makes every call start from
// the same place, and applyTheme() then paints the palette over it.
void applyScaledTheme(float scale) {
    ImGui::GetStyle() = ImGuiStyle();
    cascade::gui::theme::applyTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    // Fonts are scaled by the SAME factor, through the one global ImGui
    // multiplies every PushFont size by. Call sites therefore push the
    // desktop's own 17/15/16/14 and get a thumb-sized rail for free; see the
    // note in screen_first.cpp.
    style.FontScaleDpi = scale;
    style.FontSizeBase = cascade::gui::fonts::kUiSize;

    // A finger is not a mouse pointer: it is about 9 mm across and it hides
    // what it is touching. ImGui's desktop-sized hit areas are unusable with
    // one, so the frame padding is widened past what ScaleAllSizes gives and
    // the grab boxes are made bigger still.
    style.TouchExtraPadding = ImVec2(4.0f * scale, 6.0f * scale);
    style.FramePadding = ImVec2(style.FramePadding.x, style.FramePadding.y + 4.0f * scale);
    style.ScrollbarSize = 18.0f * scale;
    style.GrabMinSize = 24.0f * scale;
}

void initImGui(android_app* app, Shell& shell) {
    if (shell.imguiUp) { return; }
    if (!shell.egl.init(app->window)) { return; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // The window layout goes to the app's own private storage. Left at ImGui's
    // default it would be written to the process's working directory, which on
    // Android is "/" and is not writable - a silent failure every frame.
    if (app->activity != nullptr && app->activity->internalDataPath != nullptr) {
        shell.iniPath = std::string(app->activity->internalDataPath) + "/imgui.ini";
        io.IniFilename = shell.iniPath.c_str();
    } else {
        io.IniFilename = nullptr;
    }

    // NO MOUSE CURSOR CHANGES: there is no cursor to change, and the Android
    // backend does not implement the capability. Asking for it would have ImGui
    // request shapes nothing can supply.
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    shell.densityDpi = displayDensityDpi(app);
    shell.densityScale = displayDensityScale(app);
    applyScaledTheme(shell.densityScale);
    if (!cascade::gui::fonts::load()) {
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "font atlas refused a face - falling back to ImGui's own");
    }
    // fonts::load() sets FontSizeBase from the face it made default; the theme
    // was applied before it, so nothing here needs re-running.

    ImGui_ImplAndroid_Init(app->window);
    // "#version 300 es" is what imgui_impl_opengl3.cpp selects for itself when
    // IMGUI_IMPL_OPENGL_ES3 is defined (which src/platform/android/
    // CMakeLists.txt does, PUBLIC). Passing nullptr lets it choose, so the
    // shader version cannot drift out of step with the header it was compiled
    // against.
    ImGui_ImplOpenGL3_Init(nullptr);

    shell.input.configure(shell.densityScale);
    shell.lastFrame = std::chrono::steady_clock::now();
    shell.imguiUp = true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "shell up: %s, %d dpi (x%.2f), FoxSDR %s",
                        buildAbi(), shell.densityDpi,
                        static_cast<double>(shell.densityScale), appVersion());
}

void shutdownImGui(Shell& shell) {
    if (shell.imguiUp) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
        shell.imguiUp = false;
    }
    // THE GL CONTEXT GOES WITH IT, AND IN THIS ORDER. Every texture and buffer
    // the renderer backend owns lives in that context; destroying the context
    // first would leave the backend shutting down against a dead one.
    shell.egl.shutdown();
}

void drawFrame(Shell& shell) {
    const auto now = std::chrono::steady_clock::now();
    const float elapsedMs =
        std::chrono::duration<float, std::milli>(now - shell.lastFrame).count();
    shell.lastFrame = now;
    // A light exponential average. A raw per-frame figure on a phone jitters by
    // several milliseconds from thermal and scheduler noise and is unreadable.
    shell.frameMs = shell.frameMs > 0.0f ? shell.frameMs * 0.9f + elapsedMs * 0.1f : elapsedMs;

    shell.egl.refreshSize();

    // Long presses are a timeout, not an event; this is where one becomes a
    // right-click. It has to happen before NewFrame so ImGui sees the button in
    // the same frame it was decided.
    shell.input.newFrame();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();

    ShellStatus status;
    status.surfaceWidth = shell.egl.width();
    status.surfaceHeight = shell.egl.height();
    status.densityDpi = shell.densityDpi;
    status.densityScale = shell.densityScale;
    status.glVersion = shell.egl.glVersion();
    status.glRenderer = shell.egl.glRenderer();
    status.abi = buildAbi();
    status.frameMs = shell.frameMs;
    status.appVersion = appVersion();
    shell.screen.draw(status);

    ImGui::Render();
    glViewport(0, 0, shell.egl.width(), shell.egl.height());
    const ImVec4 clear = cascade::gui::theme::vec(cascade::gui::theme::kVoid);
    glClearColor(clear.x, clear.y, clear.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    shell.egl.swap();
}

}  // namespace

void onAppCmd(android_app* app, int32_t cmd) {
    Shell* shell = shellOf(app);
    if (shell == nullptr) { return; }

    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        initImGui(app, *shell);
        break;

    case APP_CMD_TERM_WINDOW:
        // The surface is being taken away. Everything that lives in the GL
        // context has to be gone before this returns, because the framework
        // destroys the window as soon as it does.
        shell->screen.setRunning(false);
        shutdownImGui(*shell);
        break;

    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
        shell->egl.refreshSize();
        break;

    case APP_CMD_CONFIG_CHANGED:
        // A fold, a rotation or a display swap can change the density under a
        // running activity. Re-reading it and re-applying the style is cheap
        // and is the difference between a foldable's inner screen being usable
        // and being drawn at the cover screen's scale.
        if (shell->imguiUp) {
            shell->densityDpi = displayDensityDpi(app);
            shell->densityScale = displayDensityScale(app);
            applyScaledTheme(shell->densityScale);
            shell->input.configure(shell->densityScale);
        }
        break;

    case APP_CMD_GAINED_FOCUS:
        shell->hasFocus = true;
        shell->lastFrame = std::chrono::steady_clock::now();
        shell->screen.setRunning(true);
        break;

    case APP_CMD_LOST_FOCUS:
    case APP_CMD_PAUSE:
    case APP_CMD_STOP:
        // Three commands rather than one: LOST_FOCUS is the reliable signal on
        // most devices, but a phone that goes straight to sleep can deliver
        // PAUSE/STOP without it, and an SDR left running in that case is the
        // expensive mistake this whole branch exists to avoid.
        shell->hasFocus = false;
        shell->screen.setRunning(false);
        break;

    default:
        break;
    }
}

int32_t onInputEvent(android_app* app, AInputEvent* event) {
    Shell* shell = shellOf(app);
    if (shell == nullptr || !shell->imguiUp) { return 0; }
    return shell->input.handleEvent(event);
}

void runFrameLoop(android_app* app) {
    Shell shell;
    app->userData = &shell;
    app->onAppCmd = onAppCmd;
    app->onInputEvent = onInputEvent;

    bool destroyed = false;
    while (!destroyed) {
        // -1 blocks until something happens; 0 drains and returns. Which one
        // is chosen here is the entire idle-power behaviour of the app: with a
        // window and focus we poll and draw, and without either we sleep in
        // the kernel until the framework has something to say.
        int timeoutMs = shell.readyToDraw() ? 0 : -1;
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(timeoutMs, nullptr, &events,
                                reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) { source->process(app, source); }
            if (app->destroyRequested != 0) {
                destroyed = true;
                break;
            }
            // Whatever else is already queued is taken now; a second blocking
            // wait inside this loop would stall the frame that the first event
            // may have just made drawable.
            timeoutMs = 0;
        }
        if (destroyed) { break; }
        if (shell.readyToDraw()) { drawFrame(shell); }
    }

    // APP_CMD_TERM_WINDOW usually got here first. Doing it again is harmless
    // (both teardowns are idempotent) and covers the path where the activity is
    // destroyed without ever having had a window.
    shell.screen.setRunning(false);
    shutdownImGui(shell);
    app->userData = nullptr;
    __android_log_print(ANDROID_LOG_INFO, kTag, "shell down");
}

}  // namespace cascade::platform::android
