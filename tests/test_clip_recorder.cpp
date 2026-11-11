// Tests for the squelch-clip logic (core/clip_recorder.hpp): the trigger state machine on synthetic gate
// traces, and the writer end to end on real files in a scratch folder (pre-roll, hang time, minimum
// length, splits at the maximum, names and time stamps, index.csv, an unwritable folder, a disk that
// refuses writes, a worker that falls behind the ring, a stop mid-clip, a worker thread).
//
// The audio is a RAMP: sample n of the ring is the 16-bit value n mod 20000, so any sample in any
// file says which ring sample it is and a file can be compared with the exact slice it must hold.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/clip_recorder.hpp"

#include "alloc_probe.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {

using cascade::core::ClipCmd;
using cascade::core::ClipEvent;
using cascade::core::ClipSettings;
using cascade::core::ClipTrigger;
using cascade::core::ClipWriter;
using cascade::core::ReplayRing;
namespace fs = std::filesystem;

constexpr double kRate = 48000.0;
constexpr std::size_t kPass = 480;                      // 10 ms
constexpr std::int64_t kT0 = 1767225600;                // 2026-01-01T00:00:00Z
std::int16_t rampInt(std::uint64_t n) { return static_cast<std::int16_t>(n % 20000); }
float rampValue(std::uint64_t n) { return static_cast<float>(n % 20000) / 32768.0f; }

// --- pure trigger helpers ----------------------------------------------------------------
struct TrigRun {
    ClipTrigger t;
    std::uint64_t head = 0;
    std::vector<ClipCmd> cmds;
    std::vector<std::uint64_t> at;  // head when each command was emitted
    explicit TrigRun(ClipSettings s = {}) { t.configure(s); }
    void step(bool gate, double seconds, double freq = 118.7e6) {
        const std::size_t passes = static_cast<std::size_t>(std::llround(seconds * 100.0));
        for (std::size_t i = 0; i < passes; ++i) {
            head += kPass;
            ClipCmd out[2];
            const int n = t.update(gate, head, kT0 + static_cast<std::int64_t>(head / 48000), freq, "NFM", out, 2);
            for (int k = 0; k < n; ++k) { cmds.push_back(out[k]); at.push_back(head); }
        }
    }
    int count(ClipCmd::Kind k) const {
        int n = 0;
        for (const auto& c : cmds) { if (c.kind == k) { ++n; } }
        return n;
    }
};

ClipSettings settings(double pre, double tail, double minS, double maxS = 600.0, double rate = kRate) {
    ClipSettings s;
    s.preRollS = pre; s.tailS = tail; s.minS = minS; s.maxClipS = maxS; s.rateHz = rate;
    return s;
}

void testTriggerBasics() {
    // Idle gate: nothing. Open 5 s after 10 s of silence: Open with the pre-roll, Close tail after the fall.
    TrigRun r(settings(2, 3, 1));
    r.step(false, 10.0);
    CHECK(r.cmds.empty());
    r.step(true, 5.0);
    CHECK(r.cmds.size() == 1 && r.cmds[0].kind == ClipCmd::Kind::Open);
    // The gate is first SEEN open at the pass after t = 10.00 s: head 480480... the first open pass ends at 10.01 s.
    const std::uint64_t openHead = 10 * 48000 + kPass;
    CHECK(r.cmds[0].gateSample == openHead);
    CHECK(r.cmds[0].sample == openHead - 2 * 48000);   // pre-roll exactly 2 s ahead
    CHECK(r.cmds[0].frequencyHz == 118.7e6);
    CHECK(std::string(r.cmds[0].mode) == "NFM");
    CHECK(r.cmds[0].unixTime == kT0 + 10);
    r.step(false, 2.9);
    CHECK(r.cmds.size() == 1);                          // still hanging: 2.9 s < 3 s tail
    r.step(false, 0.2);
    CHECK(r.cmds.size() == 2 && r.cmds[1].kind == ClipCmd::Kind::Close);
    const std::uint64_t fall = 15 * 48000 + kPass;     // first closed pass
    CHECK(r.cmds[1].sample == fall + 3 * 48000);       // tail exactly 3 s after the fall
    CHECK(r.cmds[1].clipId == r.cmds[0].clipId);
    CHECK(!r.t.clipOpen());
    r.step(false, 20.0);
    CHECK(r.cmds.size() == 2);
}

