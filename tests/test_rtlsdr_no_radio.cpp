// test_rtlsdr_no_radio.cpp - AN RTL-SDR OPEN THAT FINDS NO RADIO SAYS WHICH OF THREE THINGS IT IS (field survey 2026-11-24, report 28, rank 2).
//
// The open listed only WinUSB-bound dongles, so "plugged in on the wrong Windows driver" and "not plugged in" gave one sentence ("no RTL-SDR is bound to WinUSB on this
// machine"): 36 installs, 30 of the reports start-up restores. Now, over a FAKE bus (RtlBusView: the bound list, the unbound list, the whole-bus listing - nothing real is
// read, nothing opened):
//   N1  THE DECISION (rtlopen::decide), every combination of its three facts;
//   N2  THE OPEN'S OWN ERROR for each case, for a plain open and for a saved radio's serial: (a) wrong driver, (b) not plugged in, (c) cannot tell - three different
//       sentences, and the class word each files under: bind, absent, other (no new report word);
//   N3  THE SENTENCES: pairwise different; (a) says the driver can be installed by FoxSDR, (b) says plugged in, and the old "bound to WinUSB" sentence is gone;
//   N4  THE ENGINE'S TEST: the sentence wrapped as the engine wraps it ("<radio> did not open: ... The receiver is running on the signal generator.") is still read as
//       "wrong driver" for (a) and for no other sentence, nor for any other error text;
//   N5  NOTHING ELSE MOVES: a bound dongle list that is not empty never asks the unbound list or the bus (no extra scan on the way to a good open), and the
//       "no RTL-SDR with serial ... is present" and "no RTL-SDR at that index" sentences are word for word what they were;
//   N6  THE OTHER RADIO FAMILIES' "not bound" sentences are untouched (the HackRF's, the Airspy's keep their class and words).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "core/health_events.hpp"
#include "core/rtl_open_case.hpp"
#include "core/source_fallback.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_device.hpp"

namespace rtlopen = cascade::core::rtlopen;
namespace health = cascade::core::health;
namespace src = cascade::source;
namespace usb = cascade::usb;

