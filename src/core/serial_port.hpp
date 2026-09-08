// serial_port.hpp - the platform serial port, and the byte source the GPS
// reader is written against.
//
// WHY AN INTERFACE. GpsReader (gps_reader.hpp) needs bytes from somewhere and
// must be tested on a machine with no GPS and no COM port at all - which is
// this one. ByteSource is the whole of what the reader asks of a port: read
// some bytes, or say that nothing arrived, or say that the device is gone.
// A test implements it in twenty lines and feeds sentences, fragments and
// garbage on a schedule; the application hands the reader a SerialPort. The
// reader cannot tell the difference, which is the point.
//
// WHY "\\.\" AND WHY A FULL PATH IS ACCEPTED AS TYPED. Windows opens COM1-9
// by their bare name but COM10 and above only through the "\\.\COM10" device
// path, so the prefix is added whenever it is missing. A name that ALREADY
// starts with "\\" is passed through untouched - and that is not a
// convenience, it is the test seam: "\\.\pipe\foxsdr-gps-test" is a named
// pipe, CreateFileW opens it exactly as it opens a port, and a test can then
// sit on the server end and be the GPS. That is the only way the whole path
// from bytes on a device to a persisted receiver position can be run
// end-to-end on this bench.
//
// WHICH IS WHY COMM SETUP FAILURES ARE WARNINGS. SetCommState and
// SetCommTimeouts fail on a pipe, because it is not a communications
// resource. Treating that as an open failure would make the test seam
// useless, so a failure of either after a successful CreateFile is written to
// the diagnostic log as a warning and the port stays open. On a real COM port
// those calls succeed; on the one class of device where they do not, the
// device already has no baud rate to set.
//
// WHY READS ARE OVERLAPPED ON WINDOWS. A COM port honours COMMTIMEOUTS, so a
// plain ReadFile would return after the timeout. A pipe honours nothing: a
// synchronous ReadFile on it blocks until the far end writes, and the reader's
// stop() would then hang until the test felt like feeding it - the exact
// "stop() must return promptly" property gps_reader.hpp promises. Opening with
// FILE_FLAG_OVERLAPPED and waiting on the read's event with a timeout gives
// the same bounded read on both kinds of device, and CancelIoEx tidies up the
// one that timed out. The Linux side gets the same bound from poll(2) on a
// non-blocking descriptor, which also works for a FIFO.
//
// ENUMERATION reads HKLM\HARDWARE\DEVICEMAP\SERIALCOMM on Windows - every
// value there is a live port name ("COM3"), which is how Device Manager knows
// - and the /dev/ttyUSB*, /dev/ttyACM*, /dev/ttyS* nodes that exist on Linux.
// The registry reading is a thin wrapper; the ORDERING is the pure function
// sortSerialPortNames(), because "COM10" sorting before "COM2" is the sort
// of thing a user notices and a test can pin without a registry in the room.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SERIAL_PORT_HPP
#define CASCADE_CORE_SERIAL_PORT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core {

// Something bytes can be read from with a bounded wait.
//
// read() returns the number of bytes placed in `buf` (1..cap), 0 when the
// wait expired with nothing to deliver, or a NEGATIVE value when the source
// is finished - device unplugged, pipe closed by its server, descriptor error.
// A negative return is terminal: the caller must not read again. The wait
// bound is the implementation's (SerialPort: kReadTimeoutMs); the contract is
// only that it is SHORT, because GpsReader::stop() joins a thread that may be
// inside this call.
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual int read(char* buf, std::size_t cap) = 0;
};

// Baud rates a GPS receiver is plausibly set to. The config sanitiser and the
// GUI's combo both draw on this list, so a rate the port cannot be asked for
// cannot be persisted either. 4800 is the NMEA 0183 default and what an old
// puck ships at; 9600 is what nearly everything ships at now; the rest are
// what a u-blox configured for high-rate output uses.
inline constexpr int kSerialBaudRates[] = {4800, 9600, 19200, 38400, 57600, 115200};
inline constexpr int kDefaultGpsBaud = 9600;

// True when `baud` is one of kSerialBaudRates.
bool serialBaudSupported(int baud);

// A port name as a config file may carry it, reduced to something the port
// layer will accept: leading and trailing whitespace trimmed, any byte
// outside printable ASCII (0x20..0x7E) removed, and the result cut at
// kMaxSerialPortNameChars. Device names are short ASCII on every platform
// this ships on, so anything this strips was never a port. Empty in, empty
// out: an empty name means "no port chosen", never a default port.
inline constexpr std::size_t kMaxSerialPortNameChars = 64;
std::string sanitiseSerialPortName(const std::string& name);

