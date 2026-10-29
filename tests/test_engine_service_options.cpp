// THE ENGINE SERVICE GROWS: every option a second host needs, and every default pinned (shell retirement R6; docs/engine-stage4.md 6.25).
//
// The window application is to become a SECOND HOST of EngineService (R7, R8): the engine's life - lock, restore, start, keep, stop - is written
// once, in src/engine/engine_service.cpp, and the window sets the options it needs. R6 adds them. The rule that makes it safe is that THE
// HEADLESS BINARY IS UNHARMED: every option defaults to exactly what foxsdr-engine has always done, and each default is pinned HERE, on a
// default-constructed service, beside the 13 test_engine_main_* files that run the real binary (untouched, and green).
//
//   0  THE DEFAULTS, field by field (ServiceOptions{} is foxsdr-engine);
//   1  LOCK: Strict refuses a second program (the sentence names the holder's role - the label); BestEffort carries on and logs the line main.cpp
//      always logged, holds the lock when it can, and releaseConfig() lets go (the window's relaunch releases it before the second process starts);
//   2  HERMETIC: Headless mutes and selects the generator; Window leaves the engine as built (volume 0.5);
//   3  RECEIVE and STEPPED: the receiver starts (receive) or does not; the pass free-runs with a first pass made by start() or is stepped with none;
//   4  HEARTBEAT: armed by the option and by nothing else (a persistent service has the install id it needs);
//   5  KEEPER THREAD: a change is saved with nobody calling keep() (and not without the option); stop() joins it; keep() from outside is a no-op while it runs;
//   6  THE KEEPER'S COMPARE leaves out every self-moving field, of BOTH halves (T-a, survey 5.4): an interface half whose tester report moves every
//      100 ms makes no save in 4 s, and rides along in the save a real change makes;
//   7  STALL SEAMS: shutdownStallMs wedges between the final-state save and the receiver's stop (the config on disk is the final state, the marker
//      still false, while wedged); unloadStallMs between the release and the marker; the lines carry the label and go to stderr, or to stdout;
//   8  LABEL on the lines the service writes; PLUGIN STATUS report once, only when asked; BREADCRUMB phases in their places, only when asked;
//   9  THE ENGINE'S OWN SURFACE the host needs: configReadback() public; the engine's host is the one it was built with, for life (R9: Engine::setHost /
//      resetHost, the R6 pointer contract, are gone with the window's adapter - the engine asks the host it was given, and a host that is gone is
//      never called by the destructor).
//  10  WHAT THE WINDOW HOST NEEDS OF THE RESTORE (R7: the window no longer reads the file): restore() returns the last run's marker, the saved source as
//      the file named it, the read-back and whether writes are forbidden; announce() prints the CASCADE_CONFIG_TEST line; a service destroyed lets
//      the settings lock go (src/main.cpp relies on it across the software-GL relaunch).
//  11  THE FINAL PASS (R8): a STEPPED service has no pass of its own, so what an interface submits last is applied only if stop() makes one: finalPass
//      does, before the quiesce and the final-state save, and the default (the headless binary) does not.
//
// Break-it (docs/engine-stage4.md 6.25): the lock default Strict -> BestEffort: 1 and test_engine_main_second_instance_refused RED; the hermetic default
// Headless -> Window: 2 RED; comparable() without the mask: 6 RED; the keeper thread not joined before the quiesce: 5 under TSan / the order table.
//
// HERMETIC: this process's per-user folders and temp are a scratch tree, the settings files are scratch paths, the telemetry endpoint is a closed
// local port.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/breadcrumb.hpp"
#include "core/config.hpp"
#include "core/config_halves.hpp"
#include "engine/engine_service.hpp"
#include "engine/receiver_tables.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
namespace cmd = cascade::core::cmd;
namespace bc = cascade::core::breadcrumb;
using json = nlohmann::json;
using cascade::core::AppConfig;
using cascade::engine::EngineService;
using cascade::engine::ServiceHermetic;
using cascade::engine::ServiceLock;
using cascade::engine::ServiceOptions;

