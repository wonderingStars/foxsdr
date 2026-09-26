// test_control_ops.cpp - EVERY ControlRequest field maps to an engine command,
// in the order the fields were always applied (engine extraction stage 1,
// docs/engine-stage1.md).
//
// WHAT IS PINNED.
//
//   1. THE TABLE. One row per ControlRequest field (and per pair that is only
//      meaningful as a pair: sourceKind+soapyArgs, gainName+gainDb,
//      pluginTuneName+pluginTuneAllowed, pluginPresetName+pluginPresetIndex).
//      A request carrying just that field must translate to exactly the
//      listed ops, with the listed slots.
//
//   2. EVERY FIELD HAS A ROW. src/net/web_control.hpp is read and every
//      `std::optional<...> name;` member of ControlRequest must be named by a
//      row. A field added to the web remote without a mapping to a command
//      fails here, rather than being silently ignored by applyControlRequest.
//
//   3. THE ORDER. A request carrying everything comes out in the historical
//      order: run state, centre, mode, bandwidth, offset, ... source, device
//      settings, recorder, bookmarks, scanner range, plugins, scanner run
//      state, and the transmit key LAST.
//
//   4. THE PLUGIN CONTROLS. Every PluginControl kind maps to the command the
//      desktop control of that kind sends.
//
// Pure: no window, no receiver. Links the engine alone.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/app_commands.hpp"
#include "net/control_ops.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace cmd = cascade::core::cmd;
using cascade::net::ControlRequest;
using cascade::net::ControlOpsContext;

// One expected command: op, and the slots that matter for it.
struct Want {
    std::uint32_t op = 0;
    std::int64_t i0 = 0;
    std::int64_t i1 = 0;
    double n0 = 0.0;
    double n1 = 0.0;
    double n2 = 0.0;
    const char* text = "";
};

struct Row {
    std::vector<const char*> fields;  // the ControlRequest member(s) this row sets
    std::function<void(ControlRequest&)> set;
    std::vector<Want> want;
};

ControlOpsContext context() {
    ControlOpsContext ctx;
    ctx.bookmarkIdByRow = {11, 22, 33};
    ctx.scanStartHz = 144.0e6;
    ctx.scanStopHz = 146.0e6;
    ctx.scanStepHz = 12.5e3;
    return ctx;
}

bool matches(const cmd::QueuedCommand& q, const Want& w) {
    const std::string text = cmd::textOf(q.c, q.longText);
    return q.c.op == w.op && q.c.ival[0] == w.i0 && q.c.ival[1] == w.i1 && q.c.num[0] == w.n0 &&
           q.c.num[1] == w.n1 && q.c.num[2] == w.n2 && text == w.text &&
           q.c.structSize == sizeof(FoxCommand);
}

void printCommand(const cmd::QueuedCommand& q) {
    std::printf("        got %s i0=%lld i1=%lld n0=%g n1=%g n2=%g text=\"%s\"\n",
                cmd::opName(q.c.op), static_cast<long long>(q.c.ival[0]),
                static_cast<long long>(q.c.ival[1]), q.c.num[0], q.c.num[1], q.c.num[2],
                cmd::textOf(q.c, q.longText).c_str());
}

