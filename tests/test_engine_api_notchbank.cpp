// test_engine_api_notchbank.cpp - THE MANUAL NOTCH BANK THROUGH THE ENGINE (engine wiring lane 1, group C).
//
// SET_NOTCH_SLOT 0x0117 (slot in num[2]; on in ival[0]; with ival[1] = 1 the frequency in num[0] and the -3 dB width in num[1]):
//   A  defaults (ten slots off at 1000 Hz and 50 Hz), each radio holds its own slots, the state carries mask / frequencies / widths, the pipeline's bank follows;
//   B  refusals at the API and the engine (slot, frequency, width, not a number) change nothing; a value the engine can clamp is clamped and flagged when it comes through the local path;
//   C  saved per radio (only slots that are on or were moved) and restored in an in-process restart, in the engine and in the pipelines; a damaged list is made safe at load.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "wire1_rig.hpp"

namespace {

FoxCommand slot(std::uint32_t target, int n, bool on, bool values, double hz = 0.0, double width = 0.0) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_NOTCH_SLOT, hz, on ? 1 : 0, nullptr);
    c.ival[1] = values ? 1 : 0;
    c.num[1] = width;
    c.num[2] = n;
    c.target = target;
    return c;
}

void testA() {
    std::printf("A: defaults, per radio, the state and the pipeline\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    for (std::uint32_t n : {1u, 2u}) {
        const FoxReceiverState s = r.radio(n);
        CHECK_EQ(s.notchBankMask, 0u);
        for (int i = 0; i < 10; ++i) {
            CHECK_EQ(s.notchBankHz[i], 1000.0);
            CHECK_EQ(s.notchBankWidthHz[i], 50.0);
        }
    }
    CHECK_EQ(r.send(slot(2, 3, true, true, 1234.5, 80.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).notchBankMask == (1u << 3); }, 5000));
    CHECK_EQ(r.radio(2).notchBankHz[3], 1234.5);
    CHECK_EQ(r.radio(2).notchBankWidthHz[3], 80.0);
    CHECK_EQ(r.radio(1).notchBankMask, 0u);
    CHECK_EQ(r.send(slot(1, 9, true, true, 400.0, 20.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).notchBankMask == (1u << 9); }, 5000));
    CHECK_EQ(r.radio(2).notchBankMask, 1u << 3);
    CHECK_EQ(Access::pipeline(r.eng, 0).manualNotchCount(), 1);
    CHECK_EQ(Access::pipeline(r.eng, 1).manualNotchCount(), 1);
    // switching off alone keeps the frequency and width (the slot is remembered)
    CHECK_EQ(r.send(slot(2, 3, false, false)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).notchBankMask == 0u; }, 5000));
    CHECK_EQ(r.radio(2).notchBankHz[3], 1234.5);
    CHECK_EQ(Access::pipeline(r.eng, 1).manualNotchCount(), 0);
    CHECK_EQ(r.send(slot(9, 0, true, false)).status(), FOXAPI_NO_DEVICE);
}

void testB() {
    std::printf("B: refusals and clamping\n");
    Rig r;
    CHECK_EQ(r.send(slot(1, 0, true, true, 1500.0, 40.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).notchBankMask == 1u; }, 5000));
    const FoxReceiverState before = r.radio(1);
    struct Bad { int slot; double hz, width; const char* why; };
    const Bad bad[] = {
        {10, 1000.0, 50.0, "a slot past 9"}, {-1, 1000.0, 50.0, "a negative slot"}, {0, 19.0, 50.0, "a frequency under 20 Hz"},
        {0, 21601.0, 50.0, "a frequency over 21600 Hz"}, {0, 1000.0, 0.5, "a width under 1 Hz"}, {0, 1000.0, 2001.0, "a width over 2000 Hz"},
        {0, std::nan(""), 50.0, "a frequency that is not a number"},
    };
    for (const Bad& b : bad) {
        const testing::Outcome o = r.send(slot(1, b.slot, true, true, b.hz, b.width));
        if (o.status() != FOXAPI_OUT_OF_RANGE && o.status() != FOXAPI_BAD_ARGUMENT) { std::printf("  %s answered %d\n", b.why, o.status()); }
        CHECK(o.status() == FOXAPI_OUT_OF_RANGE || o.status() == FOXAPI_BAD_ARGUMENT);
    }
    r.pump(10);
    CHECK_EQ(r.radio(1).notchBankMask, before.notchBankMask);
    CHECK_EQ(r.radio(1).notchBankHz[0], 1500.0);
    CHECK_EQ(r.radio(1).notchBankWidthHz[0], 40.0);
    // the engine's own path clamps and says so
    const FoxCommandResult lo = r.eng.applyCommand(slot(1, 0, true, true, 5.0, 40.0), std::string());
    CHECK_EQ(lo.status, FOXAPI_OK);
    CHECK((lo.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    CHECK_EQ(lo.applied[0], 20.0);
    const FoxCommandResult hi = r.eng.applyCommand(slot(1, 0, true, true, 1500.0, 9000.0), std::string());
    CHECK((hi.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    CHECK_EQ(hi.applied[1], 2000.0);
    const FoxCommandResult badSlot = r.eng.applyCommand(slot(1, 12, true, false), std::string());
    CHECK_EQ(badSlot.status, FOXAPI_OUT_OF_RANGE);
}

void testC() {
    std::printf("C: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(slot(1, 0, true, true, 1000.0, 25.0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(slot(1, 5, false, true, 2500.0, 100.0)).status(), FOXAPI_OK);   // moved, off
        CHECK_EQ(a.send(slot(2, 9, true, true, 700.0, 10.0)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.notchBank.size(), static_cast<std::size_t>(2));
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].notchBank.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].notchBank[0].slot, 9);
        const json j = json::parse(ConfigStore::serialize(Access::snapshot(a.eng)));
        CHECK_EQ(j["notchBank"].size(), static_cast<std::size_t>(2));
        CHECK_EQ(j["radios"][0]["notchBank"][0]["hz"].get<double>(), 700.0);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).notchBankMask, 1u);
        CHECK_EQ(b.radio(1).notchBankHz[5], 2500.0);
        CHECK_EQ(b.radio(1).notchBankWidthHz[0], 25.0);
        CHECK_EQ(b.radio(2).notchBankMask, 1u << 9);
        CHECK_EQ(b.radio(2).notchBankHz[9], 700.0);
        CHECK_EQ(Access::pipeline(b.eng, 0).manualNotchCount(), 1);
        CHECK_EQ(Access::pipeline(b.eng, 1).manualNotchCount(), 1);
    }
    // a damaged list: slots out of range and repeated, numbers out of range, entries that are not objects
    {
        AppConfig back;
        std::string error;
        const std::string text = "{\"notchBank\":[{\"slot\":0,\"hz\":5,\"widthHz\":9999,\"on\":true},{\"slot\":0,\"hz\":2000},{\"slot\":11},5,{\"slot\":2,\"hz\":3000,\"widthHz\":30,\"on\":true}]}";
        CHECK(ConfigStore::parse(text, "test", back, error));
        CHECK_EQ(back.notchBank.size(), static_cast<std::size_t>(2));
        CHECK_EQ(back.notchBank[0].slot, 0);
        CHECK_EQ(back.notchBank[0].hz, 20.0);
        CHECK_EQ(back.notchBank[0].widthHz, 2000.0);
        CHECK_EQ(back.notchBank[1].slot, 2);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_notchbank\n");
    engine_api_test::hermetic("notchbank");
    Access::installHooks();
    testA();
    testB();
    testC();
    return check::finish("test_engine_api_notchbank");
}
