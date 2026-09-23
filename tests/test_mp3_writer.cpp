// Tests for core/mp3_writer.hpp - the patch page's MP3 output (0.99.17).
//
// A file that exists is not an MP3 that plays. So the test writes two seconds
// of a 1 kHz tone, then DECODES the file with Windows' own MP3 decoder (a
// Media Foundation source reader) and checks what comes back: how long it is,
// and that the tone is still 1 kHz. A writer that produced silence, the wrong
// rate, or a truncated file fails one of those.
//
// On a build without the encoder (Linux) the test checks the other half of
// the contract instead: available() is false and open() refuses with a
// sentence, rather than creating a file that is not an MP3.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/mp3_writer.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "test_check.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace {

// Decodes `path` to 16-bit mono PCM through Media Foundation. Returns false
// if the file cannot be opened or decoded at all.
bool decodeMp3(const std::string& path, std::vector<std::int16_t>& pcm, unsigned& rate) {
    pcm.clear();
    rate = 0;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { return false; }
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    bool ok = false;
    IMFSourceReader* reader = nullptr;
    const std::wstring w = std::filesystem::path(path).wstring();
    if (SUCCEEDED(MFCreateSourceReaderFromURL(w.c_str(), nullptr, &reader))) {
        IMFMediaType* want = nullptr;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        const HRESULT h = reader->SetCurrentMediaType(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want);
        want->Release();
        IMFMediaType* got = nullptr;
        UINT32 channels = 0;
        if (SUCCEEDED(h) && SUCCEEDED(reader->GetCurrentMediaType(
                                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got))) {
            UINT32 r = 0;
            got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
            got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            rate = r;
            got->Release();
            ok = channels >= 1;
            for (;;) {
                DWORD flags = 0;
                IMFSample* sample = nullptr;
                if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                                              0, nullptr, &flags, nullptr, &sample))) {
                    ok = false;
                    break;
                }
                if (sample != nullptr) {
                    IMFMediaBuffer* buf = nullptr;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                        BYTE* data = nullptr;
                        DWORD len = 0;
                        if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                            const auto* s = reinterpret_cast<const std::int16_t*>(data);
                            const std::size_t n = len / sizeof(std::int16_t);
                            for (std::size_t i = 0; i < n; i += channels) { pcm.push_back(s[i]); }
                            buf->Unlock();
                        }
                        buf->Release();
                    }
                    sample->Release();
                }
                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) { break; }
            }
        }
        reader->Release();
    }
    MFShutdown();
    CoUninitialize();
    return ok;
}

}  // namespace
#endif

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
                         ("foxsdr_test_mp3_" + std::to_string(static_cast<unsigned long>(
#ifdef _WIN32
                                                   GetCurrentProcessId()
#else
                                                   0
#endif
                                                       )));
    fs::create_directories(dir);
    const std::string path = (dir / "tone.mp3").string();

    cascade::core::Mp3Writer w;
    std::string err;
#ifdef _WIN32
    CHECK(cascade::core::Mp3Writer::available());
    // [1] Two seconds of 1 kHz at half scale, 48 kHz mono, in 20 ms blocks as
    // the patch's file worker hands it over.
    constexpr unsigned kRate = 48000;
    const bool opened = w.open(path, kRate, 1, 128, err);
    if (!opened) { std::printf("open failed: %s\n", err.c_str()); }
    CHECK(opened);
    CHECK(err.empty());
    std::vector<std::int16_t> block(960);
    std::uint64_t n = 0;
    for (int b = 0; b < 100; ++b) {
        for (auto& s : block) {
            s = static_cast<std::int16_t>(
                16000.0 * std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(n++) / kRate));
        }
        CHECK(w.write(block.data(), block.size()));
    }
    CHECK(w.framesWritten() == 96000u);
    w.close();
    CHECK(!w.isOpen());
    CHECK(fs::exists(path));
    CHECK(fs::file_size(path) > 10000u);    // ~32 KB at 128 kbit/s for two seconds

    // [2] It decodes, at the rate it was written, to about two seconds of the
    // same tone (an MP3 adds encoder delay and padding at the ends, so the
    // length is checked to within a tenth of a second, not exactly).
    std::vector<std::int16_t> pcm;
    unsigned rate = 0;
    CHECK(decodeMp3(path, pcm, rate));
    CHECK(rate == kRate);
    std::printf("decoded %zu samples at %u Hz\n", pcm.size(), rate);
    CHECK(pcm.size() > static_cast<std::size_t>(kRate) * 19 / 10);
    CHECK(pcm.size() < static_cast<std::size_t>(kRate) * 21 / 10);
    // The pitch, by rising zero crossings over the middle second.
    if (pcm.size() > static_cast<std::size_t>(kRate) * 3 / 2) {
        int crossings = 0;
        double peak = 0.0;
        for (std::size_t i = kRate / 2 + 1; i < kRate * 3 / 2; ++i) {
            if (pcm[i - 1] < 0 && pcm[i] >= 0) { ++crossings; }
            peak = std::max(peak, std::fabs(static_cast<double>(pcm[i])));
        }
        std::printf("crossings in one second: %d, peak %.0f\n", crossings, peak);
        CHECK(crossings >= 995 && crossings <= 1005);
        CHECK(peak > 12000.0 && peak < 20000.0);   // not silence, not clipped
    }

    // [3] Writing to a directory that cannot exist fails with a reason and
    // leaves the writer closed.
    cascade::core::Mp3Writer bad;
    CHECK(!bad.open((dir / "no" / "such" / "dir" / "x.mp3").string(), kRate, 1, 128, err));
    CHECK(!err.empty());
    CHECK(!bad.isOpen());
    CHECK(!bad.write(block.data(), block.size()));

    // [4] close() twice is harmless; write after close is refused.
    w.close();
    CHECK(!w.write(block.data(), block.size()));
#else
    CHECK(!cascade::core::Mp3Writer::available());
    CHECK(!w.open(path, 48000, 1, 128, err));
    CHECK(err.find("WAV") != std::string::npos);
    CHECK(!fs::exists(path));
#endif
    std::error_code ec;
    fs::remove_all(dir, ec);
    return testSummary("test_mp3_writer");
}
