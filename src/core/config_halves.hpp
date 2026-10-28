// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// THE TWO HALVES OF THE SETTINGS FILE, AS DATA (shell retirement R5; docs/engine-stage4.md 6.25, docs/api-0.3-additions.md 9.16).
//
// The settings file is ONE JSON document (core/config.hpp: AppConfig), and two writers fill it. The ENGINE fills the receiver's half
// (the source and every radio, the DSP, the plugins, the transmitter, the two servers, the usage journal: Engine::fillConfig,
// Engine::telemetryJournal, ServerHost::store). The INTERFACE fills the other (the view state, the windows' rectangles, the key
// bindings, the patch text, the language and theme, the update and diagnostics switches, the beta tester's record:
// AppWindow::currentConfig, diagnosticsJournal, testerUsageJournal). Until this header the split existed only as two lists of
// assignments in two source files, and a field in NEITHER list was kept from the file as loaded (Engine::publishConfig started from
// the loaded copy): safe, silent, and exactly the failure configsEqual's own header warns of - a setting that "reaches the file only
// when something else happens to change". This table makes every field belong to one half, by name, in one place; the ownership test
// (tests/test_config_halves.cpp) fails when AppConfig grows a field the table does not name, and holds each row against the writer it
// names.
//
//   Engine     the engine writes it; an interface never sends it.
//   Interface  an interface writes it; the engine STORES it (as a document, FOXAPP_OP_INTERFACE_CONFIG) and puts it back into every
//              snapshot, so the snapshot is the whole truth and whoever saves it saves the interface's half too.
//   Shared     written by BOTH today, and kept honest as such: dbMin and dbMax. The engine holds the display window IN FORCE (which
//              Automatic makes the standard one) and the interface holds the user's own floor and ceiling, which the file must keep
//              whichever is in force. The engine's value stands until an interface has sent its half; from then on the interface's
//              does. A headless engine (no interface) therefore saves what it always saved.
//
// `moves` marks the fields that change BY THEMSELVES - the usage report that grows every second, the exit marker, the retry queues,
// the crash-upload limiter's memory. They ride along on whatever save a real change triggers, and they must not be what makes a
// half "differ" (a half sent every look would fill the session's queue) or what makes a keeper see a change (it would never settle,
// or write every look). This is configsEqual's exclusion list, written once.
//
// WHY THE HALF CROSSES AS A DOCUMENT: the file format does not change, and a second interface (the Slint GUI) can send the same
// document or a namespaced one later. The document is the text ConfigStore::serialize makes, cut to the keys of the half, and it is
// read back by ConfigStore::parse - the same rules the file is read by - so a half arrives sanitised exactly as it would have after
// a restart.
//
// Engine-clean: core/ only (no gui/, imgui or GLFW).
#pragma once

#include <cstddef>
#include <string>

#include "core/config.hpp"

