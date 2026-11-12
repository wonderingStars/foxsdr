// rfexplorer_serial_win.hpp - the serial link of the RF Explorer sweep source: a Windows COM port opened at the baud rate the device speaks (500000 by
// default; the shared tinySA wrapper serial_port_win.hpp fixes 115200 and core/serial_port.hpp refuses anything above it, so neither can be used).
//
// UNVERIFIED ON HARDWARE: no RF Explorer is on the desk and no test opens a real COM port. What IS tested is this code's own I/O (bounded overlapped reads
// and writes, cancel on timeout, the end of the stream) over a LOCAL NAMED PIPE with the same call sequence, as core/serial_port.cpp does for the GPS
// reader; the comm-state calls (baud, 8N1) are not made on a pipe, so the baud setting itself is never exercised by a test.
//
// The line settings beyond the baud rate (8 data bits, no parity, one stop bit, no flow control, DTR and RTS asserted) are an ASSUMPTION: the maker's
// interface page gives only the baud rate. Port names are validated (COM1..COM256, or "\\.\COMn"); the port name is never logged or stored here.
// Nothing here prompts: CreateFile either opens or returns an error that is handed back as a sentence.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "source/tinysa_protocol.hpp"

namespace cascade::source::rfexplorer {

// The baud rates of the page's code list: 500000, 115200, 57600, 38400, 19200, 9600, 4800, 2400, 1200.
bool isSupportedBaud(int baud);

class RfeSerialLink final : public tinysa::SerialLink {
public:
    RfeSerialLink() = default;
    ~RfeSerialLink() override;
    RfeSerialLink(const RfeSerialLink&) = delete;
    RfeSerialLink& operator=(const RfeSerialLink&) = delete;

    // False with a plain sentence in `error`: an invalid port name or baud, a port that will not open (in use, not there).
    bool open(const std::string& comName, int baud, unsigned readTimeoutMs, std::string& error);
    // THE TEST SEAM: opens a local named pipe whose name starts with "\\.\pipe\foxsdr-rfe-" (nothing else is accepted), in the same overlapped mode.
    bool openTestPipe(const std::string& pipeName, unsigned readTimeoutMs, std::string& error);
    void close();
    bool isOpen() const { return handle_ != -1; }

    bool write(const std::uint8_t* data, std::size_t n) override;
    // Up to `cap` bytes, waiting at most the read timeout: >0 bytes, 0 nothing yet, -1 failed or closed (also the end of a pipe).
    long read(std::uint8_t* buf, std::size_t cap) override;

private:
    bool openPath(const std::string& path, int baud, bool comm, unsigned readTimeoutMs, std::string& error);
    std::intptr_t handle_ = -1;
    std::intptr_t readEvent_ = -1;
    unsigned readTimeoutMs_ = 100;
};

}  // namespace cascade::source::rfexplorer
