// Tests for gui/device_scan_plan.hpp - which SoapySDR drivers may be asked
// for devices while radios are open.
//
// The report (2026-09-23): the owner's B200 never appeared on the patch page
// while the receiver had an RTL-SDR open, because the SoapySDR scan was
// deferred outright. The danger that deferral guarded against is SoapyRTLSDR
// resetting a streaming RTL dongle - a SAME-FAMILY danger - so the plan now
// leaves out only the open radios' own drivers, and defers only when it cannot
// vouch for one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/device_scan_plan.hpp"

#include <string>
#include <vector>

#include "test_check.hpp"

using cascade::gui::OpenRadio;
using cascade::gui::planSoapyScan;
using cascade::gui::rowFromSkippedDriver;
using cascade::gui::SoapyScanMode;
using cascade::gui::soapyModulesForFamily;

namespace {

using Names = std::vector<std::string>;

void testNothingOpenIsAFullScan() {
    const auto p = planSoapyScan({}, 0, false);
    CHECK(p.mode == SoapyScanMode::Full);
    CHECK(p.skipDrivers.empty());
    // The generator and a file are not radios.
    const auto q = planSoapyScan({{"siggen", ""}, {"file", ""}}, 0, false);
    CHECK(q.mode == SoapyScanMode::Full);
    CHECK(q.skipDrivers.empty());
}

// THE OWNER'S CASE: an RTL-SDR in the receiver, looking for a B200.
void testAnOpenRtlSdrLeavesOutOnlyTheRtlDriver() {
    const auto p = planSoapyScan({{"rtlsdr", "serial=00000001"}}, 0, false);
    CHECK(p.mode == SoapyScanMode::SkipSome);
    CHECK(p.skipDrivers == Names({"rtlsdr"}));
    // uhd is not left out: that is the whole fix.
    CHECK(!rowFromSkippedDriver(p.skipDrivers, "driver=uhd,serial=31"));
    CHECK(rowFromSkippedDriver(p.skipDrivers, "driver=rtlsdr,serial=1"));
}

// A SoapySDR radio is known by its args' driver - and the receiver on a B200
// leaves only UHD out, so a second dongle can still be found.
void testAnOpenSoapyRadioLeavesOutItsOwnDriver() {
    const auto p = planSoapyScan({{"soapy", "driver=uhd,serial=31"}}, 0, false);
    CHECK(p.mode == SoapyScanMode::SkipSome);
    CHECK(p.skipDrivers == Names({"uhd"}));
    // Case in the args does not matter; nor do spaces round the '='.
    const auto q = planSoapyScan({{"SOAPY", "serial=1, Driver = HackRF"}}, 0, false);
    CHECK(q.skipDrivers == Names({"hackrf"}));
}

// The receiver's radio and every patch radio count, each family once.
void testSeveralRadiosUnionTheirFamilies() {
    const auto p = planSoapyScan({{"rtlsdr", "serial=1"},
                                  {"soapy", "driver=uhd,serial=31"},
                                  {"rtlsdr", "serial=2"},
                                  {"siggen", ""}},
                                 0, false);
    CHECK(p.mode == SoapyScanMode::SkipSome);
    CHECK(p.skipDrivers == Names({"rtlsdr", "uhd"}));
}

void testTheFamilyTable() {
    bool known = false;
    CHECK(soapyModulesForFamily("hackrf", "", known) == Names({"hackrf"}) && known);
    CHECK(soapyModulesForFamily("airspy", "", known) == Names({"airspy"}) && known);
    CHECK(soapyModulesForFamily("airspyhf", "", known) == Names({"airspyhf"}) && known);
    CHECK(soapyModulesForFamily("sdrplay", "", known) == Names({"sdrplay"}) && known);
    // A Mirics chip is claimed by the SDRplay module as well as SoapyMiri.
    CHECK(soapyModulesForFamily("mirisdr", "", known) == Names({"sdrplay", "miri", "mirisdr"}) &&
          known);
    CHECK(soapyModulesForFamily("rx888", "", known) == Names({"sddc"}) && known);
    CHECK(soapyModulesForFamily("pluto", "", known) == Names({"plutosdr"}) && known);
    CHECK(soapyModulesForFamily("siggen", "", known).empty() && known);
    // Unknown: never guessed.
    soapyModulesForFamily("newradio", "", known);
    CHECK(!known);
    soapyModulesForFamily("soapy", "serial=31", known);  // no driver named
    CHECK(!known);
}

// DEFERRED, exactly as before, whenever it cannot vouch for a driver.
void testItDefersWhenItCannotVouch() {
    // A radio still opening: its device may already be inside a driver.
    CHECK(planSoapyScan({}, 0, true).mode == SoapyScanMode::Defer);
    CHECK(planSoapyScan({{"rtlsdr", "serial=1"}}, 0, true).mode == SoapyScanMode::Defer);
    // A family nobody has told this table about.
    const auto u = planSoapyScan({{"rtlsdr", "serial=1"}, {"newradio", ""}}, 0, false);
    CHECK(u.mode == SoapyScanMode::Defer);
    CHECK(u.skipDrivers.empty());
    // A SoapySDR device open that no receiver or patch radio accounts for -
    // one the dead-device policy abandoned; its driver cannot be named.
    CHECK(planSoapyScan({}, 1, false).mode == SoapyScanMode::Defer);
    CHECK(planSoapyScan({{"soapy", "driver=uhd"}}, 1, false).mode == SoapyScanMode::Defer);
}

// A PATCH RADIO WAITS for a scan that may probe it (found in the hardware
// check: the page's first-frame scan started as the patch opened its radios).
void testWhatAScanInFlightMayProbe() {
    using cascade::gui::scanMayProbe;
    // A whole-bus scan may probe anything but the generator.
    CHECK(scanMayProbe(Names({}), "rtlsdr", "serial=1"));
    CHECK(scanMayProbe(Names({}), "soapy", "driver=uhd"));
    CHECK(!scanMayProbe(Names({}), "siggen", ""));
    // A scan that left rtlsdr out does not touch an RTL dongle...
    CHECK(!scanMayProbe(Names({"rtlsdr"}), "rtlsdr", "serial=2"));
    // ...but does ask uhd, so a B200 waits.
    CHECK(scanMayProbe(Names({"rtlsdr"}), "soapy", "driver=uhd,serial=31"));
    // A Mirics chip is only safe when every module that claims it was left out.
    CHECK(scanMayProbe(Names({"sdrplay"}), "mirisdr", ""));
    CHECK(!scanMayProbe(Names({"sdrplay", "miri", "mirisdr"}), "mirisdr", ""));
    // Unknown families are assumed probed.
    CHECK(scanMayProbe(Names({"rtlsdr"}), "newradio", ""));
    CHECK(scanMayProbe(Names({"rtlsdr"}), "soapy", "serial=9"));
}

void testRowsWithoutADriverAreNotKeptBlindly() {
    CHECK(!rowFromSkippedDriver(Names({"rtlsdr"}), "serial=1"));
    CHECK(!rowFromSkippedDriver(Names({}), "driver=rtlsdr"));
    CHECK(rowFromSkippedDriver(Names({"uhd"}), "type=b200,Driver=UHD,serial=31"));
}

}  // namespace

int main() {
    testNothingOpenIsAFullScan();
    testAnOpenRtlSdrLeavesOutOnlyTheRtlDriver();
    testAnOpenSoapyRadioLeavesOutItsOwnDriver();
    testSeveralRadiosUnionTheirFamilies();
    testTheFamilyTable();
    testItDefersWhenItCannotVouch();
    testWhatAScanInFlightMayProbe();
    testRowsWithoutADriverAreNotKeptBlindly();
    return testSummary("test_device_scan_plan");
}
