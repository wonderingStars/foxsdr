// test_tx_window_threads.cpp - the plugin-key lease's state is safe against a thread that closes the window beside the pass (stub campaign B20's
// ThreadSanitizer finding, docs/engine-stage4.md 6.23 finding 1; fixed in the commit that adds this file).
//
// THE FINDING. Engine::txPluginEnd -> txPluginDisarm closes the arming window and zeroes its owner. In the product that runs on the pass (or with no pass
// beside it: the config's load before the control thread starts, the shutdown after it is joined). A TEST, though, may call Engine::applyCommand directly
// from its own thread while the control thread free-runs (tests/test_web_pictures.cpp: e.applyCommand(RUN 0)), and then the stop's txPluginEnd wrote
// Engine::txArmOwner_ - a plain integer the pass reads every key drive - from the wrong thread. ThreadSanitizer, 4 runs of 4. The window itself
// (core::TxArming: plain fields and a std::string) had the same hazard behind the owner's, and the page's stamp and the published refusal the same again.
// The three scalars are std::atomic now and the window sits behind a lock (Engine::TxWindow, engine.hpp). This file holds both, the way the finding found them.
//
//   A  TxWindow does what core::TxArming does, one thread: the rules are master's and the wrapper adds nothing to them
//      (arm, refused arm, update and its expiry, disarm and its reason, the reason kept by a second disarm).
//   B  THE SEAM, with a real Engine and its control thread free-running: this thread opens the window for a session that does not exist and ends it
//      (txPluginEnd, the thing applyCommand's stop reaches) over and over, moving the owner, the page's stamp and the published refusal as the arm op does,
//      while the pass's own drive reads all of it and - finding the session gone - closes the window itself, and a third thread reads the reason. Nothing
//      is asserted about WHICH side closed it each round (that is the race); what is asserted is that no reader ever saw a torn value and that the window is
//      closed and the key down when the pass has stopped. THE POINT OF THE FILE IS WHAT ThreadSanitizer SAYS (tests/tsan.cmake selects it): natively a plain
//      field or a missing lock passes this too, because the hazard is a data race and not a wrong answer - take the lock out of Engine::TxWindow, or make
//      txArmOwner_ plain again, and the TSan run reports it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/tx_audio.hpp"
#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "test_check.hpp"

using cascade::core::TxArming;
using cascade::engine::Engine;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

// Every per-user folder at a scratch tree: nothing the engine writes can reach the real profile.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
    char buf[MAX_PATH];
    GetTempPathA(MAX_PATH, buf);
    const std::string tmp = std::string(buf) + "foxsdr_tx_window_" + std::to_string(pid);
#else
    const int pid = static_cast<int>(getpid());
    const std::string tmp = "/tmp/foxsdr_tx_window_" + std::to_string(pid);
#endif
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, tmp);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

