// input_hid/hid_descriptor.hpp - a parser for USB HID report descriptors and a decoder for the reports they describe. PURE: no Windows header, no
// device is opened, no thread, no clock. Feed it recorded descriptor bytes and recorded reports.
//
// SOURCE: USB-IF "Device Class Definition for HID 1.11" (https://www.usb.org/sites/default/files/hid1_11.pdf, opened and read as text 2026-11-11):
//   6.2.2.2 short items (prefix byte: bSize bits 0-1 with 0,1,2,3 = 0,1,2,4 data bytes; bType bits 2-3 with 0 Main, 1 Global, 2 Local; bTag bits
//   4-7), 6.2.2.3 long items (prefix 0xFE, a data-size byte, a long-item tag byte, then the data; no long item tags are defined),
//   6.2.2.4 Main items (Input 1000, Output 1001, Feature 1011, Collection 1010, End Collection 1100; Input/Output/Feature data bits: 0 Data/Constant,
//   1 Array/Variable, 2 Absolute/Relative, 3 No Wrap/Wrap, 6 No Null/Null state), 6.2.2.7 Global items (Usage Page 0000, Logical Min 0001, Logical Max
//   0010, Physical Min 0011, Physical Max 0100, Unit Exponent 0101, Unit 0110, Report Size 0111, Report ID 1000, Report Count 1001, Push 1010,
//   Pop 1011), 6.2.2.8 Local items (Usage 0000, Usage Minimum 0001, Usage Maximum 0010; "Local items do not carry over to the next Main item"; with
//   several Usages the last one also serves the remaining controls of the item), 5.8 and 6.2.2.7 (multibyte values little endian; Logical Min/Max
//   both >= 0 -> unsigned field, otherwise two's complement), 6.2.2.8 (a Usage item with bSize = 3, i.e. 4 bytes, is an extended usage: page in the
//   high 16 bits), 8.3 (an array field reports the index of each closed control; with Usage Min/Max the index selects Usage Min + index - Logical Min).
//   If any Report ID item appears, every report of the device starts with a one-byte report id.
// Not implemented (descriptor items read and ignored): Physical Min/Max and Unit (kept, not used), Designators, Strings, Delimiters.
//
// BOUNDS (a descriptor is outside bytes): at most 4096 bytes, 512 fields, collection depth 16, global stack depth 8, a report at most 1024 bytes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cascade::input_hid {

inline constexpr std::size_t kMaxDescriptorBytes = 4096;
inline constexpr std::size_t kMaxFields = 512;
inline constexpr std::size_t kMaxCollectionDepth = 16;
inline constexpr std::size_t kMaxPushDepth = 8;
inline constexpr std::uint32_t kMaxReportBits = 1024 * 8;

enum class ReportType : std::uint8_t { Input = 0, Output = 1, Feature = 2 };

struct Field {
    ReportType type = ReportType::Input;
    std::uint8_t reportId = 0;           // 0 = the device uses no report ids
    std::uint32_t bitOffset = 0;         // from the first payload bit (after the report-id byte, if any)
    std::uint32_t size = 0;              // bits per element
    std::uint32_t count = 0;             // elements
    std::uint32_t flags = 0;             // the Input/Output/Feature data bits
    std::uint32_t usagePage = 0;         // current page when the item was declared
    std::vector<std::uint32_t> usages;   // variable fields: one 32-bit usage (page << 16 | id) per element; empty when none was declared
    std::uint32_t usageMin = 0, usageMax = 0;   // arrays: the 32-bit usage range (0,0 when not declared)
    bool hasUsageRange = false;
    std::int32_t logMin = 0, logMax = 0;
    std::int32_t physMin = 0, physMax = 0;
    bool physDefined = false;
    int collectionDepth = 0;

    bool isConstant() const { return (flags & 1u) != 0; }
    bool isVariable() const { return (flags & 2u) != 0; }
    bool isArray() const { return !isVariable(); }
    bool isRelative() const { return (flags & 4u) != 0; }
    bool isSigned() const { return logMin < 0 || logMax < 0; }
};

