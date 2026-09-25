// THE RTL-SDR's BIAS TEE, FROM THE SOURCE PANEL'S CHECKBOX TO THE GPIO PIN.
//
// A user asked how to turn the bias tee on, and on an RTL-SDR the honest
// answer was "you cannot": the native driver had the switch but the Source
// panel's checkbox did not reach it. This file proves the path now exists AND
// that it keeps the rules that make putting 4.5 V on a connector safe:
//
//   off by default, switched on only by a tick, remembered per DONGLE, put
//   back after open only on that same dongle, never put back on a dongle with
//   no EEPROM, and the box always showing the driver's readback.
//
// Everything below the panel functions is the shipping driver, opened against
// usb::FakeUsbDevice; the evidence is the GPIO register the fake was told to
// write, modelled as a real register so a read-modify-write that clobbers
// another bit shows up as the wrong byte.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <map>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gui/bias_tee.hpp"
#include "source/rtl2832u.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

using cascade::gui::BiasTeePanel;
using cascade::gui::biasTeeAfterOpen;
using cascade::gui::biasTeeTicked;
using cascade::gui::withBiasTee;
using cascade::source::RtlSdrSource;
using cascade::usb::FakeControl;
using cascade::usb::FakeUsbDevice;

namespace {

// --- the dongle ----------------------------------------------------------------

// The R82xx's answer to the probe read (see test_rtlsdr_source.cpp for what
// each byte means): identity 0x69, PLL locked, a calibration code.
const std::vector<std::uint8_t> kTunerAnswer = {0x69, 0x00, 0x02, 0x00, 0xA4};

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

enum class Eeprom {
    None,       // no EEPROM fitted: every byte reads 0x00
    Plain,      // header 0x28 0x32, byte 7 bit 1 SET: bias tee not forced
    ForcedOn,   // header 0x28 0x32, byte 7 bit 1 CLEAR: the maker wired it on
};

// The system block (2) as the RTL2832U addresses it: writes go to wIndex
// 0x0210, reads come from 0x0200. GPO, GPOE and GPD are the three registers a
// GPIO pin is driven through.
constexpr std::uint16_t kSysWrite = 0x0210;
constexpr std::uint16_t kSysRead = 0x0200;
constexpr std::uint16_t kGpo = 0x3001;
constexpr std::uint16_t kGpoe = 0x3003;
constexpr std::uint16_t kGpd = 0x3004;

bool isSysWrite(const FakeControl& c, std::uint16_t reg) {
    return c.out && c.request == 0 && c.index == kSysWrite && c.value == reg && !c.data.empty();
}

// A dongle: an R820T (or a Blog V4's R828D), an EEPROM of the given kind, and
// the three GPIO registers remembered as a real chip remembers them.
std::unique_ptr<FakeUsbDevice> makeDongle(Eeprom eeprom, bool blogV4 = false) {
    auto f = std::make_unique<FakeUsbDevice>();
    if (blogV4) {
        f->answerIn(0, 0x0074, 0x0600, kTunerAnswer);
        f->answerIn(0x06, 0x0100, 0x0000,
                    {0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0xDA, 0x0B, 0x38, 0x28,
                     0x00, 0x01, 1, 5, 0x03, 0x01});
        f->answerIn(0x06, 0x0300, 0x0000, {0x04, 0x03, 0x09, 0x04});
        f->answerIn(0x06, 0x0301, 0x0409, stringDescriptor("RTLSDRBlog"));
        f->answerIn(0x06, 0x0305, 0x0409, stringDescriptor("Blog V4"));
    } else {
        f->answerIn(0, 0x0034, 0x0600, kTunerAnswer);
    }
    // THE EEPROM: eight single-byte reads from slave 0xa0 on the I2C block,
    // which the chip answers from an auto-incrementing pointer - so each read
    // gets the next byte. No EEPROM at all reads as zeroes.
    f->answerIn(0, 0x00a0, 0x0600, {0x00});
    if (eeprom != Eeprom::None) {
        const std::uint8_t byte7 = (eeprom == Eeprom::ForcedOn) ? 0x00 : 0x02;
        for (const std::uint8_t b : {std::uint8_t{0x28}, std::uint8_t{0x32}, std::uint8_t{0x00},
                                     std::uint8_t{0x00}, std::uint8_t{0x00}, std::uint8_t{0x00},
                                     std::uint8_t{0x00}, byte7}) {
            f->queueIn(0, 0x00a0, 0x0600, {b});
        }
    }
    // THE GPIO REGISTERS, REMEMBERED. Every accepted write to GPO/GPOE/GPD
    // becomes what the next read of that register answers.
    for (const std::uint16_t reg : {kGpo, kGpoe, kGpd}) {
        f->answerIn(0, reg, kSysRead, {0x00});
    }
    FakeUsbDevice* self = f.get();
    f->onControlOut = [self](const FakeControl& c) {
        for (const std::uint16_t reg : {kGpo, kGpoe, kGpd}) {
            if (isSysWrite(c, reg)) { self->answerIn(0, reg, kSysRead, {c.data[0]}); }
        }
    };
    return f;
}

// What the register holds NOW, per the model.
std::uint8_t reg(const FakeUsbDevice& f, std::uint16_t r) {
    const auto it = f.inAnswers.find(FakeUsbDevice::InKey{0, r, kSysRead});
    return (it == f.inAnswers.end() || it->second.empty()) ? 0 : it->second[0];
}
bool gpio0High(const FakeUsbDevice& f) { return (reg(f, kGpo) & 0x01) != 0; }
bool gpio5High(const FakeUsbDevice& f) { return (reg(f, kGpo) & 0x20) != 0; }

// Every GPO byte written since the last clear().
std::vector<std::uint8_t> gpoWrites(const FakeUsbDevice& f) {
    std::vector<std::uint8_t> out;
    for (const FakeControl& c : f.controls) {
        if (isSysWrite(c, kGpo)) { out.push_back(c.data[0]); }
    }
    return out;
}

bool anyGpoWriteWithBit0(const FakeUsbDevice& f) {
    for (const std::uint8_t b : gpoWrites(f)) {
        if ((b & 0x01) != 0) { return true; }
    }
    return false;
}

// THE PER-RADIO MEMORY (repair round 1 of the deck key): what is remembered
// for the dongle `args` names - 1 on, 0 off, -1 nothing - and a memory set
// by hand, as a saved config would carry it.
int recalled(const BiasTeePanel& p, const std::string& args) {
    return cascade::gui::biasTeeRecalled(p, "rtlsdr", args);
}
void seed(BiasTeePanel& p, const std::string& kind, const std::string& args, bool on) {
    p.remembered[cascade::core::biasTeeRadioKey(kind, args)] = on;
}

// Opens a dongle through the shipping path and runs the panel's after-open
// step on it, as AppWindow::adoptDeviceMirrors does.
FakeUsbDevice* openDongle(RtlSdrSource& src, BiasTeePanel& panel, Eeprom eeprom,
                          const std::string& args, bool blogV4 = false) {
    auto fake = makeDongle(eeprom, blogV4);
    FakeUsbDevice* f = fake.get();
    CHECK(src.openWithTransport(std::move(fake), "fake dongle " + args));
    biasTeeAfterOpen(panel, src, args);
    return f;
}

}  // namespace

