// Tests for core/plugin_host.{hpp,cpp} and the C ABI in core/plugin_abi.h.
//
// TEST STRATEGY, and the one honest gap.
//
// The load path splits into two halves that need different techniques:
//
//  1. The FILESYSTEM AND LOADER half - missing directory, empty directory,
//     non-DLL files, a file that is not a valid module at all, a valid module
//     that is not a plugin - is exercised for real: the test writes actual
//     files into a temp directory and lets PluginHost::scan run LoadLibrary
//     against them. The "valid module, no entry point" case uses a COPY of a
//     real system DLL, so GetProcAddress genuinely fails on a genuinely
//     loaded module rather than on a simulation of one.
//
//  2. The DESCRIPTOR VALIDATION half is tested through validatePluginDesc,
//     the pure function the loader delegates to. GAP, stated plainly: this
//     test cannot compile a DLL (no compiler is guaranteed at ctest time, and
//     the tests may not touch CMakeLists.txt), so the version-mismatch and
//     missing-function-pointer refusals are proven against the validator
//     rather than against a real mismatched module. The validator is the
//     whole policy - loadOne() calls it and does nothing else with the
//     descriptor - so what is untested is only the two lines that hand the
//     descriptor over, plus the SEH guard around the entry-point call, which
//     needs a deliberately faulty binary to trigger.
//
//     That gap is closable on demand: examples/example_plugin/ documents how
//     to build the sample plugin by hand, twice, the second time with
//     /DCASCADE_EXAMPLE_FORCE_ABI=999. If the environment variables
//     CASCADE_TEST_PLUGIN_OK_DIR and CASCADE_TEST_PLUGIN_BAD_DIR point at
//     directories holding those two DLLs, the final section of this test
//     loads them for real and asserts the accept/refuse outcomes. Unset (the
//     CI default) those checks are skipped with a printed note.
//
// Temp policy follows test_recorder.cpp: per-case directories named with the
// process id, removed on success, left behind on failure for autopsy.
//
// SPDX-License-Identifier: MIT
#include "core/plugin_host.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;

using cascade::core::LoadedPlugin;
using cascade::core::PluginHost;
using cascade::core::PluginRejection;

namespace {

// ---------------------------------------------------------------------------
// Temp directory helpers
// ---------------------------------------------------------------------------

fs::path tmpDir(const char* tag) {
    fs::path d = fs::path("plugin_host_" + std::to_string(TEST_GETPID()) + "_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void writeFile(const fs::path& p, const char* bytes, std::size_t n) {
    std::FILE* f = std::fopen(p.string().c_str(), "wb");
    if (f == nullptr) {
        return;
    }
    std::fwrite(bytes, 1, n, f);
    std::fclose(f);
}

// ---------------------------------------------------------------------------
// Descriptor fixtures for the pure validator
// ---------------------------------------------------------------------------
//
// The stubs exist purely to be non-null function pointers; the validator only
// checks that they are there. They are never called.

void* stubCreate(uint32_t) { return nullptr; }
void stubProcess(void*, const float*, size_t) {}
int32_t stubPoll(void*, char*, size_t) { return 0; }
void stubDestroy(void*) {}

CascadeDecoderApi validDecoder() {
    CascadeDecoderApi d{};
    d.structSize = static_cast<uint32_t>(sizeof(CascadeDecoderApi));
    d.requiredRateHz = 8000u;
    d.create = &stubCreate;
    d.process = &stubProcess;
    d.poll_text = &stubPoll;
    d.destroy = &stubDestroy;
    return d;
}

// Every case below starts from this and breaks exactly one thing, so a
// failure names the field that matters instead of a whole struct.
CascadePluginDesc validDesc(const CascadeDecoderApi* dec) {
    CascadePluginDesc p{};
    p.structSize = static_cast<uint32_t>(sizeof(CascadePluginDesc));
    p.abiVersion = static_cast<uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    p.name = "Fixture";
    p.version = "1.0.0";
    p.author = "tests";
    p.licence = "MIT";
    p.capabilities = CASCADE_CAP_DECODER;
    p.reserved = 0u;
    p.decoder = dec;
    return p;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 1. Pure descriptor validation - exhaustive
// ---------------------------------------------------------------------------

void testValidation() {
    const CascadeDecoderApi dec = validDecoder();

    // Baseline: the fixture itself must be acceptable, otherwise every
    // "rejected" result below would be meaningless.
    {
        const CascadePluginDesc p = validDesc(&dec);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }

    // A null descriptor is what a plugin returns when it declines the host.
    CHECK(cascade::core::validatePluginDesc(nullptr) == PluginRejection::NullDescriptor);

    // --- ABI version: EXACT match, nothing else. Older, newer and absurd all
    // fail identically; there is no "compatible enough".
    for (uint32_t v : {0u, static_cast<uint32_t>(CASCADE_PLUGIN_ABI_VERSION) + 1u, 999u,
                       0xFFFFFFFFu}) {
        CascadePluginDesc p = validDesc(&dec);
        p.abiVersion = v;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::AbiVersionMismatch);
    }
    if constexpr (CASCADE_PLUGIN_ABI_VERSION > 0) {
        CascadePluginDesc p = validDesc(&dec);
        p.abiVersion = static_cast<uint32_t>(CASCADE_PLUGIN_ABI_VERSION) - 1u;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::AbiVersionMismatch);
    }

    // --- Struct size: the second guard. Both directions, because layout
    // drift can add or remove bytes.
    for (uint32_t s : {0u, 4u, static_cast<uint32_t>(sizeof(CascadePluginDesc)) - 4u,
                       static_cast<uint32_t>(sizeof(CascadePluginDesc)) + 4u}) {
        CascadePluginDesc p = validDesc(&dec);
        p.structSize = s;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::DescStructSizeMismatch);
    }

