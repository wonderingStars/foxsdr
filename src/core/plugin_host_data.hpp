// plugin_host_data.hpp - what a module scan RECORDS: the data of core/plugin_host.hpp (shell retirement R11a, 2026-10-29).
//
// LoadedPlugin (one record per candidate file the scan found, with its reason when it did not load), PluginOrigin, PluginRejection and the
// functions that read them (validatePluginDesc, pluginRejectionMessage, describePluginRejection, resolveDuplicatePlugins, pluginKey) are DATA and
// arithmetic over it. The window draws them (the fitted-modules list and the module census take a record) and needs none of the machinery that
// makes them: PluginHost loads, validates and unmaps real modules, and PluginStopSet is the engine's stop list. The declarations moved here verbatim
// from plugin_host.hpp, which includes this header, so every user of PluginHost sees the same names as before; their DEFINITIONS stay in
// plugin_host.cpp. The interface includes this header and not plugin_host.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_fault.hpp"

namespace cascade::core {

// WHERE A MODULE FILE CAME FROM, as the install records (PluginRepo's installed.json) say
// (0.99.74). The host reads no manifest itself: the caller hands scan() what it already
// loaded, by bare file name, and the host stamps each record with the answer.
//   Unrecorded   no record names the file (hand-copied, or installed before records existed);
//   Official     a record names it and its source is the FoxSDR catalogue's (empty);
//   Site         a record names it and its source is a plugin site the user added.
// Only resolveDuplicatePlugins() reads it, for the one rule that a SITE'S module never takes a
// declared name away from an OFFICIAL one (see there).
enum class PluginOrigin { Unrecorded = 0, Official, Site };
// ...as the containment spells it (core/plugin_fault.hpp): the same three, in the same order.
inline fault::Origin faultOriginOf(PluginOrigin o) {
    return o == PluginOrigin::Official ? fault::Origin::Official : (o == PluginOrigin::Site ? fault::Origin::Site : fault::Origin::Unrecorded);
}

// File name -> "installed from a plugin site". A file with an entry has an install record;
// `true` is a site's, `false` the official catalogue's. Keys are bare module file names
// (pluginKey), compared ASCII case-insensitively as NTFS names are.
using PluginFileOrigins = std::map<std::string, bool>;

// Why a plugin was refused. One value per distinct failure so the tests can
// assert the exact reason instead of grepping a message, and so the UI can
// group failures if it ever wants to.
enum class PluginRejection {
    None = 0,                   // descriptor is acceptable
    NullDescriptor,             // query returned NULL (plugin declined the host)
    AbiVersionMismatch,         // abiVersion != CASCADE_PLUGIN_ABI_VERSION
    DescStructSizeMismatch,     // sizeof(CascadePluginDesc) disagrees
    ReservedNotZero,            // reserved field used by a stranger's layout
    MissingName,                // name NULL or empty
    MissingVersion,             // version NULL or empty
    MissingAuthor,              // author NULL (empty is allowed)
    MissingLicence,             // licence NULL or empty - never negotiable
    NoCapabilities,             // capabilities == 0: the plugin does nothing
    UnknownCapability,          // a bit this ABI version does not define
    MissingDecoderApi,          // CASCADE_CAP_DECODER set, decoder NULL
    DecoderStructSizeMismatch,  // sizeof(CascadeDecoderApi) disagrees
    DecoderRateOutOfRange,      // requiredRateHz neither 0 nor sane audio rate
    MissingDecoderFunction,     // one of create/process/poll_text/destroy NULL
    // --- ABI 2: the IQ decoder table, checked exactly like the audio one ---
    MissingIqDecoderApi,          // CASCADE_CAP_IQ_DECODER set, iqDecoder NULL
    IqDecoderStructSizeMismatch,  // sizeof(CascadeIqDecoderApi) disagrees
    IqDecoderRateOutOfRange,      // required/preferredRateHz not 0 and not a
                                  // rate this host could ever deliver (also
                                  // catches NaN and infinity)
    MissingIqDecoderFunction,     // create/process/poll_text/destroy NULL
                                  // (retune is optional and may be NULL)
    // --- ABI 3: the capability table replaced the per-capability pointers ---
    CapabilityCountOutOfRange,  // capabilityCount is 0, or absurdly large
    MissingCapabilityTables,    // capabilityCount > 0 but the array is NULL
    NoUsableCapability,         // every capability declared is one this host
                                // does not know, or has an unusable table, so
                                // the plugin provides this host with nothing.
                                // NOT the same as UnknownCapability: an
                                // unknown bit ALONGSIDE a usable one is fine
                                // and is ignored (see plugin_abi.h).
    // --- ABI 3: the image decoder table, checked like the other two ---
    MissingImageDecoderApi,          // bit set, table entry absent or NULL
    ImageDecoderStructSizeMismatch,  // sizeof(CascadeImageDecoderApi) disagrees
    ImageDecoderBadInputKind,        // inputKind not AUDIO or IQ
    ImageDecoderRateOutOfRange,      // rate not 0 and not sane for inputKind
    MissingImageDecoderFunction,     // a mandatory pointer is NULL
                                     // (retune is optional and may be NULL)
    // --- UI and host-services capabilities, added without an ABI bump ------
    MissingTrackSourceApi,
    TrackSourceStructSizeMismatch,
    MissingTrackSourceFunction,   // poll_paths is optional and may be NULL
    MissingPanelApi,
    PanelStructSizeMismatch,
    PanelBadColumnCount,          // columns() is mandatory; 1..MAX enforced later
    MissingPanelFunction,
    MissingHostClientApi,
    HostClientStructSizeMismatch,
    MissingHostClientFunction,
    MissingPresetApi,
    PresetStructSizeMismatch,
    MissingPresetFunction,
    MissingBasemapApi,
    BasemapStructSizeMismatch,
    MissingBasemapFunction,
    MissingBasemapAttribution,  // ODbL-derived tiles REQUIRE it; see plugin_abi.h
    BasemapBadTileSize,
    MissingTrackInfoApi,
    TrackInfoStructSizeMismatch,
    MissingTrackInfoFunction,
    MissingInstrumentApi,
    InstrumentStructSizeMismatch,
    MissingInstrumentFunction,     // create/poll_state/destroy NULL, or no title
    InstrumentHalfMemory,          // columns and poll_rows must be both set or both NULL
    MissingAudioOutApi,
    AudioOutStructSizeMismatch,
    AudioOutBadRate,       // sampleRateHz 0, or outside the audio bounds. 0 is
                           // refused here though the decoder tables allow it:
                           // see CascadeAudioOutApi - a producer that will not
                           // state its rate cannot be resampled.
    AudioOutBadChannels,   // channels neither 1 nor 2
    MissingAudioOutFunction,  // pull or active NULL
    // --- Host API level 1: the in-chain audio processor ---------------------
    MissingAudioProcessorApi,
    AudioProcessorStructSizeMismatch,
    MissingAudioProcessorFunction,  // create/process/destroy NULL, or no title
    AudioProcessorReservedNotZero,  // flags is reserved and must be 0
    // --- CASCADE_CAP_SETTINGS_UI (0.99.43) ---------------------------------
    MissingSettingsUiApi,
    SettingsUiStructSizeMismatch,
    SettingsUiBadTable,  // specs NULL, or count 0 or above CASCADE_MAX_SETTING_SPECS
    // --- CASCADE_CAP_TX_AUDIO (0.99.74, host API level 2) -------------------
    MissingTxAudioApi,
    TxAudioStructSizeMismatch,
    TxAudioBadRate,          // sampleRateHz is not 48000, the only rate this host takes
    TxAudioBadChannels,      // channels is not 1
    TxAudioReservedNotZero,  // flags is reserved and must be 0
    MissingTxAudioFunction,  // pull or active NULL
    TxAudioNeedsDecoder,     // declared with no decoder capability to ride on
    MissingImageInputApi,
    ImageInputStructSizeMismatch,
    MissingImageInputFunction,
    ImageInputNeedsDecoder,
    // --- plugin containment P1, load-time validation (docs/engine-stage4.md 6.28) ----------------------------------------------
    DuplicateCapabilityTable,    // two entries claim one declared capability: which table is the plugin's?
};

// The whole compatibility decision, as a pure function of the descriptor, so
// it is exhaustively testable without a DLL, a filesystem or a loader. `desc`
// may be null (-> NullDescriptor).
//
// Check ORDER is part of the contract: abiVersion is examined before
// structSize, and both before any other field, because those two fields are
// the only ones whose offsets are frozen across ABI versions (see the LAYOUT
// RULE in plugin_abi.h). Reading `name` out of a descriptor whose layout we
// have not yet verified would be exactly the memory-safety bug this check
// exists to prevent.
PluginRejection validatePluginDesc(const CascadePluginDesc* desc);

// Stable, human-readable text for a rejection. Never null, never empty.
const char* pluginRejectionMessage(PluginRejection r);

// Same, with the offending numbers appended where they help ("expected 1,
// plugin reports 7"). Reads only fields that are safe to read for the given
// rejection, so it is callable on any descriptor validatePluginDesc rejected.
std::string describePluginRejection(PluginRejection r, const CascadePluginDesc* desc);

// One record per candidate file found by the scan, in sorted filename order.
struct LoadedPlugin {
    std::string path;      // absolute path of the candidate file
    std::string name;      // descriptor fields, COPIED so they outlive
    std::string version;   // unloadAll(); empty when the plugin was refused
    std::string author;    // before its descriptor could be read
    std::string licence;
    std::uint32_t capabilities = 0;

