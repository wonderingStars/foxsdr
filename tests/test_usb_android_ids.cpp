// ONE LIST OF USB IDS, IN FOUR PLACES, PINNED EQUAL.
//
// A radio reaches this application only if its vendor/product pair is in
// every one of these:
//
//   1. the native drivers' own id tables (rtlSdrUsbIds() and the five beside
//      it) - what enumerateWinUsb() filters on, on every platform;
//   2. installer/linux/99-foxsdr-sdr.rules - the desktop Linux permission
//      rules, and the text form tools/gen-android-usb-ids.py generates from;
//   3. android/app/src/main/res/xml/usb_device_filter.xml - what Android
//      matches an ATTACHED device against, deciding both whether it offers to
//      launch FoxSDR and whether permission is granted without a dialog;
//   4. android/app/src/main/java/com/foxsdr/app/UsbIds.java - what the Java
//      USB layer filters UsbManager.getDeviceList() by, because that call
//      returns every attached device (keyboard, hub, charger) and asking the
//      user for permission for each would be indefensible.
//
// WHY A TEST RATHER THAN TRUST IN THE GENERATOR. The generator writes 3 and 4
// from 2, so those three cannot drift by accident - but nothing stops a hand
// edit, and NOTHING AT ALL connects 1 to the other three: a driver that gains
// an id (a new RTL2832U clone, a second HackRF pid) would work on the desktop
// and be silently unopenable on a phone, because Android would never grant
// permission for a device the filter does not list. That failure has no
// symptom a user could report except "my dongle does nothing", which is the
// hardest bug in this whole program to attribute. So the compiled-in tables
// are read here, from the same functions the drivers call, and compared
// against the three files as text.
//
// This test is PLATFORM-INDEPENDENT on purpose (no #if): the four lists must
// agree whichever host builds the repository, and the two Android files are
// read as text, not loaded.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "test_check.hpp"

#include "source/airspy_source.hpp"
#include "source/airspyhf_source.hpp"
#include "source/hackrf_source.hpp"
#include "source/msi2500.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"
#include "usb/usb_device.hpp"

namespace {

using IdPair = std::pair<std::uint16_t, std::uint16_t>;
using IdSet = std::set<IdPair>;

std::string readFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) { return {}; }
    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { out.append(buf, n); }
    std::fclose(f);
    return out;
}

// Reads `len` characters after `pos` as one unsigned number in `base`, and
// says whether exactly that many digits were there. Deliberately strict: a
// three-digit hex id or a stray space is a malformed table, not something to
// silently round.
bool readNumber(const std::string& s, std::size_t pos, std::size_t len, int base,
                unsigned long& out) {
    if (pos + len > s.size()) { return false; }
    out = 0;
    for (std::size_t i = 0; i < len; ++i) {
        const char c = s[pos + i];
        int digit = -1;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = 10 + (c - 'a');
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = 10 + (c - 'A');
        }
        if (digit < 0 || digit >= base) { return false; }
        out = out * static_cast<unsigned long>(base) + static_cast<unsigned long>(digit);
    }
    return true;
}

// Every ATTR{idVendor}=="xxxx" ... ATTR{idProduct}=="xxxx" pair in the udev
// rules, in file order (duplicates kept so the caller can count them).
std::vector<IdPair> parseUdevRules(const std::string& text) {
    std::vector<IdPair> out;
    const std::string vendorKey = "ATTR{idVendor}==\"";
    const std::string productKey = "ATTR{idProduct}==\"";
    std::size_t at = 0;
    while ((at = text.find(vendorKey, at)) != std::string::npos) {
        const std::size_t vidAt = at + vendorKey.size();
        unsigned long vid = 0;
        const std::size_t productAt = text.find(productKey, vidAt);
        unsigned long pid = 0;
        if (readNumber(text, vidAt, 4, 16, vid) && productAt != std::string::npos &&
            readNumber(text, productAt + productKey.size(), 4, 16, pid)) {
            out.push_back({static_cast<std::uint16_t>(vid), static_cast<std::uint16_t>(pid)});
        }
        at = vidAt;
    }
    return out;
}

// Replaces every <!-- ... --> region with spaces, so a commented-out element
// is not counted as a live one. THIS IS NOT PEDANTRY: the placeholder this
// file replaced carried a fully-formed <usb-device ... /> line inside its
// comment as the example of the syntax, and a parser that reads comments
// reports one entry for a filter that matches nothing at all - which is
// exactly the state this test exists to catch.
std::string withoutXmlComments(const std::string& text) {
    std::string out = text;
    std::size_t at = 0;
    while ((at = out.find("<!--", at)) != std::string::npos) {
        const std::size_t end = out.find("-->", at);
        const std::size_t stop = (end == std::string::npos) ? out.size() : end + 3;
        for (std::size_t i = at; i < stop; ++i) {
            if (out[i] != '\n') { out[i] = ' '; }
        }
        at = stop;
    }
    return out;
}

