// extio_client.cpp - see extio_client.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/extio_client.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

#include "core/file_kind.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cascade::source {

namespace ex = extio;
using Clock = std::chrono::steady_clock;

std::uint32_t ExtioClient::peMachine(const std::string& utf8Path) {
    std::ifstream in(core::fsPathFromUtf8(utf8Path), std::ios::binary);
    if (!in) { return 0; }
    std::uint8_t b[4096] = {0};
    in.read(reinterpret_cast<char*>(b), sizeof b);
    const std::size_t n = static_cast<std::size_t>(in.gcount());
    if (n < 0x40 || b[0] != 'M' || b[1] != 'Z') { return 0; }
    const std::uint32_t pe = ex::get32(b + 0x3C);
    if (pe > n - 6 || std::memcmp(b + pe, "PE\0\0", 4) != 0) { return 0; }
    return static_cast<std::uint32_t>(b[pe + 4]) | (static_cast<std::uint32_t>(b[pe + 5]) << 8);
}

#if defined(_WIN32)

struct ExtioClient::Impl {
    ExtioOptions opt;
    // process and pipe
    HANDLE pipeIn = INVALID_HANDLE_VALUE;    // helper to application (this side reads)
    HANDLE pipeOut = INVALID_HANDLE_VALUE;   // application to helper (this side writes)
    // TWO pipes, one per direction: a synchronous handle serialises its I/O, so a reader parked in ReadFile would block every WriteFile on the same handle.
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    std::thread reader;
    std::mutex writeMutex;
    // shared state, under m
    mutable std::mutex m;
    std::condition_variable cv;
    bool opened = false;
    bool started = false;
    bool readerDone = false;
    bool pipeBad = false;          // the helper's stream broke the protocol
    std::vector<ExtioFact> facts;
    // reply slot
    bool haveReply = false;
    ex::Msg replyCmd{};
    std::vector<std::uint8_t> replyPayload;
    // driver
    ex::InitReply init;
    std::uint32_t pairsPerCallback = 0;
    double sampleRate = 0.0;
    // ring
    std::vector<std::complex<float>> ring;
    std::size_t head = 0, count = 0;
    ExtioStats st;
    std::deque<ex::StatusEvent> events;
    // flood and silence
    Clock::time_point windowStart{}, lastData{};
    std::uint64_t windowPairs = 0;
    std::atomic<bool> killedByUs{false};
    bool closing = false;   // under m

    void raise(const char* code, std::string text) {   // m held
        for (const auto& f : facts) { if (f.code == code) { return; } }
        facts.push_back(ExtioFact{code, std::move(text)});
    }
    bool hasFatalFact() const { return !facts.empty(); }

    void killHelper() {
        killedByUs = true;
        if (process != nullptr) {
            ::TerminateProcess(process, 1);
            ::WaitForSingleObject(process, 3000);   // termination is asynchronous: when this returns the helper is gone
        }
    }
    bool helperAlive() const {
        if (process == nullptr) { return false; }
        return ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    }

    void onFrame(ex::Msg type, const std::uint8_t* p, std::size_t n) {
        std::unique_lock<std::mutex> lock(m);
        if (type == ex::Msg::Reply) {
            replyCmd = n > 0 ? static_cast<ex::Msg>(p[0]) : ex::Msg::Reply;
            replyPayload.assign(p, p + n);
            haveReply = true;
            cv.notify_all();
        } else if (type == ex::Msg::Data) {
            std::uint32_t pairs = 0;
            float off = 0;
            const std::uint8_t* iq = nullptr;
            if (!ex::decodeData(p, n, pairs, off, iq)) { ++st.framesBad; return; }
            if (!started) { return; }   // stragglers after stop
            const auto now = Clock::now();
            lastData = now;
            st.pairsReceived += pairs;
            // flood: more than maxRateFactor x the driver's own rate over a one-second window
            windowPairs += pairs;
            if (now - windowStart >= std::chrono::seconds(1)) {
                const double secs = std::chrono::duration<double>(now - windowStart).count();
                const double rate = static_cast<double>(windowPairs) / secs;
                const double limit = sampleRate > 0.0 ? sampleRate * opt.maxRateFactor : opt.absoluteMaxRateHz;
                if (rate > limit) {
                    raise(ex::kFactFlood, "the driver delivers samples far faster than its own sample rate");
                    cv.notify_all();
                }
                windowStart = now;
                windowPairs = 0;
            }
            if (hasFatalFact()) { return; }
            for (std::uint32_t i = 0; i < pairs; ++i) {
                if (count == ring.size()) { st.pairsOverrun += pairs - i; break; }
                float re, im;
                std::memcpy(&re, iq + static_cast<std::size_t>(i) * 8, 4);
                std::memcpy(&im, iq + static_cast<std::size_t>(i) * 8 + 4, 4);
                if (!std::isfinite(re)) { re = 0.0f; }
                if (!std::isfinite(im)) { im = 0.0f; }
                ring[(head + count) % ring.size()] = {std::clamp(re, -1.0f, 1.0f), std::clamp(im, -1.0f, 1.0f)};
                ++count;
            }
        } else if (type == ex::Msg::Status) {
            ex::StatusEvent ev;
            if (!ex::decodeStatus(p, n, ev)) { ++st.framesBad; return; }
            if (ev.status == ex::kStatusDropped) { st.helperDropped = static_cast<std::uint32_t>(ev.value); return; }
            if (ev.status == ex::kStatusBadCallbacks) { st.helperBadCallbacks = static_cast<std::uint32_t>(ev.value); return; }
            if (ev.status == 100 && ev.value > 0) { sampleRate = static_cast<double>(ev.value); }
            if (events.size() < 256) { events.push_back(ev); }
        } else {
            ++st.framesBad;
        }
    }