void testTriggerHangMergeAndSplit() {
    {   // A gap shorter than the tail merges: ONE clip.
        TrigRun r(settings(1, 3, 0.5));
        r.step(true, 2.0); r.step(false, 1.0); r.step(true, 2.0); r.step(false, 5.0);
        CHECK(r.count(ClipCmd::Kind::Open) == 1 && r.count(ClipCmd::Kind::Close) == 1);
        // The last fall is seen at the first closed pass after 2 + 1 + 2 s of ring time; the Close is 3 s after it.
        CHECK(r.cmds.back().sample == 5 * 48000 + kPass + 3 * 48000);
    }
    {   // A gap longer than the tail: TWO clips, and the second has its own pre-roll.
        TrigRun r(settings(1, 3, 0.5));
        r.step(true, 2.0); r.step(false, 4.0); r.step(true, 2.0); r.step(false, 5.0);
        CHECK(r.count(ClipCmd::Kind::Open) == 2 && r.count(ClipCmd::Kind::Close) == 2);
        CHECK(r.cmds[0].clipId != r.cmds[2].clipId);
        CHECK(r.cmds[1].sample <= r.cmds[2].sample + 0);   // the first clip ended (tail) before the second began, with pre-roll 1 s..
        CHECK(r.cmds[2].sample == r.cmds[2].gateSample - 48000);
    }
    {   // Zero tail: the clip closes on the pass that sees the gate fall.
        TrigRun r(settings(0, 0, 0));
        r.step(true, 1.0); r.step(false, 0.01);
        CHECK(r.count(ClipCmd::Kind::Open) == 1 && r.count(ClipCmd::Kind::Close) == 1);
        CHECK(r.cmds[0].sample == r.cmds[0].gateSample);   // zero pre-roll
    }
}

void testTriggerMinimumLength() {
    {   // A 0.2 s click against a 1 s minimum: Open then Drop.
        TrigRun r(settings(2, 3, 1));
        r.step(false, 5.0); r.step(true, 0.2); r.step(false, 5.0);
        CHECK(r.count(ClipCmd::Kind::Open) == 1 && r.count(ClipCmd::Kind::Drop) == 1 && r.count(ClipCmd::Kind::Close) == 0);
    }
    {   // Exactly the minimum is kept; one pass less is dropped.
        TrigRun a(settings(0, 1, 1));
        a.step(false, 1.0); a.step(true, 1.0); a.step(false, 3.0);
        CHECK(a.count(ClipCmd::Kind::Close) == 1 && a.count(ClipCmd::Kind::Drop) == 0);
        TrigRun b(settings(0, 1, 1));
        b.step(false, 1.0); b.step(true, 0.99); b.step(false, 3.0);
        CHECK(b.count(ClipCmd::Kind::Close) == 0 && b.count(ClipCmd::Kind::Drop) == 1);
    }
    {   // Two blips inside the tail add up to ONE transmission long enough: kept (open to LAST fall).
        TrigRun r(settings(0, 3, 1));
        r.step(true, 0.3); r.step(false, 0.5); r.step(true, 0.3); r.step(false, 5.0);
        CHECK(r.count(ClipCmd::Kind::Open) == 1 && r.count(ClipCmd::Kind::Close) == 1);
    }
    {   // Zero minimum keeps a single pass.
        TrigRun r(settings(0, 0, 0));
        r.step(true, 0.01); r.step(false, 1.0);
        CHECK(r.count(ClipCmd::Kind::Close) == 1);
    }
}

