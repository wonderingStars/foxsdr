// The airband per-channel outputs (src/core/airband_writer.hpp): the WAV file writer (names, folder per day, header, pre-roll, tail, size cap, day change,
// failures, never outside the root) and the loopback UDP sink (127.0.0.1 only, off until started). Files go to a folder of this test's own under the
// build folder; the socket binds 127.0.0.1 only.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <winsock2.h>

#include <ws2tcpip.h>
using SockT = SOCKET;
inline int processId() { return _getpid(); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
constexpr SockT INVALID_SOCKET = -1;
inline int processId() { return static_cast<int>(getpid()); }
#endif

#include "core/airband_writer.hpp"
#include "test_check.hpp"

using namespace cascade::core::airband;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

fs::path g_scratch;  // this run's own folder

std::vector<unsigned char> readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::uint32_t le32(const std::vector<unsigned char>& v, std::size_t at) {
    return static_cast<std::uint32_t>(v[at]) | (static_cast<std::uint32_t>(v[at + 1]) << 8) | (static_cast<std::uint32_t>(v[at + 2]) << 16) |
           (static_cast<std::uint32_t>(v[at + 3]) << 24);
}
std::int16_t sample(const std::vector<unsigned char>& v, std::size_t i) {
    return static_cast<std::int16_t>(v[44 + 2 * i] | (v[45 + 2 * i] << 8));
}

// A recognisable signal: sample k of the stream is the value k * 1e-5 (a ramp), so what ends up in the file says where it came from.
std::vector<float> ramp(std::size_t from, std::size_t n) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) { v[i] = static_cast<float>(((from + i) % 90000) * 1e-5); }
    return v;
}
std::int16_t quant(float v) { return static_cast<std::int16_t>(std::lrintf(std::min(std::max(v, -1.0f), 1.0f) * 32767.0f)); }

WriterConfig cfgIn(const fs::path& root) {
    WriterConfig c;
    c.recordingsRoot = root.string();
    c.channelIndex = 3;
    c.freqHz = 118700000.0;
    return c;
}

fs::path freshRoot(const std::string& name) {
    const fs::path r = g_scratch / name;
    std::error_code ec;
    fs::remove_all(r, ec);
    fs::create_directories(r, ec);
    return r;
}

void testNamesAndPaths() {
    UtcStamp t{2026, 11, 12, 15, 30, 12};
    CHECK(dayFolderName(t) == "2026-11-12");
    CHECK(dayCompact(t) == "20261112");
    CHECK(fileName(3, 118700000.0, t, 1, true) == "ch03_118700kHz_0001.wav");
    CHECK(fileName(3, 118700000.0, t, 12, false) == "ch03_118700kHz_20261112_0012.wav");
    CHECK(fileName(120, 118699999.0, t, 0, true) == "ch99_118700kHz_0001.wav");  // index and segment are clamped, the frequency is rounded to kHz
    CHECK(fileName(-4, std::nan(""), t, 99999, true) == "ch00_0kHz_9999.wav");
    // Nothing but a channel number, kilohertz, a date and a segment: no letters beyond the fixed words.
    const std::string n = fileName(7, 131250000.0, t, 2, false);
    for (char c : n) { CHECK((c >= '0' && c <= '9') || c == '_' || c == '.' || c == 'c' || c == 'h' || c == 'k' || c == 'H' || c == 'z' || c == 'w' || c == 'a' || c == 'v'); }
    CHECK(stampValid(t) && !stampValid(UtcStamp{1999, 1, 1, 0, 0, 0}) && !stampValid(UtcStamp{2026, 13, 1, 0, 0, 0}) && !stampValid(UtcStamp{2026, 1, 1, 24, 0, 0}));
    // pathIsUnder.
    const fs::path root = g_scratch / "under";
    CHECK(pathIsUnder(root, root));
    CHECK(pathIsUnder(root, root / "airband" / "2026-11-12" / "x.wav"));
    CHECK(pathIsUnder(root.string() + "/", root / "a"));
    CHECK(!pathIsUnder(root, g_scratch));
    CHECK(!pathIsUnder(root, g_scratch / "underling" / "x"));   // a sibling that shares a prefix
    CHECK(!pathIsUnder(root, root / ".." / "elsewhere"));
    CHECK(!pathIsUnder(root, root / "a" / ".." / ".." / "b"));
    CHECK(!pathIsUnder(fs::path(), root));
}

