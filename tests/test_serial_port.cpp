// test_serial_port.cpp - the serial port layer, on a machine with no serial
// port.
//
// The bench this is written on has no COM port ([System.IO.Ports.SerialPort]::
// GetPortNames() returns nothing) and no GPS. So the pure parts - the natural
// sort, the name sanitiser, the device-path prefix rule, the baud list - are
// pinned as tables, and the platform part is exercised through the one device
// this machine CAN provide that CreateFileW opens like a port: a named pipe.
// The test is the server end; SerialPort is the client, and it must open,
// warn (not fail) that the pipe is no communications device, time out a read
// in the promised bound, deliver bytes, and report the server's close as the
// terminal negative. That is the seam tests/test_gps_app.cpp drives through
// the whole application, proved here on its own first.
//
// Also pinned: a real open of a port that does not exist fails with the
// "no such port" wording and does not crash, and a real open of something the
// OS refuses (a directory, which CreateFileW answers with ERROR_ACCESS_DENIED
// - the same code another program holding a COM port produces) is reported
// as "in use by another program".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/serial_port.hpp"

#include "core/diag_log.hpp"
#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace cascade::core;

namespace {

using Names = std::vector<std::string>;

bool ringHas(const std::string& needle) {
    for (const std::string& line : DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { return true; }
    }
    return false;
}

void testBaud() {
    for (const int b : kSerialBaudRates) { CHECK(serialBaudSupported(b)); }
    CHECK(serialBaudSupported(4800));
    CHECK(serialBaudSupported(115200));
    CHECK(!serialBaudSupported(1200));
    CHECK(!serialBaudSupported(0));
    CHECK(!serialBaudSupported(-9600));
    CHECK(!serialBaudSupported(9601));
    static_assert(kDefaultGpsBaud == 9600, "the config sanitiser and the GUI assume 9600");
    CHECK(serialBaudSupported(kDefaultGpsBaud));
}

void testSanitise() {
    CHECK(sanitiseSerialPortName("") == "");
    CHECK(sanitiseSerialPortName("COM3") == "COM3");
    CHECK(sanitiseSerialPortName("  COM3 ") == "COM3");
    CHECK(sanitiseSerialPortName("\tCOM3\r\n") == "COM3");
    CHECK(sanitiseSerialPortName("COM\x01\x02" "3") == "COM3");
    CHECK(sanitiseSerialPortName("COM\x7f" "3") == "COM3");
    // Non-ASCII bytes (an e-acute in UTF-8) are stripped, not mangled.
    CHECK(sanitiseSerialPortName("COM\xc3\xa9" "3") == "COM3");
    CHECK(sanitiseSerialPortName("\x01\x02   ") == "");
    CHECK(sanitiseSerialPortName("/dev/ttyUSB0") == "/dev/ttyUSB0");
    CHECK(sanitiseSerialPortName("\\\\.\\pipe\\foxsdr-gps") == "\\\\.\\pipe\\foxsdr-gps");
    // The cut. 100 printable characters become exactly kMaxSerialPortNameChars.
    const std::string longName(100, 'A');
    CHECK(sanitiseSerialPortName(longName).size() == kMaxSerialPortNameChars);
    CHECK(sanitiseSerialPortName(longName) == std::string(kMaxSerialPortNameChars, 'A'));
    // A space sitting exactly at the cut does not survive as a trailing space.
    const std::string spaceAtCut = std::string(kMaxSerialPortNameChars - 1, 'B') + "  C";
    CHECK(sanitiseSerialPortName(spaceAtCut) == std::string(kMaxSerialPortNameChars - 1, 'B'));
    // Exactly at the limit is untouched.
    const std::string atLimit(kMaxSerialPortNameChars, 'C');
    CHECK(sanitiseSerialPortName(atLimit) == atLimit);
}

// The log may name a port and nothing else. The port field accepts any
// printable text and CreateFileW opens a typed file path exactly as it opens
// a port, so without this rule "\\.\C:\Users\alice\gps.nmea" would reach
// three log lines and the next crash report - and PRIVACY.md promises no
// file path ever does. Port names identify nothing and pass through; every
// other shape becomes a placeholder that carries only its kind and length.
void testLoggableName() {
    CHECK(loggableSerialPortName("COM3") == "COM3");
    CHECK(loggableSerialPortName("com3") == "com3");
    CHECK(loggableSerialPortName("COM12") == "COM12");
    CHECK(loggableSerialPortName("COM256") == "COM256");
    CHECK(loggableSerialPortName("\\\\.\\COM12") == "\\\\.\\COM12");
    CHECK(loggableSerialPortName("\\\\?\\COM3") == "\\\\?\\COM3");
    CHECK(loggableSerialPortName("/dev/ttyUSB0") == "/dev/ttyUSB0");
    CHECK(loggableSerialPortName("/dev/ttyACM12") == "/dev/ttyACM12");
    CHECK(loggableSerialPortName("/dev/ttyS0") == "/dev/ttyS0");
    CHECK(loggableSerialPortName("/dev/rfcomm0") == "/dev/rfcomm0");
    CHECK(loggableSerialPortName("") == "(no port)");
    // A typed path, Windows and Linux, and a by-id node that carries the
    // receiver's serial number: placeholder, with the length so a log reader
    // can still tell two different mistakes apart.
    const std::string typed = "\\\\.\\C:\\Users\\alice\\gps.nmea";
    CHECK(loggableSerialPortName(typed) == "(a typed device path, 27 chars)");
    CHECK(loggableSerialPortName(typed).find("alice") == std::string::npos);
    CHECK(loggableSerialPortName("/home/alice/vgps") == "(a typed device path, 16 chars)");
    CHECK(loggableSerialPortName("/dev/serial/by-id/usb-u-blox_7_-_GPS_GNSS_Receiver-if00")
          == "(a typed device path, 55 chars)");
    CHECK(loggableSerialPortName("/dev/") == "(a typed device path, 5 chars)");
    // The test seam's named pipe is a device path too, not a port: it is
    // logged as one. (Its name is the test's own, but the rule has no way to
    // know that and must not guess.)
    CHECK(loggableSerialPortName("\\\\.\\pipe\\foxsdr-gps-1234") == "(a typed device path, 24 chars)");
    // Near-misses of the COM shape are not ports: no digits, four digits, a
    // letter after the digits, a lone backslash prefix.
    CHECK(loggableSerialPortName("COM") == "(a typed device path, 3 chars)");
    CHECK(loggableSerialPortName("COM1234") == "(a typed device path, 7 chars)");
    CHECK(loggableSerialPortName("COM3x") == "(a typed device path, 5 chars)");
    CHECK(loggableSerialPortName("\\COM3") == "(a typed device path, 5 chars)");
    CHECK(loggableSerialPortName("FAKE1") == "(a typed device path, 5 chars)");
}

void testSort() {
    CHECK(sortSerialPortNames({"COM10", "COM2", "COM1"}) == Names({"COM1", "COM2", "COM10"}));
    CHECK(sortSerialPortNames({"COM1", "COM2", "COM10"}) == Names({"COM1", "COM2", "COM10"}));
    CHECK(sortSerialPortNames({"COM2", "COM10", "COM1"}) == Names({"COM1", "COM2", "COM10"}));
    CHECK(sortSerialPortNames({"/dev/ttyUSB10", "/dev/ttyUSB1", "/dev/ttyUSB0"}) ==
          Names({"/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB10"}));
    // Different prefixes group; within a prefix the number decides.
    CHECK(sortSerialPortNames({"/dev/ttyUSB0", "/dev/ttyS0", "/dev/ttyACM1", "/dev/ttyACM0"}) ==
          Names({"/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyS0", "/dev/ttyUSB0"}));
    // Duplicates collapse, empties vanish.
    CHECK(sortSerialPortNames({"COM3", "COM3", "COM3"}) == Names({"COM3"}));
    CHECK(sortSerialPortNames({"", "COM4", "", "COM3", ""}) == Names({"COM3", "COM4"}));
    CHECK(sortSerialPortNames({"", ""}) == Names({}));
    CHECK(sortSerialPortNames({}) == Names({}));
    // Prefix comparison is case-insensitive; the whole string breaks ties so
    // the output is still deterministic.
    CHECK(sortSerialPortNames({"com2", "COM10", "COM1"}) == Names({"COM1", "com2", "COM10"}));
    // Leading zeros are numbers, not different prefixes.
    CHECK(sortSerialPortNames({"COM010", "COM9"}) == Names({"COM9", "COM010"}));
    // A name without a trailing number sorts before its numbered siblings.
    CHECK(sortSerialPortNames({"COM1", "COM"}) == Names({"COM", "COM1"}));
    // A huge "number" sorts last rather than wrapping to first.
    CHECK(sortSerialPortNames({"COM99999999999999999999999", "COM2"}) ==
          Names({"COM2", "COM99999999999999999999999"}));
}

void testDevicePath() {
    CHECK(windowsDevicePath("COM3") == "\\\\.\\COM3");
    CHECK(windowsDevicePath("COM12") == "\\\\.\\COM12");
    CHECK(windowsDevicePath("\\\\.\\COM12") == "\\\\.\\COM12");
    CHECK(windowsDevicePath("\\\\.\\pipe\\x") == "\\\\.\\pipe\\x");
    CHECK(windowsDevicePath("\\\\?\\COM3") == "\\\\?\\COM3");
    // A single leading backslash is not a device path; it gets the prefix like
    // any other name, and then fails to open like any other bad name.
    CHECK(windowsDevicePath("\\COM3") == "\\\\.\\\\COM3");
}

void testEnumerate() {
    // This machine has no ports, so the only things to pin are that the call
    // returns (does not throw) and that whatever it returns is already in
    // the sorted, deduped, no-empties form the header promises.
    Names ports;
    bool threw = false;
    try {
        ports = enumerateSerialPorts();
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(ports == sortSerialPortNames(ports));
    for (const std::string& p : ports) { CHECK(!p.empty()); }
    std::printf("enumerateSerialPorts: %zu port(s)\n", ports.size());
    for (const std::string& p : ports) { std::printf("  %s\n", p.c_str()); }
}

void testClosedPort() {
    SerialPort port;
    CHECK(!port.isOpen());
    CHECK(port.name().empty());
    CHECK(port.baud() == 0);
    char buf[16];
    CHECK(port.read(buf, sizeof(buf)) < 0);
    port.close();
    port.close();
    CHECK(!port.isOpen());
}

void testBadBaudRefusedBeforeTheOs() {
    // A baud outside the list is refused before CreateFile is asked, so even
    // a name that could never open reports the baud, not the OS.
    SerialPort port;
    std::string err;
    CHECK(!port.open("COM254", 1200, err));
    CHECK(err.rfind("COM254", 0) == 0);
    CHECK(err.find("1200") != std::string::npos);
    CHECK(err.find("no such port") == std::string::npos);
    CHECK(!port.isOpen());
    err.clear();
    CHECK(!port.open("", 9600, err));
    CHECK(!err.empty());
}

void testNoSuchPort() {
    // A port this machine does not have (it has none at all - and COM254 is
    // above anything Windows ever assigns by default). The point is the
    // wording and that nothing crashes.
    Names present = enumerateSerialPorts();
    bool listed = false;
    for (const std::string& p : present) { listed = listed || (p == "COM254"); }
    CHECK(!listed);
    SerialPort port;
    std::string err;
    const bool ok = port.open("COM254", 9600, err);
    CHECK(!ok);
    CHECK(!port.isOpen());
    CHECK(err.rfind("COM254: ", 0) == 0);
#if defined(_WIN32)
    CHECK(err.find("no such port") != std::string::npos);
#else
    // The Linux path opens the name as a path; "COM254" is simply a file that
    // does not exist in the working directory.
    CHECK(err.find("no such port") != std::string::npos);
#endif
    std::printf("open(COM254): %s\n", err.c_str());
    // Failure leaves the port fully closed: a read is terminal, close is safe.
    char buf[8];
    CHECK(port.read(buf, sizeof(buf)) < 0);
    port.close();
}

#if defined(_WIN32)

void testAccessDeniedWording() {
    // CreateFileW refuses a directory opened without FILE_FLAG_BACKUP_SEMANTICS
    // with ERROR_ACCESS_DENIED - the same code a COM port held open by another
    // program produces - so a directory is the one thing on this bench that
    // exercises that mapping for real. The "\\?\" prefix keeps
    // windowsDevicePath's hands off the path.
    // temp_directory_path() carries a trailing separator, and "\\?\C:\x\" is
    // ERROR_PATH_NOT_FOUND rather than the directory - strip it.
    std::string dir = std::filesystem::temp_directory_path().string();
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) { dir.pop_back(); }
    const std::string name = "\\\\?\\" + dir;
    SerialPort port;
    std::string err;
    const bool ok = port.open(name, 9600, err);
    CHECK(!ok);
    CHECK(err.rfind(name + ": ", 0) == 0);
    CHECK(err.find("in use by another program") != std::string::npos);
    std::printf("open(directory): %s\n", err.c_str());
}

// The server end of the pipe the port under test opens as its device.
// Stages: 0 = connected and quiet, 1 = write the sentence, 2 = disconnect.
class PipeServer {
public:
    explicit PipeServer(const std::wstring& path) {
        pipe_ = ::CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096,
                                   0, nullptr);
    }
    ~PipeServer() {
        stage_ = 2;
        if (thread_.joinable()) { thread_.join(); }
        if (pipe_ != INVALID_HANDLE_VALUE) { ::CloseHandle(pipe_); }
    }
    bool valid() const { return pipe_ != INVALID_HANDLE_VALUE; }

