// The Linux transport behind usb_device.hpp: sysfs to list, usbfs (the kernel
// USBDEVFS_* ioctls on /dev/bus/usb/BBB/DDD) to talk - and, like the WinUSB
// file this one sits beside, not one line of libusb.
//
// WHY USBFS DIRECTLY AND NOT LIBUSB, recorded here because it was a decision
// made once for the product and both transports carry it. First, licensing:
// libusb-1.0 is LGPL-2.1, and installer/THIRD-PARTY-LICENSES.txt already
// states in full - for the Windows transport - that FoxSDR keeps copyleft out
// of the link entirely; a Linux build that pulled libusb in would make that
// statement false on one platform while it stayed true on the other, which is
// worse than being wrong on both. THIRD-PARTY-LICENSES.txt needs no new entry
// for this file: usbfs is not a library, it is a set of ioctls on a device
// node described by Linux's own (GPL, but not LINKED - a kernel ABI, not
// code) UAPI header <linux/usbdevice_fs.h>, exactly the way <windows.h> and
// WinUSB's ioctls are not "linking against Windows". Second, and the reason
// this decision would have been made even with a permissive-licensed libusb:
// this product's field-crash history is what a vendor's C library did to us
// from inside a thread we did not create and a lock we did not hold, and
// libusb is exactly that - its own event thread, its own hotplug thread on
// some backends, its own opinions about when to reset a device. The contract
// in usb_device.hpp (enumeration never opens a device, the caller owns every
// thread, every wait is bounded) is enforceable against a raw ioctl in a way
// it is not enforceable against a library with its own internal state
// machine. usbfs needs nothing but kernel headers already present in any
// build environment that can compile a kernel module.
//
// THE CONTRACT, exactly as winusb_device.cpp keeps it, mapped onto Linux:
//
//  1. ENUMERATION NEVER TOUCHES AN OPEN DEVICE. Devices are listed by reading
//     /sys/bus/usb/devices/*/{idVendor,idProduct,serial,product,manufacturer,
//     bcdDevice,busnum,devnum} - plain text files sysfs already has, no
//     /dev/bus/usb node is opened and no ioctl is sent to produce the list.
//  2. THE CALLER OWNS THE THREADS. Nothing here spawns anything. A bulk read
//     is one URB submitted with USBDEVFS_SUBMITURB and reaped with
//     USBDEVFS_REAPURBNDELAY on whichever thread calls readBulk(); a control
//     transfer is the single synchronous ioctl USBDEVFS_CONTROL, which the
//     kernel itself times out and cancels - no thread of ours is created to
//     wait on it.
//  3. EVERY WAIT IS BOUNDED. USBDEVFS_CONTROL carries its own timeout in
//     milliseconds and the kernel enforces it; a bulk read is bounded by
//     poll() with the caller's timeout; endBulkStream() discards every
//     outstanding URB and gives the whole ring kAbortDrainWait (below) to
//     come back, exactly as the Windows side does and for the same reason
//     (a buffer the kernel may still be writing into is a worse defect than
//     a leak on a path that only runs once a device has already gone wrong).
//
// THE ONE THING LINUX HAS THAT WINDOWS DOES NOT NEED A STEP FOR: a kernel
// driver already bound to the device. An RTL-SDR that has never been touched
// enumerates under dvb_usb_rtl28xxu, the in-tree DVB driver for this exact
// silicon - Linux's answer to "run Zadig" is USBDEVFS_DISCONNECT_CLAIM, which
// detaches whatever is bound and claims the interface for us in one ioctl,
// so there is no separate "unbound device, go install a driver" state to
// report the way enumerateUnbound() reports one on Windows. That is why
// enumerateUnbound() returns empty here: not because nothing can go wrong,
// but because what goes wrong on Linux (the device node exists and
// DISCONNECT_CLAIM still fails - almost always a permissions problem the
// udev rule below fixes, not a missing driver) is a fact about ONE open
// attempt, and openWinUsb()'s own error message says so directly rather than
// through a second enumeration pass.
//
// installer/linux/99-foxsdr-sdr.rules grants the running user's session
// access to these device nodes (TAG+="uaccess"); without it every open()
// below fails EACCES for a desktop user, which is a permissions problem and
// is reported as one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "usb/usb_device.hpp"
#include "usb/usbfs_device.hpp"

