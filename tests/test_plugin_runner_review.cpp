// test_plugin_runner_review.cpp - the plugin runner's fixes from the review of fix/freezes (2026-11-23), findings 2, 6 and 7.
//
//   finding 2: a press on a plugin's picture is never lost to a busy lock (imagePress WAITS; it used to try and drop).
//   finding 6: a rebuild does not blank the pictures the window holds (pollImages keeps them until the new set is published).
//   finding 7: a throw inside a build leaves no "starting" mark and no "rebuilding" flag, and destroys what it had built.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_api.hpp"
#include "core/plugin_host_data.hpp"
#include "core/plugin_runner.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::DecoderIdleReason;
using cascade::core::DecoderStatus;
using cascade::core::HostImage;
using cascade::core::LoadedPlugin;
using cascade::core::PluginRunner;
using Clock = std::chrono::steady_clock;

constexpr double kIqRate = 2400000.0;
constexpr double kCentre = 100000000.0;

template <class Pred>
bool waitFor(Pred p, int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < until) {
        if (p()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return p();
}

bool hasRow(PluginRunner& r, const std::string& name, DecoderIdleReason why) {
    for (const DecoderStatus& s : r.status()) {
        if (s.plugin == name && s.reason == why) { return true; }
    }
    return false;
}

// An image decoder that offers one 4x4 grey picture on its first poll, takes presses, and can block inside process() (the DSP thread holds the runner's lock there).
struct Pic {
    static inline int handle = 0;
    static inline std::atomic<int> created{0};
    static inline std::atomic<int> destroyed{0};
    static inline std::atomic<int> presses{0};
    static inline std::atomic<bool> holdProcess{false};
    static inline std::atomic<bool> inProcess{false};
    static inline std::atomic<bool> offered{false};
    static inline std::uint8_t pixels[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    static void reset() {
        created = 0; destroyed = 0; presses = 0; holdProcess = false; inProcess = false; offered = false;
    }
    static void* create(double, double) {
        ++created;
        return &handle;
    }
    static void process(void*, const float*, size_t) {
        if (holdProcess.load()) {
            inProcess = true;
            const auto until = Clock::now() + std::chrono::seconds(5);
            while (holdProcess.load() && Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
            inProcess = false;
        }
    }
    static int32_t pollImage(void*, CascadeImage* img) {
        if (offered.exchange(true)) { return 0; }
        img->pixels = pixels;
        img->width = 4;
        img->height = 4;
        img->stride = 4;
        img->format = CASCADE_IMAGE_GRAY8;
        img->complete = 1;
        img->sequence = 1;
        return 1;
    }
    static void release(void*, const CascadeImage*) {}
    static int32_t pollText(void*, char*, size_t) { return 0; }
    static void destroy(void*) { ++destroyed; }
    static void press(uint32_t, uint32_t) { ++presses; }
    static CascadeImageDecoderApi api() {
        CascadeImageDecoderApi a{};
        a.structSize = static_cast<uint32_t>(sizeof(CascadeImageDecoderApi));
        a.inputKind = CASCADE_INPUT_IQ;
        a.requiredRateHz = 0.0;
        a.create = &create;
        a.process = &process;
        a.poll_image = &pollImage;
        a.release_image = &release;
        a.poll_text = &pollText;
        a.destroy = &destroy;
        return a;
    }
    static CascadeImageInputApi input() {
        CascadeImageInputApi i{};
        i.structSize = static_cast<uint32_t>(sizeof(CascadeImageInputApi));
        i.press = &press;
        return i;
    }
};

// A plain I/Q decoder: the slow one in the rebuild test and the one a build hook throws for.
struct Iq {
    static inline int handle = 0;
    static inline std::atomic<int> created{0};
    static inline std::atomic<int> destroyed{0};
    static inline std::atomic<bool> slowCreate{false};
    static inline std::atomic<bool> releaseCreate{false};
    static inline std::atomic<int> inCreate{0};
    static void reset() { created = 0; destroyed = 0; slowCreate = false; releaseCreate = false; inCreate = 0; }
    static void* create(double, double) {
        ++inCreate;
        if (slowCreate.load()) {
            const auto until = Clock::now() + std::chrono::seconds(10);
            while (!releaseCreate.load() && Clock::now() < until) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        }
        ++created;
        --inCreate;
        return &handle;
    }
    static void process(void*, const float*, size_t) {}
    static int32_t poll(void*, char*, size_t) { return 0; }
    static void destroy(void*) { ++destroyed; }
    static CascadeIqDecoderApi api() {
        CascadeIqDecoderApi a{};
        a.structSize = static_cast<uint32_t>(sizeof(CascadeIqDecoderApi));
        a.requiredRateHz = 0.0;
        a.create = &create;
        a.process = &process;
        a.poll_text = &poll;
        a.destroy = &destroy;
        return a;
    }
};

LoadedPlugin base(const char* name) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.path = std::string("C:/plugins/") + name + ".dll";
    p.version = "1.0.0";
    return p;
}

void throwForBoom(const LoadedPlugin& lp) {
    if (lp.name == "Boom") { throw std::runtime_error("allocation failed"); }
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    const CascadeImageDecoderApi picApi = Pic::api();
    const CascadeImageInputApi picInput = Pic::input();
    const CascadeIqDecoderApi iqApi = Iq::api();
    LoadedPlugin picture = base("Picture");
    picture.imageDecoder = &picApi;
    picture.imageInput = &picInput;
    LoadedPlugin slow = base("Slow");
    slow.iqDecoder = &iqApi;
    LoadedPlugin boom = base("Boom");
    boom.iqDecoder = &iqApi;

    // ---------------------------------------------------------------- FINDING 2: A PRESS IS NEVER LOST TO A BUSY LOCK
    {
        std::printf("a press while the DSP thread holds the runner's lock is delivered when the lock is free, not dropped\n");
        Pic::reset();
        PluginRunner r;
        r.rebuild({picture}, 48000.0, kIqRate, kCentre);
        std::vector<HostImage> imgs;
        r.pollImages(imgs);                    // the picture arrives: the press needs one to know its size
        CHECK(imgs.size() == 1u && !imgs[0].pixels.empty());
        // The DSP thread is inside processIq (the lock held) for about 300 ms.
        Pic::holdProcess = true;
        std::thread dsp([&] {
            std::vector<float> block(4096 * 2, 0.0f);
            r.processIq(block.data(), 4096);
        });
        CHECK(waitFor([] { return Pic::inProcess.load(); }, 3000));
        std::thread release([] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); Pic::holdProcess = false; });
        const bool delivered = r.imagePress("Picture", 1, 1);      // the control thread's call: waits for the lock
        release.join();
        dsp.join();
        CHECK(delivered);
        CHECK(Pic::presses.load() == 1);
        r.clear();
    }

    // ---------------------------------------------------------------- FINDING 6: A REBUILD DOES NOT BLANK THE PICTURES
    {
        std::printf("pollImages keeps the pictures it has while a rebuild's creates run\n");
        Pic::reset(); Iq::reset();
        PluginRunner r;
        r.rebuild({picture}, 48000.0, kIqRate, kCentre);
        std::vector<HostImage> imgs;
        r.pollImages(imgs);
        CHECK(imgs.size() == 1u && !imgs[0].pixels.empty());
        // A rebuild whose slow plugin's create() runs for as long as the test wants (the runner is empty, with the lock free, meanwhile).
        Iq::slowCreate = true;
        std::thread rb([&] { r.rebuild({picture, slow}, 48000.0, kIqRate, kCentre); });
        CHECK(waitFor([] { return Iq::inCreate.load() == 1; }, 3000));
        for (int i = 0; i < 20; ++i) {
            r.pollImages(imgs);
            CHECK(imgs.size() == 1u);
            CHECK(!imgs.empty() && !imgs[0].pixels.empty());   // not blanked
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
        Iq::releaseCreate = true;
        rb.join();
        r.pollImages(imgs);
        CHECK(imgs.size() == 1u && !imgs.empty() && !imgs[0].pixels.empty());   // carried over by name once the new set is published
        CHECK(r.activeCount() == 2u);
        r.clear();
    }

    // ---------------------------------------------------------------- FINDING 7: A THROW OUT OF A BUILD
    {
        std::printf("a build that throws leaves no starting mark, no rebuilding flag and no leaked instance\n");
        Pic::reset(); Iq::reset();
        auto clock = std::make_shared<cascade::core::StreamClock>();
        PluginRunner r;
        r.setStreamClock(clock);
        r.rebuild({picture}, 48000.0, kIqRate, kCentre);
        // (1) startPlugin: the build throws before it makes anything.
        PluginRunner::setBuildHookForTest(&throwForBoom);
        bool threw = false;
        try { (void)r.startPlugin(boom); } catch (const std::runtime_error&) { threw = true; }
        PluginRunner::setBuildHookForTest(nullptr);
        CHECK(threw);
        CHECK(!hasRow(r, "Boom", DecoderIdleReason::Starting));    // no "starting" row for ever
        CHECK(r.startPlugin(boom));                                // and it can be started again: it is not still "already starting"
        CHECK(r.hasInstances(cascade::core::pluginKey(boom)));
        // (2) rebuild: Picture is built, then Boom throws; Picture's new instance is destroyed, not leaked, and the stream clock still runs afterwards.
        const int createdBefore = Pic::created.load();
        const int destroyedBefore = Pic::destroyed.load();
        PluginRunner::setBuildHookForTest(&throwForBoom);
        threw = false;
        try { r.rebuild({picture, boom}, 48000.0, kIqRate, kCentre); } catch (const std::runtime_error&) { threw = true; }
        PluginRunner::setBuildHookForTest(nullptr);
        CHECK(threw);
        CHECK(Pic::created.load() == createdBefore + 1);                      // Picture was built...
        CHECK(Pic::destroyed.load() == destroyedBefore + 2);                  // ...and destroyed (the old instance the rebuild put down first, and the new one the throw discarded)
        CHECK(!hasRow(r, "Picture", DecoderIdleReason::Starting));
        CHECK(!hasRow(r, "Boom", DecoderIdleReason::Starting));
        cascade::core::StreamFacts f0{};
        clock->read(f0);
        std::vector<float> block(4096 * 2, 0.0f);
        r.processIq(block.data(), 4096);
        cascade::core::StreamFacts f1{};
        clock->read(f1);
        CHECK(f1.iqFrames == f0.iqFrames + 4096u);                            // the stream clock advances: rebuilding_ was reset
        // a rebuild that is not asked to throw works again
        r.rebuild({picture, boom}, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 2u);
        r.clear();
    }

    // ---------------------------------------------------------------- THE SEAM IS THE TESTS' ONLY
    {
        std::error_code ec;
        for (const auto& e : std::filesystem::recursive_directory_iterator(std::filesystem::path(CASCADE_SOURCE_DIR) / "src", ec)) {
            if (!e.is_regular_file()) { continue; }
            const std::string name = e.path().filename().string();
            if (name == "plugin_runner.cpp" || name == "plugin_runner.hpp") { continue; }
            CHECK(readFile(e.path()).find("setBuildHookForTest(") == std::string::npos);
        }
    }

    return testSummary("test_plugin_runner_review");
}
