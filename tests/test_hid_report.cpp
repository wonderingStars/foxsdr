// test_hid_report.cpp - the HID report-descriptor parser, report decoder, device model and mapping file (input_hid/hid_descriptor.hpp, hid_map.hpp).
// NO DEVICE IS OPENED: every descriptor and report below is a byte array assembled from the item tables of the USB-IF HID 1.11 specification
// (the boot mouse of its Appendix B.2, the joystick of D.1 with its typo corrected, the boot keyboard of Appendix B, and a button box of this
// test's own that uses report ids, 16 buttons and a relative dial).
//
//   H1  the boot mouse (B.2): fields, offsets, sign, a report decoded by hand.
//   H2  the joystick (D.1): push/pop, hat with null state, button ranges, byte layout of the specification's table.
//   H3  the boot keyboard: array fields, modifier bitmap, key down and key up by report diffs.
//   H4  a button box with report ids: id prefix, relative dial, unknown id, short report, output/feature items not decoded as input.
//   H5  malformed descriptors: every error branch; none crashes; bounds.
//   H6  the mapping file: load, counts of every kind of refusal, round trip, transmit/ptt refused.
//   H7  the mapper: buttons, relative, absolute, invert, unbound, disconnected; and no transmit action exists.
//   H8  fuzz: mutated and random descriptors, random reports; invariants.
//   H9  source scan.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <string>
#include <vector>

#include "hamops_util.hpp"
#include "input_hid/hid_descriptor.hpp"
#include "input_hid/hid_map.hpp"
#include "test_check.hpp"

using namespace cascade::input_hid;
using Bytes = std::vector<std::uint8_t>;

namespace {

const Bytes kBootMouse = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x95, 0x03, 0x75, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
                          0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x75, 0x08, 0x95, 0x02, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
                          0x25, 0x7F, 0x81, 0x06, 0xC0, 0xC0};

const Bytes kJoystick = {0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0x05, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0xA4,
                         0x09, 0x30, 0x09, 0x31, 0x81, 0x02, 0x09, 0x39, 0x15, 0x00, 0x25, 0x03, 0x35, 0x00, 0x46, 0x0E, 0x01, 0x65, 0x14, 0x95, 0x01,
                         0x75, 0x04, 0x81, 0x42, 0x15, 0x00, 0x25, 0x01, 0x95, 0x02, 0x75, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x65, 0x00, 0x81,
                         0x02, 0xC0, 0x19, 0x03, 0x29, 0x04, 0x81, 0x02, 0xB4, 0x09, 0x36, 0x95, 0x01, 0x81, 0x02, 0xC0};

const Bytes kBootKeyboard = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
                             0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03,
                             0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};

// Report id 1: 16 buttons then an 8-bit relative dial (Generic Desktop Dial 0x37). Report id 2: an output item (LEDs) and a feature byte; id 3: 8-bit slider.
const Bytes kButtonBox = {0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
                          0x05, 0x01, 0x09, 0x37, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0x85, 0x02, 0x05, 0x08, 0x09, 0x01, 0x15, 0x00,
                          0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x91, 0x02, 0x75, 0x07, 0x91, 0x01, 0x09, 0x02, 0x75, 0x08, 0xB1, 0x02, 0x85, 0x03, 0x05, 0x01,
                          0x09, 0x36, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0xC0};

constexpr std::uint32_t U(std::uint32_t page, std::uint32_t id) { return (page << 16) | id; }

