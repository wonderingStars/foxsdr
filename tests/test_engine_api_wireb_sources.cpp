// test_engine_api_wireb_sources.cpp - THE WIRE-B SOURCE TABLE THROUGH THE ENGINE (engine/wire_b_sources.{hpp,cpp}): "open-<key>:<arguments>" for eleven network and vendor radios.
//
//   W1  the argument text: an address-first row takes the host with the key left out, keeps a key typed in full, refuses an empty address, an option typed first, a control character
//       and an over-long text; a device row takes options only and may take none; the table has every key once and no key the engine already owns
//   W2  every row through RADIO_ADD with nothing listening / nothing plugged in / no library: the radio is made, its source never opens, its refusal says why in the source's own words,
//       the radio stays on the generator, and the engine goes on (no hang, no dialog); the same row twice is refused BUSY by the device ledger; an empty address is BAD_ARGUMENT before a
//       slot is made
//   W3  a REMOTE session is DENIED every "open-<key>:" id on SELECT_SOURCE
//
// The success path of each source (stream arrives, tunes, stops) is the source's own test against its stand-in peer (test_ascp_source, test_hpsdr1_source, ...) and, through the engine, the
// per-source tests of this lane; a row with none is UNVERIFIED end to end and the record says so.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "engine/wire_b_sources.hpp"
#include "wire1_rig.hpp"

namespace wb = cascade::engine::wireb;

namespace {

void testArguments() {
    std::printf("W1: the argument text\n");
    std::size_t n = 0;
    const wb::SourceKind* rows = wb::sourceKinds(n);
    CHECK_EQ(n, static_cast<std::size_t>(12));   // engine merge 3: the eleventh row of wire-b plus the radio-bridges row "sdriq"
    std::set<std::string> keys;
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(keys.insert(rows[i].key).second);   // every key once
        CHECK(rows[i].make != nullptr);
        CHECK(rows[i].requestRateHz > 0.0);
        const std::string k = rows[i].key;
        // none is a key the engine owned before
        CHECK(k != "soapy" && k != "rtlsdr" && k != "pluto" && k != "rtltcp" && k != "kiwisdr" && k != "rawiq" && k != "file" && k != "siggen");
        const auto made = rows[i].make();
        CHECK(made != nullptr);
        if (made != nullptr) { CHECK_EQ(std::string(made->driverKey()), k); }   // the row's key is the source's own
    }
    const wb::SourceKind* rf = wb::findSourceByKey("rfspace");
    CHECK(rf != nullptr);
    if (rf == nullptr) { return; }
    std::string args, why;
    CHECK(wb::normaliseSourceArgs(*rf, "192.168.1.50:50000,rate=250000", args, why));
    CHECK_EQ(args, std::string("rfspace=192.168.1.50:50000,rate=250000"));
    CHECK(wb::normaliseSourceArgs(*rf, "  rfspace=host.example,timeout=500 ", args, why));
    CHECK_EQ(args, std::string("rfspace=host.example,timeout=500"));
    CHECK(!wb::normaliseSourceArgs(*rf, "", args, why));
    CHECK(why.find("address") != std::string::npos);
    CHECK(!wb::normaliseSourceArgs(*rf, "rfspace=", args, why));
    CHECK(!wb::normaliseSourceArgs(*rf, ",rate=5", args, why));
    CHECK(!wb::normaliseSourceArgs(*rf, "rate=250000,192.168.1.50", args, why));   // an option first: the address would be read as nothing
    CHECK(!wb::normaliseSourceArgs(*rf, std::string("host\x01x"), args, why));
    CHECK(!wb::normaliseSourceArgs(*rf, std::string(600, 'a'), args, why));
    const wb::SourceKind* ic = wb::findSourceByKey("icr8600");
    CHECK(ic != nullptr);
    if (ic != nullptr) {
        CHECK(wb::normaliseSourceArgs(*ic, "", args, why));
        CHECK_EQ(args, std::string());
        CHECK(wb::normaliseSourceArgs(*ic, "timeout=900", args, why));
        CHECK_EQ(args, std::string("timeout=900"));
    }
    const wb::SourceKind* sq = wb::findSourceByKey("sdriq");   // radio-bridges, registered in engine merge 3: options only, no address
    CHECK(sq != nullptr);
    if (sq != nullptr) {
        CHECK(!sq->needsAddress);
        CHECK_EQ(sq->requestRateHz, 196078.0);
        CHECK(wb::normaliseSourceArgs(*sq, "", args, why));
        CHECK_EQ(args, std::string());
        CHECK(wb::normaliseSourceArgs(*sq, "sdriq=AB12,timeout=900", args, why));
        CHECK_EQ(args, std::string("sdriq=AB12,timeout=900"));
    }
    CHECK(wb::isSourceOpenId("open-sdriq:"));
    CHECK(wb::isSourceOpenId("open-spyserver:host"));
    CHECK(!wb::isSourceOpenId("open-pluto:host"));
    CHECK(!wb::isSourceOpenId("spyserver:host"));
    CHECK(!wb::isSourceOpenId("open-spyserver"));
    CHECK(wb::findSourceByOpenWord("open-hpsdr2") != nullptr);
    CHECK(wb::findSourceByOpenWord("open-") == nullptr);
}

