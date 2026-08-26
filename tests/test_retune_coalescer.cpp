// Tests for core/retune_coalescer.hpp — the pacing that turns a wheel burst
// (one hardware tune per frame) into at most one device call per interval,
// latest value winning.
//
// The clock is caller-supplied milliseconds, so every boundary here is tested
// to the millisecond with no sleeps and no flakiness.
//
// The mutation target is the interval comparison: a coalescer that applies
// every request defeats the pacing (the burst reaches the driver), and one
// that never applies strands the user's tune. Both directions are pinned:
// the burst block proves requests inside the interval are HELD, and the due()
// block proves the held value comes out exactly once, exactly on time.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/retune_coalescer.hpp"

#include <optional>

#include "test_check.hpp"

using cascade::core::RetuneCoalescer;

int main() {
    // --- First request applies immediately -------------------------------
    // A single tune (a preset click, a CAT command) must never be delayed.
    {
        RetuneCoalescer c(50.0);
        CHECK(c.request(100.0e6, 1000.0));
        CHECK(!c.hasPending());
    }

    // --- A burst inside the interval is held, latest value wins ----------
    {
        RetuneCoalescer c(50.0);
        CHECK(c.request(100.0e6, 1000.0));      // applies, stamps t=1000
        CHECK(!c.request(100.1e6, 1010.0));     // held
        CHECK(!c.request(100.2e6, 1020.0));     // replaces the held value
        CHECK(c.hasPending());
        // Not due yet: 49.9 ms after the last apply.
        CHECK(!c.due(1049.9).has_value());
        // Due at exactly the interval, and it is the LATEST request.
        const std::optional<double> hz = c.due(1050.0);
        CHECK(hz.has_value());
        CHECK(hz.has_value() && *hz == 100.2e6);
        // Released exactly once.
        CHECK(!c.due(1100.0).has_value());
        CHECK(!c.hasPending());
    }

    // --- due() paces the NEXT burst off its own release ------------------
    {
        RetuneCoalescer c(50.0);
        CHECK(c.request(100.0e6, 1000.0));
        CHECK(!c.request(100.1e6, 1001.0));
        CHECK(c.due(1050.0).has_value());       // released, stamps t=1050
        CHECK(!c.request(100.2e6, 1060.0));     // 10 ms later: held again
        CHECK(!c.due(1099.0).has_value());
        CHECK(c.due(1100.0).has_value());
    }

    // --- Requests spaced wider than the interval all apply immediately ---
    // The scanner's dwell cadence and ordinary single tunes must see no
    // change in behaviour at all.
    {
        RetuneCoalescer c(50.0);
        CHECK(c.request(100.0e6, 1000.0));
        CHECK(c.request(101.0e6, 1050.0));
        CHECK(c.request(102.0e6, 1120.0));
        CHECK(!c.hasPending());
    }

    // --- An immediate apply supersedes an older held value ---------------
    // request() returning true means the caller applies THAT value now; a
    // stale held one firing afterwards would re-tune backwards.
    {
        RetuneCoalescer c(50.0);
        CHECK(c.request(100.0e6, 1000.0));
        CHECK(!c.request(100.1e6, 1010.0));     // held
        CHECK(c.request(105.0e6, 1050.0));      // interval passed: applies now
        CHECK(!c.hasPending());                 // 100.1 MHz must NOT fire later
        CHECK(!c.due(1200.0).has_value());
    }

    // --- noteApplied() paces against out-of-band applies ------------------
    // The device-open carry-across tunes without going through request();
    // the next burst must still be paced off that apply.
    {
        RetuneCoalescer c(50.0);
        c.noteApplied(1000.0);
        CHECK(!c.request(100.0e6, 1010.0));     // inside the interval: held
        CHECK(c.due(1050.0).has_value());
    }

    // --- clearPending() drops a value aimed at a source that is gone ------
    {
        RetuneCoalescer c(50.0);
        CHECK(c.request(100.0e6, 1000.0));
        CHECK(!c.request(100.1e6, 1010.0));
        c.clearPending();
        CHECK(!c.hasPending());
        CHECK(!c.due(1100.0).has_value());
    }

    return testSummary("test_retune_coalescer");
}
