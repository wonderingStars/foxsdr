// test_engine_api_wire_c_power.cpp - keep awake, the resource read-out with the reduced-effects policy, and wake-from-sleep, in the real engine (wiring lane C rows
// S4, S5, S3: src/engine/wire_c_power.{hpp,cpp}, engine_wire_c.cpp). Every OS call is a stand-in the test owns: no power setting is changed, no counter is read, no
// notice is registered with Windows.
//
//   K1  an audio take holds ONE keep-awake lease for exactly as long as it runs (set with ES_CONTINUOUS | ES_SYSTEM_REQUIRED, never the display; cleared when it
//       stops); an I/Q take does the same; sys.keep_awake off holds nothing; an OS that refuses is fact.sys.keepawake 0 and one log line
//   K2  the resource facts: scripted readings arrive as fact.perf.*; an unavailable source is "" and never 0; high readings turn fact.perf.reduced on after the policy's
//       streak, calm ones turn it off, perf.mode on / off overrides; the policy switches are published
//   K3  wake from sleep: a resume event after the settle time makes the engine look again (fact.sys.resume.count moves once); a suspend cancels a pending one; with
//       sys.resume_reopen off the count stays where it was
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <thread>

#include "wire_c_rig.hpp"

using namespace wirec_test;
namespace sys = cascade::core::sys;

namespace {

struct FakeOs {
    std::atomic<unsigned long> last{0};
    std::atomic<int> sets{0};
    std::atomic<int> clears{0};
    std::atomic<bool> refuse{false};
    sys::KeepAwake::Fns fns() {
        sys::KeepAwake::Fns f;
        f.setState = [this](unsigned long flags) -> unsigned long {
            if (flags == sys::kEsContinuous) { ++clears; last = flags; return 1; }
            ++sets;
            last = flags;
            return refuse.load() ? 0ul : 1ul;
        };
        return f;
    }
};

template <class Pred>
bool waitFor(Rig& r, Pred p, int ms = 4000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        r.pass(1);
        if (p()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p();
}

FoxCommand op(std::uint32_t o, std::int64_t a = 0) { return testing::cmd(o, 0.0, a, nullptr); }

void testKeepAwake(const fs::path& dir) {
    FakeOs os;
    Rig r(dir, [&os](cascade::engine::wirec::State& s) {
        s.powerSeams.keepAwake = os.fns();
        s.powerSeams.samplePeriodMs = 20;
        s.powerSeams.registerResumeWithOs = false;
    });
    r.pass(3);
    CHECK(r.fact("sys.keepawake") == "0");
    CHECK(os.sets.load() == 0);
    CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
    // an audio take
    CHECK(r.status(op(FOXAPI_OP_RECORD_AUDIO, 1)) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return r.fact("sys.keepawake") == "1"; }));
    CHECK(os.sets.load() == 1);
    CHECK(os.last.load() == (sys::kEsContinuous | sys::kEsSystemRequired));   // the machine, never the display
    CHECK((os.last.load() & sys::kEsDisplayRequired) == 0u);
    r.pass(10);
    CHECK(os.sets.load() == 1);   // one lease however many passes
    CHECK(r.status(op(FOXAPI_OP_RECORD_AUDIO, 0)) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return r.fact("sys.keepawake") == "0"; }));
    CHECK(os.clears.load() >= 1);
    // an I/Q take
    const int setsBefore = os.sets.load();
    CHECK(r.status(op(FOXAPI_OP_RECORD_IQ, 1)) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return r.fact("sys.keepawake") == "1"; }));
    CHECK(os.sets.load() == setsBefore + 1);
    CHECK(r.status(op(FOXAPI_OP_RECORD_IQ, 0)) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return r.fact("sys.keepawake") == "0"; }));
    // opt out: a take holds nothing
    CHECK(r.status(setting("sys.keep_awake", "0")) == FOXAPI_OK);
    const int setsOptOut = os.sets.load();
    CHECK(r.status(op(FOXAPI_OP_RECORD_AUDIO, 1)) == FOXAPI_OK);
    r.pass(20);
    CHECK(r.fact("sys.keepawake") == "0");
    CHECK(os.sets.load() == setsOptOut);
    CHECK(r.status(op(FOXAPI_OP_RECORD_AUDIO, 0)) == FOXAPI_OK);
}

void testKeepAwakeRefused(const fs::path& dir) {
    FakeOs os;
    os.refuse = true;
    Rig r(dir, [&os](cascade::engine::wirec::State& s) {
        s.powerSeams.keepAwake = os.fns();
        s.powerSeams.samplePeriodMs = 20;
        s.powerSeams.registerResumeWithOs = false;
    });
    CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
    CHECK(r.status(op(FOXAPI_OP_RECORD_AUDIO, 1)) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return os.sets.load() >= 1; }));   // (the take opens on a worker: the lease follows when it runs)
    r.pass(10);
    if (os.sets.load() != 1) { std::printf("      sets = %d\n", os.sets.load()); }
    CHECK(os.sets.load() >= 1);          // it asked
    CHECK(r.fact("sys.keepawake") == "0");   // the OS said no, and the fact says so
    CHECK(r.status(op(FOXAPI_OP_RECORD_AUDIO, 0)) == FOXAPI_OK);
}