namespace {

#if defined(_WIN32)
int dupFd(int fd) { return _dup(fd); }
int dup2Fd(int from, int to) { return _dup2(from, to); }
void closeFd(int fd) { _close(fd); }
int fileNo(FILE* f) { return _fileno(f); }
#else
int dupFd(int fd) { return dup(fd); }
int dup2Fd(int from, int to) { return dup2(from, to); }
void closeFd(int fd) { close(fd); }
int fileNo(FILE* f) { return fileno(f); }
#endif

// What this process writes to stdout (fd 1) and stderr (fd 2) while it is alive, read back by text(). The descriptors are put back at the end.
class Capture {
public:
    Capture(int fd, const fs::path& file) : fd_(fd), file_(file) {
        std::fflush(stdout);
        std::fflush(stderr);
        saved_ = dupFd(fd_);
        FILE* f = std::fopen(file_.string().c_str(), "wb");
        if (f != nullptr) {
            dup2Fd(fileNo(f), fd_);
            std::fclose(f);
        }
    }
    ~Capture() { restore(); }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    void restore() {
        if (saved_ < 0) { return; }
        std::fflush(stdout);
        std::fflush(stderr);
        dup2Fd(saved_, fd_);
        closeFd(saved_);
        saved_ = -1;
    }
    // What has been written so far (the stream is flushed first).
    std::string text() {
        std::fflush(stdout);
        std::fflush(stderr);
        return emt::readFile(file_);
    }

private:
    int fd_;
    fs::path file_;
    int saved_ = -1;
};

std::size_t countOf(const std::string& s, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + needle.size())) { ++n; }
    return n;
}

// Whether `needle` shows in the captured stream `file` within `ms` (the process is then INSIDE the stage that printed it).
bool seen(const fs::path& file, const char* needle, int ms);

template <class F>
bool until(int ms, F cond) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

bool seen(const fs::path& file, const char* needle, int ms) {
    return until(ms, [&] { return emt::readFile(file).find(needle) != std::string::npos; });
}

json readConfig(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) { return json(); }
    const json j = json::parse(emt::readFile(p), nullptr, false);
    return j.is_object() ? j : json();
}

fs::path seedConfig(const fs::path& dir, const char* name, const std::function<void(AppConfig&)>& edit = {}) {
    fs::create_directories(dir);
    AppConfig seed;
    seed.volume = 0.0f;   // silent
    seed.squelchDb = -100.0f;
    if (edit) { edit(seed); }
    const fs::path p = dir / name;
    std::ofstream out(p, std::ios::binary);
    out << cascade::core::ConfigStore::serialize(seed);
    return p;
}

bool running(EngineService& svc) {
    cascade::core::PublishedState ps = cascade::core::initialPublishedState();
    (void)svc.engine().snapshot()->read(ps);
    return ps.app.published && (ps.rx.flags & FOXAPI_RX_RUNNING) != 0u;
}

// A host that counts what the engine asks of it, and can be marked gone (a call after that is a call into a host that no longer exists).
class CountingHost : public cascade::engine::EngineHost {
public:
    double wallTimeS() const override {
        ++wallCalls;
        if (gone) { ++callsAfterGone; }
        return EngineHost::wallTimeS();
    }
    void pauseWatchdog() override {
        if (gone) { ++callsAfterGone; }
        EngineHost::pauseWatchdog();
    }
    void resumeWatchdog() override {
        if (gone) { ++callsAfterGone; }
        EngineHost::resumeWatchdog();
    }
    mutable std::atomic<int> wallCalls{0};
    mutable std::atomic<int> callsAfterGone{0};
    std::atomic<bool> gone{false};
};

