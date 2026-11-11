// The Windows device side: enumeration with the real APIs (whatever this PC has), opening something that is not there, and the overlapped HID reader proved
// against a REAL overlapped handle - a named pipe in message mode, so each write is one "report" - because no knob is attached to this PC: reports flow to the
// parsers and the queue, the device being unplugged is seen, and closing while a read is pending, or the device is silent, ends within a bound.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifdef _WIN32

#include <windows.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "input/hid_win.hpp"
#include "input/midi_win.hpp"
#include "test_check.hpp"

using namespace cascade::input;
using Clock = std::chrono::steady_clock;

static long long msSince(Clock::time_point t0) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count(); }

struct Pipe {
    HANDLE server = INVALID_HANDLE_VALUE;  // overlapped, the reader's end
    HANDLE client = INVALID_HANDLE_VALUE;  // the "device": writes reports
    bool ok() const { return server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE; }
};

static Pipe makePipe() {
    static int n = 0;
    const std::wstring name = L"\\\\.\\pipe\\knob-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n);
    Pipe p;
    p.server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 0, 4096, 0, nullptr);
    if (p.server == INVALID_HANDLE_VALUE) { return p; }
    p.client = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (p.client == INVALID_HANDLE_VALUE) { return p; }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ConnectNamedPipe(p.server, &ov)) {
        const DWORD e = GetLastError();
        if (e == ERROR_IO_PENDING) { WaitForSingleObject(ov.hEvent, 2000); }
    }
    CloseHandle(ov.hEvent);
    return p;
}

static bool send(const Pipe& p, std::initializer_list<int> bytes) {
    std::vector<std::uint8_t> v;
    for (int b : bytes) { v.push_back(static_cast<std::uint8_t>(b)); }
    DWORD w = 0;
    return WriteFile(p.client, v.data(), static_cast<DWORD>(v.size()), &w, nullptr) && w == v.size();
}

