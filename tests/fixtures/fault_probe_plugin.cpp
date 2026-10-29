// A REAL plugin module that misbehaves ON DEMAND, for the plugin-containment tests (plugin containment P1, docs/engine-stage4.md 6.28).
// Batch P2 systematises this into the hostile-plugin fixture; this is the MINIMUM that P1's features need - an access violation in
// create, destroy and a poll, a C++ exception thrown across the C boundary, a poll that does not return, and a fault in DllMain or in the
// descriptor query - and nothing else.
//
// Built by tests/CMakeLists.txt as a MODULE (fault_probe_fixture), into a directory of its own; it includes ONLY plugin_abi.h, as every
// third-party plugin must. IT CRASHES ON PURPOSE: the module is only ever loaded by the tests that stage a fault, from the build tree,
// and must never be copied into a plugins folder a person uses (tests/CMakeLists.txt keeps it under fixture_fault/).
//
// WHAT IT DOES IS CHOSEN BY THE ENVIRONMENT, read at each call (so one module serves every scenario and a test changes the scenario
// between calls): FAULT_PROBE is a comma-separated list of
//
//   dllmain-av          DllMain(DLL_PROCESS_ATTACH) dereferences null
//   query-av            cascade_plugin_query dereferences null
//   create-av           the decoder's create() dereferences null
//   create-throw        the decoder's create() throws a C++ exception across the C boundary
//   destroy-av          the decoder's destroy() dereferences null
//   poll-text-av        the decoder's poll_text() dereferences null
//   poll-text-wedge     the decoder's poll_text() sleeps FAULT_PROBE_WEDGE_MS (default 3000) before it answers
//   process-av          the decoder's process() dereferences null (the HOT path: the host does not guard it)
//   tracks-create-av    the track source's create() dereferences null
//   tracks-av           the track source's poll_tracks() dereferences null
//   image-poll-av       the image decoder's poll_image() dereferences null
//   create-die          the decoder's create() ends THE PROCESS with exit code 0xC0000005 (TerminateProcess: a death no handler and no guard can
//                       see, with the plugin inside its create() - what the crash-loop breaker's marker is for)
//   process-die         ...the same from the HOT path, process()
//   destroy-die         ...the same from destroy()
//   info-av             the track-info client's get_info() dereferences null
//   desc-wild-name      the descriptor's name points at unreadable memory
//   desc-wild-tables    the descriptor's capability table array points at unreadable memory
//   entry-size          the decoder entry's tableSize is not sizeof the decoder table
//   entry-dup           two entries claim the decoder capability
//
// and FAULT_PROBE_LOG names a file each call is appended to ("create", "destroy", "poll_text", "poll_tracks", "poll_image", "process"), so a test can
// prove a quarantined plugin is never called again. Unset, it writes nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#else
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <thread>
#endif

#include "plugin_abi.h"

#ifndef PROBE_VERSION
#define PROBE_VERSION "1.2.3"
#endif
#ifndef PROBE_NAME
#define PROBE_NAME "Fault Probe"
#endif

namespace {

// Whether FAULT_PROBE names `what`, matched as a whole comma-separated token.
bool has(const char* what) {
    char list[256];
#if defined(_WIN32)
    const DWORD n = ::GetEnvironmentVariableA("FAULT_PROBE", list, static_cast<DWORD>(sizeof(list)));
    if (n == 0 || n >= sizeof(list)) { return false; }
#else
    const char* e = std::getenv("FAULT_PROBE");
    if (e == nullptr || std::strlen(e) >= sizeof(list)) { return false; }
    std::strcpy(list, e);
#endif
    const std::size_t wl = std::strlen(what);
    for (const char* p = list; *p != '\0';) {
        const char* end = std::strchr(p, ',');
        const std::size_t len = end != nullptr ? static_cast<std::size_t>(end - p) : std::strlen(p);
        if (len == wl && std::strncmp(p, what, wl) == 0) { return true; }
        if (end == nullptr) { break; }
        p = end + 1;
    }
    return false;
}

void note(const char* event) {
#if defined(_WIN32)
    char path[1024];
    const DWORD n = ::GetEnvironmentVariableA("FAULT_PROBE_LOG", path, static_cast<DWORD>(sizeof(path)));
    if (n == 0 || n >= sizeof(path)) { return; }
    const HANDLE h = ::CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return; }
    char line[48];
    std::size_t len = 0;
    for (const char* p = event; *p != '\0' && len < sizeof(line) - 2; ++p) { line[len++] = *p; }
    line[len++] = '\n';
    DWORD written = 0;
    ::WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
    ::CloseHandle(h);
#else
    const char* path = std::getenv("FAULT_PROBE_LOG");
    if (path == nullptr || *path == '\0') { return; }
    std::FILE* f = std::fopen(path, "a");
    if (f == nullptr) { return; }
    std::fprintf(f, "%s\n", event);
    std::fclose(f);
#endif
}