// --- 11. THE FINAL PASS (shell retirement R8) ---------------------------------------------------------------------------------------------
// Its own function: main()'s frame already holds an EngineService per section, each the size of the engine, and one more overran the stack (0xC00000FD).
void testFinalPass(const fs::path& home) {
    // A stepped service's control thread runs a pass only when someone steps it, and when the interface's frames are over nobody does: a command submitted as
    // they ended (the window's last half of the settings) would be dropped by the quiesce. finalPass makes ONE whole pass at the head of stop(); the headless
    // default makes none (and has no interface to submit anything).
    for (const bool pass : {false, true}) {
        const fs::path cfg = seedConfig(home / (pass ? "final-pass" : "no-final-pass"), "config.json");
        ServiceOptions o;
        o.configPath = cfg.string();
        o.stepped = true;
        o.receive = false;
        o.finalPass = pass;
        EngineService svc(o);
        std::string s;
        CHECK(svc.acquireConfig(s));
        CHECK(svc.restore().loaded);
        std::string why;
        CHECK(svc.start(why));
        CHECK(svc.engine().passesEnded() == 0u);                       // stepped: nothing has passed
        svc.engine().submitCommand(cmd::makeNum(FOXAPI_OP_SET_SQUELCH, -66.0));
        svc.stop();
        std::printf("  11: stepped service, finalPass %d: squelch in the final file %.1f, passes %llu\n", pass ? 1 : 0, readConfig(cfg).value("squelchDb", 9999.0),
                    static_cast<unsigned long long>(svc.engine().passesEnded()));
        CHECK(readConfig(cfg)["telemetryCleanExit"] == true);
        if (pass) {
            CHECK(readConfig(cfg).value("squelchDb", 9999.0) == -66.0);   // applied by the final pass, so the final-state save holds it
            CHECK(svc.engine().passesEnded() >= 1u);
        } else {
            CHECK(readConfig(cfg).value("squelchDb", 9999.0) != -66.0);   // dropped: the control run that makes the first one's value the pass's doing
        }
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must leave what the test had said
    std::printf("test_engine_service_options\n");
    const fs::path root = emt::scratch("engine-options");
    const fs::path home = root / "home";
    fs::create_directories(home / "tmp");
    emt::isolate(home);
    CHECK(AppConfig{}.schemaVersion == 1);
    CHECK(cascade::core::ConfigStore::defaultPath().rfind(home.string(), 0) == 0);
    emt::setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");   // a closed local port: nothing leaves this machine

    // --- 0. THE DEFAULTS ------------------------------------------------------------------------------------------------------------------
    {
        const ServiceOptions o;
        CHECK(o.configPath.empty());
        CHECK(o.diagnosticsDir.empty());
        CHECK(o.unloadStallMs == 0);
        CHECK(o.lock == ServiceLock::Strict);
        CHECK(o.hermetic == ServiceHermetic::Headless);
        CHECK(o.receive);
        CHECK(!o.stepped);
        CHECK(!o.heartbeat);
        CHECK(!o.keeperThread);
        CHECK(o.shutdownStallMs == 0);
        CHECK(o.label == "foxsdr-engine");
        CHECK(!o.pluginStatusReport);
        CHECK(!o.frontEndFollows);   // (R7: the headless engine's window-less host treats AUTO as pinned and drives its own plugin UI)
        CHECK(!o.breadcrumbs);
        CHECK(!o.diagToStdout);
        CHECK(!o.finalPass);   // (R8: only the window application's stop makes the pass that takes its last half of the settings)
    }

    // --- 1. LOCK --------------------------------------------------------------------------------------------------------------------------
    {
        const fs::path cfg = seedConfig(home / "lock", "config.json");
        ServiceOptions first;
        first.configPath = cfg.string();
        EngineService a(first);                     // the default: Strict, role "foxsdr-engine"
        std::string sentence;
        CHECK(a.acquireConfig(sentence));
        CHECK(sentence.empty());

        ServiceOptions second;
        second.configPath = cfg.string();
        EngineService b(second);
        CHECK(!b.acquireConfig(sentence));          // Strict (the default) refuses a held file ...
        CHECK(sentence.find("foxsdr-engine: not started") == 0);
        CHECK(sentence.find("another foxsdr-engine") != std::string::npos);   // ... and names the holder's role: the default label

        ServiceOptions window;
        window.configPath = cfg.string();
        window.lock = ServiceLock::BestEffort;
        window.label = "cascade";
        EngineService c(window);
        sentence = "untouched";
        CHECK(c.acquireConfig(sentence));           // BestEffort carries on from a held file ...
        CHECK(sentence == "untouched");             // ... says nothing to the caller, and refuses nothing

        // A BestEffort service on a FREE file holds the lock, as the window does: the next program is refused, and told it is the window application.
        const fs::path cfg2 = seedConfig(home / "lock2", "config.json");
        ServiceOptions w2;
        w2.configPath = cfg2.string();
        w2.lock = ServiceLock::BestEffort;
        w2.label = "cascade";
        EngineService win(w2);
        CHECK(win.acquireConfig(sentence));
        ServiceOptions e2;
        e2.configPath = cfg2.string();
        EngineService eng(e2);
        CHECK(!eng.acquireConfig(sentence));
        CHECK(sentence.find("the FoxSDR window application") != std::string::npos);   // the role is the label
        // ... until it lets go: the window's softgl relaunch releases the lock before the second process starts (main.cpp), and the second takes it.
        win.releaseConfig();
        CHECK(eng.acquireConfig(sentence));
        // A hermetic service holds nothing and always answers true, whatever the policy.
        EngineService hermetic{ServiceOptions{}};
        CHECK(hermetic.acquireConfig(sentence));
    }

    // --- 2. HERMETIC ----------------------------------------------------------------------------------------------------------------------
    {
        EngineService headless{ServiceOptions{}};
        std::string s;
        CHECK(headless.acquireConfig(s));
        const EngineService::Restored r = headless.restore();
        CHECK(r.error.empty());
        headless.engine().publishConfig();
        const AppConfig snap = headless.engine().configSnapshot();
        CHECK(snap.volume == 0.0f);                 // muted: a service plays nothing it did not restore
        CHECK(snap.sourceKind == "siggen");         // and the generator is chosen, not just the default

        ServiceOptions w;
        w.hermetic = ServiceHermetic::Window;
        EngineService window(w);
        CHECK(window.acquireConfig(s));
        CHECK(window.restore().error.empty());
        window.engine().publishConfig();
        CHECK(window.engine().configSnapshot().volume == 0.5f);   // the engine as built: what the window's hermetic runs start from
    }

    // --- 3. RECEIVE and STEPPED -----------------------------------------------------------------------------------------------------------
    {
        std::string why;
        {
            EngineService svc{ServiceOptions{}};
            std::string s;
            CHECK(svc.acquireConfig(s));
            (void)svc.restore();
            CHECK(svc.start(why));
            CHECK(until(5000, [&] { return running(svc); }));            // receive: the default
            CHECK(svc.engine().controlThreadRunning());
            CHECK(until(2000, [&] { return svc.engine().passesEnded() >= 1u; }));   // free-running: the thread is passing
            const std::uint64_t a = svc.engine().passesEnded();
            CHECK(until(3000, [&] { return svc.engine().passesEnded() > a + 3u; }));   // ... and the thread keeps passing by itself
            svc.stop();
        }
        {
            ServiceOptions o;
            o.receive = false;
            EngineService svc(o);
            std::string s;
            CHECK(svc.acquireConfig(s));
            (void)svc.restore();
            CHECK(svc.start(why));
            CHECK(until(2000, [&] { return svc.engine().passesEnded() >= 2u; }));
            CHECK(!running(svc));                                          // not started
            svc.stop();
        }
        {
            ServiceOptions o;
            o.stepped = true;
            EngineService svc(o);
            std::string s;
            CHECK(svc.acquireConfig(s));
            (void)svc.restore();
            CHECK(svc.start(why));
            CHECK(svc.engine().controlThreadRunning());                    // the thread exists ...
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(svc.engine().passesEnded() == 0u);                       // ... runs no pass of its own, and start() made none
            svc.stop();
        }
    }

    // --- 4. HEARTBEAT ---------------------------------------------------------------------------------------------------------------------
    for (const bool want : {false, true}) {
        const fs::path cfg = seedConfig(home / (want ? "hb-on" : "hb-off"), "config.json");
        ServiceOptions o;
        o.configPath = cfg.string();
        o.heartbeat = want;
        EngineService svc(o);
        std::string s;
        CHECK(svc.acquireConfig(s));
        const EngineService::Restored r = svc.restore();
        CHECK(r.loaded);
        std::string why;
        CHECK(svc.start(why));
        CHECK(svc.engine().heartbeatConfigured() == want);   // armed by the option and by nothing else
        svc.stop();
    }

    // --- 5. KEEPER THREAD -----------------------------------------------------------------------------------------------------------------
    for (const bool thread : {false, true}) {
        const fs::path cfg = seedConfig(home / (thread ? "keep-thread" : "keep-caller"), "config.json");
        ServiceOptions o;
        o.configPath = cfg.string();
        o.keeperThread = thread;
        EngineService svc(o);
        std::string s;
        CHECK(svc.acquireConfig(s));
        CHECK(svc.restore().loaded);
        std::string why;
        CHECK(svc.start(why));
        CHECK(svc.keeperRunning() == thread);
        CHECK(svc.savesRequested() == 1u);                               // the forced first save
        svc.engine().submitCommand(cmd::makeNum(FOXAPI_OP_SET_SQUELCH, -70.0));
        if (thread) {
            // Nobody calls keep(): the keeper does, and the change lands about two seconds after it.
            CHECK(until(8000, [&] { return svc.savesRequested() == 2u; }));
            CHECK(until(5000, [&] { return readConfig(cfg).value("squelchDb", 9999.0) == -70.0; }));
            svc.keep();                                                    // a call from outside while it runs: no harm, no second look of its own
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(3500));
            CHECK(svc.savesRequested() == 1u);                             // nobody kept: the change is not saved (the caller's wait loop does that)
        }
        svc.stop();
        CHECK(!svc.keeperRunning());                                     // joined at the head of stop()
        CHECK(svc.savesRequested() == (thread ? 4u : 3u));               // + the final-state save and the marker
        CHECK(readConfig(cfg)["telemetryCleanExit"] == true);
        CHECK(readConfig(cfg).value("squelchDb", 9999.0) == -70.0);      // the thread-less run's final save carries the change too
    }

    // --- 6. THE KEEPER'S COMPARE leaves out every self-moving field, of both halves ------------------------------------------------------------
    {
        const fs::path cfg = seedConfig(home / "mask", "config.json");
        ServiceOptions o;
        o.configPath = cfg.string();
        o.keeperThread = true;
        EngineService svc(o);
        std::string s;
        CHECK(svc.acquireConfig(s));
        CHECK(svc.restore().loaded);
        std::string why;
        CHECK(svc.start(why));
        CHECK(svc.savesRequested() == 1u);
        // An interface half whose beta-tester report moves every 100 ms (and the crash limiter's memory with it): the thing a window's half is.
        const auto send = [&](int n, int railBank) {
            AppConfig half;
            half.railBank = railBank;
            half.testerUsageCurrent = "{\"u\":" + std::to_string(n) + "}";
            half.testerUsagePending = {"{\"u\":" + std::to_string(n) + "}"};
            half.crashUploadWindowCount = static_cast<std::uint32_t>(n % 7);
            svc.engine().submitCommand(cmd::makeText(FOXAPP_OP_INTERFACE_CONFIG, cascade::core::serializeInterfaceHalf(half)));
        };
        send(0, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));       // (the first document is itself a change: railBank is the default 0, so it is not)
        const std::uint64_t before = svc.savesRequested();
        for (int i = 1; i <= 40; ++i) {
            send(i, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::printf("  6: saves in 4 s of a moving report: %llu\n", static_cast<unsigned long long>(svc.savesRequested() - before));
        CHECK(svc.savesRequested() == before);                               // none: the compare leaves the moving fields out
        // A real change in the same half is saved, once, a debounce after it - WHILE THE REPORT KEEPS MOVING, which is the whole of the mask: a compare that
        // read the report as a change would restart the debounce at every document and never settle (the broken mask saves NOTHING, which the quiet
        // stretch above cannot tell from the right one) - and the save carries the newest report with it.
        int n = 41;
        send(n, 2);
        CHECK(until(9000, [&] {
            send(++n, 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return svc.savesRequested() == before + 1u;
        }));
        CHECK(until(5000, [&] { return readConfig(cfg).value("railBank", -1) == 2; }));
        const json on = readConfig(cfg);
        CHECK(on.value("testerUsageCurrent", std::string()).find("\"u\":") != std::string::npos);   // (some report of the moving ones rode along)
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        CHECK(svc.savesRequested() == before + 1u);                           // and the report that went on moving made no second save
        svc.stop();
    }

    // --- 7, 8. STALL SEAMS, LABEL, PLUGIN STATUS, BREADCRUMBS ---------------------------------------------------------------------------------
    {
        // A persistent service, stopped on a thread, so the test can look while it is wedged.
        const auto stopped = [&](const char* tag, const std::function<void(ServiceOptions&)>& edit, std::string& err, std::string& out,
                                 const std::function<void(EngineService&, const fs::path&, std::atomic<bool>&)>& whileStopping) {
            const fs::path cfg = seedConfig(home / tag, "config.json");
            ServiceOptions o;
            o.configPath = cfg.string();
            edit(o);
            Capture capErr(2, root / (std::string(tag) + ".err"));
            Capture capOut(1, root / (std::string(tag) + ".out"));
            EngineService svc(o);
            std::string s;
            CHECK(svc.acquireConfig(s));
            CHECK(svc.restore().loaded);
            std::string why;
            CHECK(svc.start(why));
            svc.engine().submitCommand(cmd::makeNum(FOXAPI_OP_SET_SQUELCH, -66.0));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::atomic<bool> done{false};
            std::thread stopper([&] {
                svc.stop();
                done = true;
            });
            whileStopping(svc, cfg, done);
            stopper.join();
            err = capErr.text();
            out = capOut.text();
            capOut.restore();
            capErr.restore();
        };
        const auto nothing = [](EngineService&, const fs::path&, std::atomic<bool>&) {};

        // THE DEFAULTS: no stall, no plugin report, no breadcrumb, stderr, "foxsdr-engine".
        {
            bc::Block block{};
            bc::attach(&block);
            std::string err;
            std::string out;
            const auto t0 = std::chrono::steady_clock::now();
            stopped("defaults", [](ServiceOptions&) {}, err, out, nothing);
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  7: a default stop took %.2f s\n", took);
            CHECK(err.find("--diag-") == std::string::npos && out.find("--diag-") == std::string::npos);   // no seam
            CHECK(out.find("plugin status:") == std::string::npos);                                         // no report
            CHECK(bc::read(&block).phase == bc::Phase::Starting);                                           // no breadcrumb set (attach starts one)
            bc::attach(nullptr);
        }
        // THE SHUTDOWN WEDGE: between the final-state save and the receiver's stop; the line carries the label and goes to stderr.
        {
            std::string err;
            std::string out;
            bool wedgedFile = false;
            std::string wedgeDiag;
            const auto t0 = std::chrono::steady_clock::now();
            stopped(
                "shutdown-stall", [](ServiceOptions& o) { o.shutdownStallMs = 1200; }, err, out,
                [&](EngineService& svc, const fs::path& cfg, std::atomic<bool>&) {
                    // inside the wedge: the line is out
                    CHECK(seen(root / "shutdown-stall.err", "--diag-shutdown-stall", 20000));
                    // while wedged: the final-state save has been ASKED for (the forced first save and this one: two requests; the write itself is the
                    // writer's, which collects the first before it starts the second - nobody keeps in this test) and the marker has not (the file is
                    // still the unclean one, and no third request has been made)
                    const std::uint64_t asked = svc.savesRequested();
                    const json j = readConfig(cfg);
                    wedgedFile = asked == 2u && j.contains("telemetryCleanExit") && j["telemetryCleanExit"] == false;
                    if (!wedgedFile) { wedgeDiag = "saves requested " + std::to_string(asked); }
                });
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            CHECK(took >= 1.2);
            if (!wedgedFile) { std::printf("  shutdown wedge: %s\n", wedgeDiag.c_str()); }
            CHECK(wedgedFile);
            CHECK(err.find("foxsdr-engine: --diag-shutdown-stall wedging the teardown for 1200 ms") != std::string::npos);
            CHECK(out.find("--diag-shutdown-stall") == std::string::npos);
        }
        // THE UNLOAD WEDGE, on stdout, under the window's label; the breadcrumb phases, in their places; the plugin report, once.
        {
            bc::Block block{};
            bc::attach(&block);
            std::string err;
            std::string out;
            bc::Phase atShutdownStall = bc::Phase::Unset;
            bc::Phase atUnloadStall = bc::Phase::Unset;
            stopped(
                "unload-stall",
                [](ServiceOptions& o) {
                    o.unloadStallMs = 1500;
                    o.shutdownStallMs = 1500;
                    o.label = "cascade";
                    o.diagToStdout = true;
                    o.breadcrumbs = true;
                    o.pluginStatusReport = true;
                },
                err, out,
                [&](EngineService&, const fs::path&, std::atomic<bool>&) {
                    CHECK(seen(root / "unload-stall.out", "--diag-shutdown-stall", 20000));
                    atShutdownStall = bc::read(&block).phase;                // wedged before the receiver's stop: no phase of the service's yet
                    CHECK(seen(root / "unload-stall.out", "--diag-unload-stall", 20000));
                    atUnloadStall = bc::read(&block).phase;                  // wedged after the release: the plugins' stage
                });
            CHECK(atShutdownStall == bc::Phase::Starting);
            CHECK(atUnloadStall == bc::Phase::ShutdownUnloadingPlugins);
            CHECK(bc::read(&block).phase == bc::Phase::ShutdownWritingMarker);   // the last one the service sets
            bc::attach(nullptr);
            CHECK(out.find("cascade: --diag-unload-stall wedging the plugin unload for 1500 ms") != std::string::npos);
            CHECK(out.find("cascade: --diag-shutdown-stall wedging the teardown for 1500 ms") != std::string::npos);
            CHECK(err.find("--diag-") == std::string::npos);                     // nothing on stderr: the stream is the option's
            CHECK(countOf(out, "plugin status:") == 1u);                         // the report, once
        }
        // A DAMAGED file's sentence carries the label.
        {
            const fs::path dir = home / "damaged";
            fs::create_directories(dir);
            {
                std::ofstream bad(dir / "config.json", std::ios::binary);
                bad << "this is not json";
            }
            for (const char* label : {"foxsdr-engine", "cascade"}) {
                Capture capErr(2, root / (std::string("damaged-") + label + ".err"));
                ServiceOptions o;
                o.configPath = (dir / "config.json").string();
                o.label = label;
                EngineService svc(o);
                std::string s;
                CHECK(svc.acquireConfig(s));
                const EngineService::Restored r = svc.restore();
                CHECK(r.damaged);
                const std::string err = capErr.text();
                capErr.restore();
                CHECK(err.find(std::string(label) + ": config: ") != std::string::npos);
                std::error_code ec;
                for (const auto& e : fs::directory_iterator(dir, ec)) {   // (the file is kept aside: put a bad one back for the next label)
                    (void)e;
                }
                std::ofstream bad(dir / "config.json", std::ios::binary);
                bad << "this is not json";
            }
        }
    }

    // --- 10. WHAT A WINDOW HOST NEEDS OF THE RESTORE (shell retirement R7) -------------------------------------------------------------------
    // The window no longer reads the settings file: the host's restore returns the four things only the file said and no table fact carries, and
    // announce() prints the CASCADE_CONFIG_TEST line the window used to print from its constructor.
    {
        const auto restoreOf = [&](const char* tag, const std::function<void(AppConfig&)>& edit, std::string& out) {
            const fs::path cfg = seedConfig(home / tag, "config.json", edit);
            ServiceOptions o;
            o.configPath = cfg.string();
            o.hermetic = ServiceHermetic::Window;
            o.label = "cascade";
            EngineService svc(o);
            std::string s;
            CHECK(svc.acquireConfig(s));
            const EngineService::Restored r = svc.restore();
            Capture capOut(1, root / (std::string(tag) + ".out"));
            EngineService::announce(r);
            out = capOut.text();
            capOut.restore();
            return r;
        };
        std::string out;
        // THE LAST RUN'S MARKER: a run that never wrote it is "unclean" - the window's trigger to offer a report - and the saved source is what the
        // FILE named (the window's I/Q path box starts from it, whether or not the file opened).
        const EngineService::Restored unclean = restoreOf("r7-unclean", [](AppConfig& c) {
            c.telemetryCleanExit = false;
            c.sourceKind = "file";
            c.iqFilePath = "C:/nowhere/capture.iq";
            c.mode = "CW";
        }, out);
        CHECK(unclean.loaded && !unclean.damaged && !unclean.writesForbidden);
        CHECK(unclean.lastRunUnclean);
        CHECK(unclean.sourceKind == "file");
        CHECK(unclean.iqFilePath == "C:/nowhere/capture.iq");
        CHECK(std::string(cascade::gui::kModeNames[unclean.readback.modeIndex]) == "CW");
        CHECK(out.rfind("config applied: mode=CW center=", 0) == 0);     // the CASCADE_CONFIG_TEST line, byte for byte what the window printed
        CHECK(!out.empty() && out.back() == '\n');
        const EngineService::Restored clean = restoreOf("r7-clean", [](AppConfig& c) { c.telemetryCleanExit = true; }, out);
        CHECK(clean.loaded && !clean.lastRunUnclean);
        // A FIRST RUN (no file): loaded, defaults, and not "unclean" (the default marker is clean).
        {
            ServiceOptions o;
            o.configPath = (home / "r7-first" / "config.json").string();
            o.hermetic = ServiceHermetic::Window;
            EngineService svc(o);
            std::string s;
            CHECK(svc.acquireConfig(s));
            const EngineService::Restored r = svc.restore();
            CHECK(r.loaded && !r.damaged && !r.lastRunUnclean && !r.writesForbidden);
        }
        // A DAMAGED file: not loaded, kept aside (so nothing is forbidden), the sentence in `error`, and the announce line says defaults.
        {
            const fs::path dir = home / "r7-damaged";
            fs::create_directories(dir);
            {
                std::ofstream bad(dir / "config.json", std::ios::binary);
                bad << "this is not json";
            }
            ServiceOptions o;
            o.configPath = (dir / "config.json").string();
            o.hermetic = ServiceHermetic::Window;
            o.label = "cascade";
            Capture capErr(2, root / "r7-damaged.err");
            EngineService svc(o);
            std::string s;
            CHECK(svc.acquireConfig(s));
            const EngineService::Restored r = svc.restore();
            const std::string err = capErr.text();
            capErr.restore();
            CHECK(r.damaged && !r.loaded && !r.error.empty() && !r.lastRunUnclean && !r.writesForbidden);
            CHECK(err.find("cascade: ") == 0 || err.find("cascade: ") != std::string::npos);     // the load error, as the window printed it
            Capture capOut(1, root / "r7-damaged.out");
            EngineService::announce(r);
            const std::string said = capOut.text();
            capOut.restore();
            CHECK(said.rfind("config applied: defaults (", 0) == 0);
        }
        // THE LOCK GOES WITH THE SERVICE (src/main.cpp relies on it: the service is destroyed before the software-GL relaunch, whose second process takes
        // the lock for its own run): a best-effort service that holds it, destroyed, leaves it free for a strict one.
        {
            const fs::path cfg = seedConfig(home / "r7-lock", "config.json");
            ServiceOptions o;
            o.configPath = cfg.string();
            o.lock = ServiceLock::BestEffort;
            o.label = "cascade";
            std::string s;
            {
                EngineService window(o);
                CHECK(window.acquireConfig(s));
                ServiceOptions strict;
                strict.configPath = cfg.string();
                EngineService second(strict);
                CHECK(!second.acquireConfig(s));                  // held: a second program is refused beside the window application
                CHECK(s.find("the FoxSDR window application") != std::string::npos);
            }
            ServiceOptions strict;
            strict.configPath = cfg.string();
            EngineService after(strict);
            CHECK(after.acquireConfig(s));                        // the window's service is gone: the lock is free
        }
    }

    // --- 9. THE ENGINE'S OWN SURFACE ---------------------------------------------------------------------------------------------------------
    {
        // configReadback(): the demod mode the pipeline mirrors hold and the centre the source reports, public.
        const fs::path cfg = seedConfig(home / "readback", "config.json", [](AppConfig& c) { c.mode = "USB"; });
        ServiceOptions o;
        o.configPath = cfg.string();
        EngineService svc(o);
        std::string s;
        CHECK(svc.acquireConfig(s));
        CHECK(svc.restore().loaded);
        const cascade::engine::ConfigReadback rb = svc.engine().configReadback();
        CHECK(std::string(cascade::gui::kModeNames[rb.modeIndex]) == "USB");
        CHECK(rb.centreHz > 0.0);
    }
    {
        // THE HOST THE ENGINE WAS BUILT WITH is the one it asks, for life (R9: there is no setHost any more - was: setHost / resetHost, R6).
        CountingHost host;
        {
            cascade::engine::Engine e(host);
            AppConfig cfg;
            e.telemetryStartup(cfg, std::string());                 // asks the host for the wall clock
            CHECK(host.wallCalls.load() >= 1);
            const int asked = host.wallCalls.load();
            // a control thread running changes nothing about who is asked
            cascade::engine::ControlThreadOptions opt;
            opt.stepped = true;
            e.startControlThread(opt);
            e.telemetryStartup(cfg, std::string());
            CHECK(host.wallCalls.load() > asked);
            e.stopControlThread();
            // the destructor's fallback: a host marked gone is never called by the engine's teardown
            host.gone = true;
        }
        CHECK(host.callsAfterGone.load() == 0);
    }
    {
        // ... and an engine built with no host asks its own, and never a host that was never given
        CountingHost bystander;
        cascade::engine::Engine e;
        AppConfig cfg;
        e.telemetryStartup(cfg, std::string());
        CHECK(bystander.wallCalls.load() == 0);
    }

    testFinalPass(home);   // (11: its own function - an EngineService is a local the size of the engine, and main's frame is already full)

    return testSummary("test_engine_service_options");
}
