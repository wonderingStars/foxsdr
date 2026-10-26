// gui/radio_setup_inventory.hpp - what the RADIO SETUP page adds to the probe's inventory from the application's own lists (stub campaign B8).
//
// The probe reads the machine; the window adds what the application's enumeration lists NOW. Master read those from members of its window; the
// engine holds them and the window has them from the table, so the arithmetic is a pure function and this holds it: which rows are radios and which
// are an address box, what a radio held open or by a patch counts for, when the SoapySDR families are judged, and the UHD images finding (R2, the
// 0.99.89 report: radioconda installs UHD with no firmware images, the scan lists nothing, and the page must say so and not "not visible").
//
// What is NOT held here, and where it is: the page itself, its census and its keys - tests/test_radio_setup_app.cpp (the real application, a
// synthetic machine). That test cannot reach this arithmetic: a synthetic machine replaces the lists whole.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/radio_setup_inventory.hpp"

#include <algorithm>
#include <string>

#include "core/radio_setup.hpp"
#include "test_check.hpp"

namespace rs = cascade::core::radiosetup;
using cascade::gui::RadioSetupLists;
using cascade::gui::radioSetupInventoryFrom;

namespace {

rs::Inventory emptyBus() {
    rs::Inventory inv;
    inv.probed = true;
    inv.usbListed = true;
    inv.nativeChecked = true;
    return inv;
}

RadioSetupLists::Row radio(const char* driver) { return RadioSetupLists::Row{driver, false}; }
RadioSetupLists::Row network(const char* driver) { return RadioSetupLists::Row{driver, true}; }

// A probe that did not run, or could not list the bus, is returned as it was: the lists cannot make an unknown known.
void testWhatWasNotProbedIsLeftAlone() {
    RadioSetupLists l;
    l.rows.push_back(radio("rtlsdr"));
    l.deviceOpen = true;
    l.soapyScanned = true;
    l.uhdNoImages = true;
    rs::Inventory unprobed;  // probed = false
    const rs::Inventory a = radioSetupInventoryFrom(unprobed, l);
    CHECK(!a.probed);
    CHECK(a.visibleCount == 0);
    CHECK(a.visible.empty());
    CHECK(!a.soapyChecked);
    CHECK(!a.uhdImagesMissing);
    rs::Inventory unlisted = emptyBus();
    unlisted.usbListed = false;
    const rs::Inventory b = radioSetupInventoryFrom(unlisted, l);
    CHECK(b.visibleCount == 0);
    CHECK(!b.soapyChecked);
}

// THE NETWORK ROWS ARE NOT RADIOS THAT WERE FOUND. The Pluto's row and the rtl_tcp server's are always in the Source list (a network cannot be
// walked, so each asks for an address): a machine whose list holds those and nothing else has no radio, and the page must still be raised for it.
// (Master's page skipped the Pluto's row only; since the rtl_tcp row arrived in 0.99.70 it counted a radio on every machine and never raised
// itself. The Source section's own "no radio hardware" rule has always skipped both.)
void testTheNetworkRowsAreNotRadios() {
    RadioSetupLists l;
    l.rows.push_back(network("pluto"));
    l.rows.push_back(network("rtltcp"));
    const rs::Inventory inv = radioSetupInventoryFrom(emptyBus(), l);
    CHECK(inv.visibleCount == 0);
    CHECK(inv.visible.empty());
    const rs::Findings f = rs::assess(inv);
    CHECK(f.showPage);  // nothing is usable and nothing is listed: the page opens by itself
    CHECK(!f.items.empty() && f.items[0].kind == rs::Kind::NoDevice);
    // ...and one radio that was found beside them is what stops it.
    l.rows.push_back(radio("airspy"));
    const rs::Inventory with = radioSetupInventoryFrom(emptyBus(), l);
    CHECK(with.visibleCount == 1);
    CHECK(!rs::assess(with).showPage);
}

// WHAT A ROW SAYS: its driver's family, once; a driver of no known family still counts as a radio.
void testTheRowsNameTheirFamilies() {
    RadioSetupLists l;
    l.rows.push_back(radio("rtlsdr"));
    l.rows.push_back(radio("uhd"));           // a SoapySDR row: the args' driver= is uhd, the family usrp
    l.rows.push_back(radio("rtlsdr"));        // a second dongle, and the same dongle through SoapySDR: one family
    l.rows.push_back(radio("somethingnew"));  // no known family
    const rs::Inventory inv = radioSetupInventoryFrom(emptyBus(), l);
    CHECK(inv.visibleCount == 4);
    CHECK(inv.visible.size() == 2u);
    CHECK(std::find(inv.visible.begin(), inv.visible.end(), std::string(rs::kFamilyRtl)) != inv.visible.end());
    CHECK(std::find(inv.visible.begin(), inv.visible.end(), std::string(rs::kFamilyUsrp)) != inv.visible.end());
}

// A RADIO THE RECEIVER OR A PATCH HAS OPEN IS A RADIO whatever any list says (a Refresh that dropped its row, a patch on a radio the combo does
// not show); the count is what the page's trigger reads.
void testAnOpenRadioIsARadio() {
    RadioSetupLists l;
    l.deviceOpen = true;
    CHECK(radioSetupInventoryFrom(emptyBus(), l).visibleCount == 1);
    CHECK(!rs::assess(radioSetupInventoryFrom(emptyBus(), l)).showPage);
    RadioSetupLists p;
    p.patchHeldRadios = 2;
    CHECK(radioSetupInventoryFrom(emptyBus(), p).visibleCount == 2);
    // The count never goes DOWN: what the probe itself counted stays.
    rs::Inventory probed = emptyBus();
    probed.visibleCount = 3;
    CHECK(radioSetupInventoryFrom(probed, RadioSetupLists{}).visibleCount == 3);
}

// THE SOAPYSDR FAMILIES ARE JUDGED ONLY AGAINST A SCAN THAT RAN AND COVERED EVERY DRIVER: the flag is "scanned, not partial, not still running".
void testTheSoapyScanGate() {
    struct Case {
        bool scanned, partial, pending, want;
    };
    const Case cases[] = {
        {true, false, false, true},   // a whole scan, done
        {false, false, false, false}, // never run (it is lazy on purpose)
        {true, true, false, false},   // left an open radio's driver out
        {true, false, true, false},   // running now
        {false, true, true, false},
    };
    for (const Case& c : cases) {
        RadioSetupLists l;
        l.soapyScanned = c.scanned;
        l.soapyScanPartial = c.partial;
        l.soapyScanPending = c.pending;
        CHECK(radioSetupInventoryFrom(emptyBus(), l).soapyChecked == c.want);
    }
}

// R2, THE IMAGES FINDING (0.99.89): a USRP on the bus, UHD installed, the scan ran whole and listed nothing - and UHD itself said it has no
// firmware images. The page says "vendor missing" (install the images), not "not visible". The flag is judged beside soapyChecked, the same gate.
void testTheUhdImagesFinding() {
    rs::Inventory machine;
    CHECK(rs::fakeInventory("usrp-unlisted", machine));  // a USRP on WinUSB, UHD present, nothing listed, the scan flags set by the lists below
    machine.soapyChecked = false;
    machine.uhdImagesMissing = false;

    RadioSetupLists scanned;
    scanned.soapyScanned = true;
    scanned.uhdNoImages = false;
    const rs::Findings plain = rs::assess(radioSetupInventoryFrom(machine, scanned));
    CHECK(!plain.items.empty() && plain.items[0].kind == rs::Kind::NotVisible);

    scanned.uhdNoImages = true;
    const rs::Inventory said = radioSetupInventoryFrom(machine, scanned);
    CHECK(said.uhdImagesMissing);
    const rs::Findings images = rs::assess(said);
    CHECK(!images.items.empty() && images.items[0].kind == rs::Kind::VendorMissing);

    // A scan that did not cover every driver cannot say it: the flag is carried but not judged.
    RadioSetupLists partial = scanned;
    partial.soapyScanPartial = true;
    const rs::Findings notYet = rs::assess(radioSetupInventoryFrom(machine, partial));
    CHECK(notYet.items.empty() || notYet.items[0].kind != rs::Kind::VendorMissing);
}

}  // namespace

int main() {
    testWhatWasNotProbedIsLeftAlone();
    testTheNetworkRowsAreNotRadios();
    testTheRowsNameTheirFamilies();
    testAnOpenRadioIsARadio();
    testTheSoapyScanGate();
    testTheUhdImagesFinding();
    return testSummary("test_radio_setup_inventory");
}
