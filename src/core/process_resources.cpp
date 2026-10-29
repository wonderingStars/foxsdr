// See process_resources.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/process_resources.hpp"

#include <cstdio>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#pragma comment(lib, "psapi.lib")
#else
#include <dirent.h>
#include <cstdlib>
#include <cstring>
#endif

namespace cascade::core {

namespace {

#if defined(_WIN32)

// THIS PROCESS'S VIDEO MEMORY, from the adapter it uses the most of. The factory
// and adapters are made and released on every read: a read is rare (see the
// header) and holding DXGI objects across a display change - the very moment a
// tester's log is about - is a way to hold stale ones.
void readVideoMemory(ProcessResources& r) {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    static HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr) { return; }
    static const auto create =
        reinterpret_cast<CreateFactoryFn>(::GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (create == nullptr) { return; }
    IDXGIFactory1* factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) ||
        factory == nullptr) {
        return;
    }
    std::int64_t bestUsed = -1;
    std::int64_t bestBudget = -1;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK || adapter == nullptr) { break; }
        IDXGIAdapter3* a3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3))) &&
            a3 != nullptr) {
            // Both segment groups: a discrete card's own memory is `local`; an
            // integrated adapter's share of system memory is mostly `non-local`.
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal{};
            const bool l = SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local));
            const bool nl =
                SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal));
            if (l || nl) {
                const std::int64_t used = static_cast<std::int64_t>((l ? local.CurrentUsage : 0) +
                                                                    (nl ? nonLocal.CurrentUsage : 0));
                const std::int64_t budget =
                    static_cast<std::int64_t>((l ? local.Budget : 0) + (nl ? nonLocal.Budget : 0));
                if (used > bestUsed) {
                    bestUsed = used;
                    bestBudget = budget;
                }
            }
            a3->Release();
        }
        adapter->Release();
    }
    factory->Release();
    r.videoUsedBytes = bestUsed;
    r.videoBudgetBytes = bestBudget;
}

#endif

long long mb(std::int64_t bytes) { return static_cast<long long>((bytes + (1 << 19)) >> 20); }

}  // namespace

ProcessResources readProcessResources() {
    ProcessResources r;
#if defined(_WIN32)
    const HANDLE self = ::GetCurrentProcess();
    r.gdiObjects = static_cast<long>(::GetGuiResources(self, GR_GDIOBJECTS));
    r.userObjects = static_cast<long>(::GetGuiResources(self, GR_USEROBJECTS));
    DWORD handles = 0;
    if (::GetProcessHandleCount(self, &handles)) { r.handles = static_cast<long>(handles); }
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    if (::GetProcessMemoryInfo(self, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm)) {
        r.memoryBytes = static_cast<std::int64_t>(pm.PrivateUsage);
    }
    readVideoMemory(r);
#else
    if (DIR* d = ::opendir("/proc/self/fd")) {
        long n = 0;
        while (const dirent* e = ::readdir(d)) {
            if (e->d_name[0] != '.') { ++n; }
        }
        ::closedir(d);
        r.handles = n;
    }
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        long pages = 0;
        long resident = 0;
        if (std::fscanf(f, "%ld %ld", &pages, &resident) == 2) {
            r.memoryBytes = static_cast<std::int64_t>(resident) * 4096;
        }
        std::fclose(f);
    }
#endif
    return r;
}

std::string processResourcesLine(const ProcessResources& r, const char* when) {
    char line[192];
    int n = std::snprintf(line, sizeof line, "resources: gdi ");
    auto count = [&](long v) {
        if (n < 0 || n >= static_cast<int>(sizeof line)) { return; }
        n += v < 0 ? std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n), "?")
                   : std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n), "%ld", v);
    };
    auto text = [&](const char* s) {
        if (n < 0 || n >= static_cast<int>(sizeof line)) { return; }
        n += std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n), "%s", s);
    };
    auto megabytes = [&](std::int64_t v) {
        if (n < 0 || n >= static_cast<int>(sizeof line)) { return; }
        n += v < 0 ? std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n), "?")
                   : std::snprintf(line + n, sizeof line - static_cast<std::size_t>(n), "%lld", mb(v));
    };
    count(r.gdiObjects);
    text(", user ");
    count(r.userObjects);
    text(", handles ");
    count(r.handles);
    text(", memory ");
    megabytes(r.memoryBytes);
    text(" MB, video ");
    megabytes(r.videoUsedBytes);
    text(" of ");
    megabytes(r.videoBudgetBytes);
    text(" MB");
    if (when != nullptr && when[0] != '\0') {
        text(" - ");
        text(when);
    }
    return std::string(line);
}

}  // namespace cascade::core
