// Tests for the browser clip request model (net/web_clip.hpp).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define WEBCLIP_GETPID _getpid
#else
#include <unistd.h>
#define WEBCLIP_GETPID getpid
#endif

#include "net/web_clip.hpp"
#include "test_check.hpp"

#ifndef CASCADE_SOURCE_DIR
#define CASCADE_SOURCE_DIR "."
#endif

using namespace cascade::net;
namespace fs = std::filesystem;

namespace {

const std::string kA = "viewerAAAAAAAA";
const std::string kB = "viewer-BBBB_bbbb";
const std::string kC = "viewer-CCCC_cccc";

fs::path g_root;

ClipRequest req(double seconds, const std::string& name = "") {
    ClipRequest r;
    r.seconds = seconds;
    r.name = name;
    return r;
}

void testSanitise() {
    CHECK(sanitiseClipName("My Clip 1!") == "My_Clip_1");
    CHECK(sanitiseClipName("") == "clip");
    CHECK(sanitiseClipName("   ") == "clip");
    CHECK(sanitiseClipName("../../etc/passwd") == "etc_passwd");
    CHECK(sanitiseClipName("..\\..\\windows\\system32") == "windows_system32");
    CHECK(sanitiseClipName("C:\\evil") == "C_evil");
    CHECK(sanitiseClipName("name.wav:stream") == "name_wav_stream");     // no ':' and no '.'
    CHECK(sanitiseClipName("-rf") == "rf");                               // never starts like an option
    CHECK(sanitiseClipName("CON") == "CON_");
    CHECK(sanitiseClipName("nul") == "nul_");
    CHECK(sanitiseClipName("com1") == "com1_");
    CHECK(sanitiseClipName("LPT9") == "LPT9_");
    CHECK(sanitiseClipName("COM10") == "COM10");                          // only COM1-9 are devices
    CHECK(sanitiseClipName("caf\xC3\xA9") == "caf");                      // non-ASCII bytes become separators and are trimmed
    CHECK(sanitiseClipName(std::string(500, 'a')).size() == kMaxClipNameChars);
    CHECK(sanitiseClipName(std::string(kMaxClipNameChars, 'a')).size() == kMaxClipNameChars);   // the boundary
    CHECK(sanitiseClipName("a___b") == "a_b");
    CHECK(sanitiseClipName(std::string("a\0b", 3)) == "a_b");
    // every output is made of safe characters only
    std::mt19937_64 rng(5);
    int bad = 0;
    for (int i = 0; i < 5000; ++i) {
        std::string s(rng() % 90, '\0');
        for (char& c : s) { c = static_cast<char>(rng()); }
        const std::string o = sanitiseClipName(s);
        if (o.empty() || o.size() > kMaxClipNameChars + 1) { ++bad; }
        for (const char c : o) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) { ++bad; }
        }
        if (o.front() == '-' || o.front() == '_') { ++bad; }
    }
    CHECK(bad == 0);
}

void testPathUnderRoot() {
    const fs::path root = g_root;
    CHECK(pathIsUnderRoot(root, root / "a.wav"));
    CHECK(pathIsUnderRoot(root, root / "sub" / "a.wav"));
    CHECK(pathIsUnderRoot(root, root));
    CHECK(!pathIsUnderRoot(root, root / ".." / "a.wav"));
    CHECK(!pathIsUnderRoot(root, root.parent_path() / "a.wav"));
    CHECK(!pathIsUnderRoot(root, fs::path(root.string() + "-evil") / "a.wav"));   // a sibling that shares a prefix
    CHECK(!pathIsUnderRoot(root, root / "sub" / ".." / ".." / "a.wav"));
    CHECK(pathIsUnderRoot(root, root / "sub" / ".." / "a.wav"));                  // normalises to inside
    CHECK(!pathIsUnderRoot(fs::path(), root / "a.wav"));
    CHECK(!pathIsUnderRoot(root, fs::path()));
    CHECK(!pathIsUnderRoot(root, fs::path("relative.wav")));
    CHECK(!pathIsUnderRoot(fs::path("relative"), fs::path("relative") / "a.wav"));
}

