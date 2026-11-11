// extio_helper.cpp - the ExtIO helper process (row R8). Loads ONE ExtIO driver DLL the user chose, calls its documented entry points and forwards
// samples and events to FoxSDR over a named pipe (source/extio_protocol.hpp). FoxSDR itself never loads the DLL.
//
//   extio_helper.exe --pipe \\.\pipe\<name> --dll <path to ExtIO_*.dll>
//
// WHAT THIS PROCESS IS FOR: to be the one that crashes, hangs or floods. It runs the DLL's code, so it runs with the user's rights (the warning
// the settings screen shows says so). It is started only after the user picks a DLL, is killed with FoxSDR (a job object on the application's
// side), and exits by itself when the pipe closes.
//
// THREADS. The main thread owns every call into the DLL (a DLL that opens a window needs the thread that made it to pump messages, so the main
// thread runs a message loop and executes commands posted by the pipe-reader thread). The DLL's own callback thread(s) only convert and queue
// samples; a writer thread puts them on the pipe. A callback that finds the queue full waits a few milliseconds and then drops (counted), so a
// slow application slows a flooding DLL down rather than eating memory.
//
// BITNESS. Most ExtIO DLLs are 32-bit. A DLL can only be loaded by a process of its own bitness, so the same source is built as a 32-bit and a
// 64-bit helper and the application picks by the DLL's PE header.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source/extio_protocol.hpp"

namespace ex = cascade::source::extio;

