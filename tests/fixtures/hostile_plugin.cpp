// A REAL plugin module that misbehaves ON DEMAND, in every way the plugin ABI lets a plugin misbehave (plugin containment batch P2,
// docs/engine-stage4.md 6.29): the hostile-plugin fixture. It widens the P1 fault probe (tests/fixtures/fault_probe_plugin.cpp) from "a fault in
// create, destroy and a poll" to the whole surface, and adds the three things the probe never did: abuse of the HOST API (the other direction
// of the boundary), lies in what the plugin hands BACK, and a referee that records every rule the HOST breaks towards the plugin.
//
// Built by tests/CMakeLists.txt as a MODULE (hostile_fixture), into a directory of its own (fixture_hostile/); it includes ONLY plugin_abi.h,
// as every third-party plugin must. IT MISBEHAVES ON PURPOSE (it crashes, hangs, floods and lies): it is only ever loaded by the tests that
// stage an abuse, from the build tree, and must never be copied into a plugins folder a person uses.
//
// It declares ten capabilities (decoder, image decoder + image input, track source, panel, instrument, basemap, track info, host client,
// preset) so every guarded entry point of core/plugin_calls.hpp has a body here.
//
// WHAT IT DOES IS CHOSEN BY THE ENVIRONMENT, read at each call (so one module serves every scenario, and a test changes the scenario between
// calls): HOSTILE_PLUGIN is a comma-separated list of modes. A mode may be scoped to one copy of the module by a prefix, "<id>:<mode>", which
// matches the copy whose file name begins "<id>-" or "<id>." (a test copies this one module under many names, each a plugin of its own).
//
//   AT EVERY GUARDED ENTRY POINT (<entry> is the word core/plugin_fault.hpp gives it: create, destroy, poll-text, poll-image, release-image,
//   poll-tracks, poll-paths, poll-state, columns, poll-rows, get-tile, release-tile, get-info, release-info, attach, press, preset-count,
//   preset-get; and `process`, the hot path):
//     <entry>-av        dereference null
//     <entry>-throw     throw a C++ exception across the C boundary
//     <entry>-spin      loop (busy) for HOSTILE_PLUGIN_SPIN_MS (default 3000), then return as if nothing had happened
//     <entry>-die       end THE PROCESS with exit code 0xC0000005 (TerminateProcess: no handler, no report)
//     <entry>-stack     recurse until the stack is gone (a stack overflow is never absorbed: the process dies, with a report)
//
//   AT LOAD AND UNLOAD:  dllmain-av, query-av, dllmain-detach-av; and the corrupt descriptors: desc-wild-name, desc-wild-tables, entry-size,
//   entry-dup, panel-title-wild, instrument-title-wild, basemap-attribution-wild, and caps-word-lie (claims a capability bit it has no entry for).
//
//   LIES in what the plugin hands back (the host must clamp, ignore or refuse - never trust):
//     poll-text-lie       poll_text returns cap + 100 (and wrote only cap)
//     poll-tracks-lie     poll_tracks returns cap + 500
//     poll-tracks-nan     tracks with NaN / infinite / out-of-range coordinates and an id with no terminator
//     poll-paths-lie      poll_paths returns cap + 10, with a vertex pointer that is garbage and one that is NULL
//     poll-rows-lie       poll_rows returns cap + 500 (panel and instrument)
//     columns-lie         columns returns 99 (the ABI has 8)
//     poll-state-lie      poll_state returns 7, with NaN values and texts with no terminator
//     poll-image-lie      poll_image returns an image of the largest legal size over a buffer of 64 bytes (a pointer that is readable for
//                         far less than width * height says), poll-image-null a NULL pixels pointer, poll-image-wild a garbage one,
//                         poll-image-huge a width and height past the ABI's bound
//     get-tile-lie / -null / -wild   the same three for a basemap tile
//     get-info-wild       get_info answers READY with string pointers that are garbage; get-info-unterminated with strings that run to the
//                         end of a page and have no terminator (the next page is not readable)
//     preset-count-lie    count() returns 1,000,000;  preset-get-lie  get() answers 1 for an index it was not asked for, labels unterminated
//     create-null         create() fails (NULL): the ABI's honest "failed", NOT a misuse - the control case
//
//   THE HOST API, abused: not by a mode but by calls the test makes into hostile_do() (below), on whatever thread the test chooses (a
//   real-time scope, a plain thread), because the abuse is "which thread" as much as "what".  thread-outlives is a mode: create() starts a
//   thread that keeps calling the host API and destroy() does not stop it (ABI rule 3 broken on purpose); process-settings-get calls
//   settings_get from inside process(), the real-time thread.
//
// THE REFEREE. Every instance records what the host does to it and the module notes each rule the HOST breaks: a call on a destroyed instance
// (instances are poisoned, never freed, so this is detectable and safe), two threads inside one instance at once, a call into the module made
// from inside another call into it, a create() from inside a call, and a GUI-capability instance (track source, panel, instrument) used from
// two threads. They are counted (hostile_violations) and appended to the log as "VIOLATION ...". A correct host leaves none.
//
// EXPORTS (not part of the ABI; the tests find them with GetProcAddress / dlsym):
//   int64_t hostile_do(const char* op, int64_t arg)        see the table in hostile_do below
//   size_t  hostile_violations(char* text, size_t cap)     the number of violations so far; their text (may be null)
//
// The log: HOSTILE_PLUGIN_LOG names a file each entry is appended to as its word ("create", "poll-text", "process", ...).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "plugin_abi.h"

