// knob_host.hpp - LANE X, row G5-16: the Slint host's consumer of the knob layer (src/input, branch feat/knob-input): which controllers are used, their events turned into the window's own actions.
//
// The layer below (cascade::input) parses a ShuttleXpress's or a PowerMate's reports into events and maps (device, control) to an action name. This unit is the part the layer's record called
// "the consumer": it owns the mapper, drains the event queue once a tick and runs each action through the SAME calls a key press makes (callbacks the window hands in), so a knob can do
// nothing a key cannot. A knob never keys a transmitter: the layer refuses transmit and ptt names at load, and applyAction refuses them again.
//
// NOTHING IS OPENED UNTIL THE PERSON ASKS: `scan` only lists (a listing opens no device for reading); `setUse(id, true)` opens one, and only while the master switch is on. The listing, the opening and
// the clock are injected (Env), so the tests run with made-up devices and the window with the real ones (knob_host_win.cpp). MIDI ports are not handled here (OPEN ROW: a MIDI controller needs
// a learn step to know which of its controls is the knob).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "input/input_event.hpp"
#include "input/mapping.hpp"

namespace foxsdr::slintknobs {

// What a knob may do: the window's own actions. Every one is a call the window already has for a key.
struct Callbacks {
    std::function<void(int detents)> tune;              // detents of the current tuning step (signed)
    std::function<void(int detents, int divisor)> tuneFine;   // detents of step / divisor
    std::function<void(double amount)> volume;          // signed detents (0.02 of the range each)
    std::function<void(double amount)> squelch;
    std::function<void(double factor)> zoom;            // > 1 zooms in
    std::function<void(int direction)> stepSize;        // +1 / -1: the next larger / smaller tuning step
    std::function<void(int direction)> mode;            // +1 / -1: the next / previous mode
    std::function<void(int radio)> radioSelect;         // 1-based
    std::function<void(const std::string& keyAction)> command;   // a key-table action id ("mute", "record"); never transmit or ptt
};

// Runs one action. Returns true when it was understood and done, false for an unknown name or a refused one. `kZoomPerDetent` is the factor of one detent.
bool applyAction(const cascade::input::ActionOut& a, const Callbacks& cb);
inline constexpr double kZoomPerDetent = 1.25;

// A controller found by a listing.
struct FoundDevice {
    std::string id;       // the stable id: "hid:0b33:0020"
    std::string name;     // "ShuttleXpress"
    int preset = 0;       // 0 ShuttleXpress, 1 PowerMate (the preset that fits it)
    std::size_t token = 0;   // the lister's own handle for opening it
};

struct Status {
    std::string id, name;
    bool use = false;        // the person has this one switched on
    bool open = false;
    std::string error;       // why it is not open ("" none)
    std::uint64_t reports = 0;
};

class KnobHost {
public:
    struct Env {
        std::function<std::vector<FoundDevice>()> list;   // enumeration only: opens nothing for reading
        // Opens one for reading: events go to the queue. Returns the object that keeps it open (destroyed = closed), or null with `error` set.
        std::function<std::shared_ptr<void>(const FoundDevice&, cascade::input::EventQueue&, std::string& error)> open;
        std::function<std::uint64_t(const std::shared_ptr<void>&)> reports;   // optional: reports read so far (a display figure)
        std::function<std::int64_t()> nowUs;                // optional: the clock; the layer's own steady clock by default
        Callbacks act;
    };
    explicit KnobHost(Env env);
    ~KnobHost();
    KnobHost(const KnobHost&) = delete;
    KnobHost& operator=(const KnobHost&) = delete;

    void setEnabled(bool on);          // off closes every device; on opens the ones marked to use
    bool enabled() const;
    void scan();                       // looks again (never opens anything for reading)
    bool setUse(const std::string& id, bool use);   // false when no such device is listed
    void setUsedIds(const std::vector<std::string>& ids);   // the saved choice: marks them (they open at the next scan while the switch is on)
    std::vector<std::string> usedIds() const;
    std::vector<Status> status() const;
    // Once a tick: the events since the last one become actions. Returns how many actions ran.
    int tick();
    std::uint64_t eventsSeen() const;
    cascade::input::EventQueue& queue();   // the devices' queue (a bounded run's stand-in device pushes into it)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The real listing and opening, over cascade::input's HID enumeration and reader (Windows).
KnobHost::Env realEnv();

}  // namespace foxsdr::slintknobs
