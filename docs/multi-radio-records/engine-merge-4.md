# Engine merge lane 4 (`engine-merge-4`): the second passes of wire-a, wire-c and wire-b merged into engine/multi-radio

Date 2026-11-13. Worktree `C:\Users\steve\foxsdr-wt\mr-dsp`, build `D:/foxsdr-build/mr-dsp` (Slint ON, Release, `--parallel 4`), scratch `D:/foxsdr-build/m4-scratch/`. Method: `engine-merge-3.md` and `engine-fixes-4.md` (same gate commands; hidden desktop `FoxSDRTestsEM`, `FOXSDR_FRAME_CAP_HZ=60`, the `gui_desktop` fixture never bypassed, `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` never set).
Start tip `437bfedd`, branch `engine/multi-radio`, `git status --short` empty (both verified). Nothing pushed. No download, no outside request, no transmit path; one test of the existing suite, `test_rtlsdr_live`, opens the real RTL-SDR dongle on this PC (section 3, red 4): it is in the `headless` label and ran in every earlier gate too.

## 1. The merges (`git merge --no-ff`, in the ordered sequence)

Every merge was made with a temporary message and amended at once to the full message (author "Steven Fox"); the tree did not change by the amend.

| # | Branch (tip) | Merge commit | Conflicts and resolution | Tests run after (real result lines) |
|---|---|---|---|---|
| 1 | feat/wire-a (`cd2b16f5`, 12 commits; verified equal to the commit ordered) | `aaeaddc8` | none (the lane had merged `437bfedd` into itself first; `git diff feat/wire-a HEAD` was empty). Two defects of the lane's own tests were found and repaired in commit `51466210` (section 2, items 1 and 2) | build exit 0 twice; 24 tests (the lane's, `test_apply_command` x3, `test_thread_inventory`, `test_health_paths`, `test_gui_disk_audit`): `88% tests passed, 3 tests failed out of 24`: `test_engine_api_wire_a_display` (flake, section 3), `test_thread_inventory` and `test_apply_command_free` (repaired, section 2); after the repairs `100% tests passed out of 5` (the apply-command variants, `test_thread_inventory`, the display test) |
| 2 | feat/wire-c (`776ebd48`, 7 commits) | `4d164836` | none (auto-merged: `plugin_abi.h`, `plugin_api.cpp`, `plugin_host.cpp`, `engine_api_lists.cpp`, `app_commands.{hpp,cpp}`, `tests/CMakeLists.txt`, `tests/thread_inventory_rows.inc`) | build exit 0 twice (the `plugin_abi.h` change recompiles most of the tree); 104 tests (`test_plugin*`, `test_engine_api_wire_c*`, `test_engine_api_wire_a*`, `test_module_census`, `test_tx_audio`, `fuzz_*plugin_hostapi`, `test_tci*`, `test_apply_command` x3, `test_thread_inventory`, `test_health_paths`): `98% tests passed, 2 tests failed out of 104`: `test_health_paths` (repaired, section 2) and `test_engine_api_wire_a_display` (flake) |
| 3 | feat/wire-b (`049255f1`; `git rev-parse feat/wire-b` read immediately before the merge: the lane had added a record commit after `8b7b7196`) | `3ae64dbb` | none (auto-merged: `engine_api_lists.cpp`, `app_commands.{hpp,cpp}`, `tests/CMakeLists.txt`, `tests/thread_inventory_rows.inc`, the registries) | build exit 0 twice; 80 tests (`test_engine_api_wireb_*`, `test_wire_b_*`, `test_spectrum_block_gpu`, `test_radio_scope`, `test_json_dump_policy`, `test_file_kind`, `test_radio_setup`, `test_engine_shell_guard`, `test_sdriq*`, `test_rfexplorer*`, `test_vendor_*`, `test_thread_inventory`, `test_apply_command` x3, `test_health_paths`, the wire-a and wire-c engine tests): `99% tests passed, 1 tests failed out of 80`: `test_engine_api_wire_a_display` only |

