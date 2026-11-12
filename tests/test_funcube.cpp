// test_funcube.cpp - FUNcube Dongle Pro / Pro+: the HID control layer and FuncubeSource (src/source/fcd_protocol.*,
// fcd_control.*) against a STAND-IN dongle (HidLink) and a stand-in sound input (SoundCardBackend). The command
// numbers, report layout, ranges and gain tables are the ones read from pothosware/SoapyFCDPP (BSL-1.0); the
// stand-in is written from the same facts, so what these tests prove is that FoxSDR's code uses them
// consistently and safely - NOT that a real dongle behaves so. UNPROVEN on a real FUNcube Dongle.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "source/fcd_control.hpp"
#include "test_check.hpp"

using namespace cascade::source;
namespace fc = cascade::source::fcd;
using Cx = std::complex<float>;

namespace {

// ---- the stand-in dongle ---------------------------------------------------------------------------------------
struct Dongle {
    std::mutex mu;
    std::condition_variable cv;
    bool plus = true;
    std::deque<std::vector<std::uint8_t>> inbox;   // input reports waiting
    std::vector<std::uint8_t> log;                 // every command byte received
    std::uint32_t freq = 0;
    std::uint8_t lna = 0, mixer = 0, ifGain = 0, bias = 1;  // bias starts ON: a previous program left it so
    std::uint8_t stage[6] = {0, 0, 0, 0, 0, 0};
    bool silent = false;       // answers nothing at all
    bool noSetReply = false;   // answers gets only
    bool failWrite = false, failRead = false;
    bool staleFirst = false;   // queues a wrong-command report before every real answer
    bool closed = false;

