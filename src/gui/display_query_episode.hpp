// display_query_episode.hpp - a run of frames in which the display could not be asked about,
// counted as ONE display change rather than one a frame.
//
// WHAT IT IS FOR. Tester log, 2026-10-28 (0.99.97 and 0.99.98, Intel UHD 630, a desktop whose
// monitor sleeps): "GLFW error 65544: Win32: Failed to query display settings" on EVERY frame for
// the three minutes the log still held, and for however long before that - the log is capped, and
// the session's start had scrolled out of it. The Dear ImGui backend asks GLFW for every monitor's
// video mode every frame (ImGui_ImplGlfw_UpdateMonitors, since 1.91.9), and while a monitor is
// detached, or an adapter is left with no display on it, EnumDisplaySettingsW fails every time it
// is asked. Each failure was then treated as a fresh display change: two log lines a frame (918 a
// minute), and the hang watchdog's ten-second grace restarted every frame - so the watchdog was
// OFF for the whole episode, which is exactly when sessions on that machine went wrong without a
// report. And the fifty GLFW lines a session is allowed were spent in three seconds, so the GLFW
// line that mattered later - 65543, the driver refusing a second context - was never written.
//
// THE RULE. Consecutive failures are one EPISODE: it begins on the first frame that failed, and
// ends once `quietS` seconds pass without a failure (the flood has gaps inside it - 0.9 s in the
// field log - which must not split it). The caller treats the beginning as the display change,
// with its one grace and its one log line, and the end as a line saying how long it lasted and
// how many queries failed. Failures counted before the frame loop started (a window built while
// the desktop is being reconfigured) are not an episode: the counter is taken as it stands.
//
// Pure, and driven by tests/test_display_query_episode.cpp: nothing in ctest can detach a monitor.
// The application end of it is tests/test_display_query_app.cpp, through the seam
// FOXSDR_TEST_DISPLAY_QUERY_FAIL.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_DISPLAY_QUERY_EPISODE_HPP
#define CASCADE_GUI_DISPLAY_QUERY_EPISODE_HPP

namespace cascade::gui {

class DisplayQueryEpisode {
public:
    // How long the queries must answer again before an episode is over. Twice the
    // longest gap seen inside a field flood (0.9 s), and well under the ten-second
    // grace a display change buys, so an episode's end is never mistaken for one.
    static constexpr double kQuietS = 2.0;

    struct Edge {
        bool began = false;
        bool ended = false;
        unsigned failures = 0;  // on `ended`: every failed query of the episode
        double seconds = 0.0;   // on `ended`: from its first failure to its last
    };

    // `seen` is the failure counter as it stands when the frame loop starts.
    explicit DisplayQueryEpisode(unsigned seen, double quietS = kQuietS)
        : seen_(seen), quietS_(quietS) {}

    // Once per frame, with the monotonic failure counter. Unsigned arithmetic, so a
    // counter that wrapped still yields the number of new failures.
    Edge update(double nowS, unsigned total) {
        Edge e;
        const unsigned fresh = total - seen_;
        seen_ = total;
        if (fresh > 0) {
            if (!open_) {
                open_ = true;
                beganS_ = nowS;
                failures_ = 0;
                e.began = true;
            }
            failures_ += fresh;
            lastS_ = nowS;
        } else if (open_ && nowS - lastS_ >= quietS_) {
            open_ = false;
            e.ended = true;
            e.failures = failures_;
            e.seconds = lastS_ - beganS_;
        }
        return e;
    }

    bool open() const { return open_; }
    unsigned failures() const { return failures_; }

private:
    unsigned seen_ = 0;
    double quietS_ = kQuietS;
    bool open_ = false;
    double beganS_ = 0.0;
    double lastS_ = 0.0;
    unsigned failures_ = 0;
};

}  // namespace cascade::gui

#endif  // CASCADE_GUI_DISPLAY_QUERY_EPISODE_HPP