Not merged: nothing ordered was left out. `feat/android`, `feat/knob-input` and every `old/*` branch were not asked for.

## 2. Repairs made by this lane (two commits, `51466210` and `966836c5`: test tables only; no product code changed)

1. `tests/thread_inventory_rows.inc`: a row for `src/core/airband_gate.hpp` (wire-a's `AsyncWavOutput` starts one `std::thread` per open airband channel file). The lane's branch added the thread without the row, so `test_thread_inventory` failed with "UNACCOUNTED THREADS" on the lane's own tip and on the merged tree. The row states why the thread is exempt from a heartbeat (it only writes a file; try-lock hand-over; stopped and joined by its destructor).
2. `tests/wire_a_apply_cover.inc`: the "nothing to undo / nothing to redo" refusals assumed an empty patch edit history. In the `_free` variant of `test_apply_command` the engine's own passes have already seen the patch graph that earlier ops of the test changed, so the history holds steps. RED before: `wire_a_apply_cover.inc:25` and `:26` (`refused(... PATCH_UNDO ...)`), `test_apply_command_free` 6.83 s Failed, twice. The test now drains the history (bounded by the 100-step history) before the refusals; the assertions are unchanged. After: `test_apply_command`, `_stepped` and `_free` Passed. Break-it: the red-then-green pair on the same test (the drain absent, then present).
3. `tests/test_health_paths.cpp:249`: the count of `raiseHostFault();` in `src/core/plugin_ui.cpp` is now 8 (wire-c's `l1SymbolRate` has a catch block like `events_seq`'s, a call with no status channel). RED before: `raises == 7` failed; after, Passed. The lane's record did not list the test among those it ran.

## 3. Gates

Headless + slint, `ctest -C Release -L "headless|slint" -I a,b -j4 --timeout 300`, 1145 tests in four slices through `m4-gate.bat` started by the hidden-desktop launcher (desktop `FoxSDRTestsEM`), waited on through a done-file; binary built at the merge commit `3ae64dbb` (the three test-table repairs were already in it; the records commit that follows changes only `docs/`):

| Slice | Result |
|---|---|
| 1-280 | `99% tests passed, 4 tests failed out of 280` (304.33 s): `test_engine_api_kiwisdr`, `test_airband_data`, `test_converter_call_sites`, `test_band_plan` |
| 281-560 | `99% tests passed, 2 tests failed out of 280` (507.55 s): `test_page_resize_id`, `test_i18n` |
| 561-840 | `99% tests passed, 3 tests failed out of 280` (424.14 s): `test_rtlsdr_live`, `test_png_decode`, `test_pump_live_traffic` |
| 841-1145 | `100% tests passed out of 305` (361.67 s) |

1145 run, 1136 passed, 9 red. Classification (each re-run alone, through ctest on the hidden desktop):

| Red | Class | Evidence |
|---|---|---|
| test_airband_data, test_band_plan, test_converter_call_sites, test_page_resize_id, test_png_decode | the five known false failures | red as in every earlier record; not investigated |
| test_engine_api_kiwisdr | load flake | `Passed 8.08 sec` alone |
| test_pump_live_traffic | load flake | `Passed 9.29 sec` alone |
| test_i18n | **real, the lanes' own: two new English strings are in no catalogue** | alone it fails the same way: `"Not saved: that is not one of the choices."` (wire-c, the settings-form choice list: `SettingEditResult::NotAnOption`) and `"this part is saved with the patch but this build does not run it yet"` (wire-a, `Problem::NodeNotRun`) are missing from all 33 `resources/lang/*.json` (`<lang>.json: 2 missing, 0 stale`). Each lane's tree has the same two gaps (neither added catalogue rows; the standing rule is English strings listed in the record, no translation). Not repaired here: it is a translation of two sentences into 32 languages (the lanes' own, not a one-line fix). Needs the translation step (`tools/i18n_keys.py`, `tools/embed-lang.py`) |
| test_rtlsdr_live | **real hardware, not a code failure** | the test opens the real RTL-SDR dongle bound to WinUSB on this PC; alone it fails the same way at the same line, 25.85 s: `the first 32768 samples REPEAT EXACTLY every 1880 samples - a fixed digital pattern, not a radio signal ... unplug the dongle, plug it back in` (`tests/test_rtlsdr_live.cpp:206`, `period == 0`; stream health clean: 0 timeouts, 0 overflows). Wire-c's record saw the same test fail under load and pass alone, so the state of the dongle is the cause (UNVERIFIED: not unplugged here, nothing of mine opens a radio) |
| test_engine_api_wire_a_display | wire-a's own intermittent, NOT in the gate (it passed there) | failed in 5 of 8 ctest runs during the merge checks and Passed in the others (`ctest` alone and in a group); the exe run directly on the hidden desktop passed 4 of 4 (`121 checks passed, 0 failed`). The failing section is D3: `tests/test_engine_api_wire_a_display.cpp:221` (`trace-mode.json` does not contain `persistence_slow` within 6 s) and `:226` (a new engine starts on `persistence_medium`): the last of seven quick `TRACE_MODE` saves does not reach the file. `BackgroundSaver` holds a write behind the one in flight and starts it only on a `poll()` of the engine pass (`src/core/background_saver.hpp`); cause not found, not repaired (UNVERIFIED) |
| test_plugin_abi3_compat | not red | passes with `0 checks` unless `FOXSDR_PUBLISHED_PLUGINS_DIR` is set (`engine-fixes-4.md`); not re-run against the published binaries |

GUI gate: `ctest -C Release -L gui -j3 --timeout 300` through the launcher on `FoxSDRTestsEM` with `FOXSDR_FRAME_CAP_HZ=60` (`m4-gui.bat`), 340 tests (the `gui_desktop_guard` among them): see section 7.

## 4. Operation-number check

Script `D:/foxsdr-build/m4-scratch/m4-opcheck.py` over `src/core/app_commands.hpp`, `src/core/wire_a_ops.hpp` and `third_party/foxsdr_api/foxsdr_api.h` (all `FOXAPP_OP_*` and `FOXAPI_OP_*` defines): `ops defined: 322 distinct values: 321`; the one value shared is `0x8800`, wire-a's own alias (`FOXAPP_OP_WIRE_A_FIRST` = `FOXAPP_OP_HISTORY_STEP`). Blocks: wire-a 0x8800-0x8831 (the last is `LEVEL_STORE`; `FOXAPP_OP_WIRE_A_LAST` is 0x88FF), wire-b 0x8900-0x8931 (0x8919-0x891B stay unused, dropped in merge 3), wire-c 0x8A00-0x8A10. Every operation of the three blocks is in the `kOps` table (wire-a's through the `WIRE_A_KOPS` macro of `wire_a_ops.hpp`, wire-b's and wire-c's in `app_commands.cpp`) and is named in the coverage of `test_apply_command` (`wire_a_apply_cover.inc`, `wire_b_apply_cases.inc`, `test_apply_command.cpp`); `test_apply_command: 2332 checks, 0 failed` in the merged tree.

## 5. The plugin interface (`src/core/plugin_abi.h`, wire-c): checks in the merged tree

* `CASCADE_MAX_SETTING_SPECS` is `32u` (`src/core/plugin_abi.h:2090`). The only users of the constant agree: `src/core/plugin_host.cpp:913` (the loader check, `SettingsUiBadTable` when `count > CASCADE_MAX_SETTING_SPECS`), `src/core/plugin_settings_ui.cpp:142` (the form), `plugin_host_data.hpp:128` (the rejection's comment); `tests/test_plugin_settings_ui.cpp:351` asserts `CASCADE_MAX_SETTING_SPECS == 32u` and that 16, 17 and 32 rows are accepted and 33 is refused by both paths. No hard-coded 16 was found in `plugin_settings_ui.cpp`, `plugin_settings_form.cpp` or `plugin_host_data.hpp`.
* Host refusal path, in the header's own words: a host OLDER than this one refuses to load a plugin whose table has 17 to 32 rows (`validatePluginDesc` returns `SettingsUiBadTable`, the whole plugin is rejected, not only its form); a plugin that must load on every released host keeps to 16 rows. This is the owner decision 1 below.
* Struct-layout tests, all Passed in the merged tree (`test_plugin_events`, `test_plugin_schedule_hint` (assert that `events_seq` is no longer the last member), `test_plugin_recorder`, `test_plugin_settings_ui`, `test_plugin_api`, `test_plugin_host`, `test_plugin_marker`, `test_plugin_abi3_compat` (0 checks unless the plugin folder is given), `test_module_census`, `fuzz_smoke_plugin_hostapi`, `fuzz_corpus_plugin_hostapi`, `test_tx_audio`) and again in the headless gate.
* Everything is append-only: `CASCADE_PLUGIN_ABI_VERSION` stays 3 and `CASCADE_HOST_API_LEVEL` stays 2; the struct size is the version and `CASCADE_HOST_HAS(host, fn)` is the test.

## PLUGIN HEADER HAND-OFF (for the plugins repository; this lane does not own it)

`git diff 437bfedd HEAD -- src/core/plugin_abi.h` at this lane's end (81 added lines, 2 changed; every host member is appended at the END of `CascadeHostApi`, after `recorder_stop`; the two members added in merge 3 are the context). The plugins repository must append the same constants, structs and members at the same places in its copy; the same diff is in `docs/multi-radio-records/wire-c-plugin-header.diff` (wire-c's own file) and the per-row table of what an older host does is in `wire-c.md` section 11.

```diff
diff --git a/src/core/plugin_abi.h b/src/core/plugin_abi.h
index 69455ca0..6325e2fa 100644
--- a/src/core/plugin_abi.h
+++ b/src/core/plugin_abi.h
@@ -266,7 +266,8 @@ extern "C" {
 #define CASCADE_CAP_TX_AUDIO 0x00004000u /* host API level 2: transmit audio under an operator-armed grant - see below */
 #define CASCADE_CAP_IMAGE_INPUT 0x00008000u /* optional input for a plugin's primary image */
 #define CASCADE_CAP_TRACK_PHOTO 0x00010000u /* a target's photo, on the track-info handle */
-#define CASCADE_CAP_ALL_KNOWN 0x0001FFFFu /* OR of every bit THIS host knows */
+#define CASCADE_CAP_SETTING_CHOICES 0x00020000u /* named options for settings-form fields (wiring lane C, 0.99.109) - see "Named choices" below the settings form */
+#define CASCADE_CAP_ALL_KNOWN 0x0003FFFFu /* OR of every bit THIS host knows */
 
 /*
  * A FLAG, NOT A CAPABILITY: CASCADE_FLAG_PRIVATE_DATA (bit 31, host 0.99.109; engine wiring W2).
@@ -1759,6 +1760,7 @@ typedef struct CascadeScheduleHint {
  * plugin (CASCADE_API_DENIED without it), it is queued and applied by the host's next pass (CASCADE_API_OK means ACCEPTED, not started), and the host
  * refuses a start while the radio is already recording (a take the user started is never taken over, and a plugin can stop only a take it started). The file
  * goes to the recordings folder named with the label first. A plugin that is stopped, unloaded or detached has its take ended by the host. */
+#define CASCADE_OPEN_URL_CHARS 201 /* wiring lane C: an address of at most 200 bytes, plus the terminator */
 #define CASCADE_RECORDER_LABEL_CHARS 33 /* the longest label is 32 bytes of [A-Za-z0-9._-], plus the terminator */
 
 /* ---- Decode events (appended after host API level 2; engine wiring W2, brief item 13 "AD3"): check CASCADE_HOST_HAS(host, post_event). ----------
@@ -1789,6 +1791,7 @@ typedef struct CascadeEvent {
  * mark). A plugin built against the old header still works - it simply never asks for more than 64; a plugin built against this one on an OLDER host gets
  * LIMIT at that host's own quota, which it must treat as the end of its marks, not as an error. The host publishes the number in fact.plugin.markerLimit. */
 #define CASCADE_MAX_MARKERS_PER_PLUGIN 256u
+#define CASCADE_MARKER_CREDIT_CHARS 48 /* wiring lane C: the longest credit is 47 bytes */
 typedef struct CascadeMarker {
     uint32_t structSize;
     uint32_t id;                /* the plugin's own; setting an id REPLACES */
@@ -1799,6 +1802,10 @@ typedef struct CascadeMarker {
     uint32_t colourRgba;        /* 0xRRGGBBAA; 0 = the host's plugin colour */
     uint32_t reserved;          /* 0 */
     char label[CASCADE_MARKER_LABEL_CHARS]; /* may be empty; UTF-8 */
+    /* Appended (wiring lane C, 0.99.109): the credit a data source asks for ("Data provided by HamQTH.com"), shown with the mark; may be empty; UTF-8,
+     * NUL-terminated. A plugin built before this field passes the smaller structSize: the host takes it and the credit is empty. A host older than this
+     * ignores bytes it does not know: the plugin still works there, without the credit. */
+    char credit[CASCADE_MARKER_CREDIT_CHARS];
 } CascadeMarker;
 
 /* The settings store. Keys: 1..63 bytes of [A-Za-z0-9._-]. Values: UTF-8
@@ -2008,6 +2015,21 @@ typedef struct CascadeHostApi {
     int32_t (*recorder_start)(void *ctx, const char *label);
     /* Ends the take THIS plugin started (no effect on any other). Same codes; a stop with no take of this plugin's running is accepted and does nothing. */
     int32_t (*recorder_stop)(void *ctx);
+
+    /* ---- Symbol rate, open-a-page and open-the-library-folder (appended after host API level 2; wiring lane C): check CASCADE_HOST_HAS(host, symbol_rate) etc. ---- */
+
+    /* The symbol rate in baud that the USER has set for the receiver (a number field on the host's own screen), 0 when none is set. Cheap to poll; a decoder that can
+     * use it (a PSK or FSK decoder with a choice of rates) reads it at the start of a block. It is a hint from the user, never a measurement. */
+    double (*symbol_rate)(void *ctx);
+    /* Asks the host to offer the user a button that opens `url` in their web browser: for a library the plugin needs and the user must fetch. NOTHING OPENS HERE.
+     * The host shows the address and the plugin's name, and opens it only when the user presses that button. The address must be https:// (nothing else is
+     * accepted), at most CASCADE_OPEN_URL_CHARS-1 bytes of printable ASCII with no space, no user name or password, a host name, no backslash. One request per
+     * plugin is held: a new one replaces the old. CASCADE_API_OK: held; CASCADE_API_BAD_ARGUMENT (NULL, empty, not an acceptable address); CASCADE_API_OUT_OF_RANGE
+     * (too long); CASCADE_API_DENIED (the plugin is stopped); CASCADE_API_DETACHED. */
+    int32_t (*request_open_url)(void *ctx, const char *url);
+    /* Asks the host to offer a button that opens THIS PLUGIN'S OWN LIBRARY FOLDER in the file manager (the host makes the folder; the plugin cannot name any
+     * other path). Same rules and codes as request_open_url, without an argument. */
+    int32_t (*request_open_library_folder)(void *ctx);
 } CascadeHostApi;
 
 /*
@@ -2065,7 +2087,7 @@ typedef struct CascadeHostApi {
 #define CASCADE_SETTING_LABEL_CHARS 32
 #define CASCADE_SETTING_PLACEHOLDER_CHARS 64
 #define CASCADE_SETTING_DEFAULT_CHARS 64
-#define CASCADE_MAX_SETTING_SPECS 16u /* per plugin; well under the store's 64 keys */
+#define CASCADE_MAX_SETTING_SPECS 32u /* per plugin; half of the store's 64 keys (raised from 16 in 0.99.109: a host older than that REFUSES TO LOAD a plugin whose table has 17..32 rows - validatePluginDesc returns SettingsUiBadTable and the whole plugin is rejected, not only its form; a plugin that must load on every host keeps to 16) */
 
 /* Field kinds. An unknown kind is skipped (not drawn, not refused) so a
  * plugin built for a later host still shows the fields this host knows. */
@@ -2127,6 +2149,58 @@ typedef struct CascadeSettingsUiApi {
     const CascadeSettingSpec *specs;
 } CascadeSettingsUiApi;
 
+/*
+ * NAMED CHOICES (CASCADE_CAP_SETTING_CHOICES, 0.99.109, wiring lane C) - a field whose values have NAMES.
+ *
+ * A combined plugin (hf-keyboard-modes) has a "mode" setting whose values are 1, 2, 3...; until now the form could only show a number box ("Mode 1PSK 2Olivia
+ * 3MFSK16 4DEX"). A plugin that declares this capability names the values, and a host that knows it draws a DROP-DOWN of the names instead.
+ *
+ * HOW A PLUGIN DECLARES ONE. The field is declared in the ordinary settings table as a NUMBER (or a TEXT) whose default is one of the option values, exactly as
+ * before; THAT is what a host older than this one draws, so the plugin stays usable there (a number box, with the plugin's own placeholder as the hint:
+ * "1=PSK 2=Olivia"). Then, in a SECOND table under this capability, each such key is given its options. A host that knows the capability replaces the box of that
+ * key with a drop-down; what is stored is still the option's `value` text, so settings_get returns the same text on every host.
+ *
+ *   * `key` must name a TEXT or NUMBER field of the settings table (not BOOL, PATH or PASSWORD); a choice for any other key is ignored.
+ *   * An option whose `value` is not valid for the field (a NUMBER's must be a decimal number; a TEXT's must fit its maxLength and be a clean line), a repeated
+ *     value, and an empty `value` are skipped; with fewer than two options left the field stays a plain box. `label` is shown as-is (never translated); an
+ *     empty label shows the value.
+ *   * The host refuses to store anything that is not one of the options (SettingEditResult::NotAnOption); a value already in the store that matches none
+ *     (written by the plugin itself, or by an older host) is shown as it is, unselected, until the user picks one.
+ *   * An older host never sees the table: it ignores the capability bit (ABI 3: unknown bits are ignored), so the plugin LOADS there and shows the number box.
+ *   * structSize of each struct is checked; the table is refused (the plugin is rejected, like any other malformed table) when count is 0 or above
+ *     CASCADE_MAX_SETTING_CHOICE_LISTS or choices is NULL; an ENTRY of an unrecognised size is skipped.
+ */
+#define CASCADE_MAX_SETTING_CHOICE_LISTS 16u
+#define CASCADE_CHOICE_MAX_OPTIONS 32u
+#define CASCADE_CHOICE_VALUE_CHARS 16
+#define CASCADE_CHOICE_LABEL_CHARS 32
+
+typedef struct CascadeChoiceOption {
+    /* The text stored in the settings store when this option is picked: 1..15 bytes, NUL-terminated. */
+    char value[CASCADE_CHOICE_VALUE_CHARS];
+    /* What the drop-down shows, NUL-terminated UTF-8, no newlines. */
+    char label[CASCADE_CHOICE_LABEL_CHARS];
+} CascadeChoiceOption;
+
+typedef struct CascadeSettingChoice {
+    /* sizeof(CascadeSettingChoice) as the PLUGIN compiled it. An entry of another size is skipped. */
+    uint32_t structSize;
+    /* The key of a TEXT or NUMBER field in the settings table. */
+    char key[CASCADE_SETTING_KEY_CHARS];
+    /* Number of valid entries in `options`, 2..CASCADE_CHOICE_MAX_OPTIONS. */
+    uint32_t optionCount;
+    CascadeChoiceOption options[CASCADE_CHOICE_MAX_OPTIONS];
+} CascadeSettingChoice;
+
+typedef struct CascadeSettingChoicesApi {
+    /* sizeof(CascadeSettingChoicesApi) as the PLUGIN compiled it. Checked. */
+    uint32_t structSize;
+    /* Number of entries at `choices`, 1..CASCADE_MAX_SETTING_CHOICE_LISTS. */
+    uint32_t count;
+    /* `count` entries, static storage, non-NULL, valid for as long as the module is loaded. */
+    const CascadeSettingChoice *choices;
+} CascadeSettingChoicesApi;
+
 /*
  * ==========================================================================
  * CASCADE_CAP_PRESET - "where do I listen for this?"
@@ -2749,6 +2823,11 @@ static inline const CascadeTrackPhotoApi *cascade_plugin_track_photo(
                                                                    CASCADE_CAP_TRACK_PHOTO);
 }
 
+static inline const CascadeSettingChoicesApi *cascade_plugin_setting_choices(
+    const CascadePluginDesc *desc) {
+    return (const CascadeSettingChoicesApi *)cascade_plugin_capability(desc, CASCADE_CAP_SETTING_CHOICES);
+}
+
 static inline const CascadePresetApi *cascade_plugin_preset(
     const CascadePluginDesc *desc) {
     return (const CascadePresetApi *)cascade_plugin_capability(desc, CASCADE_CAP_PRESET);
```

## 7. GUI gate

`ctest --test-dir D:/foxsdr-build/mr-dsp -C Release -L gui -j3 --timeout 300` through the hidden-desktop launcher (desktop `FoxSDRTestsEM`, `FOXSDR_FRAME_CAP_HZ=60`, `m4-gui.bat`), binary at `3ae64dbb`, 340 tests (the 332 of `engine-fixes-4.md`, plus the new drive scripts and tests of the three lanes): `99% tests passed, 2 tests failed out of 340`, `Total Test time (real) = 2558.86 sec`. `gui_desktop_guard` passed on `FoxSDRTestsEM` (it did not refuse); `FOXSDR_ALLOW_VISIBLE_GUI_TESTS` was never set. The machine was slower than in earlier gates (a Linux build runs in WSL): single drive scripts took about 65 s instead of about 25 s.

| Red | Class | Evidence |
|---|---|---|
| slint_drive_lanel-sweep | **caused by the merge**, fixed (a script expectation made stale by wire-b's engine fix, not a defect) | failed in the gate (25.10 s) and alone: `input script line 33: state ll.note.sweep contains Heat map and occupancy: Refused: frequency range unusable -> FAILED: it is Heat map and occupancy: done`. wire-b's second pass fixed the engine to accept `SWEEP_CONFIG` while no device is open (screen-l.md finding; 6.1 of `wire-b.md`); the script pinned the old refusal. Now expects "Heat map and occupancy: done": Passed alone (25.32 s) |
| slint_drive_lanel-outputs | **caused by the merge**, fixed (the same cause) | failed in the gate (34.70 s) and alone: lines 31, 35 and 46 `state ll.fact.tracks.state == "" -> FAILED: it is {"added":0,"enabled":false,...,"retention_s":21600.0,...}` (and the same for `coverage.state`): wire-b made both facts ALWAYS present (off = `enabled:false` with zero counts). The three lines now expect `"enabled":false`: Passed alone (34.92 s) |

Both repairs are in the commit "engine merge 4: screen lane L's drive scripts expect wire-b's corrected engine" (`src/slint/tests/drive/lanel-sweep.txt`, `lanel-outputs.txt`); `slint_drive_ledger` Passed after it. Every other gui test passed, including every drive script of screen lanes J and K and all earlier drive scripts, and the new `test_engine_api_wire_*` and `test_gui_disk_audit` tests of the lanes in the headless gate above.

The real `%APPDATA%/foxsdr/config.json`: sha256 `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` at the start, after the headless gate and after the GUI gate (`sha256sum` of the real file printed the same value each time; no test opened the real file).

## 8. Owner decisions raised by the three second-pass records (one list, de-duplicated; none was decided here)

The 18 decisions of `engine-merge-3.md` that are still open stay open and are not repeated. New or changed since then:

Plugin interface (wire-c)
1. The settings-row limit is now 32. A host older than this one REFUSES TO LOAD a plugin whose table has 17 to 32 rows (the whole plugin, not its form). Decide whether plugins with more than 16 rows (event-outputs 0.1.0 has 17; ft8-decoder may add WSPR rows) ship only with a host that has the limit, or keep to 16 until every installed host is replaced. The plugins repository must copy the header diff above first.
2. Which of the not-built interface calls are wanted: frame-log push, MFSK tone energies (both need wire-b's consumer), pick-a-file (a screen piece first), and the Visual C++ runtime check for helper programs.
3. The open-a-page and open-a-folder calls: the host never opens anything by itself and the user presses a button; confirm that a plugin may ask for any https address that passes the format rules (no allow-list of hosts).

I/Q, recording and patch (wire-a)
4. Bookmark `offsetKhz` (stored, never applied): recommendation is a receive offset in kHz from the bookmark's frequency, applied as the VFO offset when the bookmark is tuned. Not built until the owner says so.
5. FFT scan "retune by the span and repeat": recommendation is to leave it to the caller (`SET_CENTRE` then `FFT_SCAN` read).
6. Band memory attenuator and antenna: recommendation is to file them from the radio's own list facts once a device with an antenna list is open; testable only on the fake radio here.
7. The record lock default stays OFF (a source change ends an I/Q take); ON would refuse the change.
8. The level store: about 15.1 MB a radio at the 14400-row, 1024-bin default (eight radios an hour each about 121 MB); OFF by default, 1 to 64 MiB a radio. Confirm the default size.
9. Marker and pack import and export are now asynchronous: a missing, too big, existing or refused file is a FAILED job (`fact.job.*`) instead of the operation's own answer. Confirm the changed behaviour.
10. The patch Mixer is a plain sum with gain and mute (no balance, solo or limiter, only channels at the strip rate of the first); the Generator is refused on every transmit sink by an allow-list. Confirm the Mixer's scope.

Sweep, sources and GPU (wire-b)
11. The sweep-history file is plain text, CSV by default with the header line `#foxsdr-sweep-history,csv,1` (JSON and XML offered); budget 300000 points in memory, a file capped at 4 MiB. The format was the orchestrator's decision, the budget and cap were the lane's.
12. `defaultArgs` of the five device rows with no address (`timeout=1500`, `index=0`, `timeout=5000`, `timeout=5000`, `timeout=3000`): each is the source's own documented default; if "nothing typed" should be marked another way, one line changes.
13. The stitched sweep retunes the person's own radio, only on an explicit operation, and puts the centre back; default step fraction 0.6, `passes` 1.
14. One switch (`GPU_FFT_MODE`) now drives both GPU spectrum paths; separating them needs a second operation.
15. The header-only reading of `working` for the header-only library rows (`engine-fixes-4.md`) is still the owner's to confirm.

Gate
16. `test_i18n` is red until the two English sentences are translated into the 32 other languages (wire-c: "Not saved: that is not one of the choices."; wire-a: "this part is saved with the patch but this build does not run it yet"): name who translates, or allow English copies as placeholders in the catalogues.
17. `test_rtlsdr_live` opens the real RTL-SDR dongle on this PC from the `headless` label: decide whether a test may open a user's radio at all (the standing rule says no radio is opened by a lane).

## 9. UNVERIFIED

* Nothing was built for Linux; no radio of mine was opened and no outside service touched; every peer of the lanes' tests is a stand-in (the lanes' own records list what each did not prove on hardware).
* `test_engine_api_wire_a_display` (section 3): cause of its intermittent failure under ctest not found. `test_rtlsdr_live`: the dongle's fixed pattern is assumed to be the device's state, not checked by unplugging it.
* `test_plugin_abi3_compat` ran with 0 checks (not given the published plugin folder).
* The patch Generator and Scope parts do not run (the plan says so); the screen has no canvas captions for the four new kinds.
* The final-tip rebuild and the stamp tests (`test_diagnostics`, `test_diagnostics_free`): their result is in the lane's final report, because any later commit makes the stamp stale again.
