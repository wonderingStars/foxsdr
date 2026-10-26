// engine_diagnostics.cpp - the two bounded-run plugin diagnostics, from gui/app_window.cpp (window port group 14, part e).
//
// BOTH ARE TEST MACHINERY, NOT INTERFACE: they print what the engine decided, to stdout, for a headless run that has to prove
// the enforcement and the install gate worked (a screenshot of a red row proves nothing about what is MAPPED in this process).
// They read only the engine's own state, so they are the engine's. They were the window's until part e, and were the largest
// block of direct reads in the window (reportPluginStatus 33, reportPluginTestResult 3: 36 of the ratchet's last 77).
//
//   reportPluginTestResult(frame)  CASCADE_PLUGIN_TEST: the catalogue and every entry's install-gate decision. The window's
//                                  onCatalogueResult hook (a bounded run with the hook set) calls it from inside the engine's
//                                  poll, naming the frame; tests/test_catalogue_error_report.cpp reads its output.
//   reportPluginStatus(images, ui) CASCADE_PLUGIN_STATUS: what the enforcement decided, the source, the plugin UI, the runner.
//                                  run() calls it right after shutdownQuiesce (a bounded run only), where it has always stood;
//                                  the receiver plugins' PICTURES are the window's own buffer (the plugin windows poll them
//                                  into it), so the window hands them over and the engine prints them where they always came.
//                                  THE PLUGIN UI IS THE WINDOW'S TOO since the pump move's step P3 (the tracks, paths and panels
//                                  its poll found): it hands those over beside the pictures.
//
// The bodies are the window's, moved: not a word of what they print changed (tests/test_plugin_status_report.cpp holds the
// output against a fixture's).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/host_image.hpp"
#include "core/plugin_repo.hpp"
#include "engine/engine.hpp"
#include "engine/receiver_tables.hpp"

