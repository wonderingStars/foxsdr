// test_rtlsdr_locate.cpp - FINDING THE SAME RADIO AGAIN NEVER GUESSES (fix/rtlsdr-reopen, decision F4 of the independent review).
//
// A radio that dropped off the bus is found again by what identifies it, in this order: the exact interface path, the same hub-and-port location, the same serial
// when that serial was the only one of its kind at open and is the only one now. When two dongles say the same serial (a stock RTL-SDR says "00000001") and neither
// path nor location matches, NONE is opened: the answer is "ambiguous" and the re-open ends with a sentence that says it could not tell which radio it was. A listed
// device whose serial differs from the remembered one is never taken for it. Run against fake bus enumerations (no USB is touched) and the shipped lambda that the
// source arms itself with (makeRadioReopenFn), with an opener that records what it was asked to open.
//   L1  the same path (and the path compares case-insensitively);
//   L2  the dongle moved to another port: found by its unique serial;
//   L3  two dongles with the same serial, none matches path or location: ambiguous, nothing opened, `present` true;
//   L3b the same pair, one of them at our path: that one is found (the path wins);
//   L3c a shared serial at open and only one of the pair listed now: ambiguous (it may be the other one that moved);
//   L4  the candidate is in use by another program: the opener's own "in use" text comes back, the wait ends at once;
//   L5  nothing listed: absent;
//   L6  another dongle (another serial) on our port: not ours;
//   L7  a dongle with no serial that moved port: absent (its path is its only identity);
//   L8  the interface path changed with a re-enumeration but the port is the same: found by location;
//   L9  (decision S2 of the second review) two dongles that drop together and come back holding EACH OTHER's interface paths are not swapped: a path match whose
//       location contradicts the remembered one is not accepted; an unknown location on either side lets the path stand;
//   L10 (decision S11) the location is composed down to the PORT from what SetupAPI says, in order: location paths, location information, parent id plus address.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

using cascade::source::RadioIdentity;
using cascade::source::RadioLocate;
using cascade::source::locateRadio;
using cascade::source::makeRadioListedFn;
using cascade::source::makeRadioReopenFn;
using cascade::usb::FakeUsbDevice;
using cascade::usb::UsbDeviceInfo;

namespace {

UsbDeviceInfo dev(const char* path, const char* location, const char* serial) {
    UsbDeviceInfo d;
    d.vid = 0x0bda;
    d.pid = 0x2838;
    d.path = path;
    d.location = location;
    d.serial = serial;
    d.description = "RTL2838UHIDIR";
    return d;
}

RadioIdentity ident(const char* path, const char* location, const char* serial, bool unique) {
    RadioIdentity id;
    id.path = path;
    id.location = location;
    id.serial = serial;
    id.serialUnique = unique;
    return id;
}

const char* const kPathA = "\\\\?\\usb#vid_0bda&pid_2838&mi_00#7&38a180a5&0&0000#{e12d70d7}";
const char* const kPathB = "\\\\?\\usb#vid_0bda&pid_2838&mi_00#7&1c2b3d4e&0&0000#{e12d70d7}";
const char* const kPathC = "\\\\?\\usb#vid_0bda&pid_2838&mi_00#7&99aa88bb&0&0000#{e12d70d7}";
const char* const kPort1 = "PCIROOT(0)#PCI(1400)#USB(5)#USB(2)";
const char* const kPort2 = "PCIROOT(0)#PCI(1400)#USB(5)#USB(3)";
const char* const kPort3 = "PCIROOT(0)#PCI(1400)#USB(6)";

}  // namespace

