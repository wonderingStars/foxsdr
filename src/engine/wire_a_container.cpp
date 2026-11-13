// wire_a_container.cpp - WIRING LANE wire-a, recording: FLAC for audio takes (RECORD_CONTAINER; fact.record.container). The op is documented in core/wire_a_ops.hpp.
//
// HOW. The recorder writes the take as a WAV exactly as it always did (nothing in the recorder or the pipeline changes); when an AUDIO take ends, the lane converts the finished WAV to FLAC on its job worker
// (core/flac_writer.hpp: lossless, 16- or 24-bit) and, only once the FLAC was written whole, removes the WAV. The pass notices the end of a take by the radio's audio recorder going from recording to not; the take's
// path is read while it records. The WAV is finished on the recorder's own worker, so the job waits (up to ten seconds) until the file's header and size agree before it reads it. A conversion that finds the job
// slot busy waits in a queue and is retried on the next pass. The setting is kept in record-container.json beside markers.json.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "core/flac_writer.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"
#include "source/wav_meta.hpp"

namespace cascade::engine {

namespace fl = cascade::core::flac;
namespace wm = cascade::source::wavmeta;
using nlohmann::json;

void WireA::containerLoadOnce() {
    if (containerLoaded_ || dir().empty()) { return; }
    containerLoaded_ = true;
    const std::string text = readFile("record-container.json");
    if (text.empty()) { return; }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return; }
    container_ = j.value("flac", false) ? 1 : 0;
    containerKeep_ = j.value("keep", false);
    bump();
}

WireA::Result WireA::containerOp(const FoxCommand& c, Result res) {
    containerLoadOnce();
    if ((c.ival[0] != 0 && c.ival[0] != 1) || (c.ival[1] != 0 && c.ival[1] != 1)) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 WAV or 1 FLAC, ival[1] 0 or 1"); }
    if (container_ == c.ival[0] && containerKeep_ == (c.ival[1] == 1)) { return refused(res, FOXAPI_NO_CHANGE, "it is so already"); }
    container_ = static_cast<int>(c.ival[0]);
    containerKeep_ = c.ival[1] == 1;
    saveFile("record-container.json", json{{"flac", container_ == 1}, {"keep", containerKeep_}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    bump();
    return res;
}

namespace {

// Waits until the WAV is finished (the RIFF size matches the file), then converts it. Never reads or writes outside the two files; the messages carry no path.
WireA::JobResult convert(const std::string& wavPath, bool keepWav, WireA::JobCtl& ctl) {
    WireA::JobResult r;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path wav(wavPath);
    wm::WavMeta meta;
    for (int i = 0; i < 100; ++i) {
        if (ctl.cancel.load()) { r.message = "cancelled"; return r; }
        if (fs::is_regular_file(wav, ec)) {
            meta = wm::readWavMeta(wav);
            const std::uintmax_t size = fs::file_size(wav, ec);
            if (meta.ok && meta.hasData && meta.dataBytes > 0 && meta.dataOffset + meta.dataBytes <= size && meta.container == "RIFF") {
                // finished: the data chunk ends where the file does (a take still being written has a zero-length header)
                break;
            }
        }
        meta = wm::WavMeta();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!meta.ok || !meta.hasData || meta.dataBytes == 0) { r.message = "the take's WAV was not finished or is empty"; return r; }
    const unsigned ch = meta.channels, bits = meta.bitsPerSample, tag = meta.formatTag;
    const bool pcm = tag == 1 && (bits == 16 || bits == 24);
    const bool flt = tag == 3 && bits == 32;
    if ((ch != 1 && ch != 2) || !(pcm || flt)) { r.message = "this WAV is not 16 or 24-bit integer or 32-bit float audio"; return r; }
    const int outBits = (pcm && bits == 16) ? 16 : 24;
    fs::path flacPath = wav;
    flacPath.replace_extension(".flac");
    if (fs::exists(flacPath, ec)) { r.message = "a FLAC with that name exists already"; return r; }
    fl::Options o;
    o.sampleRate = meta.sampleRateHz;
    o.channels = static_cast<int>(ch);
    o.bitsPerSample = outBits;
    fl::Writer w;
    std::string err;
    if (!w.open(flacPath.string(), o, &err)) { r.message = "the FLAC could not be started"; return r; }
    std::ifstream in(wav, std::ios::binary);
    if (!in) { r.message = "the WAV could not be read"; w.finish(); fs::remove(flacPath, ec); return r; }
    in.seekg(static_cast<std::streamoff>(meta.dataOffset));
    const std::size_t bytesPerSample = bits / 8, frameBytes = bytesPerSample * ch;
    const std::size_t blockFrames = 65536;
    std::vector<unsigned char> raw(blockFrames * frameBytes);
    std::vector<std::int32_t> pcm32(blockFrames * ch);
    std::uint64_t left = meta.dataBytes / frameBytes, frames = 0;
    bool ok = true;
    while (left > 0 && ok) {
        if (ctl.cancel.load()) { ok = false; r.message = "cancelled"; break; }
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, blockFrames));
        in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(n * frameBytes));
        if (static_cast<std::size_t>(in.gcount()) != n * frameBytes) { ok = false; r.message = "the WAV could not be read to its end"; break; }
        for (std::size_t i = 0; i < n * ch; ++i) {
            const unsigned char* p = raw.data() + i * bytesPerSample;
            std::int32_t v = 0;
            if (flt) {
                float f;
                std::memcpy(&f, p, 4);
                const double s = std::isfinite(f) ? std::max(-1.0, std::min(1.0, static_cast<double>(f))) : 0.0;
                v = static_cast<std::int32_t>(std::lround(s * 8388607.0));
            } else if (bits == 16) {
                v = static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0] | (p[1] << 8)));
            } else {
                v = static_cast<std::int32_t>((p[0] << 8) | (p[1] << 16) | (static_cast<std::uint32_t>(p[2]) << 24)) >> 8;
            }
            pcm32[i] = v;
        }
        if (!w.write(pcm32.data(), n, &err)) { ok = false; r.message = "the FLAC could not be written"; break; }
        left -= n;
        frames += n;
        ctl.progress.store(static_cast<double>(frames) / static_cast<double>(meta.dataBytes / frameBytes));
    }
    in.close();
    if (!w.finish(&err) && ok) { ok = false; r.message = "the FLAC could not be finished"; }
    if (!ok) { fs::remove(flacPath, ec); return r; }   // the WAV stays
    const std::uintmax_t wavBytes = fs::file_size(wav, ec), flacBytes = fs::file_size(flacPath, ec);
    if (!keepWav) { fs::remove(wav, ec); }
    r.ok = true;
    r.message = flt ? "float take written as 24-bit" : "converted";
    r.json = json{{"frames", frames}, {"bits", outBits}, {"wavBytes", wavBytes}, {"flacBytes", flacBytes}, {"wavKept", keepWav}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return r;
}

}  // namespace

