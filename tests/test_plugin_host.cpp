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
//     That gap is closable on demand: examples/example_plugin/ (audio) and
//     examples/example_iq_plugin/ (complex baseband, ABI 2) each document how
//     to build themselves by hand, and the audio one also documents the
//     deliberately-incompatible /DCASCADE_EXAMPLE_FORCE_ABI=1 build that a
//     version-2 host must refuse. If the environment variables
//     CASCADE_TEST_PLUGIN_OK_DIR and CASCADE_TEST_PLUGIN_BAD_DIR point at
//     directories holding those DLLs, the final section of this test loads
//     them for real, DRIVES both decoder kinds through their whole lifecycle
//     (create/process/retune/poll_text/destroy), and asserts the
//     accept/refuse outcomes. Unset (the CI default) those checks are skipped
//     with a printed note.
//
// Temp policy follows test_recorder.cpp: per-case directories named with the
// process id, removed on success, left behind on failure for autopsy.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_host.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
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

// The ABI-2 IQ table's own stubs. Note the different create signature (two
// doubles, not one uint32_t) and the extra retune - if those ever drift, this
// file stops compiling, which is the point.
void* stubIqCreate(double, double) { return nullptr; }
void stubIqProcess(void*, const float*, size_t) {}
void stubIqRetune(void*, double) {}
int32_t stubIqPoll(void*, char*, size_t) { return 0; }
void stubIqDestroy(void*) {}

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

CascadeIqDecoderApi validIqDecoder() {
    CascadeIqDecoderApi q{};
    q.structSize = static_cast<uint32_t>(sizeof(CascadeIqDecoderApi));
    q.requiredRateHz = 2400000.0;  // ADS-B territory
    q.preferredRateHz = 2400000.0;
    q.create = &stubIqCreate;
    q.process = &stubIqProcess;
    q.retune = &stubIqRetune;
    q.poll_text = &stubIqPoll;
    q.destroy = &stubIqDestroy;
    return q;
}

// Every case below starts from one of these and breaks exactly one thing, so
// a failure names the field that matters instead of a whole struct.
CascadePluginDesc descFor(uint32_t caps, const CascadeDecoderApi* dec,
                          const CascadeIqDecoderApi* iq) {
    CascadePluginDesc p{};
    p.structSize = static_cast<uint32_t>(sizeof(CascadePluginDesc));
    p.abiVersion = static_cast<uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    p.name = "Fixture";
    p.version = "1.0.0";
    p.author = "tests";
    p.licence = "MIT";
    p.capabilities = caps;
    p.reserved = 0u;
    p.decoder = dec;
    p.iqDecoder = iq;
    return p;
}

// Audio-only fixture (the ABI-1 shape, still the common case).
CascadePluginDesc validDesc(const CascadeDecoderApi* dec) {
    return descFor(CASCADE_CAP_DECODER, dec, nullptr);
}