#if !defined(__linux__)
#error "usbfs_device.cpp is Linux-only; CMakeLists.txt must not compile it elsewhere"
#endif

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <dirent.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cascade::usb {

namespace {

// THE ONE BOUNDED WAIT THIS FILE PERFORMS, matching winusb_device.cpp's
// kAbortDrainWait exactly: endBulkStream() gives the WHOLE cancelled ring
// this long to come back, not this long per request, before it gives up and
// leaks it. Registered in tests/test_shutdown_budget.cpp (which finds this
// declaration by name whatever platform it is built on, since that scan
// reads source text rather than compiling it) alongside the Windows one.
constexpr std::chrono::milliseconds kAbortDrainWait{250};

constexpr std::size_t kMaxPacketBytes = 512;
constexpr std::size_t kMaxRingSize = 32;

// The interface every one of these dongles' SDR function lives on: a single
// bulk pair on a device with no other interface to confuse it with. Every
// reference implementation for these four chips (librtlsdr, libhackrf, the
// SDDC ExtIO driver, libmirisdr) claims interface 0 for exactly this reason -
// none of the four exposes a second interface at all. Unverified against a
// real dongle from this machine (none is attached); if a future device on
// this transport ever needs a different interface, this is the one place
// that assumption lives.
constexpr unsigned int kSdrInterface = 0;

std::string trimmed(std::string s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) { ++a; }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) {
        --b;
    }
    return s.substr(a, b - a);
}

std::string errnoText(const char* what, int err) {
    char msg[256];
    msg[0] = '\0';
#if defined(__GLIBC__)
    const char* s = ::strerror_r(err, msg, sizeof(msg));
    char buf[300];
    std::snprintf(buf, sizeof(buf), "%s failed: %s (errno %d)", what, s, err);
    return std::string(buf);
#else
    ::strerror_r(err, msg, sizeof(msg));
    char buf[300];
    std::snprintf(buf, sizeof(buf), "%s failed: %s (errno %d)", what, msg, err);
    return std::string(buf);
#endif
}

std::string fdPath(int fd) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "fd:%d", fd);
    return buf;
}

// ---------------------------------------------------------------------------
// THE ADOPTED-DEVICE REGISTRY. See usbfs_device.hpp for the API contract;
// this is just the storage behind it. A plain mutex-guarded vector: the
// number of entries is the number of SDRs one phone has plugged in, never
// more than a handful, so there is no case here where linear search costs
// anything worth a map.
// ---------------------------------------------------------------------------

struct AdoptedEntry {
    UsbDeviceInfo info;  // info.path is always "fd:<fd>" - see registerAdoptedDevice()
    int fd = -1;
};

std::mutex& adoptedMutex() {
    static std::mutex m;
    return m;
}

std::vector<AdoptedEntry>& adoptedRegistry() {
    static std::vector<AdoptedEntry> registry;
    return registry;
}

// Numeric compare on the trailing digits of an "fd:<n>" path so a listing
// with fd 9 and fd 10 in it sorts as a person would read it, not as the
// strings "fd:10" < "fd:9" would; falls back to plain string compare for a
// sysfs path (or anything else), which is exactly the comparison
// sysfsUsbNodesToDevices() already sorts by, unchanged.
bool devicePathLess(const UsbDeviceInfo& a, const UsbDeviceInfo& b) {
    constexpr const char* kPrefix = "fd:";
    constexpr std::size_t kPrefixLen = 3;
    const bool aFd = a.path.rfind(kPrefix, 0) == 0;
    const bool bFd = b.path.rfind(kPrefix, 0) == 0;
    if (aFd && bFd) {
        const long an = std::strtol(a.path.c_str() + kPrefixLen, nullptr, 10);
        const long bn = std::strtol(b.path.c_str() + kPrefixLen, nullptr, 10);
        if (an != bn) { return an < bn; }
        return a.path < b.path;
    }
    return a.path < b.path;
}

}  // namespace

void registerAdoptedDevice(UsbDeviceInfo info, int fd) {
    info.path = fdPath(fd);
    std::lock_guard<std::mutex> lk(adoptedMutex());
    std::vector<AdoptedEntry>& registry = adoptedRegistry();
    for (AdoptedEntry& e : registry) {
        if (e.fd == fd) {
            // A stale registration for a reused fd number: replace it rather
            // than accumulate a duplicate entry two enumerations would both
            // report.
            e.info = std::move(info);
            return;
        }
    }
    registry.push_back(AdoptedEntry{std::move(info), fd});
}

