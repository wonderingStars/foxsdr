// Tests for engine/saved_radio_retry.hpp - when a saved radio that was merely BUSY
// at start-up is tried again - and for core::health::radioFailWhy, the reason
// word the two "did not open" log lines now carry.
//
// THE FIELD REPORT BEHIND BOTH (0.99.91, RTL-SDR Blog V4): the in-app updater
// launched the installer and closed the window, the installer started the new
// build while the old process was still letting go of the dongle, the start-up
// restore got "the radio is already in use by another program", and the session
// ran on the signal generator for good: spectrum, audio and squelch all alive,
// the GAIN dial reading N/A. Nothing tried the dongle again, and the log line
// said only "did not reopen", so the reason had to be read off the anonymous
// counts. Everything here is the pure half of the repair: no window, no
// pipeline, no dongle. (The wiring - the dial's tooltip - is held by
// test_deck_knobs_app.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "core/health_events.hpp"
#include "engine/saved_radio_retry.hpp"
#include "test_check.hpp"
#include "usb/usb_device.hpp"

namespace {

namespace gui = cascade::gui;
namespace health = cascade::core::health;

// The arguments in the order the function takes them, named, so a test says
// which one it is changing. Defaults describe the due-at-the-first-delay case.
struct Q {
    bool kept = true;
    bool busy = true;
    int attempts = 0;
    bool deviceOpen = false;
    bool openPending = false;
    bool scanPending = false;
    double now = 2.0;
    double lastFail = 0.0;
    bool due() const {
        return gui::savedRadioRetryDue(kept, busy, attempts, deviceOpen, openPending, scanPending,
                                       now, lastFail);
    }
};

}  // namespace

int main() {
    std::printf("saved radio retry\n");

    // --- the schedule --------------------------------------------------------
    {
        std::printf("  four tries, 2, 4, 8 and 16 s after the last failure\n");
        CHECK(gui::kSavedRadioRetryDelaysSec[0] == 2.0);
        CHECK(gui::kSavedRadioRetryDelaysSec[1] == 4.0);
        CHECK(gui::kSavedRadioRetryDelaysSec[2] == 8.0);
        CHECK(gui::kSavedRadioRetryDelaysSec[3] == 16.0);
    }

    // --- not before the delay, exactly at it ---------------------------------
    {
        std::printf("  not due before the delay, due exactly at it\n");
        Q q;
        q.now = 1.999;
        CHECK(!q.due());
        q.now = 2.0;
        CHECK(q.due());
        q.now = 600.0;
        CHECK(q.due());  // a late frame is still due

        // The delay is counted from the LAST failure, not from the start.
        q.attempts = 1;
        q.lastFail = 10.0;
        q.now = 13.999;
        CHECK(!q.due());
        q.now = 14.0;
        CHECK(q.due());
    }

    // --- each attempt has its own delay --------------------------------------
    {
        std::printf("  the delay for attempt N is 2, 4, 8, 16 s\n");
        const double want[4] = {2.0, 4.0, 8.0, 16.0};
        for (int a = 0; a < 4; ++a) {
            Q q;
            q.attempts = a;
            q.lastFail = 100.0;
            q.now = 100.0 + want[a] - 0.001;
            CHECK(!q.due());
            q.now = 100.0 + want[a];
            CHECK(q.due());
        }
        // Attempt 0's delay does not satisfy attempt 3.
        Q q;
        q.attempts = 3;
        q.lastFail = 0.0;
        q.now = 2.0;
        CHECK(!q.due());
    }

    // --- 0.0 is a real time, not "never" -------------------------------------
    {
        std::printf("  the start-up failure is stamped 0.0 and the first try still runs\n");
        Q q;
        q.lastFail = 0.0;
        q.now = 2.0;
        CHECK(q.due());
        q.now = 1.0;
        CHECK(!q.due());
    }

    // --- bounded: four tries, then the ordinary message stands ----------------
    {
        std::printf("  stops after four tries, however long ago\n");
        Q q;
        q.attempts = 4;
        q.lastFail = 0.0;
        q.now = 100000.0;
        CHECK(!q.due());
        q.attempts = 5;
        CHECK(!q.due());
        q.attempts = -1;  // never reads outside the table
        CHECK(!q.due());
    }

    // --- only a BUSY radio is waited for -------------------------------------
    {
        std::printf("  never when the last failure was not busy\n");
        Q q;
        q.now = 1000.0;
        CHECK(q.due());
        q.busy = false;
        CHECK(!q.due());
    }

    // --- only while something is kept ----------------------------------------
    {
        std::printf("  never with nothing kept (the user chose another source)\n");
        Q q;
        q.now = 1000.0;
        q.kept = false;
        CHECK(!q.due());
    }

    // --- nothing else may be in the driver stack ------------------------------
    {
        std::printf("  never with a device open, an open pending or a scan pending\n");
        Q q;
        q.now = 1000.0;
        CHECK(q.due());
        q.deviceOpen = true;
        CHECK(!q.due());
        q.deviceOpen = false;
        q.openPending = true;
        CHECK(!q.due());
        q.openPending = false;
        q.scanPending = true;
        CHECK(!q.due());
        q.scanPending = false;
        CHECK(q.due());
    }

    // --- health::radioFailWhy: the word the log lines carry --------------------
    {
        std::printf("  radioFailWhy: the reason class as one word, never the message\n");
        // THE FIELD CASE: the shared sentence both USB transports put in the
        // error when another process holds the radio.
        CHECK(health::radioFailWhy("rtlsdr", cascade::usb::kInUseError) == "busy");
        // ...which a transport may wrap in its own words.
        CHECK(health::radioFailWhy("rtlsdr", std::string("RTL-SDR: ") + cascade::usb::kInUseError +
                                                 " (serial 00000001)") == "busy");
        // Absent: the radio is not on the bus.
        CHECK(health::radioFailWhy("rtlsdr", "no RTL-SDR with serial 00000001 is present") ==
              "absent");
        // Nothing recognised is "other" (and an empty message too).
        CHECK(health::radioFailWhy("hackrf", "something unforeseen") == "other");
        CHECK(health::radioFailWhy("rtlsdr", "") == "other");
        // The same word the anonymous count files it under: one classification.
        CHECK(health::radioFailWhy("rtlsdr", cascade::usb::kInUseError) ==
              health::radioReasonWord(
                  health::classifyRadioOpen("rtlsdr", cascade::usb::kInUseError)));
        // Whatever the message quotes back (a serial, a path), the word has none of it.
        const std::string w = health::radioFailWhy(
            "rtlsdr", "no RTL-SDR with serial EDR04ZDB2 is present at C:\\Users\\someone");
        CHECK(w == "absent");
        CHECK(w.find("EDR04ZDB2") == std::string::npos);
    }

    return testSummary("test_saved_radio_retry");
}