template <class F>
static bool waitFor(F f, int ms = 3000) {
    const auto t0 = Clock::now();
    while (msSince(t0) < ms) {
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return f();
}

int main() {
    // ---- ENUMERATION with the real APIs: no crash, sane shapes (the count is whatever this PC has) ------------------------------------------------
    {
        const auto ports = enumerateMidiInputs();
        std::set<std::string> ids;
        bool shaped = true;
        for (const auto& p : ports) {
            if (p.stableId.rfind("midi:", 0) != 0) { shaped = false; }
            ids.insert(p.stableId);
        }
        CHECK(shaped);
        CHECK(ids.size() == ports.size());  // stable ids are unique even for two ports of one name
        std::printf("note: %zu MIDI input port(s) on this PC\n", ports.size());
        CHECK(!midiPortPresent("midi:no such port, not here"));
        for (const auto& p : ports) { CHECK(midiPortPresent(p.stableId)); }

        const HidEnumeration h = enumerateHid();
        bool match = true;
        for (const auto& d : h.supported) {
            if (!findHidProduct(d.vendor, d.product) || d.stableId.rfind("hid:", 0) != 0 || d.name.empty()) { match = false; }
        }
        CHECK(match);
        std::printf("note: %zu supported-product HID interface(s), %d other, %d unreadable\n", h.supported.size(), h.otherDevices, h.unreadable);
    }

    // ---- OPENING WHAT IS NOT THERE -----------------------------------------------------------------------------------------------------------------
    {
        EventQueue q;
        MidiInputPort port;
        std::string err;
        CHECK(!port.open(99999u, "midi:nothing", q, err));
        CHECK(!err.empty() && !port.isOpen());
        err.clear();
        CHECK(!port.open(0xFFFFFFFFu, "midi:nothing", q, err) && !err.empty());
        CHECK(port.close());  // closing something never opened is fine
        CHECK(port.messages() == 0 && port.rejected() == 0);

        HidDeviceInfo bogus;
        bogus.vendor = 0x0B33; bogus.product = 0x0020; bogus.kind = HidKind::ShuttleXpress; bogus.name = "Contour ShuttleXpress";
        bogus.path = L"\\\\?\\hid#vid_0b33&pid_0020#no-such-instance#{4d1e55b2-f16f-11cf-88cb-001111000030}";
        bogus.plausible = true; bogus.inputReportBytes = 6;
        HidInputDevice dev;
        err.clear();
        CHECK(!dev.open(bogus, q, err) && !err.empty() && !dev.isOpen());
        CHECK(dev.close());
        // An interface too short for the report is refused even if it opened.
        OverlappedReader r;
        CHECK(!r.start(INVALID_HANDLE_VALUE, 6, nullptr, nullptr));
        CHECK(r.stop() && !r.running());

        // A real port, if this PC has one: opens, runs, closes within a bound.
        for (const auto& p : enumerateMidiInputs()) {
            MidiInputPort real;
            std::string e2;
            if (real.open(p, q, e2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const auto t0 = Clock::now();
                CHECK(real.close(2000));
                CHECK(msSince(t0) < 2000);
            } else {
                std::printf("note: could not open MIDI port %s: %s (in use by another program is normal)\n", p.stableId.c_str(), e2.c_str());
            }
        }
    }

    // ---- THE READER on a real overlapped handle: reports reach the parser and the queue --------------------------------------------------------------
    {
        Pipe p = makePipe();
        CHECK(p.ok());
        EventQueue q;
        HidInputDevice dev;
        CHECK(dev.attach(p.server, HidKind::ShuttleXpress, "hid:0b33:0020", 6, q));  // 6 = the report ID byte + 5
        CHECK(dev.isOpen());
        // Byte 0 is the report ID Windows prepends; then ring, jog, 0, buttons, buttons.
        send(p, {0, 0, 100, 0, 0x00, 0x00});   // baseline
        send(p, {0, 0, 101, 0, 0x10, 0x00});   // jog +1 and button 1 down
        send(p, {0, 3, 101, 0, 0x10, 0x00});   // ring to 3
        CHECK(waitFor([&] { return q.size() >= 3; }));
        std::vector<InputEvent> ev;
        q.drain(ev);
        CHECK(ev.size() == 3);
        if (ev.size() == 3) {
            CHECK(ev[0].kind == EventKind::ButtonDown && ev[0].control == kButtonBase && ev[0].device == deviceIdFor("hid:0b33:0020"));
            CHECK(ev[1].kind == EventKind::Relative && ev[1].raw == 1);
            CHECK(ev[2].kind == EventKind::Absolute && ev[2].raw == 3);
            CHECK(ev[0].timeUs > 0 && ev[2].timeUs >= ev[0].timeUs);
        }
        // A truncated and a malformed report are counted, not turned into events.
        send(p, {0});
        send(p, {0, 9, 101, 0, 0, 0});   // ring +9
        CHECK(waitFor([&] { return dev.rejected() >= 2; }));
        CHECK(q.size() == 0);
        CHECK(dev.reports() >= 4);
        CHECK(dev.close());
        CHECK(!dev.isOpen());
        CloseHandle(p.client);
    }
    {   // the PowerMate parser through the same reader
        Pipe p = makePipe();
        EventQueue q;
        HidInputDevice dev;
        CHECK(p.ok() && dev.attach(p.server, HidKind::PowerMate, "hid:077d:0410", 7, q));
        send(p, {0, 1, 0xFE, 0, 0, 0, 0});
        CHECK(waitFor([&] { return q.size() >= 2; }));
        std::vector<InputEvent> ev;
        q.drain(ev);
        CHECK(ev.size() == 2 && ev[0].kind == EventKind::ButtonDown && ev[1].kind == EventKind::Relative && ev[1].raw == -2);
        CHECK(dev.close());
        CloseHandle(p.client);
    }

    // ---- CLOSE WHILE A READ IS PENDING, DEVICE SILENT: ends within a bound -------------------------------------------------------------------------
    {
        Pipe p = makePipe();
        EventQueue q;
        HidInputDevice dev;
        CHECK(p.ok() && dev.attach(p.server, HidKind::ShuttleXpress, "hid:0b33:0020", 6, q));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the worker is now inside a pending ReadFile that nothing will ever satisfy
        const auto t0 = Clock::now();
        CHECK(dev.close(2000));
        const long long ms = msSince(t0);
        CHECK(ms < 1500);
        std::printf("note: close with a pending read took %lld ms\n", ms);
        CHECK(q.size() == 0);  // a deliberate close is not a disconnect
        CloseHandle(p.client);
    }
    // ---- UNPLUGGED: the other end vanishes while a read is pending ---------------------------------------------------------------------------------
    {
        Pipe p = makePipe();
        EventQueue q;
        HidInputDevice dev;
        CHECK(p.ok() && dev.attach(p.server, HidKind::ShuttleXpress, "hid:0b33:0020", 6, q));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CloseHandle(p.client);  // unplug
        CHECK(waitFor([&] { return q.size() >= 1; }));
        std::vector<InputEvent> ev;
        q.drain(ev);
        CHECK(ev.size() == 1 && ev[0].kind == EventKind::Disconnected && ev[0].device == deviceIdFor("hid:0b33:0020"));
        CHECK(waitFor([&] { return !dev.isOpen(); }));
        const auto t0 = Clock::now();
        CHECK(dev.close());
        CHECK(msSince(t0) < 500);
    }
    // ---- CLOSE FROM ANOTHER THREAD while the reader is busy with a stream of reports; double close; close in the destructor ------------------------
    {
        Pipe p = makePipe();
        EventQueue q(64);
        auto dev = std::make_unique<HidInputDevice>();
        CHECK(p.ok() && dev->attach(p.server, HidKind::ShuttleXpress, "hid:0b33:0020", 6, q));
        std::atomic<bool> go{true};
        std::thread writer([&] {
            int j = 0;
            while (go) { send(p, {0, 0, (j++) & 0xFF, 0, 0, 0}); std::this_thread::sleep_for(std::chrono::microseconds(200)); }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto t0 = Clock::now();
        CHECK(dev->close(2000));
        CHECK(dev->close(2000));  // twice
        CHECK(msSince(t0) < 1500);
        go = false;
        writer.join();
        dev.reset();
        CloseHandle(p.client);
    }
    // ---- OPEN / CLOSE 60 TIMES: no handle or thread leak makes it slow or hang -------------------------------------------------------------------
    {
        const auto t0 = Clock::now();
        for (int i = 0; i < 60; ++i) {
            Pipe p = makePipe();
            EventQueue q;
            HidInputDevice dev;
            if (!(p.ok() && dev.attach(p.server, HidKind::PowerMate, "hid:077d:0410", 7, q))) { CHECK(false); break; }
            if (i % 2) { send(p, {0, 0, 1, 0, 0, 0, 0}); }
            CHECK(dev.close(2000));
            CloseHandle(p.client);
        }
        CHECK(msSince(t0) < 20000);
    }
    return testSummary("test_input_win");
}

#else
#include <cstdio>
int main() { std::printf("test_input_win: Windows only\n"); return 0; }
#endif
