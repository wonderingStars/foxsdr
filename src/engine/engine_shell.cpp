// engine_shell.cpp - the implementation of the fence's one door (engine/engine_shell.hpp; engine extraction stage 4, step 5).
//
// The only file that sees both the narrow header the interface includes and the Engine itself. Every method is the line, or the few
// lines, the window used to run on the engine directly - moved here verbatim, so the engine sees exactly the calls it saw before the
// fence, in the same order. Part of the cascade_shell target (the interface links it; the headless engine does not).
//
// SINCE SHELL RETIREMENT R7 the shell does not own the engine: the host process builds it (an EngineService - main.cpp, or the test host)
// and the shell is a VIEW of it that installs the window's host (the hang watchdog's pause and resume) with Engine::setHost and puts the
// engine's own back when it goes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine_shell.hpp"

#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <utility>

#include "core/diag_log.hpp"
#include "core/patch_radio.hpp"
#include "core/telemetry.hpp"
#include "core/version.hpp"
#include "engine/engine.hpp"
#include "engine/engine_host.hpp"

namespace cascade::engine {

namespace {

// The engine's own EngineHost, answering from the front end's ShellHooks. The interface cannot include engine_host.hpp (it names the web
// server and the plugin UI's machinery), so the two calls of the engine's three that are the window's to answer are adapted here.
//
// WHAT IT DOES NOT ANSWER IS THE CLOCK (shell retirement R7). wallTimeS stays EngineHost's own: the engine's usage journal stamped the
// session's start on the service's host in its restore, before any window existed, and every EngineHost measures from one process-wide
// origin (engine_host.cpp), so the reads that follow the install continue the stamp's clock. Forwarding glfwGetTime() here would have been
// a second clock for the same session.
//
// THE WINDOW MAY GO BEFORE THE ENGINE LETS THE ADAPTER GO, and the adapter is then detached rather than dangling: the hooks pointer is atomic
// and nulled by the shell's destructor, and an adapter the engine would not give up (its control thread is running: resetHost answers false)
// is LEAKED on purpose, loudly, so that a pause or a resume the engine makes later finds a host that does nothing instead of a dead window.
class ShellHostAdapter final : public EngineHost {
public:
    explicit ShellHostAdapter(ShellHooks& hooks) : hooks_(&hooks) {}
    void pauseWatchdog() override {
        if (ShellHooks* const h = hooks_.load(std::memory_order_acquire)) { h->pauseWatchdog(); }
    }
    void resumeWatchdog() override {
        if (ShellHooks* const h = hooks_.load(std::memory_order_acquire)) { h->resumeWatchdog(); }
    }
    void detach() { hooks_.store(nullptr, std::memory_order_release); }

private:
    std::atomic<ShellHooks*> hooks_;
};

}  // namespace

struct EngineShell::Impl {
    Impl(Engine& e, ShellHooks& hooks) : engine(&e), host(new ShellHostAdapter(hooks)) {
        if (!engine->setHost(*host)) {
            delete host;   // never installed: nothing holds it
            host = nullptr;
            const char* const sentence =
                "engine: the window could not become the engine's host - its control thread is already running (Engine::setHost refused). The "
                "engine is built and restored by the host process, and the window attaches before the control thread starts";
            cascade::core::diagWarnf("%s", sentence);
            throw std::runtime_error(sentence);
        }
    }
    ~Impl() {
        if (host == nullptr) { return; }
        if (engine->resetHost()) {
            delete host;
            return;
        }
        // The engine would not give the host up: its control thread is running. Nothing may call into this window from here on, and the adapter
        // must stay alive for as long as the engine can ask it.
        host->detach();
        cascade::core::diagWarnf("engine: the window went while the control thread was still running; its host was detached and is kept");
        std::fprintf(stderr, "engine: the window went while the control thread was still running; its host is left detached\n");
        host = nullptr;   // (leaked: the engine still holds it)
    }
    Engine* engine;
    ShellHostAdapter* host;
};

EngineShell::EngineShell() = default;
EngineShell::~EngineShell() = default;

std::unique_ptr<EngineShell> EngineShell::view(Engine& engine, ShellHooks& hooks) {
    std::unique_ptr<EngineShell> shell(new EngineShell());
    shell->impl_ = std::make_unique<Impl>(engine, hooks);
    return shell;
}

Engine& rawEngine(EngineShell& shell) { return *shell.impl_->engine; }

// (the member functions below read the engine through this)
#define SHELL_ENGINE (*impl_->engine)

// --- CLASS SHELL -------------------------------------------------------------------------------------------------------------

void EngineShell::setFrontEndWaitBeat(std::function<void()> beat) { SHELL_ENGINE.setFrontEndWaitBeat(std::move(beat)); }

void EngineShell::stepPass(PassPart part) { SHELL_ENGINE.stepPass(part); }

// --- the bounded-run seams. (The engine's fields are the pass's, so a seam reaches them through runOnPass: with the pass stepped it is
// the call on the calling thread, between its parts, as before the pump move; free-running it is the pass's own thread that does it.) --

bool EngineShell::seamMeasureTick(cascade::core::EngineMeasure& measure, const cascade::core::MeasureHooks& hooks) {
    return measure.tick(SHELL_ENGINE.pipeline_, hooks);
}
void EngineShell::seamMarkPassStats() { SHELL_ENGINE.markPassStats(); }
std::string EngineShell::seamPassStatsJson() const { return SHELL_ENGINE.passStatsJson(); }

void EngineShell::seamInstallSource(std::unique_ptr<cascade::source::IqSource> source) {
    // THE PASS INSTALLS IT (the pump move's step P4): the swap and the chain's following it are the engine's own work on the engine's
    // own state, which the free-running pass is changing beside the calling thread. Posted, then one whole pass is awaited, so the
    // generator is in and the chain follows its rate when this returns - the driver reads the rate straight after.
    Engine& e = SHELL_ENGINE;
    auto held = std::make_shared<std::unique_ptr<cascade::source::IqSource>>(std::move(source));
    e.postToPass([&e, held] {
        e.installSource(std::move(*held));  // the app's one swap door
        // EVERY INSTALL SITE ENDS WITH THE CONVERTER STEP (Engine::applyConverterForSource, which also renders the diagnostics context and
        // puts the crystal correction in force): this seam is one, and tests/test_diag_context_app.cpp holds that none forgets it.
        e.applyConverterForSource();
        e.followInputRate();
    });
    (void)e.runPassNow();
}

#undef SHELL_ENGINE

}  // namespace cascade::engine