struct Descriptor {
    bool ok = false;
    std::string error;                    // a sentence when !ok
    bool usesReportIds = false;
    std::vector<Field> fields;
    // total payload bits per (type, report id)
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::uint32_t> reportBits;
    std::vector<std::uint32_t> applicationUsages;   // the top-level collections' usages (page << 16 | id)
};

namespace hid_detail {

inline std::uint32_t readU(const std::uint8_t* p, std::size_t n) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < n; ++i) { v |= static_cast<std::uint32_t>(p[i]) << (8 * i); }
    return v;
}
inline std::int32_t readS(const std::uint8_t* p, std::size_t n) {
    const std::uint32_t u = readU(p, n);
    if (n == 0) { return 0; }
    if (n < 4) {
        const std::uint32_t sign = 1u << (8 * n - 1);
        if (u & sign) { return static_cast<std::int32_t>(u | ~((sign << 1) - 1)); }
    }
    return static_cast<std::int32_t>(u);
}

struct Global {
    std::uint32_t usagePage = 0, reportSize = 0, reportCount = 0;
    std::uint8_t reportId = 0;
    std::int32_t logMin = 0, logMax = 0, physMin = 0, physMax = 0;
    bool physDefined = false;
};
struct Local {
    std::vector<std::uint32_t> usages;
    bool haveMin = false, haveMax = false;
    std::uint32_t usageMin = 0, usageMax = 0;
    void clear() { usages.clear(); haveMin = haveMax = false; usageMin = usageMax = 0; }
};

}  // namespace hid_detail

