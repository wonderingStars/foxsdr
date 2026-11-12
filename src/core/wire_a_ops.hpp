// wire_a_ops.hpp - THE OPERATIONS OF WIRING LANE wire-a (tuning, recording, display): block 0x8800-0x88FF, taken ascending, never reused.
//
// Included once, from the end of the macro list in core/app_commands.hpp; the rows of the table core/app_commands.cpp checks an op against are the one macro
// WIRE_A_KOPS at the end of kOps. Each op is documented where it is implemented (src/engine/wire_a*.cpp) and in docs/multi-radio-records/wire-a.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_WIRE_A_OPS_HPP
#define CASCADE_CORE_WIRE_A_OPS_HPP

// wire-a begin
//   HISTORY_STEP (SETTINGS): the selected radio's (target) frequency history (core/freq_history.hpp): ival[0] 0 = back, 1 = forward. NO_CHANGE at the end of the history.
#define FOXAPP_OP_HISTORY_STEP           0x8800u /* ival[0]: 0 back / 1 forward. Addresses one radio (target) */

//   CHANNEL (SETTINGS): channel mode on the selected radio (target), core/channel_plans.hpp. ival[0]: 0 = choose the plan named by the text (an id from fact.channel.plans; the radio
//   tunes to its first channel), 1 = next channel, 2 = previous channel, 3 = go to the channel the text names (its label, "16", "G7", "AIS 1"), 4 = leave channel mode.
//   A channel retunes the radio to its frequency (a marine two-frequency channel: what ships transmit), sets the plan's mode hint and its channel width as the bandwidth.
//   OUT_OF_RANGE: an unknown plan, label or ival[0]; NO_CHANGE: leaving when not in channel mode, or next / previous / go to with no plan chosen. applied[0] = the frequency.
//   fact.channel.plans (id<TAB>name lines), fact.channel.<n>.plan / .label / .freqHz / .altHz / .note / .modeHint / .numberingIsOurs.
#define FOXAPP_OP_CHANNEL                0x8801u /* ival[0]: 0 plan / 1 next / 2 previous / 3 go to / 4 leave; text: plan id or channel label. Addresses one radio (target) */

//   MARKER_EXPORT (SETTINGS): the waterfall's markers (the list FREQ_MARKERS) written to the file the text names (a full path; a file that exists is never overwritten). ival[0]: 0 = CSV
//   (number,freq_hz,noted_unix,note; a note that starts like a formula is guarded for spreadsheets), 1 = JSON (version 1). applied[0] = the number written. NO_CHANGE: no markers.
//   BAD_ARGUMENT: a path that is not a full path, or a file that exists; FAILED: the write failed (the message says why). MARKER_IMPORT (SETTINGS): the file the text names (CSV or JSON,
//   chosen by the file's extension; at most 1 MiB), read strictly: any bad row refuses the whole file and changes nothing. A marker within num[0] Hz of one in the list is merged (a note is kept
//   when the list's marker has none), a new one is added with the next number. applied[0] = added, applied[1] = merged. LIMIT: the list is full (what fit is kept). fact.markerio.* says what happened.
#define FOXAPP_OP_MARKER_EXPORT          0x8802u /* text: full path; ival[0]: 0 CSV / 1 JSON */
#define FOXAPP_OP_MARKER_IMPORT          0x8803u /* text: full path; num[0]: merge reach, Hz (0 = same frequency only) */

//   BOOKMARK_NOTE (SETTINGS): the note and tags of one bookmark (core/bookmark_notes.hpp, kept in bookmark_notes.json beside markers.json, keyed by frequency + mode so they survive the
//   bookmark list being re-imported). ival[0]: the bookmark's id; ival[1]: 0 = set, 1 = read. set: long text (or text) = JSON {"note":"...", "tags":["a","b"]} (note up to 2000 bytes, up to 16 tags of
//   32 bytes; an empty note and no tags removes the record; the text "" removes it too); read: the record's JSON in fact.bookmark.note.last (id in fact.bookmark.note.lastId, "" when none).
//   NOT_FOUND: no bookmark with that id; BAD_ARGUMENT: bad JSON, an over-long note, a bad tag, too many notes. NO_CHANGE: set to what is there. BOOKMARK_FIND (SETTINGS): ival[0]: 0 = text search of notes and
//   tags, 1 = bookmarks carrying the tag the text names; the ids come back newline-separated in fact.bookmark.find, applied[0] = the count. A bookmark that is renamed or retuned takes its note with it.
#define FOXAPP_OP_BOOKMARK_NOTE          0x8804u /* ival[0]: bookmark id; ival[1]: 0 set / 1 read; long text: JSON {note, tags} */
#define FOXAPP_OP_BOOKMARK_FIND          0x8805u /* ival[0]: 0 text / 1 tag; text: the query */

