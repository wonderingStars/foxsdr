// NUMBERED CHANNEL PLANS with a channel mode: PMR446, CB (CEPT), FRS, GMRS, MURS and marine VHF.
//
// A channel plan is a table of numbered channels, each a frequency; the channel MODE is tuning by channel number: ChannelCursor steps next / previous
// (wrapping), goes to a channel by its label, and finds the channel nearest a frequency. Receive only: a marine duplex channel has two frequencies (what
// ships transmit, what coast stations transmit) and both are kept; nothing here transmits.
//
// WHERE EVERY TABLE COMES FROM (each page was opened; this file copies no program's data file). `sources` of each plan repeats it.
//   - PMR446: ECC Decision (15)05, amended 2 March 2018, decides 2, 3 and 4 (docdb.cept.org/download/1491, read in full): the band 446.0-446.2 MHz,
//     analogue channels on 12.5 kHz spacing with the lowest carrier 446.00625 MHz; digital channels on 6.25 kHz spacing (lowest carrier 446.003125 MHz)
//     and 12.5 kHz spacing (lowest 446.00625 MHz) as of 1 January 2018. The channel COUNTS (16 on 12.5 kHz, 32 on 6.25 kHz) are the band divided by the
//     spacing, arithmetic of this file: 446.00625 + 15 x 12.5 kHz = 446.19375 MHz and 446.003125 + 31 x 6.25 kHz = 446.196875 MHz both end inside the band.
//   - CB (CEPT 26.960-27.410 MHz): ETSI EN 300 433 draft V2.1.0 (2016-02), clause 4.4 table 1, with a 10 kHz channel separation (etsi.org PDF, read as
//     text). The table is NOT in channel order at 23, 24 and 25: channel 23 is 27.255 MHz, 24 is 27.235 MHz, 25 is 27.245 MHz (the table's own order).
//     The UK 27/81 channels (27.60125-27.99125 MHz) are NOT built: Ofcom's IR 2027.2 answered HTTP 403 and only a statutory definition of the band edges
//     could be opened (see the record).
//   - FRS: 47 CFR 95.563 (eCFR, 22 channels, 12.5 kHz), GMRS: 47 CFR 95.1763 (four groups of centre frequencies), MURS: 47 CFR 95.2763 (five centre
//     frequencies), all read from the eCFR text on 2026-11-11. The GMRS and MURS rules give frequencies and no channel numbers: the labels G1..G30 and
//     M1..M5 are THIS program's, in the rule's order, and `numberingIsOurs` says so. No bandwidth is stated for them here (widthHz = 0): the rule's
//     bandwidth clause was read only as a search summary of a mirror, not opened.
//   - Marine VHF: ITU Radio Regulations 2024 edition, Volume 2, Appendix 18 (Rev. WRC-19), "Table of transmitting frequencies in the VHF maritime mobile
//     band", read as text from the ITU PDF. 25 kHz spacing (Note B). Ship transmit = 156.000 + 0.050 x N MHz for N = 1..28, 156.025 + 0.050 x (N - 60) for
//     60..88; the coast frequency is the ship's +4.600 MHz on the two-frequency channels (01-05, 07, 18-28, 60-66, 78-86) and the same as the ship's on
//     the single-frequency ones; AIS 1 = 161.975 and AIS 2 = 162.025 MHz. The test cross-checked the SET of every frequency of that table against the
//     computed set (all present, only 160.900 MHz extra: the four-digit channel 2006, Note r, not built). The four-digit channel designators (1xxx and
//     2xxx), ASM 1 and ASM 2 and the digital-only 12.5 kHz entries are NOT built.
//
// Pure data and small functions; no file, no thread. THREADS: the plan table is built once (function-local static) and is read only; a ChannelCursor is
// for one thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace cascade::core::tuning {

struct PlanChannel {
    std::string label;      // "16", "AIS 1", "G12"
    double freqHz = 0.0;    // the channel's frequency (marine: what ships transmit)
    double altHz = 0.0;     // marine two-frequency channels: what coast stations transmit; 0 when the same
    std::string note;       // a short English fact from the source, or empty
};