// Parses a report descriptor. Never throws, never reads outside [data, data+len). On any malformed item the result has ok=false and a sentence;
// fields found before the fault are discarded.
inline Descriptor parseDescriptor(const std::uint8_t* data, std::size_t len) {
    using namespace hid_detail;
    Descriptor d;
    auto fail = [&](const char* msg) {
        d = Descriptor();
        d.error = msg;
        return d;
    };
    if (data == nullptr || len == 0) { return fail("the descriptor is empty"); }
    if (len > kMaxDescriptorBytes) { return fail("the descriptor is longer than 4096 bytes"); }
    Global g;
    Local l;
    std::vector<Global> stack;
    int depth = 0;
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::uint32_t> offsets;
    std::size_t i = 0;
    while (i < len) {
        const std::uint8_t prefix = data[i++];
        if (prefix == 0xFE) {   // long item: size byte, tag byte, data (no tags are defined: skipped)
            if (i + 2 > len) { return fail("a long item is cut off"); }
            const std::size_t n = data[i];
            i += 2;
            if (i + n > len) { return fail("a long item is cut off"); }
            i += n;
            continue;
        }
        const std::size_t sizes[4] = {0, 1, 2, 4};
        const std::size_t n = sizes[prefix & 3u];
        const unsigned type = (prefix >> 2) & 3u;
        const unsigned tag = prefix >> 4;
        if (i + n > len) { return fail("an item is cut off"); }
        const std::uint8_t* p = data + i;
        i += n;
        if (type == 3u) { return fail("a reserved item type was found"); }
        if (type == 1u) {   // Global
            switch (tag) {
                case 0x0: g.usagePage = readU(p, n); break;
                case 0x1: g.logMin = readS(p, n); break;
                case 0x2: g.logMax = readS(p, n); break;
                case 0x3: g.physMin = readS(p, n); g.physDefined = true; break;
                case 0x4: g.physMax = readS(p, n); g.physDefined = true; break;
                case 0x5: case 0x6: break;   // unit exponent, unit: kept out of the model
                case 0x7: g.reportSize = readU(p, n); break;
                case 0x8: {
                    const std::uint32_t id = readU(p, n);
                    if (id == 0 || id > 255) { return fail("a report id outside 1..255"); }
                    g.reportId = static_cast<std::uint8_t>(id);
                    d.usesReportIds = true;
                    break;
                }
                case 0x9: g.reportCount = readU(p, n); break;
                case 0xA:
                    if (stack.size() >= kMaxPushDepth) { return fail("Push nests deeper than 8"); }
                    stack.push_back(g);
                    break;
                case 0xB:
                    if (stack.empty()) { return fail("Pop without Push"); }
                    g = stack.back();
                    stack.pop_back();
                    break;
                default: break;   // reserved globals: ignored
            }
            continue;
        }
        if (type == 2u) {   // Local
            switch (tag) {
                case 0x0: {
                    std::uint32_t u = readU(p, n);
                    if (n != 4) { u = (g.usagePage << 16) | (u & 0xFFFFu); }
                    if (l.usages.size() < 1024) { l.usages.push_back(u); }
                    break;
                }
                case 0x1: l.usageMin = (n == 4) ? readU(p, n) : ((g.usagePage << 16) | (readU(p, n) & 0xFFFFu)); l.haveMin = true; break;
                case 0x2: l.usageMax = (n == 4) ? readU(p, n) : ((g.usagePage << 16) | (readU(p, n) & 0xFFFFu)); l.haveMax = true; break;
                default: break;   // designators, strings, delimiters: ignored
            }
            continue;
        }
        // Main
        if (tag == 0xA) {   // Collection
            if (static_cast<std::size_t>(depth) >= kMaxCollectionDepth) { return fail("collections nest deeper than 16"); }
            if (depth == 0 && n >= 1 && p[0] == 0x01 && !l.usages.empty()) { d.applicationUsages.push_back(l.usages.front()); }
            ++depth;
            l.clear();
            continue;
        }
        if (tag == 0xC) {   // End Collection
            if (depth == 0) { return fail("End Collection without Collection"); }
            --depth;
            l.clear();
            continue;
        }
        if (tag == 0x8 || tag == 0x9 || tag == 0xB) {   // Input, Output, Feature
            const ReportType rt = tag == 0x8 ? ReportType::Input : tag == 0x9 ? ReportType::Output : ReportType::Feature;
            Field f;
            f.type = rt;
            f.reportId = g.reportId;
            f.flags = readU(p, n);
            f.size = g.reportSize;
            f.count = g.reportCount;
            f.usagePage = g.usagePage;
            f.logMin = g.logMin;
            f.logMax = g.logMax;
            f.physMin = g.physMin;
            f.physMax = g.physMax;
            f.physDefined = g.physDefined;
            f.collectionDepth = depth;
            if (f.size > 32 && !f.isConstant()) { return fail("a data field wider than 32 bits"); }
            if (static_cast<std::uint64_t>(f.size) * f.count > kMaxReportBits) { return fail("a field is larger than a 1024-byte report"); }
            auto& off = offsets[{static_cast<std::uint8_t>(rt), f.reportId}];
            f.bitOffset = off;
            if (static_cast<std::uint64_t>(off) + static_cast<std::uint64_t>(f.size) * f.count > kMaxReportBits) { return fail("a report is larger than 1024 bytes"); }
            off += f.size * f.count;
            if (!f.isConstant() && f.count > 0 && f.size > 0) {
                if (f.isVariable()) {
                    f.usages.assign(f.count, 0);
                    std::size_t u = 0;
                    for (std::uint32_t k = 0; k < f.count; ++k) {
                        std::uint32_t usage = 0;
                        if (!l.usages.empty()) {
                            usage = l.usages[std::min<std::size_t>(k, l.usages.size() - 1)];   // the last usage serves the remaining controls
                        } else if (l.haveMin && l.haveMax) {
                            const std::uint64_t cand = static_cast<std::uint64_t>(l.usageMin) + k;
                            usage = static_cast<std::uint32_t>(cand > l.usageMax ? l.usageMax : cand);
                        }
                        f.usages[k] = usage;
                        (void)u;
                    }
                } else {
                    if (l.haveMin && l.haveMax) { f.usageMin = l.usageMin; f.usageMax = l.usageMax; f.hasUsageRange = true; }
                    else if (!l.usages.empty()) { f.usages = l.usages; }
                }
                if (d.fields.size() >= kMaxFields) { return fail("more than 512 fields"); }
                d.fields.push_back(std::move(f));
            }
            l.clear();
            continue;
        }
        // reserved main tags
        l.clear();
    }
    if (depth != 0) { return fail("a Collection is not closed"); }
    for (const auto& kv : offsets) { d.reportBits[kv.first] = kv.second; }
    d.ok = true;
    return d;
}