// The process ends here, as a crash would end it, with nothing the host could have caught (no unwinding, no handler, no report).
[[noreturn]] void die() {
#if defined(_WIN32)
    ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
#else
    std::_Exit(139);
#endif
    for (;;) {}
}

// An access violation the optimiser cannot see through.
[[noreturn]] void fault() {
    volatile int* volatile p = nullptr;
    *p = 42;
    for (;;) {}
}

void wedge() {
    unsigned ms = 3000;
#if defined(_WIN32)
    char buf[16];
    const DWORD n = ::GetEnvironmentVariableA("FAULT_PROBE_WEDGE_MS", buf, static_cast<DWORD>(sizeof(buf)));
    if (n > 0 && n < sizeof(buf)) { ms = static_cast<unsigned>(std::atoi(buf)); }
    ::Sleep(ms);
#else
    const char* e = std::getenv("FAULT_PROBE_WEDGE_MS");
    if (e != nullptr) { ms = static_cast<unsigned>(std::atoi(e)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

struct Instance {
    int unused = 0;
};

// ---- the decoder -----------------------------------------------------------------------------------------------------------------
void* decoderCreate(uint32_t) {
    note("create");
    if (has("create-av")) { fault(); }
    if (has("create-die")) { die(); }
    if (has("create-throw")) { throw std::runtime_error("fault probe: create threw"); }
    return new (std::nothrow) Instance();
}

void decoderProcess(void*, const float*, size_t) {
    note("process");
    if (has("process-av")) { fault(); }
    if (has("process-die")) { die(); }
}

int32_t decoderPollText(void*, char*, size_t) {
    note("poll_text");
    if (has("poll-text-av")) { fault(); }
    if (has("poll-text-wedge")) { wedge(); }
    return 0;
}

void decoderDestroy(void* handle) {
    note("destroy");
    if (has("destroy-av")) { fault(); }
    if (has("destroy-die")) { die(); }
    delete static_cast<Instance*>(handle);
}

const CascadeDecoderApi kDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)), 0u, &decoderCreate, &decoderProcess, &decoderPollText, &decoderDestroy,
};

// ---- the track source ------------------------------------------------------------------------------------------------------------
void* tracksCreate() {
    note("tracks_create");
    if (has("tracks-create-av")) { fault(); }
    return new (std::nothrow) Instance();
}

int32_t tracksPoll(void*, CascadeTrack*, uint32_t) {
    note("poll_tracks");
    if (has("tracks-av")) { fault(); }
    return 0;
}

void tracksDestroy(void* handle) {
    note("tracks_destroy");
    delete static_cast<Instance*>(handle);
}

const CascadeTrackSourceApi kTracks = {
    static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)), &tracksCreate, &tracksPoll, nullptr, &tracksDestroy,
};

// ---- the image decoder -----------------------------------------------------------------------------------------------------------
void* imageCreate(double, double) {
    note("image_create");
    return new (std::nothrow) Instance();
}

void imageProcess(void*, const float*, size_t) {}

int32_t imagePoll(void*, CascadeImage*) {
    note("poll_image");
    if (has("image-poll-av")) { fault(); }
    return 0;
}

void imageRelease(void*, const CascadeImage*) {}

int32_t imagePollText(void*, char*, size_t) { return 0; }

void imageDestroy(void* handle) {
    note("image_destroy");
    delete static_cast<Instance*>(handle);
}