void h1Mouse() {
    const Descriptor d = parseDescriptor(kBootMouse.data(), kBootMouse.size());
    CHECK(d.ok && d.error.empty());
    CHECK(!d.usesReportIds);
    CHECK(d.fields.size() == 2);   // the padding item is constant: no field; buttons, then the X/Y pair
    CHECK(d.applicationUsages.size() == 1 && d.applicationUsages[0] == U(1, 2));
    const Field& b = d.fields[0];
    CHECK(b.bitOffset == 0 && b.size == 1 && b.count == 3 && b.isVariable() && !b.isRelative() && !b.isSigned());
    CHECK(b.usages == (std::vector<std::uint32_t>{U(9, 1), U(9, 2), U(9, 3)}));
    const Field& xy = d.fields[1];
    CHECK(xy.bitOffset == 8 && xy.size == 8 && xy.count == 2 && xy.isRelative() && xy.isSigned() && xy.logMin == -127 && xy.logMax == 127);
    CHECK(xy.usages == (std::vector<std::uint32_t>{U(1, 0x30), U(1, 0x31)}));
    CHECK(d.reportBits.at({0, 0}) == 24);
    std::vector<FieldValue> v;
    const Bytes rep = {0x05, 0xFE, 0x03};   // buttons 1 and 3; X = -2; Y = +3
    CHECK(decodeInputReport(d, rep.data(), rep.size(), v));
    CHECK(v.size() == 5);
    CHECK(v[0].usage == U(9, 1) && v[0].value == 1 && v[1].usage == U(9, 2) && v[1].value == 0 && v[2].usage == U(9, 3) && v[2].value == 1);
    CHECK(v[3].usage == U(1, 0x30) && v[3].value == -2 && v[3].relative);
    CHECK(v[4].usage == U(1, 0x31) && v[4].value == 3);
    // a longer report (the spec: a third device-specific byte may follow) is accepted, a shorter one is refused
    const Bytes longer = {0x00, 0x01, 0x01, 0x77};
    CHECK(decodeInputReport(d, longer.data(), longer.size(), v));
    const Bytes shorter = {0x01, 0x01};
    CHECK(!decodeInputReport(d, shorter.data(), shorter.size(), v) && v.empty());
    CHECK(!decodeInputReport(d, rep.data(), 0, v));
    CHECK(!decodeInputReport(d, nullptr, 3, v));
    // through the device model: events
    HidDevice dev("hid:mouse", d);
    CHECK(dev.valid() && dev.controls().size() == 5);
    std::vector<HidEvent> ev;
    CHECK(dev.parseReport(rep.data(), rep.size(), 1000, ev) == ParseResult::Ok);
    // buttons 1 and 3 down, two relative moves
    int downs = 0, rels = 0;
    for (const HidEvent& e : ev) {
        if (e.kind == EventKind::ButtonDown) { ++downs; }
        if (e.kind == EventKind::Relative) { ++rels; CHECK(e.raw == -2 || e.raw == 3); }
        CHECK(e.timeUs == 1000 && e.device == deviceIdFor("hid:mouse"));
    }
    CHECK(downs == 2 && rels == 2);
    // the same report again: no button events (no change), the relative ones repeat
    CHECK(dev.parseReport(rep.data(), rep.size(), 2000, ev) == ParseResult::Ok);
    downs = 0; rels = 0;
    for (const HidEvent& e : ev) { if (e.kind == EventKind::ButtonDown) { ++downs; } if (e.kind == EventKind::Relative) { ++rels; } }
    CHECK(downs == 0 && rels == 2);
    // release
    const Bytes up = {0x00, 0x00, 0x00};
    CHECK(dev.parseReport(up.data(), up.size(), 3000, ev) == ParseResult::Ok);
    int ups = 0;
    for (const HidEvent& e : ev) { if (e.kind == EventKind::ButtonUp) { ++ups; } CHECK(e.kind != EventKind::Relative); }
    CHECK(ups == 2);
}