    void serve(std::string payload) {
        thread_ = std::thread([this, payload = std::move(payload)]() {
            const BOOL connected = ::ConnectNamedPipe(pipe_, nullptr)
                                       ? TRUE
                                       : (::GetLastError() == ERROR_PIPE_CONNECTED);
            connected_ = connected != 0;
            while (stage_ < 1) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            if (stage_ == 1 && connected_) {
                DWORD written = 0;
                wrote_ = ::WriteFile(pipe_, payload.data(), static_cast<DWORD>(payload.size()),
                                     &written, nullptr) != 0 && written == payload.size();
                ::FlushFileBuffers(pipe_);
            }
            while (stage_ < 2) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
            ::DisconnectNamedPipe(pipe_);
            ::CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        });
    }
    void writeNow() { stage_ = 1; }
    void disconnectNow() { stage_ = 2; }
    bool connected() const { return connected_; }
    bool wrote() const { return wrote_; }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::thread thread_;
    std::atomic<int> stage_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> wrote_{false};
};

void testNamedPipe() {
    using clock = std::chrono::steady_clock;
    const std::string pipeName =
        "\\\\.\\pipe\\foxsdr-serial-" + std::to_string(::GetCurrentProcessId());
    const std::wstring widePath(pipeName.begin(), pipeName.end());
    PipeServer server(widePath);
    CHECK(server.valid());
    if (!server.valid()) { return; }
    // A sentence with a real checksum, though nothing here parses it: the
    // port layer moves bytes and must move exactly these.
    const std::string sentence = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    server.serve(sentence);

    DiagLog::instance().resetForTest();
    SerialPort port;
    std::string err;
    const bool opened = port.open(pipeName, 9600, err);
    CHECK(opened);
    if (!opened) {
        std::printf("open(pipe) failed: %s\n", err.c_str());
        server.disconnectNow();
        return;
    }
    CHECK(port.isOpen());
    CHECK(port.name() == pipeName);
    CHECK(port.baud() == 9600);
    // The pipe is no communications device: the comm setup failed and said so
    // in the log, and the port is open regardless. This is the rule that
    // makes the seam work, pinned. The warning names the device by the
    // loggable rule - a pipe is a device path, not a port, so the log carries
    // the placeholder and never the name (the name of THIS pipe is harmless;
    // the same line with a typed file path in it would not be).
    CHECK(ringHas("serial: " + loggableSerialPortName(pipeName) + ": SetCommState failed"));
    CHECK(ringHas("serial: (a typed device path, "));
    CHECK(!ringHas(pipeName));
    CHECK(!ringHas("foxsdr-serial-"));
    CHECK(ringHas("not a communications device"));

    // Give the server its moment to see the connection.
    for (int i = 0; i < 200 && !server.connected(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(server.connected());

    // A read with nothing to deliver returns 0, and takes the promised bound
    // to do it: neither hanging (a synchronous pipe read would) nor spinning
    // (0 in a microsecond would have the reader thread burn a core).
    char buf[256];
    const auto t0 = clock::now();
    const int quiet = port.read(buf, sizeof(buf));
    const double quietMs =
        std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    CHECK(quiet == 0);
    CHECK(quietMs >= SerialPort::kReadTimeoutMs * 0.5);
    CHECK(quietMs <= SerialPort::kReadTimeoutMs + 100.0);
    std::printf("quiet read: %d in %.1f ms\n", quiet, quietMs);

    // Bytes written by the server come back exactly.
    server.writeNow();
    std::string got;
    for (int i = 0; i < 20 && got.size() < sentence.size(); ++i) {
        const int n = port.read(buf, sizeof(buf));
        if (n < 0) { break; }
        got.append(buf, static_cast<std::size_t>(n));
    }
    // The bytes can be read here before the server thread has stored its
    // own "written" flag (WriteFile returns once the data is in the pipe, and
    // the client wins the race two runs in thirty), so give the flag a
    // bounded moment rather than reading it in the same instant.
    for (int i = 0; i < 200 && !server.wrote(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(server.wrote());
    CHECK(got == sentence);
    std::printf("data read: %zu of %zu bytes\n", got.size(), sentence.size());

    // The server going away is the terminal negative, seen within a few
    // read bounds.
    server.disconnectNow();
    int last = 0;
    for (int i = 0; i < 25; ++i) {
        last = port.read(buf, sizeof(buf));
        if (last < 0) { break; }
    }
    CHECK(last < 0);
    std::printf("after disconnect: %d\n", last);

    // Close twice, then a read is terminal and the name is gone.
    port.close();
    CHECK(!port.isOpen());
    CHECK(port.name().empty());
    port.close();
    CHECK(port.read(buf, sizeof(buf)) < 0);
}

void testReopenClosesFirst() {
    // Opening an already-open port closes it first (header contract). Two
    // pipes: the port must end up on the second, and the first server must
    // see its client go.
    const std::string base =
        "\\\\.\\pipe\\foxsdr-serial-reopen-" + std::to_string(::GetCurrentProcessId());
    const std::string nameA = base + "-a";
    const std::string nameB = base + "-b";
    PipeServer a(std::wstring(nameA.begin(), nameA.end()));
    PipeServer b(std::wstring(nameB.begin(), nameB.end()));
    CHECK(a.valid());
    CHECK(b.valid());
    if (!a.valid() || !b.valid()) { return; }
    a.serve("A");
    b.serve("B");
    SerialPort port;
    std::string err;
    CHECK(port.open(nameA, 4800, err));
    CHECK(port.name() == nameA);
    CHECK(port.baud() == 4800);
    CHECK(port.open(nameB, 19200, err));
    CHECK(port.name() == nameB);
    CHECK(port.baud() == 19200);
    b.writeNow();
    char buf[8];
    int n = 0;
    for (int i = 0; i < 10 && n <= 0; ++i) { n = port.read(buf, sizeof(buf)); }
    CHECK(n == 1);
    CHECK(n == 1 && buf[0] == 'B');
    a.disconnectNow();
    b.disconnectNow();
}

#endif  // _WIN32

}  // namespace

int main() {
    testBaud();
    testSanitise();
    testLoggableName();
    testSort();
    testDevicePath();
    testEnumerate();
    testClosedPort();
    testBadBaudRefusedBeforeTheOs();
    testNoSuchPort();
#if defined(_WIN32)
    testAccessDeniedWording();
    testNamedPipe();
    testReopenClosesFirst();
#else
    std::printf("test_serial_port: pipe cases skipped (named pipes are Windows-only)\n");
#endif
    return testSummary("test_serial_port");
}
