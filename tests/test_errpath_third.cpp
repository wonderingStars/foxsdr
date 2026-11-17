// Error-path third reading (docs/multi-radio-records/errpath-third.md): the failures the second review's doubted ALLOW lines turned out to hide, each made to happen against a
// stand-in and read back from the radio reporter's queue (a stand-in transport, a temporary folder; no radio, no network, no window).
//
//   1. applySourceRate (core/rate_follow_status.hpp): a rate the radio refuses is said once, by the engine's watch, when the driver did not say it, and not twice when it did;
//      a rate the driver COERCED on a call that succeeded is not a refusal and is not said. Every open, saved-radio, plugin-preset, API and patch-page rate goes through it.
//   2. PatchRadio::setCentreHz / setRadioPpm (core/patch_radio.cpp): a frequency or a frequency correction the patch page's radio refuses is said once, under the radio's own
//      identity; one the driver said itself is not said twice; one that is taken is not said at all.
//   3. Nothing a person could be identified by goes out with them: a hostile driver sentence (a frequency, a path, an address, a name) is scrubbed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/patch_radio.hpp"
#include "core/radio_events.hpp"
#include "core/radio_report.hpp"
#include "core/radio_reporter.hpp"
#include "core/rate_follow_status.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace rr = cascade::core::radioreport;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const char* const kInstallId = "a1b2c3d4e5f60718293a4b5c6d7e8f90";

struct Rig {
    fs::path dir;
    std::mutex mu;
    std::vector<std::string> bodies;
    Rig() {
        dir = fs::temp_directory_path() / ("ept-" + rr::randomHex(8));
        fs::create_directories(dir);
        rr::Reporter::Options o;
        o.dir = (dir / "radio-reports").string();
        o.endpoint = "http://127.0.0.1:9/";
        o.liveSend = true;
        o.transport = [this](const std::string&, const std::string& body) {
            std::lock_guard<std::mutex> lk(mu);
            bodies.push_back(body);
            return rr::PostResult{true, 204, 0, ""};
        };
        o.identity.installId = kInstallId;
        o.identity.version = "0.99.109";
        o.identity.os = "Windows 10.0.22631";
        o.identity.arch = "x64";
        o.identity.launches = 3;
        rr::global().configure(o);
    }
    ~Rig() {
        rr::global().shutdown();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // every message the reporter would send now, as JSON
    std::vector<json> flush() {
        {
            std::lock_guard<std::mutex> lk(mu);
            bodies.clear();
        }
        rr::global().runBatchNow();
        std::lock_guard<std::mutex> lk(mu);
        std::vector<json> out;
        for (const std::string& b : bodies) { out.push_back(json::parse(b)); }
        return out;
    }
    std::string raw() {
        std::lock_guard<std::mutex> lk(mu);
        std::string all;
        for (const std::string& b : bodies) { all += b + "\n"; }
        return all;
    }
};

std::vector<json> ofKind(const std::vector<json>& all, const char* kind) {
    std::vector<json> out;
    for (const json& m : all) {
        if (m.value("kind", std::string()) == kind) { out.push_back(m); }
    }
    return out;
}

// How many radio_settings events carry this `refused` word.
int refusedCount(const std::vector<json>& all, const std::string& refused) {
    int n = 0;
    for (const json& m : ofKind(all, "radio_settings")) {
        if (m["body"].value("refused", std::string()) == refused) { ++n; }
    }
    return n;
}

// A radio that refuses what it is told to. `saysItself`: it says so through the reporter at the place it notices, as a wired driver does.
class FakeRadio : public cascade::source::IqSource {
public:
    bool refuseRate = false, refuseFreq = false, refuseCorrection = false, coerceRate = false, saysItself = false;
    std::string text = "the radio would not take that";
    double rate = 2.4e6, centre = 100.0e6;
    std::string err;

    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return false; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate; }
    bool setSampleRateHz(double hz) override {
        if (refuseRate) { return refuse(rr::Control::Rate); }
        rate = coerceRate ? hz / 2.0 : hz;
        if (coerceRate) { err = "the radio ran at the nearest rate it has"; }
        return true;
    }
    double centerFrequencyHz() const override { return centre; }
    bool setCenterFrequencyHz(double hz) override {
        if (refuseFreq) { return refuse(rr::Control::Frequency); }
        centre = hz;
        return true;
    }
    bool hasFrequencyCorrection() const override { return true; }
    bool setFrequencyCorrectionPpm(double) override { return refuseCorrection ? refuse(rr::Control::Ppm) : true; }
    std::size_t read(std::complex<float>*, std::size_t) override { return 0; }
    const char* name() const override { return "fake"; }
    const char* lastError() const override { return err.c_str(); }

private:
    bool refuse(rr::Control c) {
        err = text;
        if (saysItself) { rr::noteControlRefused(this, "rtlsdr", c, text); }
        return false;
    }
};

