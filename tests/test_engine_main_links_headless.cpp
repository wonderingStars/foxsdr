// foxsdr-engine links provably headless (engine extraction stage 7b).
//
// The engine LIBRARY is held window-free three ways already: the configure-time walk
// of its link graph (CMakeLists.txt, "the line between them"), the scan of every file
// under the engine's directories for a window header, and engine_link_check (a program
// that links the library alone). The new BINARY is the same claim made of one more
// program, and a program can break it in ways a library cannot: by linking something
// else (a `target_link_libraries(foxsdr-engine PRIVATE cascade_gui)` is a binary with a
// window), by compiling a source that includes one, or by being left off the walk.
//
// This test holds the binary from three sides; the walk itself is the configure step -
// a link to imgui, GLFW, OpenGL, the interface, the shell or the window fails the
// CONFIGURE with "cascade_engine links the window", which the record's break-it shows:
//
//   1. THE WALK'S START LIST names foxsdr-engine (so the walk covers it), and the
//      CMakeLists.txt text says nothing about the target that could widen what it
//      links: its sources are src/engine_main.cpp alone and the only libraries named
//      for it are cascade_engine and the delay-load helper;
//   2. ITS ONE SOURCE includes nothing of the window (no gui/, imgui, GLFW, GL) and
//      names no AppWindow;
//   3. THE BUILT IMAGE (Windows) imports none of the libraries a window needs: the PE
//      import table, static and delay-loaded, has no opengl32, gdi32 or imm32. The
//      scanner is shown to see them first: cascade.exe, built from the same tree, imports
//      all three. (A byte search for the names would be wrong in the other direction: the
//      engine library carries the NAME "opengl32.dll" in a list the hang watchdog uses to
//      classify a stack, and the word "ImGui" in an embedded page - neither is a link.)
//      Linux: the equivalent read of DT_NEEDED is not written, and says so.
//      (tests/test_engine_main_smoke.cpp lists the modules of the RUNNING process.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;

namespace {

// The text with `#` comments (CMake) or // and /* */ comments (C++) blanked, line structure kept.
std::string withoutCmakeComments(const std::string& s) {
    std::string out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        // A '#' outside a quoted string starts a comment; the CMake this scans has no '#' inside quotes
        // on the lines it reads.
        const std::size_t h = line.find('#');
        out += (h == std::string::npos ? line : line.substr(0, h)) + "\n";
    }
    return out;
}

std::string withoutCppComments(const std::string& s) {
    std::string out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 2, "//") == 0) {
            while (i < s.size() && s[i] != '\n') { ++i; }
        } else if (s.compare(i, 2, "/*") == 0) {
            const std::size_t e = s.find("*/", i + 2);
            i = (e == std::string::npos) ? s.size() : e + 2;
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}

bool windowWord(const std::string& lower) {
    for (const char* w : {"gui", "imgui", "glfw", "opengl", "cascade_shell", "cascade_interface", "backends/"}) {
        if (lower.find(w) != std::string::npos) { return true; }
    }
    return false;
}

#if defined(_WIN32)
// The DLL names a PE image imports, static and delay-loaded, lower-cased. Empty when the file is not a PE64.
std::set<std::string> peImports(const fs::path& exe) {
    std::set<std::string> names;
    const std::string raw = emt::readFile(exe);
    const auto* b = reinterpret_cast<const unsigned char*>(raw.data());
    const std::size_t n = raw.size();
    const auto u16 = [&](std::size_t o) { std::uint16_t v = 0; if (o + 2 <= n) { std::memcpy(&v, b + o, 2); } return v; };
    const auto u32 = [&](std::size_t o) { std::uint32_t v = 0; if (o + 4 <= n) { std::memcpy(&v, b + o, 4); } return v; };
    if (n < 0x40 || u16(0) != 0x5a4d) { return names; }
    const std::size_t pe = u32(0x3c);
    if (u32(pe) != 0x00004550) { return names; }
    const std::size_t sections = u16(pe + 6);
    const std::size_t optSize = u16(pe + 20);
    const std::size_t opt = pe + 24;
    if (u16(opt) != 0x20b) { return names; }  // PE32+
    const std::size_t dirs = opt + 112;
    struct Sec { std::uint32_t va, size, raw; };
    std::vector<Sec> secs;
    for (std::size_t i = 0; i < sections; ++i) {
        const std::size_t s = opt + optSize + 40 * i;
        const std::uint32_t vs = u32(s + 8), va = u32(s + 12), rs = u32(s + 16), ro = u32(s + 20);
        secs.push_back({va, vs > rs ? vs : rs, ro});
    }
    const auto off = [&](std::uint32_t rva) -> std::size_t {
        for (const Sec& s : secs) {
            if (rva >= s.va && rva < s.va + s.size) { return rva - s.va + s.raw; }
        }
        return 0;
    };
    const auto cstr = [&](std::size_t o) {
        std::string r;
        while (o < n && b[o] != 0) { r.push_back(static_cast<char>(b[o++])); }
        return emt::lowered(r);
    };
    // Import directory (entry 1): 20-byte descriptors, the name's RVA at +12, ended by a zero name.
    if (const std::uint32_t rva = u32(dirs + 8 * 1); rva != 0) {
        for (std::size_t o = off(rva); o != 0 && u32(o + 12) != 0; o += 20) { names.insert(cstr(off(u32(o + 12)))); }
    }
    // Delay-load directory (entry 13): 32-byte descriptors, the name's RVA at +4.
    if (const std::uint32_t rva = u32(dirs + 8 * 13); rva != 0) {
        for (std::size_t o = off(rva); o != 0 && u32(o + 4) != 0; o += 32) { names.insert(cstr(off(u32(o + 4)))); }
    }
    return names;
}
#endif

}  // namespace

