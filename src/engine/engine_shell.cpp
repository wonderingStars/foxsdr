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
#include "core/plugin_directive.hpp"
#include "core/plugin_ui.hpp"
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

// The plugin UI the window holds, over the API core the engine owns, and the front-end side of the engine's directive link.
class PluginFeedImpl final : public HostPluginFeed {
public:
    PluginFeedImpl(Engine& engine, Steps steps, std::function<void(std::vector<cascade::core::HostImage>&)> pollImages)
        : engine_(engine),
          steps_(std::move(steps)),
          pollImages_(std::move(pollImages)),
          ui_(engine.pluginApiShared()),
          directives_(engine.pluginDirectives()) {
        // THE PLUGIN UI'S APPLIER, before the scan the start-up runs: the engine applies a directive through it inline while it runs on
        // the front end's thread, and posts it for the frame's head to apply while its control thread does (Engine::setPluginUiApplier).
        engine_.setPluginUiApplier([this](const cascade::core::PluginDirective& d) { apply(d); });
    }
    ~PluginFeedImpl() override { disconnect(); }

    const std::vector<cascade::core::HostTrack>& tracks() const override { return ui_.tracks(); }
    const std::vector<cascade::core::HostPath>& paths() const override { return ui_.paths(); }
    const std::vector<cascade::core::HostPanel>& panels() const override { return ui_.panels(); }
    const std::vector<cascade::core::HostInstrument>& instruments() const override { return ui_.instruments(); }
    const std::vector<std::string>& trackPluginNames() const override { return ui_.trackPluginNames(); }
    bool altitudeNear(const std::string& plugin, const std::string& id, double latDeg, double lonDeg,
                      double& outAltM) override {
        return ui_.altitudeNear(plugin, id, latDeg, lonDeg, outAltM);
    }
    void poll() override { ui_.poll(); }
    void pollImages(std::vector<cascade::core::HostImage>& out) override { pollImages_(out); }
    void applyNewestDirective() override {
        (void)directives_.applyNewest([this](const cascade::core::PluginDirective& d) { apply(d); });
    }
    PluginUiReport report() const override {
        PluginUiReport r;
        r.tracks = ui_.tracks();
        r.paths = ui_.paths().size();
        r.panels = ui_.panels();
        return r;
    }
    // (the tests', through rawPluginUi)
    cascade::core::PluginUi& ui() { return ui_; }
    void disconnect() override {
        if (connected_) {
            connected_ = false;
            engine_.setPluginUiApplier({});
        }
    }

private:
    // What one directive does: the window's step before, the plugin UI rebuilt or emptied, the window's step after.
    void apply(const cascade::core::PluginDirective& d) {
        PluginDirectiveFacts f;
        f.release = d.release;
        f.basemap = d.basemap;
        f.trackInfo = d.trackInfo;
        if (steps_.before) { steps_.before(f); }
        ui_.apply(d);
        if (steps_.after) { steps_.after(f); }
    }