//   FAVOURITE_SAVE / FAVOURITE_RECALL / FAVOURITE_EDIT (SETTINGS): favourites that keep a WHOLE state (core/favourite_state.hpp), up to 200, in favourites.json beside markers.json. SAVE (the selected radio, target):
//   text = JSON {"name":"...", and optionally the picture's own numbers the screen owns: "displayCentreHz", "displaySpanHz", "spectrumTopDb", "spectrumRangeDb", "waterfallFloorDb", "waterfallCeilingDb"}; the engine
//   fills the frequency, mode, bandwidth and the DSP (AGC, noise reduction and its level, noise blanker, notch, squelch, de-emphasis) from the radio. A name that exists is replaced. BAD_ARGUMENT: bad JSON, a name that is
//   empty / over 64 bytes, a value out of range; LIMIT: the 201st name. RECALL (the selected radio): text = the name; retunes the radio, sets the mode, bandwidth and the DSP, and puts the whole favourite as JSON in
//   fact.favourite.recalled (the picture's numbers are for the screen to apply). NOT_FOUND: no such name. EDIT: ival[0] 0 = remove the favourite the text names, 1 = rename (text "old<TAB>new"; NOT_FOUND / BAD_ARGUMENT
//   when it cannot). fact.favourite.names (newline-separated), fact.favourite.count.
#define FOXAPP_OP_FAVOURITE_SAVE         0x8806u /* text: JSON {name, display...}. Addresses one radio (target) */
#define FOXAPP_OP_FAVOURITE_RECALL       0x8807u /* text: the name. Addresses one radio (target) */
#define FOXAPP_OP_FAVOURITE_EDIT         0x8808u /* ival[0]: 0 remove / 1 rename; text: name, or "old<TAB>new" */

//   PACK_EXPORT / PACK_IMPORT (SETTINGS): shared bookmark packs (core/bookmark_pack.hpp, "foxsdr-pack" version 1): the bookmarks with their notes and tags and the whole-state favourites in one JSON file a user can
//   hand to another. EXPORT: text = JSON {"path":"<full path>", "name":"...", "description":"..."} (the name and description are typed by the user; nothing else is added: no id, no heard-seconds, no path);
//   a file that exists is never overwritten; applied[0] = bookmarks written, applied[1] = bookmarks left out as not exportable. NO_CHANGE: nothing to export. IMPORT: text = the pack's full path (at most 4 MiB), read
//   strictly, all or nothing; a bookmark within 1 Hz of one in the list in the same mode is a duplicate and is left out (nothing is overwritten), the rest are added with their notes and tags; a favourite whose name is
//   taken is left as it is. applied[0] = added, applied[1] = duplicates. LIMIT: the list or the favourites are full. fact.pack.last = "added N, M already there".
#define FOXAPP_OP_PACK_EXPORT            0x8809u /* text: JSON {path, name, description} */
#define FOXAPP_OP_PACK_IMPORT            0x880Au /* text: full path of the pack */

