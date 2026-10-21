// A REAL plugin module that has a PRESET and a WINDOW, for tests/test_plugin_inuse_app.cpp.
//
// WHAT IT IS FOR. A plugin that runs only while it is used is told apart from one the user has pinned
// by the gesture that started it: pressing a preset is the deliberate "run this" and pins the plugin
// ("keep running"), while the tune that merely OPENING a window does for you (the auto-preset on a
// rail row) must not. Both go through the same function in the application, and neither can be
// exercised without a plugin that publishes a preset AND has a window with a row of its own - the
// other probe modules have one or the other, never both.
//
// It declares CASCADE_CAP_IMAGE_DECODER and CASCADE_CAP_PRESET and nothing else: a picture decoder
// that never offers a picture (so its window says it is waiting), with ONE preset (100.5 MHz, mode
// left alone).
//
// It appends "preset <event>" lines to the file RESCAN_PROBE_LOG names (attach, detach, create,
// destroy), the same convention as tests/fixtures/rescan_probe_plugin.cpp. Built by
// tests/CMakeLists.txt as a MODULE and including ONLY plugin_abi.h, as every third-party plugin must.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstdio>
#include <cstdlib>
#endif

#include "plugin_abi.h"

namespace {

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
    for (const char* p = "preset "; *p != '\0'; ++p) { line[len++] = *p; }
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
    std::fprintf(f, "preset %s\n", event);
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

// Optional text capability for the shared-output regression. Its lifetime is
// independent of the image decoder's event counts used by the existing tests.
void* createText(uint32_t) { return new (std::nothrow) Instance(); }
void destroyText(void* handle) { delete static_cast<Instance*>(handle); }
const CascadeDecoderApi kTextDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)), 0u, &createText, &process, &pollText, &destroyText,
};

uint32_t presetCount() { return 1u; }

int32_t presetGet(uint32_t index, CascadePreset* out) {
    if (index != 0u || out == nullptr) { return 0; }
    std::memset(out, 0, sizeof(*out));
    out->structSize = static_cast<uint32_t>(sizeof(CascadePreset));
    std::strncpy(out->label, "Probe 100.5 MHz", CASCADE_PRESET_LABEL_CHARS - 1);
    out->frequencyHz = 100.5e6;
    out->demodMode = CASCADE_DEMOD_UNCHANGED;
    out->bandwidthHz = 0.0;
    out->sampleRateHz = 0.0;
    out->flags = 0u;
    if (std::getenv("PRESET_PROBE_KEEP_TUNED") != nullptr) {
        out->demodMode = CASCADE_DEMOD_NFM;
        out->bandwidthHz = 12500.0;
        out->flags = CASCADE_PRESET_KEEP_TUNED_ON_START | CASCADE_PRESET_FLAT_AUDIO;
    }
    if (std::getenv("PRESET_PROBE_SHOW_TEXT") != nullptr) {
        out->flags |= CASCADE_PRESET_SHOW_TEXT_OUTPUT;
    }
    return 1;
}

const CascadePresetApi kPreset = {
    static_cast<uint32_t>(sizeof(CascadePresetApi)),
    &presetCount,
    &presetGet,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_IMAGE_DECODER, static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)), &kImageDecoder},
    {CASCADE_CAP_PRESET, static_cast<uint32_t>(sizeof(CascadePresetApi)), &kPreset},
};

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    "Preset Probe",
    "1.0.1",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_PRESET,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

const CascadeCapabilityEntry kTextCapabilities[] = {
    {CASCADE_CAP_IMAGE_DECODER, static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)), &kImageDecoder},
    {CASCADE_CAP_PRESET, static_cast<uint32_t>(sizeof(CascadePresetApi)), &kPreset},
    {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kTextDecoder},
};
const CascadePluginDesc kTextDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)), CASCADE_PLUGIN_ABI_VERSION,
    "Preset Probe", "1.0.1", "FoxSDR tests", "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_PRESET | CASCADE_CAP_DECODER,
    static_cast<uint32_t>(sizeof(kTextCapabilities) / sizeof(kTextCapabilities[0])), kTextCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(
    uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    if (std::getenv("PRESET_PROBE_TEXT_CAP") != nullptr) { return &kTextDesc; }
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