void testHeaderBurstsPreRollTail() {
    const fs::path root = freshRoot("w1");
    WriterConfig c = cfgIn(root);
    c.preRollMs = 100.0;   // 4800 samples
    c.tailMs = 200.0;      // 9600 samples
    ChannelWavWriter w(c);
    const UtcStamp now{2026, 11, 12, 10, 0, 0};
    // 1 s of closed audio (held only as pre-roll), then a burst of 0.5 s open, then 1 s closed (tail written, rest dropped), then a second burst.
    std::size_t pos = 0;
    auto feed = [&](std::size_t n, bool open) {
        const std::vector<float> a = ramp(pos, n);
        pos += n;
        return w.write(a.data(), n, open, now);
    };
    CHECK(feed(48000, false));
    CHECK(!w.fileOpen() && w.filesOpened() == 0);  // nothing is created until a burst
    CHECK(feed(24000, true));
    CHECK(w.fileOpen() && w.filesOpened() == 1);
    const fs::path file = w.currentPath();
    CHECK(file.parent_path().filename() == "2026-11-12" && file.parent_path().parent_path().filename() == "airband");
    CHECK(file.filename() == "ch03_118700kHz_0001.wav");
    CHECK(feed(48000, false));  // tail 9600 written, 38400 dropped
    const std::uint64_t afterFirst = w.bytesWritten();
    // 4800 pre-roll + 24000 open + 9600 tail = 38400 samples = 76800 bytes
    CHECK(afterFirst == 76800);
    // The header is valid NOW, before close: patched at the end of the burst.
    std::vector<unsigned char> mid = readAll(file);
    CHECK(mid.size() == 44 + 76800);
    CHECK(std::memcmp(&mid[0], "RIFF", 4) == 0 && std::memcmp(&mid[8], "WAVEfmt ", 8) == 0 && std::memcmp(&mid[36], "data", 4) == 0);
    CHECK(le32(mid, 4) == 36 + 76800 && le32(mid, 40) == 76800 && le32(mid, 16) == 16);
    CHECK(mid[20] == 1 && mid[21] == 0 && mid[22] == 1 && mid[23] == 0);  // PCM, mono
    CHECK(le32(mid, 24) == 48000 && le32(mid, 28) == 96000 && mid[32] == 2 && mid[34] == 16);
    // What is in it: the 4800 samples before the gate opened (stream positions 43200 .. 48000), then the burst, then 9600 of tail.
    std::vector<float> expect;
    for (std::size_t i = 43200; i < 43200 + 38400; ++i) { expect.push_back(static_cast<float>((i % 90000) * 1e-5)); }
    bool same = true;
    for (std::size_t i = 0; i < 38400; ++i) { same = same && sample(mid, i) == quant(expect[i]); }
    CHECK(same);
    // A second burst continues the SAME file, with its own pre-roll; nothing from the dropped gap is in it.
    CHECK(feed(48000, false));
    CHECK(feed(12000, true));
    CHECK(w.filesOpened() == 1);
    const std::vector<unsigned char> two = readAll(file);
    // 76800 + (4800 + 12000) * 2 bytes
    CHECK(two.size() == 44 + 76800 + 33600);
    CHECK(sample(two, 38400) == quant(static_cast<float>(((pos - 12000 - 4800) % 90000) * 1e-5)));  // the pre-roll's first sample
    w.close();
    CHECK(!w.fileOpen());
    const std::vector<unsigned char> fin = readAll(file);
    CHECK(le32(fin, 40) == fin.size() - 44 && le32(fin, 4) == fin.size() - 8);
    std::printf("MEAS wav burst test: file %zu bytes, header data size %u, pre-roll 4800 + burst 24000 + tail 9600 samples in the first burst\n", fin.size(),
                le32(fin, 40));
    // Clipping and NaN are written as full scale and silence.
    const fs::path r2 = freshRoot("w2");
    ChannelWavWriter z(cfgIn(r2));
    const float odd[] = {2.0f, -3.0f, std::nanf(""), 0.5f};
    z.write(odd, 4, true, now);
    z.close();
    const std::vector<unsigned char> zf = readAll(r2 / "airband" / "2026-11-12" / "ch03_118700kHz_0001.wav");
    CHECK(zf.size() == 44 + 8 + 0 && sample(zf, 0) == 32767 && sample(zf, 1) == -32767 && sample(zf, 2) == 0 && sample(zf, 3) == quant(0.5f));
}