// Long enough to live on the heap (a short std::string is inside the object and tears differently): a read that tears is a crash or a wrong word.
const char* const kRoundReason = "test: this thread closed the window beside the pass (a reason longer than any small-string buffer)";
const char* const kRanOut = "the arming window ran out";

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Window = Engine::TxWindow;
    static std::uint64_t kOwner() { return 7; }   // a session id nobody holds: the pass finds its lease lost and closes the window itself
    // The arm op's effects on the lease, made from THIS thread (txPluginArmOp does the same from the pass): the window, then the stamp, then the owner.
    static void openWindow(Engine& e, std::int64_t nowMs) {
        (void)e.txArming_.arm(5, true, true, TxArming::Clock::now());
        e.txArmRefusal_.store(TxArming::Refusal::NoGrant, std::memory_order_seq_cst);
        e.txArmLiveMs_.store(nowMs, std::memory_order_seq_cst);
        e.txArmOwner_.store(kOwner(), std::memory_order_seq_cst);
    }
    static void end(Engine& e, const char* why) { e.txPluginEnd(why); }
    static bool armed(Engine& e) { return e.txArming_.armed(); }
    static std::string reason(Engine& e) { return e.txArming_.lastReason(); }
    static std::uint64_t owner(Engine& e) { return e.txArmOwner_.load(std::memory_order_seq_cst); }
    static bool keyed(Engine& e) { return e.transmitter_.pluginKeyRequested() || e.transmitter_.pluginKeyed(); }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_tx_window_threads\n");
    isolate();

    // --- A: the wrapper is the rule, one thread -------------------------------------------------------------------------------------------------------
    {
        A::Window w;
        const auto now = TxArming::Clock::now();
        CHECK(!w.armed());
        CHECK(w.lastReason().empty());
        CHECK(w.minutes() == 0);
        CHECK(w.secondsLeft(now) == 0);
        CHECK(w.arm(10, true, true, now) == TxArming::Refusal::BadMinutes);    // not one of the offered times
        CHECK(w.arm(5, false, true, now) == TxArming::Refusal::NoRadio);
        CHECK(w.arm(5, true, false, now) == TxArming::Refusal::NoGrant);
        CHECK(!w.armed());
        CHECK(w.arm(15, true, true, now) == TxArming::Refusal::None);
        CHECK(w.armed());
        CHECK(w.minutes() == 15);
        CHECK(w.secondsLeft(now) == 900);
        CHECK(w.update(now + std::chrono::minutes(1)));                        // still open a minute in
        CHECK(w.disarm(kRoundReason));                                         // true: it was open
        CHECK(!w.armed());
        CHECK(w.lastReason() == kRoundReason);
        CHECK(!w.disarm("a second close says nothing"));                       // false: it was not, and the reason of the real close stays
        CHECK(w.lastReason() == kRoundReason);
        CHECK(w.arm(5, true, true, now) == TxArming::Refusal::None);
        CHECK(w.lastReason().empty());                                         // a window that is open has no reason for closing
        bool expired = false;
        CHECK(!w.update(now + std::chrono::minutes(6), &expired));             // its time has come
        CHECK(expired);
        CHECK(!w.armed());
        CHECK(w.lastReason() == kRanOut);
        std::printf("A: the window forwards core::TxArming's rules, one thread\n");
    }

    // --- B: the seam, a real engine, the pass free-running ---------------------------------------------------------------------------------------------
    {
        Engine e;
        e.initialise();
        cascade::engine::ControlThreadOptions opts;
        opts.period = std::chrono::milliseconds(1);
        opts.watchdogThresholdMs = 5000;
        e.startControlThread(opts);

        std::atomic<bool> stop{false};
        std::atomic<long> torn{0};
        std::atomic<long> reads{0};
        std::thread reader([&] {
            while (!stop.load()) {
                const std::string r = A::reason(e);
                // Whatever the last close said, whole: nothing, this thread's own reason, or the pass's (no radio is open in this engine, so the drive's first
                // reason is "the transmit radio was closed"; the lease's and the window's own time are the other two it could give). A torn string is none of them.
                const bool whole = r.empty() || r == kRoundReason || r == "the transmit radio was closed" || r == "the window that armed this was closed" || r == kRanOut;
                if (!whole) { ++torn; }
                const std::uint64_t o = A::owner(e);
                if (o != 0 && o != A::kOwner()) { ++torn; }
                ++reads;
                std::this_thread::yield();
            }
        });

        constexpr int kRounds = 600;
        for (int i = 0; i < kRounds; ++i) {
            A::openWindow(e, cascade::core::TxAudioLink::nowMs());
            if ((i & 3) == 0) { std::this_thread::sleep_for(std::chrono::microseconds(300)); }   // let the pass in between, now and then
            A::end(e, kRoundReason);
            if ((i & 7) == 0) { std::this_thread::yield(); }
        }
        stop.store(true);
        reader.join();
        e.stopControlThread();   // the loop's last act (ApiHost::engineStopping) ends any window the pass still held

        std::printf("B: %d rounds of open + end beside the pass, %ld reads on a third thread, %ld torn\n", kRounds, reads.load(), torn.load());
        CHECK(torn.load() == 0);
        CHECK(!A::armed(e));
        CHECK(A::owner(e) == 0);
        CHECK(!A::keyed(e));
    }

    return testSummary("test_tx_window_threads");
}