void h2Joystick() {
    const Descriptor d = parseDescriptor(kJoystick.data(), kJoystick.size());
    CHECK(d.ok);
    CHECK(d.fields.size() == 5);   // X/Y, hat, buttons 1-2, buttons 3-4, the final usage
    CHECK(d.fields[0].bitOffset == 0 && d.fields[0].count == 2 && d.fields[0].size == 8);
    CHECK(d.fields[1].bitOffset == 16 && d.fields[1].size == 4 && d.fields[1].logMax == 3 && d.fields[1].physDefined && d.fields[1].physMax == 270);
    CHECK((d.fields[1].flags & 0x40u) != 0);                       // null state
    CHECK(d.fields[2].bitOffset == 20 && d.fields[2].usages == (std::vector<std::uint32_t>{U(9, 1), U(9, 2)}));
    CHECK(d.fields[3].bitOffset == 22 && d.fields[3].usages == (std::vector<std::uint32_t>{U(9, 3), U(9, 4)}));
    // after Pop the sizes are back to 8 x 2 but the item declares Report Count 1: one 8-bit field at offset 24, on the Generic Desktop page
    CHECK(d.fields[4].bitOffset == 24 && d.fields[4].size == 8 && d.fields[4].count == 1 && d.fields[4].logMin == -127);
    CHECK(d.fields[4].usages == (std::vector<std::uint32_t>{U(1, 0x36)}));
    CHECK(d.reportBits.at({0, 0}) == 32);
    const Bytes rep = {0x81, 0x7F, 0xA2, 0x40};   // X -127, Y 127, hat 2, buttons 2 and 4, last field 64
    HidDevice dev("hid:joy", d);
    std::vector<HidEvent> ev;
    CHECK(dev.parseReport(rep.data(), rep.size(), 5, ev) == ParseResult::Ok);
    std::map<std::uint32_t, HidEvent> byUsage;
    for (const HidEvent& e : ev) {
        for (const ControlInfo& c : dev.controls()) { if (c.control == e.control) { byUsage[c.usage] = e; } }
    }
    CHECK(byUsage.count(U(1, 0x30)) == 1 && byUsage[U(1, 0x30)].kind == EventKind::Absolute && byUsage[U(1, 0x30)].raw == -127 && byUsage[U(1, 0x30)].value == 0.0f);
    CHECK(byUsage[U(1, 0x31)].raw == 127 && byUsage[U(1, 0x31)].value == 1.0f);
    CHECK(byUsage[U(1, 0x39)].raw == 2 && byUsage[U(1, 0x39)].value > 0.66f && byUsage[U(1, 0x39)].value < 0.67f);
    CHECK(byUsage.count(U(9, 2)) == 1 && byUsage[U(9, 2)].kind == EventKind::ButtonDown);
    CHECK(byUsage.count(U(9, 4)) == 1 && byUsage[U(9, 4)].kind == EventKind::ButtonDown);
    CHECK(byUsage.count(U(9, 1)) == 0 && byUsage.count(U(9, 3)) == 0);
    CHECK(byUsage[U(1, 0x36)].raw == 64 && byUsage[U(1, 0x36)].value > 0.75f && byUsage[U(1, 0x36)].value < 0.76f);   // (64+127)/254
    // an axis that does not move is not reported again; one that does is
    const Bytes rep2 = {0x81, 0x00, 0xA2, 0x40};
    CHECK(dev.parseReport(rep2.data(), rep2.size(), 6, ev) == ParseResult::Ok);
    CHECK(ev.size() == 1 && ev[0].kind == EventKind::Absolute && ev[0].raw == 0);
    // a 3-byte report is too short
    CHECK(dev.parseReport(rep.data(), 3, 7, ev) == ParseResult::BadReport && ev.empty());
}

