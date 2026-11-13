// lanem_logic.hpp - SCREEN LANE M (docs/multi-radio-records/screen-m.md): the pure rules behind the controls lane M adds to the Slint window - no Slint, no Win32, no engine call.
//
//   JSON / text builders   the text of each engine operation that takes one (CW surround, the stitched sweep, the sweep history, the airband channel's FOXAIRCH1 line, the patch edit ids)
//   readers                the facts the rows show (a plugin's open request, the sweep history read-out, the spectrum's GPU counters, the level store's statistics)
//
// The typed-field helpers (parseNumber, parseInteger, trim, numberText, Built) and the rows themselves are lane L's and lane K's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lanel_logic.hpp"

namespace foxsdr::slintlanem {

using foxsdr::slintlanel::Built;

// ============================================================ I/Q file transport (row 1) ============================================================
// A speed typed in the field: 0.25 to 8.
bool parseSpeed(const std::string& text, double& out, std::string& err);
// "1.5x" for 1.5, "1x" for 1; and the words of the policy fact ("varispeed" / "blockskip"), translated; "" for an unknown fact.
std::string speedText(double speed);
std::string policyWords(const std::string& fact);
// A number of blocks to step: a whole number, never 0, -4096 to 4096.
bool parseStepBlocks(const std::string& text, long long& out, std::string& err);

// ============================================================ RIT (row 2) ============================================================
// An offset in Hz inside +-`limit` (the engine's own limit, 1..20000, default 9999). The engine is the judge; this keeps a typo from being sent.
bool parseRitHz(const std::string& text, double limit, double& out, std::string& err);
// The two numbers of the limits operation: the largest offset (1..20000) and the retune that clears it (0 = never, up to 1e9 Hz).
bool parseRitLimits(const std::string& maxText, const std::string& clearText, double& maxHz, double& clearHz, std::string& err);
// "+300 Hz" / "-1500 Hz" / "0 Hz".
std::string signedHzText(double hz);

// ============================================================ CW surround (row 3) ============================================================
Built cwSurroundJson(bool enabled, const std::string& centreHz, const std::string& spreadHz, bool invert, const std::string& toneRatio, const std::string& holdMs);

// ============================================================ the stitched sweep and the sweep history (rows 11 to 14) ============================================================
Built stitchedSweepJson(const std::string& radio, const std::string& startMHz, const std::string& stopMHz, const std::string& stepFraction, const std::string& trim, const std::string& settleMs,
                        const std::string& frames, const std::string& timeoutMs, const std::string& passes);
// action: "save", "load" or "clear"; format 0 csv / 1 json / 2 xml (save only). `save` and `load` need a full path.
Built sweepHistoryJson(const std::string& action, const std::string& path, int format);
// "12 sweeps, 3600 points of 300000" from the fact sweep.history; "" when the fact is absent.
std::string historyReadout(const std::string& historyFact);
// "step 3 of 12" from the fact sweep.stitch; "" when it is not active.
std::string stitchProgress(const std::string& stitchFact);

// ============================================================ airband channel (row 7) ============================================================
// The fields as the person sees them (kHz, MHz, dB; text), turned into the engine's FOXAIRCH1 line. The model (core/airband_channel.hpp) is the judge of every range: its sentence refuses a bad value.
struct AirbandFields {
    std::string freqMHz, bandwidthKHz = "10";
    int mode = 0;                           // 0 AM, 1 NFM
    int squelchKind = 0;                    // 0 level, 1 SNR
    std::string levelDb = "-50", snrOpenDb = "10", snrHystDb = "3", holdMs = "400";
    std::string highPassHz = "0", lowPassHz = "0";
    int filterOrder = 4;                    // 2 or 4
    std::string notches;                    // "hz/Q; hz/Q" (Q optional, default 30; up to three)
    int output = 0;                         // 0 none, 1 WAV file, 2 UDP
    bool dailyFolder = true;
    std::string maxFileMb = "100", udpPort = "0";
};
struct AirbandBuilt { bool ok = false; std::string text, err; long long freqHz = 0; };
AirbandBuilt airbandChannelText(const AirbandFields& f);
// The fields filled from a stored line (the fact airband.channel.<Hz>); false when the line is not one.
bool airbandFieldsFrom(const std::string& line, AirbandFields& out);
// The frequency typed in MHz as whole hertz (100 kHz to 6 GHz, the model's range).
bool airbandHz(const std::string& mhzText, long long& hz, std::string& err);

// ============================================================ the level store (row 8) ============================================================
// MiB per radio: 1 to 64.
bool parseCapMiB(const std::string& text, double& out, std::string& err);
// "15.1 MB" for a byte count.
std::string bytesText(double bytes);

// ============================================================ the spectrum's GPU counters (row 15) ============================================================
struct GpuCounters { bool have = false; std::string backend; bool engaged = false; double blockGpu = 0, blockCpu = 0; bool blockGpuUp = false; bool hasBlock = false; std::string error; };
GpuCounters parseGpuCounters(const std::string& gpufftFact);

// ============================================================ a plugin's request to open a page or a folder (row 19) ============================================================
struct OpenRequest { std::string moduleFile, pluginName, kind /* "url" or "folder" */, target; };
// The fact plugin.open.<n>: "<module file> US <plugin name> US url|folder US <address or folder path>". False when it is not one.
bool parseOpenRequest(const std::string& fact, OpenRequest& out);
// The text of the confirm operation: "<module file> US url|folder".
std::string openConfirmText(const OpenRequest& r);

// ============================================================ the patch edit (rows 5 and 6) ============================================================
// {"ids":[1,2]} from "1, 2 3" (whole numbers from 1; at most 64); not ok when something is not a number or the list is empty.
Built patchIdsJson(const std::string& idsText);
// Where a paste puts the set's top-left: two numbers, world units, -100000 to 100000.
bool parsePastePoint(const std::string& xText, const std::string& yText, double& x, double& y, std::string& err);

// ============================================================ the symbol rate (row 20) ============================================================
// Baud: 0 (none set) or up to 1e8.
bool parseSymbolRate(const std::string& text, double& out, std::string& err);
// The text sent for it: a whole number as digits, anything else with up to six decimals and no trailing zeros.
std::string symbolRateText(double baud);

// ============================================================ a mark's credit (row 21) ============================================================
// The mark's tooltip with the credit under it. An empty credit leaves the tooltip as it is.
std::string withCredit(const std::string& tooltip, const std::string& credit);

}  // namespace foxsdr::slintlanem