std::vector<Row> table() {
    std::vector<Row> t;
    const auto row = [&t](std::vector<const char*> f, std::function<void(ControlRequest&)> s,
                          std::vector<Want> w) { t.push_back({std::move(f), std::move(s), std::move(w)}); };
    // The receiver.
    row({"running"}, [](ControlRequest& r) { r.running = false; }, {{FOXAPI_OP_RUN, 0}});
    row({"centerHz"}, [](ControlRequest& r) { r.centerHz = 101.1e6; },
        {{FOXAPI_OP_SET_CENTRE, 0, 0, 101.1e6}});
    row({"vfoOffsetHz"}, [](ControlRequest& r) { r.vfoOffsetHz = -25e3; },
        {{FOXAPI_OP_SET_VFO_OFFSET, 0, 0, -25e3}});
    row({"mode"}, [](ControlRequest& r) { r.mode = cascade::dsp::DemodMode::LSB; },
        {{FOXAPI_OP_SET_MODE, FOXAPI_DEMOD_LSB}});
    row({"bandwidthHz"}, [](ControlRequest& r) { r.bandwidthHz = 40e3; },
        {{FOXAPI_OP_SET_BANDWIDTH, 0, 0, 40e3}});
    row({"squelchDb"}, [](ControlRequest& r) { r.squelchDb = -42.0; },
        {{FOXAPI_OP_SET_SQUELCH, 0, 0, -42.0}});
    row({"volume"}, [](ControlRequest& r) { r.volume = 0.25; }, {{FOXAPI_OP_SET_VOLUME, 0, 0, 0.25}});
    // The display range: both, or the one end that moved.
    row({"dbMin", "dbMax"}, [](ControlRequest& r) { r.dbMin = -110.0; r.dbMax = -30.0; },
        {{FOXAPI_OP_SET_DISPLAY_RANGE, 0, 0, -110.0, -30.0}});
    row({"dbMin"}, [](ControlRequest& r) { r.dbMin = -110.0; }, {{FOXAPP_OP_SET_DISPLAY_MIN, 0, 0, -110.0}});
    row({"dbMax"}, [](ControlRequest& r) { r.dbMax = -30.0; }, {{FOXAPP_OP_SET_DISPLAY_MAX, 0, 0, -30.0}});
    row({"deemphasisIndex"}, [](ControlRequest& r) { r.deemphasisIndex = 1; },
        {{FOXAPI_OP_SET_DEEMPHASIS, 1}});
    row({"stereoEnabled"}, [](ControlRequest& r) { r.stereoEnabled = false; }, {{FOXAPI_OP_SET_STEREO, 0}});
    // Noise reduction: the switch alone, the strength alone, both as one.
    row({"nrEnabled"}, [](ControlRequest& r) { r.nrEnabled = true; }, {{FOXAPI_OP_SET_NR, 1, 0}});
    row({"nrStrength"}, [](ControlRequest& r) { r.nrStrength = 0.6; },
        {{FOXAPP_OP_SET_NR_STRENGTH, 0, 0, 0.6}});
    row({"nrEnabled", "nrStrength"}, [](ControlRequest& r) { r.nrEnabled = true; r.nrStrength = 0.6; },
        {{FOXAPI_OP_SET_NR, 1, 1, 0.6}});
    // The notch: every combination the fields allow.
    row({"notchEnabled"}, [](ControlRequest& r) { r.notchEnabled = true; }, {{FOXAPI_OP_SET_NOTCH, 1, 0}});
    row({"notchFreqHz"}, [](ControlRequest& r) { r.notchFreqHz = 1000.0; },
        {{FOXAPP_OP_SET_NOTCH_FREQUENCY, 0, 0, 1000.0}});
    row({"notchQ"}, [](ControlRequest& r) { r.notchQ = 25.0; }, {{FOXAPP_OP_SET_NOTCH_Q, 0, 0, 25.0}});
    row({"notchEnabled", "notchFreqHz", "notchQ"},
        [](ControlRequest& r) { r.notchEnabled = false; r.notchFreqHz = 1000.0; r.notchQ = 25.0; },
        {{FOXAPI_OP_SET_NOTCH, 0, 1, 1000.0, 25.0}});
    row({"notchEnabled", "notchQ"}, [](ControlRequest& r) { r.notchEnabled = true; r.notchQ = 25.0; },
        {{FOXAPI_OP_SET_NOTCH, 1, 0}, {FOXAPP_OP_SET_NOTCH_Q, 0, 0, 25.0}});
    row({"autoNotch"}, [](ControlRequest& r) { r.autoNotch = true; }, {{FOXAPI_OP_SET_AUTO_NOTCH, 1}});
    // The source.
    row({"scanDevices"}, [](ControlRequest& r) { r.scanDevices = true; }, {{FOXAPI_OP_SCAN_DEVICES}});
    row({"sourceKind"}, [](ControlRequest& r) { r.sourceKind = "siggen"; },
        {{FOXAPI_OP_SELECT_SOURCE, 0, 0, 0, 0, 0, "siggen"}});
    row({"sourceKind", "soapyArgs"},
        [](ControlRequest& r) { r.sourceKind = "soapy"; r.soapyArgs = "driver=uhd,serial=31E0000"; },
        {{FOXAPI_OP_SELECT_SOURCE, 0, 0, 0, 0, 0, "soapy:driver=uhd,serial=31E0000"}});
    row({"antenna"}, [](ControlRequest& r) { r.antenna = "RX2"; },
        {{FOXAPI_OP_SET_ANTENNA, 0, 0, 0, 0, 0, "RX2"}});
    row({"sampleRateHz"}, [](ControlRequest& r) { r.sampleRateHz = 2.4e6; },
        {{FOXAPI_OP_SET_SAMPLE_RATE, 0, 0, 2.4e6}});
    row({"gainName", "gainDb"}, [](ControlRequest& r) { r.gainName = "LNA"; r.gainDb = 21.0; },
        {{FOXAPI_OP_SET_GAIN, 0, 0, 21.0, 0, 0, "LNA"}});
    row({"agc"}, [](ControlRequest& r) { r.agc = true; }, {{FOXAPI_OP_SET_DEVICE_AGC, 1}});
    // The recorder.
    row({"recordIq"}, [](ControlRequest& r) { r.recordIq = true; }, {{FOXAPI_OP_RECORD_IQ, 1}});
    row({"recordAudio"}, [](ControlRequest& r) { r.recordAudio = false; }, {{FOXAPI_OP_RECORD_AUDIO, 0}});
    // Bookmarks: rows of the published list become ids; a row that is gone
    // becomes id 0, which applyCommand refuses as NOT_FOUND.
    row({"bookmarkAdd"}, [](ControlRequest& r) { r.bookmarkAdd = "Tower"; },
        {{FOXAPI_OP_BOOKMARK_ADD, 0, 0, 0, 0, 0, "Tower"}});
    row({"bookmarkTune"}, [](ControlRequest& r) { r.bookmarkTune = 1; }, {{FOXAPI_OP_BOOKMARK_TUNE, 22}});
    row({"bookmarkRemove"}, [](ControlRequest& r) { r.bookmarkRemove = 7; },
        {{FOXAPI_OP_BOOKMARK_REMOVE, 0}});
    // The scanner: the range fields are stored (mask), the run state carries
    // the stored range with this request's fields merged in.
    row({"scanStartHz"}, [](ControlRequest& r) { r.scanStartHz = 150e6; },
        {{FOXAPP_OP_SCANNER_RANGE, 1, 0, 150e6}});
    row({"scanStopHz"}, [](ControlRequest& r) { r.scanStopHz = 151e6; },
        {{FOXAPP_OP_SCANNER_RANGE, 2, 0, 0, 151e6}});
    row({"scanStepHz"}, [](ControlRequest& r) { r.scanStepHz = 25e3; },
        {{FOXAPP_OP_SCANNER_RANGE, 4, 0, 0, 0, 25e3}});
    row({"scannerActive"}, [](ControlRequest& r) { r.scannerActive = true; },
        {{FOXAPI_OP_SCANNER_RUN, 1, 0, 144.0e6, 146.0e6, 12.5e3}});
    row({"scannerActive", "scanStartHz"},
        [](ControlRequest& r) { r.scannerActive = true; r.scanStartHz = 150e6; },
        {{FOXAPP_OP_SCANNER_RANGE, 1, 0, 150e6}, {FOXAPI_OP_SCANNER_RUN, 1, 0, 150e6, 146.0e6, 12.5e3}});
    row({"scannerSkip"}, [](ControlRequest& r) { r.scannerSkip = true; }, {{FOXAPI_OP_SCANNER_SKIP}});
    // Plugins and the store.
    row({"pluginFetch"}, [](ControlRequest& r) { r.pluginFetch = true; }, {{FOXAPI_OP_STORE_FETCH}});
    row({"pluginInstall"}, [](ControlRequest& r) { r.pluginInstall = "adsb"; },
        {{FOXAPI_OP_STORE_INSTALL, 0, 0, 0, 0, 0, "adsb"}});
    row({"pluginInstall", "acknowledgeNotice"},
        [](ControlRequest& r) { r.pluginInstall = "adsb"; r.acknowledgeNotice = true; },
        {{FOXAPI_OP_STORE_INSTALL, 1, 0, 0, 0, 0, "adsb"}});
    row({"pluginRemove"}, [](ControlRequest& r) { r.pluginRemove = "adsb-decoder-1.8.0.dll"; },
        {{FOXAPI_OP_STORE_REMOVE, 0, 0, 0, 0, 0, "adsb-decoder-1.8.0.dll"}});
    row({"pluginTuneName", "pluginTuneAllowed"},
        [](ControlRequest& r) { r.pluginTuneName = "sat.dll"; r.pluginTuneAllowed = true; },
        {{FOXAPI_OP_PLUGIN_GRANT, 1, 1, 0, 0, 0, "sat.dll"}});
    row({"pluginPresetName", "pluginPresetIndex"},
        [](ControlRequest& r) { r.pluginPresetName = "ADS-B"; r.pluginPresetIndex = 2; },
        {{FOXAPI_OP_PLUGIN_PRESET, 2, 0, 0, 0, 0, "ADS-B"}});
    // The remote transmit key.
    row({"transmitPtt"}, [](ControlRequest& r) { r.transmitPtt = true; }, {{FOXAPI_OP_TX_PTT, 1}});
    return t;
}