struct ChannelPlan {
    std::string id;
    std::string name;
    std::string region;
    double spacingHz = 0.0;       // channel spacing the source states or derives
    double widthHz = 0.0;         // the source's channel width, 0 = not verified here
    std::string modeHint;         // a DEFAULT the operator can change (the rule does not name a receiver mode)
    bool numberingIsOurs = false; // the labels are this program's, not the regulator's
    std::vector<PlanChannel> channels;
    std::vector<std::string> sources;

    double tuneBandwidthHz() const { return widthHz > 0.0 ? widthHz : 12500.0; }
};

namespace detail {

inline std::string num(int n) { return std::to_string(n); }

inline ChannelPlan pmr(const char* id, const char* name, double firstHz, double stepHz, int count) {
    ChannelPlan p;
    p.id = id; p.name = name; p.region = "Europe (CEPT)"; p.spacingHz = stepHz; p.widthHz = stepHz; p.modeHint = "NFM";
    for (int i = 0; i < count; ++i) { p.channels.push_back({num(i + 1), firstHz + static_cast<double>(i) * stepHz, 0.0, ""}); }
    p.sources = {"ECC Decision (15)05 (amended 2 March 2018), decides 2-4: https://docdb.cept.org/download/1491"};
    return p;
}

inline ChannelPlan cb() {
    ChannelPlan p;
    p.id = "cb-cept-40"; p.name = "CB 27 MHz (CEPT, 40 channels)"; p.region = "Europe (CEPT)"; p.spacingHz = 10000.0; p.widthHz = 10000.0; p.modeHint = "AM";
    // EN 300 433 draft V2.1.0 table 1, in the table's own order (carrier MHz by channel number)
    static const double mhz[40] = {26.965, 26.975, 26.985, 27.005, 27.015, 27.025, 27.035, 27.055, 27.065, 27.075, 27.085, 27.105, 27.115, 27.125, 27.135,
                                   27.155, 27.165, 27.175, 27.185, 27.205, 27.215, 27.225, 27.255, 27.235, 27.245, 27.265, 27.275, 27.285, 27.295, 27.305,
                                   27.315, 27.325, 27.335, 27.345, 27.355, 27.365, 27.375, 27.385, 27.395, 27.405};
    for (int i = 0; i < 40; ++i) { p.channels.push_back({num(i + 1), static_cast<double>(std::llround(mhz[i] * 1e6 / 1000.0)) * 1000.0, 0.0, ""}); }
    p.channels[22].note = "channels 23, 24 and 25 are out of frequency order in the standard's table";
    p.channels[23].note = p.channels[22].note;
    p.channels[24].note = p.channels[22].note;
    p.sources = {"ETSI EN 300 433 draft V2.1.0 (2016-02), clause 4.4 table 1: https://www.etsi.org/deliver/etsi_en/300400_300499/300433/02.01.00_20/en_300433v020100a.pdf",
                 "the mode is a default (AM); the permitted modulation depends on the national rule and was not read"};
    return p;
}

inline ChannelPlan frs() {
    ChannelPlan p;
    p.id = "frs-us"; p.name = "FRS (USA, 22 channels)"; p.region = "USA"; p.spacingHz = 12500.0; p.widthHz = 12500.0; p.modeHint = "NFM";
    static const double mhz[22] = {462.5625, 462.5875, 462.6125, 462.6375, 462.6625, 462.6875, 462.7125, 467.5625, 467.5875, 467.6125, 467.6375,
                                   467.6625, 467.6875, 467.7125, 462.5500, 462.5750, 462.6000, 462.6250, 462.6500, 462.6750, 462.7000, 462.7250};
    for (int i = 0; i < 22; ++i) { p.channels.push_back({num(i + 1), static_cast<double>(std::llround(mhz[i] * 1e6)), 0.0, ""}); }
    p.sources = {"47 CFR 95.563 FRS channels: https://www.ecfr.gov/api/versioner/v1/full/2026-11-01/title-47.xml?part=95&section=95.563",
                 "the rule says all FRS channels are also allotted to GMRS on a shared basis"};
    return p;
}

inline ChannelPlan gmrs() {
    ChannelPlan p;
    p.id = "gmrs-us"; p.name = "GMRS (USA, 30 frequencies)"; p.region = "USA"; p.spacingHz = 12500.0; p.modeHint = "NFM"; p.numberingIsOurs = true;
    int n = 1;
    auto group = [&](double firstMhz, int count, double stepKhz, const char* note) {
        for (int i = 0; i < count; ++i) { p.channels.push_back({"G" + num(n++), static_cast<double>(std::llround((firstMhz + i * stepKhz / 1000.0) * 1e6)), 0.0, note}); }
    };
    group(462.5500, 8, 25.0, "462 MHz main");
    group(462.5625, 7, 25.0, "462 MHz interstitial");
    group(467.5500, 8, 25.0, "467 MHz main: through a repeater or brief tests only for mobile, handheld and control units");
    group(467.5625, 7, 25.0, "467 MHz interstitial: handheld units only");
    p.sources = {"47 CFR 95.1763 GMRS channels: https://www.ecfr.gov/api/versioner/v1/full/2026-11-01/title-47.xml?part=95&section=95.1763",
                 "the rule lists the centre frequencies in four groups and gives no channel numbers: G1..G30 follow the rule's order and are this program's"};
    return p;
}

inline ChannelPlan murs() {
    ChannelPlan p;
    p.id = "murs-us"; p.name = "MURS (USA, 5 channels)"; p.region = "USA"; p.modeHint = "NFM"; p.numberingIsOurs = true;
    const double mhz[5] = {151.820, 151.880, 151.940, 154.570, 154.600};
    for (int i = 0; i < 5; ++i) { p.channels.push_back({"M" + num(i + 1), static_cast<double>(std::llround(mhz[i] * 1e6)), 0.0, ""}); }
    p.sources = {"47 CFR 95.2763 MURS channels: https://www.ecfr.gov/api/versioner/v1/full/2026-11-01/title-47.xml?part=95&section=95.2763",
                 "the rule lists five centre frequencies and no channel numbers: M1..M5 follow the rule's order and are this program's"};
    return p;
}

inline ChannelPlan marine() {
    ChannelPlan p;
    p.id = "marine-vhf-itu"; p.name = "Marine VHF (ITU Appendix 18)"; p.region = "International"; p.spacingHz = 25000.0; p.widthHz = 25000.0; p.modeHint = "NFM";
    auto duplex = [](int n) { return (n >= 1 && n <= 5) || n == 7 || (n >= 18 && n <= 28) || (n >= 60 && n <= 66) || (n >= 78 && n <= 86); };
    auto add = [&](int n) {
        const double ship = n <= 28 ? 156.000 + n * 0.050 : 156.025 + (n - 60) * 0.050;
        PlanChannel c;
        c.label = n < 10 ? "0" + num(n) : num(n);
        c.freqHz = static_cast<double>(std::llround(ship * 1e6));
        c.altHz = duplex(n) ? static_cast<double>(std::llround((ship + 4.600) * 1e6)) : 0.0;
        if (n == 16) { c.note = "distress, safety and calling"; }
        if (n == 70) { c.note = "digital selective calling for distress, safety and calling only"; }
        if (n == 13) { c.note = "navigation safety communication channel"; }
        if (n == 6) { c.note = "may also be used by aircraft for search and rescue"; }
        if (n == 75 || n == 76) { c.note = "navigation-related communications only, 1 W maximum"; }
        if (n == 15 || n == 17) { c.note = "may also be used for on-board communications at 1 W"; }
        p.channels.push_back(c);
    };
    for (int n = 1; n <= 28; ++n) { add(n); }
    for (int n = 60; n <= 88; ++n) { add(n); }
    p.channels.push_back({"AIS 1", 161975000.0, 0.0, "automatic identification system"});
    p.channels.push_back({"AIS 2", 162025000.0, 0.0, "automatic identification system"});
    p.sources = {"ITU Radio Regulations 2024 edition, Volume 2, Appendix 18 (Rev. WRC-19): https://search.itu.int/history/HistoryDigitalCollectionDocLibrary/1.49.48.en.102.pdf",
                 "not built: the four-digit channel designators, ASM 1 and ASM 2, and the digital-only entries of the table"};
    return p;
}

}  // namespace detail

