// engine_api_serial.hpp - AN ENGINE WHOSE CONSTRUCTION AND DESTRUCTION NEVER OVERLAP ANOTHER'S (the pump move's P6).
//
// WHY. Every Engine builds a Pipeline, and the Pipeline opens the default sound output ON THE CONSTRUCTING THREAD (and closes it in the
// destructor): Pa_Initialize / Pa_OpenStream / Pa_CloseStream / Pa_Terminate. PortAudio keeps its list of open streams and its
// initialisation count in plain globals with no lock of its own; the product serialises its calls on them (sink/pa_init.hpp) EXCEPT that an
// open or a close that finds the list's lock busy goes ahead without it (NoWait - the window's thread never waits for an audio driver).
// A test that builds engines on several threads at once - test_engine_api_merge's 24 workers, test_engine_api_transmit's parallel probes -
// is the only caller that ever meets that busy lock, and ThreadSanitizer reports the race in PortAudio (AddOpenStream on firstOpenStream_,
// the count read by every Pa_ call against Pa_Initialize's write). It is a TEST pattern, not a product one - one process, one engine - so
// the cure is in the tests: engines are built and destroyed one at a time, and what the tests are about (the engines' own passes, their
// control threads, the sessions on them) still runs concurrently, from the first line after the constructor returns until the destructor
// is entered. No coverage is lost: nothing here asserts on the construction itself racing another construction.
//
// USE. `engine_api_test::SerialEngine e(api(), options)` where a test wrote `testing::Engine e(api(), options)`; it IS a testing::Engine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <mutex>

#include "engine_client.hpp"

namespace engine_api_test {

// Allocated once and never destroyed: a worker thread can still be tearing an engine down while the process runs its static destructors.
inline std::mutex& engineLifecycleMutex() {
    static std::mutex* const m = new std::mutex;
    return *m;
}

// A base that takes the lock BEFORE testing::Engine is constructed and gives it up in the derived constructor's body, so the whole of
// the base's construction is inside it; the derived destructor takes it again before the base is destroyed, and this base (destroyed
// last) releases it.
struct EngineLifecycleHold {
    std::unique_lock<std::mutex> lock{engineLifecycleMutex()};
};

class SerialEngine : private EngineLifecycleHold, public testing::Engine {
public:
    explicit SerialEngine(const FoxEngineApi* api, const char* options = nullptr)
        : EngineLifecycleHold(), testing::Engine(api, options) {
        lock.unlock();
    }
    ~SerialEngine() { lock.lock(); }
    SerialEngine(const SerialEngine&) = delete;
    SerialEngine& operator=(const SerialEngine&) = delete;
};

}  // namespace engine_api_test
