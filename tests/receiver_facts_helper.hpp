// receiver_facts_helper.hpp - how a plugin-API test publishes a receiver state.
//
// Before engine stage 2 PluginApiCore kept a snapshot of its own and the tests
// published core::ReceiverFacts into it. The plugin API now reads the ONE
// receiver snapshot the application publishes (core/receiver_snapshot.hpp), so
// the tests publish there - still written against the same plain facts, which
// this maps onto a PublishedState the way AppWindow::fillPublishedState maps
// the window's members. Header-only, test-only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_TESTS_RECEIVER_FACTS_HELPER_HPP
#define CASCADE_TESTS_RECEIVER_FACTS_HELPER_HPP

#include <cstdint>
#include <cstring>

#include "core/plugin_api.hpp"
#include "core/receiver_snapshot.hpp"

namespace testfacts {

struct ReceiverFacts {
    bool running = false;
    bool deviceOpen = false;
    bool muted = false;
    bool deviceAgc = false;
    bool agcSupported = false;
    bool stereo = false;
    double centreHz = 0.0;
    double vfoOffsetHz = 0.0;
    double sampleRateHz = 0.0;
    double bandwidthHz = 0.0;
    double squelchDb = 0.0;
    double volume = 0.0;
    double signalDb = -200.0;
    std::uint32_t demodMode = CASCADE_DEMOD_NFM;
    char deviceName[CASCADE_DEVICE_NAME_CHARS] = {};
    std::uint32_t gainCount = 0;
    cascade::core::PublishedGain gains[cascade::core::kMaxPublishedGains];
    std::uint32_t rateCount = 0;
    double rates[cascade::core::kMaxPublishedRates] = {};
    double outputRateHz = 0.0;
    std::uint64_t outputFrames = 0;
};

inline cascade::core::PublishedState toState(const ReceiverFacts& f) {
    cascade::core::PublishedState s{};
    std::uint32_t fl = 0;
    if (f.running) { fl |= FOXAPI_RX_RUNNING; }
    if (f.deviceOpen) { fl |= FOXAPI_RX_DEVICE_OPEN; }
    if (f.muted) { fl |= FOXAPI_RX_MUTED; }
    if (f.deviceAgc) { fl |= FOXAPI_RX_DEVICE_AGC; }
    if (f.agcSupported) { fl |= FOXAPI_RX_AGC_SUPPORTED; }
    if (f.stereo) { fl |= FOXAPI_RX_STEREO_ACTIVE; }
    s.rx.flags = fl;
    s.rx.centreHz = f.centreHz;
    s.rx.vfoOffsetHz = f.vfoOffsetHz;
    s.rx.tunedHz = f.centreHz + f.vfoOffsetHz;
    s.rx.sampleRateHz = f.sampleRateHz;
    s.rx.bandwidthHz = f.bandwidthHz;
    s.rx.squelchDb = f.squelchDb;
    s.rx.volume = f.volume;
    s.rx.signalDb = f.signalDb;
    s.rx.demodMode = f.demodMode;
    std::memcpy(s.rx.deviceName, f.deviceName, sizeof(s.rx.deviceName));
    s.rx.gainCount = f.gainCount;
    s.app.abiGainCount = f.gainCount;
    for (std::size_t i = 0; i < cascade::core::kMaxPublishedGains; ++i) { s.app.gains[i] = f.gains[i]; }
    s.app.rateCount = f.rateCount;
    for (std::size_t i = 0; i < cascade::core::kMaxPublishedRates; ++i) { s.app.rates[i] = f.rates[i]; }
    s.app.outputRateHz = f.outputRateHz;
    s.app.outputFrames = f.outputFrames;
    return s;
}

// The one writer of `api`'s snapshot in a test, as AppWindow is in the app.
inline void publish(cascade::core::PluginApiCore& api, const ReceiverFacts& f) {
    api.snapshot().publish(toState(f), nullptr);
}

}  // namespace testfacts

#endif  // CASCADE_TESTS_RECEIVER_FACTS_HELPER_HPP