// ---- the container of I/Q takes (second pass) ----------------------------------------------------------------------------------------------

WireA::Result WireA::iqContainerOp(const FoxCommand& c, Result res) {
    iqContainerPass();   // (loads the saved setting first)
    if (c.ival[0] < 0 || c.ival[0] > 2) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 WAV, 1 RF64 or 2 SigMF"); }
    if (iqContainer_ == c.ival[0]) { return refused(res, FOXAPI_NO_CHANGE, "it is so already"); }
    iqContainer_ = static_cast<int>(c.ival[0]);
    saveFile("record-iq-container.json", json{{"container", iqContainer_}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n");
    iqContainerPass();
    bump();
    return res;
}

// Every pass: the saved setting is read once, then every radio's recorder is given the container for its next take and the radio's centre frequency (a SigMF take logs a change of it).
void WireA::iqContainerPass() {
    if (!iqContainerLoaded_ && !dir().empty()) {
        iqContainerLoaded_ = true;
        const json j = json::parse(readFile("record-iq-container.json"), nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            const int v = j.value("container", 0);
            if (v >= 0 && v <= 2) { iqContainer_ = v; bump(); }
        }
    }
    for (std::size_t slot = 0; slot < 8; ++slot) {
        if (!e_.receivers_[slot]) { continue; }
        Receiver& rx = *e_.receivers_[slot];
        (void)rx.iqRecorder_.setIqContainer(iqContainer_);
        rx.iqRecorder_.noteCentre(rx.pipeline_.activeSource().centerFrequencyHz());
    }
}

void WireA::containerPass() {
    containerLoadOnce();
    for (std::size_t slot = 0; slot < takePath_.size(); ++slot) {
        if (!e_.receivers_[slot]) { takePath_[slot].clear(); continue; }
        Receiver& rx = *e_.receivers_[slot];
        if (rx.audioRecorder_.recording()) {
            takePath_[slot] = rx.audioRecorder_.path();
        } else if (!takePath_[slot].empty()) {
            if (container_ == 1 && convertQueue_.size() < 16) { convertQueue_.push_back(takePath_[slot]); }
            takePath_[slot].clear();
        }
    }
    if (!convertQueue_.empty() && !job_.pending()) {
        const std::string path = convertQueue_.front();
        convertQueue_.erase(convertQueue_.begin());
        const bool keep = containerKeep_;
        FoxCommandResult dummy{};
        const Result r = startJob(dummy, "FLAC conversion", [path, keep](JobCtl& ctl) -> JobResult { return convert(path, keep, ctl); });
        if (r.status != FOXAPI_OK) { convertQueue_.insert(convertQueue_.begin(), path); }
    }
}

}  // namespace cascade::engine
