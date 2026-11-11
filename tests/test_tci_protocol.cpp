// test_tci_protocol.cpp - THE TCI PROTOCOL LAYER (net/tci_protocol.*), pure: a stub backend that behaves like an engine (it applies the
// commands it is handed to its radios and reports their results), no sockets.
//
//   P1  PARSING (TCI Protocol 2.1, section 3.1 and 4.7): the specification's own examples, case folding, empty arguments discarded, a command
//       with no ';' dropped, over-long names and arguments, too many arguments, an oversize message read as nothing.
//   P2  THE START-UP EXCHANGE: the initialisation commands in the order of section 4.1, READY, then the state of every receiver.
//   P3  TUNING: VFO inside the band moves only the offset; outside it moves the centre; each aims at the receiver INDEX's radio.
//   P4  THE ECHO: a set is echoed once the engine has applied it, even when nothing changed (a client waiting for the echo must not hang); a
//       change made by someone else is pushed; nothing is pushed twice.
//   P5  RECEIVE-ONLY: TRX, TUNE and TX_ENABLE answer false whatever is asked, and NO command the protocol layer can build is a transmitter op.
//   P6  RECEIVERS: out-of-range numbers are ignored without a reply; removing a radio renumbers and says so.
//   P7  AUDIO: the setting commands, the 64-byte header byte for byte, every sample type, mono and stereo, packet counters, the decimator.
//   P8  HOSTILE INPUT: random text and mutated commands never crash and never submit anything but the allowed ops.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "net/tci_protocol.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

// An engine in a box: radios with figures; commands are applied when the test says so (apply()), and their results then reported.
struct Stub : TciBackend {
    std::vector<int> nums{1, 2};
    std::map<int, TciRadioView> v;
    std::vector<FoxCommand> all;      // everything ever submitted
    std::vector<FoxCommand> waiting;  // submitted, not yet applied
    int resultsReady = 0;
    int audioFrames = 0;

    Stub() {
        for (int r : nums) {
            TciRadioView x;
            x.running = true;
            x.centreHz = 7100000.0 + (r - 1) * 1000000.0;
            x.vfoOffsetHz = 0.0;
            x.tunedHz = x.centreHz;
            x.sampleRateHz = 2400000.0;
            x.bandwidthHz = 2700.0;
            x.squelchDb = -200.0;
            x.demod = FOXAPI_DEMOD_USB;
            v[r] = x;
        }
    }
    std::vector<int> radios() override { return nums; }
    bool readRadio(int radio, TciRadioView& out) override {
        const auto it = v.find(radio);
        if (it == v.end()) { return false; }
        out = it->second;
        return true;
    }
    int submit(const std::vector<FoxCommand>& c) override {
        for (const FoxCommand& x : c) { all.push_back(x); waiting.push_back(x); }
        return static_cast<int>(c.size());
    }
    int pollResults() override {
        const int r = resultsReady;
        resultsReady = 0;
        return r;
    }
    int readAudio(int, std::uint64_t&, float*, std::size_t) override { return audioFrames; }

    // The engine's pass: apply what is waiting, then report the results.
    void apply() {
        for (const FoxCommand& c : waiting) {
            const std::uint32_t target = c.target;
            auto setAll = [&](auto&& f) {
                if (target == FOXAPI_TARGET_ALL_RADIOS) { for (auto& [r, x] : v) { f(x); } }
                else if (v.count(static_cast<int>(target)) != 0) { f(v[static_cast<int>(target)]); }
            };
            switch (c.op) {
                case FOXAPI_OP_SET_CENTRE: setAll([&](TciRadioView& x) { x.centreHz = c.num[0]; x.vfoOffsetHz = 0.0; x.tunedHz = x.centreHz; }); break;
                case FOXAPI_OP_SET_VFO_OFFSET: setAll([&](TciRadioView& x) { x.vfoOffsetHz = c.num[0]; x.tunedHz = x.centreHz + x.vfoOffsetHz; }); break;
                case FOXAPI_OP_SET_MODE: setAll([&](TciRadioView& x) { x.demod = static_cast<std::uint32_t>(c.ival[0]); }); break;
                case FOXAPI_OP_SET_BANDWIDTH: setAll([&](TciRadioView& x) { x.bandwidthHz = c.num[0]; }); break;
                case FOXAPI_OP_SET_SQUELCH: setAll([&](TciRadioView& x) { x.squelchDb = c.num[0]; }); break;
                case FOXAPI_OP_RUN: setAll([&](TciRadioView& x) { x.running = c.ival[0] != 0; }); break;
                default: break;
            }
        }
        resultsReady += static_cast<int>(waiting.size());
        waiting.clear();
    }
};

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) { s += x; }
    return s;
}
bool has(const std::vector<std::string>& v, const std::string& frame) { return std::find(v.begin(), v.end(), frame) != v.end(); }

