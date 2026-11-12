// sys_device_watch.hpp - "a USB radio was plugged in or pulled out": the device-arrival watcher (brief row USB re-plug recovery; FEATURE-GAPS R10 belongs to the
// engine serial lane A, which wires it - this is the standalone half).
//
// WHAT IT DOES. A message-only window on a thread of its own receives WM_DEVICECHANGE for the two device-interface classes the engine already enumerates
// (src/usb/winusb_device.cpp): GUID_DEVINTERFACE_USB_DEVICE {A5DCBF10-6530-11D2-901F-00C04FB951ED} and GUID_DEVINTERFACE_WINUSB {DEE824EF-729B-4A0E-9C14-B7117D33A817}.
// Each arrival or removal is parsed (parseDeviceInterfacePath) and fed to a coalescer: a re-plug is a burst of events (a composite device announces several
// interfaces, and the same radio appears under both classes), so the action fires ONCE after a quiet period (settleMs), and a stream that never goes quiet still fires
// every maxWaitMs. The callback receives the counts only: how many arrivals and removals.
//
// NEVER OPENS OR RESETS A RADIO, and registering for the notification opens nothing: RegisterDeviceNotification with a window handle only asks to be told.
//
// PRIVACY: the interface path carries the device's serial number between the product id and the class GUID. The parser KEEPS ONLY the vendor id, product id and
// class GUID and drops the serial the moment it is read; nothing is stored, logged or put in a report (PRIVACY.md: no device serial numbers).
//
// TESTS inject synthetic notifications by sending the very same WM_DEVICECHANGE message (with a DEV_BROADCAST_DEVICEINTERFACE_W built in memory) to the
// watcher's own window: inject() does exactly that, so the window procedure, the parser and the coalescer are all exercised, with no hardware.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_DEVICE_WATCH_HPP
#define CASCADE_CORE_SYS_DEVICE_WATCH_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace cascade::core::sys {

enum class DeviceChange { Arrival, Removal };

struct DeviceInterfaceInfo {
    bool valid = false;
    unsigned vid = 0;          // vendor id
    unsigned pid = 0;          // product id
    std::string classGuid;     // lower case, no braces: "dee824ef-729b-4a0e-9c14-b7117d33a817"
    bool isUsbRadioClass = false;   // one of the two classes above
};

// "\\?\USB#VID_0BDA&PID_2838#00000001#{a5dcbf10-6530-11d2-901f-00c04fb951ed}" -> vid 0x0BDA, pid 0x2838, the class. Anything that is not a USB interface path with a
// VID and a PID (a monitor, a drive, a path with the wrong shape, an over-long string) is not valid. The serial segment is read past and dropped. Case-insensitive.
DeviceInterfaceInfo parseDeviceInterfacePath(const std::string& path);

struct ArrivalConfig {
    std::int64_t settleMs = 1500;    // quiet time before acting
    std::int64_t maxWaitMs = 10000;  // act at least this often while events keep coming
};

struct ArrivalSummary {
    int arrivals = 0;
    int removals = 0;
};

// The pure coalescer (clock injected; not thread-safe, the watcher locks around it).
class ArrivalCoalescer {
public:
    explicit ArrivalCoalescer(ArrivalConfig cfg = {}) : cfg_(cfg) {}
    // Records one event if its interface is a USB radio class; anything else is ignored. Returns whether it counted.
    bool onEvent(DeviceChange change, const DeviceInterfaceInfo& info, std::int64_t nowMs);
    // The summary once the burst is over (or maxWait is reached), and the coalescer clears; false = nothing due.
    bool due(std::int64_t nowMs, ArrivalSummary& out);
    bool pending() const { return (s_.arrivals + s_.removals) > 0; }

private:
    ArrivalConfig cfg_;
    ArrivalSummary s_;
    std::int64_t first_ = 0, last_ = 0;
};

class DeviceArrivalWatcher {
public:
    using Clock = std::function<std::int64_t()>;
    DeviceArrivalWatcher(ArrivalConfig cfg, std::function<void(const ArrivalSummary&)> onSettled, Clock clock = {});
    ~DeviceArrivalWatcher();
    DeviceArrivalWatcher(const DeviceArrivalWatcher&) = delete;
    DeviceArrivalWatcher& operator=(const DeviceArrivalWatcher&) = delete;

    // Creates the window thread. With registerWithOs the two interface classes are registered (false = events only through inject()).
    // Returns false when the window could not be made or, if asked, the registration was refused.
    bool start(bool registerWithOs);
    void stop();
    bool running() const { return running_.load(); }
    bool osRegistered() const { return registered_.load(); }
    // Sends a WM_DEVICECHANGE (DBT_DEVICEARRIVAL / DBT_DEVICEREMOVECOMPLETE, a device-interface broadcast naming `path`) to the watcher's window and returns
    // once it has been handled. False when the watcher is not running.
    bool inject(DeviceChange change, const std::string& path);
    std::uint64_t eventsSeen() const { return events_.load(); }      // every device-interface broadcast received
    std::uint64_t eventsCounted() const { return counted_.load(); }  // those of a radio class
    std::uint64_t actionsRun() const { return actions_.load(); }

    // Used by the window procedure.
    void handleBroadcast(DeviceChange change, const std::string& path);
    void handleTimer();

private:
    void threadMain();

    ArrivalCoalescer coalescer_;
    std::function<void(const ArrivalSummary&)> onSettled_;
    Clock clock_;
    std::mutex mu_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> registered_{false};
    std::atomic<std::uint64_t> events_{0}, counted_{0}, actions_{0};
    std::mutex startMu_;
    bool startDone_ = false;
    bool startOk_ = false;
    bool wantOs_ = false;
    void* hwnd_ = nullptr;   // HWND, kept opaque here
    std::atomic<bool> stopRequested_{false};
};

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_DEVICE_WATCH_HPP
