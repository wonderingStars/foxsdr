// wire_c_sys.hpp - the SYSTEM operations of wiring lane C (rows S7 export / import / backup, S8 reset, S10 first-run wizard): the models core/sys_bundle, sys_reset and
// sys_first_run joined to the engine's own folders.
//
// WHY IMPORT AND RESET ARE PENDING. The running engine holds its settings in memory and writes config.json when it closes; an import or a reset that replaced the file NOW
// would be overwritten by that write, so "Import settings" would appear to work and then not. Both are therefore STAGED (wirec-pending.json and, for an import, the
// validated files in wirec-pending/) and carried out at the NEXT START, before anything reads the settings (applyPendingAtBoot, bootProcess step 2c). Nothing is deleted
// by either: an import keeps the file it replaces as <name>.before-import-<stamp>, a reset moves the files into reset-<stamp>. The confirm token of the model still
// stands between a plan and its staging, and a staged action can be cancelled until the next start.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/sys_bundle.hpp"
#include "core/sys_first_run.hpp"
#include "core/sys_reset.hpp"

namespace cascade::engine::wirec {

// "YYYYMMDD-HHMMSS" in UTC from `unixSeconds` (0 = now).
std::string utcStamp(std::int64_t unixSeconds = 0);

// What a start does with a staged import or reset. `settingsDir` is the per-user folder (named foxsdr), `dataDir` the data folder. Returns a sentence for the log ("" = nothing was
// pending); `ok` false when a staged action could not be carried out (it is then left where it is and reported).
std::string applyPendingAtBoot(const std::filesystem::path& settingsDir, const std::filesystem::path& dataDir, bool& ok);

class SysOps {
public:
    SysOps(std::filesystem::path settingsDir, std::filesystem::path dataDir) : settings_(std::move(settingsDir)), data_(std::move(dataDir)) {}

    // ---- export, import, backup ----
    // Writes a bundle of config.json, bookmarks.json and markers.json (whichever exist; secrets and the install identity are NEVER exported) to `path`. False with `why`.
    bool exportTo(const std::string& path, std::string& why, std::size_t* stripped = nullptr);
    // Reads and validates a bundle (all or nothing); keeps it for importCommit. `planText` is the plan as lines "<name>\t<New|Replace|Identical>" and `token` the confirm token.
    bool importPlan(const std::string& path, std::string& planText, std::string& token, std::string& notes, std::string& why);
    // Stages the planned import for the next start. False with `why` for a wrong or stale token.
    bool importCommit(const std::string& token, std::string& why);
    // A backup bundle in <settings>/backups, the newest `keep` kept. Returns its file name.
    bool backupNow(std::size_t keep, std::string& name, std::string& why);

    // ---- reset ----
    bool resetPlan(bool includePlugins, bool includeLogs, std::string& planText, std::string& token, std::string& why);
    bool resetStage(const std::string& token, std::string& why);

    // ---- the staged action ----
    std::string pendingKind() const;   // "", "import" or "reset"
    bool cancelPending(std::string& why);

    // ---- the first-run wizard ----
    // verbs: "radio:<n>" (index into radioChoices), "preset:<id>" ("" none), "telemetry:0|1", "next", "back", "skip". False with `why` for a choice that does not fit.
    bool wizardVerb(const std::string& verb, std::string& why);
    bool wizardFinished() const { return wizard_.finished(); }
    bool telemetryChosen() const { return telemetryChosen_; }
    cascade::core::sys::WizardOutcome wizardOutcome() const { return wizard_.outcome(); }
    std::string wizardStepName() const;
    bool firstRunNeeded() const { return cascade::core::sys::firstRunNeeded(settings_); }
    bool markDone() { return cascade::core::sys::markFirstRunDone(settings_); }
    // The radios offered ("label" lines) and the presets offered for the chosen radio ("id\ttitle" lines).
    std::string radioLines() const;
    std::string presetLines() const;

private:
    std::vector<cascade::core::sys::BundleFile> readOwnFiles() const;
    std::filesystem::path pendingDir() const { return settings_ / "wirec-pending"; }
    std::filesystem::path pendingFile() const { return settings_ / "wirec-pending.json"; }

    std::filesystem::path settings_, data_;
    cascade::core::sys::ImportResult imported_;
    std::string importToken_;
    bool haveImport_ = false;
    std::string resetToken_;
    cascade::core::sys::ResetOptions resetOptions_;
    bool haveReset_ = false;
    cascade::core::sys::FirstRunWizard wizard_;
    bool telemetryChosen_ = false;
};

}  // namespace cascade::engine::wirec
