// test_engine_api_wireb_winradio.cpp - THE WiNRADiO G39DDC AND G33DDC AS RADIOS OF THE ENGINE, SUCCESS PATH (wire-b second pass, item 34): RADIO_ADD "open-g39ddc:lib=<folder>" and
// "open-g33ddc:lib=<folder>" through the wire-b source table. The "radio" is the STAND-IN vendor library the osslibs lane builds from WiNRADiO's published document (tests/fixtures/
// g39ddc_standin.cpp and g33ddc_standin.cpp: G39DDCAPI.dll / G33DDCAPI.dll, in tests/fixture_osslibs/<config>), loaded by the source exactly as a user's library would be. Nothing of WiNRADiO's is
// in this test and no real receiver is touched.
//
// For each of the two:
//   W1  the radio opens through the engine, the library is called (one device opened, the stream started) and its tone, 5 kHz above the tuned centre, is in the radio's spectrum
//   W2  every library call arrived on ONE thread and none overlapped another (the document's one-thread rule, measured at the library)
//   W3  a retune reaches the library (the front-end / DDC frequencies change), stopping the radio stops the stream, removing it closes the device
//   W4  the radio is SAVED as kind g39ddc / g33ddc with the library folder in its arguments, and a restart from the file alone opens it again
//   W5  a library that reports no receiver is refused through the engine with a sentence and the radio stays on the generator
//
// UNVERIFIED on real WiNRADiO hardware and with WiNRADiO's real library.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "eol_fixture.hpp"
#include "source/vendor_lib.hpp"
#include "wire1_rig.hpp"

namespace {

namespace vl = cascade::source::vendorlib;

// The keys the two stand-ins share (tests/fixtures/*_standin.cpp)
enum Key : int { kMisbehave = 1, kToneHz = 2, kStarts = 14, kStops = 15, kOpens = 16, kCloses = 17, kThreads = 18, kOverlap = 19 };
constexpr std::int64_t kNoDevices = 1 << 0;

struct Fx {
    vl::Library lib;
    void (*set)(int, std::int64_t) = nullptr;
    std::int64_t (*get)(int) = nullptr;
    void (*reset)() = nullptr;
    std::string dir;
    bool load(const char* dll) {
        dir = eol::u8(eol::fixtureDir());
        if (!lib.open(dir + "\\" + dll).ok()) { return false; }
        set = reinterpret_cast<void (*)(int, std::int64_t)>(lib.symbol("eol_set"));
        get = reinterpret_cast<std::int64_t (*)(int)>(lib.symbol("eol_get"));
        reset = reinterpret_cast<void (*)()>(lib.symbol("eol_reset"));
        return set != nullptr && get != nullptr && reset != nullptr;
    }
};

bool toneIn(Rig& r, std::size_t radio, double offsetHz) {
    for (int i = 0; i < 80; ++i) {
        r.pump(10);
        if (peakAt(r.spectrum(static_cast<std::uint32_t>(radio)), offsetHz, 12)) { return true; }
    }
    return false;
}

void runOne(const char* key, const char* dll, const char* sourceName) {
    std::printf("== %s ==\n", sourceName);
    Fx fx;
    CHECK(fx.load(dll));
    if (fx.set == nullptr) { return; }
    fx.reset();
    const std::string typed = "lib=" + fx.dir;
    const std::string id = std::string("open-") + key + ":" + typed;
    AppConfig file;
    {
        Rig r;
        std::printf("W1: opens through the engine, the tone is in the spectrum\n");
        CHECK_EQ(r.add(2, id, true).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.2.source") == key; }, 30000));
        CHECK(r.until([&] { return fx.get(kOpens) >= 1 && fx.get(kStarts) >= 1; }, 20000));
        CHECK(r.until([&] { return r.radio(2).sampleRateHz > 0.0; }, 10000));
        std::printf("  the radio runs at %.0f S/s; the library saw %lld open(s), %lld start(s)\n", r.radio(2).sampleRateHz, static_cast<long long>(fx.get(kOpens)), static_cast<long long>(fx.get(kStarts)));
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 7.010e6)).status(), FOXAPI_OK);   // the stand-in's tone is at 7.015 MHz absolute
        CHECK(toneIn(r, 2, 5000.0));

        std::printf("W2: one thread, no overlap, measured at the library\n");
        CHECK_EQ(fx.get(kThreads), static_cast<std::int64_t>(1));
        CHECK(fx.get(kOverlap) <= 1);

        std::printf("W3: a retune reaches the library; stop and remove end the stream and close the device\n");
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_CENTRE, 2, 7.012e6)).status(), FOXAPI_OK);
        CHECK(toneIn(r, 2, 3000.0));
        r.pump(20);
        file = throughTheFile(r.eng);
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 2, 0, 0)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return fx.get(kStops) >= 1; }, 10000));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return fx.get(kCloses) >= 1; }, 10000));
        CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
        CHECK_EQ(fx.get(kThreads), static_cast<std::int64_t>(1));
    }

    std::printf("W4: saved as the kind with the library folder, and comes back\n");
    CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
    if (file.radios.size() == 1) {
        CHECK_EQ(file.radios[0].sourceKind, std::string(key));
        CHECK_EQ(file.radios[0].args, typed);
        std::printf("  saved: kind %s, args %s\n", file.radios[0].sourceKind.c_str(), file.radios[0].args.c_str());
    }
    {
        const std::int64_t opensBefore = fx.get(kOpens);
        Rig y(&file);
        CHECK(y.until([&] { return y.fact("radio.2.source") == key; }, 30000));
        CHECK_EQ(y.fact("radio.restore"), std::string("<absent>"));
        CHECK(y.until([&] { return fx.get(kOpens) > opensBefore; }, 20000));
        CHECK_EQ(y.send(y.aimed(FOXAPI_OP_RUN, 2, 0, 1)).status(), FOXAPI_OK);
        CHECK(toneIn(y, 2, 3000.0));
    }

    std::printf("W5: a library that reports no receiver is refused\n");
    {
        fx.reset();
        fx.set(kMisbehave, kNoDevices);
        Rig r;
        const auto o = r.add(2, id, true);
        std::printf("  RADIO_ADD -> %d\n", o.status());
        r.until([&] { return r.fact("radio.2.refusal") != "<absent>"; }, 30000);
        const std::string why = r.fact("radio.2.refusal");
        std::printf("  refusal: %s\n", why.c_str());
        CHECK(!why.empty() && why != "<absent>");
        CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
        fx.reset();
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_winradio\n");
    engine_api_test::hermetic("wirebwinradio");
    Access::installHooks();
    runOne("g39ddc", "G39DDCAPI.dll", "WiNRADiO G39DDC");
    runOne("g33ddc", "G33DDCAPI.dll", "WiNRADiO G33DDC");
    return check::finish("test_engine_api_wireb_winradio");
}
