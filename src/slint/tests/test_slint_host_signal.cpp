// The SIGNAL bank's pure logic (src/slint/signal_logic.hpp) against a STUB TABLE: the lists, facts and state a running engine would publish for
// an RTL-SDR that is open, the generator, a sound card, an Airspy, a converter in force, the filters, the sinks, the recorder, unbound radios of two
// families and a stopped device - and the exact commands each control sends, which are the ImGui window's own. No Slint, no engine.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "signal_logic.hpp"
#include "test_check.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace foxsdr::slintsignal;

namespace {

// A table of lists and facts a test fills in. Every list entry is a FoxListItem as read_list would fill it.
class StubTable : public TableReader {
public:
    std::map<std::uint32_t, std::vector<FoxListItem>> lists;
    std::map<std::string, std::string> facts;

    std::int32_t readList(std::uint32_t list, std::uint32_t index, FoxListItem* out) override {
        const auto it = lists.find(list);
        const std::int32_t n = it == lists.end() ? 0 : static_cast<std::int32_t>(it->second.size());
        if (out != nullptr && static_cast<std::int32_t>(index) < n) { *out = it->second[index]; }
        return n;
    }
    bool fact(const std::string& key, std::string& out) override {
        const auto it = facts.find(key);
        if (it == facts.end()) { out.clear(); return false; }
        out = it->second;
        return true;
    }
    void add(std::uint32_t list, std::uint64_t id, const char* name, const char* detail, std::uint32_t flags = 0, double v0 = 0, double v1 = 0, double v2 = 0, double v3 = 0) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        it.id = id;
        std::snprintf(it.name, sizeof(it.name), "%s", name);
        std::snprintf(it.detail, sizeof(it.detail), "%s", detail);
        it.flags = flags;
        it.value[0] = v0; it.value[1] = v1; it.value[2] = v2; it.value[3] = v3;
        lists[list].push_back(it);
    }
};

struct Rig {
    StubTable t;
    FoxReceiverState s{};
    ReadOptions o;
    Rig() { s.structSize = sizeof(s); }
};

void setText(char* dst, std::size_t cap, const char* text) { std::snprintf(dst, cap, "%s", text); }

template <typename T>
const T& at(const std::vector<T>& v, std::size_t i) {
    static const T none{};
    return i < v.size() ? v[i] : none;
}

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// The DEVICES list as the engine publishes it: the generator, the I/Q file and the sound card, a native RTL-SDR, the Pluto and rtl_tcp rows, a SoapySDR row.
void devices(Rig& r, int active) {
    r.t.add(FOXAPI_LIST_DEVICES, 1, "Signal generator", "siggen:", active == 0 ? FOXAPI_ITEM_ACTIVE : 0u);
    r.t.add(FOXAPI_LIST_DEVICES, 2, "IQ file", "file:", active == 1 ? FOXAPI_ITEM_ACTIVE : 0u);
    r.t.add(FOXAPI_LIST_DEVICES, 3, "Sound card", "soundcard:", active == 2 ? FOXAPI_ITEM_ACTIVE : 0u);
    r.t.add(FOXAPI_LIST_DEVICES, 4, "NESDR SMArt v5", "rtlsdr:index=0", active == 3 ? FOXAPI_ITEM_ACTIVE : 0u);
    r.t.add(FOXAPI_LIST_DEVICES, 5, "ADALM-Pluto", "pluto:uri=ip:192.168.2.1", active == 4 ? FOXAPI_ITEM_ACTIVE : 0u);
    r.t.add(FOXAPI_LIST_DEVICES, 6, "rtl_tcp server", "rtltcp:rtltcp=127.0.0.1", active == 5 ? FOXAPI_ITEM_ACTIVE : 0u);
    r.t.add(FOXAPI_LIST_DEVICES, 7, "RTL-SDR (SoapySDR)", "soapy:driver=rtlsdr,serial=00000001", active == 6 ? FOXAPI_ITEM_ACTIVE : 0u);
    r.t.facts["fact.source.row"] = std::to_string(active);
    r.t.facts["fact.source.id.4"] = "rtlsdr:index=0,serial=00000001";
    r.t.facts["fact.source.id.1"] = "siggen:";
    r.t.facts["fact.source.id.7"] = "soapy:driver=rtlsdr,serial=00000001";
}

// An RTL-SDR that is open: rates, one antenna, one TUNER gain, the bias tee present and on, the crystal correction on with a refusal.
Rig rtlOpen() {
    Rig r;
    devices(r, 3);
    r.s.flags = FOXAPI_RX_RUNNING | FOXAPI_RX_DEVICE_OPEN | FOXAPI_RX_AGC_SUPPORTED | FOXAPI_RX_BIAS_TEE_PRESENT | FOXAPI_RX_BIAS_TEE_ON;
    r.s.sampleRateHz = 2.4e6;
    r.s.tunedHz = 145.5e6;
    setText(r.s.deviceName, sizeof(r.s.deviceName), "NESDR SMArt v5");
    r.t.add(FOXAPI_LIST_SAMPLE_RATES, 0, "", "1.024 MS/s", 0, 1.024e6);
    r.t.add(FOXAPI_LIST_SAMPLE_RATES, 0, "", "2.400 MS/s", 0, 2.4e6);
    r.t.add(FOXAPI_LIST_SAMPLE_RATES, 0, "", "3.200 MS/s", 0, 3.2e6);
    r.t.add(FOXAPI_LIST_ANTENNAS, 0, "RX", "", FOXAPI_ITEM_ACTIVE);
    r.t.add(FOXAPI_LIST_GAINS, 0, "TUNER", "dB", 0, 0.0, 49.6, 0.1, 32.8);
    r.t.facts["fact.ppm.on"] = "1";
    r.t.facts["fact.ppm.method"] = "radio";
    r.t.facts["fact.ppm.value"] = "1.5";
    r.t.facts["fact.ppm.whole"] = "";
    r.t.facts["fact.ppm.error"] = "tuner refused";
    r.t.facts["fact.ppm.card"] = "1.5";
    r.t.facts["fact.ppm.model"] = "NESDR SMArt v5";
    r.t.facts["fact.ppm.serial"] = "00000001";
    r.t.facts["fact.ppm.key"] = "rtlsdr|serial=00000001";
    r.t.add(FOXAPI_LIST_CONVERTERS, 0, "", "rtlsdr|serial=00000001", FOXAPI_ITEM_ACTIVE, 0, 0, 0, 0);
    r.t.facts["fact.converter.modes"] = "0,1,2";
    r.t.facts["fact.converter.live"] = "0 0 0";
    r.t.facts["fact.biasTee.radioKey"] = "rtlsdr|serial=00000001";
    return r;
}

Rig generator() {
    Rig r;
    devices(r, 0);
    r.s.flags = FOXAPI_RX_RUNNING;
    r.s.sampleRateHz = 2.4e6;
    setText(r.s.deviceName, sizeof(r.s.deviceName), "Signal generator");
    r.t.facts["fact.ppm.on"] = "1";
    r.t.facts["fact.ppm.method"] = "na";
    r.t.facts["fact.converter.modes"] = "0,1,2";
    r.t.facts["fact.converter.live"] = "0 0 0";
    return r;
}

Rig soundCard() {
    Rig r;
    devices(r, 2);
    r.s.flags = FOXAPI_RX_RUNNING;
    r.s.sourceFlags = FOXAPI_SRC_CARD_LISTED;
    setText(r.s.deviceName, sizeof(r.s.deviceName), "Signal generator");
    r.t.add(FOXAPI_LIST_SOUND_CARDS, 1, "Line In (Realtek Audio) (Windows WASAPI)", "Windows WASAPI");
    r.t.add(FOXAPI_LIST_SOUND_CARDS, 2, "Stereo Mix (Realtek Audio) (Windows WASAPI)", "Windows WASAPI", FOXAPI_ITEM_ACTIVE);
    r.t.add(FOXAPI_LIST_SOUND_CARD_RATES, 0, "", "", 0, 44100);
    r.t.add(FOXAPI_LIST_SOUND_CARD_RATES, 0, "", "", FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_EXCLUSIVE, 192000);
    r.t.add(FOXAPI_LIST_SOUND_CARD_FORM, 0, "Stereo Mix (Realtek Audio)", "Windows WASAPI", FOXAPI_ITEM_IQ_STEREO | FOXAPI_ITEM_SWAP_IQ | FOXAPI_ITEM_PICKED, 192000, 7.1e6, 0);
    r.t.facts["fact.soundcard.device"] = "Stereo Mix (Realtek Audio)";
    r.t.facts["fact.soundcard.hostApi"] = "Windows WASAPI";
    r.t.facts["fact.soundcard.span"] = "7004000 7196000";
    r.t.facts["fact.status.soundCardMissing"] = "The saved sound card, USB Audio, is not connected.";
    r.t.facts["fact.ppm.on"] = "0";
    r.t.facts["fact.converter.modes"] = "";
    return r;
}

Rig airspy(bool free) {
    Rig r = rtlOpen();
    r.t.lists[FOXAPI_LIST_GAINS].clear();
    r.t.lists[FOXAPI_LIST_ANTENNAS].clear();
    r.s.flags = FOXAPI_RX_RUNNING | FOXAPI_RX_DEVICE_OPEN | FOXAPI_RX_BIAS_TEE_PRESENT;
    r.s.sampleRateHz = 2.5e6;
    r.t.add(FOXAPI_LIST_GAIN_MODES, 0, "Linearity", "", 0, 0);
    r.t.add(FOXAPI_LIST_GAIN_MODES, 0, "Sensitivity", "", free ? 0u : FOXAPI_ITEM_ACTIVE, 1);
    r.t.add(FOXAPI_LIST_GAIN_MODES, 0, "Free", "", free ? FOXAPI_ITEM_ACTIVE : 0u, 2);
    r.t.add(FOXAPI_LIST_DECIMATIONS, 0, "", "", 0, 1, 10e6);
    r.t.add(FOXAPI_LIST_DECIMATIONS, 0, "", "", 0, 2, 5e6);
    r.t.add(FOXAPI_LIST_DECIMATIONS, 0, "", "", FOXAPI_ITEM_ACTIVE, 4, 2.5e6);
    if (free) {
        r.t.add(FOXAPI_LIST_GAINS, 0, "LNA", "step", FOXAPI_ITEM_AGC, 0, 14, 1, 7);
        r.t.add(FOXAPI_LIST_GAINS, 0, "MIXER", "step", 0, 0, 15, 1, 3);
        r.t.add(FOXAPI_LIST_GAINS, 0, "VGA", "step", 0, 0, 15, 1, 15);
    } else {
        r.t.add(FOXAPI_LIST_GAINS, 0, "GAIN", "step", 0, 0, 21, 1, 7);
    }
    return r;
}

}  // namespace

