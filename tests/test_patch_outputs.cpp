// Tests for the patch's device keys and sound outputs (0.99.17):
// core/patch_devices.hpp and core/patch_audio.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_audio.hpp"
#include "core/patch_devices.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/mp3_writer.hpp"
#include "test_check.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace cascade::core::patch;

namespace {

unsigned long pid() {
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::vector<std::filesystem::path> filesIn(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) { out.push_back(e.path()); }
    return out;
}

}  // namespace

int main() {
    // [K1] Device keys: the generator is never "the same device" as anything;
    // hardware is the same by exact key, or by serial across drivers.
    {
        CHECK(isGeneratorKey("siggen"));
        CHECK(!isGeneratorKey("rtlsdr|serial=1"));
        CHECK(deviceDriver("rtlsdr|serial=00000001") == "rtlsdr");
        CHECK(deviceArgs("rtlsdr|serial=00000001") == "serial=00000001");
        CHECK(deviceDriver("siggen").empty());
        CHECK(makeDeviceKey("hackrf", "serial=abc") == "hackrf|serial=abc");
        CHECK(!sameDevice("siggen", "siggen"));
        CHECK(!sameDevice("", ""));
        CHECK(!sameDevice("", "rtlsdr|serial=1"));
        CHECK(sameDevice("rtlsdr|serial=1", "rtlsdr|serial=1"));
        CHECK(!sameDevice("rtlsdr|serial=1", "rtlsdr|serial=2"));
        CHECK(sameDevice("rtlsdr|serial=00000001", "soapy|driver=rtlsdr, SERIAL = 00000001"));
        CHECK(!sameDevice("rtlsdr|index=0", "rtlsdr|index=1"));   // no serial: key decides
        // A DONGLE WITH NO SERIAL, listed twice (through 0.99.34 these were
        // "different radios"): natively by index, and through SoapySDR by
        // the module that drives the same hardware. Neither key can prove
        // they differ, so they are one radio - the native rows already cover
        // every dongle of that family, so refusing the pair costs nothing.
        CHECK(sameDevice("rtlsdr|index=0", "soapy|driver=rtlsdr,label=Generic RTL2832U"));
        CHECK(sameDevice("soapy|driver=rtlsdr,serial=", "rtlsdr|index=0"));
        CHECK(sameDevice("rtlsdr|serial=00000001", "soapy|driver=RTLSDR"));  // soapy side blank
        CHECK(sameDevice("mirisdr|index=0", "soapy|driver=miri"));           // SoapyMiri's name
        CHECK(sameDevice("rx888|index=0", "soapy|driver=sddc"));             // SoapySDDC's name
        // ...but only within one family, and serials still decide when both
        // sides have one.
        CHECK(!sameDevice("rtlsdr|index=0", "soapy|driver=hackrf"));
        CHECK(!sameDevice("rtlsdr|index=0", "soapy|driver=uhd,type=b200"));
        CHECK(!sameDevice("rtlsdr|serial=00000001", "soapy|driver=rtlsdr,serial=00000002"));
        // Two SoapySDR rows of one driver come from one scan, which lists
        // each device once: they are two radios, as before.
        CHECK(!sameDevice("soapy|driver=rtlsdr,label=a", "soapy|driver=rtlsdr,label=b"));
        CHECK(argField("a=1,serial=xyz,b=2", "SERIAL") == "xyz");
        CHECK(argField("a=1", "serial").empty());
    }

    // [K2] Output keys: anything unrecognised - including nothing - is a WAV
    // file, which is the owner's default.
    {
        CHECK(outputKind("") == OutputKind::Wav);
        CHECK(outputKind("wav") == OutputKind::Wav);
        CHECK(outputKind("mp3") == OutputKind::Mp3);
        CHECK(outputKind("speakers") == OutputKind::Speakers);
        CHECK(outputKind("audio:Headphones (Arctis)") == OutputKind::Device);
        CHECK(outputDeviceName("audio:Headphones (Arctis)") == "Headphones (Arctis)");
        CHECK(outputKind("audio:") == OutputKind::Wav);          // a device with no name
        CHECK(outputKind("nonsense") == OutputKind::Wav);
        CHECK(makeOutputDeviceKey("X") == "audio:X");
    }

    // [N1] File names: safe characters only, bounded, never empty, and the
    // node id keeps two speakers with the same name apart.
    {
        CHECK(fileSafeName("Airband 1") == "Airband_1");
        CHECK(fileSafeName("a/b\\c:d*e?") == "a_b_c_d_e_");
        CHECK(fileSafeName("") == "speaker");
        CHECK(fileSafeName(std::string(200, 'x')).size() == 40u);
        CHECK(patchFilePrefix(7, "Air band") == "patch-7-Air_band");
        CHECK(patchFilePrefix(7, "S") != patchFilePrefix(8, "S"));
    }

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("foxsdr_test_patch_audio_" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(dir, ec);

    // [W1] A WAV speaker: one second at 48 kHz lands as a 16-bit mono file of
    // exactly that length, named after the node, and the face's counters see it.
    {
        std::string err;
        std::shared_ptr<AudioDest> d = makeWavDest(dir.string(), patchFilePrefix(3, "Air"), err);
        CHECK(d != nullptr);
        CHECK(err.empty());
        if (d) {
            std::vector<float> s(960);
            for (int b = 0; b < 50; ++b) {
                for (std::size_t i = 0; i < s.size(); ++i) {
                    s[i] = 0.5f * static_cast<float>(std::sin(0.13 * static_cast<double>(i)));
                }
                d->write(s.data(), s.size());
            }
            CHECK(d->samples() == 48000u);
            CHECK(d->peak() > 0.45f && d->peak() <= 0.5f);
            CHECK(d->error().empty());
            CHECK(d->describe().rfind("WAV  patch-3-Air_", 0) == 0);
            d.reset();   // closes and finalises the file
        }
        const std::vector<fs::path> files = filesIn(dir);
        CHECK(files.size() == 1u);
        if (files.size() == 1u) {
            CHECK(files[0].filename().string().rfind("patch-3-Air_", 0) == 0);
            CHECK(files[0].extension() == ".wav");
            CHECK(fs::file_size(files[0]) == 44u + 96000u);
            // The header says 48 kHz, mono, 16-bit.
            std::ifstream f(files[0], std::ios::binary);
            unsigned char h[44] = {};
            f.read(reinterpret_cast<char*>(h), 44);
            const unsigned rate = h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24);
            CHECK(rate == 48000u);
            CHECK(h[22] == 1 && h[23] == 0);      // one channel
            CHECK(h[34] == 16);                   // bits per sample
        }
        fs::remove_all(dir, ec);
    }

    // [W2] A WAV into a path that cannot be a folder is refused with a reason.
    {
        fs::create_directories(dir);
        {
            std::ofstream blocker(dir / "file");
            blocker << "x";
        }
        std::string err;
        std::shared_ptr<AudioDest> d = makeWavDest((dir / "file" / "sub").string(), "p", err);
        CHECK(d == nullptr);
        CHECK(!err.empty());
        fs::remove_all(dir, ec);
    }

    // [M1] An MP3 speaker writes an .mp3 that grows while sound is fed, on its
    // own thread, and is finalised when the speaker goes. On a build without
    // the encoder it is refused with a sentence naming WAV instead.
    {
        std::string err;
        std::shared_ptr<AudioDest> d = makeMp3Dest(dir.string(), patchFilePrefix(5, "Two m"), err);
        if (cascade::core::Mp3Writer::available()) {
            CHECK(d != nullptr);
            if (d) {
                std::vector<float> s(4800);
                for (int b = 0; b < 20; ++b) {   // two seconds
                    for (std::size_t i = 0; i < s.size(); ++i) {
                        s[i] = 0.4f * static_cast<float>(std::sin(0.2 * static_cast<double>(i)));
                    }
                    d->write(s.data(), s.size());
                }
                CHECK(d->samples() == 96000u);
                CHECK(d->describe().rfind("MP3  patch-5-Two_m_", 0) == 0);
                d.reset();
                CHECK(true);
            }
            const std::vector<fs::path> files = filesIn(dir);
            CHECK(files.size() == 1u);
            if (files.size() == 1u) {
                CHECK(files[0].extension() == ".mp3");
                CHECK(fs::file_size(files[0]) > 10000u);
            }
        } else {
            CHECK(d == nullptr);
            CHECK(err.find("WAV") != std::string::npos);
        }
        fs::remove_all(dir, ec);
    }

    return testSummary("test_patch_outputs");
}
