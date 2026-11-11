// web_clip.hpp - the request model for "record a clip in the browser and download it" (feat/webmodels).
//
// WHAT THIS IS. The decision layer between the page's Record button and the host's clip recorder: it decides whether a viewer may start a
// clip, what the file will be called and where, and whether a download request names a clip that viewer made. It records NOTHING itself: the
// recording is the core clip recorder's job (a hand-off in docs/webmodels-handoff.md says what the web server calls), and it serves nothing:
// the web server streams the file this model resolved.
//
// RULES (each tested in tests/test_web_clip.cpp):
//   1. LENGTH: a clip is kMinClipSeconds..kMaxClipSeconds long (non-finite or out of range is refused, never clamped: a viewer asking for an
//      hour is told no).
//   2. SIZE CAP: seconds x sample rate x bytes per sample must not exceed kMaxClipBytes. finished() also refuses a file the recorder wrote
//      past the cap (the clip is marked failed and cannot be downloaded).
//   3. ONE CLIP PER VIEWER AT A TIME, and at most kMaxClipsInFlight across all viewers; a viewer's finished clips are capped too
//      (kMaxClipsPerViewer, oldest forgotten first so the list never grows without bound).
//   4. PATH: the file is ALWAYS root / "<sanitised name>-<counter>.wav": the viewer's text only ever contributes characters from
//      [A-Za-z0-9_-], at most kMaxClipNameChars of them; separators, "..", drive letters, ":" streams, reserved Windows device names and
//      trailing dots can never appear. The counter makes a name unique, so a request can never overwrite another file. The final path is
//      re-checked to lie under the root (lexically, and again through the file system when the file exists, so a link planted in the root
//      cannot lead out of it) before it is handed out, for recording and for download.
//   5. OWNERSHIP: resolveDownload() answers only for a clip the SAME viewer token made and only once it finished OK.
//   6. Viewer tokens are opaque and live in memory only: this file prints nothing and writes nothing (source-scan test).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::net {

inline constexpr double kMinClipSeconds = 1.0;
inline constexpr double kMaxClipSeconds = 300.0;
inline constexpr std::uint64_t kMaxClipBytes = 64ull * 1024 * 1024;
inline constexpr std::size_t kMaxClipsInFlight = 4;
inline constexpr std::size_t kMaxClipsPerViewer = 8;
inline constexpr std::size_t kMaxClipViewers = 64;
inline constexpr std::size_t kMaxClipNameChars = 40;

enum class ClipResult {
    Ok,
    BadToken,
    BadRoot,         // the recordings root is empty or not absolute
    BadLength,
    TooBig,          // length x rate x width over kMaxClipBytes, or a bad rate/width
    ViewerBusy,      // this viewer already has a clip recording
    HostBusy,        // kMaxClipsInFlight reached
    TooManyViewers,
    NotFound,        // unknown ticket / clip id, or not this viewer's
    NotReady,        // the clip has not finished yet
    Failed,          // the clip finished badly (or past the cap)
    PathEscapes,     // the resolved path is not under the root
};

const char* clipResultText(ClipResult r);

struct ClipRequest {
    double seconds = 10.0;
    std::string name;            // free text from the page; sanitised, may be empty (then "clip")
    std::uint32_t sampleRateHz = 48000;
    std::uint32_t bytesPerSample = 2;  // per channel
    std::uint32_t channels = 1;
};

struct ClipPlan {
    std::uint64_t clipId = 0;           // names the clip in later calls (never reused)
    std::filesystem::path path;         // where the recorder writes it
    double seconds = 0.0;
    std::uint64_t estimatedBytes = 0;
};

struct ClipInfo {
    std::uint64_t clipId = 0;
    std::string fileName;               // the file name only (no directory)
    bool finished = false;
    bool ok = false;
    std::uint64_t bytes = 0;
};

// "My Clip 1!" -> "My_Clip_1_"; leading/trailing dots and underscores trimmed, runs of '_' collapsed, capped at kMaxClipNameChars,
// a reserved device name (CON PRN AUX NUL COM1-9 LPT1-9, any case) gets a trailing '_'; empty -> "clip".
std::string sanitiseClipName(const std::string& in);

// Lexical containment: `path` equals `root` or lies below it, after normalisation. Pure; no file system.
bool pathIsUnderRoot(const std::filesystem::path& root, const std::filesystem::path& path);

class WebClipBroker {
public:
    explicit WebClipBroker(std::filesystem::path recordingsRoot);

    ClipResult request(const std::string& viewerToken, const ClipRequest& req, ClipPlan& plan);
    // The recorder is done with a ticket: `bytes` written, `ok` whether it ended cleanly. Past kMaxClipBytes the clip is failed.
    ClipResult finished(const std::string& viewerToken, std::uint64_t clipId, std::uint64_t bytes, bool ok);
    // Gives up a clip that is still recording (the viewer left, the host stopped): frees the viewer's slot, marks it failed.
    void abandon(const std::string& viewerToken);

    ClipResult resolveDownload(const std::string& viewerToken, std::uint64_t clipId, std::filesystem::path& path) const;
    std::vector<ClipInfo> list(const std::string& viewerToken) const;
    // Drops a viewer's clips from the list (the files stay: the host's recordings are the user's).
    void forgetViewer(const std::string& viewerToken);

    std::size_t inFlight() const;

private:
    struct Clip {
        ClipInfo info;
        std::filesystem::path path;
    };
    struct Viewer {
        std::deque<Clip> clips;   // oldest first
    };
    mutable std::mutex mu_;
    std::filesystem::path root_;
    std::map<std::string, Viewer> viewers_;
    std::uint64_t nextId_ = 1;
};

}  // namespace cascade::net
