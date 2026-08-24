// Plugin host: discovers, validates and loads out-of-tree decoder plugins.
//
// WHY THIS EXISTS. cascade is sold as a binary. Some decoders are risky to
// ship inside it - a POCSAG or FLEX decoder is a parser fed by hostile RF, a
// crash there is a crash of the whole receiver, and several of the good
// reference implementations are GPL, which must never enter this tree (see
// PLAN.md's legal ground rules). Making those decoders separate downloads
// that the user installs solves both problems at once: the sold binary
// contains none of their code, and a badly behaved one is a plugin that
// failed rather than a product that crashed.
//
// WHAT THE HOST GUARANTEES.
//   - Every candidate file in the scanned directory produces exactly one
//     record. A plugin that cannot be loaded is recorded WITH ITS REASON and
//     not loaded; it is never silently skipped, because "my plugin does not
//     appear" with no explanation is the worst possible support ticket.
//   - A plugin is loaded only after its descriptor passes every check in
//     validatePluginDesc (exact ABI version, every struct size, all required
//     strings, a table for every declared capability, and every non-optional
//     function pointer in each of those tables). A module that fails is
//     unmapped again immediately - a rejected plugin never stays in the
//     address space.
//   - The licence string the plugin declares is copied into the record and
//     is meant to be displayed. A user-installed GPL plugin is the user's
//     business; hiding what got loaded is not acceptable either way.
//
// WHAT THE HOST DOES *NOT* GUARANTEE - read this before assuming safety.
//   - It cannot make a plugin memory-safe. A plugin that scribbles over the
//     heap, smashes its stack, or corrupts CRT state can take the process
//     down at any later moment, in code that has nothing to do with plugins.
//     There is no in-process defence against that; only a separate process
//     would provide one, and that is deliberately out of scope for now.
//   - On MSVC the single call into plugin code that the host itself makes
//     (cascade_plugin_query) is wrapped in a structured-exception handler, so
//     an access violation *inside that one call* is turned into a rejected
//     plugin instead of a dead process. That is damage limitation for the
//     common case of a stale or corrupt DLL, not a sandbox: by the time an
//     access violation has been raised the plugin's own state is suspect, so
//     the host unmaps the module and refuses it rather than continuing.
//   - Decoder callbacks (create/process/poll_text/destroy) are NOT wrapped.
//     They run in the real-time audio path where a per-call SEH frame is both
//     too costly and useless - a decoder that faults mid-stream has already
//     lost. The ABI's rule is that they must not throw or fault; the host
//     trusts it, exactly as it trusts any other loaded code.
//   - DllMain runs before the host sees the module at all. A plugin can do
//     anything it likes there, and LoadLibrary itself is the point of no
//     return. This is inherent to dynamic loading on every platform.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/plugin_abi.h"

namespace cascade::core {

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

    // Valid only while `loaded` and only until unloadAll()/scan()/destruction
    // - they point into the plugin's own image. Each is null unless the
    // plugin DECLARED the matching capability bit: a descriptor that supplies
    // a table without declaring its bit gets the table ignored here rather
    // than propagated, so no caller can end up driving a facility the plugin
    // never claimed to provide.
    const CascadeDecoderApi* decoder = nullptr;      // CASCADE_CAP_DECODER
    const CascadeIqDecoderApi* iqDecoder = nullptr;  // CASCADE_CAP_IQ_DECODER
    const CascadeImageDecoderApi* imageDecoder = nullptr;  // CASCADE_CAP_IMAGE_DECODER
    const CascadeTrackSourceApi* trackSource = nullptr;    // CASCADE_CAP_TRACK_SOURCE
    const CascadePanelApi* panel = nullptr;                // CASCADE_CAP_PANEL
    const CascadeHostClientApi* hostClient = nullptr;      // CASCADE_CAP_HOST_CLIENT
    const CascadePresetApi* preset = nullptr;              // CASCADE_CAP_PRESET
    const CascadeBasemapApi* basemap = nullptr;            // CASCADE_CAP_BASEMAP
    const CascadeTrackInfoApi* trackInfo = nullptr;        // CASCADE_CAP_TRACK_INFO

    // HMODULE (Windows) or dlopen handle (POSIX), as void* so this header
    // stays free of <windows.h>. Null unless `loaded`.
    void* nativeHandle = nullptr;
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
// DELETES NOTHING. It does not unlink, rename, move or truncate any file, and
// it does not close a module either - it only marks records, leaving
// nativeHandle set so its caller can unmap what it turned off. The stale file
// stays on disk, inert, until the user removes it with the Remove button the
// Plugins section already offers.
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

// The set of plugins the user has STOPPED: loaded, listed, and deliberately
// given no runtime instances at all.
//
// ONE implementation, shared by the two halves of the plugin system
// (PluginRunner owns the decoders, PluginUi owns the map and panel
// capabilities). Two copies of "is this plugin stopped" would eventually
// disagree, and a disagreement here means a plugin the user stopped is still
// half-running — still on the map, or still holding the receiver.
//
// Keyed on pluginKey(), for the reason stated there.
class PluginStopSet {
public:
    // Replaces the whole set. The caller (the GUI) owns the durable copy and
    // pushes it down; nothing here reads or writes a config.
    void set(std::vector<std::string> keys) { keys_ = std::move(keys); }

