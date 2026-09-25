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
    // A Mirics chip is claimed by the SDRplay module as well as SoapyMiri -
    // and the claim runs BOTH WAYS. An RSP1/RSP1A/RSP2 opened through the
    // native SDRplay API driver is still a Mirics chip on the bus, so
    // SoapyMiri's probe can reach it exactly as SoapySDRPlay's can reach a
    // unit opened by the native Mirics driver. Through 0.99.34 "sdrplay" left
    // out only {"sdrplay"}, and a scan beside an open RSP still asked miri.
    CHECK(soapyModulesForFamily("sdrplay", "", known) == Names({"sdrplay", "miri", "mirisdr"}) &&
          known);
    CHECK(soapyModulesForFamily("mirisdr", "", known) == Names({"sdrplay", "miri", "mirisdr"}) &&
          known);
    // The same unit opened THROUGH SoapySDR, by either module, is the same
    // chip with the same two modules able to probe it.
    CHECK(soapyModulesForFamily("soapy", "driver=sdrplay,serial=1", known) ==
              Names({"sdrplay", "miri", "mirisdr"}) &&
          known);
    CHECK(soapyModulesForFamily("soapy", "driver=miri,index=0", known) ==
              Names({"sdrplay", "miri", "mirisdr"}) &&
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
    // ...and so is an RSP opened through the SDRplay API: a scan that left
    // out only the SDRplay module still asks SoapyMiri, which can reach it.
    CHECK(scanMayProbe(Names({"sdrplay"}), "sdrplay", ""));
    CHECK(!scanMayProbe(Names({"sdrplay", "miri", "mirisdr"}), "sdrplay", ""));
    // THE REPORTED CASE, through the plan: an RSP open on the native SDRplay
    // driver leaves every module that claims a Mirics chip out of the scan.
    const auto rsp = planSoapyScan({{"sdrplay", "serial=1811003EFB"}}, 0, false);
    CHECK(rsp.mode == SoapyScanMode::SkipSome);
    CHECK(rsp.skipDrivers == Names({"sdrplay", "miri", "mirisdr"}));
    // Unknown families are assumed probed.
    CHECK(scanMayProbe(Names({"rtlsdr"}), "newradio", ""));
    CHECK(scanMayProbe(Names({"rtlsdr"}), "soapy", "serial=9"));
}

void testRowsWithoutADriverAreNotKeptBlindly() {
    CHECK(!rowFromSkippedDriver(Names({"rtlsdr"}), "serial=1"));
    CHECK(!rowFromSkippedDriver(Names({}), "driver=rtlsdr"));
    CHECK(rowFromSkippedDriver(Names({"uhd"}), "type=b200,Driver=UHD,serial=31"));
}

