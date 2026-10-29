// host_measure.cpp - A BOUNDED RUN'S HOST HARNESS (engine/host_measure.hpp; shell retirement R9, 2026-10-28).
//
// Every statement here was a line of the shell (engine_shell.cpp) or of the window's frame loop, moved verbatim: the engine sees exactly the calls it
// saw before, in the same order, from the same thread (the window's - the thread that calls the hooks).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/host_measure.hpp"

#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "engine/engine.hpp"

namespace cascade::engine {

// A friend of the Engine (engine.hpp): the measurement run reaches the pipeline and the pass's own work queue.
class BoundedHost {
public:
    BoundedHost(Engine& engine, std::unique_ptr<cascade::core::EngineMeasure> measure) : engine_(engine), measure_(std::move(measure)) {
        // THE CATALOGUE DIAGNOSTIC (CASCADE_PLUGIN_TEST) is read HERE, in a bounded run's host, and honoured in no other mode: an interactive session
        // can never be told to print a report by a stray environment variable. (The window reads the same variable for the fetch it starts.)
        const char* const hook = std::getenv("CASCADE_PLUGIN_TEST");
        if (hook != nullptr && *hook != '\0') { engine_.setPluginTestReport(true); }
    }

    // THE STEP: one part of the engine's pass on its own thread (the front end waits for it); with no control thread the caller runs it itself.
    void stepPass(cascade::core::PassPart part) { engine_.stepPass(part); }

    // THE END OF A FRAME. `nextFrame` is the index of the frame about to begin: the number the window's frame counter holds for it, which is what the
    // catalogue report names when a fetch resolves in that frame's pass (the window handed the same number over at the head of the frame before R9).
    bool onFrame(int nextFrame, const cascade::core::BoundedClient& client) {
        engine_.notePluginTestFrame(nextFrame);
        client_ = &client;
        // The engine measurement's one step a frame, through the same COMMANDS the START key, the mode buttons and a click on the spectrum submit -
        // queued, so a tune-to-audio latency measured here includes the command path's own wait for the next frame's drain.
        if (!measure_) { return true; }
        cascade::core::MeasureHooks hooks;
        hooks.startReceiver = client.startReceiver;
        hooks.setMode = client.setMode;
        hooks.tuneVfoHz = client.tuneVfoHz;
        hooks.markPassStats = [this] { engine_.markPassStats(); };
        hooks.passStatsJson = [this] { return engine_.passStatsJson(); };
        hooks.installSource = [this](std::unique_ptr<cascade::source::IqSource> src) { installOnPass(std::move(src)); };
        const bool more = measure_->tick(engine_.pipeline_, hooks);
        client_ = nullptr;
        if (!more) {
            measure_.reset();
            return false;
        }
        return true;
    }

private:
    // THE PASS INSTALLS IT (the pump move's step P4): the swap and the chain's following it are the engine's own work on the engine's own state, which the
    // free-running pass is changing beside the calling thread. Posted, then one whole pass is awaited, so the generator is in and the chain follows its rate
    // when this returns - the driver reads the rate straight after.
    void installOnPass(std::unique_ptr<cascade::source::IqSource> source) {
        Engine& e = engine_;
        auto held = std::make_shared<std::unique_ptr<cascade::source::IqSource>>(std::move(source));
        e.postToPass([&e, held] {
            e.installSource(std::move(*held));  // the app's one swap door
            // EVERY INSTALL SITE ENDS WITH THE CONVERTER STEP (Engine::applyConverterForSource, which also renders the diagnostics context and puts the
            // crystal correction in force): this is one, and tests/test_diag_context_app.cpp holds that none forgets it.
            e.applyConverterForSource();
            e.followInputRate();
        });
        // The window's frame is blocked in this wait, not hung: it holds its watchdog's pause for exactly this long (BoundedClient::blocked).
        if (client_ != nullptr && client_->blocked) { client_->blocked(true); }
        (void)e.runPassNow();
        if (client_ != nullptr && client_->blocked) { client_->blocked(false); }
    }

    Engine& engine_;
    const cascade::core::BoundedClient* client_ = nullptr;   // the window's, for the length of one onFrame call
    std::unique_ptr<cascade::core::EngineMeasure> measure_;
};

cascade::core::BoundedHooks makeBoundedHooks(Engine& engine, std::unique_ptr<cascade::core::EngineMeasure> measure) {
    const std::shared_ptr<BoundedHost> host = std::make_shared<BoundedHost>(engine, std::move(measure));
    cascade::core::BoundedHooks hooks;
    hooks.stepPass = [host](cascade::core::PassPart part) { host->stepPass(part); };
    hooks.onFrame = [host](int nextFrame, const cascade::core::BoundedClient& client) { return host->onFrame(nextFrame, client); };
    return hooks;
}

}  // namespace cascade::engine