std::int64_t t = 1000;   // the test clock
std::vector<std::string> tickOnce(TciConnection& c) { t += 40; return c.tick(t); }

// The only ops a TCI connection may ever submit.
const std::set<std::uint32_t> kAllowed = {FOXAPI_OP_RUN, FOXAPI_OP_SET_CENTRE, FOXAPI_OP_SET_VFO_OFFSET, FOXAPI_OP_SET_MODE,
                                          FOXAPI_OP_SET_BANDWIDTH, FOXAPI_OP_SET_SQUELCH};

void testParsing() {
    TciParse p = parseTciMessage("VFO:0,1,7100000;MODULATION:0,LSB;START;");
    CHECK(p.commands.size() == 3 && p.dropped == 0);
    CHECK(p.commands[0].name == "vfo" && p.commands[0].args == std::vector<std::string>({"0", "1", "7100000"}));
    CHECK(p.commands[1].name == "modulation" && p.commands[1].args[1] == "LSB");
    CHECK(p.commands[2].name == "start" && p.commands[2].args.empty());
    // the case of letters does not matter (3.1)
    p = parseTciMessage("vFo:0,1;");
    CHECK(p.commands.size() == 1 && p.commands[0].name == "vfo");
    p = parseTciMessage("  Modulation : 0 ;");
    CHECK(p.commands.size() == 1 && p.commands[0].name == "modulation" && p.commands[0].args == std::vector<std::string>({"0"}));
    // 4.7 note 1: empty arguments are discarded
    p = parseTciMessage("VFO:0,,7100000;");
    CHECK(p.commands.size() == 1 && p.commands[0].args == std::vector<std::string>({"0", "7100000"}));
    // spaces around commands and arguments
    p = parseTciMessage("  dds: 0 , 7100000 ;\r\n ");
    CHECK(p.commands.size() == 1 && p.commands[0].args == std::vector<std::string>({"0", "7100000"}));
    // no terminating ';' : not a command (3.1)
    p = parseTciMessage("START");
    CHECK(p.commands.empty() && p.dropped == 1);
    p = parseTciMessage("STOP;START");
    CHECK(p.commands.size() == 1 && p.dropped == 1);
    // junk names and limits
    p = parseTciMessage(";;;");
    CHECK(p.commands.empty() && p.dropped == 0);
    p = parseTciMessage("bad name:1;ok;");
    CHECK(p.commands.size() == 1 && p.commands[0].name == "ok" && p.dropped == 1);
    p = parseTciMessage(std::string(kMaxTciNameBytes + 1, 'a') + ":1;");
    CHECK(p.commands.empty() && p.dropped == 1);
    p = parseTciMessage("x:" + std::string(kMaxTciArgBytes + 1, '1') + ";");
    CHECK(p.commands.empty() && p.dropped == 1);
    p = parseTciMessage("x:1,2,3,4,5,6,7,8,9;");
    CHECK(p.commands.empty() && p.dropped == 1);
    p = parseTciMessage("x:1,2,3,4,5,6,7,8;");
    CHECK(p.commands.size() == 1 && p.commands[0].args.size() == 8);
    std::string many;
    for (int i = 0; i < 100; ++i) { many += "stop;"; }
    p = parseTciMessage(many);
    CHECK(p.commands.size() == kMaxTciCommandsPerMessage && p.dropped == 100 - static_cast<int>(kMaxTciCommandsPerMessage));
    p = parseTciMessage(std::string(kMaxTciTextBytes + 1, ';'));
    CHECK(p.oversize && p.commands.empty());
    p = parseTciMessage(std::string(kMaxTciTextBytes, ';'));
    CHECK(!p.oversize);
    // numbers and logicals (4.7 notes 2 and 3)
    std::int64_t n = 0;
    CHECK(tciParseInteger("7100000", n) && n == 7100000);
    CHECK(tciParseInteger("-350", n) && n == -350);
    CHECK(!tciParseInteger("", n) && !tciParseInteger("-", n) && !tciParseInteger("12a", n) && !tciParseInteger("1.5", n) && !tciParseInteger("+5", n));
    CHECK(!tciParseInteger("9999999999999999", n));   // 16 digits: refused, never overflows
    CHECK(tciParseBool("true") && tciParseBool("TRUE") && tciParseBool("1") && !tciParseBool("yes") && !tciParseBool("0") && !tciParseBool(""));
    // names
    std::uint32_t d = 0;
    CHECK(tciModeFromName("lsb", d) && d == FOXAPI_DEMOD_LSB);
    CHECK(tciModeFromName("NFM", d) && d == FOXAPI_DEMOD_NFM);
    CHECK(tciModeFromName("fm", d) && d == FOXAPI_DEMOD_NFM);
    CHECK(!tciModeFromName("DIGU", d) && !tciModeFromName("", d));
    CHECK(std::string(tciModeName(FOXAPI_DEMOD_USB)) == "USB" && std::string(tciModeName(FOXAPI_DEMOD_RAW)).empty() && std::string(tciModeName(0)).empty());
}

