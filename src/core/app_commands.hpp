// app_commands.hpp - the ONE vocabulary every change to the receiver is said in
// (engine extraction, stage 1; docs/engine-stage1.md).
//
// WHAT THIS IS. The FoxSDR engine API (third_party/foxsdr_api/foxsdr_api.h,
// API 0.3: 0.2 as vendored, extended by docs/api-0.3-additions.md) describes a change to the receiver as a
// FoxCommand: an op and a few typed slots. Stage 1 makes that literally the
// only way the application changes its receiver: a desktop widget, a key
// binding, a browser (web /api/control), CAT and a plugin (host API level 1)
// all end as FoxCommands applied by ONE function, AppWindow::applyCommand.
// This header is the half of that which needs no window: the extension ops,
// the builders every caller uses, and the op names for logs and tests.
//
// EXTENSION OPS (FOXAPP_OP_*, 0x8000-0x8FFF). API 0.2 has gaps (its own
// docs/API.md 10.7), and the desktop has controls whose exact behaviour no
// 0.2 op carries. Each such control gets an app-internal op here rather than
// a quiet change of behaviour; every one is listed, with its reason, in
// docs/engine-stage1.md so the API can adopt or retire it later. None of
// them is ever sent across a transport: the web and CAT parsers produce only
// API ops or the extension ops documented as translations of their own
// fields.
//
// THE LONG-TEXT CHANNEL. FoxCommand::text holds 255 bytes (API gap 10). A
// path typed into the desktop can be longer, so a command applied in-process
// may carry its full text beside the struct (QueuedCommand::longText); an op
// that takes text reads the long text when it is not empty. makeText() fills
// both, so no caller decides.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "foxsdr_api.h"

