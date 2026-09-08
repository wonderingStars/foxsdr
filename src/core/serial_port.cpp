// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// serial_port.cpp - the platform half of serial_port.hpp. The header says WHY
// (the test seam through a named pipe, the overlapped read, the comm-setup
// warning); this file says HOW, and keeps the pure helpers - the sort, the
// name sanitiser, the device-path rule - free of any platform header so the
// one test file pins them on both platforms.

#include "core/serial_port.hpp"

#include "core/diag_log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#pragma comment(lib, "advapi32.lib")
#else
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace cascade::core {

// ---------------------------------------------------------------------------
// Pure helpers - no OS below this line until the platform sections.
// ---------------------------------------------------------------------------

bool serialBaudSupported(int baud) {
    for (const int b : kSerialBaudRates) {
        if (b == baud) { return true; }
    }
    return false;
}

std::string sanitiseSerialPortName(const std::string& name) {
    // Printable ASCII only, then trim, then cut, then trim again: the cut can
    // expose a trailing space that was interior before it, and a name ending
    // in a space is not a name any port layer will open.
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u <= 0x7E) { out.push_back(c); }
    }
    auto trim = [](std::string& s) {
        std::size_t a = 0;
        std::size_t b = s.size();
        while (a < b && s[a] == ' ') { ++a; }
        while (b > a && s[b - 1] == ' ') { --b; }
        s = s.substr(a, b - a);
    };
    trim(out);
    if (out.size() > kMaxSerialPortNameChars) { out.resize(kMaxSerialPortNameChars); }
    trim(out);
    return out;
}

std::string loggableSerialPortName(const std::string& name) {
    if (name.empty()) { return "(no port)"; }
    // The Windows shape: an optional "\\.\" or "\\?\" device prefix, then
    // "COM" in either case and one to three digits. Windows never assigns
    // more than COM256, and a longer digit run is not a port this OS made.
    std::size_t i = 0;
    if (name.size() >= 4 && name[0] == '\\' && name[1] == '\\' && (name[2] == '.' || name[2] == '?')
        && name[3] == '\\') {
        i = 4;
    }
    auto isCom = [&](std::size_t at) {
        if (at + 3 > name.size()) { return false; }
        const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
        if (lower(name[at]) != 'c' || lower(name[at + 1]) != 'o' || lower(name[at + 2]) != 'm') { return false; }
        const std::size_t digits = name.size() - (at + 3);
        if (digits < 1 || digits > 3) { return false; }
        for (std::size_t k = at + 3; k < name.size(); ++k) {
            if (!std::isdigit(static_cast<unsigned char>(name[k]))) { return false; }
        }
        return true;
    };
    if (isCom(i)) { return name; }
    // The Linux shape: "/dev/" and ONE node name - letters, digits, '_', '-'
    // and '.'. A deeper path ("/dev/serial/by-id/usb-u-blox_..._GPS-if00")
    // carries the device's product string and serial number, which the
    // privacy document also promises never to send, and a "/home/..." path
    // is a file.
    if (name.rfind("/dev/", 0) == 0 && name.size() > 5) {
        bool node = true;
        for (std::size_t k = 5; k < name.size(); ++k) {
            const unsigned char c = static_cast<unsigned char>(name[k]);
            if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) { node = false; break; }
        }
        if (node) { return name; }
    }
    return "(a typed device path, " + std::to_string(name.size()) + " chars)";
}