void h3Keyboard() {
    const Descriptor d = parseDescriptor(kBootKeyboard.data(), kBootKeyboard.size());
    CHECK(d.ok);
    CHECK(d.fields.size() == 3);   // modifiers (8 x 1 bit), the reserved byte is constant: skipped; LEDs are Output; the key array
    CHECK(d.fields[0].count == 8 && d.fields[0].usages[0] == U(7, 0xE0) && d.fields[0].usages[7] == U(7, 0xE7));
    // the LED output fields are Output items: present with type Output, not in the Input decode
    CHECK(d.fields[1].type == ReportType::Output);
    const Field& arr = d.fields[2];
    CHECK(arr.isArray() && arr.count == 6 && arr.size == 8 && arr.hasUsageRange && arr.usageMin == U(7, 0) && arr.usageMax == U(7, 0x65) && arr.bitOffset == 16);
    CHECK(d.reportBits.at({0, 0}) == 64 && d.reportBits.at({1, 0}) == 8);
    HidDevice dev("hid:kbd", d);
    std::vector<HidEvent> ev;
    const Bytes r1 = {0x00, 0x00, 0x04, 0x3A, 0x5D, 0x00, 0x00, 0x00};   // the specification's example: A, F1, keypad 5
    CHECK(dev.parseReport(r1.data(), r1.size(), 1, ev) == ParseResult::Ok);
    CHECK(ev.size() == 3);
    std::set<std::uint32_t> usages;
    for (const HidEvent& e : ev) {
        CHECK(e.kind == EventKind::ButtonDown);
        for (const ControlInfo& c : dev.controls()) { if (c.control == e.control) { usages.insert(c.usage); } }
    }
    CHECK(usages == (std::set<std::uint32_t>{U(7, 4), U(7, 0x3A), U(7, 0x5D)}));
    // the order of codes in an array has no meaning: the same keys in another order is no event
    const Bytes r2 = {0x00, 0x00, 0x5D, 0x04, 0x3A, 0x00, 0x00, 0x00};
    CHECK(dev.parseReport(r2.data(), r2.size(), 2, ev) == ParseResult::Ok && ev.empty());
    // F1 released
    const Bytes r3 = {0x00, 0x00, 0x04, 0x5D, 0x00, 0x00, 0x00, 0x00};
    CHECK(dev.parseReport(r3.data(), r3.size(), 3, ev) == ParseResult::Ok);
    CHECK(ev.size() == 1 && ev[0].kind == EventKind::ButtonUp && ev[0].control == dev.controlForUsage(U(7, 0x3A)));
    // left shift (modifier bit 1) down
    const Bytes r4 = {0x02, 0x00, 0x04, 0x5D, 0x00, 0x00, 0x00, 0x00};
    CHECK(dev.parseReport(r4.data(), r4.size(), 4, ev) == ParseResult::Ok);
    CHECK(ev.size() == 1 && ev[0].kind == EventKind::ButtonDown && ev[0].control == dev.controlForUsage(U(7, 0xE1)));
    // an index beyond the Logical Maximum (0x66: the phantom/overflow area) selects nothing
    const Bytes r5 = {0x00, 0x00, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(dev.parseReport(r5.data(), r5.size(), 5, ev) == ParseResult::Ok);
    for (const HidEvent& e : ev) { CHECK(e.kind == EventKind::ButtonUp); }   // 04, 5D and shift released; nothing pressed
    // reset forgets the held state
    dev.parseReport(r1.data(), r1.size(), 6, ev);
    dev.reset();
    CHECK(dev.parseReport(r1.data(), r1.size(), 7, ev) == ParseResult::Ok && ev.size() == 3);
}

void h4ReportIds() {
    const Descriptor d = parseDescriptor(kButtonBox.data(), kButtonBox.size());
    CHECK(d.ok && d.usesReportIds);
    CHECK(d.reportBits.at({0, 1}) == 24 && d.reportBits.at({1, 2}) == 8 && d.reportBits.at({2, 2}) == 8 && d.reportBits.at({0, 3}) == 8);
    HidDevice dev("hid:box", d);
    CHECK(dev.controls().size() == 16 + 1 + 1);   // 16 buttons, the dial, the slider
    std::vector<HidEvent> ev;
    const Bytes r = {0x01, 0x05, 0x00, 0xFF};   // id 1: buttons 1 and 3; dial -1
    CHECK(dev.parseReport(r.data(), r.size(), 10, ev) == ParseResult::Ok);
    int downs = 0;
    for (const HidEvent& e : ev) {
        if (e.kind == EventKind::ButtonDown) { ++downs; CHECK(e.control == dev.controlForUsage(U(9, 1)) || e.control == dev.controlForUsage(U(9, 3))); }
        if (e.kind == EventKind::Relative) { CHECK(e.raw == -1 && e.control == dev.controlForUsage(U(1, 0x37))); }
    }
    CHECK(downs == 2 && ev.size() == 3);
    // button 16 is the top bit of the second byte
    const Bytes r16 = {0x01, 0x05, 0x80, 0x00};
    CHECK(dev.parseReport(r16.data(), r16.size(), 11, ev) == ParseResult::Ok);
    bool sawB16 = false;
    for (const HidEvent& e : ev) { if (e.kind == EventKind::ButtonDown && e.control == dev.controlForUsage(U(9, 16))) { sawB16 = true; } }
    CHECK(sawB16);
    // id 3: the slider (unsigned 8-bit, 0..255)
    const Bytes s = {0x03, 0xFF};
    CHECK(dev.parseReport(s.data(), s.size(), 12, ev) == ParseResult::Ok);
    CHECK(ev.size() == 1 && ev[0].kind == EventKind::Absolute && ev[0].raw == 255 && ev[0].value == 1.0f);
    // an id the descriptor does not define; id 2 has no Input report (only Output and Feature); a report of only the id; a short one
    const Bytes unk = {0x09, 0x01, 0x02, 0x03};
    CHECK(dev.parseReport(unk.data(), unk.size(), 13, ev) == ParseResult::BadReport && ev.empty());
    const Bytes two = {0x02, 0x01};
    CHECK(dev.parseReport(two.data(), two.size(), 14, ev) == ParseResult::BadReport);
    const Bytes onlyId = {0x01};
    CHECK(dev.parseReport(onlyId.data(), onlyId.size(), 15, ev) == ParseResult::BadReport);
    const Bytes shortR = {0x01, 0x05, 0x00};
    CHECK(dev.parseReport(shortR.data(), shortR.size(), 16, ev) == ParseResult::BadReport);
    CHECK(dev.parseReport(nullptr, 0, 17, ev) == ParseResult::BadReport);
    // a device built from a bad descriptor is invalid and refuses reports
    HidDevice bad("hid:bad", parseDescriptor(nullptr, 0));
    CHECK(!bad.valid() && bad.controls().empty() && bad.parseReport(r.data(), r.size(), 1, ev) == ParseResult::BadReport);
}

void h5Malformed() {
    auto bad = [](const Bytes& b) { const Descriptor d = parseDescriptor(b.data(), b.size()); return !d.ok && !d.error.empty() && d.fields.empty(); };
    CHECK(bad({}));
    CHECK(!parseDescriptor(nullptr, 5).ok);
    CHECK(bad(Bytes(kMaxDescriptorBytes + 1, 0x00)));
    { const Bytes z(kMaxDescriptorBytes, 0x00); const Descriptor dz = parseDescriptor(z.data(), z.size()); CHECK(dz.ok && dz.fields.empty()); }   // 4096 reserved main items: tolerated, no fields
    CHECK(bad({0x05}));                                                   // size 1, no data
    CHECK(bad({0x06, 0x01}));                                             // size 2, one byte
    CHECK(bad({0x07, 0x01, 0x02, 0x03}));                                 // size 4, three bytes
    CHECK(bad({0xFE}));                                                   // long item with no size
    CHECK(bad({0xFE, 0x05, 0x10, 0x01}));                                 // long item cut off
    { const Bytes l = {0x05, 0x01, 0xFE, 0x02, 0x10, 0xAA, 0xBB, 0x09, 0x01}; const Descriptor dl2 = parseDescriptor(l.data(), l.size()); CHECK(dl2.ok && dl2.fields.empty()); }   // a long item is skipped
    CHECK(bad({0xC0}));                                                   // End Collection without Collection
    CHECK(bad({0xA1, 0x01}));                                             // Collection never closed
    CHECK(bad({0xB4}));                                                   // Pop without Push
    CHECK(bad({0x0C}));                                                   // reserved item type 3
    CHECK(bad({0x85, 0x00}));                                             // report id 0
    CHECK(bad({0x86, 0x00, 0x01}));                                       // report id 256
    Bytes pushes;
    for (int i = 0; i < 9; ++i) { pushes.push_back(0xA4); }
    CHECK(bad(pushes));                                                   // Push nests deeper than 8
    Bytes cols;
    for (int i = 0; i < 17; ++i) { cols.push_back(0xA1); cols.push_back(0x01); }
    CHECK(bad(cols));                                                     // 17 collections
    // a 40-bit data field
    CHECK(bad({0x75, 0x28, 0x95, 0x01, 0x81, 0x02}));
    // a field larger than any report
    CHECK(bad({0x75, 0x08, 0x96, 0xFF, 0xFF, 0x81, 0x02}));
    // two fields that together pass the report size bound
    CHECK(bad({0x75, 0x08, 0x96, 0x00, 0x04, 0x81, 0x02, 0x81, 0x02, 0x81, 0x02}));
    // too many fields: 513 Input items
    Bytes many = {0x75, 0x01, 0x95, 0x01};
    for (int i = 0; i < 513; ++i) { many.push_back(0x81); many.push_back(0x02); }
    CHECK(bad(many));
    Bytes ok512 = {0x75, 0x01, 0x95, 0x01};
    for (int i = 0; i < 512; ++i) { ok512.push_back(0x81); ok512.push_back(0x02); }
    const Descriptor d512 = parseDescriptor(ok512.data(), ok512.size());
    CHECK(d512.ok && d512.fields.size() == 512 && d512.reportBits.at({0, 0}) == 512);
    // an empty main item (no usage, no fields) is not an error and yields no controls
    HidDevice nodev("hid:none", parseDescriptor(ok512.data(), ok512.size()));
    CHECK(nodev.valid() && nodev.controls().empty());
    // sign handling of the global items: a 1-byte 0xFF is -1, a 2-byte 0x00FF is 255
    const Bytes sg = {0x15, 0xFF, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02};
    const Descriptor ds = parseDescriptor(sg.data(), sg.size());
    CHECK(ds.ok && ds.fields.size() == 1 && ds.fields[0].logMin == -1 && ds.fields[0].logMax == 255);
    // a Usage with 4 data bytes is an extended usage that overrides the page
    const Bytes ext = {0x05, 0x01, 0x0B, 0x01, 0x00, 0x0C, 0x00, 0x75, 0x01, 0x95, 0x01, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02};
    const Descriptor de = parseDescriptor(ext.data(), ext.size());
    CHECK(de.ok && de.fields.size() == 1 && de.fields[0].usages == (std::vector<std::uint32_t>{U(0x0C, 1)}));
    // the last usage serves the remaining controls of an item
    const Bytes lastUse = {0x05, 0x09, 0x09, 0x01, 0x09, 0x02, 0x75, 0x01, 0x95, 0x04, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02};
    const Descriptor dl = parseDescriptor(lastUse.data(), lastUse.size());
    CHECK(dl.ok && dl.fields[0].usages == (std::vector<std::uint32_t>{U(9, 1), U(9, 2), U(9, 2), U(9, 2)}));
    // local items do not carry over to the next main item
    const Bytes carry = {0x05, 0x09, 0x09, 0x05, 0x75, 0x01, 0x95, 0x01, 0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x81, 0x02};
    const Descriptor dc = parseDescriptor(carry.data(), carry.size());
    CHECK(dc.ok && dc.fields.size() == 2 && dc.fields[0].usages[0] == U(9, 5) && dc.fields[1].usages[0] == 0);
}

void h6MappingFile() {
    MappingFile m;
    MapLoadReport r;
    const std::string good = R"({"version":1,"bindings":[
        {"page":9,"usage":1,"action":"step.up"},
        {"page":9,"usage":2,"action":"step.down","invert":true},
        {"page":1,"usage":55,"action":"tune","params":{"x":1}},
        {"page":1,"usage":54,"action":"volume"}]})";
    CHECK(loadMapping(good, m, r));
    CHECK(r.bindingsRead == 4 && r.skippedMalformed == 0 && m.bindings.size() == 4);
    CHECK(m.bindings[1].invert && m.bindings[2].params["x"] == 1);
    // refusals, each counted
    const std::string messy = R"({"version":1,"bindings":[
        7,
        {"page":9,"usage":1},
        {"page":"9","usage":1,"action":"tune"},
        {"page":9,"usage":0,"action":"tune"},
        {"page":70000,"usage":1,"action":"tune"},
        {"page":9,"usage":3,"action":"transmit"},
        {"page":9,"usage":4,"action":"PTT.on"},
        {"page":9,"usage":5,"action":"command","params":{"name":"ptt-toggle"}},
        {"page":9,"usage":6,"action":"fly"},
        {"page":9,"usage":7,"action":"tune"},
        {"page":9,"usage":7,"action":"volume"},
        {"page":9,"usage":8,"action":"command","params":{"name":"mute"}}]})";
    CHECK(loadMapping(messy, m, r));
    CHECK(r.skippedMalformed == 5);        // the integer, the missing action, the string page, usage 0, page 70000
    CHECK(r.refusedForbidden == 3);        // transmit, PTT.on, params.name ptt-toggle
    CHECK(r.unknownActions == 1);
    CHECK(r.duplicates == 1);
    CHECK(r.bindingsRead == 2 && m.bindings.size() == 2 && m.bindings[0].action == "tune" && m.bindings[1].params["name"] == "mute");
    // unparseable, wrong shape, version, size
    CHECK(!loadMapping("{{{", m, r) && r.unparseable);
    CHECK(!loadMapping("[1,2]", m, r) && r.unparseable);
    CHECK(!loadMapping("", m, r) && r.unparseable);
    CHECK(!loadMapping(R"({"version":2,"bindings":[]})", m, r) && r.wrongVersion);
    CHECK(!loadMapping(R"({"version":"1","bindings":[]})", m, r) && r.wrongVersion);
    CHECK(!loadMapping(std::string(kMaxMappingBytes + 1, ' '), m, r) && r.tooBig);
    CHECK(loadMapping(R"({"version":1})", m, r) && m.bindings.empty());           // no bindings key: empty, fine
    CHECK(loadMapping(R"({"version":1,"bindings":"x"})", m, r) && m.bindings.empty());
    // at most 256 bindings are taken
    std::string big = R"({"version":1,"bindings":[)";
    for (int i = 1; i <= 300; ++i) { big += (i > 1 ? "," : "") + std::string("{\"page\":9,\"usage\":") + std::to_string(i) + ",\"action\":\"tune\"}"; }
    big += "]}";
    CHECK(loadMapping(big, m, r) && m.bindings.size() == kMaxBindings);
    // a round trip keeps every binding
    CHECK(loadMapping(good, m, r));
    MappingFile again;
    MapLoadReport r2;
    CHECK(loadMapping(saveMapping(m), again, r2));
    CHECK(again.bindings.size() == m.bindings.size() && r2.bindingsRead == 4);
    for (std::size_t i = 0; i < m.bindings.size(); ++i) {
        CHECK(again.bindings[i].usagePage == m.bindings[i].usagePage && again.bindings[i].usage == m.bindings[i].usage && again.bindings[i].action == m.bindings[i].action &&
              again.bindings[i].invert == m.bindings[i].invert && again.bindings[i].params == m.bindings[i].params);
    }
}