void testHappyPath() {
    WebClipBroker broker(g_root);
    ClipPlan plan;
    CHECK(broker.request(kA, req(10.0, "Morning Net"), plan) == ClipResult::Ok);
    CHECK(plan.clipId != 0);
    CHECK(plan.estimatedBytes == 10u * 48000u * 2u);
    CHECK(plan.path.filename().string() == "web-clip-Morning_Net-" + std::to_string(plan.clipId) + ".wav");
    CHECK(pathIsUnderRoot(g_root, plan.path));
    CHECK(plan.path.parent_path() == g_root);
    CHECK(broker.inFlight() == 1);
    // not downloadable until it finishes
    fs::path p;
    CHECK(broker.resolveDownload(kA, plan.clipId, p) == ClipResult::NotReady);
    CHECK(broker.finished(kA, plan.clipId, 960000, true) == ClipResult::Ok);
    CHECK(broker.inFlight() == 0);
    CHECK(broker.resolveDownload(kA, plan.clipId, p) == ClipResult::Ok);
    CHECK(p.filename() == plan.path.filename());
    const auto l = broker.list(kA);
    CHECK(l.size() == 1 && l[0].finished && l[0].ok && l[0].bytes == 960000 && l[0].fileName == plan.path.filename().string());
    // the file name in the list has no directory part
    CHECK(l.size() == 1 && l[0].fileName.find('/') == std::string::npos && l[0].fileName.find('\\') == std::string::npos);
}

void testLengthLimits() {
    WebClipBroker broker(g_root);
    ClipPlan plan;
    CHECK(broker.request(kA, req(0.0), plan) == ClipResult::BadLength);
    CHECK(broker.request(kA, req(0.99), plan) == ClipResult::BadLength);
    CHECK(broker.request(kA, req(-4.0), plan) == ClipResult::BadLength);
    CHECK(broker.request(kA, req(kMaxClipSeconds + 0.01), plan) == ClipResult::BadLength);
    CHECK(broker.request(kA, req(3600.0), plan) == ClipResult::BadLength);
    CHECK(broker.request(kA, req(std::numeric_limits<double>::quiet_NaN()), plan) == ClipResult::BadLength);
    CHECK(broker.request(kA, req(std::numeric_limits<double>::infinity()), plan) == ClipResult::BadLength);
    CHECK(plan.clipId == 0 && plan.path.empty());                       // a refusal leaves no plan behind
    CHECK(broker.request(kA, req(kMinClipSeconds), plan) == ClipResult::Ok);       // the boundaries themselves
    CHECK(broker.finished(kA, plan.clipId, 10, true) == ClipResult::Ok);
    CHECK(broker.request(kA, req(kMaxClipSeconds), plan) == ClipResult::Ok);      // 300 s mono 16-bit 48 k = 28.8 MB: under the cap
    CHECK(broker.inFlight() == 1);
}

void testSizeCap() {
    WebClipBroker broker(g_root);
    ClipPlan plan;
    ClipRequest r = req(300.0);
    r.channels = 2;
    r.bytesPerSample = 4;
    r.sampleRateHz = 96000;           // 300 x 96000 x 4 x 2 = 230 MB
    CHECK(broker.request(kA, r, plan) == ClipResult::TooBig);
    r.channels = 1;
    r.bytesPerSample = 2;
    r.sampleRateHz = 48000;
    CHECK(broker.request(kA, r, plan) == ClipResult::Ok);
    CHECK(broker.finished(kA, plan.clipId, 10, true) == ClipResult::Ok);
    // zero, absurd or overflowing parameters
    r.sampleRateHz = 0;
    CHECK(broker.request(kA, r, plan) == ClipResult::TooBig);
    r.sampleRateHz = 48000;
    r.bytesPerSample = 0;
    CHECK(broker.request(kA, r, plan) == ClipResult::TooBig);
    r.bytesPerSample = 99;
    CHECK(broker.request(kA, r, plan) == ClipResult::TooBig);
    r.bytesPerSample = 2;
    r.channels = 0;
    CHECK(broker.request(kA, r, plan) == ClipResult::TooBig);
    r.channels = 1;
    r.sampleRateHz = 0xFFFFFFFFu;
    CHECK(broker.request(kA, r, plan) == ClipResult::TooBig);
    // the exact byte boundary: 64 MiB / (48000 x 2) = 699.05 s, beyond the 300 s ceiling, so test the byte cap through the rate instead:
    r.sampleRateHz = 1000000;         // 1 MS/s x 2 bytes = 2 MB per second: 33 s fits, 34 s does not (64 MiB = 67108864)
    CHECK(broker.request(kA, [&] { auto x = r; x.seconds = 33.0; return x; }(), plan) == ClipResult::Ok);
    CHECK(broker.finished(kA, plan.clipId, 10, true) == ClipResult::Ok);
    CHECK(broker.request(kA, [&] { auto x = r; x.seconds = 34.0; return x; }(), plan) == ClipResult::TooBig);
    // finished() refuses a file written past the cap: the clip is failed and cannot be downloaded
    ClipPlan p2;
    CHECK(broker.request(kB, req(5.0), p2) == ClipResult::Ok);
    CHECK(broker.finished(kB, p2.clipId, kMaxClipBytes + 1, true) == ClipResult::Failed);
    fs::path out;
    CHECK(broker.resolveDownload(kB, p2.clipId, out) == ClipResult::Failed);
    CHECK(out.empty());
    // exactly the cap is fine, an empty file or a recorder that reports failure is not
    CHECK(broker.request(kB, req(5.0), p2) == ClipResult::Ok);
    CHECK(broker.finished(kB, p2.clipId, kMaxClipBytes, true) == ClipResult::Ok);
    CHECK(broker.request(kB, req(5.0), p2) == ClipResult::Ok);
    CHECK(broker.finished(kB, p2.clipId, 0, true) == ClipResult::Failed);
    CHECK(broker.request(kB, req(5.0), p2) == ClipResult::Ok);
    CHECK(broker.finished(kB, p2.clipId, 1000, false) == ClipResult::Failed);
}

