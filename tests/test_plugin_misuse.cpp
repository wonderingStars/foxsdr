// test_plugin_misuse.cpp - THE API-MISUSE TRIPWIRES (plugin containment P2, docs/engine-stage4.md 6.29; core/plugin_fault.hpp "The tripwires").
//
// The plugin ABI states the rules a plugin lives by (core/plugin_abi.h: THREADING AND LIFETIME CONTRACT, HOST API LEVEL 1 - THREADING, AFTER
// UNLOAD). A plugin that breaks one is refused with the ABI's own answer - and, since this batch, COUNTED per plugin id and version beside its
// faults, so the answer is never the end of the matter. Every case here is driven through the hostile fixture's real module, over the real host
// bridges (PluginUi over a PluginApiCore), on the thread the case is about:
//
//   A  WRONG THREAD   settings_get / settings_set on the real-time thread: refused WRONG_THREAD, counted, nothing stored; the same calls on any
//                     other thread (a plain one, a worker) are served and not counted. And from inside the plugin's own process().
//   B  FLOODS         settings (64 keys), commands (16), markers (64), the log ring (256) and the control ring (256): the first N are served,
//                     the rest refused (LIMIT / BUSY) and counted, the queues stay at their bounds, a neighbour is not touched.
//   C  BAD CALLS      a NULL, a zero id, a missing struct: BAD_ARGUMENT, counted.
//   D  AFTER DETACH   a call through a table the host has shut down (the plugin stopped, the owner gone): DETACHED, and - from OUTSIDE any call
//                     into the plugin - counted and the plugin's MODULE PINNED; from inside one (a destroy() saving its state) not counted.
//                     The four calls older than level 1 are counted the same way.
//   E  THE THREAD THAT OUTLIVES destroy()   (ABI rule 3 broken on purpose) the module is not unmapped, and the thread's calls are refused.
//   F  THE HOST'S OWN RULE   a call into a plugin begun on a thread already inside a call into the same plugin is counted (HostReentry), and
//                     nothing the real host does counts one.
//   G  THE REFEREE ITSELF   the fixture notes a call on a destroyed instance and a GUI instance used from a second thread: it is not vacuous.
//   H  THE NOTICES    the first occurrence of each kind, per plugin, once.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_api.hpp"
#include "core/plugin_calls.hpp"
#include "core/plugin_fault.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "hostile_support.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace fault = cascade::core::fault;
namespace calls = cascade::core::calls;
using cascade::core::LoadedPlugin;
using cascade::core::PluginApiCore;
using cascade::core::PluginHost;
using cascade::core::PluginUi;
using cascade::core::RealtimeThreadScope;
using fault::Misuse;
using hostile::doOp;
using hostile::setMode;

