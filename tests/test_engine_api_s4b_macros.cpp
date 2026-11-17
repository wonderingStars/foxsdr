// test_engine_api_s4b_macros.cpp - screen lane S4b, G3-34: saving and deleting macros (FOXAPP_OP_MACRO_EDIT) through the REAL engine, with the macro player of engine_macros.cpp and a clock the test owns.
// The macro file is a scratch file; nothing leaves this computer.
//
//   K1  a macro is saved, the engine reads it again (fact.macro.names), and playing it tunes the radio (a real engine command); saving the same name replaces it
//   K2  a macro the model refuses (an unknown command, a denied one - the rotator, the listening log, a setting, a macro op, a transmit command - a bad number, no steps, a bad name, an unknown key) is refused
//       WHOLE with the model's sentence and the file is not changed; a refused file on disk is not silently overwritten
//   K3  delete: the macro goes, an unknown name is NOT_FOUND; while a macro runs an edit is BUSY
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>

#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

// A long text op (a macro does not fit the 256-byte slot): submit_text, answered on the passes.
std::int32_t longOp(Rig& rig, std::uint32_t op, const std::string& text, std::string* msg = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, 0, nullptr);
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit_text(rig.local->raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    const std::int32_t sub = n < 0 ? n : sr.status;
    if (sub != FOXAPI_OK) { return sub; }
    for (int i = 0; i < 40; ++i) {
        rig.eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(rig.local->raw(), r, 16);
        for (int32_t k = 0; k < got; ++k) {
            if (r[k].ticket == sr.ticket) {
                if (msg != nullptr) { *msg = r[k].message; }
                return r[k].status;
            }
        }
    }
    return -9999;
}