//   THE SCANNER (SETTINGS; core/scan_plan.hpp, scan_trigger.hpp and the screen C extras of core/scanner.hpp). One scanner, bound to the radio it was started on (as SCANNER_RUN does).
//   SCAN_PLAN: ival[0] 0 = set the plan from the JSON in the text (core/scan_plan.hpp toJson: up to 16 ranges, each with its own step, mode and bandwidth, a direction, the trigger numbers; strict, all or nothing; a
//   plan whose points would pass 100000 is refused), 1 = start a scan of the plan on the selected radio (target; the plan's points are the scanner's list, each stop tunes the mode and bandwidth of the range it is in;
//   DENIED on a frequency-locked radio; NO_CHANGE with no plan), 2 = clear the plan (a running plan scan stops). Any other scan start (SCANNER_RUN, a list scan) ends plan mode. fact.scan.plan (the JSON), fact.scan.plan.active.
//   SCAN_BACK: during a plan scan, step back to the point before the one the scan is on (it wraps), and carry on from there in the plan's direction. NO_CHANGE when no plan scan is running.
//   SCAN_SAVE_HIT: num[0] = a frequency (0 = the frequency the scanner's radio is on): add it as a bookmark in the group "Scan hits", with the name "Scan <MHz>", the mode and bandwidth of the plan range it lies in; NO_CHANGE
//   when a bookmark at that frequency in that mode exists already, or there is no plan. applied[0] = the frequency.
//   SCAN_TRIGGER: the scan's own trigger, apart from the squelch: ival[0] 0 = off (the squelch decides, as always), 1 = on; text (optional) = the trigger JSON (core/scan_trigger.hpp configToJson: fixed level or margin above
//   a floor that is kept for each scan point, hysteresis, floor rise). BAD_ARGUMENT for a bad config. fact.scan.trigger (the JSON), fact.scan.trigger.on.
//   SCANNER_EXTRAS: ival[0] 0 = the priority list (text: Hz values separated by ';', at most 64; num[0] = the interval in ms between visits, 0 = never), 1 = lock a frequency out (num[0] Hz, 0 = the one the scan is on;
//   num[1] ms, 0 = until cleared), 2 = clear the lockouts of 1, 3 = mute while scanning (ival[1] 0/1), 4 = per-entry thresholds (text: lines "<Hz><TAB><dB>", at most 1000; they apply to the entries of a list or plan scan),
//   5 = the locked-out frequencies kept with the configuration (text: Hz values separated by ';', at most 1000). OUT_OF_RANGE / BAD_ARGUMENT for values that are not frequencies, levels or times. fact.scanner.extras is "1"
//   (the window shows its "not used yet" note only while it is absent), fact.scan.priority, .priority.interval, .locked, .thresholds, .mute.
#define FOXAPP_OP_SCAN_PLAN              0x880Bu /* ival[0]: 0 set / 1 start / 2 clear; text: the plan JSON. Addresses one radio (target) */
#define FOXAPP_OP_SCAN_BACK              0x880Cu
#define FOXAPP_OP_SCAN_SAVE_HIT          0x880Du /* num[0]: Hz (0 = where the scanner's radio is) */
#define FOXAPP_OP_SCAN_TRIGGER           0x880Eu /* ival[0]: 0 off / 1 on; text: the trigger JSON (optional) */
#define FOXAPP_OP_SCANNER_EXTRAS         0x880Fu /* ival[0]: 0 priority / 1 lock out / 2 clear / 3 mute / 4 thresholds / 5 locked list; ival[1], num[0], num[1], text: see above */

