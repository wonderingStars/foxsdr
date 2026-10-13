// test_engine_publish_alloc.cpp - the engine's per-pass publish takes no heap
// allocation (engine stage 4, review F8).
//
// Engine::fillPublishedState runs once a control pass, and the tree's rule is
// that a pass allocates nothing. The transmit page's error line (API 0.3 state
// 8.1, FoxReceiverState::txError) used to be filled through
// Transmitter::lastError(), which returns a std::string by value: a copy, and a
// heap allocation whenever the text is longer than the small-string buffer
// (every real error sentence is); the audio sink's name was read the same way
// (AudioOut::openedDeviceName()). Held here by counting the calling thread's
// global operator new around the publish:
//
//   A  the publish allocates exactly as often with a 300-character error as
//      with none (the text adds nothing) and not at all; what it publishes is
//      the error, cut to the field on a character boundary, as it always was,
//      and the audio sink's name
//   A2 AudioOut::refreshOpenedDeviceName: the last good name stands when the
//      sink has none to offer
//   B  Transmitter::lastErrorInto: the whole text when it fits, a cut that
//      never splits a UTF-8 character when it does not, a NUL always, and no
//      write at all with no room
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "engine_api_test_env.hpp"
#include "sink/audio_out.hpp"
#include "test_check.hpp"

// --- the counter: the calling thread's allocations, while armed -----------------
namespace {
thread_local bool t_counting = false;
thread_local long t_news = 0;
}  // namespace

void* operator new(std::size_t n) {
    if (t_counting) { ++t_news; }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

// A transmitter that is only a name and an error: every frequency change is
// refused, with this text.
class RefusingSink : public cascade::source::IqSink {
public:
    bool start() override { return false; }
    void stop() override {}
    void finish() override {}
    bool running() const override { return false; }
    double sampleRateHz() const override { return 2.5e6; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 146.52e6; }
    bool setCenterFrequencyHz(double) override { return false; }
    bool frequencyRangeHz(double& lo, double& hi) const override { lo = 70.0e6; hi = 6.0e9; return true; }
    bool sampleRateRangeHz(double& lo, double& hi) const override { lo = 2.083e6; hi = 61.44e6; return true; }
    bool gainRangeDb(double& lo, double& hi) const override { lo = -89.75; hi = 0.0; return true; }
    double gainDb() const override { return 0.0; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t) override { return 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "refusing test radio"; }
    const char* lastError() const override { return error.c_str(); }

    std::string error;
};

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static RefusingSink* installSink(cascade::engine::Engine& e) {
        auto sink = std::make_unique<RefusingSink>();
        RefusingSink* raw = sink.get();
        e.transmitter_.setSink(std::move(sink));
        return raw;
    }
    // The radio refuses a retune with the sink's current text: that text is
    // now the Transmitter's last error.
    static void provokeError(cascade::engine::Engine& e) { (void)e.transmitter_.setFrequencyHz(100.0e6); }
    static std::string sinkName(cascade::engine::Engine& e) { return e.pipeline_.audio().openedDeviceName(); }
    static void refreshSink(cascade::engine::Engine& e, std::string& lastGood) {
        e.pipeline_.audio().refreshOpenedDeviceName(lastGood);
    }
    static void errorInto(cascade::engine::Engine& e, char* out, std::size_t cap) {
        e.transmitter_.lastErrorInto(out, cap);
    }
    static void publish(cascade::engine::Engine& e, cascade::core::PublishedState& ps) {
        const std::string fault;
        const cascade::core::RdsSnapshot rds;
        e.fillPublishedState(ps, fault, rds);
    }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr int kPublishes = 40;

// The heap allocations `kPublishes` publishes make on this thread.
long allocationsOfPublishes(cascade::engine::Engine& e) {
    cascade::core::PublishedState ps;
    Access::publish(e, ps);  // warm-up: scratch the first call sizes
    t_news = 0;
    t_counting = true;
    for (int i = 0; i < kPublishes; ++i) { Access::publish(e, ps); }
    t_counting = false;
    return t_news;
}

}  // namespace

int main() {
    engine_api_test::hermetic("publishalloc");
    cascade::engine::Engine e;
    e.initialise();
    RefusingSink* sink = Access::installSink(e);

    // A: the error text adds no allocation to the publish.
    sink->error.clear();
    Access::provokeError(e);
    const long quiet = allocationsOfPublishes(e);

    const std::string text = std::string("the transmitter refused the new frequency: ") + std::string(260, 'x');
    sink->error = text;
    Access::provokeError(e);
    const long loud = allocationsOfPublishes(e);
    std::printf("A: %d publishes, heap allocations with no error text: %ld, with a %zu-character one: %ld\n",
                kPublishes, quiet, text.size(), loud);
    CHECK(loud == quiet);
    // ...and the publish of a settled engine allocates nothing at all: not the
    // audio sink's name (AudioOut::openedDeviceName() is a std::string by
    // value) nor anything else the pass reads.
    CHECK(quiet == 0);
    CHECK(loud == 0);

    // ...and what it publishes is the text, cut to the field (the buffer less
    // its NUL), as before.
    cascade::core::PublishedState ps;
    Access::publish(e, ps);
    const std::size_t cap = sizeof(ps.rx.txError);
    CHECK(std::strlen(ps.rx.txError) == cap - 1);
    CHECK(std::string(ps.rx.txError) == text.substr(0, cap - 1));
    // The sink name published is the sink's own (the hermetic engine opens the
    // default output; on a machine with none both are empty).
    CHECK(std::string(ps.rx.sinkName) == Access::sinkName(e).substr(0, sizeof(ps.rx.sinkName) - 1));

    // AudioOut::refreshOpenedDeviceName keeps keepLastGoodName's rule: a sink
    // that was never opened has no name to offer, so the last good one stands;
    // an opened one's name replaces it.
    {
        cascade::sink::AudioOut fresh;
        std::string last = "Speakers";
        fresh.refreshOpenedDeviceName(last);
        CHECK(last == "Speakers");
        const std::string opened = Access::sinkName(e);
        std::string old = "previous sink";
        Access::refreshSink(e, old);
        CHECK(old == (opened.empty() ? std::string("previous sink") : opened));
    }

    // B: lastErrorInto on its own.
    const auto setError = [&](const std::string& s) {
        sink->error = s;
        Access::provokeError(e);
    };
    char buf[32];
    const auto fillBuf = [&] { std::memset(buf, 'Z', sizeof(buf)); };
    setError("boom");
    fillBuf();
    Access::errorInto(e, buf, sizeof(buf));
    CHECK(std::string(buf) == "boom");  // fits: the whole text
    fillBuf();
    Access::errorInto(e, buf, 3);
    CHECK(std::string(buf) == "bo");    // does not fit: cut, terminated...
    CHECK(buf[3] == 'Z');               // ...and nothing written past the room given
    fillBuf();
    Access::errorInto(e, buf, 0);
    CHECK(buf[0] == 'Z');               // no room: no write
    Access::errorInto(e, nullptr, 8);   // no buffer: no crash
    setError("ab\xC3\xA9");             // "ab" and a two-byte e-acute
    fillBuf();
    Access::errorInto(e, buf, 4);       // room for three bytes: the acute would be split
    CHECK(std::string(buf) == "ab");    // so it is dropped whole
    fillBuf();
    Access::errorInto(e, buf, 5);
    CHECK(std::string(buf) == "ab\xC3\xA9");
    setError("");
    fillBuf();
    Access::errorInto(e, buf, sizeof(buf));
    CHECK(buf[0] == '\0');              // no error: an empty line

    return testSummary("test_engine_publish_alloc");
}
