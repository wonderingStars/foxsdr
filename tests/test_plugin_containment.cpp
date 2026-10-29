// test_plugin_containment.cpp - the containment as the RUNNER, the PLUGIN UI and the clients see it (plugin containment P1; docs/engine-stage4.md
// 6.28, rules 1-3 of core/plugin_fault.hpp), with the real fault probe module:
//
//   A  EVERY GUARDED ENTRY POINT GOES THROUGH THE GUARD - a scan of src/ for a raw call of any of them outside core/plugin_calls.hpp. A call
//      site that bypasses the guard is a test failure, not a silent return to the unguarded host. (And the HOT calls - process, pull, active,
//      retune - are the ones that remain: the scan lists them, so adding a guard to one, or dropping one, is a decision a person made.)
//   B  THE RUNNER: a create() that faults leaves no instance and a status row; a poll_text() that faults on the DSP thread's own path is
//      absorbed, the instance is failed with the words "switched off", the plugin is never fed or polled again, its destroy() is NEVER
//      called (the plugin's own log is the witness), and the other plugin in the same runner is untouched.
//   C  THE PLUGIN UI: a track source whose create() or poll_tracks() faults is absorbed on the GUI thread's path; poll() returns, the
//      other plugin's targets are still collected, and the faulted one is never polled again.
//   D  THE TRACK-INFO CLIENT (the interface's cache): a get_info() that faults is absorbed and answered "missing".
//
// Windows has the structured exceptions the absorbing needs; elsewhere the faulting cases SKIP and say so.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/plugin_fault.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "gui/track_info_cache.hpp"
#include "test_check.hpp"
#include "fault_probe_names.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace fault = cascade::core::fault;
using cascade::core::DecoderIdleReason;
using cascade::core::LoadedPlugin;
using cascade::core::PluginHost;
using cascade::core::PluginRunner;
using cascade::core::PluginUi;

namespace {

fs::path g_root;
std::string g_probe;

[[maybe_unused]] void setEnvVar(const char* name, const char* value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value);
#else
    if (value == nullptr) { ::unsetenv(name); } else { ::setenv(name, value, 1); }
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

[[maybe_unused]] std::size_t count(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = 0; (at = text.find(what, at)) != std::string::npos; at += what.size()) {
        if ((at == 0 || text[at - 1] == '\n') && text.compare(at, what.size() + 1, what + "\n") == 0) { ++n; }
    }
    return n;
}

std::string leafOf(const char* id) { return std::string(id) + CASCADE_PROBE_SUFFIX; }

[[maybe_unused]] const LoadedPlugin* find(const PluginHost& host, const char* id) {
    for (const LoadedPlugin& p : host.plugins()) {
        if (cascade::core::pluginKey(p) == leafOf(id)) { return &p; }
    }
    return nullptr;
}

// ---- A ----------------------------------------------------------------------------------------------------------------------------
void testNoRawCalls() {
    const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src";
    // every entry point the guard owns, spelled as the host spells a call through a table pointer
    const char* guarded[] = {"->create(", "->destroy(", "->poll_text(", "->poll_image(", "->release_image(", "->poll_tracks(", "->poll_paths(",
                             "->poll_state(", "->poll_rows(", "->get_tile(", "->release_tile(", "->get_info(", "->release_info(",
                             "->attach(", "->press(", "preset->count(", "preset->get("};
    std::size_t files = 0;
    std::vector<std::string> offenders;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(src, ec)) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h") { continue; }
        const std::string name = e.path().filename().string();
        if (name == "plugin_calls.hpp" || name == "plugin_abi.h") { continue; }
        ++files;
        std::ifstream in(e.path());
        std::string line;
        int lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            const std::size_t cut = line.find("//");
            const std::string code = cut == std::string::npos ? line : line.substr(0, cut);
            for (const char* g : guarded) {
                if (code.find(g) != std::string::npos) { offenders.push_back(name + ":" + std::to_string(lineNo) + "  " + code); }
            }
        }
    }
    CHECK(files > 100);   // the scan really looked at the tree
    for (const std::string& o : offenders) { std::printf("FAIL a raw plugin call outside core/plugin_calls.hpp: %s\n", o.c_str()); }
    CHECK(offenders.empty());

    // The HOT calls (the signal path) are the ones that remain unguarded, named: a change to this list is a decision, not an accident.
    const std::string runner = readFile(src / "core" / "plugin_runner.cpp");
    for (const char* hot : {"->process(", "->pull(", "->active(", "->retune("}) { CHECK(runner.find(hot) != std::string::npos); }
    CHECK(runner.find("calls::pollText(") != std::string::npos);
    CHECK(runner.find("calls::create(") != std::string::npos);
    CHECK(runner.find("calls::destroy(") != std::string::npos);
}