void testTriggerMaxClip() {
    TrigRun r(settings(1, 2, 1, 10));
    r.step(false, 3.0); r.step(true, 25.0); r.step(false, 4.0);
    // 25 s open with a 10 s maximum: segments of 10, 10 and 5 s; all kept; each Open continues exactly where the last Close ended.
    CHECK(r.count(ClipCmd::Kind::Open) == 3 && r.count(ClipCmd::Kind::Close) == 3 && r.count(ClipCmd::Kind::Drop) == 0);
    std::vector<ClipCmd> opens, closes;
    for (auto& c : r.cmds) { (c.kind == ClipCmd::Kind::Open ? opens : closes).push_back(c); }
    CHECK(opens.size() == 3 && closes.size() == 3);
    CHECK(closes[0].sample == opens[1].sample && closes[1].sample == opens[2].sample);   // no audio lost at a joint
    CHECK(opens[1].sample == opens[1].gateSample && opens[2].sample == opens[2].gateSample);  // no pre-roll on a continuation
    CHECK(opens[1].sample - opens[0].gateSample >= 10 * 48000 && opens[1].sample - opens[0].gateSample < 10 * 48000 + 2 * kPass);
    CHECK(opens[0].clipId != opens[1].clipId && opens[1].clipId != opens[2].clipId);
    // A SHORT last segment of a split clip is not dropped as a click: 10.3 s open, 10 s maximum.
    TrigRun q(settings(0, 0, 1, 10));
    q.step(true, 10.3); q.step(false, 1.0);
    CHECK(q.count(ClipCmd::Kind::Drop) == 0 && q.count(ClipCmd::Kind::Close) == 2 && q.count(ClipCmd::Kind::Open) == 2);
}

void testTriggerEndNowAndEdges() {
    {   // endNow while open: Close at the head; no tail.
        TrigRun r(settings(2, 3, 1));
        r.step(true, 4.0);
        ClipCmd out[2];
        CHECK(r.t.endNow(r.head, out, 2) == 1);
        CHECK(out[0].kind == ClipCmd::Kind::Close && out[0].sample == r.head);
        CHECK(!r.t.clipOpen());
        CHECK(r.t.endNow(r.head, out, 2) == 0);           // nothing open any more
    }
    {   // endNow while hanging: ends at the fall plus what tail has elapsed, not before the fall.
        TrigRun r(settings(0, 3, 1));
        r.step(true, 4.0); r.step(false, 1.0);
        ClipCmd out[2];
        CHECK(r.t.endNow(r.head, out, 2) == 1);
        CHECK(out[0].kind == ClipCmd::Kind::Close && out[0].sample == r.head);   // 1 s of the 3 s tail elapsed
    }
    {   // endNow on a click: Drop.
        TrigRun r(settings(0, 3, 1));
        r.step(true, 0.2);
        ClipCmd out[2];
        CHECK(r.t.endNow(r.head, out, 2) == 1 && out[0].kind == ClipCmd::Kind::Drop);
    }
    {   // endNow when idle, with no room, or with no buffer.
        ClipTrigger t; t.configure({});
        ClipCmd out[2];
        CHECK(t.endNow(100, out, 2) == 0);
        CHECK(t.endNow(100, nullptr, 2) == 0);
        CHECK(t.update(true, 100, kT0, 1e6, "NFM", out, 1) == 0);        // no room for two: nothing done
        CHECK(!t.clipOpen());
        CHECK(t.update(true, 100, kT0, 1e6, "NFM", nullptr, 2) == 0);
    }
    {   // Settings clamp; junk takes the defaults.
        ClipTrigger t;
        ClipSettings s = t.configure(settings(99, 99, 999, 99999, 44100));
        CHECK(s.preRollS == 10.0 && s.tailS == 30.0 && s.minS == 60.0 && s.maxClipS == 3600.0 && s.rateHz == 44100.0);
        s = t.configure(settings(-1, -1, -1, 1));
        CHECK(s.preRollS == 0.0 && s.tailS == 0.0 && s.minS == 0.0 && s.maxClipS == 10.0);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        s = t.configure(settings(nan, nan, nan, nan, nan));
        CHECK(s.preRollS == 2.0 && s.tailS == 3.0 && s.minS == 1.0 && s.maxClipS == 600.0 && s.rateHz == 48000.0);
    }
    {   // Another rate: the sample arithmetic follows it.
        TrigRun r(settings(2, 3, 1, 600, 44100.0));
        r.step(false, 5.0);
        // TrigRun steps in 480-sample passes of "10 ms at 48 kHz"; at 44.1 kHz these are 10.88 ms: compare in samples.
        r.step(true, 5.0);
        CHECK(r.cmds[0].gateSample - r.cmds[0].sample == static_cast<std::uint64_t>(2 * 44100));
    }
    {   // Pre-roll is clamped at sample 0.
        TrigRun r(settings(2, 3, 1));
        r.step(true, 0.5);
        CHECK(r.cmds[0].sample == 0 && r.cmds[0].gateSample == kPass);
    }
    {   // The trigger does not allocate.
        TrigRun r(settings(2, 3, 1));
        AllocProbe probe;
        for (int i = 0; i < 2000; ++i) { r.head += kPass; ClipCmd out[2]; r.t.update((i / 300) % 2 == 0, r.head, kT0, 1e6, "NFM", out, 2); }
        CHECK(probe.news() == 0);
    }
}

