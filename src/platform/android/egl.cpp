// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "egl.hpp"

#include <GLES3/gl3.h>
#include <android/configuration.h>
#include <android/log.h>
#include <android_native_app_glue.h>

#include <algorithm>

namespace cascade::platform::android {
namespace {

constexpr const char* kTag = "FoxSDR";

void logError(const char* what) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "EGL: %s (error 0x%04x)", what,
                        static_cast<unsigned>(eglGetError()));
}

const char* glString(GLenum name) {
    const GLubyte* s = glGetString(name);
    return s != nullptr ? reinterpret_cast<const char*>(s) : "(unknown)";
}

}  // namespace

EglContext::~EglContext() { shutdown(); }

bool EglContext::init(ANativeWindow* window) {
    if (valid()) { return true; }
    if (window == nullptr) { return false; }

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
    // EGL_RENDERABLE_TYPE = EGL_OPENGL_ES3_BIT is the one that matters: without
    // it eglChooseConfig defaults to ES1 and the ES3 context created below can
    // legally be refused. It is also what makes the "#version 300 es" shaders
    // imgui_impl_opengl3 emits in this build a valid thing to ask the config
    // for.
    //
    // 8/8/8 with no alpha: the surface is opaque and full-screen, so an alpha
    // channel would cost bandwidth to blend against a window nobody can see.
    //
    // NO DEPTH AND NO STENCIL, deliberately. Dear ImGui draws 2D triangles in
    // painter's order and disables depth testing itself; asking for a 24-bit
    // depth buffer - as the upstream Android example does - allocates a
    // full-screen buffer per frame for nothing, which on a 1080x2400 phone is
    // about 7 MB of bandwidth a frame. Asking for 0 does not forbid a config
    // that has one; eglChooseConfig sorts smallest-first on these, so this
    // simply stops preferring one.
    const EGLint configAttribs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                                    EGL_RED_SIZE,        8,
                                    EGL_GREEN_SIZE,      8,
                                    EGL_BLUE_SIZE,       8,
                                    EGL_DEPTH_SIZE,      0,
                                    EGL_STENCIL_SIZE,    0,
                                    EGL_NONE};

    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    if (eglChooseConfig(display_, configAttribs, &config, 1, &numConfigs) != EGL_TRUE ||
        numConfigs == 0) {
        logError("eglChooseConfig found no ES3 window config");
        shutdown();
        return false;
    }

    // Hand the window the pixel format EGL picked. Skipping this is the classic
    // cause of a correct-looking render that the compositor shows as garbage,
    // because the buffer's format and the config's disagree.
    EGLint nativeVisual = 0;
    eglGetConfigAttrib(display_, config, EGL_NATIVE_VISUAL_ID, &nativeVisual);
    ANativeWindow_setBuffersGeometry(window, 0, 0, nativeVisual);

    const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttribs);
    if (context_ == EGL_NO_CONTEXT) {
        logError("eglCreateContext failed for ES3");
        shutdown();
        return false;
    }

    surface_ = eglCreateWindowSurface(display_, config, window, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        logError("eglCreateWindowSurface failed");
        shutdown();
        return false;
    }
    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
        logError("eglMakeCurrent failed");
        shutdown();
        return false;
    }

    // Pace to the display. A phone that renders faster than it can present is
    // burning battery to produce frames the compositor throws away, and this
    // application has no reason to run ahead of the panel.
    eglSwapInterval(display_, 1);

    window_ = window;
    glVersion_ = glString(GL_VERSION);
    glRenderer_ = glString(GL_RENDERER);
    refreshSize();
    __android_log_print(ANDROID_LOG_INFO, kTag, "EGL up: %dx%d, %s, %s", width_, height_,
                        glVersion_, glRenderer_);
    return true;
}

void EglContext::shutdown() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) { eglDestroySurface(display_, surface_); }
        if (context_ != EGL_NO_CONTEXT) { eglDestroyContext(display_, context_); }
        eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
    window_ = nullptr;
    width_ = 0;
    height_ = 0;
    glVersion_ = "(unknown)";
    glRenderer_ = "(unknown)";
}

void EglContext::refreshSize() {
    if (!valid()) { return; }
    EGLint w = 0;
    EGLint h = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &w);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &h);
    width_ = static_cast<int>(w);
    height_ = static_cast<int>(h);
}

void EglContext::swap() {
    if (!valid()) { return; }
    if (eglSwapBuffers(display_, surface_) != EGL_TRUE) {
        // EGL_BAD_SURFACE here means the framework pulled the window out from
        // under us between the draw and the present, which is a normal thing to
        // happen on a task switch. The command queue will deliver
        // APP_CMD_TERM_WINDOW; there is nothing to do but not draw again.
        logError("eglSwapBuffers failed");
    }
}

int displayDensityDpi(android_app* app) {
    constexpr int kBaselineDpi = 160;
    if (app == nullptr || app->config == nullptr) { return kBaselineDpi; }
    const int32_t density = AConfiguration_getDensity(app->config);
    if (density == ACONFIGURATION_DENSITY_DEFAULT || density == ACONFIGURATION_DENSITY_ANY ||
        density == ACONFIGURATION_DENSITY_NONE || density <= 0) {
        return kBaselineDpi;
    }
    return static_cast<int>(density);
}

float displayDensityScale(android_app* app) {
    const float scale = static_cast<float>(displayDensityDpi(app)) / 160.0f;
    return std::clamp(scale, 1.0f, 4.0f);
}

}  // namespace cascade::platform::android