    void readerLoop() {
        ex::FrameReader fr;
        std::uint8_t buf[65536];
        for (;;) {
            DWORD got = 0;
            if (!::ReadFile(pipeIn, buf, sizeof buf, &got, nullptr) || got == 0) { break; }
            if (!fr.feed(buf, got, [this](ex::Msg t, const std::uint8_t* p, std::size_t n) { onFrame(t, p, n); })) {
                std::lock_guard<std::mutex> lock(m);
                pipeBad = true;
                raise(ex::kFactProtocol, "the helper sent something that is not the protocol (" + fr.error() + ")");
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(m);
            readerDone = true;
            // the helper's end of the pipe closed with nobody having asked it to end: it crashed or exited (a crash on the driver's own thread
            // is noticed here, not by a command)
            if (!pipeBad) { raiseExit(); }
        }
        cv.notify_all();
        if (pipeBad) { killHelper(); }
    }

    // Sends a command and waits for its reply. false: a fact has been raised (timeout, helper gone).
    bool call(ex::Msg cmd, const std::vector<std::uint8_t>& payload, int timeoutMs, std::vector<std::uint8_t>& reply) {
        {
            std::lock_guard<std::mutex> lock(m);
            haveReply = false;
            if (readerDone) { raiseExit(); return false; }
        }
        {
            std::lock_guard<std::mutex> wl(writeMutex);
            const std::vector<std::uint8_t> bytes = ex::frame(cmd, payload);
            std::size_t at = 0;
            while (at < bytes.size()) {
                DWORD wrote = 0;
                if (!::WriteFile(pipeOut, bytes.data() + at, static_cast<DWORD>(bytes.size() - at), &wrote, nullptr) || wrote == 0) {
                    std::lock_guard<std::mutex> lock(m);
                    raiseExit();
                    return false;
                }
                at += wrote;
            }
        }
        std::unique_lock<std::mutex> lock(m);
        const bool got = cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return (haveReply && replyCmd == cmd) || readerDone || hasFatalFact(); });
        if (haveReply && replyCmd == cmd) {
            reply = replyPayload;
            haveReply = false;
            return true;
        }
        if (readerDone) { raiseExit(); return false; }
        if (!got) {
            static const char* names[] = {"", "load", "open", "start", "stop", "close", "set-frequency", "set-mode", "quit", "show", "hide"};
            const unsigned idx = static_cast<unsigned>(cmd);
            raise(ex::kFactTimeout, std::string("the driver did not answer its ") + (idx < 11 ? names[idx] : "?") + " call within " +
                                        std::to_string(timeoutMs) + " ms; its helper was stopped");
            lock.unlock();
            killHelper();
        }
        return false;
    }

    void raiseExit() {   // m held
        if (killedByUs || closing) { return; }
        DWORD code = 0;
        if (process != nullptr) {
            ::WaitForSingleObject(process, 2000);
            ::GetExitCodeProcess(process, &code);
        }
        char buf[160];
        std::snprintf(buf, sizeof buf, "the driver's helper process ended (exit code 0x%08lX)", static_cast<unsigned long>(code));
        raise(ex::kFactHelperExited, buf);
    }

    void teardown() {
        if (process != nullptr && helperAlive()) { ::TerminateProcess(process, 1); }
        if (pipeIn != INVALID_HANDLE_VALUE) { ::CancelIoEx(pipeIn, nullptr); }
        if (pipeOut != INVALID_HANDLE_VALUE) { ::CancelIoEx(pipeOut, nullptr); }
        if (reader.joinable()) { reader.join(); }
        if (pipeIn != INVALID_HANDLE_VALUE) { ::CloseHandle(pipeIn); pipeIn = INVALID_HANDLE_VALUE; }
        if (pipeOut != INVALID_HANDLE_VALUE) { ::CloseHandle(pipeOut); pipeOut = INVALID_HANDLE_VALUE; }
        if (reader.joinable()) { reader.join(); }
        if (process != nullptr) { ::CloseHandle(process); process = nullptr; }
        if (job != nullptr) { ::CloseHandle(job); job = nullptr; }
    }
};