// One identity per case: the reporter folds a refusal of the same radio and control repeated within five seconds, and the process is shared.
rr::RadioIdentity radioNo(int n) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "serial=0000%04d", n);
    return rr::identify("rtlsdr", buf);
}

}  // namespace

int main() {
    std::printf("test_errpath_third\n");

    // ---- 1. applySourceRate: a rate the radio refuses ----------------------------------------------------------------------------------------------
    {
        Rig t;
        FakeRadio r;
        rr::registerSource(&r, radioNo(1));
        r.refuseRate = true;
        const auto out = cascade::gui::applySourceRate(r, 3.2e6, std::string());
        CHECK(!out.ok);
        CHECK(out.sourceError == r.text);   // what the panel says is unchanged
        CHECK(refusedCount(t.flush(), "rate") == 1);   // and the owner is told, once

        // the driver said it itself (a wired driver): still exactly one, never two
        Rig t2;
        FakeRadio w;
        rr::registerSource(&w, radioNo(2));
        w.refuseRate = true;
        w.saysItself = true;
        cascade::gui::applySourceRate(w, 3.2e6, std::string());
        CHECK(refusedCount(t2.flush(), "rate") == 1);

        // a rate the driver coerced on a call that succeeded is not a refusal
        Rig t3;
        FakeRadio c;
        rr::registerSource(&c, radioNo(3));
        c.coerceRate = true;
        const auto got = cascade::gui::applySourceRate(c, 3.2e6, std::string());
        CHECK(got.ok);
        CHECK(refusedCount(t3.flush(), "rate") == 0);

        // an accepted rate says nothing
        Rig t4;
        FakeRadio a;
        rr::registerSource(&a, radioNo(4));
        CHECK(cascade::gui::applySourceRate(a, 2.0e6, std::string()).ok);
        CHECK(ofKind(t4.flush(), "radio_settings").empty());

        for (int n = 1; n <= 4; ++n) { rr::forgetRadio(radioNo(n).key); }
    }

    // ---- 2. the patch page's radio: a frequency or a correction it refuses --------------------------------------------------------------------------
    {
        Rig t;
        auto owned = std::make_unique<FakeRadio>();
        FakeRadio* radio = owned.get();
        rr::registerSource(radio, radioNo(5));
        cascade::core::patch::PatchRadio pr(1, std::move(owned), "fake");
        radio->refuseCorrection = true;
        CHECK(!pr.setRadioPpm(5.0));
        radio->refuseFreq = true;
        CHECK(!pr.setCentreHz(101.0e6));
        const std::vector<json> got = t.flush();
        CHECK(refusedCount(got, "ppm") == 1);
        CHECK(refusedCount(got, "frequency") == 1);

        // taken: nothing said
        Rig t2;
        radio->refuseCorrection = false;
        radio->refuseFreq = false;
        CHECK(pr.setRadioPpm(6.0));
        CHECK(pr.setCentreHz(102.0e6));
        CHECK(ofKind(t2.flush(), "radio_settings").empty());

        // the driver said it itself: once (a fresh radio identity: the same radio and control within five seconds is folded)
        rr::forgetRadio(radioNo(5).key);
        rr::registerSource(radio, radioNo(8));
        Rig t3;
        radio->saysItself = true;
        radio->refuseCorrection = true;
        CHECK(!pr.setRadioPpm(7.0));
        CHECK(refusedCount(t3.flush(), "ppm") == 1);
        rr::forgetRadio(radioNo(8).key);
    }

    // ---- 3. what never goes out: a hostile driver sentence, through both new routes -----------------------------------------------------------------
    {
        Rig t;
        FakeRadio r;
        rr::registerSource(&r, radioNo(6));
        r.refuseRate = true;
        r.text = "refused 433.920 MHz at C:\\Users\\alice\\Documents\\SecretFolder\\zz_secret_capture.iq via 192.168.7.77 and host studio-nas7, tuned to 14200";
        cascade::gui::applySourceRate(r, 3.2e6, std::string());
        auto owned = std::make_unique<FakeRadio>();
        FakeRadio* p = owned.get();
        rr::registerSource(p, radioNo(7));
        cascade::core::patch::PatchRadio pr(2, std::move(owned), "fake");
        p->refuseFreq = true;
        p->text = r.text;
        pr.setCentreHz(433.92e6);
        const std::vector<json> got = t.flush();
        CHECK(!ofKind(got, "radio_settings").empty());
        const std::string wire = t.raw();
        CHECK(wire.find("433.920") == std::string::npos);
        CHECK(wire.find("alice") == std::string::npos);
        CHECK(wire.find("SecretFolder") == std::string::npos);
        CHECK(wire.find("zz_secret_capture") == std::string::npos);
        CHECK(wire.find("192.168.7.77") == std::string::npos);
        CHECK(wire.find("studio-nas7") == std::string::npos);
        rr::forgetRadio(radioNo(6).key);
        rr::forgetRadio(radioNo(7).key);
    }

    return testSummary("test_errpath_third");
}
