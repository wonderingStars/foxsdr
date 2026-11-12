// icr8600_protocol.cpp - the reader of the firmware file the user supplies for the IC-R8600 [I/Q OUT] port.
// See icr8600_protocol.hpp for the sources (ibelinp/ic-r8600-usb-iq, MIT: tools/spt_to_json.py's description of
// the Cypress CSPT layout) and for what is never shipped or fetched.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/icr8600_protocol.hpp"

#include <nlohmann/json.hpp>

namespace cascade::source::icr8600 {

namespace {

int hexVal(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

bool finish(std::vector<FwRecord>& recs, std::string& error) {
    if (recs.empty()) { error = "the firmware file holds no records"; return false; }
    if (recs.size() > kMaxFirmwareRecords) { error = "the firmware file holds more than 256 records"; return false; }
    std::size_t total = 0;
    for (std::size_t i = 0; i < recs.size(); ++i) {
        total += recs[i].data.size();
        if (!firmwareAddressOk(recs[i].wValue, recs[i].data.size())) {
            error = "firmware record " + std::to_string(i + 1) + " writes outside the FX2's loadable memory";
            return false;
        }
    }
    if (total > kMaxFirmwareBytes) { error = "the firmware file is larger than 64 KiB"; return false; }
    const FwRecord& a = recs.front();
    const FwRecord& z = recs.back();
    if (!(a.wValue == 0xE600 && a.data.size() == 1 && a.data[0] == 1)) {
        error = "the firmware sequence does not begin by holding the FX2 in reset";
        return false;
    }
    if (!(z.wValue == 0xE600 && z.data.size() == 1 && z.data[0] == 0)) {
        error = "the firmware sequence does not end by releasing the FX2 from reset";
        return false;
    }
    return true;
}

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

bool parseFirmware(const std::vector<std::uint8_t>& file, std::vector<FwRecord>& out, std::string& error) {
    out.clear();
    error.clear();
    if (file.size() < 4) { error = "the firmware file is empty or too short"; return false; }
    if (file.size() > 4 * 1024 * 1024) { error = "the firmware file is far too large to be the IC-R8600's"; return false; }
    if (file[0] == 'C' && file[1] == 'S' && file[2] == 'P' && file[3] == 'T') {
        std::size_t off = 0;
        while (off < file.size()) {
            if (file.size() - off < 32 || file[off] != 'C' || file[off + 1] != 'S' || file[off + 2] != 'P' || file[off + 3] != 'T') {
                error = "the firmware script is damaged (no record header at byte " + std::to_string(off) + ")";
                out.clear();
                return false;
            }
            const std::uint8_t* h = file.data() + off;
            const std::uint8_t requestType = static_cast<std::uint8_t>(le32(h + 12) >> 24);
            const std::uint8_t request = h[16];
            const std::uint16_t wValue = static_cast<std::uint16_t>(h[18] | (h[19] << 8));
            const std::uint32_t len = le32(h + 28);
            if (requestType != 0x40 || request != 0xA0) {
                error = "firmware record " + std::to_string(out.size() + 1) + " is not the FX2 RAM write (vendor request 0xA0)";
                out.clear();
                return false;
            }
            if (len > kMaxFirmwareBytes || file.size() - off - 32 < len) {
                error = "firmware record " + std::to_string(out.size() + 1) + " runs past the end of the file";
                out.clear();
                return false;
            }
            if (out.size() >= kMaxFirmwareRecords) {
                error = "the firmware file holds more than 256 records";
                out.clear();
                return false;
            }
            FwRecord r;
            r.wValue = wValue;
            r.data.assign(file.begin() + static_cast<std::ptrdiff_t>(off + 32), file.begin() + static_cast<std::ptrdiff_t>(off + 32 + len));
            out.push_back(std::move(r));
            off += 32 + len;
        }
        if (!finish(out, error)) { out.clear(); return false; }
        return true;
    }
    // JSON: [{"wValue": N, "data": "hex"}, ...]
    std::size_t s = 0;
    while (s < file.size() && (file[s] == ' ' || file[s] == '\t' || file[s] == '\r' || file[s] == '\n')) { ++s; }
    if (s >= file.size() || file[s] != '[') {
        error = "the firmware file is neither a Cypress CSPT script (IC-R8600_usb_iq.spt) nor the JSON sequence made from it";
        return false;
    }
    const std::string text(file.begin(), file.end());
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_array()) { error = "the firmware JSON could not be read"; return false; }
    if (j.size() > kMaxFirmwareRecords) { error = "the firmware file holds more than 256 records"; return false; }
    for (const auto& e : j) {
        if (!e.is_object() || !e.contains("wValue") || !e.contains("data") || !e["wValue"].is_number_integer() || !e["data"].is_string()) {
            error = "firmware record " + std::to_string(out.size() + 1) + " is not {\"wValue\": number, \"data\": \"hex\"}";
            out.clear();
            return false;
        }
        const long long wv = e["wValue"].get<long long>();
        const std::string hex = e["data"].get<std::string>();
        if (wv < 0 || wv > 0xFFFF || hex.size() % 2 != 0 || hex.size() > 2 * kMaxFirmwareBytes) {
            error = "firmware record " + std::to_string(out.size() + 1) + " has an impossible address or data length";
            out.clear();
            return false;
        }
        FwRecord r;
        r.wValue = static_cast<std::uint16_t>(wv);
        for (std::size_t i = 0; i < hex.size(); i += 2) {
            const int a = hexVal(hex[i]), b = hexVal(hex[i + 1]);
            if (a < 0 || b < 0) {
                error = "firmware record " + std::to_string(out.size() + 1) + " has data that is not hexadecimal";
                out.clear();
                return false;
            }
            r.data.push_back(static_cast<std::uint8_t>((a << 4) | b));
        }
        out.push_back(std::move(r));
    }
    if (!finish(out, error)) { out.clear(); return false; }
    return true;
}

}  // namespace cascade::source::icr8600