// --- file helpers ------------------------------------------------------------------------
struct Wav {
    bool ok = false;
    std::uint32_t rate = 0;
    std::uint16_t channels = 0, bits = 0;
    std::vector<std::int16_t> samples;
    std::uint32_t dataBytes = 0, fileBytes = 0;
};

Wav readWav(const fs::path& p) {
    Wav w;
    std::ifstream f(p, std::ios::binary);
    if (!f) { return w; }
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    w.fileBytes = static_cast<std::uint32_t>(b.size());
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0 ||
        std::memcmp(b.data() + 12, "fmt ", 4) != 0 || std::memcmp(b.data() + 36, "data", 4) != 0) { return w; }
    auto u32 = [&](std::size_t o) { return static_cast<std::uint32_t>(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24)); };
    auto u16 = [&](std::size_t o) { return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8)); };
    w.channels = u16(22);
    w.rate = u32(24);
    w.bits = u16(34);
    w.dataBytes = u32(40);
    if (u32(4) != b.size() - 8 || w.dataBytes != b.size() - 44) { return w; }   // header sizes patched and true
    w.samples.resize(w.dataBytes / 2);
    std::memcpy(w.samples.data(), b.data() + 44, w.dataBytes);
    w.ok = true;
    return w;
}

bool isRampSlice(const std::vector<std::int16_t>& v, std::uint64_t first) {
    for (std::size_t i = 0; i < v.size(); ++i) { if (v[i] != rampInt(first + i)) { return false; } }
    return true;
}

std::vector<fs::path> wavFiles(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) { if (e.path().extension() == ".wav") { out.push_back(e.path()); } }
    std::sort(out.begin(), out.end());
    return out;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A scratch folder of this test's own, removed at the end by its exact path.
