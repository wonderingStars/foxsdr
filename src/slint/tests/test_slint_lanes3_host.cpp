// Screen lane S3's pure rules (src/slint/lanes3_logic.hpp; docs/multi-radio-records/screen-s3-recplay.md): file-name templates, sample formats, rollover, triggered clips, the replay buffer,
// timed recordings in local time, a position in a recording, a raw file's stated format and rate.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>

#include <nlohmann/json.hpp>

#include "lanes3_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slintlanes3;
using nlohmann::json;

int main() {
    double d = 0, d2 = 0; std::string e;

    // ---- file names (G3-08)
    CHECK(checkNameTemplate("").ok);
    { const NameCheck c = checkNameTemplate("{freq_mhz}/{mode}_{datetime}"); CHECK(c.ok && c.example == "100.5/WFM_20261104_101000.wav"); }
    CHECK(!checkNameTemplate("a:b").ok && !checkNameTemplate("a:b").error.empty());
    CHECK(!checkNameTemplate("../x").ok);                       // a path escape
    CHECK(!checkNameTemplate("{nonsense}").ok);
    CHECK(!checkNameTemplate("CON").ok);                        // a reserved device name
    CHECK(checkNameTemplate("{radio}_{counter:3}").example == "Radio 1_001.wav");

    // ---- sample formats (G3-07)
    CHECK(iqFormatNames().size() == 2 && audioFormatNames().size() == 3);
    CHECK(costPerSecond(true, 0, 2.0e6).find("16.0 MB") == 0);
    CHECK(costPerSecond(true, 1, 2.0e6).find("8.0 MB") == 0);
    CHECK(costPerSecond(false, 1, 48000).find("144 kB") == 0);
    CHECK(costPerSecond(true, 0, 0).empty());
    { int a = 9, b = 9; CHECK(parseFormatFact("iq=1;audio=2", a, b) && a == 1 && b == 2); CHECK(!parseFormatFact("iq=2;audio=0", a, b)); CHECK(!parseFormatFact("", a, b)); }

    // ---- rollover (G3-10)
    CHECK(parseRolloverMB("", d, e) && d == 0);
    CHECK(parseRolloverMB("100", d, e) && d == 100);
    CHECK(!parseRolloverMB("0.5", d, e) && !parseRolloverMB("100001", d, e) && !parseRolloverMB("-3", d, e) && !parseRolloverMB("abc", d, e));
    CHECK(parseRolloverMinutes("0.1", d, e) && d == 0.1);
    CHECK(!parseRolloverMinutes("0.05", d, e) && !parseRolloverMinutes("1441", d, e));
    { const auto b = rolloverJson("100", "10", true); CHECK(b.ok); const json j = json::parse(b.text); CHECK(j["maxMB"] == 100 && j["maxSeconds"] == 600 && j["align"] == true); }
    CHECK(!rolloverJson("100", "", true).ok);                   // the clock needs a time limit
    CHECK(rolloverJson("", "", false).ok);
    CHECK(rolloverReadout("{\"maxMB\":100,\"maxSeconds\":600,\"align\":true}") == "100 MB or 10 minutes, on the clock");
    CHECK(rolloverReadout("{\"maxMB\":0,\"maxSeconds\":0,\"align\":false}").empty());

    // ---- clips and the replay buffer (G3-03, G3-09, G3-02)
    { ClipFields f; f.enabled = true; const auto b = clipsJson(f); CHECK(b.ok); CHECK(json::parse(b.text)["maxClipS"] == 600); }
    { ClipFields f; f.preRollS = "11"; CHECK(!clipsJson(f).ok); f.preRollS = "2"; f.maxClipS = "9"; CHECK(!clipsJson(f).ok); f.maxClipS = "3600"; f.tailS = "31"; CHECK(!clipsJson(f).ok); }
    { ClipFields g; CHECK(clipFieldsFrom("{\"enabled\":true,\"preRollS\":4,\"tailS\":5,\"minS\":2,\"maxClipS\":90}", g) && g.enabled && g.preRollS == "4" && g.maxClipS == "90"); CHECK(!clipFieldsFrom("nope", g)); }
    CHECK(replayCost(10, 48000) == "10 s = 1.0 MB");
    CHECK(replayCost(0, 48000) == "off");
    CHECK(replayHeldText("seconds=5;bytes=480000;max=11520000") == "5 s of audio held (0.5 MB)");
    CHECK(replayHeldText("seconds=0;bytes=0;max=11520000") == "nothing held");
    CHECK(replayHeldText("").empty() && replayHeldText("garbage").empty());
    CHECK(parseReplaySeconds("30", 60, d, e) && d == 30);
    CHECK(!parseReplaySeconds("61", 60, d, e) && !parseReplaySeconds("-1", 60, d, e));
    CHECK(replayJson("30", 60).ok && !replayJson("90", 60).ok);

    // ---- timed recordings (G3-01): local time kept as a UTC instant
    const OffsetAt plus1 = [](std::int64_t) { return 60; };
    std::int64_t u = 0;
    CHECK(localToUnix("2026-11-17", "14:30", plus1, u, e) && u == 1794922200LL);
    CHECK(unixToLocal(1794922200LL, plus1) == "2026-11-17 14:30");
    CHECK(!localToUnix("2026-02-30", "10:00", plus1, u, e) && !e.empty());
    CHECK(!localToUnix("2026-11-17", "25:00", plus1, u, e));
    CHECK(!localToUnix("17-11-2026", "10:00", plus1, u, e));
    CHECK(!localToUnix("2026-11-17", "10", plus1, u, e));
    CHECK(localToUnix("2028-02-29", "10:00", plus1, u, e));          // a leap day exists
    CHECK(!localToUnix("2027-02-29", "10:00", plus1, u, e));
    // a zone with a clock change (UK rules: forward 2026-03-29 01:00 UTC, back 2026-10-25 01:00 UTC)
    const OffsetAt uk = [](std::int64_t ms) { const std::int64_t s = ms / 1000; return (s >= 1774746000LL && s < 1792890000LL) ? 60 : 0; };
    CHECK(!localToUnix("2026-03-29", "01:30", uk, u, e));            // the hour that is skipped
    CHECK(localToUnix("2026-03-29", "02:30", uk, u, e) && u == 1774747800LL);
    CHECK(localToUnix("2026-10-25", "01:30", uk, u, e) && u == 1792888200LL);   // twice a day: the first time
    CHECK(localToUnix("2026-06-01", "12:00", uk, u, e) && u == 1780311600LL);   // summer: UTC+1
    CHECK(unixToLocal(1780311600LL, uk) == "2026-06-01 12:00");
    {
        ScheduleFields f; f.name = "Test"; f.date = "2026-11-17"; f.time = "14:30"; f.lengthMin = "10"; f.radio = 2; f.repeat = 1; f.recordAudio = true; f.recordIq = true;
        const auto b = scheduleJson(f, plus1);
        CHECK(b.ok);
        const json j = json::parse(b.text);
        CHECK(j["startUnix"] == 1794922200LL && j["lengthS"] == 600 && j["repeat"] == "daily" && j["radio"] == 2 && j["recordIq"] == true && j["tune"] == false);
        f.recordAudio = false; f.recordIq = false; CHECK(!scheduleJson(f, plus1).ok);
        f.recordAudio = true; f.lengthMin = "0"; CHECK(!scheduleJson(f, plus1).ok);
        f.lengthMin = "1441"; CHECK(!scheduleJson(f, plus1).ok);
        f.lengthMin = "10"; f.tune = true; f.freqMHz = ""; CHECK(!scheduleJson(f, plus1).ok);
        f.freqMHz = "100.5"; f.mode = 2; f.bandwidthKHz = "150"; { const auto t = scheduleJson(f, plus1); CHECK(t.ok); const json k = json::parse(t.text); CHECK(k["freqHz"] == 100500000.0 && k["mode"] == 2 && k["bandwidthHz"] == 150000.0); }
        f.name = std::string(41, 'x'); CHECK(!scheduleJson(f, plus1).ok);
    }
    {
        const EntryView v = entryView("{\"id\":3,\"name\":\"Sat\",\"radio\":1,\"startUnix\":1794922200,\"lengthS\":600,\"repeat\":\"daily\",\"tune\":true,\"freqHz\":137500000,\"recordAudio\":true,\"recordIq\":false,\"enabled\":true,\"missed\":2,\"state\":\"waiting\",\"note\":\"\",\"nextUnix\":1794922200}", plus1);
        CHECK(v.ok && v.id == 3 && v.line == "Radio 1, 2026-11-17 14:30, 10 min, daily, 137.5 MHz, audio, waiting (2 missed)");
        CHECK(!entryView("garbage", plus1).ok);
    }

    // ---- playback (G3-14a)
    CHECK(parsePosition("90", 300, d, e) && d == 90);
    CHECK(parsePosition("1:30", 300, d, e) && d == 90);
    CHECK(parsePosition("1:02:03", 7200, d, e) && d == 3723);
    CHECK(parsePosition("0:12.5", 300, d, e) && d == 12.5);
    CHECK(!parsePosition("301", 300, d, e) && !parsePosition("-1", 300, d, e) && !parsePosition("", 300, d, e) && !parsePosition("1:2:3:4", 300, d, e) && !parsePosition("a:b", 300, d, e));
    CHECK(!parsePosition("1.5:10", 300, d, e));
    CHECK(positionText(90.5) == "1:30.5" && positionText(5) == "0:05" && positionText(3723) == "1:02:03" && positionText(-4) == "0:00");
    CHECK(parseLoop("10", "20", 300, d, d2, e) && d == 10 && d2 == 20);
    CHECK(!parseLoop("20", "10", 300, d, d2, e) && !parseLoop("10", "10", 300, d, d2, e) && !parseLoop("10", "400", 300, d, d2, e));

    // ---- a raw file (G3-13)
    CHECK(parseRate("2048000", d, e) && d == 2048000);
    CHECK(parseRate("2.048M", d, e) && d == 2048000);
    CHECK(parseRate("250k", d, e) && d == 250000);
    CHECK(parseRate("2 000 000", d, e) && d == 2e6);
    CHECK(!parseRate("999", d, e) && !parseRate("2G", d, e) && !parseRate("", d, e) && !parseRate("fast", d, e) && !parseRate("-5", d, e));
    CHECK(parseCentre("", d, e) && d == 0);
    CHECK(parseCentre("100.5", d, e) && d == 100.5e6);
    CHECK(parseCentre("7050k", d, e) && d == 7.05e6);
    CHECK(parseCentre("145000000", d, e) && d == 145e6);
    CHECK(!parseCentre("x", d, e) && !parseCentre("-1", d, e));
    {
        const auto b = rawFileId("C:/a b/x|y.cu8", 0, "2.048M", "100.5");
        CHECK(b.ok && b.text == "file-raw:cu8,2048000,100500000|C:/a b/x|y.cu8");
        RawId r;
        CHECK(parseRawFileId(b.text.substr(9), r) && r.format == "cu8" && r.rateHz == 2048000 && r.centreHz == 100.5e6 && r.hasCentre && r.path == "C:/a b/x|y.cu8");
        CHECK(!rawFileId("", 0, "2M", "").ok && !rawFileId("a", 3, "2M", "").ok && !rawFileId("a", 0, "9", "").ok);
        CHECK(!parseRawFileId("cu8,2048000,0", r) && !parseRawFileId("zz9,2048000,0|p", r) && !parseRawFileId("cu8,5,0|p", r) && !parseRawFileId("cu8,2048000,0|", r));
    }

    // ---- whole-file waterfall (G3-14b)
    CHECK(overviewFftSize(0) == 512 && overviewFftSize(3) == 4096 && overviewFftSize(99) == 4096 && overviewFftNames().size() == 4);
    CHECK(overviewJson("500", 1).ok && !overviewJson("10", 1).ok && !overviewJson("3000", 1).ok);

    return testSummary("test_slint_lanes3_host");
}