#ifndef HOSTILE_VERSION
#define HOSTILE_VERSION "1.2.3"
#endif
#ifndef HOSTILE_NAME
#define HOSTILE_NAME "Hostile Plugin"
#endif

namespace {

// ---- the environment ------------------------------------------------------------------------------------------------------------------
char g_leaf[160] = {0};   // this copy's file name, lower case ("hp-a-1.2.3-abi3-win-x64.dll")
char g_name[200] = {0};   // the declared name (made from the file name, so every copy is a plugin of its own)

bool envGet(const char* name, char* out, std::size_t cap) {
#if defined(_WIN32)
    const DWORD n = ::GetEnvironmentVariableA(name, out, static_cast<DWORD>(cap));
    return n > 0 && n < cap;
#else
    const char* e = std::getenv(name);
    if (e == nullptr || std::strlen(e) >= cap) { return false; }
    std::strcpy(out, e);
    return true;
#endif
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Whether this copy's file name begins with the scope `id` followed by '-' or '.'.
bool scopeMatches(const char* id, std::size_t len) {
    if (g_leaf[0] == '\0') { return true; }
    for (std::size_t i = 0; i < len; ++i) {
        if (g_leaf[i] == '\0' || g_leaf[i] != lower(id[i])) { return false; }
    }
    return g_leaf[len] == '-' || g_leaf[len] == '.';
}

bool has(const char* what) {
    char list[1024];
    if (!envGet("HOSTILE_PLUGIN", list, sizeof(list))) { return false; }
    const std::size_t wl = std::strlen(what);
    for (const char* p = list; *p != '\0';) {
        const char* end = std::strchr(p, ',');
        const std::size_t len = end != nullptr ? static_cast<std::size_t>(end - p) : std::strlen(p);
        const char* tok = p;
        std::size_t tl = len;
        const char* colon = static_cast<const char*>(std::memchr(p, ':', len));
        bool scoped = true;
        if (colon != nullptr) {
            scoped = scopeMatches(p, static_cast<std::size_t>(colon - p));
            tok = colon + 1;
            tl = len - static_cast<std::size_t>(colon - p) - 1;
        }
        if (scoped && tl == wl && std::strncmp(tok, what, wl) == 0) { return true; }
        if (end == nullptr) { break; }
        p = end + 1;
    }
    return false;
}

bool hasMode(const char* entry, const char* kind) {
    char token[64];
    std::snprintf(token, sizeof(token), "%s-%s", entry, kind);
    return has(token);
}

void note(const char* event) {
    char path[1024];
    if (!envGet("HOSTILE_PLUGIN_LOG", path, sizeof(path))) { return; }
#if defined(_WIN32)
    const HANDLE h = ::CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (h == INVALID_HANDLE_VALUE) { return; }
    char line[200];
    std::size_t len = 0;
    for (const char* p = event; *p != '\0' && len < sizeof(line) - 2; ++p) { line[len++] = *p; }
    line[len++] = '\n';
    DWORD written = 0;
    ::WriteFile(h, line, static_cast<DWORD>(len), &written, nullptr);
    ::CloseHandle(h);
#else
    std::FILE* f = std::fopen(path, "a");
    if (f == nullptr) { return; }
    std::fprintf(f, "%s\n", event);
    std::fclose(f);
#endif
}

// ---- the abuses ----------------------------------------------------------------------------------------------------------------------
[[noreturn]] void die() {
#if defined(_WIN32)
    ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
#else
    std::_Exit(139);
#endif
    for (;;) {}
}

[[noreturn]] void fault() {
    volatile int* volatile p = nullptr;
    *p = 42;
    for (;;) {}
}

// A loop that does not return until HOSTILE_PLUGIN_SPIN_MS (default 3000) has gone, and burns the processor meanwhile.
void spin() {
    unsigned ms = 3000;
    char buf[16];
    if (envGet("HOSTILE_PLUGIN_SPIN_MS", buf, sizeof(buf))) { ms = static_cast<unsigned>(std::atoi(buf)); }
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    volatile std::uint64_t x = 0;
    while (std::chrono::steady_clock::now() < until) {
        for (int i = 0; i < 20000; ++i) { x = x + 1; }
    }
}

// Recursion with a frame of real size that the optimiser cannot fold away.
void recurse(unsigned depth, volatile char* from) {
    volatile char pad[2048];
    pad[0] = static_cast<char>(depth);
    pad[1] = from != nullptr ? from[0] : 0;
    if (depth != 0xFFFFFFFFu) { recurse(depth + 1, pad); }
    pad[2] = pad[0];
}

// The five abuses at an entry point, in the order a test cares about: die, stack, fault, throw, loop.
void misbehave(const char* entry) {
    note(entry);
    if (hasMode(entry, "die")) { die(); }
    if (hasMode(entry, "stack")) { recurse(0, nullptr); }
    if (hasMode(entry, "av")) { fault(); }
    if (hasMode(entry, "throw")) { throw std::runtime_error("hostile plugin: thrown across the C boundary"); }
    if (hasMode(entry, "spin")) { spin(); }
}

// ---- the referee ----------------------------------------------------------------------------------------------------------------------
std::atomic<std::uint64_t> g_violations{0};
char g_violationText[2048] = {0};
std::atomic<int> g_violationLock{0};

void violation(const char* what, const char* entry) {
    g_violations.fetch_add(1, std::memory_order_relaxed);
    char line[160];
    std::snprintf(line, sizeof(line), "VIOLATION %s in %s", what, entry);
    note(line);
    while (g_violationLock.exchange(1, std::memory_order_acquire) != 0) {}
    const std::size_t at = std::strlen(g_violationText);
    if (at + std::strlen(line) + 2 < sizeof(g_violationText)) {
        std::strcat(g_violationText, line);
        std::strcat(g_violationText, "\n");
    }
    g_violationLock.store(0, std::memory_order_release);
}

constexpr std::uint32_t kAlive = 0xA11CE5EDu;
constexpr std::uint32_t kDead = 0xDEADBEEFu;

thread_local int t_depth = 0;   // calls into this module the calling thread is inside

enum class Kind : std::uint8_t { Decoder, Image, Tracks, Panel, Instrument, Basemap, Info };

struct Instance {
    std::atomic<std::uint32_t> magic{kAlive};
    std::atomic<int> busy{0};
    std::thread::id owner{};
    bool gui = false;
    Kind kind = Kind::Decoder;
};

// Poisoned at destroy and NEVER freed: a later call is detected without being undefined behaviour. They live in a static pool, not on the heap: the
// host LEAKS a switched-off plugin's instances on purpose, and a heap leak is what a leak checker would then report of the plugin's own allocation.
constexpr std::size_t kPool = 16384;
Instance g_pool[kPool];
std::atomic<std::size_t> g_poolNext{0};

Instance* make(Kind k, bool gui) {
    if (t_depth > 0) { violation("create-inside-a-call-into-the-module", "create"); }
    const std::size_t at = g_poolNext.fetch_add(1);
    Instance* i = at < kPool ? &g_pool[at] : nullptr;
    if (i != nullptr) {
        i->kind = k;
        i->gui = gui;
        i->owner = std::this_thread::get_id();
    }
    return i;
}

// One call on an instance: checks every rule the host owes the plugin, for the length of the call.
struct Use {
    Instance* i;
    explicit Use(void* h, const char* entry, bool destroying = false) : i(static_cast<Instance*>(h)) {
        if (i == nullptr) { violation("call-on-a-null-handle", entry); return; }
        if (i->magic.load(std::memory_order_acquire) != kAlive) { violation("call-after-destroy", entry); i = nullptr; return; }
        if (i->busy.fetch_add(1, std::memory_order_acq_rel) != 0) { violation("two-threads-in-one-instance", entry); }
        if (t_depth > 0) { violation("call-from-inside-a-call-into-the-module", entry); }
        if (i->gui && i->owner != std::this_thread::get_id()) { violation("gui-instance-used-from-a-second-thread", entry); }
        ++t_depth;
        if (destroying) { i->magic.store(kDead, std::memory_order_release); }
    }
    ~Use() {
        if (i != nullptr) {
            --t_depth;
            i->busy.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
};

// ---- a page with nothing readable after it, for strings that run off the end -------------------------------------------------------------
// Returns a pointer to `len` bytes of 'A' that END exactly at an unreadable page, or null if the platform gives no such page.
const char* edgeOfAPage(std::size_t len) {
    static char* base = nullptr;
    static std::size_t page = 0;
    if (base == nullptr) {
#if defined(_WIN32)
        SYSTEM_INFO si;
        ::GetSystemInfo(&si);
        page = si.dwPageSize;
        char* p = static_cast<char*>(::VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (p == nullptr) { return nullptr; }
        DWORD old = 0;
        ::VirtualProtect(p + page, page, PAGE_NOACCESS, &old);
        base = p;
#else
        page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        void* p = ::mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { return nullptr; }
        ::mprotect(static_cast<char*>(p) + page, page, PROT_NONE);
        base = static_cast<char*>(p);
#endif
        std::memset(base, 'A', page);
    }
    if (len > page) { len = page; }
    return base + page - len;
}

const char* wildPointer() { return reinterpret_cast<const char*>(static_cast<std::uintptr_t>(0x10)); }

// ---- the host API (set at attach) and the thread that outlives destroy() -------------------------------------------------------------------
const CascadeHostApi* g_host = nullptr;
std::atomic<bool> g_threadStop{false};
std::atomic<int> g_threadAlive{0};
std::atomic<std::uint64_t> g_threadCalls{0};
std::atomic<std::int32_t> g_threadLast{0};

void outlivingThread() {
    g_threadAlive.fetch_add(1);
    while (!g_threadStop.load()) {
        if (g_host != nullptr && g_host->log != nullptr) {
            g_threadLast.store(g_host->log(g_host->ctx, CASCADE_LOG_INFO, "the thread that outlived destroy()"));
            g_threadCalls.fetch_add(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g_threadAlive.fetch_sub(1);
}

void startOutlivingThread() {
    g_threadStop.store(false);
    std::thread(outlivingThread).detach();
}

// ---- the decoder -----------------------------------------------------------------------------------------------------------------------
void* decoderCreate(std::uint32_t) {
    misbehave("create");
    if (has("create-null")) { return nullptr; }
    if (has("thread-outlives")) { startOutlivingThread(); }
    return make(Kind::Decoder, false);
}

void decoderProcess(void* h, const float*, size_t) {
    Use use(h, "process");
    misbehave("process");
    if (has("process-settings-get") && g_host != nullptr && g_host->settings_get != nullptr) {
        char buf[16];
        const std::int32_t r = g_host->settings_get(g_host->ctx, "k", buf, sizeof(buf));
        char line[64];
        std::snprintf(line, sizeof(line), "process-settings-get=%d", static_cast<int>(r));
        note(line);
    }
}

int32_t decoderPollText(void* h, char* buf, size_t cap) {
    Use use(h, "poll-text");
    misbehave("poll-text");
    if (has("poll-text-lie")) {
        std::memset(buf, 'T', cap);
        return static_cast<int32_t>(cap + 100);
    }
    return 0;
}

void decoderDestroy(void* h) {
    Use use(h, "destroy", true);
    misbehave("destroy");
    // (thread-outlives: the thread is deliberately NOT stopped here)
}

const CascadeDecoderApi kDecoder = {
    static_cast<uint32_t>(sizeof(CascadeDecoderApi)), 0u, &decoderCreate, &decoderProcess, &decoderPollText, &decoderDestroy,
};

// ---- the image decoder (and its input) -------------------------------------------------------------------------------------------------
std::uint8_t g_pixels[8 * 8] = {0};
std::uint8_t g_tinyBuffer[64] = {0};

void* imageCreate(double, double) {
    misbehave("create");
    if (has("create-null")) { return nullptr; }
    return make(Kind::Image, false);
}
void imageProcess(void*, const float*, size_t) {}
int32_t imagePoll(void* h, CascadeImage* out) {
    Use use(h, "poll-image");
    misbehave("poll-image");
    out->width = 8;
    out->height = 8;
    out->format = CASCADE_IMAGE_GRAY8;
    out->stride = 8;
    out->complete = 1;
    out->sequence = 1;
    out->pixels = g_pixels;
    if (has("poll-image-lie")) {
        // 64 x 64 over a buffer that is readable for exactly 64 bytes and then ends at an unreadable page
        out->width = 64;
        out->height = 64;
        out->stride = 64;
        const char* edge = edgeOfAPage(64);
        out->pixels = edge != nullptr ? reinterpret_cast<const std::uint8_t*>(edge) : g_tinyBuffer;
    }
    if (has("poll-image-null")) { out->pixels = nullptr; }
    if (has("poll-image-wild")) { out->pixels = reinterpret_cast<const std::uint8_t*>(wildPointer()); }
    if (has("poll-image-huge")) {
        out->width = 0xFFFFFFFFu;
        out->height = 0xFFFFFFFFu;
        out->stride = 0xFFFFFFFFu;
    }
    return 1;
}
void imageRelease(void* h, const CascadeImage*) {
    Use use(h, "release-image");
    misbehave("release-image");
}
int32_t imagePollText(void* h, char* buf, size_t cap) {
    Use use(h, "poll-text");
    misbehave("poll-text");
    if (has("poll-text-lie")) {
        std::memset(buf, 'T', cap);
        return static_cast<int32_t>(cap + 100);
    }
    return 0;
}
void imageDestroy(void* h) {
    Use use(h, "destroy", true);
    misbehave("destroy");
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

void imagePress(std::uint32_t, std::uint32_t) { misbehave("press"); }
const CascadeImageInputApi kImageInput = {static_cast<uint32_t>(sizeof(CascadeImageInputApi)), &imagePress};

// ---- the track source -------------------------------------------------------------------------------------------------------------------
void* tracksCreate() {
    misbehave("create");
    if (has("create-null")) { return nullptr; }
    return make(Kind::Tracks, true);
}

void fillTrack(CascadeTrack& t, unsigned n) {
    std::memset(&t, 0, sizeof(t));
    std::snprintf(t.id, CASCADE_TRACK_ID_CHARS, "HOSTILE%u", n);
    std::snprintf(t.label, CASCADE_TRACK_LABEL_CHARS, "hostile %u", n);
    t.latDeg = 51.0 + 0.001 * static_cast<double>(n % 100);
    t.lonDeg = -1.0;
    t.altM = 1000.0;
    t.courseDeg = 90.0;
    t.speedMps = 100.0;
    t.ageMs = 100;
    t.kind = CASCADE_TRACK_AIRCRAFT;
}

int32_t tracksPoll(void* h, CascadeTrack* out, uint32_t cap) {
    Use use(h, "poll-tracks");
    misbehave("poll-tracks");
    if (cap == 0) { return 0; }
    if (has("poll-tracks-lie")) {
        for (uint32_t i = 0; i < cap; ++i) { fillTrack(out[i], i); }
        return static_cast<int32_t>(cap + 500);
    }
    if (has("poll-tracks-nan")) {
        const double nan = std::nan("");
        const double inf = HUGE_VAL;
        const double lat[] = {nan, inf, -inf, 91.0, -91.0, 1e300, 0.0};
        const double lon[] = {nan, nan, inf, 181.0, -181.0, -1e300, nan};
        uint32_t n = 0;
        for (; n < 7 && n < cap; ++n) {
            fillTrack(out[n], n);
            out[n].latDeg = lat[n];
            out[n].lonDeg = lon[n];
            out[n].altM = nan;
            out[n].speedMps = inf;
            out[n].courseDeg = -inf;
            std::memset(out[n].id, 'Z', sizeof(out[n].id));      // no terminator
            std::memset(out[n].label, 'Y', sizeof(out[n].label));
            out[n].kind = 0xFFFFFFFFu;
            out[n].flags = 0xFFFFFFFFu;
        }
        return static_cast<int32_t>(n);
    }
    fillTrack(out[0], 0);
    return 1;
}

CascadePathPoint g_pathPoints[2] = {{51.0, -1.0}, {51.1, -1.1}};

int32_t tracksPollPaths(void* h, CascadePath* out, uint32_t cap) {
    Use use(h, "poll-paths");
    misbehave("poll-paths");
    if (cap == 0) { return 0; }
    if (has("poll-paths-lie")) {
        for (uint32_t i = 0; i < cap; ++i) {
            std::memset(&out[i], 0, sizeof(out[i]));
            std::snprintf(out[i].id, CASCADE_TRACK_ID_CHARS, "P%u", i);
            out[i].count = 0xFFFFFFFFu;
            out[i].points = (i % 2 == 0) ? reinterpret_cast<const CascadePathPoint*>(wildPointer()) : nullptr;
        }
        return static_cast<int32_t>(cap + 10);
    }
    std::memset(&out[0], 0, sizeof(out[0]));
    std::snprintf(out[0].id, CASCADE_TRACK_ID_CHARS, "PATH");
    out[0].points = g_pathPoints;
    out[0].count = 2;
    return 1;
}

void tracksDestroy(void* h) {
    Use use(h, "destroy", true);
    misbehave("destroy");
}
const CascadeTrackSourceApi kTracks = {
    static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)), &tracksCreate, &tracksPoll, &tracksPollPaths, &tracksDestroy,
};

// ---- the panel --------------------------------------------------------------------------------------------------------------------------
void* panelCreate() {
    misbehave("create");
    if (has("create-null")) { return nullptr; }
    return make(Kind::Panel, true);
}

uint32_t panelColumns(void* h, char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    Use use(h, "columns");
    misbehave("columns");
    std::snprintf(headings[0], CASCADE_PANEL_CELL_CHARS, "Column");
    std::snprintf(headings[1], CASCADE_PANEL_CELL_CHARS, "Value");
    if (has("columns-lie")) { return 99; }
    return 2;
}

int32_t panelPollRows(void* h, CascadePanelRow* out, uint32_t cap) {
    Use use(h, "poll-rows");
    misbehave("poll-rows");
    if (cap == 0) { return 0; }
    const bool lie = has("poll-rows-lie");
    const uint32_t n = lie ? cap : 1u;
    for (uint32_t i = 0; i < n; ++i) {
        std::memset(&out[i], 0, sizeof(out[i]));
        out[i].kind = CASCADE_ROW_CELLS;
        std::snprintf(out[i].cells[0], CASCADE_PANEL_CELL_CHARS, "row %u", i);
        std::snprintf(out[i].cells[1], CASCADE_PANEL_CELL_CHARS, "%u", i);
        if (lie) { std::memset(out[i].cells[1], 'C', CASCADE_PANEL_CELL_CHARS); }   // no terminator
    }
    return static_cast<int32_t>(lie ? cap + 500 : 1u);
}

void panelDestroy(void* h) {
    Use use(h, "destroy", true);
    misbehave("destroy");
}

CascadePanelApi kPanel = {
    static_cast<uint32_t>(sizeof(CascadePanelApi)), "Hostile panel", &panelCreate, &panelColumns, &panelPollRows, &panelDestroy,
};

// ---- the instrument ---------------------------------------------------------------------------------------------------------------------
void* instrumentCreate() {
    misbehave("create");
    if (has("create-null")) { return nullptr; }
    return make(Kind::Instrument, true);
}

int32_t instrumentPollState(void* h, CascadeInstrumentState* out) {
    Use use(h, "poll-state");
    misbehave("poll-state");
    out->flags = 0;
    out->seq = 1;
    for (double& v : out->values) { v = 1.0; }
    std::snprintf(out->text[0], CASCADE_INSTRUMENT_TEXT_CHARS, "hostile");
    if (has("poll-state-lie")) {
        for (double& v : out->values) { v = std::nan(""); }
        std::memset(out->text, 'S', sizeof(out->text));   // no terminators
        out->flags = 0xFFFFFFFFu;
        return 7;
    }
    return 1;
}

uint32_t instrumentColumns(void* h, char headings[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    Use use(h, "columns");
    misbehave("columns");
    std::snprintf(headings[0], CASCADE_PANEL_CELL_CHARS, "Column");
    if (has("columns-lie")) { return 99; }
    return 1;
}

int32_t instrumentPollRows(void* h, CascadePanelRow* out, uint32_t cap) { return panelPollRows(h, out, cap); }

void instrumentDestroy(void* h) {
    Use use(h, "destroy", true);
    misbehave("destroy");
}

CascadeInstrumentApi kInstrument = {
    static_cast<uint32_t>(sizeof(CascadeInstrumentApi)),
    "Hostile instrument",
    CASCADE_INSTRUMENT_GENERIC,
    &instrumentCreate,
    &instrumentPollState,
    &instrumentColumns,
    &instrumentPollRows,
    &instrumentDestroy,
};

// ---- the basemap -----------------------------------------------------------------------------------------------------------------------
std::uint8_t g_tile[256 * 256 * 3] = {0};

void* basemapCreate() {
    misbehave("create");
    if (has("create-null")) { return nullptr; }
    return make(Kind::Basemap, false);
}

int32_t basemapGetTile(void* h, uint32_t, uint32_t, uint32_t, CascadeTile* out) {
    Use use(h, "get-tile");
    misbehave("get-tile");
    out->width = 256;
    out->height = 256;
    out->format = CASCADE_IMAGE_RGB24;
    out->stride = 256 * 3;
    out->pixels = g_tile;
    if (has("get-tile-lie")) {
        // a whole 256 x 256 tile over a buffer that is readable for exactly 64 bytes and then ends at an unreadable page
        const char* edge = edgeOfAPage(64);
        out->pixels = edge != nullptr ? reinterpret_cast<const std::uint8_t*>(edge) : g_tinyBuffer;
    }
    if (has("get-tile-null")) { out->pixels = nullptr; }
    if (has("get-tile-wild")) { out->pixels = reinterpret_cast<const std::uint8_t*>(wildPointer()); }
    return CASCADE_TILE_READY;
}

void basemapReleaseTile(void* h, const CascadeTile*) {
    Use use(h, "release-tile");
    misbehave("release-tile");
}

int32_t basemapPollText(void* h, char* buf, size_t cap) {
    Use use(h, "poll-text");
    misbehave("poll-text");
    if (has("poll-text-lie")) {
        std::memset(buf, 'T', cap);
        return static_cast<int32_t>(cap + 100);
    }
    return 0;
}

void basemapDestroy(void* h) {
    Use use(h, "destroy", true);
    misbehave("destroy");
}

CascadeBasemapApi kBasemap = {
    static_cast<uint32_t>(sizeof(CascadeBasemapApi)), "Hostile tiles", 0u, 5u, 256u, &basemapCreate, &basemapGetTile, &basemapReleaseTile,
    &basemapPollText, &basemapDestroy,
};

// ---- the track-info client --------------------------------------------------------------------------------------------------------------
void* infoCreate() {
    misbehave("create");
    if (has("create-null")) { return nullptr; }
    return make(Kind::Info, false);
}

int32_t infoGet(void* h, const char*, uint32_t, CascadeTrackInfo* out) {
    Use use(h, "get-info");
    misbehave("get-info");
    out->registration = "G-HOST";
    out->typeCode = "A319";
    out->typeName = "Hostile 319";
    out->manufacturer = "Hostile";
    out->operatorName = "Hostile Air";
    out->country = "Nowhere";
    if (has("get-info-wild")) {
        out->registration = wildPointer();
        out->typeCode = wildPointer();
        out->typeName = wildPointer();
        out->manufacturer = wildPointer();
        out->operatorName = wildPointer();
        out->country = wildPointer();
    }
    if (has("get-info-unterminated")) {
        const char* edge = edgeOfAPage(8);
        if (edge != nullptr) {
            out->registration = edge;
            out->typeCode = edge;
            out->typeName = edge;
            out->manufacturer = edge;
            out->operatorName = edge;
            out->country = edge;
        }
    }
    return CASCADE_INFO_READY;
}

void infoRelease(void* h, const CascadeTrackInfo*) {
    Use use(h, "release-info");
    misbehave("release-info");
}

int32_t infoPollText(void* h, char* buf, size_t cap) {
    Use use(h, "poll-text");
    misbehave("poll-text");
    if (has("poll-text-lie")) {
        std::memset(buf, 'T', cap);
        return static_cast<int32_t>(cap + 100);
    }
    return 0;
}

void infoDestroy(void* h) {
    Use use(h, "destroy", true);
    misbehave("destroy");
}

const CascadeTrackInfoApi kInfo = {
    static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)), &infoCreate, &infoGet, &infoRelease, &infoPollText, &infoDestroy,
};

// ---- the host client --------------------------------------------------------------------------------------------------------------------
void hostAttach(const CascadeHostApi* host) {
    misbehave("attach");
    g_host = host;
}
const CascadeHostClientApi kHostClient = {static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &hostAttach};

// ---- the presets ------------------------------------------------------------------------------------------------------------------------
uint32_t presetCount() {
    misbehave("preset-count");
    if (has("preset-count-lie")) { return 1000000u; }
    return 1;
}

int32_t presetGet(uint32_t index, CascadePreset* out) {
    misbehave("preset-get");
    if (index != 0 && !has("preset-get-lie")) { return 0; }
    out->structSize = static_cast<uint32_t>(sizeof(CascadePreset));
    std::snprintf(out->label, sizeof(out->label), "Hostile preset");
    out->frequencyHz = 100.0e6;
    out->demodMode = CASCADE_DEMOD_NFM;
    out->bandwidthHz = 12500.0;
    out->sampleRateHz = 0.0;
    out->flags = 0;
    if (has("preset-get-lie")) {
        std::memset(out->label, 'L', sizeof(out->label));   // no terminator
        out->frequencyHz = std::nan("");
        out->demodMode = 0xFFFFFFFFu;
        out->bandwidthHz = -1.0e300;
    }
    return 1;
}
const CascadePresetApi kPreset = {static_cast<uint32_t>(sizeof(CascadePresetApi)), &presetCount, &presetGet};

// ---- the descriptor ---------------------------------------------------------------------------------------------------------------------
CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)), &kDecoder},
    {CASCADE_CAP_IMAGE_DECODER, static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)), &kImage},
    {CASCADE_CAP_IMAGE_INPUT, static_cast<uint32_t>(sizeof(CascadeImageInputApi)), &kImageInput},
    {CASCADE_CAP_TRACK_SOURCE, static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)), &kTracks},
    {CASCADE_CAP_PANEL, static_cast<uint32_t>(sizeof(CascadePanelApi)), &kPanel},
    {CASCADE_CAP_INSTRUMENT, static_cast<uint32_t>(sizeof(CascadeInstrumentApi)), &kInstrument},
    {CASCADE_CAP_BASEMAP, static_cast<uint32_t>(sizeof(CascadeBasemapApi)), &kBasemap},
    {CASCADE_CAP_TRACK_INFO, static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)), &kInfo},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
    {CASCADE_CAP_PRESET, static_cast<uint32_t>(sizeof(CascadePresetApi)), &kPreset},
};
constexpr std::size_t kCapabilityCount = sizeof(kCapabilities) / sizeof(kCapabilities[0]);

