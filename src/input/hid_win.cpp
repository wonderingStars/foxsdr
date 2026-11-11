// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifdef _WIN32

#include "hid_win.hpp"

#include <setupapi.h>

extern "C" {
#include <hidsdi.h>
}
#include <hidpi.h>

#include <algorithm>
#include <map>

namespace cascade::input {

// ---------------------------------------------------------------- OverlappedReader

struct OverlappedReader::State {
    HANDLE h = INVALID_HANDLE_VALUE;
    HANDLE readEv = nullptr;   // manual-reset, for the OVERLAPPED
    HANDLE stopEv = nullptr;   // manual-reset, set by stop()
    HANDLE doneEv = nullptr;   // manual-reset, set when the worker is finished with the handle, buffer and OVERLAPPED
    std::vector<std::uint8_t> buf;
    ReportFn onReport;
    LostFn onLost;
    std::atomic<bool> stopping{false};
    std::atomic<bool> abandoned{false};  // stop() gave up: no callback may run any more
    std::thread th;
    bool started = false;
};

namespace {

void workerMain(std::shared_ptr<OverlappedReader::State> st);

}  // namespace

OverlappedReader::OverlappedReader() = default;
OverlappedReader::~OverlappedReader() { stop(); }

bool OverlappedReader::running() const {
    return st_ && st_->started && WaitForSingleObject(st_->doneEv, 0) != WAIT_OBJECT_0;
}

bool OverlappedReader::start(HANDLE handle, std::size_t bufLen, ReportFn onReport, LostFn onLost) {
    if (st_) { stop(); }
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr || bufLen == 0) {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) { CloseHandle(handle); }
        return false;
    }
    auto st = std::make_shared<State>();
    st->h = handle;
    st->buf.assign(bufLen, 0);
    st->onReport = std::move(onReport);
    st->onLost = std::move(onLost);
    st->readEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    st->stopEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    st->doneEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!st->readEv || !st->stopEv || !st->doneEv) {
        if (st->readEv) { CloseHandle(st->readEv); }
        if (st->stopEv) { CloseHandle(st->stopEv); }
        if (st->doneEv) { CloseHandle(st->doneEv); }
        CloseHandle(handle);
        return false;
    }
    st_ = st;
    st->started = true;
    st->th = std::thread([st] { workerMain(st); });
    return true;
}

bool OverlappedReader::stop(unsigned timeoutMs) {
    if (!st_) { return true; }
    std::shared_ptr<State> st = std::move(st_);
    st_.reset();
    st->stopping = true;
    SetEvent(st->stopEv);
    const bool done = WaitForSingleObject(st->doneEv, timeoutMs) == WAIT_OBJECT_0;
    if (done) {
        if (st->th.joinable()) { st->th.join(); }
        CloseHandle(st->readEv);
        CloseHandle(st->stopEv);
        CloseHandle(st->doneEv);
        return true;
    }
    // The worker is stuck in the driver: leave it its state (it still holds the buffer and OVERLAPPED the kernel may write), forbid callbacks, detach.
    st->abandoned = true;
    if (st->th.joinable()) { st->th.detach(); }
    new std::shared_ptr<State>(st);  // deliberate leak: the state must outlive a completion that may never come
    return false;
}

namespace {

void workerMain(std::shared_ptr<OverlappedReader::State> st) {
    auto deliver = [&](DWORD n) {
        if (!st->abandoned.load() && st->onReport && n > 0) { st->onReport(st->buf.data(), n); }
    };
    auto lost = [&](DWORD err) {
        if (!st->stopping.load() && !st->abandoned.load() && st->onLost) { st->onLost(err); }
    };
    while (!st->stopping.load()) {
        OVERLAPPED ov{};
        ov.hEvent = st->readEv;
        ResetEvent(st->readEv);
        DWORD n = 0;
        const BOOL ok = ReadFile(st->h, st->buf.data(), static_cast<DWORD>(st->buf.size()), nullptr, &ov);
        if (!ok) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) { lost(err); break; }
            HANDLE w[2] = {st->readEv, st->stopEv};
            const DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0 + 1) {  // stop: cancel the pending read and wait for the kernel to be finished with ov and buf
                CancelIoEx(st->h, &ov);
                if (WaitForSingleObject(st->readEv, 1000) != WAIT_OBJECT_0) {
                    CloseHandle(st->h);  // closing the handle cancels whatever is left on it
                    st->h = INVALID_HANDLE_VALUE;
                    if (WaitForSingleObject(st->readEv, 1000) != WAIT_OBJECT_0) {
                        // Never completed: ov and buf are still the kernel's. Park here (state is leaked by stop()); doneEv is NOT set.
                        while (!st->abandoned.load()) { Sleep(50); }
                        Sleep(INFINITE);
                    }
                }
                break;
            }
            if (!GetOverlappedResult(st->h, &ov, &n, FALSE)) {
                const DWORD e2 = GetLastError();
                if (e2 == ERROR_OPERATION_ABORTED && st->stopping.load()) { break; }
                lost(e2);
                break;
            }
        } else if (!GetOverlappedResult(st->h, &ov, &n, FALSE)) {
            lost(GetLastError());
            break;
        }
        deliver(n);
    }
    if (st->h != INVALID_HANDLE_VALUE) { CloseHandle(st->h); st->h = INVALID_HANDLE_VALUE; }
    SetEvent(st->doneEv);
}

}  // namespace

