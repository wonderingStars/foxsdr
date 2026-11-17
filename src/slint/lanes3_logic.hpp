// lanes3_logic.hpp - SCREEN LANE S3 (docs/multi-radio-records/screen-s3-recplay.md): the pure rules behind the recording and playback controls of the Slint window - no Slint, no Win32,
// no engine call. The typed-field helpers (parseNumber, parseInteger, trim, numberText, Built) are lane L's.
//
//   file names          the template check with an example name, the token help (G3-08)
//   sample formats      the labels, the bytes a second costs (G3-07)
//   rollover            the size and time limits of one recording file (G3-10)
//   triggered clips     squelch-triggered recording, one file per transmission, the replay buffer in seconds and megabytes (G3-03, G3-09, G3-02)
//   timed recordings    a local date and time typed by a person, stored as UTC seconds, and the list read back in local time (G3-01)
//   playback            a position typed as seconds or m:ss, the loop, a raw file's stated format and rate (G3-13, G3-14a)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lanel_logic.hpp"

namespace foxsdr::slintlanes3 {

using foxsdr::slintlanel::Built;

// ============================================================ file names (G3-08) ============================================================
struct NameCheck {
    bool ok = false;
    std::string error;      // the model's own sentence for a template it refuses
    std::string example;    // what a take would be called ("100.5/WFM_20261112_103000.wav"); for "" the default naming
};
// An empty template is the default naming and is always fine. `utcOffsetMinutes` only decides the example's time of day.
NameCheck checkNameTemplate(const std::string& text);
// One line the person can read under the field: the tokens.
std::string nameTokensHelp();

// ============================================================ sample formats (G3-07) ============================================================
// The engine's own numbers: I/Q takes 0 = 32-bit float, 1 = 16-bit integer; audio takes 0 = 16-bit, 1 = 24-bit, 2 = 32-bit float.
const std::vector<std::string>& iqFormatNames();
const std::vector<std::string>& audioFormatNames();
// What a second of a take costs, "15.3 MB a second" (I/Q at `rateHz`, 2 values a frame) or "94 kB a second" (audio at `rateHz`, 1 value a frame).
std::string costPerSecond(bool iq, int format, double rateHz);
// The facts "I/Q 0, audio 0" read back: "iq=0;audio=2". False when it is not that.
bool parseFormatFact(const std::string& fact, int& iq, int& audio);

// ============================================================ rollover (G3-10) ============================================================
// Size in MB (1 to 100000; 0 = no size limit) and time in minutes (0.1 to 1440; 0 = no time limit); both may be set: the first one reached ends the file.
bool parseRolloverMB(const std::string& text, double& mb, std::string& err);
bool parseRolloverMinutes(const std::string& text, double& minutes, std::string& err);
// The text of RECORD_ROLLOVER: {"maxMB":n,"maxSeconds":s,"align":b}.
Built rolloverJson(const std::string& mbText, const std::string& minutesText, bool alignToClock);
// "100 MB or 10 minutes, on the clock" read back from the fact record.rollover; "" = no limits.
std::string rolloverReadout(const std::string& fact);

// ============================================================ triggered clips and the replay buffer (G3-03, G3-09, G3-02) ============================================================
struct ClipFields {
    bool enabled = false;
    std::string preRollS = "2", tailS = "3", minS = "1", maxClipS = "600";
};
// The text of RECORD_CLIPS: {"enabled":b,"preRollS":..,"tailS":..,"minS":..,"maxClipS":..} with the model's ranges (0..10, 0..30, 0..60, 10..3600).
Built clipsJson(const ClipFields& f);
bool clipFieldsFrom(const std::string& fact, ClipFields& out);
// The replay buffer: seconds of audio the radio keeps so that a take or a clip can begin before the key was pressed (0 = none). The engine's
// bound is `maxSeconds` (and 'maxBytes' in the fact); this says it in seconds and megabytes: "10 s = 0.9 MB".
bool parseReplaySeconds(const std::string& text, double maxSeconds, double& s, std::string& err);
std::string replayCost(double seconds, double audioRateHz);
// The engine's "seconds=5;bytes=480000;max=..." fact as a sentence with units: "5 s of audio held (0.5 MB)"; empty when the fact is not understood.
std::string replayHeldText(const std::string& fact);
// The text of RECORD_REPLAY: {"seconds":s}.
Built replayJson(const std::string& secondsText, double maxSeconds);

// ============================================================ timed recordings (G3-01) ============================================================
struct ScheduleFields {
    std::string name, date, time, lengthMin = "10";
    int radio = 1;
    int repeat = 0;                   // 0 once, 1 daily, 2 weekly
    bool tune = false;
    std::string freqMHz;
    int mode = 0;                     // 0 = leave the mode, 1..7 = FOXAPI_DEMOD_*
    std::string bandwidthKHz;
    bool recordAudio = true, recordIq = false, returnToWhereIWas = true;
};
// The offset of the machine's zone at an instant, minutes east of UTC. The window passes the system's; a test passes a fixed one.
using OffsetAt = std::function<int(std::int64_t unixMs)>;
// "2026-11-17" and "14:30" in LOCAL time, to UTC seconds. False with a plain sentence for a date or time that is not one (the 30th of February, 25:00).
// A local time that falls in the hour a clock change skips is refused; one that falls twice takes the first.
bool localToUnix(const std::string& date, const std::string& time, const OffsetAt& offsetAt, std::int64_t& unix, std::string& err);
// The same instant in local time: "2026-11-17 14:30" (or with ":SS" when the seconds are not 0).
std::string unixToLocal(std::int64_t unix, const OffsetAt& offsetAt);
// "2026-11-17" and "14:30" for the form's defaults.
void localDateTimeParts(std::int64_t unix, const OffsetAt& offsetAt, std::string& date, std::string& time);
// The text of SCHEDULE_SET (the engine's JSON). The engine judges every range again; this keeps a typo from being sent.
Built scheduleJson(const ScheduleFields& f, const OffsetAt& offsetAt);
// One entry of fact schedule.entry.<id>, said in a line: "Radio 1, 2026-11-17 14:30, 10 min, daily, 100.5 MHz WFM, audio + I/Q, waiting".
struct EntryView {
    bool ok = false;
    int id = 0;
    std::string name, line, state, note;
    bool enabled = true;
    int missed = 0;
};
EntryView entryView(const std::string& entryJson, const OffsetAt& offsetAt);
// The words of an entry's state: waiting, recording, missed, refused, skipped, disabled, done.
std::string stateWords(const std::string& state);

// ============================================================ playback (G3-13, G3-14a) ============================================================
// A place in the recording typed as seconds ("90", "12.5") or m:ss / h:mm:ss ("1:30", "1:02:03"); 0 up to `lengthS`.
bool parsePosition(const std::string& text, double lengthS, double& seconds, std::string& err);
// "1:30.5" for 90.5 s; "0:05" for 5 s; "1:02:03" past an hour.
std::string positionText(double seconds);
// The loop's two ends: a < b <= length.
bool parseLoop(const std::string& aText, const std::string& bText, double lengthS, double& a, double& b, std::string& err);

// A raw file has no header, so the person states the sample format and rate.
const std::vector<std::string>& rawFormatNames();     // "8-bit unsigned (cu8)", "16-bit signed (cs16)", "32-bit float (cf32)"
// A rate typed as 2048000, "2.048M", "250k", "2 000 000": 1 kS/s to 1 GS/s.
bool parseRate(const std::string& text, double& hz, std::string& err);
// Hz typed as MHz ("100.5") or with a unit ("7050k", "100.5M", "7050000"): 0 up to 1e13.
bool parseCentre(const std::string& text, double& hz, std::string& err);
// The engine's source id for a raw file, "file-raw:cu8,2048000,100500000|<path>"; the path may hold any character.
Built rawFileId(const std::string& path, int formatIndex, const std::string& rateText, const std::string& centreText);
// The pieces of such an id (the engine's side): false when it is not one.
struct RawId { std::string format, path; double rateHz = 0.0, centreHz = 0.0; bool hasCentre = false; };
bool parseRawFileId(const std::string& args, RawId& out);

// ============================================================ whole-file waterfall (G3-14b) ============================================================
// The text of FILE_OVERVIEW: {"action":"start","rows":n,"fft":n}; and "cancel".
Built overviewJson(const std::string& rowsText, int fftIndex);
const std::vector<std::string>& overviewFftNames();   // 512, 1024, 2048, 4096
int overviewFftSize(int index);

}  // namespace foxsdr::slintlanes3
