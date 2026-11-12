// linux_update.hpp - the Linux in-app updater: every DECISION, and the steps that follow
// from them, written platform-neutral behind a small file-and-process interface.
//
// WHY A SEPARATE MODULE. The Windows updater (core/updater.hpp) downloads a signed-by-digest
// installer and hands it to the shell. Linux has no installer: the program is an AppImage (one
// file), an unpacked tree (a tarball the user extracted), or a package a package manager owns.
// What "update" means differs for each, and for the last it must mean "tell the user, change
// nothing". This module owns that decision and the replacement. core/updater.{hpp,cpp} is NOT
// changed and shares nothing with this but the version rules it calls (compareVersions,
// wellFormedVersion).
//
// WHAT IS PROVEN WHERE. Everything here that is logic is tested on Windows against a FAKE
// UpdateHost (an in-memory file system and process) and, for the download, against a stand-in
// HTTP server on 127.0.0.1 (tests/test_linux_update.cpp). The host that touches a real Linux
// system (core/linux_update_host.cpp: rename, chmod, tar, fork/exec, TLS download) is written
// from the POSIX / AppImage documentation and is UNPROVEN ON LINUX.
//
// THE MANIFEST. The same /api/update answer the Windows updater reads may carry a `linux`
// object:
//   "linux": { "version": "0.99.110", "channel": "stable",
//              "assets": [ {"kind":"appimage","arch":"x86_64","url":"https://foxsdr.com/...",
//                           "sha256":"<64 hex>","size":12345678}, ... ] }
// The server does not produce this yet (hand-off in docs/feature-records/platform.md). Every
// field that makes a download safe is checked here and refused rather than warned about, an
// asset that fails is dropped ALONE, and a manifest without a usable asset offers nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_LINUX_UPDATE_HPP
#define CASCADE_CORE_LINUX_UPDATE_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core::linuxupdate {

enum class Arch { X86_64, Aarch64, Unknown };
enum class PackageKind { AppImage, Tarball, Deb, Rpm, Unknown };

// How THIS running copy was installed. Decides whether we may replace anything.
enum class InstallKind {
    AppImage,        // one file the user can write; replaced by rename
    Tree,            // an unpacked tarball directory the user can write; replaced by rename
    PackageManager,  // dpkg / rpm owns the files: never touched
    Flatpak,         // sandbox: updated by flatpak
    Snap,            // updated by snapd
    Container,       // a Docker image: updated by pulling a new image
    Unknown          // cannot tell where we are
};

std::string archName(Arch a);          // "x86_64" / "aarch64" / "unknown"
Arch parseArch(const std::string& s);  // also accepts "amd64", "arm64"
std::string kindName(PackageKind k);   // "appimage" / "tarball" / "deb" / "rpm"
PackageKind parseKind(const std::string& s);
std::string installKindName(InstallKind k);

struct Asset {
    PackageKind kind = PackageKind::Unknown;
    Arch arch = Arch::Unknown;
    std::string url;
    std::string sha256;  // lower-case, 64 hex
    std::uint64_t sizeBytes = 0;
};

struct Manifest {
    std::string version;
    std::string channel;  // "stable" when the answer does not say
    std::vector<std::string> withdrawn;
    std::vector<Asset> assets;
};

inline constexpr std::size_t kMaxAssets = 16;
inline constexpr std::uint64_t kMaxAssetBytes = 1024ull * 1024ull * 1024ull;  // one gibibyte

// Parses the `linux` object of an /api/update answer (pass the WHOLE answer). False, with
// `error`, when there is no usable one. Each asset is kept or dropped on its own.
bool parseLinuxManifest(const std::string& text, Manifest& out, std::string& error);

// The query the check sends: version, channel and architecture, nothing else.
std::string updateRequestUrl(const std::string& baseUrl, const std::string& runningVersion,
                             const std::string& channelSetting, Arch arch);

// --- where we are installed ---------------------------------------------------------------------

