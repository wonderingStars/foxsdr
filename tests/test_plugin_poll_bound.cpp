// PluginRunner's poll_text drain loops must be BOUNDED per block.
//
// processAudio() and processIq() run on the real-time DSP thread with the
// runner's mutex held, and each ends by draining every decoder's text with
// poll_text() until it answers 0. The drain loops had no bound at all, so ONE
// decoder whose poll_text keeps answering a positive count - a read cursor
// that never advances, a "1 byte pending" that is never true - spun the DSP
// thread for ever, and with it every other decoder, the audio the user hears
// and every GUI reader of the runner (status(), isFeeding(), pollImages()...).
//
// THE STUCK FAKES HAVE A SAFETY NET so the unfixed code terminates instead of
// hanging the suite: after kSafetyNet calls in one block they answer 0. A
// bounded loop never gets near it; an unbounded one stops only there, which is
// what the red run shows. All three drain loops are covered - text decoders
// (pollLocked), I/Q decoders (pollIqLocked) and picture decoders' status text
// (pollImageTextLocked) - because they were three separate copies of the loop.
//
// The last block is the other half of the property: bounding the loop must not
// LOSE text. A well-behaved decoder with far more queued than one block's
// budget still delivers every byte, over the following blocks.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/plugin_runner.hpp"
#include "test_check.hpp"

namespace {

using cascade::core::DecodedLine;
using cascade::core::LoadedPlugin;
using cascade::core::PluginRunner;

constexpr int kSafetyNet = 100000;
// What "bounded" means for this test: a handful of polls per block per
// decoder, far below the safety net. The patch runner's own drain uses 8.
constexpr int kReasonablePollsPerBlock = 64;

constexpr double kIqRate = 2400000.0;
constexpr double kCentre = 1090000000.0;

// --- a stuck poll_text, shared by all three decoder kinds ---------------------

int g_stuckPolls = 0;

int32_t stuckPoll(void*, char* buf, size_t cap) {
    ++g_stuckPolls;
    if (g_stuckPolls > kSafetyNet) { return 0; }   // only the unfixed loop gets here
    if (cap == 0) { return 0; }
    buf[0] = 'x';
    return 1;   // "one byte", for ever - the cursor never advances
}

int g_handle = 0;
void* audioCreate(uint32_t) { return &g_handle; }
void* iqCreate(double, double) { return &g_handle; }
void process(void*, const float*, size_t) {}
void destroy(void*) {}
int32_t noImage(void*, CascadeImage*) { return 0; }
void noRelease(void*, const CascadeImage*) {}

// --- a well-behaved decoder with a lot to say ---------------------------------

std::string g_queued;
int g_goodPolls = 0;

int32_t goodPoll(void*, char* buf, size_t cap) {
    ++g_goodPolls;
    if (g_queued.empty()) { return 0; }
    const std::size_t n = g_queued.size() < cap ? g_queued.size() : cap;
    std::memcpy(buf, g_queued.data(), n);
    g_queued.erase(0, n);
    return static_cast<int32_t>(n);
}

LoadedPlugin plugin(const char* name) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.path = std::string(name) + ".dll";
    return p;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    CascadeDecoderApi stuckAudio{};
    stuckAudio.structSize = static_cast<uint32_t>(sizeof(CascadeDecoderApi));
    stuckAudio.requiredRateHz = 0;
    stuckAudio.create = &audioCreate;
    stuckAudio.process = &process;
    stuckAudio.poll_text = &stuckPoll;
    stuckAudio.destroy = &destroy;

    CascadeIqDecoderApi stuckIq{};
    stuckIq.structSize = static_cast<uint32_t>(sizeof(CascadeIqDecoderApi));
    stuckIq.requiredRateHz = 0.0;
    stuckIq.preferredRateHz = 0.0;
    stuckIq.create = &iqCreate;
    stuckIq.process = &process;
    stuckIq.retune = nullptr;
    stuckIq.poll_text = &stuckPoll;
    stuckIq.destroy = &destroy;