namespace {

// The natural-order key: the leading run of non-digits (lower-cased), the
// trailing number if the name ends in digits, and whether it does. "COM10"
// is {"com", 10, true}; "/dev/ttyUSB0" is {"/dev/ttyusb", 0, true}; a name
// with no trailing digits sorts before every numbered sibling of the same
// prefix, so a bare "COM" (which no OS produces, but a config could) lands
// where a person would look for it.
struct PortKey {
    std::string prefix;
    unsigned long long number = 0;
    bool numbered = false;
};

PortKey portKey(const std::string& name) {
    PortKey k;
    std::size_t i = 0;
    while (i < name.size() && !std::isdigit(static_cast<unsigned char>(name[i]))) {
        k.prefix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(name[i]))));
        ++i;
    }
    std::size_t end = name.size();
    std::size_t start = end;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(name[start - 1]))) { --start; }
    if (start < end) {
        k.numbered = true;
        // Clamp rather than overflow: a 30-digit "port number" is garbage,
        // and garbage should sort last, not wrap around to sort first.
        unsigned long long v = 0;
        for (std::size_t j = start; j < end; ++j) {
            const unsigned d = static_cast<unsigned>(name[j] - '0');
            if (v > (~0ull - d) / 10ull) { v = ~0ull; break; }
            v = v * 10ull + d;
        }
        k.number = v;
    }
    return k;
}

bool portLess(const std::string& a, const std::string& b) {
    const PortKey ka = portKey(a);
    const PortKey kb = portKey(b);
    if (ka.prefix != kb.prefix) { return ka.prefix < kb.prefix; }
    if (ka.numbered != kb.numbered) { return !ka.numbered; }
    if (ka.number != kb.number) { return ka.number < kb.number; }
    return a < b;
}

}  // namespace

std::vector<std::string> sortSerialPortNames(std::vector<std::string> names) {
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string& s) { return s.empty(); }),
                names.end());
    std::sort(names.begin(), names.end(), portLess);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::string windowsDevicePath(const std::string& name) {
    if (name.size() >= 2 && name[0] == '\\' && name[1] == '\\') { return name; }
    return "\\\\.\\" + name;
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------
#if defined(_WIN32)

namespace {

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) { return {}; }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                        nullptr, 0);
    if (n <= 0) { return {}; }
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
}

std::string toUtf8(const wchar_t* w, std::size_t len) {
    if (len == 0) { return {}; }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) { return {}; }
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), s.data(), n, nullptr, nullptr);
    return s;
}

// The OS's own sentence for an error code, without the CRLF FormatMessage
// appends. English where the system has it, so a log line from a beta
// tester's machine reads the same as one from here; the user-facing part of
// the error is our own wording anyway.
std::string systemMessage(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD n = ::FormatMessageW(flags, nullptr, code,
                               MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                               reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    if (n == 0 || buf == nullptr) {
        if (buf != nullptr) { ::LocalFree(buf); buf = nullptr; }
        n = ::FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0,
                             nullptr);
    }
    std::string text;
    if (n > 0 && buf != nullptr) {
        while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L' ')) {
            --n;
        }
        text = toUtf8(buf, n);
    }
    if (buf != nullptr) { ::LocalFree(buf); }
    if (text.empty()) { text = "error " + std::to_string(static_cast<unsigned long>(code)); }
    return text;
}

// What a person can act on. ERROR_ACCESS_DENIED on a COM port means another
// program - a GPS utility, u-center, a terminal - already has it open; the
// OS's "Access is denied" sends people to check permissions, which are never
// the problem. ERROR_FILE_NOT_FOUND is the port not existing (unplugged, or a
// typo), and the OS's wording talks about files. Everything else is passed
// through as the OS says it, since we have no better idea than it does.
std::string openFailureText(DWORD code) {
    switch (code) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return "no such port (" + systemMessage(code) + ")";
        case ERROR_ACCESS_DENIED:
            return "in use by another program (" + systemMessage(code) + ")";
        default:
            return systemMessage(code);
    }
}

HANDLE asHandle(std::intptr_t h) { return reinterpret_cast<HANDLE>(h); }

}  // namespace

SerialPort::~SerialPort() { close(); }

