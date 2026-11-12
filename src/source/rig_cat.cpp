// rig_cat.cpp - see rig_cat.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rig_cat.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cascade::source::rigcat {

const char* dialectName(Dialect d) { return d == Dialect::Kenwood ? "Kenwood" : "Yaesu"; }

bool parseDialect(const std::string& text, Dialect& d) {
    std::string t;
    for (const char c : text) { t += static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    if (t == "kenwood") { d = Dialect::Kenwood; return true; }
    if (t == "yaesu" || t == "newcat") { d = Dialect::Yaesu; return true; }
    return false;
}

const char* rigModeName(RigMode m) {
    switch (m) {
        case RigMode::Lsb: return "LSB";
        case RigMode::Usb: return "USB";
        case RigMode::Cw: return "CW";
        case RigMode::CwReverse: return "CW-R";
        case RigMode::Fm: return "FM";
        case RigMode::Am: return "AM";
        case RigMode::Fsk: return "FSK";
        case RigMode::FskReverse: return "FSK-R";
        case RigMode::RttyLsb: return "RTTY-LSB";
        case RigMode::RttyUsb: return "RTTY-USB";
        case RigMode::DataLsb: return "DATA-LSB";
        case RigMode::DataUsb: return "DATA-USB";
        case RigMode::DataFm: return "DATA-FM";
        case RigMode::FmNarrow: return "FM-N";
        case RigMode::AmNarrow: return "AM-N";
        case RigMode::C4fm: return "C4FM";
        default: break;
    }
    return "unknown";
}

RigMode modeFromCode(Dialect d, char c) {
    if (d == Dialect::Kenwood) {
        switch (c) {  // the TS-590S table: 0 and 8 are "None (setting failure)"
            case '1': return RigMode::Lsb;
            case '2': return RigMode::Usb;
            case '3': return RigMode::Cw;
            case '4': return RigMode::Fm;
            case '5': return RigMode::Am;
            case '6': return RigMode::Fsk;
            case '7': return RigMode::CwReverse;
            case '9': return RigMode::FskReverse;
            default: return RigMode::Unknown;
        }
    }
    switch (c) {  // the FT-991A table, P2 of MD
        case '1': return RigMode::Lsb;
        case '2': return RigMode::Usb;
        case '3': return RigMode::Cw;  // CW-U
        case '4': return RigMode::Fm;
        case '5': return RigMode::Am;
        case '6': return RigMode::RttyLsb;
        case '7': return RigMode::CwReverse;  // CW-L
        case '8': return RigMode::DataLsb;
        case '9': return RigMode::RttyUsb;
        case 'A': case 'a': return RigMode::DataFm;
        case 'B': case 'b': return RigMode::FmNarrow;
        case 'C': case 'c': return RigMode::DataUsb;
        case 'D': case 'd': return RigMode::AmNarrow;
        case 'E': case 'e': return RigMode::C4fm;
        default: return RigMode::Unknown;
    }
}

char modeToCode(Dialect d, RigMode m) {
    for (int i = 0; i < 128; ++i) {
        const char c = static_cast<char>(i);
        if (!std::isalnum(static_cast<unsigned char>(c)) || std::islower(static_cast<unsigned char>(c))) { continue; }
        if (modeFromCode(d, c) == m && m != RigMode::Unknown) { return c; }
    }
    return 0;
}

std::string foxModeForRigMode(RigMode m) {
    switch (m) {
        case RigMode::Lsb: case RigMode::RttyLsb: case RigMode::DataLsb: case RigMode::Fsk: return "LSB";
        case RigMode::Usb: case RigMode::RttyUsb: case RigMode::DataUsb: case RigMode::FskReverse: return "USB";
        case RigMode::Cw: case RigMode::CwReverse: return "CW";
        case RigMode::Fm: case RigMode::DataFm: case RigMode::FmNarrow: return "NFM";
        case RigMode::Am: case RigMode::AmNarrow: return "AM";
        default: break;
    }
    return std::string();
}

RigMode rigModeForFoxMode(const std::string& f) {
    if (f == "USB") { return RigMode::Usb; }
    if (f == "LSB") { return RigMode::Lsb; }
    if (f == "CW") { return RigMode::Cw; }
    if (f == "AM") { return RigMode::Am; }
    if (f == "NFM" || f == "WFM") { return RigMode::Fm; }
    return RigMode::Unknown;
}

namespace {

int freqDigits(Dialect d) { return d == Dialect::Kenwood ? 11 : 9; }

std::string upper(const std::string& s) {
    std::string o = s;
    for (char& c : o) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return o;
}

bool allDigits(const std::string& s) {
    if (s.empty()) { return false; }
    for (const char c : s) {
        if (c < '0' || c > '9') { return false; }
    }
    return true;
}

}  // namespace

std::string buildReadFrequency(Dialect) { return "FA;"; }
std::string buildReadMode(Dialect d) { return d == Dialect::Kenwood ? "MD;" : "MD0;"; }

std::string buildSetFrequency(Dialect d, double hz) {
    if (!std::isfinite(hz) || hz < 0.0) { return std::string(); }
    const double r = std::round(hz);
    double limit = 1.0;
    for (int i = 0; i < freqDigits(d); ++i) { limit *= 10.0; }
    if (r >= limit) { return std::string(); }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "FA%0*lld;", freqDigits(d), static_cast<long long>(r));
    return buf;
}