// What the process can see of itself. Gathered by the host; the decision is made from this
// record alone so it can be tested without a Linux.
struct InstallProbe {
    std::string exePath;         // resolved path of the running program ("" = unknown)
    std::string appImageEnv;     // $APPIMAGE (the AppImage file's own path), "" when unset
    bool appImageFileExists = false;
    bool flatpakInfo = false;    // /.flatpak-info exists or $FLATPAK_ID set
    bool snapEnv = false;        // $SNAP set
    bool containerEnv = false;   // /.dockerenv exists
    bool ownedByPackage = false; // dpkg -S / rpm -qf says a package owns exePath
    bool underSystemPrefix = false;  // /usr/bin, /usr/lib, /bin, /opt/<pkg> - a system location
    bool dirWritable = false;    // the user can create files beside the program
};
InstallKind detectInstallKind(const InstallProbe& p);

// --- the decision -------------------------------------------------------------------------------

enum class Verdict {
    Install,           // an update exists and can be applied by this program
    UpToDate,          // same version
    DowngradeRefused,  // the manifest names an OLDER version than the one running
    Withdrawn,         // the offered version is on the withdrawn list
    ChannelMismatch,   // the answer is for another channel than the one asked for
    NoAsset,           // nothing for this architecture / install kind
    TellOnly           // an update exists but this install is not ours to replace
};

struct Decision {
    Verdict verdict = Verdict::UpToDate;
    InstallKind install = InstallKind::Unknown;
    Asset asset;          // set for Install
    std::string version;  // the version the manifest offered
    std::string message;  // what the user is told, one or two sentences, English
};

struct Config {
    std::string runningVersion;
    std::string channelSetting;  // AppConfig::updateChannel: "beta" or anything else = stable
    Arch arch = Arch::X86_64;
};

Decision decide(const Config& cfg, const Manifest& m, const InstallProbe& probe);

// --- the interface to the file system and the processes ---------------------------------------------

struct FetchResult {
    enum class Status { Ok, Cancelled, Failed } status = Status::Failed;
    std::uint64_t totalBytes = 0;  // size of the file as the file now stands on disk
    bool resumed = false;          // false when the server ignored Range and the file restarted
    std::string error;
};

// Everything the updater does to the outside world. Paths use '/'. A fake implements it for the
// tests; core/linux_update_host.cpp is the real one.
class UpdateHost {
public:
    virtual ~UpdateHost() = default;
    virtual bool exists(const std::string& path) = 0;
    virtual std::int64_t fileSize(const std::string& path) = 0;  // -1 when absent / a directory
    virtual bool makeDir(const std::string& path) = 0;
    virtual bool writableDir(const std::string& dir) = 0;
    virtual std::uint64_t freeSpace(const std::string& dir) = 0;
    virtual bool createExclusive(const std::string& path) = 0;  // false if it exists already
    virtual bool removeAll(const std::string& path) = 0;        // a file or a tree; true if gone
    virtual bool rename(const std::string& from, const std::string& to) = 0;  // atomic, same fs
    virtual bool writeText(const std::string& path, const std::string& text) = 0;
    virtual bool readText(const std::string& path, std::string& text) = 0;
    virtual bool readHead(const std::string& path, std::size_t n, std::string& bytes) = 0;
    virtual bool sha256(const std::string& path, std::string& hexLower) = 0;
    virtual bool makeExecutable(const std::string& path) = 0;
    // Appends to `partialPath` from byte `resumeFrom` (0 = start a new file). At most `maxBytes`
    // in all. Polls `cancel` between chunks.
    virtual FetchResult fetch(const std::string& url, const std::string& partialPath,
                              std::uint64_t resumeFrom, std::uint64_t maxBytes,
                              std::atomic<bool>* cancel) = 0;
    virtual bool listArchive(const std::string& archive, std::vector<std::string>& names) = 0;
    virtual bool extractArchive(const std::string& archive, const std::string& destDir) = 0;
    // Runs `program` with `args` (no shell), waits at most timeoutMs, returns its exit code
    // (-1 = could not start or timed out) and what it printed.
    virtual int runProgram(const std::string& program, const std::vector<std::string>& args,
                           int timeoutMs, std::string& output) = 0;
};

