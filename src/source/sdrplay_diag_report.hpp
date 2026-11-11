// sdrplay_diag_report.hpp - the model side of the SDRplay diagnostic dialog (survey row W4a):
// plain data for what an SDRplay device report shows, and a formatter. No window, no API
// call: the caller fills the structure (from the API, or from sdrplay_probe's result) and the
// window prints format(). Serial numbers are never printed whole: only the last four
// characters are shown (the probe report prints a hash instead, sdrPlayProbeSerialHash).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

namespace cascade::source {

struct SdrPlayDiagDevice {
    std::string serial;          // as the API gave it; masked by the formatter
    unsigned char hwVer = 0;     // the API's hardware version byte (sdrplay_abi::kRsp1A ...)
    std::string tuner;           // "Tuner A", "Tuner B", "A and B", "neither" or ""
    std::string mode;            // RSPduo mode wording, "" for other models
    bool selected = false;       // the device this FoxSDR session has open
};

struct SdrPlayDiagReport {
    bool apiLoaded = false;
    float apiVersion = 0.0f;           // sdrplay_api_ApiVersion's answer, 0 when not asked
    std::string apiPath;               // where the API library was loaded from, "" if not
    std::string serviceSummary;        // sdrPlayServiceSummary() wording, "" if not asked
    std::vector<SdrPlayDiagDevice> devices;
    std::string errorText;             // the API's own error string for the last failure, "" if none
    std::string lastStep;              // what was being attempted when it failed
};

// The model name for a hardware-version byte ("RSP1A", "RSPduo" ...), "unknown model (N)" else.
std::string sdrPlayDiagModelName(unsigned char hwVer);

// "...3456" for a serial of four or more characters, "..." for a shorter one, "(none)" for empty.
std::string sdrPlayDiagMaskSerial(const std::string& serial);

// The text the dialog shows. Stable line order; ASCII only.
//   SDRplay diagnostic report
//   API: loaded, version 3.15 (path)      | API: not loaded
//   Service: <summary>                    | (line absent when not asked)
//   Devices: N
//     1. RSPduo  serial ...1234  [Tuner A] [Single tuner mode] (in use)
//   Last error: <text> (while: <step>)    | Last error: none
std::string formatSdrPlayDiagReport(const SdrPlayDiagReport& r);

}  // namespace cascade::source