    bool loaded = false;   // true iff the module is mapped and validated
    std::string error;     // empty iff loaded; the reason otherwise
    // THE VERDICT WORD of a refusal the host reached from the FILE and the records alone, before or without running the module
    // (core/plugin_module_check.hpp moduleVerdictWord: "truncated", "wrong-abi", "changed-since-install", "faulted-last-session",
    // "safe-mode", "corrupt", ...), or empty - for a loaded module, and for a refusal that only the loader could make (which a rescan may
    // see differently when something outside the folder changes). A refusal with a verdict is deterministic, so a rescan that found
    // nothing else changed does not need to be repeated for its sake (Engine::rescanPlugins).
    std::string verdict;
    // The module file (pluginKey) of the copy that is running INSTEAD of
    // this one, when resolveDuplicatePlugins turned this one off as another
    // copy of the same plugin; empty otherwise. What core/plugin_cleanup.hpp
    // reads to find the old copies an update left behind, rather than
    // parsing `error`, which is words for a person.
    std::string supersededBy;

    // What the install records say about where this file came from (PluginOrigin), set by
    // scan() from the PluginFileOrigins it was given; Unrecorded when it was given none.
    PluginOrigin origin = PluginOrigin::Unrecorded;

    // Valid only while `loaded` and only until unloadAll()/scan()/destruction
    // - they point into the plugin's own image. Each is null unless the
    // plugin DECLARED the matching capability bit: a descriptor that supplies
    // a table without declaring its bit gets the table ignored here rather
    // than propagated, so no caller can end up driving a facility the plugin
    // never claimed to provide.
    const CascadeDecoderApi* decoder = nullptr;      // CASCADE_CAP_DECODER
    const CascadeIqDecoderApi* iqDecoder = nullptr;  // CASCADE_CAP_IQ_DECODER
    const CascadeImageDecoderApi* imageDecoder = nullptr;  // CASCADE_CAP_IMAGE_DECODER
    const CascadeImageInputApi* imageInput = nullptr;      // CASCADE_CAP_IMAGE_INPUT
    const CascadeTrackSourceApi* trackSource = nullptr;    // CASCADE_CAP_TRACK_SOURCE
    const CascadePanelApi* panel = nullptr;                // CASCADE_CAP_PANEL
    const CascadeInstrumentApi* instrument = nullptr;      // CASCADE_CAP_INSTRUMENT
    const CascadeHostClientApi* hostClient = nullptr;      // CASCADE_CAP_HOST_CLIENT
    const CascadePresetApi* preset = nullptr;              // CASCADE_CAP_PRESET
    // CASCADE_CAP_SETTINGS_UI (0.99.43): the form the host draws for the user.
    // A static table, like `preset`; core/plugin_settings_ui.hpp turns it
    // into fields.
    const CascadeSettingsUiApi* settingsUi = nullptr;
    const CascadeBasemapApi* basemap = nullptr;            // CASCADE_CAP_BASEMAP
    const CascadeTrackInfoApi* trackInfo = nullptr;        // CASCADE_CAP_TRACK_INFO
    // CASCADE_CAP_AUDIO_OUT. Carries no create/destroy of its own: it rides on
    // whichever decoder instance this plugin's other capabilities produced (see
    // CascadeAudioOutApi), so a record with this set and no decoder table is a
    // plugin the runner will never pull from.
    const CascadeAudioOutApi* audioOut = nullptr;
    // CASCADE_CAP_AUDIO_PROCESSOR (host API level 1): transforms the
    // demodulated audio in place. Driven by PluginRunner.
    const CascadeAudioProcessorApi* audioProcessor = nullptr;
    // CASCADE_CAP_TX_AUDIO (host API level 2): audio to TRANSMIT. Like audioOut it has
    // no create/destroy and rides on this plugin's first decoder instance; the host
    // keys the transmitter only under the conditions plugin_abi.h lists. Driven by
    // PluginRunner, and never by anything else.
    const CascadeTxAudioApi* txAudio = nullptr;

