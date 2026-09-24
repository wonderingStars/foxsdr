// THE WINUSB TRANSPORT'S CANCELLATION PATHS, driven through a fake WinUSB
// table (usb/winusb_device.hpp) - the paths that run only after a device has
// already gone wrong, and that no healthy dongle can produce on demand.
//
// Two defects from the 2026-09-24 bug hunt live here, and both are "the
// kernel may still write into memory after we stopped owning it":
//
//  1. BULK: endBulkStream() leaks a ring whose cancelled reads did not drain
//     within kAbortDrainWait, "for the life of the process" - but it leaked
//     it into a MEMBER of the device, so the device's own destructor freed
//     every buffer microseconds later while a read could still complete into
//     it.
//  2. CONTROL: a control transfer that timed out, and whose cancellation did
//     not land within kAbortDrainWait, returned anyway - with the kernel still
//     holding a STACK OVERLAPPED and the CALLER'S buffer - and the next
//     transfer shared its event, so the stale completion could wake it early.
//
// The fake is the whole device: a request it answers "pending" and never
// completes is the wedged dongle, and a completion the test writes later is
// the late IRP. Every event waited on is a real kernel event, signalled the
// way the kernel signals it. Deterministic: nothing here sleeps and hopes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_check.hpp"

#if defined(_WIN32)

#include "usb/usb_device.hpp"
#include "usb/winusb_device.hpp"

using cascade::usb::UsbDevice;
using cascade::usb::WinUsbApi;

namespace {

// NTSTATUS values the kernel leaves in OVERLAPPED::Internal. Spelled here
// because WIN32_LEAN_AND_MEAN keeps ntstatus.h out.
constexpr ULONG_PTR kStatusPending = 0x00000103;
constexpr ULONG_PTR kStatusCancelled = 0xC0000120;

// One request the device handed to the "kernel". The event is copied at
// submission time, so a test can signal it later without reading an
// OVERLAPPED that may no longer exist.
struct Submitted {
    OVERLAPPED* ov = nullptr;
    HANDLE event = nullptr;
    PUCHAR data = nullptr;
    ULONG len = 0;
};

enum class Answer { Sync, PendingThenDone, PendingForever };

struct FakeKernel {
    // Whether AbortPipe / CancelIoEx complete what they cancel. False is the
    // wedged or just-removed device this file exists for.
    bool cancelCompletes = true;
    Answer control = Answer::Sync;
    std::vector<std::uint8_t> answer;
    std::vector<Submitted> reads;
    std::vector<Submitted> controls;
    std::vector<std::uint8_t> lastOutBytes;
    std::vector<OVERLAPPED*> cancelled;
    // Signalled INSIDE the next control submission: a late completion of an
    // earlier transfer landing while the next one is in flight.
    HANDLE signalDuringNextControl = nullptr;
    int frees = 0;
    int closes = 0;
};

FakeKernel g;

void resetFake() { g = FakeKernel(); }

void cancelOne(OVERLAPPED* ov) {
    if (ov->Internal != kStatusPending) { return; }
    ov->Internal = kStatusCancelled;
    ov->InternalHigh = 0;
    ::SetEvent(ov->hEvent);
}

BOOL __stdcall fakeControlTransfer(WINUSB_INTERFACE_HANDLE, WINUSB_SETUP_PACKET setup,
                                   PUCHAR data, ULONG len, PULONG moved, LPOVERLAPPED ov) {
    g.controls.push_back(Submitted{ov, ov->hEvent, data, len});
    if ((setup.RequestType & 0x80) == 0) {
        g.lastOutBytes.assign(data, data + len);
    }
    if (g.signalDuringNextControl != nullptr) {
        ::SetEvent(g.signalDuringNextControl);
        g.signalDuringNextControl = nullptr;
    }
    const ULONG n = std::min<ULONG>(len, static_cast<ULONG>(g.answer.size()));
    if (g.control == Answer::Sync || g.control == Answer::PendingThenDone) {
        if ((setup.RequestType & 0x80) != 0 && n > 0) { std::memcpy(data, g.answer.data(), n); }
        const ULONG done = ((setup.RequestType & 0x80) != 0) ? n : len;
        ov->Internal = 0;
        ov->InternalHigh = done;
        ::SetEvent(ov->hEvent);
        if (g.control == Answer::Sync) {
            if (moved != nullptr) { *moved = done; }
            return TRUE;
        }
        ::SetLastError(ERROR_IO_PENDING);
        return FALSE;
    }
    ov->Internal = kStatusPending;
    ::SetLastError(ERROR_IO_PENDING);
    return FALSE;
}

BOOL __stdcall fakeReadPipe(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR buf, ULONG len, PULONG,
                            LPOVERLAPPED ov) {
    g.reads.push_back(Submitted{ov, ov->hEvent, buf, len});
    ov->Internal = kStatusPending;
    ::SetLastError(ERROR_IO_PENDING);
    return FALSE;
}

BOOL __stdcall fakeGetOverlappedResult(WINUSB_INTERFACE_HANDLE, LPOVERLAPPED ov, LPDWORD moved,
                                       BOOL) {
    if (ov->Internal == kStatusPending) {
        ::SetLastError(ERROR_IO_INCOMPLETE);
        return FALSE;
    }
    if (ov->Internal == kStatusCancelled) {
        ::SetLastError(ERROR_OPERATION_ABORTED);
        return FALSE;
    }
    *moved = static_cast<DWORD>(ov->InternalHigh);
    return TRUE;
}

BOOL __stdcall fakeAbortPipe(WINUSB_INTERFACE_HANDLE, UCHAR) {
    if (g.cancelCompletes) {
        for (const Submitted& s : g.reads) { cancelOne(s.ov); }
    }
    return TRUE;
}

BOOL __stdcall fakeCancelIoEx(HANDLE, LPOVERLAPPED ov) {
    g.cancelled.push_back(ov);
    if (g.cancelCompletes) { cancelOne(ov); }
    return TRUE;
}

BOOL __stdcall fakeResetPipe(WINUSB_INTERFACE_HANDLE, UCHAR) { return TRUE; }
BOOL __stdcall fakeSetPipePolicy(WINUSB_INTERFACE_HANDLE, UCHAR, ULONG, ULONG, PVOID) {
    return TRUE;
}
BOOL __stdcall fakeSetPowerPolicy(WINUSB_INTERFACE_HANDLE, ULONG, ULONG, PVOID) { return TRUE; }
BOOL __stdcall fakeFree(WINUSB_INTERFACE_HANDLE) {
    ++g.frees;
    return TRUE;
}
BOOL __stdcall fakeCloseFile(HANDLE) {
    ++g.closes;
    return TRUE;
}

const WinUsbApi kFakeApi = {
    &fakeControlTransfer, &fakeReadPipe,      &fakeGetOverlappedResult, &fakeAbortPipe,
    &fakeResetPipe,       &fakeSetPipePolicy, &fakeSetPowerPolicy,      &fakeFree,
    &fakeCancelIoEx,      &fakeCloseFile,
};

std::unique_ptr<UsbDevice> makeDevice() {
    // Handles only the fake ever sees; never dereferenced, never closed for
    // real.
    return cascade::usb::makeWinUsbDeviceForTest(
        kFakeApi, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x1234)),
        reinterpret_cast<WINUSB_INTERFACE_HANDLE>(static_cast<std::uintptr_t>(0x5678)),
        "fake:winusb");
}