//   THE LANE'S ONE BACKGROUND JOB (the engine's pass does no disk work: a job that reads or writes a recording runs on a worker of core/disk_job.hpp, one at a time; BUSY while one is out). Its state is read from
//   fact.job.state ("idle", "running", "done", "failed", "cancelled"), fact.job.name, fact.job.progress (0..1), fact.job.message (words, never a path) and fact.job.result (JSON; the job's own numbers).
//   JOB_CANCEL: asks the running job to stop at its next block (a cancelled export leaves a valid file of what was written). NO_CHANGE when none is running.
//   EXPORT_BOX (SETTINGS; dsp/box_export.hpp): the part of a recorded I/Q file inside a time-and-frequency box, mixed to baseband, filtered and decimated, written as I/Q or demodulated audio. text = JSON
//   {"input":"<full path of the recording (.wav, .rf64, .sigmf-meta, .sigmf-data, .cu8, .cs16, .cf32)>", "output":"<full path of the new file>", "t0":s, "t1":s, "loHz":Hz, "hiHz":Hz (absolute RF),
//   "mode":"iq"|"am"|"fm"|"usb"|"lsb", "format":"float32"|"pcm16"|"cf32", and optionally "attenuationDb" (40..120, default 80), "minOutRateHz", "audioGain", "fmDeviationHz", "sampleRateHz" and "centerHz" (for a raw
//   file or one whose header has none)}. The output file is never an existing file. BAD_ARGUMENT: bad JSON, a path that is not a full path, an unknown mode or format, a missing number; BUSY: a job is out.
//   The refusals the plan makes (a box outside the band, an export over 4 GiB, a rate the file does not have...) arrive as fact.job.state "failed" with the plan's words in fact.job.message. On success
//   fact.job.result carries {"outRateHz","frames","decimation","taps","designAttenuationDb","clipped","peak"}.
#define FOXAPP_OP_JOB_CANCEL             0x8810u
#define FOXAPP_OP_EXPORT_BOX             0x8811u /* text: JSON, see above */

//   PROJECT (SETTINGS; core/analyser_project.hpp): the two jobs of the analyser project that read a whole recording; the project's own file (JSON, versioned) is the screen's to make and parse with the header.
//   text = JSON {"action":"hash","recording":"<full path>"} -> a job that hashes the recording (SHA-256, in 1 MiB reads, cancellable, fact.job.progress) and answers fact.job.result {"sha256","sizeBytes"};
//   {"action":"verify","project":"<full path of the .fxproj>","recording":"<full path, optional: where the recording is now>"} -> a job that loads the project and says whether the recording found is the one
//   it was made on: fact.job.result {"state":"match"|"missing"|"sizeDiffers"|"hashDiffers"|"notHashed"|"cancelled"|"readError","tags":N}. BAD_ARGUMENT for bad JSON, a path that is not a full path or an unknown action;
//   BUSY while a job is out. ATTACH (SETTINGS; core/record_attachments.hpp): screenshots, clips and notes kept with a record, in the record's own folder (an existing folder the text names) and its manifest
//   attachments.json (versioned, atomic). text = JSON {"folder":"<full path>", "action":"add"|"edit"|"remove"|"verify"|"list", and for add "kind":"screenshot"|"clip"|"note", "path":"<file to copy in>" (or "text" for
//   a note), "title":"...", for edit/remove "id":N (edit: "title" and, for a note, "text")}; the file's type is decided by its first bytes (a program renamed .png is refused), the copy is hashed, limits 500 items
//   and 1 GiB per record; runs as the lane's job; fact.job.result {"items":[{"id","kind","title","sizeBytes","state"?}]} (verify adds each file's state: ok, missing, sizeChanged, hashChanged, readError).
//   SIGMF_ANNOTATE (SETTINGS; source/sigmf_annotations.hpp): edits the annotations of a finished SigMF recording's .sigmf-meta (a job; every other key is kept, written as a temp file and renamed): text = JSON
//   {"meta":"<full path>", "action":"add", "startSec","endSec","loHz","hiHz" (an analyser box), "sampleRateHz", "totalSamples" (0 = not checked), "label","comment"} or {"meta", "action":"remove", "index":N} or
//   {"meta", "action":"list"}; fact.job.result {"annotations":N}.
#define FOXAPP_OP_PROJECT                0x8812u /* text: JSON {action: hash|verify, ...} */
#define FOXAPP_OP_ATTACH                 0x8813u /* text: JSON {folder, action, ...} */
#define FOXAPP_OP_SIGMF_ANNOTATE         0x8814u /* text: JSON {meta, action, ...} */

