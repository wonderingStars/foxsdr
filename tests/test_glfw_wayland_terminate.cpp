/*
 * GLFW'S WAYLAND TERMINATE MUST NOT UNLOAD A LIBRARY BEFORE THE DISPLAY IS
 * DISCONNECTED.
 *
 * THE REPORT (0.99.59, Linux, kernel 7.0; two reports, 2026-10-02 and
 * 2026-10-04, one user): SIGSEGV at the very end of a normal shutdown - after
 * "frame loop ended after 49452 frames; shutting down", after the crash upload
 * - with three libwayland-client.so.0 frames on top of _glfwTerminateWayland
 * (+1208 of 1260 bytes, i.e. its last libwayland call), then GLFW's terminate()
 * <- AppWindow::run <- main.
 *
 * WHAT IT IS. GLFW 3.4's _glfwTerminateWayland dlclose()d libEGL (inside
 * _glfwTerminateEGL, its very first call), libdecor, libwayland-egl, libxkbcommon
 * and libwayland-cursor BEFORE it called wl_display_disconnect(). Events that a
 * driver's own protocol code asked libwayland to queue on the display
 * (eglSwapBuffers reads the socket and files them) are still queued when the
 * windows are gone, and each queued event holds a pointer to its
 * wl_message - static data inside the module that was just unmapped.
 * wl_display_disconnect() -> wl_event_queue_release() ->
 * destroy_queued_closure() reads closure->message->signature for every one of
 * them: three libwayland frames, the last one dereferencing freed address
 * space. That is exactly the shape above, and the same stack (down to the
 * statement) is glfw/glfw issue #2744, seen on NVIDIA drivers across KDE and
 * GNOME, reduced there to a ten-line program with no SDR in it. Fixed upstream
 * after 3.4 by commit 162896e5b9 ("Wayland: free modules at end of terminate
 * function"); third_party/glfw/FOXSDR-PATCHES.md applies it here.
 *
 * WHAT THIS TEST CAN AND CANNOT PROVE. The fault is in code that only exists on
 * a Wayland desktop, and there is no Wayland desktop on the machines the suite
 * runs on, so no test can reproduce the crash itself. What is testable
 * everywhere is the ORDER the fix establishes, and that is read straight off
 * the vendored sources: every dlclose in _glfwTerminateWayland comes after
 * wl_display_disconnect, none before it, and _glfwTerminateEGL does not unload
 * libEGL on Wayland (it is unloaded at the end of the Wayland terminate
 * instead). A source-order test is a statement about the text of the patch, not
 * about the driver; it stops the vendored copy being re-synced to a plain 3.4
 * (or an upgrade that drops the patch) without anyone noticing.
 *
 * THE BREAK-IT CHECK: restoring the 3.4 text of either file (the module frees
 * back above the disconnect, or the unconditional free in _glfwTerminateEGL)
 * turns the matching block below red - recorded in the report that added this.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The text of one function with every comment removed, so that a mention of a
// call in a comment (the patch's own fence explains the order in words that
// name these calls) can never satisfy or break a check. Line comments only:
// these two GLFW files use nothing else inside the functions in question.
// Empty when the function is not found, which every caller turns into a
// failure rather than a pass.
std::string liveBody(const std::string& text, const std::string& signature) {
    const std::size_t sig = text.find(signature);
    if (sig == std::string::npos) { return {}; }
    const std::size_t open = text.find('{', sig);
    if (open == std::string::npos) { return {}; }
    int depth = 0;
    std::size_t end = std::string::npos;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') { ++depth; }
        if (text[i] == '}' && --depth == 0) { end = i; break; }
    }
    if (end == std::string::npos) { return {}; }

    std::string out;
    std::size_t pos = open;
    while (pos < end) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string::npos || eol > end) { eol = end; }
        std::string line = text.substr(pos, eol - pos);
        const std::size_t c = line.find("//");
        if (c != std::string::npos) { line.resize(c); }
        out += line;
        out += '\n';
        pos = eol + 1;
    }
    return out;
}

std::vector<std::size_t> allOf(const std::string& hay, const std::string& needle) {
    std::vector<std::size_t> at;
    for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) {
        at.push_back(p);
    }
    return at;
}

}  // namespace

int main() {
    const fs::path glfwSrc = fs::path(CASCADE_SOURCE_DIR) / "third_party" / "glfw" / "src";
    const std::string wlInit = readFile(glfwSrc / "wl_init.c");
    const std::string eglCtx = readFile(glfwSrc / "egl_context.c");
    CHECK(!wlInit.empty());
    CHECK(!eglCtx.empty());

    // ---- _glfwTerminateWayland: every module unload follows the disconnect ----
    const std::string term = liveBody(wlInit, "void _glfwTerminateWayland(void)");
    CHECK(!term.empty());

    const std::vector<std::size_t> disconnect = allOf(term, "wl_display_disconnect(");
    CHECK(disconnect.size() == 1);
    const std::size_t disconnectAt = disconnect.empty() ? std::string::npos : disconnect[0];

    const std::vector<std::size_t> frees = allOf(term, "_glfwPlatformFreeModule(");
    // libEGL (the one _glfwTerminateEGL leaves for here), libdecor, libwayland-egl,
    // libxkbcommon, libwayland-cursor: five, and no more - a sixth would be a
    // module this test has not been told about.
    CHECK(frees.size() == 5);

    std::size_t before = 0;
    for (std::size_t f : frees) {
        if (disconnectAt == std::string::npos || f < disconnectAt) { ++before; }
    }
    // THE PROPERTY. Nothing is unmapped while wl_display_disconnect can still
    // read a queued event's message out of it.
    CHECK(before == 0);

    // Each of the five modules is released, and by the handle that owns it.
    const char* const handles[] = {
        "_glfwPlatformFreeModule(_glfw.egl.handle)",
        "_glfwPlatformFreeModule(_glfw.wl.libdecor.handle)",
        "_glfwPlatformFreeModule(_glfw.wl.egl.handle)",
        "_glfwPlatformFreeModule(_glfw.wl.xkb.handle)",
        "_glfwPlatformFreeModule(_glfw.wl.cursor.handle)",
    };
    for (const char* h : handles) {
        const std::vector<std::size_t> at = allOf(term, h);
        CHECK(at.size() == 1);
        CHECK(!at.empty() && disconnectAt != std::string::npos && at[0] > disconnectAt);
    }

    // The disconnect itself is still the display's last libwayland call before
    // the module unloads - nothing was moved past it that it depends on.
    const std::size_t flushAt = term.find("wl_display_flush(");
    CHECK(flushAt != std::string::npos && disconnectAt != std::string::npos &&
          flushAt < disconnectAt);

    // ---- _glfwTerminateEGL: on Wayland it must NOT unload libEGL itself ----
    const std::string eglTerm = liveBody(eglCtx, "void _glfwTerminateEGL(void)");
    CHECK(!eglTerm.empty());
    const std::size_t eglFree = eglTerm.find("_glfwPlatformFreeModule(_glfw.egl.handle)");
    CHECK(eglFree != std::string::npos);
    if (eglFree != std::string::npos) {
        // The condition that guards the unload is the last "if (" before it.
        const std::size_t cond = eglTerm.rfind("if (", eglFree);
        CHECK(cond != std::string::npos);
        const std::string guard =
            (cond == std::string::npos) ? std::string() : eglTerm.substr(cond, eglFree - cond);
        CHECK(guard.find("_glfw.platform.platformID != GLFW_PLATFORM_WAYLAND") !=
              std::string::npos);
    }
    // The terminate still tears the EGL display down first: the library stays
    // mapped, the display is not leaked.
    CHECK(eglTerm.find("eglTerminate(") != std::string::npos);

    // ---- the patch is recorded where the next person to touch GLFW will look ----
    CHECK(wlInit.find("FOXSDR PATCH (wayland-terminate-order)") != std::string::npos);
    CHECK(eglCtx.find("FOXSDR PATCH (wayland-terminate-order)") != std::string::npos);

    return testSummary("test_glfw_wayland_terminate");
}
