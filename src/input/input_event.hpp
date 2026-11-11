// input_event.hpp - the device-independent event a tuning knob, jog wheel, button or MIDI control produces, and the bounded queue that carries it from a
// device thread to the consumer (docs/feature-records/knob-input.md).
//
// THE EVENT. Four kinds: a RELATIVE turn (signed detents, with a timestamp so the consumer can apply acceleration), an ABSOLUTE position (0..1, with the
// device's own integer in `raw`), a button going down or up, and a device going away. A device is named by a 32-bit id that is a hash of a STABLE text
// ("midi:<port name>", "hid:0b33:0020"); a control by a 16-bit id the backend defines (hid_parse.hpp, midi_parse.hpp).
//
// THE QUEUE. Many device threads push, one consumer drains. A mutex guards a fixed ring; the critical section is a few stores, never a call out, never an
// allocation, so a push cannot wait on anything but another push or the drain's copy (which holds the lock for at most `max` element copies). When the ring is full
// the OLDEST event is dropped and counted; the pusher never blocks on a slow consumer.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::input {

enum class EventKind : std::uint8_t {
    Relative = 0,     // raw = signed detents (never 0), value unused
    Absolute = 1,     // value = 0..1, raw = the device's integer position
    ButtonDown = 2,   // value = 1 (or the velocity 0..1 of a MIDI note)
    ButtonUp = 3,
    Disconnected = 4  // the device was lost; control = 0
};

struct InputEvent {
    std::uint32_t device = 0;
    std::uint16_t control = 0;
    EventKind kind = EventKind::Relative;
    std::int32_t raw = 0;
    float value = 0.0f;
    std::int64_t timeUs = 0;  // steady-clock microseconds when the device thread saw it
};

// FNV-1a 32 over the text. Stable across runs and machines (it is stored nowhere, but bindings are matched through it).
std::uint32_t deviceIdFor(const std::string& stableText);

// Steady-clock microseconds (one place, so tests and backends agree).
std::int64_t nowMicros();

class EventQueue {
public:
    explicit EventQueue(std::size_t capacity = 1024);

    // Never blocks on the consumer. If full, the oldest event is dropped and dropped() grows by one. Returns false only when it dropped.
    bool push(const InputEvent& e);

    // Moves up to `max` events (oldest first) onto the end of `out`; returns how many.
    std::size_t drain(std::vector<InputEvent>& out, std::size_t max = static_cast<std::size_t>(-1));

    std::size_t size() const;
    std::size_t capacity() const { return cap_; }
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    const std::size_t cap_;
    mutable std::mutex m_;
    std::vector<InputEvent> ring_;
    std::size_t head_ = 0;  // index of the oldest
    std::size_t count_ = 0;
    std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace cascade::input