// --- applying -----------------------------------------------------------------------------------

enum class Outcome {
    Applied,         // the new copy is in place and ran once; the old one is kept beside it
    NothingToDo,     // the decision was not Install
    Cancelled,
    NoSpace,
    NotWritable,
    Busy,            // another update is in progress (lock file present)
    DownloadFailed,
    HashMismatch,    // bytes arrived and did not hash to the published digest: refused, deleted
    BadPayload,      // right hash but not what the kind says (not an ELF, unsafe archive paths)
    ExtractFailed,
    SwapFailed,
    SmokeFailed      // the new copy did not run; the old one has been put back
};
std::string outcomeName(Outcome o);

struct ApplyResult {
    Outcome outcome = Outcome::NothingToDo;
    std::string detail;
    bool restored = false;  // true when a failed swap/smoke put the old copy back
};

struct ApplyPaths {
    std::string target;  // the AppImage file, or the tree's root directory
    std::string newVersion;
};

// Suffixes of the files an update leaves beside the target. All are in target's own directory so
// that every rename is on one file system.
inline constexpr const char* kOldSuffix = ".old";
inline constexpr const char* kPendingSuffix = ".update-pending";
inline constexpr const char* kLockSuffix = ".update-lock";

// The partial download's name is derived from the digest, so a partial from another version can
// never be resumed into this one.
std::string partialPathFor(const std::string& target, const Asset& a);

// Applies an Install decision. Blocking: call it off the GUI thread.
//   1. refuse when the directory is not writable or space is short (2x the asset);
//   2. take the lock (exclusive create); a leftover lock means Busy;
//   3. download - resuming a partial that fits and was begun for this same digest, discarding
//      one that is larger than the asset; ONE retry from zero when a resumed file fails its hash;
//   4. verify the sha256: a mismatch deletes the file and is HashMismatch - nothing is installed;
//   5. check the payload (an AppImage must be an ELF file; a tarball's names must all be relative,
//      free of "..", inside one top-level directory), extract a tarball into a staging directory;
//   6. swap: target -> target.old, staged -> target (two renames, rolled back if the second fails);
//   7. write the pending marker, run the new copy once (`--version`, exit 0, prints the version);
//      if that fails, restore the old copy and report SmokeFailed;
//   8. release the lock. The .old copy and the marker stay until confirmStartup().
ApplyResult applyUpdate(UpdateHost& host, const Decision& d, const ApplyPaths& paths,
                        std::atomic<bool>* cancel = nullptr);

// Called by the program early in EVERY start. Returns a short note when it changed something, "" when
// there was nothing to do.
//   - target missing but target.old present (an interrupted swap): the old copy is put back;
//   - marker present and `runningVersion` equals the version it names: the new copy has now started
//     for real, so the .old copy and the marker are deleted;
//   - marker present and the running version is anything else: stale, the marker is removed and
//     the .old copy kept for the user.
std::string confirmStartup(UpdateHost& host, const std::string& target,
                           const std::string& runningVersion);

// What the program's start calls (engine wiring W2): confirmStartup for an install this program replaces by itself - an AppImage file or an
// unpacked tree - and nothing at all for any other kind (a package manager's, Flatpak's, Snap's, a container's, an unknown one) or an empty
// target: those are never looked at, so no marker or ".old" beside them is ever acted on. Returns what confirmStartup returns.
std::string confirmStartupFor(UpdateHost& host, InstallKind kind, const std::string& target,
                              const std::string& runningVersion);

// The names in a tarball listing are safe to extract: non-empty, relative, no ".." component, no
// backslash or NUL, and all inside ONE top-level directory.
bool archiveListingIsSafe(const std::vector<std::string>& names, std::string& error);

}  // namespace cascade::core::linuxupdate

#endif  // CASCADE_CORE_LINUX_UPDATE_HPP
