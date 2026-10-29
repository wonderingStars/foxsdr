// plugin_module_check.hpp - is this file a plugin module at all, BEFORE anything of it runs (plugin containment P1, item 5; docs 6.28).
//
// WHY. Mapping a module runs its DllMain; there is no undoing that. Until now the host called LoadLibrary on every file in the plugins
// folder and learned what it was afterwards - from the loader's error, or from a descriptor it then read. A truncated download, a file of
// another architecture, a DLL that exports nothing, a build made for an older plugin ABI: each reached the loader (and a corrupt one can
// take the process down inside it, or hang the loader lock). This reads the file's own headers first, as bytes, and refuses what is not a
// plugin for this host with a VERDICT - a fixed word and a sentence - that the Fitted modules window shows in place of a log line.
//
// WHAT IT CHECKS (Windows PE; an ELF shared object on Linux; a Mach-O is accepted on its magic alone and UNVERIFIED):
//   readable       the file opens and is not larger than kMaxModuleBytes (a plugin is megabytes; a 4 GB file is not one);
//   image          it begins with the format's magic, its headers fit inside the file, and its section table does too;
//   architecture   the machine the image was built for is the machine this program runs on;
//   shared library the DLL / shared-object bit is set (an executable renamed .dll is not a plugin);
//   whole          no section's raw data ends beyond the end of the file (the signature of a truncated download);
//   exports        the export directory exists, is readable, and names `cascade_plugin_query`;
//   abi            a file name that declares its plugin ABI (`-abi<N>-`, the catalogue's naming) declares THIS host's.
//
// It is pure over bytes (checkModuleBytes), so every branch is tested with a hand-built image; checkModuleFile only reads the file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_MODULE_CHECK_HPP
#define CASCADE_CORE_PLUGIN_MODULE_CHECK_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/plugin_repo_data.hpp"

namespace cascade::core {

// The verdict words. Closed; written to the Fitted modules row, the log and the anonymous ledger's load class, never anything else.
enum class ModuleVerdict : std::uint8_t {
    Ok = 0,
    Unreadable,    // the file could not be read, or is absurdly large
    NotAnImage,    // not a DLL / shared object at all (no magic)
    Truncated,     // the headers or a section run past the end of the file
    WrongMachine,  // built for another processor
    NotLibrary,    // an executable, not a shared library
    NoExports,     // exports nothing
    NoEntry,       // exports things, but not cascade_plugin_query
    WrongAbi,      // its file name says it was built for another plugin ABI
    Tampered,      // a plugin site's file is not the bytes that were installed (sha256 of the install record)
    Corrupt,       // its descriptor could not be read (a wild pointer) or contradicts itself
    Faulted,       // it faulted last session and was started without it (core/plugin_marker.hpp)
    SafeMode,      // --safe-plugins
    Quarantined,   // it faulted, threw or hung this session and was switched off
};
const char* moduleVerdictWord(ModuleVerdict v);  // "ok", "unreadable", "not-a-module", "truncated", "wrong-machine", ...

struct ModuleCheck {
    ModuleVerdict verdict = ModuleVerdict::Ok;
    std::string detail;  // one sentence for a person: what is wrong, with the numbers it carries
    bool ok() const { return verdict == ModuleVerdict::Ok; }
};

inline constexpr std::uint64_t kMaxModuleBytes = 512ull * 1024ull * 1024ull;

// The headers of `data` (the whole file, `n` bytes) judged for THIS host. The symbol name is the one the plugin ABI defines.
ModuleCheck checkModuleBytes(const std::uint8_t* data, std::size_t n);

// The same judgment for a stated image format and machine instead of this host's - so every branch can be tested with a hand-built image
// on any platform. `machine` is the format's own number: a PE Machine field (0x8664 x64, 0xAA64 arm64, 0x14C x86) or an ELF e_machine
// (62 x86-64, 183 aarch64).
enum class ImageFormat : std::uint8_t { Pe, Elf, MachO };
ModuleCheck checkModuleBytesFor(ImageFormat format, std::uint32_t machine, const std::uint8_t* data, std::size_t n);
// This host's: the format its loader takes and the machine it runs on.
ImageFormat hostImageFormat();
std::uint32_t hostMachine();
// The file named by `path`, read (bounded) and judged, and its name checked for a plugin ABI it declares.
ModuleCheck checkModuleFile(const std::string& path);

// A PLUGIN SITE'S FILE THAT IS NOT THE BYTES THAT WERE INSTALLED. The install record of a module fetched from a site the user added carries
// the sha256 the site promised and the host verified at install; loadInventory re-hashes the file on disk against it at every scan and sets
// `digestMismatch`. For the official catalogue that is a note on the row (a person may hand-replace a file they build themselves); for a
// stranger's code it is a refusal: `out` is set to Tampered and the function answers true. False for every other file, and for a site's
// file whose record agrees with the disk.
bool siteFileTampered(const std::vector<InstalledPlugin>& records, const std::string& fileName, ModuleCheck& out);

// The plugin ABI a catalogue-style file name declares: "pocsag-decoder-1.0.2-abi3-win-x64.dll" -> 3. 0 when the name declares none.
unsigned abiDeclaredByFileName(const std::string& fileName);

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_MODULE_CHECK_HPP