void h7Mapper() {
    const Descriptor d = parseDescriptor(kButtonBox.data(), kButtonBox.size());
    HidDevice dev("hid:box", d);
    MappingFile m;
    MapLoadReport r;
    CHECK(loadMapping(R"({"version":1,"bindings":[
        {"page":9,"usage":1,"action":"step.up"},
        {"page":1,"usage":55,"action":"tune"},
        {"page":1,"usage":54,"action":"volume","invert":true},
        {"page":9,"usage":200,"action":"zoom"}]})", m, r));
    HidMapper map(dev, m);
    CHECK(map.boundCount() == 3 && map.bindingsWithoutControl() == 1);
    std::vector<HidEvent> ev;
    ActionOut a;
    const Bytes press = {0x01, 0x01, 0x00, 0x00};
    dev.parseReport(press.data(), press.size(), 1, ev);
    CHECK(ev.size() == 1 && map.translate(ev[0], a) && a.action == "step.up" && a.isButton && a.pressed);
    const Bytes rel = {0x01, 0x01, 0x00, 0x03};
    dev.parseReport(rel.data(), rel.size(), 2, ev);
    CHECK(ev.size() == 1 && map.translate(ev[0], a) && a.action == "tune" && a.detents == 3 && !a.isButton);
    const Bytes release = {0x01, 0x00, 0x00, 0x00};
    dev.parseReport(release.data(), release.size(), 3, ev);
    CHECK(ev.size() == 1 && map.translate(ev[0], a) && a.isButton && !a.pressed);
    // an absolute control, inverted: 0..1 mirrored
    const Bytes sl = {0x03, 0xFF};
    dev.parseReport(sl.data(), sl.size(), 4, ev);
    CHECK(ev.size() == 1 && map.translate(ev[0], a) && a.action == "volume" && a.isAbsolute && a.value == 0.0f);
    // a control with no binding yields nothing; so does a Disconnected event
    const Bytes b2 = {0x01, 0x02, 0x00, 0x00};
    dev.parseReport(b2.data(), b2.size(), 5, ev);
    CHECK(ev.size() == 1 && !map.translate(ev[0], a));
    HidEvent gone;
    gone.kind = EventKind::Disconnected;
    gone.control = dev.controlForUsage(U(9, 1));
    CHECK(!map.translate(gone, a));
    // invert on a relative control (a separate mapping), including the integer minimum
    MappingFile m2;
    CHECK(loadMapping(R"({"version":1,"bindings":[{"page":1,"usage":55,"action":"tune","invert":true}]})", m2, r));
    HidMapper inv(dev, m2);
    HidEvent e;
    e.kind = EventKind::Relative;
    e.control = dev.controlForUsage(U(1, 0x37));
    e.raw = 4;
    CHECK(inv.translate(e, a) && a.detents == -4);
    e.raw = std::numeric_limits<std::int32_t>::min();
    CHECK(inv.translate(e, a) && a.detents == std::numeric_limits<std::int32_t>::max());
    // NO TRANSMIT ACTION EXISTS: the vocabulary has none, and every spelling is refused at load time
    for (const char* n : kActionNames) { CHECK(!isForbiddenName(n)); }
    CHECK(isForbiddenName("Transmit") && isForbiddenName("ptt") && isForbiddenName("PTT.hold") && isForbiddenName("transmit.on") && !isForbiddenName("mute"));
    CHECK(!isKnownAction("transmit") && !isKnownAction("ptt") && isKnownAction("radio.select"));
}

