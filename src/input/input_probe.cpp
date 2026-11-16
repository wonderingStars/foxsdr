// input_probe - lists the MIDI input ports and the supported HID knobs this PC has, using the same code the application will use. Prints names and ids of MIDI
// ports and of the three supported products only; other HID devices are counted, never described, and no serial number is read.
//   input_probe [--listen SECONDS]    with --listen it opens every port / knob found and prints the events for that long.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "input/hid_win.hpp"
#include "input/midi_win.hpp"

using namespace cascade::input;

int main(int argc, char** argv) {
    int listen = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--listen") == 0) { listen = std::atoi(argv[i + 1]); }
    }
    const auto midi = enumerateMidiInputs();
    std::printf("MIDI input ports: %zu\n", midi.size());
    for (const auto& p : midi) { std::printf("  [%u] \"%s\"  id=%s  mfr=0x%04x prod=0x%04x\n", p.index, p.name.c_str(), p.stableId.c_str(), p.manufacturer, p.product); }

    const HidEnumeration hid = enumerateHid();
    std::printf("HID: supported-product interfaces: %zu, other HID interfaces (not described): %d, unreadable: %d\n", hid.supported.size(), hid.otherDevices, hid.unreadable);
    for (const auto& d : hid.supported) {
        std::printf("  %s  id=%s  %04x:%04x  usagePage=0x%04x usage=0x%04x inputReportBytes=%u plausible=%d\n", d.name.c_str(), d.stableId.c_str(), d.vendor, d.product,
                    d.usagePage, d.usage, d.inputReportBytes, d.plausible ? 1 : 0);
    }
    if (listen > 0) {
        EventQueue q(4096);
        std::vector<std::unique_ptr<MidiInputPort>> ports;
        std::vector<std::unique_ptr<HidInputDevice>> devs;
        for (const auto& p : midi) {
            auto port = std::make_unique<MidiInputPort>();
            std::string err;
            if (port->open(p, q, err)) { ports.push_back(std::move(port)); } else { std::printf("  open %s: %s\n", p.stableId.c_str(), err.c_str()); }
        }
        for (const auto& d : hid.supported) {
            if (!d.plausible) { continue; }
            auto dev = std::make_unique<HidInputDevice>();
            std::string err;
            if (dev->open(d, q, err)) { devs.push_back(std::move(dev)); } else { std::printf("  open %s: %s\n", d.stableId.c_str(), err.c_str()); }
        }
        std::printf("listening %d s on %zu MIDI port(s) and %zu knob(s)\n", listen, ports.size(), devs.size());
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(listen);
        while (std::chrono::steady_clock::now() < end) {
            std::vector<InputEvent> ev;
            q.drain(ev);
            for (const auto& e : ev) { std::printf("  dev=%08x ctl=0x%04x kind=%d raw=%d value=%.3f\n", e.device, e.control, static_cast<int>(e.kind), e.raw, e.value); }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        for (auto& p : ports) { p->close(); }
        for (auto& d : devs) { d->close(); }
        std::printf("dropped=%llu\n", static_cast<unsigned long long>(q.dropped()));
    }
    return 0;
}