bool SerialPort::open(const std::string& name, int baud, std::string& error) {
    close();
    error.clear();
    if (name.empty()) {
        error = "no port chosen";
        return false;
    }
    if (!serialBaudSupported(baud)) {
        error = name + ": " + std::to_string(baud) + " baud is not a supported rate";
        return false;
    }

    const std::wstring path = toWide(windowsDevicePath(name));
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD code = ::GetLastError();
        error = name + ": " + openFailureText(code);
        return false;
    }

    // The event the overlapped read signals. Without it the read has nothing
    // to wait on, so a port without one is not open.
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ev == nullptr) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(h);
        error = name + ": could not create the read event (" + systemMessage(code) + ")";
        return false;
    }

    // Comm parameters. Failure here is a WARNING, not a refusal - see the
    // header: a named pipe is not a communications resource and fails both
    // calls, and the pipe is the test seam. DTR and RTS are asserted because
    // a good many GPS pucks (and every RS-232 level shifter wired to them)
    // sit silent until the host raises them.
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    bool commOk = ::GetCommState(h, &dcb) != 0;
    if (commOk) {
        dcb.BaudRate = static_cast<DWORD>(baud);
        dcb.ByteSize = 8;
        dcb.Parity = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary = 1;
        dcb.fParity = 0;
        dcb.fOutxCtsFlow = 0;
        dcb.fOutxDsrFlow = 0;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fDsrSensitivity = 0;
        dcb.fOutX = 0;
        dcb.fInX = 0;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        dcb.fAbortOnError = 0;
        commOk = ::SetCommState(h, &dcb) != 0;
    }
    // Logged by the loggable name, never the typed one: a pipe or a file
    // path fails these calls too, and the warning must not carry it.
    if (!commOk) {
        diagWarnf("serial: %s: SetCommState failed: %s (not a communications device?)",
                  loggableSerialPortName(name).c_str(), systemMessage(::GetLastError()).c_str());
    }

    // "Return what is there now, otherwise wait up to the constant": the
    // MAXDWORD/MAXDWORD/constant triple is the documented spelling of that.
    // On a COM port this bounds the overlapped read by itself; the event wait
    // in read() bounds it on everything else.
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant = static_cast<DWORD>(kReadTimeoutMs);
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 1000;
    if (::SetCommTimeouts(h, &to) == 0) {
        diagWarnf("serial: %s: SetCommTimeouts failed: %s (not a communications device?)",
                  loggableSerialPortName(name).c_str(), systemMessage(::GetLastError()).c_str());
    }
    if (commOk) {
        // Whatever the receiver spoke while nobody was listening is half a
        // sentence at best; the assembler would drop it anyway, but a clean
        // start makes the first byte count honest.
        ::PurgeComm(h, PURGE_RXCLEAR | PURGE_RXABORT);
    }

    name_ = name;
    baud_ = baud;
    handle_ = reinterpret_cast<std::intptr_t>(h);
    readEvent_ = reinterpret_cast<std::intptr_t>(ev);
    return true;
}

void SerialPort::close() {
    if (handle_ != -1) {
        ::CloseHandle(asHandle(handle_));
        handle_ = -1;
    }
    if (readEvent_ != -1) {
        ::CloseHandle(asHandle(readEvent_));
        readEvent_ = -1;
    }
    name_.clear();
    baud_ = 0;
}

bool SerialPort::isOpen() const { return handle_ != -1; }