namespace {

HANDLE g_pipeIn = INVALID_HANDLE_VALUE;    // commands from the application (this side reads)
HANDLE g_pipeOut = INVALID_HANDLE_VALUE;   // samples and replies (this side writes); two pipes because a synchronous handle serialises its I/O
std::mutex g_writeMutex;
DWORD g_mainThread = 0;

bool writeAll(const std::vector<std::uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(g_writeMutex);
    std::size_t at = 0;
    while (at < bytes.size()) {
        DWORD wrote = 0;
        if (!::WriteFile(g_pipeOut, bytes.data() + at, static_cast<DWORD>(bytes.size() - at), &wrote, nullptr) || wrote == 0) { return false; }
        at += wrote;
    }
    return true;
}
void send(ex::Msg type, const std::vector<std::uint8_t>& payload) {
    if (!writeAll(ex::frame(type, payload))) { ::TerminateProcess(::GetCurrentProcess(), 3); }   // the application is gone
}

// ---- the DLL -----------------------------------------------------------------------------------------------------------------------------
using CallbackFn = void (*)(int, int, float, void*);
using InitFn = bool(__stdcall*)(char*, char*, int&);
using OpenFn = bool(__stdcall*)();
using StartFn = int(__stdcall*)(long);
using VoidFn = void(__stdcall*)();
using SetLoFn = int(__stdcall*)(long);
using GetLongFn = long(__stdcall*)();
using GetCharFn = char(__stdcall*)();
using ModeFn = void(__stdcall*)(char);
using SetCbFn = void(__stdcall*)(CallbackFn);
using GetStatusFn = int(__stdcall*)();

struct Dll {
    HMODULE module = nullptr;
    InitFn initHW = nullptr;
    OpenFn openHW = nullptr;
    StartFn startHW = nullptr;
    VoidFn stopHW = nullptr, closeHW = nullptr, showGUI = nullptr, hideGUI = nullptr;
    SetLoFn setHWLO = nullptr;
    GetStatusFn getStatus = nullptr;
    SetCbFn setCallback = nullptr;
    GetLongFn getHWLO = nullptr, getHWSR = nullptr, getTune = nullptr;
    GetCharFn getMode = nullptr;
    ModeFn modeChanged = nullptr;
    std::uint32_t hwType = 0;
} g_dll;

// The specification's names, plain first and then the decorated form a 32-bit stdcall DLL built without a .DEF file would export.
FARPROC entry(const char* name, int stackBytes) {
    FARPROC p = ::GetProcAddress(g_dll.module, name);
    if (p != nullptr) { return p; }
    char decorated[96];
    std::snprintf(decorated, sizeof decorated, "_%s@%d", name, stackBytes);
    return ::GetProcAddress(g_dll.module, decorated);
}

// ---- sample queue ------------------------------------------------------------------------------------------------------------------------
struct Item {
    bool isStatus = false;
    std::int32_t status = 0;
    std::vector<std::complex<float>> iq;
    float iqOffset = 0.0f;
};
std::mutex g_qMutex;
std::condition_variable g_qNotEmpty, g_qNotFull;
std::deque<Item> g_queue;
constexpr std::size_t kMaxQueued = 48;
std::atomic<bool> g_running{false};     // StartHW succeeded, StopHW not yet called
std::atomic<std::uint32_t> g_pairs{0};  // StartHW's return value
std::atomic<std::uint32_t> g_dropped{0}, g_bad{0};
std::atomic<bool> g_quit{false};

bool safeConvert(std::uint32_t type, const void* data, std::uint32_t pairs, std::complex<float>* out) {
    __try {
        return ex::convertSamples(type, static_cast<const std::uint8_t*>(data), pairs, reinterpret_cast<float*>(out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;   // a nonsense pointer from the DLL: counted, not fatal
    }
}

void push(Item&& it) {
    std::unique_lock<std::mutex> lock(g_qMutex);
    if (g_queue.size() >= kMaxQueued) {
        if (!g_qNotFull.wait_for(lock, std::chrono::milliseconds(20), [] { return g_queue.size() < kMaxQueued || g_quit.load(); })) {
            ++g_dropped;
            return;
        }
    }
    g_queue.push_back(std::move(it));
    g_qNotEmpty.notify_one();
}

// THE CALLBACK the DLL calls (the specification's `void extIOCallback(int cnt, int status, float IQoffs, void* IQdata)`).
void extioCallback(int cnt, int status, float iqOffs, void* data) {
    if (cnt < 0) {
        Item it;
        it.isStatus = true;
        it.status = status;
        push(std::move(it));
        return;
    }
    if (!g_running.load()) { return; }
    const std::uint32_t pairs = g_pairs.load();
    if (pairs == 0 || data == nullptr) { ++g_bad; return; }
    Item it;
    it.iq.resize(pairs);
    if (!safeConvert(g_dll.hwType, data, pairs, it.iq.data())) { ++g_bad; return; }
    it.iqOffset = iqOffs;
    push(std::move(it));
}

std::int32_t queryFor(std::int32_t status) {
    switch (status) {
        case 100: return g_dll.getHWSR != nullptr ? static_cast<std::int32_t>(g_dll.getHWSR()) : 0;
        case 101:
        case 104: return g_dll.getHWLO != nullptr ? static_cast<std::int32_t>(g_dll.getHWLO()) : 0;
        case 105: return g_dll.getTune != nullptr ? static_cast<std::int32_t>(g_dll.getTune()) : 0;
        case 106: return g_dll.getMode != nullptr ? static_cast<std::int32_t>(static_cast<unsigned char>(g_dll.getMode())) : 0;
        default: return 0;
    }
}

void writerThread() {
    std::uint32_t reportedDropped = 0, reportedBad = 0;
    auto lastReport = std::chrono::steady_clock::now();
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> lock(g_qMutex);
            g_qNotEmpty.wait_for(lock, std::chrono::milliseconds(250), [] { return !g_queue.empty() || g_quit.load(); });
            if (g_quit.load()) { return; }
            if (!g_queue.empty()) {
                it = std::move(g_queue.front());
                g_queue.pop_front();
                g_qNotFull.notify_one();
            }
        }
        if (it.isStatus) {
            send(ex::Msg::Status, ex::encodeStatus(it.status, queryFor(it.status)));
        } else if (!it.iq.empty()) {
            send(ex::Msg::Data, ex::encodeData(it.iq.data(), static_cast<std::uint32_t>(it.iq.size()), it.iqOffset));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(1)) {
            lastReport = now;
            const std::uint32_t d = g_dropped.load(), b = g_bad.load();
            if (d != reportedDropped) { reportedDropped = d; send(ex::Msg::Status, ex::encodeStatus(ex::kStatusDropped, static_cast<std::int32_t>(d))); }
            if (b != reportedBad) { reportedBad = b; send(ex::Msg::Status, ex::encodeStatus(ex::kStatusBadCallbacks, static_cast<std::int32_t>(b))); }
        }
    }
}

// ---- commands (run on the main thread) ---------------------------------------------------------------------------------------------------
std::wstring g_dllPath;

void doInit() {
    ex::InitReply r;
    r.result = 0;
    g_dll.module = ::LoadLibraryExW(g_dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_dll.module == nullptr) {
        r.win32Error = ::GetLastError();
        send(ex::Msg::Reply, ex::encodeInitReply(r));
        return;
    }
    g_dll.initHW = reinterpret_cast<InitFn>(entry("InitHW", 12));
    g_dll.openHW = reinterpret_cast<OpenFn>(entry("OpenHW", 0));
    g_dll.startHW = reinterpret_cast<StartFn>(entry("StartHW", 4));
    g_dll.stopHW = reinterpret_cast<VoidFn>(entry("StopHW", 0));
    g_dll.closeHW = reinterpret_cast<VoidFn>(entry("CloseHW", 0));
    g_dll.setHWLO = reinterpret_cast<SetLoFn>(entry("SetHWLO", 4));
    g_dll.getStatus = reinterpret_cast<GetStatusFn>(entry("GetStatus", 0));
    g_dll.setCallback = reinterpret_cast<SetCbFn>(entry("SetCallback", 4));
    g_dll.getHWLO = reinterpret_cast<GetLongFn>(entry("GetHWLO", 0));
    g_dll.getHWSR = reinterpret_cast<GetLongFn>(entry("GetHWSR", 0));
    g_dll.getTune = reinterpret_cast<GetLongFn>(entry("GetTune", 0));
    g_dll.getMode = reinterpret_cast<GetCharFn>(entry("GetMode", 0));
    g_dll.modeChanged = reinterpret_cast<ModeFn>(entry("ModeChanged", 4));
    g_dll.showGUI = reinterpret_cast<VoidFn>(entry("ShowGUI", 0));
    g_dll.hideGUI = reinterpret_cast<VoidFn>(entry("HideGUI", 0));
    const struct { const char* name; bool have; } mandatory[] = {
        {"InitHW", g_dll.initHW != nullptr},     {"OpenHW", g_dll.openHW != nullptr},       {"StartHW", g_dll.startHW != nullptr},
        {"StopHW", g_dll.stopHW != nullptr},     {"CloseHW", g_dll.closeHW != nullptr},     {"SetHWLO", g_dll.setHWLO != nullptr},
        {"GetStatus", g_dll.getStatus != nullptr}, {"SetCallback", g_dll.setCallback != nullptr}};
    for (const auto& m : mandatory) {
        if (!m.have) {
            if (!r.missing.empty()) { r.missing += ","; }
            r.missing += m.name;
        }
    }
    if (!r.missing.empty()) {
        send(ex::Msg::Reply, ex::encodeInitReply(r));
        return;
    }
    char name[256] = {0}, model[256] = {0};
    int type = 0;
    const bool ok = g_dll.initHW(name, model, type);
    name[255] = model[255] = 0;
    r.result = ok ? 1 : 0;
    r.hwType = static_cast<std::uint32_t>(type);
    g_dll.hwType = r.hwType;
    r.name = name;
    r.model = model;
    r.exports = (g_dll.getHWLO ? ex::kExportGetHWLO : 0u) | (g_dll.getHWSR ? ex::kExportGetHWSR : 0u) | (g_dll.getTune ? ex::kExportGetTune : 0u) |
                (g_dll.getMode ? ex::kExportGetMode : 0u) | (g_dll.showGUI ? ex::kExportShowGui : 0u);
    if (ok) { g_dll.setCallback(extioCallback); }
    send(ex::Msg::Reply, ex::encodeInitReply(r));
}

void execute(ex::Msg cmd, const std::vector<std::uint8_t>& payload) {
    switch (cmd) {
        case ex::Msg::Init: doInit(); break;
        case ex::Msg::Open: {
            const bool ok = g_dll.openHW != nullptr && g_dll.openHW();
            send(ex::Msg::Reply, ex::encodeReply(cmd, ok ? 1 : 0));
            break;
        }
        case ex::Msg::Start: {
            const long freq = payload.size() >= 4 ? static_cast<long>(static_cast<std::int32_t>(ex::get32(payload.data()))) : 0;
            const int pairs = g_dll.startHW(freq);
            std::uint32_t sr = 0;
            if (ex::judgeStartPairs(pairs) == ex::StartVerdict::Ok) {
                g_pairs = static_cast<std::uint32_t>(pairs);
                g_running = true;
                if (g_dll.getHWSR != nullptr) { sr = static_cast<std::uint32_t>(g_dll.getHWSR()); }
            } else {
                g_dll.stopHW();   // a size no host may run with: stop the driver again, report the number it gave
            }
            send(ex::Msg::Reply, ex::encodeReply(cmd, pairs, sr));
            break;
        }
        case ex::Msg::Stop:
            g_running = false;
            g_dll.stopHW();
            send(ex::Msg::Reply, ex::encodeReply(cmd, 0));
            break;
        case ex::Msg::Close:
            g_running = false;
            g_dll.closeHW();
            send(ex::Msg::Reply, ex::encodeReply(cmd, 0));
            break;
        case ex::Msg::SetLo: {
            const long freq = payload.size() >= 4 ? static_cast<long>(static_cast<std::int32_t>(ex::get32(payload.data()))) : 0;
            send(ex::Msg::Reply, ex::encodeReply(cmd, g_dll.setHWLO(freq)));
            break;
        }
        case ex::Msg::SetMode:
            if (g_dll.modeChanged != nullptr && !payload.empty()) { g_dll.modeChanged(static_cast<char>(payload[0])); }
            send(ex::Msg::Reply, ex::encodeReply(cmd, 0));
            break;
        case ex::Msg::ShowGui:
            if (g_dll.showGUI != nullptr) { g_dll.showGUI(); }
            send(ex::Msg::Reply, ex::encodeReply(cmd, 0));
            break;
        case ex::Msg::HideGui:
            if (g_dll.hideGUI != nullptr) { g_dll.hideGUI(); }
            send(ex::Msg::Reply, ex::encodeReply(cmd, 0));
            break;
        case ex::Msg::Quit:
            g_quit = true;
            g_qNotEmpty.notify_all();
            g_qNotFull.notify_all();
            ::TerminateProcess(::GetCurrentProcess(), 0);   // no FreeLibrary / DLL_PROCESS_DETACH: a vendor DLL may hang there
            break;
        default: break;   // an unknown command is ignored
    }
}

// Commands from the reader thread to the main thread.
struct Command {
    ex::Msg type;
    std::vector<std::uint8_t> payload;
};
std::mutex g_cmdMutex;
std::deque<Command> g_cmds;
constexpr UINT kWakeMain = WM_APP + 1;
HWND g_wake = nullptr;   // a message-only window: its messages are dispatched by ANY message loop on the main thread, including a modal one a driver runs

void drainCommands() {
    for (;;) {
        Command c;
        {
            std::lock_guard<std::mutex> lock(g_cmdMutex);
            if (g_cmds.empty()) { return; }
            c = std::move(g_cmds.front());
            g_cmds.pop_front();
        }
        execute(c.type, c.payload);
    }
}
LRESULT CALLBACK wakeProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == kWakeMain) { drainCommands(); return 0; }
    return ::DefWindowProcW(h, msg, w, l);
}