int main() {
    std::printf("test_rtlsdr_locate\n");
    std::string where;

    // L1: the same path.
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathA, kPort1, "00000001")};
        CHECK(locateRadio(found, ident(kPathA, kPort1, "00000001", true), where) == RadioLocate::Found);
        CHECK(where == kPathA);
        std::string upper = kPathA;
        for (char& c : upper) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
        where.clear();
        CHECK(locateRadio(found, ident(upper.c_str(), "", "", false), where) == RadioLocate::Found);
        CHECK(where == kPathA);
    }

    // L2: moved to another port (new path, new location), unique serial.
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathB, kPort2, "00000001")};
        where.clear();
        CHECK(locateRadio(found, ident(kPathA, kPort1, "00000001", true), where) == RadioLocate::Found);
        CHECK(where == kPathB);
    }

    // L3: two dongles with the same serial, neither at our path nor our location.
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathB, kPort2, "00000001"), dev(kPathC, kPort3, "00000001")};
        where = "untouched";
        CHECK(locateRadio(found, ident(kPathA, kPort1, "00000001", false), where) == RadioLocate::Ambiguous);
        CHECK(where == "untouched");
        // ...and through the shipped lambda: nothing is opened, `present` says the radio class is on the bus, the error is the one the re-open recognises
        int opened = 0;
        auto fn = makeRadioReopenFn(ident(kPathA, kPort1, "00000001", false), [&] { return found; }, [&](const std::string&, std::string&) -> std::unique_ptr<cascade::usb::UsbDevice> {
            ++opened;
            return std::make_unique<FakeUsbDevice>();
        });
        std::string error;
        bool present = false;
        CHECK(fn(error, present) == nullptr);
        CHECK(present);
        CHECK(error == cascade::source::kRadioAmbiguousError);
        CHECK(opened == 0);
        auto listed = makeRadioListedFn(ident(kPathA, kPort1, "00000001", false), [&] { return found; });
        CHECK(listed() == 1);
    }

    // L3b: the same pair with one of them at our path: the path wins.
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathA, kPort1, "00000001"), dev(kPathC, kPort3, "00000001")};
        where.clear();
        CHECK(locateRadio(found, ident(kPathA, kPort1, "00000001", false), where) == RadioLocate::Found);
        CHECK(where == kPathA);
    }

    // L3c: the serial was shared at open; now only one of the pair is listed, at neither our path nor our port: it may be the OTHER dongle.
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathC, kPort3, "00000001")};
        where = "untouched";
        CHECK(locateRadio(found, ident(kPathA, kPort1, "00000001", false), where) == RadioLocate::Ambiguous);
        CHECK(where == "untouched");
    }

    // L4: the candidate is in use by another program: the opener says so and the lambda hands that text back (the re-open ends the wait on it).
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathA, kPort1, "00000001")};
        std::string asked;
        auto fn = makeRadioReopenFn(ident(kPathA, kPort1, "00000001", true), [&] { return found; }, [&](const std::string& path, std::string& error) -> std::unique_ptr<cascade::usb::UsbDevice> {
            asked = path;
            error = cascade::usb::kInUseError;
            return nullptr;
        });
        std::string error;
        bool present = false;
        CHECK(fn(error, present) == nullptr);
        CHECK(present);
        CHECK(asked == kPathA);
        CHECK(cascade::usb::errorSaysInUse(error));
    }

    // L5: nothing listed.
    {
        const std::vector<UsbDeviceInfo> none;
        CHECK(locateRadio(none, ident(kPathA, kPort1, "00000001", true), where) == RadioLocate::Absent);
        auto fn = makeRadioReopenFn(ident(kPathA, kPort1, "00000001", true), [&] { return none; }, [](const std::string&, std::string&) -> std::unique_ptr<cascade::usb::UsbDevice> { return nullptr; });
        std::string error;
        bool present = true;
        CHECK(fn(error, present) == nullptr);
        CHECK(!present);
        auto listed = makeRadioListedFn(ident(kPathA, kPort1, "00000001", true), [&] { return none; });
        CHECK(listed() == 0);
    }

    // L6: a different dongle (another serial) now sits on our port, and ours is not listed.
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathB, kPort1, "00000002")};
        CHECK(locateRadio(found, ident(kPathA, kPort1, "00000001", true), where) == RadioLocate::Absent);
        // the same with the path equal: the device at our path says another serial, so it is not ours either
        const std::vector<UsbDeviceInfo> samePath = {dev(kPathA, kPort1, "00000002")};
        CHECK(locateRadio(samePath, ident(kPathA, kPort1, "00000001", true), where) == RadioLocate::Absent);
    }

    // L7: a dongle that reported no serial, moved to another port: its path and port are all it has, and neither matches.
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathB, kPort2, "")};
        CHECK(locateRadio(found, ident(kPathA, kPort1, "", false), where) == RadioLocate::Absent);
        // ...and on the same port with a new interface path it is found by its port
        const std::vector<UsbDeviceInfo> samePort = {dev(kPathB, kPort1, "")};
        where.clear();
        CHECK(locateRadio(samePort, ident(kPathA, kPort1, "", false), where) == RadioLocate::Found);
        CHECK(where == kPathB);
    }

    // L8: the interface path changed with a re-enumeration, the port and the serial did not.
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathB, kPort1, "00000001"), dev(kPathC, kPort3, "00000001")};
        where.clear();
        CHECK(locateRadio(found, ident(kPathA, kPort1, "00000001", false), where) == RadioLocate::Found);
        CHECK(where == kPathB);
    }

    // L9 (decision S2): the interface path of a dongle with a serial follows the SERIAL, not the port, and Windows lists two devices that say one serial as one
    // serial-named instance and one generated one (an empty serial). After both drop together the serial-named instance may be on the OTHER dongle: the path A
    // remembers now belongs to B's port. Each window must find its OWN dongle, by the location, and neither may take the other's.
    {
        const std::vector<UsbDeviceInfo> found = {dev(kPathA, kPort2, "00000001"), dev(kPathB, kPort1, "")};
        where.clear();
        CHECK(locateRadio(found, ident(kPathA, kPort1, "00000001", true), where) == RadioLocate::Found);   // A sat on port 1
        CHECK(where == kPathB);
        where.clear();
        CHECK(locateRadio(found, ident(kPathB, kPort2, "", false), where) == RadioLocate::Found);          // B sat on port 2
        CHECK(where == kPathA);
        // the path alone is not enough when the location says it is the other dongle, and nothing else matches: not found by the path
        const std::vector<UsbDeviceInfo> onlyOther = {dev(kPathA, kPort2, "00000001")};
        where = "untouched";
        CHECK(locateRadio(onlyOther, ident(kPathA, kPort1, "", false), where) == RadioLocate::Absent);
        CHECK(where == "untouched");
        // an unknown location on either side proves nothing: the path stands
        where.clear();
        CHECK(locateRadio(onlyOther, ident(kPathA, "", "", false), where) == RadioLocate::Found);
        CHECK(where == kPathA);
        const std::vector<UsbDeviceInfo> noLocation = {dev(kPathA, "", "")};
        where.clear();
        CHECK(locateRadio(noLocation, ident(kPathA, kPort1, "", false), where) == RadioLocate::Found);
        CHECK(where == kPathA);
        // a dongle with a unique serial the user moved to another port in the gap is still found, by the serial
        const std::vector<UsbDeviceInfo> moved = {dev(kPathA, kPort3, "00000001")};
        where.clear();
        CHECK(locateRadio(moved, ident(kPathA, kPort1, "00000001", true), where) == RadioLocate::Found);
        CHECK(where == kPathA);
    }

    // L10 (decision S11): the composition of a location, with fake property values.
    {
        using cascade::usb::composeUsbLocation;
        // location paths win over everything
        CHECK(composeUsbLocation("PCIROOT(0)#PCI(1400)#USB(5)#USB(2)", "Port_#0003.Hub_#0002", "USB/ROOT_HUB30/4&1&0", 3) == "PCIROOT(0)#PCI(1400)#USB(5)#USB(2)");
        // else the location information, which names port and hub
        CHECK(composeUsbLocation("", "Port_#0003.Hub_#0002", "USB/ROOT_HUB30/4&1&0", 3) == "port:Port_#0003.Hub_#0002");
        // else the parent AND the address: two dongles on one hub get different locations (the old composition gave them one string)
        const std::string one = composeUsbLocation("", "", "USB/VID_05E3&PID_0610/5&2A&0&2", 1);
        const std::string two = composeUsbLocation("", "", "USB/VID_05E3&PID_0610/5&2A&0&2", 2);
        CHECK(one == "parent:USB/VID_05E3&PID_0610/5&2A&0&2#port1");
        CHECK(one != two);
        // address zero is a port too
        CHECK(composeUsbLocation("", "", "H", 0) == "parent:H#port0");
        // with no address the hub alone is all there is (the weakest form)
        CHECK(composeUsbLocation("", "", "H", -1) == "parent:H");
        // nothing known: empty
        CHECK(composeUsbLocation("", "", "", -1).empty());
        CHECK(composeUsbLocation("", "", "", 5).empty());
    }

    return testSummary("test_rtlsdr_locate");
}
