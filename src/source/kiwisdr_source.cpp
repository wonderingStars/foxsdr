// kiwisdr_source.cpp - see kiwisdr_source.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/kiwisdr_source.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "core/diag_log.hpp"
#include "core/health_events.hpp"

namespace cascade::source {

namespace {

constexpr double kRateHz = 12000.0;   // KiwiSdrClient::audioRateHz(), confirmed by the live test

bool hostCharOk(unsigned char c) {
    // letters, digits, '.', '-', '_' and (for an IPv6 literal in brackets) ':' '[' ']'
    return std::isalnum(c) != 0 || c == '.' || c == '-' || c == '_';
}

}  // namespace

bool KiwiSdrSource::parseArgs(const std::string& argsIn, Address& out, std::string& why) {
    std::string a = argsIn;
    const auto trim = [&a] {   // spaces at the ends only
        while (!a.empty() && (a.front() == ' ' || a.front() == '\t')) { a.erase(a.begin()); }
        while (!a.empty() && (a.back() == ' ' || a.back() == '\t')) { a.pop_back(); }
    };
    trim();
    if (a.rfind("kiwisdr=", 0) == 0) { a = a.substr(8); }
    trim();
    std::string host = a;
    std::uint16_t port = 8073;
    const std::size_t colon = a.rfind(':');
    if (colon != std::string::npos) {
        host = a.substr(0, colon);
        const std::string p = a.substr(colon + 1);
        if (p.empty() || p.size() > 5) {
            why = "the port is not a number from 1 to 65535";
            return false;
        }
        long v = 0;
        for (const char ch : p) {
            if (ch < '0' || ch > '9') {
                why = "the port is not a number from 1 to 65535";
                return false;
            }
            v = v * 10 + (ch - '0');
        }
        if (v < 1 || v > 65535) {
            why = "the port is not a number from 1 to 65535";
            return false;
        }
        port = static_cast<std::uint16_t>(v);
    }
    if (host.empty()) {
        why = "no receiver address given (a host name or number, and a port if it is not 8073)";
        return false;
    }
    if (host.size() > 253) {
        why = "the receiver address is too long";
        return false;
    }
    for (const char ch : host) {
        if (!hostCharOk(static_cast<unsigned char>(ch))) {
            why = "the receiver address holds a character a host name does not (give a name or number and a port, not a web address)";
            return false;
        }
    }
    out.host = host;
    out.port = port;
    return true;
}

KiwiSdrSource::ServerMode KiwiSdrSource::serverModeFor(int engineMode) {
    switch (engineMode) {
        case 0: return {"nbfm", -6000, 6000};
        case 2: return {"am", -4900, 4900};
        case 4: return {"usb", 300, 2700};
        case 5: return {"lsb", -2700, -300};
        case 6: return {"cw", 300, 800};
        default: return {"", 0, 0};   // WFM, DSB, RAW: the receiver has no such mode word
    }
}

KiwiSdrSource::KiwiSdrSource() = default;
KiwiSdrSource::KiwiSdrSource(const Timing& timing) : timing_(timing) {}

KiwiSdrSource::~KiwiSdrSource() { closeDevice(); }

void KiwiSdrSource::setError(const std::string& s) {
    std::lock_guard<std::mutex> lk(mutex_);
    error_ = s;
}

bool KiwiSdrSource::open(const std::string& args) {
    if (open_.load() || client_) {
        setError("this source is open already");
        return false;
    }
    Address addr;
    std::string why;
    if (!parseArgs(args, addr, why)) {
        setError(why);
        return false;
    }
    auto client = std::make_unique<KiwiSdrClient>();
    KiwiSdrClient::Options o;
    o.host = addr.host;
    o.port = addr.port;
    o.connectWait = timing_.connectWait;
    o.stallWait = timing_.stallWait;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        name_ = "KiwiSDR: " + addr.host + ":" + std::to_string(addr.port);
        error_.clear();
    }
    // The address is the user's own and never goes in a report; the log line names the kind only.
    cascade::core::diagLogf("source: opening a KiwiSDR receiver");
    if (!client->open(o)) {
        setError("could not connect to the KiwiSDR receiver: " + client->status().detail);
        return false;
    }
    client_ = std::move(client);
    // The first tune: the receiver sends nothing until it has one. The engine puts the radio's own frequency and mode on straight after.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        mode_ = serverModeFor(2);
        engineMode_ = 2;
    }
    if (!tuneNow()) {
        setError("the KiwiSDR receiver connection closed before it could be tuned: " + client_->status().detail);
        client_->close();
        client_.reset();
        return false;
    }
    // Wait for the receiver's first word: audio (Playing) or a refusal. Never longer than firstAudioWait.
    const auto until = std::chrono::steady_clock::now() + timing_.firstAudioWait;
    while (client_->state() == KiwiSdrClient::State::Connecting && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const KiwiSdrClient::State st = client_->state();
    if (st != KiwiSdrClient::State::Playing) {
        std::string sentence;
        switch (st) {
            case KiwiSdrClient::State::Connecting:
                sentence = "the KiwiSDR receiver accepted the connection but sent no audio within " +
                           std::to_string(timing_.firstAudioWait.count() / 1000) + " seconds";
                break;
            case KiwiSdrClient::State::RefusedBusy: sentence = "the KiwiSDR receiver is busy: it has no free place for another listener"; break;
            case KiwiSdrClient::State::RefusedPassword: sentence = "the KiwiSDR receiver wants a password (this release cannot send one)"; break;
            case KiwiSdrClient::State::ServerDown: sentence = "the KiwiSDR receiver says it is down"; break;
            case KiwiSdrClient::State::Redirected: sentence = "the KiwiSDR receiver sends listeners to another address (not followed)"; break;
            default: sentence = "the KiwiSDR receiver ended the connection: " + client_->status().detail; break;
        }
        setError(sentence);
        const std::uint64_t before = KiwiSdrClient::threadsAbandoned();
        client_->close();
        client_.reset();
        if (KiwiSdrClient::threadsAbandoned() > before) { cascade::core::health::raiseRecovered(cascade::core::health::Recovered::ReaderLeak); }
        return false;
    }
    open_.store(true);
    return true;
}