    // HMODULE (Windows) or dlopen handle (POSIX), as void* so this header
    // stays free of <windows.h>. Null unless `loaded`.
    void* nativeHandle = nullptr;

    // THE SIZE OF `path` AS THE SCAN SAW IT, in bytes; 0 means "not measured"
    // (a record built by hand, or a size the listing could not give) and is
    // never to be drawn as a clean zero. It is taken ONCE, by scan(), out of the
    // directory listing it is already reading - and it is a field because the
    // Fitted modules window used to ask the file system for it again on every
    // frame, for every module, on the thread that draws the window: a
    // synchronous stat per plugin per frame, in a folder that may be a network
    // or synchronised profile, a scanner's, or a spun-down disk. That is the
    // fault class of the settings-folder poll fixed in 0.99.61 and it is fixed
    // the same way: nothing on the frame asks the disk, and a figure that is
    // allowed to be as old as the last scan is the whole of it. Refused records
    // carry it too - a file the host would not load still has a size.
    std::uint64_t fileBytes = 0;
};

// ONE VERSION OF A PLUGIN RUNS, NEVER TWO.
//
// A plugin's file name embeds its version, so installing 1.1.0 over 1.0.1 ADDS
// a file rather than replacing one, and nothing removes the old file. Both
// modules are then perfectly loadable, so scan() loads both, each gets its own
// instances, and the results are doubled: a track source reports every
// aircraft twice (the user's report was "4 targets" for two aeroplanes, drawn
// as two markers because the duplicates share coordinates), a decoder decodes
// twice, a basemap fights itself.
//
// This is the host's refusal to be confused by that. Given the records of one
// scan, in scan order, it keeps exactly ONE loaded record per declared plugin
// id and turns the rest OFF - loaded = false, capability pointers cleared, and
// an `error` saying which file was kept instead. Returns how many it turned
// off.
//
//   THE ID is the descriptor's `name`, copied into LoadedPlugin::name by the
//   loader. It is the only identity the module itself declares; the file name
//   is precisely what changes between two copies of one plugin. Compared
//   exactly, byte for byte.
//
//   ...AND THE RUN ID (0.99.77). The declared name can change between two
//   files of one plugin - a US tester had "TETRA" in tetra-decoder-0.1.0 and
//   "TETRA Monitor (data only, no voice)" in tetra-decoder-0.1.2, both
//   loaded, and the DECODE bank drew two sections under one widget-id scope.
//   The file name without its version (pluginRunId(pluginKey(r)),
//   "tetra-decoder") is the key the bank scopes by and the stop set, the
//   saved presets and the dormancy clock are kept under, so two LOADED records
//   with one run id are ONE plugin to all of those whatever each declares. The
//   rule is applied to what the name pass leaves: the highest version of those
//   candidates that share a run id wins (same comparator, same first-record
//   tie-break), the others are turned off with the same words - naming the
//   WINNER's name - and `supersededBy` naming the winner's file, which is what
//   the clean-up (core/plugin_cleanup.hpp) finds the copy that stays by. Two
//   records that differ in name AND in run id are two plugins and never meet;
//   a record with no path has no run id, and an empty run id matches nothing.
//
//   THE WINNER is the HIGHEST VERSION, ordered by PluginRepo::compareVersions
//   - the catalogue's own comparator, reused rather than reimplemented so the
//   host and the update check can never disagree about which of two versions
//   is newer. It compares dotted parts as NUMBERS: comparing them as text puts
//   "1.10.0" below "1.9.0" and would keep the older plugin, which is the exact
//   opposite of the point.
//
//   THE TIE-BREAK, for two files declaring the same id AND the same version,
//   is the FIRST record in the list. scan() sorts its candidates by path, so
//   that is the lexicographically first file name, and it is the same file on
//   every run - a scan that loaded a different module depending on the order
//   the filesystem happened to return would be worse than one that is
//   consistently wrong.
//
//   A PLUGIN SITE'S MODULE NEVER TAKES A NAME FROM AN OFFICIAL ONE (0.99.74).
//   The declared name is written by whoever built the binary, and the version
//   rule above would let a module from a site the user added declare an
//   official plugin's name with a higher version and switch the official one
//   off. So, for each name: if any LOADED record of it is Official (an install
//   record whose source is the FoxSDR catalogue's), every Site record of that
//   name is turned off whatever its version, with a reason that says so
//   ("Not loaded: it declares the same name as the official plugin ...") and
//   `supersededBy` naming the official module that stays. The winner among
//   the rest is chosen by the version rule as before. Everything else is
//   unchanged: two Site modules of one name compete by version, an Unrecorded
//   module (hand-copied, no record) competes by version against anything, and
//   with no origins given (every record Unrecorded) the policy is exactly the
//   one above. The FILE NAME is the publisher's to write as much as the
//   declared name is (0.99.77), so the same holds for the run id: a Site record
//   whose run id an Official record holds is turned off whatever its version,
//   with "Not loaded: its file name is the official plugin ...".
//
// DELETES NOTHING. It does not unlink, rename, move or truncate any file, and
// it does not close a module either - it only marks records, leaving
// nativeHandle set so its caller can unmap what it turned off, and naming the
// copy kept instead in LoadedPlugin::supersededBy. The stale file stays on
// disk, inert, until something removes it: since 0.99.49 an update removes the
// copies it superseded, and the plugin store's CLEAN UP OLD VERSIONS key the
// rest (core/plugin_cleanup.hpp) - or the Remove key, one file at a time.
//
// Records that did not load are ignored entirely: their descriptor was never
// read, so they have no id, and two unrelated broken files must stay two
// separately reported problems.
//
// Pure apart from the vector it is handed, so the whole policy is testable
// without a DLL, a filesystem or a loader.
std::size_t resolveDuplicatePlugins(std::vector<LoadedPlugin>& records);

// The plugin's MODULE FILE NAME, which is the identity every per-plugin
// decision in this product is keyed on: the tune grant, and now the stop.
//
// NOT the display name. The display name comes out of the plugin's own
// descriptor, so keying on it lets any plugin inherit another's setting simply
// by claiming its name; the file name it cannot change without replacing the
// granted file itself, which needs write access to the plugins directory and
// is already game over. A scan produces one record per file in one directory,
// so the file name is unique across a scan.
//
// Empty when the record has no path — and an empty key must never match
// anything, or every path-less record would share one entry.
std::string pluginKey(const LoadedPlugin& p);

}  // namespace cascade::core
