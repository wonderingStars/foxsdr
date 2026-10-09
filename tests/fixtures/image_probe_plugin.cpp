// A REAL plugin module whose output is a PICTURE, and a picture that never arrives - for
// tests/test_decoder_window_app.cpp, which has to see the plugin's image window open INSIDE the
// main window (and not past its right edge, where it used to open with only its 19 px margin on
// the screen). A decoded-picture window is drawn for an image decoder the receiver is running,
// whether or not a picture has come, and says it is waiting: which is all this module gives it.
//
// It declares CASCADE_CAP_IMAGE_DECODER and nothing else, takes demodulated audio at any rate,
// decodes nothing and never offers an image. Built by tests/CMakeLists.txt as a MODULE and
// including ONLY plugin_abi.h, as every third-party plugin must.
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

namespace {

// OPTIONAL EVENT LOG (0.99.73), the same convention as tests/fixtures/rescan_probe_plugin.cpp:
// when the environment variable RESCAN_PROBE_LOG names a file, one line "image <event>" is
// appended for each of attach, detach, create and destroy; unset, nothing is written (which is
// every run of test_decoder_window_app). tests/test_plugin_inuse_app.cpp reads it to prove WHEN
// the host creates and destroys this module's decoder - the thing a plugin that runs only while
// it is used is about. On Windows the line is written with kernel32 alone: DllMain runs under the
// loader lock, where the C runtime's file functions are not a thing to lean on.
void note(const char* event) {
#if defined(_WIN32)
    char path[1024];
    const DWORD n = ::GetEnvironmentVariableA("RESCAN_PROBE_LOG", path, static_cast<DWORD>(sizeof(path)));
    if (n == 0 || n >= sizeof(path)) { return; }
    const HANDLE h = ::CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return; }
    char line[64];
    std::size_t len = 0;
    for (const char* p = "image "; *p != '\0'; ++p) { line[len++] = *p; }
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
    std::fprintf(f, "image %s\n", event);
    std::fclose(f);
#endif
}

struct Instance {
    int unused = 0;
};

void* create(double, double) {
    note("create");
    return new (std::nothrow) Instance();
}

void process(void*, const float*, size_t) {}

int32_t pollImage(void*, CascadeImage*) { return 0; }  // none pending

void releaseImage(void*, const CascadeImage*) {}

int32_t pollText(void*, char*, size_t) { return 0; }

void destroy(void* handle) {
    note("destroy");
    delete static_cast<Instance*>(handle);
}

const CascadeImageDecoderApi kImageDecoder = {
    static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
    CASCADE_INPUT_AUDIO,
    0.0,  // requiredRateHz: any rate
    0.0,  // preferredRateHz
    &create,
    &process,
    nullptr,  // retune: this decoder does not care
    &pollImage,
    &releaseImage,
    &pollText,
    &destroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_IMAGE_DECODER, static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
     &kImageDecoder},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Image Probe",
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_IMAGE_DECODER,
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