//   KEY_ACTION (SETTINGS; core/key_actions.hpp): the keyboard actions the key table did not have, as one op on the selected radio (target); the key bindings themselves are the screen's. text = the action id:
//   "bandwidth_up" / "bandwidth_down" (the next width in the mode's own list: AM 3-12 kHz, NFM 5-25 kHz, WFM 100-250 kHz, USB / LSB 1.8-3.6 kHz, CW 100 Hz-1.2 kHz; no wrap at the ends; NO_CHANGE at an end or for
//   a mode with no list), "cycle_mode" (the next of NFM, WFM, AM, DSB, USB, CW, LSB, wrapping), "centre_signal" (retunes onto the strongest signal inside the passband: the peak of the radio's last spectrum frame
//   within half the bandwidth of the tuned frequency; NO_CHANGE when it is already within 5 Hz of it or there is none), "next_bookmark" / "previous_bookmark" (the bookmark above / below the tuned frequency, wrapping;
//   NO_CHANGE with no bookmark). "cycle_step" is the window's: the tuning step is kept by the window, not the engine, so it answers UNSUPPORTED. OUT_OF_RANGE for any other text. applied[0] = the bandwidth (the bandwidth
//   actions), the frequency (centre_signal, bookmarks) or the mode number from 1 (cycle_mode).
#define FOXAPP_OP_KEY_ACTION             0x8815u /* text: the action id. Addresses one radio (target) */

//   SET_RECORD_DAILY (SETTINGS; core/rec_daily.hpp): every recording take goes into a folder for the day it STARTS (a take that crosses midnight stays where it began), below the recordings folder (and above the
//   folders a file-name template makes, SET_RECORD_NAME). text = JSON {"style":0..4 (0 none = today's behaviour, the default; 1 "2026-11-11"; 2 "2026/11/11"; 3 "2026-11/11"; 4 "20261111"), "utc":true|false (the
//   date in UTC, the default, or the computer's local time), "template":"" (or a custom one with {yyyy} {mm} {dd} {date} {compact}, which replaces the style)}. The folder is made level by level inside the
//   recordings folder (a link, a junction or a file in the way is refused and the take keeps its default place, said in the log). BAD_ARGUMENT for bad JSON, a style out of range or a template the model
//   refuses (nothing changes); NO_CHANGE when it is in force already. Kept in record-daily.json beside markers.json. fact.record.daily is the JSON in force.
#define FOXAPP_OP_SET_RECORD_DAILY       0x8816u /* text: JSON {style, utc, template} */

//   RECORD_LOCK (SETTINGS; core/record_lock.hpp): while a radio has a take open, the settings that would make the file lie are refused (FOXAPI_DENIED, with the model's sentence: "The sample rate is locked while an
//   I/Q recording is open: the file stores one sample rate ... Stop the recording first."). ival[0]: 1 = lock while recording, 0 = off (the DEFAULT: today a source change during an I/Q take ends the take with a notice, which the lock would refuse instead; the screen hand-off wanted it on: the owner decides); ival[1]: 1 = gain is locked as well
//   (default 0). Guarded, for the radio a command names: SET_CENTRE and a SET_FREQUENCY that moves the centre (not while centre tuning keeps it) and SET_SAMPLE_RATE, SELECT_SOURCE, SET_PPM, SET_CONVERTER and RADIO_REMOVE
//   during an I/Q take (a request that re-sends the frozen centre or rate is allowed; for SET_PPM, SET_CONVERTER and SELECT_SOURCE the frozen value is not known to the lane, so any request is refused), SET_FREQUENCY,
//   SET_MODE and SET_BANDWIDTH during an IF take, RADIO_REMOVE during any take, SET_GAIN when the gain lock is on. A SigMF take, which logs its own retunes, will leave centre and correction free (the format is not
//   built yet). NO_CHANGE when it is so already. fact.record.lock "1"/"0", fact.record.lock.gain, fact.record.lock.frozen.<n> (the settings frozen on radio n now, newline-separated, for the screen to grey out).
#define FOXAPP_OP_RECORD_LOCK            0x8817u /* ival[0]: 1 lock while recording / 0 off; ival[1]: 1 lock gain too */

