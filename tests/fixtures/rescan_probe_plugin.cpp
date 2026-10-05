// A REAL plugin module that says, in a file, every time the host maps it, unmaps
// it, creates one of its decoders and destroys one - for tests/test_plugin_
// rescan_skip.cpp, which has to prove that a rescan which was skipped tore
// NOTHING down. A log line in the application proves what the application
// meant to do; this proves what happened to the module: its DllMain ran again
// (or did not), its instance was destroyed (or was not).
//
// Built by tests/CMakeLists.txt as a MODULE, twice, from this one source:
// PROBE_ID is "a" for one and "b" for the other, so the test has two distinct
// plugins (different declared names, so the host does not take one for an old
// copy of the other) and every line says which of them wrote it. It includes
// ONLY plugin_abi.h, as every third-party plugin must.
//
// WHERE IT WRITES. The file named by the environment variable
// RESCAN_PROBE_LOG, appended to, one line per event: "<id> <event>" with event
// one of attach, detach, create, destroy. Unset, it writes nothing. On Windows
// the lines are written with kernel32 alone - DllMain runs under the loader
// lock, where the C runtime's file functions are not a thing to lean on.
//
// WHAT IT DECODES: nothing. Its decoder takes samples and hands back no text.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdio>
#include <cstdlib>
#endif

#include "plugin_abi.h"

#ifndef PROBE_ID
#define PROBE_ID "a"
#endif

namespace {

void note(const char* event) {
#if defined(_WIN32)
    char path[1024];
    const DWORD n = ::GetEnvironmentVariableA("RESCAN_PROBE_LOG", path,
                                              static_cast<DWORD>(sizeof(path)));
    if (n == 0 || n >= sizeof(path)) { return; }
    const HANDLE h = ::CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return; }
    char line[64];
    std::size_t len = 0;
    for (const char* p = PROBE_ID; *p != '\0' && len < 8; ++p) { line[len++] = *p; }
    line[len++] = ' ';
    for (const char* p = event; *p != '\0' && len < sizeof(line) - 2; ++p) { line[len++] = *p; }
    line[len++] = '\n';
    DWORD written = 0;
    ::WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
    ::CloseHandle(h);
#else
    const char* path = std::getenv("RESCAN_PROBE_LOG");
    if (path == nullptr || *path == '\0') { return; }
    std::FILE* f = std::fopen(path, "a");
    if (f == nullptr) { return; }
    std::fprintf(f, "%s %s\n", PROBE_ID, event);
    std::fclose(f);
#endif
}

struct Instance {
    int unused = 0;
};

void* create(uint32_t) {
    note("create");
    return new (std::nothrow) Instance();
}

void process(void*, const float*, size_t) {}

int32_t pollText(void*, char*, size_t) { return 0; }

void destroy(void* handle) {
    note("destroy");
    delete static_cast<Instance*>(handle);
}

const CascadeDecoderApi kDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)), 0u, &create, &process, &pollText, &destroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Rescan Probe " PROBE_ID,
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_DECODER,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    return &kDesc;
}

#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { note("attach"); }
    if (reason == DLL_PROCESS_DETACH) { note("detach"); }
    return TRUE;
}
#else
__attribute__((constructor)) static void probeAttach() { note("attach"); }
__attribute__((destructor)) static void probeDetach() { note("detach"); }
#endif