void unregisterAdoptedDevice(const std::string& path) {
    std::lock_guard<std::mutex> lk(adoptedMutex());
    std::vector<AdoptedEntry>& registry = adoptedRegistry();
    registry.erase(std::remove_if(registry.begin(), registry.end(),
                                   [&](const AdoptedEntry& e) { return e.info.path == path; }),
                   registry.end());
}

std::size_t adoptedDeviceCount() {
    std::lock_guard<std::mutex> lk(adoptedMutex());
    return adoptedRegistry().size();
}

// ---------------------------------------------------------------------------
// THE WALK: reading /sys/bus/usb/devices (or a fixture standing in for it)
// without opening a single device. See usbfs_device.hpp for why this is
// split from the decision that turns it into a UsbDeviceInfo list.
// ---------------------------------------------------------------------------

namespace {

// One text file under a sysfs node, trimmed, or empty if it does not exist -
// which is the ordinary case for `serial` on most of these dongles, not an
// error.
std::string readSysfsAttr(const std::string& dir, const char* name) {
    const std::string path = dir + "/" + name;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) { return std::string(); }
    char buf[256] = {0};
    const std::size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return trimmed(std::string(buf));
}

bool parseHex16(const std::string& s, std::uint16_t& out) {
    if (s.empty()) { return false; }
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 16);
    if (end == s.c_str() || *end != '\0' || v < 0 || v > 0xFFFF) { return false; }
    out = static_cast<std::uint16_t>(v);
    return true;
}

bool parseUnsigned(const std::string& s, unsigned& out) {
    if (s.empty()) { return false; }
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || v < 0) { return false; }
    out = static_cast<unsigned>(v);
    return true;
}

}  // namespace

std::vector<SysfsUsbNode> readSysfsUsbNodes(const std::string& sysfsDevicesDir) {
    std::vector<SysfsUsbNode> out;
    DIR* dir = ::opendir(sysfsDevicesDir.c_str());
    // A MISSING DIRECTORY IS AN EMPTY BUS, NOT AN ERROR: a container or a VM
    // with no USB subsystem exposed (this build machine, measured) has no
    // /sys/bus/usb/devices at all, and "no radio hardware found" is the
    // right answer for that, not a crash.
    if (dir == nullptr) { return out; }
    for (struct dirent* e = ::readdir(dir); e != nullptr; e = ::readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") { continue; }
        const std::string path = sysfsDevicesDir + "/" + name;
        SysfsUsbNode node;
        node.name = name;
        std::uint16_t vid = 0;
        std::uint16_t pid = 0;
        node.hasIds = parseHex16(readSysfsAttr(path, "idVendor"), vid) &&
                      parseHex16(readSysfsAttr(path, "idProduct"), pid);
        node.vid = vid;
        node.pid = pid;
        node.serial = readSysfsAttr(path, "serial");
        node.product = readSysfsAttr(path, "product");
        node.manufacturer = readSysfsAttr(path, "manufacturer");
        std::uint16_t bcd = 0;
        if (parseHex16(readSysfsAttr(path, "bcdDevice"), bcd)) { node.bcdDevice = bcd; }
        unsigned busnum = 0;
        unsigned devnum = 0;
        node.hasBusDev = parseUnsigned(readSysfsAttr(path, "busnum"), busnum) &&
                         parseUnsigned(readSysfsAttr(path, "devnum"), devnum);
        node.busnum = busnum;
        node.devnum = devnum;
        out.push_back(std::move(node));
    }
    ::closedir(dir);
    return out;
}