void everyRowTranslates() {
    std::printf("  [1] each field (or pair) translates to exactly its commands\n");
    for (const Row& r : table()) {
        ControlRequest req;
        r.set(req);
        const std::vector<cmd::QueuedCommand> got = cascade::net::controlRequestToCommands(req, context());
        bool ok = got.size() == r.want.size();
        for (std::size_t i = 0; ok && i < got.size(); ++i) { ok = matches(got[i], r.want[i]); }
        std::string name;
        for (const char* f : r.fields) { name += (name.empty() ? "" : "+") + std::string(f); }
        if (!ok) {
            std::printf("FAIL: %s -> %zu command(s), want %zu\n", name.c_str(), got.size(),
                        r.want.size());
            for (const cmd::QueuedCommand& q : got) { printCommand(q); }
        }
        CHECK(ok);
        // And the translation names only ops applyCommand implements.
        for (const cmd::QueuedCommand& q : got) { CHECK(cmd::isKnownOp(q.c.op)); }
    }
}

// The ControlRequest members, read from the header the web remote is parsed
// into - so a field added there without a row here is caught.
std::vector<std::string> controlRequestFields() {
    const std::filesystem::path h =
        std::filesystem::path(CASCADE_SOURCE_DIR) / "src" / "net" / "web_control.hpp";
    std::ifstream in(h, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    std::vector<std::string> out;
    const std::size_t from = text.find("struct ControlRequest {");
    const std::size_t to = text.find("bool empty() const", from);
    if (from == std::string::npos || to == std::string::npos) { return out; }
    std::istringstream lines(text.substr(from, to - from));
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t opt = line.find("std::optional<");
        if (opt == std::string::npos) { continue; }
        const std::size_t comment = line.find("//");
        if (comment != std::string::npos && comment < opt) { continue; }
        std::size_t depth = 0;
        std::size_t i = opt + std::strlen("std::optional");
        for (; i < line.size(); ++i) {
            if (line[i] == '<') { ++depth; }
            if (line[i] == '>' && --depth == 0) { ++i; break; }
        }
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) { ++i; }
        std::size_t e = i;
        while (e < line.size() && (std::isalnum(static_cast<unsigned char>(line[e])) || line[e] == '_')) { ++e; }
        if (e > i) { out.push_back(line.substr(i, e - i)); }
    }
    return out;
}