void KiwiSdrSource::closeDevice() {
    running_.store(false);
    open_.store(false);
    if (client_) {
        const std::uint64_t before = KiwiSdrClient::threadsAbandoned();
        client_->close();
        if (KiwiSdrClient::threadsAbandoned() > before) { cascade::core::health::raiseRecovered(cascade::core::health::Recovered::ReaderLeak); }
    }
}

bool KiwiSdrSource::isOpen() const { return open_.load() && client_ != nullptr; }

bool KiwiSdrSource::start() {
    if (!isOpen()) { return false; }
    running_.store(true);
    return true;
}

void KiwiSdrSource::stop() { running_.store(false); }

bool KiwiSdrSource::running() const { return running_.load(); }

double KiwiSdrSource::sampleRateHz() const { return kRateHz; }

bool KiwiSdrSource::setSampleRateHz(double hz) { return hz == kRateHz; }

std::vector<double> KiwiSdrSource::supportedSampleRatesHz() const { return {kRateHz}; }

double KiwiSdrSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return centerHz_;
}

bool KiwiSdrSource::tuneNow() {
    ServerMode m;
    double hz = 0.0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        m = mode_;
        hz = centerHz_;
    }
    if (!client_ || m.word[0] == '\0') { return false; }
    return client_->tune(m.word, m.lowCutHz, m.highCutHz, hz);
}

bool KiwiSdrSource::setCenterFrequencyHz(double hz) {
    if (!(hz >= 0.0) || !std::isfinite(hz)) {
        setError("the KiwiSDR receiver cannot be tuned to that frequency");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        centerHz_ = hz;
    }
    if (!client_) { return true; }   // before the open: remembered, and sent with the first tune
    return tuneNow();
}

bool KiwiSdrSource::setServerMode(int engineMode) {
    const ServerMode m = serverModeFor(engineMode);
    if (m.word[0] == '\0') {
        setError("a KiwiSDR receiver has no such mode; use AM, USB, LSB, CW or narrow FM");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        mode_ = m;
        engineMode_ = engineMode;
    }
    if (!client_) { return true; }
    return tuneNow();
}

