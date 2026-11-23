// THE BULK STREAM WHEN WinUsb_SetPipePolicy(RAW_IO) IS REFUSED (field fault,
// 19-23 Nov 2026: Airspy HF+ and Mirics MSi2500, every start closed with
// "WinUsb_SetPipePolicy(RAW_IO) failed (Windows error 87)" and not one sample).
//
// Driven through the fake WinUSB table (usb/winusb_device.hpp) with a device
// whose layout is scripted: interfaces, alternate settings, endpoint types and
// packet sizes, and which handle may read which pipe. The fake answers
// RAW_IO only on a Bulk or Interrupt IN pipe (documented) that the handle's
// interface carries in its CURRENT alternate setting, and answers Windows error
// 87 to a call naming a pipe the current setting does not carry. THAT 87 IS
// INFERRED, NOT DOCUMENTED (Microsoft documents 87 only for a wrong
// ValueLength); it models the cause the review of 5f085271 points to, and the
// bench probe tests/probe_winusb_rawio.cpp settles it. A model, not a
// recording of a real HF+. What a real
// HF+ or MSi2500 reports is UNVERIFIED without one on the bench; the layout
// line this change adds to the open log is what settles it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_check.hpp"

#if defined(_WIN32)

#include "core/radio_events.hpp"
#include "source/usb_stream_note.hpp"
#include "usb/usb_device.hpp"
#include "usb/winusb_device.hpp"

using cascade::usb::UsbDevice;
using cascade::usb::WinUsbApi;

