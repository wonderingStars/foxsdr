#include "source/fcd_control.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace cascade::source {

namespace fc = fcd;

namespace {

constexpr int kGetReplyMs = 200;
constexpr int kSetReplyMs = 30;  // whether the dongle answers a Set at all is UNVERIFIED: a missing reply is not an error

bool wholeDb(double db, int& out) {
    if (!std::isfinite(db)) { return false; }
    const long n = std::lround(db);
    if (std::fabs(db - static_cast<double>(n)) > 0.01) { return false; }
    out = static_cast<int>(n);
    return true;
}

std::string lowered(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

}  // namespace

FcdControl::~FcdControl() {
    if (link_) { link_->close(); }
}

bool FcdControl::exchange(std::uint8_t command, const std::vector<std::uint8_t>& params, bool needReply, std::uint8_t* replyOut,
                          std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!link_) { error = "the dongle's control link is closed"; return false; }
    if (command == fc::cmd::ResetToBootloader) { error = "refused: the bootloader reset is never sent"; return false; }
    std::uint8_t buf[256];
    // stale reports from earlier commands would be taken for this one's answer
    for (int i = 0; i < 8; ++i) {
        if (link_->read(buf, sizeof(buf), 0) <= 0) { break; }
    }
    const auto rep = fc::report(command, params);
    if (!link_->write(rep.data(), error)) {
        if (error.empty()) { error = "the dongle did not take the command"; }
        return false;
    }
    const unsigned wait = needReply ? kGetReplyMs : kSetReplyMs;
    for (int tries = 0; tries < 4; ++tries) {
        const int n = link_->read(buf, sizeof(buf), wait);
        if (n < 0) { error = "the dongle's control link failed"; return false; }
        if (n == 0) { break; }
        fc::Reply r;
        if (fc::parseReply(buf, static_cast<std::size_t>(n), command, r)) {
            if (replyOut) { std::memcpy(replyOut, buf, std::min<std::size_t>(static_cast<std::size_t>(n), fc::kReportBytes)); }
            return true;
        }
    }
    if (needReply) {
        error = "the dongle did not answer";
        return false;
    }
    return true;
}

bool FcdControl::query(std::string& text, std::string& error) {
    std::uint8_t r[fc::kReportBytes] = {};
    if (!exchange(fc::cmd::Query, {}, true, r, error)) { return false; }
    text.clear();
    for (std::size_t i = 2; i < fc::kReportBytes && text.size() < 40; ++i) {  // the text starts where data does: byte 2 (UNVERIFIED)
        if (r[i] == 0) { break; }
        text.push_back(r[i] >= 0x20 && r[i] < 0x7F ? static_cast<char>(r[i]) : '?');
    }
    return true;
}

bool FcdControl::setFrequencyHz(double hz, double ppm, std::string& error) {
    if (!std::isfinite(hz) || !fc::tunable(hz)) {
        error = "the FUNcube Dongle tunes 150 kHz to 240 MHz and 420 MHz to 1.9 GHz";
        return false;
    }
    if (!std::isfinite(ppm) || std::fabs(ppm) > 1000.0) {
        error = "the oscillator trim must be within 1000 ppm";
        return false;
    }
    const std::uint32_t f = fc::trimmedHz(hz, ppm);
    return exchange(fc::cmd::SetFrequencyHz,
                    {static_cast<std::uint8_t>(f), static_cast<std::uint8_t>(f >> 8), static_cast<std::uint8_t>(f >> 16),
                     static_cast<std::uint8_t>(f >> 24)},
                    false, nullptr, error);
}

bool FcdControl::frequencyHz(std::uint32_t& hz, std::string& error) {
    std::uint8_t r[fc::kReportBytes] = {};
    if (!exchange(fc::cmd::GetFrequencyHz, {}, true, r, error)) { return false; }
    hz = fc::le32(r + 2);
    return true;
}

