// THE BIAS TEE MEMORY AND AN RX888 THAT WAS OPENED THROUGH ITS BOOTLOADER.
//
// Found by the second review of the deck's bias tee key (2026-09-25). An RX888
// out of a power cycle is a Cypress bootloader; Rx888Source::open uploads the
// firmware to it and then opens the FIRST running RX888 it finds - it cannot
// match the radio across the upload, because the bootloader's serial is
// Cypress's and the running firmware's is the SDDC image's. So the args the
// application saves (the bootloader's serial) do not name the radio that
// ended up open. With the per-radio memory keyed by those args, radio A's
// remembered "on" was applied to radio B - another RX888 already running -
// which nobody switched on (the reviewer's probe, promoted here).
//
// THE RULE: an RX888 opened through a firmware upload is not IDENTIFIED by its
// args. An "on" is never kept for it and a remembered "on" is never applied to
// it; an "off" may still be kept. And a serial that is all zeros (the
// bootloader's own, "0000000000000000") names nothing either.
//
// Everything below the panel functions is the shipping Rx888Source on the
// FakeRx888Bus, which re-enumerates when the bootloader is jumped exactly as
// the hardware does; the evidence is the GPIO word each fake radio was sent.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "core/bias_tee_memory.hpp"
#include "gui/bias_tee.hpp"
#include "rx888_fake_usb.hpp"
#include "source/rx888_protocol.hpp"
#include "source/rx888_source.hpp"
#include "test_check.hpp"

using cascade::gui::BiasTeePanel;
using cascade::gui::biasTeeAfterOpen;
using cascade::gui::biasTeeRecalled;
using cascade::gui::biasTeeTicked;
using cascade::source::Rx888Source;
using cascade::test::FakeRx888Bus;
using cascade::test::FakeRx888Usb;
namespace rx888 = cascade::source::rx888;

namespace {

// Whether this fake radio was ever sent a GPIO word with the HF bias tee on.
bool gotBiasOn(const FakeRx888Usb& d) {
    for (const auto& c : d.controls()) {
        if (!c.in && c.request == static_cast<std::uint8_t>(rx888::Command::GpioFx3) &&
            c.data.size() >= 2) {
            std::uint32_t w = c.data[0] | (static_cast<std::uint32_t>(c.data[1]) << 8);
            if (c.data.size() > 2) { w |= static_cast<std::uint32_t>(c.data[2]) << 16; }
            if ((w & rx888::kGpioBiasHf) != 0) { return true; }
        }
    }
    return false;
}

void attach(Rx888Source& src, FakeRx888Bus& bus) {
    src.setTransportForTest([&bus] { return bus.list(); },
                            [&bus](const std::string& path, std::string& error) {
                                return bus.open(path, error);
                            });
}

// A bootloader serial that is NOT all zeros, so the upload rule is what is
// tested here and not the all-zeros one.
const std::string kBootSerial = "B00710ADE7000001";
const std::string kBootArgs = "serial=" + kBootSerial;

}  // namespace