std::vector<UsbDeviceInfo> sysfsUsbNodesToDevices(const std::vector<SysfsUsbNode>& nodes,
                                                   const std::vector<UsbId>& ids) {
    std::vector<UsbDeviceInfo> out;
    if (ids.empty()) { return out; }
    for (const SysfsUsbNode& n : nodes) {
        // AN INTERFACE CHILD HAS NEITHER FILE: a device directory's own
        // "1-2" node carries idVendor/idProduct/busnum/devnum; an interface
        // child "1-2:1.0" carries none of them (they belong to the parent),
        // so hasIds/hasBusDev being false is what tells the two apart - no
        // name-parsing (":" splitting) needed at all.
        if (!n.hasIds || !n.hasBusDev) { continue; }
        bool matched = false;
        for (const UsbId& id : ids) {
            if (id.vid == n.vid && id.pid == n.pid) {
                matched = true;
                break;
            }
        }
        if (!matched) { continue; }
        UsbDeviceInfo info;
        info.vid = n.vid;
        info.pid = n.pid;
        info.serial = n.serial;
        // The bus-reported description, same field enumerateWinUsb() fills
        // from DEVPKEY_Device_BusReportedDeviceDesc: the device's own
        // product string, falling back to "manufacturer product" when only
        // manufacturer is missing a value worth showing alone.
        info.description = n.product;
        if (info.description.empty() && !n.manufacturer.empty()) {
            info.description = n.manufacturer;
        }
        char path[64];
        std::snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u", n.busnum, n.devnum);
        info.path = path;
        out.push_back(std::move(info));
    }
    // Stable order, the same reason enumerateWinUsb() sorts on Windows: two
    // calls in the same session must agree on what "index=0" means.
    std::sort(out.begin(), out.end(),
              [](const UsbDeviceInfo& a, const UsbDeviceInfo& b) { return a.path < b.path; });
    return out;
}

std::vector<UsbDeviceInfo> enumerateWinUsb(const std::vector<UsbId>& ids) {
    if (ids.empty()) { return std::vector<UsbDeviceInfo>(); }
    std::vector<UsbDeviceInfo> out =
        sysfsUsbNodesToDevices(readSysfsUsbNodes("/sys/bus/usb/devices"), ids);
    // THE ANDROID HALF: on desktop Linux this registry is always empty (see
    // usbfs_device.hpp), so this loop appends nothing and `out` is exactly
    // what the line above produced - the sysfs-only behaviour this file has
    // always had. On Android it is the ONLY source of devices, since
    // /sys/bus/usb/devices does not exist for an app to walk there either.
    {
        std::lock_guard<std::mutex> lk(adoptedMutex());
        for (const AdoptedEntry& e : adoptedRegistry()) {
            for (const UsbId& id : ids) {
                if (id.vid == e.info.vid && id.pid == e.info.pid) {
                    out.push_back(e.info);
                    break;
                }
            }
        }
    }
    // Re-sort with the fd-aware comparator (see devicePathLess()) rather
    // than the plain string sort sysfsUsbNodesToDevices() already applied to
    // its own half: a mixed sysfs+adopted list (never expected in practice -
    // a phone has no sysfs half - but not forbidden either) still needs one
    // consistent order.
    std::sort(out.begin(), out.end(), devicePathLess);
    return out;
}

std::vector<UsbDeviceInfo> enumerateUnbound(const std::vector<UsbId>&) {
    // See the file header: on Linux, USBDEVFS_DISCONNECT_CLAIM (in
    // openWinUsb() below) detaches whatever kernel driver - dvb_usb_rtl28xxu
    // and friends - is already bound, so there is no separate "present but
    // needs a driver installed" state the way an un-Zadig'd dongle has on
    // Windows. What CAN still fail is a permissions problem on the device
    // node (no udev rule, not in `plugdev`), and that is reported directly
    // by openWinUsb()'s own error rather than through a second enumeration
    // list a user would have no separate UI for anyway.
    return std::vector<UsbDeviceInfo>();
}

// ---------------------------------------------------------------------------
// THE DEVICE ITSELF.
// ---------------------------------------------------------------------------

namespace {

class UsbfsDevice final : public UsbDevice {
public:
    UsbfsDevice(int fd, std::string path) : fd_(fd), path_(std::move(path)) {}

    ~UsbfsDevice() override {
        endBulkStream();
        if (fd_ >= 0) {
            unsigned int iface = kSdrInterface;
            ::ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &iface);
            ::close(fd_);
        }
        // Buffers the drain gave up on are deliberately never freed; see the
        // file header on endBulkStream() and winusb_device.cpp's identical
        // reasoning.
    }

    UsbfsDevice(const UsbfsDevice&) = delete;
    UsbfsDevice& operator=(const UsbfsDevice&) = delete;

    int controlOut(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                   std::uint16_t index, const std::uint8_t* data, std::size_t len,
                   unsigned timeoutMs) override {
        return control(requestType, request, value, index, const_cast<std::uint8_t*>(data), len,
                       timeoutMs);
    }

