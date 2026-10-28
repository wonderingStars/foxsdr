// engine_shell.cpp - the implementation of the fence's one door (engine/engine_shell.hpp; engine extraction stage 4, step 5).
//
// The only file that sees both the narrow header the interface includes and the Engine itself. Every method is the line, or the few
// lines, the window used to run on the engine directly - moved here verbatim, so the engine sees exactly the calls it saw before the
// fence, in the same order. Part of the cascade_shell target (the interface links it; the headless engine does not).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/engine_shell.hpp"

#include <utility>

#include "core/patch_radio.hpp"
#include "core/telemetry.hpp"
#include "core/version.hpp"
#include "engine/engine.hpp"
#include "engine/engine_host.hpp"
#include "engine/fox_engine_api.hpp"

namespace cascade::engine {

namespace {

// The engine's own EngineHost, answering from the front end's ShellHooks. The interface cannot include engine_host.hpp (it names the web
// server and the plugin UI's machinery), so the three calls the engine still makes of its front end are adapted here.
class ShellHostAdapter final : public EngineHost {
public:
    explicit ShellHostAdapter(ShellHooks& hooks) : hooks_(hooks) {}
    double wallTimeS() const override { return hooks_.wallTimeS(); }
    void pauseWatchdog() override { hooks_.pauseWatchdog(); }
    void resumeWatchdog() override { hooks_.resumeWatchdog(); }

private:
    ShellHooks& hooks_;
};

}  // namespace

struct EngineShell::Impl {
    explicit Impl(ShellHooks& hooks) : host(hooks), engine(std::make_unique<Engine>(static_cast<EngineHost&>(host))) {}
    // Declared first, destroyed last: the engine asks it until its last call.
    ShellHostAdapter host;
    std::unique_ptr<Engine> engine;
};

EngineShell::EngineShell() = default;
EngineShell::~EngineShell() = default;

std::unique_ptr<EngineShell> EngineShell::create(ShellHooks& hooks) {
    std::unique_ptr<EngineShell> shell(new EngineShell());
    shell->impl_ = std::make_unique<Impl>(hooks);
    return shell;
}

Engine& rawEngine(EngineShell& shell) { return *shell.impl_->engine; }

// (the member functions below read the engine through this)
#define SHELL_ENGINE (*impl_->engine)

// --- CLASS SHELL -------------------------------------------------------------------------------------------------------------

void EngineShell::initialise() { SHELL_ENGINE.initialise(); }
void EngineShell::loadBookmarks() { SHELL_ENGINE.loadBookmarks(); }
void EngineShell::telemetryStartup(const cascade::core::AppConfig& cfg, const std::string& configDir) {
    SHELL_ENGINE.telemetryStartup(cfg, configDir);
}
void EngineShell::applyConfig(const cascade::core::AppConfig& cfg) { SHELL_ENGINE.applyConfig(cfg); }

EngineShell::ConfigReadback EngineShell::configReadback() const { return SHELL_ENGINE.configReadback(); }

cascade::core::AppConfig EngineShell::configSnapshot() const { return SHELL_ENGINE.configSnapshot(); }
void EngineShell::publishConfig() { SHELL_ENGINE.publishConfig(); }
void EngineShell::setFrontEndWaitBeat(std::function<void()> beat) { SHELL_ENGINE.setFrontEndWaitBeat(std::move(beat)); }
const FoxEngineApi* EngineShell::apiTable() { return engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }
FoxEngine* EngineShell::apiHandle() { return SHELL_ENGINE.apiHandle(); }

void EngineShell::startReceiver() { SHELL_ENGINE.startReceiver(); }
void EngineShell::controlPass() { SHELL_ENGINE.controlPass(); }
void EngineShell::startControlThread(const ControlThreadOptions& options) { SHELL_ENGINE.startControlThread(options); }
void EngineShell::stepPass(PassPart part) { SHELL_ENGINE.stepPass(part); }

void EngineShell::configureHeartbeat() { SHELL_ENGINE.configureHeartbeat(); }
void EngineShell::markCleanExit() { SHELL_ENGINE.telemetryCleanExit_ = true; }

void EngineShell::stopTransfers() { SHELL_ENGINE.stopTransfers(); }
void EngineShell::teardown() { SHELL_ENGINE.teardown(); }
void EngineShell::shutdownQuiesce() { SHELL_ENGINE.shutdownQuiesce(); }
void EngineShell::shutdownStop() { SHELL_ENGINE.shutdownStop(); }
void EngineShell::shutdownRelease() { SHELL_ENGINE.shutdownRelease(); }
void EngineShell::shutdownDrain(std::chrono::steady_clock::time_point drainDeadline) { SHELL_ENGINE.shutdownDrain(drainDeadline); }

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
