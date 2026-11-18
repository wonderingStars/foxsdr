// lanek_logic.hpp - SCREEN LANE K (docs/multi-radio-records/screen-k.md): the pure rules behind the rows lane K adds to the Slint window - no Slint, no Win32, no engine call.
//
//   Row          one row of the generic panel (ui/system_bank.slint KRow): a kind, an id, a label, a value; the builders below make them
//   Record       the saved choices of lane K: ONE small file, `lanek.json`, beside the settings file (never inside config.json: lane K added no key to either half of the settings;
//                the same arrangement as lane C's lanec.json). Typed getters with defaults; a value of the wrong type reads as the default; the file is read with every bound checked.
//   SettingsKeeper  what the Settings and backup rows do to FILES, over the sysmodels lane's models (core/sys_bundle, sys_profiles, sys_reset): export, back up (and prune), import
//                   preview, profiles (save, list, delete, switch), reset, undo reset, the shared-folder sync. It NEVER writes the settings file under a running engine: an import, a
//                   reset, a profile switch and an undo are PENDING operations that the host carries out after the engine has stopped and saved for the last time (main.cpp, after
//                   service.stop()), because the engine's own final save would otherwise overwrite them.
//   (the Matrix's hover-to-listen policy is core/radio_matrix_model.hpp's HoverListen, used as it is by lanek_publish.cpp.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/sys_bundle.hpp"
#include "core/sys_profiles.hpp"
#include "core/sys_reset.hpp"

namespace foxsdr::slintlanek {

namespace fs = std::filesystem;

// ============================================================ rows ============================================================
const std::vector<const char*>& wizardStrings();   // the setup's radio and preset words, for the string tables

enum RowKind { kHeading = 0, kNote = 1, kKey = 2, kTick = 3, kChoice = 4, kField = 5, kReadout = 6, kSlider = 7, kBar = 8, kMasked = 9 };
enum RowTone { kDim = 0, kWarn = 1, kErr = 2, kGood = 3 };

struct Row {
    int kind = kNote;
    std::string id, label, value, note;
    bool on = false, enabled = true;
    std::vector<std::string> options;
    int pick = 0;
    float level = 0.0f;
    int tone = 0;
};

Row rHeading(const std::string& label);
Row rNote(const std::string& text, int tone = kDim);
// A section's one status line. A refusal ("<name of the key or field>: <why>", tone kErr) is put directly under the row it names, so it is read beside the box that
// was wrong and not at the end of a section hundreds of pixels away (look review item 10); a line that names no row, or any other tone, goes at the end as before.
void placeNote(std::vector<Row>& rows, const std::string& text, int tone);
// DECISION 61 (must-fix lane A, owner decision 61): the ONE label on every radio family and setting that was proved only against stand-ins, never on a real radio. One component, one sentence.
Row rUnproven();
Row rKey(const std::string& id, const std::string& label, bool enabled = true, const std::string& tip = "", bool on = false);
// A key or tick that is dimmed says why in one line (look review M4): the reason is shown under it only while it is dimmed.
Row rKeyWhy(const std::string& id, const std::string& label, bool enabled, const std::string& why, const std::string& tip = "", bool on = false);
Row rTickWhy(const std::string& id, const std::string& label, bool on, bool enabled, const std::string& why, const std::string& tip = "");
Row rTick(const std::string& id, const std::string& label, bool on, bool enabled = true, const std::string& tip = "");
Row rChoice(const std::string& id, const std::string& label, const std::vector<std::string>& options, int pick, bool enabled = true, const std::string& tip = "");
Row rField(const std::string& id, const std::string& label, const std::string& value, const std::string& tip = "", bool enabled = true);
// A one-line field whose text is hidden on the screen (a password): kind 9. The text is the person's draft; it is sent once and dropped.
Row rMasked(const std::string& id, const std::string& label, const std::string& value, const std::string& tip = "", bool enabled = true);
Row rReadout(const std::string& label, const std::string& value);
Row rSlider(const std::string& id, const std::string& label, float level, const std::string& readout, bool enabled = true, const std::string& tip = "");
Row rBar(const std::string& label, float level, const std::string& value);

// A short signature of a list of rows, to publish only what changed.
std::string signatureOf(const std::vector<Row>& rows);

// WHAT A REPUBLISH MAY KEEP (drive fixes 2, item 2): a panel whose rows were put in a NEW model every time one figure moved (the position of a playing file, twice a second) had every control of the panel
// destroyed and made again - the field a person was typing in lost the keyboard and its text, and a press that landed meanwhile went to a control that was gone. When `now` has the shape of `last` (the
// same number of rows, the same kind and id of each) only the rows that differ need new data: `changed` is filled with their indexes and the function answers true. Anything else answers false and the
// panel is published whole, as before.
bool sameRowShape(const std::vector<Row>& last, const std::vector<Row>& now, std::vector<std::size_t>& changed);

// ============================================================ the record ============================================================
class Record {
public:
    bool loadText(const std::string& text);   // false (and an empty record) for anything that is not a JSON object; never throws
    std::string toText() const;
    bool getBool(const std::string& key, bool def) const;
    long long getInt(const std::string& key, long long def, long long lo, long long hi) const;   // clamped into lo..hi
    double getNum(const std::string& key, double def, double lo, double hi) const;
    std::string getStr(const std::string& key, const std::string& def = "", std::size_t maxLen = 2048) const;
    // Each returns true when the stored value changed (and marks the record dirty).
    bool setBool(const std::string& key, bool v);
    bool setInt(const std::string& key, long long v);
    bool setNum(const std::string& key, double v);
    bool setStr(const std::string& key, const std::string& v);
    bool dirty() const { return dirty_; }
    void clean() { dirty_ = false; }
    // the map-valued entries (the sync's last-seen hashes): name -> text, bounded
    std::map<std::string, std::string> getMap(const std::string& key) const;
    void setMap(const std::string& key, const std::map<std::string, std::string>& m);

private:
    std::map<std::string, std::string> kv_;   // key -> the value as a compact JSON text
    bool dirty_ = false;
};

// ============================================================ the settings keeper ============================================================
struct KeeperDirs {
    fs::path settings;    // the folder of config.json (named foxsdr: the reset's guard)
    fs::path data;        // %LOCALAPPDATA%/FoxSDR: plugins and logs (reset only, opt-in)
    fs::path exports;     // where "Export settings" writes
    fs::path backups;     // where "Back up now" writes
    fs::path profiles;    // the profiles
};

enum class Op { None, Import, Reset, Switch, UndoReset };

struct Pending {
    Op op = Op::None;
    std::string token;        // the confirmation token of the plan the user saw
    std::string stamp;        // "YYYYMMDD-HHMMSS"
    cascade::core::sys::ImportResult staged;      // Import
    cascade::core::sys::SwitchPlan sw;            // Switch
    cascade::core::sys::ResetOptions reset;       // Reset
    std::string undoStamp;                        // UndoReset
    std::string profile;                          // Switch: the profile's name (the switch is planned again at the end, against the files as the engine left them)
    std::string what;                             // a short word for the record and the log: the file or profile name
};

struct ExportOut {
    bool ok = false;
    std::string path;      // the file written
    std::string error;     // the model's own word when not ok
    int stripped = 0;      // how many secrets / identity values were left out
};

struct ImportPreview {
    bool ok = false;
    std::string error;
    cascade::core::sys::ImportResult res;
    cascade::core::sys::ImportPlan plan;
    int replace = 0, added = 0, identical = 0;
};

struct SyncReport {
    bool ok = false;
    std::string error;
    int pushed = 0, pulled = 0, conflicts = 0, same = 0;
    std::vector<std::string> conflictNames;
};

class SettingsKeeper {
public:
    explicit SettingsKeeper(KeeperDirs dirs) : d_(std::move(dirs)) {}
    const KeeperDirs& dirs() const { return d_; }