namespace {
std::wstring wideOf(const std::string& utf8) { return core::fsPathFromUtf8(utf8).native(); }
}  // namespace

ExtioClient::ExtioClient(ExtioOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->opt = std::move(options);
    impl_->ring.assign(std::max<std::size_t>(impl_->opt.ringPairs, 4096), {});
}
ExtioClient::~ExtioClient() { close(); }

bool ExtioClient::open() {
    Impl& I = *impl_;
    {
        std::lock_guard<std::mutex> lock(I.m);
        if (I.opened) { return true; }
        if (I.hasFatalFact()) { return false; }
        if (I.opt.dllPath.empty()) { I.raise(ex::kFactNotChosen, "no ExtIO driver has been chosen"); return false; }
    }
    // which helper: by the DLL's own bitness
    const std::uint32_t machine = peMachine(I.opt.dllPath);
    std::string helper;
    {
        std::lock_guard<std::mutex> lock(I.m);
        if (machine == 0) { I.raise(ex::kFactDllRefused, "the chosen file is not a Windows DLL"); return false; }
        if (machine != 0x8664 && machine != 0x14C) {
            I.raise(ex::kFactDllRefused, "the chosen driver is built for a processor this helper does not run on");
            return false;
        }
        helper = machine == 0x8664 ? I.opt.helperPath64 : I.opt.helperPath32;
        std::error_code ec;
        if (helper.empty() || !std::filesystem::exists(core::fsPathFromUtf8(helper), ec)) {
            I.raise(ex::kFactHelperMissing, machine == 0x14C ? "this driver is 32-bit and the 32-bit helper is not installed"
                                                             : "the 64-bit ExtIO helper is not installed");
            return false;
        }
    }
    // the pipe: a name nobody can guess, the first instance (nobody can squat it), local clients only
    std::random_device rd;
    wchar_t name[160];
    std::swprintf(name, 160, L"\\\\.\\pipe\\foxsdr-extio-%lu-%08x%08x", ::GetCurrentProcessId(), rd(), rd());
    const std::wstring base = name;
    const DWORD pmode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
    I.pipeOut = ::CreateNamedPipeW((base + L"-c").c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE, pmode, 1, 1u << 16, 1u << 16, 0, nullptr);
    I.pipeIn = ::CreateNamedPipeW((base + L"-d").c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE, pmode, 1, 1u << 16, 1u << 20, 0, nullptr);
    if (I.pipeIn == INVALID_HANDLE_VALUE || I.pipeOut == INVALID_HANDLE_VALUE) {
        std::lock_guard<std::mutex> lock(I.m);
        I.raise(ex::kFactHelperStart, "the pipe to the helper could not be created");
        I.teardown();
        return false;
    }
    I.job = ::CreateJobObjectW(nullptr, nullptr);
    if (I.job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(I.job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    std::wstring cmd = L"\"" + wideOf(helper) + L"\" --pipe " + name + L" --dll \"" + wideOf(I.opt.dllPath) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::lock_guard<std::mutex> lock(I.m);
        I.raise(ex::kFactHelperStart, "the helper process could not be started");
        I.teardown();
        return false;
    }
    I.process = pi.hProcess;
    if (I.job != nullptr) { ::AssignProcessToJobObject(I.job, pi.hProcess); }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);

    // wait for the helper to connect, without waiting for ever if it dies first
    std::atomic<int> connected{-1};
    std::thread connector([&] {
        BOOL ok = ::ConnectNamedPipe(I.pipeOut, nullptr);
        if (ok || ::GetLastError() == ERROR_PIPE_CONNECTED) {
            ok = ::ConnectNamedPipe(I.pipeIn, nullptr);
            connected = (ok || ::GetLastError() == ERROR_PIPE_CONNECTED) ? 1 : 0;
        } else {
            connected = 0;
        }
    });
    const auto deadline = Clock::now() + std::chrono::milliseconds(I.opt.initTimeoutMs);
    while (connected.load() < 0 && Clock::now() < deadline && I.helperAlive()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    if (connected.load() < 0) { ::CancelSynchronousIo(connector.native_handle()); }
    connector.join();
    if (connected.load() != 1) {
        std::lock_guard<std::mutex> lock(I.m);
        I.raise(ex::kFactHelperStart, "the helper process did not connect");
        I.teardown();
        return false;
    }
    I.reader = std::thread([&I] { I.readerLoop(); });

    std::vector<std::uint8_t> reply;
    if (!I.call(ex::Msg::Init, {}, I.opt.initTimeoutMs, reply)) { I.teardown(); return false; }
    ex::InitReply ir;
    if (!ex::decodeInitReply(reply.data(), reply.size(), ir)) {
        { std::lock_guard<std::mutex> lock(I.m); I.raise(ex::kFactProtocol, "the helper's answer to the load call was malformed"); }
        I.teardown();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(I.m);
        I.init = ir;
        if (!ir.missing.empty()) {
            I.raise(ex::kFactDllRefused, "the driver lacks required entry points: " + ir.missing);
        } else if (ir.result == 0 && ir.win32Error == ERROR_BAD_EXE_FORMAT) {
            I.raise(ex::kFactWrongBitness, "the driver could not be loaded by the helper (wrong bitness)");
        } else if (ir.result == 0 && ir.win32Error != 0) {
            I.raise(ex::kFactDllRefused, "the driver could not be loaded (Windows error " + std::to_string(ir.win32Error) + ")");
        } else if (ir.result == 0) {
            I.raise(ex::kFactDllRefused, "the driver reported that its hardware did not initialise");
        } else if (ex::bytesPerValue(ir.hwType) == 0) {
            I.raise(ex::kFactUnsupportedType, ir.hwType == 4 ? "this driver returns its samples through a sound card, which is not supported"
                                                              : "this driver declares a sample format this program does not know");
        }
        if (I.hasFatalFact()) {}
    }
    if (I.hasFatalFact()) {
        std::vector<std::uint8_t> ignore;
        I.call(ex::Msg::Close, {}, 1000, ignore);
        I.teardown();
        return false;
    }
    if (!I.call(ex::Msg::Open, {}, I.opt.initTimeoutMs, reply)) { I.teardown(); return false; }
    ex::SimpleReply sr;
    if (!ex::decodeSimpleReply(reply.data(), reply.size(), sr) || sr.result != 1) {
        { std::lock_guard<std::mutex> lock(I.m); I.raise(ex::kFactDllRefused, "the driver refused to open its hardware"); }
        I.teardown();
        return false;
    }
    std::lock_guard<std::mutex> lock(I.m);
    I.opened = true;
    return true;
}

bool ExtioClient::start(double centerHz) {
    Impl& I = *impl_;
    if (!open()) { return false; }
    {
        std::lock_guard<std::mutex> lock(I.m);
        if (I.hasFatalFact()) { return false; }
        if (I.started) { return true; }
    }
    std::vector<std::uint8_t> p, reply;
    const std::int64_t hz = static_cast<std::int64_t>(std::llround(centerHz));
    ex::put32(p, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::clamp<std::int64_t>(hz, INT32_MIN, INT32_MAX))));
    if (!I.call(ex::Msg::Start, p, I.opt.initTimeoutMs, reply)) { return false; }
    ex::SimpleReply sr;
    std::lock_guard<std::mutex> lock(I.m);
    if (!ex::decodeSimpleReply(reply.data(), reply.size(), sr)) { I.raise(ex::kFactProtocol, "the helper's answer to the start call was malformed"); return false; }
    switch (ex::judgeStartPairs(sr.result)) {
        case ex::StartVerdict::Ok: break;
        case ex::StartVerdict::Error:
            I.raise(ex::kFactDllRefused, "the driver reported an error when started (" + std::to_string(sr.result) + ")");
            return false;
        default:
            I.raise(ex::kFactBadSize, "the driver gave a block size of " + std::to_string(sr.result) + " I/Q pairs; the specification wants 512 or a multiple of it (and this program allows at most " +
                                          std::to_string(ex::kMaxPairsPerCallback) + ")");
            return false;
    }
    I.pairsPerCallback = static_cast<std::uint32_t>(sr.result);
    if (sr.extra > 0) { I.sampleRate = static_cast<double>(sr.extra); }
    I.head = I.count = 0;
    I.windowStart = I.lastData = Clock::now();
    I.windowPairs = 0;
    I.started = true;
    return true;
}