//   OVERLAY_LOAD / OVERLAY_LOOKUP (SETTINGS; core/csv_overlay.hpp): columns of the user's OWN station database (a CSV they name) shown next to what is tuned, by frequency; the file is read, never copied or changed.
//   OVERLAY_LOAD: text = JSON {"path":"<full path of the CSV>", "config":{"version":1,"freqColumn":"frequency","unit":"Hz"|"kHz"|"MHz","columns":["name","location"] (1 to 16),"delimiter":","|";"|"\t"}} -> the lane's job
//   (the file is read and parsed on the worker; at most the model's limits); text "" removes the overlay. BAD_ARGUMENT: bad JSON, a path that is not a full path, a config the model refuses. When the job is done
//   fact.overlay.rows / .skipped (rows that were not usable: counted, not fatal) / .cleaned (cells repaired) / .columns say what was loaded; a file the model refuses is a failed job (fact.job.message). OVERLAY_LOOKUP:
//   num[0] = a frequency, num[1] = the tolerance in Hz (0 = 500), ival[0] = at most that many rows (0 = 1; up to 8): fact.overlay.lookup = JSON [[["column","value"],...],...] for the rows nearest within the tolerance;
//   applied[0] = how many. NO_CHANGE when no overlay is loaded; OUT_OF_RANGE for a frequency, tolerance or count that is not one.
#define FOXAPP_OP_OVERLAY_LOAD           0x8818u /* text: JSON {path, config}, or "" to remove */
#define FOXAPP_OP_OVERLAY_LOOKUP         0x8819u /* num[0]: Hz; num[1]: tolerance Hz (0 = 500); ival[0]: how many rows (0 = 1, up to 8) */
//   GRADIENT_SET (SETTINGS; core/gradient_model.hpp): the waterfall's own colour gradient, as the model's text form "FOXGRAD1|..." (2 to 32 colour stops, an interpolation, a name). The engine checks the text with the model's
//   parser, keeps it in display-gradient.json beside markers.json and serves it back (the screen applies toTable(); the colour table itself lives in the screen's compositor): text "" goes back to the stock maps. BAD_ARGUMENT
//   with the model's sentence ("A gradient needs at least two colour stops." ...) for a text it refuses (nothing changes); NO_CHANGE when it is in force already. applied[0] = the number of stops. fact.display.gradient is the
//   text in force ("" = none), fact.display.gradient.error a damaged file's reason.
#define FOXAPP_OP_GRADIENT_SET           0x881Au /* text: FOXGRAD1|...  or "" */

//   DISPLAY_AVERAGE (SETTINGS; dsp/trace_average.hpp): what is done to the spectrum frame between the pipeline and every screen: averaging, smoothing along the frequency axis, and removal of a regular comb of spurs. text =
//   JSON {"average":"off"|"linear"|"rms"|"log"|"exponential" (default off), "frames":1..128 (8), "timeConstantS":0.005..60 (0.5), "smoothing":"none"|"moving"|"savgol" (none), "smoothWidth":odd 3..101 (9),
//   "smoothOrder":0..8 below the width (2), "comb":true|false (false), "combPeriod":0 (auto) or 4.., "combStrength":0..1 (1), "dcBins":0..9 (0), "combMemory":0..0.99 (0)}; a missing field keeps its default (the text is the
//   WHOLE setting). It is applied to the frame published to the API's spectrum ring (the picture every screen and the web page draw) for every open radio; the engine's own looks at the spectrum (centre_signal,
//   scan) see the raw frame. All off is the default and costs nothing. BAD_ARGUMENT for bad JSON or a value out of range (nothing changes); NO_CHANGE when it is in force already. fact.display.average = the JSON in force.
//   WF_HISTORY (SETTINGS; core/waterfall_history.hpp): pause and scroll back the waterfall. Per radio (target). ival[0]: 5 = switch the history for this radio on (ival[1] 1, num[0] = megabytes 16..1024, 0 = 192) or off
//   (ival[1] 0: the memory is freed); it is OFF by default (a history is up to 192 MiB a radio, allocated at the first frame after it is switched on). While on, every frame published for the radio is also kept as a line
//   of one byte a bin (0.59 dB steps over -150..0 dB). 0 = pause (hold the view where it is), 1 = back to live, 2 = scroll back num[0] lines and hold (num[1] = the window height in lines, default 256), 3 = scroll forward
//   num[0] lines (reaching the newest line returns to live), 4 = go to the line nearest the time num[0] (unix milliseconds). NO_CHANGE when the history is off, empty, or the view is where it was asked to be;
//   OUT_OF_RANGE for a count, size or time that is not one. The lines themselves are read by the host through WireA::waterfallCopy (the API has no call for them); fact.wf.<n>.* (on, megabytes, lines, behind, held,
//   slid, epoch, capacity) is the state as of the last change of anything in the fact block (it does not move on every frame).
#define FOXAPP_OP_DISPLAY_AVERAGE        0x881Bu /* text: JSON, see above */
#define FOXAPP_OP_WF_HISTORY             0x881Cu /* ival[0]: 0 pause / 1 live / 2 back / 3 forward / 4 seek / 5 on-off; num[0], num[1], ival[1]: see above. Addresses one radio (target) */