// ---------------------------------------------------------------- enumeration

namespace {

std::string toUtf8(const wchar_t* w) {
    if (!w || !*w) { return {}; }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return {}; }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::size_t minReportBytes(HidKind k) { return k == HidKind::ShuttleXpress ? 6 : 3; }  // payload (5 / 2) + the report ID byte

}  // namespace

HidEnumeration enumerateHid() {
    HidEnumeration out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { return out; }
    std::map<std::pair<std::uint16_t, std::uint16_t>, int> units;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA ifd{};
        ifd.cbSize = sizeof ifd;
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd)) { break; }
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) { continue; }
        std::vector<std::uint8_t> raw(need);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) { continue; }
        // Access 0: enough for attributes and capabilities, and allowed even for collections Windows keeps exclusive.
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { ++out.unreadable; continue; }
        HIDD_ATTRIBUTES at{};
        at.Size = sizeof at;
        const bool haveAt = HidD_GetAttributes(h, &at) != FALSE;
        const HidProduct* prod = haveAt ? findHidProduct(at.VendorID, at.ProductID) : nullptr;
        if (!prod) {
            CloseHandle(h);
            if (haveAt) { ++out.otherDevices; } else { ++out.unreadable; }
            continue;
        }
        HidDeviceInfo info;
        info.vendor = at.VendorID;
        info.product = at.ProductID;
        info.kind = prod->kind;
        info.name = prod->name;
        info.path = det->DevicePath;
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS) {
                info.usagePage = caps.UsagePage;
                info.usage = caps.Usage;
                info.inputReportBytes = caps.InputReportByteLength;
            }
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(h);
        info.plausible = info.inputReportBytes >= minReportBytes(prod->kind);
        int& unit = units[{info.vendor, info.product}];
        info.stableId = stableHidId(info.vendor, info.product, info.plausible ? unit : 0);
        if (info.plausible) { ++unit; }
        out.supported.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

// ---------------------------------------------------------------- HidInputDevice

bool HidInputDevice::attach(HANDLE h, HidKind kind, const std::string& stableId, std::size_t reportBytes, EventQueue& q) {
    close();
    const std::uint32_t dev = deviceIdFor(stableId);
    shuttle_ = std::make_unique<ShuttleXpressParser>(dev);
    powermate_ = std::make_unique<PowerMateParser>(dev);
    reports_ = 0;
    rejected_ = 0;
    EventQueue* queue = &q;
    return reader_.start(
        h, reportBytes,
        [this, kind, queue](const std::uint8_t* d, std::size_t n) {
            if (n < 2) { ++rejected_; return; }
            ++reports_;
            ParsedEvents pe;
            const HidParseResult r = kind == HidKind::ShuttleXpress ? shuttle_->parse(d + 1, n - 1, nowMicros(), pe)  // byte 0 is the report ID
                                                                    : powermate_->parse(d + 1, n - 1, nowMicros(), pe);
            if (r != HidParseResult::Ok) { ++rejected_; return; }
            for (std::size_t i = 0; i < pe.n; ++i) { queue->push(pe.e[i]); }
        },
        [dev, queue](unsigned) {
            InputEvent e;
            e.device = dev;
            e.kind = EventKind::Disconnected;
            e.timeUs = nowMicros();
            queue->push(e);
        });
}

bool HidInputDevice::open(const HidDeviceInfo& info, EventQueue& q, std::string& error) {
    HANDLE h = CreateFileW(info.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        error = "cannot open " + info.name + " for reading (Windows error " + std::to_string(e) + (e == ERROR_ACCESS_DENIED ? ", access denied: Windows keeps this collection exclusive or another program has it" : "") + ")";
        return false;
    }
    if (!info.plausible || info.inputReportBytes == 0) {
        CloseHandle(h);
        error = "this interface of " + info.name + " is too short to carry the knob's report";
        return false;
    }
    if (!attach(h, info.kind, info.stableId, info.inputReportBytes, q)) {
        error = "could not start the reader";
        return false;
    }
    return true;
}

bool HidInputDevice::close(unsigned timeoutMs) { return reader_.stop(timeoutMs); }

}  // namespace cascade::input

#endif  // _WIN32