// Every <usb-device vendor-id="N" product-id="M" /> in the Android filter.
// DECIMAL, which is Android's own requirement for this file.
std::vector<IdPair> parseUsbDeviceFilter(const std::string& text) {
    std::vector<IdPair> out;
    const std::string elem = "<usb-device";
    const std::string vendorKey = "vendor-id=\"";
    const std::string productKey = "product-id=\"";
    std::size_t at = 0;
    while ((at = text.find(elem, at)) != std::string::npos) {
        const std::size_t end = text.find('>', at);
        if (end == std::string::npos) { break; }
        const std::string tag = text.substr(at, end - at);
        const std::size_t vAt = tag.find(vendorKey);
        const std::size_t pAt = tag.find(productKey);
        if (vAt != std::string::npos && pAt != std::string::npos) {
            // Decimal, any length, rejected if absent or out of range.
            const auto decimalAt = [&tag](std::size_t i, unsigned long& value) {
                value = 0;
                std::size_t digits = 0;
                while (i < tag.size() && tag[i] >= '0' && tag[i] <= '9') {
                    value = value * 10 + static_cast<unsigned long>(tag[i] - '0');
                    ++i;
                    ++digits;
                }
                return digits > 0 && value <= 0xFFFF;
            };
            unsigned long vid = 0;
            unsigned long pid = 0;
            const bool ok = decimalAt(vAt + vendorKey.size(), vid) &&
                            decimalAt(pAt + productKey.size(), pid);
            if (ok) {
                out.push_back({static_cast<std::uint16_t>(vid), static_cast<std::uint16_t>(pid)});
            }
        }
        at = end;
    }
    return out;
}

// Every {0xVVVV, 0xPPPP} entry in UsbIds.java's SUPPORTED table.
std::vector<IdPair> parseJavaTable(const std::string& text) {
    std::vector<IdPair> out;
    const std::string open = "{0x";
    const std::string mid = ", 0x";
    std::size_t at = 0;
    while ((at = text.find(open, at)) != std::string::npos) {
        const std::size_t vidAt = at + open.size();
        unsigned long vid = 0;
        unsigned long pid = 0;
        if (readNumber(text, vidAt, 4, 16, vid) &&
            text.compare(vidAt + 4, mid.size(), mid) == 0 &&
            readNumber(text, vidAt + 4 + mid.size(), 4, 16, pid) &&
            text.compare(vidAt + 4 + mid.size() + 4, 1, "}") == 0) {
            out.push_back({static_cast<std::uint16_t>(vid), static_cast<std::uint16_t>(pid)});
        }
        at = vidAt;
    }
    return out;
}

// The union of every native driver's own id table - the same six functions
// the Source section's scan calls (see AppWindow::scanNative()), minus the
// two that are not USB walks at all (SDRplay's service query and SoapySDR).
IdSet nativeDriverIds() {
    IdSet out;
    const std::vector<std::vector<cascade::usb::UsbId>> tables = {
        cascade::source::rtlSdrUsbIds(), cascade::source::hackRfUsbIds(),
        cascade::source::airspyUsbIds(), cascade::source::airspyHfUsbIds(),
        cascade::source::msi2500::usbIds(), cascade::source::rx888UsbIds()};
    for (const std::vector<cascade::usb::UsbId>& table : tables) {
        for (const cascade::usb::UsbId& id : table) { out.insert({id.vid, id.pid}); }
    }
    return out;
}

IdSet toSet(const std::vector<IdPair>& v) { return IdSet(v.begin(), v.end()); }

// Prints what is in `a` and not in `b`, so a red run names the ids rather
// than only the counts. An id missing from the Android filter is a radio a
// phone can never open; one missing from a driver table is a filter entry
// that would launch this app for hardware it cannot use.
void reportMissing(const char* label, const IdSet& a, const IdSet& b) {
    for (const IdPair& id : a) {
        if (b.find(id) == b.end()) {
            std::printf("  %s: %04x:%04x\n", label, id.first, id.second);
        }
    }
}

}  // namespace

