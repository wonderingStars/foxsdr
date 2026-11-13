// test_engine_api_wire_a_record.cpp - THE RECORDING GROUP OF WIRING LANE wire-a, held against the real engine through its API: a real take on the generator source, written to a scratch recordings folder.
//
//   R1  DAILY FOLDERS: with the default (none) a take lands straight in the recordings folder; with style 1 it lands in <recordings>/<today, UTC>/, with style 4 in <recordings>/<yyyymmdd>/; with a file-name
//       template as well the template's folders come below the day's; a bad style / template / JSON is refused and changes nothing; the same setting again is NO_CHANGE; the setting reaches
//       record-daily.json and a new engine reads it; a junction in the way keeps the take in its default place.
//
//   L1  LOCK WHILE RECORDING: during an I/Q take the sample rate, the source and the centre are refused with the model's sentence (and nothing moves), re-sending the frozen centre is allowed, the VFO may
//       still move; after the take every one is allowed again; an audio take freezes none of them; the gain lock is its own switch; the lock can be turned off and the setting is kept in record-lock.json.
//
//   C1  FLAC FOR AUDIO TAKES: the default is WAV; with FLAC chosen an audio take on the generator ends as a .flac (the WAV removed), whose STREAMINFO (rate, channels, bits, sample count, MD5) is checked against the
//       samples of the take's own WAV (a second take is made with "keep both" so the WAV is there to compare); a take with the container off stays a WAV; refusals; persistence.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "core/flac_writer.hpp"
#include "core/recorder.hpp"
#include "source/wav_meta.hpp"
#include "wire_a_rig.hpp"

using namespace wire_a_rig;

namespace {

fs::path g_dir;

std::vector<fs::path> wavsUnder(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) { return out; }
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".wav") { out.push_back(e.path()); }
    }
    return out;
}

std::string utcDate(bool compact) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, compact ? "%Y%m%d" : "%Y-%m-%d", &tm);
    return buf;
}

// One audio take: start, wait for the file, stop.
std::vector<fs::path> take(Rig& r, const fs::path& rec, std::size_t already) {
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return wavsUnder(rec).size() > already; }));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
    r.settle(700);
    return wavsUnder(rec);
}

void testDaily() {
    std::printf("R1: daily folders\n");
    const fs::path rec = g_dir / "rec1", user = g_dir / "user1";
    fs::create_directories(rec);
    {
        Rig r(user, rec);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.1e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        // default: straight in the folder
        std::vector<fs::path> w = take(r, rec, 0);
        CHECK_EQ(w.size(), std::size_t(1));
        if (!w.empty()) { CHECK(w[0].parent_path() == rec); }
        // refusals
        const std::string bad1 = R"({"style":9})", bad2 = "{nope", bad3 = R"({"template":"{nosuch}"})", bad4 = R"({"template":"../x"})", bad5 = R"({"utc":"yes"})";
        for (const std::string* b : {&bad1, &bad2, &bad3, &bad4, &bad5}) { CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), b).status(), FOXAPI_BAD_ARGUMENT); }
        const std::string none = R"({"style":0})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &none).status(), FOXAPI_NO_CHANGE);   // that is the default
        // style 1: <rec>/<today>/
        const std::string s1 = R"({"style":1,"utc":true})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &s1).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &s1).status(), FOXAPI_NO_CHANGE);
        CHECK(r.fact("record.daily").find("\"style\":1") != std::string::npos);
        w = take(r, rec, 1);
        CHECK_EQ(w.size(), std::size_t(2));
        bool inDay = false;
        for (const fs::path& p : w) { inDay = inDay || (p.parent_path() == rec / utcDate(false)); }
        CHECK(inDay);
        // style 4 with a file-name template: <rec>/<yyyymmdd>/<template folder>/<name>
        const std::string s4 = R"({"style":4})";
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &s4).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_NAME, 0.0, 0, "{freq_mhz}/{mode}_{year}")).status(), FOXAPI_OK);
        w = take(r, rec, 2);
        CHECK_EQ(w.size(), std::size_t(3));
        bool deep = false;
        for (const fs::path& p : w) { deep = deep || (p.parent_path() == rec / utcDate(true) / "100.1"); }
        CHECK(deep);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_NAME, 0.0, 0, "")).status(), FOXAPI_OK);
        // the setting reaches the file
        CHECK(r.until([&] { return fs::exists(user / "record-daily.json"); }, 6000));
    }
    {
        Rig r(user, rec);
        r.settle(200);
        CHECK(r.fact("record.daily").find("\"style\":4") != std::string::npos);   // a new engine reads it
    }
}