struct Scratch {
    fs::path root;
    Scratch() {
        root = fs::temp_directory_path() / ("foxsdr_clip_test_" +
                                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
    }
    ~Scratch() { std::error_code ec; fs::remove_all(root, ec); }
};

// The whole pipeline in one thread: a ring fed with the ramp, a trigger, a writer serviced each pass.
struct Sim {
    ReplayRing ring;
    ClipTrigger trig;
    ClipWriter writer;
    std::vector<ClipEvent> events;
    std::uint64_t head = 0;
    explicit Sim(const fs::path& dir, ClipSettings s, double ringSeconds = 30.0, std::string prefix = "radio1_clip", int radio = 1)
        : writer([&] { ClipWriter::Config c; c.directory = dir.string(); c.prefix = prefix; c.radio = radio; c.utcNames = true; return c; }()) {
        ring.resize(kRate, ringSeconds);
        trig.configure(s);
    }
    void step(bool gate, double seconds, bool service = true, double freq = 118.7e6) {
        const std::size_t passes = static_cast<std::size_t>(std::llround(seconds * 100.0));
        for (std::size_t i = 0; i < passes; ++i) {
            std::vector<float> v(kPass);
            for (std::size_t k = 0; k < kPass; ++k) { v[k] = rampValue(head + k); }
            ring.write(v.data(), kPass, gate);
            head += kPass;
            ClipCmd out[2];
            const int n = trig.update(gate, ring.head(), kT0 + static_cast<std::int64_t>(ring.head() / 48000), freq, "NFM", out, 2);
            for (int c = 0; c < n; ++c) { writer.push(out[c]); }
            if (service) { writer.service(ring, &events); }
        }
    }
};

int countKind(const std::vector<ClipEvent>& ev, ClipEvent::Kind k) {
    int n = 0;
    for (auto& e : ev) { if (e.kind == k) { ++n; } }
    return n;
}

void testEndToEndBasicClip() {
    Scratch sc;
    const fs::path dir = sc.root / "clips";   // does not exist yet: the writer creates it
    Sim sim(dir, settings(2, 3, 1));
    sim.step(false, 10.0);
    CHECK(!fs::exists(dir));                  // nothing yet
    sim.step(true, 5.0);
    CHECK(sim.writer.clipOpen());
    sim.step(false, 10.0);
    CHECK(!sim.writer.clipOpen());
    CHECK(sim.writer.clipsSaved() == 1 && sim.writer.clipsDropped() == 0 && sim.writer.clipsFailed() == 0);
    const auto files = wavFiles(dir);
    CHECK(files.size() == 1);
    if (files.size() != 1) { return; }
    // The NAME and the time stamp: gate first seen open at the pass ending 10.01 s of ring time = unix kT0 + 10.
    CHECK(files[0].filename().string() == "radio1_clip_20260101_000010_118700.wav");
    // The CONTENT: exactly the ring slice [open - 2 s, fall + 3 s): pre-roll present, tail present, nothing else.
    const Wav w = readWav(files[0]);
    CHECK(w.ok && w.channels == 1 && w.bits == 16 && w.rate == 48000);
    const std::uint64_t openHead = 10 * 48000 + kPass, fallHead = 15 * 48000 + kPass;
    const std::uint64_t start = openHead - 2 * 48000, end = fallHead + 3 * 48000;
    CHECK(w.samples.size() == end - start);
    CHECK(isRampSlice(w.samples, start));
    CHECK(w.samples.size() > 0 && w.samples[0] == rampInt(start));   // the first sample is 2 s BEFORE the gate opened
    // The index.
    const std::string idx = slurp(dir / "index.csv");
    CHECK(idx.find("start_utc,radio,frequency_hz,mode,duration_s,peak_dbfs,gap\n") == 0);
    CHECK(idx.find("2026-01-01T00:00:10Z,1,118700000,NFM,10.000,") != std::string::npos);
    CHECK(std::count(idx.begin(), idx.end(), '\n') == 2);            // header + one row
    // The event.
    CHECK(sim.events.size() == 1 && sim.events[0].kind == ClipEvent::Kind::Saved && !sim.events[0].gap && !sim.events[0].writeFailed);
    CHECK_NEAR(sim.events[0].durationS, 10.0, 1e-6);
    CHECK_NEAR(sim.writer.lastClipS(), 10.0, 1e-6);
    CHECK(sim.events[0].peakDbfs < 0.0 && sim.events[0].peakDbfs > -6.0);   // the ramp peaks at 19999/32768 = -4.3 dBFS
}

void testEndToEndHangMergeSeparateAndClick() {
    Scratch sc;
    const fs::path dir = sc.root;
    Sim sim(dir, settings(1, 3, 1));
    sim.step(false, 2.0);
    sim.step(true, 2.0); sim.step(false, 1.0); sim.step(true, 2.0);   // hang time: one clip
    sim.step(false, 6.0);
    CHECK(wavFiles(dir).size() == 1);
    sim.step(true, 0.2); sim.step(false, 6.0);                          // a click: no file
    CHECK(wavFiles(dir).size() == 1);
    CHECK(sim.writer.clipsDropped() == 1 && countKind(sim.events, ClipEvent::Kind::Dropped) == 1);
    sim.step(true, 3.0); sim.step(false, 6.0);                          // a second real clip, separate name
    const auto files = wavFiles(dir);
    CHECK(files.size() == 2 && files[0].filename() != files[1].filename());
    CHECK(sim.writer.clipsSaved() == 2);
    const Wav a = readWav(files[0]);
    CHECK(a.ok);
    // First clip: pre 1 s + 2 + 1 + 2 s open + tail 3 s (+ the pass granularity): 9.0 s plus two passes.
    CHECK(a.samples.size() >= 9 * 48000 && a.samples.size() <= 9 * 48000 + 3 * kPass);
    const std::string idx = slurp(dir / "index.csv");
    CHECK(std::count(idx.begin(), idx.end(), '\n') == 3);               // the click has no row
}

void testEndToEndSplitAtMaximum() {
    Scratch sc;
    Sim sim(sc.root, settings(1, 2, 1, 10), 30.0);
    sim.step(false, 3.0); sim.step(true, 25.0); sim.step(false, 5.0);
    const auto files = wavFiles(sc.root);
    CHECK(files.size() == 3);
    // Joined in order, the three files are one unbroken stretch of the ramp: nothing lost or doubled at a split.
    std::vector<std::int16_t> all;
    std::uint64_t first = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const Wav w = readWav(files[i]);
        CHECK(w.ok);
        if (i == 0) { first = 3 * 48000 + kPass - 48000; }
        all.insert(all.end(), w.samples.begin(), w.samples.end());
    }
    CHECK(isRampSlice(all, first));
    CHECK(sim.writer.clipsSaved() == 3);
}