void testSizeCapAndDayChange() {
    const fs::path root = freshRoot("w3");
    WriterConfig c = cfgIn(root);
    c.maxFileBytes = 1u << 20;  // the 1 MB minimum
    c.preRollMs = 0.0;
    c.tailMs = 0.0;
    ChannelWavWriter w(c);
    CHECK(w.config().maxFileBytes == (1u << 20));
    const UtcStamp now{2026, 11, 12, 10, 0, 0};
    const std::size_t total = 1500000;  // samples: 3 MB, so four files (1 MB each, the last partly)
    std::size_t pos = 0;
    while (pos < total) {
        const std::size_t n = std::min<std::size_t>(7777, total - pos);
        const std::vector<float> a = ramp(pos, n);
        CHECK(w.write(a.data(), n, true, now));
        pos += n;
    }
    w.close();
    std::set<std::string> names;
    std::uint64_t bytes = 0;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(root / "airband" / "2026-11-12")) {
        files.push_back(e.path());
        names.insert(e.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        const std::vector<unsigned char> v = readAll(f);
        CHECK(v.size() <= 44 + (1u << 20));   // never over the cap
        CHECK(le32(v, 40) == v.size() - 44);  // every file is a valid WAV
        bytes += v.size() - 44;
    }
    CHECK(names == (std::set<std::string>{"ch03_118700kHz_0001.wav", "ch03_118700kHz_0002.wav", "ch03_118700kHz_0003.wav"}));
    CHECK(bytes == total * 2);  // nothing lost at a split
    // The samples run on across the split: last sample of file 1 and first of file 2 are consecutive stream samples.
    const std::vector<unsigned char> f1 = readAll(files[0]), f2 = readAll(files[1]);
    const std::size_t per = (1u << 20) / 2;
    CHECK(sample(f1, per - 1) == quant(static_cast<float>(((per - 1) % 90000) * 1e-5)));
    CHECK(sample(f2, 0) == quant(static_cast<float>((per % 90000) * 1e-5)));
    std::printf("MEAS size cap 1 MB: %zu files for %zu bytes of audio, none over the cap, no sample lost\n", files.size(), static_cast<std::size_t>(bytes));
    // Cap limits.
    WriterConfig lim = cfgIn(root);
    lim.maxFileBytes = 5;
    CHECK(ChannelWavWriter(lim).config().maxFileBytes == ChannelWavWriter::kMinFileBytes);
    lim.maxFileBytes = ~0ull;
    CHECK(ChannelWavWriter(lim).config().maxFileBytes == ChannelWavWriter::kMaxFileBytes);

    // A new UTC day: a new folder, segment numbering starts again; the old file is closed and valid.
    const fs::path r2 = freshRoot("w4");
    ChannelWavWriter d(cfgIn(r2));
    const std::vector<float> a = ramp(0, 4800);
    UtcStamp t1{2026, 11, 12, 23, 59, 59}, t2{2026, 11, 13, 0, 0, 1};
    CHECK(d.write(a.data(), a.size(), true, t1));
    const fs::path day1 = d.currentPath();
    CHECK(d.write(a.data(), a.size(), true, t2));
    CHECK(d.currentPath() != day1 && d.currentPath().parent_path().filename() == "2026-11-13" && d.currentPath().filename() == "ch03_118700kHz_0001.wav");
    d.close();
    CHECK(le32(readAll(day1), 40) == 4800 * 2 + 4800 * 2 * 0 + 0 || le32(readAll(day1), 40) == (4800 + 0) * 2);
    // Without a day folder the date is in the name and everything sits in airband/.
    const fs::path r3 = freshRoot("w5");
    WriterConfig nd = cfgIn(r3);
    nd.dailyFolder = false;
    ChannelWavWriter n(nd);
    n.write(a.data(), a.size(), true, t1);
    n.close();
    CHECK(fs::exists(r3 / "airband" / "ch03_118700kHz_20261112_0001.wav"));
}

