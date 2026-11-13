// test_engine_api_wireb_framelog.cpp - THE FRAME LOG THROUGH THE ENGINE (wire-b; FOXAPP_OP_FRAMELOG_* 0x891C-0x891D).
// Engine merge 3 (orchestrator decision D2, "no engine copies of the screen lane K traces") removed the eight-cursor and mark operations (0x8919-0x891B) that this file used to test
// together with the frame log; this file keeps the frame-log half of the original test_engine_api_wireb_cursors.cpp unchanged in substance.
//
//   F1  the frame log (filled by the test: no plugin call exists yet): READ shows the good frames, with the failed ones on request; keep-bad off drops the next failed frame but counts it; a new
//       capacity starts an empty log; refusals; nothing of it is in a fact until READ asks
//   G   a REMOTE session is DENIED
//   D   the dropped operations are gone: 0x8919, 0x891A and 0x891B are not operations any more
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <memory>
#include <string>

#include "engine/engine_wire_b.hpp"
#include "wire1_rig.hpp"

namespace {

std::string bigFact(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    return n >= 0 ? std::string(buf.c_str()) : std::string("<absent>");
}

testing::Outcome sendText(Rig& r, std::uint32_t op, std::uint32_t radio, const std::string& text, long long i0 = 0, long long i1 = 0, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, i0, nullptr);
    c.ival[1] = i1;
    c.target = radio;
    testing::Session& sess = who != nullptr ? *who : *r.s;
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = text.empty() ? api()->submit(sess.raw(), &c, 1, &sr) : api()->submit_text(sess.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 60 && !o.landed; ++pass) {
        r.eng.controlPass();
        FoxCommandResult res[16];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(sess.raw(), res, 16);
        for (int32_t i = 0; i < got; ++i) { if (res[i].ticket == sr.ticket) { o.landed = true; o.result = res[i]; } }
    }
    return o;
}

json fact(Rig& r, const std::string& key) {
    const json j = json::parse(bigFact(r, key), nullptr, false);
    CHECK(!j.is_discarded());
    return j.is_discarded() ? json::object() : j;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_framelog\n");
    engine_api_test::hermetic("wirebframelog");
    Access::installHooks();
    Rig r;

    std::printf("F1: the frame log\n");
    {
        auto& log = *r.eng.wireBStateForTest().frameLog;
        namespace sm = cascade::sigmeas;
        CHECK_EQ(r.fact("framelog"), std::string("<absent>"));   // nothing of it is a fact until READ asks
        for (int i = 0; i < 3; ++i) { log.push(sm::FrameRecord::withCrc(1.0 + i, 7.1e6, {0x01, 0x02, static_cast<std::uint8_t>(i)}, 0xBEEF, 0xBEEF)); }
        for (int i = 0; i < 2; ++i) { log.push(sm::FrameRecord::withCrc(10.0 + i, 7.1e6, {0xAA, static_cast<std::uint8_t>(i)}, 0x1234, 0x4321)); }
        const auto o = sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 0);
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(o.result.applied[0], 3.0);
        const json good = fact(r, "framelog");
        CHECK_EQ(good["frames"].size(), static_cast<std::size_t>(3));
        CHECK_EQ(good["held_bad"].get<std::size_t>(), static_cast<std::size_t>(2));
        CHECK_EQ(good["frames"][0]["hex"].get<std::string>(), std::string("010200"));
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 1).result.applied[0], 5.0);
        const json all = fact(r, "framelog");
        CHECK_EQ(all["frames"].size(), static_cast<std::size_t>(5));
        CHECK(!all["frames"][4]["crc_ok"].get<bool>());
        CHECK_EQ(all["frames"][4]["crc_received"].get<std::uint32_t>(), 0x1234u);
        // keep-bad off: the next failed frame is counted and not kept
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", 0, 0).status(), FOXAPI_OK);
        log.push(sm::FrameRecord::withCrc(20.0, 7.1e6, {0xCC}, 1, 2));
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 1).result.applied[0], 5.0);
        CHECK_EQ(fact(r, "framelog")["bad_total"].get<std::uint64_t>(), static_cast<std::uint64_t>(3));
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", 0, 0).status(), FOXAPI_NO_CHANGE);
        // a new capacity starts an empty log
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", -1, 2).status(), FOXAPI_OK);
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 1).result.applied[0], 0.0);
        auto& log2 = *r.eng.wireBStateForTest().frameLog;
        for (int i = 0; i < 4; ++i) { log2.push(sm::FrameRecord::withoutCrc(30.0 + i, 0.0, {static_cast<std::uint8_t>(i)})); }
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 0).result.applied[0], 2.0);   // the two newest of four
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", 2, 0).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", 0, 5000).status(), FOXAPI_OUT_OF_RANGE);
    }

    std::printf("G: REMOTE\n");
    {
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 1, 0, &remote).status(), FOXAPI_DENIED);
    }

    std::printf("D: the dropped cursor operations are not operations\n");
    {
        for (const std::uint32_t op : {0x8919u, 0x891Au, 0x891Bu}) {
            const auto o = sendText(r, op, 1, "{}");
            CHECK(o.submitStatus != FOXAPI_OK || o.status() != FOXAPI_OK);
        }
    }
    return check::finish("test_engine_api_wireb_framelog");
}
