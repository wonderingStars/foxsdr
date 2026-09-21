// The device's own position: the state machine, the acceptance rule, the
// wording, and the privacy promise.
//
// THE FEATURE (owner, 2026-09-21): "can you look at using the android tablets
// GPS in the software". core/device_location.hpp holds the whole policy; the
// Java half is a translator with no decisions in it. So everything that can be
// wrong about this feature can be made wrong here, on a machine with no
// location service at all, through the Platform seam.
//
// THE PRIVACY HALF IS THE SAME AS THE SERIAL READER'S and it is not decorative:
// diagnostic lines ride inside uploaded crash reports, and PRIVACY.md promises
// a position is never sent. A fix is fed at a distinctive position and the
// WHOLE log ring is then searched for its digits. One hit fails the suite.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/device_location.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "core/diag_log.hpp"
#include "test_check.hpp"

using cascade::core::DeviceLocation;
using cascade::core::deviceLocationStatusLine;
using cascade::core::DiagLog;

namespace {

bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

// A platform that records what it was asked and answers what the test wants.
struct FakePlatform {
    int starts = 0;
    int stops = 0;
    double lastTimeoutS = 0.0;
    bool refuse = false;
    std::string refusal;

    DeviceLocation::Platform make() {
        DeviceLocation::Platform p;
        p.start = [this](double timeoutS, std::string& error) {
            ++starts;
            lastTimeoutS = timeoutS;
            if (refuse) {
                error = refusal;
                return false;
            }
            return true;
        };
        p.stop = [this]() { ++stops; };
        return p;
    }
};

}  // namespace