    CascadeImageDecoderApi stuckImg{};
    stuckImg.structSize = static_cast<uint32_t>(sizeof(CascadeImageDecoderApi));
    stuckImg.inputKind = CASCADE_INPUT_AUDIO;
    stuckImg.requiredRateHz = 0.0;
    stuckImg.preferredRateHz = 0.0;
    stuckImg.create = &iqCreate;
    stuckImg.process = &process;
    stuckImg.retune = nullptr;
    stuckImg.poll_image = &noImage;
    stuckImg.release_image = &noRelease;
    stuckImg.poll_text = &stuckPoll;
    stuckImg.destroy = &destroy;

    const std::vector<float> audio(4800, 0.0f);
    const std::vector<float> iq(2 * 4800, 0.0f);

    // [B1] A text (audio) decoder: pollLocked, from processAudio.
    {
        LoadedPlugin p = plugin("StuckAudio");
        p.decoder = &stuckAudio;
        PluginRunner r;
        r.rebuild({p}, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 1u);
        g_stuckPolls = 0;
        r.processAudio(audio.data(), audio.size());
        std::printf("[B1] audio decoder: %d poll_text calls in one block\n", g_stuckPolls);
        CHECK(g_stuckPolls >= 1);
        CHECK(g_stuckPolls <= kReasonablePollsPerBlock);
        // Still polled on the next block: bounded is not "given up on".
        g_stuckPolls = 0;
        r.processAudio(audio.data(), audio.size());
        CHECK(g_stuckPolls >= 1);
        CHECK(g_stuckPolls <= kReasonablePollsPerBlock);
    }

    // [B2] An I/Q decoder: pollIqLocked, from processIq.
    {
        LoadedPlugin p = plugin("StuckIq");
        p.iqDecoder = &stuckIq;
        PluginRunner r;
        r.rebuild({p}, 48000.0, kIqRate, kCentre);
        CHECK(r.activeCount() == 1u);
        g_stuckPolls = 0;
        r.processIq(iq.data(), iq.size() / 2);
        std::printf("[B2] I/Q decoder: %d poll_text calls in one block\n", g_stuckPolls);
        CHECK(g_stuckPolls >= 1);
        CHECK(g_stuckPolls <= kReasonablePollsPerBlock);
    }

    // [B3] A picture decoder's status text: pollImageTextLocked.
    {
        LoadedPlugin p = plugin("StuckPicture");
        p.imageDecoder = &stuckImg;
        PluginRunner r;
        r.rebuild({p}, 48000.0, kIqRate, kCentre);
        g_stuckPolls = 0;
        r.processAudio(audio.data(), audio.size());
        std::printf("[B3] picture decoder: %d poll_text calls in one block\n", g_stuckPolls);
        CHECK(g_stuckPolls >= 1);
        CHECK(g_stuckPolls <= kReasonablePollsPerBlock);
    }

    // [B4] NOTHING IS LOST: 300 lines of 1000 characters (about 300 KB, far
    // more than one block's budget of 8 KB polls) all arrive, whole and in
    // order, over as many blocks as it takes.
    {
        CascadeDecoderApi good = stuckAudio;
        good.poll_text = &goodPoll;
        LoadedPlugin p = plugin("Chatty");
        p.decoder = &good;
        PluginRunner r;
        r.rebuild({p}, 48000.0, kIqRate, kCentre);
        g_queued.clear();
        for (int i = 0; i < 300; ++i) {
            std::string line = "L" + std::to_string(i) + ":";
            line.resize(1000, static_cast<char>('a' + (i % 26)));
            g_queued += line + "\n";
        }
        std::vector<DecodedLine> got;
        int blocks = 0;
        while (blocks < 100 && (!g_queued.empty() || blocks == 0)) {
            r.processAudio(audio.data(), audio.size());
            const std::vector<DecodedLine> part = r.drainText();
            got.insert(got.end(), part.begin(), part.end());
            ++blocks;
        }
        std::printf("[B4] %zu lines over %d blocks\n", got.size(), blocks);
        CHECK(g_queued.empty());
        CHECK(got.size() == 300u);
        bool inOrder = got.size() == 300u;
        for (std::size_t i = 0; i < got.size() && i < 300u; ++i) {
            const std::string want = "L" + std::to_string(i) + ":";
            if (got[i].text.size() != 1000u || got[i].text.compare(0, want.size(), want) != 0) {
                inOrder = false;
            }
        }
        CHECK(inOrder);
    }

    return testSummary("test_plugin_poll_bound");
}