std::string KiwiSdrSource::serverMode() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return mode_.word;
}

std::size_t KiwiSdrSource::read(std::complex<float>* dst, std::size_t n) {
    if (!client_ || n == 0) { return 0; }
    if (scratch_.size() < n) { scratch_.resize(n); }
    const std::size_t got = client_->read(scratch_.data(), n, kReadWait);
    if (!running_.load(std::memory_order_relaxed)) { return 0; }   // stopped: the audio is drained and thrown away, so the ring never backs up
    constexpr float kScale = 1.0f / 32768.0f;
    for (std::size_t i = 0; i < got; ++i) { dst[i] = std::complex<float>(static_cast<float>(scratch_[i]) * kScale, 0.0f); }
    return got;
}

bool KiwiSdrSource::faulted() const {
    if (!client_) { return false; }
    const KiwiSdrClient::State s = client_->state();
    return s != KiwiSdrClient::State::Idle && s != KiwiSdrClient::State::Connecting && s != KiwiSdrClient::State::Playing &&
           s != KiwiSdrClient::State::Closed;
}

const char* KiwiSdrSource::name() const {
    // (a std::string member returned by pointer: it is only rewritten by open(), before the pipeline sees the source)
    return name_.c_str();
}

const char* KiwiSdrSource::lastError() const {
    // The fault sentence while faulted; otherwise the last refusal. A copy into a member keeps the pointer valid.
    static thread_local std::string tl;
    if (faulted()) {
        tl = stateSentence();
        return tl.c_str();
    }
    std::lock_guard<std::mutex> lk(mutex_);
    tl = error_;
    return tl.c_str();
}

std::string KiwiSdrSource::faultedWhile() const { return faulted() ? stateSentence() : std::string(); }

KiwiSdrClient::Status KiwiSdrSource::clientStatus() const { return client_ ? client_->status() : KiwiSdrClient::Status{}; }

std::string KiwiSdrSource::stateSentence() const {
    if (!client_) { return "not connected"; }
    const KiwiSdrClient::Status s = client_->status();
    switch (s.state) {
        case KiwiSdrClient::State::RefusedBusy: return "the KiwiSDR receiver is busy: it has no free place for another listener";
        case KiwiSdrClient::State::RefusedPassword: return "the KiwiSDR receiver wants a password (this release cannot send one)";
        case KiwiSdrClient::State::ServerDown: return "the KiwiSDR receiver says it is down";
        case KiwiSdrClient::State::Redirected: return "the KiwiSDR receiver sends listeners to another address (not followed)";
        case KiwiSdrClient::State::Dropped: return "the KiwiSDR connection was lost: " + s.detail;
        case KiwiSdrClient::State::Malformed: return "the KiwiSDR receiver sent data FoxSDR could not read: " + s.detail;
        case KiwiSdrClient::State::ConnectFailed: return "could not connect to the KiwiSDR receiver: " + s.detail;
        default: return s.detail;
    }
}

std::string KiwiSdrSource::stateWord() const {
    if (!client_) { return "idle"; }
    switch (client_->state()) {
        case KiwiSdrClient::State::Idle: return "idle";
        case KiwiSdrClient::State::Connecting: return "connecting";
        case KiwiSdrClient::State::Playing: return "playing";
        case KiwiSdrClient::State::RefusedBusy: return "busy";
        case KiwiSdrClient::State::RefusedPassword: return "password";
        case KiwiSdrClient::State::ServerDown: return "down";
        case KiwiSdrClient::State::Redirected: return "redirect";
        case KiwiSdrClient::State::ConnectFailed: return "failed";
        case KiwiSdrClient::State::Dropped: return "dropped";
        case KiwiSdrClient::State::Malformed: return "malformed";
        case KiwiSdrClient::State::Closed: return "closed";
    }
    return "idle";
}

bool KiwiSdrSource::haveSmeter() const { return client_ && client_->haveSmeter(); }
double KiwiSdrSource::rssiDbm() const { return client_ ? client_->rssiDbm() : 0.0; }
std::uint64_t KiwiSdrSource::droppedSamples() const { return client_ ? client_->droppedSamples() : 0; }

}  // namespace cascade::source