// IQ-only fixture (new in ABI 2).
CascadePluginDesc validIqDesc(const CascadeIqDecoderApi* iq) {
    return descFor(CASCADE_CAP_IQ_DECODER, nullptr, iq);
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// 1. Pure descriptor validation - exhaustive
// ---------------------------------------------------------------------------

void testValidation() {
    const CascadeDecoderApi dec = validDecoder();
    const CascadeIqDecoderApi iq = validIqDecoder();

    // Baseline: the fixtures themselves must be acceptable, otherwise every
    // "rejected" result below would be meaningless.
    {
        const CascadePluginDesc p = validDesc(&dec);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
        const CascadePluginDesc q = validIqDesc(&iq);
        CHECK(cascade::core::validatePluginDesc(&q) == PluginRejection::None);
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
    // Unknown bits are refused even when a known bit is present. 0x2 used to
    // be an unknown bit and is CASCADE_CAP_IQ_DECODER as of ABI 2, so the
    // cases start above it - which is itself the ABI-bump rule working: the
    // legal set is frozen per version, and it grew.
    for (uint32_t caps : {0x4u, 0x80000000u, CASCADE_CAP_DECODER | 0x4u,
                          CASCADE_CAP_IQ_DECODER | 0x8u,
                          CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | 0x10u}) {
        CascadePluginDesc p = descFor(caps, &dec, &iq);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::UnknownCapability);
    }
    // All three legal combinations of the two known bits are acceptable when
    // the matching tables are present.
    {
        CascadePluginDesc audioOnly = descFor(CASCADE_CAP_DECODER, &dec, nullptr);
        CHECK(cascade::core::validatePluginDesc(&audioOnly) == PluginRejection::None);
        CascadePluginDesc iqOnly = descFor(CASCADE_CAP_IQ_DECODER, nullptr, &iq);
        CHECK(cascade::core::validatePluginDesc(&iqOnly) == PluginRejection::None);
        CascadePluginDesc both =
            descFor(CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER, &dec, &iq);
        CHECK(cascade::core::validatePluginDesc(&both) == PluginRejection::None);
    }
    // Declaring both bits but supplying only one table is refused, and the
    // reason names the table that is missing rather than the other one.
    {
        CascadePluginDesc p =
            descFor(CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER, &dec, nullptr);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingIqDecoderApi);
        CascadePluginDesc p2 =
            descFor(CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER, nullptr, &iq);
        CHECK(cascade::core::validatePluginDesc(&p2) == PluginRejection::MissingDecoderApi);
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

    // --- IQ decoder table (ABI 2). Same shape of checks as the audio table,
    // plus the two things that are genuinely different: two rate fields, and
    // one optional callback.
    {
        CascadePluginDesc p = validIqDesc(nullptr);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingIqDecoderApi);
    }
    for (uint32_t s : {0u, 4u, static_cast<uint32_t>(sizeof(CascadeIqDecoderApi)) - 8u,
                       static_cast<uint32_t>(sizeof(CascadeIqDecoderApi)) + 8u}) {
        CascadeIqDecoderApi bad = validIqDecoder();
        bad.structSize = s;
        CascadePluginDesc p = validIqDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) ==
              PluginRejection::IqDecoderStructSizeMismatch);
    }
    // Rates out of range, on EITHER field. NaN and the infinities are in the
    // list on purpose: a range check written as a negation would accept NaN
    // (every comparison with NaN is false) and hand it to the DSP thread.
    {
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        for (double r : {-1.0, -2400000.0, 1.0, 7999.9, CASCADE_IQ_RATE_MAX_HZ + 1.0, 1e12, nan,
                         inf, -inf}) {
            CascadeIqDecoderApi bad = validIqDecoder();
            bad.requiredRateHz = r;
            CascadePluginDesc p = validIqDesc(&bad);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::IqDecoderRateOutOfRange);

            // preferredRateHz is range-checked just as hard: it is advisory
            // to the host's POLICY, not to its arithmetic, and a UI that
            // offers to retune a device to NaN is not better than a crash.
            CascadeIqDecoderApi bad2 = validIqDecoder();
            bad2.preferredRateHz = r;
            CascadePluginDesc p2 = validIqDesc(&bad2);
            CHECK(cascade::core::validatePluginDesc(&p2) ==
                  PluginRejection::IqDecoderRateOutOfRange);
        }
    }
    // Rates in range, including 0 ("any rate") on either or both fields, and
    // the two ends of the accepted interval.
    for (double r : {0.0, CASCADE_IQ_RATE_MIN_HZ, 2048000.0, 2400000.0, 8000000.0,
                     CASCADE_IQ_RATE_MAX_HZ}) {
        CascadeIqDecoderApi ok = validIqDecoder();
        ok.requiredRateHz = r;
        ok.preferredRateHz = r;
        CascadePluginDesc p = validIqDesc(&ok);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }
    {
        // The realistic combination: no hard requirement, a stated preference.
        CascadeIqDecoderApi ok = validIqDecoder();
        ok.requiredRateHz = 0.0;
        ok.preferredRateHz = 2400000.0;
        CascadePluginDesc p = validIqDesc(&ok);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }
    // The four mandatory callbacks, one at a time.
    {
        CascadeIqDecoderApi bad = validIqDecoder();
        bad.create = nullptr;
        CascadePluginDesc p = validIqDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingIqDecoderFunction);
    }
    {
        CascadeIqDecoderApi bad = validIqDecoder();
        bad.process = nullptr;
        CascadePluginDesc p = validIqDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingIqDecoderFunction);
    }
    {
        CascadeIqDecoderApi bad = validIqDecoder();
        bad.poll_text = nullptr;
        CascadePluginDesc p = validIqDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingIqDecoderFunction);
    }
    {
        CascadeIqDecoderApi bad = validIqDecoder();
        bad.destroy = nullptr;
        CascadePluginDesc p = validIqDesc(&bad);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingIqDecoderFunction);
    }
    {
        // retune is the ONE optional pointer in either table. A decoder that
        // does not care where the receiver is tuned leaves it null, and the
        // host must accept that rather than force a do-nothing stub.
        CascadeIqDecoderApi ok = validIqDecoder();
        ok.retune = nullptr;
        CascadePluginDesc p = validIqDesc(&ok);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }
}