    void reply(std::uint8_t cmd, std::initializer_list<std::uint8_t> data) {
        if (staleFirst) { inbox.push_back(std::vector<std::uint8_t>(fc::kReportBytes, 0x77)); }
        std::vector<std::uint8_t> r(fc::kReportBytes, 0);
        r[0] = cmd;
        r[1] = 1;
        std::size_t i = 2;
        for (std::uint8_t b : data) { r[i++] = b; }
        inbox.push_back(std::move(r));
    }
    void handle(const std::uint8_t* r) {
        const std::uint8_t cmd = r[0];
        log.push_back(cmd);
        if (silent) { return; }
        auto setEcho = [&]() { if (!noSetReply) { reply(cmd, {}); } };
        switch (cmd) {
            case fc::cmd::Query: {
                std::vector<std::uint8_t> q(fc::kReportBytes, 0);
                q[0] = cmd; q[1] = 1;
                const char* t = "FCDAPP test-dongle 18.06";
                for (std::size_t i = 0; t[i]; ++i) { q[2 + i] = static_cast<std::uint8_t>(t[i]); }
                inbox.push_back(q);
                break;
            }
            case fc::cmd::SetFrequencyHz: freq = fc::le32(r + 1); setEcho(); break;
            case fc::cmd::GetFrequencyHz: reply(cmd, {static_cast<std::uint8_t>(freq), static_cast<std::uint8_t>(freq >> 8), static_cast<std::uint8_t>(freq >> 16), static_cast<std::uint8_t>(freq >> 24)}); break;
            case fc::cmd::SetLna: lna = r[1]; setEcho(); break;
            case fc::cmd::GetLna: reply(cmd, {lna}); break;
            case fc::cmd::SetMixer: mixer = r[1]; setEcho(); break;
            case fc::cmd::GetMixer: reply(cmd, {mixer}); break;
            case fc::cmd::SetBiasTee: bias = r[1]; setEcho(); break;
            case fc::cmd::GetBiasTee: reply(cmd, {bias}); break;
            default:
                if (plus && cmd == fc::cmd::SetIfGain) { ifGain = r[1]; setEcho(); break; }
                if (plus && cmd == fc::cmd::GetIfGain) { reply(cmd, {ifGain}); break; }
                if (!plus) {
                    bool done = false;
                    for (int i = 0; i < 6 && !done; ++i) {
                        if (cmd == fc::cmd::SetIfStage[i]) { stage[i] = r[1]; setEcho(); done = true; }
                        else if (cmd == fc::cmd::GetIfStage[i]) { reply(cmd, {stage[i]}); done = true; }
                    }
                    if (done) { break; }
                }
                break;  // unknown commands: no answer
        }
    }
};

class FakeHid final : public HidLink {
public:
    explicit FakeHid(std::shared_ptr<Dongle> d) : d_(std::move(d)) {}
    bool write(const std::uint8_t* r, std::string& error) override {
        std::lock_guard<std::mutex> lk(d_->mu);
        if (d_->failWrite || d_->closed) { error = "fake: write failed"; return false; }
        d_->handle(r);
        d_->cv.notify_all();
        return true;
    }
    int read(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        std::unique_lock<std::mutex> lk(d_->mu);
        if (d_->failRead) { return -1; }
        if (d_->inbox.empty() && timeoutMs > 0) {
            d_->cv.wait_for(lk, std::chrono::milliseconds(std::min(timeoutMs, 40u)), [&] { return !d_->inbox.empty(); });
        }
        if (d_->inbox.empty()) { return 0; }
        auto r = d_->inbox.front();
        d_->inbox.pop_front();
        const std::size_t n = std::min(cap, r.size());
        std::copy(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(n), dst);
        return static_cast<int>(n);
    }
    void close() override {
        std::lock_guard<std::mutex> lk(d_->mu);
        d_->closed = true;
    }

private:
    std::shared_ptr<Dongle> d_;
};

// ---- the stand-in sound input -----------------------------------------------------------------------------------
class FakeAudio final : public SoundCardBackend {
public:
    std::vector<SoundCardDevice> devices;
    double toneHz = 12000.0;
    std::atomic<int> opens{0};
    double lastRate = 0.0;
    int lastChannels = 0;
    ~FakeAudio() override { close(); }
    std::vector<SoundCardDevice> listDevices() override { return devices; }
    bool open(const SoundCardDevice&, int channels, double rateHz, bool, PushFn push, void* user, std::string&) override {
        close();
        ++opens;
        lastRate = rateHz;
        lastChannels = channels;
        push_ = push;
        user_ = user;
        run_ = true;
        th_ = std::thread([this] {
            const std::size_t block = static_cast<std::size_t>(lastRate / 100.0);
            std::vector<float> buf(block * static_cast<std::size_t>(lastChannels));
            std::mt19937 rng(7);
            std::normal_distribution<float> nd(0.0f, 0.003f);
            double ph = 0.0;
            const double w = 2.0 * 3.14159265358979323846 * toneHz / lastRate;
            while (run_) {
                for (std::size_t i = 0; i < block; ++i) {
                    buf[i * static_cast<std::size_t>(lastChannels)] = 0.5f * static_cast<float>(std::cos(ph)) + nd(rng);
                    buf[i * static_cast<std::size_t>(lastChannels) + 1] = 0.5f * static_cast<float>(std::sin(ph)) + nd(rng);
                    ph += w;
                }
                push_(user_, buf.data(), block);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        return true;
    }
    void close() override {
        run_ = false;
        if (th_.joinable()) { th_.join(); }
    }
    bool alive() override { return run_; }

private:
    PushFn push_ = nullptr;
    void* user_ = nullptr;
    std::atomic<bool> run_{false};
    std::thread th_;
};

SoundCardDevice card(const std::string& name, int channels, std::vector<double> rates) {
    SoundCardDevice d;
    d.index = 0;
    d.name = name;
    d.hostApi = "Windows WASAPI";
    d.maxInputChannels = channels;
    d.defaultRateHz = 48000.0;
    for (double r : rates) { d.rates.push_back({r, false}); }
    return d;
}

struct Rig {
    std::shared_ptr<Dongle> dongle = std::make_shared<Dongle>();
    std::shared_ptr<FakeAudio> audio = std::make_shared<FakeAudio>();
    std::unique_ptr<FuncubeSource> src;
    int openCalls = 0;
    explicit Rig(bool plus = true, bool withCard = true) {
        dongle->plus = plus;
        if (withCard) { audio->devices.push_back(card("Microphone (FUNcube Dongle Test)", 2, {48000.0, 96000.0, 192000.0})); }
        auto a = audio;
        src = std::make_unique<FuncubeSource>([a] { return std::static_pointer_cast<SoundCardBackend>(a); });
        auto d = dongle;
        bool isPlus = plus;
        src->setHidForTest(
            [isPlus]() {
                FcdDeviceInfo i;
                i.path = "fake-path";
                i.isProPlus = isPlus;
                return std::vector<FcdDeviceInfo>{i};
            },
            [d, this](const std::string&, std::string&) -> std::unique_ptr<HidLink> {
                ++openCalls;
                {
                    std::lock_guard<std::mutex> lk(d->mu);
                    d->closed = false;  // a fresh link to the same dongle
                }
                return std::make_unique<FakeHid>(d);
            });
    }
};

std::vector<Cx> readSome(FuncubeSource& s, std::size_t n, int ms) {
    std::vector<Cx> v;
    std::vector<Cx> tmp(8192);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (v.size() < n && std::chrono::steady_clock::now() < end) {
        const std::size_t got = s.read(tmp.data(), tmp.size());
        v.insert(v.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return v;
}
double toneHzOf(const std::vector<Cx>& x, double fs) {
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t i = 1; i < x.size(); ++i) { acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(x[i - 1])); }
    return std::arg(acc) * fs / (2.0 * 3.14159265358979323846);
}

// ---- protocol ---------------------------------------------------------------------------------------------------------
void testProtocol() {
    // numbers as read from fcdcmd.h
    CHECK(fc::cmd::Query == 1 && fc::cmd::SetFrequencyHz == 101 && fc::cmd::GetFrequencyHz == 102 && fc::cmd::SetLna == 110 &&
          fc::cmd::SetMixer == 114 && fc::cmd::SetIfGain == 117 && fc::cmd::SetBiasTee == 126 && fc::cmd::GetLna == 150 &&
          fc::cmd::GetMixer == 154 && fc::cmd::GetIfGain == 157 && fc::cmd::GetBiasTee == 166 && fc::cmd::ResetToBootloader == 255);
    const std::uint8_t setStage[6] = {117, 120, 121, 123, 124, 125}, getStage[6] = {157, 160, 161, 163, 164, 165};
    for (int i = 0; i < 6; ++i) { CHECK(fc::cmd::SetIfStage[i] == setStage[i] && fc::cmd::GetIfStage[i] == getStage[i]); }
    CHECK(fc::kVid == 0x04D8 && fc::kPidProPlus == 0xFB31 && fc::kPidPro == 0xFB56 && fc::kRateProPlusHz == 192000.0 && fc::kRateProHz == 96000.0);
    // report layout: 14.074 MHz = 0x00D6C090, little endian after the command, zero padded to 64
    const auto r = fc::report(fc::cmd::SetFrequencyHz, {0x90, 0xC0, 0xD6, 0x00});
    CHECK(r.size() == 64 && r[0] == 101 && r[1] == 0x90 && r[2] == 0xC0 && r[3] == 0xD6 && r[4] == 0x00 && r[5] == 0 && r[63] == 0);
    CHECK(fc::trimmedHz(14074000.0, 0.0) == 0x00D6C090u);
    const auto longp = fc::report(1, std::vector<std::uint8_t>(100, 0xAB));
    CHECK(longp.size() == 64 && longp[63] == 0xAB && longp[0] == 1);
    // trim
    CHECK(fc::trimmedHz(100000000.0, 1.5) == 100000150u);
    CHECK(fc::trimmedHz(100000000.0, -2.0) == 99999800u);
    CHECK(fc::trimmedHz(-5.0, 0.0) == 0u && fc::trimmedHz(std::nan(""), 0.0) == 0u && fc::trimmedHz(1e12, 0.0) == 0xFFFFFFFFu);
    // bands
    CHECK(fc::tunable(150000.0) && !fc::tunable(149999.0) && fc::tunable(240000000.0) && !fc::tunable(240000001.0));
    CHECK(!fc::tunable(419999999.0) && fc::tunable(420000000.0) && fc::tunable(1900000000.0) && !fc::tunable(1900000001.0));
    CHECK(!fc::tunable(std::nan("")) && !fc::tunable(-1.0) && !fc::tunable(0.0));
    // replies
    std::uint8_t good[64] = {102, 1, 0x90, 0xC0, 0xD6, 0x00};
    fc::Reply rep;
    CHECK(fc::parseReply(good, 64, 102, rep) && rep.data[0] == 0x90 && fc::le32(rep.data) == 0x00D6C090u);
    CHECK(!fc::parseReply(good, 64, 101, rep) && !fc::parseReply(good, 2, 102, rep) && !fc::parseReply(nullptr, 64, 102, rep));
    // Pro LNA: nearest valid index, never 2 or 3
    double actual = 0;
    CHECK(fc::proLnaIndexFor(-5.0, actual) == 0 && actual == -5.0);
    CHECK(fc::proLnaIndexFor(-2.5, actual) == 1 && actual == -2.5);
    CHECK(fc::proLnaIndexFor(0.0, actual) == 4 && actual == 0.0);
    CHECK(fc::proLnaIndexFor(1.0, actual) == 4);
    CHECK(fc::proLnaIndexFor(2.0, actual) == 5 && actual == 2.5);
    CHECK(fc::proLnaIndexFor(30.0, actual) == 14 && fc::proLnaIndexFor(500.0, actual) == 14 && fc::proLnaIndexFor(-500.0, actual) == 0);
    for (double d = -10.0; d <= 35.0; d += 0.25) {
        const int i = fc::proLnaIndexFor(d, actual);
        CHECK(i != 2 && i != 3 && i >= 0 && i <= 14);
    }
    // Pro IF distribution: stage 4 is gain mod 3, stage 2 gives up 3 when stage 1 is set, every register in range,
    // total close to the request (characterisation of the ported tables; the E4000 datasheet was not opened)
    int worst = 0;
    int last = -1000;
    for (int g = fc::kProIfMin; g <= fc::kProIfMax; ++g) {
        std::uint8_t regs[6];
        fc::proIfStageRegisters(g, regs);
        CHECK(regs[0] <= 1 && regs[1] <= 6 && regs[2] <= 3 && regs[3] == g % 3 && regs[4] <= 4 && regs[5] <= 4);
        const int total = fc::proIfTotalDb(regs);
        worst = std::max(worst, std::abs(total - g));
        CHECK(total >= last - 3);  // never falls by more than a stage step as the request rises
        last = total;
    }
    CHECK(worst <= 6);
    std::printf("fcd pro IF distribution: worst |total - request| = %d dB over %d..%d\n", worst, fc::kProIfMin, fc::kProIfMax);
    CHECK(fc::proStageDb(0, 0) == -3 && fc::proStageDb(0, 1) == 6 && fc::proStageDb(0, 2) == 0 && fc::proStageDb(6, 0) == 0 && fc::proStageDb(4, 4) == 15);
}

// ---- control ------------------------------------------------------------------------------------------------------------
void testControlProPlus() {
    auto d = std::make_shared<Dongle>();
    FcdControl c(std::make_unique<FakeHid>(d), true);
    std::string e, text;
    CHECK(c.isProPlus() && c.query(text, e) && text == "FCDAPP test-dongle 18.06");
    CHECK(c.setFrequencyHz(14074000.0, 0.0, e));
    std::uint32_t hz = 0;
    CHECK(c.frequencyHz(hz, e) && hz == 14074000u);
    CHECK(c.setFrequencyHz(100000000.0, 2.0, e) && c.frequencyHz(hz, e) && hz == 100000200u);
    for (double bad : {0.0, 100000.0, 300000000.0, 2e9, -1.0, std::nan("")}) {
        e.clear();
        CHECK(!c.setFrequencyHz(bad, 0.0, e) && e.find("tunes") != std::string::npos);
    }
    CHECK(!c.setFrequencyHz(100e6, 5000.0, e) && !c.setFrequencyHz(100e6, std::nan(""), e));
    CHECK(hz == 100000200u);  // refusals sent nothing
    double g = -1;
    CHECK(c.setGainDb("LNA", 10.0, e) && c.readGainDb("LNA", g, e) && g == 10.0 && d->lna == 1);
    CHECK(c.setGainDb("LNA", 0.0, e) && c.readGainDb("LNA", g, e) && g == 0.0 && d->lna == 0);
    for (double bad : {5.0, 11.0, -10.0, std::nan("")}) { CHECK(!c.setGainDb("LNA", bad, e)); }
    CHECK(c.setGainDb("Mixer", 20.0, e) && c.readGainDb("Mixer", g, e) && g == 20.0 && d->mixer == 1);
    for (double bad : {10.0, 4.0, 21.0}) { CHECK(!c.setGainDb("Mixer", bad, e)); }
    for (int v : {0, 1, 30, 58, 59}) { CHECK(c.setGainDb("IF", v, e) && c.readGainDb("IF", g, e) && g == v && d->ifGain == v); }
    for (double bad : {-1.0, 60.0, 1.5, std::nan("")}) { CHECK(!c.setGainDb("IF", bad, e)); }
    CHECK(!c.setGainDb("RF", 0.0, e) && e.find("three gains") != std::string::npos);
    bool on = true;
    CHECK(c.biasTee(on, e) && on);  // the stand-in starts with it on
    CHECK(c.setBiasTee(false, e) && c.biasTee(on, e) && !on);
    // stale reports are not mistaken for the answer
    d->staleFirst = true;
    CHECK(c.setGainDb("IF", 42, e) && c.readGainDb("IF", g, e) && g == 42.0);
    CHECK(c.frequencyHz(hz, e) && hz == 100000200u);
    d->staleFirst = false;
    CHECK(c.setGainDb("IF", 44, e));
    {   // an old answer to the SAME command is left in the inbox: it must not be taken for the new one
        std::lock_guard<std::mutex> lk(d->mu);
        std::vector<std::uint8_t> old(fc::kReportBytes, 0);
        old[0] = fc::cmd::GetIfGain;
        old[1] = 1;
        old[2] = 9;
        d->inbox.push_back(old);
    }
    CHECK(c.readGainDb("IF", g, e) && g == 44.0);
    // a dongle that does not answer sets is fine; one that does not answer gets is not
    d->noSetReply = true;
    CHECK(c.setGainDb("IF", 7, e) && c.readGainDb("IF", g, e) && g == 7.0);
    d->silent = true;
    CHECK(c.setGainDb("IF", 8, e));  // sets need no answer
    CHECK(!c.readGainDb("IF", g, e) && e.find("did not answer") != std::string::npos);
    CHECK(!c.frequencyHz(hz, e) && !c.query(text, e));
    d->silent = false;
    d->failWrite = true;
    CHECK(!c.setGainDb("IF", 9, e) && e.find("failed") != std::string::npos);
    d->failWrite = false;
    d->failRead = true;
    CHECK(!c.readGainDb("IF", g, e) && e.find("failed") != std::string::npos);
    d->failRead = false;
    // the bootloader reset is never sent, by anything above
    CHECK(std::count(d->log.begin(), d->log.end(), fc::cmd::ResetToBootloader) == 0);
    CHECK(d->log.size() > 25);
}

void testControlPro() {
    auto d = std::make_shared<Dongle>();
    d->plus = false;
    FcdControl c(std::make_unique<FakeHid>(d), false);
    std::string e;
    double g = 0;
    CHECK(!c.isProPlus() && !c.setBiasTee(true, e) && e.find("Pro+") != std::string::npos);
    bool on = false;
    CHECK(!c.biasTee(on, e));
    CHECK(c.setGainDb("LNA", 2.4, e) && d->lna == 5 && c.readGainDb("LNA", g, e) && g == 2.5);
    CHECK(c.setGainDb("LNA", -100.0, e) && d->lna == 0 && c.readGainDb("LNA", g, e) && g == -5.0);
    d->lna = 2;  // an invalid register value the dongle reports
    CHECK(!c.readGainDb("LNA", g, e) && e.find("not in the table") != std::string::npos);
    d->lna = 99;
    CHECK(!c.readGainDb("LNA", g, e));
    CHECK(c.setGainDb("Mixer", 12.0, e) && d->mixer == 1 && c.readGainDb("Mixer", g, e) && g == 12.0);
    CHECK(c.setGainDb("Mixer", 4.0, e) && d->mixer == 0 && c.readGainDb("Mixer", g, e) && g == 4.0);
    for (double bad : {0.0, 8.0, 20.0}) { CHECK(!c.setGainDb("Mixer", bad, e)); }
    d->log.clear();
    CHECK(c.setGainDb("IF", 30, e));
    CHECK(d->log.size() == 6);  // six stage registers
    for (int i = 0; i < 6; ++i) { CHECK(d->log[static_cast<std::size_t>(i)] == fc::cmd::SetIfStage[i]); }
    std::uint8_t regs[6];
    fc::proIfStageRegisters(30, regs);
    CHECK(std::equal(regs, regs + 6, d->stage));
    CHECK(c.readGainDb("IF", g, e) && g == fc::proIfTotalDb(regs));
    for (double bad : {2.0, 58.0, 1.5}) { CHECK(!c.setGainDb("IF", bad, e)); }
    for (int v = fc::kProIfMin; v <= fc::kProIfMax; ++v) { CHECK(c.setGainDb("IF", v, e)); }
    CHECK(std::count(d->log.begin(), d->log.end(), fc::cmd::ResetToBootloader) == 0);
    // a stage register the dongle reports out of range reads as 0 dB, not as garbage memory
    d->stage[4] = 200;
    CHECK(c.readGainDb("IF", g, e));
}

// ---- the source ---------------------------------------------------------------------------------------------------------------
void testSourceProPlus() {
    Rig rig(true);
    FuncubeSource& s = *rig.src;
    CHECK(!s.isOpen() && s.gains().empty() && s.antennas().empty() && !s.start() && !s.setCenterFrequencyHz(1e8));
    CHECK(s.open(""));
    CHECK(s.isOpen() && s.isProPlus() && std::string(s.name()) == "FUNcube Dongle Pro+");
    CHECK(s.sampleRateHz() == 192000.0 && s.supportedSampleRatesHz() == std::vector<double>{192000.0});
    CHECK(rig.audio->lastRate == 192000.0 && rig.audio->lastChannels == 2);
    {
        std::lock_guard<std::mutex> lk(rig.dongle->mu);
        CHECK(rig.dongle->bias == 0);              // switched OFF at open (it was left on)
        CHECK(rig.dongle->freq == 100000000u);     // tuned to the default
    }
    // gains read back from the dongle
    const auto g = s.gains();
    CHECK(g.size() == 3 && g[0].name == "LNA" && g[0].maxDb == 10.0 && g[1].name == "Mixer" && g[1].maxDb == 20.0 && g[2].name == "IF" && g[2].maxDb == 59.0);
    CHECK(s.setGainDb("LNA", 10.0) && s.gainDb("LNA") == 10.0);
    CHECK(s.setGainDb("Mixer", 20.0) && s.gainDb("Mixer") == 20.0);
    CHECK(s.setGainDb("IF", 25.0) && s.gainDb("IF") == 25.0);
    CHECK(!s.setGainDb("IF", 99.0) && !s.setGainDb("Foo", 1.0));
    double lo, hi;
    CHECK(s.frequencyRangeHz(lo, hi) && lo == 150000.0 && hi == 1900000000.0);
    CHECK(!s.setCenterFrequencyHz(300000000.0) && std::string(s.lastError()).find("tunes") != std::string::npos && s.centerFrequencyHz() == 100000000.0);
    CHECK(s.setCenterFrequencyHz(14074000.0) && s.centerFrequencyHz() == 14074000.0);
    CHECK(s.hasFrequencyCorrection() && s.setFrequencyCorrectionPpm(3.0));
    {
        std::lock_guard<std::mutex> lk(rig.dongle->mu);
        CHECK(rig.dongle->freq == 14074042u);  // 14074000 x (1 + 3e-6), truncated
    }
    CHECK(!s.setFrequencyCorrectionPpm(2000.0));
    CHECK(s.setSampleRateHz(192000.0) && !s.setSampleRateHz(96000.0) && std::string(s.lastError()).find("192") != std::string::npos);
    // antenna = bias tee
    CHECK(s.antennas() == std::vector<std::string>({"Bias tee off", "Bias tee on"}) && s.antenna() == "Bias tee off");
    CHECK(!s.setAntenna("Bias tee maybe"));
    CHECK(s.setAntenna("Bias tee on") && s.antenna() == "Bias tee on");
    {
        std::lock_guard<std::mutex> lk(rig.dongle->mu);
        CHECK(rig.dongle->bias == 1);
    }
    // stream: a tone at +12 kHz in the I/Q pair
    CHECK(s.start() && s.running());
    const std::vector<Cx> x = readSome(s, 60000, 8000);
    CHECK(x.size() >= 60000);
    CHECK(std::fabs(toneHzOf(x, 192000.0) - 12000.0) < 20.0);
    double p = 0;
    for (const Cx& c : x) { p += std::norm(c); }
    CHECK(p / static_cast<double>(x.size()) > 0.2);
    s.stop();
    // closing leaves the antenna input unpowered
    s.closeDevice();
    CHECK(!s.isOpen());
    {
        std::lock_guard<std::mutex> lk(rig.dongle->mu);
        CHECK(rig.dongle->bias == 0 && rig.dongle->closed);
        CHECK(std::count(rig.dongle->log.begin(), rig.dongle->log.end(), fc::cmd::ResetToBootloader) == 0);
    }
    CHECK(rig.audio->opens == 1);
}

void testSourceSwapAndPro() {
    {   // swapiq=1 mirrors the spectrum
        Rig rig(true);
        CHECK(rig.src->open("swapiq=1") && rig.src->start());
        const std::vector<Cx> x = readSome(*rig.src, 40000, 8000);
        CHECK(x.size() >= 40000 && toneHzOf(x, 192000.0) < -11900.0);
        rig.src->closeDevice();
    }
    {   // the Pro: 96 kS/s, no bias tee, E4000 gains
        Rig rig(false);
        rig.audio->devices[0] = card("FUNcube Dongle V1.0 Input", 2, {96000.0});
        FuncubeSource& s = *rig.src;
        CHECK(s.open("ppm=1.0"));
        CHECK(!s.isProPlus() && std::string(s.name()) == "FUNcube Dongle Pro" && s.sampleRateHz() == 96000.0 && rig.audio->lastRate == 96000.0);
        CHECK(s.antennas().empty() && s.antenna().empty() && !s.setAntenna("Bias tee on"));
        const auto g = s.gains();
        CHECK(g.size() == 3 && g[0].minDb == -5.0 && g[0].maxDb == 30.0 && g[1].minDb == 4.0 && g[2].minDb == 3.0 && g[2].maxDb == 57.0);
        CHECK(s.setGainDb("LNA", 7.4) && s.gainDb("LNA") == 7.5);
        CHECK(s.setGainDb("Mixer", 12.0) && s.gainDb("Mixer") == 12.0);
        CHECK(s.setGainDb("IF", 30.0));
        std::uint8_t regs[6];
        fc::proIfStageRegisters(30, regs);
        CHECK(s.gainDb("IF") == fc::proIfTotalDb(regs));
        CHECK(s.setCenterFrequencyHz(145000000.0));
        {
            std::lock_guard<std::mutex> lk(rig.dongle->mu);
            CHECK(rig.dongle->freq == 145000145u);
        }
        s.closeDevice();
    }
}

void testSourceRefusals() {
    {   // no dongle on HID
        Rig rig(true);
        rig.src->setHidForTest([] { return std::vector<FcdDeviceInfo>{}; }, nullptr);
        CHECK(!rig.src->open("") && std::string(rig.src->lastError()).find("no FUNcube Dongle") != std::string::npos);
    }
    {   // the index
        Rig rig(true);
        CHECK(!rig.src->open("index=1") && std::string(rig.src->lastError()).find("too high") != std::string::npos);
        CHECK(!rig.src->open("index=x") && !rig.src->open("index=-1") && !rig.src->open("index=99"));
        CHECK(!rig.src->open("ppm=5000") && !rig.src->open("ppm=x") && !rig.src->open("swapiq=2"));
    }
    {   // HID opens but the dongle does not answer the query
        Rig rig(true);
        rig.dongle->silent = true;
        CHECK(!rig.src->open("") && std::string(rig.src->lastError()).find("does not look like a FUNcube") != std::string::npos);
        CHECK(!rig.src->isOpen());
    }
    {   // the HID side cannot be opened
        Rig rig(true);
        rig.src->setHidForTest([] { FcdDeviceInfo i; i.path = "p"; return std::vector<FcdDeviceInfo>{i}; },
                               [](const std::string&, std::string& err) -> std::unique_ptr<HidLink> { err = "the control interface is busy"; return nullptr; });
        CHECK(!rig.src->open("") && std::string(rig.src->lastError()) == "the control interface is busy");
    }
    {   // no sound input of that name
        Rig rig(true, false);
        CHECK(!rig.src->open("") && std::string(rig.src->lastError()).find("audio=") != std::string::npos);
        rig.audio->devices.push_back(card("Microphone (Realtek)", 2, {48000.0}));
        CHECK(!rig.src->open(""));
        CHECK(!rig.src->open("audio=Nothing Here") && std::string(rig.src->lastError()).find("not found") != std::string::npos);
        CHECK(rig.src->open("audio=Microphone (Realtek)") == false);  // named, but it does not offer 192 kS/s
        CHECK(std::string(rig.src->lastError()).find("192") != std::string::npos);
    }
    {   // two FUNcube inputs: no guessing
        Rig rig(true);
        rig.audio->devices.push_back(card("Line (FUNcube Dongle Second)", 2, {192000.0}));
        CHECK(!rig.src->open("") && std::string(rig.src->lastError()).find("2 sound inputs") != std::string::npos);
        CHECK(rig.src->open("audio=Line (FUNcube Dongle Second)"));
        rig.src->closeDevice();
    }
    {   // a mono input is not the dongle
        Rig rig(true, false);
        rig.audio->devices.push_back(card("FUNcube mono", 1, {192000.0}));
        CHECK(!rig.src->open("") && std::string(rig.src->lastError()).find("stereo") != std::string::npos);
    }
    {   // reopen cycles leave nothing open
        Rig rig(true);
        for (int i = 0; i < 20; ++i) {
            CHECK(rig.src->open(""));
            CHECK(rig.src->start());
            rig.src->stop();
            rig.src->closeDevice();
            CHECK(!rig.src->isOpen());
        }
    }
}

void testWindowsEnumeration() {
    const auto found = enumerateFcdHid();
    for (const FcdDeviceInfo& f : found) {
        std::string l = f.path;
        for (char& c : l) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        CHECK(l.find("vid_04d8&pid_fb31") != std::string::npos || l.find("vid_04d8&pid_fb56") != std::string::npos);
    }
    std::printf("fcd HID enumeration on this machine: %zu dongle interface(s)\n", found.size());
    std::string err;
    CHECK(openFcdHid("\\\\?\\hid#vid_dead&pid_beef#7&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}", err) == nullptr);
    CHECK(err.find("not a FUNcube") != std::string::npos || err.find("Windows only") != std::string::npos);
}

}  // namespace

int main() {
    testProtocol();
    testControlProPlus();
    testControlPro();
    testSourceProPlus();
    testSourceSwapAndPro();
    testSourceRefusals();
    testWindowsEnumeration();
    return testSummary("test_funcube");
}