bool FcdControl::setGainDb(const std::string& name, double db, std::string& error) {
    int n = 0;
    if (name == "LNA") {
        if (plus_) {
            if (!wholeDb(db, n) || (n != 0 && n != 10)) { error = "the Pro+ LNA is off (0 dB) or on (10 dB)"; return false; }
            return exchange(fc::cmd::SetLna, {static_cast<std::uint8_t>(n == 10 ? 1 : 0)}, false, nullptr, error);
        }
        if (!std::isfinite(db)) { error = "that is not a gain"; return false; }
        double actual = 0.0;
        const int idx = fc::proLnaIndexFor(db, actual);
        return exchange(fc::cmd::SetLna, {static_cast<std::uint8_t>(idx)}, false, nullptr, error);
    }
    if (name == "Mixer") {
        if (plus_) {
            if (!wholeDb(db, n) || (n != 0 && n != 20)) { error = "the Pro+ mixer is off (0 dB) or on (20 dB)"; return false; }
            return exchange(fc::cmd::SetMixer, {static_cast<std::uint8_t>(n == 20 ? 1 : 0)}, false, nullptr, error);
        }
        if (!wholeDb(db, n) || (n != 4 && n != 12)) { error = "the Pro mixer is 4 or 12 dB"; return false; }
        return exchange(fc::cmd::SetMixer, {static_cast<std::uint8_t>(n == 12 ? 1 : 0)}, false, nullptr, error);
    }
    if (name == "IF") {
        if (plus_) {
            if (!wholeDb(db, n) || n < fc::kProPlusIfMin || n > fc::kProPlusIfMax) { error = "the Pro+ IF gain is 0 to 59 dB in whole dB"; return false; }
            return exchange(fc::cmd::SetIfGain, {static_cast<std::uint8_t>(n)}, false, nullptr, error);
        }
        if (!wholeDb(db, n) || n < fc::kProIfMin || n > fc::kProIfMax) { error = "the Pro IF gain is 3 to 57 dB in whole dB"; return false; }
        std::uint8_t regs[6];
        fc::proIfStageRegisters(n, regs);
        for (int i = 0; i < 6; ++i) {
            if (!exchange(fc::cmd::SetIfStage[i], {regs[i]}, false, nullptr, error)) { return false; }
        }
        return true;
    }
    error = "the FUNcube Dongle has three gains: \"LNA\", \"Mixer\" and \"IF\"";
    return false;
}

bool FcdControl::readGainDb(const std::string& name, double& db, std::string& error) {
    std::uint8_t r[fc::kReportBytes] = {};
    if (name == "LNA") {
        if (!exchange(fc::cmd::GetLna, {}, true, r, error)) { return false; }
        if (plus_) { db = r[2] ? 10.0 : 0.0; return true; }
        if (r[2] >= 15 || fc::kProLnaDb[r[2]] < -900) { error = "the dongle reported an LNA setting that is not in the table"; return false; }
        db = fc::kProLnaDb[r[2]];
        return true;
    }
    if (name == "Mixer") {
        if (!exchange(fc::cmd::GetMixer, {}, true, r, error)) { return false; }
        db = plus_ ? (r[2] ? 20.0 : 0.0) : (r[2] ? 12.0 : 4.0);
        return true;
    }
    if (name == "IF") {
        if (plus_) {
            if (!exchange(fc::cmd::GetIfGain, {}, true, r, error)) { return false; }
            if (r[2] > fc::kProPlusIfMax) { error = "the dongle reported an IF gain above 59 dB"; return false; }
            db = r[2];
            return true;
        }
        std::uint8_t regs[6];
        for (int i = 0; i < 6; ++i) {
            if (!exchange(fc::cmd::GetIfStage[i], {}, true, r, error)) { return false; }
            regs[i] = r[2];
        }
        db = fc::proIfTotalDb(regs);
        return true;
    }
    error = "the FUNcube Dongle has three gains: \"LNA\", \"Mixer\" and \"IF\"";
    return false;
}

