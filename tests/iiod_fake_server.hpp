// iiod_fake_server.hpp - an IIO daemon that lives in the test process, with
// the transmit half of the protocol in it.
//
// WHY IT IS NOT tests/test_pluto_source.cpp's FAKE. That file has one too,
// written from the same grammar, and the first instinct was to lift it into a
// shared header and have both tests use it. What stopped that is the same
// argument tests/hackrf_fake_usb.hpp makes about the shared USB fake: the
// difference is what this one CAN DO rather than a preference about where the
// code lives.
//
//   - It answers WRITEBUF, which is a three-part conversation (an ack line,
//     the payload, a byte count) and is the command the receive side never
//     sends. The receive fake has no code for it and no reason to grow any.
//   - It KEEPS THE PAYLOAD, so a test can check the actual sample bytes that
//     went down the wire against what the modulator produced - which is the
//     only way, with no radio here, to know the conversion is right.
//   - Its context carries a full transmit half: a TX LO with a powerdown, a
//     transmit gain channel with a published range, a DAC device with OUTPUT
//     scan elements and four DDS tone generators. The receive fake's context
//     has a stub of that, deliberately, because its job is to prove the
//     receive driver IGNORES it.
//   - It can DIE MID-STREAM on command, which is the case that decides
//     whether a keyed radio gets silenced.
//
// So the two coexist, and neither has to bend around the other. If a third
// Pluto driver ever arrives, this is the one to share.
//
// WHERE THE EXPECTATIONS COME FROM. There is no Pluto on this bench, so an
// expectation invented here would only prove this file agrees with itself.
// Every command and every reply shape was taken from the IIOD daemon's own
// source - iiod/lexer.l, iiod/parser.y and iiod/ops.c of libiio, read as
// documentation of a wire protocol the way a datasheet is read - and the
// WRITEBUF framing in particular was re-checked against rw_dev() and
// receive_data() in September 2026. What CANNOT be established here is the
// content of a real board's context XML or of its *_available attributes:
// those are Analog Devices' published description, and are UNVERIFIED against
// hardware. The tests are therefore written not to depend on them being
// right - the same code is served two different boards and its answers have
// to differ accordingly.
//
// A REAL SERVER ON A REAL SOCKET: 127.0.0.1, an ephemeral port, its own
// accept and connection threads, so the transport, the bounded connect and
// the line discipline are exercised rather than mocked.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cascade::test {

