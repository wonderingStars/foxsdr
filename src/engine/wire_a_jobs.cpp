// wire_a_jobs.cpp - WIRING LANE wire-a, the one background job (JOB_CANCEL, fact.job.*) and the job EXPORT_BOX (dsp/box_export.hpp).
//
// THE ENGINE'S PASS DOES NO DISK WORK (engine_disk_work.cpp), so a feature that reads or writes a recording runs on a worker of core/disk_job.hpp: this file holds the lane's single slot for one,
// the cancel flag and the progress the worker shares with the pass, and the facts. The pass only starts the work and collects the result (jobPass, once a pass, never blocks). The destructor
// asks a job still out to stop; the DiskJob's own reap bounds the wait and lets a blocked worker finish by itself (it owns everything it uses by value or through the shared control block).
//
// EXPORT_BOX reads the recording with source/iq_readers.hpp (WAV, RF64, SigMF, raw), adapts it to the model's TransportSource, and writes the box with the model's own file sink. Everything that can
// be wrong with the box (outside the band, over 4 GiB, a rate the file does not have) is the model's plan and arrives as a failed job with the plan's words.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "dsp/box_export.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"
#include "source/iq_readers.hpp"

namespace cascade::engine {

WireA::~WireA() {
    if (jobCtl_) { jobCtl_->cancel.store(true); }
    job_.reap();
}

WireA::Result WireA::startJob(Result res, const char* name, std::function<JobResult(JobCtl&)> work) {
    if (job_.pending()) { return refused(res, FOXAPI_BUSY, "a job is running already"); }
    auto ctl = std::make_shared<JobCtl>();
    const bool started = job_.request([ctl, work = std::move(work)]() -> JobResult {
        JobResult r;
        try {
            r = work(*ctl);
        } catch (...) {
            r.ok = false;
            r.message = "the job stopped on an unexpected error";
        }
        if (ctl->cancel.load() && !r.ok) { r.cancelled = true; }
        return r;
    });
    if (!started) { return refused(res, FOXAPI_BUSY, "a job is running already"); }
    jobCtl_ = ctl;
    jobName_ = name;
    jobState_ = "running";
    jobMessage_.clear();
    jobJson_.clear();
    bump();
    return res;
}

void WireA::jobPass() {
    if (jobState_ == "running" && jobCtl_) {
        // The progress moves the facts' counter in steps of a percent, not every pass.
        static_cast<void>(jobCtl_->progress.load());
    }
    JobResult r;
    if (!job_.poll(r)) { return; }
    jobState_ = r.ok ? "done" : (r.cancelled ? "cancelled" : "failed");
    jobMessage_ = r.message;
    jobJson_ = r.json;
    bump();
}

void WireA::jobFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("job.state", jobState_);
    fact("job.name", jobName_);
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.3f", jobCtl_ ? jobCtl_->progress.load() : 0.0);
    fact("job.progress", buf);
    fact("job.message", jobMessage_);
    fact("job.result", jobJson_);
}

namespace {

namespace be = cascade::dsp::boxexport;

// An I/Q file reader as the export's TransportSource.
class ReaderTransport final : public cascade::source::transport::TransportSource {
public:
    explicit ReaderTransport(cascade::source::iqfile::IqFileReader& r) : r_(r) {}
    std::uint64_t totalFrames() const override { return r_.info().totalSamples; }
    bool seek(std::uint64_t frame) override { return r_.seek(frame); }
    std::size_t read(std::complex<float>* dst, std::size_t n) override { return r_.read(dst, n); }

private:
    cascade::source::iqfile::IqFileReader& r_;
};

bool num(const nlohmann::json& j, const char* key, double& out, bool required, std::string& why) {
    if (!j.contains(key)) {
        if (required) { why = std::string("the field ") + key + " is missing"; }
        return !required;
    }
    if (!j[key].is_number() || !std::isfinite(j[key].get<double>())) { why = std::string("the field ") + key + " is not a number"; return false; }
    out = j[key].get<double>();
    return true;
}

nlohmann::json resultJson(const be::BoxResult& r) {
    return {{"outRateHz", r.outRateHz}, {"frames", r.framesWritten}, {"decimation", r.decimation}, {"taps", r.taps},
            {"designAttenuationDb", r.designAttenuationDb}, {"clipped", r.clipped}, {"peak", r.peak}};
}
}  // namespace