// ---------------------------------------------------------------------------
// 1b. The ABI-2 retirement rule, on its own because it is a product decision
//     as much as a technical one: every version-1 plugin is refused, and the
//     refusal names both versions so the author knows what to rebuild.
// ---------------------------------------------------------------------------

void testVersionOnePluginIsRefused() {
    // This test encodes the v1 -> v2 retirement literally (the strings it
    // asserts on contain "2"), so it must be revisited on the next bump
    // rather than quietly passing against a different pair of numbers.
    static_assert(CASCADE_PLUGIN_ABI_VERSION == 2,
                  "ABI moved past 2: update testVersionOnePluginIsRefused");

    const CascadeDecoderApi dec = validDecoder();

    // A descriptor that is perfect in every other respect - the audio-only
    // shape a v1 plugin has, with every string, every callback and a sane
    // rate - and claims ABI 1. Nothing about it is salvageable to the host:
    // the real thing would also be four bytes shorter, and the host has no
    // way to tell this one from that one, which is precisely why the version
    // decides on its own.
    CascadePluginDesc v1 = validDesc(&dec);
    v1.abiVersion = 1u;
    CHECK(cascade::core::validatePluginDesc(&v1) == PluginRejection::AbiVersionMismatch);

    const std::string msg =
        cascade::core::describePluginRejection(PluginRejection::AbiVersionMismatch, &v1);
    CHECK(contains(msg, "2"));  // the host's version
    CHECK(contains(msg, "1"));  // the plugin's
    CHECK(contains(msg, "host requires exactly 2"));
    CHECK(contains(msg, "plugin reports 1"));
    std::printf("  v1 plugin refusal: %s\n", msg.c_str());

    // A v1 descriptor that ALSO has the old struct size (which is what a real
    // v1 binary looks like: no iqDecoder member) is still reported as the
    // version mismatch - version is decided before size, because size can
    // only be interpreted once the layout is known.
    CascadePluginDesc v1Short = validDesc(&dec);
    v1Short.abiVersion = 1u;
    v1Short.structSize =
        static_cast<uint32_t>(sizeof(CascadePluginDesc) - sizeof(const CascadeIqDecoderApi*));
    CHECK(cascade::core::validatePluginDesc(&v1Short) == PluginRejection::AbiVersionMismatch);

    // And there is no way to get a v1 plugin loaded by dressing it up: the
    // check is on the number, not on how plausible the rest of the descriptor
    // looks.
    for (uint32_t caps : {static_cast<uint32_t>(CASCADE_CAP_DECODER),
                          static_cast<uint32_t>(CASCADE_CAP_IQ_DECODER), 0u}) {
        CascadePluginDesc p = validDesc(&dec);
        p.abiVersion = 1u;
        p.capabilities = caps;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::AbiVersionMismatch);
    }
}

