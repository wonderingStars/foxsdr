// test_engine_api_wireb_savedkinds.cpp - A NETWORK RADIO OF THE WIRE-B TABLE IS SAVED IN THE CONFIGURATION AND COMES BACK (wire-b second pass, item 33), through a real Engine. The peer is the
// RFspace stand-in of tests/test_ascp_source.cpp (ASCP over TCP and the UDP I/Q stream on 127.0.0.1), included under a renamed main, the way test_engine_api_wireb_rfspace.cpp does; the model for
// the sections is test_engine_api_kiwisdr.cpp's D and E.
//
//   D  RADIO 2: opened through RADIO_ADD "open-rfspace:", saved (kind "rfspace" and the arguments as the open normalised them, the settings of the radio beside them, nothing a person could call a
//      secret), a restart from the file alone puts the radio back on the stand-in (the tone is in its spectrum), and the next save names it again
//   E  A PEER THAT IS NOT THERE: the generator stands in, fact.radio.restore says why, the file still names the radio
//   F  RADIO 1: SELECT_SOURCE "open-rfspace:" on the first radio saves as the plain sourceKind and nativeArgs, and comes back
//   G  a radio that was removed is not saved; a file whose kind was edited into nonsense loads the radio as the generator (and the rest of the file loads)
//
// UNVERIFIED on a real NetSDR / SDR-IP / Afedri. Nothing leaves this machine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define main wb_ascp_model_main
#include "test_ascp_source.cpp"
#undef main
#undef CHECK
#undef CHECK_NEAR
#undef CHECK_EQ
#include "engine/wire_b_sources.hpp"
#include "wire1_rig.hpp"

namespace {

std::string normalised(const std::string& typed) {
    const cascade::engine::wireb::SourceKind* row = cascade::engine::wireb::findSourceByKey("rfspace");
    std::string args, why;
    CHECK(row != nullptr && cascade::engine::wireb::normaliseSourceArgs(*row, typed, args, why));
    return args;
}

bool toneIn(Rig& r, std::size_t radio) {
    for (int i = 0; i < 60; ++i) {
        r.pump(10);
        if (peakAt(r.spectrum(static_cast<std::uint32_t>(radio)), 12345.0, 8)) { return true; }
    }
    return false;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_savedkinds\n");
    engine_api_test::hermetic("wirebsavedkinds");
    Access::installHooks();
    Radio radio{RadioSpec{}};
    CHECK(radio.start());
    const std::string want = normalised(radio.args());
    AppConfig file;

    std::printf("D: radio 2 on the RFspace stand-in is saved and comes back\n");
    {
        Rig x;
        CHECK_EQ(x.add(2, "open-rfspace:" + radio.args(), true).status(), FOXAPI_OK);
        CHECK_EQ(x.add(3, "").status(), FOXAPI_OK);
        CHECK(x.until([&] { return x.fact("radio.2.source") == "rfspace"; }, 30000));
        CHECK(toneIn(x, 2));
        CHECK_EQ(x.send(x.aimed(FOXAPI_OP_SET_CENTRE, 2, 7.1e6)).status(), FOXAPI_OK);
        CHECK_EQ(x.send(x.aimed(FOXAPI_OP_SET_VOLUME, 2, 0.37)).status(), FOXAPI_OK);
        x.pump(20);
        file = throughTheFile(x.eng);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(2));
        if (file.radios.size() == 2) {
            CHECK_EQ(file.radios[0].number, 2);
            CHECK_EQ(file.radios[0].sourceKind, std::string("rfspace"));
            CHECK_EQ(file.radios[0].args, want);
            CHECK_EQ(file.radios[1].sourceKind, std::string("siggen"));
        }
        const std::string text = ConfigStore::serialize(Access::snapshot(x.eng));
        CHECK(text.find("password") == std::string::npos && text.find("token") == std::string::npos);
        const json j = json::parse(text);
        CHECK_EQ(j["radios"][0]["sourceKind"].get<std::string>(), std::string("rfspace"));
        CHECK_EQ(j["sourceKind"].get<std::string>(), std::string("siggen"));
        std::printf("  saved: kind %s, args %s\n", file.radios[0].sourceKind.c_str(), file.radios[0].args.c_str());
    }
    const int onBefore = radio.onCount.load();
    {
        Rig y(&file);   // a restart from the file alone
        CHECK(y.until([&] { return y.fact("radio.2.source") == "rfspace"; }, 30000));
        CHECK_EQ(y.fact("radio.restore"), std::string("<absent>"));
        CHECK(y.until([&] { return radio.receiverOn() && radio.onCount.load() > onBefore; }, 20000));
        CHECK(toneIn(y, 2));
        CHECK(std::fabs(y.radio(2).volume - 0.37) < 1e-6);
        CHECK_EQ(y.radio(2).centreHz, 7100000.0);
        CHECK_EQ(Access::kind(y.eng, 1), std::string("rfspace"));
        const AppConfig again = throughTheFile(y.eng);
        CHECK_EQ(again.radios.size(), file.radios.size());
        if (!again.radios.empty()) {
            CHECK_EQ(again.radios[0].sourceKind, std::string("rfspace"));
            CHECK_EQ(again.radios[0].args, file.radios[0].args);
        }
    }

