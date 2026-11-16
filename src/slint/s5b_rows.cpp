// s5b_rows.cpp - SCREEN LANE S5b's ROWS (wave 8; docs/multi-radio-records/screen-s5b.md): the rows lane S5b adds to the Source section's panel, members of S5Rows (s5_rows.hpp) so they share its facts, drafts and sender.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdlib>

#include "core/app_commands.hpp"
#include "core/i18n.hpp"
#include "lanel_logic.hpp"
#include "s5_rows.hpp"
#include "s5b_logic.hpp"

namespace foxsdr::slints5 {

namespace {
namespace slk = foxsdr::slintlanek;
namespace sll = foxsdr::slintlanel;
using slk::Row;
using slk::rHeading;
using slk::rNote;
using slk::rReadout;

// the screen's words: every literal below goes through the string tables (tools/i18n_keys.py finds the tr("...") calls)
std::string tr(const char* s) { return cascade::i18n::tr(s); }
}  // namespace

void S5Rows::buildS5b(std::vector<Row>& v) {
    // ---------------------------------------------------------------- G1-14 analogue IF bandwidth
    {
        const std::string cur = fact("radio." + rn() + ".ifBandwidth");
        v.push_back(rHeading(tr("Analogue IF bandwidth")));
        const std::string words = foxsdr::slints5b::ifBandwidthWords(cur);
        if (words.empty()) {
            v.push_back(rNote(tr("This radio has no adjustable analogue filter. Only an RTL-SDR opened by FoxSDR's own driver has one.")));
        } else {
            v.push_back(rNote(tr("The RTL-SDR tuner's own filter in front of its converter. Automatic follows the sample rate; a narrower filter keeps a strong neighbour out, a wider one lets more of the band through.")));
            v.push_back(rReadout(tr("Now"), words));
            field(v, "b.ifbw", tr("Bandwidth (kHz)"), "", tr("300 to 8000; empty or 0 = automatic"));
            key(v, "b.ifbw.set", tr("Set the analogue bandwidth"), [this] {
                long long hz = 0; std::string e;
                if (!foxsdr::slints5b::parseIfBandwidthKHz(dv("b.ifbw"), hz, e)) { refuseHere(tr("Set the analogue bandwidth"), e); return; }
                Cmd c; c.op = FOXAPI_OP_SET_DEVICE_OPTION; c.i0 = hz; c.text = "if_bandwidth"; send(tr("Set the analogue bandwidth"), c);
            }, true, tr("Read back from the radio after each change."));
            key(v, "b.ifbw.auto", tr("Automatic (from the sample rate)"), [this] {
                draft_["b.ifbw"] = std::string();
                Cmd c; c.op = FOXAPI_OP_SET_DEVICE_OPTION; c.i0 = 0; c.text = "if_bandwidth"; send(tr("Automatic (from the sample rate)"), c);
            }, cur != "0");
        }
    }
}

}  // namespace foxsdr::slints5
