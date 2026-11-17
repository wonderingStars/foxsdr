// test_cat_surface.cpp - what a CAT (rigctld-protocol) client can make the program do: the whole list, and the proof that nothing else is reachable (security lane 2, S4bS-cat).
//
// THE LIST. Every request word executeCatLine accepts, short and long form (net/cat_protocol.cpp):
//   f  \get_freq        reads the tuned frequency
//   F  \set_freq        tunes (centre and/or VFO offset, inside the program's own tuning range)
//   m  \get_mode        reads the mode and passband
//   M  \set_mode        sets one of the demodulators and, optionally, its passband (inside the program's own range)
//   v  \get_vfo         reads "VFOA"
//   V  \set_vfo         accepts the one VFO's own name, refuses any other
//   s  \get_split_vfo   reads "not split"
//   S  \set_split_vfo   accepts 0, refuses anything else
//   t  \get_ptt         reads "receive"
//   T  \set_ptt         accepts 0 (receive), REFUSES 1 and every other value
//   \get_powerstat      reads running or not
//   \dump_state \chk_vfo   fixed text
//   q  Q  \quit         hangs up
// and nothing else: every other word is answered "not implemented".
//
// THE PROOF. (1) Over the list, with every argument shape that matters, and over 300000 random and mutated lines: the only ControlRequest a line can produce has centre, offset, mode and
// bandwidth set and no other member; translated by the same function the server uses, the commands are SET_CENTRE, SET_VFO_OFFSET, SET_MODE and SET_BANDWIDTH and nothing else, none carries
// text (so no path or program name can ride on one); the values are inside the tuning and passband ranges. (2) PTT and split: no argument turns either on. (3) The server's source holds no
// call that keys, writes a file or starts a program: a scan of cat_server.cpp and cat_protocol.cpp for those words.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "net/cat_protocol.hpp"
#include "net/control_ops.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

CatRadio radio() {
    CatRadio r;
    r.running = true;
    r.centerHz = 145'000'000.0;
    r.sampleRateHz = 2'400'000.0;
    r.vfoOffsetHz = 0.0;
    r.bandwidthHz = 12'500.0;
    r.mode = "NFM";
    return r;
}

// True when `r` has no member set other than the four a CAT client may move.
bool onlyTheFour(const ControlRequest& r0) {
    ControlRequest r = r0;
    r.centerHz.reset();
    r.vfoOffsetHz.reset();
    r.mode.reset();
    r.bandwidthHz.reset();
    // every other member of ControlRequest, by name: a new member that a CAT line could set must be added here and argued for
    return !r.running && !r.squelchDb && !r.volume && !r.muted && !r.dbMin && !r.dbMax && !r.deemphasisIndex && !r.nrEnabled && !r.nrStrength && !r.notchEnabled && !r.notchFreqHz &&
           !r.notchQ && !r.autoNotch && !r.stereoEnabled;
}

std::set<std::uint32_t> allowedOps() {
    return {FOXAPI_OP_SET_CENTRE, FOXAPI_OP_SET_VFO_OFFSET, FOXAPI_OP_SET_MODE, FOXAPI_OP_SET_BANDWIDTH};
}

std::size_t g_controls = 0, g_lines = 0;

// One line through the protocol and the translation the server uses; every property of the list is checked here.
void judge(const std::string& line) {
    const CatRadio st = radio();
    const CatResult r = executeCatLine(line, st);
    ++g_lines;
    if (!r.hasControl) { return; }
    ++g_controls;
    if (!onlyTheFour(r.control)) { std::printf("      a line set a member other than the four: [%s]\n", line.c_str()); }
    CHECK(onlyTheFour(r.control));
    const std::vector<cascade::core::cmd::QueuedCommand> cmds = controlRequestToCommands(r.control, ControlOpsContext{});
    const std::set<std::uint32_t> ok = allowedOps();
    CHECK(cmds.size() <= 4u);
    for (const auto& q : cmds) {
        if (ok.count(q.c.op) == 0) { std::printf("      a line produced op %u: [%s]\n", q.c.op, line.c_str()); }
        CHECK(ok.count(q.c.op) == 1);
        CHECK(q.longText.empty());
    }
    // the values are the program's own legal ones
    if (r.control.centerHz) { CHECK(*r.control.centerHz >= kMinCenterHz && *r.control.centerHz <= kMaxCenterHz); }
    if (r.control.bandwidthHz) { CHECK(*r.control.bandwidthHz >= kMinBandwidthHz && *r.control.bandwidthHz <= kMaxBandwidthHz); }
}