int main() {
    // =======================================================================
    // 1. THE CHECKBOX APPEARS FOR AN RTL-SDR.
    //
    // The panel draws the box only for a radio withBiasTee dispatches to. An
    // RTL-SDR was deliberately left out of that dispatch until the open-time
    // policy below existed, which is why a dongle owner could not reach the
    // switch at all.
    // =======================================================================
    {
        RtlSdrSource src;
        BiasTeePanel panel;
        openDongle(src, panel, Eeprom::None, "serial=00000001");
        const bool dispatched = withBiasTee(&src, [](auto&) { return true; });
        std::printf("[1] withBiasTee reaches an RtlSdrSource: %s; panel.present %s\n",
                    dispatched ? "yes" : "no", panel.present ? "yes" : "no");
        CHECK(dispatched);
        CHECK(panel.present);
        // ...and it still does NOT reach a closed or absent source.
        CHECK(!withBiasTee(nullptr, [](auto&) { return true; }));
        src.closeDevice();
    }

    // =======================================================================
    // 2. A TICK DRIVES GPIO 0; AN UNTICK CLEARS IT.
    //
    // Oracle: rtlsdr_set_bias_tee_gpio() in librtlsdr.c - GPIO 0 made an
    // output (GPD bit clear, GPOE bit set), then GPO bit 0 set for on and
    // cleared for off.
    // =======================================================================
    {
        RtlSdrSource src;
        BiasTeePanel panel;
        FakeUsbDevice* f = openDongle(src, panel, Eeprom::Plain, "serial=00000001");
        CHECK(!panel.shown);
        CHECK(!src.biasT());
        CHECK(!gpio0High(*f));

        f->clear();
        std::string err;
        biasTeeTicked(panel, &src, "serial=00000001", true, &err);
        std::printf("[2] tick: GPO %02X, GPOE %02X, GPD %02X, shown %d, error \"%s\"\n",
                    reg(*f, kGpo), reg(*f, kGpoe), reg(*f, kGpd), panel.shown, err.c_str());
        CHECK(err.empty());
        CHECK(panel.shown);
        CHECK(src.biasT());
        CHECK(gpio0High(*f));
        CHECK((reg(*f, kGpoe) & 0x01) != 0);  // GPIO 0 is an output...
        CHECK((reg(*f, kGpd) & 0x01) == 0);   // ...and not disabled
        CHECK(anyGpoWriteWithBit0(*f));
        // THE MEMORY names this dongle and says on.
        CHECK(recalled(panel, "serial=00000001") == 1);

        f->clear();
        biasTeeTicked(panel, &src, "serial=00000001", false, &err);
        std::printf("[2] untick: GPO %02X, shown %d\n", reg(*f, kGpo), panel.shown);
        CHECK(!panel.shown);
        CHECK(!src.biasT());
        CHECK(!gpio0High(*f));
        CHECK(!gpoWrites(*f).empty());
        CHECK(recalled(panel, "serial=00000001") == 0);
        src.closeDevice();
    }

    // =======================================================================
    // 3. THE BOX IS THE READBACK: A REFUSED WRITE LEAVES IT WHERE IT WAS.
    // =======================================================================
    {
        RtlSdrSource src;
        BiasTeePanel panel;
        FakeUsbDevice* f = openDongle(src, panel, Eeprom::Plain, "serial=00000001");

        // Off, and the tick is refused by the dongle: the box stays unticked,
        // the memory is not changed, and the GPIO pin never went high.
        f->failControlAfter = f->controlCalls;
        std::string err;
        biasTeeTicked(panel, &src, "serial=00000001", true, &err);
        std::printf("[3] refused tick: shown %d, biasT %d, GPO %02X, error \"%s\"\n",
                    panel.shown, src.biasT(), reg(*f, kGpo), err.c_str());
        CHECK(!panel.shown);
        CHECK(!src.biasT());
        CHECK(!gpio0High(*f));
        CHECK(!err.empty());
        CHECK(panel.remembered.empty());

        // On, and the UNtick is refused: the box stays ticked, because the
        // power is still on the port.
        RtlSdrSource src2;
        BiasTeePanel panel2;
        FakeUsbDevice* f2 = openDongle(src2, panel2, Eeprom::Plain, "serial=00000002");
        biasTeeTicked(panel2, &src2, "serial=00000002", true, nullptr);
        CHECK(panel2.shown);
        f2->failControlAfter = f2->controlCalls;
        err.clear();
        biasTeeTicked(panel2, &src2, "serial=00000002", false, &err);
        std::printf("[3] refused untick: shown %d, biasT %d, GPO %02X\n", panel2.shown,
                    src2.biasT(), reg(*f2, kGpo));
        CHECK(panel2.shown);
        CHECK(src2.biasT());
        CHECK(gpio0High(*f2));
        CHECK(!err.empty());
        CHECK(recalled(panel2, "serial=00000002") == 1);  // what the port is doing
        src.closeDevice();
        src2.closeDevice();
    }

    // =======================================================================
    // 4. A DONGLE WITH NO EEPROM NEVER GETS IT BACK AT OPEN.
    //
    // Whatever is remembered - a DIFFERENT dongle saved "on", the SAME serial
    // saved "on", or another radio's bias tee left on - an open of a dongle
    // whose EEPROM reads as zeroes leaves GPIO 0 low. It is the cheapest
    // dongle, the least likely to survive 4.5 V, and without an EEPROM it has
    // no identity of its own to be "the same dongle" by.
    // =======================================================================
    {
        struct Case {
            const char* what;
            std::string savedArgs;
            bool savedOn;
            bool otherOn;
        };
        const Case cases[] = {
            {"a different dongle saved on", "serial=BBBBBBBB", true, false},
            {"the same serial saved on", "serial=00000001", true, false},
            {"another radio's bias tee left on", "", false, true},
        };
        for (const Case& k : cases) {
            RtlSdrSource src;
            BiasTeePanel panel;
            if (!k.savedArgs.empty()) { seed(panel, "rtlsdr", k.savedArgs, k.savedOn); }
            if (k.otherOn) { seed(panel, "hackrf", "serial=0000000000000000457863c8", true); }
            const std::map<std::string, bool> before = panel.remembered;
            auto fake = makeDongle(Eeprom::None);
            FakeUsbDevice* f = fake.get();
            CHECK(src.openWithTransport(std::move(fake), "fake EEPROM-less dongle"));
            biasTeeAfterOpen(panel, src, "serial=00000001");
            std::printf("[4] no EEPROM, %s: shown %d, biasT %d, any GPO bit0 write %d\n", k.what,
                        panel.shown, src.biasT(), anyGpoWriteWithBit0(*f));
            CHECK(!src.eepromValid());
            CHECK(panel.present);
            CHECK(!panel.shown);
            CHECK(!src.biasT());
            CHECK(!gpio0High(*f));
            CHECK(!anyGpoWriteWithBit0(*f));
            // The memory is not rewritten by an open: it is the user's choice,
            // and only a tick changes it.
            CHECK(panel.remembered == before);

            // ...and the user CAN still tick it on in this session.
            biasTeeTicked(panel, &src, "serial=00000001", true, nullptr);
            CHECK(panel.shown);
            CHECK(gpio0High(*f));
            src.closeDevice();
        }
    }

    // =======================================================================
    // 5. THE SAME SAVED DONGLE GETS ITS "ON" BACK; NOTHING ELSE DOES.
    // =======================================================================
    {
        // Same serial, EEPROM present: re-applied.
        {
            RtlSdrSource src;
            BiasTeePanel panel;
            seed(panel, "rtlsdr", "serial=00000001", true);
            FakeUsbDevice* f = openDongle(src, panel, Eeprom::Plain, "serial=00000001");
            std::printf("[5] same dongle saved on: shown %d, GPO %02X\n", panel.shown,
                        reg(*f, kGpo));
            CHECK(panel.shown);
            CHECK(src.biasT());
            CHECK(gpio0High(*f));
            src.closeDevice();
        }
        // A different serial, EEPROM present: left off.
        {
            RtlSdrSource src;
            BiasTeePanel panel;
            seed(panel, "rtlsdr", "serial=BBBBBBBB", true);
            FakeUsbDevice* f = openDongle(src, panel, Eeprom::Plain, "serial=00000001");
            std::printf("[5] different dongle saved on: shown %d, GPO %02X\n", panel.shown,
                        reg(*f, kGpo));
            CHECK(!panel.shown);
            CHECK(!gpio0High(*f));
            CHECK(!anyGpoWriteWithBit0(*f));
            src.closeDevice();
        }
        // "index=0" is a place in the USB list, not a dongle: whatever is
        // plugged in first answers to it. Never re-applied as "on".
        {
            RtlSdrSource src;
            BiasTeePanel panel;
            seed(panel, "rtlsdr", "index=0", true);  // by hand: no rule writes this
            FakeUsbDevice* f = openDongle(src, panel, Eeprom::Plain, "index=0");
            std::printf("[5] index=0 saved on: shown %d\n", panel.shown);
            CHECK(!panel.shown);
            CHECK(!gpio0High(*f));
            src.closeDevice();
        }
        // Another radio's "on" (the HackRF on the same bench) does not leak
        // onto a dongle...
        {
            RtlSdrSource src;
            BiasTeePanel panel;
            seed(panel, "hackrf", "serial=0000000000000000457863c8", true);
            FakeUsbDevice* f = openDongle(src, panel, Eeprom::Plain, "serial=00000001");
            std::printf("[5] another radio saved on: shown %d\n", panel.shown);
            CHECK(!panel.shown);
            CHECK(!gpio0High(*f));
            // ...and a whole session of ticking the dongle leaves that other
            // radio's setting exactly as it was.
            biasTeeTicked(panel, &src, "serial=00000001", true, nullptr);
            biasTeeTicked(panel, &src, "serial=00000001", false, nullptr);
            CHECK(panel.remembered.at(cascade::core::biasTeeRadioKey(
                "hackrf", "serial=0000000000000000457863c8")));
            src.closeDevice();
        }
    }

    // =======================================================================
    // 6. A DONGLE WHOSE EEPROM FORCES IT ON OPENS TICKED, AND UNTICKING
    //    WORKS - AND IS REMEMBERED FOR THAT DONGLE.
    // =======================================================================
    {
        RtlSdrSource src;
        BiasTeePanel panel;
        FakeUsbDevice* f = openDongle(src, panel, Eeprom::ForcedOn, "serial=00000007");
        std::printf("[6] forced dongle opened: shown %d, forced %d, GPO %02X\n", panel.shown,
                    src.biasTForcedByEeprom(), reg(*f, kGpo));
        CHECK(src.eepromValid());
        CHECK(src.biasTForcedByEeprom());
        CHECK(panel.shown);
        CHECK(src.biasT());
        CHECK(gpio0High(*f));

        biasTeeTicked(panel, &src, "serial=00000007", false, nullptr);
        std::printf("[6] unticked: shown %d, GPO %02X\n", panel.shown, reg(*f, kGpo));
        CHECK(!panel.shown);
        CHECK(!src.biasT());
        CHECK(!gpio0High(*f));
        CHECK(recalled(panel, "serial=00000007") == 0);
        src.closeDevice();

        // The next open of THAT dongle honours the untick.
        RtlSdrSource again;
        FakeUsbDevice* f2 = openDongle(again, panel, Eeprom::ForcedOn, "serial=00000007");
        std::printf("[6] reopened with the untick remembered: shown %d, GPO %02X\n",
                    panel.shown, reg(*f2, kGpo));
        CHECK(!panel.shown);
        CHECK(!again.biasT());
        CHECK(!gpio0High(*f2));
        again.closeDevice();

        // A DIFFERENT forced dongle is not governed by that memory: it opens
        // as its maker wired it.
        RtlSdrSource other;
        FakeUsbDevice* f3 = openDongle(other, panel, Eeprom::ForcedOn, "serial=00000008");
        CHECK(panel.shown);
        CHECK(gpio0High(*f3));
        other.closeDevice();
    }

    // =======================================================================
    // 7. THE BIAS TEE AND THE BLOG V4's UPCONVERTER SWITCH SHARE ONE REGISTER
    //    AND MUST NOT FIGHT.
    //
    // GPIO 0 is the bias tee; GPIO 5 is the V4's HF upconverter switch, which
    // the tuner code throws on every band change (tuner_r82xx.cpp). Both are
    // bits of the same GPO byte, and both are written read-modify-write under
    // the one device lock - so each must leave the other's bit exactly as it
    // found it. The register model is what makes a clobber visible here.
    // =======================================================================
    {
        RtlSdrSource src;
        BiasTeePanel panel;
        FakeUsbDevice* f =
            openDongle(src, panel, Eeprom::Plain, "serial=0000V4V4", /*blogV4=*/true);
        CHECK(src.tunerName() == std::string("R828D (RTL-SDR Blog V4)"));
        biasTeeTicked(panel, &src, "serial=0000V4V4", true, nullptr);
        CHECK(gpio0High(*f));

        // HF: the upconverter path, GPIO 5 LOW. The bias tee stays on through
        // every GPO write the retune made.
        f->clear();
        CHECK(src.setCenterFrequencyHz(7000000.0));
        std::vector<std::uint8_t> w = gpoWrites(*f);
        bool bit0Kept = !w.empty();
        for (const std::uint8_t b : w) { bit0Kept = bit0Kept && (b & 0x01) != 0; }
        std::printf("[7] V4 to 7 MHz with the bias tee on: %zu GPO write(s), GPO %02X\n",
                    w.size(), reg(*f, kGpo));
        CHECK(bit0Kept);
        CHECK(!gpio5High(*f));
        CHECK(gpio0High(*f));

        // VHF: GPIO 5 HIGH, bias tee still on.
        f->clear();
        CHECK(src.setCenterFrequencyHz(100000000.0));
        std::printf("[7] V4 to 100 MHz: GPO %02X\n", reg(*f, kGpo));
        CHECK(gpio5High(*f));
        CHECK(gpio0High(*f));
        CHECK((reg(*f, kGpoe) & 0x21) == 0x21);  // both pins outputs

        // Unticking leaves GPIO 5 where the tuner put it...
        biasTeeTicked(panel, &src, "serial=0000V4V4", false, nullptr);
        std::printf("[7] bias tee off at 100 MHz: GPO %02X\n", reg(*f, kGpo));
        CHECK(!gpio0High(*f));
        CHECK(gpio5High(*f));

        // ...and a band change afterwards does not switch the bias tee back on.
        f->clear();
        CHECK(src.setCenterFrequencyHz(7000000.0));
        CHECK(!gpio5High(*f));
        CHECK(!gpio0High(*f));
        CHECK(!anyGpoWriteWithBit0(*f));
        src.closeDevice();
    }

    return testSummary("test_rtl_bias_tee");
}
