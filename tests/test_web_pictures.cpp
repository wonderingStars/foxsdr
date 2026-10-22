// test_web_pictures.cpp - THE RECEIVER PLUGINS' PICTURES ARE THE ENGINE'S (engine extraction stage 5e; docs/engine-stage4.md 6.13).
//
// A picture decoder OFFERS a picture once, when it changes; PluginRunner::pollImages takes the offer into a caller-owned accumulator. Until 5e that
// accumulator was the window's (it drew the image windows from it and re-encoded every picture into the web server once a frame), so a headless engine
// served the page no pictures and - had the engine kept an accumulator of its own beside the window's - two pollers would have split every picture
// between them. The engine's ImageFeed (engine/image_feed.hpp) is the ONE accumulator: the window's frame reads it (copyTo), the feed's own worker
// polls it while the web server listens and encodes the BMPs, and /api/status lists the headers of the set the server holds.
//
// THE FEED, against fake decoders (hooks that consume their offers, as a real decoder does):
//   U1  ONE ACCUMULATOR, NO SPLIT: a window reading every few milliseconds and the worker polling beside it see EVERY picture - the revision each
//       ends on is the number the decoder offered, never fewer.
//   U2  THE COPY IS BY REVISION: an entry that did not change is not copied again (its buffer stays where it was); a changed one is replaced whole; the
//       shape follows the accumulator's.
//   U3  THE ENCODE IS THE WORKER'S AND THE HEADERS FOLLOW THE SET: the set reaches the web server from the worker's thread, never the caller's, and
//       when the server is handed revision R the headers still name the revision before it - so the status never names a picture the server cannot
//       serve (the page caches /api/image/<n>?rev=R under that URL for good).
//   U4  NOT LISTENING, NOTHING IS ENCODED: a woken worker does nothing while the web server is not serving; the window's reads still poll.
//
// THE WHOLE CHAIN, with no window anywhere (the headless engine of the future): a real picture decoder (the group-13 fixture) in the engine's runner, fed
// by the pipeline's audio on the signal generator, the web server hosted by the engine - the page's status lists the picture's header and /api/image/0
// serves its BMP, byte for byte what the decoder offered.
//   P1  HEADLESS PICTURES: status header and BMP coherent, every property a function of the picture's sequence (the fixture's contract).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/host_image.hpp"
#include "core/plugin_abi.h"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/image_feed.hpp"
#include "engine/server_host.hpp"
#include "engine_api_test_env.hpp"
#include "foxsdr_api.h"
#include "test_check.hpp"

// (test_check.hpp has CHECK only: a comparison that prints what it saw)
template <class T>
std::string showVal(const T& v) {
    if constexpr (std::is_convertible_v<const T&, std::string>) {
        return "\"" + std::string(v) + "\"";
    } else {
        return std::to_string(v);
    }
}
#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        const auto va_ = (a);                                                                            \
        const auto vb_ = (b);                                                                            \
        CHECK(va_ == vb_);                                                                               \
        if (!(va_ == vb_)) {                                                                             \
            std::printf("      %s = %s, %s = %s\n", #a, showVal(va_).c_str(), #b, showVal(vb_).c_str()); \
        }                                                                                                \
    } while (0)

namespace cascade::gui {
struct AppWindowTestAccess {
    static void setPluginDir(std::string (*dir)()) { cascade::engine::Engine::testHooks_.pluginDir = dir; }
    static std::size_t activeDecoders(cascade::engine::Engine& e) { return e.pluginRunner_.activeCount(); }
};
}  // namespace cascade::gui