void everyFieldHasARow() {
    std::printf("  [2] every ControlRequest field in web_control.hpp has a row\n");
    const std::vector<std::string> fields = controlRequestFields();
    std::printf("      %zu fields read from the header\n", fields.size());
    CHECK(fields.size() >= 40u);  // the header was found and read
    std::set<std::string> covered;
    for (const Row& r : table()) {
        for (const char* f : r.fields) { covered.insert(f); }
    }
    for (const std::string& f : fields) {
        if (covered.count(f) == 0) { std::printf("FAIL: ControlRequest::%s has no command row\n", f.c_str()); }
        CHECK(covered.count(f) != 0);
    }
    // And no row names a field the struct does not have.
    const std::set<std::string> real(fields.begin(), fields.end());
    for (const std::string& f : covered) {
        if (real.count(f) == 0) { std::printf("FAIL: row names ControlRequest::%s, which does not exist\n", f.c_str()); }
        CHECK(real.count(f) != 0);
    }
}

void orderIsTheHistoricalOrder() {
    std::printf("  [3] a request carrying everything keeps the historical order\n");
    ControlRequest r;
    r.transmitPtt = false;  // set first, must come out last
    r.scannerActive = false;
    r.pluginPresetName = "ADS-B";
    r.pluginPresetIndex = 0;
    r.pluginTuneName = "x.dll";
    r.pluginTuneAllowed = false;
    r.pluginRemove = "y.dll";
    r.pluginInstall = "z";
    r.pluginFetch = true;
    r.scanStartHz = 1.0e6;
    r.bookmarkRemove = 0;
    r.bookmarkTune = 0;
    r.bookmarkAdd = "b";
    r.recordAudio = true;
    r.recordIq = true;
    r.agc = false;
    r.gainName = "LNA";
    r.gainDb = 1.0;
    r.sampleRateHz = 2.0e6;
    r.antenna = "A";
    r.sourceKind = "siggen";
    r.scanDevices = true;
    r.autoNotch = false;
    r.nrEnabled = false;
    r.stereoEnabled = true;
    r.deemphasisIndex = 0;
    r.dbMin = -100.0;
    r.volume = 0.5;
    r.squelchDb = -60.0;
    r.vfoOffsetHz = 0.0;
    r.bandwidthHz = 12.5e3;
    r.mode = cascade::dsp::DemodMode::NFM;
    r.centerHz = 100e6;
    r.running = true;
    const std::vector<std::uint32_t> want = {
        FOXAPI_OP_RUN,           FOXAPI_OP_SET_CENTRE,     FOXAPI_OP_SET_MODE,
        FOXAPI_OP_SET_BANDWIDTH, FOXAPI_OP_SET_VFO_OFFSET, FOXAPI_OP_SET_SQUELCH,
        FOXAPI_OP_SET_VOLUME,    FOXAPP_OP_SET_DISPLAY_MIN, FOXAPI_OP_SET_DEEMPHASIS,
        FOXAPI_OP_SET_STEREO,    FOXAPI_OP_SET_NR,         FOXAPI_OP_SET_AUTO_NOTCH,
        FOXAPI_OP_SCAN_DEVICES,  FOXAPI_OP_SELECT_SOURCE,  FOXAPI_OP_SET_ANTENNA,
        FOXAPI_OP_SET_SAMPLE_RATE, FOXAPI_OP_SET_GAIN,     FOXAPI_OP_SET_DEVICE_AGC,
        FOXAPI_OP_RECORD_IQ,     FOXAPI_OP_RECORD_AUDIO,   FOXAPI_OP_BOOKMARK_ADD,
        FOXAPI_OP_BOOKMARK_TUNE, FOXAPI_OP_BOOKMARK_REMOVE, FOXAPP_OP_SCANNER_RANGE,
        FOXAPI_OP_STORE_FETCH,   FOXAPI_OP_STORE_INSTALL,  FOXAPI_OP_STORE_REMOVE,
        FOXAPI_OP_PLUGIN_GRANT,  FOXAPI_OP_PLUGIN_PRESET,  FOXAPI_OP_SCANNER_RUN,
        FOXAPI_OP_TX_PTT};
    const std::vector<cmd::QueuedCommand> got = cascade::net::controlRequestToCommands(r, context());
    CHECK(got.size() == want.size());
    for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) {
        if (got[i].c.op != want[i]) {
            std::printf("FAIL: command %zu is %s, want %s\n", i, cmd::opName(got[i].c.op),
                        cmd::opName(want[i]));
        }
        CHECK(got[i].c.op == want[i]);
    }
    // An empty request is nothing at all.
    CHECK(cascade::net::controlRequestToCommands(ControlRequest{}, context()).empty());
}

