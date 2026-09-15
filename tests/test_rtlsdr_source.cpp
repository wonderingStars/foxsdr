// THE NATIVE RTL-SDR DRIVER, PROVED ON THE WIRE.
//
// WHAT THIS FILE IS FOR, and why it is not a set of "did it return true"
// tests. A tuner whose PLL registers are written in the wrong order locks to
// a plausible frequency and every call in the chain returns success. A
// resampler ratio computed with the crystal off by a factor reports a rate
// nobody can tell is wrong until they compare a known signal. A tuner write
// sent while the demodulator's I2C repeater is off goes nowhere AND REPORTS
// SUCCESS, because the RTL2832U acknowledges the block write whether or not
// anything on the far side heard it. None of those is visible from a return
// value; all of them are visible in the transcript of bytes the driver put on
// the wire.
//
// So: the driver is opened against usb::FakeUsbDevice, which records every
// control transfer, and each block below asserts the transcript against
// values DERIVED FROM THE BEHAVIOUR ORACLE'S OWN ARITHMETIC - each one named
// in the comment beside it, with the calculation written out so a reader can
// check it without the oracle in front of them. Everything after the
// transport is the shipping code path.
//
// MASKED WRITES ARE ASSERTED THROUGH THEIR MASK, and that is not a weakening.
// The R82xx cannot be read back by address, so a field write is a
// read-modify-write against a shadow whose other bits carry the whole history
// of the initialisation. The bits the operation under test is RESPONSIBLE FOR
// are exactly the bits inside its mask; asserting the rest would be asserting
// the init sequence over again in every block, and would go red for a change
// that altered nothing this test is about.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "source/rtl2832u.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/tuner_r82xx.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

using cascade::source::Rtl2832u;
using cascade::source::RtlBlock;
using cascade::source::RtlSdrSource;
using cascade::source::TunerR82xx;
using cascade::usb::FakeControl;
using cascade::usb::FakeUsbDevice;

namespace {

// --- reading the transcript --------------------------------------------------

// One OUT transfer, in the three shapes this driver produces.
//   demod write : wIndex < 0x100 (it is 0x10 | page), wValue = (addr << 8)|0x20
//   block write : wIndex = (block << 8) | 0x10,        wValue = address
// A tuner write is a block write to block 6 (I2C) whose payload is
// {register, value}.
bool isDemodWrite(const FakeControl& c) { return c.out && c.index < 0x100; }
bool isBlockWrite(const FakeControl& c, int block) {
    return c.out && c.index == ((block << 8) | 0x10);
}

std::string describe(const FakeControl& c) {
    char head[48];
    if (isDemodWrite(c)) {
        std::snprintf(head, sizeof(head), "D%u %02X=", c.index & 0x0f,
                      static_cast<unsigned>(c.value >> 8));
    } else {
        std::snprintf(head, sizeof(head), "B%u %04X=", c.index >> 8,
                      static_cast<unsigned>(c.value));
    }
    std::string s(head);
    for (std::size_t i = 0; i < c.data.size(); ++i) {
        char b[8];
        std::snprintf(b, sizeof(b), "%s%02X", i ? " " : "", c.data[i]);
        s += b;
    }
    return s;
}

void dump(const char* what, const std::vector<FakeControl>& v) {
    std::printf("--- %s (%zu writes) ---\n", what, v.size());
    for (const FakeControl& c : v) { std::printf("    %s\n", describe(c).c_str()); }
}

// Every tuner (I2C block 6) write in a transcript, as {register, value}.
struct TunerWrite {
    std::uint8_t reg;
    std::uint8_t value;
};
std::vector<TunerWrite> tunerWrites(const std::vector<FakeControl>& v, std::uint8_t slave) {
    std::vector<TunerWrite> out;
    for (const FakeControl& c : v) {
        if (!isBlockWrite(c, 6) || c.value != slave) { continue; }
        // A block write of N values starting at `reg` is N single writes.
        for (std::size_t i = 1; i < c.data.size(); ++i) {
            out.push_back({static_cast<std::uint8_t>(c.data[0] + (i - 1)), c.data[i]});
        }
    }
    return out;
}

// Every demod write, as {page, address, value}.
struct DemodWrite {
    std::uint8_t page;
    std::uint8_t addr;
    std::uint8_t value;
};
std::vector<DemodWrite> demodWrites(const std::vector<FakeControl>& v) {
    std::vector<DemodWrite> out;
    for (const FakeControl& c : v) {
        if (!isDemodWrite(c) || c.data.empty()) { continue; }
        out.push_back({static_cast<std::uint8_t>(c.index & 0x0f),
                       static_cast<std::uint8_t>(c.value >> 8), c.data[0]});
    }
    return out;
}

// What the operation under test was responsible for: a register, the bits it
// owns, and the value those bits must hold. mask 0xff is a whole-register
// write.
struct Expect {
    std::uint8_t reg;
    std::uint8_t value;
    std::uint8_t mask;
};

// Asserts that `writes` IS this sequence - same length, same order, same
// masked values. Prints both sides on a mismatch, because "expected 12 got
// 11" is useless and a wire trace is not.
void expectTunerSequence(const char* what, const std::vector<TunerWrite>& writes,
                         const std::vector<Expect>& want) {
    bool ok = writes.size() == want.size();
    for (std::size_t i = 0; ok && i < want.size(); ++i) {
        if (writes[i].reg != want[i].reg) { ok = false; }
        if ((writes[i].value & want[i].mask) != (want[i].value & want[i].mask)) { ok = false; }
    }
    if (!ok) {
        std::printf("*** %s: the tuner register sequence is not what the oracle's arithmetic\n"
                    "*** says it must be.\n*** expected %zu writes, saw %zu:\n",
                    what, want.size(), writes.size());
        const std::size_t n = writes.size() > want.size() ? writes.size() : want.size();
        for (std::size_t i = 0; i < n; ++i) {
            char lhs[40] = "        (none)";
            char rhs[40] = "(none)";
            if (i < want.size()) {
                std::snprintf(lhs, sizeof(lhs), "  want %02X = %02X & %02X", want[i].reg,
                              want[i].value & want[i].mask, want[i].mask);
            }
            if (i < writes.size()) {
                std::snprintf(rhs, sizeof(rhs), "saw %02X = %02X", writes[i].reg,
                              writes[i].value);
            }
            std::printf("%-30s %s\n", lhs, rhs);
        }
    }
    CHECK(ok);
}

// --- the fake, dressed as a dongle -------------------------------------------

// THE ANSWERS THE R82xx GIVES BACK, and every bit of them is load-bearing.
// The part can only be read from register 0 onwards and it BIT-REVERSES every
// byte it returns, so these are the RAW bus bytes and the driver's own
// reversal turns them into the fields below:
//
//   raw[0] 0x69 - the identity byte, compared raw (the oracle's probe does
//                 the same; 0x69 on the wire is 0x96 in the register)
//   raw[2] 0x02 - reversed to 0x40, which is the PLL's "locked" bit
//   raw[4] 0xA4 - reversed to 0x25: the low nibble (5) is the IF filter's
//                 calibration code, and bits 5:4 (binary 10 = 2) are the VCO
//                 autotune report the divider correction is judged against
const std::vector<std::uint8_t> kTunerAnswer = {0x69, 0x00, 0x02, 0x00, 0xA4};

// A string descriptor, as GET_DESCRIPTOR returns one: length, type 3, then
// UTF-16LE.
std::vector<std::uint8_t> stringDescriptor(const std::string& s) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(2 + 2 * s.size()));
    out.push_back(0x03);
    for (const char c : s) {
        out.push_back(static_cast<std::uint8_t>(c));
        out.push_back(0x00);
    }
    return out;
}