int SerialPort::read(char* buf, std::size_t cap) {
    if (!isOpen() || buf == nullptr || cap == 0) { return -1; }
    HANDLE h = asHandle(handle_);
    HANDLE ev = asHandle(readEvent_);
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(cap, 0x7FFFFFFF));

    // A completed read of zero bytes means two different things. On a COM
    // port it is the timeout - COMMTIMEOUTS elapsed with nothing to deliver -
    // and the caller must simply ask again. On a pipe it is the end: the
    // server has nothing more and never will. GetFileType tells the two
    // apart, and is only consulted on that one path.
    auto zeroBytes = [&]() -> int {
        return ::GetFileType(h) == FILE_TYPE_PIPE ? -1 : 0;
    };

    OVERLAPPED ov{};
    ov.hEvent = ev;
    ::ResetEvent(ev);
    DWORD n = 0;
    if (::ReadFile(h, buf, want, &n, &ov)) {
        return n > 0 ? static_cast<int>(n) : zeroBytes();
    }
    const DWORD code = ::GetLastError();
    if (code != ERROR_IO_PENDING) { return -1; }

    const DWORD wait = ::WaitForSingleObject(ev, static_cast<DWORD>(kReadTimeoutMs));
    if (wait == WAIT_OBJECT_0) {
        if (!::GetOverlappedResult(h, &ov, &n, FALSE)) { return -1; }
        return n > 0 ? static_cast<int>(n) : zeroBytes();
    }
    if (wait != WAIT_TIMEOUT) { return -1; }

    // Timed out with the read still posted. Cancel it and WAIT for the
    // cancellation to land: returning while the OS still owns `buf` would be
    // a write into whatever the caller reuses that memory for. The race where
    // data arrived between the timeout and the cancel is real and is handed
    // back as data, not lost.
    ::CancelIoEx(h, &ov);
    if (::GetOverlappedResult(h, &ov, &n, TRUE)) {
        return n > 0 ? static_cast<int>(n) : 0;
    }
    const DWORD after = ::GetLastError();
    if (after == ERROR_OPERATION_ABORTED) { return 0; }
    return -1;
}

std::vector<std::string> enumerateSerialPorts() {
    std::vector<std::string> names;
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ,
                        &key) != ERROR_SUCCESS) {
        return names;
    }
    // The VALUE DATA is the port name ("COM3"); the value NAME is the driver's
    // own device path ("\Device\Serial0", "\Device\VCP0"), which is nothing a
    // user could type. 16383 characters is the documented value-name bound.
    std::vector<wchar_t> valueName(16384);
    std::vector<unsigned char> data(1024);
    for (DWORD index = 0;; ++index) {
        DWORD nameLen = static_cast<DWORD>(valueName.size());
        DWORD type = 0;
        DWORD dataLen = static_cast<DWORD>(data.size());
        const LONG rc = ::RegEnumValueW(key, index, valueName.data(), &nameLen, nullptr, &type,
                                        data.data(), &dataLen);
        if (rc == ERROR_MORE_DATA) {
            data.resize(dataLen + 2);
            --index;  // retry this slot with room for it
            continue;
        }
        if (rc != ERROR_SUCCESS) { break; }
        if (type != REG_SZ && type != REG_EXPAND_SZ) { continue; }
        const wchar_t* w = reinterpret_cast<const wchar_t*>(data.data());
        std::size_t chars = dataLen / sizeof(wchar_t);
        while (chars > 0 && w[chars - 1] == L'\0') { --chars; }
        const std::string name = sanitiseSerialPortName(toUtf8(w, chars));
        if (!name.empty()) { names.push_back(name); }
    }
    ::RegCloseKey(key);
    return sortSerialPortNames(std::move(names));
}

// ---------------------------------------------------------------------------
// Linux (and anything else POSIX enough to have termios and poll)
// ---------------------------------------------------------------------------
#else

namespace {

// termios wants its own constants, not the number. The list is exactly
// kSerialBaudRates; serialBaudSupported() has already refused anything else.
speed_t termiosSpeed(int baud) {
    switch (baud) {
        case 4800: return B4800;
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default: return B9600;
    }
}

std::string errnoText(int e) {
    const char* s = std::strerror(e);
    return s != nullptr ? std::string(s) : ("errno " + std::to_string(e));
}

// The same wording as the Windows side, for the same reasons: EACCES/EBUSY
// on a tty is another program (or a missing dialout membership, which the
// parenthesised OS text still says), ENOENT is the device not being there.
std::string openFailureText(int e) {
    switch (e) {
        case ENOENT:
        case ENOTDIR:
            return "no such port (" + errnoText(e) + ")";
        case EACCES:
        case EBUSY:
            return "in use by another program (" + errnoText(e) + ")";
        default:
            return errnoText(e);
    }
}

}  // namespace