WireA::Result WireA::jobOp(const FoxCommand& c, const std::string& text, Result res) {
    if (c.op == FOXAPP_OP_JOB_CANCEL) {
        if (!job_.pending() || !jobCtl_) { return refused(res, FOXAPI_NO_CHANGE, "no job is running"); }
        jobCtl_->cancel.store(true);
        return res;
    }
    // EXPORT_BOX
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is not a JSON object"); }
    std::string why;
    if (!j.contains("input") || !j["input"].is_string() || !j.contains("output") || !j["output"].is_string()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the input and output paths are missing"); }
    const std::string in = j["input"].get<std::string>(), out = j["output"].get<std::string>();
    if (!fullPath(in, why) || !fullPath(out, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
    be::BoxSpec spec;
    double rate = 0.0, centre = 0.0;
    bool ok = num(j, "t0", spec.t0, true, why) && num(j, "t1", spec.t1, true, why) && num(j, "loHz", spec.loHz, true, why) && num(j, "hiHz", spec.hiHz, true, why) &&
              num(j, "attenuationDb", spec.attenuationDb, false, why) && num(j, "minOutRateHz", spec.minOutRateHz, false, why) && num(j, "audioGain", spec.audioGain, false, why) &&
              num(j, "fmDeviationHz", spec.fmDeviationHz, false, why) && num(j, "sampleRateHz", rate, false, why) && num(j, "centerHz", centre, false, why);
    if (!ok) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
    const std::string mode = j.value("mode", std::string("iq")), format = j.value("format", std::string("float32"));
    if (mode == "iq") { spec.mode = be::Mode::Iq; }
    else if (mode == "am") { spec.mode = be::Mode::Am; }
    else if (mode == "fm") { spec.mode = be::Mode::Fm; }
    else if (mode == "usb") { spec.mode = be::Mode::Usb; }
    else if (mode == "lsb") { spec.mode = be::Mode::Lsb; }
    else { return refused(res, FOXAPI_BAD_ARGUMENT, "the mode is iq, am, fm, usb or lsb"); }
    if (format == "float32") { spec.format = be::Format::WavFloat32; }
    else if (format == "pcm16") { spec.format = be::Format::WavPcm16; }
    else if (format == "cf32") { spec.format = be::Format::RawCf32; }
    else { return refused(res, FOXAPI_BAD_ARGUMENT, "the format is float32, pcm16 or cf32"); }
    const bool hasCentre = j.contains("centerHz");
    return startJob(res, "box export", [in, out, spec, rate, centre, hasCentre](JobCtl& ctl) mutable -> JobResult {
        JobResult r;
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(out), ec)) { r.message = "the output file exists already"; return r; }
        std::string err;
        cascade::source::iqfile::RawParams rp;
        rp.sampleRateHz = rate;
        rp.centerFrequencyHz = centre;
        rp.hasCenter = hasCentre;
        std::unique_ptr<cascade::source::iqfile::IqFileReader> reader = cascade::source::iqfile::openAuto(in, rp, &err);
        if (!reader) {
            // The reader's own words, unless they name the file: a message never carries a path.
            r.message = "the recording could not be opened" + (err.empty() || err.find(in) != std::string::npos ? std::string() : ": " + err);
            return r;
        }
        const cascade::source::iqfile::IqInfo& info = reader->info();
        spec.sampleRateHz = info.sampleRateHz;
        spec.centerHz = hasCentre ? centre : info.centerFrequencyHz;
        if (!(spec.sampleRateHz > 0.0)) { r.message = "the recording does not say its sample rate: give sampleRateHz"; return r; }
        if (!hasCentre && !info.centerKnown) { r.message = "the recording does not say its centre frequency: give centerHz"; return r; }
        ReaderTransport src(*reader);
        const be::BoxResult b = be::exportBoxToFile(src, spec, std::filesystem::path(out), &ctl.cancel, [&ctl](double f) { ctl.progress.store(f); });
        r.ok = b.ok;
        r.cancelled = b.cancelled;
        r.message = b.ok ? std::string("exported") : (b.cancelled ? std::string("cancelled") : b.error);
        r.json = resultJson(b).dump();
        return r;
    });
}

}  // namespace cascade::engine