// Is `p` inside the calling thread's stack? A request the kernel may still
// complete must never be: once control() returns, that frame is reused by
// whatever this thread calls next.
bool onThisThreadsStack(const void* p) {
    ULONG_PTR lo = 0;
    ULONG_PTR hi = 0;
    ::GetCurrentThreadStackLimits(&lo, &hi);
    const auto a = reinterpret_cast<ULONG_PTR>(p);
    return a >= lo && a < hi;
}

bool wasCancelled(OVERLAPPED* ov) {
    return std::find(g.cancelled.begin(), g.cancelled.end(), ov) != g.cancelled.end();
}

// The late IRP: the kernel finishing a request after the transport gave up
// on it - bytes into its buffer, status into its OVERLAPPED, its event set.
void completeLate(const Submitted& s, std::uint8_t fill) {
    if (s.data != nullptr && s.len > 0) { std::memset(s.data, fill, s.len); }
    s.ov->Internal = 0;
    s.ov->InternalHigh = s.len;
    ::SetEvent(s.event);
}

long long msSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
}

}  // namespace

int main() {
    // =====================================================================
    // 1. A HEALTHY TEARDOWN frees its ring: the cancelled reads come back,
    //    nothing is leaked, and the handles are released once each. The
    //    control half of the bulk tests below - a "leak" that happened here
    //    too would make their count prove nothing.
    // =====================================================================
    {
        resetFake();
        const std::size_t live0 = cascade::usb::liveUsbRequestsForTest();
        auto dev = makeDevice();
        CHECK(dev->beginBulkStream(0x81, 512, 4));
        CHECK(g.reads.size() == 4);
        CHECK(cascade::usb::liveUsbRequestsForTest() == live0 + 4);
        const auto t0 = std::chrono::steady_clock::now();
        dev.reset();
        CHECK(msSince(t0) < 200);  // drained at once, no bound spent
        CHECK(cascade::usb::liveUsbRequestsForTest() == live0);
        CHECK(g.frees == 1);
        CHECK(g.closes == 1);
    }

    // =====================================================================
    // 2. A WEDGED RING OUTLIVES ITS DEVICE (finding usb-transport-1).
    //
    // The device is destroyed while four cancelled reads never come back:
    // the unplugged-dongle teardown the leak was written for. The drain
    // bound must be honoured, and then every request block - buffer,
    // OVERLAPPED, event - must STILL EXIST after the device is gone, because
    // the kernel still holds pointers into all of them. The defect put them
    // in a member vector, so the device's own member teardown freed them.
    // =====================================================================
    {
        resetFake();
        g.cancelCompletes = false;
        const std::size_t live0 = cascade::usb::liveUsbRequestsForTest();
        auto dev = makeDevice();
        CHECK(dev->beginBulkStream(0x81, 512, 4));
        CHECK(g.reads.size() == 4);
        const auto t0 = std::chrono::steady_clock::now();
        dev.reset();
        const long long elapsed = msSince(t0);
        // The bound (kAbortDrainWait, 250 ms) was spent, and not exceeded by
        // much: one deadline for the whole ring, not one per request.
        CHECK(elapsed >= 200);
        CHECK(elapsed < 1000);
        CHECK(g.frees == 1);
        CHECK(g.closes == 1);
        const std::size_t live = cascade::usb::liveUsbRequestsForTest();
        if (live != live0 + 4) {
            std::printf("     %zu of 4 abandoned bulk requests survived their device\n",
                        live - live0);
        }
        CHECK(live == live0 + 4);
        // The late completions land now. Into memory that is still owned when
        // the leak is real; on the defective code this write would itself be
        // the heap use-after-free, so it is only performed once the count
        // has shown the blocks exist.
        if (live == live0 + 4) {
            for (const Submitted& s : g.reads) { completeLate(s, 0x5A); }
            CHECK(cascade::usb::liveUsbRequestsForTest() == live0 + 4);
        }
    }

    // =====================================================================
    // 3. CONTROL TRANSFERS THAT ANSWER still move the right bytes both ways,
    //    synchronously and through the overlapped wait. The fix routes every
    //    transfer through the transport's own buffer, so this is the half
    //    that proves nothing was lost on the way.
    // =====================================================================
    {
        resetFake();
        auto dev = makeDevice();
        g.answer = {0x11, 0x22, 0x33, 0x44};

        g.control = Answer::Sync;
        std::uint8_t in[4] = {0, 0, 0, 0};
        CHECK(dev->controlIn(0xC0, 0x01, 0, 0, in, sizeof(in), 100) == 4);
        CHECK(std::vector<std::uint8_t>(in, in + 4) == g.answer);

        g.control = Answer::PendingThenDone;
        std::uint8_t in2[4] = {0, 0, 0, 0};
        CHECK(dev->controlIn(0xC0, 0x01, 0, 0, in2, sizeof(in2), 100) == 4);
        CHECK(std::vector<std::uint8_t>(in2, in2 + 4) == g.answer);

        // A short answer is reported short, and only those bytes are copied.
        g.answer = {0x99, 0x98};
        std::uint8_t in3[4] = {0, 0, 0, 0};
        CHECK(dev->controlIn(0xC0, 0x01, 0, 0, in3, sizeof(in3), 100) == 2);
        CHECK(std::vector<std::uint8_t>(in3, in3 + 4) ==
              (std::vector<std::uint8_t>{0x99, 0x98, 0x00, 0x00}));

        const std::uint8_t out[3] = {0x07, 0x08, 0x09};
        g.control = Answer::Sync;
        CHECK(dev->controlOut(0x40, 0x02, 0, 0, out, sizeof(out), 100) == 3);
        CHECK(g.lastOutBytes == (std::vector<std::uint8_t>{0x07, 0x08, 0x09}));
        g.control = Answer::PendingThenDone;
        CHECK(dev->controlOut(0x40, 0x02, 0, 0, out, sizeof(out), 100) == 3);
        CHECK(g.lastOutBytes == (std::vector<std::uint8_t>{0x07, 0x08, 0x09}));

        // A zero-length OUT (the commonest RTL-SDR register write shape).
        CHECK(dev->controlOut(0x40, 0x03, 0x1234, 0x10, nullptr, 0, 100) == 0);
    }

    // =====================================================================
    // 4. A TIMED-OUT CONTROL TRANSFER WHOSE CANCEL LANDS is an ordinary
    //    error: -1, the timeout named, nothing leaked, and the next transfer
    //    works.
    // =====================================================================
    {
        resetFake();
        auto dev = makeDevice();
        g.answer = {0x01};
        g.control = Answer::Sync;
        std::uint8_t b[1] = {0};
        CHECK(dev->controlIn(0xC0, 0x01, 0, 0, b, 1, 100) == 1);
        const std::size_t live0 = cascade::usb::liveUsbRequestsForTest();

        g.control = Answer::PendingForever;
        std::uint8_t t[4] = {0, 0, 0, 0};
        CHECK(dev->controlIn(0xC0, 0x01, 0, 0, t, sizeof(t), 20) == -1);
        CHECK(dev->lastError().find("did not answer within its timeout") != std::string::npos);
        CHECK(!g.controls.empty() && wasCancelled(g.controls.back().ov));
        CHECK(cascade::usb::liveUsbRequestsForTest() == live0);

        g.control = Answer::Sync;
        g.answer = {0x42};
        CHECK(dev->controlIn(0xC0, 0x01, 0, 0, b, 1, 100) == 1);
        CHECK(b[0] == 0x42);
    }

    // =====================================================================
    // 5. A TIMED-OUT CONTROL TRANSFER WHOSE CANCEL NEVER LANDS
    //    (finding usb-transport-3).
    //
    // The kernel still holds the request when control() returns. So what it
    // holds must be (a) not on this thread's stack, (b) not the caller's
    // buffer, and (c) still alive after the DEVICE is gone. The defect
    // handed the kernel a stack OVERLAPPED and the caller's own `data`, and
    // returned regardless of whether the cancel had landed.
    // =====================================================================
    {
        resetFake();
        const std::size_t liveStart = cascade::usb::liveUsbRequestsForTest();
        auto dev = makeDevice();
        g.cancelCompletes = false;
        g.control = Answer::PendingForever;

        std::uint8_t caller[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(dev->controlIn(0xC0, 0x01, 0, 0, caller, sizeof(caller), 20) == -1);
        const long long elapsed = msSince(t0);
        CHECK(elapsed < 1000);  // bounded: the timeout plus the drain bound
        CHECK(g.controls.size() == 1);
        const Submitted first = g.controls.empty() ? Submitted{} : g.controls.front();
        CHECK(wasCancelled(first.ov));

        const bool ovSafe = first.ov != nullptr && !onThisThreadsStack(first.ov);
        const bool dataSafe = first.data != caller;
        if (!ovSafe) {
            std::printf("     an abandoned control transfer's OVERLAPPED is on the caller's "
                        "stack\n");
        }
        if (!dataSafe) {
            std::printf("     an abandoned control transfer still targets the caller's "
                        "buffer\n");
        }
        CHECK(ovSafe);
        CHECK(dataSafe);

        // The late completion lands. Only performed when it is safe to - on
        // the defective code it would write into a dead stack frame and into
        // `caller` - and when it lands the caller's bytes are untouched.
        if (ovSafe && dataSafe) {
            completeLate(first, 0xAB);
            CHECK(std::vector<std::uint8_t>(caller, caller + 8) ==
                  std::vector<std::uint8_t>(8, 0x00));
        }

        // THE NEXT TRANSFER HAS ITS OWN EVENT. The stale request's
        // completion arrives while the next one is in flight; with one event
        // shared between them that wakes the new transfer early, which then
        // returns WITHOUT cancelling its own still-live request - the defect
        // again, one call later.
        g.signalDuringNextControl = first.event;
        std::uint8_t caller2[2] = {0, 0};
        CHECK(dev->controlIn(0xC0, 0x01, 0, 0, caller2, sizeof(caller2), 20) == -1);
        CHECK(g.controls.size() == 2);
        const Submitted second = g.controls.size() >= 2 ? g.controls[1] : Submitted{};
        CHECK(second.event != first.event);
        // Counted, not only looked up by address: a stack OVERLAPPED for the
        // second call lands at the SAME address as the first one's, so a
        // pointer lookup alone would credit the second with the first's
        // cancel.
        CHECK(g.cancelled.size() == 2);
        CHECK(wasCancelled(second.ov));
        CHECK(dev->lastError().find("did not answer within its timeout") != std::string::npos);

        // Both abandoned blocks outlive the device.
        dev.reset();
        const std::size_t live = cascade::usb::liveUsbRequestsForTest();
        if (live != liveStart + 2) {
            std::printf("     %zu of 2 abandoned control requests survived their device\n",
                        live - liveStart);
        }
        CHECK(live == liveStart + 2);
        if (live == liveStart + 2 && second.ov != nullptr && !onThisThreadsStack(second.ov) &&
            second.data != caller2) {
            completeLate(second, 0xCD);
            CHECK(caller2[0] == 0 && caller2[1] == 0);
        }
    }

    return testSummary("test_winusb_abandon");
}

#else  // !_WIN32

int main() {
    SKIP_LINUX("the WinUSB transport and its fake table exist only on Windows");
    return testSummary("test_winusb_abandon");
}

#endif  // _WIN32