void testStartup() {
    Stub s;
    TciConnection c(s);
    const std::vector<std::string> b = c.begin();
    // section 4.1's order, then READY, then the state
    CHECK(b.size() > 12);
    CHECK(b[0] == "VFO_LIMITS:1,6000000000;");
    CHECK(b[1] == "IF_LIMITS:-1200000,1200000;");
    CHECK(b[2] == "TRX_COUNT:2;");
    CHECK(b[3] == "CHANNELS_COUNT:1;");
    CHECK(b[4] == "DEVICE:FoxSDR;");
    CHECK(b[5] == "RECEIVE_ONLY:true;");
    CHECK(b[6].rfind("MODULATIONS_LIST:AM,LSB,USB,CW,NFM", 0) == 0);
    CHECK(b[7] == "PROTOCOL:FoxSDR,2.1;");
    CHECK(b[8] == "READY;");
    CHECK(b[9] == "START;");
    CHECK(has(b, "DDS:0,7100000;") && has(b, "VFO:0,0,7100000;") && has(b, "IF:0,0,0;") && has(b, "MODULATION:0,USB;"));
    CHECK(has(b, "RX_FILTER_BAND:0,0,2700;") && has(b, "RX_ENABLE:0,true;"));
    CHECK(has(b, "DDS:1,8100000;") && has(b, "VFO:1,0,8100000;"));
    CHECK(has(b, "TRX:0,false;") && has(b, "TX_ENABLE:0,false;") && has(b, "TRX:1,false;"));
    CHECK(has(b, "SQL_ENABLE:0,false;"));
    // limits from the radios when they report one
    Stub r;
    r.v[1].limitLoHz = 24000000.0;
    r.v[1].limitHiHz = 1766000000.0;
    r.v[2].limitLoHz = 34000000.0;
    r.v[2].limitHiHz = 6016000000.0;
    TciConnection c2(r);
    CHECK(c2.begin()[0] == "VFO_LIMITS:24000000,6016000000;");
    // stopped radios: STOP; and RX_ENABLE false
    Stub q;
    q.v[1].running = q.v[2].running = false;
    TciConnection c3(q);
    const std::vector<std::string> b3 = c3.begin();
    CHECK(b3[9] == "STOP;" && has(b3, "RX_ENABLE:0,false;"));
    // nothing is pushed twice: the first tick after begin() has nothing to say
    CHECK(tickOnce(c).empty());
    CHECK(tickOnce(c).empty());
}

void testTuning() {
    Stub s;
    TciConnection c(s);
    c.begin();
    // VFO 7.1005 MHz on radio 0 (centre 7.1 MHz, rate 2.4 MS/s): inside the band, only the offset moves
    CHECK(c.onText("VFO:0,0,7100500;").empty());
    CHECK(s.all.size() == 1 && s.all[0].op == FOXAPI_OP_SET_VFO_OFFSET && s.all[0].target == 1 && s.all[0].num[0] == 500.0);
    // VFO far outside: the centre moves (and the offset returns to 0)
    s.all.clear();
    c.onText("VFO:1,0,14200000;");
    CHECK(s.all.size() == 2 && s.all[0].op == FOXAPI_OP_SET_CENTRE && s.all[0].target == 2 && s.all[0].num[0] == 14200000.0);
    CHECK(s.all[1].op == FOXAPI_OP_SET_VFO_OFFSET && s.all[1].target == 2 && s.all[1].num[0] == 0.0);
    // DDS and IF
    s.all.clear();
    c.onText("DDS:0,10000000;IF:0,0,-1500;");
    CHECK(s.all.size() == 2 && s.all[0].op == FOXAPI_OP_SET_CENTRE && s.all[0].num[0] == 10000000.0);
    CHECK(s.all[1].op == FOXAPI_OP_SET_VFO_OFFSET && s.all[1].num[0] == -1500.0);
    // modulation, filter band, squelch, per-receiver run
    s.all.clear();
    c.onText("MODULATION:1,nfm;RX_FILTER_BAND:0,-5000,5000;SQL_LEVEL:0,-80;RX_ENABLE:1,false;");
    CHECK(s.all.size() == 4);
    CHECK(s.all[0].op == FOXAPI_OP_SET_MODE && s.all[0].target == 2 && s.all[0].ival[0] == FOXAPI_DEMOD_NFM);
    CHECK(s.all[1].op == FOXAPI_OP_SET_BANDWIDTH && s.all[1].target == 1 && s.all[1].num[0] == 10000.0);
    CHECK(s.all[2].op == FOXAPI_OP_SET_SQUELCH && s.all[2].num[0] == -80.0);
    CHECK(s.all[3].op == FOXAPI_OP_RUN && s.all[3].target == 2 && s.all[3].ival[0] == 0);
    // START / STOP are for every radio
    s.all.clear();
    c.onText("STOP;");
    CHECK(s.all.size() == 1 && s.all[0].op == FOXAPI_OP_RUN && s.all[0].target == FOXAPI_TARGET_ALL_RADIOS && s.all[0].ival[0] == 0);
    // a frequency that is not a number is a READ request (4.7 note 2): nothing submitted, the state comes back
    s.all.clear();
    c.onText("VFO:0,0,-5;VFO:0,0,abc;");
    CHECK(s.all.empty());
    // a channel other than A (0) does not exist: ignored
    c.onText("VFO:0,1,7100000;IF:0,2,5;");
    CHECK(s.all.empty());
    // squelch: off writes -200, on restores the last level
    s.all.clear();
    c.onText("SQL_LEVEL:0,-70;SQL_ENABLE:0,false;SQL_ENABLE:0,true;");
    CHECK(s.all.size() == 3 && s.all[0].num[0] == -70.0 && s.all[1].num[0] == -200.0 && s.all[2].num[0] == -70.0);
    // SQL_LEVEL is clamped to the specification's -140..0
    s.all.clear();
    c.onText("SQL_LEVEL:0,-500;SQL_LEVEL:0,40;");
    CHECK(s.all.size() == 2 && s.all[0].num[0] == -140.0 && s.all[1].num[0] == 0.0);
}