    int controlIn(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                  std::uint16_t index, std::uint8_t* data, std::size_t len,
                  unsigned timeoutMs) override {
        return control(requestType, request, value, index, data, len, timeoutMs);
    }

    bool beginBulkStream(std::uint8_t endpoint, std::size_t bufferBytes,
                         std::size_t bufferCount) override {
        if (streaming()) { return true; }
        if (fd_ < 0) {
            setError("beginBulkStream() on a closed device");
            return false;
        }
        // The same rule the fake enforces and the Windows side enforces: a
        // buffer that is not a whole number of maximum-size packets is
        // refused up front rather than discovered as a short, silently
        // truncated read.
        if (bufferBytes == 0 || (bufferBytes % kMaxPacketBytes) != 0) {
            setError("beginBulkStream() needs a buffer size that is a multiple of 512 bytes");
            return false;
        }
        if (bufferCount == 0 || bufferCount > kMaxRingSize) {
            setError("beginBulkStream() needs between 1 and 32 buffers");
            return false;
        }

        // THE PIPE IS CLEARED BEFORE THE FIRST READ, matching
        // WinUsb_ResetPipe's role on the Windows side: whatever the previous
        // owner of this node left behind - a halted endpoint, a data toggle
        // out of step - survives a close, and CLEAR_HALT resynchronises both.
        // Best-effort and unchecked, exactly as the Windows call is: a pipe
        // that was never halted fails this harmlessly.
        resetPipe(endpoint);

        endpoint_ = endpoint;
        ring_.clear();
        ring_.resize(bufferCount);
        for (std::size_t i = 0; i < bufferCount; ++i) {
            ring_[i] = std::make_unique<Request>();
            ring_[i]->buffer.assign(bufferBytes, 0);
        }
        head_ = 0;
        streaming_ = true;
        for (std::size_t i = 0; i < ring_.size(); ++i) {
            if (!queue(*ring_[i])) {
                endBulkStream();
                return false;
            }
        }
        return true;
    }