void testOnePerViewerAndHostCap() {
    WebClipBroker broker(g_root);
    ClipPlan plan, other;
    CHECK(broker.request(kA, req(5.0), plan) == ClipResult::Ok);
    CHECK(broker.request(kA, req(5.0), other) == ClipResult::ViewerBusy);       // one at a time
    CHECK(other.clipId == 0);
    CHECK(broker.request(kB, req(5.0), other) == ClipResult::Ok);               // another viewer is fine
    CHECK(broker.request("viewer-CCCC-cccc", req(5.0), other) == ClipResult::Ok);
    CHECK(broker.request("viewer-DDDD-dddd", req(5.0), other) == ClipResult::Ok);
    CHECK(broker.inFlight() == kMaxClipsInFlight);
    CHECK(broker.request("viewer-EEEE-eeee", req(5.0), other) == ClipResult::HostBusy);
    // finishing one frees a slot for the next viewer AND for the same viewer
    CHECK(broker.finished(kA, plan.clipId, 100, true) == ClipResult::Ok);
    CHECK(broker.request("viewer-EEEE-eeee", req(5.0), other) == ClipResult::Ok);
    CHECK(broker.request(kA, req(5.0), other) == ClipResult::HostBusy);          // host is full again
    // abandon (viewer left): slot freed, clip failed
    broker.abandon(kB);
    CHECK(broker.inFlight() == kMaxClipsInFlight - 1);
    fs::path p;
    const auto bl = broker.list(kB);
    CHECK(bl.size() == 1 && bl[0].finished && !bl[0].ok);
    CHECK(broker.resolveDownload(kB, bl[0].clipId, p) == ClipResult::Failed);
    CHECK(broker.finished(kB, bl[0].clipId, 100, true) == ClipResult::NotFound);  // a late answer for an abandoned clip is not a clip
    // forgetViewer never frees an in-flight clip's accounting
    broker.forgetViewer("viewer-CCCC-cccc");
    CHECK(broker.inFlight() == kMaxClipsInFlight - 1);
    broker.abandon("viewer-CCCC-cccc");
    broker.forgetViewer("viewer-CCCC-cccc");
    CHECK(broker.list("viewer-CCCC-cccc").empty());
}