// --- extension ops -------------------------------------------------------------
// Tuning.
#define FOXAPP_OP_VFO_TO_ABSOLUTE        0x8101u /* num[0]: air Hz; ival[0]: 1 = snap to the mode's raster. Click-to-tune. */
#define FOXAPP_OP_SET_VFO_OFFSET_FREE    0x8102u /* num[0]: offset Hz, NOT clamped into the band (the rail's VFO slider) */
// Demodulator and audio.
#define FOXAPP_OP_SET_BANDWIDTH_STEP     0x8201u /* ival[0]: index into the bandwidth steps; not clamped (the rail combo) */
#define FOXAPP_OP_SET_BANDWIDTH_DRAG     0x8202u /* num[0]: Hz, clamped; the step index left alone (a spectrum edge drag) */
#define FOXAPP_OP_SET_NR_STRENGTH        0x8203u /* num[0]: 0..1, the enable left alone */
#define FOXAPP_OP_SET_NOTCH_FREQUENCY    0x8204u /* num[0]: Hz, the enable and Q left alone */
#define FOXAPP_OP_SET_NOTCH_Q            0x8205u /* num[0]: Q, the enable and frequency left alone */
// Display.
#define FOXAPP_OP_SET_DISPLAY_MIN        0x8301u /* num[0]: dB; dbMax kept, dbMin yields to the minimum span */
#define FOXAPP_OP_SET_DISPLAY_MAX        0x8302u /* num[0]: dB; dbMin kept, dbMax yields to the minimum span */
// Source and device.
#define FOXAPP_OP_SCAN_DEVICES_ON_OPEN   0x8401u /* the Source list opening: native always, SoapySDR if never (wholly) scanned */
#define FOXAPP_OP_SET_NETWORK_USRP_SCAN  0x8402u /* ival[0]: 0/1 "Look for network USRPs"; on rescans SoapySDR */
#define FOXAPP_OP_SET_GAIN_NO_READBACK   0x8403u /* text: stage; num[0]: dB. The radar scope's GAIN knob (no readback) */
#define FOXAPP_OP_SET_CONVERTER          0x8404u /* ival[0]: 0 off 1 up 2 down; ival[1]: inverted; num[0]: LO Hz */
#define FOXAPP_OP_SOUNDCARD_IQ_CENTRE    0x8405u /* num[0]: Hz the external receiver is tuned to; a running I/Q card follows at once */
#define FOXAPP_OP_SCAN_NATIVE_ONLY       0x8406u /* a Radio part added (0.99.40): the native walk only, never the SoapySDR probe */
#define FOXAPP_OP_PATCH_RADIO_LIST_OPENED 0x8407u /* a Radio's device list opened (0.99.40): native at once; SoapySDR a deferrable wish, first time only */
// Bookmarks and scanner.
#define FOXAPP_OP_BOOKMARK_IMPORT_FILE   0x8601u /* text (or long text): a PATH on this machine; LOCAL only */
#define FOXAPP_OP_SCANNER_RANGE          0x8602u /* ival[0]: mask 1 start 2 stop 4 step 8 reconfigure-if-running; num[0..2]: Hz */
#define FOXAPP_OP_BOOKMARK_EXPORT        0x8603u /* long text: bookmark ids, decimal, space-separated; writes them as SDR# XML under the record folder; ival[0] 1: the text is a preset's (group's) name and its rows go out as a CSV; the file is written on a worker (B19); LOCAL only */
// THE FREQUENCY LIST'S TICK, ROWS TYPED IN, AND THE SCANNER'S LIST MODE (stub campaign B6, 0.99.60 - 0.99.66; docs/api-0.3-additions.md 9.6), all LOCAL
// sessions only and none of them on the web profile. A bookmark's tick is what the scanner's list mode and the Airband monitor play.
//   BOOKMARK_TICK (SETTINGS): ival[0]: the bookmark's id; ival[1]: 0 untick, 1 tick. NOT_FOUND for an id the list does not hold. A row already
//   as asked changes nothing (and writes nothing).
//   BOOKMARK_ADD_ROWS (SETTINGS): long text (or text), one row a line: "<Hz>\t<mode>\t<bandwidth Hz>\t<flags>\t<group>\t<name>" (the group and the
//   name are the last two fields and may be empty; a name with a tab in it is the sender's to clean; <flags> is bit 0 ticked, bit 1 favourite - "0" and
//   "1" as every sender wrote them before the favourite was carried, stub campaign B19). ival[0]: 0 a row TYPED IN - added as the list's
//   own add does (a name already in use gets " (2)"), and judged by the rule a tune the radio refuses is: a frequency outside what the open radio
//   covers (a converter's LO taken out) is OUT_OF_RANGE with the sentence that says what it does cover - 1 a LIST (an airport's rows) - added as an
//   import does, a row identical in name, frequency and group to one already there skipped, no range judged. applied[0]: the rows added. BAD_ARGUMENT
//   for text with no row in it.
//   SCANNER_LIST (TUNE): ival[0]: 0 range mode (the scanner scans the range again), 1 list mode (it scans the ticked rows, in frequency order, each
//   tuned with its own mode and bandwidth) - either stops a running scan, as the checkbox did - 2 START a list scan (list mode, whatever it was).
//   A start with nothing ticked is NO_CHANGE. List mode is session-only. While a list scan runs it follows the ticks: a row ticked or unticked
//   restarts it from the first row, nothing ticked stops it. SCANNER_RUN (a range scan, from the desk or a browser) leaves list mode.
#define FOXAPP_OP_BOOKMARK_TICK          0x8604u /* ival[0]: the bookmark's id; ival[1]: 0 untick / 1 tick */
// BOOKMARK GROUPS OF THE USER'S OWN (master 0.99.93, 97ea658c; merged 2026-10-27; docs/api-0.3-additions.md 9.9), LOCAL sessions only, SETTINGS. One op, three
// actions, the way FOXAPP_OP_AIRBAND is one op: ival[0] is the action. The group names are trimmed of blanks by the ENGINE (a name of blanks alone is no group).
//   0 ADD CURRENT IN A GROUP: text (or long text) "<group>\x1F<name>" (an empty group is no group; an empty name is the frequency, as BOOKMARK_ADD names one).
//     Added as BOOKMARK_ADD adds it - the tuned station, the live mode and the requested bandwidth. applied[0]: the new bookmark's id. BAD_ARGUMENT for a text with no \x1F.
//   1 MOVE ONE BOOKMARK: ival[1]: its id; text: the group it goes to ("" = no group). NOT_FOUND for an id the list does not hold. A bookmark already in that group
//     changes nothing and writes nothing.
//   2 RENAME A GROUP: text "<from>\x1F<to>": every bookmark of <from> goes to <to> - a rename when <to> is new, a merge when the list already has it. applied[0]: how many
//     moved; the note says "Renamed ...". BAD_ARGUMENT for an empty <to> (the rows' own "No group" is how a group is emptied); <from> == <to> changes nothing; NOT_FOUND
//     when no bookmark is in <from>. A preset the Airband monitor is playing keeps playing under its new name.
#define FOXAPP_OP_BOOKMARK_GROUP         0x860Bu /* ival[0]: 0 add current in a group / 1 move one (ival[1]: id) / 2 rename a group; text per action (see above) */
#define FOXAPP_OP_BOOKMARK_ADD_ROWS      0x8605u /* long text: "<Hz>\t<mode>\t<bw Hz>\t<flags: 1 tick, 2 favourite>\t<group>\t<name>" lines; ival[0]: 0 typed rows / 1 a list */
#define FOXAPP_OP_SCANNER_LIST           0x860Au /* ival[0]: 0 range mode / 1 list mode / 2 start a list scan */
// THE WATERFALL'S MARKERS (stub campaign B10, 0.99.57; docs/api-0.3-additions.md 9.8), all LOCAL sessions only and none of them on the web profile. A marker is
// a pencil tick made while looking at something else - a frequency, a number to call it by, the moment it was made and a note added later - and NOT a bookmark;
// the list is the engine's (Engine::freqMarkers_, markers.json, saved on a worker) and is read as the list FREQ_MARKERS (53). THE NUMBERS ARE NAMES: M1, M2, ... are
// handed out in the order markers are dropped and never again while the list lives - only CLEAR starts the count at M1 again - and the count is persisted.
//   MARKER_DROP (SETTINGS): num[0]: the frequency, ABSOLUTE Hz (the pointer's, on a patch Display part the part's radio's centre taken in: never the part's
//   offset); num[1]: the merge reach, Hz (0: none) - a marker within it of one already there is THAT marker and nothing is added, so a second right-click
//   on the same signal does not stack two flags on one pixel. applied[0]: the marker's number; applied[1]: 1 a new marker, 0 the one already there. The
//   engine stamps the time. OUT_OF_RANGE for a frequency that is not positive, LIMIT when the list holds its 200 (or the numbers are spent).
//   MARKER_REMOVE (SETTINGS): ival[0]: the marker's number. NOT_FOUND for a number the list does not hold. The number is not handed out again.
//   MARKER_CLEAR (SETTINGS): no arguments. Empties the list and starts the numbering again at M1. NO_CHANGE for a list that is empty already.
//   MARKER_NOTE (SETTINGS): ival[0]: the marker's number; text (or long text): the note, as typed ("" clears it). NOT_FOUND for a number the list does not
//   hold; the same note again changes nothing and writes nothing.
#define FOXAPP_OP_MARKER_DROP            0x8606u /* num[0]: absolute Hz; num[1]: merge reach Hz (0 none); applied[0]: the number, applied[1]: 1 new / 0 the one already there */
#define FOXAPP_OP_MARKER_REMOVE          0x8607u /* ival[0]: the marker's number */
#define FOXAPP_OP_MARKER_CLEAR           0x8608u /* no arguments: empties the list, the numbering starts again at M1 */
#define FOXAPP_OP_MARKER_NOTE            0x8609u /* ival[0]: the marker's number; text (or long text): its note */
// TIMED RECORDINGS (docs/API.md 6.23; engine_schedule.cpp, core/schedule.hpp), all LOCAL sessions only and none of them on any internal profile. A schedule is up to 32
// entries - a radio, a start (UTC seconds), a length, once / daily / weekly, optionally a frequency / mode / bandwidth to tune to first, audio and / or I/Q to
// record, and whether to put the tuning back - kept in the settings and read as the facts fact.schedule.* (one JSON document per entry). The clock starts an
// entry at its time, up to 30 s late; a start that fell while FoxSDR was closed or the clock jumped is MISSED (counted, shown, never run late). An entry whose
// radio is closed, stopped, opening, already recording or held by the scanner is REFUSED at its start, in words. Entries on one radio may not overlap.
//   SCHEDULE_SET (SETTINGS): long text (or text): a JSON object with any of name, radio, startUnix, lengthS, repeat ("once" | "daily" | "weekly"), tune, freqHz,
//   mode (FOXAPI_DEMOD_* 1..7), bandwidthHz, recordAudio, recordIq, returnToWhereIWas, enabled; an UNKNOWN key is an error. ival[0]: 0 adds an entry, an id replaces
//   that entry (every field not named keeps its value). applied[0]: the entry's id. OUT_OF_RANGE (the sentence names the field), BUSY (it overlaps another entry on
//   the radio), LIMIT (the 32nd is the last), NOT_FOUND (no such id), BAD_ARGUMENT (not an object, a wrong kind of value, an unknown key).
//   SCHEDULE_REMOVE (SETTINGS): ival[0]: the id. A recording it is making ends at the next pass. NOT_FOUND.
//   SCHEDULE_ENABLE (SETTINGS): ival[0]: the id; ival[1]: 0 off / 1 on. NO_CHANGE when it already is; BUSY when switching it on would overlap.
//   SCHEDULE_RUN_NOW (SETTINGS): ival[0]: the id. Starts it at the next pass, outside its schedule (it does not use up a scheduled start). BUSY while it records.
//   SCHEDULE_SKIP (SETTINGS): ival[0]: the id. The next start passes without happening. NO_CHANGE for a once-entry that has no next start.
#define FOXAPP_OP_SCHEDULE_SET           0x860Cu /* long text: the entry as JSON; ival[0]: 0 add / an id to replace; applied[0]: the id */
#define FOXAPP_OP_SCHEDULE_REMOVE        0x860Du /* ival[0]: the id */
#define FOXAPP_OP_SCHEDULE_ENABLE        0x860Eu /* ival[0]: the id; ival[1]: 0/1 */
#define FOXAPP_OP_SCHEDULE_RUN_NOW       0x860Fu /* ival[0]: the id */
#define FOXAPP_OP_SCHEDULE_SKIP          0x8610u /* ival[0]: the id */
//   SCHEDULE_FROM_HINT (SETTINGS): a plugin's hint (a predicted satellite pass, CascadeScheduleHint; fact.schedule.hint.<n>) made into an entry, because THE USER
//   PICKED IT. ival[0]: the hint's number n (1..fact.schedule.hint.count); ival[1]: the radio (1..8, 0 = radio 1); text: the entry's name ("" = the hint's label). The
//   entry starts a minute before the hint's start and ends a minute after its end, tunes to its frequency / mode / bandwidth, records the I/Q and puts the tuning back;
//   it is then an ordinary entry (SCHEDULE_SET's refusals apply: OUT_OF_RANGE, BUSY on overlap, LIMIT). NOT_FOUND for a number the list does not hold. applied[0]: the id.
#define FOXAPP_OP_SCHEDULE_FROM_HINT     0x8611u /* ival[0]: hint number; ival[1]: radio; text: name; applied[0]: the new entry's id */
//   CAL_MEASURE (SETTINGS; engine wiring lane 1, S-meter calibration): the selected radio (target) must be running. num[0]: the known level in dBm (-150..+30) of the signal on it. ival[0]: 0 = the
//   band that holds the radio's frequency, [0, 30 MHz) or [30 MHz, 6 GHz); 1 = the band num[1] .. num[2] Hz. The engine averages the radio's level for 3 s and stores offset = known dBm - measured
//   dBFS for that band, for that DEVICE (fact.smeter.cal.state says measuring, then done or failed). BUSY while another measurement runs; OUT_OF_RANGE a level or band that is not one.
//   CAL_CLEAR (SETTINGS): the selected radio's device. ival[0]: 0 = the entry that covers num[0] Hz (0 = the radio's own frequency); 1 = every entry of the device. NOT_FOUND when there is none.
#define FOXAPP_OP_CAL_MEASURE            0x8612u /* num[0]: known dBm; ival[0]: 0 default band / 1 explicit; num[1], num[2]: lo, hi Hz. Addresses one radio (target) */
#define FOXAPP_OP_CAL_CLEAR              0x8613u /* ival[0]: 0 one entry (num[0] Hz, 0 = this radio's) / 1 all of the device. Addresses one radio (target) */
//   MACROS (SETTINGS; engine wiring W2, core/macros.hpp, docs/api-0.3-additions.md 9.34): named command sequences kept in macros.json in the per-user folder, played on the control
//   pass with the engine's clock. The steps are the engine's own commands (on the first radio) minus the model's denied prefixes (anything that transmits, grants, installs, edits the
//   servers, uploads, tests, or starts another macro op). MACRO_RUN text: the macro's name; OK once started; NOT_FOUND no such macro; BUSY one is running; BAD_ARGUMENT the file is refused
//   (the message names the macro and step; no macro is available then) or the name is empty. MACRO_STOP: NO_CHANGE when none is running. MACRO_RELOAD reads the file again (BUSY while
//   one runs; BAD_ARGUMENT with the file's error). A step the engine refuses ends the run with its words in fact.macro.last. The facts: macro.names (newline separated), macro.running,
//   macro.last, macro.error, macro.steps. All three are the application's own (LOCAL sessions only).
#define FOXAPP_OP_MACRO_RUN              0x8614u /* text: the macro's name */
#define FOXAPP_OP_MACRO_STOP             0x8615u
#define FOXAPP_OP_MACRO_RELOAD           0x8616u
//   RADIO_LOCK (SETTINGS; engine wiring W2, slint-gaps item 3's engine half, docs/api-0.3-additions.md 9.35): ival[0] 1 = lock the selected radio's (target) FREQUENCY, 0 = unlock. While a radio is
//   locked the engine refuses, with FOXAPI_DENIED, every tuning command aimed at it - SET_CENTRE, SET_FREQUENCY, SET_VFO_OFFSET, STEP_TUNE, BOOKMARK_TUNE, PLUGIN_PRESET, VFO_TO_ABSOLUTE,
//   SET_VFO_OFFSET_FREE, USER_PRESET_APPLY, a list scan start, an air-band listen / next block / tune one, a SCANNER_RUN that starts - and every other way a retune reaches the source (a
//   running scan's next step, a plugin's request_tune) is a no-op: the lock is the ENGINE's, so a plugin, the web page and CAT cannot move a locked radio. Mode, bandwidth, volume, squelch,
//   stop, start and the scanner's stop are not tuning and still work; DECODER_START applies a preset on the engine's own side and is not in the set. NO_CHANGE when it is already so.
//   Not saved (a restart unlocks). fact.radio.<n>.locked is "1" or "0".
#define FOXAPP_OP_RADIO_LOCK             0x8617u /* ival[0]: 1 lock / 0 unlock. Addresses one radio (target) */
//   SET_RECORD_NAME (SETTINGS; engine wiring W2, core/rec_naming.hpp, docs/api-0.3-additions.md 9.36): the file-name TEMPLATE for every recording take, kept in record-name.txt in the per-user folder.
//   text: the template ("{freq_mhz}/{mode}_{datetime}", tokens {date} {time} {datetime} {year} {month} {day} {hour} {min} {sec} {ms} {tz} {freq} {freq_khz} {freq_mhz} {mode} {radio} {bookmark}
//   {counter} {counter:N}; '/' separates up to three folders below the recordings folder; the extension is added; local time) or "" to go back to the default names. BAD_ARGUMENT with the model's
//   words for a template it refuses (nothing changes); NO_CHANGE when it is in force already. fact.record.nameTemplate reads it back.
#define FOXAPP_OP_SET_RECORD_NAME        0x8618u /* text: the template, "" = the default naming */
//   SET_STEREO_SYSTEM (SETTINGS; engine wiring W2, dsp/oirt_stereo.hpp, docs/api-0.3-additions.md 9.37): which composite decoder the selected radio's (target) WFM mode runs. ival[0]: 0 = AUTOMATIC
//   (OIRT polar modulation while the tuned frequency is inside 65.8 to 74 MHz, the pilot-tone system everywhere else: the default), 1 = the pilot-tone system always, 2 = OIRT always; any other value is
//   OUT_OF_RANGE. NO_CHANGE when it is already so. Not saved (a restart returns to automatic). fact.radio.<n>.stereo.system reads auto / pilot / oirt, fact.radio.<n>.stereo.decoder the decoder in the
//   path now (pilot / oirt: the OIRT decoder runs only in WFM at a channel rate of 100 kHz or more).
//   FILE_SEEK and FILE_LOOP (SETTINGS; engine serial lane A, row P15, source/iq_file_source.hpp, docs/api-0.3-additions.md 9.39): the seek bar and the A-B loop of a radio that is playing an I/Q file (target).
//   FILE_SEEK: num[0] is the place to play from, ival[0] = 0 in seconds from the start of the recording, 1 as a fraction 0..1 of its length; OUT_OF_RANGE past either end, UNSUPPORTED when the radio is
//   not playing a file. FILE_LOOP: ival[0] = 1 loops between num[0] and num[1] seconds (0 <= start < end <= the length; the playing wraps from the end back to the start with no gap), ival[0] = 0 clears
//   the loop. The position, length, loop and real start time are read back from the radio's state (filePositionSec and its neighbours); none of it is saved (a file starts from the top).
#define FOXAPP_OP_FILE_SEEK              0x861Au /* ival[0]: 0 seconds / 1 fraction of the length; num[0]: the place. Addresses one radio (target) */
#define FOXAPP_OP_FILE_LOOP              0x861Bu /* ival[0]: 1 loop between num[0] and num[1] seconds / 0 clear. Addresses one radio (target) */
#define FOXAPP_OP_VFO_SELECT                 0x861Cu /* ival[0]: 0 = VFO A, 1 = VFO B. Addresses one radio (target) */
#define FOXAPP_OP_VFO_SWAP                   0x861Du /* swap the two VFOs: the radio takes the other letter's frequency, mode and bandwidth and the other letter keeps what it had. Addresses one radio (target) */
#define FOXAPP_OP_VFO_STORE                  0x861Eu /* ival[0] = 0: copy the radio's tune into the other VFO; 1: set the other VFO to ival[1] mode (1-based FOXAPI_DEMOD), num[0] Hz, num[1] bandwidth Hz, and the active letter to num[2] (0/1). Addresses one radio (target) */
//   SET_FM_WIDTH (SETTINGS; engine serial lane A, row D7): ival[0] 0..4 = narrow 12.5 kHz, medium 30 kHz, medium-wide 50 kHz, broadcast 150 kHz, super-wide 225 kHz (core/receiver_tables.hpp kFmWidthHz), applied
//   to the selected radio's (target) channel filter in NFM or WFM (UNSUPPORTED in any other mode); a width the channel cannot pass (0.9 of its rate) is held lower and the result carries CLAMPED. applied[0] is the width.
#define FOXAPP_OP_SET_FM_WIDTH           0x861Fu /* ival[0]: 0..4 narrow / medium / medium-wide / broadcast / super-wide. Addresses one radio (target) */
//   CHANNEL_CLAIM and CHANNEL_RELEASE (SETTINGS; engine serial lane A, row R19, core/tuner_pool.hpp): tuner pooling. CHANNEL_CLAIM: num[0] the frequency, Hz; ival[0] the mode (1-based FOXAPI_DEMOD_*, 0 = leave it);
//   num[1] the bandwidth, Hz (0 = leave it). The pool picks a FREE radio (open and running, not claimed, not locked, not recording, not scanning): first one whose span on show already covers the frequency
//   (tuned by its VFO alone, its hardware untouched), then one that can tune there by moving its centre; the lowest number among equals. applied[0] is the radio's number, applied[1] 1 when it was tuned by its
//   VFO alone and 2 when its centre moved. LIMIT when no radio is free (the message says why), OUT_OF_RANGE for a frequency that is not a frequency. The radio stays claimed (fact.radio.<n>.claimed = 1)
//   until CHANNEL_RELEASE (ival[0] = its number; NO_DEVICE for a radio that is not there, NO_CHANGE for one that is not claimed). Claims are not saved.
#define FOXAPP_OP_CHANNEL_CLAIM          0x8620u /* num[0]: Hz; ival[0]: mode (0 keep); num[1]: bandwidth Hz (0 keep). Not radio-scoped: the pool chooses */
#define FOXAPP_OP_CHANNEL_RELEASE        0x8621u /* ival[0]: the radio's number */
//   MPX_RECORD (SETTINGS; engine serial lane A, row D4): ival[0] 1 starts, 0 stops a recording of the FM MULTIPLEX of the selected radio (target) - the composite the discriminator gives before the stereo decoder
//   (mono, the 19 kHz pilot, the 38 kHz difference, the 57 kHz RDS), resampled to 192 kHz, written as a 32-bit float mono WAV named mpx_<date>_<time>_192000Hz.wav in the recordings folder, for a program such as
//   RDS Spy to open. UNSUPPORTED unless the radio is in WFM (the multiplex exists only there), NO_CHANGE when it is so already, FAILED with the recorder's words when the file cannot be made. It ends with the
//   radio (stop, removal, shutdown). fact.radio.<n>.mpx reads 1 while it records. The file start blocks for the disk (a control-thread call, like a settings write).
#define FOXAPP_OP_MPX_RECORD             0x8622u /* ival[0]: 1 start / 0 stop. Addresses one radio (target) */
//   IF_RECORD (SETTINGS; engine serial lane A, row P4): ival[0] 1 starts, 0 stops a recording of the IF of the selected radio (target): the channel after the VFO has mixed it to DC and the channel filter has taken
//   everything outside the passband away, at the channel rate (about 200 kHz), as I/Q in the radio's I/Q file format (SET_RECORD_FORMAT: 32-bit float or 16-bit integer), in if_<date>_<time>_<rate>Hz.wav in the
//   recordings folder. The same in every mode, before the demodulator and the squelch. NO_CHANGE when it is so already, FAILED with the recorder's words; it ends with the radio and when the input rate
//   changes (the channel rate with it). fact.radio.<n>.ifrec reads 1 while it records. The file start blocks for the disk once.
#define FOXAPP_OP_IF_RECORD              0x8623u /* ival[0]: 1 start / 0 stop. Addresses one radio (target) */
//   PPM_MEASURE (SETTINGS; engine serial lane A, row R18, core/ppm_auto.hpp): measure the selected radio's (target) crystal error from the 19 kHz stereo pilot of the broadcast station it is tuned to. ival[0] 1 starts,
//   0 cancels; num[0] the time in seconds (10 to 300; 0 = 30). WFM only (UNSUPPORTED otherwise); NO_CHANGE when it is so already. It reads the pilot PLL every 100 ms, ignores the first two seconds and any reading
//   without a locked pilot of level 0.5 or more, and ends Done (20 or more readings) or Failed (fewer: no pilot). The number is a SUGGESTION: nothing is applied (SET_PPM applies a value). fact.radio.<n>.ppmauto.state is
//   idle / measuring / done / failed, .progress 0..1, .ppm (positive = the crystal runs fast, the sign of SET_PPM), .uncertainty (one standard error, statistical only: the station's own pilot accuracy is not known
//   here), .readings. A reading is only as good as the station: measure two or three and trust the ones that agree.
#define FOXAPP_OP_PPM_MEASURE            0x8624u /* ival[0]: 1 start / 0 cancel; num[0]: seconds (10..300, 0 = 30). Addresses one radio (target) */
//   SET_IF_ZOOM (SETTINGS; engine serial lane A, row S12): a second, high-resolution spectrum of the selected radio's (target) IF passband (dsp/zoom_fft.hpp). ival[0] the FFT size (0 off; a power of two from 256 to
//   16384); num[0] the width of the whole picture in Hz (100 up to the channel rate; the decimation is the channel rate over it, rounded, so applied[0] says the span actually shown). The frame is read in process with
//   Engine::zoomFrame(radio, ...): the engine C API still serves ONE spectrum frame per radio, so the zoom frame is not on the wire yet. fact.radio.<n>.zoom is off, or "<bins> bins of <binHz> Hz over <span> Hz".
//   Not saved with the radio: it is a view, set again by the screen that wants it. OUT_OF_RANGE for a size or width that is not allowed, NO_CHANGE when it is so already.
#define FOXAPP_OP_SET_IF_ZOOM            0x8625u /* ival[0]: FFT size or 0 off; num[0]: span Hz. Addresses one radio (target) */
#define FOXAPP_OP_SET_STEREO_SYSTEM      0x8619u /* ival[0]: 0 automatic / 1 pilot tone / 2 OIRT. Addresses one radio (target) */
// Plugins and the store.
#define FOXAPP_OP_USER_PRESET_APPLY      0x8701u /* text: plugin key; ival[0]: ordinal among that plugin's user presets */
#define FOXAPP_OP_USER_PRESET_FORGET_AT  0x8702u /* text: plugin key; ival[0]: ordinal among that plugin's user presets */
#define FOXAPP_OP_PLUGIN_AUTO_PRESET     0x8703u /* text: plugin key; ival[0]: 0 it was started, 1 its window was opened */
#define FOXAPP_OP_DECODER_STOP_LIST      0x8704u /* text (or long text): plugin keys, one per line; ival[0]: 1 = and end the mute they caused */
#define FOXAPP_OP_STORE_UPDATE           0x8705u /* text: catalogue id of an installed plugin to update */
#define FOXAPP_OP_STORE_REMOVE_BLOCKED   0x8706u /* text: file name of a plugin the catalogue policy blocked */
// HOW A FITTED PLUGIN RUNS (stub campaign B2, 0.99.73; docs/api-0.3-additions.md 9.3), both LOCAL sessions only.
//   PLUGIN_RUN_MODE (SETTINGS): text: the plugin's module file name (or its id); ival[0]: 0 AUTO (runs while something uses it), 1 ALWAYS
//   (pinned: "keep running"), 2 STOPPED. NOT_FOUND for a name no installed plugin carries and no run state records; OUT_OF_RANGE for any other
//   ival[0]. Pins or stops that ONE plugin's instances; nothing else is rebuilt.
//   PLUGIN_USE (VIEW): long text (or text), the window's whole table of what it shows, replacing the last: one line "<file>\t<bits>" per plugin
//   with at least one signal, the bits FOXAPP_PLUGIN_USE_*. Empty text clears the table. Sent on CHANGE only; the engine adds the signals it
//   knows itself (a patch node, the speakers, a text sink, a browser session, a standing duty).
#define FOXAPP_OP_PLUGIN_RUN_MODE        0x8707u /* text: plugin key; ival[0]: 0 auto, 1 always, 2 stopped */
#define FOXAPP_OP_PLUGIN_USE             0x8708u /* long text: "<file>\t<bits>" lines - what the window shows of each plugin; empty clears */
#define FOXAPP_PLUGIN_USE_WINDOW         1u /* one of the plugin's image / panel / instrument windows is shown */
#define FOXAPP_PLUGIN_USE_MAP_PAGE       2u /* the plugin's map page is open (a track source) */
#define FOXAPP_PLUGIN_USE_RADAR_SCOPE    4u /* the radar scope is showing (every track source) */
#define FOXAPP_PLUGIN_USE_DECODER_OUTPUT 8u /* the Decoder output window is open (a text decoder) */
#define FOXAPP_PLUGIN_USE_ALL            15u
// THE STORE'S SITES, PICTURES AND CLEAN-UP (stub campaign B3, 0.99.49 - 0.99.74; docs/api-0.3-additions.md 9.4), all LOCAL sessions only and
// none of them on the web profile: a REMOTE session can neither add a source of code nor start a transfer to an address a stranger chose.
//   STORE_SITE (ADMIN): text: the site's address as typed (it is normalised and re-checked here: PluginRepo::normaliseSiteUrl - https only, no
//   credentials, a host - and refused when it is already listed, is the official address, or the list is full); ival[0]: 0 ADD, 1 REMOVE;
//   ival[1] (ADD): 1 = the user ticked the box that says what a site is (the consent) - without it ADD is refused and nothing is written.
//   ADD starts a catalogue read. Neither runs under a transfer in flight (BUSY). The refusal's sentence is the result's message and
//   fact.store.siteError. REMOVE leaves what was installed from the site installed.
//   STORE_PICTURES_WANT (ADMIN): text: a catalogue KEY (the id, or "<id>@<site label>"); ival[0]: 1 = ask again for a picture that failed.
//   Queues the entry's pictures (one transfer at a time, after any read or download); NOT_FOUND for a key the catalogue does not hold.
//   STORE_CLEAN_OLD (ADMIN): ival[0] 0: remove EVERY old copy the last scan found superseded (the store's CLEAN UP key, once confirmed - the
//   window asks first; fact.store.oldCopies is what it lists); ival[0] 1: REMOVE FILE on the orphaned module named in text, which is judged
//   again against the engine's own scan. Not under a transfer (BUSY).
#define FOXAPP_OP_STORE_SITE             0x8709u /* text: the site's address; ival[0]: 0 add, 1 remove; ival[1] (add): 1 = consent given */
#define FOXAPP_OP_STORE_PICTURES_WANT    0x870Au /* text: catalogue key; ival[0]: 1 = retry a failed picture */
#define FOXAPP_OP_STORE_CLEAN_OLD        0x870Bu /* ival[0]: 0 remove every old copy, 1 remove the orphaned file named in text */
// A PLUGIN'S SETTINGS FORM, edited (stub campaign B9, 0.99.43; docs/api-0.3-additions.md 9.7), LOCAL sessions only and not on the web profile:
//   PLUGIN_SETTING_EDIT (SETTINGS): text (or long text): "<module file>\x1F<key>\x1F<value>" - the module's file name (PLUGINS' detail), the field's key
//   (PLUGIN_SETTINGS' spec fact) and the text the person typed or chose, exactly as typed (the value may be empty: a cleared box is a value, not a
//   deletion). The ENGINE judges it (core::commitSettingEdit: the field's own length and kind, UTF-8, no control character, a number that is one) and
//   writes it through PluginApiCore::settingsUiSet - the one path that advances the plugin's settings_seq, once for an accepted edit that changes the
//   value and never for a refused one. The ANSWER carries the verdict: OK for an accepted edit, otherwise a refusal whose applied[0] is the
//   core::SettingEditResult (1 TooLong, 2 NotANumber, 3 BadText, 4 StoreFull, 5 StoreError, 6 Refused) and whose message says it in English; NOT_FOUND
//   for a module that is not loaded or declares no such field. applied[0] is 0 for an accepted edit.
#define FOXAPP_OP_PLUGIN_SETTING_EDIT    0x870Cu /* text: "<module file>\x1F<key>\x1F<value>"; answered with the verdict in applied[0] */
// THE TRANSMIT GRANT (stub campaign B13; see FOXAPP_OP_TX_PLUGIN_ARM, 0x842B, for the whole of the safety argument). The third of a plugin's grants, and the ONLY
// one that is NOT a kind of FOXAPI_OP_PLUGIN_GRANT: that op is on the web profile, so the grant that lets a plugin key a radio has an op of its own, LOCAL sessions only,
// grant TRANSMIT, never on the web profile and refused to every REMOTE session. It is worth nothing alone: the plugin keys only while the operator's arming window is open too.
//   PLUGIN_TX_GRANT (TRANSMIT): text: the module's file name; ival[0]: 1 grant, 0 revoke. A grant is for a LOADED module that declares CASCADE_CAP_TX_AUDIO
//   (NOT_FOUND otherwise); a revoke is accepted for any name (a grant held for a module that has gone can still be taken back). The grant is saved
//   (AppConfig::pluginTransmitAllowed); the arming window is not, ever. A revoke unkeys, and closes the window when it was the last grant, in the same pass.
#define FOXAPP_OP_PLUGIN_TX_GRANT        0x870Du /* text: the module file; ival[0]: 1 grant / 0 revoke */
// RE-ENABLING A PLUGIN THE CONTAINMENT SWITCHED OFF (plugin containment P1, docs/engine-stage4.md 6.28), LOCAL sessions only and NOT on the web
// profile: a person's one click on the Fitted modules row (or the start notice). text: the plugin's module file name or its id. A plugin the crash-loop
// breaker skipped at this start (it was in use when the last run ended unexpectedly) or disabled by a second strike, or one the guard switched off
// this session after it faulted, threw or stopped answering, is allowed again and the plugins folder is scanned afresh. NOT_FOUND for a name no
// plugin carries; NO_CHANGE for a plugin that is not switched off; BAD_ARGUMENT for empty text. A REMOTE session is DENIED (three ways, as
// the transmit grant is): nothing a browser sends can switch a faulted plugin back on.
#define FOXAPP_OP_PLUGIN_REENABLE        0x870Eu /* text: the module file or id */
// A CLICK ON A PLUGIN'S PICTURE (the 0.99.100 merge, docs/api-0.3-additions.md 9.18; master 0.99.81 / 0.99.98), LOCAL sessions only and NOT on the web profile.
// text: the plugin's name as the picture carries it (HostImage::plugin); ival[0], ival[1]: the click in the picture's OWN pixels (x, y). The engine hands it to
// the plugin's image-input capability (CascadeImageInputApi::press) through the guard (PluginRunner::imagePress -> calls::press) - the window cannot call a plugin.
// BAD_ARGUMENT for empty text, OUT_OF_RANGE for a coordinate that is negative or past 32 bits; NO_CHANGE when nothing was delivered (no such picture, the plugin takes
// no input, its picture is inert, the click is outside the picture, or the plugin is switched off). A REMOTE session is DENIED.
#define FOXAPP_OP_IMAGE_PRESS            0x870Fu /* text: the plugin's name; ival[0]: x, ival[1]: y, in the picture's pixels */
// wire-b begin: MEASUREMENT AND RADIO-SOURCE WIRING (docs/multi-radio-records/wire-b.md; block 0x8900-0x89FF, ascending, this lane's alone). All LOCAL sessions only
// (application ops are refused to a REMOTE session). A read operation answers in a LOCAL fact (the record names each) and never writes to disk.
//   FIELD_TABLE (SETTINGS; core/field_strength.hpp): text (or long text): a user-supplied CSV table "<frequency>,<dB>" per line (the model's rules: strictly increasing
//   frequency, 65536 points, 1 MiB); ival[0]: 0 the ANTENNA FACTOR table (dB per metre), 1 the CABLE LOSS table (dB); ival[1]: the frequency unit of the file, 0 Hz,
//   1 kHz, 2 MHz (the default), 3 GHz. Empty text clears the table. BAD_ARGUMENT with the model's words and the line for a table it refuses (the old table stays);
//   OUT_OF_RANGE for another table or unit; NO_CHANGE when clearing an empty table. applied[0]: the points now held.
//   FIELD_CONFIG (SETTINGS): num[0]: the preamp gain in dB (-100..100), num[1]: the input impedance in ohm (1..1000); ival[0]: 1 show the field strength, 0 hide it.
//   NO_CHANGE when all three are what they were. Not saved.
//   FIELD_READ (SETTINGS; addresses one radio (target)): reads the radio's level now (signalDbm: calibrated, or the dBFS standing in as an estimate) at its tuned frequency through
//   the tables. FAILED with a sentence when no antenna-factor table is loaded. applied[0]: dBuV/m; applied[1]: bits 1 the antenna factor was HELD from the table's end, 2 the cable
//   loss was held, 4 the level is NOT calibrated (an estimate). The fact radio.<n>.field.readout holds the text "<x> dBuV/m at <f> MHz" and the held-value note.
#define FOXAPP_OP_FIELD_TABLE            0x8900u /* long text: the CSV table; ival[0]: 0 antenna factor / 1 cable loss; ival[1]: unit 0 Hz / 1 kHz / 2 MHz / 3 GHz */
#define FOXAPP_OP_FIELD_CONFIG           0x8901u /* num[0]: preamp gain dB; num[1]: input ohm; ival[0]: 1 show / 0 hide */
#define FOXAPP_OP_FIELD_READ             0x8902u /* addresses one radio (target); applied[0]: dBuV/m, applied[1]: the flag bits */
//   SIGNAL MEASUREMENTS (SETTINGS; dsp/sigmeas_*.hpp through engine/wire_b_sigmeas.hpp): nine READ operations, one per measurement block, each addressing one radio (target).
//   text (or long text): ONE JSON object of parameters, every key optional, an unknown key or a value outside its range is BAD_ARGUMENT naming the key (nothing is measured).
//   The span is read from the radio's channel taps: the channel I/Q after the VFO at the channel rate (the audio block reads the finished 48 kHz audio) - at most the newest 1.3 s
//   (the tap's size). "duration_s" (default 0.25, 0.001 to 10) is the length and "end_ago_s" (default 0, 0 to 60) how long before the newest sample the span ENDS; a span the tap
//   does not hold is cut to what it holds. Frequencies in every answer are Hz from the CHANNEL CENTRE (the tuned frequency). FAILED when the radio is not running or its tap is empty.
//   The answer is the LOCAL fact radio.<n>.sigmeas.<name> (one JSON object: op, radio, tuned_hz, rate_hz, samples, valid, reason and "result"); the command result says OK, with
//   applied[0] 1 when the block measured something and 0 when it could not (message: the block's one-phrase reason: "span too short", "no signal found" ...), applied[1] the samples used.
//   The keys of each block are in docs/multi-radio-records/wire-b.md (SCREEN HAND-OFF). Nothing is written to disk; nothing is saved.
#define FOXAPP_OP_SIGMEAS_BANDWIDTH      0x8903u /* occupied and x-dB bandwidth: fact radio.<n>.sigmeas.bandwidth */
#define FOXAPP_OP_SIGMEAS_MODULATION     0x8904u /* FM deviation and AM depth: fact ...sigmeas.modulation */
#define FOXAPP_OP_SIGMEAS_SYMBOLS        0x8905u /* "mode": fsk / ook / cursor (symbol period from two cursors): fact ...sigmeas.symbols */
#define FOXAPP_OP_SIGMEAS_VSTIME         0x8906u /* amplitude, frequency and phase against time: fact ...sigmeas.vstime */
#define FOXAPP_OP_SIGMEAS_BURSTS         0x8907u /* burst list and repeat period: fact ...sigmeas.bursts */
#define FOXAPP_OP_SIGMEAS_HOPCHIRP       0x8908u /* frequency hopping and chirp tracking: fact ...sigmeas.hopchirp */
#define FOXAPP_OP_SIGMEAS_AUDIO          0x8909u /* SINAD, THD, SFDR, ENOB of the finished audio: fact ...sigmeas.audio */
#define FOXAPP_OP_SIGMEAS_DRIFT          0x890Au /* drift search (de-chirp): fact ...sigmeas.drift */
#define FOXAPP_OP_SIGMEAS_HISTOGRAM      0x890Bu /* amplitude / magnitude histogram and the power CCDF: fact ...sigmeas.histogram */
//   PSEUDO_DOPPLER (SETTINGS; dsp/pseudo_doppler.hpp, the rfmon lane bearing from one receiver and a switched four-antenna array): the same span rules as the measurements above (one radio,
//   JSON text, the channel I/Q); keys "samples_per_dwell" (8 to 1e6, default 100) and "start_sample" (0 to 1e9, default 0: where antenna 0 first dwell begins in the span), plus
//   "duration_s" / "end_ago_s". It ESTIMATES from I/Q already in the span: the antenna switch is hardware this program does not drive. Answer in the fact radio.<n>.sigmeas.pseudodoppler:
//   bearing_deg (counter-clockwise from antenna 0 towards the transmitter), quality 0 to 1, weak (quality under 0.5) and the sentence "Signal too weak or not steady for a bearing".
#define FOXAPP_OP_PSEUDO_DOPPLER         0x890Cu /* addresses one radio (target): fact radio.<n>.sigmeas.pseudodoppler */
//   THE BIT VIEW (SETTINGS; core/bits/*.hpp through engine/wire_b_bits.hpp), eleven operations, each addressing one radio (target); LOCAL sessions only; nothing is written to disk. KEEP slices the
//   newest span of the radio channel I/Q on demand (no tap in the receive path) into the radio bounded ring of level runs (256 slices); the other operations work on the kept slices and on the frames
//   of the last REDECODE. text is ONE JSON object of parameters (unknown key = BAD_ARGUMENT naming it) except SPEC_CHECK / SPEC_RUN (text = the decoder file, "foxsdr-bitspec 1") and
//   TABLE_CHECK (text = the code-table file). The answer is the LOCAL fact radio.<n>.bits.<name> (one JSON object: ok / valid / reason and the result); the command is OK with applied[0] 1 when
//   there is a result and 0 when there is none (message: the reason). KEEP: duration_s, end_ago_s, label, smooth_us, threshold, min_span_ratio; applied[1] is the new slice id (0 when none).
//   REDECODE / SWEEP: slice (id, 0 = the newest), line_codes (one to three words), invert, balance, chip_us, phase, sync, sync_errors, both_polarities, framing, fixed_bits, flag, unstuff, whiten,
//   max_frames, max_frame_bits. FIND_CRC / FIND_SUM: max_head_bytes, max_tail_bytes, max_hits, widths. DIFF: a, b (frame numbers from 1). SPEC_RUN: ival[0] the slice (0 = every kept slice).
#define FOXAPP_OP_BITS_KEEP              0x890Du /* addresses one radio (target): fact radio.<n>.bits.keep */
#define FOXAPP_OP_BITS_LIST              0x890Eu /* fact ...bits.list */
#define FOXAPP_OP_BITS_REDECODE          0x890Fu /* fact ...bits.redecode (the frames are kept for FIND_CRC, FIND_SUM, INFER and DIFF) */
#define FOXAPP_OP_BITS_SWEEP             0x8910u /* every line code and polarity on one slice, ranked: fact ...bits.sweep */
#define FOXAPP_OP_BITS_FIND_CRC          0x8911u /* fact ...bits.findcrc */
#define FOXAPP_OP_BITS_FIND_SUM          0x8912u /* fact ...bits.findsum */
#define FOXAPP_OP_BITS_INFER             0x8913u /* field inference over the decoded frames: fact ...bits.infer */
#define FOXAPP_OP_BITS_DIFF              0x8914u /* two decoded frames aligned: fact ...bits.diff */
#define FOXAPP_OP_BITS_SPEC_CHECK        0x8915u /* long text: a decoder file; errors with line numbers: fact ...bits.speccheck */
#define FOXAPP_OP_BITS_SPEC_RUN          0x8916u /* long text: a decoder file; ival[0]: slice id (0 all): fact ...bits.specrun */
#define FOXAPP_OP_BITS_TABLE_CHECK       0x8917u /* long text: a code-table file; errors with line numbers: fact ...bits.tablecheck */
//   GPU_FFT_MODE (SETTINGS; core/pipeline_gpufft.cpp, dsp/gpu_fft.hpp): ival[0]: 0 CPU (the DEFAULT at every start: no Direct3D device is ever made), 1 AUTO (the GPU only from 65 536 points; the legacy spectrum
//   path of the pipeline is smaller, so Auto stays on the CPU estimator there), 2 GPU (always). Applies to every radio OPEN NOW (a radio added later starts on CPU) and is NOT saved. OUT_OF_RANGE for another
//   value, NO_CHANGE when every radio is on that mode. A failure of the GPU path is never an error: the CPU path runs on, silently, and the facts say what runs. The device is made off the DSP thread.
//   Facts (LOCAL): radio.<n>.gpufft = one JSON object {mode, engaged, backend ("cpu"/"gpu"), submitted, completed_gpu, completed_cpu, dropped_frames, device_lost, error}; gpu_fft.mode = the last mode asked for.
//   Not reached by this operation: the spectrum BLOCK (SET_SPECTRUM, the large FFT sizes) - see the record.
#define FOXAPP_OP_GPU_FFT_MODE           0x8918u /* ival[0]: 0 CPU / 1 Auto / 2 GPU; every open radio; facts radio.<n>.gpufft, gpu_fft.mode */
//   THE EIGHT CURSORS AND THE FRAME LOG (SETTINGS; dsp/sigmeas_cursors.hpp, dsp/sigmeas_framelog.hpp through engine/wire_b_cursors.hpp), LOCAL sessions only, nothing written to disk. The engine keeps one
//   CursorSet8 per radio (mode free / locked / harmonic, spacing, reference, eight frequencies from the CHANNEL CENTRE; not saved). CURSOR_SET (text: JSON; keys mode, spacing_hz, reference 1-8,
//   range_low_hz + range_high_hz, set: [{"cursor": 1-8, "freq_hz": n}]) changes a copy and keeps it only when every part was accepted (else BAD_ARGUMENT naming the key; nothing changes).
//   CURSOR_READ (JSON: duration_s, end_ago_s, search_hz, snap) reads the level under every cursor from the Welch spectrum of the newest span of the radio's channel I/Q and, with snap and in free mode, moves
//   the cursors onto the peaks. CURSOR_MARKS (JSON: kind harmonic / sideband, base_hz or carrier_hz, spacing_hz, count, low_hz, high_hz, search_hz) lists harmonic or sideband marks with their levels.
//   Facts radio.<n>.sigmeas.cursors (SET and READ) and radio.<n>.sigmeas.marks. FRAMELOG_CONFIG (ival[0]: -1 keep, 0 drop / 1 keep frames that FAILED their CRC; ival[1]: capacity 1..4096, 0 keep) and
//   FRAMELOG_READ (ival[0]: 1 include the failed frames) work on the engine's one FRAME LOG: frames kept IN MEMORY ONLY with their CRC flag (no plugin call fills it yet: an ABI request); the fact framelog
//   holds the frames as hex only when READ asks, LOCAL only.
#define FOXAPP_OP_CURSOR_SET             0x8919u /* addresses one radio (target): text JSON; fact radio.<n>.sigmeas.cursors */
#define FOXAPP_OP_CURSOR_READ            0x891Au /* addresses one radio (target): text JSON; fact radio.<n>.sigmeas.cursors */
#define FOXAPP_OP_CURSOR_MARKS           0x891Bu /* addresses one radio (target): text JSON; fact radio.<n>.sigmeas.marks */
#define FOXAPP_OP_FRAMELOG_CONFIG        0x891Cu /* ival[0]: -1 keep / 0 / 1 keep CRC-failed frames; ival[1]: capacity 1..4096 (0 keep) */
#define FOXAPP_OP_FRAMELOG_READ          0x891Du /* ival[0]: 1 include CRC-failed frames; fact framelog */
//   THE LIMIT MASK (SETTINGS; core/spectrum_mask.hpp, the rfmon lane limit lines with pass / fail, alarm and capture on breach), LOCAL sessions only. MASK_LOAD (text, long text: a "foxmask 1" file of
//   upper / lower segments in absolute Hz and dB; empty text removes the mask) - BAD_ARGUMENT with the model's words and the LINE for a file it refuses (the old mask stays), NO_CHANGE for removing a
//   mask that is not there. MASK_CONFIG (text: one JSON object, every key optional: enabled, raise_frames 1-100 (default 3), clear_frames 1-100 (5), capture (record I/Q on a breach, default false),
//   post_roll_s 0-60 (2), min_interval_s 0-3600 (10); unknown key = BAD_ARGUMENT; FAILED when enabled is asked with no mask loaded). While enabled, once per control pass and for every open radio the engine
//   evaluates the radio newest spectrum frame (absolute frequencies from its centre and sample rate) against the mask: pass / worst margin, an alarm raised after raise_frames failing frames in a
//   row and cleared after clear_frames passing ones, a log line per event (kind, margin, frequency, frames: no identifier). With capture on, the alarm starts the radio normal I/Q recording (in
//   the recordings folder) unless it is recording already or the minimum interval has not passed, and stops it post_roll_s later. THERE IS NO I/Q PRE-ROLL RING: the pre-roll of the model is 0.
//   Fact radio.<n>.mask = {enabled, loaded, pass, worst_margin_db, worst_freq_hz, alarm, events [...], captures, capture_note}. Not saved.
#define FOXAPP_OP_MASK_LOAD              0x891Eu /* long text: the foxmask file; empty removes it; fact radio.<n>.mask */
#define FOXAPP_OP_MASK_CONFIG            0x891Fu /* text: JSON {enabled, raise_frames, clear_frames, capture, post_roll_s, min_interval_s} */
//   THE SWEEP DEVICE (SETTINGS; engine/wire_b_sweep.hpp over source/tinysa_protocol.hpp, core/sweep_stitch.hpp, core/sweep_occupancy.hpp), LOCAL sessions only: a tinySA / tinySA Ultra on a serial
//   port as a SOURCE OF SWEEP LINES (not I/Q: it is not a radio of the engine). OFF until SWEEP_OPEN. SWEEP_OPEN (text: JSON {kind "tinysa" | "tinysa_ultra", port "COM1".."COM256", start_mhz, stop_mhz,
//   points 2..1048576 (default 450), interval_ms 0..60000 (default 0)}): BAD_ARGUMENT for a key or value that is wrong (an invalid port name never reaches the system), BUSY when a device is open already,
//   FAILED with a sentence when the port cannot be opened. One worker thread asks for a sweep over and over and feeds every line to a stitched heatmap and, when configured, an occupancy model; a device
//   that stops answering ends the worker with a sentence (the port name is never put in a fact, a log or a report). SWEEP_CLOSE: NO_CHANGE when nothing is open. SWEEP_CONFIG (JSON: "heatmap":
//   {cols 1..65536, rows 2..65536, freq_mode "max"|"mean", time_mode}, "occupancy": {start_mhz, stop_mhz, step_khz, margin_db 0..200, floor_percentile 0..0.5, fixed_floor_db} or null to drop it): works while
//   open or not; the model's refusal words come back. SWEEP_READ (JSON: max_points 2..4096 default 512, x 0..1 for the click-to-tune frequency, occupancy true for the channel table as CSV): answers in
//   the fact sweep.read. Fact sweep.state = {open, kind, sweeps, refused, last_error, points, start_hz, stop_hz, heat_rows, heat_cols, occ_channels, occ_sweeps}. UNVERIFIED on a real device.
//   (KIND "hackrf": a HackRF in sweep mode, RECEIVE ONLY: JSON {kind "hackrf", serial (optional), start_mhz, stop_mhz (ONE range of whole MHz, 1..6000), step_mhz 1..20 (10), lna_db 0..40 (16), vga_db 0..62 (16), amp bool,
//   sample_rate_msps 2..20 (20), fft_size 32..65536 (1024)}; a "port" is refused; the LINEAR sweep only: the interleaved style needs GPL code, the owner's decision.)
#define FOXAPP_OP_SWEEP_OPEN             0x8920u /* text JSON; fact sweep.state */
#define FOXAPP_OP_SWEEP_CLOSE            0x8921u
#define FOXAPP_OP_SWEEP_CONFIG           0x8922u /* text JSON */
#define FOXAPP_OP_SWEEP_READ             0x8923u /* text JSON; fact sweep.read */
//   NETWORK I/Q OUT (SETTINGS; net/iq_out.hpp, a tap on the radio RAW BAND at the input rate where the I/Q recorder is fed), per radio (target), LOCAL sessions only. OFF until IQ_OUT_START. IQ_OUT_START
//   (text: JSON {format "u8" (default) | "s16" | "f32", header bool (the 28-byte FXIQ header, default false), udp bool (default false: TCP), port 0..65535 (0 = the OS picks; UDP needs 1..65535),
//   dest "127.0.0.1" (UDP; nothing else is accepted)}): the TCP server binds 127.0.0.1 ONLY (a wider bind raises the firewall prompt and is the owner's decision, so the operation has no key for it);
//   BAD_ARGUMENT for a wrong key or value, BUSY when this radio sends already, FAILED with the model's sentence when the socket cannot be made. applied[0]: the TCP port. IQ_OUT_STOP: NO_CHANGE
//   when this radio does not send. The producer is never slowed: a slow client drops samples and the facts count them. Fact radio.<n>.iqout = {running, udp, port, format, header, rate_hz,
//   samples_in, samples_dropped, clients_accepted}. A radio that is removed stops its output; nothing is saved: after a restart every output is off.
#define FOXAPP_OP_IQ_OUT_START           0x8924u /* addresses one radio (target): text JSON */
#define FOXAPP_OP_IQ_OUT_STOP            0x8925u /* addresses one radio (target) */
//   RADIOS THAT FOLLOW A MASTER (SETTINGS; core/radio_matrix_model.hpp FollowSet, SDRuno's follow / LO LOCK), LOCAL sessions only, not saved. A follower keeps the distance it had when it was linked: when
//   the master's tuned frequency changes, the engine retunes the follower (and what follows it) to master + offset on its next control pass, through the ordinary SET_FREQUENCY, so a follower that is
//   frequency-locked or cannot reach the frequency stays put and the fact says so. FOLLOW_SET: ival[0] the FOLLOWER radio 1..8, ival[1] its MASTER 1..8; applied[0]: the offset in Hz recorded now (follower
//   minus master). NO_DEVICE for a radio that is not open, BAD_ARGUMENT for the same radio twice or a link that would make a cycle, BUSY when the follower follows another master already (FOLLOW_CLEAR first).
//   FOLLOW_CLEAR: ival[0] the follower (0 = every link); NO_CHANGE when there is none. FOLLOW_LOCK: ival[0] the follower, ival[1] 1 lock (it stays while its master moves) / 0 unlock; NO_CHANGE when it is so
//   already or the radio follows nothing. FOLLOW_KEEP: ival[0] the follower: the distance it has NOW (it was nudged) becomes its offset. Facts: matrix.rows = a JSON array with one row per open radio
//   {radio, state, tuned_hz, mode, level_dbfs, muted, follows (0 none), locked, offset_hz, follow_note}; a radio that is closed or removed leaves every link it was in.
#define FOXAPP_OP_FOLLOW_SET             0x8926u /* ival[0]: follower 1..8; ival[1]: master 1..8; applied[0]: the offset */
#define FOXAPP_OP_FOLLOW_CLEAR           0x8927u /* ival[0]: follower (0 = all) */
#define FOXAPP_OP_FOLLOW_LOCK            0x8928u /* ival[0]: follower; ival[1]: 1 lock / 0 unlock */
#define FOXAPP_OP_FOLLOW_KEEP            0x8929u /* ival[0]: follower: the distance it has now is its offset */
//   SBS (BASESTATION) TEXT OUT (SETTINGS; net/aircraft_out.hpp SbsOut): the aircraft tracks the plugins publish (CASCADE_TRACK_AIRCRAFT with a six-digit ICAO hex id and a position) as MSG,3 (airborne
//   position: altitude in feet, latitude, longitude) and MSG,4 (velocity: ground speed in knots, track) lines on a TCP port, for the map programs that read this format. OFF until SBS_OUT_START
//   (text: JSON {port 0..65535 (0 = the OS picks; the port is in applied[0])}); the server binds 127.0.0.1 ONLY (no key for a wider bind: the owner's decision), LOCAL sessions only, not saved. A line is
//   sent when a track's position, altitude, speed or course changed (to about a metre); no callsign (MSG,1), squawk or any label is ever sent, and nothing is written to disk. BAD_ARGUMENT for a wrong
//   key or value, BUSY when it is on, FAILED with the socket's sentence. SBS_OUT_STOP: NO_CHANGE when off. Fact sbs.out = {running, port, lines_sent, tracks_seen, clients_accepted}. The tracks come
//   from what the front end hands the engine (the window's plugin poll): a headless engine with no window has none, so no line is sent there.
#define FOXAPP_OP_SBS_OUT_START          0x892Au /* text JSON; applied[0]: the port; fact sbs.out */
#define FOXAPP_OP_SBS_OUT_STOP           0x892Bu
//   THE TRACK STORE (SETTINGS; core/track_store.hpp, positions kept for a chosen time and asked "as at T" with interpolation, for a replay), LOCAL sessions only. IN MEMORY ONLY: nothing is written to
//   disk (the model's save / load are not wired: the positions of other people's aircraft and ships on disk is the owner's decision), OFF until enabled, not saved: every restart starts empty and off.
//   TRACKS_CONFIG (text: JSON {enabled bool, retention_s 60..2592000 (default 21600, six hours), clear bool (drops every point)}; unknown key = BAD_ARGUMENT; NO_CHANGE when nothing differs). While enabled, once per
//   control pass the engine adds to the store every track the front end hands it (the window's plugin poll: aircraft, vessels and stations with a position and an id of 1 to 24 of letters, digits and _ . / -),
//   at the time the plugin last heard it (now minus ageMs), when its position changed (about a metre). A headless engine has no front end, so it stores nothing. TRACKS_READ (text: JSON {t unix seconds
//   (default now), kind "aircraft" | "vessel" | "station" (default all), max 1..2000 (default 500)} for the picture at T; or {trail: {id, kind, t0, t1, max_points 2..2000}} for one track; or {range: true}
//   for the first and last time held): the answer is the LOCAL fact tracks.read. Fact tracks.state = {enabled, retention_s, added, replaced, refused, too_old, evicted, tracks}.
#define FOXAPP_OP_TRACKS_CONFIG          0x892Cu /* text JSON; fact tracks.state */
#define FOXAPP_OP_TRACKS_READ            0x892Du /* text JSON; fact tracks.read */
//   THE ADS-B COVERAGE GRID (SETTINGS; core/adsb_coverage.hpp: a heat map of the positions heard with an ageing weight and, per altitude band, the greatest distance heard in each bearing in the last 24 hours), LOCAL
//   sessions only. IN MEMORY ONLY (the model's save / load are not called), OFF until enabled, not saved; disabling drops everything. COVERAGE_CONFIG (text: JSON {enabled bool, rx_lat -90..90, rx_lon -180..180
//   (the receiver's position: GIVEN BY THE CALLER, never read from the GPS or the location of the machine; needed to enable), cell_deg 0.001..1 (0.05), half_life_s 3600..31536000 (3 days), clear bool}): while
//   enabled, once per control pass every aircraft track the front end hands the engine with a position is added (altitude in feet from altM; NaN = the unknown band) when it moved. COVERAGE_READ (JSON:
//   band -1..6 (-1 every band), max_cells 1..20000 (default 2000), min_weight (0.05)): the heat cells (centre, weight) and the range outline (kilometres per bearing bin) in the fact coverage.read; the
//   receiver position is NOT in the answer. Fact coverage.state = {enabled, cells, added, bad_input, too_far, stale, future, max_range_km}.
#define FOXAPP_OP_COVERAGE_CONFIG        0x892Eu /* text JSON; fact coverage.state */
#define FOXAPP_OP_COVERAGE_READ          0x892Fu /* text JSON; fact coverage.read */
// wire-b end
// fact.plugin.<module file>.tx: ONE DECIMAL NUMBER, the bits of what the plate says about a module that declares CASCADE_CAP_TX_AUDIO (core::PluginRunner::txBits plus the two the
// engine adds). The window turns them into the plate's sentence (gui::txPlateState); the engine never words it.
#define FOXAPP_TXBIT_DECLARED  1u   /* the runner has a transmit-audio instance of it */
#define FOXAPP_TXBIT_ASKED     2u   /* its active() has been non-zero at least once this session: the GRANT TRANSMIT key is offered */
#define FOXAPP_TXBIT_HOLDER    4u   /* it holds the transmitter's audio now */
#define FOXAPP_TXBIT_WAITING   8u   /* granted and wanting the key, but another plugin holds it */
#define FOXAPP_TXBIT_STOPPED   16u  /* its pull() reported a permanent error: stopped for good */
#define FOXAPP_TXBIT_GRANTED   32u  /* it holds the TRANSMIT grant */
#define FOXAPP_TXBIT_ON_AIR    64u  /* the transmitter is keyed and this is the plugin it is keyed for */
// The Transmit page's open flag (engine/stage3b-pre 2b, docs/engine-stage3.md
// OPEN 1/10, SAFETY): ival[0] 0/1. Since engine stage 4 the flag gates NO key:
// the consent the window withdraws when its page closes (gui::txKeyCommands) is
// what releases a remote key held. This op only moves the flag, at once, for
// the normal UI path (the Transmit toolbar switch, the page's own close button).
#define FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN 0x8408u /* ival[0]: 0/1 - closing releases a remote key held, at once */
// A typed frequency the Source panel could not parse (engine/stage3b-pre 2c,
// docs/engine-stage3.md OPEN 1): text, or empty to CLEAR the line - the
// window's own validation, never a radio's answer (every radio-side error
// already reaches sourceError_ from inside the Engine).
#define FOXAPP_OP_SET_SOURCE_ERROR       0x840Au /* text: the red line under the Source controls; empty clears it */
// The Bookmarks panel's own note (an export's result - an import's note is
// set by the Engine itself, from FOXAPI_OP_BOOKMARK_IMPORT).
#define FOXAPP_OP_SET_BOOKMARK_NOTE      0x840Bu /* text: replaces bookmarkImportNote_ */
// A GROUP OF ONE-WAY "FORGET THIS" LINES (engine/stage3b-pre 2c): each
// resets exactly one status line/flag the window used to clear or consume in
// place once its purpose was served, back to its default - never anything a
// radio, the GPS reader, a scan or a decoder might still need to report a
// NEW instance of a moment later (this op only ever narrows what is shown,
// never widens it, which is why one enum-selected op can cover all of them
// safely). See cascade::core::cmd::FoxAppStatus below for the ival[0] values.
#define FOXAPP_OP_CLEAR_STATUS           0x840Cu /* ival[0]: FoxAppStatus - which line/flag to reset to its default */
// Fields-to-commands round 2 (engine/stage3b-pre, docs/engine-stage3.md OPEN
// item 1): a window-side DRAFT buffer, committed by a command when the
// operator is done editing - never the engine field itself, keystroke by
// keystroke.
#define FOXAPP_OP_SET_CATALOGUE_URL      0x840Du /* text: pluginCatalogueUrl_, committed on deactivate-after-edit */
// plutoUri_ and transmitArgs_: the window edits a LOCAL draft buffer (never
// the engine field) and, on "Open", sends the draft's text as
// FOXAPI_OP_SELECT_SOURCE "open-pluto:uri=..." / FOXAPI_OP_TX_OPEN, whose
// handlers persist the field from that same text. Round 3 added the two ops
// below so each edit is ALSO committed as it is typed: before the drafts, an
// address typed and never opened was still what config.json kept, and it is
// again.
// The scanner form needs NO new op at all: scanStartMhz_/scanStopMhz_/
// scanStepKhz_ commit through the EXISTING FOXAPP_OP_SCANNER_RANGE (bits
// 1/2/4, num[0..2] in Hz - the same op the web remote already uses; bit 8
// folds in the panel's own reconfigure-if-running check), and
// scanDwellMs_/scanHoldMs_/scanResumeMs_/scanListenMs_ commit through the
// EXISTING FOXAPI_OP_SCANNER_CONFIG (num[0..3], which already
// reconfigures-if-running on its own - found only after a from-scratch
// FOXAPP_OP_SCANNER_TIMING briefly duplicated it byte for byte).
#define FOXAPP_OP_SOUND_CARD_FORM        0x8412u /* text: device '\x1F' hostApi; ival[0]: 1 IqStereo | 2 channel right | 4 swapIq | 8 pickedFromList; num[0]: cardRateHz; num[1]: iqCentreHz; ival[1]: the FIELD MASK, FOXAPP_SOUND_CARD_FIELD_* (0 = the whole form) (gui::soundCardFormCommand / soundCardFormFromCommand, core/soundcard_panel.hpp) */
#define FOXAPP_OP_PATCH_LOOK_FOR_RADIOS  0x8413u /* no args: "Look for radios" pressed in the patch radio inspector */
// "Stop and resume sound" needs no new op: FOXAPP_OP_DECODER_STOP_LIST
// already covers it, unchanged - the mute lifts on its own once the engine's
// own mute state machine (advanceMutePopup) sees the decoder actually stop.
#define FOXAPP_OP_MUTE_KEEP_RUNNING      0x8414u /* no args: "Keep it running" pressed - sets muteKeptRunning_ and closes the subject together, one decision */
// The Airspy R2/Mini panel (0.99.41, engine/stage3b-pre Airspy round, OPEN
// 2/3): drawAirspyControls used to call Engine::chooseAirspyDecimation/
// GainMode/Agc directly (kControlMayCall) and read the open device through
// Engine::asAirspyDevice()'s raw pointer, every frame. Now: commands, queued
// like every other gain control in the Source section (the figure is read
// back from the published state on the next frame the readback lands on -
// see drawSourceSection's own gain sliders), and the panel reads its display
// state from PublishedState::app's airspy* fields (receiver_snapshot.hpp)
// instead of the pointer.
#define FOXAPP_OP_AIRSPY_DECIMATION      0x8415u /* ival[0]: the decimation factor (1/2/4/8/16/32/64) */
#define FOXAPP_OP_AIRSPY_GAIN_MODE       0x8416u /* ival[0]: cascade::source::AirspySource::GainMode */
#define FOXAPP_OP_AIRSPY_AGC             0x8417u /* ival[0]: 0 LNA, 1 Mixer; ival[1]: 0/1 on */
// Engine round 3 fix (docs/review-harness/engine-round3-review.md, 4 and 7).
#define FOXAPP_OP_SET_PLUTO_URI          0x8418u /* text: the Pluto address box, committed as typed (plutoUri_) */
#define FOXAPP_OP_SET_TRANSMIT_ARGS      0x8419u /* text: the Transmit address box, committed as typed (transmitArgs_); refused while a board is open */
// THE DESKTOP'S OWN USAGE-REPORTING SWITCH. FOXAPI_OP_TELEMETRY_ENABLE is the
// API's slot for the same thing, and applyCommand REFUSES it (FOXAPI_DENIED)
// whoever sends it: reporting is consent given at this machine, and a plugin,
// a browser or a future API session must never be able to switch it on
// silently. An extension op is never sent across a transport (see the top of
// this file), so this one is reachable only from the desktop's checkbox.
#define FOXAPP_OP_TELEMETRY_CONSENT      0x841Au /* ival[0]: 0/1. On mints an install id only if reporting was off; off forgets it */
// THE PATCH GRAPH AS ONE COMMAND (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 6, Design A). The Patch page edits a draft and sends the whole of it;
// the Engine replaces patchGraph_ with it, or refuses it and keeps the graph
// it had. The text is core::patch::graphCommandText's (core/patch_draft.hpp):
// the config's own patch document, ids kept, plus next-id and centre-chosen
// lines the document parser already skips. Always long text in practice.
#define FOXAPP_OP_PATCH_SET_GRAPH        0x841Bu /* text (long text): the graph; ival[0]: FOXAPP_PATCH_GRAPH_* flags */
// 0x841C was FOXAPP_OP_WEB_CONTROL_STOPPED (engine/stage3b-pre, docs/engine-stage3.md OPEN 7 (c)/10): "the server that carries a browser's
// transmit key has been stopped, disabled or never started", sent by the window and, since the pump move, by the pass's standing condition.
// RETIRED in stage 5d (docs/engine-stage4.md 6.12): the web server's engine session closes when the server stops, and a closed session's key
// is opened by the key rules at the next pass. The number is not reused.
// THE SOUND CARD LIST IS WANTED (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 2): the Source section's sound card row is showing. The Engine lists
// the cards on its worker unless they are listed already, a list is already
// being taken, or a card is opening - the test the panel used to make itself
// before calling Engine::scanSoundCards directly. Sent every frame the row is
// shown unlisted; idempotent.
#define FOXAPP_OP_SOUND_CARDS_WANTED     0x841Du /* no args */
// THE STORE WINDOW'S ADD ALL KEY (engine/stage3b-pre, docs/engine-stage3.md
// OPEN 5): the ENGINE plans the run from its own catalogue and inventory; the
// command carries only what the engine cannot know - ival[0], the ADD ALL
// tick (every maker's notice acknowledged), and text, the catalogue id of
// the ONE module whose own notice tick is on (the store's selected row), or
// empty. FOXAPI_OP_STORE_UPDATE_ALL is the same with no per-module tick.
#define FOXAPP_OP_STORE_ADD_ALL          0x841Eu /* ival[0]: 0/1 every notice acknowledged; text: the one module id acknowledged by its own tick */
// A PANEL WAS OPENED (engine stage 4, window port group 3): the anonymous
// usage report lists which of the window's pages were used this run, and the
// window used to hand each page's name to the engine from its own frame, every
// frame the page was open. The page now SAYS it once, as this op: text, the
// panel's name. The Engine records it once (a name already listed is not
// listed twice) and stops at 16 names, exactly as the direct call did. An
// empty name is BAD_ARGUMENT. LOCAL sessions only (every app op is): a remote
// session's pages are not this installation's usage. Grant VIEW: it reads and
// changes nothing but that list. Not one of the "safer" commands, so never
// merged - a repeat is answered by the engine's own de-dupe, and the window
// sends each name once a run (AppWindow::uiNotePanel).
#define FOXAPP_OP_NOTE_PANEL             0x841Fu /* text: the panel's name, as the usage report lists it ("radar", "map") */
// A SOUND CARD CHOSEN FROM THE LIST (engine stage 4, window port group 11, API 0.3 8.10): the Source section's
// Device combo sends the entry it was clicked on, and the ENGINE does what the combo's own handler did - the card's
// name and host API into the form (marked as chosen from the list), the "not connected" line cleared, and the rate
// kept if the new card offers it in the form's format, else its default, else its highest. ival[0]: the
// SOUND_CARDS entry's id (index + 1); NOT_FOUND for none, and before the cards are listed. Grant SETTINGS, LOCAL
// sessions only (every app op is).
#define FOXAPP_OP_SOUND_CARD_PICK        0x8420u /* ival[0]: the SOUND_CARDS entry's id */
#define FOXAPP_OP_SET_PATCH_PAGE_OPEN    0x8421u /* ival[0]: 0/1 - the standard window's patch view is showing (API 0.3 8.12); the engine's pass runs the patch while it is, and stops it when it goes (default: showing) */
// THE PATCH RADIO'S RECORDINGS LISTED (API 0.3 8.12): the engine reads the I/Q recordings of the record folder and of
// FOXSDR_PATCH_SAMPLES (at most kMaxRecordingOpens headers opened, a file already read is not opened again), on its own
// pass, and PATCH_DEVICES (the recordings' rows, with their rates) and RECORDINGS kind 3 change as the answer. The Radio
// part's device list asks when it opens and "Look for radios" asks again. No arguments. Grant SETTINGS, LOCAL sessions only
// (every app op is).
#define FOXAPP_OP_PATCH_LIST_RECORDINGS  0x8422u /* no arguments - list the I/Q recordings a patch Radio can play (API 0.3 8.12) */
// THE TRACK-INFO PLUGIN'S STATUS LINE (the pump move, step P2): the window owns the plugin's cache (GUI thread only) and polls its
// text in its own frame; each line it finds is sent here and lands in the Decoder output backlog as "Aircraft info", counted like
// every other line. Replaces the host hook the engine's pass used to ask (EngineHost::drainTrackInfoText). Grant VIEW (it reads and
// changes nothing but that backlog), LOCAL sessions only (every app op is).
#define FOXAPP_OP_TRACK_INFO_LINE        0x8423u /* text (or long text): one status line from the track-info plugin; empty is BAD_ARGUMENT */
// THE 0.99.84 MERGE'S OPS (docs/engine-merge-0.99.84.md; docs/api-0.3-additions.md section 9): what master's window did with engine fields in place and
// the table had no op for. Same rules as every app op: LOCAL sessions only, grant CONTROL unless said.
#define FOXAPP_OP_SET_RTLTCP_ADDR        0x8424u /* text: the rtl_tcp address box, committed as typed (rtlTcpAddr_); the Pluto box's twin */
#define FOXAPP_OP_BANDWIDTH_DRAG_END     0x8428u /* the drag on the spectrum's band edge has settled (the mouse is let go): one log line for the width it ended on */
#define FOXAPP_OP_TEST_HOLD_AUDIO_LOCK   0x8427u /* ival[0]: ms (0.99.68, bounded runs only: FOXSDR_HOLD_AUDIO_LOCK) - the first stop a control request makes of a running receiver first has a thread take the audio mutex for that long; 0 = no seam */
#define FOXAPP_OP_SET_PPM                0x8426u /* ival[0]: the crystal correction's switch (0/1); num[0]: the open radio's value, ppm (core::sanitisePpm; 0 = none) */
#define FOXAPP_OP_SET_AUDIO_BUFFER       0x8425u /* ival[0]: the Sinks rail's AUDIO BUFFER, 0 AUTOMATIC | 120 | 240 | 480 | 960 ms (sink::validAudioBufferSetting) */
// THE AIRBAND MONITOR (stub campaign B6, 0.99.60 - 0.99.71; docs/api-0.3-additions.md 9.6), LOCAL sessions only, grant TUNE (it tunes the receiver):
// several ticked AM and NFM channels off the receiver's one radio, mixed to the speakers, each behind its own squelch. ival[0] is the action:
//   0 STOP: the monitor stops (NO_CHANGE when it was neither listening nor waiting).
//   1 LISTEN: text: the preset to play - a bookmark group - or "" for every ticked AM or NFM row. Nothing ticked there is NO_CHANGE with the note
//   "Tick at least one AM or NFM frequency to listen to."; a radio that cannot be divided to an audio rate, or a block the radio will not tune to, is
//   FAILED with the note. A patch view that is showing is handed back FIRST: the engine closes it (the receiver view) and the monitor starts on the
//   pass after the patch has let go of the radio - applied[0] is then 2. A monitor already listening takes the new preset and starts again.
//   2 NEXT BLOCK: moves on to the next block now (NO_CHANGE with one block, or not listening).
//   3 ABOVE THE NOISE: the squelch to 8 dB over the median channel level the strips have reported (NO_CHANGE with no level yet); applied[1]: the dB.
//   4 SQUELCH: num[0] dB, clamped to -110..0 (applied[1]); a listening monitor rebuilds its block with it. 5 HOLD: num[0] seconds, clamped to 0..60: how
//   long a block stays after its last word; a listening multi-block monitor starts again.
//   6 TUNE ONE: ival[1]: a bookmark's id. The monitor stops ("Tuned to <name> alone.") and the receiver tunes to that row, as a click on it would.
// applied[0] is the state after: 0 not listening, 1 listening, 2 waiting for the patch to hand the radio back. What it is doing is the facts
// fact.airband.* and the live list AIRBAND_CHANNELS.
#define FOXAPP_OP_AIRBAND                0x8429u /* ival[0]: 0 stop / 1 listen (text: preset) / 2 next block / 3 above the noise / 4 squelch / 5 hold / 6 tune one */
// AUTOMATIC SQUELCH (stub campaign B12, 0.99.74; docs/api-0.3-additions.md 9.9). The AUTO key under the top bar's SQUELCH dial: while it is on, the ENGINE keeps the
// squelch threshold 8 dB above the noise floor it measures under the channel (the quietest of the last ten seconds of the meter, a carrier that holds the gate open
// never teaching it - core/noise_floor_tracker.hpp) or, while the Airband monitor listens, applies the monitor's "above the noise" rule to the monitor's own squelch
// every quarter second. LOCAL sessions only and not on the web profile; grant SETTINGS.
//   ival[0]: 0 off / 1 on. OK for what it already is (nothing changes). Turning it on forgets any floor measured before; applied[0] is the switch after.
// A HAND ON ANY THRESHOLD IS A MANUAL CHOICE: an accepted SET_SQUELCH (the dial, the Radio section's slider, the keys, a web page), an accepted AIRBAND squelch (4) and
// an AIRBAND above-the-noise (3) pressed while the monitor listens (whether or not the rule then finds a level) turn it OFF, whoever sent them; a refused or out-of-range
// one, and an above-the-noise with nothing listening, change nothing. A threshold restored from the config does not. The switch is saved (AppConfig::squelchAuto) and the
// switch and the floor are published as fact.squelch.auto / fact.squelch.floorDb.
#define FOXAPP_OP_SET_SQUELCH_AUTO       0x842Au /* ival[0]: 0 off / 1 on; applied[0]: the switch after */
// A PLUGIN MAY KEY THE TRANSMITTER (stub campaign B13, 0.99.74's CASCADE_CAP_TX_AUDIO; docs/api-0.3-additions.md 9.10). SAFETY-CRITICAL, and these three ops are the whole of
// the table's surface for it. EVERY ONE IS LOCAL-ONLY: grant TRANSMIT, not in the web profile (docs/api-0.3-additions.md 8.16.3's 38 ops do not grow), refused to every REMOTE
// session at submit (three times over: the application's ops, TRANSMIT being a web remote's PTT and nothing else, the profile) and again where it is applied. NONE OF THEM RIDES
// ON FOXAPI_OP_PLUGIN_GRANT, which IS in the web profile: its kinds stay 1 (tune) and 2 (settings), and kind 3 is still "no such grant".
//   TX_PLUGIN_ARM: the operator's ARMING WINDOW, which the ENGINE holds (a window crash, a frozen window or a closed page cannot leave a plugin able to key). It belongs to the SESSION
//     that opened it (its LEASE) and lasts while that session is alive and keeps saying the TRANSMIT page is on screen.
//       ival[0]: 5 / 15 / 60  open (or restart) the window for that many minutes; refused with a reason (applied[0] 1 no radio is open, 2 no plugin holds the grant) otherwise
//                0            close it (ival[1]: 0 "you pressed OFF", 1 "the TRANSMIT page was closed")
//                -1           THE PAGE IS LIVE, said again (the window sends it every frame the page is drawn while a window is open); anything else is OUT_OF_RANGE.
//     applyCommand REFUSES it (a window is a session's, as a PTT is): only the table's host, which knows the sender, applies it.
#define FOXAPP_OP_TX_PLUGIN_ARM          0x842Bu /* ival[0]: 5 / 15 / 60 arm, 0 close, -1 the page is live; ival[1]: 0 OFF pressed / 1 page closed */
//   TX_DATA_FLAT: the page's DATA (flat) tick, for a plugin's audio only: on (the default) it goes out as NFM with the pre-emphasis off, whatever mode the page shows. Session-only
//     (never saved). ival[0]: 0 / 1.
#define FOXAPP_OP_TX_DATA_FLAT           0x842Cu /* ival[0]: 0 off / 1 on */
// THE TWO TEST OPS OF SHELL RETIREMENT R2 (docs/engine-stage4.md 6.25; API 0.3 9.13). They replace the bounded-run seams seamPatchStart and
// seamBiasStandIn (methods of the retired shell, deleted in R10), which the window called through the door: now the window sends them on its own session, in bounded runs only, as it sends
// FOXAPP_OP_TEST_HOLD_AUDIO_LOCK. Like every FOXAPP_OP_* they are refused to a REMOTE session ("the application's own commands are for sessions on this machine").
//   TEST_PATCH_RUNNING: FOXSDR_PATCH_START. No arguments. Sets the patch graph's "running" flag and NOTHING else - it does not switch any radio on (that is
//     what FOXAPI_OP_PATCH_RUN does, through patchPressStart), which is the point: a graph whose radios are off is what the tests that use it are about.
//   TEST_BIAS_STAND_IN: FOXSDR_FORCE_BIAS_KEY. ival[0]: 0 none / 1 accept / 2 refuse (cascade::gui::BiasStandIn); anything else is OUT_OF_RANGE. The stand-in
//     bias tee in front of the deck's key (core/bias_tee.hpp) while no real radio with one is open.
#define FOXAPP_OP_TEST_PATCH_RUNNING     0x842Du /* no arguments (FOXSDR_PATCH_START, bounded runs only): the patch graph is marked running, no radio is switched on */
#define FOXAPP_OP_TEST_BIAS_STAND_IN     0x842Eu /* ival[0]: 0 none / 1 accept / 2 refuse (FOXSDR_FORCE_BIAS_KEY, bounded runs only) */
// THE INTERFACE'S HALF OF THE SETTINGS FILE (shell retirement R5; API 0.3 9.16; core/config_halves.hpp). The settings file has two halves, and an interface
// owns one of them (its view state, the key bindings, the patch text, the update and diagnostics switches ...). It hands that half to the engine as a
// DOCUMENT - the text ConfigStore::serialize writes, cut to the keys of the half - and the engine stores it and puts it into every snapshot, so whoever
// saves the snapshot saves the interface's half with it. The engine hands the half it started with back as the LOCAL fact fact.config.interface (read at
// the call, the start-up state of the half: every open flag cleared). Like every FOXAPP_OP_* it is refused to a REMOTE session.
//   INTERFACE_CONFIG (SETTINGS): long text, the document. It REPLACES the half held (every field of the half is in the document; one that is missing
//     reads as its default, as in the file). A document that is not JSON is BAD_ARGUMENT and changes nothing. The Diagnostics switch in it moves the
//     control thread's hang-report directory (ControlThreadOptions::diagnosticsDir).
#define FOXAPP_OP_INTERFACE_CONFIG       0x842Fu /* long text: the interface's half of the settings file, as a document (LOCAL only) */
// FOXAPP_OP_SOUND_CARD_FORM's ival[1], the FIELD MASK (API 0.3 8.10): which fields the command carries. 0 (and
// ALL) is the whole form. With a mask, the fields not named keep the engine's own values.
#define FOXAPP_SOUND_CARD_FIELD_DEVICE   1  /* text: device and host API */
#define FOXAPP_SOUND_CARD_FIELD_FORMAT   2  /* ival[0] bit 1 */
#define FOXAPP_SOUND_CARD_FIELD_CHANNEL  4  /* ival[0] bit 2 */
#define FOXAPP_SOUND_CARD_FIELD_SWAP_IQ  8  /* ival[0] bit 4 */
#define FOXAPP_SOUND_CARD_FIELD_PICKED   16 /* ival[0] bit 8 */
#define FOXAPP_SOUND_CARD_FIELD_RATE     32 /* num[0] */
#define FOXAPP_SOUND_CARD_FIELD_CENTRE   64 /* num[1] */
#define FOXAPP_SOUND_CARD_FIELDS_ALL     127
// FOXAPP_OP_PATCH_SET_GRAPH's ival[0]: set when the graph is a NEW DOCUMENT
// (the config's patch, a patch file) rather than an edit of the graph the
// engine already has. The Engine counts these (Engine::patchGraphEpoch_), so a
// window part-way through an edit of the old document knows not to put that
// edit on top of the new one.
#define FOXAPP_PATCH_GRAPH_DOCUMENT      1
// FOXAPP_OP_PATCH_SET_GRAPH's ival[1] (API 0.3 8.12): the graph VERSION the
// sender's draft was last in step with (fact.patch.version, read in the same
// publish as the graph it edited). 0 is "unconditional" (a document load, and
// today's behaviour). A version that is not the current one is MERGED by the
// engine (core::patch::rebaseDraft against the retained graph of that
// version) and not overwritten; one the engine no longer retains is refused
// FOXAPI_FAILED. The result's applied[0] is the version after the command and
// applied[1] is 1 when the engine merged.

