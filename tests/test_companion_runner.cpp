// test_companion_runner.cpp - THE CHECK BEFORE A COMPANION RUNS, at the point where it matters: PluginRunner makes no instance of a catalogue companion's bridge unless the
// bridge's separate program is the file that was installed (core/companion_programs.hpp checkBeforeStart, called by PluginRunner::buildPluginLocked). No DLL is loaded: a fake
// decoder table counts what the runner did.
//
//   R1  the right program: the instance is made and the row says it is running
//   R2  a program swapped after install: NO instance, a plain sentence on the plugin's row, the refusal counted on the companion fault path; put back, it starts
//   R3  a program deleted, truncated, recorded with no checksum: no instance each time
//   R4  a plugin with no companion record is not asked anything (a program the user chose is theirs)
//   R5  startPlugin (a woken plugin) goes through the same check as rebuild
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/companion_programs.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
#include "core/plugin_runner.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace cp = cascade::core::companion;
using cascade::core::DecoderIdleReason;
using cascade::core::DecoderStatus;
using cascade::core::LoadedPlugin;
using cascade::core::PluginRepo;
using cascade::core::PluginRunner;

namespace {

int g_created = 0;
void* fakeCreate(uint32_t) {
    ++g_created;
    return &g_created;
}
void fakeProcess(void*, const float*, size_t) {}
int32_t fakePoll(void*, char*, size_t) { return 0; }
void fakeDestroy(void*) {}

CascadeDecoderApi makeApi() {
    CascadeDecoderApi a{};
    a.structSize = static_cast<uint32_t>(sizeof(CascadeDecoderApi));
    a.requiredRateHz = 0;
    a.create = &fakeCreate;
    a.process = &fakeProcess;
    a.poll_text = &fakePoll;
    a.destroy = &fakeDestroy;
    return a;
}

void put(const fs::path& p, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string sha(const std::string& bytes) {
    std::string hex, err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

DecoderStatus rowOf(const PluginRunner& r, const std::string& name) {
    for (const DecoderStatus& s : r.status()) {
        if (s.plugin == name) { return s; }
    }
    return DecoderStatus{};
}

std::uint64_t breaks() {
    std::array<std::uint64_t, 4> sums{};
    cp::takeHostFaults(sums);
    return sums[2];
}

}  // namespace

int main() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const fs::path dir = fs::temp_directory_path() / ("foxsdr_companion_runner_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    cp::setPackagedForTest(0);

    const std::string bridge = "companion-x-1.0.0-abi3-win-x64.dll";
    const std::string program = "the program as installed";
    put(dir / bridge, "stand-in bridge");
    put(dir / "helpers" / "x-helper.exe", program);
    cp::Record rec;
    rec.plugin = bridge;
    rec.id = "companion-x";
    rec.file = "x-helper.exe";
    rec.sha256 = sha(program);
    rec.size = program.size();
    rec.via = "download";
    std::string err;
    CHECK(cp::recordProgram(dir.string(), rec, err));
    (void)breaks();

    const CascadeDecoderApi api = makeApi();
    LoadedPlugin lp;
    lp.loaded = true;
    lp.name = "Companion X";
    lp.version = "1.0.0";
    lp.decoder = &api;
    lp.path = (dir / bridge).string();

    // R1
    std::printf("[R1] the right program\n");
    {
        g_created = 0;
        PluginRunner r;
        r.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
        CHECK(g_created == 1);
        CHECK(rowOf(r, "Companion X").reason == DecoderIdleReason::Running);
        CHECK(breaks() == 0);
    }
    // R2
    std::printf("[R2] swapped after install\n");
    {
        put(dir / "helpers" / "x-helper.exe", "a different program");
        g_created = 0;
        PluginRunner r;
        r.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
        CHECK(g_created == 0);   // NO INSTANCE: create() was never called, so the bridge never started the program
        const DecoderStatus st = rowOf(r, "Companion X");
        CHECK(st.reason == DecoderIdleReason::CreateFailed);
        CHECK(st.detail.find("did not start") != std::string::npos && st.detail.find("changed on disk") != std::string::npos && st.detail.find("x-helper.exe") != std::string::npos);
        CHECK(r.activeCount() == 0u);
        CHECK(breaks() == 1);
        // put back: it starts
        put(dir / "helpers" / "x-helper.exe", program);
        PluginRunner r2;
        r2.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
        CHECK(g_created == 1 && rowOf(r2, "Companion X").reason == DecoderIdleReason::Running);
    }
    // R3
    std::printf("[R3] gone, truncated, no checksum\n");
    {
        g_created = 0;
        fs::remove(dir / "helpers" / "x-helper.exe");
        {
            PluginRunner r;
            r.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
            CHECK(g_created == 0 && rowOf(r, "Companion X").detail.find("not where it was installed") != std::string::npos);
        }
        put(dir / "helpers" / "x-helper.exe", "");
        {
            PluginRunner r;
            r.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
            CHECK(g_created == 0);
        }
        put(dir / "helpers" / "x-helper.exe", program);
        cp::Record none = rec;
        none.sha256 = "";
        CHECK(cp::recordProgram(dir.string(), none, err));
        {
            PluginRunner r;
            r.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
            CHECK(g_created == 0 && rowOf(r, "Companion X").detail.find("no checksum") != std::string::npos);
        }
        CHECK(cp::recordProgram(dir.string(), rec, err));
        CHECK(breaks() == 3);
    }
    // R4
    std::printf("[R4] a plugin with no record\n");
    {
        g_created = 0;
        LoadedPlugin other = lp;
        other.name = "Not a companion";
        other.path = (dir / "other-1.0.0-abi3-win-x64.dll").string();
        put(dir / "helpers" / "x-helper.exe", "whatever");   // even a program that matches nothing: this plugin has no record
        PluginRunner r;
        r.rebuild({other}, 48000.0, 2400000.0, 100000000.0);
        CHECK(g_created == 1 && rowOf(r, "Not a companion").reason == DecoderIdleReason::Running);
        CHECK(breaks() == 0);
        put(dir / "helpers" / "x-helper.exe", program);
    }
    // R5
    std::printf("[R5] a woken plugin goes through the same check\n");
    {
        g_created = 0;
        PluginRunner r;
        r.rebuild({lp}, 48000.0, 2400000.0, 100000000.0);
        CHECK(g_created == 1);
        r.stopPlugin(cascade::core::pluginKey(lp));
        put(dir / "helpers" / "x-helper.exe", "swapped while it slept");
        const int before = g_created;
        (void)r.startPlugin(lp);
        CHECK(g_created == before);   // refused: no new instance
        CHECK(breaks() == 1);
        put(dir / "helpers" / "x-helper.exe", program);
        (void)r.startPlugin(lp);
        CHECK(g_created == before + 1);
    }
    cp::setPackagedForTest(-1);
    fs::remove_all(dir, ec);
    return testSummary("test_companion_runner");
}