    int readBulk(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        if (!streaming_) {
            setError("readBulk() with no stream");
            return -1;
        }
        Request& req = *ring_[head_];
        if (!req.pending) {
            if (!queue(req)) { return -1; }
        }

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) { return 0; }  // a quiet pipe is a timeout, not an error
            const long remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          deadline - now)
                                          .count();
            struct pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            // usbfs's own documented convention (also how libusb's usbfs
            // backend has always driven this fd): the device file reports
            // POLLOUT exactly when at least one submitted URB has completed
            // and is ready to reap - it carries no data itself, so there is
            // nothing to read from it.
            const int pr = ::poll(&pfd, 1, static_cast<int>(std::min<long>(remainingMs, 60000)));
            if (pr < 0) {
                if (errno == EINTR) { continue; }
                setError(errnoText("polling a bulk read", errno));
                return -1;
            }
            if (pr == 0) { continue; }  // re-check the deadline at the top

            // Reap everything currently ready. Bulk completions on one
            // endpoint arrive in submission order (a USB pipe is a single
            // ordered stream), so ring_[head_] is expected to be among the
            // first reaped; the loop still drains every ready urb before
            // deciding, so a slower reap under load never leaves one behind
            // to be reaped as garbage by a later call.
            for (;;) {
                struct usbdevfs_urb* done = nullptr;
                if (::ioctl(fd_, USBDEVFS_REAPURBNDELAY, &done) != 0) {
                    if (errno == EAGAIN) { break; }
                    setError(errnoText("reaping a bulk read", errno));
                    return -1;
                }
                for (auto& slot : ring_) {
                    if (slot && &slot->urb == done) {
                        slot->pending = false;
                        break;
                    }
                }
            }
            if (!req.pending) { break; }
        }

        if (req.urb.status != 0) {
            // A device that has gone (unplugged: ENODEV: -19) or stalled
            // (EPIPE: -32) is a NEGATIVE return, never a hang and never a
            // silent zero that reads as an idle radio.
            setError(errnoText("a bulk read", -req.urb.status));
            return -1;
        }
        const std::size_t got =
            std::min<std::size_t>(static_cast<std::size_t>(req.urb.actual_length), cap);
        if (got > 0) { std::memcpy(dst, req.buffer.data(), got); }
        head_ = (head_ + 1) % ring_.size();
        if (!queue(req)) { return -1; }
        return static_cast<int>(got);
    }

    void endBulkStream() override {
        if (!streaming_ && ring_.empty()) { return; }
        streaming_ = false;
        for (auto& slot : ring_) {
            if (slot && slot->pending) { ::ioctl(fd_, USBDEVFS_DISCARDURB, &slot->urb); }
        }
        // ONE deadline for the WHOLE ring, exactly as winusb_device.cpp's
        // comment states for its own kAbortDrainWait.
        const auto deadline = std::chrono::steady_clock::now() + kAbortDrainWait;
        bool allDrained = true;
        for (auto& slot : ring_) {
            if (!slot || !slot->pending) { continue; }
            for (;;) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    allDrained = false;
                    break;
                }
                const long remainingMs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                struct pollfd pfd{};
                pfd.fd = fd_;
                pfd.events = POLLOUT;
                const int pr = ::poll(&pfd, 1, static_cast<int>(std::max<long>(remainingMs, 0)));
                if (pr <= 0) { continue; }  // re-check the deadline; EINTR falls the same way
                struct usbdevfs_urb* done = nullptr;
                if (::ioctl(fd_, USBDEVFS_REAPURBNDELAY, &done) != 0) { continue; }
                for (auto& s : ring_) {
                    if (s && &s->urb == done) { s->pending = false; }
                }
                if (!slot->pending) { break; }
            }
            if (slot->pending) { break; }  // out of time; stop polling, go leak
        }
        if (allDrained) {
            ring_.clear();
        } else {
            // THE LEAK, on purpose - see winusb_device.cpp's identical
            // reasoning: at least one URB may still be live in the kernel
            // with a pointer into slot->buffer, and freeing that buffer
            // under a live DMA write is worse than leaking it.
            for (auto& slot : ring_) {
                if (slot) { leaked_.push_back(std::move(slot)); }
            }
            ring_.clear();
            setError("a discarded bulk read did not complete within the drain bound; its "
                     "buffer is deliberately leaked rather than freed under the device");
        }
        endpoint_ = 0;
        head_ = 0;
    }

    bool streaming() const override { return streaming_; }

    bool resetPipe(std::uint8_t endpoint) override {
        if (fd_ < 0) { return false; }
        unsigned int ep = endpoint;
        if (::ioctl(fd_, USBDEVFS_CLEAR_HALT, &ep) == 0) { return true; }
        // Not every kernel/controller answers CLEAR_HALT on a pipe that was
        // never halted; USBDEVFS_RESETEP resynchronises the data toggle
        // without requiring the halt condition WinUsb_ResetPipe's Linux
        // analogue would otherwise need.
        ep = endpoint;
        if (::ioctl(fd_, USBDEVFS_RESETEP, &ep) == 0) { return true; }
        setError(errnoText("clearing a bulk pipe", errno));
        return false;
    }

    const std::string& path() const override { return path_; }
    const std::string& lastError() const override { return lastError_; }

