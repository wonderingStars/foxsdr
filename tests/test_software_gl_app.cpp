// THE SOFTWARE RENDERER STANDING IN FOR THE GRAPHICS DRIVER, through the REAL cascade.exe (0.99.90).
//
// The field log (0.99.89, Windows): "WGL: The driver does not appear to support OpenGL", then
// glfwCreateWindow failed and FoxSDR ended with a message box. This build stages Mesa's llvmpipe
// in softgl\ beside cascade.exe (CMakeLists.txt, CASCADE_SOFTGL), and a run whose graphics driver
// cannot be used hands itself to a second process on it (gui/software_gl.hpp). These are the runs
// that end in a WINDOW, on a desktop whose driver works, reached with the real seams:
//
//   hardware    FOXSDR_TEST_WINDOW_FAIL=hardware: the window is refused while the graphics driver
//               is in use (GLFW's real error), the run hands over, the second process loads the
//               software renderer before GLFW, gets its window on llvmpipe, draws its frames and
//               ends 0 - and the first ends with that 0. The second process's standard output
//               reaches the file the first one was given (the hand-over passes it on).
//   old-driver  FOXSDR_TEST_WINDOW_FAIL=old-driver: the graphics driver's context is taken to say
//               OpenGL 2.1, older than the 3.0 the ImGui backend's shaders need; it is let go of
//               before anything is built on it, and the run goes the same way.
//   software    FOXSDR_GL=software: llvmpipe from the start, in one process.
//   driver      nothing set: the graphics driver is asked first, and on a machine whose driver
//               works nothing about the software renderer happens at all.
//
// Every run points FOXSDR_DIAG_DIR, CASCADE_CONFIG_TEST, APPDATA and LOCALAPPDATA at a scratch
// tree: the user's configuration, log and plugins are never touched. Each is a bounded
// `--frames 3` run on the signal generator with nothing uploaded. Each shows a window for a moment,
// which is why this is a gui-labelled test.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::size_t count(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) { ++n; }
    return n;
}

// Every `needle` is in `hay`, each after the one before it.
bool inOrder(const std::string& hay, const std::vector<std::string>& needles) {
    std::size_t from = 0;
    for (const std::string& n : needles) {
        const std::size_t at = hay.find(n, from);
        if (at == std::string::npos) {
            std::printf("  missing, or out of order: \"%s\"\n", n.c_str());
            return false;
        }
        from = at + n.size();
    }
    return true;
}

const char* const kRunVars[] = {"FOXSDR_DIAG_DIR", "CASCADE_CONFIG_TEST", "APPDATA",  "LOCALAPPDATA",
                                "FOXSDR_TEST_WINDOW_FAIL", "FOXSDR_GL",     "GALLIUM_DRIVER", "FOXSDR_CRASH_URL"};

struct Run {
    bool started = false;
    DWORD code = 0xDEAD0001ul;
    std::string log;  // the diagnostics log both processes wrote
    std::string out;  // standard output and error, as the process that was started was given them
};

Run runApp(const char* tag, const std::vector<std::pair<const char*, const char*>>& env) {
    Run r;
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    const fs::path dir = base / (std::string("cascade-softgl-") + tag + "-" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "appdata", ec);
    fs::create_directories(dir / "localappdata", ec);
    {
        std::ofstream cfg(dir / "config.json", std::ios::binary | std::ios::trunc);
        cfg << "{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"sourceKind\": \"siggen\", "
               "\"volume\": 0.0, \"uiTheme\": \"today\", \"interfaceScale\": \"100\" }\n";
    }
    for (const char* v : kRunVars) { ::SetEnvironmentVariableA(v, nullptr); }
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", dir.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", (dir / "config.json").string().c_str());
    ::SetEnvironmentVariableA("APPDATA", (dir / "appdata").string().c_str());
    ::SetEnvironmentVariableA("LOCALAPPDATA", (dir / "localappdata").string().c_str());
    for (const auto& [name, value] : env) { ::SetEnvironmentVariableA(name, value); }

    // A FILE for standard output and error, not NUL: what the second process prints must arrive in
    // it, which is the hand-over passing on its redirected handles.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const fs::path outPath = dir / "out.txt";
    HANDLE out = ::CreateFileW(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "\\cascade.exe";
    std::string cmd = "\"" + exe + "\" --frames 3";
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi{};
    const BOOL started =
        ::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    for (const char* v : kRunVars) { ::SetEnvironmentVariableA(v, nullptr); }
    if (out != INVALID_HANDLE_VALUE) { ::CloseHandle(out); }
    if (nul != INVALID_HANDLE_VALUE) { ::CloseHandle(nul); }
    r.started = started != 0;
    if (!r.started) { return r; }
    ::CloseHandle(pi.hThread);
    if (::WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) { ::GetExitCodeProcess(pi.hProcess, &r.code); }
    ::CloseHandle(pi.hProcess);
    r.log = readFile(dir / "logs" / "foxsdr.log");
    r.out = readFile(outPath);
    std::printf("%s: exit %lu, %zu bytes of log\n", tag, static_cast<unsigned long>(r.code), r.log.size());
    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    return r;
}