void testPreRollShorterThanAvailable() {
    Scratch sc;
    Sim sim(sc.root, settings(2, 1, 0.5));
    sim.step(true, 1.0); sim.step(false, 3.0);   // gate open 0.5 s into... at t = 0: no history to pre-roll from
    const auto files = wavFiles(sc.root);
    CHECK(files.size() == 1);
    const Wav w = readWav(files[0]);
    CHECK(w.ok && w.samples.size() > 0 && isRampSlice(w.samples, 0));    // starts at the very first sample, no error
    CHECK(!sim.events.empty() && !sim.events[0].gap);
}

void testUnwritableFolderAndRecovery() {
    Scratch sc;
    // A FILE where the folder should be: create_directories under it must fail.
    const fs::path blocker = sc.root / "blocker";
    { std::ofstream f(blocker); f << "not a folder"; }
    Sim sim(blocker / "clips", settings(1, 1, 0.5));
    sim.step(false, 1.0); sim.step(true, 2.0); sim.step(false, 4.0);
    CHECK(sim.writer.clipsFailed() == 1 && sim.writer.clipsSaved() == 0);
    CHECK(countKind(sim.events, ClipEvent::Kind::Failed) == 1);
    for (auto& e : sim.events) { if (e.kind == ClipEvent::Kind::Failed) { CHECK(!e.error.empty()); } }
    CHECK(!sim.writer.clipOpen());
    // The folder is fixed: the next clip works, nothing is stuck.
    sim.writer.setDirectory((sc.root / "good").string());
    sim.step(true, 2.0); sim.step(false, 4.0);
    CHECK(sim.writer.clipsSaved() == 1 && sim.writer.clipsFailed() == 1);
    CHECK(wavFiles(sc.root / "good").size() == 1);
}

void testDiskRefusesWrites() {
    Scratch sc;
    Sim sim(sc.root, settings(1, 1, 0.5));
    // An opener that hands the writer a READ-ONLY handle on an existing file: every write is refused, as on a full disk.
    sim.writer.setOpenerForTest([](const cascade::core::Recorder::OpenRequest& req, cascade::core::Recorder::OpenedFile& out, std::string& error) {
        { std::ofstream f(req.path, std::ios::binary); f << "x"; }
        std::FILE* fp = std::fopen(req.path.c_str(), "rb");
        if (fp == nullptr) { error = "test opener failed"; return false; }
        out.file.reset(fp);
        out.path = req.path;
        out.kind = req.kind;
        return true;
    });
    sim.step(false, 1.0); sim.step(true, 2.0); sim.step(false, 4.0);
    CHECK(sim.writer.clipsSaved() == 1);                               // finished, reported, not hung and not crashed
    CHECK(!sim.events.empty() && sim.events[0].writeFailed && !sim.events[0].error.empty());
    CHECK(!sim.writer.clipOpen());
}

void testWorkerFallsBehindTheRing() {
    Scratch sc;
    Sim sim(sc.root, settings(1, 1, 0.5), 5.0);        // a 5 s ring
    sim.step(false, 1.0);
    sim.step(true, 2.0, false);                          // the worker is NOT serviced for 2 s ...
    sim.step(true, 8.0, false);                          // ... then 8 s more: the clip start is long gone from the ring
    sim.step(false, 4.0, true);
    CHECK(sim.writer.clipsSaved() == 1);
    CHECK(!sim.events.empty() && sim.events[0].gap);
    const std::string idx = slurp(sc.root / "index.csv");
    CHECK(idx.find(",gap\n") != std::string::npos);
    const auto files = wavFiles(sc.root);
    CHECK(files.size() == 1 && readWav(files[0]).ok);
    // Less audio than the clip should hold (the lost stretch is absent, not filled), and what is there is ramp.
    const Wav w = readWav(files[0]);
    CHECK(!w.samples.empty());
    CHECK(w.samples.size() < (1 + 10 + 1) * 48000);
    bool ramp = true;
    for (std::size_t i = 1; i < w.samples.size(); ++i) {
        const int d = (static_cast<int>(w.samples[i]) - static_cast<int>(w.samples[i - 1]) + 20000) % 20000;
        if (d != 1 && w.samples[i] != 0) { ramp = false; break; }   // consecutive, or the wrap (and a jump over the lost stretch once)
        if (d != 1) { break; }
    }
    CHECK(ramp);
}