// The context a transmit-capable Pluto answers PRINT with. The escaped ">>"
// in the scan formats is how XML carries them.
//
// THE TWO altvoltage CHANNELS ON THE PHY ARE THE POINT of this document.
// altvoltage0 is the RECEIVER's oscillator and altvoltage1 is the
// transmitter's, they are the same shape as each other, and a driver that
// picked the wrong one would retune the receiver every time the operator
// moved the transmit frequency. Both are here so that mistake is visible.
inline const char* kTxContextXml = R"XML(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE context [<!ELEMENT context (device | context-attribute)*><!ELEMENT context-attribute EMPTY><!ELEMENT device (channel | attribute | debug-attribute | buffer-attribute)*><!ELEMENT channel (scan-element?, attribute*)><!ELEMENT attribute EMPTY><!ELEMENT scan-element EMPTY><!ATTLIST context name CDATA #REQUIRED><!ATTLIST device id CDATA #REQUIRED name CDATA #IMPLIED>]>
<context name="network" description="Linux (none) 5.15.0 #1 SMP PREEMPT">
	<context-attribute name="hw_model" value="Analog Devices PlutoSDR Rev.B (Z7010-AD9363A)" />
	<context-attribute name="hw_serial" value="104473222c8700071700" />
	<context-attribute name="fw_version" value="v0.38" />
	<device id="iio:device0" name="ad9361-phy">
		<channel id="altvoltage0" name="RX_LO" type="output">
			<attribute name="frequency" filename="out_altvoltage0_RX_LO_frequency" />
			<attribute name="frequency_available" filename="out_altvoltage0_RX_LO_frequency_available" />
			<attribute name="powerdown" filename="out_altvoltage0_RX_LO_powerdown" />
		</channel>
		<channel id="altvoltage1" name="TX_LO" type="output">
			<attribute name="frequency" filename="out_altvoltage1_TX_LO_frequency" />
			<attribute name="frequency_available" filename="out_altvoltage1_TX_LO_frequency_available" />
			<attribute name="powerdown" filename="out_altvoltage1_TX_LO_powerdown" />
		</channel>
		<channel id="voltage0" type="input">
			<attribute name="hardwaregain" filename="in_voltage0_hardwaregain" />
			<attribute name="hardwaregain_available" filename="in_voltage0_hardwaregain_available" />
			<attribute name="gain_control_mode" filename="in_voltage0_gain_control_mode" />
			<attribute name="sampling_frequency" filename="in_voltage_sampling_frequency" />
			<attribute name="sampling_frequency_available" filename="in_voltage_sampling_frequency_available" />
			<attribute name="rf_port_select" filename="in_voltage0_rf_port_select" />
		</channel>
		<channel id="voltage0" type="output">
			<attribute name="hardwaregain" filename="out_voltage0_hardwaregain" />
			<attribute name="hardwaregain_available" filename="out_voltage0_hardwaregain_available" />
			<attribute name="rf_port_select" filename="out_voltage0_rf_port_select" />
			<attribute name="rf_port_select_available" filename="out_voltage0_rf_port_select_available" />
			<attribute name="sampling_frequency" filename="out_voltage_sampling_frequency" />
			<attribute name="sampling_frequency_available" filename="out_voltage_sampling_frequency_available" />
			<attribute name="rf_bandwidth" filename="out_voltage_rf_bandwidth" />
			<attribute name="rf_bandwidth_available" filename="out_voltage_rf_bandwidth_available" />
		</channel>
		<attribute name="xo_correction" filename="xo_correction" />
	</device>
	<device id="iio:device3" name="cf-ad9361-dds-core-lpc">
		<channel id="voltage0" type="output">
			<scan-element index="0" format="le:S16/16&gt;&gt;0" />
		</channel>
		<channel id="voltage1" type="output">
			<scan-element index="1" format="le:S16/16&gt;&gt;0" />
		</channel>
		<channel id="altvoltage0" name="TX1_I_F1" type="output">
			<attribute name="raw" filename="out_altvoltage0_TX1_I_F1_raw" />
			<attribute name="frequency" filename="out_altvoltage0_TX1_I_F1_frequency" />
		</channel>
		<channel id="altvoltage1" name="TX1_I_F2" type="output">
			<attribute name="raw" filename="out_altvoltage1_TX1_I_F2_raw" />
		</channel>
		<channel id="altvoltage2" name="TX1_Q_F1" type="output">
			<attribute name="raw" filename="out_altvoltage2_TX1_Q_F1_raw" />
		</channel>
		<channel id="altvoltage3" name="TX1_Q_F2" type="output">
			<attribute name="raw" filename="out_altvoltage3_TX1_Q_F2_raw" />
		</channel>
	</device>
	<device id="iio:device4" name="cf-ad9361-lpc">
		<channel id="voltage0" type="input">
			<scan-element index="0" format="le:S12/16&gt;&gt;0" />
		</channel>
		<channel id="voltage1" type="input">
			<scan-element index="1" format="le:S12/16&gt;&gt;0" />
		</channel>
	</device>
</context>
)XML";

// --- socket plumbing --------------------------------------------------------

