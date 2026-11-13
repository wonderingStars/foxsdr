// Screen lane M's pure rules (src/slint/lanem_logic.hpp; docs/multi-radio-records/screen-m.md): what a typed field means for the operations of the wiring second passes (the I/Q file's transport, the receive
// clarifier, CW surround, the stitched sweep and its history, an airband channel's settings, the level store, the patch edit, the symbol rate), the readers of their facts (the history read-out, the GPU
// counters, a plugin's open request) and a mark's credit line; the named choices of a plugin's settings field (kit_logic parseChoices).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/airband_channel.hpp"
#include "foxsdr_api.h"
#include "lanem_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slintlanem;
using nlohmann::json;

int main() {
    double d = 0, d2 = 0; long long n = 0; std::string e;

    // ---------------------------------------------------------------- the I/Q file's transport (row 1)
    CHECK(parseSpeed("0.25", d, e) && d == 0.25);                      // both ends are inside
    CHECK(parseSpeed("8", d, e) && d == 8.0);
    CHECK(!parseSpeed("0.2499", d, e) && e.find("0.25") != std::string::npos);   // the sentence names the range
    CHECK(!parseSpeed("8.01", d, e));
    CHECK(!parseSpeed("", d, e) && !parseSpeed("fast", d, e) && !parseSpeed("-1", d, e) && !parseSpeed("0", d, e));
    CHECK(speedText(1) == "1x" && speedText(1.5) == "1.5x" && speedText(0.25) == "0.25x");
    CHECK(policyWords("varispeed") == "varispeed" && policyWords("blockskip") == "block skip" && policyWords("other").empty());
    CHECK(parseStepBlocks("1", n, e) && n == 1);
    CHECK(parseStepBlocks("-4096", n, e) && n == -4096);
    CHECK(parseStepBlocks("4096", n, e) && n == 4096);
    CHECK(!parseStepBlocks("0", n, e) && !e.empty());                  // never 0
    CHECK(!parseStepBlocks("4097", n, e) && !parseStepBlocks("-4097", n, e) && !parseStepBlocks("1.5", n, e) && !parseStepBlocks("", n, e));

    // ---------------------------------------------------------------- the receive clarifier (row 2)
    CHECK(parseRitHz("300", 9999, d, e) && d == 300.0);
    CHECK(parseRitHz("-9999", 9999, d, e) && d == -9999.0);
    CHECK(!parseRitHz("10000", 9999, d, e) && e.find("9999") != std::string::npos);
    CHECK(parseRitHz("15000", 20000, d, e) && d == 15000.0);           // the engine's own limit moves the range
    CHECK(parseRitHz("5", 0, d, e) && parseRitHz("5", 99999, d, e));   // a nonsense limit falls back to 9999, never to a wider range
    CHECK(!parseRitHz("10000", 99999, d, e));
    CHECK(!parseRitHz("abc", 9999, d, e) && !parseRitHz("", 9999, d, e));
    CHECK(parseRitLimits("9999", "0", d, d2, e) && d == 9999.0 && d2 == 0.0);
    CHECK(parseRitLimits("1", "1000000", d, d2, e) && d == 1.0 && d2 == 1e6);
    CHECK(!parseRitLimits("0", "0", d, d2, e) && e.find("Largest offset") != std::string::npos);
    CHECK(!parseRitLimits("20001", "0", d, d2, e));
    CHECK(!parseRitLimits("100", "-1", d, d2, e) && e.find("Clear on a retune") != std::string::npos);
    CHECK(signedHzText(300) == "+300 Hz" && signedHzText(-1500) == "-1500 Hz" && signedHzText(0) == "0 Hz" && signedHzText(299.6) == "+300 Hz");

    // ---------------------------------------------------------------- CW surround (row 3)
    {
        const Built b = cwSurroundJson(true, "700", "400", false, "100", "400");
        CHECK(b.ok);
        const json j = json::parse(b.text);
        CHECK(j["enabled"] == true && j["centreHz"] == 700.0 && j["spreadHz"] == 400.0 && j["invert"] == false && j["toneRatio"] == 100.0 && j["holdMs"] == 400.0);
        CHECK(cwSurroundJson(false, "200", "50", true, "2", "0").ok && cwSurroundJson(false, "1500", "2000", true, "1000", "5000").ok);   // the range ends
        CHECK(!cwSurroundJson(true, "199", "400", false, "100", "400").ok && !cwSurroundJson(true, "1501", "400", false, "100", "400").ok);
        CHECK(!cwSurroundJson(true, "700", "49", false, "100", "400").ok && !cwSurroundJson(true, "700", "2001", false, "100", "400").ok);
        CHECK(!cwSurroundJson(true, "700", "400", false, "1", "400").ok && !cwSurroundJson(true, "700", "400", false, "1001", "400").ok);
        CHECK(!cwSurroundJson(true, "700", "400", false, "100", "5001").ok && !cwSurroundJson(true, "700", "400", false, "100", "-1").ok);
        const Built bad = cwSurroundJson(true, "", "400", false, "100", "400");
        CHECK(!bad.ok && bad.err.find("Centre pitch") != std::string::npos);   // the sentence names the field
    }

    // ---------------------------------------------------------------- the stitched sweep (rows 11 and 12)
    {
        const Built b = stitchedSweepJson("2", "144", "146", "0.6", "0.12", "150", "2", "4000", "1");
        CHECK(b.ok);
        const json j = json::parse(b.text);
        CHECK(j["kind"] == "radio" && j["radio"] == 2 && j["start_mhz"] == 144.0 && j["stop_mhz"] == 146.0 && j["step_fraction"] == 0.6 && j["trim"] == 0.12 && j["settle_ms"] == 150 && j["frames"] == 2 &&
              j["timeout_ms"] == 4000 && j["passes"] == 1);
        CHECK(!j.contains("port") && !j.contains("serial"));                          // a radio, never a port
        CHECK(stitchedSweepJson("1", "1", "2", "0.3", "0", "0", "1", "500", "0").ok && stitchedSweepJson("8", "1", "2", "0.7", "0.4", "5000", "64", "60000", "1000000").ok);   // the ends
        CHECK(!stitchedSweepJson("0", "1", "2", "0.6", "0.12", "150", "2", "4000", "1").ok && !stitchedSweepJson("9", "1", "2", "0.6", "0.12", "150", "2", "4000", "1").ok);
        CHECK(!stitchedSweepJson("1", "146", "144", "0.6", "0.12", "150", "2", "4000", "1").ok);   // the stop is above the start
        CHECK(!stitchedSweepJson("1", "144", "144", "0.6", "0.12", "150", "2", "4000", "1").ok);
        CHECK(!stitchedSweepJson("1", "", "146", "0.6", "0.12", "150", "2", "4000", "1").ok && !stitchedSweepJson("1", "144", "", "0.6", "0.12", "150", "2", "4000", "1").ok);
        CHECK(!stitchedSweepJson("1", "144", "146", "0.29", "0.12", "150", "2", "4000", "1").ok && !stitchedSweepJson("1", "144", "146", "0.71", "0.12", "150", "2", "4000", "1").ok);
        CHECK(!stitchedSweepJson("1", "144", "146", "0.6", "0.41", "150", "2", "4000", "1").ok && !stitchedSweepJson("1", "144", "146", "0.6", "0.12", "5001", "2", "4000", "1").ok);
        CHECK(!stitchedSweepJson("1", "144", "146", "0.6", "0.12", "150", "0", "4000", "1").ok && !stitchedSweepJson("1", "144", "146", "0.6", "0.12", "150", "65", "4000", "1").ok);
        CHECK(!stitchedSweepJson("1", "144", "146", "0.6", "0.12", "150", "2", "499", "1").ok && !stitchedSweepJson("1", "144", "146", "0.6", "0.12", "150", "2", "60001", "1").ok);
        CHECK(!stitchedSweepJson("1", "144", "146", "0.6", "0.12", "150", "2", "4000", "-1").ok && !stitchedSweepJson("1", "144", "146", "0.6", "0.12", "150", "2", "4000", "1000001").ok);
        CHECK(!stitchedSweepJson("1.5", "144", "146", "0.6", "0.12", "150", "2", "4000", "1").ok);   // a whole number of radios
    }

    // ---------------------------------------------------------------- the sweep history (rows 13 and 14)
    {
        const Built s = sweepHistoryJson("save", "C:/foxsdr/h.csv", 0);
        CHECK(s.ok);
        json j = json::parse(s.text);
        CHECK(j["action"] == "save" && j["path"] == "C:/foxsdr/h.csv" && j["format"] == "csv");
        CHECK(json::parse(sweepHistoryJson("save", "C:/foxsdr/h.json", 1).text)["format"] == "json");
        CHECK(json::parse(sweepHistoryJson("save", "C:/foxsdr/h.xml", 2).text)["format"] == "xml");
        const Built l = sweepHistoryJson("load", "  C:/foxsdr/h.csv  ", 2);
        CHECK(l.ok);
        j = json::parse(l.text);
        CHECK(j["action"] == "load" && j["path"] == "C:/foxsdr/h.csv" && !j.contains("format"));   // a load finds the format itself
        const Built c = sweepHistoryJson("clear", "", 0);
        CHECK(c.ok && json::parse(c.text) == json({{"action", "clear"}}));
        CHECK(sweepHistoryJson("clear", "anything", 0).ok);                // clear takes no path
        CHECK(!sweepHistoryJson("save", "", 0).ok && !sweepHistoryJson("load", "   ", 0).ok);
        CHECK(!sweepHistoryJson("save", "relative/h.csv", 0).ok && !sweepHistoryJson("load", "h.csv", 0).ok);   // a FULL path only
        CHECK(!sweepHistoryJson("save", "C:/foxsdr/h\n.csv", 0).ok);
        CHECK(!sweepHistoryJson("delete", "C:/foxsdr/h.csv", 0).ok);
        CHECK(historyReadout("{\"lines\":12,\"points\":3600,\"keep_points\":300000}") == "12 sweeps, 3600 points of 300000");
        CHECK(historyReadout("{\"lines\":0,\"points\":0,\"keep_points\":300000}") == "0 sweeps, 0 points of 300000");
        CHECK(historyReadout("").empty() && historyReadout("not json").empty() && historyReadout("[1]").empty());
        CHECK(stitchProgress("{\"active\":true,\"step\":3,\"centres\":12}") == "step 3 of 12");
        CHECK(stitchProgress("{\"active\":false,\"step\":3,\"centres\":12}").empty() && stitchProgress("").empty() && stitchProgress("x").empty());
    }

    // ---------------------------------------------------------------- an airband channel (row 7)
    {
        AirbandFields f;
        f.freqMHz = "118.1";
        AirbandBuilt b = airbandChannelText(f);
        CHECK(b.ok && b.freqHz == 118100000 && b.text.rfind("FOXAIRCH1|118100000|10000|0|0|-500|100|30|400|0|0|4|", 0) == 0);   // the defaults: level squelch -50 dB, no filter, output off
        const cascade::core::airband::ParseResult p = cascade::core::airband::parse(b.text);   // the engine's own parser takes it
        CHECK(p.ok && p.config.freqHz == 118100000 && p.config.output == cascade::core::airband::OutputKind::None);
        // everything set
        f.bandwidthKHz = "25"; f.mode = 1; f.squelchKind = 1; f.snrOpenDb = "12"; f.snrHystDb = "4"; f.holdMs = "800"; f.highPassHz = "300"; f.lowPassHz = "3000"; f.filterOrder = 2;
        f.notches = "1000/30; 2000"; f.output = 2; f.udpPort = "5005"; f.dailyFolder = false; f.maxFileMb = "50";
        b = airbandChannelText(f);
        CHECK(b.ok);
        const cascade::core::airband::ParseResult q = cascade::core::airband::parse(b.text);
        CHECK(q.ok && q.config.bandwidthHz == 25000 && q.config.mode == 1 && q.config.squelchKind == cascade::core::airband::SquelchKind::Snr && q.config.snrOpenTenthsDb == 120 &&
              q.config.snrHysteresisTenthsDb == 40 && q.config.holdMs == 800 && q.config.highPassHz == 300 && q.config.lowPassHz == 3000 && q.config.filterOrder == 2 &&
              q.config.notches[0].hz == 1000 && q.config.notches[0].qTenths == 300 && q.config.notches[1].hz == 2000 && q.config.notches[1].qTenths == 300 && q.config.notches[2].hz == 0 &&
              q.config.output == cascade::core::airband::OutputKind::Udp && q.config.udpPort == 5005 && !q.config.dailyFolder && q.config.maxFileMb == 50);
        // the fields come back from the line
        AirbandFields back;
        CHECK(airbandFieldsFrom(b.text, back) && back.freqMHz == "118.1" && back.bandwidthKHz == "25" && back.mode == 1 && back.squelchKind == 1 && back.snrOpenDb == "12" && back.holdMs == "800" &&
              back.highPassHz == "300" && back.lowPassHz == "3000" && back.filterOrder == 2 && back.notches == "1000/30; 2000/30" && back.output == 2 && back.udpPort == "5005" && !back.dailyFolder && back.maxFileMb == "50");
        CHECK(!airbandFieldsFrom("", back) && !airbandFieldsFrom("FOXAIRCH1|1", back) && !airbandFieldsFrom("garbage", back));
        // the model's own sentences refuse what is out of range (the screen does not widen them)
        AirbandFields g;
        g.freqMHz = "118.1";
        g.highPassHz = "3000"; g.lowPassHz = "300";                                   // the high-pass is not below the low-pass
        b = airbandChannelText(g);
        CHECK(!b.ok && b.err.find("high-pass must be below") != std::string::npos);
        g = AirbandFields{}; g.freqMHz = "118.1"; g.output = 2; g.udpPort = "0";      // a stream needs a port
        b = airbandChannelText(g);
        CHECK(!b.ok && b.err.find("needs a port") != std::string::npos);
        g.udpPort = "1023"; b = airbandChannelText(g);
        CHECK(!b.ok && b.err.find("1024 and 65535") != std::string::npos);
        g.udpPort = "1024"; CHECK(airbandChannelText(g).ok);
        g = AirbandFields{}; g.freqMHz = "118.1"; g.notches = "1000; 2000; 3000; 4000";
        CHECK(!airbandChannelText(g).ok);                                             // at most three notches
        g.notches = "49"; CHECK(!airbandChannelText(g).ok);
        g.notches = "1000/2"; CHECK(!airbandChannelText(g).ok);
        g.notches = "1000/101"; CHECK(!airbandChannelText(g).ok);
        g = AirbandFields{}; g.freqMHz = "0.05"; CHECK(!airbandChannelText(g).ok);    // below 100 kHz
        g.freqMHz = "6001"; CHECK(!airbandChannelText(g).ok);
        g.freqMHz = ""; CHECK(!airbandChannelText(g).ok);
        g.freqMHz = "118,1"; CHECK(!airbandChannelText(g).ok);
        g = AirbandFields{}; g.freqMHz = "118.1"; g.levelDb = "1"; CHECK(!airbandChannelText(g).ok);
        g.levelDb = "-121"; CHECK(!airbandChannelText(g).ok);
        g = AirbandFields{}; g.freqMHz = "118.1"; g.maxFileMb = "0"; CHECK(!airbandChannelText(g).ok);
        g.maxFileMb = "2001"; CHECK(!airbandChannelText(g).ok);
        g = AirbandFields{}; g.freqMHz = "118.1"; g.bandwidthKHz = "0.5"; CHECK(!airbandChannelText(g).ok);
        g.bandwidthKHz = "201"; CHECK(!airbandChannelText(g).ok);
        long long hz = 0;
        CHECK(airbandHz("118.1", hz, e) && hz == 118100000);
        CHECK(airbandHz("0.1", hz, e) && hz == 100000 && airbandHz("6000", hz, e) && hz == 6000000000LL);
        CHECK(!airbandHz("", hz, e) && !airbandHz("x", hz, e));
    }

    // ---------------------------------------------------------------- the level store (row 8)
    CHECK(parseCapMiB("1", d, e) && d == 1.0 && parseCapMiB("64", d, e) && d == 64.0 && parseCapMiB("16", d, e) && d == 16.0);
    CHECK(!parseCapMiB("0", d, e) && !parseCapMiB("65", d, e) && !parseCapMiB("-1", d, e) && !parseCapMiB("", d, e) && !parseCapMiB("lots", d, e));
    CHECK(bytesText(15091200) == "15.1 MB" && bytesText(0) == "0.0 MB");

    // ---------------------------------------------------------------- the spectrum's GPU counters (row 15)
    {
        const GpuCounters g = parseGpuCounters("{\"mode\":2,\"engaged\":true,\"backend\":\"gpu\",\"error\":\"\",\"block_gpu_frames\":120,\"block_cpu_frames\":3,\"block_gpu_up\":true}");
        CHECK(g.have && g.engaged && g.backend == "gpu" && g.hasBlock && g.blockGpu == 120 && g.blockCpu == 3 && g.blockGpuUp && g.error.empty());
        const GpuCounters old = parseGpuCounters("{\"mode\":0,\"engaged\":false,\"backend\":\"cpu\",\"error\":\"no device\"}");   // an engine without the block counters
        CHECK(old.have && !old.hasBlock && old.error == "no device" && old.backend == "cpu");
        CHECK(!parseGpuCounters("").have && !parseGpuCounters("nonsense").have && !parseGpuCounters("[1]").have);
    }

    // ---------------------------------------------------------------- a plugin's request to open a page or a folder (row 19)
    {
        OpenRequest r;
        CHECK(parseOpenRequest(std::string("ft8.dll") + '\x1F' + "FT8 decoder" + '\x1F' + "url" + '\x1F' + "https://example.org/get", r));
        CHECK(r.moduleFile == "ft8.dll" && r.pluginName == "FT8 decoder" && r.kind == "url" && r.target == "https://example.org/get");
        CHECK(openConfirmText(r) == std::string("ft8.dll") + '\x1F' + "url");   // exactly what the engine's confirm takes
        OpenRequest f;
        CHECK(parseOpenRequest(std::string("a.dll") + '\x1F' + "A" + '\x1F' + "folder" + '\x1F' + "C:/Users/x/plugin-libs/A", f) && f.kind == "folder" && openConfirmText(f) == std::string("a.dll") + '\x1F' + "folder");
        OpenRequest x;
        CHECK(!parseOpenRequest("", x) && !parseOpenRequest("only one part", x));
        CHECK(!parseOpenRequest(std::string("a") + '\x1F' + "b" + '\x1F' + "url", x));                                         // three parts
        CHECK(!parseOpenRequest(std::string("a") + '\x1F' + "b" + '\x1F' + "shell" + '\x1F' + "calc.exe", x));                 // a kind that is neither
        CHECK(!parseOpenRequest(std::string("") + '\x1F' + "b" + '\x1F' + "url" + '\x1F' + "https://x", x));                    // no module
        CHECK(!parseOpenRequest(std::string("a") + '\x1F' + "b" + '\x1F' + "url" + '\x1F' + "", x));                            // no address
        // an address containing the separator stays whole (only the first three are cut)
        CHECK(parseOpenRequest(std::string("a") + '\x1F' + "b" + '\x1F' + "url" + '\x1F' + "https://x/\x1F" "y", x) && x.target == std::string("https://x/\x1F") + "y");
    }

    // ---------------------------------------------------------------- the patch edit (rows 5 and 6)
    {
        const Built b = patchIdsJson("1, 2 3;4");
        CHECK(b.ok && json::parse(b.text) == json({{"ids", {1, 2, 3, 4}}}));
        CHECK(patchIdsJson("7").ok && json::parse(patchIdsJson("7").text)["ids"][0] == 7);
        CHECK(!patchIdsJson("").ok && !patchIdsJson("  ,; ").ok);                 // empty
        CHECK(!patchIdsJson("0").ok && !patchIdsJson("-1").ok && !patchIdsJson("1.5").ok && !patchIdsJson("a").ok && !patchIdsJson("1 x").ok);
        std::string many;
        for (int i = 1; i <= 64; ++i) { many += std::to_string(i) + " "; }
        CHECK(patchIdsJson(many).ok);
        CHECK(!patchIdsJson(many + "65").ok);                                     // at most 64
        CHECK(parsePastePoint("40", "-12.5", d, d2, e) && d == 40.0 && d2 == -12.5);
        CHECK(parsePastePoint("-100000", "100000", d, d2, e));
        CHECK(!parsePastePoint("100001", "0", d, d2, e) && e.find("Paste at x") != std::string::npos);
        CHECK(!parsePastePoint("0", "x", d, d2, e) && e.find("Paste at y") != std::string::npos);
    }

    // ---------------------------------------------------------------- the symbol rate (row 20)
    CHECK(parseSymbolRate("0", d, e) && d == 0.0);                                // none set
    CHECK(parseSymbolRate("31.25", d, e) && d == 31.25);
    CHECK(parseSymbolRate("100000000", d, e));
    CHECK(!parseSymbolRate("100000001", d, e) && !parseSymbolRate("-1", d, e) && !parseSymbolRate("", d, e) && !parseSymbolRate("baud", d, e));
    CHECK(symbolRateText(1200) == "1200" && symbolRateText(31.25) == "31.25" && symbolRateText(0) == "0" && symbolRateText(0.5) == "0.5");

    // ---------------------------------------------------------------- a mark's credit (row 21)
    CHECK(withCredit("Tower  118.1 MHz", "Data provided by HamQTH.com") == "Tower  118.1 MHz\nData provided by HamQTH.com");
    CHECK(withCredit("Tower", "") == "Tower" && withCredit("Tower", "   ") == "Tower");   // none: the tooltip is as it was
    CHECK(withCredit("", "Credit") == "Credit");
    CHECK(withCredit("Tower\nfrom a plugin", " Credit ") == "Tower\nfrom a plugin\nCredit");

    return testSummary("test_slint_lanem_host");
}