void h8Fuzz() {
    hamops_test::Rng rng(9);
    const std::vector<const Bytes*> seeds = {&kBootMouse, &kJoystick, &kBootKeyboard, &kButtonBox};
    int okCount = 0;
    for (int it = 0; it < 20000; ++it) {
        Bytes b;
        const int mode = static_cast<int>(rng.below(4));
        if (mode == 0) {
            const std::size_t n = rng.below(200);
            for (std::size_t i = 0; i < n; ++i) { b.push_back(static_cast<std::uint8_t>(rng.below(256))); }
        } else {
            b = *seeds[rng.below(4)];
            const int muts = 1 + static_cast<int>(rng.below(4));
            for (int k = 0; k < muts && !b.empty(); ++k) {
                const std::size_t pos = rng.below(static_cast<std::uint32_t>(b.size()));
                const int op = static_cast<int>(rng.below(3));
                if (op == 0) { b[pos] = static_cast<std::uint8_t>(rng.below(256)); }
                else if (op == 1) { b.erase(b.begin() + static_cast<std::ptrdiff_t>(pos)); }
                else { b.insert(b.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<std::uint8_t>(rng.below(256))); }
            }
            if (mode == 3 && !b.empty()) { b.resize(rng.below(static_cast<std::uint32_t>(b.size()) + 1)); }
        }
        const Descriptor d = parseDescriptor(b.data(), b.size());
        if (!d.ok) { CHECK(d.fields.empty() && !d.error.empty()); continue; }
        ++okCount;
        CHECK(d.fields.size() <= kMaxFields);
        for (const Field& f : d.fields) {
            CHECK(static_cast<std::uint64_t>(f.bitOffset) + static_cast<std::uint64_t>(f.size) * f.count <= kMaxReportBits);
            CHECK(f.isConstant() || f.size <= 32);
        }
        HidDevice dev("hid:fuzz", d);
        CHECK(dev.controls().size() <= kMaxControls);
        std::vector<HidEvent> ev;
        for (int r = 0; r < 5; ++r) {
            Bytes rep(rng.below(40));
            for (auto& x : rep) { x = static_cast<std::uint8_t>(rng.below(256)); }
            const ParseResult pr = dev.parseReport(rep.data(), rep.size(), r, ev);
            if (pr == ParseResult::BadReport) { CHECK(ev.empty()); }
            for (const HidEvent& e : ev) {
                CHECK(e.value >= 0.0f && e.value <= 1.0f);
                if (e.kind == EventKind::Relative) { CHECK(e.raw != 0); }
            }
        }
    }
    CHECK(okCount > 500);
    // the mapping loader against random text
    for (int it = 0; it < 5000; ++it) {
        std::string t;
        const std::size_t n = rng.below(120);
        const std::string alpha = "{}[]\":,0123456789 abcpageusrnolvfi.";
        for (std::size_t i = 0; i < n; ++i) { t.push_back(alpha[rng.below(static_cast<std::uint32_t>(alpha.size()))]); }
        MappingFile m;
        MapLoadReport r;
        loadMapping(t, m, r);
        CHECK(m.bindings.size() <= kMaxBindings);
        for (const Binding& b : m.bindings) { CHECK(isKnownAction(b.action) && !isForbiddenName(b.action)); }
    }
}

