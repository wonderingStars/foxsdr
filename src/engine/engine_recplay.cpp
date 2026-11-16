// engine_recplay.cpp - SCREEN LANE S3 (docs/multi-radio-records/screen-s3-recplay.md): the engine's side of the recording and playback controls.
//
//   facts      record.schedule.ids (the ids of the timed recordings, so a screen can list them), radio.<n>.record.format ("iq=0;audio=0"), radio.<n>.file.header ("1" while the radio plays
//              an I/Q file whose centre came from the file's own header), radio.<n>.record.rolled (files a take has gone on into), radio.<n>.record.replay ("seconds=10;bytes=960000;max=...")
//              and radio.<n>.record.clips ("saved=2;dropped=1;failed=0;open=0"). Nothing here names a file or a folder.
//   rollover   G3-10: settings rec.roll_mb, rec.roll_min, rec.roll_align (the wiring lane C registry). A take - I/Q or audio - goes on into a NEXT FILE with no sample lost: at four fifths of the
//              limit the pass opens the next file on a worker (the template and the day's folder apply, as they do to every take), the recorder swaps to it between two blocks, and the
//              file it left goes to the finishing worker.
//   clips      G3-03 and G3-09: settings rec.clip_*. While on, the radio's squelch gate opens a clip file and closes it when the transmission ends (core/clip_recorder.hpp: time before, time after,
//              shortest kept, longest). One file per transmission, in <recordings folder>/clips, named radio<N>_clip_<date>_<time>_<kHz>.wav.
//   replay     G3-02: setting rec.replay_s. The radio's audio is kept in memory for that many seconds (16-bit, 96 kB a second); a clip begins that long before the squelch opened, and an audio take
//              that starts with the buffer on begins with it. Bounded at 120 s (11.5 MB per radio); the clips raise the buffer to hold their time before the transmission plus 10 s.
//
// A failure (a full or missing folder, a disk that refuses writes) is said in words (recordError_) and counted (RecordStart::poll's noteRecordFailed, noteRecordFailed for a clip), never silent.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/clip_recorder.hpp"
#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/receiver_tables.hpp"
#include "core/record_finish.hpp"
#include "core/recorder.hpp"
#include "core/replay_ring.hpp"
#include "core/schedule.hpp"
#include "engine/engine.hpp"
#include "engine/record_start.hpp"
#include "engine/wire_c.hpp"
#include "source/iq_file_source.hpp"

namespace cascade::engine {

namespace {

using cascade::core::ClipCmd;
using cascade::core::ClipEvent;
using cascade::core::ClipSettings;
using cascade::core::ClipTrigger;
using cascade::core::ClipWriter;
using cascade::core::RecordFinisher;
using cascade::core::Recorder;
using cascade::core::ReplayRing;

constexpr double kAudioRate = 48000.0;

struct Cfg {
    double rollMB = 0, rollMin = 0, preS = 2, tailS = 3, minS = 1, maxS = 600, replayS = 0;
    bool align = false, clipOn = false;
};

// One radio's runtime: the replay buffer, the clip trigger and writer with their worker, and the rollover's pending opens. Owned by the Receiver (declared before its pipeline, so the DSP thread is
// gone when this is destroyed).
struct RecRt {
    ReplayRing ring;
    ClipTrigger trig;
    std::unique_ptr<ClipWriter> writer;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::mutex evMu;
    std::vector<ClipEvent> events;
    double ringSeconds = 0.0;      // what the ring holds now
    bool attached = false;         // the pipeline writes into the ring
    std::string clipDir;
    ClipSettings clipSet;
    bool clipRunning = false;
    cascade::gui::RecordStart nextIq, nextAudio;
    std::vector<std::shared_ptr<RecordFinisher::Ticket>> tickets;
    std::uint64_t seenRollIq = ~0ull, seenRollAudio = ~0ull;
    std::string seenPathIq, seenPathAudio;
    std::uint64_t rolledIq = 0, rolledAudio = 0;
    std::int64_t retryAtMs = 0;
    std::string lastSig;           // what the facts say, to tell the fact block when it changed