std::int32_t ExtioClient::setCenterHz(double hz) {
    Impl& I = *impl_;
    std::vector<std::uint8_t> p, reply;
    ex::put32(p, static_cast<std::uint32_t>(static_cast<std::int32_t>(std::clamp<std::int64_t>(std::llround(hz), INT32_MIN, INT32_MAX))));
    if (!I.call(ex::Msg::SetLo, p, I.opt.commandTimeoutMs, reply)) { return INT32_MIN; }
    ex::SimpleReply sr;
    if (!ex::decodeSimpleReply(reply.data(), reply.size(), sr)) { return INT32_MIN; }
    return sr.result;
}

void ExtioClient::stop() {
    Impl& I = *impl_;
    bool was;
    {
        std::lock_guard<std::mutex> lock(I.m);
        was = I.started;
        I.started = false;
        I.head = I.count = 0;
    }
    if (!was) { return; }
    std::vector<std::uint8_t> reply;
    I.call(ex::Msg::Stop, {}, I.opt.commandTimeoutMs, reply);
}

void ExtioClient::close() {
    Impl& I = *impl_;
    if (I.process == nullptr && !I.reader.joinable()) { return; }
    std::vector<std::uint8_t> reply;
    {
        std::lock_guard<std::mutex> lock(I.m);
        I.started = false;
        I.closing = true;   // from here the helper ending is what was asked for
    }
    if (I.helperAlive()) {
        bool fatal;
        { std::lock_guard<std::mutex> lock(I.m); fatal = I.hasFatalFact(); }
        if (!fatal) { I.call(ex::Msg::Close, {}, 2000, reply); }
        std::lock_guard<std::mutex> wl(I.writeMutex);
        const auto q = ex::frame(ex::Msg::Quit);
        DWORD w = 0;
        ::WriteFile(I.pipeOut, q.data(), static_cast<DWORD>(q.size()), &w, nullptr);
        ::WaitForSingleObject(I.process, 1000);
    }
    I.killedByUs = true;
    I.teardown();
    std::lock_guard<std::mutex> lock(I.m);
    I.opened = false;
}