    const std::vector<std::string>& keys() const { return keys_; }

    bool contains(const std::string& key) const;
    // Convenience for the rebuild loops, which have the record and not the key.
    bool contains(const LoadedPlugin& p) const { return contains(pluginKey(p)); }

private:
    std::vector<std::string> keys_;
};

class PluginHost {
public:
    PluginHost() = default;
    ~PluginHost();  // unloadAll(): a host leaving scope unmaps its plugins

    // Owns OS module handles; copying one would double-free them.
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    // Where this installation keeps its plugins.
    //
    // "<directory of the running executable>/plugins" WHEN THAT DIRECTORY CAN
    // BE WRITTEN, which covers a portable copy, a developer build, and an
    // installation the user chose to put somewhere they own. Next to the
    // binary stays the preferred answer, because plugins are code and a
    // directory only the installer can write cannot be quietly repopulated.
    //
    // Otherwise a per-user directory: %LOCALAPPDATA%/foxsdr/plugins on
    // Windows, $XDG_DATA_HOME (or ~/.local/share) /foxsdr/plugins elsewhere.
    // That is less a weakening of the rule above than an admission of what
    // the rule was worth. A default installation lands under Program Files,
    // which is read-only to the user running the app, so WITHOUT this
    // fallback the catalogue cannot save its version policy and no plugin can
    // ever be installed at all - which is what shipped, and what this
    // repairs. The property the original comment claimed to protect does not
    // hold either way: FoxSDR does not run elevated, so anyone who can write
    // to the per-user directory can already run code as that user and gains
    // no privilege by routing it through this application.
    //
    // Falls back to the relative "plugins" if the executable path cannot be
    // determined (which would mean a very unusual host process).
    static std::string defaultPluginDir();

    // The two candidates defaultPluginDir() chooses between, exposed so the
    // choice is testable and so the GUI can name the one in use.
    static std::string exePluginDir();
    static std::string userPluginDir();

    // True if this process can actually put a file in `dir`. Probes by
    // WRITING one and removing it again, because existence, is_directory and
    // even access() all answer "fine" for an installed program directory that
    // then refuses every write. When `dir` does not exist yet, the parent
    // that would have to hold it is probed instead, so the answer means "a
    // plugin can be installed here" rather than "this exists". Leaves nothing
    // behind, and never throws.
    static bool directoryIsWritable(const std::string& dir);

    // The decision itself, pure and total: `exeDir` when it is writable,
    // otherwise `userDir` - except that an empty `userDir` (nothing in the
    // environment to derive one from) yields `exeDir`, so the caller reports
    // the real failure against the real path rather than against "".
    static std::string choosePluginDir(const std::string& exeDir, const std::string& userDir,
                                       bool exeDirWritable);

    // True if `filename` has the shared-library extension this platform's
    // plugins use (".dll" on Windows, ".so"/".dylib" elsewhere). Case
    // insensitive on Windows, matching the filesystem. Pure and testable.
    static bool hasPluginExtension(const std::string& filename);

    // Scans `dir` for candidate files and tries to load each one.
    //
    // Always succeeds in the sense that it never throws and never reports a
    // global failure: a directory that does not exist, is not a directory, or
    // cannot be read yields an EMPTY plugin list, which is exactly right for
    // the overwhelmingly common case of a user who installed no plugins. A
    // missing plugins directory is not an error condition to surface.
    //
    // Any previously loaded plugins are unloaded first, so scan() is
    // idempotent and re-scannable after the user drops in a new file.
    void scan(const std::string& dir);

    // Convenience: scan(defaultPluginDir()).
    void scanDefault();

    // Every candidate from the last scan, loaded or refused, sorted by
    // filename so the UI order is deterministic across runs.
    const std::vector<LoadedPlugin>& plugins() const { return plugins_; }

    // Number of records with loaded == true.
    std::size_t loadedCount() const;

    // The directory the last scan looked in (empty before the first scan).
    const std::string& directory() const { return directory_; }

    // Unmaps every loaded module and clears the records. Idempotent: calling
    // it twice, or before any scan, is a no-op. After it returns, every
    // `decoder` and `iqDecoder` pointer previously handed out is dangling -
    // callers must drop their decoder instances BEFORE calling this.
    void unloadAll();

private:
    std::vector<LoadedPlugin> plugins_;
    std::string directory_;
};

}  // namespace cascade::core
