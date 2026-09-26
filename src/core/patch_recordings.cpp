// patch_recordings.cpp - see patch_recordings.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_recordings.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <system_error>

#include "core/patch_devices.hpp"

namespace cascade::core::patch {

namespace {

namespace fs = std::filesystem;

// As UTF-8, which never fails: what the list SHOWS (ImGui draws UTF-8).
std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ".wav" in any case: recorders on Windows write ".WAV" as often as ".wav".
//
// COMPARED ON THE PATH'S OWN CHARACTERS, never converted. On Windows those are
// UTF-16 units, and NTFS allows any sequence of them - an unpaired surrogate
// included - which MSVC's u8string() answers by THROWING. The first cut
// converted the extension of every file in the folder, and one such name
// closed the application from a combo on the GUI thread (review of f7d1cfc).
// ASCII folding only: ".wav" is four ASCII characters, so nothing else can
// match it.
bool hasWavExtension(const fs::path& p) {
    // Held by value: native() is a reference INTO the path extension() returns.
    const fs::path extPath = p.extension();
    const auto& ext = extPath.native();
    static constexpr char kWav[] = ".wav";
    if (ext.size() != 4) { return false; }
    for (std::size_t i = 0; i < 4; ++i) {
        auto c = ext[i];
        if (c >= 'A' && c <= 'Z') { c = static_cast<decltype(c)>(c - 'A' + 'a'); }
        if (c != static_cast<decltype(c)>(kWav[i])) { return false; }
    }
    return true;
}

// The time a file was last written, as a plain number for the cache.
long long writeStamp(const fs::directory_entry& e) {
    std::error_code ec;
    const auto t = e.last_write_time(ec);
    return ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
}

// THE PATH AS IqFileSource WILL OPEN IT: a narrow string, which on Windows is
// the ANSI code page (IqFileSource hands it to std::ifstream, and the folders
// come from the environment in the same encoding). A name that code page
// cannot spell cannot be opened by it either, so it is skipped, not listed.
bool narrowPath(const fs::path& p, std::string& out) {
    try {
        out = p.string();
        return true;
    } catch (...) {
        return false;
    }
}

// ORDERED BY FILE NAME as a person reads it: letters compared without their
// case, so "C_TONE.WAV" sits between "b_..." and "d_...", not before "a_...".
bool nameBefore(const RecordingInfo& a, const RecordingInfo& b) {
    const auto lower = [](std::string s) {
        for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return s;
    };
    const std::string la = lower(a.fileName);
    const std::string lb = lower(b.fileName);
    if (la != lb) { return la < lb; }
    return a.path < b.path;
}

}  // namespace

bool isPatchOutputFile(const std::string& fileName) {
    // "patch-" (any case - Windows names are), then the node's number, then
    // "-": the shape core::patchFilePrefix gives every speaker's file.
    static constexpr char kPrefix[] = "patch-";
    constexpr std::size_t n = sizeof(kPrefix) - 1;
    if (fileName.size() <= n) { return false; }
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(fileName[i])) != kPrefix[i]) { return false; }
    }
    std::size_t i = n;
    while (i < fileName.size() && std::isdigit(static_cast<unsigned char>(fileName[i])) != 0) { ++i; }
    return i > n && i < fileName.size() && fileName[i] == '-';
}

std::vector<RecordingInfo> listIqRecordings(const std::vector<std::string>& dirs,
                                            std::size_t maxListed, RecordingProbeCache* cache,
                                            std::size_t maxOpens) {
    std::vector<RecordingInfo> out;
    std::set<std::string> seen;   // iqFileIdentity of every file listed
    std::size_t opened = 0;       // header reads THIS listing made
    for (const std::string& dir : dirs) {
        if (dir.empty()) { continue; }
        std::error_code ec;
        const fs::path folder(dir);
        if (!fs::is_directory(folder, ec)) { continue; }
        // Every entry is checked with an error_code: a folder that vanishes
        // or a file that cannot be read is skipped, never thrown out of a
        // frame.
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (out.size() >= maxListed) { break; }
            // AND NOTHING ELSE MAY THROW OUT OF IT EITHER: a name conversion
            // that fails (see hasWavExtension) skips that one file.
            try {
                std::error_code fec;
                if (!it->is_regular_file(fec) || fec) { continue; }
                const fs::path& p = it->path();
                if (!hasWavExtension(p)) { continue; }
                std::string path;
                if (!narrowPath(p, path)) { continue; }
                const std::string fileName = utf8(p.filename());
                // The patch's own speaker recordings: sound, not I/Q, and they
                // pile up in this very folder - passed over unopened.
                if (isPatchOutputFile(fileName)) { continue; }
                const std::string id = iqFileIdentity(path);
                if (seen.count(id) != 0) { continue; }
                // WHAT WILL PLAY: the header read by the very class that will
                // play it, so a file listed here opens when the patch starts -
                // or what a listing before this one learned, while the file is
                // still the size and age it was then.
                const std::uintmax_t size = it->file_size(fec);
                const long long stamp = writeStamp(*it);
                RecordingProbeCache::Entry entry;
                bool known = false;
                if (cache != nullptr) {
                    const auto c = cache->entries.find(id);
                    known = c != cache->entries.end() && c->second.size == size &&
                            c->second.mtime == stamp;
                    if (known) { entry = c->second; }
                }
                if (!known) {
                    if (opened >= maxOpens) { continue; }   // the next listing reads it
                    ++opened;
                    if (cache != nullptr) { ++cache->opens; }
                    cascade::source::IqFileSource probe;
                    entry.size = size;
                    entry.mtime = stamp;
                    entry.playable = probe.open(path);
                    entry.rateHz = entry.playable ? probe.sampleRateHz() : 0.0;
                    if (cache != nullptr) { cache->entries[id] = entry; }
                }
                if (!entry.playable) { continue; }
                seen.insert(id);
                RecordingInfo r;
                r.path = path;
                r.fileName = fileName;
                r.key = makeIqFileKey(path);
                r.rateHz = entry.rateHz;
                out.push_back(std::move(r));
            } catch (...) {
                continue;
            }
        }
    }
    std::sort(out.begin(), out.end(), nameBefore);
    return out;
}

std::unique_ptr<cascade::source::IqFileSource> openIqRecording(const std::string& key,
                                                               double radioHz,
                                                               std::string& error) {
    error.clear();
    if (!isIqFileKey(key)) {
        error = "not a recording";
        return nullptr;
    }
    auto src = std::make_unique<cascade::source::IqFileSource>();
    if (!src->open(iqFilePath(key))) {
        const char* why = src->lastError();
        error = (why != nullptr && *why != '\0') ? why : "the recording would not open";
        return nullptr;
    }
    // NOMINAL: a file has no tuner. The node's frequency says which air
    // frequency the recording is baseband around, and every channel and
    // decoder is measured against it. It arrives here ALREADY CONVERTED - the
    // caller's radioFromAir() through the I/Q file converter - and the patch
    // radio's converter view reads it back as air (test_converter_call_sites).
    src->setCenterFrequencyHz(radioHz);
    return src;
}

}  // namespace cascade::core::patch