void testJunction() {
    std::printf("R2: a link in the way\n");
    const fs::path rec = g_dir / "rec2", user = g_dir / "user2", elsewhere = g_dir / "elsewhere";
    fs::create_directories(rec);
    fs::create_directories(elsewhere);
#if defined(_WIN32)
    // A junction made without a shell or a privilege: mklink /J through cmd would raise nothing visible, but the model's own test makes its own: here a directory symlink is not allowed without a
    // privilege, so the junction is made with the Win32 call the model's test uses (CreateSymbolicLink needs Developer Mode); when neither can be made the check is reported as skipped.
    const std::string day = utcDate(false);
    std::error_code ec;
    fs::create_directory_symlink(elsewhere, rec / day, ec);
    if (ec) {
        std::printf("  SKIPPED: no link could be made here (%s)\n", ec.message().c_str());
        return;
    }
#else
    std::error_code ec;
    fs::create_directory_symlink(elsewhere, rec / utcDate(false), ec);
#endif
    Rig r(user, rec);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.1e6, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    const std::string s1 = R"({"style":1,"utc":true})";
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_SET_RECORD_DAILY), &s1).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return !wavsUnder(rec).empty(); }));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
    r.settle(700);
    CHECK(wavsUnder(elsewhere).empty());   // nothing went through the link
    const std::vector<fs::path> w = wavsUnder(rec);
    // (the symlink itself is a directory entry: recursive_directory_iterator does not follow it, so the take is the one directly in the folder)
    CHECK(!w.empty());
    if (!w.empty()) { CHECK(w[0].parent_path() == rec); }
}

void testLock() {
    std::printf("L1: lock while recording\n");
    const fs::path rec = g_dir / "rec3", user = g_dir / "user3";
    fs::create_directories(rec);
    {
        Rig r(user, rec);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.lock"), std::string("0"));   // the default is off: today's behaviour
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 0)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.lock"), std::string("1"));
        // an I/Q take. Its file is opened by a worker: the file is on disk BEFORE the engine collects the open and arms the recorder. That window is a few milliseconds in
        // real life and made 1.5 s wide here, so the retune below arrives inside it every time (before engine fix 4 it was accepted in the window: the test then passed
        // or failed with the machine's speed, about 1 run in 15).
        Access::slowIqOpenOnce(r.eng, 1500);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return !wavsUnder(rec).empty(); }));
        testing::Outcome o = r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 101.0e6, 0));
        CHECK_EQ(o.status(), FOXAPI_DENIED);
        CHECK(std::string(o.result.message).find("centre frequency is locked while an I/Q recording is open") != std::string::npos);
        CHECK(r.fact("record.lock.reason").find("Stop the recording first") != std::string::npos);   // the whole sentence (the reply's message is short)
        CHECK(r.fact("record.lock.reason").find("the recording stores one centre frequency") != std::string::npos);
        CHECK_EQ(r.fact("tuned.1.centreHz"), std::string("100000000"));   // nothing moved
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6, 0)).status(), FOXAPI_OK);   // the frozen value again is allowed
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SAMPLE_RATE, 1.0e6, 0)).status(), FOXAPI_DENIED);
        FoxCommand src = testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0.0, 0, "siggen");
        CHECK_EQ(r.send(src).status(), FOXAPI_DENIED);
        CHECK(r.fact("record.lock.frozen.1").find("sample rate") != std::string::npos);
        CHECK(r.fact("record.lock.frozen.1").find("centre frequency") != std::string::npos);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_VFO_OFFSET, 5000.0, 0)).status(), FOXAPI_OK);   // the VFO is the user's to move
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_GAIN, 10.0, 0)).status() == FOXAPI_DENIED, false);   // gain is free unless the gain lock is on
        // the slow open comes back and the take is armed: the lock holds on the same frozen values; then the take ends and everything is allowed again
        r.settle(1800);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 101.0e6, 0)).status(), FOXAPI_DENIED);
        CHECK_EQ(r.fact("tuned.1.centreHz"), std::string("100000000"));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
        r.settle(700);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 101.0e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.lock.frozen.1"), std::string());
        // an audio take freezes none of these
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
        r.settle(300);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 102.0e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
        r.settle(700);
        // the switch: off allows the change during a take; on again refuses; the same setting is NO_CHANGE; a bad value is refused
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 1)).status(), FOXAPI_NO_CHANGE);   // on already
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 5)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.lock"), std::string("0"));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
        r.settle(300);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 103.0e6, 0)).status(), FOXAPI_OK);   // not locked
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
        r.settle(700);
        // gain lock: its own switch
        FoxCommand both = testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 1);
        both.ival[1] = 1;
        CHECK_EQ(r.send(both).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.lock.gain"), std::string("1"));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
        r.settle(300);
        // (the generator has no device, so SET_GAIN is answered NO_DEVICE before the engine's apply: the gain lock is held by its switch and facts here, UNVERIFIED against a real device)
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
        r.settle(700);
        CHECK(r.until([&] {
            std::ifstream f(user / "record-lock.json");
            std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return t.find("\"gain\":true") != std::string::npos;
        }, 6000));
    }
    {
        Rig r(user, rec);
        r.settle(200);
        CHECK_EQ(r.fact("record.lock.gain"), std::string("1"));   // a new engine reads the switches
    }
}