namespace {

using cascade::core::HostImage;
using cascade::engine::ImageFeed;
using Access = cascade::gui::AppWindowTestAccess;
namespace cmd = cascade::core::cmd;
namespace fs = std::filesystem;

bool waitUntil(int ms, const std::function<bool()>& pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ---- fake decoders: a poll that CONSUMES its offer, as PluginRunner::pollImages does of a real decoder --------------------------------------------

class FakeDecoders {
public:
    // The decoder has a new picture to offer (it is taken by the next poll, whoever makes it).
    void offer() {
        std::lock_guard<std::mutex> lk(m_);
        ++pending_;
        ++offered_;
    }
    std::uint64_t offered() const {
        std::lock_guard<std::mutex> lk(m_);
        return offered_;
    }
    // The runner's pollImages, in miniature: the accumulator is re-seeded to the decoders' shape, and a pending picture is taken into slot 0.
    void poll(std::vector<HostImage>& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (out.size() != slots_) { out.resize(slots_); }
        for (std::size_t i = 0; i < slots_; ++i) { out[i].plugin = "fake" + std::to_string(i); }
        if (pending_ > 0 && slots_ > 0) {
            --pending_;
            HostImage& hi = out[0];
            hi.width = 4;
            hi.height = 2;
            hi.format = CASCADE_IMAGE_RGB24;
            hi.sequence = ++taken_;
            hi.complete = (hi.sequence % 3u) == 0u;
            hi.pixels.assign(4 * 2 * 3, static_cast<std::uint8_t>(hi.sequence & 0xFFu));
            ++hi.revision;
        }
    }
    void setSlots(std::size_t n) {
        std::lock_guard<std::mutex> lk(m_);
        slots_ = n;
    }
    // Calls made to poll, for U4.
    std::atomic<int> polls{0};

private:
    mutable std::mutex m_;
    std::size_t slots_ = 1;
    std::uint64_t pending_ = 0;
    std::uint64_t offered_ = 0;
    std::uint64_t taken_ = 0;
};

struct Served {
    std::mutex m;
    std::vector<std::vector<cascade::net::WebImage>> sets;
    std::vector<std::thread::id> threads;
    // What headers() said at the moment each set arrived (the feed publishes the headers AFTER the set).
    std::vector<std::uint64_t> headerRevisionAtPublish;
    std::vector<std::size_t> headerCountAtPublish;
};

// A feed wired to the fakes, the way ServerHost wires the real one; `serving` is the test's switch.
struct FeedRig {
    FakeDecoders decoders;
    Served served;
    std::atomic<bool> serving{true};
    ImageFeed feed;
    FeedRig() {
        ImageFeed::Hooks h;
        h.poll = [this](std::vector<HostImage>& out) {
            ++decoders.polls;
            decoders.poll(out);
        };
        h.serving = [this] { return serving.load(); };
        h.publish = [this](std::vector<cascade::net::WebImage> set) {
            const auto hd = feed.headers();
            std::lock_guard<std::mutex> lk(served.m);
            served.headerRevisionAtPublish.push_back(hd != nullptr && !hd->empty() ? hd->front().revision : 0u);
            served.headerCountAtPublish.push_back(hd != nullptr ? hd->size() : 0u);
            served.sets.push_back(std::move(set));
            served.threads.push_back(std::this_thread::get_id());
        };
        feed.attach(std::move(h));
    }
    std::size_t setsPublished() {
        std::lock_guard<std::mutex> lk(served.m);
        return served.sets.size();
    }
};

// U1. One accumulator, no split.
void testNoSplit() {
    FeedRig r;
    constexpr int kPictures = 120;
    std::atomic<bool> producing{true};
    std::thread producer([&] {
        for (int i = 0; i < kPictures; ++i) {
            r.decoders.offer();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        producing = false;
    });
    // The window: reads the feed every few milliseconds into its own copy, as its frame does. The worker polls beside it (the server is serving).
    std::vector<HostImage> window;
    r.feed.wake();
    std::uint64_t windowSawMax = 0;
    std::uint64_t windowDistinct = 0;
    std::uint64_t lastRev = 0;
    while (producing.load()) {
        r.feed.copyTo(window);
        if (!window.empty()) {
            windowSawMax = std::max(windowSawMax, window[0].revision);
            if (window[0].revision != lastRev) {
                ++windowDistinct;
                lastRev = window[0].revision;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    producer.join();
    // Drain: one more read (takes what is still pending), then the worker's cycle for the served set.
    r.feed.copyTo(window);
    CHECK(r.feed.waitIdle(std::chrono::seconds(5)));
    r.feed.cycleForTest();
    CHECK_EQ(r.decoders.offered(), static_cast<std::uint64_t>(kPictures));
    CHECK(!window.empty());
    CHECK_EQ(window.empty() ? 0u : window[0].revision, static_cast<std::uint64_t>(kPictures));   // the window's copy ends on the last picture offered
    {
        std::lock_guard<std::mutex> lk(r.served.m);
        CHECK(!r.served.sets.empty());
        // The served set ends on it too, and every picture it was handed is whole (the fake's pixels are a function of the sequence).
        const auto& last = r.served.sets.back();
        CHECK_EQ(last.size(), std::size_t{1});
        CHECK_EQ(last.empty() ? 0u : last[0].revision, static_cast<std::uint64_t>(kPictures));
    }
    const auto hd = r.feed.headers();
    CHECK(hd != nullptr && hd->size() == 1u && hd->front().revision == static_cast<std::uint64_t>(kPictures));
    std::printf("U1: %d pictures offered once each; the window's copy ended on #%llu (it saw %llu distinct revisions between its reads), the worker's set and the headers on #%llu - no picture went to one poller and was lost to the other\n",
                kPictures, static_cast<unsigned long long>(window.empty() ? 0u : window[0].revision), static_cast<unsigned long long>(windowDistinct),
                static_cast<unsigned long long>(hd != nullptr && !hd->empty() ? hd->front().revision : 0u));
}

// U2. The copy is by revision.
void testCopyIsByRevision() {
    FeedRig r;
    r.serving = false;   // only the window reads here
    r.decoders.setSlots(2);
    std::vector<HostImage> out;
    r.decoders.offer();
    r.feed.copyTo(out);
    CHECK_EQ(out.size(), std::size_t{2});
    CHECK_EQ(out[0].revision, std::uint64_t{1});
    CHECK_EQ(out[0].pixels.size(), std::size_t{24});
    const std::uint8_t* before = out[0].pixels.data();
    // Nothing new: the same picture, not copied again (the buffer is where it was).
    r.feed.copyTo(out);
    CHECK(out[0].pixels.data() == before);
    CHECK_EQ(out[0].revision, std::uint64_t{1});
    // A new picture replaces the entry whole.
    r.decoders.offer();
    r.feed.copyTo(out);
    CHECK_EQ(out[0].revision, std::uint64_t{2});
    CHECK_EQ(static_cast<unsigned>(out[0].pixels[0]), 2u);
    // The shape follows the accumulator's: a decoder removed.
    r.decoders.setSlots(1);
    r.feed.copyTo(out);
    CHECK_EQ(out.size(), std::size_t{1});
    r.decoders.setSlots(0);
    r.feed.copyTo(out);
    CHECK(out.empty());
    std::printf("U2: an unchanged picture is not copied again, a new one replaces its entry whole, and the shape follows the decoders (2 -> 1 -> 0)\n");
}

// U3. The encode and the publish are the worker's; the headers follow the set.
void testWorkerPublishesAndTheHeadersFollow() {
    FeedRig r;
    const std::thread::id me = std::this_thread::get_id();
    std::vector<HostImage> window;
    r.decoders.offer();
    r.feed.copyTo(window);   // the window's read finds a picture the served set does not have, and wakes the worker
    CHECK(waitUntil(5000, [&] { return r.setsPublished() >= 1; }));
    CHECK(r.feed.waitIdle(std::chrono::seconds(5)));
    {
        std::lock_guard<std::mutex> lk(r.served.m);
        CHECK(!r.served.threads.empty());
        for (const std::thread::id& t : r.served.threads) { CHECK(t != me); }
        // The set reached the server while the headers still named nothing: they follow it.
        CHECK_EQ(r.served.headerCountAtPublish.front(), std::size_t{0});
    }
    auto hd = r.feed.headers();
    CHECK(hd != nullptr && hd->size() == 1u && hd->front().revision == 1u);
    // The next picture: when the server is handed revision 2 the headers still say 1.
    r.decoders.offer();
    r.feed.copyTo(window);
    CHECK(waitUntil(5000, [&] { return r.setsPublished() >= 2; }));
    CHECK(r.feed.waitIdle(std::chrono::seconds(5)));
    {
        std::lock_guard<std::mutex> lk(r.served.m);
        CHECK_EQ(r.served.sets.back()[0].revision, std::uint64_t{2});
        CHECK_EQ(r.served.headerRevisionAtPublish.back(), std::uint64_t{1});
    }
    hd = r.feed.headers();
    CHECK(hd != nullptr && hd->front().revision == 2u);
    // The BMP is real: 24-bit, "BM", the picture's width and height.
    {
        std::lock_guard<std::mutex> lk(r.served.m);
        const std::vector<std::uint8_t>& bmp = r.served.sets.back()[0].bmp;
        CHECK(bmp.size() > 54u && bmp[0] == 'B' && bmp[1] == 'M');
    }
    std::printf("U3: the set reached the server from the worker's thread, and the headers named revision N only after the server held it (at the publish of #2 they still said #1)\n");
}

// U4. Not serving: nothing is encoded.
void testNothingEncodedWhileNotServing() {
    FeedRig r;
    r.serving = false;
    std::vector<HostImage> window;
    r.decoders.offer();
    r.feed.copyTo(window);   // the window polls; it would wake the worker only when serving
    r.feed.wake();           // a wake that arrives while the server is not serving (the worker starts and goes back to sleep)
    CHECK(r.feed.waitIdle(std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(r.setsPublished(), std::size_t{0});
    CHECK(r.feed.headers() == nullptr);
    CHECK_EQ(window.empty() ? 0u : window[0].revision, std::uint64_t{1});
    // The server starts listening: the wake it gets makes the worker catch up without any window.
    r.serving = true;
    r.feed.wake();
    CHECK(waitUntil(5000, [&] { return r.setsPublished() >= 1; }));
    const auto hd = r.feed.headers();
    CHECK(hd != nullptr && hd->size() == 1u && hd->front().revision == 1u);
    std::printf("U4: nothing encoded while the server was not serving (the window's reads still polled); once it listened the worker caught up with no window\n");
}

// ---- P1: the whole chain, headless --------------------------------------------------------------------------------------------------------------

fs::path g_pluginDir;
std::string pluginPath() { return g_pluginDir.string(); }

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

int freePort() {
#if defined(_WIN32)
    WSADATA d{};
    ::WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const SOCKET bad = INVALID_SOCKET;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const int bad = -1;
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    int port = 0;
    if (s != bad && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        socklen_t len = sizeof(a);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) { port = ntohs(a.sin_port); }
    }
#if defined(_WIN32)
    if (s != bad) { ::closesocket(s); }
#else
    if (s != bad) { ::close(s); }
#endif
    return port;
}

// What picture N is, byte for byte (tests/fixture_group13_plugin.cpp): odd N RGB24 8x6, even N GRAY8 5x3, byte i = (N*13 + i*7) & 0xFF.
bool bmpIsPicture(const std::string& body, std::uint64_t n, std::uint32_t& w, std::uint32_t& h) {
    if (body.size() < 54 || body[0] != 'B' || body[1] != 'M') { return false; }
    const auto u32 = [&](std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[at])) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[at + 1])) << 8) |
               (static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[at + 2])) << 16) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(body[at + 3])) << 24);
    };
    const std::uint32_t off = u32(10);
    w = u32(18);
    h = u32(22);
    const bool rgb = (n % 2u) == 1u;
    if (w != (rgb ? 8u : 5u) || h != (rgb ? 6u : 3u)) { return false; }
    const std::size_t rowBytes = (static_cast<std::size_t>(w) * 3u + 3u) & ~std::size_t{3};   // 24-bit rows are padded to 4 bytes
    if (body.size() < off + rowBytes * h) { return false; }
    // The BMP is bottom-up BGR; the decoder's pixels are top-down (RGB24: R,G,B per pixel; GRAY8: one byte per pixel, repeated into B,G,R).
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::size_t bmpRow = off + rowBytes * (h - 1 - y);
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t px[3];
            if (rgb) {
                for (int c = 0; c < 3; ++c) { px[c] = static_cast<std::uint8_t>((n * 13u + (static_cast<std::size_t>(y) * w * 3u + x * 3u + static_cast<std::size_t>(c)) * 7u) & 0xFFu); }
            } else {
                const std::uint8_t g = static_cast<std::uint8_t>((n * 13u + (static_cast<std::size_t>(y) * w + x) * 7u) & 0xFFu);
                px[0] = px[1] = px[2] = g;
            }
            const std::size_t at = bmpRow + static_cast<std::size_t>(x) * 3u;
            // BGR in the file
            if (static_cast<std::uint8_t>(body[at + 0]) != px[2] || static_cast<std::uint8_t>(body[at + 1]) != px[1] ||
                static_cast<std::uint8_t>(body[at + 2]) != px[0]) {
                return false;
            }
        }
    }
    return true;
}

