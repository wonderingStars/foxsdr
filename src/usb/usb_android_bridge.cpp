// usb_android_bridge.cpp - the extern "C" wrapper usb_android_bridge.h
// declares, translating a JNI translation unit's plain-C call into the C++
// adopted-device registry in usbfs_device.hpp. See that header for the
// registry's own contract (thread-safety, what an empty desktop-Linux
// registry means, the fd-ownership decision) and usb_android_bridge.h for
// why this boundary is plain C rather than exposing the C++ types directly.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "usb/usb_android_bridge.h"

#if !defined(__linux__)
#error "usb_android_bridge.cpp is Linux(/Android)-only; CMakeLists.txt must not compile it elsewhere"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "usb/usb_device.hpp"
#include "usb/usbfs_device.hpp"

namespace {

// Heap-allocates a copy of `s` with std::malloc, matching
// foxsdr_usb_free_string()'s std::free() - never `new`/`delete`, so the
// allocator on both sides of this call is unambiguously the C one, whatever
// C++ runtime either side of the JNI boundary happens to be built with.
char* heapCopy(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (out == nullptr) { return nullptr; }
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

}  // namespace

char* foxsdr_usb_register(int fd, uint16_t vid, uint16_t pid, const char* serial_utf8,
                          const char* product_utf8) {
    if (fd < 0) { return nullptr; }
    cascade::usb::UsbDeviceInfo info;
    info.vid = vid;
    info.pid = pid;
    info.serial = (serial_utf8 != nullptr) ? serial_utf8 : std::string();
    info.description = (product_utf8 != nullptr) ? product_utf8 : std::string();
    // registerAdoptedDevice() overwrites info.path itself ("fd:<fd>"); build
    // the same string here only to hand it back to the caller.
    cascade::usb::registerAdoptedDevice(info, fd);
    char path[32];
    std::snprintf(path, sizeof(path), "fd:%d", fd);
    return heapCopy(path);
}

void foxsdr_usb_unregister(const char* path) {
    if (path == nullptr) { return; }
    cascade::usb::unregisterAdoptedDevice(path);
}

void foxsdr_usb_free_string(char* s) { std::free(s); }

int foxsdr_usb_list_count(void) {
    return static_cast<int>(cascade::usb::adoptedDeviceCount());
}