    // Version is checked BEFORE size: a plugin that got both wrong must be
    // reported as the version mismatch, since size cannot be interpreted
    // until the version establishes what the layout should be.
    {
        CascadePluginDesc p = validDesc(&dec);
        p.abiVersion = 42u;
        p.structSize = 12u;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::AbiVersionMismatch);
    }

    // --- Reserved padding must be zero.
    {
        CascadePluginDesc p = validDesc(&dec);
        p.reserved = 1u;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::ReservedNotZero);
    }

    // --- Required strings. Null and empty are both refused for name,
    // version and licence; author may be empty but not null.
    {
        CascadePluginDesc p = validDesc(&dec);
        p.name = nullptr;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingName);
        p.name = "";
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingName);
    }
    {
        CascadePluginDesc p = validDesc(&dec);
        p.version = nullptr;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingVersion);
        p.version = "";
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingVersion);
    }
    {
        CascadePluginDesc p = validDesc(&dec);
        p.author = nullptr;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingAuthor);
        p.author = "";  // anonymous is allowed; unstated is not
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }
    {
        // The licence rule is a product requirement, not a technical one: the
        // host must always be able to tell the user what it loaded.
        CascadePluginDesc p = validDesc(&dec);
        p.licence = nullptr;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingLicence);
        p.licence = "";
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingLicence);
        p.licence = "GPL-3.0-only";  // a GPL plugin is fine - it just has to say so
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }

    // --- Capabilities.
    {
        CascadePluginDesc p = validDesc(&dec);
        p.capabilities = 0u;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::NoCapabilities);
    }
    for (uint32_t caps : {0x2u, 0x80000000u, CASCADE_CAP_DECODER | 0x4u}) {
        CascadePluginDesc p = validDesc(&dec);
        p.capabilities = caps;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::UnknownCapability);
    }

    // --- Decoder table.
    {
        CascadePluginDesc p = validDesc(nullptr);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingDecoderApi);
    }
    for (uint32_t s : {0u, static_cast<uint32_t>(sizeof(CascadeDecoderApi)) + 8u}) {
        CascadeDecoderApi bad = validDecoder();
        bad.structSize = s;
        CascadePluginDesc p = validDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::DecoderStructSizeMismatch);
    }
    for (uint32_t rate : {1u, 999u, 1000001u, 48000000u}) {
        CascadeDecoderApi bad = validDecoder();
        bad.requiredRateHz = rate;
        CascadePluginDesc p = validDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::DecoderRateOutOfRange);
    }
    for (uint32_t rate : {0u, 1000u, 8000u, 48000u, 1000000u}) {
        CascadeDecoderApi ok = validDecoder();
        ok.requiredRateHz = rate;  // 0 means "any rate"
        CascadePluginDesc p = validDesc(&ok);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }
    // Each of the four callbacks individually: a table missing any one of
    // them is unusable, and finding that out at load time beats finding it
    // out from a null-pointer call on the audio thread.
    {
        CascadeDecoderApi bad = validDecoder();
        bad.create = nullptr;
        CascadePluginDesc p = validDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingDecoderFunction);
    }
    {
        CascadeDecoderApi bad = validDecoder();
        bad.process = nullptr;
        CascadePluginDesc p = validDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingDecoderFunction);
    }
    {
        CascadeDecoderApi bad = validDecoder();
        bad.poll_text = nullptr;
        CascadePluginDesc p = validDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingDecoderFunction);
    }
    {
        CascadeDecoderApi bad = validDecoder();
        bad.destroy = nullptr;
        CascadePluginDesc p = validDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingDecoderFunction);
    }
}