int main() {
    // ---- an RTL-SDR that is open ----
    {
        Rig r = rtlOpen();
        r.o.patch_held.clear();
        const SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(m.device_open && m.device_index == 3 && m.devices.size() == 7);
        CHECK(m.device_label == "NESDR SMArt v5 (native)");
        CHECK(at(m.devices, 0).label == "Signal generator" && at(m.devices, 1).label == "IQ file" && at(m.devices, 2).label == "Sound card");
        CHECK(at(m.devices, 3).label == "NESDR SMArt v5 (native)" && at(m.devices, 3).id == "4");
        CHECK(at(m.devices, 6).label == "RTL-SDR (SoapySDR)");   // a SoapySDR row is its own label
        CHECK(m.refresh_enabled && !m.look_for_usrps && m.setup_key_label == "RADIO SETUP");
        CHECK(m.chip == "NESDR SMAr" && m.chip_tone == Tone::Good && m.lamp_lit && m.lamp_tone == Tone::Good);
        CHECK(m.rates.size() == 3 && m.rate_text == "2.400 MS/s" && at(m.rates, 1).label == "2.400 MS/s" && at(m.rates, 1).id == "2400000");
        CHECK(m.actual_rate == "actual 2.4 MS/s");
        CHECK(m.antennas.size() == 1 && m.single_port && m.antenna_text == "Antenna: RX");
        CHECK(m.auto_gain_shown && !m.auto_gain && m.agc_note.empty());
        CHECK(m.stages.size() == 1 && at(m.stages, 0).name == "TUNER" && at(m.stages, 0).text == "32.8 dB" && at(m.stages, 0).unit == "dB" && at(m.stages, 0).enabled);
        CHECK_NEAR(at(m.stages, 0).norm, 32.8 / 49.6, 1e-5);
        CHECK(m.bias_shown && m.bias_on && m.bias_enabled);
        CHECK(m.ppm_on && m.ppm_shown_value && m.ppm_radio_line == "For NESDR SMArt v5, serial 00000001" && m.ppm_text == "+1.5 ppm");
        CHECK(m.ppm_note == "Corrected in the radio itself." && m.ppm_refusal == "The radio refused the correction: tuner refused");
        CHECK(m.ppm_chip == "PPM +1.5" && m.ppm_stored == "+1.5");
        CHECK(m.converter_shown && m.converter_mode == 0 && m.converter_modes.size() == 3 && m.converter_on_air.empty() && m.converter_alias.empty());
        CHECK(m.variant == 0 && m.options.empty());
        CHECK(m.advisory.empty() && m.error_line.empty() && m.stopped_line.empty() && m.mismatch_note.empty());
        // the ImGui tooltips of the controls ride along, so the UI does not word them again
        CHECK(has(m.usrps_tooltip, "Ask UHD for USRPs on the network") && has(m.bias_tooltip, "Sends about 4.5 V up the antenna cable") && has(m.ppm_tooltip, "Corrects the error of this radio's crystal"));
        CHECK(has(m.converter_tooltip, "An up- or down-converter between the antenna and this radio") && has(m.converter_lo_tooltip, "A number with no unit is MHz.") && has(m.converter_inverts_tooltip, "high-side LO"));
        CHECK(!m.sdrplay_key_shown && m.sdrplay_key_label.empty());
        // the SDRplay service key: shown on the publisher's say-so, dead while its helper runs, with the outcome under it
        r.o.sdrplay_key_shown = true;
        r.o.sdrplay_note = "Restart cancelled.";
        SourceModel key = readSource(r.t, r.s, r.o);
        CHECK(key.sdrplay_key_shown && key.sdrplay_key_enabled && key.sdrplay_key_label == "RESTART SDRPLAY SERVICE" && key.sdrplay_line == "Restart cancelled.");
        r.o.sdrplay_restarting = true;
        key = readSource(r.t, r.s, r.o);
        CHECK(!key.sdrplay_key_enabled && key.sdrplay_line == "Restarting the SDRplay API Service - answer the Windows prompt.");
        r.o.sdrplay_key_shown = false;
        r.o.sdrplay_restarting = false;
        r.o.sdrplay_note.clear();
        // the same radio with Auto gain on greys its sliders, and a second antenna turns the readout into a dropdown
        r.s.flags |= FOXAPI_RX_DEVICE_AGC;
        r.t.add(FOXAPI_LIST_ANTENNAS, 0, "TX/RX", "");
        const SourceModel m2 = readSource(r.t, r.s, r.o);
        CHECK(m2.auto_gain && !at(m2.stages, 0).enabled && !m2.single_port && m2.antenna_text == "RX" && m2.antennas.size() == 2 && at(m2.antennas, 1).id == "TX/RX");
        // a radio with no AGC says so
        Rig noAgc = rtlOpen();
        noAgc.s.flags &= ~FOXAPI_RX_AGC_SUPPORTED;
        const SourceModel m3 = readSource(noAgc.t, noAgc.s, noAgc.o);
        CHECK(!m3.auto_gain_shown && m3.agc_note == "Auto gain: not supported");
        // a rate the driver landed on is the row nearest the state's
        Rig coerced = rtlOpen();
        coerced.s.sampleRateHz = 3.1e6;
        CHECK(readSource(coerced.t, coerced.s, coerced.o).rate_text == "3.200 MS/s");
        // the bias tee is not offered by a radio without one, and not while a radio is opening
        Rig noBias = rtlOpen();
        noBias.s.flags &= ~(FOXAPI_RX_BIAS_TEE_PRESENT | FOXAPI_RX_BIAS_TEE_ON);
        CHECK(!readSource(noBias.t, noBias.s, noBias.o).bias_shown);
        Rig opening = rtlOpen();
        opening.s.flags |= FOXAPI_RX_DEVICE_OPENING;
        opening.t.facts["fact.source.opening"] = "NESDR SMArt v5";
        const SourceModel mo = readSource(opening.t, opening.s, opening.o);
        CHECK(!mo.bias_enabled && mo.opening && !mo.refresh_enabled && mo.busy_line == "Opening NESDR SMArt v5..." && has(mo.advisory, "Opening NESDR SMArt v5..."));
        // a network-USRP tick, a status line, a tune mismatch
        Rig status = rtlOpen();
        status.s.sourceFlags = FOXAPI_SRC_NETWORK_USRPS;
        status.t.facts["fact.status.sourceError"] = "Could not set the gain";
        status.t.facts["fact.status.tuneMismatchNote"] = "The radio would not tune to 6 GHz.";
        const SourceModel ms = readSource(status.t, status.s, status.o);
        CHECK(ms.look_for_usrps && ms.error_line == "Could not set the gain" && ms.mismatch_note == "The radio would not tune to 6 GHz.");
        Rig scanning = rtlOpen();
        scanning.s.sourceFlags = FOXAPI_SRC_SCAN_PENDING;
        const SourceModel msc = readSource(scanning.t, scanning.s, scanning.o);
        CHECK(!msc.refresh_enabled && msc.busy_line == "Scanning for devices...");
    }

    // ---- the signal generator: no stages, no bias, nothing to correct ----
    {
        Rig r = generator();
        const SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(!m.device_open && m.device_index == 0 && m.device_label == "Signal generator");
        CHECK(m.chip == "Signal gen" && m.chip_tone == Tone::Neutral && m.lamp_lit);
        CHECK(m.rates.empty() && m.rate_text.empty() && m.stages.empty() && m.antennas.empty() && !m.auto_gain_shown);
        CHECK(!m.bias_shown && m.options.empty() && m.variant == 0);
        CHECK(m.ppm_on && !m.ppm_shown_value && m.ppm_note == "Not used for the signal generator, a sound card or an I/Q file - none has a crystal of its own to correct.");
        CHECK(m.ppm_radio_line.empty() && m.ppm_text.empty() && m.ppm_chip == "ON");
        // the saved radio did not open: the combo says so, the lamp goes red, the section says what it means for next time
        Rig kept = generator();
        kept.t.facts["fact.source.keep"] = "rtlsdr";
        kept.t.facts["fact.status.restoreKeepLabel"] = "NESDR SMArt v5";
        const SourceModel mk = readSource(kept.t, kept.s, kept.o);
        CHECK(mk.device_label == "NESDR SMArt v5 (saved, not open)" && mk.lamp_lit && mk.lamp_tone == Tone::Err);
        CHECK(has(mk.advisory, "NESDR SMArt v5 is still the saved radio. The signal generator is running in its place") && mk.advisory_tone == Tone::Warn);
        kept.t.facts["fact.source.keep"] = "file";
        const SourceModel mf = readSource(kept.t, kept.s, kept.o);
        CHECK(has(mf.advisory, "is still the saved I/Q file.") && mf.lamp_tone == Tone::Good);   // a file is said in words, not by the lamp
        kept.t.facts["fact.source.keep"] = "soundcard";
        CHECK(has(readSource(kept.t, kept.s, kept.o).advisory, "is still the saved sound card."));
        // a patch holds a radio: the chip and the combo say so
        Rig patch = generator();
        patch.s.flags |= FOXAPI_RX_PATCH_RUNNING;
        patch.o.patch_held = {"NESDR SMArt v5"};
        const SourceModel mp = readSource(patch.t, patch.s, patch.o);
        CHECK(mp.chip == "ON PATCH" && mp.device_label == "Signal generator - the patch has NESDR SMArt v5");
        CHECK(at(mp.devices, 0).enabled && at(mp.devices, 1).enabled && at(mp.devices, 2).enabled);
        CHECK(!at(mp.devices, 3).enabled && !at(mp.devices, 6).enabled && has(at(mp.devices, 3).tooltip, "The patch is running and has the radios"));
    }

    // ---- an I/Q file, the Pluto and rtl_tcp: the variant by the combo's row, and the boxes ----
    {
        Rig r = generator();
        r.t.lists[FOXAPI_LIST_DEVICES].clear();
        devices(r, 1);
        r.o.iq_path = "C:\\iq\\capture.wav";
        r.t.facts["fact.source.iqOpening"] = "1";
        r.o.iq_open_wait_s = 2.4;
        SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(m.variant == 1 && m.iq_path == "C:\\iq\\capture.wav" && m.iq_note == "Waiting for the disk to open the file: 2 s");
        r.o.iq_open_wait_s = 0.4;
        CHECK(readSource(r.t, r.s, r.o).iq_note.empty());
        r.o.iq_open_wait_s = 3.0;
        r.t.facts["fact.source.iqOpening"] = "0";
        CHECK(readSource(r.t, r.s, r.o).iq_note.empty());   // the header is not being read any more

        Rig p = generator();
        p.t.lists[FOXAPI_LIST_DEVICES].clear();
        devices(p, 4);
        p.t.facts["fact.source.plutoUri"] = "ip:192.168.2.1";
        const SourceModel mp = readSource(p.t, p.s, p.o);
        CHECK(mp.variant == 2 && mp.pluto_address == "ip:192.168.2.1" && has(mp.pluto_help, "A Pluto on its USB cable answers at ip:192.168.2.1."));
        CHECK(has(mp.pluto_help, "Nothing is contacted until you press Open."));

        Rig t = generator();
        t.t.lists[FOXAPI_LIST_DEVICES].clear();
        devices(t, 5);
        t.t.facts["fact.source.rtlTcpAddr"] = "192.168.0.9:1234";
        const SourceModel mt = readSource(t.t, t.s, t.o);
        CHECK(mt.variant == 3 && mt.rtltcp_address == "192.168.0.9:1234" && has(mt.rtltcp_help, "The address of a running rtl_tcp server, as host or host:port"));
    }

    // ---- a sound card: variant 4 with the form ----
    {
        Rig r = soundCard();
        const SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(m.variant == 4 && m.device_index == 2 && m.chip == "Signal gen");
        CHECK(m.cards.size() == 2 && m.card_index == 1 && at(m.cards, 1).id == "2" && at(m.cards, 0).label == "Line In (Realtek Audio) (Windows WASAPI)");
        CHECK(m.card_text == "Stereo Mix (Realtek Audio) (Windows WASAPI)" && m.card_can_open && m.card_enabled);
        CHECK(m.card_format_index == 1 && m.card_channel_index == 0 && m.card_swap_iq && m.card_centre_mhz == "7.100000");
        CHECK(m.card_rates.size() == 2 && at(m.card_rates, 0).label == "44.1 kHz" && at(m.card_rates, 1).label == "192 kHz (exclusive)" && at(m.card_rates, 1).id == "192000");
        CHECK(m.card_rate_index == 1 && has(at(m.card_rates, 1).tooltip, "Exclusive mode: FoxSDR has the card to itself"));
        CHECK(m.card_receives == "Receives 7.0040 MHz to 7.1960 MHz." && m.card_warning == "The saved sound card, USB Audio, is not connected.");
        CHECK(m.card_busy.empty() && m.card_none.empty());
        CHECK(m.converter_shown == false);   // an I/Q card takes no converter: the engine offers no modes
        // real mono on the right channel: the channel dropdown, not the swap and the centre
        Rig mono = soundCard();
        mono.t.lists[FOXAPI_LIST_SOUND_CARD_FORM].clear();
        mono.t.add(FOXAPI_LIST_SOUND_CARD_FORM, 0, "Stereo Mix (Realtek Audio)", "Windows WASAPI", FOXAPI_ITEM_PICKED, 44100, 0, 1);
        mono.t.facts["fact.converter.modes"] = "0,2";
        mono.t.facts["fact.converter.upIgnored"] = "1";
        const SourceModel mm = readSource(mono.t, mono.s, mono.o);
        CHECK(mm.card_format_index == 0 && mm.card_channel_index == 1 && !mm.card_swap_iq && mm.converter_shown && mm.converter_modes.size() == 2 && at(mm.converter_modes, 1).id == "2");
        CHECK(has(mm.converter_alias, "A sound card takes a down-converter only: the up-converter set for this card is not used."));
        // busy and empty
        Rig busy = soundCard();
        busy.s.sourceFlags = FOXAPI_SRC_CARD_LISTED | FOXAPI_SRC_CARD_OPENING;
        const SourceModel mb = readSource(busy.t, busy.s, busy.o);
        CHECK(mb.card_busy == "Opening Stereo Mix (Realtek Audio) (Windows WASAPI)..." && !mb.card_enabled);
        busy.s.sourceFlags = FOXAPI_SRC_CARD_SCANNING;
        CHECK(readSource(busy.t, busy.s, busy.o).card_busy == "Scanning for devices...");
        Rig none = soundCard();
        none.t.lists[FOXAPI_LIST_SOUND_CARDS].clear();
        none.t.lists[FOXAPI_LIST_SOUND_CARD_FORM].clear();
        none.t.facts["fact.soundcard.device"] = "";
        const SourceModel mn = readSource(none.t, none.s, none.o);
        CHECK(mn.card_none == "No sound card inputs found." && !mn.card_can_open && mn.card_index == -1 && mn.card_text == "-");
    }

    // ---- an Airspy: variant 5 ----
    {
        Rig r = airspy(true);
        const SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(m.variant == 5 && m.stages.empty() && !m.auto_gain_shown && m.agc_note.empty());
        CHECK(m.airspy_decimations.size() == 3 && at(m.airspy_decimations, 0).label == "None" && at(m.airspy_decimations, 2).label == "4" && at(m.airspy_decimations, 2).id == "4");
        CHECK(m.airspy_decimation_index == 2 && m.airspy_hardware_rate == "radio at 10 MS/s, decimated by 4");
        CHECK(m.airspy_gain_mode == 2 && m.airspy_lna_agc && !m.airspy_mixer_agc);
        CHECK(m.airspy_stages.size() == 3 && at(m.airspy_stages, 0).name == "LNA" && !at(m.airspy_stages, 0).enabled && at(m.airspy_stages, 1).enabled && at(m.airspy_stages, 0).unit == "step");
        CHECK_NEAR(m.airspy_lna, 0.5, 1e-6);
        CHECK_NEAR(m.airspy_mixer, 3.0 / 15.0, 1e-6);
        CHECK_NEAR(m.airspy_vga, 1.0, 1e-6);
        CHECK(at(m.airspy_stages, 2).text == "15");
        Rig s = airspy(false);
        const SourceModel ms = readSource(s.t, s.s, s.o);
        CHECK(ms.variant == 5 && ms.airspy_gain_mode == 0 && !ms.airspy_lna_agc && !ms.airspy_mixer_agc);
        CHECK_NEAR(ms.airspy_gain_norm, 7.0 / 21.0, 1e-6);
        Rig plain = airspy(false);
        plain.t.lists[FOXAPI_LIST_DECIMATIONS].clear();
        plain.t.add(FOXAPI_LIST_DECIMATIONS, 0, "", "", FOXAPI_ITEM_ACTIVE, 1, 10e6);
        plain.t.add(FOXAPI_LIST_DECIMATIONS, 0, "", "", 0, 2, 5e6);
        const SourceModel mpl = readSource(plain.t, plain.s, plain.o);
        CHECK(mpl.airspy_decimation_index == 0 && mpl.airspy_hardware_rate.empty());
    }

    // ---- the radio's own switches: variant 6 ----
    {
        Rig r = rtlOpen();
        r.t.add(FOXAPI_LIST_DEVICE_OPTIONS, 0, "rf_notch", "", FOXAPI_ITEM_ACTIVE);
        r.t.add(FOXAPI_LIST_DEVICE_OPTIONS, 0, "dab_notch", "", 0);
        r.t.add(FOXAPI_LIST_DEVICE_OPTIONS, 0, "hdr", "", 0);
        const SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(m.variant == 6 && m.options.size() == 3);
        CHECK(at(m.options, 0).name == "rf_notch" && at(m.options, 0).label == "FM notch" && at(m.options, 0).on && at(m.options, 0).enabled);
        CHECK(at(m.options, 1).name == "dab_notch" && at(m.options, 1).label == "DAB notch" && !at(m.options, 1).on);
        CHECK(at(m.options, 2).name == "hdr" && at(m.options, 2).label == "HDR mode");
        CHECK(has(at(m.options, 0).tooltip, "broadcast-FM notch filter") && has(at(m.options, 2).tooltip, "high-dynamic-range path below 2 MHz"));
        Rig rx = rtlOpen();
        rx.t.add(FOXAPI_LIST_DEVICE_OPTIONS, 0, "dither", "", FOXAPI_ITEM_ACTIVE);
        const SourceModel mx = readSource(rx.t, rx.s, rx.o);
        CHECK(mx.options.size() == 2 && at(mx.options, 0).name == "dither" && at(mx.options, 0).label == "ADC dither" && at(mx.options, 0).on);
        CHECK(at(mx.options, 1).name == "randomiser" && at(mx.options, 1).label == "ADC randomiser" && !at(mx.options, 1).on);   // either one shows the pair
    }

    // ---- a converter in force: variant 7 ----
    {
        Rig r = rtlOpen();
        r.t.lists[FOXAPI_LIST_CONVERTERS].clear();
        r.t.add(FOXAPI_LIST_CONVERTERS, 0, "", "rtlsdr|serial=00000001", FOXAPI_ITEM_ACTIVE, 1, 125e6, 0, 125.0172e6);
        r.t.facts["fact.converter.live"] = "1 125000000 0";
        r.t.facts["fact.converter.alias"] = "notCarried";
        r.s.tunedHz = 17200;
        const SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(m.variant == 7 && m.converter_shown && m.converter_mode == 1 && m.converter_lo == "125 MHz" && !m.converter_inverts);
        CHECK(m.converter_on_air == "Listening on 17.2 kHz on the air - 125.0172 MHz at the radio.");
        CHECK(has(m.converter_alias, "Opened through SoapySDR because the native driver refused this radio. The converter set for it was not carried over"));
        CHECK(m.converter_modes.size() == 3 && at(m.converter_modes, 1).id == "1" && at(m.converter_modes, 1).label == "Up-converter" && at(m.converter_modes, 2).label == "Down-converter" && at(m.converter_modes, 0).label == "Off");
        r.t.facts["fact.converter.alias"] = "carried";
        CHECK(has(readSource(r.t, r.s, r.o).converter_alias, "the converter set for it still applies."));
        // a stored mode this source cannot take reads as Off; a bad LO text is the error line
        Rig down = rtlOpen();
        down.t.lists[FOXAPI_LIST_CONVERTERS].clear();
        down.t.add(FOXAPI_LIST_CONVERTERS, 0, "", "k", FOXAPI_ITEM_ACTIVE, 1, 125e6, 1, 0);
        down.t.facts["fact.converter.modes"] = "0,2";
        down.o.converter_lo_bad = "12 furlongs";
        const SourceModel md = readSource(down.t, down.s, down.o);
        CHECK(md.converter_mode == 0 && md.converter_inverts && md.converter_error.empty());   // the LO box is not drawn with the mode Off, nor its error
        CHECK(md.converter_lo == "125 MHz" && md.variant == 0);   // the stored LO is kept for the radio that can use it
        r.o.converter_lo_bad = "12 furlongs";
        CHECK(readSource(r.t, r.s, r.o).converter_error == "could not read frequency \"12 furlongs\"");   // with the mode on, it is the error line
    }

    // ---- unbound radios of two families, the SDRplay advice, no hardware ----
    {
        Rig r = generator();
        r.t.add(FOXAPI_LIST_UNBOUND_RADIOS, 0, "Realtek RTL2838UHIDIR", "rtl", 0, 0x0bda, 0x2838);
        r.t.add(FOXAPI_LIST_UNBOUND_RADIOS, 0, "", "rx888", 0, 0x04b4, 0x00f3);
        r.t.add(FOXAPI_LIST_UNBOUND_RADIOS, 0, "Mystery Radio", "", 0, 0x1234, 0x5678);
        r.t.facts["fact.status.sdrPlayAdvice"] = "SDRplay API 3.x is not installed. Install it from sdrplay.com.";
        r.t.facts["fact.status.sdrPlayApiDetail"] = "looked in C:\\Program Files\\SDRplay";
        const SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(has(m.advisory, "Realtek RTL2838UHIDIR is plugged in but is not bound to WinUSB, so nothing can open it. Run Zadig (zadig.akeo.ie), tick Options -> List All Devices, select \"Bulk-In, Interface (Interface 0)\", choose WinUSB and click Replace Driver, then press Refresh."));
        CHECK(has(m.advisory, "A USB radio is plugged in but is not bound to WinUSB, so nothing can open it. An RX888 has to be done TWICE"));
        CHECK(has(m.advisory, "Mystery Radio is plugged in but is not bound to WinUSB, so nothing can open it. Run Zadig (zadig.akeo.ie), tick Options -> List All Devices, select this radio in the dropdown"));
        CHECK(has(m.advisory, "SDRplay API 3.x is not installed.") && has(m.advisory, "SDRplay API: looked in C:\\Program Files\\SDRplay") && m.advisory_tone == Tone::Warn);
        // an RSP row in the list silences the SDRplay advice
        r.s.sourceFlags = FOXAPI_SRC_SDRPLAY_ROWS;
        CHECK(!has(readSource(r.t, r.s, r.o).advisory, "SDRplay API 3.x"));
        // the setup page lists the same radios with their ids and their advice
        r.o.setup_shown = true;
        const RadioSetupModel rs = readRadioSetup(r.t, r.s, r.o);
        CHECK(rs.shown && rs.unbound.size() == 3 && at(rs.unbound, 0).name == "Realtek RTL2838UHIDIR" && at(rs.unbound, 0).family == "rtl" && at(rs.unbound, 0).vid == "0BDA" && at(rs.unbound, 0).pid == "2838");
        CHECK(at(rs.unbound, 1).name == "A USB radio" && at(rs.unbound, 1).family == "rx888" && at(rs.unbound, 1).vid == "04B4" && at(rs.unbound, 1).pid == "00F3" && has(at(rs.unbound, 1).advice, "RX888 has to be done TWICE"));
        CHECK(has(at(rs.unbound, 0).advice, "Bulk-In, Interface (Interface 0)") && !has(at(rs.unbound, 2).advice, "Bulk-In"));
        // a complete scan that found nothing: the block, the RTL-SDR paragraph and the evidence
        Rig n = generator();
        n.t.lists[FOXAPI_LIST_DEVICES].resize(3);   // the generator, the file and the card, and the two rows that ask for an address
        n.t.add(FOXAPI_LIST_DEVICES, 5, "ADALM-Pluto", "pluto:uri=ip:192.168.2.1");
        n.t.add(FOXAPI_LIST_DEVICES, 6, "rtl_tcp server", "rtltcp:rtltcp=127.0.0.1");
        n.s.sourceFlags = FOXAPI_SRC_SCANNED;
        const SourceModel mn = readSource(n.t, n.s, n.o);
        CHECK(has(mn.advisory, "No radio hardware found") && has(mn.advisory, "FoxSDR reaches radios through SoapySDR vendor modules") && has(mn.advisory, "No vendor modules are loaded. Install PothosSDR"));
        CHECK(has(mn.advisory, "RTL-SDR dongle? Windows also needs a WinUSB driver bound to it."));
        n.t.add(FOXAPI_LIST_SCAN_EVIDENCE, 0, "PothosSDR", "C:\\Program Files\\PothosSDR", 0, 1);
        n.t.add(FOXAPI_LIST_SCAN_EVIDENCE, 0, "", "C:\\Program Files\\PothosSDR\\lib\\SoapySDR\\modules0.8", 0, 2);
        n.t.add(FOXAPI_LIST_SCAN_EVIDENCE, 0, "", "rtlsdrSupport.dll", 0, 3);
        const SourceModel mn2 = readSource(n.t, n.s, n.o);
        CHECK(has(mn2.advisory, "1 vendor module is loaded, but none reported a device.") && !has(mn2.advisory, "No vendor modules are loaded"));
        n.o.setup_shown = true;
        const RadioSetupModel ns = readRadioSetup(n.t, n.s, n.o);
        std::string joined;
        for (const std::string& l : ns.lines) { joined += l + "\n"; }
        CHECK(has(joined, "found PothosSDR at C:\\Program Files\\PothosSDR") && has(joined, "search: C:\\Program Files\\PothosSDR\\lib\\SoapySDR\\modules0.8") && has(joined, "module: rtlsdrSupport.dll"));
        // a radio in either list, or a scan that is partial or busy, shows no block
        CHECK(has(readSource(n.t, n.s, n.o).advisory, "No radio hardware found"));   // the Pluto and rtl_tcp rows are not radios that were found
        Rig nat = n;
        nat.t.add(FOXAPI_LIST_DEVICES, 9, "NESDR SMArt v5", "rtlsdr:index=0");
        CHECK(!has(readSource(nat.t, nat.s, nat.o).advisory, "No radio hardware found"));
        Rig soapy = n;
        soapy.t.add(FOXAPI_LIST_DEVICES, 9, "RTL-SDR (SoapySDR)", "soapy:driver=rtlsdr");
        CHECK(!has(readSource(soapy.t, soapy.s, soapy.o).advisory, "No radio hardware found"));
        CHECK(noRadioHardwareFound(true, false, false, 0, 0) && !noRadioHardwareFound(false, false, false, 0, 0) && !noRadioHardwareFound(true, true, false, 0, 0));
        CHECK(!noRadioHardwareFound(true, false, true, 0, 0) && !noRadioHardwareFound(true, false, false, 1, 0) && !noRadioHardwareFound(true, false, false, 0, 1));
        // the scan notes: UHD left out, UHD without images, the drivers that crashed, the deferral beside an open radio
        Rig hints = rtlOpen();
        hints.s.sourceFlags = FOXAPI_SRC_UHD_LEFT_OUT | FOXAPI_SRC_SCAN_GATED;
        hints.t.facts["fact.source.uhdNoImages"] = "1";
        hints.t.facts["fact.source.usrpOnUsb"] = "1";
        hints.t.facts["fact.source.faultedDrivers"] = "uhd, airspy";
        hints.t.facts["fact.source.scanGate"] = "NESDR SMArt v5";
        hints.t.facts["fact.source.scanSkip"] = "";
        const SourceModel mh = readSource(hints.t, hints.s, hints.o);
        CHECK(has(mh.advisory, "Network USRPs are not searched - tick Look for network USRPs to include them"));
        CHECK(has(mh.advisory, "A USRP is plugged in but UHD has no firmware images"));
        CHECK(has(mh.advisory, "Not scanned again this session: uhd, airspy (it crashed the device scan)"));
        CHECK(has(mh.advisory, "Refreshed the native radios. The SoapySDR scan is deferred while NESDR SMArt v5 is open"));
        hints.t.facts["fact.source.scanSkip"] = "uhd";
        CHECK(has(readSource(hints.t, hints.s, hints.o).advisory, "While NESDR SMArt v5 is open, Refresh looks for every other kind of radio but leaves out the uhd driver"));
        hints.s.sourceFlags |= FOXAPI_SRC_NETWORK_USRPS;
        CHECK(!has(readSource(hints.t, hints.s, hints.o).advisory, "Network USRPs are not searched"));   // not while the box is ticked
    }

    // ---- a stopped device ----
    {
        Rig r = rtlOpen();
        r.s.flags = (r.s.flags & ~FOXAPI_RX_RUNNING) | FOXAPI_RX_FAULTED;
        r.t.facts["fact.status.deviceFault"] = "The radio was unplugged.";
        const SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(m.stopped_line == "Device stopped: The radio was unplugged.\nReconnect it and pick the source again, or switch to the signal generator.");
        CHECK(m.lamp_lit && m.lamp_tone == Tone::Err);
        r.t.facts["fact.status.deviceFault"] = "The SDRplay session was lost, then restart FoxSDR.";
        CHECK(readSource(r.t, r.s, r.o).stopped_line == "Device stopped: The SDRplay session was lost, then restart FoxSDR.");   // the driver has said what to do
        Rig ok = rtlOpen();
        CHECK(readSource(ok.t, ok.s, ok.o).stopped_line.empty());
    }

    // ---- the chip and the variant helpers ----
    {
        CHECK(sourceChipText("Signal generator") == "Signal gen" && sourceChipText("SoapySDR: B200") == "B200" && sourceChipText("IQ file") == "IQ file");
        CHECK(sourceChipText("") .empty() && sourceChipText("RTL-SDR Blog V4 Dongle") == "RTL-SDR Bl");
        CHECK(sourceChipText("\xC3\x9C" "n\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 R\xC3\xA4" "dio XYZ") == "\xC3\x9C" "n\xC3\xAF" "c\xC3\xB6" "d\xC3\xA9 R\xC3\xA4");   // ten characters, never a half letter
        CHECK(sourceChip("Signal generator", {}) == "Signal gen" && sourceChip("Signal generator", {"A"}) == "ON PATCH");
        CHECK(sourceRowText("Signal generator", {}) == "Signal generator" && sourceRowText("Signal generator", {"A", "B"}) == "Signal generator - the patch has A, B");
        CHECK(radioNotOpenLamp("rtlsdr", false, true, false) && !radioNotOpenLamp("", false, true, false) && !radioNotOpenLamp("file", false, true, false));
        CHECK(!radioNotOpenLamp("rtlsdr", true, true, false) && !radioNotOpenLamp("rtlsdr", false, false, false) && !radioNotOpenLamp("soundcard", false, true, true));
        CHECK(sourceVariant(1, false, false, false, false, false, false) == kVariantIqFile && sourceVariant(3, true, false, true, false, false, false) == kVariantPluto);
        CHECK(sourceVariant(3, false, true, true, false, false, false) == kVariantRtlTcp && sourceVariant(2, false, false, false, false, false, false) == kVariantSoundCard);
        CHECK(sourceVariant(3, false, false, true, true, true, true) == kVariantAirspy && sourceVariant(3, false, false, true, false, true, true) == kVariantOptions);
        CHECK(sourceVariant(3, false, false, true, false, false, true) == kVariantConverter && sourceVariant(3, false, false, true, false, false, false) == kVariantNone && sourceVariant(0, false, false, false, false, false, false) == kVariantNone);
        CHECK(sourceVariant(0, false, false, false, false, false, true) == kVariantConverter);   // the generator with a converter set shows it
    }

    // ---- gains ----
    {
        CHECK_NEAR(gainNorm(0, 49.6, 32.8), 0.6613, 1e-4);
        CHECK_NEAR(gainNorm(-4.7, 16.1, -4.7), 0.0, 1e-9);
        CHECK_NEAR(gainNorm(0, 0, 30), 0.5, 1e-9);   // a driver that reports no range: the fallback 0..60 dB
        CHECK_NEAR(gainNorm(0, 10, 99), 1.0, 1e-9);
        CHECK_NEAR(gainFromNorm(0, 49.6, 0.5f, false), 24.8, 1e-5);
        CHECK_NEAR(gainFromNorm(0, 0, 0.5f, false), 30.0, 1e-5);
        CHECK_NEAR(gainFromNorm(0, 21, 0.34f, true), 7.0, 1e-9);   // a step stage is a whole step
        CHECK(gainText(32.84, false) == "32.8 dB" && gainText(7.0, true) == "7" && gainText(-4.66, false) == "-4.7 dB");
    }

    // ---- the Airspy words ----
    {
        CHECK(airspyGainModeCode(0) == 1 && airspyGainModeCode(1) == 0 && airspyGainModeCode(2) == 2);   // Sensitive = Sensitivity 1, Linear = Linearity 0
        CHECK(airspyGainModeIndex(1) == 0 && airspyGainModeIndex(0) == 1 && airspyGainModeIndex(2) == 2 && airspyGainModeIndex(9) == 0);
        CHECK(airspyModeWord(0) == "Sensitive" && airspyModeWord(1) == "Linear" && airspyModeWord(2) == "Free");
        CHECK(has(airspyModeTooltip(0), "the most gain early in the chain, for weak signals") && has(airspyModeTooltip(1), "gain held back early in the chain") && has(airspyModeTooltip(2), "LNA, mixer and VGA yourself"));
        CHECK(airspyDecimationLabel(1) == "None" && airspyDecimationLabel(8) == "8");
        CHECK(airspyHardwareLine(10e6, 4) == "radio at 10 MS/s, decimated by 4" && airspyHardwareLine(10e6, 1).empty() && airspyHardwareLine(6e6, 2) == "radio at 6 MS/s, decimated by 2");
    }

    // ---- the sound card words ----
    {
        CHECK(soundCardHzText(192000) == "192 kHz" && soundCardHzText(44100) == "44.1 kHz" && soundCardHzText(7.05e6) == "7.0500 MHz");
        CHECK(soundCardRateLabel(48000, false) == "48 kHz" && soundCardRateLabel(48000, true) == "48 kHz (exclusive)");
        CHECK(soundCardReceivesLine(7.0e6, 7.048e6) == "Receives 7.0000 MHz to 7.0480 MHz.");
    }

    // ---- the converter words ----
    {
        CHECK(converterAliasNote("").empty() && converterAliasNote("zzz").empty() && !converterAliasNote("notCarried").empty() && !converterAliasNote("carried").empty());
        CHECK(converterUnusableNote(false).empty() && !converterUnusableNote(true).empty());
        CHECK(converterOnAirLine(17200, 125017200) == "Listening on 17.2 kHz on the air - 125.0172 MHz at the radio.");
        CHECK(converterLoError("abc") == "could not read frequency \"abc\"");
        CHECK(kConverterQuickLoHz[0] == 125e6 && kConverterQuickLoHz[1] == 100e6 && kConverterQuickLoHz[2] == 2e6);
    }

    // ---- Zadig advice by family ----
    {
        CHECK(has(zadigAdvice("", "rtl"), "A USB radio is plugged in") && has(zadigAdvice("X", "rtl"), "X is plugged in") && has(zadigAdvice("X", "rx888"), "(04B4:00F1)"));
        CHECK(has(zadigAdvice("X", "other"), "select this radio in the dropdown") && has(zadigAdvice("X", ""), "select this radio in the dropdown"));
    }

    // ---- frequency correction ----
    {
        CHECK(ppmValueText(1.5) == "+1.5 ppm" && ppmValueText(-12.0) == "-12.0 ppm" && ppmValueText(0.0) == "+0.0 ppm" && ppmValueText(500.0) == "+200.0 ppm");
        double v = 0;
        CHECK(parsePpm("+2.5", v) && v == 2.5 && parsePpm("-3 ppm", v) && v == -3.0 && parsePpm(" 0.04 ", v) && v == 0.0);
        CHECK(!parsePpm("abc", v) && !parsePpm("", v) && parsePpm("999", v) && v == 200.0);   // clamped, as sanitisePpm
        CHECK(ppmHowText("radio", true, 2) == "Corrected in the radio itself, in whole ppm: +2." && ppmHowText("radio", false, 0) == "Corrected in the radio itself.");
        CHECK(ppmHowText("retune", false, 0) == "Corrected by retuning - the centre frequency only; the sample rate keeps its error.");
        CHECK(ppmChip(false, "1.5") == "OFF" && ppmChip(true, "") == "ON" && ppmChip(true, "1.5") == "PPM +1.5" && ppmChip(true, "-0.26") == "PPM -0.3");
        // a whole-ppm radio shows its integer; a retune radio says so
        Rig r = rtlOpen();
        r.t.facts["fact.ppm.whole"] = "2";
        CHECK(readSource(r.t, r.s, r.o).ppm_note == "Corrected in the radio itself, in whole ppm: +2.");
        r.t.facts["fact.ppm.method"] = "retune";
        r.t.facts["fact.ppm.serial"] = "";
        const SourceModel m = readSource(r.t, r.s, r.o);
        CHECK(has(m.ppm_note, "Corrected by retuning") && m.ppm_radio_line == "For NESDR SMArt v5");
        r.t.facts["fact.ppm.on"] = "0";
        const SourceModel off = readSource(r.t, r.s, r.o);
        CHECK(!off.ppm_on && !off.ppm_shown_value && off.ppm_note.empty() && off.ppm_refusal.empty() && off.ppm_chip == "OFF" && off.ppm_text == "+1.5 ppm");
    }

    // ---- the Radio row's WFM extras ----
    {
        FoxReceiverState s{};
        s.structSize = sizeof(s);
        s.demodMode = FOXAPI_DEMOD_NFM;
        const RadioExtrasModel nfm = readRadioExtras(s);
        CHECK(!nfm.wfm && !nfm.rds_shown);
        s.demodMode = FOXAPI_DEMOD_WFM;
        s.flags = FOXAPI_RX_STEREO_ENABLED | FOXAPI_RX_STEREO_ACTIVE | FOXAPI_RX_PILOT_LOCKED;
        s.pilotLevel = 0.123;
        s.rdsFlags = FOXAPI_RDS_SYNCED | FOXAPI_RDS_PI_VALID | FOXAPI_RDS_PS_VALID | FOXAPI_RDS_RT_VALID | FOXAPI_RDS_TP | FOXAPI_RDS_TA;
        s.rdsPi = 0xC201;
        s.rdsPty = 10;
        setText(s.rdsPs, sizeof(s.rdsPs), "BBC R2  ");
        setText(s.rdsRadioText, sizeof(s.rdsRadioText), "Now playing: something good");
        s.rdsGroups = 120;
        s.rdsErrors = 3;
        const RadioExtrasModel st = readRadioExtras(s);
        CHECK(st.wfm && st.stereo && st.stereo_word == "ST" && st.stereo_tone == Tone::Good && st.pilot_text == "pilot 0.12");
        CHECK(st.rds_shown && st.rds_ps == "PS  BBC R2  " && st.rds_pi == "PI  C201   PTY 10  TP  TA" && st.rds_rt == "Now playing: something good" && st.rds_stats == "groups 120 | block errors 3" && st.rds_status.empty());
        CHECK(has(st.stereo_tooltip, "Decode the 38 kHz difference channel"));
        s.flags = FOXAPI_RX_PILOT_LOCKED;   // forced mono over a pilot
        const RadioExtrasModel forced = readRadioExtras(s);
        CHECK(!forced.stereo && forced.stereo_word == "ST (forced mono)" && forced.stereo_tone == Tone::Warn && forced.pilot_text == "pilot 0.12");
        s.flags = 0;
        s.pilotLevel = 0;
        s.rdsFlags = 0;
        const RadioExtrasModel mono = readRadioExtras(s);
        CHECK(mono.stereo_word == "MONO" && mono.stereo_tone == Tone::Neutral && mono.pilot_text.empty() && mono.rds_status == "RDS: no data" && mono.rds_ps.empty() && mono.rds_stats.empty());
        s.rdsFlags = FOXAPI_RDS_SYNCED;
        CHECK(readRadioExtras(s).rds_status == "RDS: syncing...");
        s.rdsFlags = FOXAPI_RDS_PI_VALID;   // PI alone: the other lines show their blanks
        const RadioExtrasModel pi = readRadioExtras(s);
        CHECK(pi.rds_ps == "PS  --------" && pi.rds_pi == "PI  C201   PTY 10" && pi.rds_rt.empty() && pi.rds_status.empty());
    }

    // ---- the audio filters ----
    {
        FoxReceiverState s{};
        s.structSize = sizeof(s);
        const AudioFiltersModel off = readAudioFilters(s);
        CHECK(off.chip == "OFF" && !off.lamp_lit && !off.nr_on && !off.notch_on && !off.auto_notch_on && off.auto_notch_status.empty() && !off.auto_notch_locked);
        CHECK(off.chain_note == "chain: notch -> auto-notch -> noise reduction");
        s.flags = FOXAPI_RX_NR | FOXAPI_RX_AUTO_NOTCH | FOXAPI_RX_AUTO_NOTCH_ENGAGED;
        s.nrStrength = 0.5;
        s.notchHz = 1000.0;
        s.notchQ = 30.0;
        s.autoNotchHz = 1234.4;
        const AudioFiltersModel on = readAudioFilters(s);
        CHECK(on.chip == "2 ON" && on.lamp_lit && on.nr_on && on.auto_notch_on && !on.notch_on);
        CHECK_NEAR(on.nr_strength, 0.5, 1e-6);
        CHECK(on.nr_text == "0.50" && on.notch_freq_text == "1000 Hz" && on.notch_q_text == "30");
        CHECK_NEAR(on.notch_freq, std::log(100.0) / std::log(2000.0), 1e-5);
        CHECK_NEAR(on.notch_q, 29.0 / 199.0, 1e-5);
        CHECK(on.auto_notch_status == "on 1234 Hz" && on.auto_notch_locked);
        s.flags = FOXAPI_RX_AUTO_NOTCH | FOXAPI_RX_NOTCH | FOXAPI_RX_NR;
        const AudioFiltersModel three = readAudioFilters(s);
        CHECK(three.chip == "3 ON" && three.auto_notch_status == "searching" && !three.auto_notch_locked);
        CHECK(autoNotchStatus(false, false, 0).empty() && autoNotchStatus(true, true, 999.6) == "on 1000 Hz" && autoNotchStatus(true, false, 0) == "searching");
        CHECK(filtersChip(0) == "OFF" && filtersChip(1) == "1 ON" && filtersChip(3) == "3 ON");
        CHECK_NEAR(notchNormFromHz(10), 0.0, 1e-9);
        CHECK_NEAR(notchNormFromHz(20000), 1.0, 1e-9);
        CHECK_NEAR(notchHzFromNorm(0.0f), 10.0, 1e-6);
        CHECK_NEAR(notchHzFromNorm(1.0f), 20000.0, 1e-3);
        CHECK_NEAR(notchHzFromNorm(notchNormFromHz(1000.0)), 1000.0, 1e-2);
        CHECK_NEAR(notchNormFromHz(5.0), 0.0, 1e-9);   // clamped to the slider's span
        CHECK_NEAR(notchQNorm(1), 0.0, 1e-9);
        CHECK_NEAR(notchQNorm(200), 1.0, 1e-9);
        CHECK_NEAR(notchQFromNorm(0.5f), 100.5, 1e-4);
    }

    // ---- the sinks, with a muted-by cause ----
    {
        Rig r;
        r.t.add(FOXAPI_LIST_AUDIO_DEVICES, 0, "Speakers (Realtek)", "", 0, 0);
        r.t.add(FOXAPI_LIST_AUDIO_DEVICES, 0, "Headphones (USB)", "", FOXAPI_ITEM_ACTIVE, 2);
        r.t.facts["fact.audio.bufferMs"] = "0";
        r.t.facts["fact.audio.leadMs"] = "240";
        r.t.facts["fact.audio.leadNote"] = "raised 240 3";
        r.t.facts["fact.status.audioHealth"] = "The audio output stalled and was reopened.";
        r.s.audioFlags = FOXAPI_AUDIO_EVER_OPENED | FOXAPI_AUDIO_STREAM_ALIVE;
        setText(r.s.audioMutedBy, sizeof(r.s.audioMutedBy), "POCSAG");
        const SinksModel m = readSinks(r.t, r.s);
        CHECK(m.devices.size() == 2 && m.device_index == 1 && m.device_text == "Headphones (USB)" && at(m.devices, 1).id == "2" && at(m.devices, 0).label == "Speakers (Realtek)");
        CHECK(m.none_line.empty() && m.health_note == "The audio output stalled and was reopened.");
        CHECK(m.buffer_text == "AUTOMATIC  240 ms" && m.buffers.size() == 5 && m.buffer_index == 0 && at(m.buffers, 0).label == "AUTOMATIC" && at(m.buffers, 2).label == "240 ms" && at(m.buffers, 2).id == "240");
        CHECK(m.lead_notice == "Audio buffer raised to 240 ms: this computer fell behind 3 times in the last minute.");
        CHECK(m.muted_by == "Muted by POCSAG" && m.muted_note == "sound returns when the plugin stops");
        CHECK(m.chip == "MUTED" && m.chip_tone == Tone::Warn && m.lamp_lit && has(m.buffer_tooltip, "How much sound is held back before it plays."));
        r.t.facts["fact.audio.bufferMs"] = "480";
        r.s.audioMutedBy[0] = '\0';
        const SinksModel fixed = readSinks(r.t, r.s);
        CHECK(fixed.buffer_text == "480 ms" && fixed.buffer_index == 3 && fixed.muted_by.empty() && fixed.muted_note.empty() && fixed.chip == "ON" && fixed.chip_tone == Tone::Good);
        // no devices
        Rig none;
        const SinksModel mn = readSinks(none.t, none.s);
        CHECK(mn.devices.empty() && mn.device_index == -1 && mn.none_line == "No audio output devices" && mn.chip == "NO DEV" && mn.chip_tone == Tone::Neutral && !mn.lamp_lit);
        // the four states of the chip
        FoxReceiverState s{};
        CHECK(sinkChip(s).word == "NO DEV");
        s.audioFlags = FOXAPI_AUDIO_OPENING;
        CHECK(sinkChip(s).word == "OPENING" && sinkChip(s).tone == Tone::Warn && sinkChip(s).lit);
        s.audioFlags = FOXAPI_AUDIO_EVER_OPENED;
        CHECK(sinkChip(s).word == "DEAD" && sinkChip(s).tone == Tone::Err && sinkChip(s).lit);
        s.audioFlags = FOXAPI_AUDIO_EVER_OPENED | FOXAPI_AUDIO_STREAM_ALIVE;
        CHECK(sinkChip(s).word == "ON" && sinkChip(s).tone == Tone::Good);
        // the buffer words
        CHECK(bufferChoices() == std::vector<int>({0, 120, 240, 480, 960}));
        CHECK(bufferLabel(0) == "AUTOMATIC" && bufferLabel(960) == "960 ms" && bufferPreview(0, 120) == "AUTOMATIC  120 ms" && bufferPreview(240, 120) == "240 ms");
        CHECK(audioLeadNotice("restored 240 0") == "Audio buffer restored to 240 ms: the depth this computer needed when it last ran.");
        CHECK(audioLeadNotice("lowered 120 15") == "Audio buffer lowered to 120 ms after 15 quiet minutes.");
        CHECK(audioLeadNotice("").empty() && audioLeadNotice("raised").empty() && audioLeadNotice("nonsense 1 2").empty());
    }

    // ---- the recorder: idle, recording, starting, cancelling, full, stalled ----
    {
        Rig idle;
        idle.t.facts["fact.recorder.dir"] = "C:\\Users\\steve\\Documents\\FoxSDR";
        idle.t.facts["fact.recorder.start"] = "00";
        const RecorderModel m = readRecorder(idle.t, idle.s, idle.o);
        CHECK(m.folder == "C:\\Users\\steve\\Documents\\FoxSDR" && m.iq_label == "Record IQ" && m.audio_label == "Record audio" && !m.iq_running && !m.audio_running);
        CHECK(m.iq_enabled && m.audio_enabled && m.iq_progress.empty() && m.audio_progress.empty() && m.chip == "OFF" && m.chip_tone == Tone::Neutral && !m.lamp_lit);
        CHECK(m.hint.empty() && m.muted_warning.empty() && m.error_line.empty() && m.notice.empty() && m.import_label == "Import");

        Rig rec;
        rec.s.flags = FOXAPI_RX_RUNNING | FOXAPI_RX_RECORDING_IQ | FOXAPI_RX_RECORDING_AUDIO;
        setText(rec.s.audioMutedBy, sizeof(rec.s.audioMutedBy), "POCSAG");
        rec.t.add(FOXAPI_LIST_RECORDINGS, 0, "iq-1.wav", "C:\\r\\iq-1.wav", FOXAPI_ITEM_ACTIVE, 1, 8.0e6, 2.4e6, 3.0);
        rec.t.add(FOXAPI_LIST_RECORDINGS, 0, "audio-1.wav", "C:\\r\\audio-1.wav", FOXAPI_ITEM_ACTIVE, 2, 96000, 48000, 1.0);
        rec.t.facts["fact.recorder.start"] = "00";
        const RecorderModel mr = readRecorder(rec.t, rec.s, rec.o);
        CHECK(mr.iq_label == "Stop IQ" && mr.iq_running && mr.iq_progress == "IQ 3.0 s | 1000000 samples | 8.0 MB");
        CHECK(mr.audio_label == "Stop audio" && mr.audio_running && mr.audio_progress == "Audio 1.0 s | 48000 samples | 0.1 MB");
        CHECK(mr.chip == "REC" && mr.chip_tone == Tone::Err && mr.lamp_lit && mr.muted_warning == "Muted by POCSAG - this take is silent" && mr.hint.empty());
        // the audio take alone: no warning for an I/Q take, and a stopped receiver is said
        Rig iqOnly = rec;
        iqOnly.s.flags = FOXAPI_RX_RECORDING_IQ;
        iqOnly.t.lists[FOXAPI_LIST_RECORDINGS].erase(iqOnly.t.lists[FOXAPI_LIST_RECORDINGS].begin() + 1);
        const RecorderModel mi = readRecorder(iqOnly.t, iqOnly.s, iqOnly.o);
        CHECK(mi.muted_warning.empty() && mi.hint == "(press Play to feed the recorders)" && mi.audio_label == "Record audio");

        Rig st;
        st.t.facts["fact.recorder.start"] = "10";
        st.o.iq_start_wait_s = 2.2;
        const RecorderModel ms = readRecorder(st.t, st.s, st.o);
        CHECK(ms.iq_label == "Starting IQ - click to cancel" && ms.iq_enabled && ms.iq_progress == "Waiting for the disk to open the file: 2 s" && ms.audio_label == "Record audio" && ms.iq_start == 1);
        st.o.iq_start_wait_s = 0.5;
        CHECK(readRecorder(st.t, st.s, st.o).iq_progress.empty());   // a start answered at once never shows the line
        st.t.facts["fact.recorder.start"] = "02";
        st.o.audio_start_wait_s = 1.0;
        const RecorderModel mc = readRecorder(st.t, st.s, st.o);
        CHECK(mc.audio_label == "Cancelling audio start..." && !mc.audio_enabled && mc.iq_enabled && mc.audio_progress == "Waiting for the disk to open the file: 1 s" && mc.audio_start == 2);

        Rig full;
        full.t.add(FOXAPI_LIST_RECORDINGS, 0, "iq-2.wav", "C:\\r\\iq-2.wav", FOXAPI_ITEM_FULL, 1, 4.29e9, 2.4e6, 300.0);
        const RecorderModel mf = readRecorder(full.t, full.s, full.o);
        CHECK(mf.chip == "FULL" && mf.chip_tone == Tone::Warn && !mf.lamp_lit && has(mf.notice, "I/Q take reached the 4 GB limit for a WAV file") && has(mf.notice, "the file was closed and is complete"));
        full.t.lists[FOXAPI_LIST_RECORDINGS].clear();
        full.t.add(FOXAPI_LIST_RECORDINGS, 0, "a.wav", "C:\\r\\a.wav", FOXAPI_ITEM_FULL, 2, 4.29e9, 48000, 300.0);
        CHECK(has(readRecorder(full.t, full.s, full.o).notice, "audio take reached the 4 GB limit for a WAV file"));

        Rig stalled;
        stalled.s.flags = FOXAPI_RX_RUNNING | FOXAPI_RX_RECORDING_IQ;
        stalled.t.add(FOXAPI_LIST_RECORDINGS, 0, "iq-3.wav", "C:\\r\\iq-3.wav", FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_STALLED, 1, 1.0e6, 2.4e6, 2.0);
        stalled.t.facts["fact.status.recordError"] = "Could not create the recording folder.";
        stalled.t.facts["fact.status.recordNotice"] = "Saved to the Documents folder.";
        const RecorderModel mst = readRecorder(stalled.t, stalled.s, stalled.o);
        CHECK(mst.chip == "STALLED" && mst.chip_tone == Tone::Err && mst.lamp_lit);
        CHECK(has(mst.error_line, "Could not create the recording folder.") && has(mst.error_line, "disk refused the file - nothing more is being kept") && mst.notice == "Saved to the Documents folder.");
        // the labels and the take's own words
        CHECK(recorderLabel(Take::Iq, 0, false) == "Record IQ" && recorderLabel(Take::Iq, 0, true) == "Stop IQ" && recorderLabel(Take::Iq, 1, false) == "Starting IQ - click to cancel" && recorderLabel(Take::Iq, 2, false) == "Cancelling IQ start...");
        CHECK(recorderLabel(Take::Audio, 0, false) == "Record audio" && recorderLabel(Take::Audio, 0, true) == "Stop audio" && recorderLabel(Take::Audio, 1, false) == "Starting audio - click to cancel" && recorderLabel(Take::Audio, 2, false) == "Cancelling audio start...");
        CHECK(takeProgress(Take::Iq, 3.04, 8.0e6) == "IQ 3.0 s | 1000000 samples | 8.0 MB" && takeProgress(Take::Audio, 0, 0) == "Audio 0.0 s | 0 samples | 0.0 MB");
        CHECK(diskWaitLine(0.99).empty() && diskWaitLine(1.0) == "Waiting for the disk to open the file: 1 s" && diskWaitLine(12.4) == "Waiting for the disk to open the file: 12 s");
        CHECK(recorderStartOf("10", Take::Iq) == 1 && recorderStartOf("10", Take::Audio) == 0 && recorderStartOf("02", Take::Audio) == 2 && recorderStartOf("", Take::Iq) == 0 && recorderStartOf("1", Take::Audio) == 0);
    }

    // ---- the Transmit row's state ----
    {
        FoxReceiverState s{};
        s.structSize = sizeof(s);
        TransmitModel t = readTransmit(s);
        CHECK(t.chip == "NO RADIO" && t.tone == Tone::Neutral && !t.available && !t.keyed && !t.armed && t.fault.empty());
        s.flags = FOXAPI_RX_TX_AVAILABLE;
        t = readTransmit(s);
        CHECK(t.chip == "READY" && t.tone == Tone::Warn && t.available);
        s.flags = FOXAPI_RX_TX_AVAILABLE | FOXAPI_RX_TX_KEYED | FOXAPI_RX_TX_REMOTE_ARMED;
        t = readTransmit(s);
        CHECK(t.chip == "ON AIR" && t.tone == Tone::Err && t.keyed && t.armed);
        s.flags = FOXAPI_RX_TX_AVAILABLE | FOXAPI_RX_TX_KEYED;
        s.txFlags = FOXAPI_TX_FAULTED;
        setText(s.txError, sizeof(s.txError), "The Pluto stopped answering.");
        t = readTransmit(s);
        CHECK(t.chip == "FAULT" && t.tone == Tone::Err && t.fault == "The Pluto stopped answering.");   // a fault outranks the key
        CHECK(transmitChip(false, false, false).word == "NO RADIO" && transmitChip(true, false, false).word == "READY" && transmitChip(true, false, true).word == "ON AIR" && transmitChip(false, true, false).word == "FAULT");
    }

    // ---- the import dialog and the Radio setup page ----
    {
        ReadOptions o;
        o.import_shown = true;
        o.import_path = "C:\\lists\\frequencies.xml";
        o.import_reason = "Could not import: the file \"x.csv\" could not be read";
        o.import_kind = 1;
        o.import_reading = true;
        o.import_wait_s = 2.0;
        const ImportDialogModel d = readImportDialog(o);
        CHECK(d.shown && d.path == "C:\\lists\\frequencies.xml" && d.error == o.import_reason && d.kind_index == 1 && d.kinds.size() == 2);
        CHECK(at(d.kinds, 0).id == "frequencies" && at(d.kinds, 0).label == "Frequency lists (*.xml, *.csv)" && at(d.kinds, 1).id == "iq" && at(d.kinds, 1).label == "IQ file");
        CHECK(d.reading_line == "Waiting for the disk to open the file: 2 s");
        o.import_wait_s = 0.2;
        CHECK(readImportDialog(o).reading_line.empty());
        o.import_reading = false;
        o.import_wait_s = 5.0;
        CHECK(readImportDialog(o).reading_line.empty());
        CHECK(!readImportDialog(ReadOptions{}).shown);

        Rig r;
        r.o.setup_shown = true;
        r.o.setup_checking = true;
        r.o.setup_dont_show = true;
        RadioSetupModel rs = readRadioSetup(r.t, r.s, r.o);
        CHECK(rs.shown && rs.title == "RADIO SETUP" && rs.busy == "Checking this computer...");
        CHECK(at(rs.keys, 0).id == "check-again" && at(rs.keys, 0).label == "CHECK AGAIN" && !at(rs.keys, 0).enabled);
        CHECK(rs.keys.size() == 2 && at(rs.keys, 1).id == "dont-show" && at(rs.keys, 1).label == "Don't show this again" && at(rs.keys, 1).on && at(rs.keys, 1).enabled);
        r.o.setup_checking = false;
        r.o.setup_have_answer = true;
        r.o.setup_show_page = true;
        SetupFinding f;
        f.title = "RTL-SDR: the wrong driver is bound";
        f.explanation = "Windows has bound a television driver to it.";
        f.install = "Run Zadig and choose WinUSB.";
        f.url = "https://zadig.akeo.ie";
        f.install_target = "USB\\VID_0BDA&PID_2838\\1";
        r.o.findings.push_back(f);
        r.o.setup_install_status = "Installing the driver - answer the Windows permission prompt...";
        rs = readRadioSetup(r.t, r.s, r.o);
        CHECK(rs.busy.empty() && at(rs.lines, 0) == "FoxSDR found no radio it can use. This is what this computer shows, and what to do about it:");
        CHECK(at(rs.lines, 1) == f.title && at(rs.lines, 2) == f.explanation && at(rs.lines, 3) == f.install && rs.lines.back() == r.o.setup_install_status);
        CHECK(at(rs.keys, 0).id == "install-driver:USB\\VID_0BDA&PID_2838\\1" && at(rs.keys, 0).label == "INSTALL DRIVER" && at(rs.keys, 0).enabled && has(at(rs.keys, 0).tooltip, "One Windows permission prompt, no Zadig."));
        CHECK(at(rs.keys, 1).id == "link:https://zadig.akeo.ie" && at(rs.keys, 1).label == "zadig.akeo.ie" && at(rs.keys, 1).tooltip == "https://zadig.akeo.ie");
        CHECK(at(rs.keys, 2).id == "check-again" && at(rs.keys, 2).enabled && at(rs.keys, 3).id == "dont-show");
        r.o.setup_installing = true;
        CHECK(!at(readRadioSetup(r.t, r.s, r.o).keys, 0).enabled);
        r.o.setup_show_page = false;
        CHECK(at(readRadioSetup(r.t, r.s, r.o).lines, 0) == "This is what this computer shows about your radios:");
        CHECK(!readRadioSetup(r.t, r.s, ReadOptions{}).shown);
    }

    // ---- readSignal composes them all, reading the table once through the seam ----
    {
        Rig r = rtlOpen();
        r.t.add(FOXAPI_LIST_AUDIO_DEVICES, 0, "Speakers", "", FOXAPI_ITEM_ACTIVE, 0);
        r.s.demodMode = FOXAPI_DEMOD_WFM;
        r.s.flags |= FOXAPI_RX_NR;
        r.s.audioFlags = FOXAPI_AUDIO_EVER_OPENED | FOXAPI_AUDIO_STREAM_ALIVE;
        const SignalModels all = readSignal(r.t, r.s, r.o);
        CHECK(all.source.device_open && all.radio.wfm && all.filters.nr_on && all.sinks.devices.size() == 1 && all.recorder.iq_label == "Record IQ" && all.transmit.chip == "NO RADIO");
        CHECK(!all.setup.shown && !all.import.shown);
    }

    // ---- the commands: the ImGui window's own, exactly ----
    {
        using namespace cascade::core::cmd;
        Rig r = rtlOpen();
        const SourceModel m = readSource(r.t, r.s, r.o);

        Command c = opDeviceSelected(r.t, m, 3);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SELECT_SOURCE && textOf(c.q.c, c.q.longText) == "rtlsdr:index=0,serial=00000001" && c.grant == FOXAPI_GRANT_SETTINGS);
        CHECK(!opDeviceSelected(r.t, m, 5).valid);    // the fact for entry 6 is not held: nothing is sent
        CHECK(!opDeviceSelected(r.t, m, 99).valid && !opDeviceSelected(r.t, m, -1).valid);
        Rig held = generator();
        held.s.flags |= FOXAPI_RX_PATCH_RUNNING;
        held.o.patch_held = {"X"};
        const SourceModel mh = readSource(held.t, held.s, held.o);
        CHECK(!opDeviceSelected(held.t, mh, 6).valid && opDeviceSelected(held.t, mh, 0).valid);   // a hardware row is greyed while the patch runs; the generator is not
        CHECK(opDeviceSelected(held.t, mh, 0).q.c.op == FOXAPI_OP_SELECT_SOURCE && textOf(opDeviceSelected(held.t, mh, 0).q.c, "") == "siggen:");

        c = opRefresh();
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SCAN_DEVICES && c.q.c.op == 0x0301u);
        c = opScanOnOpen();
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SCAN_DEVICES_ON_OPEN);
        c = opUsrps(true);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_NETWORK_USRP_SCAN && c.q.c.ival[0] == 1 && opUsrps(false).q.c.ival[0] == 0);
        c = opRateSelected(m, 2);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SET_SAMPLE_RATE && c.q.c.num[0] == 3.2e6);
        CHECK(!opRateSelected(m, 1).valid && !opRateSelected(m, 9).valid);   // the row already shown sends nothing
        Rig twoPorts = rtlOpen();
        twoPorts.t.add(FOXAPI_LIST_ANTENNAS, 0, "TX/RX", "");
        c = opAntennaSelected(readSource(twoPorts.t, twoPorts.s, twoPorts.o), 1);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SET_ANTENNA && textOf(c.q.c, c.q.longText) == "TX/RX");
        CHECK(!opAntennaSelected(m, 0).valid);
        c = opAutoGain(true);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SET_DEVICE_AGC && c.q.c.ival[0] == 1);
        c = opGainChanged(m, 0, 0.5f);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SET_GAIN && textOf(c.q.c, c.q.longText) == "TUNER" && std::fabs(c.q.c.num[0] - 24.8) < 1e-4);
        CHECK(!opGainChanged(m, 4, 0.5f).valid && !opGainChanged(m, -1, 0.5f).valid);
        c = opBiasTee(false, "");
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SET_BIAS_TEE && c.q.c.ival[0] == 0 && textOf(c.q.c, "").empty());
        c = opBiasTee(true, "rtlsdr|serial=00000001");
        CHECK(c.q.c.ival[0] == 1 && textOf(c.q.c, c.q.longText) == "rtlsdr|serial=00000001");
        c = opPpmToggled(true, 1.5);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_PPM && c.q.c.ival[0] == 1 && c.q.c.num[0] == 1.5);
        c = opPpmToggled(false, 1.5);
        CHECK(c.q.c.ival[0] == 0 && c.q.c.num[0] == 1.5);
        c = opPpmCommitted("2.34", 1.5);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_PPM && c.q.c.ival[0] == 1 && c.q.c.num[0] == 2.3);
        CHECK(!opPpmCommitted("1.5 ppm", 1.5).valid && !opPpmCommitted("junk", 1.5).valid);
        CHECK(opPpmCommitted("500", 1.5).q.c.num[0] == 200.0);

        c = opIqOpen("C:\\iq\\a.wav");
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SELECT_SOURCE && textOf(c.q.c, c.q.longText) == "file:C:\\iq\\a.wav");
        CHECK(!opIqOpen("").valid);
        const std::string longPath = "C:\\" + std::string(300, 'x') + ".wav";
        c = opIqOpen(longPath);
        CHECK(c.valid && !c.q.longText.empty() && textOf(c.q.c, c.q.longText) == "file:" + longPath);
        c = opPlutoAddress("ip:pluto.local");
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_PLUTO_URI && textOf(c.q.c, "") == "ip:pluto.local");
        c = opPlutoOpen("ip:192.168.2.1");
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SELECT_SOURCE && textOf(c.q.c, "") == "open-pluto:uri=ip:192.168.2.1");
        c = opRtlTcpAddress("host:1234");
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_RTLTCP_ADDR && textOf(c.q.c, "") == "host:1234");
        c = opRtlTcpOpen("host:1234");
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SELECT_SOURCE && textOf(c.q.c, "") == "open-rtltcp:rtltcp=host:1234");

        Rig cardRig = soundCard();
        const SourceModel cm = readSource(cardRig.t, cardRig.s, cardRig.o);
        c = opCardSelected(cm, 0);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SOUND_CARD_PICK && c.q.c.ival[0] == 1);
        CHECK(!opCardSelected(cm, 1).valid && !opCardSelected(cm, 7).valid);   // the card already chosen sends nothing
        c = opCardFormat(0);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SOUND_CARD_FORM && textOf(c.q.c, "") == std::string(1, '\x1F') && c.q.c.ival[0] == 0 && c.q.c.ival[1] == FOXAPP_SOUND_CARD_FIELD_FORMAT);
        CHECK(opCardFormat(1).q.c.ival[0] == 1);
        CHECK(c.q.c.num[0] == 48000.0 && c.q.c.num[1] == 0.0);   // the other fields ride at a default SoundCardSettings' values, as the drawer sends them; the mask names the one that counts
        c = opCardChannel(1);
        CHECK(c.q.c.op == FOXAPP_OP_SOUND_CARD_FORM && c.q.c.ival[0] == 2 && c.q.c.ival[1] == FOXAPP_SOUND_CARD_FIELD_CHANNEL && opCardChannel(0).q.c.ival[0] == 0);
        c = opCardSwap(true);
        CHECK(c.q.c.op == FOXAPP_OP_SOUND_CARD_FORM && c.q.c.ival[0] == 4 && c.q.c.ival[1] == FOXAPP_SOUND_CARD_FIELD_SWAP_IQ && opCardSwap(false).q.c.ival[0] == 0);
        std::vector<Command> two = opCardCentre("7.05");
        CHECK(two.size() == 2 && at(two, 0).q.c.op == FOXAPP_OP_SOUND_CARD_FORM && at(two, 0).q.c.ival[1] == FOXAPP_SOUND_CARD_FIELD_CENTRE && at(two, 0).q.c.num[1] == 7.05e6 && at(two, 0).q.c.num[0] == 48000.0);
        CHECK(two.size() == 2 && at(two, 1).q.c.op == FOXAPP_OP_SOUNDCARD_IQ_CENTRE && at(two, 1).q.c.num[0] == 7.05e6);
        CHECK(opCardCentre("abc").empty() && opCardCentre("").empty() && opCardCentre("inf").empty());
        c = opCardRate(cm, 0);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SOUND_CARD_FORM && c.q.c.ival[1] == FOXAPP_SOUND_CARD_FIELD_RATE && c.q.c.num[0] == 44100);
        CHECK(!opCardRate(cm, 5).valid && opCardRate(cm, 1).valid && opCardRate(cm, 1).q.c.num[0] == 192000);   // the rate in use sends too, as its Selectable does
        c = opCardOpen(cm);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SELECT_SOURCE && textOf(c.q.c, "") == "soundcard:open");
        SourceModel nocard = cm;
        nocard.card_can_open = false;
        CHECK(!opCardOpen(nocard).valid);

        Rig ar = airspy(true);
        const SourceModel am = readSource(ar.t, ar.s, ar.o);
        c = opAirspyDecimation(am, 1);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_AIRSPY_DECIMATION && c.q.c.ival[0] == 2 && !opAirspyDecimation(am, 2).valid);
        c = opAirspyGainMode(am, 0);   // the radio is in Free (button 2)
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_AIRSPY_GAIN_MODE && c.q.c.ival[0] == 1 && opAirspyGainMode(am, 1).q.c.ival[0] == 0 && !opAirspyGainMode(am, 2).valid && !opAirspyGainMode(am, 3).valid);
        c = opAirspyAgc(false, true);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_AIRSPY_AGC && c.q.c.ival[0] == 0 && c.q.c.ival[1] == 1);
        c = opAirspyAgc(true, false);
        CHECK(c.q.c.ival[0] == 1 && c.q.c.ival[1] == 0);
        c = opAirspyStage(am, "VGA", 0.4f);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SET_GAIN && textOf(c.q.c, "") == "VGA" && c.q.c.num[0] == 6.0);
        CHECK(!opAirspyStage(am, "NOPE", 0.4f).valid);
        Rig as = airspy(false);
        c = opAirspyGain(readSource(as.t, as.s, as.o), 0.5f);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SET_GAIN && textOf(c.q.c, "") == "GAIN" && c.q.c.num[0] == 11.0 && c.q.c.op != FOXAPP_OP_SET_GAIN_NO_READBACK);
        CHECK(!opAirspyGain(m, 0.5f).valid);   // an RTL-SDR has no Airspy stage

        Rig opt = rtlOpen();
        opt.t.add(FOXAPI_LIST_DEVICE_OPTIONS, 0, "hdr", "", 0);
        c = opOptionToggled(readSource(opt.t, opt.s, opt.o), 0, true);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SET_DEVICE_OPTION && textOf(c.q.c, "") == "hdr" && c.q.c.ival[0] == 1 && !opOptionToggled(m, 0, true).valid);

        ConverterSetting off;
        c = opConverterMode(off, 1);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_CONVERTER && c.q.c.ival[0] == 1 && c.q.c.ival[1] == 0 && c.q.c.num[0] == 125e6);   // switched on with no LO: the default
        ConverterSetting two100{1, 100e6, true};
        c = opConverterMode(two100, 2);
        CHECK(c.q.c.ival[0] == 2 && c.q.c.ival[1] == 1 && c.q.c.num[0] == 100e6);
        c = opConverterMode(two100, 0);
        CHECK(c.q.c.ival[0] == 0 && c.q.c.num[0] == 100e6);
        c = opConverterLo(two100, "2 MHz");
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_CONVERTER && c.q.c.ival[0] == 1 && c.q.c.ival[1] == 1 && c.q.c.num[0] == 2e6);
        CHECK(!opConverterLo(two100, "100 MHz").valid && !opConverterLo(two100, "garbage").valid && opConverterLo(two100, "125000 kHz").q.c.num[0] == 125e6);
        c = opConverterQuick(two100, 0);
        CHECK(c.valid && c.q.c.num[0] == 125e6 && c.q.c.ival[0] == 1 && !opConverterQuick(two100, 1).valid && opConverterQuick(two100, 2).q.c.num[0] == 2e6 && !opConverterQuick(two100, 3).valid);
        c = opConverterInverts(two100, false);
        CHECK(c.valid && c.q.c.ival[0] == 1 && c.q.c.ival[1] == 0 && c.q.c.num[0] == 100e6);

        c = opStereo(true);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SET_STEREO && c.q.c.ival[0] == 1);
        c = opNr(true);
        CHECK(c.q.c.op == FOXAPI_OP_SET_NR && c.q.c.ival[0] == 1 && c.q.c.ival[1] == 0);
        c = opNrStrength(0.75f);
        CHECK(c.q.c.op == FOXAPP_OP_SET_NR_STRENGTH && c.q.c.num[0] == 0.75);
        CHECK(opNrStrength(1.5f).q.c.num[0] == 1.0 && opNrStrength(-1.f).q.c.num[0] == 0.0);
        c = opNotch(true);
        CHECK(c.q.c.op == FOXAPI_OP_SET_NOTCH && c.q.c.ival[0] == 1 && c.q.c.ival[1] == 0);
        c = opNotchFreq(1.0f);
        CHECK(c.q.c.op == FOXAPP_OP_SET_NOTCH_FREQUENCY && std::fabs(c.q.c.num[0] - 20000.0) < 1e-2);
        c = opNotchQ(0.5f);
        CHECK(c.q.c.op == FOXAPP_OP_SET_NOTCH_Q && std::fabs(c.q.c.num[0] - 100.5) < 1e-3);
        c = opAutoNotch(true);
        CHECK(c.q.c.op == FOXAPI_OP_SET_AUTO_NOTCH && c.q.c.ival[0] == 1);

        Rig sr;
        sr.t.add(FOXAPI_LIST_AUDIO_DEVICES, 0, "Speakers", "", 0, 0);
        sr.t.add(FOXAPI_LIST_AUDIO_DEVICES, 0, "Headphones", "", FOXAPI_ITEM_ACTIVE, 7);
        const SinksModel sm = readSinks(sr.t, sr.s);
        c = opSinkDevice(sm, 0);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_AUDIO_DEVICE && c.q.c.ival[0] == 0 && !opSinkDevice(sm, 4).valid && !opSinkDevice(sm, -1).valid);
        CHECK(opSinkDevice(sm, 1).valid && opSinkDevice(sm, 1).q.c.ival[0] == 7);   // the row in use sends too, as the Selectable does: it re-opens the output
        c = opSinkBuffer(sm, 2);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_AUDIO_BUFFER && c.q.c.ival[0] == 240 && opSinkBuffer(sm, 0).q.c.ival[0] == 0 && !opSinkBuffer(sm, 5).valid);
        c = opTransmitPage(true);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN && c.q.c.ival[0] == 1 && c.grant == FOXAPI_GRANT_TRANSMIT);

        RecorderModel rm;
        rm.iq_label = "Record IQ";
        c = opRecorderIq(rm);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_RECORD_IQ && c.q.c.ival[0] == 1);
        rm.iq_running = true;
        CHECK(opRecorderIq(rm).q.c.ival[0] == 0);
        rm.iq_running = false;
        rm.iq_start = 1;
        CHECK(opRecorderIq(rm).valid && opRecorderIq(rm).q.c.ival[0] == 0);   // the press withdraws the start
        rm.iq_start = 2;
        CHECK(!opRecorderIq(rm).valid);                                       // while it is being cancelled nothing is sent
        c = opRecorderAudio(RecorderModel{});
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_RECORD_AUDIO && c.q.c.ival[0] == 1);
        RecorderModel am2;
        am2.audio_running = true;
        CHECK(opRecorderAudio(am2).q.c.ival[0] == 0);
        am2.audio_running = false;
        am2.audio_start = 2;
        CHECK(!opRecorderAudio(am2).valid);

        c = opSetupCheckAgain();
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SCAN_DEVICES);
        c = opImportPath("C:\\lists\\f.csv", 0);
        CHECK(c.valid && c.q.c.op == FOXAPP_OP_BOOKMARK_IMPORT_FILE && textOf(c.q.c, c.q.longText) == "C:\\lists\\f.csv");
        c = opImportPath("C:\\iq\\a.wav", 1);
        CHECK(c.valid && c.q.c.op == FOXAPI_OP_SELECT_SOURCE && textOf(c.q.c, c.q.longText) == "file:C:\\iq\\a.wav");
        CHECK(!opImportPath("", 0).valid && !opImportPath("x", 7).valid);

        // the grants the engine asks (fox_engine_api.cpp grantFor): SETTINGS for the receiver, TRANSMIT for the page
        CHECK(grantFor(FOXAPI_OP_SET_GAIN) == FOXAPI_GRANT_SETTINGS && grantFor(FOXAPP_OP_SET_PPM) == FOXAPI_GRANT_SETTINGS && grantFor(FOXAPI_OP_RECORD_IQ) == FOXAPI_GRANT_SETTINGS);
        CHECK(grantFor(FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN) == FOXAPI_GRANT_TRANSMIT && grantFor(FOXAPP_OP_NOTE_PANEL) == FOXAPI_GRANT_VIEW && grantFor(FOXAPI_OP_SETTING_SET) == FOXAPI_GRANT_ADMIN);
        CHECK(grantFor(FOXAPI_OP_SET_FREQUENCY) == FOXAPI_GRANT_TUNE);
    }

    return testSummary("test_slint_host_signal");
}
