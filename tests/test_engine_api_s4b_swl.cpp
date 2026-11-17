// test_engine_api_s4b_swl.cpp - screen lane S4b, G3-49: the listening (SWL) log through the REAL engine: the engine adds the time, frequency, mode and level of the radio the person is listening to,
// the person's texts are cleaned and cut, the log is the person's own file beside the settings, the ADIF export goes to a full path they name ending .adi or .adif, and nothing of an entry is ever a fact.
// A stand-in radio (a tone source) is open; nothing leaves this computer.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <thread>

#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

constexpr double kPi = 3.14159265358979323846;

class Tone final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 240000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return center_; }
    bool setCenterFrequencyHz(double hz) override { center_ = hz; return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double a = 2.0 * kPi * 15000.0 * static_cast<double>(k_) / 240000.0;
            dst[i] = {static_cast<float>(0.3 * std::cos(a)), static_cast<float>(0.3 * std::sin(a))};
        }
        return n;
    }
    const char* name() const override { return "s4b-tone"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    double center_ = 100.0e6;
    std::uint64_t k_ = 0;
};

FoxCommand swl(const std::string& json) { return textOp(FOXAPP_OP_SWL, json); }

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_swl");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path dir = scratch("s4bswl");
    {
        Rig rig(dir);
        Access::useSource(rig.eng, std::make_unique<Tone>());
        CHECK(rig.status(testing::cmd(FOXAPI_OP_RUN, 0, 1)) == FOXAPI_OK);
        CHECK(rig.status(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)) == FOXAPI_OK);
        CHECK(rig.status(testing::cmd(FOXAPI_OP_SET_CENTRE, 100.0e6)) == FOXAPI_OK);
        CHECK(rig.status(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)) == FOXAPI_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        rig.pass(4);
        std::string msg;
        CHECK(rig.fact("swl.count") == "0" && rig.fact("swl.error").empty() && rig.fact("wirec.swl.operator").empty());
        // an entry: the engine adds the figures, the person the words
        CHECK(rig.status(swl(R"({"action":"add","call":"BBCWS","station":"World Service","notes":"clear signal"})")) == FOXAPI_OK);
        CHECK(rig.fact("swl.count") == "1");
        const std::string file = readAll(dir / "swl-log.json");
        CHECK(file.find("BBCWS") != std::string::npos && file.find("World Service") != std::string::npos && file.find("100000000") != std::string::npos);   // (the frequency the radio is tuned to)
        // an empty entry is a log of the frequency alone; control characters go, a long note is cut, unknown keys and non-text values are refused
        CHECK(rig.status(swl(R"({"action":"add"})")) == FOXAPI_OK);
        const std::string messy = std::string(R"({"action":"add","notes":"a)") + "\\" + "u0001b" + "\\" + "nc" + std::string(150, 'z') + "\"}";
        CHECK(rig.status(swl(messy)) == FOXAPI_OK);
        const std::string f2 = readAll(dir / "swl-log.json");
        CHECK(f2.find(std::string(151, 'z')) == std::string::npos && f2.find(std::string(150, 'z')) != std::string::npos && f2.find("\\u0001") == std::string::npos);
        CHECK(rig.status(swl(R"({"action":"add","freq":5})")) == FOXAPI_BAD_ARGUMENT && rig.status(swl(R"({"action":"add","call":5})")) == FOXAPI_BAD_ARGUMENT);
        CHECK(rig.status(swl("nope")) == FOXAPI_BAD_ARGUMENT && rig.status(swl(R"({"action":"dance"})")) == FOXAPI_BAD_ARGUMENT && rig.status(swl(R"({"action":"remove"})")) == FOXAPI_BAD_ARGUMENT);
        CHECK(rig.fact("swl.count") == "3");
        // nothing of an entry is a fact or a setting
        for (const char* k : {"swl.error", "swl.export.last", "swl.count"}) {
            const std::string v = rig.fact(k);
            CHECK(v.find("BBCWS") == std::string::npos && v.find("World Service") == std::string::npos && v.find("clear signal") == std::string::npos);
        }
        CHECK(readAll(dir / "wirec.json").find("BBCWS") == std::string::npos);
        // the listener's identifier and square
        CHECK(rig.status(setting("swl.operator", "G4XYZ")) == FOXAPI_OK && rig.status(setting("swl.grid", "IO91wm")) == FOXAPI_OK);
        CHECK(rig.status(setting("swl.grid", "ZZ99")) != FOXAPI_OK && rig.status(setting("swl.grid", "IO9")) != FOXAPI_OK && rig.status(setting("swl.operator", "a\tb")) != FOXAPI_OK);
        // export: a full path ending .adi or .adif only
        const fs::path out = dir / "heard.adi";
        const std::string outText = out.string();
        std::string esc;
        for (const char ch : outText) { if (ch == '\\') { esc += "\\\\"; } else { esc += ch; } }
        CHECK(rig.status(swl(R"({"action":"export","path":"heard.adi"})")) == FOXAPI_BAD_ARGUMENT);
        CHECK(rig.status(swl("{\"action\":\"export\",\"path\":\"" + esc + ".txt\"}")) == FOXAPI_BAD_ARGUMENT);
        CHECK(!fs::exists(dir / "heard.adi.txt"));
        CHECK(rig.status(swl("{\"action\":\"export\",\"path\":\"" + esc + "\"}")) == FOXAPI_OK);
        const std::string adi = readAll(out);
        CHECK(adi.find("ADIF") != std::string::npos && adi.find("SWL") != std::string::npos && adi.find("G4XYZ") != std::string::npos && (adi.find("IO91WM") != std::string::npos || adi.find("IO91wm") != std::string::npos));
        CHECK(rig.fact("swl.export.last").find("ADIF: 3 entries written") != std::string::npos);
        // removal and clearing
        CHECK(rig.status(swl(R"({"action":"remove","index":9})")) == FOXAPI_NOT_FOUND && rig.status(swl(R"({"action":"remove","index":0})")) == FOXAPI_NOT_FOUND);
        CHECK(rig.status(swl(R"({"action":"remove","index":1})")) == FOXAPI_OK && rig.fact("swl.count") == "2");
        CHECK(rig.status(swl(R"({"action":"clear"})")) == FOXAPI_OK && rig.fact("swl.count") == "0");
        CHECK(rig.status(swl(R"({"action":"clear"})")) == FOXAPI_NO_CHANGE);
        CHECK(rig.status(swl("{\"action\":\"export\",\"path\":\"" + esc + "\"}")) == FOXAPI_BAD_ARGUMENT);   // nothing to write
        CHECK(rig.status(swl(R"({"action":"add","call":"KEPT"})")) == FOXAPI_OK);
    }
    {
        Rig again(dir);   // a restart: the log is the person's own file and comes back
        CHECK(again.fact("swl.count") == "1");
    }
    {
        // the model directly: a text longer than the slot of an op is cut at 200 bytes, never inside a character
        cascade::engine::s4b::SwlBook book(dir / "direct.json");
        std::string why;
        const std::string long8 = std::string(199, 'a') + "Ã©" + std::string(100, 'b');   // an e-acute straddles the 200-byte cut
        const nlohmann::json j = {{"notes", long8}, {"call", std::string(500, 'c')}};
        CHECK(book.add(j.dump(), 1700000000, 7.1e6, "LSB", -60.0, 1, why));
        CHECK(book.entries().size() == 1 && book.entries()[0].notes.size() == 199 && book.entries()[0].call.size() == 200);
        CHECK(!book.add("{\"call\":\"x\"}", 1700000000, 0.0, "AM", 0.0, 1, why));   // no frequency, nothing to log
        CHECK(!book.add("[1]", 1700000000, 7.1e6, "AM", 0.0, 1, why) && !book.add("{\"a\":1}", 1700000000, 7.1e6, "AM", 0.0, 1, why));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::finish("test_engine_api_s4b_swl");
}
