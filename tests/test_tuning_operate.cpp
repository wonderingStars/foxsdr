// The operating-side models of the tuning lane: per-band memory, frequency history, RIT/XIT, channel plans with the channel mode, the companion-program
// launcher (stand-in spawner only: no process is ever started here), and the keyboard-action handlers.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/band_memory.hpp"
#include "core/channel_plans.hpp"
#include "core/companion_launcher.hpp"
#include "core/freq_history.hpp"
#include "core/key_actions.hpp"
#include "core/rit_xit.hpp"
#include "test_check.hpp"

namespace ct = cascade::core::tuning;

namespace {

struct Lcg {
    std::uint64_t s;
    explicit Lcg(std::uint64_t seed) : s(seed) {}
    std::uint32_t u32() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<std::uint32_t>(s >> 32); }
};

std::string mutate(const std::string& base, Lcg& r) {
    std::string s = base;
    const int edits = 1 + static_cast<int>(r.u32() % 4);
    for (int e = 0; e < edits; ++e) {
        switch (r.u32() % 4) {
            case 0: s[r.u32() % s.size()] = static_cast<char>(r.u32() & 0xFF); break;
            case 1: s.erase(r.u32() % s.size(), 1 + r.u32() % 8); break;
            case 2: s.insert(r.u32() % s.size(), 1, "{}[]\":,0123456789e-+.\\/"[r.u32() % 23]); break;
            default: s.resize(1 + r.u32() % s.size()); break;
        }
        if (s.empty()) { s = "x"; }
    }
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
ct::BandSettings full(double vol, const char* agc, int att, double top, double range, double zoom, const char* ant) {
    ct::BandSettings s;
    s.volume = vol; s.agc = agc; s.attenuatorDb = att; s.spectrumTopDb = top; s.spectrumRangeDb = range; s.zoomSpanHz = zoom; s.antenna = ant;
    return s;
}

void testBandMemory() {
    ct::BandMemory m;
    std::string err;
    CHECK(!m.recall("40 m").has_value());
    CHECK(m.remember("40 m", full(0.4, "slow", 10, -30, 80, 200000, "Antenna A"), &err));
    auto r = m.recall("40 m");
    CHECK(r.has_value() && *r == full(0.4, "slow", 10, -30, 80, 200000, "Antenna A"));
    // only the set fields change; the others are kept
    ct::BandSettings vol;
    vol.volume = 0.9;
    CHECK(m.remember("40 m", vol));
    r = m.recall("40 m");
    CHECK(r && *r->volume == 0.9 && *r->agc == "slow" && *r->antenna == "Antenna A");
    // an empty set remembers nothing and creates nothing
    CHECK(m.remember("20 m", ct::BandSettings()) && m.size() == 1);
    // the mask turns kinds off, on the way in and on the way out
    ct::BandMemory k;
    k.setEnabledFields(ct::kBandAntenna | ct::kBandAgc);
    CHECK(k.enabledFields() == (ct::kBandAntenna | ct::kBandAgc));
    CHECK(k.remember("airband", full(0.5, "fast", 0, -20, 90, 1e6, "Antenna B")));
    auto kr = k.recall("airband");
    CHECK(kr && !kr->volume && !kr->attenuatorDb && !kr->zoomSpanHz && !kr->spectrumTopDb && *kr->agc == "fast" && *kr->antenna == "Antenna B");
    ct::BandSettings onlyVol;
    onlyVol.volume = 0.2;
    CHECK(k.remember("empty", onlyVol) && !k.recall("empty").has_value());   // nothing enabled was in it
    k.setEnabledFields(ct::kBandAllFields);
    CHECK(!k.recall("airband")->volume);                                     // it was never stored
    k.setEnabledFields(0xFFFF);
    CHECK(k.enabledFields() == ct::kBandAllFields);
    // the band switch
    ct::BandMemory b;
    const auto sw0 = b.onBandChange("40 m", "20 m", full(0.3, "slow", 0, -30, 80, 1e5, "Antenna A"));
    CHECK(sw0.saved && !sw0.apply.has_value());             // 40 m filed, 20 m has nothing yet
    const auto sw1 = b.onBandChange("20 m", "40 m", full(0.7, "fast", 6, -40, 70, 2e5, "Antenna B"));
    CHECK(sw1.saved && sw1.apply && *sw1.apply->volume == 0.3 && *sw1.apply->antenna == "Antenna A");
    const auto sw2 = b.onBandChange("40 m", "40 m", full(0.1, "off", 0, -30, 80, 1e5, "Antenna A"));
    CHECK(!sw2.saved && !sw2.apply);                       // the same band: nothing moves
    CHECK(*b.recall("40 m")->volume == 0.3);
    const auto sw3 = b.onBandChange("", "20 m", ct::BandSettings());
    CHECK(!sw3.saved && sw3.apply && *sw3.apply->volume == 0.7);   // from no band (a gap in the plan): only the recall
    const auto sw4 = b.onBandChange("20 m", "", full(0.55, "slow", 0, -30, 80, 1e5, "Antenna A"));
    CHECK(sw4.saved && !sw4.apply);                        // into a gap: file the old band, apply nothing
    CHECK(*b.recall("20 m")->volume == 0.55);
    // refusals change nothing
    const nlohmann::json before = b.toJson();
    CHECK(!b.remember("", vol, &err));
    CHECK(!b.remember(std::string(49, 'b'), vol));
    CHECK(!b.remember("b", full(1.1, "slow", 0, -30, 80, 1e5, "A")));
    CHECK(!b.remember("b", full(-0.1, "slow", 0, -30, 80, 1e5, "A")));
    CHECK(!b.remember("b", full(0.5, "a b", 0, -30, 80, 1e5, "A")));
    CHECK(!b.remember("b", full(0.5, "slow", 121, -30, 80, 1e5, "A")));
    CHECK(!b.remember("b", full(0.5, "slow", -1, -30, 80, 1e5, "A")));
    CHECK(!b.remember("b", full(0.5, "slow", 0, 101, 80, 1e5, "A")));
    CHECK(!b.remember("b", full(0.5, "slow", 0, -30, 9, 1e5, "A")));
    CHECK(!b.remember("b", full(0.5, "slow", 0, -30, 80, 99, "A")));
    CHECK(!b.remember("b", full(0.5, "slow", 0, -30, 80, 1e5, "")));
    CHECK(!b.remember("b", full(0.5, "slow", 0, -30, 80, 1e5, "bad\nport")));
    CHECK(!b.remember("b", full(0.5, "slow", 0, -30, 80, 1e5, std::string(33, 'a').c_str())));
    ct::BandSettings nan; nan.volume = NAN;
    CHECK(!b.remember("b", nan));
    CHECK(b.toJson() == before);
    // boundaries accepted
    CHECK(b.remember("edge", full(0.0, "o", 0, -200, 10, 100, "a")));
    CHECK(b.remember("edge", full(1.0, "12345678", 120, 100, 200, 1e10, std::string(32, 'a').c_str())));
    // the 200-band limit
    ct::BandMemory many;
    for (int i = 0; i < 200; ++i) { CHECK(many.remember("band" + std::to_string(i), vol)); }
    CHECK(!many.remember("band200", vol, &err) && err == "more than 200 bands");
    CHECK(many.remember("band0", vol));
    CHECK(many.forget("band0") && !many.forget("band0") && many.size() == 199);
    many.clear();
    CHECK(many.size() == 0);
}

void testBandMemoryJson() {
    ct::BandMemory a;
    CHECK(a.remember("40 m", full(0.4, "slow", 10, -30, 80, 200000, "Antenna A")));
    ct::BandSettings p; p.volume = 0.25;
    CHECK(a.remember("Airband \xC3\xA9", p));
    ct::BandMemory b;
    std::string err;
    CHECK(b.fromJson(nlohmann::json::parse(a.toJson().dump()), &err));
    CHECK(b.toJson() == a.toJson());
    CHECK(*b.recall("40 m")->attenuatorDb == 10 && *b.recall("Airband \xC3\xA9")->volume == 0.25);
    const nlohmann::json good = a.toJson();
    auto refuse = [&](nlohmann::json bad) {
        ct::BandMemory c;
        CHECK(c.remember("keep", p));
        CHECK(!c.fromJson(bad, &err));
        CHECK(c.size() == 1 && c.recall("keep").has_value());
    };
    nlohmann::json v = good; v["version"] = 2; refuse(v);
    v = good; v.erase("bands"); refuse(v);
    v = good; v["bands"][0]["volume"] = 2.0; refuse(v);
    v = good; v["bands"][0]["volume"] = "x"; refuse(v);
    v = good; v["bands"][0]["agc"] = 5; refuse(v);
    v = good; v["bands"][0]["attenuatorDb"] = 3.5; refuse(v);
    v = good; v["bands"][0]["attenuatorDb"] = 500; refuse(v);
    v = good; v["bands"][0]["antenna"] = ""; refuse(v);
    v = good; v["bands"][0]["band"] = ""; refuse(v);
    v = good; v["bands"][1]["band"] = v["bands"][0]["band"]; refuse(v);       // repeated
    v = good; v["bands"][1] = {{"band", "empty"}}; refuse(v);                   // an entry with no setting
    v = good; v["bands"][0] = 7; refuse(v);
    refuse(nlohmann::json::array());
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
void testHistory() {
    ct::FreqHistory h;
    CHECK(!h.back().has_value() && !h.forward().has_value() && !h.current().has_value() && !h.canBack() && !h.canForward());
    CHECK(h.push(7.074e6, "USB", 0));
    CHECK(h.push(14.074e6, "USB", 5000));
    CHECK(h.push(145.5e6, "NFM", 10000));
    CHECK(h.size() == 3 && h.position() == 2);
    auto e = h.back();
    CHECK(e && e->hz == 14.074e6 && e->mode == "USB");
    // the retune the back() causes arrives as a push of the same entry: ignored
    CHECK(!h.push(14.074e6, "USB", 10100));
    CHECK(h.size() == 3 && h.position() == 1);
    e = h.back();
    CHECK(e && e->hz == 7.074e6);
    CHECK(!h.back().has_value() && !h.canBack());
    e = h.forward();
    CHECK(e && e->hz == 14.074e6);
    // the same frequency in another mode is a different entry; 0.4 Hz away in the same mode is the same
    CHECK(!h.push(14.074e6 + 0.4, "USB", 20000));
    CHECK(h.push(14.074e6, "LSB", 30000));
    CHECK(h.size() == 3);                                 // pushed after back(): the entry ahead (145.5) was thrown away: [7.074, 14.074 USB, 14.074 LSB]
    CHECK(h.entries()[2].mode == "LSB" && !h.canForward());
    // refusals
    CHECK(!h.push(NAN, "USB", 0) && !h.push(0.0, "USB", 0) && !h.push(-1.0, "USB", 0) && !h.push(1e14, "USB", 0) && !h.push(1e6, "bad mode", 0) && !h.push(1e6, "", 0));
    CHECK(h.size() == 3);
    // capacity: 20 pushes keep the newest 16
    ct::FreqHistory c;
    for (int i = 0; i < 20; ++i) { CHECK(c.push(1e6 + i * 1000.0, "AM", i * 10000.0)); }
    CHECK(c.size() == 16 && c.entries().front().hz == 1e6 + 4000.0 && c.entries().back().hz == 1e6 + 19000.0 && c.position() == 15);
    int back = 0;
    while (c.back()) { ++back; }
    CHECK(back == 15);
    h.clear();
    CHECK(h.size() == 0 && !h.current());
}

void testHistoryCoalesce() {
    ct::FreqHistory h;
    CHECK(h.coalesceMs() == 1500.0);
    // the first entry is kept even when the next arrives at once
    CHECK(h.push(7e6, "AM", 0));
    CHECK(h.push(7.001e6, "AM", 100));
    CHECK(h.size() == 2);
    // a dial spin after a pause is one step: [A, B, spin end]
    ct::FreqHistory s;
    s.push(7e6, "AM", 0);
    s.push(8e6, "AM", 5000);
    for (int i = 1; i <= 30; ++i) { s.push(8e6 + i * 1000.0, "AM", 5000 + i * 100.0); }
    CHECK(s.size() == 3);
    CHECK(s.entries()[0].hz == 7e6 && s.entries()[1].hz == 8e6 && s.entries()[2].hz == 8e6 + 30000.0);
    auto b = s.back();
    CHECK(b && b->hz == 8e6);                              // back goes to where the spin began
    // a spin that comes back to where it began leaves no new entry
    ct::FreqHistory t;
    t.push(7e6, "AM", 0);
    t.push(8e6, "AM", 5000);
    t.push(8.1e6, "AM", 5100);
    t.push(8.2e6, "AM", 5200);
    CHECK(t.size() == 3);
    t.push(8e6, "AM", 5300);
    CHECK(t.size() == 2 && t.entries().back().hz == 8e6);
    // the pause ends a spin: the next move after 2 s is a new entry
    ct::FreqHistory u;
    u.push(7e6, "AM", 0);
    u.push(8e6, "AM", 5000);
    u.push(8.1e6, "AM", 5100);
    u.push(9e6, "AM", 8000);
    CHECK(u.size() == 4);
    // after back() a move is a new step, never replaces
    ct::FreqHistory n;
    n.push(7e6, "AM", 0);
    n.push(8e6, "AM", 5000);
    n.push(8.1e6, "AM", 5100);
    n.back();
    CHECK(n.push(7.5e6, "AM", 5150));
    CHECK(n.size() == 3 && n.entries().back().hz == 7.5e6 && n.entries()[1].hz == 8e6);
    // a clock that goes backwards never coalesces; coalescing can be turned off
    ct::FreqHistory bw;
    bw.push(7e6, "AM", 10000);
    bw.push(8e6, "AM", 20000);
    bw.push(8.1e6, "AM", 20100);
    bw.push(8.2e6, "AM", 100);
    CHECK(bw.size() == 4);
    ct::FreqHistory off;
    off.setCoalesceMs(0);
    CHECK(off.coalesceMs() == 0.0);
    off.push(7e6, "AM", 0);
    off.push(8e6, "AM", 5000);
    for (int i = 1; i <= 5; ++i) { off.push(8e6 + i * 1000.0, "AM", 5000 + i * 10.0); }
    CHECK(off.size() == 7);
    off.setCoalesceMs(NAN);
    CHECK(off.coalesceMs() == 0.0);
}

void testHistoryJson() {
    ct::FreqHistory a;
    a.push(7e6, "USB", 0);
    a.push(8e6, "AM", 5000);
    a.push(9e6, "NFM", 10000);
    a.back();
    ct::FreqHistory b;
    std::string err;
    CHECK(b.fromJson(nlohmann::json::parse(a.toJson().dump()), &err));
    CHECK(b.size() == 3 && b.position() == 1 && b.entries()[2].mode == "NFM");
    const nlohmann::json good = a.toJson();
    auto refuse = [&](nlohmann::json bad) {
        ct::FreqHistory c;
        c.push(1e6, "AM", 0);
        CHECK(!c.fromJson(bad, &err));
        CHECK(c.size() == 1 && c.entries()[0].hz == 1e6);
    };
    nlohmann::json v = good; v["version"] = 2; refuse(v);
    v = good; v["position"] = 3; refuse(v);
    v = good; v["position"] = -1; refuse(v);
    v = good; v.erase("entries"); refuse(v);
    v = good; v["entries"][0]["hz"] = -5; refuse(v);
    v = good; v["entries"][0]["mode"] = "a b"; refuse(v);
    v = good; v["entries"] = nlohmann::json::array(); for (int i = 0; i < 17; ++i) { v["entries"].push_back(good["entries"][0]); } v["position"] = 0; refuse(v);
    v = good; v["entries"] = nlohmann::json::array(); v["position"] = 1; refuse(v);
    refuse(nlohmann::json::array());
    ct::FreqHistory e;
    CHECK(e.fromJson({{"version", 1}, {"position", 0}, {"entries", nlohmann::json::array()}}) && e.size() == 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
void testRitXit() {
    ct::RitXit r;
    CHECK(r.maxHz() == 9999.0 && r.catMaxRit() == 9999 && r.catMaxXit() == 9999);
    CHECK(r.rxHz(7.074e6) == 7.074e6 && r.txHz(7.074e6) == 7.074e6 && r.receiveShiftHz() == 0.0);
    CHECK(r.setRit(350.0) && r.setXit(-120.0));
    CHECK(r.rxHz(7.074e6) == 7.074e6 && r.txHz(7.074e6) == 7.074e6);        // set but off: nothing moves
    r.setRitOn(true);
    CHECK(r.rxHz(7.074e6) == 7.074e6 + 350.0 && r.txHz(7.074e6) == 7.074e6 && r.receiveShiftHz() == 350.0);
    r.setXitOn(true);
    CHECK(r.txHz(7.074e6) == 7.074e6 - 120.0 && r.rxHz(7.074e6) == 7.074e6 + 350.0);       // independent
    // marks: the spectrum shows each line only when its clarifier is on and moved
    ct::ClarifierMarks m = r.marks(7.074e6);
    CHECK(m.showRx && m.rxHz == 7.074e6 + 350.0 && m.showTx && m.txHz == 7.074e6 - 120.0);
    r.clearRit();
    m = r.marks(7.074e6);
    CHECK(!m.showRx && m.showTx);
    r.setXitOn(false);
    CHECK(!r.marks(1e6).showTx);
    // clamping says so
    CHECK(!r.setRit(20000.0) && r.rit() == 9999.0);
    CHECK(!r.setRit(-20000.0) && r.rit() == -9999.0);
    CHECK(r.setRit(9999.0) && r.rit() == 9999.0);                                           // the edge is taken as given
    CHECK(!r.stepRit(1.0) && r.rit() == 9999.0);
    CHECK(r.stepRit(-1.0) && r.rit() == 9998.0);
    CHECK(r.stepXit(25.0) && r.xit() == -95.0);
    // NaN and infinity change nothing
    CHECK(!r.setRit(NAN) && r.rit() == 9998.0);
    CHECK(!r.stepRit(INFINITY) && !r.stepXit(NAN) && r.rit() == 9998.0);
    CHECK(!r.setXit(INFINITY));
    // a lower limit re-clamps; invalid limits are refused
    CHECK(r.setMaxHz(500.0) && r.rit() == 500.0 && r.xit() == -95.0 && r.catMaxRit() == 500);
    CHECK(!r.setMaxHz(0.5) && !r.setMaxHz(20001.0) && !r.setMaxHz(NAN) && r.maxHz() == 500.0);
    CHECK(r.setMaxHz(20000.0) && r.setMaxHz(1.0) && r.rit() == 1.0);
    r.setMaxHz(9999.0);
    r.clear();
    CHECK(r.rit() == 0.0 && r.xit() == 0.0);
    // clear on retune: off by default, the edge does not clear, a larger move does
    r.setRit(500.0); r.setXit(300.0);
    CHECK(!r.onVfoMoved(7e6, 14e6) && r.rit() == 500.0);
    r.setClearOnRetuneHz(5000.0);
    CHECK(!r.onVfoMoved(7.000e6, 7.005e6) && r.rit() == 500.0);                         // exactly the limit
    CHECK(r.onVfoMoved(7.000e6, 7.0051e6) && r.rit() == 0.0 && r.xit() == 0.0);
    CHECK(!r.onVfoMoved(7e6, 14e6));                                                      // nothing to clear
    CHECK(!r.onVfoMoved(NAN, 14e6));
    r.setClearOnRetuneHz(-1.0);
    CHECK(r.clearOnRetuneHz() == 0.0);
    // text
    CHECK(ct::RitXit::formatOffset(350.0) == "+350 Hz");
    CHECK(ct::RitXit::formatOffset(-350.4) == "-350 Hz");
    CHECK(ct::RitXit::formatOffset(0.0) == "0 Hz");
    CHECK(ct::RitXit::formatOffset(1250.0) == "+1.25 kHz");
    CHECK(ct::RitXit::formatOffset(-9999.0) == "-10.00 kHz");
    CHECK(ct::RitXit::formatOffset(999.4) == "+999 Hz");
    CHECK(ct::RitXit::formatOffset(NAN) == "0 Hz");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
const ct::PlanChannel* ch(const ct::ChannelPlan& p, const std::string& label) {
    for (const auto& c : p.channels) { if (c.label == label) { return &c; } }
    return nullptr;
}

void testChannelPlans() {
    const auto& plans = ct::channelPlans();
    CHECK(plans.size() == 8);
    std::set<std::string> ids;
    for (const ct::ChannelPlan& p : plans) {
        CHECK(ids.insert(p.id).second);
        CHECK(!p.name.empty() && !p.sources.empty() && !p.channels.empty() && !p.modeHint.empty());
        std::set<std::string> labels;
        std::set<long long> freqs;
        for (const auto& c : p.channels) {
            CHECK(labels.insert(c.label).second);
            CHECK(c.freqHz > 1e6 && c.freqHz < 1e9);
            CHECK(freqs.insert(std::llround(c.freqHz)).second);              // no two channels of a plan share a frequency
            CHECK(c.freqHz == std::floor(c.freqHz));                         // whole hertz
        }
        for (const std::string& s : p.sources) { CHECK(!s.empty()); }
        CHECK(ct::findPlan(p.id) == &p);
        CHECK(p.tuneBandwidthHz() > 0.0);
    }
    CHECK(ct::findPlan("nope") == nullptr);

    // PMR446 (ECC/DEC/(15)05 decides 2-4)
    const ct::ChannelPlan& pa = *ct::findPlan("pmr446-analogue");
    CHECK(pa.channels.size() == 16 && pa.channels[0].freqHz == 446006250.0 && pa.channels[15].freqHz == 446193750.0 && pa.spacingHz == 12500.0);
    for (std::size_t i = 1; i < pa.channels.size(); ++i) { CHECK(pa.channels[i].freqHz - pa.channels[i - 1].freqHz == 12500.0); }
    const ct::ChannelPlan& pd = *ct::findPlan("pmr446-digital-6k25");
    CHECK(pd.channels.size() == 32 && pd.channels[0].freqHz == 446003125.0 && pd.channels[31].freqHz == 446196875.0 && pd.widthHz == 6250.0);
    for (std::size_t i = 1; i < pd.channels.size(); ++i) { CHECK(pd.channels[i].freqHz - pd.channels[i - 1].freqHz == 6250.0); }
    const ct::ChannelPlan& pd12 = *ct::findPlan("pmr446-digital-12k5");
    CHECK(pd12.channels.size() == 16 && pd12.channels[0].freqHz == 446006250.0);
    for (const ct::ChannelPlan* p : {&pa, &pd, &pd12}) {
        CHECK(p->channels.front().freqHz > 446.0e6 && p->channels.back().freqHz < 446.2e6);   // inside the band 446.0-446.2 MHz
    }

    // CB (EN 300 433 table 1)
    const ct::ChannelPlan& cb = *ct::findPlan("cb-cept-40");
    CHECK(cb.channels.size() == 40 && cb.channels[0].freqHz == 26965000.0 && cb.channels[39].freqHz == 27405000.0 && cb.widthHz == 10000.0);
    CHECK(ch(cb, "23")->freqHz == 27255000.0 && ch(cb, "24")->freqHz == 27235000.0 && ch(cb, "25")->freqHz == 27245000.0);   // the table's own order
    CHECK(ch(cb, "4")->freqHz == 27005000.0 && ch(cb, "20")->freqHz == 27205000.0 && ch(cb, "21")->freqHz == 27215000.0 && ch(cb, "22")->freqHz == 27225000.0);
    CHECK(ch(cb, "26")->freqHz == 27265000.0 && ch(cb, "38")->freqHz == 27385000.0 && ch(cb, "39")->freqHz == 27395000.0);
    CHECK(!ch(cb, "23")->note.empty() && ch(cb, "10")->note.empty());
    // the 40 carriers are the 10 kHz lattice 26.965..27.405 with the five gaps the standard leaves
    std::set<long long> have;
    for (const auto& c : cb.channels) { have.insert(std::llround(c.freqHz)); }
    int gaps = 0;
    for (long long f = 26965000; f <= 27405000; f += 10000) { if (!have.count(f)) { ++gaps; } }
    CHECK(gaps == 5);   // 45 lattice points, 40 channels: the unused ones are 26.995, 27.045, 27.095, 27.145 and 27.195 MHz
    CHECK(!have.count(26995000) && !have.count(27045000) && !have.count(27095000) && !have.count(27145000) && !have.count(27195000));
    // FRS (47 CFR 95.563)
    const ct::ChannelPlan& frs = *ct::findPlan("frs-us");
    CHECK(frs.channels.size() == 22 && ch(frs, "1")->freqHz == 462562500.0 && ch(frs, "7")->freqHz == 462712500.0 && ch(frs, "8")->freqHz == 467562500.0 &&
          ch(frs, "14")->freqHz == 467712500.0 && ch(frs, "15")->freqHz == 462550000.0 && ch(frs, "22")->freqHz == 462725000.0 && frs.widthHz == 12500.0);
    // GMRS (47 CFR 95.1763): 8 + 7 + 8 + 7 frequencies, ours the numbering
    const ct::ChannelPlan& gmrs = *ct::findPlan("gmrs-us");
    CHECK(gmrs.channels.size() == 30 && gmrs.numberingIsOurs && gmrs.widthHz == 0.0 && gmrs.tuneBandwidthHz() == 12500.0);
    CHECK(ch(gmrs, "G1")->freqHz == 462550000.0 && ch(gmrs, "G8")->freqHz == 462725000.0 && ch(gmrs, "G9")->freqHz == 462562500.0 && ch(gmrs, "G15")->freqHz == 462712500.0 &&
          ch(gmrs, "G16")->freqHz == 467550000.0 && ch(gmrs, "G23")->freqHz == 467725000.0 && ch(gmrs, "G24")->freqHz == 467562500.0 && ch(gmrs, "G30")->freqHz == 467712500.0);
    // every FRS frequency is also a GMRS frequency (the rule: shared)
    std::set<long long> g;
    for (const auto& c : gmrs.channels) { g.insert(std::llround(c.freqHz)); }
    for (const auto& c : frs.channels) { CHECK(g.count(std::llround(c.freqHz)) == 1); }
    // MURS (47 CFR 95.2763)
    const ct::ChannelPlan& murs = *ct::findPlan("murs-us");
    CHECK(murs.channels.size() == 5 && murs.numberingIsOurs && ch(murs, "M1")->freqHz == 151820000.0 && ch(murs, "M2")->freqHz == 151880000.0 && ch(murs, "M3")->freqHz == 151940000.0 &&
          ch(murs, "M4")->freqHz == 154570000.0 && ch(murs, "M5")->freqHz == 154600000.0);
    CHECK(!frs.numberingIsOurs && !pa.numberingIsOurs && !cb.numberingIsOurs);
    // Marine (ITU RR Appendix 18, Rev. WRC-19)
    const ct::ChannelPlan& mar = *ct::findPlan("marine-vhf-itu");
    CHECK(mar.channels.size() == 28 + 29 + 2 && mar.spacingHz == 25000.0);
    auto pair = [&](const char* l, double ship, double coast) { const auto* c = ch(mar, l); CHECK(c != nullptr); if (c) { CHECK(c->freqHz == ship); CHECK(c->altHz == coast); } };
    pair("01", 156050000.0, 160650000.0);
    pair("05", 156250000.0, 160850000.0);
    pair("06", 156300000.0, 0.0);
    pair("07", 156350000.0, 160950000.0);
    pair("09", 156450000.0, 0.0);
    pair("13", 156650000.0, 0.0);
    pair("16", 156800000.0, 0.0);
    pair("17", 156850000.0, 0.0);
    pair("18", 156900000.0, 161500000.0);
    pair("28", 157400000.0, 162000000.0);
    pair("60", 156025000.0, 160625000.0);
    pair("66", 156325000.0, 160925000.0);
    pair("67", 156375000.0, 0.0);
    pair("70", 156525000.0, 0.0);
    pair("77", 156875000.0, 0.0);
    pair("78", 156925000.0, 161525000.0);
    pair("86", 157325000.0, 161925000.0);
    pair("87", 157375000.0, 0.0);
    pair("88", 157425000.0, 0.0);
    pair("AIS 1", 161975000.0, 0.0);
    pair("AIS 2", 162025000.0, 0.0);
    CHECK(ch(mar, "16")->note == "distress, safety and calling" && ch(mar, "70")->note.find("digital selective calling") == 0);
    CHECK(ch(mar, "29") == nullptr && ch(mar, "59") == nullptr && ch(mar, "89") == nullptr && ch(mar, "00") == nullptr);
    // ship transmit frequencies climb in 25 kHz steps across 01..28 and 60..88 interleaved
    std::set<long long> ships;
    for (const auto& c : mar.channels) { ships.insert(std::llround(c.freqHz)); }
    CHECK(ships.size() == mar.channels.size());
    long long prev = 0;
    int gap25 = 0;
    for (const long long f : ships) { if (prev != 0 && f - prev == 25000) { ++gap25; } prev = f; }
    CHECK(gap25 >= 55);
    CHECK(ct::channelMhzText(446006250.0) == "446.00625 MHz");
    CHECK(ct::channelMhzText(156800000.0) == "156.800 MHz");
    CHECK(ct::channelMhzText(446003125.0) == "446.003125 MHz");
}

void testChannelCursor() {
    ct::ChannelCursor c;
    CHECK(!c.active() && c.current() == nullptr && c.next() == nullptr && c.prev() == nullptr && !c.goTo("1") && c.nearest(1e8, 1e3) == nullptr);
    CHECK(!c.setPlan("nope") && !c.active());
    CHECK(c.setPlan("pmr446-analogue"));
    CHECK(c.current()->label == "1" && c.plan()->id == "pmr446-analogue");
    CHECK(c.next()->label == "2" && c.prev()->label == "1");
    CHECK(c.prev()->label == "16");                                // back from the first wraps
    CHECK(c.next()->label == "1");                                 // and forward from the last wraps
    for (int i = 0; i < 16; ++i) { c.next(); }
    CHECK(c.current()->label == "1");
    CHECK(c.goTo("9") && c.current()->freqHz == 446006250.0 + 8 * 12500.0);
    CHECK(!c.goTo("99") && c.current()->label == "9");            // a failed goTo keeps the place
    // nearest: within tolerance, the cursor follows it; outside, nothing
    const ct::PlanChannel* n = c.nearest(446.0185e6, 2000.0);
    CHECK(n != nullptr && n->label == "2" && c.current()->label == "2");
    CHECK(c.nearest(446.0125e6, 100.0) == nullptr && c.current()->label == "2");
    CHECK(c.nearest(NAN, 100.0) == nullptr && c.nearest(446e6, -1.0) == nullptr);
    CHECK(c.nearest(446.00625e6, 0.0)->label == "1");             // exactly on a channel with zero tolerance
    // a failed setPlan keeps the old plan
    CHECK(!c.setPlan("x") && c.plan()->id == "pmr446-analogue");
    // marine: either frequency of a duplex channel finds it
    CHECK(c.setPlan("marine-vhf-itu"));
    CHECK(c.nearest(160.650e6, 1000.0)->label == "01" && c.nearest(156.050e6, 1000.0)->label == "01");
    CHECK(c.nearest(156.800e6, 1000.0)->label == "16");
    CHECK(c.goTo("AIS 2") && c.next()->label == "01");
    CHECK(c.prev()->label == "AIS 2");
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
struct FakeSpawner : ct::Spawner {
    std::vector<std::pair<std::string, std::string>> calls;
    bool ok = true;
    bool spawn(const std::string& exe, const std::string& cmd, std::string& err) override {
        calls.emplace_back(exe, cmd);
        if (!ok) { err = "stand-in refused"; }
        return ok;
    }
};

// CommandLineToArgvW as Microsoft documents it (2n backslashes + quote -> n backslashes, a quote begins or ends a quoted run; 2n+1 backslashes + quote -> n
// backslashes and a literal quote; backslashes not before a quote are literal), used to prove the quoting round-trips.
std::vector<std::string> parseWindowsCommandLine(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) { ++i; }
        if (i >= s.size()) { break; }
        std::string arg;
        bool inQ = false;
        while (i < s.size() && (inQ || (s[i] != ' ' && s[i] != '\t'))) {
            std::size_t bs = 0;
            while (i < s.size() && s[i] == '\\') { ++bs; ++i; }
            if (i < s.size() && s[i] == '"') {
                arg.append(bs / 2, '\\');
                if (bs % 2 == 1) { arg.push_back('"'); ++i; }
                else {
                    if (inQ && i + 1 < s.size() && s[i + 1] == '"') { arg.push_back('"'); i += 2; }   // "" inside a quoted run: a literal quote
                    else { inQ = !inQ; ++i; }
                }
            } else {
                arg.append(bs, '\\');
                if (i < s.size() && (inQ || (s[i] != ' ' && s[i] != '\t'))) { arg.push_back(s[i]); ++i; }
            }
        }
        out.push_back(arg);
    }
    return out;
}

void testLauncherPaths() {
    const char* good[] = {"C:\\Tools\\logger.exe", "c:/tools/x.EXE", "D:\\a b\\c d\\prog.exe", "/usr/bin/tool", "C:\\x.exe"};
    for (const char* p : good) { CHECK(ct::pathProblem(p).empty()); }
    struct Bad { std::string path; const char* why; };
    const std::vector<Bad> bad = {
        {"", "empty"}, {"logger.exe", "relative"}, {"tools\\logger.exe", "relative"}, {".\\logger.exe", "dot relative"}, {"..\\logger.exe", "dotdot relative"},
        {"\\\\server\\share\\x.exe", "UNC"}, {"//server/share/x.exe", "UNC slashes"}, {"\\\\?\\C:\\x.exe", "extended"}, {"\\\\.\\pipe\\x", "device"},
        {"C:\\a\\..\\b.exe", "dotdot part"}, {"C:\\a\\.\\b.exe", "dot part"}, {"C:\\tools\\*.exe", "wildcard"}, {"C:\\tools\\a?.exe", "wildcard ?"}, {"C:\\to\"ols\\a.exe", "quote"},
        {"C:\\tools\\a|b.exe", "pipe"}, {"C:\\tools\\a.bat", "bat"}, {"C:\\tools\\a.cmd", "cmd"}, {"C:\\tools\\a.com", "com"}, {"C:\\tools\\a.ps1", "ps1"}, {"C:\\tools\\a", "no extension"},
        {"C:\\tools\\.exe", "bare extension"}, {"C:\\tools\\", "folder"}, {"C:x.exe", "drive relative"}, {"C:\\x.exe:stream", "stream"}, {"http://host/x.exe", "url"},
        {"C:\\a\nb.exe", "newline"}, {std::string("C:\\a\0b.exe", 11), "NUL"}, {"C:\\a\x07.exe", "bell"}, {"C:\\" + std::string(260, 'a') + ".exe", "too long"},
        {"C:\\tools\\a.exe ", "trailing space"}, {"C:\\tools\\a.exe.", "trailing dot"}, {"cmd", "bare name on PATH"}, {"C:\\tools\\a.exe\\", "exe as folder"},
    };
    for (const Bad& b : bad) {
        const std::string why = ct::pathProblem(b.path);
        CHECK(!why.empty());
        if (why.empty()) { std::printf("  path accepted: %s\n", b.why); }
    }
    // the boundary: 259 bytes is accepted, 260 refused
    std::string e259 = "C:\\" + std::string(259 - 3 - 4, 'a') + ".exe";
    CHECK(e259.size() == 259 && ct::pathProblem(e259).empty());
    CHECK(!ct::pathProblem(e259 + "a").empty());
    CHECK(!ct::pathProblem("C:\\" + std::string(260 - 3 - 4, 'a') + ".exe").empty());
    // arguments
    CHECK(ct::argProblem("--port=4533").empty() && ct::argProblem("").empty() && ct::argProblem(std::string(1024, 'a')).empty());
    CHECK(!ct::argProblem(std::string(1025, 'a')).empty() && !ct::argProblem("a\nb").empty() && !ct::argProblem(std::string("a\0b", 3)).empty() && !ct::argProblem("a\tb").empty());
    // entries
    ct::CompanionEntry e;
    e.label = "Logger"; e.path = "C:\\Tools\\logger.exe";
    CHECK(ct::entryProblem(e).empty());
    e.label = ""; CHECK(!ct::entryProblem(e).empty());
    e.label = "bad\nlabel"; CHECK(!ct::entryProblem(e).empty());
    e.label = "ok"; e.args = std::vector<std::string>(17, "a"); CHECK(!ct::entryProblem(e).empty());
    e.args = std::vector<std::string>(16, "a"); CHECK(ct::entryProblem(e).empty());
}

void testQuoting() {
    CHECK(ct::quoteWindowsArg("plain") == "plain");
    CHECK(ct::quoteWindowsArg("a b") == "\"a b\"");
    CHECK(ct::quoteWindowsArg("") == "\"\"");
    CHECK(ct::quoteWindowsArg("a\"b") == "\"a\\\"b\"");
    CHECK(ct::quoteWindowsArg("C:\\dir\\") == "C:\\dir\\");
    CHECK(ct::quoteWindowsArg("C:\\my dir\\") == "\"C:\\my dir\\\\\"");
    CHECK(ct::quoteWindowsCommandLine("C:\\Tools\\a b.exe", {"-x", "two words", "q\"q"}) == "\"C:\\Tools\\a b.exe\" -x \"two words\" \"q\\\"q\"");
    // property: any argument list reads back exactly through the documented parse
    Lcg rng(77);
    const char alpha[] = "ab \t\"\\\\\\'=;&|<>%^x1";
    for (int i = 0; i < 20000; ++i) {
        std::vector<std::string> args;
        const int n = static_cast<int>(rng.u32() % 4);
        for (int k = 0; k < n; ++k) {
            std::string a;
            const int len = static_cast<int>(rng.u32() % 9);
            for (int c = 0; c < len; ++c) { a.push_back(alpha[rng.u32() % (sizeof alpha - 1)]); }
            args.push_back(a);
        }
        const std::string exe = (rng.u32() % 2) ? "C:\\Tools\\logger.exe" : "C:\\My Tools\\log ger.exe";
        const std::string line = ct::quoteWindowsCommandLine(exe, args);
        std::vector<std::string> want = {exe};
        want.insert(want.end(), args.begin(), args.end());
        CHECK(parseWindowsCommandLine(line) == want);
    }
    // injection: an argument that tries to add another argument or a shell command stays one argument
    const std::string line = ct::quoteWindowsCommandLine("C:\\a.exe", {"x\" --evil \"y", "a & calc.exe", "%PATH%"});
    CHECK(parseWindowsCommandLine(line).size() == 4);
}

void testLauncher() {
    std::set<std::string> existing = {"C:\\Tools\\logger.exe", "C:\\Tools\\rot.exe"};
    ct::CompanionLauncher L([&](const std::string& p) { return existing.count(p) != 0; });
    FakeSpawner sp;
    std::string err;
    CHECK(!L.enabled());                                           // OFF by default
    ct::CompanionEntry a;
    a.label = "Logger"; a.path = "C:\\Tools\\logger.exe"; a.args = {"--port", "4533", "my log.txt"};
    ct::CompanionEntry b;
    b.label = "Rotator"; b.path = "C:\\Tools\\rot.exe"; b.startWithApp = true;
    CHECK(L.add(a, &err) && L.add(b, &err) && L.entries().size() == 2);
    // off: refused whoever asks
    auto r = L.launch(0, ct::LaunchOrigin::UserAction, 0, sp);
    CHECK(!r.launched && r.reason == "the launcher is off" && sp.calls.empty());
    CHECK(L.launchAtAppStart(0, sp) == 0 && sp.calls.empty());
    L.setEnabled(true);
    // the user starts it: exact command line, no shell
    r = L.launch(0, ct::LaunchOrigin::UserAction, 1000, sp);
    CHECK(r.launched && r.reason.empty() && sp.calls.size() == 1);
    CHECK(sp.calls[0].first == "C:\\Tools\\logger.exe" && sp.calls[0].second == "C:\\Tools\\logger.exe --port 4533 \"my log.txt\"");
    // a double click does not start two copies; after 2 s it may start again; a clock that goes backwards counts as "a moment ago" only inside the window
    r = L.launch(0, ct::LaunchOrigin::UserAction, 1500, sp);
    CHECK(!r.launched && r.reason == "started a moment ago" && sp.calls.size() == 1);
    r = L.launch(0, ct::LaunchOrigin::UserAction, 2999, sp);
    CHECK(!r.launched);
    r = L.launch(0, ct::LaunchOrigin::UserAction, 3000, sp);
    CHECK(r.launched && sp.calls.size() == 2);
    r = L.launch(0, ct::LaunchOrigin::UserAction, 100, sp);   // far earlier than the last start: never mistaken for a repeat
    CHECK(r.launched && sp.calls.size() == 3);
    // the origins that may NEVER launch
    for (const ct::LaunchOrigin o : {ct::LaunchOrigin::Remote, ct::LaunchOrigin::Plugin, ct::LaunchOrigin::Network}) {
        r = L.launch(1, o, 1e6, sp);
        CHECK(!r.launched && r.reason == "only the user or the app start may launch a program");
    }
    CHECK(sp.calls.size() == 3);
    // app start: only entries the user ticked
    r = L.launch(0, ct::LaunchOrigin::AppStart, 1e7, sp);
    CHECK(!r.launched && r.reason == "the entry is not set to start with the app");
    r = L.launch(1, ct::LaunchOrigin::AppStart, 1e7, sp);
    CHECK(r.launched && sp.calls.size() == 4 && sp.calls[3].second == "C:\\Tools\\rot.exe");
    sp.calls.clear();
    CHECK(L.launchAtAppStart(2e7, sp) == 1 && sp.calls.size() == 1 && sp.calls[0].first == "C:\\Tools\\rot.exe");
    // missing file, bad index, a failing spawner
    existing.erase("C:\\Tools\\rot.exe");
    r = L.launch(1, ct::LaunchOrigin::UserAction, 3e7, sp);
    CHECK(!r.launched && r.reason == "the file does not exist");
    r = L.launch(5, ct::LaunchOrigin::UserAction, 3e7, sp);
    CHECK(!r.launched && r.reason == "no such entry");
    sp.ok = false;
    r = L.launch(0, ct::LaunchOrigin::UserAction, 4e7, sp);
    CHECK(!r.launched && r.reason == "stand-in refused");
    sp.ok = true;
    r = L.launch(0, ct::LaunchOrigin::UserAction, 4e7 + 10, sp);       // a failed start does not begin the 2 s window
    CHECK(r.launched);
    // a launcher whose file check is missing refuses
    ct::CompanionLauncher none(nullptr);
    none.setEnabled(true);
    CHECK(none.add(a) && !none.launch(0, ct::LaunchOrigin::UserAction, 0, sp).launched);
    // entries are validated on add: nothing invalid gets in
    ct::CompanionEntry bad = a;
    bad.path = "logger.exe";
    CHECK(!L.add(bad, &err) && err == "the path is not a full path" && L.entries().size() == 2);
    bad = a; bad.path = "C:\\Tools\\x.bat";
    CHECK(!L.add(bad));
    for (int i = 0; i < 6; ++i) { CHECK(L.add(a)); }
    CHECK(L.entries().size() == 8 && !L.add(a, &err) && err == "more than 8 entries");
    CHECK(L.remove(7) && !L.remove(7) && L.entries().size() == 7);
    // the stand-in recorded every call: nothing else ever reached a spawner (this test starts no process)
}

void testLauncherJson() {
    ct::CompanionLauncher a([](const std::string&) { return true; });
    ct::CompanionEntry e;
    e.label = "Logger \xC3\xA9"; e.path = "C:\\Tools\\logger.exe"; e.args = {"--x", "a b"}; e.startWithApp = true;
    CHECK(a.add(e));
    a.setEnabled(true);
    ct::CompanionLauncher b([](const std::string&) { return true; });
    std::string err;
    CHECK(b.fromJson(nlohmann::json::parse(a.toJson().dump()), &err));
    CHECK(b.enabled() && b.entries().size() == 1 && b.entries()[0].args == e.args && b.entries()[0].startWithApp && b.entries()[0].label == e.label);
    const nlohmann::json good = a.toJson();
    auto refuse = [&](nlohmann::json bad) {
        ct::CompanionLauncher c([](const std::string&) { return true; });
        CHECK(c.add(e));
        CHECK(!c.fromJson(bad, &err));
        CHECK(c.entries().size() == 1 && !c.enabled());
    };
    nlohmann::json v = good; v["version"] = 2; refuse(v);
    v = good; v.erase("enabled"); refuse(v);
    v = good; v["enabled"] = "yes"; refuse(v);
    v = good; v["entries"][0]["path"] = "logger.exe"; refuse(v);
    v = good; v["entries"][0]["path"] = "\\\\server\\x.exe"; refuse(v);
    v = good; v["entries"][0]["args"] = nlohmann::json::array({5}); refuse(v);
    v = good; v["entries"][0]["args"] = nlohmann::json::array({"a\nb"}); refuse(v);
    v = good; v["entries"][0].erase("startWithApp"); refuse(v);
    v = good; v["entries"] = nlohmann::json::array(); for (int i = 0; i < 9; ++i) { v["entries"].push_back(good["entries"][0]); } refuse(v);
    refuse(nlohmann::json::array());
    // a launcher loaded from a file that says "off" or says nothing is off; the default is off
    ct::CompanionLauncher d([](const std::string&) { return true; });
    CHECK(!d.enabled());
    CHECK(d.fromJson({{"version", 1}, {"enabled", false}, {"entries", nlohmann::json::array()}}) && !d.enabled());
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testLauncherSourceScan() {
    // The header starts nothing: no process-creation call, no shell, no network, no file write; only standard headers, nlohmann and the shared text rules.
    std::string here = __FILE__;
    for (char& c : here) { if (c == '\\') { c = '/'; } }
    const std::string root = here.substr(0, here.rfind("/tests/"));
    const std::string all = readFile(root + "/src/core/companion_launcher.hpp");
    CHECK(all.size() > 1000);
    std::string src;                                    // the code without its // comments (the comments name what is NOT done)
    {
        std::istringstream in(all);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t c = line.find("//");
            src += (c == std::string::npos ? line : line.substr(0, c)) + std::string(1, static_cast<char>(10));
        }
    }
    for (const char* bad : {"CreateProcess", "ShellExecute", "system(", "popen", "_wsystem", "WinExec", "execv", "execl", "fork(", "posix_spawn", "<windows.h>", "<winsock", "socket(", "ofstream", "fopen", "std::filesystem"}) {
        CHECK(src.find(bad) == std::string::npos);
        if (src.find(bad) != std::string::npos) { std::printf("  found: %s\n", bad); }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
void testKeyActions() {
    const auto& list = ct::keyActionList();
    CHECK(list.size() == 7);
    std::set<std::string> ids, chords;
    for (const auto& a : list) {
        CHECK(ids.insert(a.idText).second && chords.insert(a.proposedChord).second);
        CHECK(a.label[0] != '\0' && ct::findKeyAction(a.idText) == &a);
    }
    CHECK(ct::findKeyAction("nope") == nullptr);
    // bandwidth: up and down through the mode's list, no wrap
    const auto usb = ct::defaultBandwidths("USB");
    CHECK(!usb.empty() && ct::defaultBandwidths("CW").size() > 2 && ct::defaultBandwidths("RAW").empty());
    CHECK(*ct::bandwidthStep(usb, 2400, +1) == 2700 && *ct::bandwidthStep(usb, 2400, -1) == 2100);
    CHECK(*ct::bandwidthStep(usb, 3600, +1) == 3600 && *ct::bandwidthStep(usb, 1800, -1) == 1800);   // the ends hold
    CHECK(*ct::bandwidthStep(usb, 2500, +1) == 2700 && *ct::bandwidthStep(usb, 2500, -1) == 2400);   // from between entries
    CHECK(*ct::bandwidthStep(usb, 5000, +1) == 5000 && *ct::bandwidthStep(usb, 5000, -1) == 3600);   // from beyond the list
    CHECK(*ct::bandwidthStep(usb, 100, -1) == 100 && *ct::bandwidthStep(usb, 100, +1) == 1800);
    CHECK(!ct::bandwidthStep({}, 2400, 1) && !ct::bandwidthStep(usb, NAN, 1) && !ct::bandwidthStep(usb, 2400, 0));
    // cycle step: wraps both ways; from a value not in the list
    const std::vector<double> steps = {100, 500, 1000, 5000, 12500};
    CHECK(*ct::cycleStep(steps, 1000, +1) == 5000 && *ct::cycleStep(steps, 12500, +1) == 100);
    CHECK(*ct::cycleStep(steps, 100, -1) == 12500 && *ct::cycleStep(steps, 1000, -1) == 500);
    CHECK(*ct::cycleStep(steps, 700, +1) == 1000 && *ct::cycleStep(steps, 700, -1) == 500);
    CHECK(*ct::cycleStep({5000, 100, 1000}, 100, +1) == 1000);   // an unsorted list is sorted first
    CHECK(!ct::cycleStep({}, 100, 1) && !ct::cycleStep(steps, NAN, 1) && !ct::cycleStep(steps, 100, 0));
    CHECK(*ct::cycleStep({1000}, 1000, +1) == 1000 && *ct::cycleStep({1000}, 1000, -1) == 1000);
    // cycle mode
    const std::vector<std::string> modes = {"AM", "NFM", "WFM", "USB", "LSB", "CW"};
    CHECK(*ct::cycleMode(modes, "AM", +1) == "NFM" && *ct::cycleMode(modes, "CW", +1) == "AM");
    CHECK(*ct::cycleMode(modes, "AM", -1) == "CW" && *ct::cycleMode(modes, "USB", -1) == "WFM");
    CHECK(*ct::cycleMode(modes, "RAW", +1) == "AM" && *ct::cycleMode(modes, "RAW", -1) == "CW");
    CHECK(!ct::cycleMode({}, "AM", 1) && !ct::cycleMode(modes, "AM", 0));
    // centre the signal
    CHECK(*ct::centreOnSignal(7.074e6, 7.074e6 + 200.0, 3000.0) == 7.074e6 + 200.0);
    CHECK(!ct::centreOnSignal(7.074e6, 7.074e6 + 3.0, 3000.0));            // already centred (inside the dead band)
    CHECK(*ct::centreOnSignal(7.074e6, 7.074e6 + 5.1, 3000.0) == 7.074e6 + 5.1);
    CHECK(!ct::centreOnSignal(7.074e6, 7.074e6 + 5.0, 3000.0));            // the dead-band edge counts as centred
    CHECK(*ct::centreOnSignal(7.074e6, 7.074e6 + 3000.0, 3000.0) == 7.074e6 + 3000.0);   // the largest move allowed
    CHECK(!ct::centreOnSignal(7.074e6, 7.074e6 + 3000.1, 3000.0));
    CHECK(!ct::centreOnSignal(7.074e6, NAN, 3000.0) && !ct::centreOnSignal(NAN, 7e6, 3000.0) && !ct::centreOnSignal(7e6, 7.001e6, 0.0) && !ct::centreOnSignal(7e6, 7.001e6, NAN));
    // bookmarks
    const std::vector<double> bm = {100e6, 121.5e6, 145.5e6, 446.0e6};
    CHECK(*ct::nextBookmark(bm, 121.5e6) == 145.5e6 && *ct::nextBookmark(bm, 110e6) == 121.5e6 && *ct::nextBookmark(bm, 446.0e6) == 100e6 && *ct::nextBookmark(bm, 500e6) == 100e6);
    CHECK(*ct::nextBookmark(bm, 121.5e6 + 0.4) == 145.5e6);                  // within half a hertz is "on" the bookmark
    CHECK(*ct::previousBookmark(bm, 145.5e6) == 121.5e6 && *ct::previousBookmark(bm, 130e6) == 121.5e6 && *ct::previousBookmark(bm, 100e6) == 446.0e6 && *ct::previousBookmark(bm, 50e6) == 446.0e6);
    CHECK(*ct::previousBookmark(bm, 121.5e6 - 0.4) == 100e6);
    CHECK(*ct::nextBookmark({7e6}, 7e6) == 7e6 && *ct::previousBookmark({7e6}, 7e6) == 7e6);
    CHECK(!ct::nextBookmark({}, 7e6) && !ct::previousBookmark({}, 7e6) && !ct::nextBookmark(bm, NAN) && !ct::previousBookmark(bm, NAN));
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
void fuzzJson() {
    Lcg r(31337);
    ct::BandMemory bm;
    bm.remember("40 m", full(0.4, "slow", 10, -30, 80, 200000, "Antenna A"));
    const std::string bmText = bm.toJson().dump();
    ct::FreqHistory h;
    h.push(7e6, "USB", 0); h.push(8e6, "AM", 5000);
    const std::string hText = h.toJson().dump();
    ct::CompanionLauncher cl([](const std::string&) { return true; });
    ct::CompanionEntry e; e.label = "L"; e.path = "C:\\Tools\\l.exe"; e.args = {"a"};
    cl.add(e);
    const std::string clText = cl.toJson().dump();
    int okB = 0, okH = 0, okC = 0;
    const int kRuns = 30000;
    for (int i = 0; i < kRuns; ++i) {
        const nlohmann::json jb = nlohmann::json::parse(mutate(bmText, r), nullptr, false);
        ct::BandMemory b;
        if (!jb.is_discarded() && b.fromJson(jb)) { ++okB; }
        const nlohmann::json jh = nlohmann::json::parse(mutate(hText, r), nullptr, false);
        ct::FreqHistory hh;
        if (!jh.is_discarded() && hh.fromJson(jh)) { ++okH; CHECK(hh.size() <= 16); }
        const nlohmann::json jc = nlohmann::json::parse(mutate(clText, r), nullptr, false);
        ct::CompanionLauncher c([](const std::string&) { return true; });
        if (!jc.is_discarded() && c.fromJson(jc)) {
            ++okC;
            for (const auto& en : c.entries()) { CHECK(ct::entryProblem(en).empty()); }
        }
    }
    std::printf("fuzz: %d runs each; accepted band memory %d, history %d, launcher %d\n", kRuns, okB, okH, okC);
    CHECK(okB > 50 && okH > 50 && okC > 50);
}

}  // namespace

int main() {
    testBandMemory();
    testBandMemoryJson();
    testHistory();
    testHistoryCoalesce();
    testHistoryJson();
    testRitXit();
    testChannelPlans();
    testChannelCursor();
    testLauncherPaths();
    testQuoting();
    testLauncher();
    testLauncherJson();
    testLauncherSourceScan();
    testKeyActions();
    fuzzJson();
    return testSummary("test_tuning_operate");
}
