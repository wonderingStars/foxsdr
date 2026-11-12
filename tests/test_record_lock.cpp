// Lock while recording (core/record_lock.hpp): the frozen-settings table for every kind, eight independent radios, same-value requests, retune-logging
// formats, policy switches, refusal texts, concurrency and a randomised comparison with a plain reference model.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

#include "core/record_lock.hpp"
#include "test_check.hpp"

using namespace cascade::core::reclock;

namespace {

Frozen frozenOf() {
    Frozen f;
    f.centerHz = 7.1e6; f.sampleRateHz = 2.4e6; f.correctionPpm = 1.5; f.converterHz = 0.0;
    f.channelHz = 7.1e6 + 5000; f.channelBandwidthHz = 2700; f.audioRateHz = 48000;
    f.sourceId = 3; f.channelMode = 2;
    return f;
}

// the table of what must be frozen, written out by hand: [kind][setting]
bool expectFrozen(RecordKind k, Setting s, bool logs) {
    const bool iq = k == RecordKind::BasebandIq, ch = k == RecordKind::ChannelIf, au = k == RecordKind::Audio;
    switch (s) {
        case Setting::SampleRate: case Setting::Source: case Setting::ConverterOffset: return iq || ch;
        case Setting::CenterFrequency: case Setting::FrequencyCorrection: return (iq && !logs) || ch;
        case Setting::CloseRadio: return true;
        case Setting::ChannelFrequency: case Setting::ChannelMode: case Setting::ChannelBandwidth: return ch;
        case Setting::AudioRate: return au;
        default: return false;
    }
}

double differentValue(Setting s, const Frozen& f) {
    switch (s) {
        case Setting::CenterFrequency: return f.centerHz + 1000.0;
        case Setting::SampleRate: return f.sampleRateHz * 2;
        case Setting::FrequencyCorrection: return f.correctionPpm + 1;
        case Setting::ConverterOffset: return f.converterHz + 125e6;
        case Setting::Source: return f.sourceId + 1;
        case Setting::CloseRadio: return 1.0;
        case Setting::ChannelFrequency: return f.channelHz + 100;
        case Setting::ChannelMode: return f.channelMode + 1;
        case Setting::ChannelBandwidth: return f.channelBandwidthHz + 50;
        case Setting::AudioRate: return 44100;
        default: return 6.0;
    }
}
double sameValue(Setting s, const Frozen& f) {
    switch (s) {
        case Setting::CenterFrequency: return f.centerHz;
        case Setting::SampleRate: return f.sampleRateHz;
        case Setting::FrequencyCorrection: return f.correctionPpm;
        case Setting::ConverterOffset: return f.converterHz;
        case Setting::Source: return f.sourceId;
        case Setting::ChannelFrequency: return f.channelHz;
        case Setting::ChannelMode: return f.channelMode;
        case Setting::ChannelBandwidth: return f.channelBandwidthHz;
        case Setting::AudioRate: return f.audioRateHz;
        default: return 0.0;
    }
}

void testTable() {
    const Frozen f = frozenOf();
    int rows = 0;
    for (RecordKind k : {RecordKind::BasebandIq, RecordKind::ChannelIf, RecordKind::Audio}) {
        for (bool logs : {false, true}) {
            RecordLock L;
            CHECK(L.recordingStarted(2, k, f, logs));
            for (int i = 0; i < static_cast<int>(Setting::Count); ++i) {
                const Setting s = static_cast<Setting>(i);
                const Verdict v = L.check(2, s, differentValue(s, f));
                const bool frozen = expectFrozen(k, s, logs);
                CHECK(v.allowed == !frozen);
                CHECK(v.reason.empty() == v.allowed);
                if (!v.allowed) {
                    CHECK(v.reason.find(settingName(s)) != std::string::npos);
                    CHECK(v.reason.find(kindName(k)) != std::string::npos);
                    CHECK(v.reason.find("Stop the recording first") != std::string::npos);
                }
                // the same value is never refused (except closing the radio and gain, which have no "same")
                if (s != Setting::CloseRadio && s != Setting::Gain) { CHECK(L.check(2, s, sameValue(s, f)).allowed); }
                ++rows;
            }
            // nothing frozen on another radio, nothing frozen after the stop
            for (int i = 0; i < static_cast<int>(Setting::Count); ++i) { CHECK(L.check(5, static_cast<Setting>(i), 1e9).allowed); }
            CHECK(L.recordingStopped(2));
            for (int i = 0; i < static_cast<int>(Setting::Count); ++i) { CHECK(L.check(2, static_cast<Setting>(i), 1e9).allowed); }
        }
    }
    std::printf("table rows checked: %d\n", rows);
}

void testRadiosAndLifecycle() {
    RecordLock L;
    const Frozen f = frozenOf();
    for (int r = 0; r < kMaxRadios; ++r) { CHECK(L.recordingStarted(r, RecordKind::BasebandIq, f)); CHECK(L.recording(r)); }
    CHECK(!L.recordingStarted(0, RecordKind::Audio, f));          // already open: nothing changes
    CHECK(!L.check(0, Setting::SampleRate, 1.0).allowed);          // still the I/Q kind
    CHECK(!L.recordingStarted(-1, RecordKind::Audio, f) && !L.recordingStarted(kMaxRadios, RecordKind::Audio, f));
    CHECK(!L.recordingStopped(-1) && !L.recordingStopped(99));
    CHECK(L.recordingStopped(4));
    CHECK(!L.recordingStopped(4));                                 // idempotent
    CHECK(L.check(4, Setting::SampleRate, 1.0).allowed);
    CHECK(!L.check(3, Setting::SampleRate, 1.0).allowed);
    CHECK(!L.check(-1, Setting::Gain, 1.0).allowed && !L.check(8, Setting::Gain, 1.0).allowed);
    CHECK(!L.check(-1, Setting::Gain, 1.0).reason.empty());
    CHECK(!L.check(0, static_cast<Setting>(99), 1.0).allowed);
    CHECK(!L.recording(-1) && !L.recording(8));
    // NaN / infinity are never "the same value"
    CHECK(!L.check(0, Setting::SampleRate, std::nan("")).allowed);
    CHECK(!L.check(0, Setting::CenterFrequency, HUGE_VAL).allowed);
}

void testPolicy() {
    const Frozen f = frozenOf();
    RecordLock L;
    L.recordingStarted(0, RecordKind::BasebandIq, f);
    CHECK(L.check(0, Setting::Gain, 30).allowed);                   // default: gain is free
    Policy p; p.lockGain = true; L.setPolicy(p);
    CHECK(!L.check(0, Setting::Gain, 30).allowed);
    CHECK(L.check(0, Setting::Gain, 30).reason.find("calibrated") != std::string::npos);
    p.lockWhileRecording = false; L.setPolicy(p);                    // master switch off: nothing frozen
    for (int i = 0; i < static_cast<int>(Setting::Count); ++i) { CHECK(L.check(0, static_cast<Setting>(i), 1e9).allowed); }
    CHECK(L.frozenSettings(0).empty());
    p.lockWhileRecording = true; L.setPolicy(p);
    const auto fz = L.frozenSettings(0);
    CHECK(!fz.empty());
    bool hasRate = false, hasGain = false;
    for (const auto& e : fz) {
        hasRate = hasRate || e.setting == Setting::SampleRate;
        hasGain = hasGain || e.setting == Setting::Gain;
        CHECK(!e.label.empty() && !e.reason.empty());
    }
    CHECK(hasRate && hasGain);
    CHECK(L.frozenSettings(1).empty() && L.frozenSettings(-1).empty());
}

void testConcurrency() {
    RecordLock L;
    const Frozen f = frozenOf();
    std::atomic<bool> stop{false};
    std::atomic<long> calls{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t) {
        th.emplace_back([&, t] {
            std::mt19937 g(100 + t);
            while (!stop.load()) {
                const int r = static_cast<int>(g() % kMaxRadios);
                const unsigned pick = g() % 3;
                if (pick == 0) { L.recordingStarted(r, static_cast<RecordKind>(g() % 3), f, g() % 2 == 0); }
                else if (pick == 1) { L.recordingStopped(r); }
                else {
                    (void)L.check(r, static_cast<Setting>(g() % static_cast<unsigned>(Setting::Count)), 1.0 * (g() % 5));
                    (void)L.frozenSettings(r);
                }
                ++calls;
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop = true;
    for (auto& t : th) { t.join(); }
    for (int r = 0; r < kMaxRadios; ++r) { L.recordingStopped(r); }
    bool clean = true;
    for (int r = 0; r < kMaxRadios; ++r) { clean = clean && !L.recording(r); }
    std::printf("concurrency: %ld calls\n", calls.load());
    CHECK(clean);
    CHECK(calls.load() > 1000);
}

void testRandomAgainstReference() {
    struct Ref { bool open = false; RecordKind k = RecordKind::BasebandIq; bool logs = false; };
    std::mt19937 g(31337);
    RecordLock L;
    Ref ref[kMaxRadios];
    const Frozen f = frozenOf();
    long compared = 0;
    bool ok = true;
    for (int i = 0; i < 40000; ++i) {
        const int r = static_cast<int>(g() % kMaxRadios);
        switch (g() % 4) {
            case 0: {
                const RecordKind k = static_cast<RecordKind>(g() % 3);
                const bool logs = g() % 2 == 0;
                const bool did = L.recordingStarted(r, k, f, logs);
                ok = ok && did == !ref[r].open;
                if (did) { ref[r].open = true; ref[r].k = k; ref[r].logs = logs; }
                break;
            }
            case 1: {
                const bool did = L.recordingStopped(r);
                ok = ok && did == ref[r].open;
                ref[r] = Ref();
                break;
            }
            default: {
                const Setting s = static_cast<Setting>(g() % static_cast<unsigned>(Setting::Count));
                const bool wantFrozen = ref[r].open && expectFrozen(ref[r].k, s, ref[r].logs) && s != Setting::Gain;
                ok = ok && L.check(r, s, differentValue(s, f)).allowed == !wantFrozen;
                ++compared;
            }
        }
    }
    std::printf("randomised: %ld checks against the reference model\n", compared);
    CHECK(ok);
}

}  // namespace

int main() {
    testTable();
    testRadiosAndLifecycle();
    testPolicy();
    testConcurrency();
    testRandomAgainstReference();
    return testSummary("test_record_lock");
}