void testFailuresAndConfinement() {
    const UtcStamp now{2026, 11, 12, 10, 0, 0};
    const std::vector<float> a = ramp(0, 4800);
    // No folder set / the folder does not exist / the folder is a file: Failed, nothing thrown, nothing created, audio dropped and counted.
    {
        ChannelWavWriter w;  // default config: no root
        CHECK(!w.write(a.data(), a.size(), true, now));
        CHECK(w.state() == WriterState::Failed && !w.lastError().empty());
        CHECK(!w.write(a.data(), a.size(), true, now) && w.droppedSamples() >= a.size());
    }
    const fs::path gone = g_scratch / "never-made";
    {
        WriterConfig c = cfgIn(gone);
        ChannelWavWriter w(c);
        CHECK(!w.write(a.data(), a.size(), true, now));
        CHECK(w.state() == WriterState::Failed);
        CHECK(!fs::exists(gone));  // the root is never created by the writer
        // reset() clears it; after the folder exists a burst works.
        std::error_code ec;
        fs::create_directories(gone, ec);
        w.reset();
        CHECK(w.state() == WriterState::Idle);
        CHECK(w.write(a.data(), a.size(), true, now) && w.fileOpen());
        w.close();
    }
    {
        const fs::path file = g_scratch / "a-file";
        std::ofstream(file) << "x";
        ChannelWavWriter w(cfgIn(file));
        CHECK(!w.write(a.data(), a.size(), true, now));
        CHECK(w.state() == WriterState::Failed);
    }
    // A bad clock reading is refused, not guessed at.
    {
        const fs::path r = freshRoot("w6");
        ChannelWavWriter w(cfgIn(r));
        CHECK(!w.write(a.data(), a.size(), true, UtcStamp{1970, 1, 1, 0, 0, 0}));
        CHECK(w.state() == WriterState::Failed);
        CHECK(fs::is_empty(r));
    }
    // An unwritable place (the day folder name is taken by a FILE): the open fails, the writer fails, nothing escapes.
    {
        const fs::path r = freshRoot("w7");
        fs::create_directories(r / "airband");
        std::ofstream(r / "airband" / "2026-11-12") << "block";
        ChannelWavWriter w(cfgIn(r));
        CHECK(!w.write(a.data(), a.size(), true, now));
        CHECK(w.state() == WriterState::Failed);
    }
    // CONFINEMENT: after a whole session with several channels, days and segments, the only things that changed under the scratch folder are inside
    // the roots, and every file is named by the pattern.
    const fs::path r = freshRoot("w8");
    const std::set<std::string> before = [&] {
        std::set<std::string> s;
        for (const auto& e : fs::directory_iterator(g_scratch)) { s.insert(e.path().filename().string()); }
        return s;
    }();
    for (int ch = 0; ch < 4; ++ch) {
        WriterConfig c = cfgIn(r);
        c.channelIndex = ch;
        c.freqHz = 118000000.0 + 25000.0 * ch;
        c.maxFileBytes = 1u << 20;
        ChannelWavWriter w(c);
        for (int day = 12; day <= 14; ++day) {
            UtcStamp t{2026, 11, day, 5, 0, 0};
            for (int k = 0; k < 3; ++k) { w.write(a.data(), a.size(), true, t); }
        }
        w.close();
    }
    std::set<std::string> after;
    for (const auto& e : fs::directory_iterator(g_scratch)) { after.insert(e.path().filename().string()); }
    CHECK(before == after);
    std::size_t files = 0;
    for (const auto& e : fs::recursive_directory_iterator(r)) {
        CHECK(pathIsUnder(r, e.path()));
        if (e.is_regular_file()) {
            ++files;
            const std::string n = e.path().filename().string();
            CHECK(n.size() > 4 && n.substr(0, 2) == "ch" && n.substr(n.size() - 4) == ".wav");
        }
    }
    CHECK(files == 4 * 3);
}

void testSourceScan() {
    // The writer and the channel model must not print: the host captures stdout and stderr into its log and its reports. Neither may name anything
    // personal. (Files are read from the source tree by the path of this test's own file.)
    const fs::path here = fs::path(__FILE__).parent_path().parent_path() / "src" / "core";
    for (const char* name : {"airband_writer.hpp", "airband_channel.hpp"}) {
        std::ifstream f(here / name);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        CHECK(text.size() > 1000);
        bool prints = false;
        for (const char* needle : {"stderr", "stdout", "std::cerr", "std::cout", "fprintf", "puts(", "OutputDebugString"}) { prints = prints || text.find(needle) != std::string::npos; }
        for (std::size_t at = text.find("printf("); at != std::string::npos; at = text.find("printf(", at + 1)) {
            if (at < 2 || text.compare(at - 2, 2, "sn") != 0) { prints = true; }  // only snprintf into a buffer
        }
        CHECK(!prints);
        for (const char* ident : {"callsign", "call sign", "registration", "tail number", "ICAO", "squawk"}) { CHECK(text.find(std::string("\"") + ident) == std::string::npos); }
    }
}