const char* const kLlvmpipe = " window: GL vendor Mesa; renderer llvmpipe (";

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    const fs::path softgl = fs::path(CASCADE_APP_BINDIR) / "softgl";
    CHECK(fs::exists(softgl / "opengl32.dll") && fs::exists(softgl / "libgallium_wgl.dll"));

    // hardware: refused on the driver, handed over, drawn on llvmpipe, 0 passed back.
    {
        const Run r = runApp("hardware", {{"FOXSDR_TEST_WINDOW_FAIL", "hardware"}});
        CHECK(r.started && r.code == 0);
        CHECK(inOrder(r.log, {" window: GLFW initialised; platform chosen: win32",
                              " window: glfwCreateWindow failed",
                              "(GLFW error 65543); starting again with the software renderer in softgl",
                              " window: handing over to a second FoxSDR process with the software renderer",
                              " window: started again by process ",
                              " window: FOXSDR_GL=software; the software renderer is used from the start",
                              " window: software renderer loaded from softgl (GALLIUM_DRIVER=llvmpipe)",
                              " window: GLFW initialised; platform chosen: win32",
                              " window: window created",
                              kLlvmpipe,
                              " window: window shown; the Dear ImGui backends are up",
                              " window: the second FoxSDR process ended with exit code 0"}));
        CHECK(count(r.log, " window: handing over") == 1);
        CHECK(!contains(r.log, "error dialog"));
        // the second process's own output, through the handle the first was given
        CHECK(contains(r.out, "cascade: rendered 3 frames"));
    }

    // old-driver: a context that works but is too old is let go of, and the run goes the same way.
    {
        const Run r = runApp("old-driver", {{"FOXSDR_TEST_WINDOW_FAIL", "old-driver"}});
        CHECK(r.started && r.code == 0);
        CHECK(inOrder(r.log, {" window: window created",
                              " window: GL vendor ",
                              " window: the graphics driver offers OpenGL 2.1; FoxSDR needs 3.0",
                              "; starting again with the software renderer in softgl",
                              " window: handing over to a second FoxSDR process",
                              " window: software renderer loaded from softgl",
                              kLlvmpipe,
                              " window: window shown",
                              " window: the second FoxSDR process ended with exit code 0"}));
        // the first process never showed its window, and nothing was built on the old context
        CHECK(count(r.log, " window: window shown") == 1);
        CHECK(count(r.log, "fonts: ") == 1);
        CHECK(contains(r.out, "cascade: rendered 3 frames"));
    }

    // software: llvmpipe from the start, one process.
    {
        const Run r = runApp("software", {{"FOXSDR_GL", "software"}});
        CHECK(r.started && r.code == 0);
        CHECK(inOrder(r.log, {" window: FOXSDR_GL=software; the software renderer is used from the start",
                              " window: software renderer loaded from softgl (GALLIUM_DRIVER=llvmpipe)",
                              " window: GLFW initialised", kLlvmpipe, " window: window shown"}));
        CHECK(!contains(r.log, "handing over") && !contains(r.log, "started again by process"));
        CHECK(contains(r.out, "cascade: rendered 3 frames"));
    }

    // driver: nothing set - the graphics driver is asked first. On a machine whose driver works
    // (any machine this gui test runs on) the software renderer is never touched.
    {
        const Run r = runApp("driver", {});
        CHECK(r.started && r.code == 0);
        CHECK(contains(r.log, " window: window shown"));
        CHECK(!contains(r.log, "FOXSDR_GL="));
        CHECK(!contains(r.log, "software renderer"));
        CHECK(!contains(r.log, "handing over"));
    }
#else
    std::printf("test_software_gl_app: the software renderer is part of the Windows package only\n");
#endif
    return g_checksFailed == 0 ? 0 : 1;
}
