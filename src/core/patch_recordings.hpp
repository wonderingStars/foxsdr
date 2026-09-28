// patch_recordings.hpp - I/Q recordings as patch radios (0.99.40): finding
// them for the Radio node's device list, and opening one.
//
// A recording is a device key like a radio's ("iqfile|path=...", see
// core/patch_devices.hpp), played by source/iq_file_source.hpp - a 2-channel
// RIFF/WAVE file, PCM16 or float32, looped for ever with no seam - and paced
// to real time by the patch radio's reader exactly as the signal generator is
// (IqFileSource is free-running: selfPaced() is false).
//
// THE LIST IS WHAT WILL PLAY. Every .wav in the folders asked is opened far
// enough to read its header, and only those IqFileSource accepts are listed,
// with their rate: a mono speaker recording in the same folder is not offered
// as a radio only to fail when the patch starts. Header reads are bounded
// (IqFileSource::kMaxHeaderChunks) and the list is capped, because it is read
// on the GUI thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "source/iq_file_source.hpp"

namespace cascade::core::patch {

struct RecordingInfo {
    std::string path;       // as found: folder + file name
    std::string fileName;   // the file name alone, for the list
    std::string key;        // the Radio node's device key for it
    double rateHz = 0.0;    // from its header
};

// The most recordings one list holds - far more than anyone keeps in a
// recordings folder, few enough that listing them never stalls a frame.
inline constexpr std::size_t kMaxRecordingsListed = 200;

// The most files ONE listing opens to read a header. The listing runs on the
// GUI thread, and the folder it reads also collects every WAV the patch's own
// speakers write; a file already read is remembered (RecordingProbeCache), so
// a folder bigger than this is read over the next few openings of the list.
inline constexpr std::size_t kMaxRecordingOpens = 64;

// What a listing learned of each file, so the next one need not open it
// again: its size and time when read, and whether it plays and at what rate.
// Keyed by iqFileIdentity(path). `opens` counts every header read made
// through it - how a test sees the cost.
struct RecordingProbeCache {
    struct Entry {
        std::uintmax_t size = 0;
        long long mtime = 0;
        bool playable = false;
        double rateHz = 0.0;
    };
    std::map<std::string, Entry> entries;
    std::size_t opens = 0;
};

// Whether `fileName` is one the patch writes itself - a speaker's recording,
// "patch-<node>-<name>_<timestamp>.wav" (core::patchFilePrefix and the stamp
// makeWavDest adds). Those are sound, never I/Q, and are passed over by name.
bool isPatchOutputFile(const std::string& fileName);

// Every playable recording directly in each of `dirs` (not in folders below
// them), sorted by file name and then by folder; a file in two of the folders
// asked (or one asked twice) is listed once. Empty and missing folders are
// skipped. At most `maxListed`, and at most `maxOpens` files opened to read a
// header - with a `cache`, only files it has not seen at their present size
// and time count. A file whose name cannot be converted is skipped: nothing
// here throws.
std::vector<RecordingInfo> listIqRecordings(const std::vector<std::string>& dirs,
                                            std::size_t maxListed = kMaxRecordingsListed,
                                            RecordingProbeCache* cache = nullptr,
                                            std::size_t maxOpens = kMaxRecordingOpens);

// Opens the recording `key` names for a patch radio: its header read, and
// `radioHz` given it as its centre - the node's frequency (the air frequency
// the recording is baseband around) ALREADY THROUGH the I/Q file converter,
// core::radioFromAir, as every figure told to a radio is; with no converter
// set the two are the same number. Nothing else: its rate is its own. nullptr
// with the reason in `error` when the key is not a recording or the file will
// not open (IqFileSource's own sentence).
std::unique_ptr<cascade::source::IqFileSource> openIqRecording(const std::string& key,
                                                               double radioHz,
                                                               std::string& error);

}  // namespace cascade::core::patch
