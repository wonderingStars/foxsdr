// wire_c_portable.cpp - see wire_c_portable.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c_portable.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "core/config.hpp"
#include "core/sys_portable.hpp"
#include "engine/wire_c_sys.hpp"

namespace cascade::engine {

namespace sys = cascade::core::sys;

PortableBoot applyPortableMode(const std::filesystem::path& exeDir, const std::function<void(const std::string&, const std::string&)>& setEnv) {
    PortableBoot out;
    sys::PortablePaths p = sys::resolvePortable(exeDir);
    if (!p.markerFound) { return out; }
    if (!p.portable) {
        out.refused = true;
        out.message = "This copy of FoxSDR is set up to run from a stick, but its data folder cannot be used: " + p.error;
        return out;
    }
    std::string err;
    if (!sys::ensurePortableRoots(exeDir, p, err)) {
        out.refused = true;
        out.message = "This copy of FoxSDR is set up to run from a stick, but its data folder cannot be used: " + err;
        return out;
    }
    for (const auto& [name, value] : sys::portableEnvOverrides(p)) { setEnv(name, value); }
    out.portable = true;
    return out;
}

PortableBoot applyPortableModeToProcess() {
    const std::filesystem::path exeDir = sys::currentExeDir();
    if (exeDir.empty()) { return PortableBoot{}; }   // the program's own folder cannot be found: nothing says portable
    const PortableBoot r = applyPortableMode(exeDir, [](const std::string& name, const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name.c_str(), value.c_str());
        SetEnvironmentVariableA(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    });
    if (r.refused) {
        std::fprintf(stderr, "%s\n", r.message.c_str());
        std::error_code ec;
        std::ofstream f(exeDir / "foxsdr-portable-error.txt", std::ios::binary | std::ios::trunc);
        if (f) { f << r.message << "\n"; }
    }
    return r;
}

void applyPendingSystemActionsToProcess(int argc, char** argv) {
    try {
        static const char* const kToolFlags[] = {"--version", "--help", "-h", "--selftest", "--benchmark", "--rds-check", "--record-check", "--update-check", "--enumerate-json"};
        for (int i = 1; i < argc && argv != nullptr; ++i) {
            for (const char* flag : kToolFlags) {
                if (argv[i] != nullptr && std::string(argv[i]) == flag) { return; }
            }
        }
        const std::filesystem::path settings = std::filesystem::path(cascade::core::ConfigStore::defaultPath()).parent_path();
        if (settings.empty()) { return; }
        std::error_code ec;
        if (!std::filesystem::exists(settings / "wirec-pending.json", ec)) { return; }
        std::filesystem::path data;
#if defined(_WIN32)
        if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0') { data = std::filesystem::path(local) / "foxsdr"; }
#else
        if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') { data = std::filesystem::path(xdg) / "foxsdr"; }
        else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') { data = std::filesystem::path(home) / ".local" / "share" / "foxsdr"; }
#endif
        bool ok = true;
        const std::string said = wirec::applyPendingAtBoot(settings, data, ok);
        if (said.empty()) { return; }
        std::fprintf(stderr, "%s\n", said.c_str());
        std::ofstream f(settings / "wirec-last-boot.txt", std::ios::binary | std::ios::trunc);
        if (f) { f << said << "\n"; }
    } catch (...) {
        // a start must never fail because of this
    }
}

}  // namespace cascade::engine
