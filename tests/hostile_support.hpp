// hostile_support.hpp - what the hostile-plugin tests share (plugin containment P2, docs/engine-stage4.md 6.29): the fixture's file names, the
// environment that selects its misbehaviour, its two exports, a scratch folder of copies, and a helper that survives a fault in the HOST so a
// test can say "the host did not die" as a check instead of as a crashed test.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_TESTS_HOSTILE_SUPPORT_HPP
#define CASCADE_TESTS_HOSTILE_SUPPORT_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>

#include "core/plugin_host.hpp"
#include "fault_probe_names.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace hostile {

namespace fs = std::filesystem;

// The fixture reads these at each call.
inline void setMode(const char* modes) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA("HOSTILE_PLUGIN", modes);
#else
    if (modes == nullptr) { ::unsetenv("HOSTILE_PLUGIN"); } else { ::setenv("HOSTILE_PLUGIN", modes, 1); }
#endif
}
inline void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
#else
    if (value == nullptr) { ::unsetenv(name); } else { ::setenv(name, value, 1); }
#endif
}

// The file name a copy of the fixture is given: its id is the part before "-<version>", so "hp-a" is the plugin `hp-a`.
inline std::string leafOf(const std::string& id) { return id + CASCADE_PROBE_SUFFIX; }

inline std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Lines of `text` equal to `line`.
inline std::size_t countLines(const std::string& text, const std::string& line) {
    std::size_t n = 0;
    std::size_t at = 0;
    while ((at = text.find(line + "\n", at)) != std::string::npos) {
        if (at == 0 || text[at - 1] == '\n') { ++n; }
        at += line.size();
    }
    return n;
}

inline const cascade::core::LoadedPlugin* find(const cascade::core::PluginHost& host, const std::string& id) {
    for (const cascade::core::LoadedPlugin& p : host.plugins()) {
        if (cascade::core::pluginKey(p) == leafOf(id)) { return &p; }
    }
    return nullptr;
}

// ---- the fixture's two exports -----------------------------------------------------------------------------------------------------------
inline void* symbolOf(void* module, const char* name) {
    if (module == nullptr) { return nullptr; }
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(module), name));
#else
    return ::dlsym(module, name);
#endif
}

// The host API abused on the CALLING thread (see tests/fixtures/hostile_plugin.cpp hostile_do for the ops).
inline std::int64_t doOp(const cascade::core::LoadedPlugin& lp, const char* op, std::int64_t arg = 0) {
    using Fn = std::int64_t (*)(const char*, std::int64_t);
    auto* fn = reinterpret_cast<Fn>(symbolOf(lp.nativeHandle, "hostile_do"));
    return fn != nullptr ? fn(op, arg) : -9999;
}

// How many rules the HOST broke towards the plugin (the fixture's referee); `text` receives them, one a line.
inline std::size_t violations(const cascade::core::LoadedPlugin& lp, std::string* text = nullptr) {
    using Fn = std::size_t (*)(char*, std::size_t);
    auto* fn = reinterpret_cast<Fn>(symbolOf(lp.nativeHandle, "hostile_violations"));
    if (fn == nullptr) { return static_cast<std::size_t>(-1); }
    char buf[2048] = {0};
    const std::size_t n = fn(buf, sizeof(buf));
    if (text != nullptr) { *text = buf; }
    return n;
}

// ---- surviving a fault in the HOST ----------------------------------------------------------------------------------------------------
// Runs fn(ctx). On Windows an access violation (or any structured exception) inside it is caught and reported as `false` (with the code), so
// a test whose subject is "the host reads a pointer a plugin lied about" can fail as a check instead of ending the test program. Elsewhere there
// is nothing to catch with: a fault ends the program, which is the same finding, louder.
#if defined(_WIN32)
inline bool runSeh(void (*fn)(void*), void* ctx, unsigned long* code) {
    __try {
        fn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (code != nullptr) { *code = GetExceptionCode(); }
        return false;
    }
}
#else
inline bool runSeh(void (*fn)(void*), void* ctx, unsigned long* code) {
    (void)code;
    fn(ctx);
    return true;
}
#endif

template <class F>
bool survives(F&& f, unsigned long* code = nullptr) {
    return runSeh([](void* p) { (*static_cast<std::remove_reference_t<F>*>(p))(); }, &f, code);
}

}  // namespace hostile

#endif  // CASCADE_TESTS_HOSTILE_SUPPORT_HPP