void testOwnership() {
    WebClipBroker broker(g_root);
    ClipPlan plan;
    CHECK(broker.request(kA, req(5.0), plan) == ClipResult::Ok);
    CHECK(broker.finished(kA, plan.clipId, 500, true) == ClipResult::Ok);
    fs::path p;
    CHECK(broker.resolveDownload(kB, plan.clipId, p) == ClipResult::NotFound);    // B cannot fetch A's clip by id
    CHECK(p.empty());
    CHECK(broker.finished(kB, plan.clipId, 500, true) == ClipResult::NotFound);
    CHECK(broker.resolveDownload(kA, plan.clipId + 1000, p) == ClipResult::NotFound);
    CHECK(broker.resolveDownload("bad", plan.clipId, p) == ClipResult::BadToken);
    CHECK(broker.resolveDownload(kA, 0, p) == ClipResult::NotFound);
    CHECK(broker.list(kB).empty());
    // a second finish for the same clip is refused
    CHECK(broker.finished(kA, plan.clipId, 500, true) == ClipResult::NotFound);
    // ids never repeat
    ClipPlan p2, p3;
    CHECK(broker.request(kA, req(5.0), p2) == ClipResult::Ok);
    CHECK(broker.finished(kA, p2.clipId, 5, true) == ClipResult::Ok);
    CHECK(broker.request(kB, req(5.0), p3) == ClipResult::Ok);
    CHECK(p2.clipId != plan.clipId && p3.clipId != p2.clipId && p2.path != p3.path);
}

void testSameNameNeverOverwrites() {
    WebClipBroker broker(g_root);
    std::vector<fs::path> paths;
    for (int i = 0; i < 6; ++i) {
        ClipPlan plan;
        CHECK(broker.request(kA, req(5.0, "same"), plan) == ClipResult::Ok);
        paths.push_back(plan.path);
        CHECK(broker.finished(kA, plan.clipId, 5, true) == ClipResult::Ok);
    }
    std::sort(paths.begin(), paths.end());
    CHECK(std::adjacent_find(paths.begin(), paths.end()) == paths.end());
}

void testPerViewerListIsBounded() {
    WebClipBroker broker(g_root);
    for (std::size_t i = 0; i < kMaxClipsPerViewer + 5; ++i) {
        ClipPlan plan;
        CHECK(broker.request(kA, req(5.0), plan) == ClipResult::Ok);
        CHECK(broker.finished(kA, plan.clipId, 5, true) == ClipResult::Ok);
    }
    const auto l = broker.list(kA);
    CHECK(l.size() == kMaxClipsPerViewer);
    // the oldest were forgotten: ids ascend and the last one is the newest
    for (std::size_t i = 1; i < l.size(); ++i) { CHECK(l[i].clipId > l[i - 1].clipId); }
}

void testViewerCap() {
    WebClipBroker broker(g_root);
    for (std::size_t i = 0; i < kMaxClipViewers; ++i) {
        ClipPlan plan;
        const std::string tok = "viewer" + std::to_string(1000 + i);
        CHECK(broker.request(tok, req(5.0), plan) == ClipResult::Ok);
        CHECK(broker.finished(tok, plan.clipId, 5, true) == ClipResult::Ok);
    }
    ClipPlan plan;
    CHECK(broker.request("viewer-one-more", req(5.0), plan) == ClipResult::TooManyViewers);
    broker.forgetViewer("viewer1000");
    CHECK(broker.request("viewer-one-more", req(5.0), plan) == ClipResult::Ok);
}

void testBadRootAndToken() {
    ClipPlan plan;
    WebClipBroker none{fs::path()};
    CHECK(none.request(kA, req(5.0), plan) == ClipResult::BadRoot);
    WebClipBroker rel{fs::path("recordings")};
    CHECK(rel.request(kA, req(5.0), plan) == ClipResult::BadRoot);
    WebClipBroker ok(g_root);
    CHECK(ok.request("", req(5.0), plan) == ClipResult::BadToken);
    CHECK(ok.request("short", req(5.0), plan) == ClipResult::BadToken);
    CHECK(ok.request("has/slash/in/it", req(5.0), plan) == ClipResult::BadToken);
    CHECK(ok.inFlight() == 0);
    // hostile names never leave the root, whatever they say
    for (const char* name : {"../../../x", "..", "/abs/path", "C:/x", "\\\\server\\share\\x", "a/../../b", "CON", "x\0y"}) {
        ClipPlan p;
        WebClipBroker b(g_root);
        CHECK(b.request(kA, req(5.0, name), p) == ClipResult::Ok);
        CHECK(pathIsUnderRoot(g_root, p.path) && p.path.parent_path() == g_root);
    }
}