namespace cascade::core {

enum class ConfigOwner { Engine, Interface, Shared };

struct ConfigFieldInfo {
    const char* name;   // the AppConfig member, which is also the field's key in the JSON
    ConfigOwner owner;
    bool moves;         // changes by itself: excluded from "did the half change" and from the keepers' compare
};

// THE TABLE: one row per AppConfig member, in declaration order. X(member, owner, moves).
#define FOXSDR_CONFIG_FIELDS(X) \
    X(schemaVersion,           Engine,    false) \
    X(sourceKind,              Engine,    false) \
    X(soapyAntenna,            Engine,    false) \
    X(soapyArgs,               Engine,    false) \
    X(lookForNetworkUsrps,     Engine,    false) \
    X(radioSetupDontShow,      Interface, false) \
    X(nativeArgs,              Engine,    false) \
    X(biasTee,                 Engine,    false) \
    X(converters,              Engine,    false) \
    X(ppmCorrection,           Engine,    false) \
    X(ppm,                     Engine,    false) \
    X(airspy,                  Engine,    false) \
    X(autoGainByRadio,         Engine,    false) \
    X(manualGainByRadio,       Engine,    false) \
    X(plutoUri,                Engine,    false) \
    X(rtlTcpAddr,              Engine,    false) \
    X(iqFilePath,              Engine,    false) \
    X(soundCard,               Engine,    false) \
    X(centerHz,                Engine,    false) \
    X(mode,                    Engine,    false) \
    X(bandwidthHz,             Engine,    false) \
    X(squelchDb,               Engine,    false) \
    X(squelchAuto,             Engine,    false) \
    X(volume,                  Engine,    false) \
    X(audioBufferMs,           Engine,    false) \
    X(audioBufferAutoMs,       Engine,    false) \
    X(dbMin,                   Shared,    false) \
    X(dbMax,                   Shared,    false) \
    X(waterfallRangeMode,      Interface, false) \
    X(splitRatio,              Interface, false) \
    X(vfoOffsetHz,             Engine,    false) \
    X(sampleRateHz,            Engine,    false) \
    X(stereoEnabled,           Engine,    false) \
    X(deemphasisIndex,         Engine,    false) \
    X(nrEnabled,               Engine,    false) \
    X(nrStrength,              Engine,    false) \
    X(notchEnabled,            Engine,    false) \
    X(notchFreqHz,             Engine,    false) \
    X(notchQ,                  Engine,    false) \
    X(autoNotch,               Engine,    false) \
    X(bandPlanOverlay,         Interface, false) \
    X(patch,                   Interface, false) \
    X(patchPresets,            Interface, false) \
    X(patchPresetPrevious,     Interface, false) \
    X(mainView,                Interface, false) \
    X(railCollapsed,           Interface, false) \
    X(statusCollapsed,         Interface, false) \
    X(displayOpen,             Interface, false) \
    X(bandPlanSelection,       Engine,    false) \
    X(bandPlanSize,            Interface, false) \
    X(bandPlanPalette,         Interface, false) \
    X(tunerDisplayStyle,       Interface, false) \
    X(meterStyleVolume,        Interface, false) \
    X(meterStyleRate,          Interface, false) \
    X(uiTheme,                 Interface, false) \
    X(counterScale,            Interface, false) \
    X(counterSwitches,         Interface, false) \
    X(readingsScale,           Interface, false) \
    X(interfaceScale,          Interface, false) \
    X(language,                Interface, false) \
    X(country,                 Interface, false) \
    X(mapTrails,               Interface, false) \
    X(mapTrailAltitudeColours, Interface, false) \
    X(bookmarkMarkers,         Interface, false) \
    X(bookmarkStackNames,      Interface, false) \
    X(spectrumTraceMode,       Interface, false) \
    X(spectrumAverageMs,       Interface, false) \
    X(mapTrailStyle,           Interface, false) \
    X(aircraftIconPx,          Interface, false) \
    X(mapTrailWidthPx,         Interface, false) \
    X(mapListOnScreenOnly,     Interface, false) \
    X(scopeMode,               Interface, false) \
    X(scopeRangeNm,            Interface, false) \
    X(demodScopeOpen,          Interface, false) \
    X(demodScopeSignal,        Interface, false) \
    X(demodScopeTimebase,      Interface, false) \
    X(demodScopeGain,          Interface, false) \
    X(demodScopeAutoGain,      Interface, false) \
    X(demodScopeDisplay,       Interface, false) \
    X(transmitOpen,            Engine,    false) \
    X(transmitMode,            Engine,    false) \
    X(transmitInput,           Engine,    false) \
    X(transmitPowerDb,         Engine,    false) \
    X(transmitSplit,           Engine,    false) \
    X(transmitSplitHz,         Engine,    false) \
    X(transmitToneHz,          Engine,    false) \
    X(transmitMonitor,         Engine,    false) \
    X(transmitArgs,            Engine,    false) \
    X(railBank,                Interface, false) \
    X(keyBindings,             Interface, false) \
    X(mapWindowWidth,          Interface, false) \
    X(mapWindowHeight,         Interface, false) \
    X(mapWindowX,              Interface, false) \
    X(mapWindowY,              Interface, false) \
    X(mapPages,                Interface, false) \
    X(rxPositionSet,           Engine,    false) \
    X(rxLatDeg,                Engine,    false) \
    X(rxLonDeg,                Engine,    false) \
    X(gpsPort,                 Interface, false) \
    X(gpsBaud,                 Interface, false) \
    X(pluginCatalogueUrl,      Engine,    false) \
    X(pluginSites,             Engine,    false) \
    X(pluginBrowserOpen,       Interface, false) \
    X(fittedModulesOpen,       Interface, false) \
    X(fittedModulesX,          Interface, false) \
    X(fittedModulesY,          Interface, false) \
    X(fittedModulesWidth,      Interface, false) \
    X(fittedModulesHeight,     Interface, false) \
    X(pluginStoreX,            Interface, false) \
    X(pluginStoreY,            Interface, false) \
    X(pluginStoreWidth,        Interface, false) \
    X(pluginStoreHeight,       Interface, false) \
    X(pluginLastUpdateCheck,   Engine,    false) \
    X(pluginTuneAllowed,       Engine,    false) \
    X(closedWindows,           Engine,    false) \
    X(pluginsStopped,          Engine,    false) \
    X(pluginRun,               Engine,    false) \
    X(pluginMuteOverride,      Engine,    false) \
    X(pluginSettingsAllowed,   Engine,    false) \
    X(pluginTransmitAllowed,   Engine,    false) \
    X(pluginSettings,          Engine,    false) \
    X(userPresets,             Engine,    false) \
    X(webEnabled,              Engine,    false) \
    X(webBindAddress,          Engine,    false) \
    X(webPort,                 Engine,    false) \
    X(catEnabled,              Engine,    false) \
    X(catBindAll,              Engine,    false) \
    X(catPort,                 Engine,    false) \
    X(webUsername,             Engine,    false) \
    X(webPasswordRecord,       Engine,    false) \
    X(updateCheckEnabled,      Interface, false) \
    X(updateChannel,           Interface, false) \
    X(messagesSeen,            Interface, false) \
    X(telemetryEnabled,        Engine,    false) \
    X(telemetryInstallId,      Engine,    false) \
    X(telemetryLaunches,       Engine,    false) \
    X(telemetryCrashes,        Engine,    false) \
    X(telemetryExitsDied,      Engine,    false) \
    X(telemetryExitsKilled,    Engine,    false) \
    X(telemetryExitsEnded,     Engine,    false) \
    X(telemetryExitsUnknown,   Engine,    false) \
    X(telemetrySessionStarted, Engine,    true) \
    X(telemetryFirstRun,       Engine,    false) \
    X(telemetryFirstVersion,   Engine,    false) \
    X(telemetryCleanExit,      Engine,    true) \
    X(telemetryPending,        Engine,    true) \
    X(testerToken,             Interface, false) \
    X(testerTokenInvalid,      Interface, true) \
    X(testerUsageCurrent,      Interface, true) \
    X(testerUsagePending,      Interface, true) \
    X(testerAppToken,          Interface, false) \
    X(testerAppTokenName,      Interface, false) \
    X(diagnosticsEnabled,      Interface, false) \
    X(diagnosticsMinidump,     Interface, false) \
    X(crashUploadRecent,       Interface, true) \
    X(crashUploadWindowStart,  Interface, true) \
    X(crashUploadWindowCount,  Interface, true) \
    X(crashUploadBlockedUntil, Interface, true)

inline constexpr ConfigFieldInfo kConfigFields[] = {
#define FOXSDR_CONFIG_ROW(name, owner, moves) {#name, ConfigOwner::owner, moves},
    FOXSDR_CONFIG_FIELDS(FOXSDR_CONFIG_ROW)
#undef FOXSDR_CONFIG_ROW
};
inline constexpr std::size_t kConfigFieldCount = sizeof(kConfigFields) / sizeof(kConfigFields[0]);

// Calls f(name, owner, moves, memberPointer) for every field: the pointer lets one generic lambda read or write any of them
// (`cfg.*p`). The only place the member names are spelled besides AppConfig itself.
template <class F>
void forEachConfigField(F&& f) {
#define FOXSDR_CONFIG_VISIT(name, owner, moves) f(#name, ConfigOwner::owner, moves, &AppConfig::name);
    FOXSDR_CONFIG_FIELDS(FOXSDR_CONFIG_VISIT)
#undef FOXSDR_CONFIG_VISIT
}

// The engine's half of `from` into `to`: the Engine fields and the Shared ones (the engine's own value is the fallback).
void copyEngineHalf(const AppConfig& from, AppConfig& to);

// The interface's half of `from` into `to`: the Interface fields, and the Shared ones when `includeShared`.
void copyInterfaceHalf(const AppConfig& from, AppConfig& to, bool includeShared = true);

// Whether the interface's half of `a` and `b` is the same - Interface and Shared fields, the self-moving ones left out. What an
// interface asks before it sends.
bool interfaceHalvesEqual(const AppConfig& a, const AppConfig& b);

// The self-moving fields of both halves set back to their defaults: what a keeper compares, so that a field that changes by itself
// can neither keep it from settling nor make it write every look.
void clearBookkeeping(AppConfig& cfg);

// THE DOCUMENT. serializeInterfaceHalf: the interface's half of `cfg` as the text ConfigStore::serialize writes, cut to the keys of
// the half (the legacy map rectangle is always kept when it is set - ConfigStore::serialize drops it once a page exists, and a
// reader that still wants it as the default for a new page must not lose it on the way). parseInterfaceHalf: the same rules the
// file is read by (ConfigStore::parse); `out` is a whole AppConfig whose interface half is the document's and whose other fields are
// defaults - a caller takes the half with copyInterfaceHalf. False with `error` for a document that is not JSON.
std::string serializeInterfaceHalf(const AppConfig& cfg);
bool parseInterfaceHalf(const std::string& document, AppConfig& out, std::string& error);

}  // namespace cascade::core
