// test_audio_route.cpp - where one radio's sound goes (multi-radio stage 4): AudioOut's ROUTE (both, left, right, none) and the SOLO silence, headless
// through pullBlock (the exact code the device callback runs), against expectations computed in the test.
//
//   A  A ROUTE IS A CHANNEL, NOT A FOLD: left plays on the left channel and the right channel is silent (and the other way round); none plays nothing;
//      both is the stream untouched;
//   B  A SWITCH IS A RAMP, NEVER A CLICK: from both to none the gain goes from 1 to 0 in kRouteRampFrames steps of equal size, so no two neighbouring
//      samples of a constant signal differ by more than one step - and a hard cut (a gain that jumped) would differ by the whole signal;
//   C  THE SOLO SILENCE IS ITS OWN SWITCH: silenced holds the stream at zero whatever the route, the route is kept, and un-silencing brings back the
//      route's gains - a user's volume is untouched throughout;
//   D  A MONO STREAM has no left and right: left and right play as both; none and silenced play nothing.
//
// The stream is opened for real and closed again before pullBlock is measured (the pattern of test_audio_out.cpp), so no device callback races the ring.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sink/audio_out.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "test_check.hpp"

using cascade::sink::AudioOut;

namespace {

void primeAndDrain(AudioOut& ao, int channels) {
    const std::size_t chan = channels == 2 ? 2 : 1;
    std::vector<float> pad(AudioOut::kPrimeFrames * chan, 0.0f);
    CHECK(ao.write(pad.data(), pad.size()) == pad.size());
    std::vector<float> junk(pad.size());
    CHECK(AudioOut::pullBlock(&ao, junk.data(), AudioOut::kPrimeFrames) == pad.size());
}

// `frames` stereo frames of a constant 0.5 on both channels, pulled through the callback; the interleaved result.
std::vector<float> pull(AudioOut& ao, std::size_t frames, int channels) {
    const std::size_t chan = channels == 2 ? 2 : 1;
    std::vector<float> in(frames * chan, 0.5f);
    CHECK(ao.write(in.data(), in.size()) == in.size());
    std::vector<float> out(frames * chan, 99.0f);
    CHECK(AudioOut::pullBlock(&ao, out.data(), frames) == in.size());
    return out;
}

bool openedAs(AudioOut& ao, int channels) {
    const bool ok = ao.open(-1, 48000.0, channels);
    CHECK(ok);
    if (!ok) { return false; }
    ao.close();
    primeAndDrain(ao, channels);
    return true;
}

void testRoutesAreChannels() {
    AudioOut ao;
    if (!openedAs(ao, 2)) { return; }
    // The ramp runs in 10 ms (480 frames); look past it.
    const std::size_t kFrames = AudioOut::kRouteRampFrames * 2;
    CHECK(ao.route() == AudioOut::kRouteBoth);
    {
        const std::vector<float> o = pull(ao, kFrames, 2);
        for (std::size_t i = 0; i < o.size(); ++i) { CHECK(o[i] == 0.5f); }
    }
    ao.setRoute(AudioOut::kRouteLeft);
    CHECK(ao.route() == AudioOut::kRouteLeft);
    {
        const std::vector<float> o = pull(ao, kFrames, 2);
        // Past the ramp: left is the stream, right is silent - the sound is NOT folded into the left.
        const std::size_t last = (kFrames - 1) * 2;
        CHECK(o[last] == 0.5f);
        CHECK(o[last + 1] == 0.0f);
    }
    ao.setRoute(AudioOut::kRouteRight);
    {
        const std::vector<float> o = pull(ao, kFrames, 2);
        const std::size_t last = (kFrames - 1) * 2;
        CHECK(o[last] == 0.0f);
        CHECK(o[last + 1] == 0.5f);
    }
    ao.setRoute(AudioOut::kRouteNone);
    {
        const std::vector<float> o = pull(ao, kFrames, 2);
        const std::size_t last = (kFrames - 1) * 2;
        CHECK(o[last] == 0.0f);
        CHECK(o[last + 1] == 0.0f);
    }
    ao.setRoute(AudioOut::kRouteBoth);
    {
        const std::vector<float> o = pull(ao, kFrames, 2);
        const std::size_t last = (kFrames - 1) * 2;
        CHECK(o[last] == 0.5f);
        CHECK(o[last + 1] == 0.5f);
    }
    // A route that is not one is both.
    ao.setRoute(77);
    CHECK(ao.route() == AudioOut::kRouteBoth);
}

void testASwitchIsARamp() {
    AudioOut ao;
    if (!openedAs(ao, 2)) { return; }
    const std::size_t kFrames = AudioOut::kRouteRampFrames * 2;
    (void)pull(ao, 64, 2);
    ao.setRoute(AudioOut::kRouteNone);
    const std::vector<float> o = pull(ao, kFrames, 2);
    // Left channel, frame by frame: monotone down, one step at a time, from (nearly) 0.5 to 0.
    const float step = 0.5f / static_cast<float>(AudioOut::kRouteRampFrames);
    float worst = 0.0f;
    for (std::size_t f = 1; f < kFrames; ++f) {
        worst = std::max(worst, std::fabs(o[2 * f] - o[2 * (f - 1)]));
        CHECK(o[2 * f] <= o[2 * (f - 1)] + 1e-6f);
    }
    CHECK(worst <= step * 1.01f);
    CHECK(o[0] > 0.49f);                       // it starts where it was...
    CHECK(o[2 * (kFrames - 1)] == 0.0f);       // ...and ends at the target
    // It took the whole ramp, not a jump: the midpoint is half way.
    CHECK(std::fabs(o[2 * (AudioOut::kRouteRampFrames / 2)] - 0.25f) < 0.02f);
}

void testTheSoloSilenceIsItsOwnSwitch() {
    AudioOut ao;
    if (!openedAs(ao, 2)) { return; }
    const std::size_t kFrames = AudioOut::kRouteRampFrames * 2;
    ao.setVolume(0.8f);
    ao.setRoute(AudioOut::kRouteLeft);
    (void)pull(ao, kFrames, 2);
    ao.setSilenced(true);
    CHECK(ao.silenced());
    {
        const std::vector<float> o = pull(ao, kFrames, 2);
        const std::size_t last = (kFrames - 1) * 2;
        CHECK(o[last] == 0.0f);
        CHECK(o[last + 1] == 0.0f);
    }
    // The route and the volume were never touched.
    CHECK(ao.route() == AudioOut::kRouteLeft);
    ao.setSilenced(false);
    {
        const std::vector<float> o = pull(ao, kFrames, 2);
        const std::size_t last = (kFrames - 1) * 2;
        CHECK(o[last] == 0.5f * 0.8f);    // the volume (0.8) and the route (left) as they were
        CHECK(o[last + 1] == 0.0f);
    }
}

void testAMonoStreamHasNoLeftAndRight() {
    AudioOut ao;
    if (!openedAs(ao, 1)) { return; }
    const std::size_t kFrames = AudioOut::kRouteRampFrames * 2;
    ao.setRoute(AudioOut::kRouteLeft);
    {
        const std::vector<float> o = pull(ao, kFrames, 1);
        CHECK(o[kFrames - 1] == 0.5f);
    }
    ao.setRoute(AudioOut::kRouteRight);
    {
        const std::vector<float> o = pull(ao, kFrames, 1);
        CHECK(o[kFrames - 1] == 0.5f);
    }
    ao.setRoute(AudioOut::kRouteNone);
    {
        const std::vector<float> o = pull(ao, kFrames, 1);
        CHECK(o[kFrames - 1] == 0.0f);
    }
    ao.setRoute(AudioOut::kRouteBoth);
    ao.setSilenced(true);
    {
        const std::vector<float> o = pull(ao, kFrames * 2, 1);
        CHECK(o[kFrames * 2 - 1] == 0.0f);
    }
}

}  // namespace

int main() {
    std::printf("test_audio_route\n");
    testRoutesAreChannels();
    testASwitchIsARamp();
    testTheSoloSilenceIsItsOwnSwitch();
    testAMonoStreamHasNoLeftAndRight();
    return testSummary("test_audio_route");
}
