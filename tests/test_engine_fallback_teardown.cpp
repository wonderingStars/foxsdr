// test_engine_fallback_teardown.cpp - ~Engine's FALLBACK teardown must not
// call hooks on a half-destroyed front end (engine/stage3b-pre "3b-pre"
// round, docs/engine-stage3.md, the plan memory's Low item: "~Engine
// fallback teardown can call hooks on a half-destroyed window if AppWindow
// ctor throws").
//
// THE SCENARIO. gui::AppWindow declares `std::unique_ptr<Engine>
// engineHolder_` FIRST, specifically so it is destroyed LAST - after every
// window member that could still call into it. A NORMAL AppWindow lifetime
// never reaches Engine's fallback destructor path at all: ~AppWindow calls
// engine_.teardown() itself, which sets Engine::tornDown_, before
// engineHolder_ is ever destroyed. The one way the fallback path
// (`if (!tornDown_) { stopTransfers(); teardown(); }` in ~Engine) runs with
// an EXTERNAL host is AppWindow's OWN CONSTRUCTOR throwing before it gets
// that far: the object is never "born" (its destructor never runs at all),
// so its vtable stays its own throughout the unwind, while every member
// declared AFTER engineHolder_ that HAD been constructed already is torn
// down in reverse order - possibly before engineHolder_ (and so ~Engine)
// ever runs. teardown() unconditionally calls host_.onPluginsUnloading()
// (via detachAndUnloadPlugins()), so a host override that reads the
// front end's own data would read data that is already gone.
//
// THIS TEST reproduces the exact shape with a minimal stand-in front end
// (FakeWindow) instead of the real, heavy AppWindow: engineHolder_ declared
// first, a Sentinel member declared after it (mirroring one of AppWindow's
// real members - the basemap, the plugin UI, anything reachable from
// onPluginsUnloading), and a constructor that throws after both are built.
// FakeWindow overrides onPluginsUnloading() to record whether it is called
// AFTER Sentinel's destructor has already run - which is exactly the
// half-destroyed state the real bug would read from. A real
// crash/UB-on-destroyed-state can't be scripted portably as a pass/fail
// check; this is the closest observable proxy the same bug produces: the
// hook fires, and it fires late.
//
// WITH THE FIX, FakeWindow::onPluginsUnloading() must never run at all: the
// fallback repoints host_ at the engine's own ownHost_ before it does
// anything, so teardown()'s unconditional onPluginsUnloading() call lands on
// ownHost_'s harmless default, never on FakeWindow's override.
//
// PROVEN RED against the named mutant (dropping the `host_ = &ownHost_;`
// reseat at the top of ~Engine's fallback block, so host_ still refers to
// the half-destroyed FakeWindow): 2 of 5 checks failed
// (!g_hookCalledAtAll, !g_hookCalledAfterSentinelGone - the hook fired, and
// fired late), source restored byte-identical (diffed against a saved copy)
// and reverified green, 5/5.
//
// LINKS THE ENGINE ALONE (no gui/ header): tests/CMakeLists.txt links any
// test that does not include one against cascade_engine, so this binary
// existing and running is itself proof the fix needs nothing of the window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <memory>
#include <stdexcept>

#include "engine/engine.hpp"
#include "engine/engine_host.hpp"
#include "test_check.hpp"