namespace {

constexpr ULONG_PTR kStatusPending = 0x00000103;
constexpr ULONG_PTR kStatusCancelled = 0xC0000120;

WINUSB_INTERFACE_HANDLE H(std::uintptr_t v) { return reinterpret_cast<WINUSB_INTERFACE_HANDLE>(v); }

struct PipeSpec {
    int iface = 0;
    int alt = 0;
    UCHAR id = 0;
    USBD_PIPE_TYPE type = UsbdPipeTypeBulk;
    USHORT mps = 512;
};

struct Fake {
    std::vector<PipeSpec> pipes;
    std::vector<std::pair<int, int>> emptyAlts;  // (iface, alt number) settings with no endpoints
    DWORD setAltFails = 0;                       // nonzero: setCurrentAlternateSetting fails with it
    int numIfaces = 1;
    int currentAlt[4] = {0, 0, 0, 0};
    bool rawIoAlways87 = false;  // refuse RAW_IO even on a pipe that is fine
    bool readsAlways87 = false;  // refuse every read of the stream pipe
    bool rawIoAcceptsAnyPipe = false;  // MODEL OF MICROSOFT'S "has no effect": RAW_IO answers TRUE for any pipe the current setting carries, isochronous included
    bool canLook = true;         // false: the new query entries are null
    int rawIoCalls = 0;
    int queryCalls = 0;
    int frees = 0;
    int altSets = 0;
    std::vector<int> altSetNumbers;  // every setting number passed to the setter
    std::vector<std::pair<WINUSB_INTERFACE_HANDLE, UCHAR>> readOn;   // (handle, pipe) of every read
    std::vector<std::pair<WINUSB_INTERFACE_HANDLE, UCHAR>> resetOn;
    std::vector<OVERLAPPED*> inFlight;
};
Fake g;

int ifaceOf(WINUSB_INTERFACE_HANDLE h) {
    const auto v = reinterpret_cast<std::uintptr_t>(h);
    return v == 0x5678 ? 0 : static_cast<int>(v - 0x6000);
}
WINUSB_INTERFACE_HANDLE handleFor(int iface) { return iface == 0 ? H(0x5678) : H(0x6000 + iface); }

// The alternate settings of an interface, as WinUSB indexes them: sorted by
// bAlternateSetting. The NUMBER is the descriptor's value; the INDEX is the
// position in this list (WinUsb_QueryInterfaceSettings takes the index).
std::vector<int> altNumbers(int iface) {
    std::vector<int> out;
    for (const PipeSpec& p : g.pipes) {
        if (p.iface == iface) { out.push_back(p.alt); }
    }
    for (const auto& e : g.emptyAlts) {
        if (e.first == iface) { out.push_back(e.second); }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const PipeSpec* findIn(int iface, UCHAR id) {
    for (const PipeSpec& p : g.pipes) {
        if (p.iface == iface && p.alt == g.currentAlt[iface] && p.id == id) { return &p; }
    }
    return nullptr;
}

BOOL __stdcall fControl(WINUSB_INTERFACE_HANDLE, WINUSB_SETUP_PACKET, PUCHAR, ULONG, PULONG,
                        LPOVERLAPPED) {
    ::SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL __stdcall fRead(WINUSB_INTERFACE_HANDLE h, UCHAR pipe, PUCHAR buf, ULONG len, PULONG,
                     LPOVERLAPPED ov) {
    g.readOn.emplace_back(h, pipe);
    if (g.readsAlways87 || findIn(ifaceOf(h), pipe) == nullptr) {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memset(buf, 0x5A, len);
    ov->Internal = 0;
    ov->InternalHigh = len;
    g.inFlight.push_back(ov);
    ::SetEvent(ov->hEvent);
    ::SetLastError(ERROR_IO_PENDING);
    return FALSE;
}

BOOL __stdcall fGetResult(WINUSB_INTERFACE_HANDLE, LPOVERLAPPED ov, LPDWORD moved, BOOL) {
    if (ov->Internal == kStatusCancelled) {
        ::SetLastError(ERROR_OPERATION_ABORTED);
        return FALSE;
    }
    *moved = static_cast<DWORD>(ov->InternalHigh);
    return TRUE;
}

BOOL __stdcall fAbort(WINUSB_INTERFACE_HANDLE, UCHAR) { return TRUE; }
BOOL __stdcall fReset(WINUSB_INTERFACE_HANDLE h, UCHAR pipe) {
    g.resetOn.emplace_back(h, pipe);
    return TRUE;
}

BOOL __stdcall fPolicy(WINUSB_INTERFACE_HANDLE h, UCHAR pipe, ULONG policy, ULONG len, PVOID) {
    if (policy != RAW_IO) { return TRUE; }
    ++g.rawIoCalls;
    if (len != 1) {  // Microsoft: a wrong ValueLength is ERROR_INVALID_PARAMETER
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const PipeSpec* p = findIn(ifaceOf(h), pipe);
    const bool ok = !g.rawIoAlways87 && p != nullptr &&
                    (g.rawIoAcceptsAnyPipe || ((pipe & 0x80) != 0 && (p->type == UsbdPipeTypeBulk || p->type == UsbdPipeTypeInterrupt)));
    if (!ok) { ::SetLastError(ERROR_INVALID_PARAMETER); }
    return ok ? TRUE : FALSE;
}
BOOL __stdcall fPower(WINUSB_INTERFACE_HANDLE, ULONG, ULONG, PVOID) { return TRUE; }
BOOL __stdcall fFree(WINUSB_INTERFACE_HANDLE) {
    ++g.frees;
    return TRUE;
}
BOOL __stdcall fCancel(HANDLE, LPOVERLAPPED ov) {
    if (ov->Internal != kStatusPending) { return TRUE; }
    return TRUE;
}
BOOL __stdcall fClose(HANDLE) { return TRUE; }

BOOL __stdcall fQueryIf(WINUSB_INTERFACE_HANDLE h, UCHAR idx, PUSB_INTERFACE_DESCRIPTOR d) {
    ++g.queryCalls;
    const int iface = ifaceOf(h);
    const std::vector<int> alts = altNumbers(iface);
    if (idx >= alts.size()) {
        ::SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    const int alt = alts[idx];
    int n = 0;
    for (const PipeSpec& p : g.pipes) {
        if (p.iface == iface && p.alt == alt) { ++n; }
    }
    std::memset(d, 0, sizeof(*d));
    d->bInterfaceNumber = static_cast<UCHAR>(iface);
    d->bAlternateSetting = static_cast<UCHAR>(alt);
    d->bNumEndpoints = static_cast<UCHAR>(n);
    return TRUE;
}

BOOL __stdcall fQueryPipe(WINUSB_INTERFACE_HANDLE h, UCHAR idx, UCHAR pipeIdx,
                          PWINUSB_PIPE_INFORMATION pi) {
    ++g.queryCalls;
    const int iface = ifaceOf(h);
    const std::vector<int> alts = altNumbers(iface);
    if (idx >= alts.size()) {
        ::SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    int seen = 0;
    for (const PipeSpec& p : g.pipes) {
        if (p.iface != iface || p.alt != alts[idx]) { continue; }
        if (seen++ == pipeIdx) {
            std::memset(pi, 0, sizeof(*pi));
            pi->PipeType = p.type;
            pi->PipeId = p.id;
            pi->MaximumPacketSize = p.mps;
            return TRUE;
        }
    }
    ::SetLastError(ERROR_NO_MORE_ITEMS);
    return FALSE;
}

BOOL __stdcall fAssoc(WINUSB_INTERFACE_HANDLE, UCHAR idx, PWINUSB_INTERFACE_HANDLE out) {
    if (idx + 1 >= g.numIfaces) {
        ::SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    *out = handleFor(idx + 1);
    return TRUE;
}

BOOL __stdcall fSetAlt(WINUSB_INTERFACE_HANDLE h, UCHAR alt) {
    ++g.altSets;
    g.altSetNumbers.push_back(alt);
    if (g.setAltFails != 0) {
        ::SetLastError(g.setAltFails);
        return FALSE;
    }
    g.currentAlt[ifaceOf(h)] = alt;
    return TRUE;
}
BOOL __stdcall fGetAlt(WINUSB_INTERFACE_HANDLE h, PUCHAR alt) {
    *alt = static_cast<UCHAR>(g.currentAlt[ifaceOf(h)]);
    return TRUE;
}

WinUsbApi makeApi() {
    WinUsbApi a = {
        &fControl, &fRead, &fGetResult, &fAbort, &fReset, &fPolicy, &fPower, &fFree, &fCancel,
        &fClose,
    };
    if (g.canLook) {
        a.queryInterfaceSettings = &fQueryIf;
        a.queryPipe = &fQueryPipe;
        a.getAssociatedInterface = &fAssoc;
        a.setCurrentAlternateSetting = &fSetAlt;
        a.getCurrentAlternateSetting = &fGetAlt;
    }
    return a;
}

void reset() { g = Fake(); }

// The device under test is built on a table that must outlive it.
struct Rig {
    WinUsbApi api = makeApi();
    std::unique_ptr<UsbDevice> dev;
    Rig() {
        dev = cascade::usb::makeWinUsbDeviceForTest(
            api, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x1234)), H(0x5678),
            "fake:winusb");
    }
};

bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

}  // namespace

int main() {
    // 1. THE HEALTHY PATH IS UNTOUCHED (the RTL-SDR's: 6,652 good stream minutes
    //    on 0.99.112). RAW_IO is accepted once, the streams and policies are as before, no note
    //    is made. CHANGED BY THE DECISION OF REVIEW FINDING B (98aa57e2): the pipe's TYPE is
    //    read first, from the descriptors WinUSB already holds, before any pipe call names
    //    it, so the layout is queried (host side only, nothing on the bus) where this test
    //    used to assert zero queries.
    {
        reset();
        g.pipes = {{0, 0, 0x81, UsbdPipeTypeBulk, 512}};
        Rig r;
        CHECK(r.dev->beginBulkStream(0x81, 16384, 4));
        CHECK(g.rawIoCalls == 1);
        CHECK(g.queryCalls > 0);
        CHECK(r.dev->takeStreamNote().empty());
        std::uint8_t buf[16384];
        CHECK(r.dev->readBulk(buf, sizeof(buf), 1000) == 16384);
        r.dev->endBulkStream();
    }

    // 2. RAW_IO REFUSED (error 87) ON A USABLE BULK IN PIPE: the stream carries
    //    on with plain reads, says so once, and delivers samples. (The fake's
    //    forced refusal is a rule no source says WinUSB has; it reproduces the old
    //    RETURN VALUE of beginBulkStream - false with "WinUsb_SetPipePolicy(RAW_IO)
    //    failed (Windows error 87)" - not a proven field cause.)
    {
        reset();
        g.pipes = {{0, 0, 0x81, UsbdPipeTypeBulk, 512}};
        g.rawIoAlways87 = true;
        Rig r;
        const bool began = r.dev->beginBulkStream(0x81, 16384, 4);
        if (!began) { std::printf("     began=false: %s\n", r.dev->lastError().c_str()); }
        CHECK(began);
        CHECK(r.dev->streaming());
        std::uint8_t buf[16384] = {0};
        CHECK(r.dev->readBulk(buf, sizeof(buf), 1000) == 16384);
        CHECK(buf[0] == 0x5A);
        const std::string note = r.dev->takeStreamNote();
        CHECK(has(note, "RAW_IO refused on pipe 0x81"));
        CHECK(has(note, "Windows error 87"));
        CHECK(has(note, "interface 0 alt 0 (current): 0x81 bulk packet 512"));
        CHECK(r.dev->takeStreamNote().empty());  // once
        r.dev->endBulkStream();
    }

    // 3. THE ENDPOINT IS NOT THERE: a clear reason, no unplug advice, no reads.
    {
        reset();
        g.pipes = {{0, 0, 0x82, UsbdPipeTypeBulk, 512}, {0, 0, 0x02, UsbdPipeTypeBulk, 512}};
        Rig r;
        CHECK(!r.dev->beginBulkStream(0x81, 16384, 4));
        const std::string e = r.dev->lastError();
        CHECK(cascade::usb::errorIsLayoutProblem(e));
        CHECK(has(e, "endpoint 0x81 does not exist"));
        CHECK(has(e, "0x82 bulk packet 512"));
        CHECK(!has(e, "unplug"));
        CHECK(g.readOn.empty());
        CHECK(!r.dev->streaming());
    }

    // 4. THE ENDPOINT IS ISOCHRONOUS (the Mirics hypothesis): said in words.
    {
        reset();
        g.pipes = {{0, 0, 0x81, UsbdPipeTypeIsochronous, 1024}};
        Rig r;
        CHECK(!r.dev->beginBulkStream(0x81, 16384, 4));
        const std::string e = r.dev->lastError();
        CHECK(cascade::usb::errorIsLayoutProblem(e));
        CHECK(has(e, "isochronous"));
        CHECK(has(e, "0x81 isochronous packet 1024"));
        CHECK(g.readOn.empty());
        CHECK(g.rawIoCalls == 0);   // the type was read first: no pipe policy named the pipe (finding B)
    }

    // 5. AN OUT ENDPOINT asked for as a stream.
    {
        reset();
        g.pipes = {{0, 0, 0x01, UsbdPipeTypeBulk, 512}};
        Rig r;
        CHECK(!r.dev->beginBulkStream(0x01, 16384, 4));
        CHECK(has(r.dev->lastError(), "an OUT endpoint"));
    }

    // 6. THE BULK PIPE IS IN AN ALTERNATE SETTING THAT WAS NEVER SELECTED: the
    //    transport selects it, RAW_IO is then accepted, and reads work (the fake
    //    refuses reads of a pipe the current alternate setting does not carry).
    {
        reset();
        g.pipes = {{0, 0, 0x83, UsbdPipeTypeInterrupt, 8}, {0, 1, 0x81, UsbdPipeTypeBulk, 512}};
        Rig r;
        CHECK(r.dev->beginBulkStream(0x81, 16384, 4));
        CHECK(g.altSets == 1 && g.currentAlt[0] == 1);
        std::uint8_t buf[16384];
        CHECK(r.dev->readBulk(buf, sizeof(buf), 1000) == 16384);
        const std::string note = r.dev->takeStreamNote();
        CHECK(has(note, "interface 0 alt 1"));
        r.dev->endBulkStream();
    }

    // 7. THE BULK PIPE IS ON A LATER INTERFACE: it needs that interface's own
    //    handle (WinUsb_GetAssociatedInterface). Reads and the pre-start
    //    resetPipe go through it, and teardown frees both handles.
    {
        reset();
        g.numIfaces = 2;
        g.pipes = {{0, 0, 0x83, UsbdPipeTypeInterrupt, 8}, {1, 0, 0x81, UsbdPipeTypeBulk, 512}};
        {
            Rig r;
            CHECK(r.dev->beginBulkStream(0x81, 16384, 4));
            CHECK(!g.readOn.empty());
            bool allOnSecond = true;
            for (const auto& rd : g.readOn) { allOnSecond = allOnSecond && rd.first == H(0x6001); }
            CHECK(allOnSecond);
            std::uint8_t buf[16384];
            CHECK(r.dev->readBulk(buf, sizeof(buf), 1000) == 16384);
            CHECK(has(r.dev->takeStreamNote(), "is in interface 1 alt 0"));
            r.dev->endBulkStream();
            CHECK(r.dev->resetPipe(0x81));
            CHECK(g.resetOn.back().first == H(0x6001));
        }
        CHECK(g.frees == 2);
    }

    // 8. THE TABLE CANNOT LOOK (old tables leave the new entries null) and RAW_IO
    //    is refused: plain reads, and a note that says the layout is unavailable.
    {
        reset();
        g.canLook = false;
        g.pipes = {{0, 0, 0x81, UsbdPipeTypeBulk, 512}};
        g.rawIoAlways87 = true;
        Rig r;
        CHECK(r.dev->beginBulkStream(0x81, 16384, 4));
        CHECK(has(r.dev->takeStreamNote(), "USB layout unavailable"));
        CHECK(r.dev->endpointLayout() == "USB layout unavailable");
        r.dev->endBulkStream();
    }

    // 9. PLAIN READS REFUSED TOO (pipe id wrong for this handle, layout unknown):
    //    a reason that names the endpoint instead of a bare error number.
    {
        reset();
        g.pipes = {{0, 0, 0x81, UsbdPipeTypeBulk, 512}};
        g.rawIoAlways87 = true;
        g.readsAlways87 = true;
        Rig r;
        CHECK(!r.dev->beginBulkStream(0x81, 16384, 4));
        const std::string e = r.dev->lastError();
        CHECK(cascade::usb::errorIsLayoutProblem(e));
        CHECK(has(e, "refused plain reads as well"));
        CHECK(has(e, "interface 0 alt 0 (current): 0x81 bulk packet 512"));
    }

    // 10. endpointLayout() for the open log line: every interface and alternate
    //     setting, no serial.
    {
        reset();
        g.numIfaces = 2;
        g.pipes = {{0, 0, 0x81, UsbdPipeTypeBulk, 512},
                   {0, 1, 0x81, UsbdPipeTypeIsochronous, 1024},
                   {1, 0, 0x02, UsbdPipeTypeBulk, 64}};
        Rig r;
        const std::string l = r.dev->endpointLayout();
        CHECK(l == "interface 0 alt 0 (current): 0x81 bulk packet 512; "
                   "interface 0 alt 1: 0x81 isochronous packet 1024; "
                   "interface 1 alt 0 (current): 0x02 bulk packet 64");
    }

    // 12. AN ISOCHRONOUS 0x81 IN AN EARLIER SETTING MUST NOT HIDE A BULK 0x81 IN A
    //     LATER ONE (review finding 1): the stream moves to setting 3.
    {
        reset();
        g.emptyAlts = {{0, 0}};
        g.pipes = {{0, 1, 0x81, UsbdPipeTypeIsochronous, 1024}, {0, 3, 0x81, UsbdPipeTypeBulk, 512}};
        Rig r;
        CHECK(r.dev->beginBulkStream(0x81, 16384, 4));
        CHECK(g.currentAlt[0] == 3);
        std::uint8_t buf[16384];
        CHECK(r.dev->readBulk(buf, sizeof(buf), 1000) == 16384);
        const std::string note = r.dev->takeStreamNote();
        CHECK(has(note, cascade::usb::kStreamNoteMoved));  // RAW_IO is on: a move, not a fallback
        CHECK(!has(note, cascade::usb::kStreamNoteFallback));
        r.dev->endBulkStream();
    }

    // 13. NON-CONTIGUOUS SETTING NUMBERS (finding 4): the setter gets the
    //     descriptor's bAlternateSetting (5), not the index (1), and the layout
    //     prints the number.
    {
        reset();
        g.pipes = {{0, 2, 0x83, UsbdPipeTypeInterrupt, 8}, {0, 5, 0x81, UsbdPipeTypeBulk, 512}};
        g.currentAlt[0] = 2;
        Rig r;
        CHECK(r.dev->endpointLayout() ==
              "interface 0 alt 2 (current): 0x83 interrupt packet 8; "
              "interface 0 alt 5: 0x81 bulk packet 512");
        CHECK(r.dev->beginBulkStream(0x81, 16384, 4));
        CHECK(g.altSetNumbers.size() == 1 && g.altSetNumbers[0] == 5);
        r.dev->endBulkStream();
    }

    // 14. THE HF+ SHAPE, and settings with no endpoints (finding 5): alt 0 is empty
    //     and current, alt 1 carries bulk 0x81. The line says so.
    {
        reset();
        g.emptyAlts = {{0, 0}};
        g.pipes = {{0, 1, 0x81, UsbdPipeTypeBulk, 512}};
        Rig r;
        CHECK(r.dev->endpointLayout() ==
              "interface 0 alt 0 (current): no endpoints; interface 0 alt 1: 0x81 bulk packet 512");
        // Selected by the driver at open: no recovery is needed at start.
        CHECK(r.dev->selectAlternateSetting(0, 1));
        CHECK(g.currentAlt[0] == 1);
        CHECK(r.dev->beginBulkStream(0x81, 16384, 4));
        CHECK(r.dev->takeStreamNote().empty());
        CHECK(g.rawIoCalls == 1);
        r.dev->endBulkStream();
    }

    // 15. ensureStreamingSetting (the Mirics open): selects the bulk setting when
    //     the current one lacks it; isochronous-only changes nothing.
    {
        reset();
        g.emptyAlts = {{0, 0}};
        g.pipes = {{0, 1, 0x81, UsbdPipeTypeBulk, 512}};
        Rig r;
        CHECK(r.dev->ensureStreamingSetting(0x81));
        CHECK(g.currentAlt[0] == 1);
        reset();
        g.pipes = {{0, 0, 0x81, UsbdPipeTypeIsochronous, 1024}};
        Rig r2;
        CHECK(!r2.dev->ensureStreamingSetting(0x81));
        CHECK(g.altSets == 0);
    }

    // 16. A GENUINE UNPLUG while selecting the setting (finding 6): error 1167 is
    //     not a layout fact, so the sentence keeps the transfer-fault words.
    {
        reset();
        g.emptyAlts = {{0, 0}};
        g.pipes = {{0, 1, 0x81, UsbdPipeTypeBulk, 512}};
        g.setAltFails = ERROR_DEVICE_NOT_CONNECTED;
        Rig r;
        CHECK(!r.dev->beginBulkStream(0x81, 16384, 4));
        CHECK(!cascade::usb::errorIsLayoutProblem(r.dev->lastError()));
        CHECK(has(r.dev->lastError(), "Windows error 1167"));
    }

    // 17. A SECOND START keeps the route found by the first: no second note, and
    //     RAW_IO stays on.
    {
        reset();
        g.numIfaces = 2;
        g.pipes = {{0, 0, 0x83, UsbdPipeTypeInterrupt, 8}, {1, 0, 0x81, UsbdPipeTypeBulk, 512}};
        Rig r;
        CHECK(r.dev->beginBulkStream(0x81, 16384, 4));
        CHECK(!r.dev->takeStreamNote().empty());
        r.dev->endBulkStream();
        CHECK(r.dev->beginBulkStream(0x81, 16384, 4));
        CHECK(r.dev->takeStreamNote().empty());
        r.dev->endBulkStream();
    }

    // 18. ERROR 31 ON THE SELECT IS NOT "THE RADIO IS GONE" (review of 98aa57e2, finding F): a device that
    //     refuses SET_INTERFACE by stalling is most likely reported as ERROR_GEN_FAILURE (31). It gets the
    //     select-failed sentence with the layout, not the transfer-fault words and not "unplug it".
    {
        reset();
        g.emptyAlts = {{0, 0}};
        g.pipes = {{0, 1, 0x81, UsbdPipeTypeBulk, 512}};
        g.setAltFails = ERROR_GEN_FAILURE;
        Rig r;
        CHECK(!r.dev->beginBulkStream(0x81, 16384, 4));
        const std::string e = r.dev->lastError();
        CHECK(cascade::usb::errorIsLayoutProblem(e));
        CHECK(has(e, "alternate setting 1, which could not be selected (Windows error 31)"));
        const std::string s = cascade::source::transportFaultSentence("queueing the sample transfers", e);
        CHECK(!has(s, "unplug"));
        CHECK(cascade::core::radioreport::closeCauseFor(true, s) == "fault");
    }

    // 19. AN ISOCHRONOUS-ONLY 0x81 IS RECOGNISED BEFORE ANY PIPE CALL (finding B). The fake models Microsoft's "a
    //     policy set on a pipe type it does not apply to has no effect": RAW_IO answers TRUE even on the isochronous
    //     pipe. The old code then queued reads and told the failure as "stopped answering ... unplug it". Now the
    //     pipe's type is read first: no RAW_IO call, no read, a true fault sentence, and the report cause is "fault".
    for (const bool current : {true, false}) {
        reset();
        g.rawIoAcceptsAnyPipe = true;
        if (current) {
            g.pipes = {{0, 0, 0x81, UsbdPipeTypeIsochronous, 1024}};
        } else {
            g.emptyAlts = {{0, 0}};
            g.pipes = {{0, 1, 0x81, UsbdPipeTypeIsochronous, 1024}};
        }
        Rig r;
        CHECK(!r.dev->beginBulkStream(0x81, 16384, 4));
        const std::string e = r.dev->lastError();
        CHECK(cascade::usb::errorIsLayoutProblem(e));
        CHECK(has(e, "this radio's Windows driver setting offers only an isochronous stream, which FoxSDR cannot read"));
        CHECK(g.rawIoCalls == 0);
        CHECK(g.readOn.empty());
        CHECK(!r.dev->streaming());
        const std::string s = cascade::source::transportFaultSentence("queueing the sample transfers", e);
        CHECK(!has(s, "unplug"));
        CHECK(!has(s, "stopped answering"));
        CHECK(cascade::core::radioreport::closeCauseFor(true, s) == "fault");   // never "unplugged"
    }

    // 20. ensureStreamingSetting NEVER ANSWERS FALSE WITHOUT ITS OWN REASON, and never with a stale Windows error (finding C).
    {
        // absent
        reset();
        g.pipes = {{0, 0, 0x82, UsbdPipeTypeBulk, 512}};
        Rig a;
        CHECK(!a.dev->ensureStreamingSetting(0x81));
        CHECK(cascade::usb::errorIsLayoutProblem(a.dev->lastError()) && has(a.dev->lastError(), "endpoint 0x81 does not exist"));
        // isochronous only
        reset();
        g.pipes = {{0, 0, 0x81, UsbdPipeTypeIsochronous, 1024}};
        Rig b;
        CHECK(!b.dev->ensureStreamingSetting(0x81));
        CHECK(cascade::usb::errorIsLayoutProblem(b.dev->lastError()) && has(b.dev->lastError(), "only an isochronous stream, which FoxSDR cannot read"));
        // the table cannot look
        reset();
        g.canLook = false;
        g.pipes = {{0, 0, 0x81, UsbdPipeTypeBulk, 512}};
        Rig c;
        CHECK(!c.dev->ensureStreamingSetting(0x81));
        CHECK(has(c.dev->lastError(), "could not be read"));
        CHECK(!cascade::usb::errorIsLayoutProblem(c.dev->lastError()));   // not a verdict about the radio
        // the select itself fails: the Windows error of THAT call, read where it failed (a decoy error is left on the thread first)
        reset();
        g.emptyAlts = {{0, 0}};
        g.pipes = {{0, 1, 0x81, UsbdPipeTypeBulk, 512}};
        g.setAltFails = ERROR_ACCESS_DENIED;
        Rig d;
        ::SetLastError(ERROR_FILE_NOT_FOUND);
        CHECK(!d.dev->ensureStreamingSetting(0x81));
        CHECK(has(d.dev->lastError(), "Windows error 5"));
        CHECK(!has(d.dev->lastError(), "Windows error 2)"));
        // and the healthy answer is still true
        reset();
        g.emptyAlts = {{0, 0}};
        g.pipes = {{0, 1, 0x81, UsbdPipeTypeBulk, 512}};
        Rig e;
        CHECK(e.dev->ensureStreamingSetting(0x81));
        CHECK(g.currentAlt[0] == 1);
    }

    // 21. "altmoved" IS FILED ONCE PER RADIO OPEN, not once per process (finding E): two radios (two device objects) that each moved report once each; a
    //     second report from the same device is not filed.
    {
        const auto movedDevice = [](std::unique_ptr<UsbDevice>& dev, Rig*& rig) {
            reset();
            g.numIfaces = 2;
            g.pipes = {{0, 0, 0x83, UsbdPipeTypeInterrupt, 8}, {1, 0, 0x81, UsbdPipeTypeBulk, 512}};
            rig = new Rig();
            CHECK(rig->dev->beginBulkStream(0x81, 16384, 4));
            dev = std::move(rig->dev);
        };
        std::unique_ptr<UsbDevice> d1, d2;
        Rig* r1 = nullptr;
        Rig* r2 = nullptr;
        movedDevice(d1, r1);
        movedDevice(d2, r2);
        CHECK(cascade::source::reportStreamNote(*d1, nullptr, "airspyhf"));    // the first radio's move is filed
        CHECK(cascade::source::reportStreamNote(*d2, nullptr, "airspyhf"));    // and the second radio's, which a process-wide flag would have swallowed
        CHECK(d1->takeAltMovedFiled());                                        // each device has now filed its own
        CHECK(d2->takeAltMovedFiled());
        d1.reset();
        d2.reset();
        delete r1;
        delete r2;
    }


    // 11. THE USER-FACING SENTENCE. A layout problem is not an unplugged radio:
    //     no "unplug it" advice, and the close classifier does not call it so.
    //     An ordinary transfer fault keeps the advice (the old behaviour).
    {
        const std::string layoutText =
            std::string(cascade::usb::kLayoutErrorPrefix) + "endpoint 0x81 is isochronous";
        const std::string s =
            cascade::source::transportFaultSentence("queueing the sample transfers", layoutText);
        CHECK(!has(s, "unplug"));
        CHECK(has(s, "isochronous"));
        CHECK(cascade::core::radioreport::closeCauseFor(true, s) == "fault");
        const std::string old = cascade::source::transportFaultSentence(
            "reading samples", "a bulk read failed (Windows error 31)");
        CHECK(has(old, "unplug it and plug it back in"));
    }

    return testSummary("test_winusb_rawio");
}

#else  // !_WIN32

int main() {
    SKIP_LINUX("the WinUSB transport and its fake table exist only on Windows");
    return testSummary("test_winusb_rawio");
}

#endif  // _WIN32
