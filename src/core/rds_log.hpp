// rds_log.hpp - A LOG OF WHAT THE RDS DECODER READS: station name, programme type, radio text and the time (engine lane E1, FEATURE-GAPS G4-53).
//
// A user switch, OFF by default (wire-c settings rds.log, rds.log.folder, rds.log.name). While it is on, every change of what a radio's RDS decoder has
// recovered - the programme service name (PS), the programme type (PTY) or the radio text (RT) - is one line of a CSV file in the folder the user chose:
//
//     time_utc,radio,station_name,programme_type_code,programme_type,radio_text
//
// TEXT ONLY. No frequency, no PI code, no position and no identifier of the person is written, and nothing of this ever goes to a report: the file is the user's
// own. (A user's file-name template may name the frequency - {freq_mhz} - because the file is theirs; the line never does.)
//
// OFF THE AUDIO THREAD. observe() is called from the engine's control thread (the publish pass): it compares the snapshot with the last one logged for that radio,
// and when something differs hands one line to a queue under a short lock. A worker thread of its own does all the file work (create the folder, open for append,
// write, flush), so a slow or full disk can never stall a pass; a queue that grows past kMaxQueued drops the NEWEST lines and counts them (dropped()).
//
// FILE NAMES are the recorder's rules (core/rec_naming.hpp): the template defaults to "rds_{date}" (one file a day, appended to), tokens {date} {time} {datetime} {freq_mhz}
// {radio} {counter} ..., up to three folders below the chosen folder, the extension ".csv" added; a template the rules refuse is reported and nothing is written.
//
// A FAILURE is said, never swallowed: error() holds the sentence (fact rds.log.error) until the next good write.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "dsp/rds.hpp"

namespace cascade::core::rdslog {

inline constexpr std::size_t kMaxQueued = 512;           // lines waiting for the worker
inline constexpr std::size_t kMaxTextBytes = 64;         // a station name or radio text never passes this
inline constexpr const char* kDefaultTemplate = "rds_{date}";

// The programme type's name in the European RDS table (IEC 62106); "" for a code above 31. (North American RBDS names differ: the code column is always there.)
const char* programmeTypeName(int pty);

// One CSV field: quoted when it holds a comma, a quote or a line break; quotes doubled; control characters and bytes the RDS basic character set leaves undefined
// become '?'; at most kMaxTextBytes.
std::string csvField(const std::string& text);

std::string headerLine();
// "2026-11-16T09:30:12Z,1,BBC R4,4,Educate,Now: the shipping forecast\n". `utc` is whole seconds; the time is UTC.
std::string formatLine(std::int64_t unixMs, int radio, const std::string& ps, int pty, const std::string& radioText);

// What a decoder snapshot changes about the last line logged for its radio. True (and `ps`, `pty`, `rt` filled) when a line is due: the name is valid and differs,
// or the type differs while a name is known, or a non-empty radio text differs. Nothing is due for a snapshot with no valid name and no text.
struct Last { std::string ps; int pty = -1; std::string rt; };
bool changed(const Last& last, const dsp::RdsState& st);

struct Options {
    bool enabled = false;
    std::string folder;                         // absolute; required while enabled
    std::string nameTemplate = kDefaultTemplate;
    bool operator==(const Options& o) const { return enabled == o.enabled && folder == o.folder && nameTemplate == o.nameTemplate; }
};

class Logger {
public:
    Logger() = default;
    ~Logger() { stop(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Control thread. Takes effect at once; false with `why` set when the options are refused (nothing is written then; error() says it too).
    bool configure(const Options& o, std::string& why);
    // Control thread, at most a few times a second per radio. `frequencyHz` is only for the file-name template.
    void observe(int radio, const dsp::RdsState& st, std::int64_t unixMs, double frequencyHz);
    // Waits for the queue to be written (tests, shutdown), then stops the worker. Safe to call twice.
    void stop();
    void flushForTest();

    std::uint64_t written() const { return written_.load(); }
    std::uint64_t dropped() const { return dropped_.load(); }
    std::string error() const;
    std::string lastFileName() const;   // the file's NAME only (never the folder)

private:
    struct Item { std::int64_t unixMs; int radio; std::string line; double frequencyHz; };
    void workerMain();
    bool writeOne(const Item& it, std::string& why);

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    Options opt_;
    std::map<int, Last> last_;
    std::string error_;
    std::string lastFile_;
    std::thread worker_;
    bool stopping_ = false;
    bool busy_ = false;
    std::atomic<std::uint64_t> written_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::uint64_t counter_ = 0;
    std::string openPath_;
};

}  // namespace cascade::core::rdslog