bool FcdControl::setBiasTee(bool on, std::string& error) {
    if (!plus_) { error = "only the Pro+ has a bias tee"; return false; }
    return exchange(fc::cmd::SetBiasTee, {static_cast<std::uint8_t>(on ? 1 : 0)}, false, nullptr, error);
}

bool FcdControl::biasTee(bool& on, std::string& error) {
    if (!plus_) { error = "only the Pro+ has a bias tee"; return false; }
    std::uint8_t r[fc::kReportBytes] = {};
    if (!exchange(fc::cmd::GetBiasTee, {}, true, r, error)) { return false; }
    on = r[2] != 0;
    return true;
}

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

FuncubeSource::FuncubeSource(SoundCardSource::BackendFactory audio) : audio_(audio), factory_(std::move(audio)) {}

FuncubeSource::~FuncubeSource() { closeDevice(); }

void FuncubeSource::setHidForTest(HidEnumFn enumerate, HidOpenFn opener) {
    std::lock_guard<std::mutex> lk(mu_);
    enumFn_ = std::move(enumerate);
    openFn_ = std::move(opener);
}

void FuncubeSource::setError(const std::string& s) { err_ = s; }

bool FuncubeSource::open(const std::string& args) {
    closeDevice();
    std::lock_guard<std::mutex> lk(mu_);
    err_.clear();
    long index = 0;
    const std::string idx = argValue(args, "index");
    if (!idx.empty()) {
        char* e = nullptr;
        index = std::strtol(idx.c_str(), &e, 10);
        if (e == idx.c_str() || *e != '\0' || index < 0 || index > 15) { setError("the dongle index must be 0 to 15"); return false; }
    }
    ppm_ = 0.0;
    const std::string pp = argValue(args, "ppm");
    if (!pp.empty()) {
        char* e = nullptr;
        ppm_ = std::strtod(pp.c_str(), &e);
        if (e == pp.c_str() || *e != '\0' || !std::isfinite(ppm_) || std::fabs(ppm_) > 1000.0) { setError("the ppm trim must be within 1000"); return false; }
    }
    const std::string swap = argValue(args, "swapiq");
    if (!swap.empty() && swap != "0" && swap != "1") { setError("swapiq is 0 or 1"); return false; }

    // --- the HID side ---------------------------------------------------------------------------------
    const std::vector<FcdDeviceInfo> found = enumFn_ ? enumFn_() : enumerateFcdHid();
    if (found.empty()) {
        setError("no FUNcube Dongle Pro or Pro+ control interface found (is it plugged in?)");
        return false;
    }
    if (static_cast<std::size_t>(index) >= found.size()) {
        setError("only " + std::to_string(found.size()) + " FUNcube Dongle(s) found; index " + std::to_string(index) + " is too high");
        return false;
    }
    const FcdDeviceInfo& info = found[static_cast<std::size_t>(index)];
    std::string herr;
    std::unique_ptr<HidLink> link = openFn_ ? openFn_(info.path, herr) : openFcdHid(info.path, herr);
    if (!link) {
        setError(herr.empty() ? std::string("the FUNcube Dongle's control interface could not be opened") : herr);
        return false;
    }
    plus_ = info.isProPlus;
    auto ctl = std::make_unique<FcdControl>(std::move(link), plus_);
    std::string q;
    if (!ctl->query(q, herr)) {
        setError("this does not look like a FUNcube Dongle: " + herr);
        return false;
    }

    // --- the sound side: the one input that is the dongle ------------------------------------------------
    std::shared_ptr<SoundCardBackend> be = factory_ ? factory_() : makePortAudioSoundCardBackend();
    const std::vector<SoundCardDevice> list = be->listDevices();
    const std::string wantName = argValue(args, "audio"), wantApi = argValue(args, "api");
    int at = -1;
    if (!wantName.empty()) {
        std::vector<int> hits;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].name == wantName && (wantApi.empty() || list[i].hostApi == wantApi)) { hits.push_back(static_cast<int>(i)); }
        }
        if (hits.size() != 1) {
            setError(hits.empty() ? "the sound input named in audio= was not found"
                                  : "the sound input named in audio= is ambiguous: add api=<host API>");
            return false;
        }
        at = hits[0];
    } else {
        std::vector<int> hits;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (lowered(list[i].name).find("funcube") != std::string::npos && list[i].maxInputChannels >= 2) { hits.push_back(static_cast<int>(i)); }
        }
        if (hits.empty()) {
            setError("no stereo sound input with \"FUNcube\" in its name was found; name the dongle's input with audio=<input name>");
            return false;
        }
        if (hits.size() > 1) {
            setError(std::to_string(hits.size()) + " sound inputs with \"FUNcube\" in their name were found; choose one with audio=<input name>");
            return false;
        }
        at = hits[0];
    }
    SoundCardSettings s;
    s.device = list[static_cast<std::size_t>(at)].name;
    s.hostApi = list[static_cast<std::size_t>(at)].hostApi;
    s.cardRateHz = plus_ ? fc::kRateProPlusHz : fc::kRateProHz;
    s.format = SoundCardFormat::IqStereo;
    s.swapIq = swap == "1";
    s.iqCentreHz = freqHz_;
    s.pickedFromList = true;
    if (!audio_.openWith(s, list)) {
        setError(std::string("the dongle's sound input could not be opened: ") + audio_.lastError());
        return false;
    }
    if (std::fabs(audio_.sampleRateHz() - s.cardRateHz) > 1.0) {
        audio_.closeDevice();
        setError(std::string("the dongle's sound input does not offer ") + (plus_ ? "192" : "96") + " kS/s");
        return false;
    }
    // --- start state: tune, read the gains the dongle has ---------------------------------------------------
    ctl_ = std::move(ctl);
    bias_ = false;
    std::string e2;
    ctl_->setBiasTee(false, e2);  // never left on by a previous program's choice: off until the user selects it
    if (!ctl_->setFrequencyHz(freqHz_, ppm_, e2)) {
        audio_.closeDevice();
        ctl_.reset();
        setError(e2);
        return false;
    }
    for (const char* g : {"LNA", "Mixer", "IF"}) {
        double v = 0.0;
        if (ctl_->readGainDb(g, v, e2)) {
            (std::string(g) == "LNA" ? lna_ : std::string(g) == "Mixer" ? mixer_ : if_) = v;
        }
    }
    name_ = plus_ ? "FUNcube Dongle Pro+" : "FUNcube Dongle Pro";
    open_ = true;
    return true;
}