void testTheList() {
    // every accepted word, with arguments of every shape
    const std::vector<std::string> words = {"f", "F", "m", "M", "v", "V", "s", "S", "t", "T", "q", "Q", "\\get_freq", "\\set_freq", "\\get_mode", "\\set_mode", "\\get_vfo", "\\set_vfo",
                                            "\\get_split_vfo", "\\set_split_vfo", "\\get_ptt", "\\set_ptt", "\\get_powerstat", "\\dump_state", "\\chk_vfo", "\\quit"};
    const std::vector<std::string> args = {"", " 0", " 1", " 2", " -1", " 145000000", " 145500000.5", " 1e9", " 99999999999999999999", " nan", " inf", " VFOA", " VFOB", " USB", " USB 2400", " FM 0",
                                           " AM 99999999", " CW -5", " PKTUSB 3000", " RAW", " WFM", " ../../x", " C:\\x", " $(calc)", " ;calc", " 1 2 3 4 5 6 7 8"};
    for (const std::string& w : words) {
        for (const std::string& a : args) {
            judge(w + a);
            judge(w + a + "\r");
        }
    }
    std::printf("the list: %zu words, %zu arguments, %zu lines judged, %zu of them set a control\n", words.size(), args.size(), g_lines, g_controls);
    CHECK(g_controls > 20);
    // a word that is not on the list is "not implemented", never obeyed
    for (const char* w : {"x", "R", "\\set_func", "\\send_cmd", "\\set_conf", "\\set_level", "\\set_parm", "\\set_trn", "\\send_morse", "\\set_rptr_shift", "\\set_powerstat", "\\power", "\\vfo_op",
                          "\\set_bank", "\\set_mem", "\\set_channel", "\\set_ctcss_tone", "\\set_ant", "\\set_xit", "\\set_rit", "\\set_ts", "\\set_clock", "\\halt", "\\send_dtmf", "w", "W", "!", "|"}) {
        const CatResult r = executeCatLine(std::string(w) + " 1 2", radio());
        CHECK(!r.hasControl);
        CHECK(r.reply.rfind("RPRT -", 0) == 0);
    }
}

void testNoWayToKeyOrSplit() {
    for (const char* line : {"T 1", "T 2", "T 99", "T -1", "T 1 VFOA", "\\set_ptt 1", "\\set_ptt 3", "S 1", "S 1 VFOA", "S 2", "\\set_split_vfo 1", "\\set_split_vfo 1 VFOB"}) {
        const CatResult r = executeCatLine(line, radio());
        CHECK(!r.hasControl);                   // nothing is handed to the engine
        CHECK(r.reply.rfind("RPRT -", 0) == 0);  // and the client is told no
    }
    for (const char* line : {"T 0", "S 0", "\\set_ptt 0", "\\set_split_vfo 0"}) {
        const CatResult r = executeCatLine(line, radio());
        CHECK(!r.hasControl && r.reply == "RPRT 0\n");   // "receive" and "not split" are the state it is in: said yes to, nothing done
    }
    CHECK(executeCatLine("t", radio()).reply == "0\n");   // the PTT always reads receive
    CHECK(executeCatLine("s", radio()).reply == "0\nVFOA\n");
}

void testRandomLines() {
    std::mt19937 rng(20261117);
    const std::vector<std::string> seeds = {"F 145000000", "F 7100000", "M USB 2400", "M FM 0", "V VFOA", "S 0", "T 0", "T 1", "S 1", "\\set_freq 1e6", "\\set_mode AM 6000", "\\dump_state", "f", "m", "t",
                                            "\\set_ptt 1", "\\send_morse CQ", "\\set_level 1 2", "\\set_func PTT 1", "\\send_cmd 00", "\\set_conf rig_pathname=C:\\x", "\\set_trn RIG"};
    for (int i = 0; i < 300000; ++i) {
        std::string line;
        switch (i % 4) {
            case 0: {   // random bytes
                const std::size_t n = rng() % 80;
                for (std::size_t k = 0; k < n; ++k) { line.push_back(static_cast<char>(rng())); }
                break;
            }
            case 1: {   // a seed with a few bytes replaced
                line = seeds[rng() % seeds.size()];
                for (int f = 0, m = static_cast<int>(rng() % 4); f < m && !line.empty(); ++f) { line[rng() % line.size()] = static_cast<char>(rng()); }
                break;
            }
            case 2: {   // a seed with words of another seed glued on
                line = seeds[rng() % seeds.size()] + " " + seeds[rng() % seeds.size()];
                break;
            }
            default: {  // a long-form word made of random letters, with numeric arguments
                line = "\\";
                const std::size_t n = 1 + rng() % 12;
                for (std::size_t k = 0; k < n; ++k) { line.push_back(static_cast<char>("abcdefghijklmnopqrstuvwxyz_"[rng() % 27])); }
                line += " " + std::to_string(static_cast<long long>(rng()) - 2000000000LL) + " " + std::to_string(rng() % 100000);
                break;
            }
        }
        // a line with an embedded newline is two lines to the server; judge each as the server would
        std::size_t from = 0;
        for (;;) {
            const std::size_t nl = line.find('\n', from);
            judge(line.substr(from, nl == std::string::npos ? std::string::npos : nl - from));
            if (nl == std::string::npos) { break; }
            from = nl + 1;
        }
    }
    std::printf("random: %zu lines judged in all, %zu set a control, every one inside the four members and four ops\n", g_lines, g_controls);
}

// The two source files that carry the protocol hold no call that keys a transmitter, opens a file or starts a program.
std::string g_root;
void testSourceHoldsNoSuchCall() {
    for (const char* rel : {"src/net/cat_protocol.cpp", "src/net/cat_server.cpp"}) {
        std::string path = g_root + "/" + rel;
        std::ifstream f(path, std::ios::binary);
        CHECK(static_cast<bool>(f));
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(text.size() > 1000);
        for (const char* bad : {"FOXAPI_OP_TX", "TRANSMIT", "SET_PTT", "fopen(", "ofstream", "ifstream", "CreateProcess", "ShellExecute", "system(", "popen(", "LoadLibrary", "FOXAPI_OP_FILE", "FOXAPI_OP_SETTING",
                                "FOXAPP_OP_"}) {
            if (text.find(bad) != std::string::npos) { std::printf("      %s contains %s\n", rel, bad); }
            CHECK(text.find(bad) == std::string::npos);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) { g_root = argv[1]; }
    testTheList();
    testNoWayToKeyOrSplit();
    testRandomLines();
    testSourceHoldsNoSuchCall();
    return testSummary("test_cat_surface");
}
