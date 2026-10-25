// software_gl.hpp - Mesa's llvmpipe, loaded in place of the graphics driver's OpenGL (0.99.90).
//
// WHAT IT IS. The Windows package carries two files of Mesa 26.2.4 (pal1000/mesa-dist-win, pinned
// in third_party/softgl/PINNED.txt) in a folder of their own, softgl\ beside cascade.exe:
// opengl32.dll, Mesa's WGL front end, and libgallium_wgl.dll, the renderer it imports. Together
// they are a complete OpenGL 4.6 drawn by the processor. gui/window_platform.hpp says WHEN it is
// used; this says HOW.
//
// HOW THE SWAP WORKS. cascade.exe delay-loads opengl32.dll (/DELAYLOAD in CMakeLists.txt), GLFW
// loads it by name when it first makes a context, and Dear ImGui's OpenGL loader does the same.
// Windows answers a load by NAME from the list of modules already in the process before it looks
// in any folder ("Dynamic-link library search order", Microsoft Learn: the loaded-module list), so
// once Mesa's opengl32.dll is loaded from its full path, every later request for "opengl32.dll" in
// this process gets Mesa's - GDI's own SwapBuffers and SetPixelFormat included. Loading it from a
// folder of its own, never from beside cascade.exe, is the point: next to the executable it would
// replace the graphics driver for everyone.
//
// ONLY BEFORE THE SYSTEM'S IS LOADED. That is why load() runs at the top of AppWindow::run, before
// GLFW asks for anything, and only when FOXSDR_GL=software says so. A process that has already
// asked the graphics driver for a context keeps the system's opengl32.dll for good (measured on an
// NVIDIA desktop: still loaded after glfwTerminate), so the fallback after a refused window is a
// second process: relaunchAndWait() starts this executable again with FOXSDR_GL=software and the
// same command line, after the first one's application object - its plugins, its sound device,
// its listening ports - is gone, and the first one waits for it and ends with its exit code. A
// load() that finds another opengl32.dll already loaded says so and changes nothing.
//
// GALLIUM_DRIVER is set to llvmpipe first, unless the environment already names a driver: Mesa's
// default on a machine with a Direct3D 12 driver is its D3D12 back end, which needs a dxil.dll
// that is not shipped and is not what was measured. Mesa reads the variable when it loads, from
// the process environment, so it is set with SetEnvironmentVariable before the load.
//
// Off Windows there is nothing to load: present() is false, load() says Missing and there is no
// relaunch.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_SOFTWARE_GL_HPP
#define CASCADE_GUI_SOFTWARE_GL_HPP

#include <string>

namespace cascade::gui::softgl {

// Both files are in softgl\ beside the running executable.
bool present();

enum class LoadStatus {
    Loaded,             // Mesa's opengl32.dll is this process's opengl32.dll from now on
    Missing,            // the files are not there (or this is not Windows)
    SystemStillLoaded,  // another opengl32.dll is already loaded; nothing was changed
    Failed              // LoadLibrary refused one of the files; `error` says why
};

struct LoadResult {
    LoadStatus status = LoadStatus::Missing;
    unsigned long error = 0;    // GetLastError() when Failed
    std::string galliumDriver;  // GALLIUM_DRIVER as Mesa will read it, when Loaded
};

// Idempotent: once Loaded, every later call answers Loaded.
LoadResult load();

// This process draws with the software renderer (load() answered Loaded).
bool loaded();

// THE SECOND PROCESS. The variable that marks it, holding the first one's process id: the first
// one keeps the primary-instance claim (core/tester_link.hpp) while it waits, so the second is
// the primary instance in all but name.
inline constexpr const char* kRelaunchEnv = "FOXSDR_SOFTGL_RELAUNCH";

struct RelaunchResult {
    bool started = false;
    unsigned long error = 0;     // GetLastError() when not started
    unsigned long exitCode = 1;  // the second process's, when started
};

// Starts this executable again with FOXSDR_GL=software and kRelaunchEnv set, the same command line
// and the same redirected standard handles (a console is shared anyway), and waits for it to end.
// Windows only; elsewhere it starts nothing.
RelaunchResult relaunchAndWait();

// When relaunchAndWait could not start the second process: the log line, and the person told as a
// refused window tells them (gui/window_platform.hpp, failureMessage; no dialog in a bounded run).
void tellUserRelaunchFailed(unsigned long error, int frames);

// In the second process, once, at the top of main(): whether this process is one, and the first
// one's id in `parent`. The variable is cleared, so that nothing this process starts inherits it.
bool takeRelaunchMarker(std::string& parent);

}  // namespace cascade::gui::softgl

#endif  // CASCADE_GUI_SOFTWARE_GL_HPP