// ---------------------------------------------------------------- UDP

void sockInit() {
#if defined(_WIN32)
    static bool done = false;
    if (!done) {
        WSADATA d{};
        ::WSAStartup(MAKEWORD(2, 2), &d);
        done = true;
    }
#endif
}
void closeSock(SockT s) {
#if defined(_WIN32)
    if (s != INVALID_SOCKET) { ::closesocket(s); }
#else
    if (s != INVALID_SOCKET) { ::close(s); }
#endif
}

struct Receiver {
    SockT s = INVALID_SOCKET;
    int port = 0;
    Receiver() {
        sockInit();
        s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
        ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t len = sizeof(a);
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
#if defined(_WIN32)
        DWORD tv = 100;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{0, 100000};
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
    ~Receiver() { closeSock(s); }
    std::vector<std::string> collect(int ms) {
        std::vector<std::string> out;
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        char buf[4096];
        while (Clock::now() < end) {
            const int n = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
            if (n > 0) { out.emplace_back(buf, static_cast<std::size_t>(n)); }
        }
        return out;
    }
};

void testUdpSink() {
    LoopbackAudioSink sink;
    std::string err;
    // Off until started: pushing does nothing; a bad port is refused with a sentence and no socket.
    const std::vector<float> a = ramp(0, 4800);
    sink.push(a.data(), a.size(), true);
    CHECK(!sink.running() && sink.queuedFrames() == 0);
    CHECK(!sink.start(0, err) && !err.empty() && !sink.running());
    CHECK(!sink.start(80, err) && !sink.start(70000, err) && !sink.start(-5, err));
    Receiver rx;
    CHECK(rx.port > 1023);
    CHECK(sink.start(rx.port, err));
    CHECK(sink.running());
    // Silence (a closed gate) is not sent.
    sink.push(a.data(), a.size(), false);
    CHECK(sink.queuedFrames() == 0);
    // 9600 gated samples (0.2 s) arrive as 20 packets of 480 frames, byte for byte what the wire format says.
    std::vector<float> sent;
    for (int i = 0; i < 2; ++i) {
        const std::vector<float> blk = ramp(static_cast<std::size_t>(i) * 4800, 4800);
        sent.insert(sent.end(), blk.begin(), blk.end());
        sink.push(blk.data(), blk.size(), true);
    }
    const std::vector<std::string> pk = rx.collect(1500);
    std::printf("MEAS loopback stream: %zu datagrams for 9600 samples (packets sent %llu)\n", pk.size(), static_cast<unsigned long long>(sink.packetsSent()));
    CHECK(pk.size() == 20);
    std::string all;
    for (const std::string& p : pk) {
        CHECK(p.size() == 960);
        all += p;
    }
    const std::string want = cascade::net::udpAudioPayload(sent.data(), sent.size());
    CHECK(all == want);
    CHECK(sink.droppedFrames() == 0);
    // After stop nothing more is sent, and a second start works.
    sink.stop();
    CHECK(!sink.running());
    const std::uint64_t n = sink.packetsSent();
    sink.push(a.data(), a.size(), true);
    CHECK(rx.collect(300).empty() && sink.packetsSent() == n);
    CHECK(sink.start(rx.port, err));
    sink.stop();
    // A ring that is not drained drops and counts (the sink was never started in this block: start it against a dead port, push far more than fits).
    LoopbackAudioSink full;
    const fs::path none;
    Receiver dead;
    const int deadPort = dead.port;
    CHECK(full.start(deadPort, err));
    std::vector<float> big(LoopbackAudioSink::kRingFrames * 4, 0.25f);
    full.push(big.data(), big.size(), true);
    CHECK(full.queuedFrames() + full.droppedFrames() == big.size());
    full.stop();
    (void)none;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path exe = argc > 0 ? fs::path(argv[0]) : fs::path(".");
    std::error_code ec;
    exe = fs::absolute(exe, ec);
    g_scratch = exe.parent_path() / ("dspm_airband_" + std::to_string(processId()));
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    testNamesAndPaths();
    testHeaderBurstsPreRollTail();
    testSizeCapAndDayChange();
    testFailuresAndConfinement();
    testSourceScan();
    testUdpSink();
    fs::remove_all(g_scratch, ec);
    return testSummary("test_airband_writer");
}