int main() {
    const std::string repo = CASCADE_SOURCE_DIR;
    const std::string rulesPath = repo + "/installer/linux/99-foxsdr-sdr.rules";
    const std::string xmlPath = repo + "/android/app/src/main/res/xml/usb_device_filter.xml";
    const std::string javaPath =
        repo + "/android/app/src/main/java/com/foxsdr/app/UsbIds.java";

    const std::string rulesText = readFile(rulesPath);
    const std::string xmlText = readFile(xmlPath);
    const std::string javaText = readFile(javaPath);

    // MISSING FILE IS A FAILURE, NOT A SKIP. All three are checked in.
    CHECK(!rulesText.empty());
    CHECK(!xmlText.empty());
    CHECK(!javaText.empty());
    if (rulesText.empty()) { std::printf("  cannot read %s\n", rulesPath.c_str()); }
    if (xmlText.empty()) { std::printf("  cannot read %s\n", xmlPath.c_str()); }
    if (javaText.empty()) { std::printf("  cannot read %s\n", javaPath.c_str()); }

    // Comments blanked out first - see withoutXmlComments() for the
    // placeholder this file replaced, which had a whole example element in one.
    const std::string xmlLive = withoutXmlComments(xmlText);
    const std::vector<IdPair> rulesList = parseUdevRules(rulesText);
    const std::vector<IdPair> xmlList = parseUsbDeviceFilter(xmlLive);
    const std::vector<IdPair> javaList = parseJavaTable(javaText);

    const IdSet native = nativeDriverIds();
    const IdSet rules = toSet(rulesList);
    const IdSet xml = toSet(xmlList);
    const IdSet java = toSet(javaList);

    // A parser that found nothing would make every comparison below pass by
    // comparing two empty sets, so each list is required to be non-trivial
    // first. 40 is well under the real count (56 when this was written) and
    // is a floor, not a pin: adding a radio must not have to edit this test.
    CHECK(native.size() > 40);
    CHECK(rules.size() > 40);
    CHECK(xml.size() > 40);
    CHECK(java.size() > 40);
    std::printf("ids: native=%zu rules=%zu xml=%zu java=%zu\n", native.size(), rules.size(),
                xml.size(), java.size());

    // NO DUPLICATES in either generated file: a repeated <usb-device> entry is
    // harmless to Android and a repeated Java row is harmless to the filter,
    // but both mean the generator was bypassed, which is the thing this test
    // exists to notice.
    CHECK(xmlList.size() == xml.size());
    CHECK(javaList.size() == java.size());
    CHECK(rulesList.size() == rules.size());

    // THE FOUR-WAY EQUALITY.
    CHECK(rules == native);
    if (rules != native) {
        reportMissing("in the udev rules but no driver table", rules, native);
        reportMissing("in a driver table but not the udev rules", native, rules);
    }
    CHECK(xml == rules);
    if (xml != rules) {
        reportMissing("in usb_device_filter.xml but not the udev rules", xml, rules);
        reportMissing("in the udev rules but not usb_device_filter.xml", rules, xml);
    }
    CHECK(java == rules);
    if (java != rules) {
        reportMissing("in UsbIds.java but not the udev rules", java, rules);
        reportMissing("in the udev rules but not UsbIds.java", rules, java);
    }

    // ANDROID REJECTS HEX IN THIS FILE, and the failure is a resource-compile
    // error a long way from here, so the rule is pinned: no "0x" anywhere
    // inside a <usb-device> element. (The hex form lives in a comment AFTER
    // the element's closing bracket on each line, which is why this looks
    // inside the element only.)
    {
        std::size_t at = 0;
        int elements = 0;
        while ((at = xmlLive.find("<usb-device", at)) != std::string::npos) {
            const std::size_t end = xmlLive.find('>', at);
            CHECK(end != std::string::npos);
            if (end == std::string::npos) { break; }
            CHECK(xmlLive.find("0x", at) > end);
            ++elements;
            at = end;
        }
        CHECK(elements == static_cast<int>(xml.size()));
    }

    // BOTH GENERATED FILES SAY SO. Someone editing one by hand is the whole
    // reason the equality above is checked; the marker is what tells them
    // where the list actually comes from.
    CHECK(xmlText.find("GENERATED FILE") != std::string::npos);
    CHECK(javaText.find("GENERATED FILE") != std::string::npos);
    CHECK(xmlText.find("gen-android-usb-ids.py") != std::string::npos);
    CHECK(javaText.find("gen-android-usb-ids.py") != std::string::npos);

    // The Java helper the USB layer actually calls, exercised on a real id
    // and on one nothing here will ever carry (0xffff:0xffff is not
    // assignable) - its logic lives in the generated file, so this is the
    // only place it is checked at all.
    CHECK(javaText.find("static boolean supported(") != std::string::npos);

    return testSummary("test_usb_android_ids");
}