namespace {

using cascade::engine::Engine;
using cascade::engine::EngineHost;

// Set by FakeWindow::onPluginsUnloading if it ever runs after sentinelDestroyed_
// is true. Static (not a FakeWindow member) because the whole point is that
// FakeWindow itself is destroyed abnormally (constructor threw) - nothing
// outside the throwing constructor's stack frame may safely hold a pointer
// into it once unwinding starts, but a call the *hook itself* makes while
// FakeWindow is still alive (mid-unwind, before ~FakeWindow - which never
// runs) is exactly what this records.
bool g_hookCalledAfterSentinelGone = false;
bool g_hookCalledAtAll = false;
// Set by Sentinel's own destructor, independent of FakeWindow's instance
// field of almost the same name - proof the risky ordering (Sentinel gone
// before engineHolder_/~Engine runs) really happened, whether or not the
// hook fired at all. Without this, a change that broke the SCENARIO (say,
// FakeWindow's constructor no longer throwing) would leave every CHECK below
// vacuously green.
bool g_sentinelDestroyed = false;

// A stand-in for one of AppWindow's many members that engine host hooks
// reach into (the basemap, the plugin UI, the store deck...): declared AFTER
// engineHolder_ below, so the unwind destroys it BEFORE engineHolder_ (and so
// before ~Engine) runs, exactly mirroring AppWindow's real field order.
struct Sentinel {
    bool* destroyedFlag;
    explicit Sentinel(bool* flag) : destroyedFlag(flag) {}
    ~Sentinel() {
        *destroyedFlag = true;
        g_sentinelDestroyed = true;
    }
};

// The minimal front end: implements EngineHost (as AppWindow does), holds
// its Engine by unique_ptr declared FIRST, and throws out of its OWN
// constructor after both members exist - so FakeWindow's destructor never
// runs (construction failed), but Sentinel's already has by the time
// engineHolder_ unwinds.
struct FakeWindow : EngineHost {
    std::unique_ptr<Engine> engineHolder_;  // declared first: destroyed LAST
    Engine& engine_;
    bool sentinelDestroyed_ = false;
    Sentinel sentinel_{&sentinelDestroyed_};  // declared after: destroyed FIRST

    void onPluginsUnloading() override {
        g_hookCalledAtAll = true;
        if (sentinelDestroyed_) { g_hookCalledAfterSentinelGone = true; }
    }

    FakeWindow()
        : engineHolder_(std::make_unique<Engine>(*this)), engine_(*engineHolder_) {
        throw std::runtime_error("simulated AppWindow constructor failure");
    }
};

}  // namespace

int main() {
    std::printf("test_engine_fallback_teardown\n");

    bool threw = false;
    try {
        FakeWindow fw;
        (void)fw;
    } catch (const std::exception& e) {
        threw = true;
        std::printf("  FakeWindow() threw as scripted: %s\n", e.what());
    }
    CHECK(threw);
    // SANITY: the risky ordering this test is built to reproduce really
    // happened - Sentinel (declared after engineHolder_) was destroyed during
    // the unwind, before engineHolder_'s own destructor (and so ~Engine) ran.
    // Without this, a change that broke the SCENARIO itself (not the fix)
    // would leave the two CHECKs below vacuously green.
    CHECK(g_sentinelDestroyed);
    // THE ACTUAL PROPERTY: the fallback teardown must never reach the front
    // end's override AT ALL - it detaches to the engine's own host before
    // doing anything, precisely because it cannot tell a safe external host
    // from one mid-unwind. teardown()'s onPluginsUnloading() call is
    // unconditional (no plugins need to be loaded for it to fire), so with
    // the fix it always lands on ownHost_'s own no-op, never on
    // FakeWindow's override.
    CHECK(!g_hookCalledAtAll);
    CHECK(!g_hookCalledAfterSentinelGone);
    std::printf("  onPluginsUnloading called at all: %s; called after Sentinel was gone: %s\n",
                g_hookCalledAtAll ? "yes" : "no",
                g_hookCalledAfterSentinelGone ? "yes" : "no");

    // A second, independent front end (the normal, well-behaved case): an
    // Engine whose owner calls teardown() itself before destruction reaches
    // tornDown_ = true, so this fallback path is dead code for it and the
    // host is never redirected - a control against the fix disabling itself
    // even in the ordinary case a headless Engine already covers elsewhere
    // (tests/test_engine_headless.cpp).
    {
        struct NormalHost : EngineHost {
            int unloadCalls = 0;
            void onPluginsUnloading() override { ++unloadCalls; }
        };
        NormalHost host;
        {
            Engine e(host);
            e.stopTransfers();
            e.teardown();
        }  // ~Engine here: tornDown_ is already true, fallback does nothing more
        CHECK(host.unloadCalls == 1);
        std::printf("  normal teardown: onPluginsUnloading called %d time(s) (expect 1)\n",
                    host.unloadCalls);
    }

    return testSummary("test_engine_fallback_teardown");
}