// ---- B ----------------------------------------------------------------------------------------------------------------------------
[[maybe_unused]] void feedBlock(PluginRunner& r) {
    std::vector<float> audio(480, 0.0f);
    r.processAudio(audio.data(), audio.size());
}

void testRunner(PluginHost& host) {
#if defined(_WIN32)
    fault::resetForTest();
    const LoadedPlugin* bad = find(host, "ct-run-bad");
    const LoadedPlugin* good = find(host, "ct-run-good");
    CHECK(bad != nullptr && bad->loaded && good != nullptr && good->loaded);
    if (bad == nullptr || good == nullptr) { return; }
    const fs::path badLog = g_root / "ct-run-bad.log";
    const fs::path goodLog = g_root / "ct-run-good.log";
    fs::remove(badLog);
    fs::remove(goodLog);
    // (one process-wide log variable: the probe writes where FAULT_PROBE_LOG says at the time of each call, so it is pointed at the bad
    // one's file while the bad one is exercised and at the good one's when the good one is)
    PluginRunner runner;

    // ---- B1: create() faults
    setEnvVar("FAULT_PROBE", "create-av");
    setEnvVar("FAULT_PROBE_LOG", badLog.string().c_str());
    runner.rebuild({*bad}, 48000.0, 2400000.0, 100000000.0);
    setEnvVar("FAULT_PROBE", nullptr);
    CHECK(!runner.hasInstances(cascade::core::pluginKey(*bad)));
    CHECK(fault::isQuarantinedKey(cascade::core::pluginKey(*bad)));
    CHECK(fault::totals().faults == 1);
    {
        bool row = false;
        for (const cascade::core::DecoderStatus& st : runner.status()) {
            if (st.key == cascade::core::pluginKey(*bad)) {
                row = row || st.reason == DecoderIdleReason::CreateFailed;
            }
        }
        // (a quarantined plugin is built no more, and a plugin that faulted IN create() had its row written before the quarantine was seen)
        CHECK(row);
    }
    // built again: the guard refuses, so create() is not called a second time
    runner.rebuild({*bad}, 48000.0, 2400000.0, 100000000.0);
    CHECK(count(readFile(badLog), "create") == 1);
    feedBlock(runner);   // nothing to feed, nothing dies
    runner.clear();

    // ---- B2: poll_text() faults on the DSP path. ONE plugin faults (the probe's behaviour is chosen by a process-wide variable, so the faulting
    // case is run with the bad plugin alone; the good one is exercised in B3).
    fault::resetForTest();
    fs::remove(badLog);
    runner.rebuild({*bad}, 48000.0, 2400000.0, 100000000.0);
    CHECK(runner.hasInstances(cascade::core::pluginKey(*bad)));
    feedBlock(runner);                       // healthy
    CHECK(!fault::isQuarantinedKey(cascade::core::pluginKey(*bad)));
    setEnvVar("FAULT_PROBE", "poll-text-av");
    feedBlock(runner);                       // the DSP path: process(), then the poll - which faults here, under the runner's lock
    setEnvVar("FAULT_PROBE", nullptr);
    CHECK(fault::isQuarantinedKey(cascade::core::pluginKey(*bad)));
    CHECK(fault::quarantineEntry(fault::slotFor(cascade::core::pluginKey(*bad))) == fault::Entry::PollText);
    CHECK(fault::totals().faults == 1 && fault::totals().quarantines == 1);
    {
        bool failedRow = false;
        for (const cascade::core::DecoderStatus& st : runner.status()) {
            if (st.key == cascade::core::pluginKey(*bad) && st.reason == DecoderIdleReason::PollFailed) {
                failedRow = st.detail.find("switched off") != std::string::npos && st.detail.find("RE-ENABLE") != std::string::npos;
            }
        }
        CHECK(failedRow);
        CHECK(!runner.isFeeding(cascade::core::pluginKey(*bad)));
    }
    // ...said in the decoder log, once for each of the plugin's two failed instances
    {
        std::size_t said = 0;
        for (const cascade::core::DecodedLine& l : runner.drainText()) { said += l.text.find("switched off") != std::string::npos ? 1u : 0u; }
        if (said != 2) { std::printf("said %zu lines with 'switched off'\n", said); }
        CHECK(said == 2);   // one per failed instance: the probe declares a decoder and an image decoder, so the runner holds two
    }
    // the hot path never reaches a switched-off plugin: were process() called, the probe would kill this process
    setEnvVar("FAULT_PROBE", "process-av");
    feedBlock(runner);
    feedBlock(runner);
    setEnvVar("FAULT_PROBE", nullptr);
    // never destroyed: stopping or clearing a switched-off plugin does not call it
    fs::remove(badLog);
    setEnvVar("FAULT_PROBE_LOG", badLog.string().c_str());
    runner.stopPlugin(cascade::core::pluginKey(*bad));
    runner.clear();
    CHECK(count(readFile(badLog), "destroy") == 0);
    setEnvVar("FAULT_PROBE_LOG", nullptr);
    CHECK(!runner.hasInstances(cascade::core::pluginKey(*bad)));

    // ---- B3: a plugin that is switched off does not touch its neighbour
    fault::resetForTest();
    runner.setDormant({});   // (stopPlugin above put the bad one to sleep until startPlugin: a rebuild would skip it)
    runner.rebuild({*bad, *good}, 48000.0, 2400000.0, 100000000.0);
    feedBlock(runner);
    CHECK(runner.isFeeding(cascade::core::pluginKey(*bad)) && runner.isFeeding(cascade::core::pluginKey(*good)));
    const std::size_t fedBefore = runner.audioFramesFed();
    CHECK(fault::quarantine(fault::slotFor(cascade::core::pluginKey(*bad)), fault::Why::Fault, fault::Entry::PollText));
    feedBlock(runner);
    feedBlock(runner);
    CHECK(runner.audioFramesFed() == fedBefore + 960);   // the good plugin is still fed every block
    CHECK(runner.isFeeding(cascade::core::pluginKey(*good)));
    CHECK(!fault::isQuarantinedKey(cascade::core::pluginKey(*good)));
    runner.clear();

    // ---- B4: THE HOT LOOP DOES NOT FEED A PLUGIN THAT IS SWITCHED OFF, even when its instance has not been marked failed (the guard decided on some
    // other entry point - a window's poll, a wedge - while this instance was healthy). process() is the unguarded hot call: were it made, a fault in it
    // would end the process; the probe notes every block it is given, so "it was not fed" is read off the plugin and not off the runner.
    fault::resetForTest();
    fs::remove(badLog);
    setEnvVar("FAULT_PROBE_LOG", badLog.string().c_str());
    runner.setDormant({});
    runner.rebuild({*bad}, 48000.0, 2400000.0, 100000000.0);
    feedBlock(runner);
    const std::size_t processedBefore = count(readFile(badLog), "process");
    CHECK(processedBefore >= 1u);
    CHECK(fault::quarantine(fault::slotFor(cascade::core::pluginKey(*bad)), fault::Why::Wedge, fault::Entry::PollImage));
    feedBlock(runner);
    feedBlock(runner);
    feedBlock(runner);
    CHECK(count(readFile(badLog), "process") == processedBefore);
    setEnvVar("FAULT_PROBE_LOG", nullptr);
    runner.clear();
    (void)goodLog;
#else
    (void)host;
    SKIP_LINUX("structured exceptions: a plugin's fault cannot be absorbed off Windows");
#endif
}