void testEcho() {
    Stub s;
    TciConnection c(s);
    c.begin();
    tickOnce(c);
    c.onText("VFO:0,0,7100500;");
    // not applied yet: no echo (it would be the old value)
    CHECK(tickOnce(c).empty());
    s.apply();
    std::vector<std::string> out = tickOnce(c);
    CHECK(has(out, "VFO:0,0,7100500;") && has(out, "IF:0,0,500;"));
    CHECK(!has(out, "DDS:0,7100000;"));   // the centre did not change
    CHECK(tickOnce(c).empty());            // and nothing twice
    // a set that changes nothing is still echoed (a client that waits for the echo must not hang)
    c.onText("VFO:0,0,7100500;");
    s.apply();
    out = tickOnce(c);
    CHECK(has(out, "VFO:0,0,7100500;"));
    // a refused set (the engine did not change it) is echoed as the current value
    c.onText("MODULATION:0,AM;");
    s.apply();
    s.v[1].demod = FOXAPI_DEMOD_USB;   // as if the engine had refused
    out = tickOnce(c);
    CHECK(has(out, "MODULATION:0,USB;"));
    // a read request is answered with the state
    c.onText("DDS:0;");
    out = tickOnce(c);
    CHECK(has(out, "DDS:0,7100000;"));
    // someone else's change (another client, the operator) is pushed
    s.v[2].demod = FOXAPI_DEMOD_AM;
    s.v[2].bandwidthHz = 6000.0;
    out = tickOnce(c);
    CHECK(has(out, "MODULATION:1,AM;") && has(out, "RX_FILTER_BAND:1,-3000,3000;"));
    CHECK(tickOnce(c).empty());
    // the whole radio stopping pushes STOP; once, and RX_ENABLE
    s.v[1].running = s.v[2].running = false;
    out = tickOnce(c);
    CHECK(has(out, "STOP;") && has(out, "RX_ENABLE:0,false;") && has(out, "RX_ENABLE:1,false;"));
    CHECK(tickOnce(c).empty());
    // LSB and USB filters are one-sided
    s.v[1].demod = FOXAPI_DEMOD_LSB;
    out = tickOnce(c);
    CHECK(has(out, "RX_FILTER_BAND:0,-2700,0;"));
    // the sample rate changing pushes IF_LIMITS for receiver 0
    s.v[1].sampleRateHz = 1000000.0;
    out = tickOnce(c);
    CHECK(has(out, "IF_LIMITS:-500000,500000;"));
    // a set whose result takes long: the echo goes out after 2 s anyway
    Stub s2;
    TciConnection c2(s2);
    c2.begin();
    t = 100000;
    c2.tick(t);
    c2.onText("VFO:0,0,7100200;");
    CHECK(c2.tick(t + 500).empty());
    CHECK(has(c2.tick(t + 2500), "VFO:0,0,7100000;"));
}

