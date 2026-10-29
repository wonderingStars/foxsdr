// Fuzz target: the host API a plugin calls (plugin containment P2, docs/engine-stage4.md 6.29) - a SEQUENCE of calls with arbitrary arguments
// against the core that answers them (core/plugin_api.hpp), with the owner changing the world between them: grants given and withdrawn, a
// plugin stopped and started, queues drained.
//
// The host API is the one door a plugin has INTO the host, and it is called from whatever thread the plugin likes. Its rules are the ABI's
// (core/plugin_abi.h, HOST API LEVEL 1): validate and permission-check at once, queue into bounded rings, answer DETACHED once the table is shut
// down, never wait, never grow. The fuzzer is the plugin: raw doubles (NaN, infinities, denormals), structs with any structSize the plugin could
// compile, labels with no terminator, indices past the end, buffers of exactly the size it names.
//
// THE PROPERTIES: every answer is one the ABI defines; the queues never exceed their bounds (what the owner drains is at most the ring's size);
// a plugin that is not live is answered DETACHED by every call and leaves nothing behind (no mark, no command, no queued request); what is
// accepted is sane (a mark has a positive finite frequency, a queued control has finite numbers in the ABI's ranges); and the per-plugin quotas
// hold (64 marks, 16 commands).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/plugin_api.hpp"
#include "fuzz_common.hpp"

using cascade::core::PluginApiClient;
using cascade::core::PluginApiCore;
using cascade::core::PluginControl;

namespace {

bool isAbiAnswer(std::int32_t r) { return r >= CASCADE_API_WRONG_THREAD && r <= 1; }   // OK .. WRONG_THREAD, and poll_command's 1

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    PluginApiCore core;
    const std::string keyA = "fuzz-a-1.0.0-abi3-linux-x64.so";
    const std::string keyB = "fuzz-b-1.0.0-abi3-linux-x64.so";
    PluginApiClient& a = core.client(keyA, "Fuzz A", CASCADE_CAP_HOST_CLIENT);
    PluginApiClient& b = core.client(keyB, "Fuzz B", CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_RECEIVER_LOCATOR);
    core.setLiveSet({keyA, keyB});