namespace cascade::engine {

void Engine::reportPluginTestResult(int frame) const {
    // Bounded-run diagnostic only (CASCADE_PLUGIN_TEST). Machine-readable on
    // purpose: the install gate is a UI decision, and this is the only way a
    // headless run can prove which way it went.
    if (!statusText().catalogError.empty()) {
        std::printf("plugin catalogue: FAILED frame=%d %s\n", frame,
                    statusText().catalogError.c_str());
        return;
    }
    std::printf("plugin catalogue: frame=%d entries=%d\n", frame,
                static_cast<int>(catalog_.size()));
    for (int i = 0; i < static_cast<int>(catalog_.size()); ++i) {
        const cascade::core::PluginCatalogEntry& e =
            catalog_[static_cast<std::size_t>(i)];
        // Both answers: what the gate says with the acknowledgement UNTICKED
        // (what the user first sees) and with it ticked. A gate that stopped
        // requiring the tick would print install=ok on the first of these.
        const std::string noAck = pluginInstallBlockedReason(i, false);
        const std::string ack = pluginInstallBlockedReason(i, true);
        const std::string noAckText = noAck.empty() ? "ok" : ("blocked(" + noAck + ")");
        const std::string ackText = ack.empty() ? "ok" : ("blocked(" + ack + ")");
        std::printf("plugin entry: id=%s abi=%u compatible=%d installed=%d legal=%d "
                    "licence=\"%s\" install=%s installAcked=%s\n",
                    e.id.c_str(), static_cast<unsigned>(e.abiVersion),
                    e.compatible ? 1 : 0, catalogEntryInstalled(e) ? 1 : 0,
                    e.legalNotice.empty() ? 0 : 1, e.licence.c_str(),
                    noAckText.c_str(), ackText.c_str());
    }
}

void Engine::reportPluginStatus(const std::vector<cascade::core::HostImage>& images, const PluginUiReport& ui) {
    // Bounded-run diagnostic only (CASCADE_PLUGIN_STATUS). It exists because
    // "the plugin is disabled" is a claim about what is MAPPED in this
    // process, and a screenshot of a red row proves nothing about that. Every
    // number here is read from the live objects the GUI draws from.
    const std::vector<cascade::core::LoadedPlugin>& list = pluginHost_.plugins();
    const std::size_t blocked = cascade::core::PluginRepo::blockedCount(
        pluginInventory_.plugins, pluginInventory_.policies);
    std::printf("plugin status: dir=%s candidates=%d loaded=%d blocked=%d managed=%d "
                "unmanaged=%d\n",
                pluginDir_.c_str(), static_cast<int>(list.size()),
                static_cast<int>(pluginHost_.loadedCount()), static_cast<int>(blocked),
                static_cast<int>(pluginInventory_.plugins.size()),
                static_cast<int>(pluginInventory_.unmanaged.size()));
    if (!statusText().pluginEnforceError.empty()) {
        std::printf("plugin enforce: %s\n", statusText().pluginEnforceError.c_str());
    }
    for (const std::string& note : pluginInventory_.notes) {
        std::printf("plugin inventory: %s\n", note.c_str());
    }
    for (const cascade::core::LoadedPlugin& p : list) {
        const std::string file = std::filesystem::path(p.path).filename().string();
        std::printf("plugin host: file=%s loaded=%d name=%s version=%s error=%s\n",
                    file.c_str(), p.loaded ? 1 : 0, p.name.c_str(), p.version.c_str(),
                    p.error.empty() ? "-" : p.error.c_str());
    }

    // The SOURCE, because a decoder starved of samples is indistinguishable
    // from a decoder that decoded nothing, and sourceError_ is otherwise only
    // ever shown in the GUI - invisible to exactly the headless runs used to
    // verify decoding.
    // The device rate is printed to six decimals ON PURPOSE. A hardware rate
    // readback is a double the tuner computed from a master clock divider, and
    // it is very often NOT the round number that was asked for. The chain
    // requires an integer channel rate, so 2400000.000001 is refused while
    // "2400000" appears in every log - which reads as a nonsensical refusal of
    // a rate that is obviously fine.
    std::printf("source status: kind=%s name=%s deviceRate=%.6f chainRate=%.6f "
                "centre=%.0f running=%d error=%s\n",
                sourceKind_.c_str(), pipeline_.activeSourceName(),
                pipeline_.activeSource().sampleRateHz(), pipeline_.inputRateHz(),
                pipeline_.activeSource().centerFrequencyHz(),
                pipeline_.running() ? 1 : 0,
                statusText().sourceError.empty() ? "-" : statusText().sourceError.c_str());

    // The RUNNER, reported separately from the host, because "loaded" and
    // "being fed real audio" are different claims and the whole point of this
    // subsystem is the second one. A screenshot of a loaded plugin proves
    // nothing about whether a single sample ever reached it.
    // The UI capabilities, reported like the decoders: "the plugin is loaded"
    // and "the plugin put something on the map" are different claims.
    // tuneGranted is printed beside tuneRequests because the pair is the whole
    // permission story: a request with no grant is a refusal the user can undo,
    // and a grant with no request is a permission nothing is using.
    std::printf("plugin ui: tracks=%d paths=%d panels=%d tuneRequests=%d tuneGranted=%d\n",
                static_cast<int>(ui.tracks.size()),
                static_cast<int>(ui.paths),
                static_cast<int>(ui.panels.size()),
                static_cast<int>(pluginApi_->tuneRequesterCount()),
                static_cast<int>(pluginTuneAllowed_.size()));
    for (const cascade::core::HostPanel& p : ui.panels) {
        std::printf("plugin panel: name=%s title=\"%s\" columns=%d rows=%d\n",
                    p.plugin.c_str(), p.title.c_str(), static_cast<int>(p.headings.size()),
                    static_cast<int>(p.rows.size()));
    }
    for (const cascade::core::HostImage& im : images) {
        std::printf("plugin image: from=%s %ux%u fmt=%u complete=%d seq=%llu bytes=%zu\n",
                    im.plugin.c_str(), im.width, im.height, im.format, im.complete ? 1 : 0,
                    static_cast<unsigned long long>(im.sequence), im.pixels.size());
    }
    for (const cascade::core::HostTrack& t : ui.tracks) {
        std::printf("plugin track: from=%s id=%s label=%s pos=%.4f,%.4f kind=%u\n",
                    t.plugin.c_str(), t.t.id, t.t.label, t.t.latDeg, t.t.lonDeg, t.t.kind);
    }

    // WHICH PLUGINS ARE STOPPED, printed even when the list is empty (as
    // "stopped=0"), because "no line" and "nothing stopped" have to be
    // distinguishable in a bounded run's output - the same rule the counts
    // above follow.
    // (0.99.73: the plugin's ID, not its file name - a stop outlives an update.)
    {
        const std::vector<std::string> stoppedIds = stoppedPluginIds();
        std::printf("plugin stopped: count=%d\n", static_cast<int>(stoppedIds.size()));
        for (const std::string& k : stoppedIds) {
            std::printf("plugin stopped: file=%s\n", k.c_str());
        }
        // HOW EVERY OTHER PLUGIN RUNS: pinned ("always") ones, and which plugins are dormant right now. Printed in every run, including
        // "none", for the reason the counts above are.
        int always = 0;
        for (const auto& [id, mode] : pluginRun_) {
            if (mode == cascade::core::PluginRun::Always) { ++always; }
        }
        int idle = 0;
        for (const cascade::core::LoadedPlugin& p : pluginHost_.plugins()) {
            if (p.loaded && pluginIsIdle(cascade::core::pluginKey(p))) { ++idle; }
        }
        std::printf("plugin run: always=%d idle=%d\n", always, idle);
    }
    // THE SNAPSHOT THE MUTE DECISION IS TAKEN FROM, one line per loaded
    // plugin. `mutes` is the setting the checkbox shows - the capability
    // default with the user's override applied - and `running` is whether the
    // plugin has any claim on the audio at all, which is where "not stopped"
    // was once wrongly good enough. Printed because the alternative way to
    // find out which plugins mute by default is to read five checkboxes off a
    // screenshot, and a README written from a screenshot is how the shipped
    // documentation came to name two plugins that do not consume I/Q.
    for (const cascade::core::MutePlugin& m : muteStates_) {
        std::printf("plugin mute: file=%s name=%s running=%d mutes=%d presets=%d\n",
                    m.key.c_str(), m.name.c_str(), m.running ? 1 : 0, m.mutes ? 1 : 0,
                    static_cast<int>(m.presets.size()));
    }
    // THE MUTE, printed every run including "not muted", for the same reason
    // the counts above are: in a bounded run "no line" and "nothing is muting"
    // must not look the same.
    std::printf("audio mute: muted=%d by=%s\n",
                pipeline_.audioMuted() ? 1 : 0, muteSubjectText().c_str());
    std::printf("plugin runner: active=%d lines=%d audioFed=%llu iqFed=%llu\n",
                static_cast<int>(pluginRunner_.activeCount()),
                static_cast<int>(decoderLog_.size()),
                static_cast<unsigned long long>(pluginRunner_.audioFramesFed()),
                static_cast<unsigned long long>(pluginRunner_.iqFramesFed()));
    for (const cascade::core::DecoderStatus& s : pluginRunner_.status()) {
        std::printf("plugin decode: name=%s state=%s%s\n", s.plugin.c_str(),
                    s.reason == cascade::core::DecoderIdleReason::Running ? "running"
                                                                          : "idle",
                    s.reason == cascade::core::DecoderIdleReason::Running
                        ? ""
                        : (" reason=" + s.detail).c_str());
    }
    // The last few decoded lines: the only evidence that the chain from
    // antenna to plugin text actually closed.
    std::size_t shown = 0;
    for (auto it = decoderLog_.rbegin(); it != decoderLog_.rend() && shown < 5; ++it, ++shown) {
        std::printf("plugin text: [%s] %s\n", it->plugin.c_str(), it->text.c_str());
    }
    for (const cascade::core::BlockedPlugin& b : pluginBlocked_) {
        // mapped=1 would mean enforcement failed: the host would have a record
        // of a file it was supposed never to see.
        int mapped = 0;
        for (const cascade::core::LoadedPlugin& p : list) {
            const std::string file = std::filesystem::path(p.path).filename().string();
            if (cascade::gui::equalsFileNameAscii(file, b.installed.file)) { mapped = 1; }
        }
        const char* reason =
            b.reason == cascade::core::PluginBlockReason::AbiMismatch ? "abi-mismatch"
                                                                      : "below-minimum-version";
        std::printf("plugin blocked: id=%s file=%s version=%s floor=%s catalogue=%s "
                    "reason=%s mapped=%d message=\"%s\"\n",
                    b.installed.id.c_str(), b.installed.file.c_str(),
                    b.installed.version.c_str(),
                    b.policy.minSupportedVersion.empty() ? "-"
                                                         : b.policy.minSupportedVersion.c_str(),
                    b.policy.catalogueVersion.empty() ? "-" : b.policy.catalogueVersion.c_str(),
                    reason, mapped, b.message.c_str());
    }
    for (const cascade::core::PluginUpdate& u : plannedPluginUpdates()) {
        std::printf("plugin update: id=%s from=%s to=%s\n", u.id.c_str(),
                    u.fromVersion.c_str(), u.toVersion.c_str());
    }
}

}  // namespace cascade::engine