private:
    struct Request {
        std::vector<std::uint8_t> buffer;
        bool pending = false;
        // MUST BE THE LAST MEMBER: usbdevfs_urb ends in a genuine C99
        // flexible array member (iso_frame_desc[], unused for a bulk
        // transfer but still part of the type), which GCC/Clang only accept
        // as the last field of an enclosing struct.
        struct usbdevfs_urb urb{};
    };

    void setError(std::string msg) { lastError_ = std::move(msg); }

    bool queue(Request& req) {
        std::memset(&req.urb, 0, sizeof(req.urb));
        req.urb.type = USBDEVFS_URB_TYPE_BULK;
        req.urb.endpoint = endpoint_;
        req.urb.buffer = req.buffer.data();
        req.urb.buffer_length = static_cast<int>(req.buffer.size());
        if (::ioctl(fd_, USBDEVFS_SUBMITURB, &req.urb) != 0) {
            setError(errnoText("queueing a bulk read", errno));
            req.pending = false;
            return false;
        }
        req.pending = true;
        return true;
    }

    int control(std::uint8_t requestType, std::uint8_t request, std::uint16_t value,
                std::uint16_t index, std::uint8_t* data, std::size_t len, unsigned timeoutMs) {
        if (fd_ < 0) {
            setError("control transfer on a closed device");
            return -1;
        }
        if (len > 0xFFFF) {
            setError("control transfer longer than a USB setup packet can describe");
            return -1;
        }
        // usbfs serialises control transfers on its own device-wide lock, but
        // the driver's caller-owns-the-threads contract already forbids two
        // threads calling into one UsbDevice at once for control; this mutex
        // is belt to that braces, matching the Windows side.
        std::lock_guard<std::mutex> lk(controlMutex_);
        struct usbdevfs_ctrltransfer ctrl{};
        ctrl.bRequestType = requestType;
        ctrl.bRequest = request;
        ctrl.wValue = value;
        ctrl.wIndex = index;
        ctrl.wLength = static_cast<std::uint16_t>(len);
        ctrl.timeout = timeoutMs;
        ctrl.data = data;
        // USBDEVFS_CONTROL is itself the bounded wait (rule 3): the kernel
        // honours ctrl.timeout, cancelling and returning -ETIMEDOUT on its
        // own if the device does not answer - no poll loop needed here.
        const int r = ::ioctl(fd_, USBDEVFS_CONTROL, &ctrl);
        if (r < 0) {
            const int err = errno;
            if (err == ETIMEDOUT) {
                setError("a control transfer did not answer within its timeout");
            } else if (err == ENODEV) {
                setError("the radio is no longer present");
            } else if (err == EPIPE) {
                setError("a control transfer stalled");
            } else {
                setError(errnoText("a control transfer", err));
            }
            return -1;
        }
        return r;
    }

    int fd_ = -1;
    std::string path_;
    std::string lastError_;

    std::mutex controlMutex_;

    std::uint8_t endpoint_ = 0;
    bool streaming_ = false;
    std::size_t head_ = 0;
    std::vector<std::unique_ptr<Request>> ring_;
    std::vector<std::unique_ptr<Request>> leaked_;
};

}  // namespace

// ---------------------------------------------------------------------------
// ADOPTING AN ANDROID-OPENED FD. See usbfs_device.hpp for the full contract
// (fd ownership, why USBDEVFS_CLAIMINTERFACE and never DISCONNECT_CLAIM here,
// what an EBUSY claim means). This is the one place that reasoning is acted
// on.
// ---------------------------------------------------------------------------
std::unique_ptr<UsbDevice> adoptUsbFd(int fd, std::uint16_t vid, std::uint16_t pid,
                                      const std::string& serial, std::string& error) {
    error.clear();
    if (fd < 0) {
        error = "adoptUsbFd() needs an already-open file descriptor";
        return nullptr;
    }

    // THE OWNERSHIP DECISION: dup, never take the caller's fd value itself -
    // see usbfs_device.hpp's comment on this function for why the Java side
    // needs its own fd to outlive whatever this native UsbfsDevice does.
    const int newFd = ::dup(fd);
    if (newFd < 0) {
        error = errnoText("duplicating the adopted USB file descriptor", errno);
        return nullptr;
    }
    // Match the O_CLOEXEC the sysfs open() path already asks for; a dup()
    // does not inherit the FD_CLOEXEC flag from the fd it was duplicated
    // from; it is our own duplicate to close, not the original, so its exec
    // behaviour is our call to make, and every other fd this file opens is
    // O_CLOEXEC.
    ::fcntl(newFd, F_SETFD, FD_CLOEXEC);

    char idText[32];
    std::snprintf(idText, sizeof(idText), "%04x:%04x", vid, pid);
    const std::string label =
        std::string(idText) + (serial.empty() ? std::string() : (" (serial " + serial + ")"));

    unsigned int iface = kSdrInterface;
    if (::ioctl(newFd, USBDEVFS_CLAIMINTERFACE, &iface) != 0) {
        const int err = errno;
        if (err == EBUSY) {
            // Java's own UsbDeviceConnection.claimInterface() almost
            // certainly already holds it - see usbfs_device.hpp: nothing
            // else on Android binds a kernel driver to an SDR's interface
            // for this ioctl to be contending with. Not a failure; usbfs
            // control/bulk transfers on this fd work regardless of which
            // side of the JNI boundary issued the claim.
            std::fprintf(stderr,
                         "usbfs: interface %u already claimed for %s (fd %d) - assuming "
                         "Android's UsbDeviceConnection did it; proceeding\n",
                         kSdrInterface, label.c_str(), fd);
        } else if (err == ENOTTY) {
            // The one failure this function's own test can produce without
            // real hardware: a pipe or /dev/null answers USBDEVFS_CLAIMINTERFACE
            // with "no such ioctl on this file", which is exactly what "this
            // is not a usbfs fd at all" looks like from here.
            error = "fd " + std::to_string(fd) + " (" + label + ") is not a usbfs device";
            ::close(newFd);
            return nullptr;
        } else {
            error = errnoText(("claiming the USB interface for " + label).c_str(), err);
            ::close(newFd);
            return nullptr;
        }
    }

    return std::make_unique<UsbfsDevice>(newFd, fdPath(fd));
}