void testReceiveOnly() {
    Stub s;
    TciConnection c(s);
    c.begin();
    std::vector<std::string> r = c.onText("TRX:0,true;");
    CHECK(r == std::vector<std::string>({"TRX:0,false;"}));
    r = c.onText("TRX:1,true,tci;");
    CHECK(r == std::vector<std::string>({"TRX:1,false;"}));
    r = c.onText("TRX:0;");
    CHECK(r == std::vector<std::string>({"TRX:0,false;"}));
    r = c.onText("TUNE:0,true;");
    CHECK(r == std::vector<std::string>({"TUNE:0,false;"}));
    r = c.onText("TX_ENABLE:0,true;");
    CHECK(r == std::vector<std::string>({"TX_ENABLE:0,false;"}));
    CHECK(c.counters().txRefused == 4);
    CHECK(s.all.empty());
    // the rest of the transmit and CW vocabulary: ignored without a reply, and no command is ever built from it
    const char* ignored[] = {"DRIVE:0,50;", "TUNE_DRIVE:0,10;", "CW_MACROS:0,CQ DE X;", "CW_MSG:0,a,b,c;", "CW_MACROS_STOP;", "CW_TERMINAL:0,true;",
                             "KEYER:0,true,50;", "MON_ENABLE:true;", "RIT_ENABLE:0,true;", "XIT_ENABLE:0,true;", "SPLIT_ENABLE:0,true;",
                             "IQ_START:0;", "IQ_STOP:0;", "IQ_SAMPLERATE:48000;", "LINE_OUT_START:0;", "LINE_OUT_RECORDER_SAVE:0,C|x.wav;",
                             "RX_SENSORS_ENABLE:true;", "TX_SENSORS_ENABLE:true;", "SPOT:A,CW,7100000,1,x;", "SPOT_CLEAR;", "PREAMP:0,true;",
                             "CTCSS_ENABLE:0,true;", "SET_IN_FOCUS;", "no_such_command:1,2;", "VFO_LOCK:0,0,true;"};
    for (const char* text : ignored) {
        CHECK(c.onText(text).empty());
    }
    CHECK(s.all.empty());
    CHECK(c.counters().ignored >= sizeof(ignored) / sizeof(ignored[0]));
    // every command name of the specification's tables, with receiver arguments in range and out: only allowed ops are ever built
    const char* names[] = {"vfo_limits", "if_limits", "trx_count", "channels_count", "device", "receive_only", "modulations_list", "protocol", "ready",
                           "start", "stop", "dds", "if", "vfo", "modulation", "trx", "tune", "drive", "tune_drive", "rit_enable", "xit_enable",
                           "split_enable", "rit_offset", "xit_offset", "rx_channel_enable", "rx_filter_band", "cw_macros_speed", "cw_macros_delay",
                           "cw_keyer_speed", "volume", "mute", "rx_mute", "rx_volume", "rx_balance", "mon_volume", "mon_enable", "agc_mode",
                           "agc_gain", "rx_nb_enable", "rx_nb_param", "rx_bin_enable", "rx_nr_enable", "rx_anc_enable", "rx_anf_enable",
                           "rx_apf_enable", "rx_dse_enable", "rx_nf_enable", "lock", "sql_enable", "sql_level", "digl_offset", "digu_offset",
                           "tx_enable", "cw_macros_speed_up", "cw_macros_speed_down", "spot", "spot_delete", "iq_samplerate", "audio_samplerate",
                           "iq_start", "iq_stop", "audio_start", "audio_stop", "line_out_start", "line_out_stop", "line_out_recorder_start",
                           "line_out_recorder_save", "line_out_recorder_break", "spot_clear", "audio_stream_sample_type", "audio_stream_channels",
                           "audio_stream_samples", "tx_stream_audio_buffering", "clicked_on_spot", "tx_footswitch", "tx_frequency", "app_focus",
                           "set_in_focus", "keyer", "rx_sensors_enable", "tx_sensors_enable", "rx_sensors", "tx_sensors", "vfo_lock",
                           "rx_channel_sensors", "rx_enable", "ecoder_switch_rx", "ecoder_switch_channel", "hw_sensors_enable", "hw_sensors",
                           "cw_macros", "cw_msg", "cw_macros_stop", "cw_terminal", "cw_mon", "callsign_send", "cw_macros_empty"};
    std::mt19937 rng(7);
    const char* argsets[] = {"", ":0", ":0,1", ":0,0,7100000", ":1,true", ":9,1", ":0,0,true,1", ":true", ":-1", ":0,x", ":0,0,0"};
    for (const char* nm : names) {
        for (const char* a : argsets) {
            c.onText(std::string(nm) + a + ";");
        }
    }
    for (const FoxCommand& cmd : s.all) { CHECK(kAllowed.count(cmd.op) == 1); }
    (void)rng;
    std::printf("receive-only: %zu commands built from %zu names x %zu argument shapes, all of them in the allowed set\n", s.all.size(),
                sizeof(names) / sizeof(names[0]), sizeof(argsets) / sizeof(argsets[0]));
}