#if defined(_WIN32)
using Sock = SOCKET;
inline constexpr Sock kBadSock = INVALID_SOCKET;
inline void closeSock(Sock s) {
    if (s != kBadSock) { ::closesocket(s); }
}
inline bool startNetwork() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
inline void setRecvTimeout(Sock s, int ms) {
    DWORD v = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
}
inline int sendBytes(Sock s, const char* p, int n) { return ::send(s, p, n, 0); }
inline int recvBytes(Sock s, char* p, int n) { return ::recv(s, p, n, 0); }
using SockLen = int;
#else
using Sock = int;
inline constexpr Sock kBadSock = -1;
inline void closeSock(Sock s) {
    if (s != kBadSock) { ::close(s); }
}
inline bool startNetwork() { return true; }
inline void setRecvTimeout(Sock s, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
inline int sendBytes(Sock s, const char* p, int n) {
    return static_cast<int>(::send(s, p, static_cast<std::size_t>(n), MSG_NOSIGNAL));
}
inline int recvBytes(Sock s, char* p, int n) {
    return static_cast<int>(::recv(s, p, static_cast<std::size_t>(n), 0));
}
using SockLen = socklen_t;
#endif

// --- the fake daemon --------------------------------------------------------

class FakeTxIiod {
public:
    ~FakeTxIiod() { stop(); }

    bool start(std::string& error) {
        if (!startNetwork()) {
            error = "no network stack";
            return false;
        }
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kBadSock) {
            error = "no socket";
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // an ephemeral port, so two tests never collide
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            error = "bind failed";
            closeSock(listener_);
            listener_ = kBadSock;
            return false;
        }
        SockLen len = sizeof(addr);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            error = "getsockname failed";
            closeSock(listener_);
            listener_ = kBadSock;
            return false;
        }
        port_ = ::ntohs(addr.sin_port);
        if (::listen(listener_, 8) != 0) {
            error = "listen failed";
            closeSock(listener_);
            listener_ = kBadSock;
            return false;
        }
        run_.store(true);
        acceptor_ = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop() {
        // The acceptor first, so nothing new arrives; then the workers, each
        // of which notices run_ inside its own 100 ms receive timeout and
        // closes ITS OWN socket. Closing them from here would be a double
        // close the moment a worker was already on its way out.
        run_.store(false);
        if (acceptor_.joinable()) { acceptor_.join(); }
        for (std::thread& t : workers_) {
            if (t.joinable()) { t.join(); }
        }
        workers_.clear();
        if (listener_ != kBadSock) {
            closeSock(listener_);
            listener_ = kBadSock;
        }
    }

    std::uint16_t port() const { return port_; }

    void setAttr(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lk(mutex_);
        attrs_[key] = value;
    }
    std::string attr(const std::string& key) {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto it = attrs_.find(key);
        return it == attrs_.end() ? std::string() : it->second;
    }

    // Everything one connection was sent, in order. Connection 0 is the
    // control connection a driver opens first; connection 1 is the stream.
    std::vector<std::string> commands(std::size_t connection) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (connection >= log_.size()) { return {}; }
        return log_[connection];
    }
    std::size_t connections() {
        std::lock_guard<std::mutex> lk(mutex_);
        return log_.size();
    }

    // The sample bytes of every WRITEBUF, concatenated in arrival order.
    std::vector<std::uint8_t> transmitted() {
        std::lock_guard<std::mutex> lk(mutex_);
        return sent_;
    }
    std::size_t writeBufCount() const { return writeBufs_.load(); }

    // A command line the daemon should refuse, matched WHOLE. Whole rather
    // than by substring because "READ ... frequency" is a prefix of
    // "READ ... frequency_available", and a refusal that hit both would test
    // something other than what it says.
    void refuse(const std::string& line, int status) {
        std::lock_guard<std::mutex> lk(mutex_);
        refusals_[line] = status;
    }

    // After this many WRITEBUFs, drop the connection without answering - a
    // board unplugged in the middle of a transmission.
    std::atomic<int> dieAfterWriteBufs{-1};

    // While set, a WRITEBUF's payload is taken but its byte count is NOT sent
    // back, so the driver's writer sits inside that one writeBuf - the way it
    // sits inside one on a real board while the DAC works through its queue.
    // What it lets a test do deterministically is put stop()/finish() in
    // front of a writer that is known to be mid-buffer with the ring full
    // behind it, instead of racing a loopback socket to get there first.
    std::atomic<bool> holdWriteBufAcks{false};