// A plain R820T dongle: tuner at 0x34, no interesting USB strings, an EEPROM
// whose first byte is not the RTL2832U magic (so the bias tee stays off).
void dressAsR820T(FakeUsbDevice& fake) {
    fake.answerIn(0, 0x0034, 0x0600, kTunerAnswer);
    fake.answerIn(0, 0x00a0, 0x0600, {0x00});
}

// An RTL-SDR Blog V4: an R828D at 0x74, and the two USB strings that are the
// ONLY thing distinguishing it from any other R828D dongle.
void dressAsBlogV4(FakeUsbDevice& fake) {
    fake.answerIn(0, 0x0074, 0x0600, kTunerAnswer);
    fake.answerIn(0, 0x00a0, 0x0600, {0x00});
    fake.answerIn(0x06, 0x0301, 0x0409, stringDescriptor("RTLSDRBlog"));
    fake.answerIn(0x06, 0x0302, 0x0409, stringDescriptor("Blog V4"));
}

}  // namespace

int main() {
    // =======================================================================
    // 1. THE RESAMPLER'S ARITHMETIC.
    //
    // Oracle: rtlsdr_set_sample_rate() in librtlsdr.c.
    //   ratio = (xtal * 2^22) / rate, then ratio &= 0x0ffffffc
    //   real  = ratio | ((ratio & 0x08000000) << 1)
    //   exact = (xtal * 2^22) / real
    // and the two halves are written to demod page 1 registers 0x9f and 0xa1
    // as TWO-BYTE BIG-ENDIAN values - which is the opposite endianness to a
    // two-byte READ on the same chip, and getting it backwards programmes a
    // plausible wrong rate.
    // =======================================================================
    {
        FakeUsbDevice fake;
        Rtl2832u rtl(fake, 100);

        // 2.4 MS/s: 28,800,000 * 4,194,304 = 120,795,955,200,000
        //           / 2,400,000            =      50,331,648 = 0x03000000
        //           & 0x0ffffffc           =      0x03000000 (already aligned)
        //           bit 27 clear, so real == ratio and the rate is EXACT.
        double actual = 0.0;
        CHECK(rtl.setSampleRate(2400000, actual));
        CHECK_NEAR(actual, 2400000.0, 0.001);
        const std::vector<DemodWrite> d = demodWrites(fake.writes());
        // 0x9f takes the high half (0x0300), 0xa1 the low (0x0000); each is
        // ONE two-byte transfer, so the bytes are checked on the transfer.
        CHECK(d.size() >= 4);
        CHECK(d[0].page == 1 && d[0].addr == 0x9f);
        CHECK(d[1].page == 1 && d[1].addr == 0xa1);
        std::vector<FakeControl> w = fake.writes();
        CHECK(w[0].data.size() == 2 && w[0].data[0] == 0x03 && w[0].data[1] == 0x00);
        CHECK(w[1].data.size() == 2 && w[1].data[0] == 0x00 && w[1].data[1] == 0x00);

        // 1.0 MS/s: 120,795,955,200,000 / 1,000,000 = 120,795,955.2 -> the
        //           uint32 truncation gives 0x07333333, masked to 0x07333330
        //           = 120,795,952, and the achieved rate is therefore
        //           120,795,955,200,000 / 120,795,952 = 1,000,000.0265 Hz.
        //           This is exactly the number librtlsdr prints as "Exact
        //           sample rate is".
        fake.clear();
        actual = 0.0;
        CHECK(rtl.setSampleRate(1000000, actual));
        CHECK_NEAR(actual, 1000000.0265, 0.002);
        w = fake.writes();
        CHECK(w[0].data.size() == 2 && w[0].data[0] == 0x07 && w[0].data[1] == 0x33);
        CHECK(w[1].data.size() == 2 && w[1].data[0] == 0x33 && w[1].data[1] == 0x30);
        std::printf("rate 1.0 MS/s -> ratio bytes %02X %02X %02X %02X, exact %.4f Hz\n",
                    w[0].data[0], w[0].data[1], w[1].data[0], w[1].data[1], actual);

        // THE TWO WINDOWS. The resampler cannot express 300 k - 900 k and the
        // endpoint cannot carry more than 3.2 MS/s, so both are REFUSED
        // rather than programmed and silently not delivered.
        CHECK(!Rtl2832u::rateSupported(500000));
        CHECK(!Rtl2832u::rateSupported(4000000));
        CHECK(!Rtl2832u::rateSupported(225000));
        CHECK(Rtl2832u::rateSupported(250000));
        CHECK(Rtl2832u::rateSupported(3200000));
        double refused = -1.0;
        CHECK(!rtl.setSampleRate(500000, refused));
        CHECK(refused == -1.0);  // untouched on a refusal
    }

    // =======================================================================
    // 2. THE DOWN-CONVERTER AND THE PPM TRIM.
    //
    // Oracle: rtlsdr_set_if_freq() - if_freq = -((freq * 2^22) / xtal),
    // written top six bits first to page 1 registers 0x19, 0x1a, 0x1b.
    //   3,570,000 * 4,194,304 / 28,800,000 = 519,918.24 -> 519,918
    //   negated  = -519,918 = 0xFFF81112
    //   0x19 = (x >> 16) & 0x3f = 0xF8 & 0x3f = 0x38
    //   0x1a = (x >>  8) & 0xff = 0x11
    //   0x1b =  x        & 0xff = 0x12
    // =======================================================================
    {
        FakeUsbDevice fake;
        Rtl2832u rtl(fake, 100);
        CHECK(rtl.setIfFreqHz(3570000));
        const std::vector<DemodWrite> d = demodWrites(fake.writes());
        CHECK(d.size() == 3);
        CHECK(d[0].page == 1 && d[0].addr == 0x19 && d[0].value == 0x38);
        CHECK(d[1].page == 1 && d[1].addr == 0x1a && d[1].value == 0x11);
        CHECK(d[2].page == 1 && d[2].addr == 0x1b && d[2].value == 0x12);

        // Oracle: rtlsdr_set_sample_freq_correction() -
        //   offs = -ppm * 2^24 / 1e6; 0x3f takes the low byte, 0x3e the top
        //   six bits. -10 ppm -> -167.77 -> -167 = 0xFF59, so 0x3f = 0x59 and
        //   0x3e = 0xFF & 0x3f = 0x3f.
        fake.clear();
        CHECK(rtl.setFreqCorrectionPpm(10));
        const std::vector<DemodWrite> p = demodWrites(fake.writes());
        CHECK(p.size() == 2);
        CHECK(p[0].addr == 0x3f && p[0].value == 0x59);
        CHECK(p[1].addr == 0x3e && p[1].value == 0x3f);
        // ...and the corrected crystal the tuner's PLL will use.
        CHECK(rtl.correctedXtalHz() == 28800288u);
    }

    // =======================================================================
    // 3. THE I2C REPEATER, which is the difference between a tuner write that
    //    happens and one that is acknowledged and thrown away.
    //
    // Oracle: rtlsdr_set_i2c_repeater() - demod page 1 register 0x01, 0x18 on
    // and 0x10 off. The same register is the soft reset, which is why "off"
    // is 0x10 rather than 0.
    // =======================================================================
    {
        FakeUsbDevice fake;
        Rtl2832u rtl(fake, 100);
        CHECK(rtl.setI2cRepeater(true));
        CHECK(rtl.setI2cRepeater(false));
        const std::vector<DemodWrite> d = demodWrites(fake.writes());
        CHECK(d.size() == 2);
        CHECK(d[0].page == 1 && d[0].addr == 0x01 && d[0].value == 0x18);
        CHECK(d[1].page == 1 && d[1].addr == 0x01 && d[1].value == 0x10);

        // AND THE DUMMY READ AFTER EVERY DEMOD WRITE. The chip needs the
        // extra bus cycle; without it writes are dropped at random, which
        // presents as a dongle that tunes to the wrong place once in a while.
        // Oracle: the rtlsdr_demod_read_reg(dev, 0x0a, 0x01, 1) at the foot
        // of rtlsdr_demod_write_reg().
        int dummies = 0;
        for (const FakeControl& c : fake.controls) {
            if (!c.out && c.index == 0x0a && c.value == ((0x01 << 8) | 0x20)) { ++dummies; }
        }
        CHECK(dummies == 2);
    }

    // =======================================================================
    // 4. THE BASEBAND INITIALISATION, and in particular the FIR.
    //
    // Oracle: rtlsdr_init_baseband() and rtlsdr_set_fir(). The 16 DAB/FM
    // coefficients are packed as eight signed bytes followed by eight signed
    // 12-bit values three bytes to a pair, and written to page 1 registers
    // 0x1c..0x2f. Worked through for the last pair:
    //   404 -> 0x19, then (404 << 4) | ((421 >> 8) & 0x0f) = 0x41, then
    //   421 & 0xff = 0xA5.
    // =======================================================================
    {
        FakeUsbDevice fake;
        Rtl2832u rtl(fake, 100);
        CHECK(rtl.initBaseband());
        const std::vector<DemodWrite> d = demodWrites(fake.writes());

        static const std::uint8_t kFir[20] = {0xCA, 0xDC, 0xD7, 0xD8, 0xE0, 0xF2, 0x0E,
                                              0x35, 0x06, 0x50, 0x9C, 0x0D, 0x71, 0x11,
                                              0x14, 0x71, 0x74, 0x19, 0x41, 0xA5};
        int firSeen = 0;
        for (const DemodWrite& w : d) {
            if (w.page == 1 && w.addr >= 0x1c && w.addr <= 0x2f) {
                const int i = w.addr - 0x1c;
                CHECK(w.value == kFir[i]);
                ++firSeen;
            }
        }
        std::printf("FIR: %d of 20 coefficient registers written\n", firSeen);
        CHECK(firSeen == 20);

        // The three writes that turn a television receiver into an ADC: SDR
        // mode, PID filtering off, and the demodulator's digital AGC off.
        bool sdrMode = false;
        for (const DemodWrite& w : d) {
            if (w.page == 0 && w.addr == 0x19 && w.value == 0x05) { sdrMode = true; }
        }
        CHECK(sdrMode);

        // The USB block's own setup, as BLOCK writes rather than demod ones.
        const std::vector<FakeControl> w = fake.writes();
        CHECK(isBlockWrite(w[0], 1) && w[0].value == 0x2000 && w[0].data[0] == 0x09);
        CHECK(isBlockWrite(w[1], 1) && w[1].value == 0x2158);
        CHECK(isBlockWrite(w[2], 1) && w[2].value == 0x2148 && w[2].data[0] == 0x10 &&
              w[2].data[1] == 0x02);
        // Power: the demodulator supply then the ADC and PLL block.
        CHECK(isBlockWrite(w[3], 2) && w[3].value == 0x300b && w[3].data[0] == 0x22);
        CHECK(isBlockWrite(w[4], 2) && w[4].value == 0x3000 && w[4].data[0] == 0xe8);
    }

    // =======================================================================
    // 5. OPENING A DONGLE, END TO END, THROUGH THE SHIPPING PATH.
    // =======================================================================
    FakeUsbDevice* live = nullptr;  // borrowed; the source owns the transport
    RtlSdrSource src;
    {
        auto fake = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fake);
        live = fake.get();
        CHECK(src.openWithTransport(std::move(fake), "fake R820T dongle"));
        CHECK(src.isOpen());
        CHECK(std::string(src.driverKey()) == "rtlsdr");
        CHECK(src.tunerName() == std::string("R820T"));
        std::printf("opened: %s, tuner %s, %.4f MS/s, %.4f MHz\n", src.name(),
                    src.tunerName().c_str(), src.sampleRateHz() / 1e6,
                    src.centerFrequencyHz() / 1e6);
        CHECK_NEAR(src.sampleRateHz(), 2400000.0, 1.0);
        CHECK_NEAR(src.centerFrequencyHz(), 100000000.0, 1.0);

        // The advertised range is the R82xx's own, and a tune outside it is
        // refused with a sentence rather than accepted and ignored.
        double lo = 0.0;
        double hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 24000000.0, 1.0);
        CHECK_NEAR(hi, 1766000000.0, 1.0);
        CHECK(!src.setCenterFrequencyHz(2000000000.0));
        CHECK(std::string(src.lastError()).find("outside") != std::string::npos);

        // ONE ANTENNA on a plain dongle, and it accepts its own name.
        CHECK(src.antennas().size() == 1);
        CHECK(src.antenna() == "RX");
        CHECK(src.setAntenna("RX"));
        CHECK(!src.setAntenna("HF"));

        // THE BIAS TEE IS OFF. This fake's EEPROM does not carry the
        // RTL2832U magic, so the force-on bit is not believed - which is the
        // guard that stops 4.5 V appearing on a stranger's coax because a
        // dongle had no EEPROM at all.
        CHECK(!src.biasTee());
    }

    // =======================================================================
    // 6. TUNING TO 100 MHz, BYTE BY BYTE.
    //
    // The IF is 1,815,000 Hz, not the nominal 3,570,000: the bandwidth was
    // set to the 2.4 MS/s sample rate at open, and the oracle's
    // r82xx_set_bandwidth() moves the intermediate frequency as it assembles
    // the filter from a low-pass corner and two high-pass sections -
    //   2,300,000 + 380,000 + 350,000 - (1,700,000+380,000+350,000)/2
    //   = 3,030,000 - 1,215,000 = 1,815,000.
    // So the local oscillator is 100,000,000 + 1,815,000 = 101,815,000.
    //
    // Oracle: r82xx_set_freq() -> r82xx_set_mux(), r82xx_set_vga_gain(),
    // r82xx_set_pll().
    //
    // r82xx_set_mux at 101 MHz picks the "100 MHz and up" row of the band
    // table: open_d 0x00, rf_mux_ploy 0x02, tf_c 0x34.
    //
    // r82xx_set_pll for 101,815,000 Hz against a 28.8 MHz reference:
    //   mix_div    = 32          (101,815 kHz * 32 = 3,258,080 kHz, inside
    //                             the VCO's 1,770,000 - 3,540,000 kHz window)
    //   div_num    = 4           (log2(32) - 1), unchanged because the part's
    //                             autotune report (2) equals the R820T's
    //                             reference (2), so register 0x10 bits 7:5
    //                             take 4 << 5 = 0x80
    //   vco        = 3,258,080,000 Hz; nint = vco / (2*28.8M) = 56
    //   ni, si     = (56-13)/4 = 10, 56-40-13 = 3
    //   register 0x14 = ni + (si << 6) = 10 + 192 = 0xCA
    //   vco_fra    = 32,480 kHz, so the sigma-delta is powered UP
    //                (register 0x12 bit 3 clear)
    //   sdm        = 32768 + 4096 + 64 + 16 + 8 + 2 = 36,954 = 0x905A
    //                -> register 0x16 = 0x90, register 0x15 = 0x5A
    // =======================================================================
    {
        live->clear();
        CHECK(src.setCenterFrequencyHz(100000000.0));
        const std::vector<FakeControl> w = live->writes();
        dump("tune to 100 MHz", w);

        const std::vector<Expect> want = {
            {0x17, 0x00, 0x08},  // set_mux: open drain, 100 MHz row
            {0x1a, 0x02, 0xc3},  // set_mux: tracking filter, low band
            {0x1b, 0x34, 0xff},  // set_mux: tracking-filter band code
            {0x10, 0x00, 0x0b},  // set_mux: crystal loading, high-cap 0 pF
            {0x08, 0x00, 0x3f},  // set_mux: image-rejection calibration, unused
            {0x09, 0x00, 0x3f},  // set_mux: image-rejection calibration, unused
            {0x0c, 0x08, 0x9f},  // set_vga_gain: index 8 (16.3 dB)
            {0x10, 0x00, 0x10},  // set_pll: reference divider /1
            {0x1a, 0x00, 0x0c},  // set_pll: autotune window 128 kHz
            {0x12, 0x06, 0xff},  // set_pll: VCO current at maximum
            {0x10, 0x80, 0xe0},  // set_pll: divider number 4
            {0x14, 0xCA, 0xff},  // set_pll: integer divider, ni=10 si=3
            {0x12, 0x00, 0x08},  // set_pll: sigma-delta powered up
            {0x16, 0x90, 0xff},  // set_pll: sigma-delta high byte
            {0x15, 0x5A, 0xff},  // set_pll: sigma-delta low byte
            {0x1a, 0x08, 0x08},  // set_pll: autotune narrowed after lock
        };
        expectTunerSequence("tune to 100 MHz", tunerWrites(w, 0x34), want);

        // THE REPEATER BRACKETS THE WHOLE THING, and this is the assertion
        // that catches the failure mode nothing else can: a tuner write with
        // the repeater off is acknowledged by the RTL2832U and heard by
        // nobody.
        const std::vector<DemodWrite> d = demodWrites(w);
        CHECK(d.size() >= 2);
        CHECK(d.front().page == 1 && d.front().addr == 0x01 && d.front().value == 0x18);
        CHECK(d.back().page == 1 && d.back().addr == 0x01 && d.back().value == 0x10);
        CHECK_NEAR(src.centerFrequencyHz(), 100000000.0, 1.0);
    }

    // =======================================================================
    // 7. TUNING TO 1090 MHz (the frequency this product's ADS-B users live
    //    at), which exercises a different band row and a different divider.
    //
    // LO = 1,090,000,000 + 1,815,000 = 1,091,815,000.
    //   mix_div = 2 (1,091,815 kHz * 2 = 2,183,630 kHz, in the VCO window)
    //   div_num = 0, unchanged -> register 0x10 bits 7:5 = 0x00
    //   nint    = 2,183,630,000 / 57,600,000 = 37; ni = 6, si = 0
    //   0x14    = 6
    //   vco_fra = 52,430 kHz
    //   sdm     = 32768+16384+8192+2048+256+4+2 = 59,654 = 0xE906
    //   band row: 650 MHz and up - rf_mux_ploy 0x40 (tracking filter
    //   bypassed), tf_c 0x00.
    // =======================================================================
    {
        live->clear();
        CHECK(src.setCenterFrequencyHz(1090000000.0));
        const std::vector<FakeControl> w = live->writes();
        dump("tune to 1090 MHz", w);
        const std::vector<Expect> want = {
            {0x17, 0x00, 0x08}, {0x1a, 0x40, 0xc3}, {0x1b, 0x00, 0xff},
            {0x10, 0x00, 0x0b}, {0x08, 0x00, 0x3f}, {0x09, 0x00, 0x3f},
            {0x0c, 0x08, 0x9f}, {0x10, 0x00, 0x10}, {0x1a, 0x00, 0x0c},
            {0x12, 0x06, 0xff}, {0x10, 0x00, 0xe0}, {0x14, 0x06, 0xff},
            {0x12, 0x00, 0x08}, {0x16, 0xE9, 0xff}, {0x15, 0x06, 0xff},
            {0x1a, 0x08, 0x08},
        };
        expectTunerSequence("tune to 1090 MHz", tunerWrites(w, 0x34), want);
    }

    // =======================================================================
    // 8. GAIN.
    //
    // Oracle: r82xx_set_gain() and the three measured step tables. The
    // aggregate walks the LNA and mixer up alternately:
    //   24.0 dB -> the walk passes 22.9 dB at LNA 7 and stops at 25.4 dB
    //              once the mixer reaches 7 (this is the 254 entry in
    //              librtlsdr's own gain list)
    //   49.6 dB -> the top of the ladder: LNA 15 and mixer 14. Mixer step 15
    //              is NEGATIVE, so 15/15 is QUIETER - 48.8 dB - which is why
    //              the advertised maximum is the peak of the walk and not its
    //              end.
    //    0.0 dB -> LNA 0, mixer 0, and the walk stops before its first step.
    // =======================================================================
    {
        const std::vector<cascade::source::GainInfo> g = src.gains();
        CHECK(g.size() == 4);
        CHECK(g[0].name == "TUNER");
        CHECK_NEAR(g[0].minDb, 0.0, 0.001);
        CHECK_NEAR(g[0].maxDb, 49.6, 0.001);
        CHECK(g[1].name == "LNA");
        CHECK_NEAR(g[1].maxDb, 33.5, 0.001);
        CHECK(g[2].name == "MIXER");
        CHECK_NEAR(g[2].maxDb, 16.1, 0.001);
        CHECK(g[3].name == "VGA");
        CHECK_NEAR(g[3].minDb, -4.7, 0.001);
        CHECK_NEAR(g[3].maxDb, 40.8, 0.001);

        live->clear();
        CHECK(src.setGainDb("TUNER", 24.0));
        std::vector<FakeControl> w = live->writes();
        dump("gain 24.0 dB", w);
        expectTunerSequence("gain 24.0 dB", tunerWrites(w, 0x34),
                            {
                                {0x05, 0x10, 0x10},  // LNA off its own loop
                                {0x07, 0x00, 0x10},  // mixer off its own loop
                                {0x0c, 0x08, 0x9f},  // VGA index 8
                                {0x05, 0x07, 0x0f},  // LNA index 7
                                {0x07, 0x07, 0x0f},  // mixer index 7
                            });
        CHECK_NEAR(src.gainDb("TUNER"), 25.4, 0.001);
        CHECK_NEAR(src.gainDb("LNA"), 16.6, 0.001);
        CHECK_NEAR(src.gainDb("MIXER"), 8.8, 0.001);

        live->clear();
        CHECK(src.setGainDb("TUNER", 49.6));
        w = live->writes();
        expectTunerSequence("gain 49.6 dB", tunerWrites(w, 0x34),
                            {
                                {0x05, 0x10, 0x10},
                                {0x07, 0x00, 0x10},
                                {0x0c, 0x08, 0x9f},
                                {0x05, 0x0f, 0x0f},  // LNA 15
                                {0x07, 0x0e, 0x0f},  // mixer 14, not 15
                            });
        CHECK_NEAR(src.gainDb("TUNER"), 49.6, 0.001);

        live->clear();
        CHECK(src.setGainDb("TUNER", 0.0));
        expectTunerSequence("gain 0.0 dB", tunerWrites(live->writes(), 0x34),
                            {
                                {0x05, 0x10, 0x10},
                                {0x07, 0x00, 0x10},
                                {0x0c, 0x08, 0x9f},
                                {0x05, 0x00, 0x0f},
                                {0x07, 0x00, 0x0f},
                            });
        CHECK_NEAR(src.gainDb("TUNER"), 0.0, 0.001);

        // OUT OF RANGE IS CLAMPED, not refused - the DeviceSource contract -
        // and gainDb() reports what was actually set.
        CHECK(src.setGainDb("TUNER", 500.0));
        CHECK_NEAR(src.gainDb("TUNER"), 49.6, 0.001);
        CHECK(!src.setGainDb("NOSUCH", 10.0));

        // A named stage on its own.
        live->clear();
        CHECK(src.setGainDb("VGA", 16.3));
        expectTunerSequence("VGA to 16.3 dB", tunerWrites(live->writes(), 0x34),
                            {{0x0c, 0x08, 0x9f}});
        CHECK_NEAR(src.gainDb("VGA"), 16.3, 0.001);

        // AGC on: both loops handed back to the part, the VGA parked at
        // 26.5 dB (index 11), and the demodulator's own digital AGC brought
        // in step so the two cannot fight over the same signal.
        live->clear();
        CHECK(src.setAutoGain(true));
        CHECK(src.autoGain());
        w = live->writes();
        dump("AGC on", w);
        expectTunerSequence("AGC on", tunerWrites(w, 0x34),
                            {
                                {0x05, 0x00, 0x10},  // LNA loop on
                                {0x07, 0x10, 0x10},  // mixer loop on
                                {0x0c, 0x0b, 0x9f},  // VGA parked at index 11
                            });
        bool demodAgcOn = false;
        for (const DemodWrite& dw : demodWrites(w)) {
            if (dw.page == 0 && dw.addr == 0x19 && dw.value == 0x25) { demodAgcOn = true; }
        }
        CHECK(demodAgcOn);

        live->clear();
        CHECK(src.setAutoGain(false));
        CHECK(!src.autoGain());
        w = live->writes();
        expectTunerSequence("AGC off", tunerWrites(w, 0x34),
                            {
                                {0x05, 0x10, 0x10},
                                {0x07, 0x00, 0x10},
                                {0x0c, 0x08, 0x9f},
                                {0x05, 0x0f, 0x0f},  // the manual indices restored
                                {0x07, 0x0e, 0x0f},
                            });
        bool demodAgcOff = false;
        for (const DemodWrite& dw : demodWrites(w)) {
            if (dw.page == 0 && dw.addr == 0x19 && dw.value == 0x05) { demodAgcOff = true; }
        }
        CHECK(demodAgcOff);
    }

    // =======================================================================
    // 9. A SAMPLE-RATE CHANGE IS MADE ON A QUIET STREAM.
    // =======================================================================
    {
        CHECK(src.start());
        CHECK(src.running());
        const int startsBefore = live->streamStarts;
        const int stopsBefore = live->streamStops;
        live->clear();
        CHECK(src.setSampleRateHz(1024000.0));
        CHECK_NEAR(src.sampleRateHz(), 1024000.0, 20.0);
        // The stream really was taken down and put back, not reprogrammed
        // underneath itself.
        std::printf("rate change while streaming: %d stop(s), %d start(s)\n",
                    live->streamStops - stopsBefore, live->streamStarts - startsBefore);
        CHECK(live->streamStops == stopsBefore + 1);
        CHECK(live->streamStarts == startsBefore + 1);

        // A rate nobody supports is COERCED to the nearest one, not refused:
        // a preset asking for 2 MS/s should get 2.048.
        CHECK(src.setSampleRateHz(2000000.0));
        CHECK_NEAR(src.sampleRateHz(), 2048000.0, 20.0);
        src.stop();
        CHECK(!src.running());
    }

    // =======================================================================
    // 10. SAMPLES: the byte pair to complex<float> conversion, and the
    //     reader thread that carries it.
    // =======================================================================
    {
        // A buffer whose bytes are known, delivered through the whole reader
        // path. 0x00 is the bottom of the scale, 0x80 is the middle, 0xFF the
        // top; the map is (b - 127.5) / 128, so those are -0.996, +0.004 and
        // +0.996 - inside [-1, 1) as the pipeline requires.
        std::vector<std::uint8_t> payload(1024);
        for (std::size_t i = 0; i < payload.size(); i += 4) {
            payload[i] = 0x00;
            payload[i + 1] = 0xFF;
            payload[i + 2] = 0x80;
            payload[i + 3] = 0x80;
        }
        live->bulkQueue.push_back(payload);
        CHECK(src.start());
        std::vector<std::complex<float>> got(1024);
        std::size_t total = 0;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (total < 512 && std::chrono::steady_clock::now() < until) {
            total += src.read(got.data() + total, 512 - total);
        }
        std::printf("reader delivered %zu samples; first pair (%.4f, %.4f) (%.4f, %.4f)\n",
                    total, got[0].real(), got[0].imag(), got[1].real(), got[1].imag());
        CHECK(total == 512);
        CHECK_NEAR(got[0].real(), -0.99609375, 1e-6);
        CHECK_NEAR(got[0].imag(), 0.99609375, 1e-6);
        CHECK_NEAR(got[1].real(), 0.00390625, 1e-6);
        CHECK_NEAR(got[1].imag(), 0.00390625, 1e-6);
        CHECK(!src.faulted());

        // The health line, in the same words the Soapy path writes, so one
        // log reads the same whichever way a radio was opened.
        const std::string health = src.streamHealthLine();
        std::printf("%s\n", health.c_str());
        CHECK(health.find("source: stream health - reads ") == 0);
        CHECK(health.find("512 samples in") != std::string::npos);
        src.stop();
    }

    // =======================================================================
    // 11. A DEVICE THAT GOES AWAY MID-STREAM.
    //
    // The whole point of owning the reader thread: a negative read faults the
    // source, the thread LEAVES, and stop() returns promptly rather than
    // waiting for a dead device. A driver that retried forever here is
    // exactly what made a pulled RTL-SDR look like a frozen application.
    // =======================================================================
    {
        live->failBulkAfter = 0;  // the very next read fails
        CHECK(src.start());
        const auto t0 = std::chrono::steady_clock::now();
        bool faulted = false;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
            if (src.faulted()) {
                faulted = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const double faultMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        std::printf("a lost device faulted the source after %.0f ms: %s\n", faultMs,
                    src.lastError());
        CHECK(faulted);
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");

        // AND THE THREAD ACTUALLY LEFT, which is a different claim from "the
        // fault was recorded" and the one that matters. A reader that records
        // the fault and keeps looping hammers a dead device at full speed
        // until stop() - a whole core burned to produce nothing, which is
        // exactly the shape of the frozen-spectrum reports this driver
        // exists to end. stop() still returns promptly in that state, so
        // timing alone cannot see it: the evidence has to be that NO FURTHER
        // READ WAS ATTEMPTED.
        const int readsAtFault = live->bulkReads;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::printf("reads at the fault: %d; 250 ms later: %d\n", readsAtFault,
                    live->bulkReads);
        CHECK(live->bulkReads == readsAtFault);

        const auto t1 = std::chrono::steady_clock::now();
        src.stop();
        const double stopMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1)
                .count();
        std::printf("stop() after the fault returned in %.0f ms\n", stopMs);
        CHECK(stopMs < 500.0);
        CHECK(!src.running());
        // A condemned radio refuses to start again rather than pretending.
        CHECK(!src.start());
    }

    // =======================================================================
    // 12. AN RTL-SDR BLOG V4 AT 7 MHz - the upconverter path.
    //
    // Sixteen of this product's users have a V4, and without this path a V4
    // hears NOTHING below 24 MHz.
    //
    // Oracle: r82xx_set_freq() in the rtlsdrblog fork, the is_rtlsdr_blog_v4
    // branch. The tuner is asked for 7,000,000 + 28,800,000 = 35,800,000, so
    // the LO is 35,800,000 + 1,815,000 = 37,615,000.
    //   mix_div = 64 (37,615 kHz * 64 = 2,407,360 kHz, in the VCO window)
    //   div_num = 5, then MINUS ONE: this is an R828D, whose VCO power
    //             reference is 1, and the part's autotune reports 2 - so
    //             register 0x10 bits 7:5 take 4 << 5 = 0x80
    //   nint    = 2,407,360,000 / 57,600,000 = 41; ni = 7, si = 0 -> 0x14 = 7
    //   vco_fra = 45,760 kHz
    //   sdm     = 32768+16384+2048+512+256+64+32 = 52,064 = 0xCB60
    //   band row: 0 MHz - open_d 0x08, rf_mux_ploy 0x02, tf_c 0xDF
    // then the V4's own work: the notch filters stay ON (7 MHz is outside all
    // three notch bands), the tracking filter is BYPASSED for HF, and the
    // input switch moves to cable 2 with GPIO 5 driving the upconverter.
    // =======================================================================
    {
        RtlSdrSource v4;
        auto fake = std::make_unique<FakeUsbDevice>();
        dressAsBlogV4(*fake);
        FakeUsbDevice* f = fake.get();
        CHECK(v4.openWithTransport(std::move(fake), "fake Blog V4"));
        CHECK(v4.tunerName() == std::string("R828D (RTL-SDR Blog V4)"));

        // The range that makes a V4 a V4.
        double lo = 0.0;
        double hi = 0.0;
        CHECK(v4.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 500000.0, 1.0);
        CHECK(v4.antennas().size() == 2);

        f->clear();
        CHECK(v4.setCenterFrequencyHz(7000000.0));
        const std::vector<FakeControl> w = f->writes();
        dump("V4 tune to 7 MHz", w);
        expectTunerSequence("V4 tune to 7 MHz", tunerWrites(w, 0x74),
                            {
                                {0x17, 0x08, 0x08},  // set_mux: 0 MHz row, open drain low
                                {0x1a, 0x02, 0xc3},  // set_mux: tracking filter, low band
                                {0x1b, 0xDF, 0xff},  // set_mux: band code
                                {0x10, 0x00, 0x0b},
                                {0x08, 0x00, 0x3f},
                                {0x09, 0x00, 0x3f},
                                {0x0c, 0x08, 0x9f},
                                {0x10, 0x00, 0x10},
                                {0x1a, 0x00, 0x0c},
                                {0x12, 0x06, 0xff},
                                {0x10, 0x80, 0xe0},  // divider 4 after the -1 correction
                                {0x14, 0x07, 0xff},  // ni = 7, si = 0
                                {0x12, 0x00, 0x08},
                                {0x16, 0xCB, 0xff},  // sigma-delta high byte
                                {0x15, 0x60, 0xff},  // sigma-delta low byte
                                {0x1a, 0x08, 0x08},
                                {0x17, 0x08, 0x08},  // notches ON (7 MHz is outside them)
                                {0x1a, 0x40, 0xc3},  // tracking filter BYPASSED for HF
                                {0x1b, 0x00, 0xff},  // ...and its band code cleared
                                {0x06, 0x08, 0x08},  // cable 2 in: the HF input
                                {0x05, 0x00, 0x40},  // cable 1 (VHF) out
                                {0x05, 0x20, 0x20},  // air in (UHF) out
                            });

        // THE UPCONVERTER SWITCH, which lives on the DONGLE and not on the
        // tuner: GPIO 5 driven low for the HF path. It reaches the wire as
        // system-block writes to GPD, GPOE and GPO.
        bool sawGpo = false;
        for (const FakeControl& c : w) {
            if (isBlockWrite(c, 2) && c.value == 0x3001) { sawGpo = true; }
        }
        CHECK(sawGpo);
        CHECK(v4.antenna() == "HF");

        // ...and above 28.8 MHz the same dongle reports the ordinary path.
        CHECK(v4.setCenterFrequencyHz(100000000.0));
        CHECK(v4.antenna() == "RX");
        // The notch filters come OFF inside the broadcast FM band, which is
        // the opposite of what the name suggests and is the whole point:
        // 100 MHz is inside the 85-112 MHz notch.
        f->clear();
        CHECK(v4.setCenterFrequencyHz(95000000.0));
        const std::vector<TunerWrite> nw = tunerWrites(f->writes(), 0x74);
        bool notchOpened = false;
        for (std::size_t i = 6; i < nw.size(); ++i) {
            if (nw[i].reg == 0x17 && (nw[i].value & 0x08) == 0x00) { notchOpened = true; }
        }
        CHECK(notchOpened);
        v4.closeDevice();
    }

    // =======================================================================
    // 13. AN OPEN THAT FAILS LEAVES NOTHING HALF BUILT.
    // =======================================================================
    {
        RtlSdrSource bad;
        auto fake = std::make_unique<FakeUsbDevice>();
        // Nothing dressed: no tuner answers at either address.
        CHECK(!bad.openWithTransport(std::move(fake), "a dongle with no tuner"));
        CHECK(!bad.isOpen());
        CHECK(std::string(bad.lastError()).find("tuner") != std::string::npos);
        CHECK(bad.gains().size() == 4);  // the list is static; it does not crash
        CHECK(!bad.start());

        RtlSdrSource silent;
        auto mute = std::make_unique<FakeUsbDevice>();
        mute->failControlAfter = 0;  // the very first register write fails
        CHECK(!silent.openWithTransport(std::move(mute), "a dongle that does not answer"));
        CHECK(std::string(silent.lastError()).find("did not answer") != std::string::npos);

        RtlSdrSource none;
        CHECK(!none.openWithTransport(nullptr, "nothing at all"));
    }

    return testSummary("test_rtlsdr_source");
}
