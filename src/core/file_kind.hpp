// core/file_kind.hpp - what a file somebody dropped on the window IS, decided by its content and nothing else in the file system but
// the sibling a pair needs. No window, no Slint, no ImGui.
//
// WHY THIS EXISTS (feature rows P17 and W2: "drop a file on the window"). A person drags something from Explorer: a recording, a list
// of frequencies, a plugin. The window must know what to do with it BEFORE it opens anything, and it must never guess from the
// extension alone (a renamed file is common, an extension that lies is common, and a .wav that is a mono voice recording must not be
// opened as an I/Q source). This file answers with one closed word per file plus one sentence of detail, from the first bytes of the
// file, using the readers the program already has:
//   - IqRecording : RIFF/WAVE, two channels, PCM 16-bit or IEEE float 32 - exactly what source/iq_file_source.hpp plays. Where the path
//                   is short plain ASCII the real IqFileSource::open() is asked as well and its refusal wins (it also wants a data chunk).
//   - Wav         : any other RIFF/WAVE (mono, 24-bit, ADPCM, a broken header): a sound file, not an I/Q recording.
//   - Rf64        : an RF64 file (a RIFF file that outgrew 4 GiB, EBU Tech 3306). IqFileSource does not play it; the word says so.
//   - SigMfPair   : a .sigmf-meta whose content is a SigMF metadata document (a JSON object with a "global" member that holds
//                   "core:datatype") with its .sigmf-data beside it - or the .sigmf-data of such a pair. One item per pair.
//   - SigMfMeta   : the same document with no data file beside it (reported, with the reason).
//   - BookmarkCsv / BookmarkXml : a frequency list core/freq_import.hpp reads (CSV with at least one usable frequency; SDR# XML).
//   - PluginPackage : a shared library core/plugin_module_check.hpp judges fit for THIS machine (a plugin module).
//   - Folder, Missing (it vanished between the drop and the look), Unreadable, Unknown.
//
// THE PRIVACY RULE (PRIVACY.md): the detail sentence never carries a path or a file name - the caller may log kind words and details.
//
// Every parser of outside bytes here is bounded (the head read is at most kHeadBytes, the chunk walk at most kMaxChunks, a text list
// at most kMaxListBytes, a module at core::kMaxModuleBytes) and never throws.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_FILE_KIND_HPP
#define CASCADE_CORE_FILE_KIND_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cascade::core {

enum class FileKind : std::uint8_t {
    Unknown = 0,
    Unreadable,
    Missing,
    Folder,
    IqRecording,
    Wav,
    Rf64,
    SigMfPair,
    SigMfMeta,
    BookmarkCsv,
    BookmarkXml,
    PluginPackage,
};
// "unknown", "unreadable", "missing", "folder", "iq-recording", "wav", "rf64", "sigmf-pair", "sigmf-meta", "bookmark-csv", "bookmark-xml",
// "plugin-package". Closed: these are the words a log or a fact may carry.
const char* fileKindWord(FileKind k);

inline constexpr std::size_t kHeadBytes = 64u * 1024u;          // how much of a file's start decides most kinds
inline constexpr std::size_t kMaxChunks = 256;                  // RIFF chunk headers one walk may read
inline constexpr std::uint64_t kMaxListBytes = 32ull * 1024ull * 1024ull;  // the largest frequency list read

struct FileClass {
    FileKind kind = FileKind::Unknown;
    std::string path;      // UTF-8, as dropped; for a SigMF pair the .sigmf-meta
    std::string dataPath;  // a SigMF pair's .sigmf-data
    std::string detail;    // one sentence, no path, no file name
    // Header figures when the kind is a sound file (0 when not read).
    std::uint32_t channels = 0;
    std::uint32_t sampleRateHz = 0;
    std::uint32_t bitsPerSample = 0;
    std::uint32_t formatTag = 0;
    std::uint64_t entries = 0;  // a frequency list's usable entries
};

// What a RIFF/WAVE or RF64 header says, read from the first `n` bytes of a file. Pure; fuzzed.
struct WavHeader {
    bool riff = false;       // "RIFF" ... "WAVE"
    bool rf64 = false;       // "RF64" ... "WAVE"
    bool haveFmt = false;
    bool haveData = false;
    std::uint32_t formatTag = 0;
    std::uint32_t channels = 0;
    std::uint32_t sampleRateHz = 0;
    std::uint32_t bitsPerSample = 0;
    std::string problem;     // set when the header is damaged
};
WavHeader parseWavHeader(const std::uint8_t* data, std::size_t n);

// Is `text` (the head of a file) a SigMF metadata document: a JSON object whose "global" member holds "core:datatype"? Not a JSON
// parser: it checks the structure the standard guarantees and is as tolerant as a person's hand-edited file needs.
bool looksLikeSigMfMeta(const std::uint8_t* data, std::size_t n);

// One path, UTF-8. Reads from disk. Never throws; a path of any length (over 259 characters is handled with the long-path prefix).
FileClass classifyFile(const std::string& utf8Path);

// The whole drop, in order: each path classified, and a SigMF meta and its data file dropped TOGETHER folded into one SigMfPair item
// (at the meta's position). A path given twice is classified once per occurrence. At most `maxFiles` are looked at; the rest are not
// classified and `*dropped` (when given) says how many.
std::vector<FileClass> classifyDrop(const std::vector<std::string>& utf8Paths, std::size_t maxFiles = 4096, std::size_t* dropped = nullptr);

// The file-system path a UTF-8 string names, correct for non-ASCII names, and (Windows) given the "\?\" long-path prefix when it is
// absolute and too long for the legacy limit. Exposed for the drop module's tests.
std::filesystem::path fsPathFromUtf8(const std::string& utf8);

}  // namespace cascade::core

#endif  // CASCADE_CORE_FILE_KIND_HPP
