// minute_events.hpp - the NAMES of the application's own events in a digest minute, for the DSP minute's log line (2026-11-23).
//
// WHY. The ringdrop and audiolead words are the two largest groups on the owner's crash page. A minute that held something the
// APPLICATION did - a source switch, a live sample-rate change (the DSP thread is joined and respawned while the source keeps writing), a
// plugin reload or wake (a decoder's create() can hold the runner's lock for hundreds of milliseconds), a sound output re-opened - is
// useful context for whoever reads the report: the DSP minute's line says `events=rate|switch|plugins|audio` beside the numbers.
//
// THE EVENTS ARE CONTEXT, NEVER AN EXCUSE (review of 70d16f00, 2026-11-23). An earlier version of this header judged minutes through the
// events (a loss minute that held one was "logged, not filed", unless it was the third in a row). That hid real loss: the owner's rule is
// that every handled fault is reported, and a drop that shares a minute with a rate change is still a drop - the deliberate pause itself
// is already removed from the count (Pipeline::ringDroppedAbsent, bounded to 2 s), and nothing else is deliberate. No event, and no
// count of events, decides whether a minute is filed; there is no mask in any decision any more.
//
// Pure: no clock, no log, no engine - tests/test_ringdrop_measure.cpp section 1 drives every case.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace cascade::core {

enum MinuteEvent : unsigned {
    kMinuteEventRate = 1u,      // a live sample-rate change (Pipeline::setInputRateHz)
    kMinuteEventSwitch = 2u,    // a source switch (Pipeline::setSource)
    kMinuteEventPlugins = 4u,   // a plugin rescan, wake or stop
    kMinuteEventAudio = 8u,     // a sound output opened (again)
};

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