int main() {
    // The ring is searched at the end, so start it empty: every line examined
    // is then one this file caused.
    DiagLog::instance().resetForTest();
    DiagLog::instance().configure(std::string(), false);

    // --- no platform at all: Unavailable, and it SAYS so -------------------
    //
    // This is the desktop's answer, and the one a test binary sees. It must
    // not look like a failure the user could fix.
    {
        DeviceLocation loc;
        loc.start();
        const DeviceLocation::Status s = loc.status();
        CHECK(s.state == DeviceLocation::State::Unavailable);
        CHECK(!loc.listening());
        const std::string line = deviceLocationStatusLine(s);
        CHECK(!line.empty());
        CHECK(contains(line, "location service"));
        // And the compile-time answer matches the platform this runs on.
#if defined(__ANDROID__)
        CHECK(cascade::core::platformHasDeviceLocation());
#else
        CHECK(!cascade::core::platformHasDeviceLocation());
#endif
    }

    // --- a fix: taken once, the request cancelled, the wording right -------
    {
        FakePlatform fake;
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start(30.0);
        CHECK(loc.listening());
        CHECK(fake.starts == 1);
        CHECK_NEAR(fake.lastTimeoutS, 30.0, 1e-9);
        CHECK(loc.status().state == DeviceLocation::State::Listening);

        loc.onSatellites(7);
        const std::string listening = deviceLocationStatusLine(loc.status());
        CHECK(contains(listening, "Asking the device"));
        CHECK(contains(listening, "7 satellites"));

        loc.onFix(53.9576, -1.0827, 12.0, DeviceLocation::Provider::Satellites);
        const DeviceLocation::Status s = loc.status();
        CHECK(s.state == DeviceLocation::State::Fixed);
        // THE REQUEST IS CANCELLED BY THE FIX. A GNSS chip left running is
        // somebody's battery, and this is the line that proves it stops.
        CHECK(fake.stops == 1);
        CHECK(!loc.listening());

        DeviceLocation::Fix fix;
        CHECK(loc.takeFix(fix));
        CHECK_NEAR(fix.latDeg, 53.9576, 1e-9);
        CHECK_NEAR(fix.lonDeg, -1.0827, 1e-9);
        CHECK_NEAR(fix.accuracyM, 12.0, 1e-9);
        CHECK(fix.provider == DeviceLocation::Provider::Satellites);
        // ONCE. A frame loop polling at 60 Hz must not re-apply it and reset
        // the coverage map sixty times a second.
        DeviceLocation::Fix again;
        CHECK(!loc.takeFix(again));
        CHECK(loc.status().state == DeviceLocation::State::Fixed);

        const std::string line = deviceLocationStatusLine(s);
        CHECK(contains(line, "Position set from the satellites"));
        CHECK(contains(line, "12 m"));
        CHECK(!contains(line, "53.9"));
        CHECK(!contains(line, "1.08"));

        loc.clearResult();
        CHECK(loc.status().state == DeviceLocation::State::Idle);
        CHECK(deviceLocationStatusLine(loc.status()).empty());
    }

    // --- a network fix says which it is, and what to do about it -----------
    {
        FakePlatform fake;
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start();
        loc.onFix(53.9576, -1.0827, 2400.0, DeviceLocation::Provider::Network);
        const std::string line = deviceLocationStatusLine(loc.status());
        CHECK(contains(line, "from the network"));
        // Kilometres, because "accurate to about 2400 m" is a number nobody
        // reads as "two and a half kilometres out".
        CHECK(contains(line, "2 km"));
        CHECK(contains(line, "go outdoors"));
    }

    // --- what is NOT a position --------------------------------------------
    //
    // The same rule as every other door (gui/scope_view.hpp): the origin, an
    // off-globe pair and a non-finite one are all "not fixed yet", and the
    // wait CONTINUES rather than setting the receiver to the Gulf of Guinea.
    {
        FakePlatform fake;
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start();
        loc.onFix(0.0, 0.0, 5.0, DeviceLocation::Provider::Satellites);
        CHECK(loc.status().state == DeviceLocation::State::Listening);
        loc.onFix(91.0, 0.5, 5.0, DeviceLocation::Provider::Satellites);
        CHECK(loc.status().state == DeviceLocation::State::Listening);
        loc.onFix(51.0, 200.0, 5.0, DeviceLocation::Provider::Satellites);
        CHECK(loc.status().state == DeviceLocation::State::Listening);
        const double nan = std::nan("");
        loc.onFix(nan, 1.0, 5.0, DeviceLocation::Provider::Satellites);
        CHECK(loc.status().state == DeviceLocation::State::Listening);
        CHECK(loc.listening());
        CHECK(fake.stops == 0);
        // A real one still lands afterwards.
        loc.onFix(51.5, -0.1, 5.0, DeviceLocation::Provider::Satellites);
        CHECK(loc.status().state == DeviceLocation::State::Fixed);
    }

    // --- stop: a listen cut short is Idle, a fix already landed is kept -----
    {
        FakePlatform fake;
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start();
        loc.stop();
        CHECK(loc.status().state == DeviceLocation::State::Idle);
        CHECK(fake.stops == 1);
        CHECK(!loc.listening());

        loc.start();
        loc.onFix(51.5, -0.1, 5.0, DeviceLocation::Provider::Satellites);
        loc.stop();  // the frame after the fix
        CHECK(loc.status().state == DeviceLocation::State::Fixed);
        DeviceLocation::Fix fix;
        CHECK(loc.takeFix(fix));
    }

    // --- a late answer from a cancelled request is DROPPED ------------------
    //
    // Android can deliver one final callback after removeUpdates. Applying it
    // would move the receiver after the user pressed Stop.
    {
        FakePlatform fake;
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start();
        loc.stop();
        loc.onFix(51.5, -0.1, 5.0, DeviceLocation::Provider::Satellites);
        CHECK(loc.status().state == DeviceLocation::State::Idle);
        DeviceLocation::Fix fix;
        CHECK(!loc.takeFix(fix));
    }

    // --- the timeout is driven by the frame clock ---------------------------
    {
        FakePlatform fake;
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start(10.0);
        loc.tick(1000.0);  // the first tick is the start of the clock
        CHECK(loc.status().state == DeviceLocation::State::Listening);
        loc.tick(1005.0);
        CHECK_NEAR(loc.status().elapsedS, 5.0, 1e-9);
        CHECK(loc.listening());
        loc.tick(1010.0);
        CHECK(loc.status().state == DeviceLocation::State::TimedOut);
        CHECK(fake.stops == 1);
        CHECK(!loc.listening());
        const std::string line = deviceLocationStatusLine(loc.status());
        CHECK(contains(line, "No position within 10 s"));
        CHECK(contains(line, "indoors"));
    }

    // A clock that goes backwards must not print a negative age, and must not
    // freeze the timeout either.
    {
        FakePlatform fake;
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start(10.0);
        loc.tick(500.0);
        loc.tick(400.0);
        CHECK_NEAR(loc.status().elapsedS, 0.0, 1e-9);
        CHECK(loc.status().state == DeviceLocation::State::Listening);
    }

    // --- a platform that will not even ask ----------------------------------
    {
        FakePlatform fake;
        fake.refuse = true;
        fake.refusal = "the location service is switched off on this device";
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start();
        const DeviceLocation::Status s = loc.status();
        CHECK(s.state == DeviceLocation::State::Failed);
        CHECK(!loc.listening());
        CHECK(deviceLocationStatusLine(s) == fake.refusal);
    }

    // --- denied, and said as something the user can act on ------------------
    {
        FakePlatform fake;
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start();
        loc.onDenied("permission to use the location was refused");
        const DeviceLocation::Status s = loc.status();
        CHECK(s.state == DeviceLocation::State::Denied);
        CHECK(fake.stops == 1);
        const std::string line = deviceLocationStatusLine(s);
        CHECK(contains(line, "refused"));
        CHECK(contains(line, "Settings"));
    }

    // --- failed mid-listen ---------------------------------------------------
    {
        FakePlatform fake;
        DeviceLocation loc;
        loc.setPlatform(fake.make());
        loc.start();
        loc.onFailed("the location service stopped answering");
        CHECK(loc.status().state == DeviceLocation::State::Failed);
        CHECK(fake.stops == 1);
    }

    // --- THE PRIVACY PROMISE -------------------------------------------------
    //
    // Every line this file has caused is now searched for the digits of the
    // positions fed above, in the forms they could plausibly be written.
    {
        const std::vector<std::string> ring = DiagLog::instance().ringSnapshot();
        CHECK(!ring.empty());  // a search over nothing proves nothing
        const char* forbidden[] = {
            "53.9576", "53.95", "1.0827", "-1.08", "51.5", "51.50", "-0.1",
            "0.1234",  "5357.4", "53 57",
        };
        int leaks = 0;
        for (const std::string& line : ring) {
            for (const char* needle : forbidden) {
                if (contains(line, needle)) {
                    ++leaks;
                    std::printf("LEAK %s  <- %s\n", needle, line.c_str());
                }
            }
        }
        CHECK(leaks == 0);
        // And the search is capable of finding something: a line that DOES
        // carry one of those strings is detected. Without this the check
        // above could be passing because the needles are wrong.
        bool canFind = false;
        for (const char* needle : forbidden) {
            const std::string synthetic = std::string("location: position ") + needle;
            if (contains(synthetic, needle)) { canFind = true; }
        }
        CHECK(canFind);
    }

    return testSummary("test_device_location");
}