// PURE: what the diagnostic log may call a port. The log rides inside
// uploaded crash reports, and PRIVACY.md promises it never carries the name
// or path of a file - and the port field accepts anything printable, so
// "\\.\C:\Users\alice\gps.nmea" opens (CreateFileW is happy to hand back a
// disk file) and would then be written into three log lines. A port NAME
// identifies nothing about a person: "COM3", "\\.\COM12", "/dev/ttyUSB0".
// Those are returned as they are; anything else - a pipe, a typed path, a
// name the sanitiser would not have produced - is replaced by a placeholder
// that says only what kind of thing it was and how long ("(a typed device
// path, 26 chars)"), and the empty name by "(no port)". The status line on
// screen still shows the real name; it is the LOG this rule is for.
std::string loggableSerialPortName(const std::string& name);

// PURE: dedupes, drops empties, and orders port names the way a person
// expects - "COM2" before "COM10", "ttyUSB0" before "ttyUSB1" before
// "ttyUSB10". The rule is natural order: compare the leading run of
// non-digits case-insensitively, then the trailing number numerically, then
// the whole string as a tiebreak. Input order is irrelevant to the output.
std::vector<std::string> sortSerialPortNames(std::vector<std::string> names);

// The ports present right now, already through sortSerialPortNames(). Empty
// when there are none, and empty (never throwing) when the registry key or
// /dev cannot be read. Windows: the values of
// HKLM\HARDWARE\DEVICEMAP\SERIALCOMM ("COM3"). Linux: /dev/ttyUSB*,
// /dev/ttyACM* and /dev/ttyS* that exist, as full paths.
//
// Not cheap enough to call every frame (a registry open, or a directory
// scan) and not expensive either; the GUI calls it when its combo opens.
std::vector<std::string> enumerateSerialPorts();

// PURE: the name CreateFileW will be given. A name already starting with
// "\\" (a device path, a pipe, a UNC-style "\\?\" path) is returned as typed;
// anything else gets the "\\.\" prefix. Defined on every platform so the
// rule can be tested from one test file; only the Windows open() applies it.
std::string windowsDevicePath(const std::string& name);

// One serial port (or, on Windows, anything CreateFileW opens by path).
//
// Not copyable: it owns a handle. Not movable either, on purpose - the reader
// thread holds a pointer to it, and a port that could move out from under
// that pointer is a use-after-free waiting for a refactor.
class SerialPort final : public ByteSource {
public:
    // The bound on one read(). 200 ms is long against a 1 Hz receiver's
    // sentence gaps (so a fix is not delayed by polling) and short against a
    // person pressing Stop (stop() returns inside this plus a scheduler
    // quantum). Applied through COMMTIMEOUTS on a COM port and through the
    // overlapped wait on everything, so a pipe gets the same bound.
    static constexpr int kReadTimeoutMs = 200;

    SerialPort() = default;
    ~SerialPort() override;  // closes if open
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // Opens `name` at `baud`, 8 data bits, no parity, 1 stop bit, no flow
    // control, and the read bound above. Windows: `name` goes through
    // windowsDevicePath(); Linux: `name` is a path ("/dev/ttyUSB0") used as
    // is. Returns false with `error` filled (human-readable, includes the
    // OS's message: "COM3: The system cannot find the file specified") when
    // the device cannot be opened; a comm-parameter failure on an open
    // device is a logged warning and still returns true (header comment).
    // A baud outside kSerialBaudRates is refused before the OS is asked.
    // Opening an already-open port closes it first.
    bool open(const std::string& name, int baud, std::string& error);

    // Safe on a closed port. After close(), read() returns a negative value.
    void close();
    bool isOpen() const;

    // ByteSource contract: bytes read, 0 on timeout, negative when the device
    // is gone. Must be called from ONE thread at a time (the reader's); it is
    // not made re-entrant because nothing needs it to be.
    int read(char* buf, std::size_t cap) override;

    // The name given to open(), for status lines ("COM3", never the expanded
    // "\\.\COM3" - that is the OS's spelling, not the user's). Empty when
    // closed.
    const std::string& name() const { return name_; }
    int baud() const { return baud_; }

private:
    std::string name_;
    int baud_ = 0;
    // Windows: a HANDLE (INVALID_HANDLE_VALUE is -1, hence the sentinel);
    // Linux: a file descriptor. Kept as an integer so this header pulls in
    // neither <windows.h> nor <termios.h>.
    std::intptr_t handle_ = -1;
    // Windows only: the manual-reset event the overlapped read signals.
    // Created with the port, destroyed with it.
    std::intptr_t readEvent_ = -1;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_SERIAL_PORT_HPP