private:
    void acceptLoop() {
        while (run_.load()) {
            // SELECT, THEN ACCEPT. SO_RCVTIMEO does not bound accept() on
            // Windows, so an acceptor that relied on it blocks for ever and
            // stop()'s join never returns.
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listener_, &rfds);
            timeval tv{};
            tv.tv_usec = 100 * 1000;
#if defined(_WIN32)
            const int ready = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
            const int ready = ::select(listener_ + 1, &rfds, nullptr, nullptr, &tv);
#endif
            if (ready <= 0) { continue; }
            sockaddr_in from{};
            SockLen len = sizeof(from);
            const Sock s = ::accept(listener_, reinterpret_cast<sockaddr*>(&from), &len);
            if (s == kBadSock) { continue; }
            setRecvTimeout(s, 100);
            std::size_t index = 0;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                index = log_.size();
                log_.emplace_back();
            }
            workers_.emplace_back([this, s, index] { serve(s, index); });
        }
    }

    bool readLine(Sock s, std::string& out) {
        out.clear();
        char c = 0;
        while (out.size() < 1024) {
            const int n = recvBytes(s, &c, 1);
            if (n == 0) { return false; }
            if (n < 0) {
                if (!run_.load()) { return false; }
                continue;  // the 100 ms poll expiring, not a failure
            }
            if (c == '\n') { return true; }
            out += c;
        }
        return false;
    }

    bool readExactly(Sock s, char* p, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            const int r = recvBytes(s, p + got, static_cast<int>(n - got));
            if (r == 0) { return false; }
            if (r < 0) {
                if (!run_.load()) { return false; }
                continue;
            }
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    bool sendText(Sock s, const std::string& text) {
        std::size_t sent = 0;
        while (sent < text.size()) {
            const int n =
                sendBytes(s, text.data() + sent, static_cast<int>(text.size() - sent));
            if (n <= 0) { return false; }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    void record(std::size_t index, const std::string& what) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (index < log_.size()) { log_[index].push_back(what); }
    }

    int refusalFor(const std::string& line) {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto it = refusals_.find(line);
        return it == refusals_.end() ? 0 : it->second;
    }

    static std::vector<std::string> words(const std::string& line) {
        std::vector<std::string> out;
        std::size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && line[i] == ' ') { ++i; }
            const std::size_t start = i;
            while (i < line.size() && line[i] != ' ') { ++i; }
            if (i > start) { out.push_back(line.substr(start, i - start)); }
        }
        return out;
    }

    void serve(Sock s, std::size_t index) {
        std::string line;
        while (run_.load() && readLine(s, line)) {
            const std::vector<std::string> w = words(line);
            if (w.empty()) { continue; }
            const std::string& cmd = w[0];

            if (cmd == "WRITE") {
                // ops.c write_dev_attr / write_chn_attr: the command line
                // states a byte count and exactly that many bytes follow.
                const long count = std::atol(w.back().c_str());
                std::string payload(static_cast<std::size_t>(count), '\0');
                if (count > 0 && !readExactly(s, &payload[0], payload.size())) { break; }
                std::string shown;
                for (const char c : payload) {
                    if (c == '\0') {
                        shown += "<NUL>";
                    } else {
                        shown += c;
                    }
                }
                record(index, line + " | " + shown);
                const int refusal = refusalFor(line);
                if (refusal != 0) {
                    sendText(s, std::to_string(refusal) + "\n");
                    continue;
                }
                std::string value = payload;
                while (!value.empty() && (value.back() == '\0' || value.back() == '\n')) {
                    value.pop_back();
                }
                std::string key;
                if (w.size() >= 6) {  // WRITE dev INPUT chn attr count
                    key = w[1] + "/" + (w[2] == "OUTPUT" ? "out" : "in") + "/" + w[3] + "/" +
                          w[4];
                } else {  // WRITE dev attr count
                    key = w[1] + "/" + w[2];
                }
                setAttr(key, value);
                sendText(s, std::to_string(count) + "\n");
                continue;
            }

            if (cmd == "WRITEBUF") {
                // ops.c rw_dev with is_write true -> rw_buffer -> receive_data:
                //   a "0" line while thd->new_client is set (which rw_buffer
                //   sets on EVERY call, exactly as it does for the read
                //   side's mask line), then the payload, then rw_dev's own
                //   print_value of what was taken.
                const long count = w.size() >= 3 ? std::atol(w[2].c_str()) : 0;
                record(index, line);
                const int refusal = refusalFor(line);
                if (refusal != 0) {
                    sendText(s, std::to_string(refusal) + "\n");
                    continue;
                }
                sendText(s, "0\n");
                std::string payload(static_cast<std::size_t>(count), '\0');
                if (count > 0 && !readExactly(s, &payload[0], payload.size())) { break; }
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    sent_.insert(sent_.end(), payload.begin(), payload.end());
                }
                const int n = static_cast<int>(writeBufs_.fetch_add(1)) + 1;
                const int die = dieAfterWriteBufs.load();
                if (die >= 0 && n >= die) {
                    // THE BOARD GOING, mid-transmission: the payload was
                    // taken and the connection then dropped without the byte
                    // count. A keyed radio and a dead socket is the state the
                    // driver's fault path exists for.
                    break;
                }
                while (holdWriteBufAcks.load() && run_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                sendText(s, std::to_string(count) + "\n");
                continue;
            }

            record(index, line);
            const int refusal = refusalFor(line);
            if (refusal != 0) {
                sendText(s, std::to_string(refusal) + "\n");
                continue;
            }

            if (cmd == "VERSION") {
                sendText(s, "1.1.5f2e3b0\n");  // parser.y's VERSION rule
            } else if (cmd == "PRINT") {
                const std::string xml = kTxContextXml;
                sendText(s, std::to_string(xml.size()) + "\n" + xml + "\n");
            } else if (cmd == "TIMEOUT" || cmd == "SET" || cmd == "CLOSE") {
                sendText(s, "0\n");
            } else if (cmd == "OPEN") {
                // ops.c open_dev: a mask whose length is not exactly
                // (channels + 31) / 32 * 8 characters is -EINVAL before
                // anything else happens. OPEN <device> <samples> <mask>.
                const std::string mask = w.size() >= 4 ? w[3] : std::string();
                sendText(s, mask.size() == 8 ? "0\n" : "-22\n");
            } else if (cmd == "READ") {
                std::string key;
                if (w.size() >= 5) {
                    key = w[1] + "/" + (w[2] == "OUTPUT" ? "out" : "in") + "/" + w[3] + "/" +
                          w[4];
                } else if (w.size() >= 3) {
                    key = w[1] + "/" + w[2];
                }
                std::string value;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lk(mutex_);
                    const auto it = attrs_.find(key);
                    if (it != attrs_.end()) {
                        value = it->second;
                        found = true;
                    }
                }
                if (!found) {
                    sendText(s, "-2\n");  // -ENOENT, as ops.c answers
                } else {
                    sendText(s, std::to_string(value.size()) + "\n" + value + "\n");
                }
            } else {
                sendText(s, "-22\n");  // -EINVAL, as yyerror answers
            }
        }
        closeSock(s);
    }

    Sock listener_ = kBadSock;
    std::uint16_t port_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<int> writeBufs_{0};
    std::thread acceptor_;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::vector<std::vector<std::string>> log_;
    std::map<std::string, std::string> attrs_;
    std::map<std::string, int> refusals_;
    std::vector<std::uint8_t> sent_;
};

