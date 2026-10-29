// image_feed.cpp - see image_feed.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/image_feed.hpp"

#include <algorithm>
#include <utility>

#include "core/image_write.hpp"
#include "core/thread_beats.hpp"

namespace cascade::engine {

namespace {
// How often the worker polls the decoders while the server is listening. A decoder builds a picture a line at a time (a scan line of a slow-scan
// picture is a second or more), so this is far finer than the picture changes; a window that is framing polls at its own rate besides.
constexpr std::chrono::milliseconds kPollPeriod{50};
}  // namespace

ImageFeed::ImageFeed() = default;

ImageFeed::~ImageFeed() { shutdown(); }

void ImageFeed::attach(Hooks hooks) { hooks_ = std::move(hooks); }

std::vector<ImageFeed::SlotKey> ImageFeed::keysOf(const std::vector<cascade::core::HostImage>& v) {
    std::vector<SlotKey> k;
    k.reserve(v.size());
    for (const cascade::core::HostImage& im : v) { k.push_back({im.plugin, im.revision, im.sequence, im.width, im.height}); }
    return k;
}

void ImageFeed::copyTo(std::vector<cascade::core::HostImage>& out) {
    bool pending = false;
    {
        std::lock_guard<std::mutex> lk(pm_);
        if (hooks_.poll) { hooks_.poll(acc_); }
        out.resize(acc_.size());
        for (std::size_t i = 0; i < acc_.size(); ++i) {
            const cascade::core::HostImage& a = acc_[i];
            cascade::core::HostImage& o = out[i];
            if (o.plugin == a.plugin && o.revision == a.revision && o.sequence == a.sequence && o.width == a.width && o.height == a.height) {
                o.interactive = a.interactive;  // the same picture: only the decoder's input flag can have moved
            } else {
                o = a;
            }
        }
        // (compared in place: this runs in the window's frame, and a key built per slot would copy the plugin's name every time)
        pending = acc_.size() != published_.size();
        for (std::size_t i = 0; !pending && i < acc_.size(); ++i) {
            const SlotKey& k = published_[i];
            const cascade::core::HostImage& a = acc_[i];
            pending = k.plugin != a.plugin || k.revision != a.revision || k.sequence != a.sequence || k.width != a.width || k.height != a.height;
        }
    }
    if (pending && serving()) { wake(); }
}

std::shared_ptr<const std::vector<cascade::net::RadioStatus::Image>> ImageFeed::headers() const {
    std::lock_guard<std::mutex> lk(hm_);
    return headers_;
}

void ImageFeed::wake() {
    {
        std::lock_guard<std::mutex> lk(wm_);
        if (stop_) { return; }
        kick_ = true;
        if (!worker_.joinable()) { worker_ = std::thread([this] { workerLoop(); }); }
    }
    wcv_.notify_one();
}

void ImageFeed::shutdown() {
    std::thread w;
    {
        std::lock_guard<std::mutex> lk(wm_);
        stop_ = true;
        w = std::move(worker_);
    }
    wcv_.notify_all();
    if (w.joinable() && w.get_id() != std::this_thread::get_id()) { w.join(); }
    idleCv_.notify_all();
}

bool ImageFeed::waitIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(wm_);
    return idleCv_.wait_for(lk, timeout, [this] { return !kick_ && !busy_; });
}

void ImageFeed::workerLoop() {
    // THE HEARTBEAT (core/thread_beats.hpp, 6.27). Not serving, the worker is parked on its condition variable until a kick -
    // a declared wait ("job"), not a wedge; serving, it wakes every kPollPeriod and beats.
    cascade::core::beats::Registration beat(cascade::core::beats::kThreadImageFeed);
    std::unique_lock<std::mutex> lk(wm_);
    for (;;) {
        // Listening: a cycle every kPollPeriod (the poll is the work; the encode follows a change). Not listening: asleep until woken - the
        // server's start wakes it, and so does a window's poll that finds a picture the served set does not have.
        if (serving()) {
            beat.beat("poll");
            wcv_.wait_for(lk, kPollPeriod, [this] { return stop_ || kick_; });
        } else {
            beat.wait(cascade::core::beats::kWaitJob);
            wcv_.wait(lk, [this] { return stop_ || kick_; });
        }
        beat.beat("woke");
        if (stop_) { return; }
        kick_ = false;
        if (!serving()) { continue; }
        busy_ = true;
        lk.unlock();
        beat.beat("cycle");
        cycle();
        lk.lock();
        busy_ = false;
        idleCv_.notify_all();
    }
}

void ImageFeed::cycleForTest() { cycle(); }

void ImageFeed::cycle() {
    std::lock_guard<std::mutex> cycleLock(cm_);
    std::vector<cascade::core::HostImage> fresh;  // copies of the slots whose picture is not the one published
    std::vector<std::size_t> which;
    std::vector<SlotKey> keys;
    {
        std::lock_guard<std::mutex> lk(pm_);
        if (hooks_.poll) { hooks_.poll(acc_); }
        keys = keysOf(acc_);
        if (keys == published_) { return; }
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i >= published_.size() || keys[i] != published_[i]) {
                which.push_back(i);
                fresh.push_back(acc_[i]);
            }
        }
        published_ = keys;
    }

    // The encode is outside every lock but cm_: the window's frame and the decoders' next poll are not held up by it.
    std::vector<cascade::net::WebImage> next(keys.size());
    std::size_t f = 0;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (f < which.size() && which[f] == i) {
            const cascade::core::HostImage& src = fresh[f++];
            cascade::net::WebImage& w = next[i];
            w.plugin = src.plugin;
            w.width = src.width;
            w.height = src.height;
            w.complete = src.complete;
            w.revision = src.revision;
            std::string err;
            // A decoder that has produced no pixels yet encodes to nothing; the slot is still published so the browser can show that it
            // exists and is waiting, rather than the picture appearing from nowhere later.
            if (!cascade::core::encodeBmp24(src, w.bmp, err)) { w.bmp.clear(); }
        } else if (i < served_.size()) {
            next[i] = served_[i];  // the picture it was: not encoded again
        }
    }
    served_ = std::move(next);
    if (hooks_.publish) { hooks_.publish(served_); }

    // THE HEADERS LAST: the status names a revision only once the server holds its bitmap.
    auto h = std::make_shared<std::vector<cascade::net::RadioStatus::Image>>();
    h->reserve(served_.size());
    for (const cascade::net::WebImage& w : served_) { h->push_back({w.plugin, w.width, w.height, w.complete, w.revision}); }
    std::lock_guard<std::mutex> lk(hm_);
    headers_ = std::move(h);
}

}  // namespace cascade::engine