void testReceivers() {
    Stub s;
    s.nums = {1, 2, 3};
    TciRadioView x = s.v[1];
    x.centreHz = x.tunedHz = 3000000.0;
    s.v[3] = x;
    TciConnection c(s);
    c.begin();
    tickOnce(c);
    // out of range: ignored, no reply, no command
    CHECK(c.onText("VFO:3,0,7100000;DDS:7;MODULATION:-1,AM;RX_ENABLE:abc,true;").empty());
    CHECK(s.all.empty());
    CHECK(c.counters().ignored == 4);
    // receiver 2 is radio 3
    c.onText("DDS:2,3500000;");
    CHECK(s.all.size() == 1 && s.all[0].target == 3);
    s.apply();
    CHECK(has(tickOnce(c), "DDS:2,3500000;"));
    // radio 2 goes away: the numbering shifts - the count is said, and the new receiver 1 (radio 3) is described
    s.nums = {1, 3};
    s.v.erase(2);
    const std::vector<std::string> out = tickOnce(c);
    CHECK(has(out, "TRX_COUNT:2;"));
    CHECK(has(out, "DDS:1,3500000;") && has(out, "VFO:1,0,3500000;"));
    CHECK(!has(out, "DDS:2,3500000;"));
    // a radio that cannot be read this time is skipped, not an error
    s.v.erase(3);
    tickOnce(c);
    // a radio is added: said again
    s.v[2] = s.v[1];
    s.nums = {1, 2};
    CHECK(has(tickOnce(c), "TRX_COUNT:2;"));
}

std::uint32_t le32(const std::string& s, std::size_t off) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(s[off])) | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 2])) << 16) | (static_cast<std::uint32_t>(static_cast<unsigned char>(s[off + 3])) << 24);
}