std::vector<fs::path> filesWithExt(const fs::path& dir, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ext) { out.push_back(e.path()); }
    }
    return out;
}

// The finish hook: the NEXT take's file takes a moment to close, so a Record pressed behind it is QUEUED (Engine::iqStartQueued_), not started.
void slowClose(std::FILE*) { std::this_thread::sleep_for(std::chrono::milliseconds(2500)); }

void testLockQueued() {
    std::printf("L2: lock while a take is queued behind a finishing one\n");
    const fs::path rec = g_dir / "rec6", user = g_dir / "user6";
    fs::create_directories(rec);
    Rig r(user, rec);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_LOCK, 0.0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return !wavsUnder(rec).empty(); }));
    r.settle(500);
    cascade::core::Recorder::setFinishHookForTest(&slowClose);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);   // the take ends; its file is still closing
    r.settle(300);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 101.0e6, 0)).status(), FOXAPI_OK);   // nothing is recording and nothing is queued yet: allowed
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);        // pressed behind the finish: queued
    testing::Outcome o = r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 102.0e6, 0));
    CHECK_EQ(o.status(), FOXAPI_DENIED);                                                   // the person has asked for a take: the centre is frozen
    CHECK_EQ(r.fact("tuned.1.centreHz"), std::string("100000000"));
    r.settle(3500);                                                                        // the close ends, the queued take starts
    cascade::core::Recorder::setFinishHookForTest(nullptr);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 102.0e6, 0)).status(), FOXAPI_DENIED);   // and the lock holds in the take itself
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
    r.settle(3000);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CENTRE, 102.0e6, 0)).status(), FOXAPI_OK);
}