// ---- C ----------------------------------------------------------------------------------------------------------------------------
void testUi(PluginHost& host) {
#if defined(_WIN32)
    fault::resetForTest();
    const LoadedPlugin* a = find(host, "ct-ui-a");
    const LoadedPlugin* b = find(host, "ct-ui-b");
    CHECK(a != nullptr && a->loaded && b != nullptr && b->loaded);
    if (a == nullptr || b == nullptr) { return; }

    // ---- C1: a track source's create() faults on the GUI thread's path: no instance, the other plugin's is built
    PluginUi ui;
    setEnvVar("FAULT_PROBE", "tracks-create-av");
    ui.rebuild({*a});
    setEnvVar("FAULT_PROBE", nullptr);
    CHECK(!ui.hasInstances(cascade::core::pluginKey(*a)));
    CHECK(fault::isQuarantinedKey(cascade::core::pluginKey(*a)));
    ui.poll();   // nothing to poll, nothing dies
    ui.rebuild({*a});   // switched off: not built again
    CHECK(!ui.hasInstances(cascade::core::pluginKey(*a)));

    // ---- C2: a poll_tracks() that faults
    fault::resetForTest();
    PluginUi ui2;
    ui2.rebuild({*b});
    CHECK(ui2.hasInstances(cascade::core::pluginKey(*b)));
    ui2.poll();                                  // healthy
    setEnvVar("FAULT_PROBE", "tracks-av");
    ui2.poll();                                  // faults inside the frame's poll: absorbed; poll() RETURNED
    setEnvVar("FAULT_PROBE", nullptr);
    CHECK(fault::isQuarantinedKey(cascade::core::pluginKey(*b)));
    CHECK(fault::quarantineEntry(fault::slotFor(cascade::core::pluginKey(*b))) == fault::Entry::PollTracks);
    const fs::path logB = g_root / "ct-ui-b.log";
    fs::remove(logB);
    setEnvVar("FAULT_PROBE_LOG", logB.string().c_str());
    ui2.poll();
    ui2.poll();
    CHECK(count(readFile(logB), "poll_tracks") == 0);   // never polled again
    ui2.stopPlugin(cascade::core::pluginKey(*b));
    ui2.clear();
    CHECK(count(readFile(logB), "tracks_destroy") == 0);   // ...and never destroyed
    setEnvVar("FAULT_PROBE_LOG", nullptr);
    CHECK(ui2.trackCount() == 0);

    // ---- D: the track-info client
    fault::resetForTest();
    const LoadedPlugin* c = find(host, "ct-ui-c");
    CHECK(c != nullptr && c->loaded && c->trackInfo != nullptr);
    if (c != nullptr && c->loaded && c->trackInfo != nullptr) {
        cascade::gui::TrackInfoCache cache;
        cache.attach(c->trackInfo, cascade::core::pluginKey(*c));
        CHECK(cache.active());
        setEnvVar("FAULT_PROBE", "info-av");
        const cascade::gui::TrackInfoCache::Info* info = cache.get("ABC123", CASCADE_TRACK_AIRCRAFT);
        setEnvVar("FAULT_PROBE", nullptr);
        (void)info;   // (a missing answer is null or an empty record; what matters is that the call RETURNED)
        CHECK(fault::isQuarantinedKey(cascade::core::pluginKey(*c)));
        CHECK(fault::quarantineEntry(fault::slotFor(cascade::core::pluginKey(*c))) == fault::Entry::GetInfo);
        cache.detach();
        // a switched-off plugin is not attached at all
        cascade::gui::TrackInfoCache again;
        again.attach(c->trackInfo, cascade::core::pluginKey(*c));
        CHECK(!again.active());
    }
#else
    (void)host;
    SKIP_LINUX("structured exceptions: a plugin's fault cannot be absorbed off Windows");
#endif
}

}  // namespace