void FuncubeSource::closeDevice() {
    std::lock_guard<std::mutex> lk(mu_);
    if (ctl_ && bias_) {
        std::string e;
        ctl_->setBiasTee(false, e);  // best effort: the antenna input is not left powered
    }
    audio_.closeDevice();
    ctl_.reset();
    open_ = false;
    bias_ = false;
}

bool FuncubeSource::start() {
    if (!open_) { setError("start() called with no dongle open"); return false; }
    return audio_.start();
}
void FuncubeSource::stop() { audio_.stop(); }
bool FuncubeSource::running() const { return audio_.running(); }
double FuncubeSource::sampleRateHz() const { return open_ ? audio_.sampleRateHz() : 0.0; }
bool FuncubeSource::setSampleRateHz(double hz) {
    if (!open_) { setError("setSampleRateHz() called with no dongle open"); return false; }
    if (std::fabs(hz - audio_.sampleRateHz()) < 1.0) { return true; }
    setError(plus_ ? "the Pro+ delivers 192 kS/s only" : "the Pro delivers 96 kS/s only");
    return false;
}
std::vector<double> FuncubeSource::supportedSampleRatesHz() const { return {plus_ ? fc::kRateProPlusHz : fc::kRateProHz}; }
double FuncubeSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(mu_);
    return freqHz_;
}

