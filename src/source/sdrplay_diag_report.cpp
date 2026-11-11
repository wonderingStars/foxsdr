// sdrplay_diag_report.cpp - see sdrplay_diag_report.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrplay_diag_report.hpp"

#include <cstdio>

#include "source/sdrplay_api_decl.hpp"

namespace cascade::source {

namespace abi = sdrplay_abi;

std::string sdrPlayDiagModelName(unsigned char hwVer) {
    switch (hwVer) {
        case abi::kRsp1: return "RSP1";
        case abi::kRsp2: return "RSP2";
        case abi::kRspDuo: return "RSPduo";
        case abi::kRspDx: return "RSPdx";
        case abi::kRsp1B: return "RSP1B";
        case abi::kRspDxR2: return "RSPdx-R2";
        case abi::kRsp1A: return "RSP1A";
        default: break;
    }
    return "unknown model (" + std::to_string(static_cast<int>(hwVer)) + ")";
}

std::string sdrPlayDiagMaskSerial(const std::string& serial) {
    if (serial.empty()) { return "(none)"; }
    if (serial.size() < 4) { return "..."; }
    return "..." + serial.substr(serial.size() - 4);
}

std::string formatSdrPlayDiagReport(const SdrPlayDiagReport& r) {
    std::string out = "SDRplay diagnostic report\n";
    if (r.apiLoaded) {
        char v[32];
        std::snprintf(v, sizeof(v), "%.2f", static_cast<double>(r.apiVersion));
        out += std::string("API: loaded, version ") + v;
        if (!r.apiPath.empty()) { out += " (" + r.apiPath + ")"; }
        out += "\n";
    } else {
        out += "API: not loaded\n";
    }
    if (!r.serviceSummary.empty()) { out += "Service: " + r.serviceSummary + "\n"; }
    out += "Devices: " + std::to_string(r.devices.size()) + "\n";
    int n = 0;
    for (const SdrPlayDiagDevice& d : r.devices) {
        out += "  " + std::to_string(++n) + ". " + sdrPlayDiagModelName(d.hwVer) + "  serial " +
               sdrPlayDiagMaskSerial(d.serial);
        if (!d.tuner.empty()) { out += "  [" + d.tuner + "]"; }
        if (!d.mode.empty()) { out += " [" + d.mode + "]"; }
        if (d.selected) { out += " (in use)"; }
        out += "\n";
    }
    if (r.errorText.empty()) {
        out += "Last error: none";
    } else {
        out += "Last error: " + r.errorText;
        if (!r.lastStep.empty()) { out += " (while: " + r.lastStep + ")"; }
    }
    out += "\n";
    return out;
}

}  // namespace cascade::source