// ---- E ----------------------------------------------------------------------------------------------------------------------------
// A SWITCHED-OFF MODULE IS NEVER UNMAPPED: its instances were leaked and a thread it started may still be inside the image. The module is read off
// the operating system (is its file still a mapped module of this process?), and a plugin that was not switched off is unmapped as ever.
void testPinned() {
#if defined(_WIN32)
    fault::resetForTest();
    const fs::path dir = g_root / "pinned";
    std::error_code ec;
    fs::create_directories(dir);
    for (const char* id : {"ct-pin-a", "ct-pin-b"}) { fs::copy_file(g_probe, dir / leafOf(id), fs::copy_options::overwrite_existing, ec); }
    PluginHost h;
    h.scan(dir.string());
    CHECK(h.loadedCount() == 2);
    CHECK(::GetModuleHandleA(leafOf("ct-pin-a").c_str()) != nullptr);
    CHECK(::GetModuleHandleA(leafOf("ct-pin-b").c_str()) != nullptr);
    CHECK(fault::quarantine(fault::slotFor(leafOf("ct-pin-a")), fault::Why::Fault, fault::Entry::PollText));
    h.unloadAll();
    CHECK(::GetModuleHandleA(leafOf("ct-pin-a").c_str()) != nullptr);   // switched off: left mapped
    CHECK(::GetModuleHandleA(leafOf("ct-pin-b").c_str()) == nullptr);   // not switched off: unmapped
    // and a scan while the quarantine stands does not map it again (the host answers with a verdict, the file is not opened)
    PluginHost again;
    again.scan(dir.string());
    const LoadedPlugin* a = find(again, "ct-pin-a");
    CHECK(a != nullptr && !a->loaded && a->verdict == "switched-off");
    fault::resetForTest();
#else
    SKIP_LINUX("module mapping is read off the Windows loader");
#endif
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: test_plugin_containment <fault probe module>\n");
        return 2;
    }
    g_probe = argv[1];
    g_root = fs::temp_directory_path() / ("foxsdr-test-plugin-containment-" + std::to_string(static_cast<unsigned long long>(
#if defined(_WIN32)
                                                                                                ::GetCurrentProcessId()
#else
                                                                                                1
#endif
                                                                                                    )));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root / "plugins");
    for (const char* id : {"ct-run-bad", "ct-run-good", "ct-ui-a", "ct-ui-b", "ct-ui-c"}) {
        fs::copy_file(g_probe, g_root / "plugins" / leafOf(id), fs::copy_options::overwrite_existing, ec);
        CHECK(!ec);
    }
    testNoRawCalls();

    PluginHost host;
    host.scan((g_root / "plugins").string());
    CHECK(host.loadedCount() == 5);
    if (host.loadedCount() != 5) {
        for (const LoadedPlugin& p : host.plugins()) { std::printf("  %s: %s%s\n", p.path.c_str(), p.loaded ? "loaded" : "NOT LOADED: ", p.error.c_str()); }
    }
    testRunner(host);
    testUi(host);

    host.unloadAll();
    testPinned();
    fs::remove_all(g_root, ec);
    return testSummary("test_plugin_containment");
}
