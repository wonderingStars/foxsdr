// A recording that is cut short UNDER A RUNNING PIPELINE must reach
// Pipeline::faulted() - the FAIL lamp - and bring the pipeline down.
//
// IqFileSource already latches faulted() and zero-fills when its file is
// truncated or removed mid-play (tests/test_iq_file_source.cpp checks that
// against the source alone), and its header says the pipeline's source thread
// polls that flag. Only the SELF-PACED branch of the source thread did. A file
// source is free-running - the pipeline paces it - so the fault was latched
// and never read: the spectrum scrolled on zeros for ever, every lamp stayed
// green, and an IQ or audio recording running at the time kept writing a
// valid-looking file of silence.
//
// The file here is the real IqFileSource on a real WAV, overwritten with a
// stub while the pipeline plays it (the source opens its handle shared, which
// is what lets the rewrite happen). The source's own flag is checked too, so
// a red run says which half failed: the source noticing, or the pipeline
// listening.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/pipeline.hpp"
#include "source/iq_file_source.hpp"
#include "test_check.hpp"

namespace {

void putU16(std::vector<unsigned char>& v, std::uint16_t x) {
    v.push_back(static_cast<unsigned char>(x & 0xFFu));
    v.push_back(static_cast<unsigned char>((x >> 8) & 0xFFu));
}

void putU32(std::vector<unsigned char>& v, std::uint32_t x) {
    for (int s = 0; s < 32; s += 8) { v.push_back(static_cast<unsigned char>((x >> s) & 0xFFu)); }
}

// A 16-bit stereo (I/Q) PCM WAV of `frames` frames at `rateHz`.
std::vector<unsigned char> iqWav(std::uint32_t rateHz, std::uint32_t frames) {
    std::vector<unsigned char> v;
    const std::uint32_t dataBytes = frames * 4u;
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    putU32(v, 36u + dataBytes);
    v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    putU32(v, 16u);
    putU16(v, 1u);            // PCM
    putU16(v, 2u);            // I and Q
    putU32(v, rateHz);
    putU32(v, rateHz * 4u);   // byte rate
    putU16(v, 4u);            // block align
    putU16(v, 16u);           // bits
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    putU32(v, dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        putU16(v, static_cast<std::uint16_t>(1000 + (i % 97)));
        putU16(v, static_cast<std::uint16_t>(2000 + (i % 89)));
    }
    return v;
}

bool writeFile(const std::string& path, const std::vector<unsigned char>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string path =
        "pipeline_file_fault_" + std::to_string(TEST_GETPID()) + ".wav";

    {
        // 2 s at 250 kHz: long enough that the pipeline is well inside the
        // data chunk when the file is cut, and a rate the chain accepts.
        CHECK(writeFile(path, iqWav(250000u, 500000u)));
        auto file = std::make_unique<cascade::source::IqFileSource>();
        CHECK(file->open(path));
        cascade::source::IqFileSource* raw = file.get();
        CHECK(!raw->selfPaced());   // the branch this test is about

        cascade::core::Pipeline::Config cfg;
        cfg.sampleRateHz = 250000.0;
        cfg.fftSize = 1024;
        cfg.averagingAlpha = 1.0f;
        cfg.audioEnabled = false;
        cascade::core::Pipeline p(cfg);
        p.setSource(std::move(file));
        p.start();

        // Playing: frames arrive and nothing is wrong yet.
        cascade::core::SpectrumFrame f;
        CHECK(waitFor([&] { return p.getLatestFrame(f); }, 5000));
        CHECK(!p.faulted());

        // Cut the recording down to a header and a handful of frames.
        CHECK(writeFile(path, iqWav(250000u, 8u)));

        // The source notices within a few reads...
        const bool sourceSaw = waitFor([&] { return raw->faulted(); }, 5000);
        CHECK(sourceSaw);
        // ...and the PIPELINE must hear about it. Before the fix this waited
        // out its whole timeout with faulted() false and running() true.
        const bool pipelineSaw = waitFor([&] { return p.faulted(); }, 5000);
        std::printf("source faulted: %d, pipeline faulted: %d, running: %d\n",
                    sourceSaw ? 1 : 0, pipelineSaw ? 1 : 0, p.running() ? 1 : 0);
        CHECK(pipelineSaw);
        const std::string msg = p.faultMessage();
        std::printf("fault message: %s\n", msg.c_str());
        CHECK(msg.find("source thread") != std::string::npos);
        CHECK(msg.find("I/O error") != std::string::npos);

        // And it comes down, rather than scrolling a spectrum of zeros.
        CHECK(waitFor([&] { return !p.running(); }, 5000));
        p.stop();
        CHECK(!p.running());
    }

    const int rc = testSummary("test_pipeline_file_fault");
    if (rc == 0) { std::remove(path.c_str()); }
    return rc;
}
