// s5_logic.hpp - SCREEN LANE S5 (wave 8; docs/multi-radio-records/screen-s5-sources.md): the pure rules behind the radio and source controls lane S5 adds to the Slint window - no Slint, no Win32, no engine call.
//
//   raw I/Q stream      the address text of "open-rawiq:" built from the fields (G1-05)
//   automatic ppm       the measuring time, the read-out words and the rule that never lets a measurement replace a correction the person set (G1-17)
//   tuner pooling       the frequency / bandwidth / mode of a claim and the sentence its answer becomes (G1-18)
//   direct sampling     the four choices of the RTL-SDR's direct-sampling setting (G1-11)
//
// The typed-field helpers (parseNumber, trim, numberText, Built) are lane L's; the rows themselves are lane K's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

#include "lanel_logic.hpp"

namespace foxsdr::slints5 {

using foxsdr::slintlanel::Built;

// ============================================================ raw I/Q over TCP or UDP (G1-05) ============================================================
// The sample formats of the stream, in the order the choice lists them: f32 (the default of FoxSDR's own sender), s16, u8 (rtl_sdr's).
std::vector<std::string> rawIqFormatWords();      // "32-bit float", "16-bit integer", "8-bit unsigned"
struct RawIqFields {
    int udp = 0;                    // 0 = FoxSDR connects (TCP), 1 = FoxSDR listens (UDP)
    std::string host = "127.0.0.1";
    std::string port;
    int format = 0;                 // index into rawIqFormatWords()
    std::string rateHz;
    std::string centreHz = "0";
    bool header = false;            // the stream carries FoxSDR's own 28-byte header: the format, rate and centre then come from it
    std::string timeoutMs = "3000";
};
// A host: a name or an address, no scheme, no path, no space, no '?' or '&' (the engine's grammar would read them as something else), at most 253 characters.
bool validHost(const std::string& host, std::string& err);
// The text after "open-rawiq:", which the engine's own grammar (source/raw_iq_net_source.hpp) judges again. `text` is empty when `ok` is false.
Built rawIqOpenText(const RawIqFields& f);
// The words under the address: what Open will do in this protocol, and who can reach the port.
std::string rawIqProtocolNote(const RawIqFields& f);

// ============================================================ automatic ppm (G1-17) ============================================================
// Seconds to measure: 10 to 300; empty or 0 = 30 (the engine's default).
bool parsePpmSeconds(const std::string& text, double& out, std::string& err);
// The engine's state word (idle / measuring / done / failed), translated.
std::string ppmStateWords(const std::string& state);
// "+3.20 ppm, plus or minus 0.12 (48 readings)"; "" when there are no readings.
std::string ppmReading(double ppm, double uncertainty, int readings);
// The correction already in force ("on" fact and the stored value): -1 none, 0 a correction of 0, otherwise the number of ppm. What "use this" would replace.
struct PpmInForce { bool on = false; double ppm = 0.0; };
// True when using `measured` would replace a different correction the person has switched on (so the key asks twice).
bool replacesTypedPpm(const PpmInForce& now, double measured);
// The reference the measurement used, in words.
std::string ppmReference();

// ============================================================ tuner pooling (G1-18) ============================================================
// The mode choice of a claim: "Leave as it is" then the modes in the engine's order; index 0 is 0 ("leave it"), index k is FOXAPI_DEMOD_* k.
std::vector<std::string> claimModeWords();
// A frequency typed in MHz: above 0, below 1 THz.
bool parseClaimMHz(const std::string& text, double& hz, std::string& err);
// A bandwidth typed in kHz: 0 (leave it) up to 1 GHz.
bool parseClaimKHz(const std::string& text, double& hz, std::string& err);
// What the engine's answer means: applied[0] the radio's number, applied[1] 1 when the radio's VFO did it alone, 2 when its centre moved.
std::string claimAnswerText(double radio, double how);

// ============================================================ direct sampling (G1-11) ============================================================
// The four choices, the engine's order: automatic, never, the I branch, the Q branch.
std::vector<std::string> directSamplingWords();
// The engine's choice fact ("0".."3") as an index; -1 for anything else.
int directSamplingIndex(const std::string& fact);

}  // namespace foxsdr::slints5
