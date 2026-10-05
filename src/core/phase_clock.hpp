// phase_clock.hpp - where a long operation on the GUI thread spent its time.
//
// WHY THIS EXISTS. A session's log showed a plugin rescan that took two minutes
// (the line announcing it, and the lines announcing the plugins it loaded, were
// 119.976 s apart) and said nothing about which step it spent them in. That is
// the whole of what the log could say, because a rescan is one function that
// does six different kinds of work in a row - it calls every plugin's destroy()
// (third-party code, and for a plugin with a worker thread a join), unmaps every
// module (a DllMain and static destructors under the loader lock), re-hashes
// every installed file against the install record, lists the folder, and maps
// and validates every module again - and any one of them can wait on a disk, a
// network, a thread or a lock that is not this program's. The six have six
// different owners and six different fixes, and a freeze that cannot be told
// apart from the others is a freeze nobody can act on.
//
// WHAT IT IS. A stopwatch with named laps, and one sentence about them:
// "plugins: reloaded in 3.6 s (decoders 0.0, patch 0.0, panels and map 0.2,
// unload 0.3, inventory 1.9, load 1.1, restart 0.1)". When the total reaches the
// frame threshold it is a warning that says so - the window did not draw for
// that long - and names the slowest lap, because that is the sentence a report
// is read for.
//
// WHAT IT IS NOT. It does not move anything off the GUI thread, bound anything
// or excuse anything: it measures. The fix for a plugin that will not stop is a
// design decision (docs/DIAGNOSTICS.md, "A plugin rescan runs on the GUI
// thread"); this is what lets the next field report say which plugin step it is
// the fix for.
//
// NO NAMES, NO PATHS, NO NUMBERS BUT TIMES. The log rides inside uploaded
// reports (PRIVACY.md), so a lap is named by a fixed word of the caller's - never
// a plugin, a file or a folder.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PHASE_CLOCK_HPP
#define CASCADE_CORE_PHASE_CLOCK_HPP

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/diag_log.hpp"

namespace cascade::core {

class PhaseClock {
public:
    // Seconds on any monotonic clock. Injectable so a test can say what a lap
    // cost without waiting for it.
    using NowFn = std::function<double()>;

    struct Lap {
        std::string name;
        double seconds = 0.0;
    };

    PhaseClock() : now_(steadySeconds) {}
    explicit PhaseClock(NowFn now) : now_(std::move(now)) {}

    // Closes the lap in progress, if any, and opens `name`. A name that has been
    // opened before ADDS to its lap rather than starting a second one, so a
    // sequence that visits a step twice reads as one figure for the step.
    void begin(const char* name) {
        close();
        open_ = true;
        openName_ = name != nullptr ? name : "";
        openedAt_ = now_();
    }

    // Closes the lap in progress, if any. Safe to call twice, and safe to call
    // with nothing open.
    void end() { close(); }

    // The laps in the order they were first opened.
    const std::vector<Lap>& laps() const { return laps_; }

    // The sum of the CLOSED laps. A lap still open is not in it until end().
    double totalSeconds() const {
        double t = 0.0;
        for (const Lap& l : laps_) { t += l.seconds; }
        return t;
    }

    // The longest closed lap, or nullptr when there is none. The earlier of two
    // equal laps wins, so the answer does not change with floating-point noise.
    const Lap* slowest() const {
        const Lap* best = nullptr;
        for (const Lap& l : laps_) {
            if (best == nullptr || l.seconds > best->seconds) { best = &l; }
        }
        return best;
    }

    // The sentence for the log. `what` is the caller's own prefix ("plugins:
    // reload"); `warnAtSeconds` is where "took" becomes "froze the window".
    // Seconds to one decimal, because the question is which lap was the minutes
    // and which were the milliseconds.
    std::string describe(const char* what, double warnAtSeconds) const {
        const double total = totalSeconds();
        std::string out = what != nullptr ? what : "";
        char buf[96];
        if (total >= warnAtSeconds) {
            std::snprintf(buf, sizeof(buf), " took %.1f s - the window did not draw for that long",
                          total);
            out += buf;
            if (const Lap* s = slowest()) {
                std::snprintf(buf, sizeof(buf), "; slowest step: %s %.1f s", s->name.c_str(),
                              s->seconds);
                out += buf;
            }
        } else {
            std::snprintf(buf, sizeof(buf), " took %.1f s", total);
            out += buf;
        }
        if (!laps_.empty()) {
            out += " (";
            for (std::size_t i = 0; i < laps_.size(); ++i) {
                if (i != 0) { out += ", "; }
                std::snprintf(buf, sizeof(buf), " %.1f", laps_[i].seconds);
                out += laps_[i].name;
                out += buf;
            }
            out += ")";
        }
        return out;
    }

    // Writes the sentence to the application log: a warning when the total has
    // reached `warnAtSeconds`, an ordinary line otherwise.
    void log(const char* what, double warnAtSeconds) const {
        const std::string line = describe(what, warnAtSeconds);
        if (totalSeconds() >= warnAtSeconds) {
            diagWarnf("%s", line.c_str());
        } else {
            diagLogf("%s", line.c_str());
        }
    }

private:
    static double steadySeconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void close() {
        if (!open_) { return; }
        open_ = false;
        const double lap = now_() - openedAt_;
        for (Lap& l : laps_) {
            if (l.name == openName_) {
                l.seconds += lap;
                return;
            }
        }
        laps_.push_back(Lap{openName_, lap});
    }

    NowFn now_;
    std::vector<Lap> laps_;
    bool open_ = false;
    std::string openName_;
    double openedAt_ = 0.0;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PHASE_CLOCK_HPP