    for (int step = 0; step < 160 && in.remaining() > 0; ++step) {
        PluginApiClient& c = (in.u8() & 1u) != 0u ? a : b;
        const std::string& key = (&c == &a) ? keyA : keyB;
        switch (in.u8() % 14) {
            case 0: {   // the receiver, read: into a struct of exactly the size the plugin names
                const std::uint32_t declared = in.inRange(0, 400);
                std::vector<std::uint8_t> raw(declared);
                if (declared >= 4) { std::memcpy(raw.data(), &declared, 4); }
                auto* out = reinterpret_cast<CascadeReceiverState*>(raw.data());
                const std::int32_t r = core.getState(c, declared >= 4 ? out : nullptr);
                FUZZ_REQUIRE(isAbiAnswer(r));
                break;
            }
            case 1: {
                CascadeGainInfo g;
                std::memset(&g, 0, sizeof(g));
                g.structSize = in.inRange(0, static_cast<std::uint32_t>(sizeof(g)));   // (a plugin names the size of the struct it allocated, no more)
                FUZZ_REQUIRE(isAbiAnswer(core.getGain(c, in.u32(), (in.u8() & 7u) != 0u ? &g : nullptr)));
                break;
            }
            case 2: {
                const std::uint32_t cap = in.inRange(0, 20);
                std::vector<double> rates(cap);
                FUZZ_REQUIRE(core.getSampleRates(c, cap != 0 ? rates.data() : nullptr, cap) >= CASCADE_API_WRONG_THREAD);   // a count, or an error
                break;
            }
            case 3: {
                CascadeStreamInfo s;
                std::memset(&s, 0, sizeof(s));
                s.structSize = in.inRange(0, static_cast<std::uint32_t>(sizeof(s)));
                FUZZ_REQUIRE(isAbiAnswer(core.getStreamInfo(c, (in.u8() & 7u) != 0u ? &s : nullptr)));
                break;
            }
            case 4: {   // a request to change the receiver, with any number
                PluginControl req;
                req.kind = static_cast<PluginControl::Kind>(in.u8() % 11);   // (the kinds the host's own trampolines send: eleven)
                req.value = in.rawDouble();
                req.mode = in.u32();
                req.flag = (in.u8() & 1u) != 0u;
                const std::string gain = in.str(in.inRange(0, 40));
                std::snprintf(req.gainName, sizeof(req.gainName), "%s", gain.c_str());
                FUZZ_REQUIRE(isAbiAnswer(core.requestControl(c, req)));
                break;
            }
            case 5: {   // a mark: a struct of the size the plugin names, with a label that may have no end
                // (a plugin names the size of the struct it allocated: at least the 4 bytes that hold the size, or it passes no struct at all)
                std::uint32_t declared = in.inRange(0, 120);
                if (declared < 4) { declared = 0; }
                std::vector<std::uint8_t> raw(declared);
                for (auto& byte : raw) { byte = in.u8(); }
                if (declared >= 4) { std::memcpy(raw.data(), &declared, 4); }
                const std::int32_t r = core.setMarker(c, declared != 0 ? reinterpret_cast<const CascadeMarker*>(raw.data()) : nullptr);
                FUZZ_REQUIRE(isAbiAnswer(r));
                if (declared < sizeof(CascadeMarker)) { FUZZ_REQUIRE(r == CASCADE_API_BAD_ARGUMENT || r == CASCADE_API_DETACHED); }   // a short struct is refused, never read past
                break;
            }
            case 6: FUZZ_REQUIRE(isAbiAnswer(core.removeMarker(c, in.u32()))); break;
            case 7: FUZZ_REQUIRE(isAbiAnswer(core.clearMarkers(c))); break;
            case 8: {
                const std::string text = in.str(in.inRange(0, 700));
                FUZZ_REQUIRE(isAbiAnswer(core.log(c, in.u32() % 6u, (in.u8() & 15u) != 0u ? text.c_str() : nullptr)));
                break;
            }
            case 9: {   // commands
                const std::string label = in.str(in.inRange(0, 50));
                FUZZ_REQUIRE(isAbiAnswer(core.addCommand(c, in.u32() % 40u, label.c_str())));
                if ((in.u8() & 3u) == 0u) { FUZZ_REQUIRE(isAbiAnswer(core.removeCommand(c, in.u32() % 40u))); }
                std::uint32_t id = 0;
                FUZZ_REQUIRE(isAbiAnswer(core.pollCommand(c, (in.u8() & 7u) != 0u ? &id : nullptr)));
                (void)core.pressCommand(key, in.u32() % 40u);
                break;
            }
            case 10: {   // the owner: grants, the stop set, a plugin put to sleep and woken
                switch (in.u8() % 6) {
                    case 0: core.setTuneGranted(key, (in.u8() & 1u) != 0u); break;
                    case 1: core.setSettingsGranted(key, (in.u8() & 1u) != 0u); break;
                    case 2: core.setTxGranted(key, (in.u8() & 1u) != 0u); break;
                    case 3: core.setStopped((in.u8() & 1u) != 0u ? std::vector<std::string>{"fuzz-a"} : std::vector<std::string>{}); break;
                    case 4: core.setLive(key, (in.u8() & 1u) != 0u); break;
                    case 5: core.setLiveSet((in.u8() & 1u) != 0u ? std::vector<std::string>{keyA, keyB} : std::vector<std::string>{keyB}); break;
                }
                break;
            }
            case 11: {   // the owner drains
                std::vector<PluginControl> controls;
                core.takeControls(controls);
                FUZZ_REQUIRE(controls.size() <= PluginApiCore::kControlQueue);
                for (const PluginControl& req : controls) {
                    // what was queued had passed the checks: no NaN or infinity got through to a control
                    if (req.kind != PluginControl::Kind::DeviceAgc && req.kind != PluginControl::Kind::Running && req.kind != PluginControl::Kind::Muted &&
                        req.kind != PluginControl::Kind::Mode) {
                        FUZZ_REQUIRE(std::isfinite(req.value));
                    }
                    FUZZ_REQUIRE(std::memchr(req.gainName, '\0', sizeof(req.gainName)) != nullptr);
                }
                std::vector<cascade::core::PluginLogLine> lines;
                core.takeLog(lines);
                FUZZ_REQUIRE(lines.size() <= PluginApiCore::kLogQueue);
                for (const auto& l : lines) { FUZZ_REQUIRE(l.text.size() < CASCADE_LOG_TEXT_BYTES); }
                break;
            }
            case 12: {   // the legacy request, queued newest-wins
                const std::int32_t r = core.queueCentreRetune(key, in.rawDouble());
                FUZZ_REQUIRE(r >= CASCADE_TUNE_FAILED && r <= CASCADE_TUNE_OK);
                std::vector<PluginApiCore::CentreRetune> retunes;
                core.takeCentreRetunes(retunes);
                FUZZ_REQUIRE(retunes.size() <= PluginApiCore::kCentreRetuneQueue);
                break;
            }
            case 13: {   // what the owner sees
                std::vector<cascade::core::HostMarker> marks;
                core.markers(marks);
                FUZZ_REQUIRE(marks.size() <= 2 * static_cast<std::size_t>(CASCADE_MAX_MARKERS_PER_PLUGIN));
                for (const auto& m : marks) {
                    FUZZ_REQUIRE(std::isfinite(m.m.freqHz) && m.m.freqHz > 0.0 && m.m.freqHz < 1e12);
                    FUZZ_REQUIRE(m.m.kind == CASCADE_MARKER_POINT || m.m.kind == CASCADE_MARKER_SPAN);
                    FUZZ_REQUIRE(std::memchr(m.m.label, '\0', sizeof(m.m.label)) != nullptr);
                    if (m.m.kind == CASCADE_MARKER_SPAN) { FUZZ_REQUIRE(std::isfinite(m.m.widthHz) && m.m.widthHz > 0.0 && m.m.widthHz <= 1e10); }
                }
                FUZZ_REQUIRE(core.commands(keyA).size() <= CASCADE_MAX_COMMANDS_PER_PLUGIN);
                FUZZ_REQUIRE(core.commands(keyB).size() <= CASCADE_MAX_COMMANDS_PER_PLUGIN);
                break;
            }
        }
        // A PLUGIN THAT IS NOT LIVE leaves nothing behind and is answered DETACHED.
        if (!c.live.load()) {
            CascadeMarker m;
            std::memset(&m, 0, sizeof(m));
            m.structSize = sizeof(m);
            m.freqHz = 1.0e6;
            FUZZ_REQUIRE(core.setMarker(c, &m) == CASCADE_API_DETACHED);
            FUZZ_REQUIRE(core.log(c, CASCADE_LOG_INFO, "x") == CASCADE_API_DETACHED);
            FUZZ_REQUIRE(core.addCommand(c, 1, "x") == CASCADE_API_DETACHED);
            FUZZ_REQUIRE(core.commands(key).empty());
        }
    }
    return 0;
}