const CascadeImageDecoderApi kImage = {
    static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)),
    CASCADE_INPUT_AUDIO,
    0.0,
    0.0,
    &imageCreate,
    &imageProcess,
    nullptr,
    &imagePoll,
    &imageRelease,
    &imagePollText,
    &imageDestroy,
};

// ---- the track-info client -------------------------------------------------------------------------------------------------------
void* infoCreate() {
    note("info_create");
    return new (std::nothrow) Instance();
}

int32_t infoGet(void*, const char*, uint32_t, CascadeTrackInfo*) {
    note("get_info");
    if (has("info-av")) { fault(); }
    return CASCADE_INFO_MISSING;
}

void infoRelease(void*, const CascadeTrackInfo*) {}

int32_t infoPollText(void*, char*, size_t) { return 0; }

void infoDestroy(void* handle) {
    note("info_destroy");
    delete static_cast<Instance*>(handle);
}

const CascadeTrackInfoApi kInfo = {
    static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)), &infoCreate, &infoGet, &infoRelease, &infoPollText, &infoDestroy,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
    {CASCADE_CAP_TRACK_SOURCE, static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)), &kTracks},
    {CASCADE_CAP_IMAGE_DECODER, static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)), &kImage},
    {CASCADE_CAP_TRACK_INFO, static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)), &kInfo},
};

// NOT const: on Windows the declared NAME is made from the module's own file name the first time the host asks, so a test that copies this
// one module under several file names gets several plugins the host does not take for old copies of one another (the host keeps one
// loaded record per declared name).
CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    PROBE_NAME,
    PROBE_VERSION,
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    CASCADE_CAP_DECODER | CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_TRACK_INFO,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

char g_name[160] = {0};
#if defined(_WIN32)
HINSTANCE g_self = nullptr;
#endif

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t hostAbiVersion) {
    if (has("query-av")) { fault(); }
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
#if defined(_WIN32)
    if (g_name[0] == '\0' && g_self != nullptr) {
        char path[MAX_PATH] = {0};
        if (::GetModuleFileNameA(g_self, path, MAX_PATH) != 0) {
            const char* leaf = path;
            for (const char* p = path; *p != '\0'; ++p) {
                if (*p == '\\' || *p == '/') { leaf = p + 1; }
            }
            std::strcpy(g_name, PROBE_NAME " ");
            std::strncat(g_name, leaf, sizeof(g_name) - sizeof(PROBE_NAME) - 2);
            kDesc.name = g_name;
        }
    }
#else
    // The same on POSIX: the declared name is made from the shared object's own file name (dladdr names the file this function lives in).
    if (g_name[0] == '\0') {
        Dl_info info;
        if (::dladdr(reinterpret_cast<const void*>(&cascade_plugin_query), &info) != 0 && info.dli_fname != nullptr) {
            const char* leaf = info.dli_fname;
            for (const char* p = info.dli_fname; *p != '\0'; ++p) {
                if (*p == '/') { leaf = p + 1; }
            }
            std::strcpy(g_name, PROBE_NAME " ");
            std::strncat(g_name, leaf, sizeof(g_name) - sizeof(PROBE_NAME) - 2);
            kDesc.name = g_name;
        }
    }
#endif
    // THE CORRUPT DESCRIPTORS (load-time validation, docs 6.28): each hands back a descriptor the host must refuse without dying.
    static CascadePluginDesc bad;
    static CascadeCapabilityEntry badEntries[6];
    if (has("desc-wild-name") || has("desc-wild-tables") || has("entry-size") || has("entry-dup")) {
        bad = kDesc;
        std::memcpy(badEntries, kCapabilities, sizeof(kCapabilities));
        bad.capabilityTables = badEntries;
        if (has("desc-wild-name")) { bad.name = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(0x10)); }
        if (has("desc-wild-tables")) {
            bad.capabilityTables = reinterpret_cast<const CascadeCapabilityEntry*>(static_cast<std::uintptr_t>(0x10));
        }
        if (has("entry-size")) { badEntries[0].tableSize += 8u; }
        if (has("entry-dup")) {
            bad.capabilityCount = 5;
            badEntries[4] = badEntries[0];
        }
        return &bad;
    }
    return &kDesc;
}

#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        if (has("dllmain-av")) { fault(); }
    }
    return TRUE;
}
#endif