// One decoded element of an Input report.
struct FieldValue {
    std::size_t field = 0;       // index into Descriptor::fields
    std::uint32_t element = 0;   // which element of the field
    std::uint32_t usage = 0;     // page << 16 | id; 0 when the descriptor gave none (array elements: the resolved usage, 0 when the index selects nothing)
    std::int32_t value = 0;      // sign-extended when the field is signed
    bool relative = false;
    bool isArrayIndex = false;   // value is an array index, usage already resolved
};

// The bits [offset, offset+size) of a report payload, little-endian bit order. Caller has checked the bounds.
inline std::uint32_t extractBits(const std::uint8_t* payload, std::uint32_t offset, std::uint32_t size) {
    std::uint32_t v = 0;
    for (std::uint32_t k = 0; k < size; ++k) {
        const std::uint32_t bit = offset + k;
        if ((payload[bit / 8] >> (bit % 8)) & 1u) { v |= (1u << k); }
    }
    return v;
}

// Decodes one Input report (as read from the device: with the leading report-id byte when the descriptor uses ids). Returns false, with `out` empty,
// when the report is too short for its id, has an id the descriptor does not define, or is empty. Padding (constant) fields are skipped.
inline bool decodeInputReport(const Descriptor& d, const std::uint8_t* data, std::size_t len, std::vector<FieldValue>& out) {
    out.clear();
    if (!d.ok || data == nullptr || len == 0) { return false; }
    std::uint8_t id = 0;
    const std::uint8_t* payload = data;
    std::size_t plen = len;
    if (d.usesReportIds) { id = data[0]; payload = data + 1; plen = len - 1; }
    const auto it = d.reportBits.find({static_cast<std::uint8_t>(ReportType::Input), id});
    if (it == d.reportBits.end()) { return false; }
    if (plen * 8 < it->second) { return false; }
    for (std::size_t fi = 0; fi < d.fields.size(); ++fi) {
        const Field& f = d.fields[fi];
        if (f.type != ReportType::Input || f.reportId != id) { continue; }
        for (std::uint32_t e = 0; e < f.count; ++e) {
            std::uint32_t raw = extractBits(payload, f.bitOffset + e * f.size, f.size);
            FieldValue v;
            v.field = fi;
            v.element = e;
            v.relative = f.isRelative();
            if (f.isSigned() && f.size < 32 && f.size > 0 && (raw & (1u << (f.size - 1)))) { raw |= ~((1u << f.size) - 1u); }
            v.value = static_cast<std::int32_t>(raw);
            if (f.isVariable()) {
                v.usage = e < f.usages.size() ? f.usages[e] : 0;
            } else {
                v.isArrayIndex = true;
                const std::int64_t idx = static_cast<std::int64_t>(v.value) - f.logMin;
                const bool inRange = v.value >= f.logMin && v.value <= f.logMax;   // outside = the null state: no control is closed
                if (inRange && f.hasUsageRange && static_cast<std::int64_t>(f.usageMin) + idx <= f.usageMax) {
                    v.usage = static_cast<std::uint32_t>(f.usageMin + idx);
                } else if (inRange && !f.hasUsageRange && static_cast<std::size_t>(idx) < f.usages.size()) {
                    v.usage = f.usages[static_cast<std::size_t>(idx)];
                }
                if ((v.usage & 0xFFFFu) == 0) { v.usage = 0; }   // usage id 0 is "no event"
            }
            out.push_back(v);
        }
    }
    return true;
}

}  // namespace cascade::input_hid
