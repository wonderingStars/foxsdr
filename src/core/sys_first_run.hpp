// sys_first_run.hpp - the first-run wizard as a model: its steps, the radio choice, the starter presets that fit that radio, and the completion flag.
//
// WHAT EXISTS TODAY: only the RADIO SETUP card (core/radio_setup.hpp) and its don't-show-again tick (AppConfig::radioSetupDontShow). There is no wizard and no starter content.
//
// STEPS: Welcome -> Radio -> Preset -> Privacy -> Done. next() moves on only when the step's choice is made (Radio needs a family - "no radio yet" is a family; Preset may be "none";
// Privacy needs the person to see the switch and press on); back() never loses a choice; skip() ends the wizard from any step with nothing applied except the completion flag.
// finish() returns what to APPLY, as data the host writes through its own paths: the source kind (the config's own word: "rtlsdr", "airspyhf", ...), the starter bookmarks (core::Bookmark,
// the very struct bookmarks.json is made from, so they go in through FreqManager::addMany with no conversion), and the usage-reporting choice (default ON, the owner's product decision
// recorded on AppConfig::telemetryEnabled; the wizard shows it and lets the person turn it off - nothing is sent by the wizard itself).
//
// STARTER PRESETS ARE DATA: a short list of well-known allocations per use (FM broadcast, airband emergency, marine channel 16, PMR446, 2 m, time signals, shortwave, amateur HF),
// each with the modes the receiver names (NFM WFM AM USB LSB CW). A preset is offered for a radio ONLY IF EVERY frequency lies inside that radio's tuning window. Windows come from the
// application's own driver headers where it has them (airspy_protocol.hpp, airspyhf_protocol.hpp, hackrf_protocol.hpp, rx888_protocol.hpp, tuner_msi001.hpp); for the families with no
// range constant in the source (RTL-SDR, SDRplay, Pluto) a CONSERVATIVE window of 88-470 MHz is used and the record says so. The frequencies themselves are the owner's to review: they are
// standard allocations typed from general knowledge and were NOT re-verified against a primary document in this session (listed as UNVERIFIED in the record).
//
// FIRST RUN is detected without touching config.hpp: a marker file `first-run-done` in the settings folder. firstRunNeeded() is true only when there is NO config.json AND no marker, so
// anyone who already has settings never sees the wizard; markFirstRunDone() writes the marker.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_FIRST_RUN_HPP
#define CASCADE_CORE_SYS_FIRST_RUN_HPP

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/bookmark.hpp"

namespace cascade::core::sys {

enum class RadioFamily { NoneYet, RtlSdr, Airspy, AirspyHf, Sdrplay, HackRf, Rx888, MiriSdr, Pluto, IqFile, SoundCard };

struct RadioChoiceInfo {
    RadioFamily family;
    const char* sourceKind;   // AppConfig::sourceKind
    const char* label;        // English label for the choice list
    std::vector<std::pair<double, double>> windowsHz;   // empty = no starter presets (no radio, a file, a sound card)
    bool windowFromApp;       // false = conservative, no range constant in the source
};

const std::vector<RadioChoiceInfo>& radioChoices();
const RadioChoiceInfo& radioChoice(RadioFamily f);

struct StarterPreset {
    std::string id;
    std::string title;                                  // English
    std::vector<cascade::core::Bookmark> bookmarks;
};

// The whole catalogue, and the part of it that fits a radio.
const std::vector<StarterPreset>& allStarterPresets();
std::vector<StarterPreset> startersFor(RadioFamily f);

enum class WizardStep { Welcome, Radio, Preset, Privacy, Done };

struct WizardOutcome {
    bool applied = false;                 // false after a skip: nothing but the completion flag
    std::string sourceKind;               // "" = leave the program's own default
    std::vector<cascade::core::Bookmark> bookmarks;
    bool telemetryEnabled = true;
    std::string presetId;
};

class FirstRunWizard {
public:
    WizardStep step() const { return step_; }
    bool canNext() const;
    bool next();                          // false (no change) when the step's choice is missing
    bool back();                          // false at the first step
    bool chooseRadio(RadioFamily f);
    bool choosePreset(const std::string& id);   // "" = none; false when the id is unknown or does not fit the chosen radio
    void chooseTelemetry(bool on) { telemetry_ = on; telemetrySeen_ = true; }
    void skip() { skipped_ = true; step_ = WizardStep::Done; }
    bool finished() const { return step_ == WizardStep::Done; }
    // Valid once finished(); the outcome to apply.
    WizardOutcome outcome() const;
    // The presets the Preset step lists for the chosen radio.
    std::vector<StarterPreset> offered() const { return radioChosen_ ? startersFor(radio_) : std::vector<StarterPreset>{}; }

private:
    WizardStep step_ = WizardStep::Welcome;
    RadioFamily radio_ = RadioFamily::NoneYet;
    bool radioChosen_ = false;
    std::string presetId_;
    bool telemetry_ = true;
    bool telemetrySeen_ = false;
    bool skipped_ = false;
};

inline constexpr const char* kFirstRunMarker = "first-run-done";
bool firstRunNeeded(const std::filesystem::path& settingsDir);       // no config.json and no marker
bool markFirstRunDone(const std::filesystem::path& settingsDir);     // creates the folder if needed, writes the marker

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_FIRST_RUN_HPP