void testEveryRowRefuses() {
    std::printf("W2: every row with nothing there\n");
    Access::installHooks();
    Rig r;
    std::size_t n = 0;
    const wb::SourceKind* rows = wb::sourceKinds(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::string key = rows[i].key;
        const std::string id = rows[i].needsAddress ? "open-" + key + ":127.0.0.1:1,timeout=300" : "open-" + key + ":timeout=300";
        std::printf("  %s\n", id.c_str());
        const auto added = r.add(2, id, true);
        CHECK_EQ(added.status(), FOXAPI_OK);
        // the open answers on a later pass: the radio ends on the generator with a refusal that says why
        CHECK(r.until([&] { return r.fact("radio.2.refusal") != "<absent>" && !r.fact("radio.2.refusal").empty(); }, 25000));
        const std::string why = r.fact("radio.2.refusal");
        std::printf("    -> %.150s\n", why.c_str());
        CHECK(!why.empty());
        CHECK_EQ(r.fact("radio.2.source"), std::string("siggen"));
        CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));   // the first radio never noticed
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
    }
    // the same row twice: the device ledger refuses the second by its key
    {
        CHECK_EQ(r.add(2, "open-hpsdr1:127.0.0.1:9,timeout=300", false).status(), FOXAPI_OK);
        const auto again = r.add(3, "open-hpsdr1:127.0.0.1:9,timeout=300", false);
        CHECK_EQ(again.status(), FOXAPI_BUSY);
        // a typed key in full is the same radio
        CHECK_EQ(r.add(3, "open-hpsdr1:hpsdr1=127.0.0.1:9,timeout=300", false).status(), FOXAPI_BUSY);
        // another port is another radio
        CHECK_EQ(r.add(3, "open-hpsdr1:127.0.0.1:10,timeout=300", false).status(), FOXAPI_OK);
    }
    // an empty address is refused before a slot is made
    {
        const auto bad = r.add(4, "open-rfspace:", false);
        CHECK_EQ(bad.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(bad.result.message).find("address") != std::string::npos);
        CHECK_EQ(r.fact("radio.4.source"), std::string("<absent>"));
    }
}

void testRemoteDenied() {
    std::printf("W3: a REMOTE session cannot open them\n");
    Rig r;
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    std::size_t n = 0;
    const wb::SourceKind* rows = wb::sourceKinds(n);
    for (std::size_t i = 0; i < n; ++i) {
        const FoxCommand c = testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0.0, 0, (std::string("open-") + rows[i].key + ":127.0.0.1:1").c_str());
        // the refusal is delivered as a result (the API's rule 2), so the command is sent and its result read
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t got = api()->submit(remote.raw(), &c, 1, &sr);
        o.submitStatus = got < 0 ? got : sr.status;
        for (int pass = 0; pass < 40 && o.submitStatus == FOXAPI_OK && !o.landed; ++pass) {
            r.eng.controlPass();
            FoxCommandResult res[8];
            for (auto& x : res) { x.structSize = sizeof(x); }
            const int32_t k = api()->poll_results(remote.raw(), res, 8);
            for (int32_t j = 0; j < k; ++j) {
                if (res[j].ticket == sr.ticket) { o.landed = true; o.result = res[j]; }
            }
        }
        CHECK_EQ(o.status(), FOXAPI_DENIED);
    }
    CHECK_EQ(r.fact("radio.1.source"), std::string("siggen"));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_sources\n");
    engine_api_test::hermetic("wirebsources");
    testArguments();
    testEveryRowRefuses();
    testRemoteDenied();
    return check::finish("test_engine_api_wireb_sources");
}