void longTextTravels() {
    std::printf("  [4] text longer than FoxCommand::text travels whole\n");
    ControlRequest r;
    r.sourceKind = "soapy";
    r.soapyArgs = std::string(400, 'a');
    const std::vector<cmd::QueuedCommand> got = cascade::net::controlRequestToCommands(r, context());
    CHECK(got.size() == 1u);
    if (!got.empty()) {
        CHECK(cmd::textOf(got[0].c, got[0].longText) == "soapy:" + std::string(400, 'a'));
        CHECK(std::strlen(got[0].c.text) == FOXAPI_TEXT_CHARS - 1);  // truncated, NUL-terminated
    }
}

void pluginControlsMap() {
    std::printf("  [5] every plugin control kind maps to the desktop's command\n");
    using K = cascade::core::PluginControl::Kind;
    struct Case {
        K kind;
        std::uint32_t op;
    };
    const Case cases[] = {
        {K::Frequency, FOXAPI_OP_SET_FREQUENCY}, {K::VfoOffset, FOXAPI_OP_SET_VFO_OFFSET},
        {K::Mode, FOXAPI_OP_SET_MODE},           {K::Bandwidth, FOXAPI_OP_SET_BANDWIDTH},
        {K::Squelch, FOXAPI_OP_SET_SQUELCH},     {K::SampleRate, FOXAPI_OP_SET_SAMPLE_RATE},
        {K::Gain, FOXAPI_OP_SET_GAIN},           {K::DeviceAgc, FOXAPI_OP_SET_DEVICE_AGC},
        {K::Running, FOXAPI_OP_RUN},             {K::Volume, FOXAPI_OP_SET_VOLUME},
        {K::Muted, FOXAPI_OP_SET_MUTED},
    };
    CHECK(sizeof(cases) / sizeof(cases[0]) == static_cast<std::size_t>(K::Muted) + 1u);
    for (const Case& k : cases) {
        cascade::core::PluginControl c;
        c.kind = k.kind;
        c.value = 123.0;
        c.mode = FOXAPI_DEMOD_AM;
        c.flag = true;
        std::snprintf(c.gainName, sizeof(c.gainName), "%s", "VGA");
        FoxCommand out{};
        CHECK(cascade::net::pluginControlToCommand(c, out));
        CHECK(out.op == k.op);
        CHECK(out.structSize == sizeof(FoxCommand));
        if (k.kind == K::Mode) { CHECK(out.ival[0] == FOXAPI_DEMOD_AM); }
        if (k.kind == K::Gain) {
            CHECK(std::string(out.text) == "VGA");
            CHECK(out.num[0] == 123.0);
        }
        if (k.kind == K::Running || k.kind == K::Muted || k.kind == K::DeviceAgc) {
            CHECK(out.ival[0] == 1);
        }
        if (k.kind == K::Frequency || k.kind == K::Volume || k.kind == K::Squelch) {
            CHECK(out.num[0] == 123.0);
        }
    }
}