// A stock AD9363 board's published state - the numbers Analog Devices
// documents for an un-unlocked Pluto.
inline void stockTxBoard(FakeTxIiod& d) {
    d.setAttr("ad9361-phy/out/altvoltage1/frequency", "2400000000");
    d.setAttr("ad9361-phy/out/altvoltage1/frequency_available", "[325000000 1 3800000000]");
    d.setAttr("ad9361-phy/out/altvoltage1/powerdown", "1");
    d.setAttr("ad9361-phy/out/altvoltage0/frequency", "433000000");
    d.setAttr("ad9361-phy/out/altvoltage0/frequency_available", "[325000000 1 3800000000]");
    d.setAttr("ad9361-phy/out/voltage0/sampling_frequency", "2500000");
    d.setAttr("ad9361-phy/out/voltage0/sampling_frequency_available", "[2083333 1 61440000]");
    d.setAttr("ad9361-phy/out/voltage0/rf_bandwidth", "18000000");
    d.setAttr("ad9361-phy/out/voltage0/rf_bandwidth_available", "[200000 1 40000000]");
    // As sysfs prints it. NOTE THE SIGN: this is an ATTENUATION, 0 is full
    // output, and -89.75 is as quiet as an AD9361 goes.
    d.setAttr("ad9361-phy/out/voltage0/hardwaregain", "0.000000 dB");
    d.setAttr("ad9361-phy/out/voltage0/hardwaregain_available", "[-89.750000 0.250000 0.000000]");
    d.setAttr("ad9361-phy/out/voltage0/rf_port_select", "A");
    d.setAttr("ad9361-phy/out/voltage0/rf_port_select_available", "A B");
}

// The same board with the AD9364 unlock applied: 70 MHz to 6 GHz. Served so
// the driver's limits can be proven to come off the BOARD rather than out of
// a table - a driver with a built-in range passes one of these and not both.
inline void unlockedTxBoard(FakeTxIiod& d) {
    stockTxBoard(d);
    d.setAttr("ad9361-phy/out/altvoltage1/frequency_available", "[70000000 1 6000000000]");
}

}  // namespace cascade::test
