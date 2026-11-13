// recorder_containers.cpp - the RF64 and SigMF backends of the I/Q recorder (wire-a second pass; see core/recorder.hpp, IqBackend). Both writers already existed, standalone and tested
// (source/rf64_writer.hpp, source/sigmf_writer.hpp); this is the glue that gives them a take's life: a name reserved exclusively, the recorder's counters, a retune log.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "core/recorder.hpp"
#include "core/unique_file.hpp"
#include "source/rf64_writer.hpp"
#include "source/sigmf_writer.hpp"

namespace cascade::core {

namespace {

std::atomic<std::uint64_t> g_plainLimit{0};

class Rf64Backend final : public IqBackend {
public:
    bool open(const std::string& path, double rateHz, int sampleFormat, std::string& err) {
        cascade::source::iqfile::Rf64Options o;
        o.sampleRateHz = std::floor(rateHz + 0.5);
        if (g_plainLimit.load() != 0) { o.plainLimitBytes = g_plainLimit.load(); }
        o.format = sampleFormat == kRecordIqInt16 ? cascade::source::iqfile::WriteFormat::S16 : cascade::source::iqfile::WriteFormat::F32;
        return w_.open(path, o, &err);
    }
    bool write(const std::complex<float>* s, std::size_t n) override { return w_.write(s, n, nullptr); }
    bool close() override { std::string ignore; return w_.close(&ignore); }
private:
    cascade::source::iqfile::Rf64Writer w_;
};

class SigmfBackend final : public IqBackend {
public:
    bool open(const std::string& path, double rateHz, int sampleFormat, double centreHz, std::int64_t startNs, std::string& err) {
        cascade::source::iqfile::SigmfOptions o;
        o.type = sampleFormat == kRecordIqInt16 ? cascade::source::iqfile::SigmfType::Ci16Le : cascade::source::iqfile::SigmfType::Cf32Le;
        o.sampleRateHz = rateHz;
        o.hasCentre = centreHz != 0.0;
        o.centreHz = centreHz;
        o.startUnixNs = startNs;
        o.recorder = "FoxSDR";
        return w_.open(path, o, &err);
    }
    bool write(const std::complex<float>* s, std::size_t n) override { return w_.write(s, n, nullptr); }
    void retune(double centreHz) override {
        const std::int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        (void)w_.addCapture(centreHz, ns, nullptr);
        (void)w_.flushMetadata(nullptr);   // a crash after a retune still leaves the metadata that says so
    }
    bool close() override { std::string ignore; return w_.close(&ignore); }
private:
    cascade::source::iqfile::SigmfWriter w_;
};

}  // namespace

void Recorder::setRf64PlainLimitForTest(std::uint64_t bytes) { g_plainLimit.store(bytes); }

bool openIqContainer(const Recorder::OpenRequest& req, Recorder::OpenedFile& out, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(req.directory), ec);
    if (ec || !fs::is_directory(fs::path(req.directory))) {
        error = "recorder: cannot create directory \"" + req.directory + "\"";
        return false;
    }
    // The name is reserved exclusively, as a WAV take's is (core/unique_file.hpp): an existing file is never opened over.
    std::string used;
    bool exhausted = false;
    if (!reserveUnique(req.path, used, exhausted)) {
        error = "recorder: cannot create \"" + req.path + "\"";
        return false;
    }
    Recorder::OpenedFile opened;
    opened.kind = req.kind;
    opened.sampleFormat = req.sampleFormat;
    opened.container = req.container;
    opened.path = used;
    std::string err;
    if (req.container == kIqContainerRf64) {
        auto b = std::make_unique<Rf64Backend>();
        if (!b->open(used, req.sampleRateHz, req.sampleFormat, err)) { error = "recorder: " + err; return false; }
        opened.backend = std::move(b);
    } else {
        auto b = std::make_unique<SigmfBackend>();
        if (!b->open(used, req.sampleRateHz, req.sampleFormat, req.centreHz, req.startUnixNs, err)) { error = "recorder: " + err; return false; }
        opened.backend = std::move(b);
    }
    out = std::move(opened);
    return true;
}

}  // namespace cascade::core