constexpr std::uint32_t kAllCaps = CASCADE_CAP_DECODER | CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_IMAGE_INPUT | CASCADE_CAP_TRACK_SOURCE |
                                   CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT | CASCADE_CAP_BASEMAP | CASCADE_CAP_TRACK_INFO |
                                   CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_PRESET;

// NOT const: the declared NAME is made from the module's own file name the first time the host asks (so a test that copies this one module
// under several names gets several plugins the host does not take for old copies of one another), and the corrupt descriptors below are
// made from it.
CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    HOSTILE_NAME,
    HOSTILE_VERSION,
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    kAllCaps,
    static_cast<uint32_t>(kCapabilityCount),
    kCapabilities,
};

#if defined(_WIN32)
HINSTANCE g_self = nullptr;
#endif

// This copy's file name: the leaf of the module's path.
void learnLeaf() {
    if (g_leaf[0] != '\0') { return; }
    const char* path = nullptr;
#if defined(_WIN32)
    char buf[MAX_PATH] = {0};
    if (g_self != nullptr && ::GetModuleFileNameA(g_self, buf, MAX_PATH) != 0) { path = buf; }
#else
    Dl_info info;
    if (::dladdr(reinterpret_cast<const void*>(&learnLeaf), &info) != 0) { path = info.dli_fname; }
#endif
    if (path == nullptr) { return; }
    const char* leaf = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') { leaf = p + 1; }
    }
    std::size_t n = 0;
    for (; leaf[n] != '\0' && n + 1 < sizeof(g_leaf); ++n) { g_leaf[n] = lower(leaf[n]); }
    g_leaf[n] = '\0';
    std::snprintf(g_name, sizeof(g_name), "%s %s", HOSTILE_NAME, leaf);
    kDesc.name = g_name;
}

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t hostAbiVersion) {
    learnLeaf();
    if (has("query-av")) { fault(); }
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    // The corrupt descriptors: each hands back something the host must refuse without dying.
    static CascadePluginDesc bad;
    static CascadeCapabilityEntry badEntries[16];
    static CascadePanelApi badPanel;
    static CascadeInstrumentApi badInstrument;
    static CascadeBasemapApi badBasemap;
    const bool lies = has("desc-wild-name") || has("desc-wild-tables") || has("entry-size") || has("entry-dup") ||
                      has("panel-title-wild") || has("instrument-title-wild") || has("basemap-attribution-wild") || has("caps-word-lie");
    if (!lies) { return &kDesc; }
    bad = kDesc;
    std::memcpy(badEntries, kCapabilities, sizeof(kCapabilities));
    bad.capabilityTables = badEntries;
    if (has("desc-wild-name")) { bad.name = wildPointer(); }
    if (has("desc-wild-tables")) { bad.capabilityTables = reinterpret_cast<const CascadeCapabilityEntry*>(static_cast<std::uintptr_t>(0x10)); }
    if (has("entry-size")) { badEntries[0].tableSize += 8u; }
    if (has("entry-dup")) {
        badEntries[bad.capabilityCount] = badEntries[0];
        ++bad.capabilityCount;
    }
    if (has("panel-title-wild")) {
        badPanel = kPanel;
        badPanel.title = wildPointer();
        badEntries[4].table = &badPanel;
    }
    if (has("instrument-title-wild")) {
        badInstrument = kInstrument;
        badInstrument.title = wildPointer();
        badEntries[5].table = &badInstrument;
    }
    if (has("basemap-attribution-wild")) {
        badBasemap = kBasemap;
        badBasemap.attribution = wildPointer();
        badEntries[6].table = &badBasemap;
    }
    // (the word says one capability, the entries say none: the audio-out bit with no entry for it)
    if (has("caps-word-lie")) { bad.capabilities |= CASCADE_CAP_AUDIO_OUT; }
    return &bad;
}

