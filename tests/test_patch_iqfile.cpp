// Tests for the patch Radio's I/Q RECORDING device (0.99.40): a Radio node
// that plays a 2-channel WAV (source/iq_file_source.hpp) on a loop, in real
// time, as though it were a radio - so a patch can be built, decoded and put
// on a map with no radio on the desk.
//
// WHAT IS PINNED, each against something the test made itself:
//
//   [K] the device key: "iqfile|path=<path>", the path back byte for byte
//       whatever it holds (a comma, "serial=", spaces, UTF-8) - so a key is
//       never read through the "a=1,b=2" argument grammar the radio keys use;
//   [D] one recording, one Radio (the DeviceTwice rule, as for hardware): the
//       same file twice is the same device - on Windows whatever its case or
//       slashes - two files are two devices, and a recording is never the
//       same device as a radio or the generator;
//   [R] the rate belongs to the file: a node's rate setting never makes a
//       recording a different device to open, and the recording's converter
//       is the receiver's I/Q file one;
//   [L] the picker's list: every playable WAV in the folders asked, each once,
//       with its header's rate - and nothing IqFileSource would refuse;
//   [O] opening one: the header's rate, the node's centre as the file's centre
//       (nominal - nothing is "tuned"), and IqFileSource's own reason when it
//       will not open;
//   [P] THREE RECORDINGS PLAYING AT ONCE on three PatchRadios, each paced to
//       real time (not a minute of signal a second), each hearing its own tone
//       where it was written, and a centre change moving the label and not
//       the signal;
//   [S] the document: a key full of awkward characters survives serialise and
//       parse, in the format documents are already written in;
//   [M] FIVE recordings, five decoders, ONE map: the owner's "pipe up to 5 sdrs
//       in to a single map" at the plan level, with every radio a recording.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/patch_devices.hpp"
#include "core/patch_graph.hpp"
#include "core/patch_io.hpp"
#include "core/patch_plan.hpp"
#include "core/patch_radio.hpp"
#include "core/patch_recordings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "test_check.hpp"

using namespace cascade::core::patch;
namespace fs = std::filesystem;

namespace {

constexpr double kTwoPi = 6.28318530717958647692;

void put16(std::ofstream& f, std::uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>(v >> 8)};
    f.write(b, 2);
}
void put32(std::ofstream& f, std::uint32_t v) {
    const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                       static_cast<char>((v >> 16) & 0xFF), static_cast<char>(v >> 24)};
    f.write(b, 4);
}

// A recording of one complex tone `toneHz` from centre: I = cos, Q = sin, at
// half scale, `seconds` long. PCM16 or float32; `channels` other than 2 makes
// a file IqFileSource must refuse.
void writeToneWav(const fs::path& p, std::uint32_t rate, double toneHz, double seconds,
                  bool float32 = false, std::uint16_t channels = 2) {
    const std::uint32_t frames = static_cast<std::uint32_t>(rate * seconds);
    const std::uint16_t bits = float32 ? 32 : 16;
    const std::uint32_t bpf = channels * (bits / 8u);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write("RIFF", 4);
    put32(f, 36 + frames * bpf);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put32(f, 16);
    put16(f, float32 ? 3 : 1);
    put16(f, channels);
    put32(f, rate);
    put32(f, rate * bpf);
    put16(f, static_cast<std::uint16_t>(bpf));
    put16(f, bits);
    f.write("data", 4);
    put32(f, frames * bpf);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double ph = kTwoPi * toneHz * static_cast<double>(i) / rate;
        const double v[2] = {0.5 * std::cos(ph), 0.5 * std::sin(ph)};
        for (std::uint16_t c = 0; c < channels; ++c) {
            if (float32) {
                const float x = static_cast<float>(v[c % 2]);
                f.write(reinterpret_cast<const char*>(&x), 4);
            } else {
                put16(f, static_cast<std::uint16_t>(static_cast<std::int16_t>(v[c % 2] * 32767.0)));
            }
        }
    }
}

// The strongest bin of a 2048-point fftshifted spectrum, in Hz from centre.
double peakHz(const std::vector<float>& db, double rate) {
    const auto peak = std::max_element(db.begin(), db.end()) - db.begin();
    return (static_cast<double>(peak) - 1024.0) * rate / 2048.0;
}

}  // namespace