#define FOXAPP_OP_WIRE_A_FIRST           0x8800u
#define FOXAPP_OP_WIRE_A_LAST            0x88FFu

#define WIRE_A_KOPS \
    {FOXAPP_OP_HISTORY_STEP, "APP_HISTORY_STEP", true},     {FOXAPP_OP_CHANNEL, "APP_CHANNEL", true}, \
    {FOXAPP_OP_MARKER_EXPORT, "APP_MARKER_EXPORT", true}, \
    {FOXAPP_OP_MARKER_IMPORT, "APP_MARKER_IMPORT", true}, \
    {FOXAPP_OP_BOOKMARK_NOTE, "APP_BOOKMARK_NOTE", true}, \
    {FOXAPP_OP_BOOKMARK_FIND, "APP_BOOKMARK_FIND", true}, \
    {FOXAPP_OP_FAVOURITE_SAVE, "APP_FAVOURITE_SAVE", true}, \
    {FOXAPP_OP_FAVOURITE_RECALL, "APP_FAVOURITE_RECALL", true}, \
    {FOXAPP_OP_FAVOURITE_EDIT, "APP_FAVOURITE_EDIT", true}, \
    {FOXAPP_OP_PACK_EXPORT, "APP_PACK_EXPORT", true}, \
    {FOXAPP_OP_PACK_IMPORT, "APP_PACK_IMPORT", true}, \
    {FOXAPP_OP_SCAN_PLAN, "APP_SCAN_PLAN", true}, \
    {FOXAPP_OP_SCAN_BACK, "APP_SCAN_BACK", true}, \
    {FOXAPP_OP_SCAN_SAVE_HIT, "APP_SCAN_SAVE_HIT", true}, \
    {FOXAPP_OP_SCAN_TRIGGER, "APP_SCAN_TRIGGER", true}, \
    {FOXAPP_OP_SCANNER_EXTRAS, "APP_SCANNER_EXTRAS", true}, \
    {FOXAPP_OP_JOB_CANCEL, "APP_JOB_CANCEL", true}, \
    {FOXAPP_OP_EXPORT_BOX, "APP_EXPORT_BOX", true}, \
    {FOXAPP_OP_PROJECT, "APP_PROJECT", true}, \
    {FOXAPP_OP_ATTACH, "APP_ATTACH", true}, \
    {FOXAPP_OP_SIGMF_ANNOTATE, "APP_SIGMF_ANNOTATE", true}, \
    {FOXAPP_OP_KEY_ACTION, "APP_KEY_ACTION", true}, \
    {FOXAPP_OP_SET_RECORD_DAILY, "APP_SET_RECORD_DAILY", true}, \
    {FOXAPP_OP_RECORD_LOCK, "APP_RECORD_LOCK", true}, \
    {FOXAPP_OP_OVERLAY_LOAD, "APP_OVERLAY_LOAD", true}, \
    {FOXAPP_OP_OVERLAY_LOOKUP, "APP_OVERLAY_LOOKUP", true}, \
    {FOXAPP_OP_GRADIENT_SET, "APP_GRADIENT_SET", true}, \
    {FOXAPP_OP_DISPLAY_AVERAGE, "APP_DISPLAY_AVERAGE", true}, \
    {FOXAPP_OP_WF_HISTORY, "APP_WF_HISTORY", true},
// wire-a end

#endif  // CASCADE_CORE_WIRE_A_OPS_HPP