    // The files the bundle carries, as they are on disk now (a file that is absent is simply left out).
    bool readCurrent(std::vector<cascade::core::sys::BundleFile>& out, std::string& err) const;

    ExportOut exportTo(const fs::path& dir, const std::string& stamp, const std::string& appVersion) const;
    ExportOut backupNow(const std::string& stamp, const std::string& appVersion, int keep) const;
    // *.foxsdr-bundle in a folder, newest first by name; at most `limit`
    std::vector<std::string> listBundles(const fs::path& dir, std::size_t limit = 50) const;
    ImportPreview previewImport(const fs::path& file) const;

    std::vector<std::string> profileNames() const;
    bool saveProfile(const std::string& name, bool overwrite, const std::string& appVersion, std::string& err) const;
    bool deleteProfile(const std::string& name, std::string& err) const;
    cascade::core::sys::SwitchPlan planSwitch(const std::string& name) const;

    cascade::core::sys::ResetPlan planReset(const cascade::core::sys::ResetOptions& o) const;
    std::vector<std::string> resetStamps() const;   // "reset-<stamp>" folders in the settings folder, newest first (the stamps only)

    // The profile folder against a shared folder (never a conflict decided silently: a conflict is reported and left alone).
    SyncReport sync(const fs::path& shared, std::map<std::string, std::string>& base) const;

    // THE FILE WORK, at the end of the run, after the engine has made its last save. The plan the user confirmed named WHAT would be replaced; the engine's last save has since changed the files' bytes, so the
    // models' confirmation token (a hash of what is on disk) is asked for again here, and the work is refused only when the plan is no longer the same list of files. False with `detail` set when it could not be done (nothing is half-done: the models roll back).
    bool apply(const Pending& p, std::string& detail) const;

private:
    KeeperDirs d_;
};

// "YYYYMMDD-HHMMSS" of a unix time, local time (the stamp the models want: 8 digits, '-', 6 digits)
std::string stampOf(std::int64_t unixSeconds);

}  // namespace foxsdr::slintlanek
