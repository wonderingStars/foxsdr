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
#include <map>
#include <string>
#include <vector>

#include <functional>

#include "core/plugin_abi.h"
#include "core/plugin_host_data.hpp"
#include "core/plugin_module_check.hpp"

namespace cascade::core {


// The set of plugins the user has STOPPED: loaded, listed, and deliberately
// given no runtime instances at all.
//
// ONE implementation, shared by the two halves of the plugin system
// (PluginRunner owns the decoders, PluginUi owns the map and panel
// capabilities). Two copies of "is this plugin stopped" would eventually
// disagree, and a disagreement here means a plugin the user stopped is still
// half-running — still on the map, or still holding the receiver.
//
// KEYED ON THE PLUGIN'S ID, not on its file name (0.99.73, core/plugin_run.hpp): the module
// file name carries the version, so a stop kept against "pocsag-decoder-1.0.2-...dll" was lost
// the day the plugin updated to 1.0.3 and the stopped decoder came back to life. Each entry
// is reduced to its id when it is set (a legacy file name and an id both work), and a key is
// reduced the same way when it is asked about, so the versioned file name no longer matters.
// pluginKey() is still the identity the tune grant and the mute override use, for the reason
// stated there.
class PluginStopSet {
public:
    // Replaces the whole set. The caller (the GUI) owns the durable copy and
    // pushes it down; nothing here reads or writes a config.
    void set(std::vector<std::string> keys);

    // The ids held, in the order given, without duplicates or empties.
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
    //
    // INSIDE AN MSIX PACKAGE the per-user directory is used and the write
    // probe is NOT RUN AT ALL. The package directory is read-only by design,
    // so the probe's answer is known in advance - and skipping it means the
    // application never attempts a write into
    // C:\Program Files\WindowsApps\<full name>, rather than attempting one
    // that is refused. See core/package_identity.hpp.
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
    //
    // `packaged` is "this process is running from an MSIX package" (see
    // core/package_identity.hpp). It OVERRIDES `exeDirWritable` rather than
    // being folded into it, because the two say different things: inside a
    // package the exe directory is C:\Program Files\WindowsApps\<full name>,
    // which Microsoft documents as read-only and which the OS protects, so
    // the per-user directory is the answer even if a probe somehow said
    // otherwise. It defaults to false so every existing caller and every
    // existing test keeps its exact meaning.
    static std::string choosePluginDir(const std::string& exeDir, const std::string& userDir,
                                       bool exeDirWritable, bool packaged = false);

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
    //
    // `origins` is what the install records say about each file (PluginFileOrigins),
    // handed in by the caller that already loaded the manifest; scan() stamps each
    // record's `origin` with it and resolveDuplicatePlugins() uses it for the one rule
    // that a plugin site's module never takes a declared name from an official one.
    // Empty (the default) means no records: every module is Unrecorded.
    void scan(const std::string& dir, const PluginFileOrigins& origins = PluginFileOrigins{});

    // THE LOAD GATE (plugin containment P1): asked of every candidate BEFORE its file is read or mapped, with the module's file name. It
    // answers true to refuse the file - with the verdict and the sentence that say why (core/plugin_marker.hpp's Containment::gate: the
    // plugin faulted last session, safe mode; the engine adds a plugin site's file that is not the bytes that were installed) - and the
    // record is then kept with `loaded == false`, that error and that verdict, and nothing of the module runs. Unset (the default), every
    // file is a candidate. Called on the scanning thread.
    using LoadGate = std::function<bool(const std::string& fileName, ModuleVerdict& verdict, std::string& sentence)>;
    void setLoadGate(LoadGate gate) { loadGate_ = std::move(gate); }

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
    LoadGate loadGate_;
};

}  // namespace cascade::core