std::size_t ExtioClient::read(std::complex<float>* dst, std::size_t n) {
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lock(I.m);
    if (I.started && !I.hasFatalFact()) {
        const auto now = Clock::now();
        if (I.count == 0 && now - I.lastData > std::chrono::milliseconds(I.opt.silenceMs)) {
            I.raise(ex::kFactSilent, "the driver has delivered no samples for " + std::to_string(I.opt.silenceMs) + " ms");
        }
    }
    if (I.hasFatalFact() && I.process != nullptr && I.helperAlive()) {
        // a fatal fact (flood, protocol, silence) ends the helper; never leave a misbehaving driver running
        ::TerminateProcess(I.process, 1);
        ::WaitForSingleObject(I.process, 3000);
        I.killedByUs = true;
    }
    if (I.hasFatalFact()) { return 0; }
    const std::size_t take = std::min(n, I.count);
    for (std::size_t i = 0; i < take; ++i) { dst[i] = I.ring[(I.head + i) % I.ring.size()]; }
    I.head = (I.head + take) % I.ring.size();
    I.count -= take;
    I.st.pairsRead += take;
    return take;
}

bool ExtioClient::isOpen() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->opened && !impl_->hasFatalFact(); }
bool ExtioClient::running() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->started && !impl_->hasFatalFact(); }
bool ExtioClient::faulted() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->hasFatalFact(); }
ExtioFact ExtioClient::fact() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->facts.empty() ? ExtioFact{} : impl_->facts.front(); }
std::vector<ExtioFact> ExtioClient::facts() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->facts; }
double ExtioClient::sampleRateHz() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->sampleRate; }
std::string ExtioClient::driverName() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->init.name; }
std::string ExtioClient::driverModel() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->init.model; }
std::uint32_t ExtioClient::hwType() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->init.hwType; }
ExtioStats ExtioClient::stats() const { std::lock_guard<std::mutex> lock(impl_->m); return impl_->st; }
std::vector<ex::StatusEvent> ExtioClient::takeStatusEvents() {
    std::lock_guard<std::mutex> lock(impl_->m);
    std::vector<ex::StatusEvent> out(impl_->events.begin(), impl_->events.end());
    impl_->events.clear();
    return out;
}
bool ExtioClient::helperAlive() const { return impl_->helperAlive(); }
std::uint32_t ExtioClient::helperExitCode() const {
    DWORD code = 0;
    if (impl_->process != nullptr) { ::GetExitCodeProcess(impl_->process, &code); }
    return static_cast<std::uint32_t>(code);
}

