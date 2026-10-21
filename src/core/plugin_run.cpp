// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/plugin_run.hpp"

#include <cstddef>

#include "core/plugin_abi.h"
#include "core/user_presets.hpp"

namespace cascade::core {

namespace {

bool endsWithNoCase(const std::string& s, const char* suffix) {
    std::size_t n = 0;
    while (suffix[n] != '\0') { ++n; }
    if (s.size() < n) { return false; }
    for (std::size_t i = 0; i < n; ++i) {
        char c = s[s.size() - n + i];
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
        if (c != suffix[i]) { return false; }
    }
    return true;
}

}  // namespace

const char* pluginRunWord(PluginRun r) {
    switch (r) {
        case PluginRun::Auto: return "auto";
        case PluginRun::Always: return "always";
        case PluginRun::Stopped: return "stopped";
    }
    return "auto";
}

bool parsePluginRun(const std::string& word, PluginRun& out) {
    if (word == "auto") {
        out = PluginRun::Auto;
        return true;
    }
    if (word == "always") {
        out = PluginRun::Always;
        return true;
    }
    if (word == "stopped") {
        out = PluginRun::Stopped;
        return true;
    }
    return false;
}

std::string pluginRunId(const std::string& moduleFileOrId) {
    std::string s = moduleFileOrId;
    const std::size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) { s = s.substr(slash + 1); }
    if (s.empty()) { return s; }
    // A MODULE FILE NAME is recognised by its extension; anything else is an id already, and
    // is returned as it is (see the header: this must be idempotent).
    if (endsWithNoCase(s, ".dll") || endsWithNoCase(s, ".so") || endsWithNoCase(s, ".dylib")) {
        return userPresetKey(s);
    }
    return s;
}

namespace {

// The capabilities something can be opened or fed by, which is what can wake a module.
constexpr std::uint32_t kWakeableCaps = CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER |
                                        CASCADE_CAP_IMAGE_DECODER | CASCADE_CAP_TRACK_SOURCE |
                                        CASCADE_CAP_PANEL | CASCADE_CAP_INSTRUMENT;

}  // namespace

bool pluginCapsHaveLifecycle(std::uint32_t capabilities) {
    return (capabilities & (kWakeableCaps | CASCADE_CAP_AUDIO_PROCESSOR | CASCADE_CAP_HOST_CLIENT)) != 0u;
}

bool pluginCapsStandingDuty(std::uint32_t capabilities) {
    if ((capabilities & CASCADE_CAP_AUDIO_PROCESSOR) != 0u) { return true; }
    return (capabilities & CASCADE_CAP_HOST_CLIENT) != 0u && (capabilities & kWakeableCaps) == 0u;
}

bool pluginWanted(const PluginUse& u) {
    return u.window || u.mapPage || u.radarScope || u.patchNode || u.playingAudio ||
           u.decoderOutput || u.browserSession || u.textSink || u.standing;
}

const char* pluginUseSignal(const PluginUse& u) {
    if (u.window) { return "window open"; }
    if (u.mapPage) { return "map page open"; }
    if (u.radarScope) { return "radar scope showing"; }
    if (u.patchNode) { return "patch node running"; }
    if (u.playingAudio) { return "playing audio"; }
    if (u.decoderOutput) { return "decoder output window open"; }
    if (u.browserSession) { return "browser connected"; }
    if (u.textSink) { return "wired to a Text sink"; }
    if (u.standing) { return "standing duty"; }
    return "";
}

PluginLifeStep stepPluginLife(PluginRun mode, bool wanted, std::int64_t nowMs,
                              std::int64_t dormantAfterMs, PluginLife& life) {
    switch (mode) {
        case PluginRun::Stopped:
            if (life.running) {
                life.running = false;
                return PluginLifeStep::Sleep;
            }
            return PluginLifeStep::None;
        case PluginRun::Always:
            life.lastWantedMs = nowMs;
            if (!life.running) {
                life.running = true;
                return PluginLifeStep::Wake;
            }
            return PluginLifeStep::None;
        case PluginRun::Auto:
            break;
    }
    if (wanted) {
        life.lastWantedMs = nowMs;
        if (!life.running) {
            life.running = true;
            return PluginLifeStep::Wake;
        }
        return PluginLifeStep::None;
    }
    if (life.running && nowMs - life.lastWantedMs >= dormantAfterMs) {
        life.running = false;
        return PluginLifeStep::Sleep;
    }
    return PluginLifeStep::None;
}

}  // namespace cascade::core