    void stopWorker() {
        if (!worker.joinable()) { return; }
        stop.store(true);
        worker.join();
        stop.store(false);
    }
    ~RecRt() { stopWorker(); }
};

RecRt* rtOf(Receiver& r, bool create) {
    if (!r.recplayRt_ && create) { r.recplayRt_ = std::make_shared<RecRt>(); }
    return static_cast<RecRt*>(r.recplayRt_.get());
}

std::int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

}  // namespace

// ============================================================ facts ============================================================
void Engine::recplayFacts(const std::function<void(const std::string&, const std::string&)>& put) const {
    std::string ids;
    for (const cascade::core::sched::Entry& e : schedule_.entries()) {
        if (!ids.empty()) { ids += ','; }
        ids += std::to_string(e.id);
    }
    put("record.schedule.ids", ids);
}

void Engine::recplayRadioFacts(std::uint32_t n, const std::function<void(const std::string&, const std::string&)>& put) const {
    if (n < 1u || n > kMaxReceivers) { return; }
    Receiver& r = *receivers_[n - 1u];
    const std::string pre = "radio." + std::to_string(n) + ".";
    put(pre + "record.format", "iq=" + std::to_string(r.recordIqFormat_) + ";audio=" + std::to_string(r.recordAudioFormat_));
    const auto* file = dynamic_cast<const cascade::source::IqFileSource*>(&r.pipeline_.rawSource());
    if (file != nullptr && file->totalFrames() > 0) { put(pre + "file.header", file->centerFromHeader() ? "1" : "0"); }
    if (const RecRt* rt = rtOf(r, false)) {
        put(pre + "record.rolled", std::to_string(rt->rolledIq + rt->rolledAudio));
        char b[96];
        std::snprintf(b, sizeof b, "seconds=%g;bytes=%.0f;max=%.0f", rt->ringSeconds, rt->ringSeconds * kAudioRate * 2.0, 120.0 * kAudioRate * 2.0);
        put(pre + "record.replay", b);
        if (rt->writer) {
            std::snprintf(b, sizeof b, "saved=%d;dropped=%d;failed=%d;open=%d", rt->writer->clipsSaved(), rt->writer->clipsDropped(), rt->writer->clipsFailed(), rt->writer->clipOpen() ? 1 : 0);
            put(pre + "record.clips", b);
        }
    }
}

// ============================================================ the pass ============================================================
void Engine::recplayPump() {
    wirec::State& ws = wireC();
    Cfg c;
    c.rollMB = ws.settings().getReal("rec.roll_mb");
    c.rollMin = ws.settings().getReal("rec.roll_min");
    c.align = ws.settings().getBool("rec.roll_align");
    c.clipOn = ws.settings().getBool("rec.clip_on");
    c.preS = ws.settings().getReal("rec.clip_pre");
    c.tailS = ws.settings().getReal("rec.clip_tail");
    c.minS = ws.settings().getReal("rec.clip_min");
    c.maxS = ws.settings().getReal("rec.clip_max");
    c.replayS = ws.settings().getReal("rec.replay_s");
    const bool rolling = c.rollMB > 0.0 || c.rollMin > 0.0;
    const bool wantRing = c.replayS > 0.0 || c.clipOn;
    RecRt* rt = rtOf(rx(), rolling || wantRing);
    if (rt == nullptr) { return; }
    const std::int64_t tMs = nowMs();

    // ---------------------------------------------------------------- the replay buffer and the clips
    const double ringWant = std::min(120.0, std::max(c.replayS, c.clipOn ? c.preS + 10.0 : 0.0)) * (wantRing ? 1.0 : 0.0);
    const bool clipChanged = rt->clipRunning != c.clipOn || rt->clipSet.preRollS != c.preS || rt->clipSet.tailS != c.tailS || rt->clipSet.minS != c.minS || rt->clipSet.maxClipS != c.maxS;
    if (std::fabs(ringWant - rt->ringSeconds) > 1e-9 || (clipChanged && c.clipOn != rt->clipRunning)) {
        // A change of the buffer or of the clips' switch: detach the ring from the DSP thread (setReplayRing takes the lock the thread writes under), stop the worker, then resize.
        rx().pipeline_.setReplayRing(nullptr);
        rt->attached = false;
        if (rt->clipRunning && rt->worker.joinable()) {
            rt->stopWorker();
            std::vector<ClipEvent> ev;
            if (rt->writer) { rt->writer->finishAll(rt->ring, &ev); }
            std::lock_guard<std::mutex> lk(rt->evMu);
            rt->events.insert(rt->events.end(), ev.begin(), ev.end());
        }
        rt->clipRunning = false;
        (void)rt->ring.resize(kAudioRate, ringWant);
        rt->ringSeconds = ringWant;
        if (ringWant > 0.0) { rx().pipeline_.setReplayRing(&rt->ring); rt->attached = true; }
    }
    if (c.clipOn && !rt->clipRunning && rt->attached) {
        ClipWriter::Config wc;
        wc.directory = (std::filesystem::path(recordDir_) / "clips").string();
        wc.prefix = "radio" + std::to_string(scopedSlot() + 1u) + "_clip";
        wc.radio = static_cast<int>(scopedSlot() + 1u);
        wc.rateHz = kAudioRate;
        rt->writer = std::make_unique<ClipWriter>(wc);
        ClipSettings cs;
        cs.rateHz = kAudioRate; cs.preRollS = c.preS; cs.tailS = c.tailS; cs.minS = c.minS; cs.maxClipS = c.maxS;
        rt->clipSet = rt->trig.configure(cs);
        rt->trig.reset();
        RecRt* raw = rt;
        raw->worker = std::thread([raw] {
            while (!raw->stop.load()) {
                std::vector<ClipEvent> ev;
                raw->writer->service(raw->ring, &ev);
                if (!ev.empty()) { std::lock_guard<std::mutex> lk(raw->evMu); raw->events.insert(raw->events.end(), ev.begin(), ev.end()); }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        rt->clipRunning = true;
    } else if (!c.clipOn && rt->clipRunning) {
        // switched off: the clip under way is closed where it is, then the worker stops
        ClipCmd out[2];
        const int n = rt->trig.endNow(rt->ring.head(), out, 2);
        for (int i = 0; i < n; ++i) { rt->writer->push(out[i]); }
        rt->stopWorker();
        std::vector<ClipEvent> ev;
        rt->writer->finishAll(rt->ring, &ev);
        std::lock_guard<std::mutex> lk(rt->evMu);
        rt->events.insert(rt->events.end(), ev.begin(), ev.end());
        rt->clipRunning = false;
    }
    if (rt->clipRunning) {
        const ClipSettings cur = rt->trig.settings();
        if (cur.preRollS != c.preS || cur.tailS != c.tailS || cur.minS != c.minS || cur.maxClipS != c.maxS) {
            ClipSettings cs = cur;
            cs.preRollS = c.preS; cs.tailS = c.tailS; cs.minS = c.minS; cs.maxClipS = c.maxS;
            rt->clipSet = rt->trig.configure(cs);
        }
        ClipCmd out[2];
        const auto unixNow = static_cast<std::int64_t>(tMs / 1000);
        const int n = rt->trig.update(rx().pipeline_.squelchOpen(), rt->ring.head(), unixNow, currentAbsoluteHz(),
                                      cascade::gui::kModeNames[rx().modeIndex_ >= 0 && rx().modeIndex_ < 8 ? rx().modeIndex_ : 0], out, 2);
        for (int i = 0; i < n; ++i) { rt->writer->push(out[i]); }
    }
    {   // what the worker reported: a clip that could not be saved is said in words and counted
        std::vector<ClipEvent> ev;
        { std::lock_guard<std::mutex> lk(rt->evMu); ev.swap(rt->events); }
        for (const ClipEvent& e : ev) {
            if (e.kind == ClipEvent::Kind::Failed || e.writeFailed) {
                cascade::core::health::noteRecordFailed();
                rx().recordError_ = "A clip could not be saved. Check that the recordings folder exists, can be written to and has room; clips are in its \"clips\" folder.";
            }
        }
    }

    // ---------------------------------------------------------------- the rollover of a take
    struct Kind { bool iq; Recorder& rec; cascade::gui::RecordStart& next; std::uint64_t& seen; std::string& seenPath; std::uint64_t& rolled; };
    Kind kinds[2] = {{true, rx().iqRecorder_, rt->nextIq, rt->seenRollIq, rt->seenPathIq, rt->rolledIq}, {false, rx().audioRecorder_, rt->nextAudio, rt->seenRollAudio, rt->seenPathAudio, rt->rolledAudio}};
    for (Kind& k : kinds) {
        Recorder& rec = k.rec;
        const double rate = k.iq ? rx().pipeline_.inputRateHz() : kAudioRate;
        const bool on = rolling && rec.recording();
        if (!on) {
            rec.setRollover(0, 0);
            // a next file that was opened for a take that has ended is closed and removed: it holds no sample
            std::string unused;
            if (rec.takeNextUnused(unused) && !unused.empty()) { std::error_code ec; std::filesystem::remove(unused, ec); }
            cascade::gui::RecordStart::Result r;
            if (k.next.poll(r) && r.ok) { const std::string p = r.file.path; r.file = Recorder::OpenedFile{}; std::error_code ec; std::filesystem::remove(p, ec); }
            k.seen = ~0ull;
        } else {
            // the limits of the file that is open now: the size as bytes of samples; the time as frames (from the clock, when asked: the file ends at the next whole multiple of the period, UTC)
            const std::uint64_t cur = rec.rolloverCount();
            if (k.seen != cur || k.seenPath != rec.path()) {
                k.seen = cur;
                k.seenPath = rec.path();
                const std::uint64_t bytes = c.rollMB > 0.0 ? static_cast<std::uint64_t>(c.rollMB * 1.0e6) : 0ull;
                std::uint64_t frames = 0;
                if (c.rollMin > 0.0) {
                    double seconds = c.rollMin * 60.0;
                    if (c.align) {
                        const double nowS = static_cast<double>(tMs) / 1000.0;
                        const double into = std::fmod(nowS, c.rollMin * 60.0);
                        seconds = std::max(0.001, c.rollMin * 60.0 - into);
                    }
                    frames = static_cast<std::uint64_t>(std::max(1.0, seconds * rate));
                }
                rec.setRollover(bytes, frames);
            }
            if (rec.rolloverWanted() && !k.next.pending() && tMs >= rt->retryAtMs) {
                // the cheap half of a new take's start, on a scratch recorder (the real one is recording and refuses): the same format, container, name and folders as any take
                Recorder scratch;
                scratch.setSampleFormat(k.iq ? cascade::core::RecordKind::BasebandIq : cascade::core::RecordKind::Audio, rec.sampleFormat(k.iq ? cascade::core::RecordKind::BasebandIq : cascade::core::RecordKind::Audio));
                if (k.iq) { (void)scratch.setIqContainer(rec.iqContainer()); scratch.noteCentre(rx().pipeline_.activeSource().centerFrequencyHz()); }
                Recorder::OpenRequest req;
                std::string err;
                if (scratch.prepare(k.iq ? cascade::core::RecordKind::BasebandIq : cascade::core::RecordKind::Audio, recordDir_, rate, takePrefix(k.iq), req, err)) {
                    applyRecordName(req);
                    (void)k.next.request(rec.opener(), std::move(req));
                } else {
                    rx().recordError_ = "The recording could not go on into a new file: " + err + ". It goes on in the file it has.";
                    cascade::core::health::noteRecordFailed();
                    rt->retryAtMs = tMs + 10000;
                }
            }
        }
        cascade::gui::RecordStart::Result r;
        if (k.next.poll(r)) {
            if (r.cancelled) {
                // nothing to do: the take was stopped
            } else if (!r.ok) {
                // RecordStart::poll has counted the failed open; the person is told
                rx().recordError_ = "The recording could not go on into a new file (the folder is missing, cannot be written to or is full). It goes on in the file it has.";
                rt->retryAtMs = tMs + 10000;
            } else if (rec.recording()) {
                rec.queueNext(std::move(r.file));
            } else {
                const std::string p = r.file.path;
                r.file = Recorder::OpenedFile{};
                std::error_code ec;
                std::filesystem::remove(p, ec);
            }
        }
        Recorder::FinishRequest fr;
        while (rec.takeRolled(fr)) {
            rt->tickets.push_back(RecordFinisher::submit(std::move(fr)));
            ++k.rolled;
        }
    }
    {   // the facts above are read from the fact block, which is republished when a version moves: this is that version for them
        char sg[160];
        std::snprintf(sg, sizeof sg, "%g|%llu|%llu|%d|%d|%d|%d", rt->ringSeconds, static_cast<unsigned long long>(rt->rolledIq), static_cast<unsigned long long>(rt->rolledAudio),
                      rt->writer ? rt->writer->clipsSaved() : 0, rt->writer ? rt->writer->clipsDropped() : 0, rt->writer ? rt->writer->clipsFailed() : 0, rt->writer && rt->writer->clipOpen() ? 1 : 0);
        if (rt->lastSig != sg) { rt->lastSig = sg; ++wireCVersion_; }
    }
    for (auto it = rt->tickets.begin(); it != rt->tickets.end();) {
        if ((*it)->done.load()) {
            if (!(*it)->ok.load()) {
                cascade::core::health::noteRecovered(cascade::core::health::Recovered::TakeHeader);
                cascade::core::diagWarnf("recorder: a file the recording went on from could not be finalised - it keeps the header it was opened with");
            }
            it = rt->tickets.erase(it);
        } else {
            ++it;
        }
    }
}

// ============================================================ the replay buffer ahead of an audio take ============================================================
void Engine::recplayPreRoll(Recorder& rec) {
    RecRt* rt = rtOf(rx(), false);
    if (rt == nullptr || !rt->attached || rt->ringSeconds <= 0.0) { return; }
    const double want = wireC().settings().getReal("rec.replay_s");
    if (!(want > 0.0)) { return; }
    const std::uint64_t head = rt->ring.head();
    const std::uint64_t held = head - rt->ring.oldest();
    const std::uint64_t n = std::min<std::uint64_t>(held, static_cast<std::uint64_t>(want * kAudioRate));
    if (n == 0) { return; }
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(std::min<std::uint64_t>(n, 1u << 20)));
    std::vector<float> f;
    std::uint64_t at = head - n;
    std::uint64_t left = n;
    while (left > 0) {
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(left, pcm.size()));
        std::uint64_t first = at;
        const std::size_t got = rt->ring.copyRange(at, take, pcm.data(), &first);
        if (got == 0) { break; }
        f.resize(got);
        for (std::size_t i = 0; i < got; ++i) { f[i] = static_cast<float>(pcm[i]) / 32768.0f; }
        rec.writeAudio(f.data(), got);   // the recorder has begun and the pipeline has not been given it yet: no write is in flight
        at = first + got;
        left = n > (at - (head - n)) ? n - (at - (head - n)) : 0;
    }
}

}  // namespace cascade::engine