void testFlac() {
    std::printf("C1: FLAC for audio takes\n");
    const fs::path rec = g_dir / "rec4", user = g_dir / "user4";
    fs::create_directories(rec);
    {
        Rig r(user, rec);
        CHECK_EQ(r.fact("record.container"), std::string("wav"));
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_CONTAINER, 0.0, 0)).status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_CONTAINER, 0.0, 7)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.1e6, 0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
        // WAV by default
        {
            CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
            r.settle(700);
            CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
            r.settle(1200);
            CHECK_EQ(filesWithExt(rec, ".wav").size(), std::size_t(1));
            CHECK(filesWithExt(rec, ".flac").empty());
        }
        // FLAC, keep both
        FoxCommand keep = testing::cmd(FOXAPP_OP_RECORD_CONTAINER, 0.0, 1);
        keep.ival[1] = 1;
        CHECK_EQ(r.send(keep).status(), FOXAPI_OK);
        CHECK_EQ(r.fact("record.container"), std::string("flac"));
        CHECK_EQ(r.fact("record.container.keep"), std::string("1"));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
        r.settle(900);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("job.name") == "FLAC conversion" && r.fact("job.state") != "running"; }, 20000));
        CHECK_EQ(r.fact("job.state"), std::string("done"));
        const std::vector<fs::path> flacs = filesWithExt(rec, ".flac");
        CHECK_EQ(flacs.size(), std::size_t(1));
        CHECK_EQ(filesWithExt(rec, ".wav").size(), std::size_t(2));   // both kept
        if (!flacs.empty()) {
            fs::path wav = flacs[0];
            wav.replace_extension(".wav");
            CHECK(fs::exists(wav));
            // STREAMINFO of the FLAC against the WAV's own samples
            std::ifstream f(flacs[0], std::ios::binary);
            std::vector<unsigned char> head(8 + 34);
            f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
            CHECK(head[0] == 'f' && head[1] == 'L' && head[2] == 'a' && head[3] == 'C');
            const unsigned char* si = head.data() + 8;
            const unsigned rate = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
            const unsigned channels = ((si[12] >> 1) & 7) + 1;
            const unsigned bps = (((si[12] & 1) << 4) | (si[13] >> 4)) + 1;
            const std::uint64_t total = (static_cast<std::uint64_t>(si[13] & 15) << 32) | (static_cast<std::uint64_t>(si[14]) << 24) | (si[15] << 16) | (si[16] << 8) | si[17];
            const cascade::source::wavmeta::WavMeta m = cascade::source::wavmeta::readWavMeta(wav);
            CHECK(m.ok);
            CHECK_EQ(rate, m.sampleRateHz);
            CHECK_EQ(channels, m.channels);
            CHECK_EQ(bps, 16u);   // the default audio take is 16-bit
            CHECK_EQ(total, m.dataBytes / (m.channels * (m.bitsPerSample / 8)));
            // the MD5 of the samples, made here from the WAV (the RFC's definition: little-endian samples sign-extended to whole bytes; 16-bit = the samples as they are)
            std::ifstream w(wav, std::ios::binary);
            w.seekg(static_cast<std::streamoff>(m.dataOffset));
            std::vector<unsigned char> pcm(static_cast<std::size_t>(m.dataBytes));
            w.read(reinterpret_cast<char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
            cascade::core::flac::Md5 md5;
            md5.update(pcm.data(), pcm.size());
            const auto sum = md5.finish();
            bool same = true;
            for (int i = 0; i < 16; ++i) { same = same && sum[i] == si[18 + i]; }
            CHECK(same);
        }
        // FLAC, WAV removed
        CHECK_EQ(r.send(testing::cmd(FOXAPP_OP_RECORD_CONTAINER, 0.0, 1)).status(), FOXAPI_OK);
        const std::size_t flacBefore = filesWithExt(rec, ".flac").size();
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
        r.settle(700);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return filesWithExt(rec, ".flac").size() > flacBefore; }, 20000));
        r.settle(500);
        CHECK_EQ(filesWithExt(rec, ".wav").size(), std::size_t(2));   // the new take's WAV is gone; the two kept ones remain
        CHECK(r.until([&] { return fs::exists(user / "record-container.json"); }, 6000));
    }
    {
        Rig r(user, rec);
        r.settle(200);
        CHECK_EQ(r.fact("record.container"), std::string("flac"));   // a new engine reads it
    }
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_a_record");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_dir = fs::temp_directory_path() / ("foxsdr_wire_a_record_" + std::to_string(::_getpid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testDaily();
    testJunction();
    testLock();
    testLockQueued();
    testFlac();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_wire_a_record");
}