// scripted readings
struct Script {
    std::atomic<double> memoryPercent{40.0};
    std::atomic<bool> gpuAvailable{false};
    std::atomic<std::int64_t> now{0};
    sys::ResourceSources sources() {
        sys::ResourceSources s;
        s.processCpu = [this](std::uint64_t& t, unsigned& cpus) { cpus = 4; now += 100; t = static_cast<std::uint64_t>(now.load()) * 10000ull * 4ull / 10ull; return true; };   // 10 % of the machine
        s.workingSet = [](std::uint64_t& b) { b = 123ull * 1024ull * 1024ull; return true; };
        s.systemMemory = [this](sys::SystemMemory& m) { m.loadPercent = memoryPercent.load(); m.totalBytes = 16ull << 30; m.availableBytes = 8ull << 30; return true; };
        s.gpuInstances = [this](std::vector<sys::GpuInstance>& v) {
            if (!gpuAvailable.load()) { return false; }
            v.push_back({"pid_1_luid_0x0_engtype_3D", 55.0});
            return true;
        };
        s.nowMs = [this]() -> std::int64_t { return now.load(); };
        s.processId = []() -> std::uint32_t { return 1; };
        return s;
    }
};

void testResources(const fs::path& dir) {
    Script sc;
    Rig r(dir, [&sc](cascade::engine::wirec::State& s) {
        s.powerSeams.sources = sc.sources();
        s.powerSeams.samplePeriodMs = 5;
        s.powerSeams.registerResumeWithOs = false;
    });
    CHECK(waitFor(r, [&] { return !r.fact("perf.memory_mb").empty() && r.fact("perf.memory_mb") != "<absent>"; }));
    CHECK(r.fact("perf.memory_mb") == "123");
    CHECK(r.fact("perf.system_memory_percent") == "40");
    CHECK(r.fact("perf.gpu_percent").empty());            // no GPU counters here: "", never 0
    CHECK(r.fact("perf.reduced") == "0");
    CHECK(r.fact("perf.switch.animations") == "1");
    // a GPU that answers
    sc.gpuAvailable = true;
    CHECK(waitFor(r, [&] { return r.fact("perf.gpu_percent") == "55"; }));
    // high memory: reduced after the policy's streak of high samples
    sc.memoryPercent = 96.0;
    CHECK(waitFor(r, [&] { return r.fact("perf.reduced") == "1"; }));
    CHECK(r.fact("perf.switch.animations") == "0");
    CHECK(r.fact("perf.switch.ui_frame_cap_hz") == "30");
    CHECK(r.fact("perf.switch.waterfall_rows_per_second") == "15");
    // the user's override wins in both directions
    CHECK(r.status(setting("perf.mode", "off")) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return r.fact("perf.reduced") == "0"; }));
    CHECK(r.status(setting("perf.mode", "on")) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return r.fact("perf.reduced") == "1"; }));
    // auto again with calm readings: back to full effects after the longer calm streak
    sc.memoryPercent = 30.0;
    CHECK(r.status(setting("perf.mode", "auto")) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return r.fact("perf.reduced") == "0"; }, 8000));
}

void testResume(const fs::path& dir) {
    FakeOs os;
    Rig r(dir, [&os](cascade::engine::wirec::State& s) {
        s.powerSeams.keepAwake = os.fns();
        s.powerSeams.samplePeriodMs = 50;
        s.powerSeams.registerResumeWithOs = false;
        s.powerSeams.resume.settleMs = 30;
        s.powerSeams.resume.minIntervalMs = 0;
    });
    r.pass(5);
    auto& power = Access::wirec(r.eng).power();
    CHECK(power.resumeWatchStarted());
    CHECK(r.fact("sys.resume.count") == "0");
    power.injectPowerEvent(sys::PowerEvent::ResumeAutomatic);
    CHECK(waitFor(r, [&] { return r.fact("sys.resume.count") == "1"; }));
    // a suspend that follows a resume cancels the action that was pending
    power.injectPowerEvent(sys::PowerEvent::ResumeAutomatic);
    power.injectPowerEvent(sys::PowerEvent::Suspend);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    r.pass(10);
    CHECK(r.fact("sys.resume.count") == "1");
    // switched off: the event is taken and nothing is done
    CHECK(r.status(setting("sys.resume_reopen", "0")) == FOXAPI_OK);
    power.injectPowerEvent(sys::PowerEvent::ResumeAutomatic);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    r.pass(10);
    CHECK(r.fact("sys.resume.count") == "1");
    CHECK(power.resumeCount() >= 2u);
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c_power");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("power");
    int n = 0;
    for (auto fn : {testKeepAwake, testKeepAwakeRefused, testResources, testResume}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_wire_c_power");
}