namespace {

fs::path g_root;
std::string g_fixture;

std::uint64_t misuse(const char* id, Misuse k) {
    for (const fault::HealthRow& r : fault::health()) {
        if (r.id == id) { return r.misuse[static_cast<std::size_t>(k)]; }
    }
    return 0;
}

std::uint64_t misuseTotalOf(const char* id) {
    std::uint64_t n = 0;
    for (std::size_t k = 0; k < fault::kMisuseCount; ++k) { n += misuse(id, static_cast<Misuse>(k)); }
    return n;
}

void fresh() {
    fault::resetForTest();
    (void)fault::drainEvents();
    (void)fault::drainMisuseNotices();
    setMode(nullptr);
}

// A PluginUi over a core of its own with the fixture attached (live, host pointer stored by its attach()).
struct Rig {
    std::shared_ptr<PluginApiCore> core = std::make_shared<PluginApiCore>();
    std::unique_ptr<PluginUi> ui;
    const LoadedPlugin* lp = nullptr;
    std::string key;
    explicit Rig(const LoadedPlugin& plugin) : lp(&plugin), key(cascade::core::pluginKey(plugin)) {
        ui = std::make_unique<PluginUi>(core);
        ui->rebuild({plugin});
    }
};

// ---- A ----------------------------------------------------------------------------------------------------------------------------------
void testWrongThread(const LoadedPlugin& lp) {
    std::printf("  A: the settings store on the real-time thread\n");
    fresh();
    Rig rig(lp);
    CHECK(doOp(lp, "stat-host") == 1);   // attached: the fixture holds the host table
    // a plain thread: served, not counted
    CHECK(doOp(lp, "settings-set") == CASCADE_API_OK);
    CHECK(doOp(lp, "settings-get") >= 0);
    CHECK(misuse("hp-u", Misuse::WrongThread) == 0);
    const std::uint64_t generation = rig.core->settingsGeneration();
    // the real-time thread: refused and counted, and nothing is stored
    {
        RealtimeThreadScope scope;
        CHECK(doOp(lp, "settings-get") == CASCADE_API_WRONG_THREAD);
        CHECK(doOp(lp, "settings-set") == CASCADE_API_WRONG_THREAD);
    }
    CHECK(misuse("hp-u", Misuse::WrongThread) == 2);
    CHECK(rig.core->settingsGeneration() == generation);
    // a worker thread that is not marked real-time is served
    std::int64_t fromWorker = -1;
    std::thread([&] { fromWorker = doOp(lp, "settings-get"); }).join();
    CHECK(fromWorker >= 0);
    CHECK(misuse("hp-u", Misuse::WrongThread) == 2);
    // ...and from inside the plugin's own process(), on the runner's real-time path
    {
        cascade::core::PluginRunner runner;
        runner.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
        const fs::path log = g_root / "wrongthread.log";
        std::error_code ec;
        fs::remove(log, ec);
        hostile::setEnv("HOSTILE_PLUGIN_LOG", log.string().c_str());
        setMode("process-settings-get");
        {
            RealtimeThreadScope scope;   // what the pipeline's DSP thread holds while it feeds the runner
            std::vector<float> audio(480, 0.0f);
            for (int i = 0; i < 3; ++i) { runner.processAudio(audio.data(), audio.size()); }
        }
        setMode(nullptr);
        hostile::setEnv("HOSTILE_PLUGIN_LOG", nullptr);
        const std::string text = hostile::readFile(log);
        CHECK(hostile::countLines(text, "process-settings-get=-11") == 3);   // the plugin was told, three times
        CHECK(misuse("hp-u", Misuse::WrongThread) == 5);
        runner.clear();
    }
    CHECK(!fault::isQuarantined(fault::slotFor(lp.path)));   // refused and counted; the ABI has an answer, and it is not a quarantine
}

// ---- B ----------------------------------------------------------------------------------------------------------------------------------
void testFloods(const LoadedPlugin& lp, const LoadedPlugin& neighbour) {
    std::printf("  B: floods of the bounded stores and queues\n");
    fresh();
    Rig rig(lp);
    rig.core->setSettingsGranted(rig.key, true);
    // a neighbour on the same core, to show a flood is the flooder's alone
    rig.ui->rebuild({lp, neighbour});
    CHECK(doOp(neighbour, "stat-host") == 1);

    // settings: 64 keys, then LIMIT
    CHECK(doOp(lp, "settings-flood", 100) == 100 - static_cast<int>(CASCADE_MAX_SETTINGS_PER_PLUGIN));
    CHECK(misuse("hp-u", Misuse::Flood) == 100 - CASCADE_MAX_SETTINGS_PER_PLUGIN);
    // commands: 16, then LIMIT
    CHECK(doOp(lp, "command-flood", 40) == 40 - static_cast<int>(CASCADE_MAX_COMMANDS_PER_PLUGIN));
    CHECK(rig.core->commands(rig.key).size() == CASCADE_MAX_COMMANDS_PER_PLUGIN);   // the store stayed at its bound
    // markers: 64, then LIMIT
    CHECK(doOp(lp, "marker-flood", 100) == 100 - static_cast<int>(CASCADE_MAX_MARKERS_PER_PLUGIN));
    // the log ring: 256 between frames, then BUSY
    CHECK(doOp(lp, "log-flood", 400) == 400 - static_cast<int>(PluginApiCore::kLogQueue));
    {
        std::vector<cascade::core::PluginLogLine> lines;
        rig.core->takeLog(lines);
        CHECK(lines.size() == PluginApiCore::kLogQueue);   // never grown past its bound
    }
    // the control ring: 256 accepted requests (SETTINGS granted), then BUSY
    CHECK(doOp(lp, "control-flood", 300) == 300 - static_cast<int>(PluginApiCore::kControlQueue));
    {
        std::vector<cascade::core::PluginControl> controls;
        rig.core->takeControls(controls);
        CHECK(controls.size() == PluginApiCore::kControlQueue);
    }
    const std::uint64_t flooded = (100 - CASCADE_MAX_SETTINGS_PER_PLUGIN) + (40 - CASCADE_MAX_COMMANDS_PER_PLUGIN) +
                                  (100 - CASCADE_MAX_MARKERS_PER_PLUGIN) + (400 - PluginApiCore::kLogQueue) +
                                  (300 - PluginApiCore::kControlQueue);
    CHECK(misuse("hp-u", Misuse::Flood) == flooded);
    // the neighbour is untouched, and is still served in full
    CHECK(misuseTotalOf("hp-v") == 0);
    CHECK(doOp(neighbour, "settings-flood", 10) == 0);
    CHECK(misuse("hp-v", Misuse::Flood) == 0);
    // a refused call stored nothing: the settings store holds exactly the 64
    CHECK(rig.core->settingsSnapshot()[lp.name].size() == CASCADE_MAX_SETTINGS_PER_PLUGIN);
    CHECK(!fault::isQuarantined(fault::slotFor(lp.path)));
}

// ---- C ----------------------------------------------------------------------------------------------------------------------------------
void testBadCalls(const LoadedPlugin& lp) {
    std::printf("  C: malformed calls\n");
    fresh();
    Rig rig(lp);
    CHECK(doOp(lp, "bad-call") == 5);   // all five answered BAD_ARGUMENT
    CHECK(misuse("hp-u", Misuse::BadCall) == 5);
    CHECK(misuse("hp-u", Misuse::Flood) == 0 && misuse("hp-u", Misuse::WrongThread) == 0);
    // a well-formed call is not a bad one
    CHECK(doOp(lp, "log-one") == CASCADE_API_OK);
    CHECK(misuse("hp-u", Misuse::BadCall) == 5);
}

// ---- D ----------------------------------------------------------------------------------------------------------------------------------
void testAfterDetach(const LoadedPlugin& lp) {
    std::printf("  D: calls through a table the host has shut down\n");
    fresh();
    fault::Slot* const slot = fault::slotFor(lp.path);
    {
        Rig rig(lp);
        CHECK(doOp(lp, "log-one") == CASCADE_API_OK);
        CHECK(misuse("hp-u", Misuse::AfterDetach) == 0 && !fault::isPinned(slot));
        // the plugin is stopped: its instances destroyed, its client no longer live
        rig.ui->stopPlugin(rig.key);
        // INSIDE a call into the plugin (a destroy() saving its state on the way out): the ABI's own allowance, answered and not counted
        std::int64_t inside = 0;
        const fault::Outcome o = fault::call(slot, fault::Entry::Destroy, [&] { inside = doOp(lp, "log-one"); });
        CHECK(o == fault::Outcome::Ran);
        CHECK(inside == CASCADE_API_DETACHED);
        CHECK(misuse("hp-u", Misuse::AfterDetach) == 0);
        CHECK(!fault::isPinned(slot));
        // from OUTSIDE any call into it: the plugin has a thread it did not stop - counted, and its module is pinned
        CHECK(doOp(lp, "log-one") == CASCADE_API_DETACHED);
        CHECK(misuse("hp-u", Misuse::AfterDetach) == 1);
        CHECK(fault::isPinned(slot));
        CHECK(!fault::isQuarantined(slot));   // pinned is not switched off: nothing about its features changes
        CHECK(doOp(lp, "settings-set") == CASCADE_API_DETACHED);
        CHECK(doOp(lp, "state") == CASCADE_API_DETACHED);
        CHECK(misuse("hp-u", Misuse::AfterDetach) == 3);
        // started again, it is served again
        CHECK(rig.ui->startPlugin(lp));
        CHECK(doOp(lp, "log-one") == CASCADE_API_OK);
        CHECK(misuse("hp-u", Misuse::AfterDetach) == 3);
    }
    // the OWNER is gone (the window closed, the process is exiting): every call through its bridge is DETACHED, the four old ones included
    fresh();
    {
        auto rig = std::make_unique<Rig>(lp);
        CHECK(doOp(lp, "log-one") == CASCADE_API_OK);   // served while the owner lives
        CHECK(doOp(lp, "time") == 0);                     // (no host services in a bare PluginUi: 0 either way, and not counted)
        CHECK(misuse("hp-u", Misuse::AfterDetach) == 0);
        rig.reset();
        CHECK(doOp(lp, "time") == 0);
        CHECK(doOp(lp, "centre") == 0);
        CHECK(doOp(lp, "rate") == 0);
        CHECK(doOp(lp, "tune") == CASCADE_TUNE_FAILED);
        CHECK(doOp(lp, "log-one") == CASCADE_API_DETACHED);
        CHECK(doOp(lp, "settings-get") == CASCADE_API_DETACHED);
        CHECK(misuse("hp-u", Misuse::AfterDetach) == 6);
    }
}

// ---- E ----------------------------------------------------------------------------------------------------------------------------------
void testThreadOutlivesDestroy(const std::string& pluginsDir) {
    std::printf("  E: a thread that outlives destroy() - the module is not unmapped under it\n");
    fresh();
    PluginHost host;
    host.scan(pluginsDir);
    const LoadedPlugin* lp = hostile::find(host, "hp-w");
    CHECK(lp != nullptr && lp->loaded);
    if (lp == nullptr || !lp->loaded) { return; }
    // (the module's exports, taken now: unloadAll() forgets the handle)
    using Fn = std::int64_t (*)(const char*, std::int64_t);
    const Fn op = reinterpret_cast<Fn>(hostile::symbolOf(lp->nativeHandle, "hostile_do"));
    CHECK(op != nullptr);
    fault::Slot* const slot = fault::slotFor(lp->path);
    {
        PluginUi ui;
        ui.rebuild({*lp});
        cascade::core::PluginRunner runner;
        // create() starts the thread, which calls the host API every millisecond and which destroy() does not stop
        setMode("hp-w:thread-outlives");
        runner.rebuild({*lp}, 48000.0, 2400000.0, 100000000.0);
        setMode(nullptr);
        for (int i = 0; i < 200 && op("stat-thread-calls", 0) < 5; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        CHECK(op("stat-thread-alive", 0) == 1);
        CHECK(op("stat-thread-calls", 0) >= 5);
        CHECK(op("stat-thread-last", 0) == CASCADE_API_OK);   // served while the plugin is live
        CHECK(misuse("hp-w", Misuse::AfterDetach) == 0);
        // the plugin is stopped and its instances destroyed: the thread is still there
        ui.stopPlugin(cascade::core::pluginKey(*lp));
        runner.clear();
        for (int i = 0; i < 200 && misuse("hp-w", Misuse::AfterDetach) == 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        CHECK(misuse("hp-w", Misuse::AfterDetach) >= 1);   // its next call was refused DETACHED, from outside any call into the plugin
        CHECK(op("stat-thread-last", 0) == CASCADE_API_DETACHED);
        CHECK(fault::isPinned(slot));
    }
    // the host unloads everything: this module is left mapped, because a thread is running in it
    host.unloadAll();
#if defined(_WIN32)
    CHECK(::GetModuleHandleA(hostile::leafOf("hp-w").c_str()) != nullptr);
#endif
    CHECK(op("stat-thread-alive", 0) == 1);   // and the thread is still running, in a module that is still mapped
    op("thread-stop", 0);
    for (int i = 0; i < 400 && op("stat-thread-alive", 0) != 0; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    CHECK(op("stat-thread-alive", 0) == 0);
}

// ---- F ----------------------------------------------------------------------------------------------------------------------------------
void testHostReentry(const LoadedPlugin& lp) {
    std::printf("  F: the host's own rule - no call into a plugin from inside a call into it\n");
    fresh();
    fault::Slot* const slot = fault::slotFor(lp.path);
    char buf[64];
    void* h = calls::create(slot, lp.decoder, 48000u);
    CHECK(h != nullptr);
    // sequential calls, a call on a worker thread beside another thread's: not nested
    CHECK(calls::pollText(slot, lp.decoder, h, buf, sizeof(buf)) == 0);
    CHECK(calls::pollText(slot, lp.decoder, h, buf, sizeof(buf)) == 0);
    CHECK(misuse("hp-u", Misuse::HostReentry) == 0);
    // nested on one thread: a host that calls plugin code from inside plugin code (a callback from a host API call, say)
    (void)fault::call(slot, fault::Entry::PollRows, [&] { (void)calls::pollText(slot, lp.decoder, h, buf, sizeof(buf)); });
    CHECK(misuse("hp-u", Misuse::HostReentry) == 1);
    // another plugin's call inside this one's is not this plugin's re-entry
    fresh();
    fault::Slot* const other = fault::slotFor("some-other-plugin");
    (void)fault::call(slot, fault::Entry::PollRows, [&] { (void)fault::call(other, fault::Entry::PollRows, [] {}); });
    CHECK(misuse("hp-u", Misuse::HostReentry) == 0);
    (void)calls::destroy(slot, lp.decoder, h);
}

// ---- G ----------------------------------------------------------------------------------------------------------------------------------
void testRefereeIsNotVacuous(const LoadedPlugin& lp) {
    std::printf("  G: the fixture's referee notes what a host does wrong\n");
    fresh();
    fault::Slot* const slot = fault::slotFor(lp.path);
    std::string text;
    const std::size_t before = hostile::violations(lp, &text);
    void* h = calls::create(slot, lp.decoder, 48000u);
    char buf[64];
    CHECK(calls::pollText(slot, lp.decoder, h, buf, sizeof(buf)) == 0);
    CHECK(calls::destroy(slot, lp.decoder, h));
    CHECK(hostile::violations(lp) == before);                          // the book was followed
    (void)calls::pollText(slot, lp.decoder, h, buf, sizeof(buf));      // a call on a destroyed instance: the host's mistake
    CHECK(hostile::violations(lp, &text) == before + 1);
    CHECK(text.find("call-after-destroy in poll-text") != std::string::npos);
    // a GUI-capability instance used from a second thread
    void* t = calls::create(slot, lp.trackSource);
    CascadeTrack tracks[2];
    CHECK(calls::pollTracks(slot, lp.trackSource, t, tracks, 2) >= 0);
    CHECK(hostile::violations(lp) == before + 1);
    std::thread([&] { (void)calls::pollTracks(slot, lp.trackSource, t, tracks, 2); }).join();
    CHECK(hostile::violations(lp, &text) == before + 2);
    CHECK(text.find("gui-instance-used-from-a-second-thread in poll-tracks") != std::string::npos);
    CHECK(calls::destroy(slot, lp.trackSource, t));
}

// ---- H ----------------------------------------------------------------------------------------------------------------------------------
void testNotices(const LoadedPlugin& lp) {
    std::printf("  H: the first occurrence of each kind is said once\n");
    fresh();
    Rig rig(lp);
    CHECK(fault::drainMisuseNotices().empty());   // nothing yet
    {
        RealtimeThreadScope scope;
        (void)doOp(lp, "settings-get");
    }
    (void)doOp(lp, "bad-call");
    const std::vector<fault::MisuseNotice> first = fault::drainMisuseNotices();
    CHECK(first.size() == 2);
    bool sawWrong = false, sawBad = false;
    for (const fault::MisuseNotice& n : first) {
        CHECK(n.id == "hp-u" && n.version == "1.2.3");
        CHECK(n.publicId.rfind("site-", 0) == 0);   // not an official plugin: named by its hash wherever it leaves the machine
        sawWrong = sawWrong || (n.kind == Misuse::WrongThread && n.count == 1);
        sawBad = sawBad || (n.kind == Misuse::BadCall && n.count == 5);
    }
    CHECK(sawWrong && sawBad);
    // more of the same is counted and not said again; a new kind is said
    {
        RealtimeThreadScope scope;
        (void)doOp(lp, "settings-get");
    }
    CHECK(fault::drainMisuseNotices().empty());
    (void)doOp(lp, "log-flood", 300);
    const std::vector<fault::MisuseNotice> second = fault::drainMisuseNotices();
    CHECK(second.size() == 1 && second[0].kind == Misuse::Flood);
    CHECK(fault::drainMisuseNotices().empty());
    CHECK(fault::misuseTotal(Misuse::WrongThread) == 2);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        std::printf("usage: test_plugin_misuse <hostile fixture module>\n");
        return 2;
    }
    g_fixture = argv[1];
    g_root = fs::temp_directory_path() / ("foxsdr-test-plugin-misuse-" + std::to_string(static_cast<unsigned long long>(
#if defined(_WIN32)
                                                                                  ::GetCurrentProcessId()
#else
                                                                                  1
#endif
                                                                                      )));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root / "plugins");
    fs::create_directories(g_root / "outlives");
    for (const char* id : {"hp-u", "hp-v"}) { fs::copy_file(g_fixture, g_root / "plugins" / hostile::leafOf(id), fs::copy_options::overwrite_existing, ec); }
    fs::copy_file(g_fixture, g_root / "outlives" / hostile::leafOf("hp-w"), fs::copy_options::overwrite_existing, ec);
    setMode(nullptr);

    PluginHost host;
    host.scan((g_root / "plugins").string());
    const LoadedPlugin* u = hostile::find(host, "hp-u");
    const LoadedPlugin* v = hostile::find(host, "hp-v");
    CHECK(u != nullptr && u->loaded && v != nullptr && v->loaded);
    if (u != nullptr && u->loaded && v != nullptr && v->loaded) {
        testWrongThread(*u);
        testFloods(*u, *v);
        testBadCalls(*u);
        testAfterDetach(*u);
        testHostReentry(*u);
        testRefereeIsNotVacuous(*u);
        testNotices(*u);
    }
    testThreadOutlivesDestroy((g_root / "outlives").string());

    host.unloadAll();
    fs::remove_all(g_root, ec);
    return testSummary("test_plugin_misuse");
}