// ---- the host API, abused on the caller's thread --------------------------------------------------------------------------------------
//
//   op                  arg      does                                                                         answers
//   state               -        get_state                                                                    the call's code
//   settings-get        -        settings_get("k")                                                            the call's code
//   settings-set        -        settings_set("k", "v")                                                       the call's code
//   centre / rate / time / tune   the four calls older than level 1                                       the value (0 / FAILED once the host is gone)
//   settings-flood      n        settings_set of n distinct keys                                              how many calls were refused
//   command-flood       n        add_command of n distinct ids                                                how many calls were refused
//   marker-flood        n        set_marker of n distinct ids                                                 how many calls were refused
//   log-flood           n        n log lines                                                                  how many calls were refused
//   control-flood       n        set_volume n times                                                           how many calls were refused
//   bad-call            -        get_state(NULL), settings_get(NULL), add_command(0), set_marker(NULL), log(NULL)   how many answered BAD_ARGUMENT
//   log-one             -        one log line                                                                 the call's code
//   poll-command        -        poll_command                                                                 the call's code
//   thread-start        -        starts the thread that outlives destroy()                                    0
//   thread-stop         -        asks it to stop                                                              0
//   stat-thread-alive / stat-thread-calls / stat-thread-last / stat-host                                      the figure
extern "C" CASCADE_PLUGIN_EXPORT std::int64_t hostile_do(const char* op, std::int64_t arg) {
    if (op == nullptr) { return -1000; }
    const auto is = [&](const char* name) { return std::strcmp(op, name) == 0; };
    if (is("thread-start")) {
        startOutlivingThread();
        return 0;
    }
    if (is("thread-stop")) {
        g_threadStop.store(true);
        return 0;
    }
    if (is("stat-thread-alive")) { return g_threadAlive.load(); }
    if (is("stat-thread-calls")) { return static_cast<std::int64_t>(g_threadCalls.load()); }
    if (is("stat-thread-last")) { return g_threadLast.load(); }
    if (is("stat-host")) { return g_host != nullptr ? 1 : 0; }
    const CascadeHostApi* h = g_host;
    if (h == nullptr) { return -2000; }   // never attached
    void* ctx = h->ctx;
    if (is("state")) {
        CascadeReceiverState st;
        std::memset(&st, 0, sizeof(st));
        st.structSize = static_cast<uint32_t>(sizeof(st));
        return h->get_state(ctx, &st);
    }
    if (is("settings-get")) {
        char buf[16];
        return h->settings_get(ctx, "k", buf, sizeof(buf));
    }
    if (is("settings-set")) { return h->settings_set(ctx, "k", "v"); }
    // the four calls the table has had since before level 1 (they answer a value, not a code: 0 when the host is gone)
    if (is("centre")) { return static_cast<std::int64_t>(h->centre_hz(ctx)); }
    if (is("rate")) { return static_cast<std::int64_t>(h->sample_rate_hz(ctx)); }
    if (is("time")) { return h->unix_time_ms(ctx); }
    if (is("tune")) { return h->request_tune(ctx, 100.0e6); }
    if (is("log-one")) { return h->log(ctx, CASCADE_LOG_INFO, "hostile"); }
    if (is("poll-command")) {
        std::uint32_t id = 0;
        return h->poll_command(ctx, &id);
    }
    if (is("bad-call")) {
        std::int64_t bad = 0;
        bad += h->get_state(ctx, nullptr) == CASCADE_API_BAD_ARGUMENT ? 1 : 0;
        bad += h->settings_get(ctx, nullptr, nullptr, 0) == CASCADE_API_BAD_ARGUMENT ? 1 : 0;
        bad += h->add_command(ctx, 0u, "") == CASCADE_API_BAD_ARGUMENT ? 1 : 0;
        bad += h->set_marker(ctx, nullptr) == CASCADE_API_BAD_ARGUMENT ? 1 : 0;
        bad += h->log(ctx, CASCADE_LOG_INFO, nullptr) == CASCADE_API_BAD_ARGUMENT ? 1 : 0;
        return bad;
    }
    std::int64_t refused = 0;
    if (is("settings-flood")) {
        for (std::int64_t i = 0; i < arg; ++i) {
            char key[32];
            std::snprintf(key, sizeof(key), "flood.%lld", static_cast<long long>(i));
            if (h->settings_set(ctx, key, "v") != CASCADE_API_OK) { ++refused; }
        }
        return refused;
    }
    if (is("command-flood")) {
        for (std::int64_t i = 0; i < arg; ++i) {
            if (h->add_command(ctx, static_cast<uint32_t>(i + 1), "cmd") != CASCADE_API_OK) { ++refused; }
        }
        return refused;
    }
    if (is("marker-flood")) {
        for (std::int64_t i = 0; i < arg; ++i) {
            CascadeMarker m;
            std::memset(&m, 0, sizeof(m));
            m.structSize = static_cast<uint32_t>(sizeof(m));
            m.id = static_cast<uint32_t>(i + 1);
            m.kind = CASCADE_MARKER_POINT;
            m.freqHz = 100.0e6 + static_cast<double>(i);
            if (h->set_marker(ctx, &m) != CASCADE_API_OK) { ++refused; }
        }
        return refused;
    }
    if (is("log-flood")) {
        for (std::int64_t i = 0; i < arg; ++i) {
            if (h->log(ctx, CASCADE_LOG_INFO, "flood") != CASCADE_API_OK) { ++refused; }
        }
        return refused;
    }
    if (is("control-flood")) {
        for (std::int64_t i = 0; i < arg; ++i) {
            if (h->set_volume(ctx, 0.5) != CASCADE_API_OK) { ++refused; }
        }
        return refused;
    }
    return -3000;   // an op the fixture does not know
}

extern "C" CASCADE_PLUGIN_EXPORT std::size_t hostile_violations(char* text, std::size_t cap) {
    if (text != nullptr && cap > 0) {
        while (g_violationLock.exchange(1, std::memory_order_acquire) != 0) {}
        std::strncpy(text, g_violationText, cap - 1);
        text[cap - 1] = '\0';
        g_violationLock.store(0, std::memory_order_release);
    }
    return static_cast<std::size_t>(g_violations.load());
}

#if defined(_WIN32)
BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        if (has("dllmain-av")) { fault(); }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (has("dllmain-detach-av")) { fault(); }
    }
    return TRUE;
}
#endif