int main() {
    const fs::path root =
        fs::temp_directory_path() / ("cascade-patch-iqfile-" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "recordings", ec);
    fs::create_directories(root / "samples" / "deeper", ec);

    // --- [K] the key -------------------------------------------------------------
    {
        // Every awkward thing a Windows file name may hold, and the two that
        // would fool the "a=1,b=2" grammar: a comma and "serial=".
        const std::string path = "C:\\Rec ordings\\1090,serial=7 \xC3\xA9t\xC3\xA9 %2D.wav";
        const std::string key = makeIqFileKey(path);
        CHECK(key == std::string("iqfile|path=") + path);
        CHECK(isIqFileKey(key));
        CHECK(iqFilePath(key) == path);
        CHECK(deviceDriver(key) == kIqFileDriver);
        CHECK(!isGeneratorKey(key));
        CHECK(!isIqFileKey("siggen"));
        CHECK(!isIqFileKey("rtlsdr|serial=1"));
        CHECK(!isIqFileKey("iqfile|path="));   // no file named
        CHECK(iqFilePath("rtlsdr|path=C:\\x.wav").empty());
    }

    // --- [D] one recording, one Radio ------------------------------------------
    {
        const std::string a = makeIqFileKey("C:\\rec\\a.wav");
        const std::string b = makeIqFileKey("C:\\rec\\b.wav");
        CHECK(sameDevice(a, a));
        CHECK(!sameDevice(a, b));
        CHECK(!sameDevice(a, "siggen"));
        CHECK(!sameDevice(a, "rtlsdr|serial=1"));
        CHECK(!sameDevice("rtlsdr|serial=1", a));
        // The comma+serial in a PATH must not make two files "the same radio"
        // by the serial rule.
        CHECK(!sameDevice(makeIqFileKey("C:\\x,serial=1.wav"), makeIqFileKey("C:\\y,serial=1.wav")));
        CHECK(!sameDevice(makeIqFileKey("C:\\x,serial=1.wav"), "rtlsdr|serial=1"));
#if defined(_WIN32)
        // One file, however it is spelled to the file system.
        CHECK(sameDevice(makeIqFileKey("C:\\Rec\\A.wav"), makeIqFileKey("c:/rec/a.WAV")));
#endif
        // And in the plan: the second radio on the same file is DeviceTwice,
        // a radio on another file is not.
        Graph g;
        const NodeId r1 = g.addNode(NodeKind::Radio, "R1", PortType::Iq);
        const NodeId r2 = g.addNode(NodeKind::Radio, "R2", PortType::Iq);
        const NodeId r3 = g.addNode(NodeKind::Radio, "R3", PortType::Iq);
        g.mutableNode(r1)->device = a;
        g.mutableNode(r2)->device = a;
        g.mutableNode(r3)->device = b;
        const Plan p = compile(g, {{r1, 48000.0, 1e8}, {r2, 48000.0, 1e8}, {r3, 48000.0, 1e8}});
        const auto has = [&p](NodeId n, Problem pr) {
            return std::any_of(p.problems.begin(), p.problems.end(),
                               [&](const NodeProblem& x) { return x.node == n && x.problem == pr; });
        };
        CHECK(!has(r1, Problem::DeviceTwice));
        CHECK(has(r2, Problem::DeviceTwice));
        CHECK(!has(r3, Problem::DeviceTwice));
    }

    // --- [R] the rate belongs to the file ----------------------------------------
    {
        const std::string f = makeIqFileKey("C:\\rec\\a.wav");
        CHECK(deviceSetsItsOwnRate(f));
        CHECK(!deviceSetsItsOwnRate("siggen"));
        CHECK(!deviceSetsItsOwnRate("rtlsdr|serial=1"));
        // A node's rate setting does not make a recording a different thing to
        // open - otherwise following the file's rate would reopen it forever.
        CHECK(radioOpenIdentity(f, 2.0e6) == radioOpenIdentity(f, 48000.0));
        // ...while it does for a radio, whose rate IS a setting.
        CHECK(radioOpenIdentity("rtlsdr|serial=1", 2.0e6) !=
              radioOpenIdentity("rtlsdr|serial=1", 2.4e6));
        CHECK(radioOpenIdentity("siggen", 2.0e6) != radioOpenIdentity("siggen", 1.0e6));
        // Its converter is the receiver's one for I/Q files.
        CHECK(converterKeyForDevice(f) == "file");
        CHECK(converterKeyForDevice("rtlsdr|serial=1") == "rtlsdr|serial=1");
        CHECK(converterKeyForDevice("siggen") == "siggen");
    }

    // --- [L] the picker's list ------------------------------------------------------
    const fs::path pcm = root / "recordings" / "b_tone_48k.wav";
    const fs::path flt = root / "samples" / "a_tone_250k.wav";
    const fs::path upper = root / "samples" / "C_TONE_96K.WAV";
    writeToneWav(pcm, 48000, 5000.0, 1.0);
    writeToneWav(flt, 250000, 40000.0, 1.0, true);
    writeToneWav(upper, 96000, -10000.0, 1.0);
    writeToneWav(root / "recordings" / "mono.wav", 48000, 1000.0, 0.2, false, 1);
    {
        std::ofstream t(root / "recordings" / "notes.txt");
        t << "not a recording\n";
    }
    {
        std::ofstream t(root / "recordings" / "broken.wav", std::ios::binary);
        t << "RIFF";
    }
    writeToneWav(root / "samples" / "deeper" / "nested.wav", 48000, 1000.0, 0.2);
    {
        const std::vector<RecordingInfo> got = listIqRecordings(
            {(root / "recordings").string(), (root / "samples").string(),
             (root / "samples").string(),              // asked twice: listed once
             (root / "no such folder").string(), ""});  // absent and empty: ignored
        std::printf("listed %zu recording(s)\n", got.size());
        for (const RecordingInfo& r : got) {
            std::printf("  %s  %.0f S/s  %s\n", r.fileName.c_str(), r.rateHz, r.key.c_str());
        }
        CHECK(got.size() == 3u);
        if (got.size() == 3u) {
            // By file name, whichever folder it is in.
            CHECK(got[0].fileName == "a_tone_250k.wav");
            CHECK(got[1].fileName == "b_tone_48k.wav");
            CHECK(got[2].fileName == "C_TONE_96K.WAV");
            CHECK(got[0].rateHz == 250000.0);
            CHECK(got[1].rateHz == 48000.0);
            CHECK(got[2].rateHz == 96000.0);
            CHECK(got[1].key == makeIqFileKey(pcm.string()));
            CHECK(fs::path(iqFilePath(got[0].key)) == flt);
        }
        // The cap holds.
        CHECK(listIqRecordings({(root / "recordings").string(), (root / "samples").string()}, 2)
                  .size() == 2u);
        CHECK(listIqRecordings({}).empty());
    }

    // --- [O] opening one -------------------------------------------------------------
    {
        std::string err;
        auto src = openIqRecording(makeIqFileKey(pcm.string()), 162.0e6, err);
        CHECK(src != nullptr);
        CHECK(err.empty());
        if (src) {
            CHECK(src->sampleRateHz() == 48000.0);
            CHECK(src->centerFrequencyHz() == 162.0e6);
            CHECK(!src->selfPaced());   // paced by the reader, as the generator is
        }
        auto mono = openIqRecording(makeIqFileKey((root / "recordings" / "mono.wav").string()),
                                    1e8, err);
        CHECK(mono == nullptr);
        CHECK(err.find("2 channels") != std::string::npos);
        auto none = openIqRecording(makeIqFileKey((root / "absent.wav").string()), 1e8, err);
        CHECK(none == nullptr);
        CHECK(!err.empty());
        auto notAFile = openIqRecording("rtlsdr|serial=1", 1e8, err);
        CHECK(notAFile == nullptr);
        CHECK(!err.empty());
    }

    // --- [P] three recordings playing at once, in real time --------------------------
    {
        struct Case {
            fs::path file;
            double rate;
            double tone;
            double centre;
        };
        const Case cases[3] = {{pcm, 48000.0, 5000.0, 162.0e6},
                               {flt, 250000.0, 40000.0, 1090.0e6},
                               {upper, 96000.0, -10000.0, 144.8e6}};
        std::vector<std::unique_ptr<PatchRadio>> radios;
        for (int i = 0; i < 3; ++i) {
            std::string err;
            auto src = openIqRecording(makeIqFileKey(cases[i].file.string()), cases[i].centre, err);
            CHECK(src != nullptr);
            if (!src) { return testSummary("test_patch_iqfile"); }
            radios.push_back(std::make_unique<PatchRadio>(static_cast<NodeId>(i + 1),
                                                          std::move(src), "Recording"));
            CHECK(radios.back()->rateHz() == cases[i].rate);
            CHECK(radios.back()->centreHz() == cases[i].centre);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (auto& r : radios) {
            std::string err;
            CHECK(r->start(err));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        // A new centre relabels the recording; it does not move its signal.
        CHECK(radios[0]->setCentreHz(162.025e6));
        CHECK(radios[0]->centreHz() == 162.025e6);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        const double ranSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (int i = 0; i < 3; ++i) {
            PatchRadio& r = *radios[static_cast<std::size_t>(i)];
            CHECK(r.running());
            CHECK(r.fault().empty());
            // REAL TIME: the reader reads 10 ms chunks, so blocks x chunk is
            // the samples played; about rate x seconds of them, not a minute
            // of recording a second.
            const double chunk = std::floor(cases[i].rate * 0.010 + 0.5);
            const double played = static_cast<double>(r.blocksRead()) * chunk;
            const double expect = cases[i].rate * ranSec;
            std::printf("recording %d: played %.0f samples in %.2f s, expected about %.0f\n", i,
                        played, ranSec, expect);
            CHECK(played > 0.8 * expect);
            CHECK(played < 1.2 * expect);
            std::vector<float> db;
            std::uint64_t seq = 0;
            CHECK(r.spectrum(db, seq));
            if (db.size() == 2048u) {
                const double hz = peakHz(db, cases[i].rate);
                std::printf("recording %d: peak at %+.0f Hz (written at %+.0f)\n", i, hz,
                            cases[i].tone);
                CHECK(std::fabs(hz - cases[i].tone) <= 2.0 * cases[i].rate / 2048.0);
            }
        }
        for (auto& r : radios) { r->stop(); }
        for (auto& r : radios) { CHECK(!r->running()); }
    }

    // --- [S] the document -------------------------------------------------------------
    {
        const std::string path = "C:\\Rec ordings\\1090,serial=7 \xC3\xA9t\xC3\xA9 %2D - \\x.wav";
        Graph g;
        const NodeId r = g.addNode(NodeKind::Radio, "Recorded", PortType::Iq);
        g.mutableNode(r)->device = makeIqFileKey(path);
        g.mutableNode(r)->freqHz = 1090.0e6;
        g.mutableNode(r)->rateHz = 2.0e6;
        const std::string text = serialise(g, 0.0f, 0.0f, 1.0f);
        const LoadResult back = parse(text);
        CHECK(back.ok);
        CHECK(back.dropped == 0);
        CHECK(back.graph.nodes().size() == 1u);
        if (back.graph.nodes().size() == 1u) {
            const Node& n = back.graph.nodes()[0];
            CHECK(n.device == makeIqFileKey(path));
            CHECK(iqFilePath(n.device) == path);
            CHECK(n.freqHz == 1090.0e6);
        }
        // A recording is a device key like any other: no format of its own.
        CHECK(text.rfind(std::string(kPatchMagic) + " " + std::to_string(kPatchFormat) + "\n", 0) ==
              0);
    }

    // --- [M] five recordings, five decoders, one map -----------------------------------
    {
        Graph g;
        const std::vector<DecoderInfo> cat{
            {"adsb.dll", "ADS-B Aircraft", PortType::Iq, 0.0, false, true},
            {"ais.dll", "AIS Ships", PortType::Iq, 0.0, false, true},
            {"aprs.dll", "APRS", PortType::Iq, 0.0, false, true},
            {"sonde.dll", "Radiosonde", PortType::Iq, 0.0, false, true},
            {"sats.dll", "Satellites", PortType::Iq, 0.0, false, true}};
        const NodeId map = g.addNode(NodeKind::Map, "Map", PortType::Track);
        std::vector<RadioInfo> radios;
        for (std::size_t i = 0; i < kMaxRadios; ++i) {
            const NodeId r = g.addNode(NodeKind::Radio, "R", PortType::Iq);
            CHECK(r != kNoNode);
            g.mutableNode(r)->device = makeIqFileKey("C:\\rec\\" + std::to_string(i) + ".wav");
            const NodeId d = g.addNode(NodeKind::Decoder, cat[i].name, PortType::Iq);
            g.mutableNode(d)->plugin = cat[i].key;
            CHECK(g.connect(r, 0, d, 0) == Connect::Ok);
            CHECK(g.connect(d, 1, map, static_cast<PortIndex>(i)) == Connect::Ok);
            radios.push_back({r, 2.0e6, 100.0e6 + 1.0e6 * static_cast<double>(i)});
        }
        // A sixth radio is refused by the graph, as it is for hardware.
        CHECK(g.addNode(NodeKind::Radio, "R6", PortType::Iq) == kNoNode);
        const Plan p = compile(g, radios, &cat);
        CHECK(p.runnable);
        CHECK(p.decoders.size() == kMaxRadios);
        std::size_t blocking = 0;
        for (const NodeProblem& np : p.problems) {
            if (!isAdvisory(np.problem)) {
                ++blocking;
                std::printf("  blocking problem on node %u: %s\n", static_cast<unsigned>(np.node),
                            problemText(np.problem));
            }
        }
        CHECK(blocking == 0u);
        const std::vector<std::string> src = mapSources(g, map, cat);
        CHECK(src.size() == kMaxRadios);
        CHECK(src == std::vector<std::string>({"ADS-B Aircraft", "AIS Ships", "APRS", "Radiosonde",
                                               "Satellites"}));
    }

    const int rc = testSummary("test_patch_iqfile");
    if (rc == 0) { fs::remove_all(root, ec); }
    return rc;
}
