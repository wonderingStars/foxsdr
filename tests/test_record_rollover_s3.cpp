// test_record_rollover_s3.cpp - A TAKE THAT GOES ON INTO A NEXT FILE (screen lane S3, G3-10; docs/multi-radio-records/screen-s3-recplay.md).
//
// The recorder's writing thread swaps files between two blocks when a limit is reached and a next file was handed over; the control thread opens the next file, hands it over and finishes the
// file that was left. Proved here with the two threads really running:
//   A  by frames: every file holds about the limit, the files in order are ONE ramp - no sample lost, none repeated - and the headers say what each file holds
//   B  by bytes (I/Q): the same, with 8-byte frames
//   C  no next file ready in time: the take goes on in its file, nothing is lost, nothing hangs
//   D  a limit set after the take began; no limit (0) never rotates
//   E  a next file handed over for a take that ended is returned unused (and its name) and holds no sample
//   F  a next file whose opener failed is never queued: the engine's side; here a take that never gets one still ends cleanly
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/recorder.hpp"
#include "test_check.hpp"

using cascade::core::RecordKind;
using cascade::core::Recorder;
namespace fs = std::filesystem;

namespace {

fs::path dir(const char* tag) {
    const fs::path d = fs::temp_directory_path() / (std::string("foxsdr_test_rollover_s3_") + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::vector<unsigned char> readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }

// Opens the next file the way the engine's pass does (a worker's opener; here the same call on the control thread).
bool openNext(Recorder& scratch, RecordKind kind, const fs::path& d, double rate, int tag, Recorder::OpenedFile& out) {
    Recorder::OpenRequest req;
    std::string err;
    if (!scratch.prepare(kind, d.string(), rate, "roll" + std::to_string(tag), req, err)) { return false; }
    req.path = (d / ("part" + std::to_string(1000 + tag) + ".wav")).string();
    return Recorder::openFile(req, out, err);
}

struct Outcome { std::vector<std::string> files; std::uint64_t written = 0; };

// Runs one take of `blocks` blocks of `block` frames on a writer thread, rolling with `maxFrames`/`maxBytes`; the control thread (this one) keeps the next file ready unless `starve`.
Outcome run(RecordKind kind, const fs::path& d, std::uint64_t maxBytes, std::uint64_t maxFrames, int blocks, std::size_t block, bool starve, bool afterStart = false) {
    Outcome o;
    const double rate = kind == RecordKind::Audio ? 48000.0 : 250000.0;
    Recorder rec;
    std::string err;
    CHECK(rec.start(kind, d.string(), rate, err, "roll0"));
    o.files.push_back(rec.path());
    if (!afterStart) { rec.setRollover(maxBytes, maxFrames); }
    Recorder scratch;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        std::vector<float> a(block);
        std::vector<std::complex<float>> q(block);
        std::uint64_t idx = 0;
        for (int b = 0; b < blocks; ++b) {
            for (std::size_t i = 0; i < block; ++i, ++idx) {
                const float v = static_cast<float>(idx % 30000) / 32768.0f;
                a[i] = v;
                q[i] = {v, -v};
            }
            if (kind == RecordKind::Audio) { rec.writeAudio(a.data(), block); } else { rec.writeIq(q.data(), block); }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        done.store(true);
    });
    int tag = 1;
    std::size_t takenPaths = 0;
    bool setLate = afterStart;
    while (!done.load()) {
        if (setLate && rec.samplesWritten() > block * 3) { rec.setRollover(maxBytes, maxFrames); setLate = false; }
        if (!starve && rec.rolloverWanted()) {
            Recorder::OpenedFile f;
            if (openNext(scratch, kind, d, rate, tag, f)) { o.files.push_back(f.path); ++tag; rec.queueNext(std::move(f)); }
        }
        Recorder::FinishRequest fr;
        while (rec.takeRolled(fr)) { CHECK(Recorder::finishFile(std::move(fr))); }
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    writer.join();
    Recorder::FinishRequest fr;
    while (rec.takeRolled(fr)) { CHECK(Recorder::finishFile(std::move(fr))); }
    (void)takenPaths;
    rec.stop();
    // a next file queued but never used
    std::string unused;
    if (rec.takeNextUnused(unused)) { std::error_code ec; fs::remove(unused, ec); o.files.erase(std::remove(o.files.begin(), o.files.end(), unused), o.files.end()); }
    o.written = static_cast<std::uint64_t>(blocks) * block;
    return o;
}

// The samples of every file, in order, as the ramp index they must be; false at the first break. Returns the count.
bool continuous(const Outcome& o, RecordKind kind, std::uint64_t& total, std::vector<std::uint64_t>& perFile) {
    std::uint64_t expect = 0;
    total = 0;
    perFile.clear();
    for (const std::string& f : o.files) {
        const auto b = readFile(f);
        if (b.size() < 44) { return false; }
        const std::uint32_t dataBytes = le32(b.data() + 40);
        const std::size_t fb = kind == RecordKind::Audio ? 2 : 8;
        if (dataBytes != b.size() - 44) { return false; }   // the header says what the file holds
        const std::uint64_t frames = dataBytes / fb;
        for (std::uint64_t i = 0; i < frames; ++i) {
            float v;
            if (kind == RecordKind::Audio) {
                const auto s = static_cast<std::int16_t>(b[44 + 2 * i] | (b[45 + 2 * i] << 8));
                v = static_cast<float>(s);
            } else {
                const std::uint32_t u = le32(b.data() + 44 + 8 * i);
                float r; std::memcpy(&r, &u, 4);
                v = r * 32768.0f;
            }
            if (static_cast<std::uint64_t>(std::lround(v)) != expect % 30000) { return false; }
            ++expect;
        }
        perFile.push_back(frames);
        total += frames;
    }
    return true;
}

}  // namespace

int main() {
    // ---- A: audio, by frames
    {
        const fs::path d = dir("a");
        const Outcome o = run(RecordKind::Audio, d, 0, 4800, 400, 480, false);   // 192 000 frames, 4 s: files of about 4800 frames
        std::uint64_t total = 0; std::vector<std::uint64_t> per;
        CHECK(continuous(o, RecordKind::Audio, total, per));
        CHECK(total == o.written);                                  // no frame lost, none repeated
        CHECK(per.size() >= 20);                                    // it did go on into new files
        for (std::size_t i = 0; i + 1 < per.size(); ++i) { CHECK(per[i] >= 4800 && per[i] <= 4800 + 480); }   // each file ends at the first block boundary after the limit
    }
    // ---- B: I/Q, by bytes
    {
        const fs::path d = dir("b");
        const Outcome o = run(RecordKind::BasebandIq, d, 80000, 0, 200, 500, false);   // 100 000 frames of 8 bytes; files of about 80 000 bytes = 10 000 frames
        std::uint64_t total = 0; std::vector<std::uint64_t> per;
        CHECK(continuous(o, RecordKind::BasebandIq, total, per));
        CHECK(total == o.written && per.size() >= 9);
        for (std::size_t i = 0; i + 1 < per.size(); ++i) { CHECK(per[i] * 8 >= 80000 && per[i] * 8 <= 80000 + 500 * 8); }
    }
    // ---- C: the next file never arrives: one file, nothing lost, no hang
    {
        const fs::path d = dir("c");
        const auto t0 = std::chrono::steady_clock::now();
        const Outcome o = run(RecordKind::Audio, d, 0, 4800, 100, 480, true);
        std::uint64_t total = 0; std::vector<std::uint64_t> per;
        CHECK(continuous(o, RecordKind::Audio, total, per) && total == o.written && per.size() == 1);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20));
    }
    // ---- D: a limit set after the take began; none at all
    {
        const fs::path d = dir("d");
        const Outcome o = run(RecordKind::Audio, d, 0, 4800, 300, 480, false, true);
        std::uint64_t total = 0; std::vector<std::uint64_t> per;
        CHECK(continuous(o, RecordKind::Audio, total, per) && total == o.written && per.size() >= 10);
        const fs::path d2 = dir("d2");
        const Outcome n = run(RecordKind::Audio, d2, 0, 0, 100, 480, false);
        CHECK(continuous(n, RecordKind::Audio, total, per) && total == n.written && per.size() == 1);
    }
    // ---- E: a next file for a take that ended is handed back unused and holds no sample
    {
        const fs::path d = dir("e");
        Recorder rec, scratch;
        std::string err;
        CHECK(rec.start(RecordKind::Audio, d.string(), 48000.0, err, "roll0"));
        Recorder::OpenedFile f;
        CHECK(openNext(scratch, RecordKind::Audio, d, 48000.0, 1, f));
        const std::string name = f.path;
        rec.queueNext(std::move(f));
        rec.stop();
        std::string unused;
        CHECK(rec.takeNextUnused(unused) && unused == name);
        CHECK(!rec.takeNextUnused(unused));
        CHECK(readFile(name).size() == 44);   // the zero-sample header only
        Recorder::FinishRequest fr;
        CHECK(!rec.takeRolled(fr) && rec.rolloverCount() == 0);
    }
    // ---- F: a limit that is never reached never rotates
    {
        const fs::path d = dir("f");
        const Outcome o = run(RecordKind::Audio, d, 1u << 30, 0, 100, 480, false);
        std::uint64_t total = 0; std::vector<std::uint64_t> per;
        CHECK(continuous(o, RecordKind::Audio, total, per) && per.size() == 1);
    }
    for (const char* t : {"a", "b", "c", "d", "d2", "e", "f"}) { std::error_code ec; fs::remove_all(fs::temp_directory_path() / (std::string("foxsdr_test_rollover_s3_") + t), ec); }
    return testSummary("test_record_rollover_s3");
}
