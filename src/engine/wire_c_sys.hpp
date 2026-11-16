// wire_c_sys.hpp - the first-run wizard of wiring lane C (row S10): the model core/sys_first_run joined to the engine's own folders.
//
// RETIRED (engine lane E1, owner decision 74, 2026-11-16): the settings EXPORT, IMPORT, BACKUP and RESET that used to live here (staged at the next start, wirec-pending.json) were a second
// copy of the SYSTEM page's "Settings and backup" section (slint/lanek_*), which works the files at the end of the run. They are removed with their ops, facts and boot step.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/sys_first_run.hpp"

namespace cascade::engine::wirec {

class SysOps {
public:
    SysOps(std::filesystem::path settingsDir, std::filesystem::path dataDir) : settings_(std::move(settingsDir)), data_(std::move(dataDir)) {}

    // ---- the first-run wizard ----
    // verbs: "radio:<n>" (index into radioChoices), "preset:<id>" ("" none), "telemetry:0|1", "next", "back", "skip". False with `why` for a choice that does not fit.
    bool wizardVerb(const std::string& verb, std::string& why);
    bool wizardFinished() const { return wizard_.finished(); }
    bool telemetryChosen() const { return telemetryChosen_; }
    cascade::core::sys::WizardOutcome wizardOutcome() const { return wizard_.outcome(); }
    std::string wizardStepName() const;
    bool firstRunNeeded() const { return cascade::core::sys::firstRunNeeded(settings_); }
    bool markDone() { return cascade::core::sys::markFirstRunDone(settings_); }
    // The radios offered ("label" lines) and the presets offered for the chosen radio ("id" TAB "title" lines).
    std::string radioLines() const;
    std::string presetLines() const;

private:
    std::filesystem::path settings_, data_;
    cascade::core::sys::FirstRunWizard wizard_;
    bool telemetryChosen_ = false;
};

}  // namespace cascade::engine::wirec