// Messages must be usable in a support conversation: never empty, and
// carrying the actual numbers where a number is the whole story.
void testRejectionMessages() {
    const CascadeDecoderApi dec = validDecoder();
    for (int i = 0; i <= static_cast<int>(PluginRejection::MissingDecoderFunction); ++i) {
        const char* m = cascade::core::pluginRejectionMessage(static_cast<PluginRejection>(i));
        CHECK(m != nullptr && m[0] != '\0');
    }

    CascadePluginDesc p = validDesc(&dec);
    p.abiVersion = 999u;
    const std::string s =
        cascade::core::describePluginRejection(PluginRejection::AbiVersionMismatch, &p);
    CHECK(contains(s, "999"));
    CHECK(contains(s, std::to_string(CASCADE_PLUGIN_ABI_VERSION).c_str()));

    // Callable with a null descriptor (the NullDescriptor path) without
    // dereferencing it.
    CHECK(!cascade::core::describePluginRejection(PluginRejection::NullDescriptor, nullptr).empty());
}

// ---------------------------------------------------------------------------
// 2. Filename filtering
// ---------------------------------------------------------------------------

void testExtensionFilter() {
#ifdef _WIN32
    CHECK(PluginHost::hasPluginExtension("a.dll"));
    CHECK(PluginHost::hasPluginExtension("MyPlugin.DLL"));  // NTFS is case-insensitive
    CHECK(PluginHost::hasPluginExtension("x.y.Dll"));
    CHECK(!PluginHost::hasPluginExtension(".dll"));  // extension with no name
    CHECK(!PluginHost::hasPluginExtension("a.dll.txt"));
    CHECK(!PluginHost::hasPluginExtension("a.so"));
#else
    CHECK(PluginHost::hasPluginExtension("a.so"));
    CHECK(!PluginHost::hasPluginExtension("a.dll"));
#endif
    CHECK(!PluginHost::hasPluginExtension(""));
    CHECK(!PluginHost::hasPluginExtension("readme.txt"));
    CHECK(!PluginHost::hasPluginExtension("plugin"));
}

// ---------------------------------------------------------------------------
// 3. Scanning - real directories, real LoadLibrary
// ---------------------------------------------------------------------------

void testScanMissingDirectory() {
    PluginHost host;
    const fs::path missing =
        fs::path("plugin_host_" + std::to_string(TEST_GETPID()) + "_does_not_exist");
    std::error_code ec;
    fs::remove_all(missing, ec);

    host.scan(missing.string());
    // Clean and empty: a user with no plugins directory has done nothing
    // wrong, so there is nothing to report.
    CHECK(host.plugins().empty());
    CHECK(host.loadedCount() == 0);
    CHECK(host.directory() == missing.string());

    // A path that exists but is a FILE, not a directory, is the same story.
    const fs::path f = fs::path("plugin_host_" + std::to_string(TEST_GETPID()) + "_file");
    writeFile(f, "x", 1);
    host.scan(f.string());
    CHECK(host.plugins().empty());
    fs::remove(f, ec);
}

void testScanEmptyDirectory() {
    const fs::path d = tmpDir("empty");
    PluginHost host;
    host.scan(d.string());
    CHECK(host.plugins().empty());
    CHECK(host.loadedCount() == 0);

    // Files that are not modules at all are filtered before any load is
    // attempted - a README next to the plugins is not a failed plugin.
    writeFile(d / "README.txt", "hello", 5);
    writeFile(d / "config.json", "{}", 2);
    host.scan(d.string());
    CHECK(host.plugins().empty());

    std::error_code ec;
    fs::remove_all(d, ec);
}

void testScanGarbageModule() {
    const fs::path d = tmpDir("garbage");
    // Not a PE image: LoadLibrary must fail, and the host must record that
    // with a reason rather than crashing or skipping silently.
    const char junk[] = "this is not a portable executable";
    writeFile(d / "broken.dll", junk, sizeof(junk) - 1);

    PluginHost host;
    host.scan(d.string());
    CHECK(host.plugins().size() == 1);
    CHECK(host.loadedCount() == 0);
    if (host.plugins().size() == 1) {
        const LoadedPlugin& rec = host.plugins()[0];
        CHECK(!rec.loaded);
        CHECK(!rec.error.empty());
        CHECK(contains(rec.path, "broken.dll"));
        CHECK(rec.name.empty());          // nothing was believed about it
        CHECK(rec.nativeHandle == nullptr);
        CHECK(rec.decoder == nullptr);
        std::printf("  garbage module reason: %s\n", rec.error.c_str());
    }

    std::error_code ec;
    fs::remove_all(d, ec);
}

