// mapping.hpp - (device, control) -> action. PURE (no Windows header, no thread, no clock: time comes in with each event and each tick()).
//
// An InputEvent from the queue goes in; zero or more ActionOuts come out, for the consumer (the Slint host, later) to turn into the same calls a key press makes.
// This layer never touches the engine, the config code or the screen.
//
// ACTIONS (the names are the file format; see kActionNames):
//   relative or ring: tune (n detents of the current step), tune.fine (n detents of step / divisor, params.divisor default 10), tune.shuttle (the shuttle ring:
//   a repeat rate that follows the deflection), volume, squelch, zoom.   buttons: step.up, step.down, mode.next, mode.prev, radio.select (params.n 1..8),
//   command (params.name = a key-table action id such as "mute" or "record"; the consumer runs that key's action).
// Any action name or command name that begins with "transmit" or "ptt" is REFUSED at load time: a knob must never key a transmitter.
//
// ACCELERATION (relative controls): speed = |detents| / seconds since the previous event of that control; factor = 1 + (maxFactor - 1) * clamp((speed -
// slowPerSec) / (fastPerSec - slowPerSec), 0, 1). maxFactor 1 (the default) is off.
// SHUTTLE RING (tune.shuttle): the deflection (-7..+7) inside `deadZone` does nothing; beyond it the rate rises linearly from minRateHz at the first step past
// the dead zone to maxRateHz at full deflection; tick() turns the rate into whole detents, the first one at the instant the ring leaves the dead zone.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "input_event.hpp"
#include "midi_parse.hpp"

namespace cascade::input {

struct Accel {
    double slowPerSec = 8.0;
    double fastPerSec = 40.0;
    double maxFactor = 1.0;
};

struct Binding {
    std::string device;  // stable id text: "midi:<port>" / "hid:0b33:0020"
    std::uint16_t control = 0;
    std::string action;
    nlohmann::json params = nlohmann::json::object();
    MidiEncoding encoding = MidiEncoding::Absolute7;  // MIDI CC controls: how the value is read
    bool invert = false;                              // negate relative amounts, mirror absolute ones
    Accel accel;
    int deadZone = 0;         // ring: 0..6
    double minRateHz = 2.0;   // ring
    double maxRateHz = 30.0;  // ring
    nlohmann::json extra = nlohmann::json::object();  // fields this version does not know, kept so a save does not lose them
};

struct Modifier {  // while this button is down, relative amounts are multiplied by `factor`
    std::string device;
    std::uint16_t control = 0;
    double factor = 10.0;
};

struct Settings {
    int version = 1;
    std::vector<Binding> bindings;
    std::optional<Modifier> modifier;
    nlohmann::json extra = nlohmann::json::object();
};

struct LoadReport {
    int bindingsRead = 0;
    int skippedMalformed = 0;  // not an object, or device / action / control missing or the wrong type
    int refusedForbidden = 0;  // a transmit / ptt action
    int unknownEncodings = 0;  // encoding name not recognised (read as absolute7)
    int clamped = 0;           // a number outside its range, pulled in
    bool unparseable = false;  // the text was not JSON
};

extern const char* const kActionNames[];
extern const std::size_t kActionCount;
bool isKnownAction(const std::string& name);
bool isForbiddenName(const std::string& name);  // begins with "transmit" or "ptt", any case

nlohmann::json settingsToJson(const Settings& s);
Settings settingsFromJson(const nlohmann::json& j, LoadReport* report = nullptr);
Settings settingsFromText(const std::string& text, LoadReport* report = nullptr);
std::string settingsToText(const Settings& s);

// "ShuttleXpress: jog = tune, ring = scan speed, buttons = step size / mode / mute / record" (design record, feature 10(b)).
Settings shuttleXpressPreset(const std::string& deviceStableId);
Settings powerMatePreset(const std::string& deviceStableId);  // turn = tune, push = mute

struct ActionOut {
    std::string action;
    nlohmann::json params;
    double amount = 0.0;   // signed detents (after acceleration and the modifier), or 0..1 when `absolute`; buttons: params-driven, default +1
    bool absolute = false;
    std::uint32_t device = 0;
    std::uint16_t control = 0;
    std::int64_t timeUs = 0;
};

class Mapper {
public:
    Mapper() = default;
    explicit Mapper(Settings s) { setSettings(std::move(s)); }
    void setSettings(Settings s);
    const Settings& settings() const { return s_; }

    // The MIDI control configurations to hand to a MidiDecoder for this device (encoding only: the mapper applies `invert`).
    std::vector<std::pair<std::uint16_t, MidiControlConfig>> midiConfigsFor(const std::string& deviceStableId) const;

    std::vector<ActionOut> process(const InputEvent& e);
    // Shuttle-ring repeats. Call at the consumer's tick (any rate; the repeat is computed from elapsed time, one burst capped at a quarter second).
    std::vector<ActionOut> tick(std::int64_t nowUs);

private:
    struct Ring {
        int defl = 0;
        double acc = 0.0;
        std::int64_t lastUs = 0;
    };
    const Binding* find(std::uint32_t dev, std::uint16_t control) const;

    Settings s_;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::size_t> index_;
    std::uint32_t modDevice_ = 0;
    bool modDown_ = false;
    std::map<std::pair<std::uint32_t, std::uint16_t>, std::int64_t> lastUs_;  // previous event time per relative control
    std::map<std::pair<std::uint32_t, std::uint16_t>, Ring> rings_;
};

}  // namespace cascade::input