    std::printf("E: the peer is not there\n");
    radio.stop();
    {
        Rig z(&file);
        CHECK(z.until([&] { return z.fact("radio.restore") != "<absent>"; }, 60000));
        CHECK(z.fact("radio.restore").find("Radio 2:") != std::string::npos);
        std::printf("  restore note: %s\n", z.fact("radio.restore").c_str());
        CHECK_EQ(z.fact("radio.2.source"), std::string("siggen"));
        CHECK(z.fact("radio.2.refusal") != "<absent>");
        const AppConfig again = throughTheFile(z.eng);
        CHECK_EQ(again.radios.size(), file.radios.size());
        if (!again.radios.empty()) {
            CHECK_EQ(again.radios[0].sourceKind, std::string("rfspace"));   // the file still names the radio
            CHECK_EQ(again.radios[0].args, file.radios[0].args);
        }
    }

    std::printf("F: radio 1\n");
    Radio radio2{RadioSpec{}};
    CHECK(radio2.start());
    AppConfig one;
    {
        Rig r;
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SELECT_SOURCE, 0, 0.0, 0, ("open-rfspace:" + radio2.args()).c_str())).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.fact("radio.1.source") == "rfspace"; }, 30000));
        CHECK_EQ(r.send(r.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
        CHECK(toneIn(r, 1));
        one = throughTheFile(r.eng);
        CHECK_EQ(one.sourceKind, std::string("rfspace"));
        CHECK_EQ(one.nativeArgs, normalised(radio2.args()));
    }
    {
        Rig y(&one);
        CHECK(y.until([&] { return y.fact("radio.1.source") == "rfspace"; }, 30000));
        CHECK_EQ(y.send(y.aimed(FOXAPI_OP_RUN, 1, 0.0, 1)).status(), FOXAPI_OK);
        CHECK(toneIn(y, 1));
    }

    std::printf("G: a removed radio is not saved; a kind edited into nonsense is the generator\n");
    {
        Rig y(&file);   // (the peer of D is gone: radio 2 is on the generator and still named)
        CHECK(y.until([&] { return y.fact("radio.restore") != "<absent>"; }, 60000));
        CHECK_EQ(y.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        y.pump(10);
        const AppConfig gone = throughTheFile(y.eng);
        CHECK(gone.radios.empty() || gone.radios[0].sourceKind != "rfspace");
    }
    {
        AppConfig junk = file;
        junk.radios[0].sourceKind = "rfspac";
        AppConfig back;
        std::string error;
        CHECK(ConfigStore::parse(ConfigStore::serialize(junk), "test", back, error));
        CHECK_EQ(back.radios[0].sourceKind, std::string("siggen"));
    }
    radio2.stop();
    return check::finish("test_engine_api_wireb_savedkinds");
}