// A real, loadable module that simply is not a cascade plugin. Copying a
// system DLL is what makes this a genuine GetProcAddress failure on a genuine
// HMODULE instead of a stand-in.
void testScanRealModuleWithoutEntryPoint() {
#ifdef _WIN32
    const char* candidates[] = {
        "C:/Windows/System32/msimg32.dll",
        "C:/Windows/System32/version.dll",
        "C:/Windows/System32/winmm.dll",
    };
    fs::path donor;
    for (const char* c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) {
            donor = c;
            break;
        }
    }
    if (donor.empty()) {
        std::printf("  SKIP: no donor system DLL found for the no-entry-point case\n");
        return;
    }

    const fs::path d = tmpDir("noentry");
    std::error_code ec;
    // Renamed so the loader cannot satisfy it from the KnownDLLs cache: the
    // file on disk is what must be loaded.
    //
    // Hard link first, copy as a fallback. A link publishes no new file
    // content, so an on-access antivirus has nothing new to scan; a fresh
    // copy of a system DLL into a build directory is precisely the pattern
    // that makes a scanner stop and think, and that cost showed up as
    // multi-second (once multi-minute) test runs on this machine.
    fs::create_hard_link(donor, d / "notaplugin.dll", ec);
    if (ec) {
        ec.clear();
        fs::copy_file(donor, d / "notaplugin.dll", fs::copy_options::overwrite_existing, ec);
    }
    if (ec) {
        std::printf("  SKIP: could not stage donor DLL (%s)\n", ec.message().c_str());
        fs::remove_all(d, ec);
        return;
    }

    PluginHost host;
    host.scan(d.string());
    CHECK(host.plugins().size() == 1);
    CHECK(host.loadedCount() == 0);
    if (host.plugins().size() == 1) {
        const LoadedPlugin& rec = host.plugins()[0];
        CHECK(!rec.loaded);
        CHECK(contains(rec.error, "cascade_plugin_query"));
        CHECK(rec.nativeHandle == nullptr);  // unmapped again immediately
        std::printf("  no-entry-point reason: %s\n", rec.error.c_str());
    }

    // The module must be unmapped, or this delete fails - which is exactly
    // the assertion worth making: a refused plugin does not stay resident.
    host.unloadAll();
    fs::remove_all(d, ec);
    CHECK(!ec);
    CHECK(!fs::exists(d));
#else
    std::printf("  SKIP: no-entry-point case is Windows-specific here\n");
#endif
}

// Ordering must not depend on the filesystem's iteration order.
void testDeterministicOrder() {
    const fs::path d = tmpDir("order");
    const char junk[] = "nope";
    writeFile(d / "zulu.dll", junk, sizeof(junk) - 1);
    writeFile(d / "alpha.dll", junk, sizeof(junk) - 1);
    writeFile(d / "mike.dll", junk, sizeof(junk) - 1);

    PluginHost host;
    host.scan(d.string());
    CHECK(host.plugins().size() == 3);
    if (host.plugins().size() == 3) {
        CHECK(contains(host.plugins()[0].path, "alpha.dll"));
        CHECK(contains(host.plugins()[1].path, "mike.dll"));
        CHECK(contains(host.plugins()[2].path, "zulu.dll"));
        // Every candidate produced a record with a reason; none vanished.
        for (const LoadedPlugin& rec : host.plugins()) {
            CHECK(!rec.error.empty());
        }
    }

    std::error_code ec;
    fs::remove_all(d, ec);
}

void testUnloadAllIdempotent() {
    PluginHost host;
    // Before any scan.
    host.unloadAll();
    host.unloadAll();
    CHECK(host.plugins().empty());

    const fs::path d = tmpDir("unload");
    const char junk[] = "nope";
    writeFile(d / "one.dll", junk, sizeof(junk) - 1);
    host.scan(d.string());
    CHECK(host.plugins().size() == 1);

    host.unloadAll();
    CHECK(host.plugins().empty());
    CHECK(host.loadedCount() == 0);
    host.unloadAll();  // second call must be a no-op, not a double free
    host.unloadAll();
    CHECK(host.plugins().empty());
    CHECK(host.loadedCount() == 0);

    // A re-scan after unloading works and repopulates.
    host.scan(d.string());
    CHECK(host.plugins().size() == 1);
    // scan() itself unloads first, so scanning twice must not accumulate.
    host.scan(d.string());
    CHECK(host.plugins().size() == 1);

    std::error_code ec;
    fs::remove_all(d, ec);
}

