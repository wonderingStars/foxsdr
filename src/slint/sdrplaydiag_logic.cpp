// sdrplaydiag_logic.cpp - see sdrplaydiag_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "sdrplaydiag_logic.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slintdiag {

namespace {

class StandInRunner final : public ProbeRunner {
public:
    explicit StandInRunner(int polls) : polls_(polls) {}
    bool start(const std::string& outPath, bool biasTeeOn, std::string&) override {
        std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
        f << "stand-in SDRplay diagnostic report (no radio was touched)" << '\n' << "bias tee test: " << (biasTeeOn ? "on" : "off") << '\n';
        return static_cast<bool>(f);
    }
    bool running() override { return polls_-- > 0; }
    int exitCode() const override { return 0; }
private:
    int polls_;
};

std::string stamped(const std::string& dir) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char name[64];
    std::snprintf(name, sizeof(name), "sdrplay-diagnostic-%04d%02d%02d-%02d%02d%02d.txt", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return dir + "/" + name;
}

}  // namespace

std::unique_ptr<ProbeRunner> makeStandInRunner(int polls) { return std::make_unique<StandInRunner>(polls); }

SdrPlayDiag::SdrPlayDiag(RunnerFactory factory, std::function<std::string()> reportsDir) : factory_(std::move(factory)), reportsDir_(std::move(reportsDir)) {}

SdrPlayDiag::View SdrPlayDiag::view(bool sdrplayOpen) const {
    using cascade::i18n::tr;
    View v;
    v.intro = tr("For SDRplay owners: steps the RSP through every sample rate, band, LNA state and antenna once, times every call to the SDRplay API, and writes one text file you can attach to a bug report. It contains no serial number and no file names.");
    v.keyEnabled = child_ == nullptr && !sdrplayOpen;
    v.status = status_;
    if (sdrplayOpen && child_ == nullptr) { v.status = tr("FoxSDR has the SDRplay radio open - choose another source first."); }
    v.dialogShown = dialog_;
    v.warning = tr("This takes the SDRplay radio for a minute or two. Close SDRuno or any other program that is using it first.");
    v.biasAsk = biasAsk_;
    v.biasQuestion = tr("The bias tee puts DC power on the antenna socket. Only switch it on if what is connected is meant to be powered that way, such as an active antenna. Switch it on for a moment during the test?");
    v.biasOn = biasOn_;
    return v;
}

void SdrPlayDiag::press() {
    if (child_ != nullptr) { return; }
    dialog_ = true;
    biasAsk_ = false;
    biasOn_ = false;
}

void SdrPlayDiag::tickBias(bool want) {
    if (!dialog_ || biasAsk_) { return; }
    if (want) { biasAsk_ = true; } else { biasOn_ = false; }
}

void SdrPlayDiag::biasYes() {
    if (!dialog_ || !biasAsk_) { return; }
    biasOn_ = true;
    biasAsk_ = false;
}

void SdrPlayDiag::biasCancel() {
    if (!dialog_ || !biasAsk_) { return; }
    biasOn_ = false;
    biasAsk_ = false;
}

void SdrPlayDiag::cancel() {
    dialog_ = false;
    biasAsk_ = false;
    biasOn_ = false;   // the confirmation belongs to the dialog it was given in
}

void SdrPlayDiag::run() {
    using cascade::i18n::tr;
    if (!dialog_ || biasAsk_ || child_ != nullptr) { return; }
    dialog_ = false;
    std::string dir = reportsDir_ ? reportsDir_() : std::string();
    if (dir.empty()) {
        std::error_code tec;
        dir = std::filesystem::temp_directory_path(tec).string();
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    path_ = stamped(dir);
    std::unique_ptr<ProbeRunner> child = factory_();
    std::string error;
    ++starts_;
    const bool bias = biasOn_;   // THE ONLY WAY THE BIAS TEE REACHES THE PROBE: biasOn_, which only the separate confirmation sets
    lastBias_ = bias;
    biasOn_ = false;             // spent: the next dialog asks again
    if (!child || !child->start(path_, bias, error)) {
        status_ = cascade::core::formatText(tr("SDRplay diagnostic could not start: %s"), error.c_str());
        cascade::core::diagWarnf("SDRplay diagnostic could not start - %s", error.c_str());
        return;
    }
    child_ = std::move(child);
    report_.clear();
    status_ = tr("SDRplay diagnostic running - this takes a minute or two...");
    cascade::core::diagLogf("SDRplay diagnostic started (bias tee test %s)", bias ? "confirmed" : "off");
}

void SdrPlayDiag::poll() {
    using cascade::i18n::tr;
    if (child_ == nullptr || child_->running()) { return; }
    const int code = child_->exitCode();
    child_.reset();
    std::string text;
    {
        std::ifstream f(path_, std::ios::binary);
        if (f) { text.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
    }
    if (!text.empty()) {
        report_ = text;
        status_ = cascade::core::formatText(tr("SDRplay diagnostic finished: %s"), path_.c_str());
        cascade::core::diagLogf("SDRplay diagnostic finished - exit %d, %zu bytes", code, text.size());
    } else {
        const std::string why = "no file was written (exit " + std::to_string(code) + ")";
        status_ = cascade::core::formatText(tr("SDRplay diagnostic could not start: %s"), why.c_str());
        cascade::core::diagWarnf("SDRplay diagnostic wrote no file - exit %d", code);
    }
}

}  // namespace foxsdr::slintdiag