void testStopMidClipAndRingOff() {
    {   // finishAll mid-clip: the file is closed with a TRUE header and holds up to the ring head.
        Scratch sc;
        Sim sim(sc.root, settings(1, 3, 1));
        sim.step(false, 1.0); sim.step(true, 3.0);
        CHECK(sim.writer.clipOpen());
        ClipCmd out[2];
        const int n = sim.trig.endNow(sim.ring.head(), out, 2);
        CHECK(n == 1);
        sim.writer.push(out[0]);
        sim.writer.finishAll(sim.ring, &sim.events);
        CHECK(!sim.writer.clipOpen() && sim.writer.clipsSaved() == 1);
        const auto files = wavFiles(sc.root);
        CHECK(files.size() == 1);
        const Wav w = readWav(files[0]);
        CHECK(w.ok);
        // Gate first seen open at head 48000 + kPass, pre-roll 1 s -> start = kPass; ended NOW at head = 4 s, no tail.
        CHECK(w.samples.size() == 4 * 48000 - kPass);
        CHECK(isRampSlice(w.samples, kPass));
    }
    {   // finishAll with no Close queued at all: closes at the ring head.
        Scratch sc;
        Sim sim(sc.root, settings(1, 3, 1));
        sim.step(true, 3.0);
        sim.writer.finishAll(sim.ring, &sim.events);
        CHECK(!sim.writer.clipOpen() && wavFiles(sc.root).size() == 1 && readWav(wavFiles(sc.root)[0]).ok);
    }
    {   // The ring is switched off while a clip is open: the clip ends, nothing spins.
        Scratch sc;
        Sim sim(sc.root, settings(1, 1, 0.5));
        sim.step(true, 2.0);
        sim.ring.resize(kRate, 0.0);
        sim.step(false, 3.0, true);
        sim.writer.finishAll(sim.ring, &sim.events);
        CHECK(!sim.writer.clipOpen());
    }
    {   // finishAll with nothing open is a no-op.
        Scratch sc;
        Sim sim(sc.root, settings(1, 1, 0.5));
        CHECK(sim.writer.finishAll(sim.ring, &sim.events) == 0);
    }
}

void testTwoRadiosAndNameCollision() {
    Scratch sc;
    Sim a(sc.root, settings(1, 1, 0.5), 30.0, "radio1_clip", 1);
    Sim b(sc.root, settings(1, 1, 0.5), 30.0, "radio2_clip", 2);
    for (int i = 0; i < 3; ++i) {   // interleaved passes, different gates
        a.step(i == 1, 1.0);
        b.step(i != 1, 1.0, true, 121.5e6);
    }
    a.step(false, 3.0); b.step(false, 3.0);
    const auto files = wavFiles(sc.root);
    int r1 = 0, r2 = 0;
    for (auto& f : files) {
        if (f.filename().string().rfind("radio1_clip_", 0) == 0) { ++r1; }
        if (f.filename().string().rfind("radio2_clip_", 0) == 0) { ++r2; }
    }
    CHECK(r1 >= 1 && r2 >= 1);
    // Two clips opened in the same second with the same frequency do not overwrite each other.
    ClipWriter w([&] { ClipWriter::Config c; c.directory = (sc.root / "dup").string(); c.utcNames = true; return c; }());
    ReplayRing ring;
    ring.resize(kRate, 5.0);
    std::vector<float> v(48000, 0.1f);
    ring.write(v.data(), v.size(), true);
    for (std::uint32_t id = 1; id <= 2; ++id) {
        ClipCmd o; o.kind = ClipCmd::Kind::Open; o.clipId = id; o.sample = 0; o.unixTime = kT0; o.frequencyHz = 100e6; std::strncpy(o.mode, "NFM", 7);
        ClipCmd c; c.kind = ClipCmd::Kind::Close; c.clipId = id; c.sample = 24000;
        w.push(o); w.push(c);
        w.service(ring);
    }
    CHECK(wavFiles(sc.root / "dup").size() == 2);
}

