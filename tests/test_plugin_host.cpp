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

CascadePresetApi validPreset() {
    CascadePresetApi p{};
    p.structSize = static_cast<uint32_t>(sizeof(CascadePresetApi));
    p.count = &presetCount;
    p.get = &presetGet;
    return p;
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
    testRealPluginDirs();
    return testSummary("test_plugin_host");
}