std::string buildSetMode(Dialect d, RigMode m) {
    const char c = modeToCode(d, m);
    if (c == 0) { return std::string(); }
    return std::string(d == Dialect::Kenwood ? "MD" : "MD0") + c + ";";
}

Answer parseAnswer(Dialect d, const std::string& message) {
    Answer a;
    if (message.size() < 2 || message.back() != ';') { return a; }  // Malformed
    if (message == "?;") { a.kind = AnswerKind::ErrorSyntax; return a; }
    if (message == "E;") { a.kind = AnswerKind::ErrorComm; return a; }
    if (message == "O;") { a.kind = AnswerKind::ErrorBusy; return a; }
    if (message.size() < 3) { return a; }
    const std::string cmd = upper(message.substr(0, 2));
    const std::string body = message.substr(2, message.size() - 3);
    if (cmd == "FA") {
        if (static_cast<int>(body.size()) != freqDigits(d) || !allDigits(body)) { return a; }
        long long v = 0;
        for (const char c : body) { v = v * 10 + (c - '0'); }
        a.kind = AnswerKind::Frequency;
        a.frequencyHz = static_cast<double>(v);
        return a;
    }
    if (cmd == "MD") {
        if (d == Dialect::Kenwood) {
            if (body.size() != 1) { return a; }
            a.kind = AnswerKind::Mode;
            a.mode = modeFromCode(d, body[0]);
            return a;
        }
        if (body.size() != 2 || body[0] != '0') {
            if (body.size() == 2) { a.kind = AnswerKind::Other; }  // another receiver's mode: not ours
            return a;
        }
        a.kind = AnswerKind::Mode;
        a.mode = modeFromCode(d, body[1]);
        return a;
    }
    // Any other two-letter command word with a plausible body is well-formed but not ours.
    if (std::isalpha(static_cast<unsigned char>(cmd[0])) && std::isalpha(static_cast<unsigned char>(cmd[1]))) {
        a.kind = AnswerKind::Other;
    }
    return a;
}

// --- the splitter --------------------------------------------------------------------------

void MessageSplitter::feed(const std::uint8_t* data, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const char c = static_cast<char>(data[i]);
        if (skipping_) {
            if (c == ';') { skipping_ = false; ++dropped_; }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (buf_.empty()) { continue; }  // a line feed between messages is not part of one
        }
        buf_ += c;
        if (c == ';') {
            ready_.push_back(buf_);
            buf_.clear();
        } else if (buf_.size() >= kMaxMessage) {
            buf_.clear();
            skipping_ = true;
        }
    }
}

bool MessageSplitter::next(std::string& out) {
    if (pos_ >= ready_.size()) { ready_.clear(); pos_ = 0; return false; }
    out = std::move(ready_[pos_++]);
    return true;
}

// --- the follower --------------------------------------------------------------------------

void RigFollower::apply(const std::string& message) {
    const Answer a = parseAnswer(dialect_, message);
    switch (a.kind) {
        case AnswerKind::Frequency:
            state_.frequencyKnown = true;
            state_.frequencyHz = a.frequencyHz;
            gotFrequency_ = true;
            break;
        case AnswerKind::Mode:
            state_.modeKnown = true;
            state_.mode = a.mode;
            gotMode_ = true;
            break;
        case AnswerKind::ErrorSyntax:
        case AnswerKind::ErrorComm:
        case AnswerKind::ErrorBusy:
            ++rigErrors_;
            sawError_ = true;
            break;
        case AnswerKind::Malformed: ++malformed_; break;
        default: break;
    }
}