void testHeadlessPictures() {
    // The fixture picture decoder in a plugin folder of this test's own.
    g_pluginDir = fs::temp_directory_path() / ("foxsdr_web_pictures_plugins_" +
#if defined(_WIN32)
                                                std::to_string(::GetCurrentProcessId()));
#else
                                                std::to_string(::getpid()));
#endif
    std::error_code ec;
    fs::remove_all(g_pluginDir, ec);
    fs::create_directories(g_pluginDir, ec);
    const fs::path fixture(FIXTURE_GROUP13_PLUGIN);
    fs::copy_file(fixture, g_pluginDir / fixture.filename(), fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    Access::setPluginDir(&pluginPath);

    {
        cascade::engine::Engine e;
        e.configureApi("testHooks=1;servers=1");
        e.initialise();
        (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1));
        e.controlPass();
        e.startControlThread();
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_ADMIN | FOXAPI_GRANT_AUDIO;
        p.clientName = "the operator";
        FoxSession* op = nullptr;
        CHECK(api()->open_session(e.apiHandle(), &p, &op) == FOXAPI_OK);
        CHECK(Access::activeDecoders(e) >= 1u);   // the runner built the picture decoder: the engine's, with no window

        const int port = freePort();
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_SERVER_CONFIG;
        const std::string json = "{\"web\":{\"enabled\":true,\"bindAddress\":\"127.0.0.1\",\"port\":" + std::to_string(port) + "}}";
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        CHECK(api()->submit_text(op, &c, json.data(), static_cast<std::uint32_t>(json.size()), &sr) >= 0);
        CHECK(waitUntil(20000, [&] { return e.servers().webServer().running(); }));

        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);
        // The page's status lists the picture once the engine has polled it and its feed has published the set.
        nlohmann::json img;
        const bool listed = waitUntil(30000, [&] {
            const auto st = cli.Get("/api/status");
            if (!st || st->status != 200) { return false; }
            const nlohmann::json j = nlohmann::json::parse(st->body, nullptr, false);
            if (j.is_discarded() || !j.contains("images") || !j["images"].is_array() || j["images"].empty()) { return false; }
            img = j["images"][0];
            return img.value("hasPicture", false) && img.value("width", 0u) > 0u;
        });
        CHECK(listed);
        if (!listed) {
            // (nothing was served: the chain is broken, and the rest of the case has nothing to look at)
            api()->close_session(op);
            e.shutdown();
            Access::setPluginDir(nullptr);
            fs::remove_all(g_pluginDir, ec);
            return;
        }
        // Freeze the picture: stop the receiver, so no audio reaches the decoder and no new picture is offered, then let the feed settle on the last.
        (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 0));
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        std::uint64_t revision = 0;
        std::string body;
        // The header and the bitmap are one set: read the status header, then the bitmap, until two reads agree on the revision (no picture arrived between).
        const bool steady = waitUntil(20000, [&] {
            const auto st = cli.Get("/api/status");
            if (!st || st->status != 200) { return false; }
            const nlohmann::json j = nlohmann::json::parse(st->body, nullptr, false);
            if (j.is_discarded() || j["images"].empty()) { return false; }
            const std::uint64_t r1 = j["images"][0].value("revision", std::uint64_t{0});
            const auto im = cli.Get("/api/image/0");
            if (!im || im->status != 200) { return false; }
            const auto st2 = cli.Get("/api/status");
            if (!st2 || st2->status != 200) { return false; }
            const nlohmann::json j2 = nlohmann::json::parse(st2->body, nullptr, false);
            if (j2.is_discarded() || j2["images"].empty()) { return false; }
            const std::uint64_t r2 = j2["images"][0].value("revision", std::uint64_t{0});
            if (r1 != r2 || r1 == 0) { return false; }
            revision = r1;
            body = im->body;
            return true;
        });
        CHECK(steady);
        {
            const auto im = cli.Get("/api/image/0");
            CHECK(im && im->status == 200);
            if (im) { CHECK_EQ(im->get_header_value("Content-Type"), std::string("image/bmp")); }
        }
        std::uint32_t w = 0, h = 0;
        // The decoder's picture number is the header's revision (both count the pictures taken, from 1) and the bitmap is that picture, whole.
        CHECK(bmpIsPicture(body, revision, w, h));
        CHECK_EQ(img.value("plugin", std::string()).empty() ? 1u : 0u, 0u);
        std::printf("P1: headless - the status lists \"%s\" (hasPicture, revision %llu) and /api/image/0 serves its %ux%u BMP, every pixel a function of picture #%llu; no window in the process\n",
                    img.value("plugin", std::string()).c_str(), static_cast<unsigned long long>(revision), w, h, static_cast<unsigned long long>(revision));
        // The window-less engine also hands its pictures out: the one accumulator can be read by a front end that comes later (the window's copy).
        std::vector<HostImage> late;
        e.servers().images().copyTo(late);
        CHECK_EQ(late.size(), std::size_t{1});
        CHECK_EQ(late.empty() ? 0u : late[0].revision, revision);
        api()->close_session(op);
        e.shutdown();
    }
    Access::setPluginDir(nullptr);
    fs::remove_all(g_pluginDir, ec);
}

}  // namespace

int main(int argc, char** argv) {
    std::string only;
    if (argc > 1) { only = argv[1]; }
    const auto want = [&](const char* id) { return only.empty() || only == id; };
    engine_api_test::hermetic("webpictures");
    std::printf("test_web_pictures\n");
    if (want("U1")) { testNoSplit(); }
    if (want("U2")) { testCopyIsByRevision(); }
    if (want("U3")) { testWorkerPublishesAndTheHeadersFollow(); }
    if (want("U4")) { testNothingEncodedWhileNotServing(); }
    if (want("P1")) { testHeadlessPictures(); }
    return testSummary("test_web_pictures");
}
