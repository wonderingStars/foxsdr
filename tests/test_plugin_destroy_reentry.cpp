// A plugin that calls the host back from inside its own destroy().
//
// WHY THIS FILE EXISTS. Two crash reports, one cause. On Windows (0.96.3, a
// HackRF, seven seconds in, at shutdown) the Survey Engine plugin faulted
// 0xC0000005 with the stack
//
//   survey-engine!finishDwell +1379
//   survey-engine!iq_destroy +54
//   cascade!cascade::core::PluginRunner::destroyLocked +263
//   cascade!cascade::core::PluginRunner::~PluginRunner +57
//   cascade!cascade::gui::AppWindow::~AppWindow +2000
//
// and on Android the same path aborted inside libc++ with
//
//   PluginRunner::clear -> destroyLocked -> <plugin> destroy -> hostTime
//     -> PluginUi::hasServices -> std::lock_guard -> abort
//
// The plugin is doing nothing wrong. plugin_abi.h, CascadeHostClientApi::
// attach, promises the plugin "a table that remains valid for as long as the
// plugin is loaded" and tells it to store the pointer; Survey Engine stores it
// and asks the host for the time while finishing its last dwell. The HOST broke
// that promise two ways, and this file pins both:
//
//   1. PluginUi::clear() FREED the bridges that back that table, while every
//      module was still mapped - and ~AppWindow destroyed pluginUi_ BEFORE
//      pluginRunner_ (reverse declaration order), so by the time a decoder's
//      destroy() ran, the table it had stored was freed memory whose `self`
//      pointed at a destroyed PluginUi. Reading it faulted on Windows;
//      surviving the read and locking the dead host's mutex aborted on Android.
//
//   2. PluginRunner::destroyLocked() called destroy() with mutex_ HELD, so a
//      plugin that asks the host to retune from inside destroy() re-enters
//      PluginRunner::retune() on the same thread - a self-deadlock on a
//      non-recursive std::mutex (MSVC) or an abort (libc++).
//
// Both are host defects and both are fixed in the host, so that no plugin can
// do this again whatever it calls at destroy time.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::HostServices;
using cascade::core::LoadedPlugin;
using cascade::core::PluginRunner;
using cascade::core::PluginUi;

// --- Case A: a plugin that retunes from inside destroy() -------------------
//
// Modelling the real path exactly: Survey Engine's stepSweep() calls
// host->request_tune(), the host's tune service ends in
// AppWindow::applyRetuneNow() -> pluginRunner_.retune(), and retune() takes the
// same mutex_ destroyLocked() was holding. The fake calls the runner directly
// so the test needs no window, but it is the same re-entry through the same
// lock.

PluginRunner* g_runner = nullptr;
int g_reentryDestroys = 0;
bool g_reentrySawStatus = false;

void* reentryCreate(double, double) { return &g_reentryDestroys; }
void reentryProcess(void*, const float*, std::size_t) {}
std::int32_t reentryPoll(void*, char*, std::size_t) { return 0; }

void reentryDestroy(void* handle) {
    if (handle == nullptr) { return; }
    ++g_reentryDestroys;
    if (g_runner == nullptr) { return; }
    // Everything a host service can reach back into. Each of these takes
    // PluginRunner::mutex_.
    g_runner->retune(101.1e6);
    g_reentrySawStatus = !g_runner->status().empty();
    (void)g_runner->isFeeding("reentry.dll");
    (void)g_runner->activeCount();
}

CascadeIqDecoderApi makeReentryApi() {
    CascadeIqDecoderApi a{};
    a.structSize = static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi));
    a.requiredRateHz = 0.0;
    a.create = &reentryCreate;
    a.process = &reentryProcess;
    a.poll_text = &reentryPoll;
    a.destroy = &reentryDestroy;
    return a;
}

// --- Case B/C: a plugin that keeps the host table and uses it at destroy ---

const CascadeHostApi* g_kept = nullptr;
std::int64_t g_timeSeenAtDestroy = -1;

void keeperAttach(const CascadeHostApi* host) { g_kept = host; }

CascadeHostClientApi makeKeeperClient() {
    CascadeHostClientApi c{};
    c.structSize = static_cast<std::uint32_t>(sizeof(CascadeHostClientApi));
    c.attach = &keeperAttach;
    return c;
}

int g_keeperHandle = 0;

void* keeperCreate(double, double) { return &g_keeperHandle; }
void keeperProcess(void*, const float*, std::size_t) {}
std::int32_t keeperPoll(void*, char*, std::size_t) { return 0; }

// What Survey Engine's iq_destroy does: finish the dwell, and timestamp it
// with the host's clock.
void keeperDestroy(void* handle) {
    if (handle == nullptr) { return; }
    const CascadeHostApi* h = g_kept;
    if (h != nullptr && h->unix_time_ms != nullptr) {
        g_timeSeenAtDestroy = h->unix_time_ms(h->ctx);
    }
}