// --- F204602B5329B268: UHD IS ASKED ONLY WHEN A USRP COULD BE HERE ---------
//
// The child probing driver=uhd died on a machine whose only radio was an
// SDRplay RSPdx. The rule, with a fake USB list standing in for the SetupAPI
// / sysfs listing: no USRP on the bus -> uhd left out; a USRP plugged in, a
// saved or open UHD source, or the network switch -> asked.
void testUhdIsAskedOnlyWhenAUsrpCouldBeHere() {
    using cascade::gui::isUsrpUsbId;
    using cascade::gui::soapyDriversWithNoHardware;
    using cascade::gui::UsbVidPid;
    const Names uhd{"uhd"};

    // The field machine: an SDRplay RSPdx (1df7:3060) and ordinary USB kit.
    const std::vector<UsbVidPid> sdrplayOnly{{0x1DF7, 0x3060}, {0x046D, 0xC52B}, {0x8087, 0x0026}};
    CHECK(soapyDriversWithNoHardware(sdrplayOnly, true, Names({}), false) == uhd);
    // An empty bus is still a listing that answered.
    CHECK(soapyDriversWithNoHardware({}, true, Names({}), false) == uhd);
    // Soapy args naming other drivers do not bring uhd back.
    CHECK(soapyDriversWithNoHardware(sdrplayOnly, true,
                                     Names({"driver=sdrplay,serial=1", "", "driver=rtlsdr"}),
                                     false) == uhd);

    // A USRP ON THE BUS, every id UHD itself searches (b200_iface.hpp /
    // uhd-usrp.rules): each alone brings uhd back.
    const std::vector<UsbVidPid> usrps{{0x2500, 0x0020},   // B200/B210
                                       {0x2500, 0x0021},   // B200mini
                                       {0x2500, 0x0022},   // B205mini
                                       {0x2500, 0x0023},   // B206mini
                                       {0x2500, 0x0002},   // B100
                                       {0x2500, 0x7777},   // any later Ettus product
                                       {0x3923, 0x7813},   // NI-branded B200
                                       {0x3923, 0x7814},   // NI-branded B210
                                       {0xFFFE, 0x0002}};  // USRP1
    for (const UsbVidPid& id : usrps) {
        CHECK(isUsrpUsbId(id));
        std::vector<UsbVidPid> bus = sdrplayOnly;
        bus.push_back(id);
        CHECK(soapyDriversWithNoHardware(bus, true, Names({}), false).empty());
    }
    // National Instruments makes much that is not a USRP, and a bare FX3
    // bootloader is not something b200_find looks for.
    for (const UsbVidPid& id : std::vector<UsbVidPid>{{0x3923, 0x7812},
                                                      {0x3923, 0x7815},
                                                      {0x3923, 0x7166},
                                                      {0x04B4, 0x00F3},
                                                      {0x04B4, 0x00F0},
                                                      {0xFFFE, 0x0003},
                                                      {0x2501, 0x0020}}) {
        CHECK(!isUsrpUsbId(id));
        std::vector<UsbVidPid> bus = sdrplayOnly;
        bus.push_back(id);
        CHECK(soapyDriversWithNoHardware(bus, true, Names({}), false) == uhd);
    }

    // A SAVED OR OPEN UHD SOURCE: the user has a USRP - on the network, or
    // switched off today - and its args are how the scan knows. Case and
    // spacing as a hand-edited config might spell them.
    CHECK(soapyDriversWithNoHardware(sdrplayOnly, true, Names({"driver=uhd,serial=31"}), false)
              .empty());
    CHECK(soapyDriversWithNoHardware(sdrplayOnly, true,
                                     Names({"", "type=usrp2, Driver = UHD ,addr=192.168.10.2"}),
                                     false)
              .empty());

    // THE NETWORK SWITCH.
    CHECK(soapyDriversWithNoHardware(sdrplayOnly, true, Names({}), true).empty());

    // A LISTING THAT FAILED is not an empty bus: absence is unknown, and the
    // probe runs as it always did.
    CHECK(soapyDriversWithNoHardware({}, false, Names({}), false).empty());
}

// THE HINT UNDER THE SOURCE LIST (review of 5e7b968): shown exactly when the
// scan left UHD out, and never otherwise - driven from the same answer the
// scan used, end to end through the rule above.
void testTheNetworkUsrpHintOnlyWhenUhdWasSkipped() {
    using cascade::gui::networkUsrpHint;
    using cascade::gui::soapyDriversWithNoHardware;
    using cascade::gui::UsbVidPid;
    const std::string want =
        "Network USRPs are not searched - tick Look for network USRPs to include them";
    const std::vector<UsbVidPid> sdrplayOnly{{0x1DF7, 0x3060}};
    const std::vector<UsbVidPid> withB200{{0x1DF7, 0x3060}, {0x2500, 0x0020}};

    // Skipped: the hint, word for word (it is a catalogue key).
    const char* skipped =
        networkUsrpHint(soapyDriversWithNoHardware(sdrplayOnly, true, Names({}), false));
    CHECK(skipped != nullptr);
    CHECK(skipped != nullptr && want == skipped);
    CHECK(networkUsrpHint(Names({"UHD"})) != nullptr);

    // Asked, every way the rule asks: no hint.
    CHECK(networkUsrpHint(soapyDriversWithNoHardware(withB200, true, Names({}), false)) ==
          nullptr);
    CHECK(networkUsrpHint(soapyDriversWithNoHardware(sdrplayOnly, true, Names({}), true)) ==
          nullptr);
    CHECK(networkUsrpHint(soapyDriversWithNoHardware(
              sdrplayOnly, true, Names({"driver=uhd,addr=192.168.10.2"}), false)) == nullptr);
    CHECK(networkUsrpHint(soapyDriversWithNoHardware({}, false, Names({}), false)) == nullptr);
    // Other drivers left out are not UHD.
    CHECK(networkUsrpHint(Names({})) == nullptr);
    CHECK(networkUsrpHint(Names({"rtlsdr", "uhdx", "sdrplay"})) == nullptr);
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
    testUhdIsAskedOnlyWhenAUsrpCouldBeHere();
    testTheNetworkUsrpHintOnlyWhenUhdWasSkipped();
    return testSummary("test_device_scan_plan");
}
