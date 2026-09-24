// A patch radio ABANDONED by stop() - its driver would not give the reader
// thread back within kStopWaitMs - must not leave its decoder plugins' handles
// to die later on that abandoned thread.
//
// THE DEFECT. PatchRadio::stop() detaches a wedged reader and swaps in a fresh
// Shared, so the OLD Shared - and with it the Runner, the running StripSet and
// every decoder handle in it - is owned only by the detached thread. Nothing
// flushed that runner: the app's plugin-unload path (detachAndUnloadPlugins)
// flushes the runners of the radios still in its map, and this one had just
// been erased from it (and would answer runner() from the NEW Shared anyway).
// So after a rescan unmapped the plugin DLLs, the reader finally coming back
// from the driver destroyed the old Shared ON ITS OWN THREAD, calling the
// plugin's destroy() - into code that was no longer mapped, on a thread the
// ABI does not allow. The same sequence runs at every exit.
//
// THE INTERLEAVING IS DRIVEN, NOT HOPED FOR. The source below delivers blocks
// normally until the test wedges it; its next read() then parks and IGNORES
// stop() - the wedged vendor stack - until the test lets it go. So the test
// knows the reader is inside read() when stop() runs, that stop() must take
// the abandonment path, and exactly when the abandoned thread wakes.
//
// The fake decoder is written to the real C ABI and records every destroy()
// and the thread it ran on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/patch_graph.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_radio.hpp"
#include "core/patch_runner.hpp"
#include "core/plugin_abi.h"
#include "source/iq_source.hpp"
#include "test_check.hpp"

using namespace cascade::core::patch;

namespace {

constexpr double kRate = 2400000.0;
constexpr double kCentre = 100000000.0;

// --- the fake decoder ------------------------------------------------------------

struct Ledger {
    std::mutex m;
    int creates = 0;
    int destroys = 0;
    std::vector<std::thread::id> destroyThreads;
};
Ledger g_ledger;
std::atomic<long long> g_processCalls{0};

int g_handle = 0;

void* iqCreate(double, double) {
    std::lock_guard<std::mutex> lock(g_ledger.m);
    ++g_ledger.creates;
    return &g_handle;
}

void iqProcess(void*, const float*, std::size_t) {
    g_processCalls.fetch_add(1, std::memory_order_relaxed);
}

std::int32_t iqPoll(void*, char*, std::size_t) { return 0; }

void iqDestroy(void*) {
    std::lock_guard<std::mutex> lock(g_ledger.m);
    ++g_ledger.destroys;
    g_ledger.destroyThreads.push_back(std::this_thread::get_id());
}

int destroys() {
    std::lock_guard<std::mutex> lock(g_ledger.m);
    return g_ledger.destroys;
}

// --- a source whose driver wedges on command ---------------------------------------

std::atomic<bool> g_sourceGone{false};

class WedgingSource final : public cascade::source::IqSource {
public:
    ~WedgingSource() override { g_sourceGone.store(true, std::memory_order_release); }
    bool start() override { return true; }
    // A wedged vendor stack: the abort is accepted and does nothing.
    void stop() override { stopCalls_.fetch_add(1, std::memory_order_relaxed); }
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return kCentre; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Wedging test source"; }
    const char* lastError() const override { return ""; }

    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::unique_lock<std::mutex> lk(m_);
        if (wedge_) {
            wedged_ = true;
            cv_.notify_all();
            cv_.wait(lk, [&] { return released_; });
            return 0;
        }
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        for (std::size_t i = 0; i < n; ++i) { dst[i] = std::complex<float>(0.5f, 0.0f); }
        return n;
    }

    void wedge() {
        std::lock_guard<std::mutex> lk(m_);
        wedge_ = true;
    }
    bool waitWedged(int ms) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return wedged_; });
    }
    void release() {
        {
            std::lock_guard<std::mutex> lk(m_);
            released_ = true;
        }
        cv_.notify_all();
    }
    int stopCalls() const { return stopCalls_.load(std::memory_order_relaxed); }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool wedge_ = false;
    bool wedged_ = false;
    bool released_ = false;
    std::atomic<int> stopCalls_{0};
};

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    CascadeIqDecoderApi api{};
    api.structSize = sizeof(CascadeIqDecoderApi);
    api.requiredRateHz = 0.0;
    api.preferredRateHz = 0.0;
    api.create = iqCreate;
    api.process = iqProcess;
    api.retune = nullptr;
    api.poll_text = iqPoll;
    api.destroy = iqDestroy;
    const std::vector<DecoderInfo> cat = {{"wide.dll", "Wide", PortType::Iq, 0.0}};
    std::vector<PluginApis> apis(1);
    apis[0].iq = &api;

    // Radio -> I/Q decoder on the whole capture.
    Graph g;
    const NodeId radio = g.addNode(NodeKind::Radio, "Radio", PortType::Iq);
    const NodeId dec = g.addNode(NodeKind::Decoder, "Wide", PortType::Iq);
    g.mutableNode(dec)->plugin = "wide.dll";
    CHECK(g.connect(radio, 0, dec, 0) == Connect::Ok);
    const Plan plan = compile(g, kRate, kCentre, &cat);
    CHECK(plan.decoders.size() == 1u);

    const std::thread::id control = std::this_thread::get_id();

    {
        auto owned = std::make_unique<WedgingSource>();
        WedgingSource* src = owned.get();
        PatchRadio r(radio, std::move(owned), "Wedging radio");
        r.runner().publish(buildStripSet(plan, g, kRate, kNoNode, 48000.0, &cat, &apis));
        {
            std::lock_guard<std::mutex> lock(g_ledger.m);
            CHECK(g_ledger.creates == 1);
        }

        std::string err;
        CHECK(r.start(err));
        // The reader has adopted the set and is feeding the decoder.
        CHECK(waitFor([&] { return g_processCalls.load() >= 2; }, 5000));

        // Wedge the driver, and wait until the reader is parked inside it.
        src->wedge();
        CHECK(src->waitWedged(5000));

        const auto t0 = std::chrono::steady_clock::now();
        r.stop();
        const double stopMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        std::printf("stop() took %.0f ms (abandonment path is %d ms)\n", stopMs,
                    PatchRadio::kStopWaitMs);
        CHECK(stopMs >= PatchRadio::kStopWaitMs - 50);   // it really was abandoned
        CHECK(src->stopCalls() >= 1);

        // THE PROPERTY: by the time stop() returns, the abandoned radio's
        // decoders are already destroyed - once, on THIS thread - so a plugin
        // unload straight after cannot pull their code out from under them.
        const int atStop = destroys();
        std::printf("decoder destroys when stop() returned: %d\n", atStop);
        CHECK(atStop == 1);
        const long long processAtStop = g_processCalls.load();

        // Now the driver lets go. The abandoned reader sees run == false,
        // leaves, and drops the last reference to the old Shared.
        src->release();
        CHECK(waitFor([&] { return g_sourceGone.load(std::memory_order_acquire); }, 5000));

        std::lock_guard<std::mutex> lock(g_ledger.m);
        std::printf("decoder destroys after the abandoned reader left: %d\n", g_ledger.destroys);
        CHECK(g_ledger.destroys == 1);                       // exactly once, not again
        bool allOnControl = !g_ledger.destroyThreads.empty();
        for (const std::thread::id t : g_ledger.destroyThreads) {
            if (t != control) { allOnControl = false; }
        }
        CHECK(allOnControl);                                 // never on the reader thread
        CHECK(g_processCalls.load() == processAtStop);       // nothing fed after stop()
    }

    return testSummary("test_patch_radio_abandon");
}