void h9SourceScan() {
    for (const char* f : {"src/input_hid/hid_descriptor.hpp", "src/input_hid/hid_map.hpp"}) {
        const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile(f));
        CHECK(!src.empty());
        for (const char* bad : {"stderr", "stdout", "std::cerr", "std::cout", "settings_set", "CreateFile", "hidsdi", "HidD_", "SetupDi", "std::thread", "fopen", "ofstream",
                                "#include <windows.h>", "OutputDebugString"}) {
            CHECK(src.find(bad) == std::string::npos);
        }
    }
    // the event type is a copy, the knob lane's header is not included
    const std::string raw = hamops_test::srcFile("src/input_hid/hid_map.hpp");
    std::size_t pos = 0;
    int includes = 0;
    while ((pos = raw.find("#include", pos)) != std::string::npos) {
        const std::size_t eol = raw.find(static_cast<char>(10), pos);
        const std::string line = raw.substr(pos, eol - pos);
        CHECK(line.find("input_event") == std::string::npos && line.find("input/") == std::string::npos);
        ++includes;
        pos = eol;
    }
    CHECK(includes > 5);
}

}  // namespace

int main() {
    h1Mouse();
    h2Joystick();
    h3Keyboard();
    h4ReportIds();
    h5Malformed();
    h6MappingFile();
    h7Mapper();
    h8Fuzz();
    h9SourceScan();
    return testSummary("test_hid_report");
}