void RigFollower::consume(const std::uint8_t* data, std::size_t n) {
    splitter_.feed(data, n);
    std::string m;
    while (splitter_.next(m)) { apply(m); }
}

bool RigFollower::poll(std::chrono::milliseconds timeout) {
    error_.clear();
    gotFrequency_ = gotMode_ = sawError_ = false;
    const std::string q = buildReadFrequency(dialect_) + buildReadMode(dialect_);
    if (!stream_.write(q.data(), q.size())) {
        error_ = std::string("could not write to the rig: ") + stream_.lastError();
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::uint8_t buf[256];
    while (!(gotFrequency_ && gotMode_) && !sawError_) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            error_ = "the rig did not answer in time";
            return false;
        }
        std::size_t got = 0;
        if (!stream_.read(buf, sizeof(buf), got, left)) {
            error_ = std::string("the rig connection failed: ") + stream_.lastError();
            return false;
        }
        if (got > 0) { consume(buf, got); }
    }
    if (sawError_) { error_ = "the rig answered with an error message"; return false; }
    return true;
}

// --- the emulator ---------------------------------------------------------------------------

std::string RigEmulator::handle(const std::string& message) {
    ++handled_;
    const auto refuse = [this]() { ++refused_; return std::string("?;"); };
    if (message.size() < 3 || message.back() != ';') { return refuse(); }
    const std::string cmd = upper(message.substr(0, 2));
    const std::string body = message.substr(2, message.size() - 3);
    if (cmd == "FA") {
        if (body.empty()) {  // read
            if (!host_.frequencyHz) { return refuse(); }
            const std::string s = buildSetFrequency(dialect_, host_.frequencyHz());
            if (s.empty()) { return refuse(); }  // a frequency the digits cannot hold is not truncated
            return s;  // the answer has the form of the set command
        }
        if (static_cast<int>(body.size()) != freqDigits(dialect_) || !allDigits(body)) { return refuse(); }
        long long v = 0;
        for (const char c : body) { v = v * 10 + (c - '0'); }
        if (!host_.setFrequencyHz || !host_.setFrequencyHz(static_cast<double>(v))) { return refuse(); }
        return std::string();  // a set command has no answer
    }
    if (cmd == "MD") {
        const bool yaesu = dialect_ == Dialect::Yaesu;
        if (yaesu) {
            if (body == "0") {  // read
                if (!host_.foxMode) { return refuse(); }
                const std::string s = buildSetMode(dialect_, rigModeForFoxMode(host_.foxMode()));
                return s.empty() ? refuse() : s;
            }
            if (body.size() != 2 || body[0] != '0') { return refuse(); }
        } else {
            if (body.empty()) {
                if (!host_.foxMode) { return refuse(); }
                const std::string s = buildSetMode(dialect_, rigModeForFoxMode(host_.foxMode()));
                return s.empty() ? refuse() : s;
            }
            if (body.size() != 1) { return refuse(); }
        }
        const RigMode m = modeFromCode(dialect_, body.back());
        const std::string fox = foxModeForRigMode(m);
        if (fox.empty() || !host_.setFoxMode || !host_.setFoxMode(fox)) { return refuse(); }
        return std::string();
    }
    return refuse();  // well-formed, not implemented
}

std::string RigEmulator::feed(const std::uint8_t* data, std::size_t n) {
    splitter_.feed(data, n);
    std::string out, m;
    while (splitter_.next(m)) { out += handle(m); }
    return out;
}

// --- byte streams ---------------------------------------------------------------------------