namespace {

usb::UsbDeviceInfo dongle(const std::string& serial) {
    usb::UsbDeviceInfo d;
    d.vid = 0x0BDA;
    d.pid = 0x2838;
    d.serial = serial;
    d.description = "RTL2838UHIDIR";
    return d;
}

struct Bus {
    std::vector<usb::UsbDeviceInfo> bound, unbound;
    bool listed = true;
    std::vector<usb::UsbId> present;
    int boundCalls = 0, unboundCalls = 0, presentCalls = 0;
};

src::RtlBusView viewOf(Bus& b) {
    src::RtlBusView v;
    v.bound = [&b] { ++b.boundCalls; return b.bound; };
    v.unbound = [&b] { ++b.unboundCalls; return b.unbound; };
    v.present = [&b](std::vector<usb::UsbId>& out) { ++b.presentCalls; out = b.present; return b.listed; };
    return v;
}

std::string openError(Bus& b, const std::string& args) {
    src::RtlSdrSource s;
    const bool ok = s.openOver(args, viewOf(b));
    CHECK(!ok);
    return s.lastError();
}

void testTheDecision() {
    using rtlopen::Case;
    for (const bool listed : {false, true}) {
        for (const bool onBus : {false, true}) {
            for (const bool unbound : {false, true}) {
                const Case c = rtlopen::decide(listed, onBus, unbound);
                const Case want = unbound ? Case::WrongDriver : (listed && !onBus) ? Case::NotPlugged : Case::CannotTell;
                CHECK(c == want);
            }
        }
    }
    // the three named in the brief
    CHECK(rtlopen::decide(true, true, true) == Case::WrongDriver);    // a dongle on the bus, on the wrong driver
    CHECK(rtlopen::decide(true, false, false) == Case::NotPlugged);   // the bus was listed and holds none
    CHECK(rtlopen::decide(false, false, false) == Case::CannotTell);  // the bus could not be listed
    CHECK(rtlopen::decide(true, true, false) == Case::CannotTell);    // one is on the bus and the transport cannot say why it is not usable (Linux: no "unbound" list)
}

void testTheOpenOverAFakeBus() {
    const std::string wrong = rtlopen::sentence(rtlopen::Case::WrongDriver);
    const std::string none = rtlopen::sentence(rtlopen::Case::NotPlugged);
    const std::string tell = rtlopen::sentence(rtlopen::Case::CannotTell);
    for (const char* args : {"", "index=0", "serial=00000001"}) {   // a plain pick, an index, a saved radio's serial: the bound list is empty, so the answer is the same
        {   // (a) the dongle is on the bus with no WinUSB interface
            Bus b;
            b.unbound = {dongle("00000001")};
            b.present = {{0x0BDA, 0x2838}, {0x046D, 0xC52B}};
            const std::string e = openError(b, args);
            std::printf("  (a) args \"%s\": %s\n", args, e.c_str());
            CHECK(e == wrong);
            CHECK(health::classifyRadioOpen("rtlsdr", e) == health::RadioReason::Bind);
            CHECK(std::string(health::radioReportClassWord("rtlsdr", e)) == "bind");
        }
        {   // (b) the bus was listed and no RTL-SDR is on it
            Bus b;
            b.present = {{0x046D, 0xC52B}, {0x1D50, 0x6089}};
            const std::string e = openError(b, args);
            std::printf("  (b) args \"%s\": %s\n", args, e.c_str());
            CHECK(e == none);
            CHECK(health::classifyRadioOpen("rtlsdr", e) == health::RadioReason::Absent);
            CHECK(std::string(health::radioReportClassWord("rtlsdr", e)) == "absent");
        }
        {   // (c) the bus listing could not be taken
            Bus b;
            b.listed = false;
            const std::string e = openError(b, args);
            std::printf("  (c) args \"%s\": %s\n", args, e.c_str());
            CHECK(e == tell);
            CHECK(health::classifyRadioOpen("rtlsdr", e) == health::RadioReason::Other);
            CHECK(std::string(health::radioReportClassWord("rtlsdr", e)) == "other");
        }
        {   // (c) an RTL-SDR is on the bus, the unbound list is empty (no such list on this platform)
            Bus b;
            b.present = {{0x0BDA, 0x2838}};
            CHECK(openError(b, args) == tell);
        }
    }
    // the unbound list wins over a listing that failed: the dongle is plainly there
    {
        Bus b;
        b.unbound = {dongle("00000001")};
        b.listed = false;
        CHECK(openError(b, "") == wrong);
    }
    // every USB id of the RTL-SDR family counts as "an RTL-SDR on the bus"
    for (const usb::UsbId& id : src::rtlSdrUsbIds()) {
        Bus b;
        b.present = {id};
        CHECK(openError(b, "") == tell);
    }
}

void testTheSentences() {
    const std::string a = rtlopen::sentence(rtlopen::Case::WrongDriver);
    const std::string b = rtlopen::sentence(rtlopen::Case::NotPlugged);
    const std::string c = rtlopen::sentence(rtlopen::Case::CannotTell);
    CHECK(a != b && a != c && b != c);
    CHECK(a.find("plugged in") != std::string::npos && a.find("WinUSB") != std::string::npos && a.find("install") != std::string::npos);   // says the driver is the problem and that FoxSDR can fix it
    CHECK(b.find("No RTL-SDR is plugged") != std::string::npos);
    CHECK(c.find("could not tell") != std::string::npos);
    const std::string old = std::string("no RTL-SDR is ") + usb::kBindHint + " on this machine";
    for (const std::string& s : {a, b, c}) { CHECK(s != old && s.find(old) == std::string::npos); }
    // each is a whole sentence with a full stop
    for (const std::string& s : {a, b, c}) { CHECK(!s.empty() && s.back() == '.'); }
}

void testTheEnginesTest() {
    const std::string a = rtlopen::sentence(rtlopen::Case::WrongDriver);
    const std::string b = rtlopen::sentence(rtlopen::Case::NotPlugged);
    const std::string c = rtlopen::sentence(rtlopen::Case::CannotTell);
    const std::string wrapped = cascade::gui::radioNotOpenedSentence("RTL2838UHIDIR", a);
    CHECK(wrapped.find("did not open") != std::string::npos);
    CHECK(rtlopen::saysWrongDriver(a));
    CHECK(rtlopen::saysWrongDriver(wrapped));
    CHECK(!rtlopen::saysWrongDriver(b) && !rtlopen::saysWrongDriver(c));
    CHECK(!rtlopen::saysWrongDriver(cascade::gui::radioNotOpenedSentence("RTL-SDR", b)));
    CHECK(!rtlopen::saysWrongDriver(cascade::gui::radioNotOpenedSentence("RTL-SDR", c)));
    for (const char* other : {"", "the radio is already in use by another program", "no such device", "no HackRF found (is it plugged in, and bound to WinUSB?)",
                              "WinUsb_Initialize (is the device bound to WinUSB?): The parameter is incorrect.", "no RTL-SDR with serial 00000001 is present"}) {
        CHECK(!rtlopen::saysWrongDriver(other));
        CHECK(!rtlopen::saysWrongDriver(cascade::gui::radioNotOpenedSentence("RTL-SDR", other)));
    }
    // and the classifier reads the wrapped sentence the way it reads the bare one (the report carries the bare one; the screen shows the wrapped one)
    CHECK(health::classifyRadioOpen("rtlsdr", wrapped) == health::RadioReason::Bind);
}

void testNothingElseMoves() {
    // a non-empty bound list never asks for the unbound list or the bus listing
    {
        Bus b;
        b.bound = {dongle("00000001")};
        src::RtlSdrSource s;
        s.openOver("serial=00000002", viewOf(b));   // a serial that is not there
        CHECK(std::string(s.lastError()) == "no RTL-SDR with serial 00000002 is present");
        CHECK(b.boundCalls == 1 && b.unboundCalls == 0 && b.presentCalls == 0);
        CHECK(health::classifyRadioOpen("rtlsdr", s.lastError()) == health::RadioReason::Absent);
        src::RtlSdrSource t;
        t.openOver("index=3", viewOf(b));
        CHECK(std::string(t.lastError()) == "there is no RTL-SDR at that index");
        CHECK(b.unboundCalls == 0 && b.presentCalls == 0);
    }
    // an empty bound list asks each of the other two exactly once
    {
        Bus b;
        openError(b, "");
        CHECK(b.boundCalls == 1 && b.unboundCalls == 1 && b.presentCalls == 1);
    }
    // the shipped bus view has all three questions armed (the open uses it)
    const src::RtlBusView shipped = src::shippedRtlBusView();
    CHECK(static_cast<bool>(shipped.bound) && static_cast<bool>(shipped.unbound) && static_cast<bool>(shipped.present));
}

void testOtherFamiliesKeepTheirWords() {
    // the other drivers' "not bound" sentences keep their class (they are not this change's)
    CHECK(health::classifyRadioOpen("hackrf", "no HackRF found (is it plugged in, and bound to WinUSB?)") == health::RadioReason::Bind);
    CHECK(health::classifyRadioOpen("airspy", "no Airspy found (is it plugged in, and bound to WinUSB?)") == health::RadioReason::Bind);
    CHECK(health::classifyRadioOpen("rtlsdr", std::string("no RTL-SDR is ") + usb::kBindHint + " on this machine") == health::RadioReason::Bind);   // the old words, still read as bind
    CHECK(health::classifyRadioOpen("rtlsdr", "no RTL-SDR with serial 00000001 is present") == health::RadioReason::Absent);
}

}  // namespace

int main() {
    std::printf("test_rtlsdr_no_radio\n");
    testTheDecision();
    testTheOpenOverAFakeBus();
    testTheSentences();
    testTheEnginesTest();
    testNothingElseMoves();
    testOtherFamiliesKeepTheirWords();
    return testSummary("test_rtlsdr_no_radio");
}
