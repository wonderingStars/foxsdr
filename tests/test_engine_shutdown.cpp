// test_engine_shutdown.cpp - the engine's own teardown, in its order
// (engine/stage3b-pre, docs/engine-stage3.md OPEN 9 and OPEN 7 (d)).
//
// Until this round AppWindow::run() spelled the engine's teardown out line by
// line between its own steps. Now the engine owns it, in three ordered phases
// the window calls between its steps (it has a final config save that must
// read live state before the pipeline stops, and a clean-exit marker that must
// wait for the join), and Engine::shutdown() runs all three for a front end
// with nothing of its own to do in between:
//   shutdownQuiesce  the TRANSMITTER FIRST (a keyed radio cannot wait), then
//                    the GPS reader and its last poll, then both recordings -
//                    everything the final save must see settled. Stage 3b
//                    joins the control thread AFTER this phase, so the
//                    transmitter is always stopped before that join (OPEN 7
//                    (d)).
//   shutdownStop     the bookmark flush, then the pipeline join.
//   shutdownRelease  the patch's radios, then the plugins unloaded.
//
//   A  a keyed transmitter and a running receiver: after shutdownQuiesce the
//      radio is quiet and the key open while the pipeline is STILL RUNNING -
//      the transmitter really is first
//   B  after shutdownStop the pipeline has stopped
//   C  Engine::shutdown() on a second engine does all of it, and is safe to
//      call twice
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "source/tx_sink.hpp"
#include "test_check.hpp"

using cascade::engine::Engine;

namespace {

class RecordingSink : public cascade::source::IqSink {
public:
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    void finish() override { stop(); }
    bool running() const override { return running_; }
    double sampleRateHz() const override { return 2.5e6; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 146.52e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 70.0e6;
        hi = 6.0e9;
        return true;
    }
    bool sampleRateRangeHz(double& lo, double& hi) const override {
        lo = 2.083e6;
        hi = 61.44e6;
        return true;
    }
    bool gainRangeDb(double& lo, double& hi) const override {
        lo = -89.75;
        hi = 0.0;
        return true;
    }
    double gainDb() const override { return 0.0; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t n) override { return running_ ? n : 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "test radio"; }
    const char* lastError() const override { return ""; }

private:
    std::atomic<bool> running_{false};
};

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static RecordingSink* keyTransmitter(Engine& e) {
        auto sink = std::make_unique<RecordingSink>();
        RecordingSink* raw = sink.get();
        e.transmitter_.setSink(std::move(sink));
        e.transmitter_.setInput(cascade::core::TxInput::Tone);
        e.pumpTransmitter(/*pageLive=*/true, /*latchPressed=*/true, /*pttHeld=*/false);
        return raw;
    }
    static bool transmitting(Engine& e) { return e.transmitter_.transmitting(); }
    static bool latched(Engine& e) { return e.transmitter_.latched(); }
    static void startReceiver(Engine& e) { e.startReceiver(); }
    static bool running(Engine& e) { return e.pipeline_.running(); }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_shutdown\n");
#if defined(_WIN32)
    const std::string tmp = std::string(std::getenv("TEMP") != nullptr ? std::getenv("TEMP") : ".") +
                            "\\foxsdr_engine_shutdown_" + std::to_string(_getpid());
#else
    const std::string tmp = "/tmp/foxsdr_engine_shutdown_" + std::to_string(static_cast<int>(getpid()));
#endif
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, tmp);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    {
        Engine e;
        e.initialise();
        A::startReceiver(e);
        RecordingSink* radio = A::keyTransmitter(e);
        CHECK(A::transmitting(e));
        CHECK(radio->running());
        CHECK(A::running(e));

        // A
        e.shutdownQuiesce();
        std::printf("A: after shutdownQuiesce - transmitting=%d radio running=%d latched=%d, pipeline "
                    "running=%d\n",
                    A::transmitting(e) ? 1 : 0, radio->running() ? 1 : 0, A::latched(e) ? 1 : 0,
                    A::running(e) ? 1 : 0);
        CHECK(!A::transmitting(e));
        CHECK(!radio->running());
        CHECK(!A::latched(e));
        CHECK(A::running(e));   // the receiver is still going: the transmitter came FIRST

        // B
        e.shutdownStop();
        std::printf("B: after shutdownStop - pipeline running=%d\n", A::running(e) ? 1 : 0);
        CHECK(!A::running(e));
        e.shutdownRelease();
    }

    // C
    {
        Engine e;
        e.initialise();
        A::startReceiver(e);
        RecordingSink* radio = A::keyTransmitter(e);
        CHECK(radio->running());
        e.shutdown();
        e.shutdown();   // twice: every step is idempotent
        std::printf("C: after Engine::shutdown() twice - transmitting=%d pipeline running=%d\n",
                    A::transmitting(e) ? 1 : 0, A::running(e) ? 1 : 0);
        CHECK(!A::transmitting(e));
        CHECK(!radio->running());
        CHECK(!A::running(e));
    }

    return testSummary("test_engine_shutdown");
}