inline const std::vector<ChannelPlan>& channelPlans() {
    static const std::vector<ChannelPlan> plans = [] {
        std::vector<ChannelPlan> v;
        v.push_back(detail::pmr("pmr446-analogue", "PMR446 analogue (16 channels)", 446.00625e6, 12500.0, 16));
        v.push_back(detail::pmr("pmr446-digital-12k5", "PMR446 digital 12.5 kHz (16 channels)", 446.00625e6, 12500.0, 16));
        v.push_back(detail::pmr("pmr446-digital-6k25", "PMR446 digital 6.25 kHz (32 channels)", 446.003125e6, 6250.0, 32));
        v.push_back(detail::cb());
        v.push_back(detail::frs());
        v.push_back(detail::gmrs());
        v.push_back(detail::murs());
        v.push_back(detail::marine());
        return v;
    }();
    return plans;
}

inline const ChannelPlan* findPlan(const std::string& id) {
    for (const ChannelPlan& p : channelPlans()) { if (p.id == id) { return &p; } }
    return nullptr;
}

// The channel mode: a position in one plan.
class ChannelCursor {
public:
    // false when there is no such plan; the cursor then keeps what it had.
    bool setPlan(const std::string& id) {
        const ChannelPlan* p = findPlan(id);
        if (p == nullptr || p->channels.empty()) { return false; }
        plan_ = p;
        idx_ = 0;
        return true;
    }
    const ChannelPlan* plan() const { return plan_; }
    bool active() const { return plan_ != nullptr; }
    const PlanChannel* current() const { return plan_ == nullptr ? nullptr : &plan_->channels[idx_]; }
    const PlanChannel* next() { return step(1); }
    const PlanChannel* prev() { return step(-1); }
    // By label ("16", "AIS 1", "G7"); false when the plan has no such channel.
    bool goTo(const std::string& label) {
        if (plan_ == nullptr) { return false; }
        for (std::size_t i = 0; i < plan_->channels.size(); ++i) {
            if (plan_->channels[i].label == label) { idx_ = i; return true; }
        }
        return false;
    }
    // The channel whose frequency (either of a marine channel's two) is nearest `hz` within tolHz, or nullptr. The cursor moves to it.
    const PlanChannel* nearest(double hz, double tolHz) {
        if (plan_ == nullptr || !std::isfinite(hz) || !std::isfinite(tolHz) || tolHz < 0.0) { return nullptr; }
        std::size_t best = plan_->channels.size();
        double bd = tolHz;
        for (std::size_t i = 0; i < plan_->channels.size(); ++i) {
            const PlanChannel& c = plan_->channels[i];
            double d = std::fabs(c.freqHz - hz);
            if (c.altHz > 0.0) { d = std::min(d, std::fabs(c.altHz - hz)); }
            if (d <= bd) { bd = d; best = i; }
        }
        if (best == plan_->channels.size()) { return nullptr; }
        idx_ = best;
        return &plan_->channels[best];
    }

private:
    const PlanChannel* step(int dir) {
        if (plan_ == nullptr) { return nullptr; }
        const std::size_t n = plan_->channels.size();
        idx_ = dir > 0 ? (idx_ + 1) % n : (idx_ + n - 1) % n;
        return &plan_->channels[idx_];
    }
    const ChannelPlan* plan_ = nullptr;
    std::size_t idx_ = 0;
};

// "446.00625 MHz" with the decimals the value needs (3 to 6).
inline std::string channelMhzText(double hz) {
    char buf[40];
    for (int d = 3; d <= 6; ++d) {
        std::snprintf(buf, sizeof buf, "%.*f", d, hz / 1e6);
        if (std::fabs(std::strtod(buf, nullptr) * 1e6 - hz) < 0.5) { break; }
    }
    return std::string(buf) + " MHz";
}

}  // namespace cascade::core::tuning