bool FuncubeSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open_ || !ctl_) { setError("setCenterFrequencyHz() called with no dongle open"); return false; }
    std::string e;
    if (!ctl_->setFrequencyHz(hz, ppm_, e)) { setError(e); return false; }
    freqHz_ = hz;
    audio_.setCenterFrequencyHz(hz);  // I/Q mode: records what the tuner is set to
    return true;
}

bool FuncubeSource::setFrequencyCorrectionPpm(double ppm) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open_ || !ctl_) { setError("setFrequencyCorrectionPpm() called with no dongle open"); return false; }
    std::string e;
    if (!ctl_->setFrequencyHz(freqHz_, ppm, e)) { setError(e); return false; }
    ppm_ = ppm;
    return true;
}

std::size_t FuncubeSource::read(std::complex<float>* dst, std::size_t n) { return audio_.read(dst, n); }
bool FuncubeSource::faulted() const { return audio_.faulted(); }
std::string FuncubeSource::faultedWhile() const { return audio_.faultedWhile(); }
const char* FuncubeSource::name() const { return name_.c_str(); }
const char* FuncubeSource::lastError() const { return err_.empty() ? audio_.lastError() : err_.c_str(); }

std::vector<GainInfo> FuncubeSource::gains() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<GainInfo> v;
    if (!open_) { return v; }
    GainInfo l, m, i;
    l.name = "LNA";
    m.name = "Mixer";
    i.name = "IF";
    if (plus_) {
        l.minDb = 0; l.maxDb = 10; l.stepDb = 10;
        m.minDb = 0; m.maxDb = 20; m.stepDb = 20;
        i.minDb = 0; i.maxDb = 59; i.stepDb = 1;
    } else {
        l.minDb = -5; l.maxDb = 30; l.stepDb = 2.5;
        m.minDb = 4; m.maxDb = 12; m.stepDb = 8;
        i.minDb = 3; i.maxDb = 57; i.stepDb = 1;
    }
    v.push_back(l);
    v.push_back(m);
    v.push_back(i);
    return v;
}

bool FuncubeSource::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open_ || !ctl_) { setError("setGainDb() called with no dongle open"); return false; }
    std::string e;
    if (!ctl_->setGainDb(name, db, e)) { setError(e); return false; }
    double back = 0.0;
    if (ctl_->readGainDb(name, back, e)) {
        (name == "LNA" ? lna_ : name == "Mixer" ? mixer_ : if_) = back;
    }
    return true;
}

double FuncubeSource::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    return name == "LNA" ? lna_ : name == "Mixer" ? mixer_ : name == "IF" ? if_ : 0.0;
}

std::vector<std::string> FuncubeSource::antennas() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (open_ && plus_) { return {"Bias tee off", "Bias tee on"}; }  // only the Pro+ has a bias tee
    return {};
}

bool FuncubeSource::setAntenna(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!open_ || !ctl_) { setError("setAntenna() called with no dongle open"); return false; }
    if (name != "Bias tee off" && name != "Bias tee on") { setError("the choices are \"Bias tee off\" and \"Bias tee on\""); return false; }
    std::string e;
    if (!ctl_->setBiasTee(name == "Bias tee on", e)) { setError(e); return false; }
    bias_ = name == "Bias tee on";
    return true;
}

std::string FuncubeSource::antenna() const {
    std::lock_guard<std::mutex> lk(mu_);
    return open_ && plus_ ? (bias_ ? "Bias tee on" : "Bias tee off") : std::string();
}

bool FuncubeSource::frequencyRangeHz(double& lo, double& hi) const {
    lo = fc::kBands[0].loHz;
    hi = fc::kBands[1].hiHz;
    return true;
}

}  // namespace cascade::source
