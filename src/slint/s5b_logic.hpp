// s5b_logic.hpp - SCREEN LANE S5b (wave 8; docs/multi-radio-records/screen-s5b.md): the pure rules behind the radio and source controls lane S5b adds on top of lane S5's panel - no Slint, no Win32, no engine call.
//
//   analogue IF bandwidth   the RTL-SDR tuner's own filter, in kHz, 0 = automatic (G1-14)
//
// The typed-field helpers (parseNumber, trim, numberText, Built) are lane L's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "lanel_logic.hpp"

namespace foxsdr::slints5b {

// ============================================================ analogue IF bandwidth (G1-14) ============================================================
// A bandwidth typed in kHz: empty or 0 = automatic (the filter follows the sample rate); otherwise 300 to 8000 (the driver's own range, which judges again). `hz` is 0 for automatic.
bool parseIfBandwidthKHz(const std::string& text, long long& hz, std::string& err);
// The engine's fact ("radio.N.ifBandwidth", Hz) in words: "Automatic (from the sample rate)" or "400 kHz"; "" for anything that is not a number.
std::string ifBandwidthWords(const std::string& fact);

}  // namespace foxsdr::slints5b


namespace foxsdr::slints5b {

// ============================================================ CAT follow, OmniRig, virtual COM (G1-16, G3-18a, G3-18b) ============================================================
// The four ways, the engine's cat.link.mode keys in the same order: off, follow (a serial port to a rig), emulate (a virtual COM port to a logging program), omnirig.
std::vector<std::string> catModeWords();
const char* catModeKey(int index);       // "off" for an index outside 0..3
int catModeIndex(const std::string& key);  // 0 for a key that is not one of the four
// The engine's link state (off opening following emulating error) in words.
std::string catStateWords(const std::string& state);
// "101.1 MHz, USB": the rig's frequency fact (Hz, digits) and mode; "" for anything that is not a number.
std::string catRigWords(const std::string& hzText, const std::string& mode);

}  // namespace foxsdr::slints5b

// ============================================================ SoapySDR driver bandwidth and settings (G1-15a, G1-15b) ============================================================
// What the connected driver reported, read back from the engine's facts ("radio.N.soapy.*"); nothing here is a fixed list.
namespace foxsdr::slints5b {

struct SoapySettingView {
    std::string key, name, description, units, type /*bool int float string*/, value, def;
    bool hasRange = false;
    double lo = 0.0, hi = 0.0;
    std::vector<std::string> options, optionNames;
};
// The settings the driver listed for radio `radio` (`fact` answers "radio.N.soapy.<what>" without the "fact." prefix, empty when the engine has none). At most 64, in the driver's order; a row with no key is skipped.
std::vector<SoapySettingView> readSoapySettings(const std::function<std::string(const std::string&)>& fact, int radio);
// The driver's bandwidth in kHz as words, "" when the driver has none; `lo`/`hi` are the driver's own range in Hz (0 when absent).
bool readSoapyBandwidth(const std::function<std::string(const std::string&)>& fact, int radio, double& currentHz, double& loHz, double& hiHz);
std::string soapyBandwidthWords(double hz);
// A bandwidth typed in kHz within the driver's range (the driver judges again).
bool parseSoapyBandwidthKHz(const std::string& text, double loHz, double hiHz, double& hz, std::string& err);
// The value the person typed or picked, checked against the type, range and options the driver reported, as the text to write ("true"/"false" for a tick); the driver judges again.
bool soapyValueText(const SoapySettingView& s, const std::string& typed, std::string& out, std::string& err);
// The command text: "soapy:<key>=<value>" (at most 255 bytes: a longer one is refused here with a sentence).
bool soapyWriteCommand(const std::string& key, const std::string& value, std::string& out, std::string& err);
// "Crystal trim (ppm)": the driver's own name with its units.
std::string soapySettingLabel(const SoapySettingView& s);
// "-50 to 50" or "" when the driver gave no range.
std::string soapyRangeWords(const SoapySettingView& s);

}  // namespace foxsdr::slints5b