int main() {
    std::printf("test_bias_tee_rx888\n");

    // =======================================================================
    // 1. A's REMEMBERED "ON" DOES NOT POWER RADIO B.
    //
    // B is already running its firmware; A has just been plugged in. The
    // memory holds an "on" under A's (bootloader) args. The open uploads to
    // A's bootloader and ends up on B.
    // =======================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDCBBBBBBBBBBBB");
        bus.addBootloader(kBootSerial, "SDDCAAAAAAAAAAAA");
        const std::shared_ptr<FakeRx888Usb> b = bus.deviceFor(FakeRx888Bus::kPidStreamer);
        Rx888Source src;
        attach(src, bus);
        BiasTeePanel panel;
        panel.remembered[cascade::core::biasTeeRadioKey("rx888", kBootArgs)] = true;
        const bool ok = src.open(kBootArgs);
        CHECK(ok);
        CHECK(src.firmwareWasUploaded());
        if (ok) {
            biasTeeAfterOpen(panel, src, kBootArgs);
            std::printf("[1] upload open with A's on remembered: shown %d, B's HF bias ON sent %d\n",
                        panel.shown, gotBiasOn(*b));
            CHECK(!gotBiasOn(*b));
            CHECK(!panel.shown);
            CHECK(!src.biasT());
            CHECK(!cascade::gui::biasTeeWillRestoreOn(panel, src, kBootArgs));
            src.closeDevice();
        }
    }

    // =======================================================================
    // 2. AN "ON" SWITCHED AFTER AN UPLOAD OPEN IS NOT REMEMBERED; AN "OFF" IS.
    // =======================================================================
    {
        FakeRx888Bus bus;
        bus.addBootloader(kBootSerial, "SDDCAAAAAAAAAAAA");
        Rx888Source src;
        attach(src, bus);
        BiasTeePanel panel;
        CHECK(src.open(kBootArgs));
        CHECK(src.firmwareWasUploaded());
        biasTeeAfterOpen(panel, src, kBootArgs);
        std::string err;
        biasTeeTicked(panel, &src, kBootArgs, true, &err);
        std::printf("[2] switched on after an upload open: shown %d, remembered %d\n",
                    panel.shown, biasTeeRecalled(panel, "rx888", kBootArgs));
        CHECK(err.empty());
        CHECK(panel.shown);  // it IS on now - the switch itself works
        CHECK(biasTeeRecalled(panel, "rx888", kBootArgs) == -1);
        biasTeeTicked(panel, &src, kBootArgs, false, &err);
        CHECK(!panel.shown);
        CHECK(biasTeeRecalled(panel, "rx888", kBootArgs) == 0);
        src.closeDevice();
    }

    // =======================================================================
    // 3. THE CONTROL: AN RX888 OPENED DIRECTLY (no upload) BY ITS OWN SERIAL
    //    IS REMEMBERED AND COMES BACK ON.
    // =======================================================================
    {
        const std::string args = "serial=SDDCAAAAAAAAAAAA";
        BiasTeePanel panel;
        {
            FakeRx888Bus bus;
            bus.addStreamer("SDDCAAAAAAAAAAAA");
            Rx888Source src;
            attach(src, bus);
            CHECK(src.open(args));
            CHECK(!src.firmwareWasUploaded());
            biasTeeAfterOpen(panel, src, args);
            CHECK(cascade::gui::biasTeeWillRestoreOn(panel, src, args));
            biasTeeTicked(panel, &src, args, true, nullptr);
            CHECK(biasTeeRecalled(panel, "rx888", args) == 1);
            src.closeDevice();
        }
        FakeRx888Bus bus;
        bus.addStreamer("SDDCAAAAAAAAAAAA");
        const std::shared_ptr<FakeRx888Usb> a = bus.deviceFor(FakeRx888Bus::kPidStreamer);
        Rx888Source src;
        attach(src, bus);
        CHECK(src.open(args));
        biasTeeAfterOpen(panel, src, args);
        std::printf("[3] direct open, remembered on: shown %d, HF bias ON sent %d\n", panel.shown,
                    gotBiasOn(*a));
        CHECK(panel.shown);
        CHECK(gotBiasOn(*a));
        src.closeDevice();
    }

    // =======================================================================
    // 4. AN ALL-ZEROS SERIAL NAMES NOTHING (the bootloader's own, and what a
    //    board with no serial programmed reports): no "on" kept or applied.
    // =======================================================================
    {
        const std::string args = "serial=0000000000000000";
        FakeRx888Bus bus;
        bus.addStreamer("0000000000000000");
        const std::shared_ptr<FakeRx888Usb> a = bus.deviceFor(FakeRx888Bus::kPidStreamer);
        Rx888Source src;
        attach(src, bus);
        BiasTeePanel panel;
        panel.remembered[cascade::core::biasTeeRadioKey("rx888", args)] = true;
        CHECK(src.open(args));
        CHECK(!src.firmwareWasUploaded());
        biasTeeAfterOpen(panel, src, args);
        std::printf("[4] all-zeros serial, on remembered: shown %d, HF bias ON sent %d\n",
                    panel.shown, gotBiasOn(*a));
        CHECK(!panel.shown);
        CHECK(!gotBiasOn(*a));
        panel.remembered.clear();
        CHECK(!cascade::gui::biasTeeWillRestoreOn(panel, src, args));
        biasTeeTicked(panel, &src, args, true, nullptr);
        CHECK(panel.shown);
        CHECK(biasTeeRecalled(panel, "rx888", args) == -1);
        src.closeDevice();
    }

    return testSummary("test_bias_tee_rx888");
}