#else  // not Windows: ExtIO drivers are Windows DLLs; every call refuses with the reason

struct ExtioClient::Impl {
    ExtioOptions opt;
    std::vector<ExtioFact> facts;
};
ExtioClient::ExtioClient(ExtioOptions o) : impl_(std::make_unique<Impl>()) { impl_->opt = std::move(o); }
ExtioClient::~ExtioClient() = default;
bool ExtioClient::open() {
    if (impl_->facts.empty()) { impl_->facts.push_back(ExtioFact{ex::kFactHelperMissing, "ExtIO drivers are Windows DLLs"}); }
    return false;
}
bool ExtioClient::start(double) { return open(); }
std::int32_t ExtioClient::setCenterHz(double) { return INT32_MIN; }
void ExtioClient::stop() {}
void ExtioClient::close() {}
std::size_t ExtioClient::read(std::complex<float>*, std::size_t) { return 0; }
bool ExtioClient::isOpen() const { return false; }
bool ExtioClient::running() const { return false; }
bool ExtioClient::faulted() const { return !impl_->facts.empty(); }
ExtioFact ExtioClient::fact() const { return impl_->facts.empty() ? ExtioFact{} : impl_->facts.front(); }
std::vector<ExtioFact> ExtioClient::facts() const { return impl_->facts; }
double ExtioClient::sampleRateHz() const { return 0; }
std::string ExtioClient::driverName() const { return {}; }
std::string ExtioClient::driverModel() const { return {}; }
std::uint32_t ExtioClient::hwType() const { return 0; }
ExtioStats ExtioClient::stats() const { return {}; }
std::vector<ex::StatusEvent> ExtioClient::takeStatusEvents() { return {}; }
bool ExtioClient::helperAlive() const { return false; }
std::uint32_t ExtioClient::helperExitCode() const { return 0; }

#endif

// ---- ExtioSource -------------------------------------------------------------------------------------------------------------------------
ExtioSource::ExtioSource(ExtioOptions options) : client_(std::move(options)) {}

bool ExtioSource::start() {
    if (!client_.start(centerHz_)) {
        lastError_ = client_.fact().text;
        return false;
    }
    if (!client_.driverName().empty()) { name_ = "ExtIO: " + client_.driverName(); }
    lastError_.clear();
    return true;
}
void ExtioSource::stop() { client_.stop(); }
bool ExtioSource::running() const { return client_.running(); }
double ExtioSource::sampleRateHz() const { return client_.sampleRateHz(); }
bool ExtioSource::setSampleRateHz(double) {
    lastError_ = "the sample rate of an ExtIO radio is set in the driver's own window";
    return false;
}
bool ExtioSource::setCenterFrequencyHz(double hz) {
    centerHz_ = hz;
    if (!client_.running()) { return true; }   // applied by start()
    const std::int32_t r = client_.setCenterHz(hz);
    if (r == 0) { return true; }
    if (r == INT32_MIN) { lastError_ = client_.fact().text; }
    else if (r < 0) { lastError_ = "below the lowest frequency this radio can tune (" + std::to_string(-static_cast<std::int64_t>(r)) + " Hz)"; }
    else { lastError_ = "above the highest frequency this radio can tune (" + std::to_string(r) + " Hz)"; }
    return false;
}
std::size_t ExtioSource::read(std::complex<float>* dst, std::size_t n) { return client_.read(dst, n); }
bool ExtioSource::faulted() const { return client_.faulted(); }

}  // namespace cascade::source