void testAudio() {
    // the header, byte for byte (section 3.4)
    {
        const float two[4] = {0.5f, -0.5f, 1.0f, -1.0f};
        const std::string p = tciPackAudio(1, 24000, TciSampleType::Int16, 2, 7, two, 2);
        CHECK(p.size() == 64 + 4 * 2);
        CHECK(le32(p, 0) == 1 && le32(p, 4) == 24000 && le32(p, 8) == 0 && le32(p, 12) == 0 && le32(p, 16) == 0);
        CHECK(le32(p, 20) == 4);   // length: the samples in data[]
        CHECK(le32(p, 24) == kTciRxAudioStream && le32(p, 28) == 2 && le32(p, 32) == 7);
        for (int i = 36; i < 64; i += 4) { CHECK(le32(p, static_cast<std::size_t>(i)) == 0); }
        CHECK(static_cast<std::int16_t>(le32(p, 64) & 0xFFFF) == 16384 || static_cast<std::int16_t>(le32(p, 64) & 0xFFFF) == 16383);
        CHECK(static_cast<std::int16_t>(le32(p, 66) & 0xFFFF) == -16384 || static_cast<std::int16_t>(le32(p, 66) & 0xFFFF) == -16383);
        CHECK(static_cast<std::int16_t>(le32(p, 68) & 0xFFFF) == 32767 && static_cast<std::int16_t>(le32(p, 70) & 0xFFFF) == -32767);
    }
    // each sample type: size and full-scale value, clipping, NaN
    {
        const float v[3] = {1.5f, -2.0f, std::nanf("")};
        const std::string i16 = tciPackAudio(0, 48000, TciSampleType::Int16, 1, 0, v, 3);
        CHECK(i16.size() == 64 + 6 && static_cast<std::int16_t>(le32(i16, 64) & 0xFFFF) == 32767 && static_cast<std::int16_t>(le32(i16, 66) & 0xFFFF) == -32767);
        CHECK(static_cast<std::int16_t>(le32(i16, 68) & 0xFFFF) == 0);
        const std::string i24 = tciPackAudio(0, 48000, TciSampleType::Int24, 1, 0, v, 1);
        CHECK(i24.size() == 64 + 3 && static_cast<unsigned char>(i24[64]) == 0xFF && static_cast<unsigned char>(i24[65]) == 0xFF && static_cast<unsigned char>(i24[66]) == 0x7F);
        const std::string i32 = tciPackAudio(0, 48000, TciSampleType::Int32, 1, 0, v, 2);
        CHECK(i32.size() == 64 + 8 && le32(i32, 64) == 0x7FFFFFFFu && le32(i32, 68) == 0x80000001u);
        const float one = 0.25f;
        const std::string f32 = tciPackAudio(0, 48000, TciSampleType::Float32, 1, 0, &one, 1);
        float back;
        const std::uint32_t bits = le32(f32, 64);
        std::memcpy(&back, &bits, 4);
        CHECK(f32.size() == 68 && back == 0.25f);
        CHECK(le32(f32, 8) == 3);
    }
    // the connection's audio commands
    Stub s;
    TciConnection c(s);
    c.begin();
    CHECK(c.onText("AUDIO_SAMPLERATE;") == std::vector<std::string>({"AUDIO_SAMPLERATE:48000;"}));
    CHECK(c.onText("AUDIO_SAMPLERATE:12000;") == std::vector<std::string>({"AUDIO_SAMPLERATE:12000;"}));
    CHECK(c.onText("AUDIO_SAMPLERATE:11025;") == std::vector<std::string>({"AUDIO_SAMPLERATE:12000;"}));   // unsupported: ignored, the current rate comes back
    CHECK(c.onText("AUDIO_SAMPLERATE:48000;") == std::vector<std::string>({"AUDIO_SAMPLERATE:48000;"}));
    CHECK(c.onText("AUDIO_STREAM_SAMPLE_TYPE;") == std::vector<std::string>({"AUDIO_STREAM_SAMPLE_TYPE:float32;"}));   // the default (3.4)
    CHECK(c.onText("AUDIO_STREAM_SAMPLE_TYPE:INT16;") == std::vector<std::string>({"AUDIO_STREAM_SAMPLE_TYPE:int16;"}));
    CHECK(c.onText("AUDIO_STREAM_SAMPLE_TYPE:bogus;") == std::vector<std::string>({"AUDIO_STREAM_SAMPLE_TYPE:int16;"}));
    CHECK(c.onText("AUDIO_STREAM_CHANNELS;") == std::vector<std::string>({"AUDIO_STREAM_CHANNELS:2;"}));
    CHECK(c.onText("AUDIO_STREAM_CHANNELS:9;") == std::vector<std::string>({"AUDIO_STREAM_CHANNELS:2;"}));    // limited to 1..2
    CHECK(c.onText("AUDIO_STREAM_CHANNELS:0;") == std::vector<std::string>({"AUDIO_STREAM_CHANNELS:1;"}));
    CHECK(c.onText("AUDIO_STREAM_SAMPLES;") == std::vector<std::string>({"AUDIO_STREAM_SAMPLES:2048;"}));
    CHECK(c.onText("AUDIO_STREAM_SAMPLES:5;") == std::vector<std::string>({"AUDIO_STREAM_SAMPLES:100;"}));
    CHECK(c.onText("AUDIO_STREAM_SAMPLES:99999;") == std::vector<std::string>({"AUDIO_STREAM_SAMPLES:2048;"}));
    CHECK(c.onText("AUDIO_STREAM_SAMPLES:512;") == std::vector<std::string>({"AUDIO_STREAM_SAMPLES:512;"}));
    // mono, int16, 512 samples a packet at 48 kHz: packets come out whole, counted, and carry the receiver's INDEX
    CHECK(c.onText("AUDIO_STREAM_CHANNELS:1;AUDIO_START:1;").size() == 1);
    CHECK(c.audioRadios() == std::vector<int>({2}));
    std::vector<float> ramp(1000);
    for (std::size_t i = 0; i < ramp.size(); ++i) { ramp[i] = static_cast<float>(i) / 2000.0f; }
    std::vector<std::string> pk = c.audioPackets(2, ramp.data(), ramp.size());
    CHECK(pk.size() == 1 && pk[0].size() == 64 + 512 * 2);
    CHECK(le32(pk[0], 0) == 1 && le32(pk[0], 4) == 48000 && le32(pk[0], 8) == 0 && le32(pk[0], 20) == 512 && le32(pk[0], 28) == 1 && le32(pk[0], 32) == 0);
    pk = c.audioPackets(2, ramp.data(), ramp.size());   // 488 left over + 1000 = 1488: two more packets
    CHECK(pk.size() == 2 && le32(pk[0], 32) == 1 && le32(pk[1], 32) == 2);
    CHECK(c.audioPackets(1, ramp.data(), ramp.size()).empty());   // radio 1's stream was never started
    c.onText("AUDIO_STOP:1;");
    CHECK(c.audioRadios().empty() && c.audioPackets(2, ramp.data(), ramp.size()).empty());
    // stereo float32 at 24 kHz: both channels carry the signal; the default packet is 1024 samples (3.4), length counts every sample
    c.onText("AUDIO_SAMPLERATE:24000;AUDIO_STREAM_SAMPLE_TYPE:float32;AUDIO_STREAM_CHANNELS:2;AUDIO_STREAM_SAMPLES:1024;AUDIO_START:0;");
    std::vector<float> big(48000, 0.5f);
    pk = c.audioPackets(1, big.data(), big.size());
    CHECK(!pk.empty());
    if (!pk.empty()) {
        CHECK(pk[0].size() == 64 + 1024 * 4 && le32(pk[0], 4) == 24000 && le32(pk[0], 20) == 1024 && le32(pk[0], 28) == 2 && le32(pk[0], 8) == 3);
        float l, r;
        std::uint32_t b1 = le32(pk[0], 64 + 4 * 600), b2 = le32(pk[0], 64 + 4 * 601);
        std::memcpy(&l, &b1, 4); std::memcpy(&r, &b2, 4);
        CHECK(std::fabs(l - 0.5f) < 1e-3f && std::fabs(r - 0.5f) < 1e-3f);   // DC through the decimator, after its start-up
    }
    // 48 kHz in, 24 kHz out: about half as many samples
    const std::size_t packets = pk.size();
    CHECK(packets >= 1 && packets <= 48000 / 2 / 512 + 1);
}