    Engine& engine_;
    Steps steps_;
    std::function<void(std::vector<cascade::core::HostImage>&)> pollImages_;
    cascade::core::PluginUi ui_;
    cascade::core::PluginDirectiveLink& directives_;
    bool connected_ = true;
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
cascade::core::PluginUi& rawPluginUi(HostPluginFeed& feed) { return static_cast<PluginFeedImpl&>(feed).ui(); }

// (the member functions below read the engine through this)
#define SHELL_ENGINE (*impl_->engine)

// --- CLASS SHELL -------------------------------------------------------------------------------------------------------------

void EngineShell::initialise() { SHELL_ENGINE.initialise(); }
void EngineShell::loadBookmarks() { SHELL_ENGINE.loadBookmarks(); }
void EngineShell::telemetryStartup(const cascade::core::AppConfig& cfg, const std::string& configDir) {
    SHELL_ENGINE.telemetryStartup(cfg, configDir);
}
void EngineShell::applyConfig(const cascade::core::AppConfig& cfg) { SHELL_ENGINE.applyConfig(cfg); }
std::string EngineShell::pluginCatalogueUrl() const { return SHELL_ENGINE.pluginCatalogueUrl_; }

EngineShell::ConfigReadback EngineShell::configReadback() const {
    Engine& e = SHELL_ENGINE;
    ConfigReadback r;
    r.modeIndex = e.modeIndex_;
    r.centreHz = e.pipeline_.activeSource().centerFrequencyHz();
    return r;
}

cascade::core::AppConfig EngineShell::configSnapshot() const { return SHELL_ENGINE.configSnapshot(); }
void EngineShell::publishConfig() { SHELL_ENGINE.publishConfig(); }
void EngineShell::publishStatusText() { SHELL_ENGINE.publishStatusText(); }
EngineStatusText EngineShell::statusText() const { return SHELL_ENGINE.statusText(); }
void EngineShell::setFrontEndFacts(FrontEndFacts facts) { SHELL_ENGINE.setFrontEndFacts(std::move(facts)); }
void EngineShell::setFrontEndWaitBeat(std::function<void()> beat) { SHELL_ENGINE.setFrontEndWaitBeat(std::move(beat)); }
void EngineShell::setControlWatchdogReportDir(const std::string& dir) { SHELL_ENGINE.setControlWatchdogReportDir(dir); }
const FoxEngineApi* EngineShell::apiTable() { return engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }
FoxEngine* EngineShell::apiHandle() { return SHELL_ENGINE.apiHandle(); }

void EngineShell::startReceiver() { SHELL_ENGINE.startReceiver(); }
void EngineShell::controlPass() { SHELL_ENGINE.controlPass(); }
void EngineShell::startControlThread(const ControlThreadOptions& options) { SHELL_ENGINE.startControlThread(options); }
void EngineShell::stepPass(PassPart part) { SHELL_ENGINE.stepPass(part); }

void EngineShell::configureHeartbeat() {
    Engine& e = SHELL_ENGINE;
    e.telemetryHeartbeat_.configure(cascade::core::telemetryEndpoint(), e.telemetryInstallId_, cascade::versionString());
}
void EngineShell::markCleanExit() { SHELL_ENGINE.telemetryCleanExit_ = true; }

void EngineShell::stopTransfers() { SHELL_ENGINE.stopTransfers(); }
void EngineShell::teardown() { SHELL_ENGINE.teardown(); }
void EngineShell::shutdownQuiesce() { SHELL_ENGINE.shutdownQuiesce(); }
void EngineShell::shutdownStop() { SHELL_ENGINE.shutdownStop(); }
void EngineShell::shutdownRelease(std::chrono::steady_clock::time_point drainDeadline) { SHELL_ENGINE.shutdownRelease(drainDeadline); }
void EngineShell::reportPluginStatus(const std::vector<cascade::core::HostImage>& images, const PluginUiReport& ui) {
    SHELL_ENGINE.reportPluginStatus(images, ui);
}

// --- the bounded-run seams. (The engine's fields are the pass's, so a seam reaches them through runOnPass: with the pass stepped it is
// the call on the calling thread, between its parts, as before the pump move; free-running it is the pass's own thread that does it.) --

void EngineShell::seamBiasStandIn(cascade::gui::BiasStandIn standIn) { SHELL_ENGINE.biasStandIn_ = standIn; }

void EngineShell::seamFetchCatalogue(const std::string& url) {
    Engine& e = SHELL_ENGINE;
    e.runOnPass([&] {
        e.pluginCatalogueUrl_ = url;
        e.startCatalogFetch();
    });
}

void EngineShell::seamSelectNativeDevice(const std::string& deviceKey) {
    Engine& e = SHELL_ENGINE;
    e.runOnPass([&] {
        e.scanNative();
        for (std::size_t i = 0; i < e.nativeDevices_.size(); ++i) {
            if (cascade::core::patch::makeDeviceKey(e.nativeDevices_[i].driver, e.nativeDevices_[i].args) == deviceKey) {
                // The row's own command, as a click would send it.
                // (the engine's own spelling of the row's id, a const query: the table's fact.source.id.<n> is the same function)
                e.submitCommand(cascade::core::cmd::makeText(FOXAPI_OP_SELECT_SOURCE,
                                                             e.sourceIdForRow(kNativeRowBase + static_cast<int>(i))));
                break;
            }
        }
    });
}

bool EngineShell::seamDeviceOpenPending() {
    Engine& e = SHELL_ENGINE;
    bool pending = false;
    e.runOnPass([&] { pending = e.deviceOpenPending_; });
    return pending;
}

EngineShell::PresetPress EngineShell::seamPressPreset(const std::string& name) {
    Engine& e = SHELL_ENGINE;
    PresetPress out;
    // What the pass knows - the loaded plugins, their validated presets - is read where the pass cannot change it; the windows a plugin
    // with no preset opens are the front end's own and are opened after.
    e.runOnPass([&] {
        for (const cascade::core::LoadedPlugin& lp : e.pluginHost_.plugins()) {
            if (lp.name != name) { continue; }
            out.found = true;
            const std::vector<cascade::gui::IndexedPreset> ps = e.validatedPresets(lp);
            if (!ps.empty()) {
                // Its key's own command, queued AFTER the tune above so the preset still has the last word.
                e.submitCommand(cascade::core::cmd::makeText(FOXAPI_OP_PLUGIN_PRESET, cascade::core::pluginKey(lp),
                                                             static_cast<std::int64_t>(ps.front().index)));
            } else {
                e.recordPluginStopped(cascade::core::pluginKey(lp), false);
                out.openWindows = true;
                out.name = lp.name;
                out.capabilities = lp.capabilities;
            }
            break;
        }
    });
    return out;
}

void EngineShell::seamPatchStart() {
    Engine& e = SHELL_ENGINE;
    e.runOnPass([&] { e.patchRunning_ = true; });
}

bool EngineShell::seamPatchRunning() {
    Engine& e = SHELL_ENGINE;
    bool running = false;
    e.runOnPass([&] { running = e.patchRunning_; });
    return running;
}

void EngineShell::seamSubmit(const FoxCommand& command) { SHELL_ENGINE.submitCommand(command); }
void EngineShell::seamSubmit(cascade::core::cmd::QueuedCommand command) { SHELL_ENGINE.submitCommand(std::move(command)); }

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

// --- CLASS HOST --------------------------------------------------------------------------------------------------------------

std::unique_ptr<HostPluginFeed> EngineShell::makePluginFeed(HostPluginFeed::Steps steps) {
    Engine& e = SHELL_ENGINE;
    return std::make_unique<PluginFeedImpl>(
        e, std::move(steps), [&e](std::vector<cascade::core::HostImage>& out) { e.pluginRunner_.pollImages(out); });
}

// --- CLASS WEB / CAT ---------------------------------------------------------------------------------------------------------

void EngineShell::setRemoteControlSource(RemoteControlSource source) { SHELL_ENGINE.setRemoteControlSource(std::move(source)); }
void EngineShell::webRemoteBeat() { SHELL_ENGINE.api().webRemoteBeat(); }
std::shared_ptr<const cascade::core::ReceiverSnapshot::Full> EngineShell::readFullSnapshot() const {
    return SHELL_ENGINE.receiverSnapshot_->readFull();
}
bool EngineShell::readSnapshot(cascade::core::PublishedState& out) const { return SHELL_ENGINE.receiverSnapshot_->read(out); }
std::uint64_t EngineShell::audioSamplesProduced() const { return SHELL_ENGINE.pipeline_.audioSamplesProduced(); }
std::size_t EngineShell::audioTap(float* out, std::size_t frames) const { return SHELL_ENGINE.pipeline_.audioTap(out, frames); }
double EngineShell::inputRateHz() const { return SHELL_ENGINE.pipeline_.inputRateHz(); }

#undef SHELL_ENGINE

}  // namespace cascade::engine