int main() {
    std::printf("test_engine_main_links_headless\n");
    const fs::path src = fs::path(CASCADE_SOURCE_DIR);

    // --- 1. the walk's start list and what CMake says of the target ----------------------------------------
    const std::string cmakeRaw = emt::readFile(src / "CMakeLists.txt");
    CHECK(!cmakeRaw.empty());
    const std::string cmake = withoutCmakeComments(cmakeRaw);
    CHECK(emt::contains(cmake, "set(_todo cascade_engine cascade_engine_link_check foxsdr-reports foxsdr-engine)"));
    CHECK(emt::contains(cmake, "NOT _t STREQUAL \"foxsdr-engine\""));
    // The one executable, from the one source.
    CHECK(emt::contains(cmake, "add_executable(foxsdr-engine src/engine_main.cpp)"));
    {
        // Every statement that names the target: nothing that widens what it links or compiles, and the
        // libraries named for it are the engine and the delay-load helper only.
        const std::regex stmt(R"(([A-Za-z_]+)\(\s*foxsdr-engine\b([^)]*)\))");
        int statements = 0;
        for (std::sregex_iterator it(cmake.begin(), cmake.end(), stmt), end; it != end; ++it) {
            ++statements;
            const std::string fn = emt::lowered((*it)[1].str());
            const std::string args = emt::lowered((*it)[2].str());
            std::printf("  CMake: %s(foxsdr-engine%s)\n", fn.c_str(), args.c_str());
            if (fn == "target_link_libraries") {
                std::istringstream words(args);
                std::string w;
                while (words >> w) {
                    const bool allowed = w == "private" || w == "cascade_engine" || w == "delayimp";
                    if (!allowed) { std::printf("    foxsdr-engine links '%s'\n", w.c_str()); }
                    CHECK(allowed);
                }
            } else if (fn == "add_dependencies") {
                // STAGE 7g: ordering only, and only on the one target that stages the executables' resources
                // (cascade_stage_resources, copy_directory of resources/bandplans and resources/firmware): it
                // links nothing and compiles nothing, but it is a statement about the target, so what it may
                // name is pinned here, not left to the general rule below.
                std::istringstream words(args);
                std::string w;
                while (words >> w) {
                    const bool allowed = w == "cascade_stage_resources";
                    if (!allowed) { std::printf("    foxsdr-engine now depends on '%s'\n", w.c_str()); }
                    CHECK(allowed);
                }
                CHECK(!windowWord(args));
            } else if (fn != "add_executable" && fn != "target_link_options" &&
                       fn != "cascade_sanitize_stage_runtime") {
                // target_sources / target_include_directories / target_compile_definitions and the rest: not for it.
                std::printf("    unexpected: %s\n", fn.c_str());
                CHECK(false);
            }
            if (fn == "target_link_options") { CHECK(!windowWord(args)); }
        }
        CHECK(statements >= 3);  // add_executable, target_link_libraries, and the delay-load options
    }

    // --- 2. its one source ---------------------------------------------------------------------------------
    const std::string mainSrc = withoutCppComments(emt::readFile(src / "src" / "engine_main.cpp"));
    CHECK(!mainSrc.empty());
    {
        const std::regex inc(R"re(#\s*include\s*[<"]([^>"]+)[>"])re");
        int includes = 0;
        for (std::sregex_iterator it(mainSrc.begin(), mainSrc.end(), inc), end; it != end; ++it) {
            ++includes;
            const std::string h = emt::lowered((*it)[1].str());
            if (windowWord(h)) { std::printf("  engine_main.cpp includes %s\n", h.c_str()); }
            CHECK(!windowWord(h));
        }
        CHECK(includes >= 6);
        CHECK(!emt::contains(mainSrc, "AppWindow"));
        CHECK(!emt::contains(mainSrc, "EngineShell"));
    }

    // --- 3. the built image ---------------------------------------------------------------------------------
#if defined(_WIN32)
    {
        const std::set<std::string> engine = peImports(emt::appExe("foxsdr-engine"));
        const std::set<std::string> cascade = peImports(emt::appExe("cascade"));
        CHECK(!engine.empty());
        CHECK(engine.count("kernel32.dll") == 1);  // the reader really read the engine's table
        // THE CONTROL: the scanner sees what a window needs, in the image that has a window.
        for (const char* lib : {"opengl32.dll", "gdi32.dll", "imm32.dll"}) {
            CHECK(cascade.count(lib) == 1);
            if (engine.count(lib) != 0) { std::printf("  foxsdr-engine imports %s\n", lib); }
            CHECK(engine.count(lib) == 0);
        }
        for (const std::string& dll : engine) {
            CHECK(dll.find("glfw") == std::string::npos);
            CHECK(dll.find("imgui") == std::string::npos);
        }
        CHECK(engine.count("soapysdr.dll") == 1);  // delay-loaded, like cascade's: a missing runtime is not a loader modal
    }
#else
    SKIP_LINUX("the engine's DT_NEEDED list is not read; the loaded-module check in test_engine_main_smoke holds");
#endif

    return testSummary("test_engine_main_links_headless");
}