void testDecimator() {
    const double pi = 3.14159265358979323846;
    const auto measure = [&](int factor, double hz) {
        TciDecimator d(factor);
        std::vector<float> in(48000);
        for (std::size_t i = 0; i < in.size(); ++i) { in[i] = static_cast<float>(std::sin(2.0 * pi * hz * static_cast<double>(i) / 48000.0)); }
        std::vector<float> out;
        d.process(in.data(), in.size(), out);
        CHECK(out.size() == in.size() / static_cast<std::size_t>(factor));
        double peak = 0.0;
        for (std::size_t i = out.size() / 2; i < out.size(); ++i) { peak = std::max(peak, static_cast<double>(std::fabs(out[i]))); }   // after the settling
        return peak;
    };
    CHECK(std::fabs(measure(1, 1000.0) - 1.0) < 1e-3);
    for (const int f : {2, 4, 6}) {
        const double nyq = 24000.0 / f;
        const double pass = measure(f, 0.25 * nyq);
        CHECK(pass > 0.95 && pass < 1.05);
        const double stop = measure(f, 1.25 * nyq);   // above the new Nyquist: would alias into the band without the filter
        CHECK(stop < 0.01);   // better than -40 dB
        std::printf("decimator /%d: %.0f Hz passes at %.4f, %.0f Hz (would alias) comes through at %.5f\n", f, 0.25 * nyq, pass, 1.25 * nyq, stop);
    }
    // chunked processing equals one big call
    TciDecimator a(4), b(4);
    std::vector<float> in(10000), whole, parts;
    for (std::size_t i = 0; i < in.size(); ++i) { in[i] = static_cast<float>(std::sin(0.01 * static_cast<double>(i)) + 0.3 * std::sin(0.7 * static_cast<double>(i))); }
    a.process(in.data(), in.size(), whole);
    for (std::size_t off = 0; off < in.size(); off += 777) { b.process(in.data() + off, std::min<std::size_t>(777, in.size() - off), parts); }
    CHECK(whole.size() == parts.size());
    double maxd = 0.0;
    for (std::size_t i = 0; i < std::min(whole.size(), parts.size()); ++i) { maxd = std::max(maxd, static_cast<double>(std::fabs(whole[i] - parts[i]))); }
    CHECK(maxd < 1e-6);
}

void testHostile() {
    Stub s;
    TciConnection c(s);
    c.begin();
    std::mt19937 rng(424242);
    const char* frags[] = {"VFO", "DDS", "IF", "MODULATION", "RX_ENABLE", "RX_FILTER_BAND", "SQL_LEVEL", "SQL_ENABLE", "AUDIO_START", "AUDIO_SAMPLERATE",
                           "AUDIO_STREAM_SAMPLES", "TRX", "TUNE", "START", "STOP", ":", ",", ";", "0", "1", "-1", "9999999999999999999", "true", "usb",
                           "\x01", "\xFF", " ", "\r\n", "7100000", "-0", "1e9", "NaN"};
    const int nf = sizeof(frags) / sizeof(frags[0]);
    for (int iter = 0; iter < 20000; ++iter) {
        std::string text;
        const int n = 1 + static_cast<int>(rng() % 12);
        for (int i = 0; i < n; ++i) { text += frags[rng() % static_cast<unsigned>(nf)]; }
        c.onText(text);
        if (iter % 50 == 0) { s.apply(); c.tick(t += 40); }
        if (iter % 500 == 0) { s.waiting.clear(); s.all.erase(s.all.begin(), s.all.end() - std::min<std::size_t>(s.all.size(), 100)); }
    }
    for (const FoxCommand& cmd : s.all) { CHECK(kAllowed.count(cmd.op) == 1); }
    // pure random bytes
    for (int iter = 0; iter < 5000; ++iter) {
        std::string text(rng() % 300, '\0');
        for (char& ch : text) { ch = static_cast<char>(rng()); }
        c.onText(text);
    }
    // an enormous message is read as nothing
    const std::uint64_t before = c.counters().oversize;
    c.onText(std::string(100000, 'a'));
    CHECK(c.counters().oversize == before + 1);
    // every number the engine will see is finite
    for (const FoxCommand& cmd : s.all) { for (double d : cmd.num) { CHECK(std::isfinite(d)); } }
    // the batch the connection hands over per message is bounded by the per-message command limit
    CHECK(s.all.size() < 20000u * 32u);
}

}  // namespace

int main() {
    testParsing();
    testStartup();
    testTuning();
    testEcho();
    testReceiveOnly();
    testReceivers();
    testAudio();
    testDecimator();
    testHostile();
    return testSummary("test_tci_protocol");
}