namespace {

class TcpByteStream : public ByteStream {
public:
    explicit TcpByteStream(std::unique_ptr<iiod::Transport> t) : t_(std::move(t)) {}
    ~TcpByteStream() override { close(); }
    bool write(const void* d, std::size_t n) override { return t_ && t_->sendAll(d, n); }
    bool read(void* d, std::size_t max, std::size_t& got, std::chrono::milliseconds timeout) override {
        got = 0;
        if (!t_) { return false; }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (t_->recvSome(d, max, got)) { return true; }
            const std::string e = t_->lastError();
            if (e.find("timed out") == std::string::npos) { return false; }  // closed or failed
            got = 0;
            if (std::chrono::steady_clock::now() >= deadline) { return true; }  // a quiet wait
        }
    }
    void close() override {
        if (t_) { t_->shutdown(); t_->close(); t_.reset(); }
    }
    const char* lastError() const override { return t_ ? t_->lastError() : "closed"; }

private:
    std::unique_ptr<iiod::Transport> t_;
};

#if defined(_WIN32)
class SerialByteStream : public ByteStream {
public:
    explicit SerialByteStream(void* h) : h_(h) {}
    ~SerialByteStream() override { close(); }
    bool write(const void* d, std::size_t n) override {
        if (h_ == nullptr) { err_ = "closed"; return false; }
        DWORD w = 0;
        if (!::WriteFile(static_cast<HANDLE>(h_), d, static_cast<DWORD>(n), &w, nullptr) || w != n) {
            err_ = "a serial write failed";
            return false;
        }
        return true;
    }
    bool read(void* d, std::size_t max, std::size_t& got, std::chrono::milliseconds timeout) override {
        got = 0;
        if (h_ == nullptr) { err_ = "closed"; return false; }
        COMMTIMEOUTS ct{};
        ct.ReadIntervalTimeout = MAXDWORD;
        ct.ReadTotalTimeoutMultiplier = MAXDWORD;
        ct.ReadTotalTimeoutConstant = static_cast<DWORD>(std::min<long long>(timeout.count(), 60000));
        ::SetCommTimeouts(static_cast<HANDLE>(h_), &ct);
        DWORD r = 0;
        if (!::ReadFile(static_cast<HANDLE>(h_), d, static_cast<DWORD>(max), &r, nullptr)) {
            err_ = "a serial read failed";
            return false;
        }
        got = r;
        return true;
    }
    void close() override {
        if (h_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(h_)); h_ = nullptr; }
    }
    const char* lastError() const override { return err_.c_str(); }

private:
    void* h_;
    std::string err_;
};
#endif

}  // namespace

std::unique_ptr<ByteStream> connectTcpByteStream(const std::string& host, std::uint16_t port, std::string& error) {
    iiod::PeerWording w;
    w.name = "the rig";
    w.unknownHostHint = "is the address of the serial-to-network bridge right?";
    std::unique_ptr<iiod::Transport> t =
        iiod::connectTcp(host, port, std::chrono::milliseconds(3000), std::chrono::milliseconds(100), error, w);
    if (!t) { return nullptr; }
    return std::unique_ptr<ByteStream>(new TcpByteStream(std::move(t)));
}

std::unique_ptr<ByteStream> openSerialByteStream(const std::string& port, const SerialSettings& s, std::string& error) {
#if defined(_WIN32)
    if (s.baud < 300 || s.baud > 4000000) { error = "the baud rate must be given (300 to 4000000)"; return nullptr; }
    if (s.stopBits != 1 && s.stopBits != 2) { error = "stop bits must be 1 or 2"; return nullptr; }
    // "COM3" becomes the device name with the \\.\ prefix, which also reaches COM10 and above.
    std::string name = port;
    const char bs = static_cast<char>(92);
    if (name.compare(0, 4, std::string(2, bs) + "." + bs) != 0) { name = std::string(2, bs) + "." + bs + port; }
    for (const char c : port) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == bs || c == '.')) { error = "the port name has an odd character"; return nullptr; }
    }
    const HANDLE h = ::CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { error = "could not open the serial port (error " + std::to_string(::GetLastError()) + ")"; return nullptr; }
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(h, &dcb)) { ::CloseHandle(h); error = "could not read the serial port's state"; return nullptr; }
    dcb.BaudRate = static_cast<DWORD>(s.baud);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = s.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    if (!::SetCommState(h, &dcb)) { ::CloseHandle(h); error = "could not set the serial parameters"; return nullptr; }
    return std::unique_ptr<ByteStream>(new SerialByteStream(h));
#else
    (void)port;
    (void)s;
    error = "serial ports are not built on this platform";
    return nullptr;
#endif
}

}  // namespace cascade::source::rigcat
