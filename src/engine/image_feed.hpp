// image_feed.hpp - THE RECEIVER PLUGINS' PICTURES, polled and served by the ENGINE (engine extraction stage 5e; docs/engine-stage4.md 6.13).
//
// The image decoders live in the engine's PluginRunner (it owns the instances and feeds them samples). A decoder OFFERS a picture once, when
// it changes, and PluginRunner::pollImages takes the offer into a caller-owned accumulator - so there can be exactly ONE accumulator that
// polls, or two pollers would split the pictures between them and neither would ever see the whole picture. Until 5e that one belonged to the
// window (it drew the plugin's image windows from it, and re-encoded every picture into the web server's set once a frame); a headless
// engine had no pictures at all. The accumulator is the engine's now:
//
//   * ONE accumulator, here, polled under one lock by whoever asks - the window's frame (copyTo: it wants the pictures for its own image
//     windows, so a window that is framing polls at its frame rate exactly as it did) and this feed's own worker (which polls at 20 Hz while
//     the web server is listening, window or none, so a hidden window or a headless engine still serves the page its pictures);
//   * the BMP encode is the WORKER'S, never the pass's and never a frame's: when a slot's picture changed (its plugin, revision or sequence
//     differs from the one last published) the worker encodes that slot, rebuilds the served set and hands it to the web server
//     (WebServer::setImages), and only THEN publishes the headers /api/status lists - so the status never names a revision the server cannot
//     yet serve (the page fetches /api/image/<n>?rev=<revision> once per revision and the browser caches it under that URL: a header that led
//     the bitmap would cache the previous picture under the new revision for good);
//   * the status's picture headers are read by the engine's publish (headers()): a shared pointer, no allocation, no lock of the runner's.
//
// THREADS. copyTo and every poll are any-thread; the worker is created the first time something wakes it while the server is serving (a
// hermetic engine that never serves never has one), and shutdown() joins it. Nothing here is called from the control pass except headers().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_IMAGE_FEED_HPP
#define CASCADE_ENGINE_IMAGE_FEED_HPP

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/host_image.hpp"
#include "net/web_server.hpp"

namespace cascade::engine {

class ImageFeed {
public:
    struct Hooks {
        // PluginRunner::pollImages: takes the decoders' new pictures into the accumulator (caller-owned, under the runner's own mutex).
        std::function<void(std::vector<cascade::core::HostImage>&)> poll;
        // Whether the web server is listening - what the worker's polling and encoding are for. Cheap and lock-free.
        std::function<bool()> serving;
        // WebServer::setImages: replaces the served set. Thread-safe.
        std::function<void(std::vector<cascade::net::WebImage>)> publish;
    };

    ImageFeed();
    ~ImageFeed();
    ImageFeed(const ImageFeed&) = delete;
    ImageFeed& operator=(const ImageFeed&) = delete;

    // Installs the hooks, once, before any thread can call anything here (ServerHost's constructor: serving and publish are its own), and
    // the decoders' poll (the engine's constructor, through ServerHost::attach: the runner is the engine's).
    void attach(Hooks hooks);
    void setPoll(std::function<void(std::vector<cascade::core::HostImage>&)> poll) { hooks_.poll = std::move(poll); }

    // THE WINDOW'S READ (HostPluginFeed::pollImages): polls the decoders now - on the caller's thread, as the window always has - and brings
    // `out` up to date with the accumulator: an entry whose plugin, revision, sequence and size match is left as it is (its pixels are not
    // copied again, which is what made polling every frame free), any other is replaced whole; the shape follows the accumulator's. A change
    // the served set does not yet carry wakes the worker (when the server is serving).
    void copyTo(std::vector<cascade::core::HostImage>& out);

    // The pictures' headers as the served set carries them (null before the first publish). Read by the engine's publish for /api/status.
    std::shared_ptr<const std::vector<cascade::net::RadioStatus::Image>> headers() const;

    // Asks the worker for a cycle (creating it the first time). A no-op after shutdown(). Any thread.
    void wake();

    // Joins the worker. Idempotent; any thread but the worker's.
    void shutdown();

    // TESTS: nothing pending and the worker inside no cycle; false on timeout.
    bool waitIdle(std::chrono::milliseconds timeout);
    // TESTS: one whole cycle (poll, encode what changed, publish the set and the headers) on the caller's thread, whether or not the server
    // is serving. The worker's own cycle is this, made when it is woken.
    void cycleForTest();

private:
    struct SlotKey {
        std::string plugin;
        std::uint64_t revision = 0;
        std::uint64_t sequence = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool operator==(const SlotKey& o) const {
            return plugin == o.plugin && revision == o.revision && sequence == o.sequence && width == o.width && height == o.height;
        }
        bool operator!=(const SlotKey& o) const { return !(*this == o); }
    };
    static std::vector<SlotKey> keysOf(const std::vector<cascade::core::HostImage>& v);
    bool serving() const { return hooks_.serving && hooks_.serving(); }
    void cycle();
    void workerLoop();

    Hooks hooks_;

    // The accumulator, and what the worker last published (both under pm_). The poll itself happens under pm_: one poller at a time.
    std::mutex pm_;
    std::vector<cascade::core::HostImage> acc_;
    std::vector<SlotKey> published_;

    // cycle() runs whole under cm_ (the worker's, or a test's): served_ is the set the server holds, kept to rebuild it around a changed slot.
    std::mutex cm_;
    std::vector<cascade::net::WebImage> served_;

    mutable std::mutex hm_;
    std::shared_ptr<const std::vector<cascade::net::RadioStatus::Image>> headers_;

    std::mutex wm_;
    std::condition_variable wcv_;
    std::condition_variable idleCv_;
    std::thread worker_;
    bool kick_ = false;
    bool busy_ = false;
    bool stop_ = false;
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_IMAGE_FEED_HPP