std::unique_ptr<UsbDevice> openWinUsb(const std::string& path, std::string& error) {
    error.clear();
    if (path.empty()) {
        error = "openWinUsb() needs a device interface path";
        return nullptr;
    }
    if (path.rfind("fd:", 0) == 0) {
        // AN ADOPTED-DEVICE PATH, not a filesystem path at all: look up what
        // registerAdoptedDevice() recorded for it and hand off to
        // adoptUsbFd() with the vid/pid/serial that came with the
        // registration, rather than opening `path` as a file (which it is
        // not - open() on a string like "fd:37" would just fail ENOENT and
        // report a confusing message for what is really "not registered").
        // The lookup copies the entry and releases the registry lock before
        // calling adoptUsbFd(), so a slow dup()/ioctl() below never holds up
        // a concurrent register/unregister call.
        bool found = false;
        int lookupFd = -1;
        std::uint16_t lookupVid = 0;
        std::uint16_t lookupPid = 0;
        std::string lookupSerial;
        {
            std::lock_guard<std::mutex> lk(adoptedMutex());
            for (const AdoptedEntry& e : adoptedRegistry()) {
                if (e.info.path == path) {
                    found = true;
                    lookupFd = e.fd;
                    lookupVid = e.info.vid;
                    lookupPid = e.info.pid;
                    lookupSerial = e.info.serial;
                    break;
                }
            }
        }
        if (!found) {
            error = "no adopted USB device is registered at " + path +
                    " (it may have been unregistered, or the app restarted without Android "
                    "re-granting permission)";
            return nullptr;
        }
        return adoptUsbFd(lookupFd, lookupVid, lookupPid, lookupSerial, error);
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        if (err == ENOENT) {
            error = "the radio is no longer present";
        } else if (err == EACCES) {
            error = "permission denied opening " + path +
                    " - see installer/linux/99-foxsdr-sdr.rules and installer/linux/README.md";
        } else if (err == EBUSY) {
            error = "the radio is already in use by another program";
        } else {
            error = errnoText(("opening " + path).c_str(), err);
        }
        return nullptr;
    }

    // DETACH WHATEVER KERNEL DRIVER IS BOUND AND CLAIM THE INTERFACE, in one
    // ioctl - Linux's answer to "run Zadig". USBDEVFS_DISCONNECT_CLAIM with
    // EXCEPT_DRIVER "usbfs" means "disconnect the current driver (an
    // RTL-SDR's dvb_usb_rtl28xxu, almost always) unless it is already usbfs,
    // then claim" - the same idiom libusb's own usbfs backend uses, chosen so
    // this transport's behaviour on an out-of-the-box dongle matches what
    // every existing Linux SDR tool already does, rather than requiring a
    // manual `unbind` step the Windows side has no equivalent of.
    struct usbdevfs_disconnect_claim dc{};
    dc.interface = kSdrInterface;
    dc.flags = USBDEVFS_DISCONNECT_CLAIM_EXCEPT_DRIVER;
    std::snprintf(dc.driver, sizeof(dc.driver), "usbfs");
    bool claimed = ::ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &dc) == 0;
    if (!claimed && errno == ENOTTY) {
        // An old kernel without USBDEVFS_DISCONNECT_CLAIM (pre-4.10, not
        // expected on any machine this ships to, and not this build's
        // 6.18 WSL2 kernel - unverified against a genuinely old kernel for
        // lack of one). A device with no kernel driver already attached
        // still opens fine through the plain claim.
        unsigned int iface = kSdrInterface;
        claimed = ::ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) == 0;
    }
    if (!claimed) {
        const int err = errno;
        error = errnoText("claiming the radio's USB interface", err);
        if (err == EBUSY) {
            error = "the radio is already in use by another program or its kernel driver "
                    "could not be detached";
        }
        ::close(fd);
        return nullptr;
    }

    return std::make_unique<UsbfsDevice>(fd, path);
}

}  // namespace cascade::usb