void readerThread() {
    ex::FrameReader reader;
    std::uint8_t buf[8192];
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(g_pipeIn, buf, sizeof buf, &got, nullptr) || got == 0) { break; }   // the application closed the pipe
        const bool ok = reader.feed(buf, got, [](ex::Msg t, const std::uint8_t* p, std::size_t n) {
            {
                std::lock_guard<std::mutex> lock(g_cmdMutex);
                g_cmds.push_back(Command{t, std::vector<std::uint8_t>(p, p + n)});
            }
            ::PostMessageW(g_wake, kWakeMain, 0, 0);
        });
        if (!ok) { break; }
    }
    ::TerminateProcess(::GetCurrentProcess(), 4);   // pipe closed or garbled: nothing to serve any more
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    std::wstring pipe;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--pipe") { pipe = argv[i + 1]; }
        if (std::wstring(argv[i]) == L"--dll") { g_dllPath = argv[i + 1]; }
    }
    if (pipe.empty() || g_dllPath.empty()) { return 2; }
    g_pipeIn = ::CreateFileW((pipe + L"-c").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_pipeIn == INVALID_HANDLE_VALUE) { return 3; }
    g_pipeOut = ::CreateFileW((pipe + L"-d").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (g_pipeOut == INVALID_HANDLE_VALUE) { return 3; }
    g_mainThread = ::GetCurrentThreadId();
    MSG m;
    WNDCLASSW wc{};
    wc.lpfnWndProc = wakeProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FoxSdrExtioHelperWake";
    ::RegisterClassW(&wc);
    g_wake = ::CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (g_wake == nullptr) { return 5; }
    std::thread writer(writerThread);
    std::thread reader(readerThread);
    writer.detach();
    reader.detach();
    while (::GetMessageW(&m, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&m);
        ::DispatchMessageW(&m);
    }
    return 0;
}
