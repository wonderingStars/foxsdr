// engine_api_test_env.hpp - the hermetic environment the engine-API tests run
// the real engine in: every per-user folder a scratch directory of this
// process (the engine's record folder and any config are taken from them),
// telemetry pointed at a closed port, and the plugin search left to a folder
// that has none. Called once, first thing in main().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace engine_api_test {

inline void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

inline void hermetic(const char* tag) {
#if defined(_WIN32)
    const int pid = _getpid();
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    const std::string tmp = std::string(buf) + "foxsdr_engine_api_" + tag + "_" + std::to_string(pid);
#else
    const int pid = static_cast<int>(getpid());
    const std::string tmp = "/tmp/foxsdr_engine_api_" + std::string(tag) + "_" + std::to_string(pid);
#endif
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
                          "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, tmp);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

}  // namespace engine_api_test