// A link planted INSIDE the root that leads outside must not be followed by a download. Windows needs a privilege or developer mode for a symlink,
// so a directory junction is made with mklink /J (no privilege); if neither can be made the check is counted as skipped, never a silent pass.
void testLinkEscape() {
    const fs::path outside = g_root.parent_path() / (g_root.filename().string() + "_outside");
    std::error_code ec;
    fs::create_directories(outside, ec);
    { std::ofstream f(outside / "secret.wav", std::ios::binary); f << "secret"; }
    WebClipBroker broker(g_root);
    ClipPlan plan;
    CHECK(broker.request(kA, req(5.0, "link"), plan) == ClipResult::Ok);
    CHECK(broker.finished(kA, plan.clipId, 6, true) == ClipResult::Ok);
    // make the planned FILE itself a link to the outside file
    bool linked = false;
    std::error_code lec;
    fs::create_symlink(outside / "secret.wav", plan.path, lec);
    linked = !lec;
#if defined(_WIN32)
    if (!linked) {
        // No symlink privilege: a directory JUNCTION needs none. The planted name is a junction to the outside DIRECTORY; the broker's
        // real-path check must see that it leaves the root. (cmd.exe runs without a window; nothing here can prompt.)
        const std::string cmd = "cmd /c mklink /J \"" + plan.path.string() + "\" \"" + outside.string() + "\" >nul 2>&1";
        linked = std::system(cmd.c_str()) == 0 && fs::exists(plan.path, lec);
    }
#endif
    fs::path p;
    if (linked) {
        CHECK(broker.resolveDownload(kA, plan.clipId, p) == ClipResult::PathEscapes);
        CHECK(p.empty());
        fs::remove(plan.path, ec);          // removes the link itself, never what it points at
        CHECK(fs::exists(outside / "secret.wav", ec));
    } else {
        SKIP_LINUX("symbolic links need a privilege on this Windows account; the escape through a link was NOT exercised");
    }
    // a real file in the root resolves, and resolves to a path under the real root
    { std::ofstream f(plan.path, std::ios::binary); f << "real"; }
    CHECK(broker.resolveDownload(kA, plan.clipId, p) == ClipResult::Ok);
    CHECK(pathIsUnderRoot(fs::weakly_canonical(g_root), p));
    fs::remove(plan.path, ec);
    fs::remove_all(outside, ec);
}

void testConcurrency() {
    WebClipBroker broker(g_root);
    std::atomic<int> over{0};
    std::atomic<int> okCount{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&, t] {
            const std::string tok = "thread-viewer-" + std::to_string(t);
            for (int i = 0; i < 400; ++i) {
                ClipPlan plan;
                const ClipResult r = broker.request(tok, req(2.0, "t"), plan);
                if (broker.inFlight() > kMaxClipsInFlight) { ++over; }
                if (r == ClipResult::Ok) {
                    ++okCount;
                    broker.finished(tok, plan.clipId, 100, (i % 5) != 0);
                }
            }
        });
    }
    for (auto& th : ts) { th.join(); }
    CHECK(over.load() == 0);
    CHECK(okCount.load() > 0);
    CHECK(broker.inFlight() == 0);
}

void testSourceNeverPrintsOrWrites() {
    std::ifstream in(std::string(CASCADE_SOURCE_DIR) + "/src/net/web_clip.cpp", std::ios::binary);
    CHECK(static_cast<bool>(in));
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!src.empty());
    for (const char* banned : {"printf", "std::cout", "std::cerr", "fopen", "ofstream", "fstream", "fwrite", "OutputDebugString", "stderr", "stdout",
                               "settings_set", "getenv", "create_directories", "remove("}) {
        CHECK(src.find(banned) == std::string::npos);
    }
}

}  // namespace

int main() {
    std::error_code ec;
    g_root = fs::temp_directory_path(ec) / ("foxsdr_web_clip_" + std::to_string(WEBCLIP_GETPID()) + "_rec");
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);
    g_root = fs::weakly_canonical(g_root, ec);
    testSanitise();
    testPathUnderRoot();
    testHappyPath();
    testLengthLimits();
    testSizeCap();
    testOnePerViewerAndHostCap();
    testOwnership();
    testSameNameNeverOverwrites();
    testPerViewerListIsBounded();
    testViewerCap();
    testBadRootAndToken();
    testLinkEscape();
    testConcurrency();
    testSourceNeverPrintsOrWrites();
    fs::remove_all(g_root, ec);
    fs::remove_all(g_root.parent_path() / (g_root.filename().string() + "_outside"), ec);
    return testSummary("test_web_clip");
}
