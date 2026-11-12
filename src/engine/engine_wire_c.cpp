// engine_wire_c.cpp - the engine's side of wiring lane C: the op block 0x8A00-0x8AFF, its facts, its once-a-pass pump.
// The settings registry and the state are in wire_c.{hpp,cpp}; each feature of the lane adds its own file next to this one and one `case` here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "engine/engine.hpp"
#include "engine/wire_c.hpp"

namespace cascade::engine {

namespace wc = cascade::engine::wirec;

wc::State& Engine::wireC() {
    std::lock_guard<std::mutex> lk(wireCMutex_);   // (the first caller may be the control pass or the fact builder: whichever asks first builds it)
    if (!wireC_) {
        const std::filesystem::path folder = wireCFolderForTest_.empty()
            ? std::filesystem::path(cascade::core::ConfigStore::defaultPath()).parent_path()
            : std::filesystem::path(wireCFolderForTest_);
        wireC_ = std::make_unique<wc::State>(folder);
    }
    return *wireC_;
}

std::int32_t Engine::applyWireC(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    (void)res;
    switch (c.op) {
        case FOXAPP_OP_WIREC_SETTING: {
            const std::size_t sep = text.find('\x1F');
            if (sep == std::string::npos || sep == 0) {
                why = "the text is \"<key>\\x1F<value>\"";
                return FOXAPI_BAD_ARGUMENT;
            }
            switch (wireC().settings().set(text.substr(0, sep), text.substr(sep + 1), why)) {
                case wc::SetResult::Changed: ++wireCVersion_; return FOXAPI_OK;
                case wc::SetResult::NoChange: return FOXAPI_NO_CHANGE;
                case wc::SetResult::NotFound: return FOXAPI_NOT_FOUND;
                case wc::SetResult::BadValue: return FOXAPI_BAD_ARGUMENT;
                case wc::SetResult::OutOfRange: return FOXAPI_OUT_OF_RANGE;
                case wc::SetResult::SaveFailed: return FOXAPI_FAILED;
            }
            return FOXAPI_FAILED;
        }
        default:
            break;
    }
    why = "not an operation of this block";
    return FOXAPI_UNSUPPORTED;
}

void Engine::wireCFacts(const std::function<void(const std::string&, const std::string&)>& fact) {
    wc::State& s = wireC();
    s.settings().each([&fact](const std::string& key, const std::string& value) { fact("wirec." + key, value); });
    fact("wirec.version", std::to_string(wireCVersion()));
    fact("wirec.error", s.settings().error());
    // POWER AND RESOURCES (wire_c_power.hpp). "" is "not available" - a reading that could not be made is never shown as 0.
    if (s.hasPower()) {
        wc::Power& p = s.power();
        const wc::PerfFacts f = p.perf();
        fact("sys.keepawake", p.keepAwakeHeld() && p.keepAwakeActive() ? "1" : "0");
        fact("sys.resume.count", std::to_string(s.resumeActions));
        fact("perf.cpu_percent", f.cpuPercent);
        fact("perf.memory_mb", f.memoryMb);
        fact("perf.system_memory_percent", f.systemMemoryPercent);
        fact("perf.gpu_percent", f.gpuPercent);
        fact("perf.reduced", f.reduced ? "1" : "0");
        fact("perf.switch.ui_frame_cap_hz", std::to_string(f.switches.uiFrameCapHz));
        fact("perf.switch.waterfall_rows_per_second", std::to_string(f.switches.waterfallRowsPerSecond));
        fact("perf.switch.animations", f.switches.animations ? "1" : "0");
        fact("perf.switch.glow_and_shadows", f.switches.glowAndShadows ? "1" : "0");
        fact("perf.switch.three_d_views_live", f.switches.threeDViewsLive ? "1" : "0");
        fact("perf.switch.background_map_redraw", f.switches.backgroundMapRedraw ? "1" : "0");
    }
}

std::uint64_t Engine::wireCVersion() {
    return wireCVersion_ + wireC().settings().version();
}

// Anything recording right now: an audio or I/Q take, the IF or multiplex take of any radio, or a scheduled capture that is starting or recording. The state, not the
// start and stop sites, so a take that ends any way at all ends the hold (wire_c_power.hpp).
bool Engine::wireCRecordingBusy() const {
    for (const auto& r : receivers_) {
        if (r && (r->iqRecorder_.recording() || r->audioRecorder_.recording() || r->ifRecorder_.recording() || r->mpxRecorder_.recording())) { return true; }
    }
    for (const auto& [id, rt] : scheduleRt_) {
        (void)id;
        if (rt.state == "recording" || rt.state == "starting") { return true; }
    }
    return false;
}

// What the engine does after the PC wakes: the radios are looked for again (USB ones re-enumerate while it sleeps) and a saved radio that was waiting is tried at
// once, the same two steps the hot-plug notice takes. It does NOT close and reopen a radio that is open and healthy: whether that is wanted is not settled.
void Engine::wireCResumeAction() {
    scanNative();
    scanSoapy();
    if (rx().savedRadioRetryBusy_) {
        rx().savedRadioRetryAttempts_ = 0;
        rx().savedRadioRetryLastFailSec_ = -1.0e9;
    }
}

void Engine::pumpWireC() {
    wc::State& s = wireC();
    wc::Power& p = s.power();
    if (!s.powerStarted) {
        s.powerStarted = true;
        p.startSampler();
        p.startResumeWatch();
    }
    const std::string mode = s.settings().get("perf.mode");
    p.setPerfMode(mode == "on" ? cascade::core::sys::EffectsMode::On : mode == "off" ? cascade::core::sys::EffectsMode::Off : cascade::core::sys::EffectsMode::Auto);
    if (p.keepAwakePass(s.settings().getBool("sys.keep_awake") && wireCRecordingBusy())) { ++wireCVersion_; }
    if (p.perfVersion() != s.perfSeen) {
        s.perfSeen = p.perfVersion();
        ++wireCVersion_;
    }
    // WEB: the cross-origin allow-list (net/web_wire_c.hpp). The server takes a new list at any time; a list it refuses is logged and the old one stays.
    {
        const std::string origins = s.settings().get("web.cors_origins");
        if (origins != s.corsApplied) {
            std::vector<std::string> lines;
            std::size_t from = 0;
            while (from <= origins.size()) {
                std::size_t to = origins.find('\n', from);
                if (to == std::string::npos) { to = origins.size(); }
                std::string line = origins.substr(from, to - from);
                if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                if (!line.empty()) { lines.push_back(line); }
                from = to + 1;
            }
            std::string err;
            if (servers().webServer().setCors(lines, err)) { s.corsApplied = origins; }
            else { s.corsApplied = origins; cascade::core::diagLogf("web: the cross-origin list was refused (%s)", err.c_str()); }
        }
    }
    if (p.takeResume()) {
        if (s.settings().getBool("sys.resume_reopen")) {
            cascade::core::diagLogf("power: the PC woke from sleep; looking for the radios again");
            wireCResumeAction();
            ++s.resumeActions;
        }
        ++wireCVersion_;
    }
}

}  // namespace cascade::engine
