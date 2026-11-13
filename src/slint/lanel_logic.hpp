// lanel_logic.hpp - SCREEN LANE L (docs/multi-radio-records/screen-l.md): the pure rules behind the rows lane L adds to the Slint window - no Slint, no Win32, no engine call.
//
//   number and text parsing   what a typed field means (a number in a stated unit and range; a list)
//   JSON builders             the text of each engine operation that takes a JSON object, built from what the person typed (nlohmann, strict; the ENGINE is still the judge of every value)
//   readers                   a result JSON flattened into "name: value" lines for a readout; the needed-libraries table (nine rows, seven states); the words of a refused answer
//
// The rows themselves are lane K's (lanek_logic.hpp: Row, rKey, rTick, rChoice, rField, rReadout, rNote ...).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lanek_logic.hpp"

namespace foxsdr::slintlanel {

using foxsdr::slintlanek::Row;

// ============================================================ typed text ============================================================
// A number typed into a field: dot decimals only, surrounding spaces ignored, a finite value inside [lo, hi]. `err` (English, not translated: the caller translates) is set when it is not.
bool parseNumber(const std::string& text, double lo, double hi, double& out, std::string& err);
bool parseInteger(const std::string& text, long long lo, long long hi, long long& out, std::string& err);
// A list typed as "a, b; c" -> trimmed non-empty items (at most `maxItems`, each at most `maxLen` bytes; false when one is too long).
bool splitList(const std::string& text, std::size_t maxItems, std::size_t maxLen, std::vector<std::string>& out);
std::string trim(const std::string& s);
// "1.5" for 1.5, "7" for 7: a number as the field shows it (never an exponent for the sizes used here).
std::string numberText(double v);
// The Windows serial-port name the sweep and the RF Explorer accept: COM1 to COM256, upper case.
bool validComPort(const std::string& text, std::string& normalised);

// ============================================================ operation texts ============================================================
// Each builder returns the compact JSON text of the operation (UTF-8 made valid; keys the engine documents) and leaves `err` set (English) when what was typed cannot be a request.
struct Built { bool ok = false; std::string text, err; };

Built scanTriggerJson(int mode /*0 fixed, 1 above the noise floor*/, const std::string& fixedDb, const std::string& marginDb, const std::string& hysteresisDb, const std::string& riseDb);
// ranges: one per line "<start MHz> <stop MHz> <step kHz> <mode> <bandwidth kHz> [label]"; direction 0 forward / 1 backward; the trigger part is the trigger JSON built above
Built scanPlanJson(const std::string& rangesText, int direction, const std::string& triggerJson);
Built fftScanJson(bool enabled, const std::string& startMHz, const std::string& stopMHz, const std::string& stepKHz, int mode /*0 relative 1 fixed*/, const std::string& marginDb, const std::string& fixedDb, bool merge);
Built bandMemoryJson(bool enabled, bool volume, bool agc);
Built recordDailyJson(int style, bool utc);
Built overlayLoadJson(const std::string& path, int unit /*0 Hz 1 kHz 2 MHz*/, const std::string& freqColumn, const std::string& columnsText, int delimiter /*0 comma 1 semicolon 2 tab*/);
Built packExportJson(const std::string& path, const std::string& name, const std::string& description);
Built favouriteSaveJson(const std::string& name);
Built bookmarkNoteJson(const std::string& note, const std::string& tagsText);
Built bookmarkFieldsJson(const std::string& squelchDb, const std::string& thresholdDb, const std::string& offsetKhz, const std::string& toneHz);
Built recordingsListJson(const std::string& textFilter, int sort);
Built sigmeasJson(const std::string& durationS, const std::string& endAgoS);
Built pseudoDopplerJson(const std::string& samplesPerDwell, const std::string& durationS);
Built maskConfigJson(bool enabled, const std::string& raiseFrames, const std::string& clearFrames, bool capture, const std::string& postRollS, const std::string& minIntervalS);
Built sweepOpenJson(int kind /*0 tinySA 1 tinySA Ultra 2 HackRF 3 RF Explorer*/, const std::string& port, const std::string& baud, const std::string& startMHz, const std::string& stopMHz, const std::string& points,
                    const std::string& intervalMs, const std::string& stepMHz, const std::string& lnaDb, const std::string& vgaDb, bool amp, const std::string& topDbm, const std::string& bottomDbm);
Built sweepHeatmapJson(const std::string& cols, const std::string& rows, int freqMode /*0 max 1 mean*/, bool occupancyOn, const std::string& startMHz, const std::string& stopMHz, const std::string& stepKHz,
                       const std::string& marginDb, const std::string& floorPercentile, const std::string& fixedFloorDb);
Built iqOutJson(int format /*0 u8 1 s16 2 f32*/, bool header, bool udp, const std::string& port);
Built sbsOutJson(const std::string& port);
Built tracksConfigJson(bool enabled, const std::string& retentionS);
Built coverageConfigJson(bool enabled, const std::string& rxLat, const std::string& rxLon, const std::string& cellDeg, const std::string& halfLifeS);
Built qsoJson(const std::string& unixTime, const std::string& kHz, const std::string& mode, const std::string& call, const std::string& myCall, const std::string& sent, const std::string& received);
Built cabrilloHeaderJson(const std::string& contest, const std::string& callsign, const std::string& location, const std::string& name, const std::string& addressLines, const std::string& operators, const std::string& soapbox);
Built neededCheckJson(const std::string& id, const std::string& path);
Built exportBoxJson(const std::string& input, const std::string& output, const std::string& t0, const std::string& t1, const std::string& loHz, const std::string& hiHz, int mode, int format);
Built openRadioId(int kindIndex, const std::string& arguments);   // "open-<key>:<arguments>" for the network and vendor sources

// The labels and keys of the choices above, in one place (the drive scripts and the tests read them).
const std::vector<std::string>& channelPlanIds();
const std::vector<std::string>& sourceKinds();     // the keys of "open-<key>:" in the order the list shows them
const std::vector<std::string>& sourceKindNames(); // their English names

// ============================================================ reading results ============================================================
// A JSON result as "name: value" lines (objects dotted, an array as its length and its first items), at most `maxLines`. Anything that is not JSON comes back as one line of the text itself.
std::vector<std::pair<std::string, std::string>> flattenJson(const std::string& text, std::size_t maxLines);
// Whether `text` holds a JSON object whose member `key` is the string / number / boolean; "" / 0 / false otherwise.
std::string jsonString(const std::string& text, const char* key);
double jsonNumber(const std::string& text, const char* key, double def);
bool jsonBool(const std::string& text, const char* key, bool def);
// The word of an engine status for a refusal line: "Refused", "Nothing changed", "Not found", ... plus the engine's own sentence when there is one.
std::string refusalText(int status, const std::string& engineMessage);
bool statusIsRefusal(int status);   // not OK and not NO_CHANGE

// ============================================================ needed libraries (section C) ============================================================
struct NeededRow {
    std::string id, sourceKind, name, kind, why, publisher, officialPage, pageStatus, installKind, folderNote, licence, notSupplied, fetchAllowed, minVersionNote;
    std::vector<std::string> fileNames, steps;
    bool bits64 = true;
    bool checked = false;
    std::string state, sentence, depth, version, foundIn, file;
    std::vector<std::string> missing;
};
std::vector<NeededRow> parseNeeded(const std::string& neededLibsJson);
// What the state is called on the screen. A header-only "working" is "Found (not test-loaded)", NEVER "Working"; only depth "loaded" is "Found and working".
std::string neededStateWords(const NeededRow& r);
// The tone of that line (lanek RowTone): good only for a loaded, working library.
int neededStateTone(const NeededRow& r);
// The rows of the panel for one library (name, what needs it, state, where to get it, the steps, the licence line). `draftPath` is what the person typed for a copy-in library.
void neededRows(const NeededRow& r, const std::string& draftPath, std::vector<Row>& out);
// Whether the row takes a file the person picked (the "copy-in" library files): the two WiNRADiO rows, never the FTDI installer.
bool neededTakesFile(const NeededRow& r);

}  // namespace foxsdr::slintlanel
