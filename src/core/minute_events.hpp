// minute_events.hpp - the DELIBERATE EVENTS of a digest minute, shared by the DSP minute and the audio minute (2026-11-23).
//
// WHY. The ringdrop and audiolead words are the two largest groups on the owner's crash page, and a minute that held something the
// APPLICATION did on purpose - a source switch, a live sample-rate change (the DSP thread is joined and respawned while the source
// keeps writing), a plugin reload or wake (a decoder's create() can hold the runner's lock for hundreds of milliseconds), a sound
// output re-opened - loses samples or starves a callback by construction, on a machine that is keeping up perfectly. The audio digest
// already skipped a minute that reloaded plugins and discounted stops and output opens; the DSP digest had no rule at all. One event
// mask, set by the code that performs those events (Pipeline::setSource / setInputRateHz count their own; the engine counts plugin
// reloads and wakes and reads the sink's own open count), read by BOTH digests through the helpers below, so the two cannot drift.
//
// THE OVERRIDE. A machine that is really behind is not hidden by a busy spell: three CONSECUTIVE loss minutes are judged whatever
// events they held.
//
// A discounted minute is still LOGGED, with the event named; it is only not filed as a fault. No threshold lives here.
//
// Pure: no clock, no log, no engine - tests/test_minute_events.cpp drives every case.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace cascade::core {

enum MinuteEvent : unsigned {
    kMinuteEventRate = 1u,      // a live sample-rate change (Pipeline::setInputRateHz)
    kMinuteEventSwitch = 2u,    // a source switch (Pipeline::setSource)
    kMinuteEventPlugins = 4u,   // a plugin rescan, wake or stop
    kMinuteEventAudio = 8u,     // a sound output opened (again)
};

// Three consecutive loss minutes are judged whatever events they held.
constexpr int kMinuteEventOverrideRun = 3;

// The monotonic count of each kind of event, as a digest sees it. A digest keeps a copy as its mark.
struct MinuteEventCounts {
    std::uint64_t rate = 0;
    std::uint64_t source = 0;
    std::uint64_t plugins = 0;
    std::uint64_t audio = 0;
};

// The kinds whose count moved since `mark` (a count that went BACKWARDS - a sink replaced under the engine - reads as no event), and
// `mark` advanced to `now`: taken whether or not anything was lost, so a minute's mask is its own.
inline unsigned minuteEventsSince(const MinuteEventCounts& now, MinuteEventCounts& mark) {
    unsigned mask = 0;
    if (now.rate > mark.rate) { mask |= kMinuteEventRate; }
    if (now.source > mark.source) { mask |= kMinuteEventSwitch; }
    if (now.plugins > mark.plugins) { mask |= kMinuteEventPlugins; }
    if (now.audio > mark.audio) { mask |= kMinuteEventAudio; }
    mark = now;
    return mask;
}

// The judgment of one minute. `lossMinute` is the digest's own test (a hearable share lost; starved callbacks at the raise threshold),
// `mask` the minute's events (only the kinds this digest discounts for), `lossRun` the digest's count of consecutive loss minutes,
// updated here. True = judge it (file the word, raise the lead); false = log it with its events and move on.
inline bool minuteJudged(bool lossMinute, unsigned mask, int& lossRun) {
    lossRun = lossMinute ? lossRun + 1 : 0;
    if (!lossMinute) { return false; }
    return mask == 0u || lossRun >= kMinuteEventOverrideRun;
}

// "rate|switch|plugins|audio" in that order, or "none". Fixed words only: nothing a user typed or a device is called.
inline std::size_t minuteEventsText(unsigned mask, char* out, std::size_t cap) {
    if (out == nullptr || cap == 0) { return 0; }
    std::size_t at = 0;
    const auto put = [&](const char* word) {
        for (const char* c = word; *c != '\0' && at + 1 < cap; ++c) { out[at++] = *c; }
    };
    bool any = false;
    const struct { unsigned bit; const char* word; } kinds[] = {
        {kMinuteEventRate, "rate"}, {kMinuteEventSwitch, "switch"}, {kMinuteEventPlugins, "plugins"}, {kMinuteEventAudio, "audio"}};
    for (const auto& k : kinds) {
        if ((mask & k.bit) == 0u) { continue; }
        if (any) { put("|"); }
        put(k.word);
        any = true;
    }
    if (!any) { put("none"); }
    out[at] = '\0';
    return at;
}

}  // namespace cascade::core
