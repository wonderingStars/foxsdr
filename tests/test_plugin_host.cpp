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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
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

// The extension the host actually scans for on this platform. Fixtures must
// use it rather than a literal ".dll": a scan test that stages a .dll on Linux
// is testing nothing, because the extension filter discards the file before
// the loader ever sees it, and the test then fails for a reason that has
// nothing to do with what it is named after.
#if defined(_WIN32)
constexpr const char* kModExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModExt = ".dylib";
#else
constexpr const char* kModExt = ".so";
#endif

// Fixture filename with the platform's module extension: mod("broken") gives
// "broken.dll" on Windows and "broken.so" elsewhere.
std::string mod(const char* stem) { return std::string(stem) + kModExt; }

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
// ABI 3: the descriptor BORROWS a capability array rather than carrying a
// pointer per capability, so that array has to outlive the descriptor this
// returns. The tests keep descriptors alive across statements and in places
// hold two at once, which rules out a local array (dangles the moment we
// return) and a shared static one (the two would alias). A small deliberate
// leak is the honest answer in a test binary: a few dozen 16-byte allocations
// that are never freed, and no lifetime question to get wrong.
//
// An entry is emitted only for a NON-NULL table, while `caps` is set
// independently. That preserves what the old fixture expressed by nulling a
// member: pass the bit with a null table to get "declares it, supplies no
// table", which is still MissingDecoderApi and friends.
CascadePluginDesc descFor(uint32_t caps, const CascadeDecoderApi* dec,
                          const CascadeIqDecoderApi* iq,
                          const CascadeImageDecoderApi* img = nullptr) {
    auto* entries = new CascadeCapabilityEntry[3]{};
    uint32_t n = 0;
    if (dec != nullptr) {
        entries[n++] = {CASCADE_CAP_DECODER, static_cast<uint32_t>(sizeof(CascadeDecoderApi)),
                        dec};
    }
    if (iq != nullptr) {
        entries[n++] = {CASCADE_CAP_IQ_DECODER,
                        static_cast<uint32_t>(sizeof(CascadeIqDecoderApi)), iq};
    }
    if (img != nullptr) {
        entries[n++] = {CASCADE_CAP_IMAGE_DECODER,
                        static_cast<uint32_t>(sizeof(CascadeImageDecoderApi)), img};
    }

    CascadePluginDesc p{};
    p.structSize = static_cast<uint32_t>(sizeof(CascadePluginDesc));
    p.abiVersion = static_cast<uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    p.name = "Fixture";
    p.version = "1.0.0";
    p.author = "tests";
    p.licence = "MIT";
    p.capabilities = caps;
    // A descriptor with no tables at all still needs a non-zero count and a
    // non-null array to get past the structural checks and reach the
    // per-capability ones, which is what the "declares it, supplies nothing"
    // cases are testing. n==0 leaves one zeroed entry, which findCapability
    // Table skips because its capability field names no bit.
    p.capabilityCount = n > 0 ? n : 1u;
    p.capabilityTables = entries;
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

    // --- The capability table itself (ABI 3). These replace the old
    // "reserved must be zero" case: reserved is gone, and the array that took
    // its place has its own structural rules.
    {
        CascadePluginDesc p = validDesc(&dec);
        p.capabilityCount = 0u;
        CHECK(cascade::core::validatePluginDesc(&p) ==
              PluginRejection::CapabilityCountOutOfRange);
    }
    {
        // A count large enough to walk arbitrary memory is refused rather
        // than trusted and iterated.
        CascadePluginDesc p = validDesc(&dec);
        p.capabilityCount = 1000000u;
        CHECK(cascade::core::validatePluginDesc(&p) ==
              PluginRejection::CapabilityCountOutOfRange);
    }
    {
        CascadePluginDesc p = validDesc(&dec);
        p.capabilityTables = nullptr;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingCapabilityTables);
    }

    // --- Unknown capability bits are IGNORED when something usable remains,
    // which is the whole point of ABI 3 and the thing that makes future
    // capabilities additive. A plugin built for a later host still loads here
    // and provides whatever this host understands.
    {
        CascadePluginDesc p = validDesc(&dec);
        p.capabilities = CASCADE_CAP_DECODER | 0x40000000u;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }
    {
        // ...but a plugin whose capabilities are ALL unknown provides this
        // host nothing, and saying so beats loading a decoration.
        CascadePluginDesc p = validDesc(&dec);
        p.capabilities = 0x40000000u;
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::NoUsableCapability);
        const std::string msg = cascade::core::describePluginRejection(
            PluginRejection::NoUsableCapability, &p);
        CHECK(contains(msg.c_str(), "newer version"));
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
    // ABI 3 REVERSED the rule these cases used to assert. Up to ABI 2 an
    // unknown bit meant the descriptor could not be what it claimed, so the
    // plugin was refused outright; that was the only safe reading while
    // capabilities were trailing struct members, because an unknown bit
    // implied a layout this host could not know the size of.
    //
    // Now the tables are out of line and each carries its own size, so an
    // unknown bit costs nothing: the host reads the tables it recognises and
    // never dereferences one it does not. Refusing would mean a plugin built
    // against a later host is useless here even for the parts both understand,
    // which is exactly the flag-day problem ABI 3 exists to end.
    //
    // Note 0x4 has since become CASCADE_CAP_IMAGE_DECODER, so the unknown
    // cases start above it.
    for (uint32_t caps : {CASCADE_CAP_DECODER | 0x80000000u,
                          CASCADE_CAP_IQ_DECODER | 0x08000000u,
                          CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | 0x10000000u}) {
        CascadePluginDesc p = descFor(caps, &dec, &iq);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }
    // Only-unknown is still refused, because then there is nothing to run.
    for (uint32_t caps : {0x80000000u, 0x08000000u, 0x10000000u | 0x20000000u}) {
        CascadePluginDesc p = descFor(caps, &dec, &iq);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::NoUsableCapability);
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
    // This test encodes the retirement rule literally (the strings it asserts
    // on contain the host's version number), so it must be revisited on each
    // bump rather than quietly passing against a different pair of numbers.
    static_assert(CASCADE_PLUGIN_ABI_VERSION == 3,
                  "ABI moved past 3: update testVersionOnePluginIsRefused");

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
    CHECK(contains(msg, "3"));  // the host's version
    CHECK(contains(msg, "1"));  // the plugin's
    CHECK(contains(msg, "host requires exactly 3"));
    CHECK(contains(msg, "plugin reports 1"));
    std::printf("  v1 plugin refusal: %s\n", msg.c_str());

    // Version 2 is refused on exactly the same terms. Worth asserting
    // separately from version 1: v2 is the version every plugin in the
    // catalogue was built against before this bump, so it is the one users
    // will actually hit, and "one behind" must not be treated as good enough.
    CascadePluginDesc v2 = validDesc(&dec);
    v2.abiVersion = 2u;
    CHECK(cascade::core::validatePluginDesc(&v2) == PluginRejection::AbiVersionMismatch);
    const std::string msg2 =
        cascade::core::describePluginRejection(PluginRejection::AbiVersionMismatch, &v2);
    CHECK(contains(msg2, "host requires exactly 3"));
    CHECK(contains(msg2, "plugin reports 2"));

    // An older descriptor ALSO has a different struct size (a v2 descriptor
    // carried two trailing table pointers where this one carries a count and
    // an array) and is still reported as the version mismatch - version is
    // decided before size, because size can only be interpreted once the
    // layout is known.
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
    writeFile(d / mod("broken"), junk, sizeof(junk) - 1);

    PluginHost host;
    host.scan(d.string());
    CHECK(host.plugins().size() == 1);
    CHECK(host.loadedCount() == 0);
    if (host.plugins().size() == 1) {
        const LoadedPlugin& rec = host.plugins()[0];
        CHECK(!rec.loaded);
        CHECK(!rec.error.empty());
        CHECK(contains(rec.path, mod("broken").c_str()));
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
    writeFile(d / mod("zulu"), junk, sizeof(junk) - 1);
    writeFile(d / mod("alpha"), junk, sizeof(junk) - 1);
    writeFile(d / mod("mike"), junk, sizeof(junk) - 1);

    PluginHost host;
    host.scan(d.string());
    CHECK(host.plugins().size() == 3);
    if (host.plugins().size() == 3) {
        CHECK(contains(host.plugins()[0].path, mod("alpha").c_str()));
        CHECK(contains(host.plugins()[1].path, mod("mike").c_str()));
        CHECK(contains(host.plugins()[2].path, mod("zulu").c_str()));
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
    writeFile(d / mod("one"), junk, sizeof(junk) - 1);
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
    // An absolute path, never a bare relative one - unless the process path
    // could not be determined at all, which cannot happen for a normal test
    // binary. Which of the two candidates it is depends on where this test
    // binary sits, so the assertion below is the invariant that matters
    // rather than the identity of the directory.
    CHECK(fs::path(d).is_absolute());
    // THE POINT OF THE WHOLE CHOICE: whatever comes back must be somewhere a
    // plugin can actually be written. A directory that fails this is one
    // where the catalogue cannot cache a policy and no install can ever
    // succeed - which is exactly what an installed copy under Program Files
    // returned before defaultPluginDir learned to fall back.
    CHECK(PluginHost::directoryIsWritable(d));
    std::printf("  default plugin dir: %s\n", d.c_str());

    // Scanning it must be harmless whether or not it exists.
    PluginHost host;
    host.scanDefault();
    CHECK(host.directory() == d);
}

// The DIRECTORY CHOICE, which is the decision that makes plugins possible at
// all on an installed copy. Split the way cat_protocol is: a pure function
// holding the policy, and a probe that touches the filesystem.

void testPluginDirChoice() {
    // The whole policy, as a pure function.
    CHECK(PluginHost::choosePluginDir("/exe/plugins", "/user/plugins", true) == "/exe/plugins");
    CHECK(PluginHost::choosePluginDir("/exe/plugins", "/user/plugins", false) == "/user/plugins");
    // Nothing in the environment to derive a per-user path from: keep the
    // real one, so the error the user is shown names a directory that exists
    // rather than "".
    CHECK(PluginHost::choosePluginDir("/exe/plugins", "", false) == "/exe/plugins");

    // Both candidates are absolute, named, and distinct on any ordinary
    // desktop - a fallback that resolved to the same place would be no
    // fallback at all.
    const std::string exeDir = PluginHost::exePluginDir();
    const std::string userDir = PluginHost::userPluginDir();
    CHECK(!exeDir.empty());
    CHECK(contains(exeDir, "plugins"));
    CHECK(!userDir.empty());
    if (!userDir.empty()) {
        CHECK(fs::path(userDir).is_absolute());
        CHECK(contains(userDir, "foxsdr"));
        CHECK(contains(userDir, "plugins"));
        CHECK(userDir != exeDir);
    }
    std::printf("  exe plugin dir:  %s\n", exeDir.c_str());
    std::printf("  user plugin dir: %s\n", userDir.c_str());
}

void testDirectoryWritability() {
    const fs::path d = tmpDir("writable");
    CHECK(PluginHost::directoryIsWritable(d.string()));
    // A directory that does not exist YET answers for the parent that would
    // have to hold it. This is the first-run case on every installation, and
    // getting it wrong would send a portable copy to the per-user directory.
    CHECK(PluginHost::directoryIsWritable((d / "not_yet").string()));
    CHECK(!fs::exists(d / "not_yet"));
    // The probe writes, so it must also clean up: nothing may survive it.
    int leftovers = 0;
    for (const auto& entry : fs::directory_iterator(d)) {
        (void)entry;
        ++leftovers;
    }
    CHECK(leftovers == 0);
    CHECK(!PluginHost::directoryIsWritable(""));

    // A path whose parent is a FILE can hold nothing, on either platform.
    const fs::path f = d / "a_file";
    {
        std::ofstream o(f, std::ios::binary | std::ios::trunc);
        o << "x";
    }
    CHECK(!PluginHost::directoryIsWritable((f / "child").string()));

    // THE CASE THE WRITE PROBE EXISTS FOR, and the one an is_directory check
    // gets wrong: a directory that exists, lists and traverses perfectly well
    // and still refuses every write. Reachable portably only on POSIX; the
    // Windows equivalent is an ACL, verified by hand against Program Files
    // (BUILTIN\Users hold read+execute there and nothing more), which is
    // where the reported bug came from.
#ifndef _WIN32
    const fs::path ro = d / "read_only";
    std::error_code ec;
    fs::create_directories(ro, ec);
    fs::permissions(ro, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace, ec);
    if (!ec && geteuid() != 0) {
        CHECK(!PluginHost::directoryIsWritable(ro.string()));
        // The same path, to the check this replaced. Both answers are here so
        // the difference between them is the thing under test.
        CHECK(fs::is_directory(ro, ec));
    } else {
        std::printf("  SKIP: read-only directory check (root ignores mode bits)\n");
    }
    fs::permissions(ro, fs::perms::owner_all, fs::perm_options::replace, ec);
#else
    std::printf("  SKIP: read-only directory check (needs POSIX mode bits)\n");
#endif

    std::error_code cleanup;
    fs::remove_all(d, cleanup);
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

// ---------------------------------------------------------------------------
// The UI and host-services capabilities, added to ABI 3 WITHOUT bumping it.
//
// The point of these is not only that they work: it is that they exist at all
// without a version change. Every assertion here runs against the same
// CASCADE_PLUGIN_ABI_VERSION that the already-published plugins were built
// against.
// ---------------------------------------------------------------------------

namespace uicaps {

void* trackCreate() { return reinterpret_cast<void*>(1); }
int32_t trackPoll(void*, CascadeTrack*, uint32_t) { return 0; }
void trackDestroy(void*) {}

void* panelCreate() { return reinterpret_cast<void*>(1); }
uint32_t panelColumns(void*, char h[CASCADE_PANEL_MAX_COLUMNS][CASCADE_PANEL_CELL_CHARS]) {
    std::snprintf(h[0], CASCADE_PANEL_CELL_CHARS, "Satellite");
    return 1u;
}
int32_t panelPoll(void*, CascadePanelRow*, uint32_t) { return 0; }
void panelDestroy(void*) {}

void hostAttach(const CascadeHostApi*) {}

CascadeTrackSourceApi validTrack() {
    CascadeTrackSourceApi t{};
    t.structSize = static_cast<uint32_t>(sizeof(CascadeTrackSourceApi));
    t.create = &trackCreate;
    t.poll_tracks = &trackPoll;
    t.poll_paths = nullptr;  // optional by contract
    t.destroy = &trackDestroy;
    return t;
}

CascadePanelApi validPanel() {
    CascadePanelApi p{};
    p.structSize = static_cast<uint32_t>(sizeof(CascadePanelApi));
    p.title = "Satellite passes";
    p.create = &panelCreate;
    p.columns = &panelColumns;
    p.poll_rows = &panelPoll;
    p.destroy = &panelDestroy;
    return p;
}

CascadeHostClientApi validHostClient() {
    CascadeHostClientApi h{};
    h.structSize = static_cast<uint32_t>(sizeof(CascadeHostClientApi));
    h.attach = &hostAttach;
    return h;
}

uint32_t presetCount() { return 1u; }
int32_t presetGet(uint32_t index, CascadePreset* out) {
    if (index != 0u || out == nullptr) { return 0; }
    std::snprintf(out->label, CASCADE_PRESET_LABEL_CHARS, "ADS-B 1090 MHz");
    out->frequencyHz = 1090000000.0;
    out->demodMode = CASCADE_DEMOD_RAW;
    out->bandwidthHz = 0.0;
    out->sampleRateHz = 2400000.0;
    out->flags = CASCADE_PRESET_DEVICE_CENTRE;
    return 1;
}

// A stable panel table to point a deliberately-mismatched entry at.
const CascadePanelApi& validPanelStorage() {
    static const CascadePanelApi p = validPanel();
    return p;
}

void* bmCreate() { return reinterpret_cast<void*>(1); }
int32_t bmGetTile(void*, uint32_t, uint32_t, uint32_t, CascadeTile*) {
    return CASCADE_TILE_PENDING;
}
void bmRelease(void*, const CascadeTile*) {}
int32_t bmPollText(void*, char*, size_t) { return 0; }
void bmDestroy(void*) {}

CascadeBasemapApi validBasemap() {
    CascadeBasemapApi b{};
    b.structSize = static_cast<uint32_t>(sizeof(CascadeBasemapApi));
    b.attribution = "Map data (c) OpenStreetMap contributors";
    b.minZoom = 0u;
    b.maxZoom = 19u;
    b.tileSize = 256u;
    b.create = &bmCreate;
    b.get_tile = &bmGetTile;
    b.release_tile = &bmRelease;
    b.poll_text = &bmPollText;
    b.destroy = &bmDestroy;
    return b;
}

CascadePresetApi validPreset() {
    CascadePresetApi p{};
    p.structSize = static_cast<uint32_t>(sizeof(CascadePresetApi));
    p.count = &presetCount;
    p.get = &presetGet;
    return p;
}

void* tiCreate() { return reinterpret_cast<void*>(1); }
int32_t tiGetInfo(void*, const char*, uint32_t, CascadeTrackInfo*) {
    return CASCADE_INFO_MISSING;
}
void tiRelease(void*, const CascadeTrackInfo*) {}
int32_t tiPollText(void*, char*, size_t) { return 0; }
void tiDestroy(void*) {}

CascadeTrackInfoApi validTrackInfo() {
    CascadeTrackInfoApi t{};
    t.structSize = static_cast<uint32_t>(sizeof(CascadeTrackInfoApi));
    t.create = &tiCreate;
    t.get_info = &tiGetInfo;
    t.release_info = &tiRelease;
    t.poll_text = &tiPollText;
    t.destroy = &tiDestroy;
    return t;
}

// Builds a descriptor from an arbitrary set of capability entries. The array
// is leaked deliberately, for the reason descFor documents.
CascadePluginDesc descWith(uint32_t caps, const CascadeCapabilityEntry* entries,
                           uint32_t count) {
    auto* owned = new CascadeCapabilityEntry[count > 0 ? count : 1]{};
    for (uint32_t i = 0; i < count; ++i) { owned[i] = entries[i]; }
    CascadePluginDesc p{};
    p.structSize = static_cast<uint32_t>(sizeof(CascadePluginDesc));
    p.abiVersion = static_cast<uint32_t>(CASCADE_PLUGIN_ABI_VERSION);
    p.name = "UiFixture";
    p.version = "1.0.0";
    p.author = "tests";
    p.licence = "MIT";
    p.capabilities = caps;
    p.capabilityCount = count > 0 ? count : 1u;
    p.capabilityTables = owned;
    return p;
}

}  // namespace uicaps

void testUiCapabilities() {
    using namespace uicaps;
    const CascadeTrackSourceApi track = validTrack();
    const CascadePanelApi panel = validPanel();
    const CascadeHostClientApi hostc = validHostClient();

    const CascadeCapabilityEntry trackEntry{
        CASCADE_CAP_TRACK_SOURCE, static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)), &track};
    const CascadeCapabilityEntry panelEntry{
        CASCADE_CAP_PANEL, static_cast<uint32_t>(sizeof(CascadePanelApi)), &panel};
    const CascadeCapabilityEntry hostEntry{
        CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &hostc};

    // THE CASE THAT MOTIVATED ALL OF THIS: a plugin that decodes NOTHING.
    // A satellite tracker consumes no signal - it needs TLEs and a clock - so
    // if a decoder-less plugin cannot load, a tracker cannot be a plugin.
    {
        CascadePluginDesc p = descWith(CASCADE_CAP_TRACK_SOURCE, &trackEntry, 1);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }
    {
        CascadePluginDesc p = descWith(CASCADE_CAP_PANEL, &panelEntry, 1);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }
    // The full satellite-tracker shape: map targets, a window, and the right
    // to ask the host to retune.
    {
        const CascadeCapabilityEntry all[3] = {trackEntry, panelEntry, hostEntry};
        CascadePluginDesc p = descWith(
            CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_PANEL | CASCADE_CAP_HOST_CLIENT, all, 3);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
    }

    // --- CASCADE_CAP_BASEMAP ----------------------------------------------
    {
        using namespace uicaps;
        const CascadeBasemapApi bm = validBasemap();
        const CascadeCapabilityEntry bmEntry{
            CASCADE_CAP_BASEMAP, static_cast<uint32_t>(sizeof(CascadeBasemapApi)), &bm};

        // A basemap IS a usable capability on its own, unlike a preset or a
        // host client: a plugin that supplies map imagery and nothing else has
        // done something for the user.
        {
            CascadePluginDesc p = descWith(CASCADE_CAP_BASEMAP, &bmEntry, 1);
            CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
        }

        // THE ONE THAT MATTERS. Map imagery is overwhelmingly OSM-derived and
        // therefore ODbL, which REQUIRES attribution. A basemap the host
        // cannot credit is refused rather than drawn uncredited - the whole
        // reason imagery lives in a plugin is to keep that obligation
        // attached to whoever supplies the tiles, and a host that would draw
        // it anyway makes the arrangement worthless.
        {
            CascadeBasemapApi bad = validBasemap();
            bad.attribution = nullptr;
            const CascadeCapabilityEntry e{
                CASCADE_CAP_BASEMAP, static_cast<uint32_t>(sizeof(CascadeBasemapApi)),
                &bad};
            CascadePluginDesc p = descWith(CASCADE_CAP_BASEMAP, &e, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::MissingBasemapAttribution);
        }
        {
            CascadeBasemapApi bad = validBasemap();
            bad.attribution = "";  // present but empty is no credit at all
            const CascadeCapabilityEntry e{
                CASCADE_CAP_BASEMAP, static_cast<uint32_t>(sizeof(CascadeBasemapApi)),
                &bad};
            CascadePluginDesc p = descWith(CASCADE_CAP_BASEMAP, &e, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::MissingBasemapAttribution);
        }

        // Declared but not supplied, a table from another ABI build, and a
        // null function - the same three shapes every capability is checked
        // for.
        {
            const CascadeCapabilityEntry panelE{
                CASCADE_CAP_PANEL, static_cast<uint32_t>(sizeof(CascadePanelApi)),
                &validPanelStorage()};
            CascadePluginDesc p = descWith(CASCADE_CAP_BASEMAP, &panelE, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::MissingBasemapApi);
        }
        {
            CascadeBasemapApi bad = validBasemap();
            bad.structSize = static_cast<uint32_t>(sizeof(CascadeBasemapApi)) + 8u;
            const CascadeCapabilityEntry e{
                CASCADE_CAP_BASEMAP, static_cast<uint32_t>(sizeof(CascadeBasemapApi)),
                &bad};
            CascadePluginDesc p = descWith(CASCADE_CAP_BASEMAP, &e, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::BasemapStructSizeMismatch);
        }
        {
            CascadeBasemapApi bad = validBasemap();
            bad.get_tile = nullptr;
            const CascadeCapabilityEntry e{
                CASCADE_CAP_BASEMAP, static_cast<uint32_t>(sizeof(CascadeBasemapApi)),
                &bad};
            CascadePluginDesc p = descWith(CASCADE_CAP_BASEMAP, &e, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::MissingBasemapFunction);
        }
        // A tile size or zoom range the host cannot draw with.
        {
            CascadeBasemapApi bad = validBasemap();
            bad.tileSize = 8u;
            const CascadeCapabilityEntry e{
                CASCADE_CAP_BASEMAP, static_cast<uint32_t>(sizeof(CascadeBasemapApi)),
                &bad};
            CascadePluginDesc p = descWith(CASCADE_CAP_BASEMAP, &e, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::BasemapBadTileSize);
        }
        {
            CascadeBasemapApi bad = validBasemap();
            bad.minZoom = 12u;
            bad.maxZoom = 3u;  // inverted
            const CascadeCapabilityEntry e{
                CASCADE_CAP_BASEMAP, static_cast<uint32_t>(sizeof(CascadeBasemapApi)),
                &bad};
            CascadePluginDesc p = descWith(CASCADE_CAP_BASEMAP, &e, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::BasemapBadTileSize);
        }
    }

    // --- CASCADE_CAP_TRACK_INFO, added without an ABI bump ----------------
    {
        using namespace uicaps;
        const CascadeTrackInfoApi ti = validTrackInfo();
        const CascadeCapabilityEntry tiEntry{
            CASCADE_CAP_TRACK_INFO, static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)),
            &ti};

        // Enrichment alone is a usable capability: a plugin that can say WHO
        // every aircraft on the map is has done something for the user, even
        // though it decodes nothing itself.
        {
            CascadePluginDesc p = descWith(CASCADE_CAP_TRACK_INFO, &tiEntry, 1);
            CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
        }
        // Declared but not supplied, a table from another ABI build, and a
        // null function - the same three shapes every capability is checked
        // for.
        {
            const CascadeCapabilityEntry panelE{
                CASCADE_CAP_PANEL, static_cast<uint32_t>(sizeof(CascadePanelApi)),
                &validPanelStorage()};
            CascadePluginDesc p = descWith(CASCADE_CAP_TRACK_INFO, &panelE, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::MissingTrackInfoApi);
        }
        {
            CascadeTrackInfoApi bad = validTrackInfo();
            bad.structSize = static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)) + 8u;
            const CascadeCapabilityEntry e{
                CASCADE_CAP_TRACK_INFO, static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)),
                &bad};
            CascadePluginDesc p = descWith(CASCADE_CAP_TRACK_INFO, &e, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::TrackInfoStructSizeMismatch);
        }
        {
            CascadeTrackInfoApi bad = validTrackInfo();
            bad.release_info = nullptr;
            const CascadeCapabilityEntry e{
                CASCADE_CAP_TRACK_INFO, static_cast<uint32_t>(sizeof(CascadeTrackInfoApi)),
                &bad};
            CascadePluginDesc p = descWith(CASCADE_CAP_TRACK_INFO, &e, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::MissingTrackInfoFunction);
        }
    }

    // --- CASCADE_CAP_PRESET, added without an ABI bump --------------------
    {
        using namespace uicaps;
        const CascadeIqDecoderApi iq = validIqDecoder();
        const CascadePresetApi preset = validPreset();
        const CascadeCapabilityEntry iqEntry{
            CASCADE_CAP_IQ_DECODER, static_cast<uint32_t>(sizeof(CascadeIqDecoderApi)), &iq};
        const CascadeCapabilityEntry presetEntry{
            CASCADE_CAP_PRESET, static_cast<uint32_t>(sizeof(CascadePresetApi)), &preset};

        // The shape this exists for: a decoder that also says where to find
        // its signal.
        {
            const CascadeCapabilityEntry all[2] = {iqEntry, presetEntry};
            CascadePluginDesc p =
                descWith(CASCADE_CAP_IQ_DECODER | CASCADE_CAP_PRESET, all, 2);
            CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
        }

        // A PRESET ALONE IS NOT ENOUGH, for the same reason a host client
        // alone is not: telling the user where to tune decodes nothing, plots
        // nothing and shows nothing. Loading it would put a row in the list
        // that looks like a decoder and is not.
        {
            CascadePluginDesc p = descWith(CASCADE_CAP_PRESET, &presetEntry, 1);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::NoUsableCapability);
        }

        // Declared but not supplied.
        {
            CascadePluginDesc p = descWith(CASCADE_CAP_IQ_DECODER | CASCADE_CAP_PRESET,
                                           &iqEntry, 1);
            CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingPresetApi);
        }
        // A table from a different build of the ABI.
        {
            CascadePresetApi bad = validPreset();
            bad.structSize = static_cast<uint32_t>(sizeof(CascadePresetApi)) + 8u;
            const CascadeCapabilityEntry badEntry{
                CASCADE_CAP_PRESET, static_cast<uint32_t>(sizeof(CascadePresetApi)), &bad};
            const CascadeCapabilityEntry all[2] = {iqEntry, badEntry};
            CascadePluginDesc p =
                descWith(CASCADE_CAP_IQ_DECODER | CASCADE_CAP_PRESET, all, 2);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::PresetStructSizeMismatch);
        }
        // Both functions are mandatory: there is no useful half of this table.
        {
            CascadePresetApi bad = validPreset();
            bad.get = nullptr;
            const CascadeCapabilityEntry badEntry{
                CASCADE_CAP_PRESET, static_cast<uint32_t>(sizeof(CascadePresetApi)), &bad};
            const CascadeCapabilityEntry all[2] = {iqEntry, badEntry};
            CascadePluginDesc p =
                descWith(CASCADE_CAP_IQ_DECODER | CASCADE_CAP_PRESET, all, 2);
            CHECK(cascade::core::validatePluginDesc(&p) ==
                  PluginRejection::MissingPresetFunction);
        }

        // THE COMPATIBILITY PROMISE: a plugin built before this bit existed
        // declares none of it and must be entirely unaffected.
        {
            CascadePluginDesc p = descWith(CASCADE_CAP_IQ_DECODER, &iqEntry, 1);
            CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::None);
        }
    }

    // HOST_CLIENT ALONE IS NOT ENOUGH. Being able to ask the host for things
    // is not doing anything for the user: such a plugin decodes nothing, plots
    // nothing and shows nothing, and loading it would put a row in the list
    // that looks functional and is not.
    {
        CascadePluginDesc p = descWith(CASCADE_CAP_HOST_CLIENT, &hostEntry, 1);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::NoUsableCapability);
    }

    // Declared but not supplied.
    {
        CascadePluginDesc p = descWith(CASCADE_CAP_TRACK_SOURCE, &panelEntry, 1);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingTrackSourceApi);
    }
    {
        CascadePluginDesc p = descWith(CASCADE_CAP_PANEL, &trackEntry, 1);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingPanelApi);
    }

    // Size disagreement disables THAT capability's plugin rather than being
    // read at the wrong offsets.
    {
        CascadeTrackSourceApi bad = validTrack();
        bad.structSize = 8u;
        const CascadeCapabilityEntry e{CASCADE_CAP_TRACK_SOURCE,
                                       static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)),
                                       &bad};
        CascadePluginDesc p = descWith(CASCADE_CAP_TRACK_SOURCE, &e, 1);
        CHECK(cascade::core::validatePluginDesc(&p) ==
              PluginRejection::TrackSourceStructSizeMismatch);
    }

    // Mandatory pointers. poll_paths stays NULL throughout and must remain
    // acceptable - it is optional by contract, and a source with no polylines
    // is the common case.
    {
        CascadeTrackSourceApi bad = validTrack();
        bad.poll_tracks = nullptr;
        const CascadeCapabilityEntry e{CASCADE_CAP_TRACK_SOURCE,
                                       static_cast<uint32_t>(sizeof(CascadeTrackSourceApi)),
                                       &bad};
        CascadePluginDesc p = descWith(CASCADE_CAP_TRACK_SOURCE, &e, 1);
        CHECK(cascade::core::validatePluginDesc(&p) ==
              PluginRejection::MissingTrackSourceFunction);
    }
    {
        // A window with no name is a window the user cannot identify among
        // several, so an empty title is refused like a null pointer.
        CascadePanelApi bad = validPanel();
        bad.title = "";
        const CascadeCapabilityEntry e{
            CASCADE_CAP_PANEL, static_cast<uint32_t>(sizeof(CascadePanelApi)), &bad};
        CascadePluginDesc p = descWith(CASCADE_CAP_PANEL, &e, 1);
        CHECK(cascade::core::validatePluginDesc(&p) == PluginRejection::MissingPanelFunction);
    }
    {
        CascadeHostClientApi bad = validHostClient();
        bad.attach = nullptr;
        const CascadeCapabilityEntry all[2] = {
            trackEntry,
            {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
             &bad}};
        CascadePluginDesc p =
            descWith(CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_HOST_CLIENT, all, 2);
        CHECK(cascade::core::validatePluginDesc(&p) ==
              PluginRejection::MissingHostClientFunction);
    }

    // The typed accessors resolve to the right tables, and to null for a
    // capability the plugin does not have.
    {
        const CascadeCapabilityEntry all[2] = {trackEntry, panelEntry};
        CascadePluginDesc p =
            descWith(CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_PANEL, all, 2);
        CHECK(cascade_plugin_track_source(&p) == &track);
        CHECK(cascade_plugin_panel(&p) == &panel);
        CHECK(cascade_plugin_host_client(&p) == nullptr);
        CHECK(cascade_plugin_audio_decoder(&p) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// Duplicate plugin ids: one version of a plugin runs, never two
// ---------------------------------------------------------------------------
//
// WHY THIS SECTION EXISTS. A plugin file name embeds its version, so
// installing 1.1.0 over 1.0.1 ADDS a file instead of replacing one. Both
// modules then load, both get their own instances, and a track source reports
// every aircraft twice - "4 targets" for two aeroplanes, with two markers,
// because the duplicates sit on identical coordinates. The host's answer is to
// load exactly one module per declared plugin id and leave the rest inert
// and, by the owner's explicit ruling, TO DELETE NOTHING.
//
// The policy is tested through resolveDuplicatePlugins(), the pure function
// scan() delegates to, for the same reason validatePluginDesc() is tested that
// way: this test cannot compile a DLL, so it cannot stage two real modules
// that declare one id. What that leaves untested is the two lines in scan()
// that call the function and unmap the losers. The no-deletion assertion below
// is made against the REAL scan() as well, because that is the property the
// owner reserved and it must be pinned on the code path the product runs.

// A byte whose ADDRESS stands in for a capability table living inside a
// plugin's image. Never dereferenced; it exists so the test can prove the
// pointer is cleared when a record is turned off.
unsigned char g_fakeTableByte = 0;

// A record shaped exactly as loadOne() leaves a successfully loaded module.
// nativeHandle stays null - the pure function never touches it, and a fake
// handle would be a real unmap in the one place that unmaps.
LoadedPlugin loadedRec(const fs::path& dir, const std::string& file, const char* id,
                       const char* version) {
    LoadedPlugin r;
    r.path = (dir / file).string();
    r.name = id;
    r.version = version;
    r.author = "Test";
    r.licence = "MIT";
    r.capabilities = CASCADE_CAP_IQ_DECODER;
    r.loaded = true;
    r.iqDecoder = reinterpret_cast<const CascadeIqDecoderApi*>(&g_fakeTableByte);
    return r;
}

// The contents of a directory, name and bytes, sorted. Two of these compared
// with == is the "nothing removed, nothing rewritten, nothing truncated"
// assertion.
std::vector<std::pair<std::string, std::string>> snapshotDir(const fs::path& d) {
    std::vector<std::pair<std::string, std::string>> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(d, ec)) {
        std::error_code fileEc;
        if (!e.is_regular_file(fileEc) || fileEc) { continue; }
        std::ifstream f(e.path(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
        out.emplace_back(e.path().filename().string(), bytes);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// One distinguishable file per name, so a snapshot comparison tells a deletion
// from a truncation from a rewrite.
void stageFiles(const fs::path& d, const std::vector<std::string>& names) {
    for (const std::string& n : names) {
        const std::string body = "module bytes for " + n;
        writeFile(d / n, body.data(), body.size());
    }
}

std::size_t loadedIn(const std::vector<LoadedPlugin>& recs) {
    std::size_t n = 0;
    for (const LoadedPlugin& r : recs) {
        if (r.loaded) { ++n; }
    }
    return n;
}

// The user's real case, and the case a string comparison gets wrong.
void testDuplicateKeepsHigherVersion() {
    const fs::path d = tmpDir("dup_higher");

    // 1.0.1 vs 1.1.0 - exactly what the ADS-B plugin update produced. Sorted
    // by file name the OLDER one comes first, which is how scan() presents
    // them.
    {
        const std::vector<std::string> names = {mod("adsb-decoder-1.0.1-abi3-win-x64"),
                                                mod("adsb-decoder-1.1.0-abi3-win-x64")};
        stageFiles(d, names);
        const auto before = snapshotDir(d);

        std::vector<LoadedPlugin> recs = {loadedRec(d, names[0], "ADS-B", "1.0.1"),
                                          loadedRec(d, names[1], "ADS-B", "1.1.0")};
        const std::size_t skipped = cascade::core::resolveDuplicatePlugins(recs);
        CHECK(skipped == 1);
        CHECK(loadedIn(recs) == 1);
        CHECK(!recs[0].loaded);
        CHECK(recs[1].loaded);
        CHECK(recs[1].error.empty());
        // A turned-off record must not keep a pointer into an image the
        // caller is about to unmap.
        CHECK(recs[0].iqDecoder == nullptr);
        CHECK(recs[1].iqDecoder != nullptr);

        CHECK(snapshotDir(d) == before);
        std::error_code ec;
        for (const std::string& n : names) { CHECK(fs::exists(d / n, ec)); }
    }

    // 1.9.0 vs 1.10.0. As text "1.10.0" < "1.9.0", so a string comparison
    // keeps the OLDER plugin - the exact opposite of the point. Sorted file
    // names put the newer one FIRST here, so this also proves the answer is
    // not simply "the last record wins".
    {
        std::error_code ec;
        fs::remove_all(d, ec);
        fs::create_directories(d, ec);
        const std::vector<std::string> names = {mod("thing-1.10.0"), mod("thing-1.9.0")};
        stageFiles(d, names);
        const auto before = snapshotDir(d);

        std::vector<LoadedPlugin> recs = {loadedRec(d, names[0], "Thing", "1.10.0"),
                                          loadedRec(d, names[1], "Thing", "1.9.0")};
        CHECK(cascade::core::resolveDuplicatePlugins(recs) == 1);
        CHECK(loadedIn(recs) == 1);
        CHECK(recs[0].loaded);   // 1.10.0
        CHECK(!recs[1].loaded);  // 1.9.0
        CHECK(contains(recs[1].error, mod("thing-1.10.0").c_str()));

        CHECK(snapshotDir(d) == before);
    }

    std::error_code ec;
    fs::remove_all(d, ec);
}

// Three copies of one plugin - what two updates in a row leave behind.
void testDuplicateThreeModules() {
    const fs::path d = tmpDir("dup_three");
    const std::vector<std::string> names = {mod("aprs-1.0.0"), mod("aprs-1.2.0"),
                                            mod("aprs-1.10.0")};
    stageFiles(d, names);
    const auto before = snapshotDir(d);

    std::vector<LoadedPlugin> recs = {loadedRec(d, names[0], "APRS", "1.0.0"),
                                      loadedRec(d, names[1], "APRS", "1.2.0"),
                                      loadedRec(d, names[2], "APRS", "1.10.0")};
    CHECK(cascade::core::resolveDuplicatePlugins(recs) == 2);
    CHECK(loadedIn(recs) == 1);
    CHECK(!recs[0].loaded);
    CHECK(!recs[1].loaded);
    CHECK(recs[2].loaded);  // 1.10.0 is the highest, not "1.2.0" by text

    CHECK(snapshotDir(d) == before);
    std::error_code ec;
    fs::remove_all(d, ec);
}

// Different plugins must be left completely alone.
void testDuplicateDifferentIdsUntouched() {
    const fs::path d = tmpDir("dup_distinct");
    const std::vector<std::string> names = {mod("adsb-1.1.0"), mod("aprs-1.0.0"),
                                            mod("pocsag-2.0.0")};
    stageFiles(d, names);
    const auto before = snapshotDir(d);

    std::vector<LoadedPlugin> recs = {loadedRec(d, names[0], "ADS-B", "1.1.0"),
                                      loadedRec(d, names[1], "APRS", "1.0.0"),
                                      loadedRec(d, names[2], "POCSAG", "2.0.0")};
    CHECK(cascade::core::resolveDuplicatePlugins(recs) == 0);
    CHECK(loadedIn(recs) == 3);
    for (const LoadedPlugin& r : recs) {
        CHECK(r.loaded);
        CHECK(r.error.empty());
        CHECK(r.iqDecoder != nullptr);
    }

    CHECK(snapshotDir(d) == before);
    std::error_code ec;
    fs::remove_all(d, ec);
}

// The skip has to be VISIBLE. The Plugins section renders a record that did
// not load as its file name plus this text, so both files must be
// identifiable from the row, and the user must be told the file is inert and
// removable with the button already beside it.
void testDuplicateSkipIsReported() {
    const fs::path d = tmpDir("dup_report");
    const std::vector<std::string> names = {mod("adsb-decoder-1.0.1-abi3-win-x64"),
                                            mod("adsb-decoder-1.1.0-abi3-win-x64")};
    stageFiles(d, names);

    std::vector<LoadedPlugin> recs = {loadedRec(d, names[0], "ADS-B", "1.0.1"),
                                      loadedRec(d, names[1], "ADS-B", "1.1.0")};
    CHECK(cascade::core::resolveDuplicatePlugins(recs) == 1);

    const std::string why = recs[0].error;
    CHECK(!why.empty());
    // The plugin both files claim to be.
    CHECK(contains(why, "ADS-B"));
    // The one that was KEPT, by file name and by version.
    CHECK(contains(why, mod("adsb-decoder-1.1.0-abi3-win-x64").c_str()));
    CHECK(contains(why, "1.1.0"));
    // The one that was IGNORED, by its own version - the row already shows
    // this file's name above the reason.
    CHECK(contains(why, "1.0.1"));
    // And what the user can do about it, in the words of the button that does
    // it. Nothing was deleted, so the file sits there until they say so.
    CHECK(contains(why, "Remove"));
    std::printf("  duplicate skip reason: %s\n", why.c_str());

    // The kept record carries no error: it is a perfectly good plugin.
    CHECK(recs[1].error.empty());

    std::error_code ec;
    fs::remove_all(d, ec);
}

// Same id AND same version. Whatever is chosen must be chosen by a stated rule
// and not by the order the filesystem happened to hand the files over: the
// first record in scan order wins, and scan order is sorted by path, so
// reversing the input keeps the same FILE and not merely the same index.
void testDuplicateTieBreakIsDeterministic() {
    const fs::path d = tmpDir("dup_tie");
    const std::vector<std::string> names = {mod("aaa-copy"), mod("zzz-copy")};
    stageFiles(d, names);
    const auto before = snapshotDir(d);

    {
        std::vector<LoadedPlugin> recs = {loadedRec(d, names[0], "AIS", "2.0.0"),
                                          loadedRec(d, names[1], "AIS", "2.0.0")};
        CHECK(cascade::core::resolveDuplicatePlugins(recs) == 1);
        CHECK(recs[0].loaded);
        CHECK(contains(recs[0].path, mod("aaa-copy").c_str()));
        CHECK(!recs[1].loaded);
    }
    {
        // Handed over the other way round. The rule is "first in the list",
        // and the list scan() builds is sorted by path, so the same file
        // survives a real scan however the directory was iterated.
        std::vector<LoadedPlugin> recs = {loadedRec(d, names[1], "AIS", "2.0.0"),
                                          loadedRec(d, names[0], "AIS", "2.0.0")};
        CHECK(cascade::core::resolveDuplicatePlugins(recs) == 1);
        CHECK(recs[0].loaded);
        CHECK(contains(recs[0].path, mod("zzz-copy").c_str()));
        CHECK(!recs[1].loaded);
    }

    CHECK(snapshotDir(d) == before);
    std::error_code ec;
    fs::remove_all(d, ec);
}

// A refused candidate has no id at all - its descriptor was never read - so
// several of them must never be collapsed into one. Two garbage files are two
// separate problems and the user needs both reported.
void testDuplicateIgnoresRefusedRecords() {
    const fs::path d = tmpDir("dup_refused");
    const std::vector<std::string> names = {mod("junk_a"), mod("junk_b")};
    stageFiles(d, names);

    std::vector<LoadedPlugin> recs;
    for (const std::string& n : names) {
        LoadedPlugin r;
        r.path = (d / n).string();
        r.error = "not a cascade plugin";
        recs.push_back(r);
    }
    CHECK(cascade::core::resolveDuplicatePlugins(recs) == 0);
    CHECK(recs.size() == 2);
    CHECK(recs[0].error == "not a cascade plugin");
    CHECK(recs[1].error == "not a cascade plugin");

    std::error_code ec;
    fs::remove_all(d, ec);
}

// THE OWNER'S RULING, pinned against the code path the product runs.
//
// scan() is given a directory holding a duplicate-looking pair plus unrelated
// files, and afterwards the directory must be byte for byte what it was. The
// application never removes a user's plugin on its own initiative: a stale
// file stays on disk for ever, and the user removes it if and when they want
// to, with the Remove button.
void testScanRemovesNothingFromDisk() {
    const fs::path d = tmpDir("dup_nodelete");
    const std::vector<std::string> names = {mod("adsb-decoder-1.0.1-abi3-win-x64"),
                                            mod("adsb-decoder-1.1.0-abi3-win-x64"),
                                            mod("aprs-1.0.0")};
    stageFiles(d, names);
    // Not a module at all, and therefore not something a scan has any excuse
    // to touch either.
    const char manifest[] = "{\"plugins\":[]}";
    writeFile(d / "installed.json", manifest, sizeof(manifest) - 1);

    const auto before = snapshotDir(d);
    CHECK(before.size() == 4);

    PluginHost host;
    host.scan(d.string());
    host.scan(d.string());  // and a rescan, which is what the GUI does
    host.unloadAll();

    const auto after = snapshotDir(d);
    CHECK(after.size() == 4);
    CHECK(after == before);  // every file, byte for byte
    std::error_code ec;
    for (const std::string& n : names) { CHECK(fs::exists(d / n, ec)); }
    CHECK(fs::exists(d / "installed.json", ec));

    // Every candidate still produced its own record: de-duplication turns a
    // record OFF, it never makes one disappear.
    CHECK(host.plugins().empty());  // unloadAll() cleared them
    host.scan(d.string());
    CHECK(host.plugins().size() == 3);  // installed.json is not a module
    CHECK(snapshotDir(d) == before);

    host.unloadAll();
    fs::remove_all(d, ec);
}

int main() {
    testValidation();
    testUiCapabilities();
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
    testPluginDirChoice();
    testDirectoryWritability();
    testDuplicateKeepsHigherVersion();
    testDuplicateThreeModules();
    testDuplicateDifferentIdsUntouched();
    testDuplicateSkipIsReported();
    testDuplicateTieBreakIsDeterministic();
    testDuplicateIgnoresRefusedRecords();
    testScanRemovesNothingFromDisk();
    testRealPluginDirs();
    return testSummary("test_plugin_host");
}