void testDefaultDirectory() {
    const std::string d = PluginHost::defaultPluginDir();
    CHECK(!d.empty());
    CHECK(contains(d, "plugins"));
    // Next to the executable, not a bare relative path - unless the process
    // path could not be determined at all, which cannot happen for a normal
    // test binary.
    CHECK(fs::path(d).is_absolute());
    std::printf("  default plugin dir: %s\n", d.c_str());

    // Scanning it must be harmless whether or not it exists.
    PluginHost host;
    host.scanDefault();
    CHECK(host.directory() == d);
}

// ---------------------------------------------------------------------------
// 4. Optional: real plugin DLLs, if the operator built them (see the header
//    comment). These close the documented gap when they run.
// ---------------------------------------------------------------------------

void testRealPluginDirs() {
    const char* okDir = std::getenv("CASCADE_TEST_PLUGIN_OK_DIR");
    const char* badDir = std::getenv("CASCADE_TEST_PLUGIN_BAD_DIR");
    if (okDir == nullptr && badDir == nullptr) {
        std::printf(
            "  SKIP: real-DLL checks (set CASCADE_TEST_PLUGIN_OK_DIR /\n"
            "        CASCADE_TEST_PLUGIN_BAD_DIR to directories holding the\n"
            "        example plugin built per examples/example_plugin/)\n");
        return;
    }

    if (okDir != nullptr) {
        PluginHost host;
        host.scan(okDir);
        CHECK(host.plugins().size() >= 1);
        CHECK(host.loadedCount() >= 1);
        for (const LoadedPlugin& rec : host.plugins()) {
            std::printf("  OK-dir: %s loaded=%d name='%s' v='%s' licence='%s' err='%s'\n",
                        rec.path.c_str(), rec.loaded ? 1 : 0, rec.name.c_str(),
                        rec.version.c_str(), rec.licence.c_str(), rec.error.c_str());
            if (rec.loaded) {
                CHECK(rec.error.empty());
                CHECK(!rec.name.empty());
                CHECK(!rec.licence.empty());
                CHECK((rec.capabilities & CASCADE_CAP_DECODER) != 0u);
                CHECK(rec.decoder != nullptr);
                CHECK(rec.nativeHandle != nullptr);
                // Drive the decoder end to end through the ABI: create,
                // feed a second of full-scale audio, collect the text.
                if (rec.decoder != nullptr) {
                    const uint32_t rate =
                        rec.decoder->requiredRateHz != 0u ? rec.decoder->requiredRateHz : 8000u;
                    void* inst = rec.decoder->create(rate);
                    CHECK(inst != nullptr);
                    if (inst != nullptr) {
                        std::vector<float> audio(rate, 0.5f);
                        rec.decoder->process(inst, audio.data(), audio.size());
                        char buf[256];
                        const int32_t n = rec.decoder->poll_text(inst, buf, sizeof(buf));
                        CHECK(n > 0);
                        if (n > 0) {
                            std::printf("  decoder said: %.*s", static_cast<int>(n), buf);
                        }
                        rec.decoder->destroy(inst);
                    }
                }
            }
        }
        host.unloadAll();
    }

    if (badDir != nullptr) {
        PluginHost host;
        host.scan(badDir);
        CHECK(host.plugins().size() >= 1);
        // The whole point: a mismatched ABI is refused, with a reason, and
        // nothing from it is loaded.
        CHECK(host.loadedCount() == 0);
        for (const LoadedPlugin& rec : host.plugins()) {
            std::printf("  BAD-dir: %s loaded=%d err='%s'\n", rec.path.c_str(),
                        rec.loaded ? 1 : 0, rec.error.c_str());
            CHECK(!rec.loaded);
            CHECK(!rec.error.empty());
        }
        host.unloadAll();
    }
}

}  // namespace

int main() {
    testValidation();
    testRejectionMessages();
    testExtensionFilter();
    testScanMissingDirectory();
    testScanEmptyDirectory();
    testScanGarbageModule();
    testScanRealModuleWithoutEntryPoint();
    testDeterministicOrder();
    testUnloadAllIdempotent();
    testDefaultDirectory();
    testRealPluginDirs();
    return testSummary("test_plugin_host");
}