void testWorkerThread() {
    // The pass thread feeds blocks at a pace, the worker thread services every 2 ms; the clips come out exactly right.
    Scratch sc;
    ReplayRing ring;
    ring.resize(kRate, 20.0);
    ClipWriter writer([&] { ClipWriter::Config c; c.directory = sc.root.string(); c.utcNames = true; return c; }());
    ClipTrigger trig;
    trig.configure(settings(1, 1, 0.5));
    std::atomic<bool> stop{false};
    std::vector<ClipEvent> events;
    std::thread worker([&] {
        while (!stop.load()) { writer.service(ring, &events); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    });
    std::uint64_t head = 0;
    const struct { bool gate; int passes; } script[] = {{false, 100}, {true, 300}, {false, 500}, {true, 50}, {false, 100}, {true, 200}, {false, 500}};
    for (const auto& seg : script) {
        for (int i = 0; i < seg.passes; ++i) {
            std::vector<float> v(kPass);
            for (std::size_t k = 0; k < kPass; ++k) { v[k] = rampValue(head + k); }
            ring.write(v.data(), kPass, seg.gate);
            head += kPass;
            ClipCmd out[2];
            const int n = trig.update(seg.gate, ring.head(), kT0 + static_cast<std::int64_t>(head / 48000), 145.5e6, "NFM", out, 2);
            for (int c = 0; c < n; ++c) { writer.push(out[c]); }
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(300);
            while (std::chrono::steady_clock::now() < until) {}
        }
    }
    // Let the worker drain, then stop it and finish.
    for (int i = 0; i < 200 && writer.clipOpen(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    stop = true;
    worker.join();
    writer.finishAll(ring, &events);
    const auto files = wavFiles(sc.root);
    std::printf("  worker thread: %zu files, saved %d dropped %d lost commands %d\n", files.size(), writer.clipsSaved(), writer.clipsDropped(), writer.commandsLost());
    CHECK(writer.commandsLost() == 0);
    CHECK(writer.clipsSaved() == 2 && writer.clipsDropped() == 0);   // a 3 s clip, then (after a 5 s gap) a 0.5 s blip, a 1 s gap that is inside the 1 s tail, and 2 s: ONE more clip
    bool allRamp = !files.empty();
    for (auto& f : files) {
        const Wav w = readWav(f);
        if (!w.ok || w.samples.empty()) { allRamp = false; continue; }
        // Find where this clip starts on the ramp (the first value); then it must be contiguous from there.
        std::uint64_t cand = static_cast<std::uint64_t>(w.samples[0]);
        bool found = false;
        for (std::uint64_t base = 0; base < head && !found; base += 20000) {
            const std::uint64_t first = base + cand;
            if (first + w.samples.size() <= head && isRampSlice(w.samples, first)) { found = true; }
        }
        allRamp = allRamp && found;
    }
    CHECK(allRamp);
}

void testNoAllocationInPush() {
    ClipWriter w;
    ClipCmd c;
    AllocProbe probe;
    for (int i = 0; i < 100; ++i) { w.push(c); }
    CHECK(probe.news() == 0);
    CHECK(w.commandsLost() == 100 - 32);   // the queue holds 32; the rest are counted, never blocked on
}

}  // namespace

int main() {
    testTriggerBasics();
    testTriggerHangMergeAndSplit();
    testTriggerMinimumLength();
    testTriggerMaxClip();
    testTriggerEndNowAndEdges();
    testEndToEndBasicClip();
    testEndToEndHangMergeSeparateAndClick();
    testEndToEndSplitAtMaximum();
    testPreRollShorterThanAvailable();
    testUnwritableFolderAndRecovery();
    testDiskRefusesWrites();
    testWorkerFallsBehindTheRing();
    testStopMidClipAndRingOff();
    testTwoRadiosAndNameCollision();
    testWorkerThread();
    testNoAllocationInPush();
    return testSummary("test_clip_recorder");
}