std::string macroJson(const std::string& name, const std::string& stepsJson) { return "{\"action\":\"save\",\"name\":\"" + name + "\",\"steps\":" + stepsJson + "}"; }

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_macros");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = scratch("s4bmac");
    const fs::path file = dir / "macros.json";
    {
        Rig rig(dir, [](cascade::engine::wirec::State&) {});
        Access::useMacros(rig.eng, file.string());
        Access::macroClock(rig.eng, 1000);
        std::string msg;
        CHECK(rig.fact("macro.names").empty());
        // K1
        const std::string good = macroJson("Go to 40 m", R"([{"cmd":"SET_FREQUENCY","num":[7100000],"delayMs":200},{"cmd":"SET_MODE","ival":[7]}])");
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, good, &msg) == FOXAPI_OK);
        CHECK(fs::exists(file) && rig.fact("macro.names") == "Go to 40 m" && rig.fact("macro.error").empty());
        CHECK(rig.status(textOp(FOXAPP_OP_MACRO_RUN, "Go to 40 m")) == FOXAPI_OK);
        for (int i = 0; i < 10; ++i) { Access::macroClock(rig.eng, 1000 + 300 * (i + 1)); rig.pass(2); }
        CHECK(std::fabs(rig.local->state().tunedHz - 7.1e6) < 1.0 && rig.local->state().demodMode == FOXAPI_DEMOD_LSB);
        // the same name replaces
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, macroJson("Go to 40 m", R"([{"cmd":"SET_FREQUENCY","num":[7200000]}])")) == FOXAPI_OK);
        CHECK(rig.fact("macro.names") == "Go to 40 m");
        CHECK(rig.status(textOp(FOXAPP_OP_MACRO_RUN, "Go to 40 m")) == FOXAPI_OK);
        for (int i = 0; i < 6; ++i) { Access::macroClock(rig.eng, 5000 + 300 * (i + 1)); rig.pass(2); }
        CHECK(std::fabs(rig.local->state().tunedHz - 7.2e6) < 1.0);
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, macroJson("Second", R"([{"cmd":"SET_MODE","ival":[3]}])")) == FOXAPI_OK);
        CHECK(rig.fact("macro.names") == "Go to 40 m\nSecond");
        // every op the window's recorder keeps is a command the macro model knows by that name
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, macroJson("Names", R"([{"cmd":"SET_FREQUENCY","num":[7e6]},{"cmd":"SET_CENTRE","num":[7e6]},{"cmd":"SET_MODE","ival":[3]},{"cmd":"SET_BANDWIDTH","num":[2400]},{"cmd":"SET_SQUELCH","num":[-60]},{"cmd":"SET_VOLUME","num":[0.5]},{"cmd":"SET_MUTED","ival":[1]}])")) == FOXAPI_OK);
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, R"({"action":"delete","name":"Names"})") == FOXAPI_OK);
        const std::string before = readAll(file);

        // K2: refused whole, the file untouched
        const struct { const char* what; std::string json; } bad[] = {
            {"unknown command", macroJson("x", R"([{"cmd":"NO_SUCH_COMMAND"}])")},
            {"the rotator", macroJson("x", R"([{"cmd":"APP_ROTATOR"}])")},
            {"the listening log", macroJson("x", R"([{"cmd":"APP_SWL"}])")},
            {"a setting", macroJson("x", R"([{"cmd":"APP_WIREC_SETTING"}])")},
            {"a macro op", macroJson("x", R"([{"cmd":"APP_MACRO_EDIT"}])")},
            {"a transmit command", macroJson("x", R"([{"cmd":"TX_KEY"}])")},
            {"a number that is not", macroJson("x", R"([{"cmd":"SET_FREQUENCY","num":["a"]}])")},
            {"too many numbers", macroJson("x", R"([{"cmd":"SET_FREQUENCY","num":[1,2,3,4,5]}])")},
            {"a negative delay", macroJson("x", R"([{"cmd":"SET_FREQUENCY","num":[7e6],"delayMs":-5}])")},
            {"a step that is not an object", macroJson("x", R"([5])")},
            {"no steps", macroJson("x", "[]")},
            {"steps that are not a list", macroJson("x", "5")},
            {"an empty name", macroJson("", R"([{"cmd":"SET_MODE","ival":[3]}])")},
            {"a control character in the name", macroJson("a\\u0001b", R"([{"cmd":"SET_MODE","ival":[3]}])")},
            {"an unknown key", std::string(R"({"action":"save","name":"x","steps":[{"cmd":"SET_MODE","ival":[3]}],"run":true})")},
            {"an unknown action", std::string(R"({"action":"play","name":"x"})")},
            {"not json", "nope"},
        };
        for (const auto& b : bad) {
            msg.clear();
            const std::int32_t st = longOp(rig, FOXAPP_OP_MACRO_EDIT, b.json, &msg);
            CHECK(st == FOXAPI_BAD_ARGUMENT);
            if (st != FOXAPI_BAD_ARGUMENT) { std::printf("  accepted: %s -> %d %s\n", b.what, st, msg.c_str()); }
            CHECK(readAll(file) == before);
        }
        {
            // more than 500 steps
            std::string steps = "[";
            for (int i = 0; i < 501; ++i) { steps += std::string(i ? "," : "") + R"({"cmd":"SET_MODE","ival":[3]})"; }
            steps += "]";
            CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, macroJson("big", steps)) == FOXAPI_LIMIT);
            CHECK(readAll(file) == before);
        }
        // a file on disk the model refuses is not written over
        {
            Access::macroClock(rig.eng, 90000);
            const std::string saved = readAll(file);
            std::FILE* f = std::fopen(file.string().c_str(), "wb");
            std::fputs("{\"version\":1,\"macros\":[{\"name\":\"bad\",\"steps\":[{\"cmd\":\"TX_KEY\"}]}]}", f);
            std::fclose(f);
            msg.clear();
            CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, macroJson("y", R"([{"cmd":"SET_MODE","ival":[3]}])"), &msg) == FOXAPI_BAD_ARGUMENT && msg.find("refused") != std::string::npos);
            CHECK(readAll(file).find("TX_KEY") != std::string::npos);
            std::FILE* g = std::fopen(file.string().c_str(), "wb");
            std::fwrite(saved.data(), 1, saved.size(), g);
            std::fclose(g);
            CHECK(rig.status(textOp(FOXAPP_OP_MACRO_RELOAD, "")) == FOXAPI_OK && rig.fact("macro.names") == "Go to 40 m\nSecond");
        }

        // K3: BUSY while one runs, then delete
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, macroJson("Slow", R"([{"cmd":"SET_MODE","ival":[3],"delayMs":60000},{"cmd":"SET_MODE","ival":[2]}])")) == FOXAPI_OK);
        CHECK(rig.status(textOp(FOXAPP_OP_MACRO_RUN, "Slow")) == FOXAPI_OK);
        rig.pass(3);
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, macroJson("z", R"([{"cmd":"SET_MODE","ival":[3]}])")) == FOXAPI_BUSY);
        CHECK(rig.status(textOp(FOXAPP_OP_MACRO_STOP, "")) == FOXAPI_OK);
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, R"({"action":"delete","name":"Slow"})") == FOXAPI_OK);
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, R"({"action":"delete","name":"Slow"})") == FOXAPI_NOT_FOUND);
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, R"({"action":"delete","name":"Second","x":1})") == FOXAPI_BAD_ARGUMENT);
        CHECK(longOp(rig, FOXAPP_OP_MACRO_EDIT, R"({"action":"delete","name":"Second"})") == FOXAPI_OK);
        CHECK(rig.fact("macro.names") == "Go to 40 m");
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::finish("test_engine_api_s4b_macros");
}