void modeNamesAreTheApiOrder() {
    std::printf("  [6] mode names map to FOXAPI_DEMOD_* 1..8 in the mode keys' order\n");
    const char* names[8] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
    const std::uint32_t api[8] = {FOXAPI_DEMOD_NFM, FOXAPI_DEMOD_WFM, FOXAPI_DEMOD_AM,
                                  FOXAPI_DEMOD_DSB, FOXAPI_DEMOD_USB, FOXAPI_DEMOD_CW,
                                  FOXAPI_DEMOD_LSB, FOXAPI_DEMOD_RAW};
    for (int i = 0; i < 8; ++i) {
        CHECK(cmd::foxDemodFromName(names[i]) == api[i]);
        CHECK(api[i] == static_cast<std::uint32_t>(i + 1));
    }
    CHECK(cmd::foxDemodFromName("FM") == 0u);
    CHECK(cmd::foxDemodFromName(nullptr) == 0u);
    // Every DemodMode's own name maps somewhere (the web's vocabulary).
    for (std::size_t m = 0; m < cascade::dsp::kDemodModeCount; ++m) {
        CHECK(cmd::foxDemodFromName(cascade::dsp::modeName(static_cast<cascade::dsp::DemodMode>(m))) != 0u);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    everyRowTranslates();
    everyFieldHasARow();
    orderIsTheHistoricalOrder();
    longTextTravels();
    pluginControlsMap();
    modeNamesAreTheApiOrder();
    return testSummary("test_control_ops");
}