// Messages must be usable in a support conversation: never empty, and
// carrying the actual numbers where a number is the whole story.
void testRejectionMessages() {
    const CascadeDecoderApi dec = validDecoder();
    // Up to the LAST enumerator, so a new rejection added without a message
    // fails here instead of reaching a user as "unknown rejection".
    for (int i = 0; i <= static_cast<int>(PluginRejection::MissingIqDecoderFunction); ++i) {
        const char* m = cascade::core::pluginRejectionMessage(static_cast<PluginRejection>(i));
        CHECK(m != nullptr && m[0] != '\0');
        CHECK(std::strcmp(m, "unknown rejection") != 0);
    }

    CascadePluginDesc p = validDesc(&dec);
    p.abiVersion = 999u;
    const std::string s =
        cascade::core::describePluginRejection(PluginRejection::AbiVersionMismatch, &p);
    CHECK(contains(s, "999"));
    CHECK(contains(s, std::to_string(CASCADE_PLUGIN_ABI_VERSION).c_str()));

    // The IQ-specific messages carry their numbers too: a size mismatch says
    // both sizes, and an out-of-range rate says the rate AND the bounds.
    {
        CascadeIqDecoderApi bad = validIqDecoder();
        bad.structSize = 4u;
        CascadePluginDesc q = validIqDesc(&bad);
        const std::string m = cascade::core::describePluginRejection(
            PluginRejection::IqDecoderStructSizeMismatch, &q);
        CHECK(contains(m, std::to_string(sizeof(CascadeIqDecoderApi)).c_str()));
        CHECK(contains(m, "4"));
    }
    {
        CascadeIqDecoderApi bad = validIqDecoder();
        bad.requiredRateHz = 1e12;
        CascadePluginDesc q = validIqDesc(&bad);
        const std::string m =
            cascade::core::describePluginRejection(PluginRejection::IqDecoderRateOutOfRange, &q);
        CHECK(contains(m, "1e+12"));
        CHECK(contains(m, "8000"));
        std::printf("  IQ rate refusal: %s\n", m.c_str());
    }

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
        int drivenAudio = 0;
        int drivenIq = 0;
        for (const LoadedPlugin& rec : host.plugins()) {
            std::printf("  OK-dir: %s loaded=%d name='%s' v='%s' licence='%s' caps=0x%08X err='%s'\n",
                        rec.path.c_str(), rec.loaded ? 1 : 0, rec.name.c_str(),
                        rec.version.c_str(), rec.licence.c_str(),
                        static_cast<unsigned>(rec.capabilities), rec.error.c_str());
            if (!rec.loaded) {
                continue;
            }
            CHECK(rec.error.empty());
            CHECK(!rec.name.empty());
            CHECK(!rec.licence.empty());
            CHECK(rec.nativeHandle != nullptr);
            // At least one known capability, and a table for each declared
            // bit - the same invariant the validator enforces, re-checked
            // here against a REAL module rather than a fixture.
            CHECK((rec.capabilities & (CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER)) != 0u);
            CHECK(((rec.capabilities & CASCADE_CAP_DECODER) != 0u) == (rec.decoder != nullptr));
            CHECK(((rec.capabilities & CASCADE_CAP_IQ_DECODER) != 0u) ==
                  (rec.iqDecoder != nullptr));

            // Drive the audio decoder end to end through the ABI: create,
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
                        ++drivenAudio;
                    }
                    rec.decoder->destroy(inst);
                }
            }

            // Drive the IQ decoder the same way. The signal is a complex tone
            // at a KNOWN offset from centre, built here as interleaved I,Q -
            // so if the plugin misreads the interleaving, the frequency it
            // reports is wrong and this check has something to show for it.
            if (rec.iqDecoder != nullptr) {
                const CascadeIqDecoderApi* q = rec.iqDecoder;
                const double rate = q->requiredRateHz != 0.0 ? q->requiredRateHz : 2400000.0;
                const double centerHz = 1090000000.0;  // ADS-B, the driving case
                void* inst = q->create(rate, centerHz);
                CHECK(inst != nullptr);
                if (inst != nullptr) {
                    // 8192 complex samples = 16384 floats. A tone at
                    // +rate/10 from DC, amplitude 0.5 per component-pair
                    // (|z| = 0.5, so rms 0.5 and peak 0.5).
                    const std::size_t frames = 8192u;
                    const double toneHz = rate / 10.0;
                    std::vector<float> iqBuf(2u * frames);
                    for (std::size_t k = 0; k < frames; ++k) {
                        const double ph = 2.0 * 3.14159265358979323846 * toneHz *
                                          static_cast<double>(k) / rate;
                        iqBuf[2u * k] = static_cast<float>(0.5 * std::cos(ph));
                        iqBuf[2u * k + 1u] = static_cast<float>(0.5 * std::sin(ph));
                    }
                    // retune is optional in the ABI: the host must check for
                    // NULL, and so must this test.
                    if (q->retune != nullptr) {
                        q->retune(inst, centerHz + 1000000.0);
                    }
                    q->process(inst, iqBuf.data(), frames);
                    char buf[256];
                    const int32_t n = q->poll_text(inst, buf, sizeof(buf));
                    CHECK(n > 0);
                    if (n > 0) {
                        std::printf("  iq decoder said: %.*s", static_cast<int>(n), buf);
                        ++drivenIq;
                    }
                    q->destroy(inst);
                }
            }
        }
        std::printf("  drove %d audio decoder(s) and %d IQ decoder(s) for real\n", drivenAudio,
                    drivenIq);
        host.unloadAll();
    }

    if (badDir != nullptr) {
        PluginHost host;
        host.scan(badDir);
        CHECK(host.plugins().size() >= 1);
        // The whole point: a mismatched ABI is refused, with a reason, and
        // nothing from it is loaded. Build the example with
        // /DCASCADE_EXAMPLE_FORCE_ABI=1 and this covers the retirement rule
        // against a real module: the DLL loads, its entry point runs, its
        // descriptor is read, and the host still says no.
        CHECK(host.loadedCount() == 0);
        for (const LoadedPlugin& rec : host.plugins()) {
            std::printf("  BAD-dir: %s loaded=%d err='%s'\n", rec.path.c_str(),
                        rec.loaded ? 1 : 0, rec.error.c_str());
            CHECK(!rec.loaded);
            CHECK(!rec.error.empty());
            CHECK(rec.decoder == nullptr);
            CHECK(rec.iqDecoder == nullptr);
        }
        host.unloadAll();
    }
}

}  // namespace

int main() {
    testValidation();
    testVersionOnePluginIsRefused();
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
