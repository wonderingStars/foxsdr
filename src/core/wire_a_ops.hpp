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

#define FOXAPP_OP_WIRE_A_FIRST           0x8800u
#define FOXAPP_OP_WIRE_A_LAST            0x88FFu

#define WIRE_A_KOPS \
    {FOXAPP_OP_HISTORY_STEP, "APP_HISTORY_STEP", true},     {FOXAPP_OP_CHANNEL, "APP_CHANNEL", true}, \
    {FOXAPP_OP_MARKER_EXPORT, "APP_MARKER_EXPORT", true}, \
    {FOXAPP_OP_MARKER_IMPORT, "APP_MARKER_IMPORT", true}, \
    {FOXAPP_OP_BOOKMARK_NOTE, "APP_BOOKMARK_NOTE", true}, \
    {FOXAPP_OP_BOOKMARK_FIND, "APP_BOOKMARK_FIND", true},
// wire-a end

#endif  // CASCADE_CORE_WIRE_A_OPS_HPP
