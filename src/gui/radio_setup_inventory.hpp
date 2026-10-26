// radio_setup_inventory.hpp - what the RADIO SETUP page adds to the probe's inventory from the application's own lists (stub campaign B8).
//
// The probe (core/radio_setup.hpp) reads the machine: the USB bus, what is installed. It cannot know the second half - what the application's
// own enumeration lists NOW, whether a radio is open, whether a patch holds one, how the SoapySDR scan went - and the page must not tell a person
// whose radio is listed that there is none. Master read those from the window's own members (nativeDevices_, soapyDevices_, device_,
// patchHeldRadioNames, soapyScanned_ ...); the engine holds them now and the window has them from the table: the Source combo's rows (DEVICES),
// the patch's radios (PATCH_RADIOS), the state's flags and fact.source.uhdNoImages. This is the arithmetic, pure and ImGui-free, so a test can
// hold it (tests/test_radio_setup_inventory.cpp); the window fills RadioSetupLists and calls it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_RADIO_SETUP_INVENTORY_HPP
#define CASCADE_GUI_RADIO_SETUP_INVENTORY_HPP

#include <algorithm>
#include <string>
#include <vector>

#include "core/radio_setup.hpp"

namespace cascade::gui {

struct RadioSetupLists {
    struct Row {
        std::string driver;      // the driver key of the row: a native row's own, a SoapySDR row's "driver=" ("rtlsdr", "uhd", "airspy"...)
        bool network = false;    // the Pluto's and the rtl_tcp server's rows: always in the list, an address box and not a radio that was found
    };
    std::vector<Row> rows;       // the Source combo's native and SoapySDR rows (not the generator, the I/Q file or the sound card)
    bool deviceOpen = false;     // a radio is open (FOXAPI_RX_DEVICE_OPEN)
    int patchHeldRadios = 0;     // hardware radios the running patch holds
    bool soapyScanned = false;   // a SoapySDR scan has been started this session
    bool soapyScanPartial = false;  // the last scan left an open radio's driver out
    bool soapyScanPending = false;  // a scan is running
    bool uhdNoImages = false;    // fact.source.uhdNoImages: UHD itself said it has no firmware images
};

// What the probe saw, plus what the lists hold. A probe that did not run, or could not list the bus, is returned as it was: the lists cannot
// make an unknown known.
inline core::radiosetup::Inventory radioSetupInventoryFrom(core::radiosetup::Inventory inv, const RadioSetupLists& l) {
    if (!inv.probed || !inv.usbListed) { return inv; }
    int listed = 0;
    for (const RadioSetupLists::Row& r : l.rows) {
        // The Pluto's and the rtl_tcp server's rows are always in the list (a network cannot be walked, so they ask for an address): they say
        // nothing about what is plugged in. (Master's page skipped the Pluto's only, so since 0.99.70 - when the rtl_tcp row arrived - it counted
        // a radio listed on every machine and never raised itself; the Source section's own "no radio" rule has always skipped both.)
        if (r.network) { continue; }
        ++listed;
        const std::string family = core::radiosetup::familyForDriver(r.driver);
        if (!family.empty() && std::find(inv.visible.begin(), inv.visible.end(), family) == inv.visible.end()) {
            inv.visible.push_back(family);
        }
    }
    // A radio the receiver or a patch has OPEN is a radio whatever any list says.
    if (l.deviceOpen) { ++listed; }
    listed += l.patchHeldRadios;
    inv.visibleCount = std::max(inv.visibleCount, listed);
    // The SoapySDR families are judged only against a scan that actually ran and covered every driver...
    inv.soapyChecked = l.soapyScanned && !l.soapyScanPartial && !l.soapyScanPending;
    // ...and what that scan itself said about UHD's firmware images (2026-10-21): assess() reads it only beside soapyChecked, the same gate.
    inv.uhdImagesMissing = l.uhdNoImages;
    return inv;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_RADIO_SETUP_INVENTORY_HPP