SerialPort::~SerialPort() { close(); }

bool SerialPort::open(const std::string& name, int baud, std::string& error) {
    close();
    error.clear();
    if (name.empty()) {
        error = "no port chosen";
        return false;
    }
    if (!serialBaudSupported(baud)) {
        error = name + ": " + std::to_string(baud) + " baud is not a supported rate";
        return false;
    }

    // O_NOCTTY so a receiver's line noise can never become this process's
    // controlling terminal; O_NONBLOCK so the open itself does not wait for
    // carrier, and so the read is bounded by poll() rather than by the
    // driver. Read-only: a position reader has nothing to say to a GPS.
    const int fd = ::open(name.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = name + ": " + openFailureText(errno);
        return false;
    }

    termios tio{};
    if (::tcgetattr(fd, &tio) == 0) {
        ::cfmakeraw(&tio);
        ::cfsetispeed(&tio, termiosSpeed(baud));
        ::cfsetospeed(&tio, termiosSpeed(baud));
        tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | CSTOPB | CRTSCTS);
        tio.c_cflag |= static_cast<tcflag_t>(CS8 | CLOCAL | CREAD);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        if (::tcsetattr(fd, TCSANOW, &tio) != 0) {
            diagWarnf("serial: %s: tcsetattr failed: %s (not a communications device?)",
                      loggableSerialPortName(name).c_str(), errnoText(errno).c_str());
        } else {
            ::tcflush(fd, TCIFLUSH);
        }
    } else {
        // ENOTTY is a FIFO or a regular file - the Linux spelling of the
        // named-pipe seam. Warn, as the header promises, and carry on - by
        // the loggable name, because a FIFO or a file is exactly the kind of
        // path the log must not carry.
        diagWarnf("serial: %s: tcgetattr failed: %s (not a communications device?)",
                  loggableSerialPortName(name).c_str(), errnoText(errno).c_str());
    }

    name_ = name;
    baud_ = baud;
    handle_ = fd;
    return true;
}

void SerialPort::close() {
    if (handle_ != -1) {
        ::close(static_cast<int>(handle_));
        handle_ = -1;
    }
    readEvent_ = -1;
    name_.clear();
    baud_ = 0;
}

bool SerialPort::isOpen() const { return handle_ != -1; }

int SerialPort::read(char* buf, std::size_t cap) {
    if (!isOpen() || buf == nullptr || cap == 0) { return -1; }
    const int fd = static_cast<int>(handle_);
    pollfd p{};
    p.fd = fd;
    p.events = POLLIN;
    const int ready = ::poll(&p, 1, kReadTimeoutMs);
    if (ready == 0) { return 0; }
    if (ready < 0) { return errno == EINTR ? 0 : -1; }
    if ((p.revents & (POLLERR | POLLNVAL)) != 0) { return -1; }
    if ((p.revents & POLLIN) == 0) {
        // POLLHUP alone: the writer is gone and nothing is queued.
        return -1;
    }
    const ssize_t n = ::read(fd, buf, cap);
    if (n > 0) { return static_cast<int>(std::min<ssize_t>(n, 0x7FFFFFFF)); }
    if (n == 0) { return -1; }  // EOF: unplugged, or the FIFO's writer closed
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { return 0; }
    return -1;
}

std::vector<std::string> enumerateSerialPorts() {
    std::vector<std::string> names;
    std::error_code ec;
    const std::filesystem::path dev("/dev");
    for (const auto& entry : std::filesystem::directory_iterator(dev, ec)) {
        const std::string leaf = entry.path().filename().string();
        if (leaf.rfind("ttyUSB", 0) == 0 || leaf.rfind("ttyACM", 0) == 0 ||
            leaf.rfind("ttyS", 0) == 0) {
            names.push_back(entry.path().string());
        }
    }
    return sortSerialPortNames(std::move(names));
}

#endif

}  // namespace cascade::core
