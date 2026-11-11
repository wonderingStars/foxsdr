// serial_port_win.hpp - the THIN real-port wrapper for tinysa_protocol.hpp's
// SerialLink: a Windows COM port opened in raw mode with a short read timeout.
//
// UNVERIFIED ON HARDWARE and never opened by any test (no tinySA is on the
// desk, and a test must not open a real serial port). Only isValidComPortName,
// which touches nothing, is tested. The port name is never logged or stored by
// this file. Nothing here prompts: CreateFile on a COM name either opens or
// returns an error that is handed back as a false.
//
// The line settings below (raw bytes, 8N1, no flow control, 115200 baud as a
// placeholder the virtual USB serial port ignores) are NOT taken from the
// tinySA page, which states no baud rate; a virtual COM port over USB takes
// the host's rate without caring. Whether the device needs more (DTR) is
// UNVERIFIED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "source/tinysa_protocol.hpp"

namespace cascade::source::tinysa {

// "COM1".."COM256" or "\\.\COM1".."\\.\COM256", nothing else (no paths, no
// devices of another kind). The only way a port name reaches CreateFile.
bool isValidComPortName(const std::string& name);

class WinSerialLink final : public SerialLink {
public:
    WinSerialLink() = default;
    ~WinSerialLink() override;
    WinSerialLink(const WinSerialLink&) = delete;
    WinSerialLink& operator=(const WinSerialLink&) = delete;

    // false for an invalid name, a port that will not open, or a non-Windows build.
    bool open(const std::string& name, unsigned readTimeoutMs = 100);
    void close();
    bool isOpen() const { return handle_ != nullptr; }

    bool write(const std::uint8_t* data, std::size_t n) override;
    long read(std::uint8_t* buf, std::size_t cap) override;

private:
    void* handle_ = nullptr;
};

}  // namespace cascade::source::tinysa
