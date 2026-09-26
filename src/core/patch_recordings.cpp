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
bool hasWavExtension(const fs::path& p) {
    std::string ext = utf8(p.extension());
    for (char& c : ext) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return ext == ".wav";
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

std::vector<RecordingInfo> listIqRecordings(const std::vector<std::string>& dirs,
                                            std::size_t maxListed) {
    std::vector<RecordingInfo> out;
    std::set<std::string> seen;   // iqFileIdentity of every file listed
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
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec) { continue; }
            const fs::path& p = it->path();
            if (!hasWavExtension(p)) { continue; }
            std::string path;
            if (!narrowPath(p, path)) { continue; }
            if (!seen.insert(iqFileIdentity(path)).second) { continue; }
            // WHAT WILL PLAY: the header read by the very class that will
            // play it, so a file listed here opens when the patch starts.
            cascade::source::IqFileSource probe;
            if (!probe.open(path)) { continue; }
            RecordingInfo r;
            r.path = path;
            r.fileName = utf8(p.filename());
            r.key = makeIqFileKey(path);
            r.rateHz = probe.sampleRateHz();
            out.push_back(std::move(r));
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