CascadeIqDecoderApi makeKeeperApi() {
    CascadeIqDecoderApi a{};
    a.structSize = static_cast<std::uint32_t>(sizeof(CascadeIqDecoderApi));
    a.requiredRateHz = 0.0;
    a.create = &keeperCreate;
    a.process = &keeperProcess;
    a.poll_text = &keeperPoll;
    a.destroy = &keeperDestroy;
    return a;
}

LoadedPlugin makePlugin(const char* name, const char* path) {
    LoadedPlugin lp;
    lp.name = name;
    lp.path = path;
    lp.loaded = true;
    return lp;
}

// The host clock the fake must observe. A distinctive value, so "the host
// answered" and "the host answered zero because the table was detached" are
// different results and not the same one.
constexpr std::int64_t kHostTimeMs = 1726500000123LL;

}  // namespace

int main() {
    // UNBUFFERED, deliberately. Every case below is a candidate for a hard
    // stop - a deadlock, an abort in the standard library, a fault inside a
    // plugin's destroy() - and a buffered line dies with the process, which
    // would leave the failure looking like a silent crash with no place in it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("-- Case A: destroy() re-enters the runner --\n");
    // =====================================================================
    // Case A - destroy() re-enters the runner through a host service.
    //
    // RED IS A HANG, not a wrong value: on the pre-fix code destroyLocked()
    // holds mutex_ and retune() blocks on it for ever. The destroy therefore
    // runs on its own thread with a bounded wait, so the defect is a named
    // failure instead of a suite that never finishes.
    // =====================================================================
    {
        const CascadeIqDecoderApi api = makeReentryApi();
        LoadedPlugin lp = makePlugin("Re-entrant", "reentry.dll");
        lp.iqDecoder = &api;
        const std::vector<LoadedPlugin> plugins{lp};

        // Heap-allocated and deliberately never deleted on the timeout path:
        // if the worker is wedged inside the runner, destroying the runner
        // under it would turn a reported deadlock into undefined behaviour.
        auto* runner = new PluginRunner();
        g_runner = runner;
        runner->rebuild(plugins, 48000.0, 2.4e6, 100.0e6);
        CHECK(runner->activeCount() == 1);

        // WHAT ESCAPED clear() IS PART OF THE EVIDENCE. A non-recursive
        // std::mutex re-locked on the same thread is undefined behaviour and
        // the two standard libraries answer differently: libc++ (Android)
        // aborts, and MSVC's throws std::system_error out of lock() - which
        // then unwinds through a plugin's destroy() and out of the thread
        // function, i.e. std::terminate, killing the process with 0xC0000409
        // before any check could be printed. Caught here so the pre-fix
        // failure is a named FAIL with a reason rather than a silent crash.
        std::promise<void> done;
        std::future<void> fut = done.get_future();
        std::string thrown;
        std::thread worker([runner, &done, &thrown] {
            try {
                runner->clear();
            } catch (const std::exception& e) {
                thrown = e.what();
            } catch (...) {
                thrown = "a non-std exception";
            }
            done.set_value();
        });

        const bool finished = fut.wait_for(std::chrono::seconds(10)) ==
                              std::future_status::ready;
        CHECK(finished);
        if (!finished) {
            worker.detach();
            g_runner = nullptr;
            std::printf(
                "FAIL %s:%d  clear() did not return within 10 s: the plugin's "
                "destroy() re-entered PluginRunner while destroyLocked() held "
                "its mutex\n",
                __FILE__, __LINE__);
            ++g_checksFailed;
            // The worker owns mutex_ for ever. Report and leave without
            // running any destructor that would touch the wedged runner.
            const int rc = testSummary("test_plugin_destroy_reentry");
            std::fflush(stdout);
            std::_Exit(rc == 0 ? 1 : rc);
        }
        worker.join();
        if (!thrown.empty()) {
            std::printf("FAIL %s:%d  clear() let an exception out of the "
                        "plugin's destroy(): %s\n",
                        __FILE__, __LINE__, thrown.c_str());
            ++g_checksFailed;
        }
        ++g_checksRun;
        CHECK(g_reentryDestroys == 1);
        // The re-entrant calls have to have really run, or this case proves
        // nothing: a destroy that returned because the fake did nothing looks
        // exactly like one that returned because the lock was free.
        CHECK(g_reentrySawStatus == false);  // status_ is emptied before destroy
        CHECK(runner->activeCount() == 0);
        g_runner = nullptr;
        delete runner;
    }

    std::printf("-- Case B: the table outlives PluginUi::clear() --\n");
    // =====================================================================
    // Case B - the host table outlives PluginUi::clear().
    //
    // plugin_abi.h promises the table stays valid "for as long as the plugin
    // is loaded", and clear() runs while every module is still mapped. Counted
    // rather than dereferenced: reading a freed bridge is undefined behaviour
    // that usually reads back intact, so a test that only called through the
    // pointer would pass against the very defect it is named for.
    // =====================================================================
    {
        const CascadeHostClientApi client = makeKeeperClient();
        LoadedPlugin lp = makePlugin("Keeper", "keeper.dll");
        lp.hostClient = &client;
        const std::vector<LoadedPlugin> plugins{lp};

        const std::size_t before = cascade::core::hostBridgeCount();

        PluginUi ui;
        HostServices svc;
        svc.centreHz = [] { return 100.0e6; };
        svc.sampleRateHz = [] { return 2.4e6; };
        svc.unixTimeMs = [] { return kHostTimeMs; };
        ui.setServices(std::move(svc));

        g_kept = nullptr;
        ui.rebuild(plugins);
        CHECK(g_kept != nullptr);
        CHECK(cascade::core::hostBridgeCount() == before + 1);
        CHECK(cascade::core::attachedHostBridgeCount() == 1);

        // A REBUILD IS NOT A NEW BRIDGE. rebuild() runs on every source change;
        // one bridge per rebuild both grew without bound and left the plugin
        // holding an older table than the one the host considered current.
        const CascadeHostApi* first = g_kept;
        ui.rebuild(plugins);
        CHECK(cascade::core::hostBridgeCount() == before + 1);
        CHECK(g_kept == first);

        // clear() must not take the table away from a plugin still loaded.
        ui.clear();
        CHECK(cascade::core::hostBridgeCount() == before + 1);
        CHECK(cascade::core::attachedHostBridgeCount() == 1);
        // ...and it must still WORK, which is what the plugin's destroy needs.
        CHECK(first->unix_time_ms != nullptr);
        CHECK(first->unix_time_ms(first->ctx) == kHostTimeMs);
    }

    std::printf("-- Case C: a destroyed PluginUi leaves a safe table --\n");
    // =====================================================================
    // Case C - a destroyed PluginUi leaves a SAFE table behind.
    //
    // The order fixed in ~AppWindow means the runner is cleared while the UI
    // is alive, so this should never be reached in the product. It is the net
    // underneath that: a bridge whose PluginUi has gone answers the ABI's
    // "nothing" instead of locking a destroyed mutex.
    // =====================================================================
    {
        const CascadeHostClientApi client = makeKeeperClient();
        LoadedPlugin lp = makePlugin("Keeper2", "keeper2.dll");
        lp.hostClient = &client;
        const std::vector<LoadedPlugin> plugins{lp};

        const std::size_t before = cascade::core::hostBridgeCount();
        g_kept = nullptr;
        {
            PluginUi ui;
            HostServices svc;
            svc.centreHz = [] { return 100.0e6; };
            svc.sampleRateHz = [] { return 2.4e6; };
            svc.unixTimeMs = [] { return kHostTimeMs; };
            ui.setServices(std::move(svc));
            ui.rebuild(plugins);
        }
        CHECK(g_kept != nullptr);
        // Not freed - a loaded module may still hold the pointer - but no
        // longer attached to a host.
        CHECK(cascade::core::hostBridgeCount() == before + 1);
        CHECK(cascade::core::attachedHostBridgeCount() == 0);
        CHECK(g_kept->unix_time_ms(g_kept->ctx) == 0);
        CHECK(g_kept->centre_hz(g_kept->ctx) == 0.0);
        CHECK(g_kept->request_tune(g_kept->ctx, 100.0e6) == CASCADE_TUNE_FAILED);
    }

    std::printf("-- Case D: the product's teardown order --\n");
    // =====================================================================
    // Case D - the whole teardown, in the product's order: the runner's
    // instances are destroyed while the host services are still there, and a
    // decoder's destroy() reads the host clock successfully.
    //
    // This is the sequence AppWindow::detachAndUnloadPlugins() performs and
    // that ~AppWindow now performs too, instead of leaving it to reverse
    // member-declaration order, which had it backwards.
    // =====================================================================
    {
        const CascadeHostClientApi client = makeKeeperClient();
        const CascadeIqDecoderApi api = makeKeeperApi();
        LoadedPlugin lp = makePlugin("Keeper3", "keeper3.dll");
        lp.hostClient = &client;
        lp.iqDecoder = &api;
        const std::vector<LoadedPlugin> plugins{lp};

        PluginUi ui;
        PluginRunner runner;
        HostServices svc;
        svc.centreHz = [] { return 100.0e6; };
        svc.sampleRateHz = [] { return 2.4e6; };
        svc.unixTimeMs = [] { return kHostTimeMs; };
        ui.setServices(std::move(svc));

        g_kept = nullptr;
        g_timeSeenAtDestroy = -1;
        ui.rebuild(plugins);
        runner.rebuild(plugins, 48000.0, 2.4e6, 100.0e6);
        CHECK(runner.activeCount() == 1);
        CHECK(g_kept != nullptr);

        runner.clear();   // the decoder's destroy() asks the host for the time
        ui.clear();

        CHECK(g_timeSeenAtDestroy == kHostTimeMs);
    }

    return testSummary("test_plugin_destroy_reentry");
}