#define FOXAPP_OP_FIRST 0x8000u
#define FOXAPP_OP_LAST  0x8FFFu

namespace cascade::core::cmd {

// FOXAPP_OP_CLEAR_STATUS's ival[0]. Each resets exactly one field a status
// panel used to clear or consume in place; see the field named in each
// comment (engine.hpp) for what it means and who else writes it.
enum FoxAppStatus : std::int64_t {
    FOXAPP_STATUS_GPS_REFUSAL = 1,               // gpsRefusal_.clear()
    FOXAPP_STATUS_SOUND_CARD_MISSING = 2,         // soundCardMissing_.clear()
    FOXAPP_STATUS_DECODER_LOG = 3,                // decoderLog_.clear()
    FOXAPP_STATUS_SOAPY_SCAN_DEFERRED_LOGGED = 4, // soapyScanDeferredLogged_ = false
    FOXAPP_STATUS_MUTE_POPUP_QUEUED = 5,          // mutePopupQueued_ = false (consumed)
    FOXAPP_STATUS_GPS_RESULT = 6,                 // gpsReader_.clearResult(): a finished read's line goes (a no-op while it listens)
};

// A command and, when its text did not fit FoxCommand::text, the whole of it.
struct QueuedCommand {
    FoxCommand c{};
    std::string longText;
};

// A zeroed command of `op`, structSize set - every slot an op does not read
// stays zero, as the API requires.
inline FoxCommand make(std::uint32_t op) {
    FoxCommand c;
    std::memset(&c, 0, sizeof(c));
    c.structSize = static_cast<std::uint32_t>(sizeof(FoxCommand));
    c.op = op;
    return c;
}

inline FoxCommand makeInt(std::uint32_t op, std::int64_t v0, std::int64_t v1 = 0) {
    FoxCommand c = make(op);
    c.ival[0] = v0;
    c.ival[1] = v1;
    return c;
}

inline FoxCommand makeNum(std::uint32_t op, double n0, double n1 = 0.0) {
    FoxCommand c = make(op);
    c.num[0] = n0;
    c.num[1] = n1;
    return c;
}

// num[0] and ival[0] together (STEP_TUNE's steps and size, a click-to-tune's
// frequency and snap, a split's frequency and switch).
inline FoxCommand makeNumInt(std::uint32_t op, double n0, std::int64_t v0) {
    FoxCommand c = make(op);
    c.num[0] = n0;
    c.ival[0] = v0;
    return c;
}

// Puts `s` into c.text (truncated, always NUL-terminated) and answers the
// long text to carry beside it: empty when it fitted, the whole of `s` when
// it did not.
inline std::string putText(FoxCommand& c, const std::string& s) {
    const std::size_t n = std::min(s.size(), static_cast<std::size_t>(FOXAPI_TEXT_CHARS - 1));
    std::memcpy(c.text, s.data(), n);
    c.text[n] = '\0';
    return (s.size() > n) ? s : std::string();
}

inline QueuedCommand makeText(std::uint32_t op, const std::string& s, std::int64_t v0 = 0,
                              std::int64_t v1 = 0, double n0 = 0.0) {
    QueuedCommand q;
    q.c = make(op);
    q.c.ival[0] = v0;
    q.c.ival[1] = v1;
    q.c.num[0] = n0;
    q.longText = putText(q.c, s);
    return q;
}

// The text an op reads: the long text when one came with it, else the slot
// (read only up to its NUL, never past the array).
inline std::string textOf(const FoxCommand& c, const std::string& longText) {
    if (!longText.empty()) { return longText; }
    const void* nul = std::memchr(c.text, '\0', sizeof(c.text));
    const std::size_t n = (nul != nullptr) ? static_cast<std::size_t>(static_cast<const char*>(nul) - c.text)
                                           : sizeof(c.text);
    return std::string(c.text, n);
}

// Mode names in FOXAPI_DEMOD_* order: NFM is 1 ... RAW is 8, the same order
// as the desktop's mode keys (and CASCADE_DEMOD_*). 0 for a name it does not
// carry, which applyCommand refuses as out of range.
inline std::uint32_t foxDemodFromName(const char* name) {
    static constexpr const char* kNames[8] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
    if (name == nullptr) { return 0u; }
    for (std::uint32_t i = 0; i < 8u; ++i) {
        if (std::strcmp(kNames[i], name) == 0) { return i + 1u; }
    }
    return 0u;
}

// The op's name for logs, docs and tests ("SET_CENTRE", "APP_VFO_TO_ABSOLUTE");
// "?" for an op nothing here knows.
const char* opName(std::uint32_t op);

// True for every op applyCommand implements - the API's and the extensions.
bool isKnownOp(std::uint32_t op);

inline bool isAppOp(std::uint32_t op) { return op >= FOXAPP_OP_FIRST && op <= FOXAPP_OP_LAST; }

// Every op isKnownOp answers true for, in a fixed order (tests, docs).
const std::uint32_t* knownOps(std::size_t& count);

}  // namespace cascade::core::cmd
