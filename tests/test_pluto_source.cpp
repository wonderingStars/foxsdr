// test_pluto_source.cpp - the ADALM-Pluto driver, proven command for command
// against a fake IIOD daemon written from the same grammar the real one
// parses.
//
// WHERE THE EXPECTATIONS COME FROM. There is no Pluto on this bench, so an
// expectation invented here would only prove this file agrees with itself.
// Every command and every reply shape below was taken from the IIOD daemon's
// own source - iiod/lexer.l, iiod/parser.y and iiod/ops.c of libiio, read as
// documentation of a wire protocol - and each block names the handler it came
// from. What CANNOT be proven here is the content of a real board's context
// XML and of its *_available attributes: those are from Analog Devices'
// published description of the Pluto and are UNVERIFIED against hardware. So
// the tests are written not to depend on them being right: what is asserted
// is that the driver reports whatever the board said, which is checked by
// serving TWO DIFFERENT boards (a stock AD9363 range and an AD9364-unlocked
// one) from the same code and requiring the driver's answers to differ
// accordingly. A driver with a built-in table passes neither.
//
// THE FAKE IS A REAL SERVER ON A REAL SOCKET. 127.0.0.1, an ephemeral port,
// its own accept and connection threads - so the Winsock/BSD transport, the
// bounded connect, the line discipline and the two-connection design are all
// exercised rather than mocked. The pure request/response layer is exercised
// separately through a scripted transport with no socket at all, because the
// exact bytes a command becomes are easier to read there and are the thing a
// protocol regression would change first.
//
// THE DELIBERATE BREAKS. Every block that matters was watched go RED against
// a broken driver before it was trusted green; the failing lines are in the
// report for this change-set.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
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

#include "source/iiod_client.hpp"
#include "source/pluto_source.hpp"
#include "test_check.hpp"

using cascade::source::NativeDeviceInfo;
using cascade::source::PlutoSource;
namespace iiod = cascade::source::iiod;

namespace {

// --- the context a Pluto answers PRINT with -------------------------------
//
// UNVERIFIED AGAINST HARDWARE (see the file header): this is the shape Analog
// Devices documents, reduced to the devices and channels this driver looks
// for plus enough noise - a transmit device, a DDS device, attributes we do
// not use - that the scanner is proven to ignore what it does not want. The
// escaped ">>" in the scan formats is how XML carries them and is exactly the
// case that broke a first draft of the parser.
const char* kContextXml = R"XML(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE context [<!ELEMENT context (device | context-attribute)*><!ELEMENT context-attribute EMPTY><!ELEMENT device (channel | attribute | debug-attribute | buffer-attribute)*><!ELEMENT channel (scan-element?, attribute*)><!ELEMENT attribute EMPTY><!ELEMENT scan-element EMPTY><!ATTLIST context name CDATA #REQUIRED><!ATTLIST device id CDATA #REQUIRED name CDATA #IMPLIED>]>
<context name="network" description="Linux (none) 5.15.0 #1 SMP PREEMPT">
	<context-attribute name="hw_model" value="Analog Devices PlutoSDR Rev.B (Z7010-AD9363A)" />
	<context-attribute name="hw_serial" value="104473222c8700071700" />
	<context-attribute name="fw_version" value="v0.38" />
	<context-attribute name="local,kernel" value="5.15.0" />
	<device id="iio:device0" name="ad9361-phy">
		<channel id="altvoltage0" name="RX_LO" type="output">
			<attribute name="frequency" filename="out_altvoltage0_RX_LO_frequency" />
			<attribute name="frequency_available" filename="out_altvoltage0_RX_LO_frequency_available" />
			<attribute name="fastlock_store" filename="out_altvoltage0_RX_LO_fastlock_store" />
		</channel>
		<channel id="altvoltage1" name="TX_LO" type="output">
			<attribute name="frequency" filename="out_altvoltage1_TX_LO_frequency" />
			<attribute name="frequency_available" filename="out_altvoltage1_TX_LO_frequency_available" />
		</channel>
		<channel id="voltage0" type="input">
			<attribute name="hardwaregain" filename="in_voltage0_hardwaregain" />
			<attribute name="hardwaregain_available" filename="in_voltage0_hardwaregain_available" />
			<attribute name="gain_control_mode" filename="in_voltage0_gain_control_mode" />
			<attribute name="gain_control_mode_available" filename="in_voltage0_gain_control_mode_available" />
			<attribute name="rf_bandwidth" filename="in_voltage_rf_bandwidth" />
			<attribute name="rf_bandwidth_available" filename="in_voltage_rf_bandwidth_available" />
			<attribute name="sampling_frequency" filename="in_voltage_sampling_frequency" />
			<attribute name="sampling_frequency_available" filename="in_voltage_sampling_frequency_available" />
			<attribute name="rf_port_select" filename="in_voltage0_rf_port_select" />
			<attribute name="rssi" filename="in_voltage0_rssi" />
		</channel>
		<channel id="voltage0" type="output">
			<attribute name="hardwaregain" filename="out_voltage0_hardwaregain" />
			<attribute name="rf_port_select" filename="out_voltage0_rf_port_select" />
		</channel>
		<attribute name="xo_correction" filename="xo_correction" />
		<attribute name="rx_path_rates" filename="rx_path_rates" />
		<attribute name="trx_rate_governor" filename="trx_rate_governor" />
		<attribute name="filter_fir_config" filename="filter_fir_config" />
		<debug-attribute name="direct_reg_access" />
	</device>
	<device id="iio:device3" name="cf-ad9361-dds-core-lpc">
		<channel id="voltage0" type="output">
			<scan-element index="0" format="le:S16/16&gt;&gt;0" />
		</channel>
		<channel id="voltage1" type="output">
			<scan-element index="1" format="le:S16/16&gt;&gt;0" />
		</channel>
	</device>
	<device id="iio:device4" name="cf-ad9361-lpc">
		<channel id="voltage0" type="input">
			<scan-element index="0" format="le:S12/16&gt;&gt;0" />
		</channel>
		<channel id="voltage1" type="input">
			<scan-element index="1" format="le:S12/16&gt;&gt;0" />
		</channel>
		<buffer-attribute name="watermark" />
	</device>
</context>
)XML";

// --- socket plumbing for the fake -----------------------------------------

#if defined(_WIN32)
using Sock = SOCKET;
constexpr Sock kBadSock = INVALID_SOCKET;
void closeSock(Sock s) {
    if (s != kBadSock) { ::closesocket(s); }
}
bool startNetwork() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
void setRecvTimeout(Sock s, int ms) {
    DWORD v = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof(v));
}
int sendBytes(Sock s, const char* p, int n) { return ::send(s, p, n, 0); }
int recvBytes(Sock s, char* p, int n) { return ::recv(s, p, n, 0); }
using SockLen = int;
#else
using Sock = int;
constexpr Sock kBadSock = -1;
void closeSock(Sock s) {
    if (s != kBadSock) { ::close(s); }
}
bool startNetwork() { return true; }
void setRecvTimeout(Sock s, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
int sendBytes(Sock s, const char* p, int n) {
    return static_cast<int>(::send(s, p, static_cast<std::size_t>(n), MSG_NOSIGNAL));
}
int recvBytes(Sock s, char* p, int n) {
    return static_cast<int>(::recv(s, p, static_cast<std::size_t>(n), 0));
}
using SockLen = socklen_t;
#endif

// The sample a fake buffer carries at word `w` of buffer `k`. A ramp with a
// multiplier, so a lost buffer, a repeated one, a swapped I/Q pair or an
// off-by-one in the mask-line handling all show up as a mismatch at a named
// index rather than as a count that happens to come out right. Every value
// lands inside the 12-bit signed range the format promises, which is what
// makes the expected float exact.
int fakeWord(long long k, long long w) {
    return static_cast<int>(((k * 32768 + w) * 7) % 4096) - 2048;
}

// --- the fake daemon -------------------------------------------------------
//
// One listening socket, one thread per connection, and a command loop written
// straight from parser.y. It answers exactly as ops.c does - a decimal length
// or a negative errno, then the payload - which is the whole point: a driver
// proven against a fake that answers in a shape the real daemon does not use
// is a driver proven against nothing.
class FakeIiod {
public:
    enum class WhenEmpty { Error, Silent, Short };

    ~FakeIiod() { stop(); }

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
        // of which notices run_ within its own 100 ms receive timeout and
        // closes ITS OWN socket. Closing them from here instead would be a
        // double close the moment a worker was already on its way out.
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

    std::atomic<int> buffersToServe{1000000};
    std::atomic<WhenEmpty> whenEmpty{WhenEmpty::Error};
    std::atomic<int> emptyStatus{-19};
    // A command line the daemon should refuse, matched WHOLE. Whole rather
    // than by substring because "READ ... frequency" is a prefix of
    // "READ ... frequency_available", and a refusal that hit both would test
    // something other than what it says.
    void refuse(const std::string& line, int status) {
        std::lock_guard<std::mutex> lk(mutex_);
        refusals_[line] = status;
    }

private:
    void acceptLoop() {
        while (run_.load()) {
            // SELECT, THEN ACCEPT. SO_RCVTIMEO does not bound accept() on
            // Windows - it is documented as applying to receives - so an
            // acceptor that relied on it blocks for ever and stop()'s join
            // never returns. That is not a hypothetical: this fake hung the
            // whole suite on its first run, with no output at all, because
            // the first daemon's destructor could not join its acceptor.
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
        long long bufferIndex = 0;
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
                    key = w[1] + "/" + (w[2] == "OUTPUT" ? "out" : "in") + "/" + w[3] + "/" + w[4];
                } else {  // WRITE dev attr count
                    key = w[1] + "/" + w[2];
                }
                setAttr(key, value);
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
                // parser.y's VERSION rule: one bare line, no length prefix.
                sendText(s, "1.1.5f2e3b0\n");
            } else if (cmd == "PRINT") {
                // parser.y's PRINT rule: the length, the document, a newline
                // the length does not count.
                const std::string xml = kContextXml;
                sendText(s, std::to_string(xml.size()) + "\n" + xml + "\n");
            } else if (cmd == "TIMEOUT" || cmd == "SET" || cmd == "CLOSE") {
                sendText(s, "0\n");
            } else if (cmd == "OPEN") {
                // ops.c open_dev: a mask whose length is not exactly
                // (channels + 31) / 32 * 8 characters is -EINVAL before
                // anything else happens.
                // OPEN <device> <samples_count> <mask>, so the mask is the
                // FOURTH word (parser.y's OPEN rule).
                const std::string mask = w.size() >= 4 ? w[3] : std::string();
                sendText(s, mask.size() == 8 ? "0\n" : "-22\n");
                bufferIndex = 0;
            } else if (cmd == "READ") {
                std::string key;
                if (w.size() >= 5) {
                    key = w[1] + "/" + (w[2] == "OUTPUT" ? "out" : "in") + "/" + w[3] + "/" + w[4];
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
            } else if (cmd == "READBUF") {
                const long want = w.size() >= 3 ? std::atol(w[2].c_str()) : 0;
                if (buffersToServe.load() <= 0) {
                    const WhenEmpty mode = whenEmpty.load();
                    if (mode == WhenEmpty::Silent) {
                        // Answer nothing at all, ever. The client's own
                        // receive bound is what has to save it.
                        continue;
                    }
                    if (mode == WhenEmpty::Short) {
                        const long half = (want / 8) * 4;
                        std::string data(static_cast<std::size_t>(half), '\0');
                        sendText(s, std::to_string(half) + "\n00000003\n");
                        sendText(s, data);
                        continue;
                    }
                    sendText(s, std::to_string(emptyStatus.load()) + "\n");
                    continue;
                }
                buffersToServe.fetch_sub(1);
                const std::size_t wordCount = static_cast<std::size_t>(want) / 2;
                std::string data(static_cast<std::size_t>(want), '\0');
                for (std::size_t i = 0; i < wordCount; ++i) {
                    const int v = fakeWord(bufferIndex, static_cast<long long>(i));
                    const std::uint16_t raw = static_cast<std::uint16_t>(v & 0xFFFF);
                    data[2 * i] = static_cast<char>(raw & 0xFF);
                    data[2 * i + 1] = static_cast<char>((raw >> 8) & 0xFF);
                }
                ++bufferIndex;
                // ops.c send_data: the length, then the enabled-channel mask
                // - on EVERY readbuf, because rw_buffer sets new_client true
                // each time it is called - then the raw bytes.
                sendText(s, std::to_string(want) + "\n00000003\n");
                sendText(s, data);
            } else {
                sendText(s, "-22\n");  // -EINVAL, as yyerror answers
            }
        }
        closeSock(s);
    }

    Sock listener_ = kBadSock;
    std::uint16_t port_ = 0;
    std::atomic<bool> run_{false};
    std::thread acceptor_;
    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::vector<std::vector<std::string>> log_;
    std::map<std::string, std::string> attrs_;
    std::map<std::string, int> refusals_;
};

// A board's published state. Everything here is what the DEVICE says, and the
// tests change it to prove the driver reads rather than assumes.
void stockAd9363(FakeIiod& d) {
    d.setAttr("ad9361-phy/xo_correction", "39999976");
    d.setAttr("ad9361-phy/rx_path_rates",
              "BBPLL:983040000 ADC:245760000 R2:122880000 R1:61440000 RF:30720000 "
              "RXSAMP:2500000");
    d.setAttr("ad9361-phy/out/altvoltage0/frequency", "2400000000");
    d.setAttr("ad9361-phy/out/altvoltage0/frequency_available", "[325000000 1 3800000000]");
    d.setAttr("ad9361-phy/in/voltage0/sampling_frequency", "2500000");
    d.setAttr("ad9361-phy/in/voltage0/sampling_frequency_available", "[2083333 1 61440000]");
    d.setAttr("ad9361-phy/in/voltage0/rf_bandwidth", "18000000");
    d.setAttr("ad9361-phy/in/voltage0/rf_bandwidth_available", "[200000 1 56000000]");
    // As sysfs prints it: a number, a space and a unit. A driver that took
    // the whole string as a double would read 0.
    d.setAttr("ad9361-phy/in/voltage0/hardwaregain", "40.000000 dB");
    d.setAttr("ad9361-phy/in/voltage0/hardwaregain_available", "[-3 1 71]");
    d.setAttr("ad9361-phy/in/voltage0/gain_control_mode", "slow_attack");
    d.setAttr("ad9361-phy/in/voltage0/gain_control_mode_available",
              "manual fast_attack slow_attack hybrid");
    d.setAttr("ad9361-phy/in/voltage0/rf_port_select", "A_BALANCED");
}

// A scripted transport for the pure layer: what the client SENT, and what it
// is given back. No socket, no thread, no timing.
class ScriptedTransport : public iiod::Transport {
public:
    explicit ScriptedTransport(std::string reply) : reply_(std::move(reply)) {}

    bool sendAll(const void* data, std::size_t n) override {
        sent.append(static_cast<const char*>(data), n);
        return true;
    }
    bool recvAll(void* data, std::size_t n) override {
        if (pos_ + n > reply_.size()) {
            error_ = "the fake ran out of script";
            return false;
        }
        std::memcpy(data, reply_.data() + pos_, n);
        pos_ += n;
        return true;
    }
    void close() override {}
    const char* lastError() const override { return error_.c_str(); }

    std::string sent;

private:
    std::string reply_;
    std::size_t pos_ = 0;
    std::string error_;
};

// Waits for a predicate, bounded, so a test that would otherwise hang fails
// instead.
template <typename Fn>
bool waitFor(Fn fn, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fn()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fn();
}

// Bounds-safe indexing. A `CHECK(v.size() == n)` followed by `v[i]` is an
// out-of-bounds read in exactly the run that has something to report - the
// harness records a failed check and carries on, so the crash lands instead
// of the message. (mayhem-b200, 2026-08-13.)
std::string at(const std::vector<std::string>& v, std::size_t i) {
    return i < v.size() ? v[i] : std::string("<nothing sent>");
}

bool sameCommand(const char* label, const std::vector<std::string>& got, std::size_t i,
                 const char* want) {
    const std::string have = at(got, i);
    const bool ok = have == want;
    if (!ok) { std::printf("     %s[%zu]: got \"%s\", want \"%s\"\n", label, i, have.c_str(), want); }
    return ok;
}

void dump(const char* label, const std::vector<std::string>& v) {
    std::printf("     %s sent %zu commands:\n", label, v.size());
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::printf("       [%zu] %s\n", i, v[i].c_str());
    }
}

std::string uriFor(std::uint16_t port) {
    return "uri=ip:127.0.0.1:" + std::to_string(static_cast<unsigned>(port));
}

}  // namespace

int main() {
    // =====================================================================
    // 1. THE CONTEXT, parsed. What PRINT returns is the only description of
    //    the board there is; everything the driver does afterwards depends
    //    on getting it right.
    // =====================================================================
    {
        iiod::Context ctx;
        std::string error;
        const bool ok = iiod::parseContext(kContextXml, ctx, error);
        if (!ok) { std::printf("     parseContext said: %s\n", error.c_str()); }
        CHECK(ok);
        CHECK(ctx.devices.size() == 3);
        CHECK(ctx.attr("hw_model") == "Analog Devices PlutoSDR Rev.B (Z7010-AD9363A)");
        CHECK(ctx.attr("fw_version") == "v0.38");
        CHECK(ctx.attr("nothing_like_this").empty());

        const iiod::Device* phy = ctx.findDevice("ad9361-phy");
        CHECK(phy != nullptr);
        if (phy != nullptr) {
            CHECK(phy->id == "iio:device0");
            // The device's own attributes, and not the channels'.
            CHECK(phy->hasAttr("xo_correction"));
            CHECK(phy->hasAttr("filter_fir_config"));
            CHECK(!phy->hasAttr("hardwaregain"));
            // Four channels: two output LOs, an input voltage0 and an output
            // voltage0 - and the two voltage0s are told apart by DIRECTION,
            // which is the case that makes a naive id lookup return the
            // transmit channel's gain.
            CHECK(phy->channels.size() == 4);
            const iiod::Channel* rx = phy->findChannel("voltage0", false);
            const iiod::Channel* tx = phy->findChannel("voltage0", true);
            CHECK(rx != nullptr);
            CHECK(tx != nullptr);
            CHECK(rx != tx);
            if (rx != nullptr) {
                CHECK(rx->hasAttr("sampling_frequency_available"));
                CHECK(rx->hasAttr("rssi"));
                CHECK(!rx->hasScanElement);
            }
            if (tx != nullptr) { CHECK(!tx->hasAttr("sampling_frequency_available")); }
            // The LO is findable by its id and by the name the XML gives it,
            // exactly as the daemon's own lexer resolves a channel token.
            CHECK(phy->findChannel("altvoltage0", true) != nullptr);
            CHECK(phy->findChannel("RX_LO", true) != nullptr);
            CHECK(phy->findChannel("RX_LO", false) == nullptr);  // wrong direction
        }

        const iiod::Device* cap = ctx.findDevice("cf-ad9361-lpc");
        CHECK(cap != nullptr);
        if (cap != nullptr) {
            CHECK(cap->channels.size() == 2);
            CHECK(cap->channels[0].hasScanElement);
            CHECK(cap->channels[0].scanIndex == 0);
            CHECK(cap->channels[1].scanIndex == 1);
            // THE ESCAPED FORMAT, unescaped. "&gt;&gt;" is how XML carries
            // ">>", and a scanner that passed it through would fail to parse
            // every capture channel on every board.
            CHECK(cap->channels[0].scanFormat == "le:S12/16>>0");
        }

        // A document that is not a context at all, and one with no devices:
        // both are refused with a sentence rather than accepted as empty.
        iiod::Context bad;
        CHECK(!iiod::parseContext("<html><body>go away</body></html>", bad, error));
        CHECK(!error.empty());
        CHECK(!iiod::parseContext("<context name=\"network\"></context>", bad, error));
    }

    // =====================================================================
    // 2. THE SAMPLE LAYOUT, and the arithmetic that turns a word into a
    //    number between -1 and 1.
    // =====================================================================
    {
        iiod::SampleFormat f;
        CHECK(iiod::parseSampleFormat("le:S12/16>>0", f));
        CHECK(f.littleEndian);
        CHECK(f.isSigned);
        CHECK(f.bits == 12);
        CHECK(f.storageBits == 16);
        CHECK(f.shift == 0);
        CHECK(f.storageBytes() == 2);

        iiod::SampleFormat g;
        CHECK(iiod::parseSampleFormat("be:U14/16>>2", g));
        CHECK(!g.littleEndian);
        CHECK(!g.isSigned);
        CHECK(g.bits == 14);
        CHECK(g.shift == 2);

        CHECK(!iiod::parseSampleFormat("", f));
        CHECK(!iiod::parseSampleFormat("le:X12/16>>0", f));
        CHECK(!iiod::parseSampleFormat("le:S12", f));
        CHECK(!iiod::parseSampleFormat("le:S20/16>>0", f));  // more bits than it stores

        // The conversion, against words computed by hand from the format.
        iiod::SampleFormat s12;
        CHECK(iiod::parseSampleFormat("le:S12/16>>0", s12));
        struct WordCase {
            std::uint8_t lo;
            std::uint8_t hi;
            float want;
        };
        const WordCase cases[] = {
            {0x00, 0x00, 0.0f},                 // zero
            {0x01, 0x00, 1.0f / 2048.0f},       // the smallest positive step
            {0xFF, 0x07, 2047.0f / 2048.0f},    // the largest positive value
            {0x00, 0x08, -1.0f},                // 0x800: the sign bit of twelve
            {0xFF, 0xFF, -1.0f / 2048.0f},      // -1, already sign-extended by the board
            {0x00, 0xF8, -1.0f},                // 0xF800: the same -2048, extended
        };
        for (const WordCase& c : cases) {
            const std::uint8_t word[2] = {c.lo, c.hi};
            const float got = iiod::convertSample(word, s12);
            if (got != c.want) {
                std::printf("     convertSample(%02x %02x) = %.9f, want %.9f\n",
                            static_cast<unsigned>(c.lo), static_cast<unsigned>(c.hi),
                            static_cast<double>(got), static_cast<double>(c.want));
            }
            CHECK(got == c.want);
        }
    }

    // =====================================================================
    // 3. THE VALUE SHAPES: ranges, word lists and the OPEN mask.
    // =====================================================================
    {
        iiod::Range r;
        CHECK(iiod::parseRange("[70000000 1 6000000000]", r));
        CHECK_NEAR(r.min, 70.0e6, 0.5);
        CHECK_NEAR(r.step, 1.0, 1e-9);
        CHECK_NEAR(r.max, 6.0e9, 0.5);
        CHECK(iiod::parseRange("[-3 1 71]", r));
        CHECK_NEAR(r.min, -3.0, 1e-9);
        CHECK_NEAR(r.max, 71.0, 1e-9);
        CHECK(!iiod::parseRange("manual fast_attack", r));
        CHECK(!iiod::parseRange("[1 2]", r));
        CHECK(!iiod::parseRange("", r));

        const std::vector<std::string> modes =
            iiod::splitWords("manual fast_attack slow_attack hybrid");
        CHECK(modes.size() == 4);
        CHECK(!modes.empty() && modes[0] == "manual");
        CHECK(iiod::splitWords("   ").empty());

        // ops.c open_dev refuses any mask that is not exactly
        // (channels + 31) / 32 * 8 characters, so the WIDTH is as
        // load-bearing as the bits.
        CHECK(iiod::channelMask({0, 1}, 2) == "00000003");
        CHECK(iiod::channelMask({0}, 2) == "00000001");
        CHECK(iiod::channelMask({1}, 2) == "00000002");
        CHECK(iiod::channelMask({}, 2) == "00000000");
        CHECK(iiod::channelMask({0, 1}, 33).size() == 16);
        CHECK(iiod::channelMask({32}, 33) == "0000000100000000");

        CHECK(iiod::errnoName(-19).find("ENODEV") == 0);
        CHECK(iiod::errnoName(-22).find("EINVAL") == 0);
        CHECK(iiod::errnoName(-12345).empty());
    }

    // =====================================================================
    // 4. THE COMMANDS, byte for byte, through a transport with no socket in
    //    it. Each one names the daemon handler its reply shape came from.
    // =====================================================================
    {
        // parser.y VERSION: a bare line.
        {
            auto t = std::make_unique<ScriptedTransport>("1.1.5f2e3b0\n");
            ScriptedTransport* raw = t.get();
            iiod::Client c(std::move(t));
            std::string v;
            CHECK(c.version(v));
            CHECK(v == "1.1.5f2e3b0");
            CHECK(raw->sent == "VERSION\n");
        }
        // parser.y PRINT: length, document, a newline the length excludes.
        {
            const std::string xml = "<context name=\"n\"><device id=\"d\"/></context>";
            auto t = std::make_unique<ScriptedTransport>(std::to_string(xml.size()) + "\n" + xml +
                                                         "\n");
            ScriptedTransport* raw = t.get();
            iiod::Client c(std::move(t));
            std::string got;
            CHECK(c.print(got));
            CHECK(got == xml);
            CHECK(raw->sent == "PRINT\n");
        }
        // ops.c read_chn_attr: length, value, newline. The value comes back
        // with sysfs's trailing newline removed.
        {
            auto t = std::make_unique<ScriptedTransport>("11\n2400000000\n\n");
            ScriptedTransport* raw = t.get();
            iiod::Client c(std::move(t));
            std::string v;
            CHECK(c.readChannelAttr("ad9361-phy", true, "altvoltage0", "frequency", v));
            CHECK(v == "2400000000");
            CHECK(raw->sent == "READ ad9361-phy OUTPUT altvoltage0 frequency\n");
        }
        // ...and an INPUT channel, which is a different token in the same
        // position (lexer.l's IN_OUT rule).
        {
            auto t = std::make_unique<ScriptedTransport>("7\n2500000\n");
            ScriptedTransport* raw = t.get();
            iiod::Client c(std::move(t));
            std::string v;
            CHECK(c.readChannelAttr("ad9361-phy", false, "voltage0", "sampling_frequency", v));
            CHECK(v == "2500000");
            CHECK(raw->sent == "READ ad9361-phy INPUT voltage0 sampling_frequency\n");
        }
        // ops.c read_dev_attr, the device-attribute form with no channel.
        {
            auto t = std::make_unique<ScriptedTransport>("8\n39999976\n");
            ScriptedTransport* raw = t.get();
            iiod::Client c(std::move(t));
            std::string v;
            CHECK(c.readDeviceAttr("ad9361-phy", "xo_correction", v));
            CHECK(v == "39999976");
            CHECK(raw->sent == "READ ad9361-phy xo_correction\n");
        }
        // ops.c write_chn_attr: the count on the line, then exactly that many
        // bytes - the value AND its terminating NUL.
        {
            auto t = std::make_unique<ScriptedTransport>("11\n");
            ScriptedTransport* raw = t.get();
            iiod::Client c(std::move(t));
            CHECK(c.writeChannelAttr("ad9361-phy", true, "altvoltage0", "frequency",
                                     "1090000000"));
            const std::string want =
                std::string("WRITE ad9361-phy OUTPUT altvoltage0 frequency 11\n") +
                std::string("1090000000") + std::string(1, '\0');
            if (raw->sent != want) {
                std::printf("     WRITE sent %zu bytes, wanted %zu\n", raw->sent.size(),
                            want.size());
            }
            CHECK(raw->sent == want);
        }
        // ops.c set_buffers_count and open_dev, both of which answer a single
        // 0 - and BUFFERS_COUNT goes first, which block 7 proves on the wire.
        {
            auto t = std::make_unique<ScriptedTransport>("0\n0\n0\n");
            ScriptedTransport* raw = t.get();
            iiod::Client c(std::move(t));
            CHECK(c.setBuffersCount("cf-ad9361-lpc", 4));
            CHECK(c.openBuffer("cf-ad9361-lpc", 16384, "00000003"));
            CHECK(c.closeBuffer("cf-ad9361-lpc"));
            CHECK(raw->sent ==
                  "SET cf-ad9361-lpc BUFFERS_COUNT 4\nOPEN cf-ad9361-lpc 16384 00000003\n"
                  "CLOSE cf-ad9361-lpc\n");
        }
        // ops.c rw_dev -> send_data: length, mask, data. The MASK IS THERE
        // EVERY TIME (rw_buffer sets new_client on each call), which is the
        // single detail most likely to be got wrong and the hardest to
        // diagnose afterwards - it shifts the sample stream by nine bytes.
        {
            std::string script;
            for (int i = 0; i < 2; ++i) {
                script += "8\n00000003\n";
                script += std::string("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
            }
            auto t = std::make_unique<ScriptedTransport>(script);
            ScriptedTransport* raw = t.get();
            iiod::Client c(std::move(t));
            std::vector<std::uint8_t> data;
            std::string mask;
            CHECK(c.readBuf("cf-ad9361-lpc", 8, data, &mask));
            CHECK(data.size() == 8);
            CHECK(mask == "00000003");
            CHECK(!data.empty() && data[0] == 1);
            // The second one lands correctly only if the mask was consumed
            // both times.
            CHECK(c.readBuf("cf-ad9361-lpc", 8, data, &mask));
            CHECK(data.size() == 8);
            CHECK(!data.empty() && data[0] == 1);
            CHECK(raw->sent == "READBUF cf-ad9361-lpc 8\nREADBUF cf-ad9361-lpc 8\n");
        }
        // A negative errno line is the failure, and it carries NO mask and no
        // data behind it (ops.c rw_dev prints the value only when ret <= 0).
        {
            auto t = std::make_unique<ScriptedTransport>("-19\n");
            iiod::Client c(std::move(t));
            std::vector<std::uint8_t> data;
            CHECK(!c.readBuf("cf-ad9361-lpc", 8, data));
            CHECK(c.lastStatus() == -19);
            CHECK(std::string(c.lastError()).find("ENODEV") != std::string::npos);
            CHECK(data.empty());
        }
        // A short buffer is a fault, not something to stitch together.
        {
            auto t = std::make_unique<ScriptedTransport>(std::string("4\n00000003\n") +
                                                          std::string("\x01\x02\x03\x04", 4));
            iiod::Client c(std::move(t));
            std::vector<std::uint8_t> data;
            CHECK(!c.readBuf("cf-ad9361-lpc", 8, data));
            CHECK(std::string(c.lastError()).find("short") != std::string::npos);
        }
        // A reply that is not a number at all - an ssh banner on the wrong
        // port - is reported as such rather than parsed as zero.
        {
            auto t = std::make_unique<ScriptedTransport>("SSH-2.0-OpenSSH_9.2\n");
            iiod::Client c(std::move(t));
            std::string xml;
            CHECK(!c.print(xml));
            CHECK(std::string(c.lastError()).find("unexpected") != std::string::npos);
        }
    }

    // =====================================================================
    // 5. THE ARGS STRING: what the enumeration offers, and what open() takes
    //    back from it.
    // =====================================================================
    {
        const std::vector<NativeDeviceInfo> rows = cascade::source::enumeratePluto();
        CHECK(rows.size() == 2);
        if (rows.size() == 2) {
            CHECK(rows[0].driver == "pluto");
            CHECK(rows[0].args == "uri=ip:192.168.2.1");
            // THE LABEL SAYS NOTHING WAS CONTACTED, because nothing was.
            CHECK(rows[0].label == "ADALM-Pluto at ip:192.168.2.1 (not yet contacted)");
            CHECK(rows[1].args == "uri=ip:pluto.local");
            CHECK(rows[1].label.find("(not yet contacted)") != std::string::npos);
        }

        std::string host;
        std::uint16_t port = 0;
        std::string error;
        CHECK(cascade::source::parsePlutoUri("uri=ip:192.168.2.1", host, port, error));
        CHECK(host == "192.168.2.1");
        CHECK(port == 30431);
        CHECK(cascade::source::parsePlutoUri("uri=ip:pluto.local", host, port, error));
        CHECK(host == "pluto.local");
        CHECK(cascade::source::parsePlutoUri("uri=ip:10.0.0.7:12345", host, port, error));
        CHECK(host == "10.0.0.7");
        CHECK(port == 12345);
        CHECK(cascade::source::parsePlutoUri("host=192.168.3.9", host, port, error));
        CHECK(host == "192.168.3.9");
        // An empty args string is the board on the USB cable.
        CHECK(cascade::source::parsePlutoUri("", host, port, error));
        CHECK(host == "192.168.2.1");
        // A backend we cannot reach is refused BY NAME, not half-attempted.
        CHECK(!cascade::source::parsePlutoUri("uri=usb:1.4.5", host, port, error));
        CHECK(error.find("libiio") != std::string::npos);
    }

    // =====================================================================
    // 6. OPEN, against the fake daemon: the whole conversation, in order.
    // =====================================================================
    {
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);

        PlutoSource src;
        const bool opened = src.open(uriFor(daemon.port()));
        if (!opened) { std::printf("     open() said: %s\n", src.lastError()); }
        CHECK(opened);
        CHECK(src.isOpen());
        CHECK(!src.faulted());
        CHECK(std::string(src.driverKey()) == "pluto");
        CHECK(src.selfPaced());

        // ONE CONNECTION so far. The stream connection is not made until
        // start(), because a source that is merely selected must not hold a
        // capture buffer open on the board.
        CHECK(daemon.connections() == 1);

        const std::vector<std::string> c = daemon.commands(0);
        if (c.size() != 14) { dump("open", c); }
        CHECK(c.size() == 14);
        CHECK(sameCommand("open", c, 0, "VERSION"));
        CHECK(sameCommand("open", c, 1, "PRINT"));
        CHECK(sameCommand("open", c, 2, "READ ad9361-phy xo_correction"));
        CHECK(sameCommand("open", c, 3, "READ ad9361-phy rx_path_rates"));
        CHECK(sameCommand("open", c, 4,
                          "READ ad9361-phy OUTPUT altvoltage0 frequency_available"));
        CHECK(sameCommand("open", c, 5,
                          "READ ad9361-phy INPUT voltage0 sampling_frequency_available"));
        CHECK(sameCommand("open", c, 6,
                          "READ ad9361-phy INPUT voltage0 rf_bandwidth_available"));
        CHECK(sameCommand("open", c, 7,
                          "READ ad9361-phy INPUT voltage0 hardwaregain_available"));
        CHECK(sameCommand("open", c, 8,
                          "READ ad9361-phy INPUT voltage0 gain_control_mode_available"));
        CHECK(sameCommand("open", c, 9, "READ ad9361-phy OUTPUT altvoltage0 frequency"));
        CHECK(sameCommand("open", c, 10, "READ ad9361-phy INPUT voltage0 sampling_frequency"));
        CHECK(sameCommand("open", c, 11, "READ ad9361-phy INPUT voltage0 hardwaregain"));
        CHECK(sameCommand("open", c, 12, "READ ad9361-phy INPUT voltage0 gain_control_mode"));
        // The one thing open programs: the analogue bandwidth follows the
        // rate the board reported, because nothing else on screen would ever
        // reveal an inherited 200 kHz filter.
        CHECK(sameCommand("open", c, 13,
                          "WRITE ad9361-phy INPUT voltage0 rf_bandwidth 8 | 2500000<NUL>"));
        CHECK(daemon.attr("ad9361-phy/in/voltage0/rf_bandwidth") == "2500000");

        // What it learned, all of it read off the board.
        CHECK(src.phyDeviceName() == "ad9361-phy");
        CHECK(src.captureDeviceName() == "cf-ad9361-lpc");
        CHECK(src.hardwareModel() == "Analog Devices PlutoSDR Rev.B (Z7010-AD9363A)");
        CHECK(src.firmwareVersion() == "v0.38");
        CHECK(src.serialNumber() == "104473222c8700071700");
        CHECK(src.daemonVersion() == "1.1.5f2e3b0");
        CHECK_NEAR(src.xoCorrectionHz(), 39999976.0, 0.5);
        CHECK(src.rxPathRates().find("BBPLL:983040000") == 0);
        CHECK(std::string(src.name()).find("PlutoSDR Rev.B") != std::string::npos);
        CHECK(std::string(src.name()).find("127.0.0.1") != std::string::npos);

        CHECK_NEAR(src.centerFrequencyHz(), 2.4e9, 0.5);
        CHECK_NEAR(src.sampleRateHz(), 2.5e6, 0.5);
        // "40.000000 dB" is a number and a unit; only the number is the gain.
        CHECK_NEAR(src.gainDb("RX"), 40.0, 1e-9);
        CHECK(src.autoGainSupported());
        CHECK(src.autoGain());  // the board was left in slow_attack

        // THE LIMITS ARE THE BOARD'S. This one published the stock AD9363
        // range, so that is what is reported and the unlock is not claimed.
        double lo = 0.0, hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 325.0e6, 0.5);
        CHECK_NEAR(hi, 3.8e9, 0.5);
        CHECK(!src.ad9364Unlocked());

        const std::vector<cascade::source::GainInfo> g = src.gains();
        CHECK(g.size() == 1);
        if (!g.empty()) {
            CHECK(g[0].name == "RX");
            CHECK_NEAR(g[0].minDb, -3.0, 1e-9);
            CHECK_NEAR(g[0].maxDb, 71.0, 1e-9);
            CHECK(g[0].unit == cascade::source::GainUnit::Decibels);
        }

        const std::vector<double> rates = src.supportedSampleRatesHz();
        CHECK(rates.size() >= 3);
        if (!rates.empty()) {
            CHECK_NEAR(rates.front(), 2083333.0, 0.5);
            CHECK_NEAR(rates.back(), 61440000.0, 0.5);
            CHECK(std::is_sorted(rates.begin(), rates.end()));
        }

        CHECK(src.antennas().size() == 1);
        CHECK(src.setAntenna("A_BALANCED"));
        CHECK(!src.setAntenna("TX"));

        src.closeDevice();
        CHECK(!src.isOpen());
    }

    // =====================================================================
    // 7. A DIFFERENT BOARD, SAME CODE: the AD9364 unlock changes what the
    //    driver reports, because the driver asked rather than assumed. A
    //    table-driven driver passes block 6 and fails this one.
    // =====================================================================
    {
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);
        daemon.setAttr("ad9361-phy/out/altvoltage0/frequency_available",
                       "[70000000 1 6000000000]");
        daemon.setAttr("ad9361-phy/in/voltage0/hardwaregain_available", "[-10 1 62]");
        daemon.setAttr("ad9361-phy/in/voltage0/sampling_frequency_available",
                       "[521000 1 61440000]");

        PlutoSource src;
        CHECK(src.open(uriFor(daemon.port())));
        double lo = 0.0, hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 70.0e6, 0.5);
        CHECK_NEAR(hi, 6.0e9, 0.5);
        CHECK(src.ad9364Unlocked());
        const std::vector<cascade::source::GainInfo> g = src.gains();
        CHECK(g.size() == 1);
        if (!g.empty()) {
            CHECK_NEAR(g[0].minDb, -10.0, 1e-9);
            CHECK_NEAR(g[0].maxDb, 62.0, 1e-9);
        }
        // This board has a FIR loaded, so its floor is lower - and the driver
        // honours the board's number rather than the 2.083 MS/s that is true
        // of a stock one.
        CHECK(src.setSampleRateHz(1.0e6));
        CHECK_NEAR(src.sampleRateHz(), 1.0e6, 0.5);
        CHECK(!src.setSampleRateHz(400.0e3));
        CHECK(std::string(src.lastError()).find("filter_fir_config") != std::string::npos);
        src.closeDevice();
    }

    // =====================================================================
    // 8. A BOARD THAT PUBLISHES NO RANGE. "Not known" is an answer; a made-up
    //    range is not, and frequencyRangeHz's contract says false.
    // =====================================================================
    {
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);
        // The attribute is in the context XML, so it IS asked for - and the
        // daemon answers -ENOENT, exactly as ops.c does for an attribute it
        // cannot find.
        daemon.refuse("READ ad9361-phy OUTPUT altvoltage0 frequency_available", -2);

        PlutoSource src;
        CHECK(src.open(uriFor(daemon.port())));
        double lo = 1.0, hi = 2.0;
        CHECK(!src.frequencyRangeHz(lo, hi));
        CHECK(!src.ad9364Unlocked());
        // A refusal on an optional attribute is NOT a dead board: the source
        // opened, and it still tunes.
        CHECK(!src.faulted());
        CHECK(!src.deviceDead());
        CHECK(src.setCenterFrequencyHz(144.0e6));
        src.closeDevice();
    }

    // =====================================================================
    // 9. TUNE, RATE, GAIN AND AGC, each as the exact command it becomes.
    // =====================================================================
    {
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);
        daemon.setAttr("ad9361-phy/out/altvoltage0/frequency_available",
                       "[70000000 1 6000000000]");

        PlutoSource src;
        CHECK(src.open(uriFor(daemon.port())));
        const std::size_t base = daemon.commands(0).size();

        CHECK(src.setCenterFrequencyHz(1.09e9));
        CHECK_NEAR(src.centerFrequencyHz(), 1.09e9, 0.5);
        {
            const std::vector<std::string> c = daemon.commands(0);
            CHECK(c.size() == base + 1);
            CHECK(sameCommand("tune", c, base,
                              "WRITE ad9361-phy OUTPUT altvoltage0 frequency 11 | "
                              "1090000000<NUL>"));
        }
        CHECK(daemon.attr("ad9361-phy/out/altvoltage0/frequency") == "1090000000");

        // Outside the board's published range is refused, and nothing is
        // sent: a tune that silently lands somewhere else is worse than one
        // that does not happen.
        CHECK(!src.setCenterFrequencyHz(30.0e6));
        CHECK(!src.setCenterFrequencyHz(7.0e9));
        CHECK(daemon.commands(0).size() == base + 1);
        CHECK(!src.faulted());
        CHECK(std::string(src.lastError()).find("outside that") != std::string::npos);

        // A rate change is the rate AND the filter that goes with it - two
        // commands, always, because an AD9361 keeps its last bandwidth.
        const std::size_t beforeRate = daemon.commands(0).size();
        CHECK(src.setSampleRateHz(10.0e6));
        {
            const std::vector<std::string> c = daemon.commands(0);
            CHECK(c.size() == beforeRate + 2);
            CHECK(sameCommand("rate", c, beforeRate,
                              "WRITE ad9361-phy INPUT voltage0 sampling_frequency 9 | "
                              "10000000<NUL>"));
            CHECK(sameCommand("rate", c, beforeRate + 1,
                              "WRITE ad9361-phy INPUT voltage0 rf_bandwidth 9 | 10000000<NUL>"));
        }
        CHECK_NEAR(src.sampleRateHz(), 10.0e6, 0.5);

        // Above the board's ceiling is coerced down to it and said so; the
        // bandwidth follows and is CLAMPED to the bandwidth range, which is
        // narrower than the rate range on a real AD9361.
        CHECK(src.setSampleRateHz(100.0e6));
        CHECK_NEAR(src.sampleRateHz(), 61440000.0, 0.5);
        CHECK(daemon.attr("ad9361-phy/in/voltage0/rf_bandwidth") == "56000000");
        CHECK(std::string(src.lastError()).find("coerced") != std::string::npos);
        CHECK(!src.faulted());

        // Below it is refused with the sentence that names what would have to
        // be written to go lower.
        CHECK(!src.setSampleRateHz(1.0e6));
        CHECK(std::string(src.lastError()).find("filter_fir_config") != std::string::npos);

        // --- gains -------------------------------------------------------
        // The board was left in slow_attack, so a hand-set gain turns the AGC
        // off FIRST and then sets the number. Both commands, in that order.
        const std::size_t beforeGain = daemon.commands(0).size();
        CHECK(src.autoGain());
        CHECK(src.setGainDb("RX", 30.0));
        {
            const std::vector<std::string> c = daemon.commands(0);
            CHECK(c.size() == beforeGain + 2);
            CHECK(sameCommand("gain", c, beforeGain,
                              "WRITE ad9361-phy INPUT voltage0 gain_control_mode 7 | "
                              "manual<NUL>"));
            CHECK(sameCommand("gain", c, beforeGain + 1,
                              "WRITE ad9361-phy INPUT voltage0 hardwaregain 3 | 30<NUL>"));
        }
        CHECK(!src.autoGain());
        CHECK_NEAR(src.gainDb("RX"), 30.0, 1e-9);

        // Out of range is CLAMPED and the readback says what was actually
        // programmed (device_source.hpp's contract).
        CHECK(src.setGainDb("RX", 999.0));
        CHECK_NEAR(src.gainDb("RX"), 71.0, 1e-9);
        CHECK(daemon.attr("ad9361-phy/in/voltage0/hardwaregain") == "71");
        CHECK(src.setGainDb("RX", -50.0));
        CHECK_NEAR(src.gainDb("RX"), -3.0, 1e-9);
        CHECK(daemon.attr("ad9361-phy/in/voltage0/hardwaregain") == "-3");

        // An unknown gain name is a refusal, not a command.
        const std::size_t beforeBadGain = daemon.commands(0).size();
        CHECK(!src.setGainDb("LNA", 20.0));
        CHECK(daemon.commands(0).size() == beforeBadGain);

        // AGC back on is slow_attack, the mode that settles rather than the
        // one that chases.
        const std::size_t beforeAgc = daemon.commands(0).size();
        CHECK(src.setAutoGain(true));
        {
            const std::vector<std::string> c = daemon.commands(0);
            CHECK(sameCommand("agc", c, beforeAgc,
                              "WRITE ad9361-phy INPUT voltage0 gain_control_mode 12 | "
                              "slow_attack<NUL>"));
        }
        CHECK(src.autoGain());
        CHECK(src.setAutoGain(false));
        CHECK(!src.autoGain());
        CHECK(daemon.attr("ad9361-phy/in/voltage0/gain_control_mode") == "manual");

        src.closeDevice();
    }

    // =====================================================================
    // 10. STREAMING: the second connection, the OPEN/READBUF/CLOSE sequence
    //     with its byte counts, and every sample once and in order.
    // =====================================================================
    {
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);
        daemon.buffersToServe.store(2);

        PlutoSource src;
        CHECK(src.open(uriFor(daemon.port())));
        CHECK(src.start());
        CHECK(src.running());
        CHECK(waitFor([&daemon] { return daemon.connections() >= 2; },
                      std::chrono::milliseconds(1000)));

        constexpr std::size_t kSamplesPerBuffer = 16384;
        constexpr std::size_t kBufferBytes = kSamplesPerBuffer * 4;
        const std::size_t wantSamples = 2 * kSamplesPerBuffer;

        std::vector<std::complex<float>> got;
        got.reserve(wantSamples + 64);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (got.size() < wantSamples && std::chrono::steady_clock::now() < deadline) {
            std::complex<float> chunk[1024];
            const std::size_t n = src.read(chunk, 1024);
            for (std::size_t i = 0; i < n; ++i) { got.push_back(chunk[i]); }
        }
        if (got.size() != wantSamples) {
            std::printf("     streaming: got %zu samples, expected %zu (last error: %s)\n",
                        got.size(), wantSamples, src.lastError());
        }
        CHECK(got.size() == wantSamples);

        // Every sample, against the same ramp the fake generated - so a lost
        // buffer, a repeated one or a mask line read at the wrong time all
        // show up at a named index.
        std::size_t firstBad = wantSamples;
        for (std::size_t i = 0; i < wantSamples && i < got.size(); ++i) {
            const long long buffer = static_cast<long long>(i / kSamplesPerBuffer);
            const long long within = static_cast<long long>(i % kSamplesPerBuffer);
            const float wantI = static_cast<float>(fakeWord(buffer, 2 * within)) / 2048.0f;
            const float wantQ = static_cast<float>(fakeWord(buffer, 2 * within + 1)) / 2048.0f;
            if (got[i].real() != wantI || got[i].imag() != wantQ) {
                firstBad = i;
                std::printf(
                    "     streaming: sample %zu is (%.6f, %.6f), expected (%.6f, %.6f)\n", i,
                    static_cast<double>(got[i].real()), static_cast<double>(got[i].imag()),
                    static_cast<double>(wantI), static_cast<double>(wantQ));
                break;
            }
        }
        CHECK(firstBad == wantSamples);
        CHECK(src.droppedBuffers() == 0);

        // The stream connection's conversation, from the first command to the
        // last: the buffer count BEFORE the open (ops.c reads it only when it
        // makes the buffer), the open with the samples count and the mask,
        // then readbufs of exactly one buffer each.
        const std::vector<std::string> s = daemon.commands(1);
        if (s.size() < 4) { dump("stream", s); }
        CHECK(s.size() >= 4);
        CHECK(sameCommand("stream", s, 0, "TIMEOUT 4000"));
        CHECK(sameCommand("stream", s, 1, "SET cf-ad9361-lpc BUFFERS_COUNT 4"));
        CHECK(sameCommand("stream", s, 2, "OPEN cf-ad9361-lpc 16384 00000003"));
        // 65536 is 16384 samples of two 16-bit channels, which is what the
        // READBUF above asks for and what kBufferBytes computes independently
        // from the format in the context.
        CHECK(sameCommand("stream", s, 3,
                          ("READBUF cf-ad9361-lpc " + std::to_string(kBufferBytes)).c_str()));

        const std::string line = src.streamHealthLine();
        CHECK(line.find("source: stream health - reads ") == 0);
        CHECK(line.find(", with samples ") != std::string::npos);
        CHECK(line.find(", timeouts ") != std::string::npos);
        CHECK(line.find(", overflows ") != std::string::npos);
        CHECK(line.find(", errors ") != std::string::npos);
        CHECK(line.find(", longest gap ") != std::string::npos);
        CHECK(line.find(" samples in ") != std::string::npos);

        // The board ran out of buffers and answered -ENODEV, which is the
        // device going: faulted, dead, and named.
        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(2000)));
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(std::string(src.lastError()).find("stopped answering") != std::string::npos);
        // read() on a faulted source is the contract's 0, not a hang.
        std::complex<float> tail[8];
        CHECK(src.read(tail, 8) == 0);
        // Nothing further is attempted on a dead board.
        CHECK(!src.start());
        CHECK(!src.setCenterFrequencyHz(500.0e6));
        src.closeDevice();
    }

    // =====================================================================
    // 11. STOP WHILE STREAMING comes back promptly, and CLOSEs the capture
    //     device on its way out.
    // =====================================================================
    {
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);

        const unsigned long long abandonedBefore = PlutoSource::readersAbandoned();
        PlutoSource src;
        CHECK(src.open(uriFor(daemon.port())));
        CHECK(src.start());
        CHECK(waitFor([&daemon] { return daemon.commands(1).size() >= 5; },
                      std::chrono::milliseconds(2000)));

        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        if (elapsed >= 500) { std::printf("     stop() took %lld ms\n", (long long)elapsed); }
        CHECK(elapsed < 500);
        CHECK(!src.running());
        CHECK(!src.faulted());
        // The reader EXITED rather than being abandoned. Elapsed time alone
        // would still pass with the bound deleted; this number is the
        // difference.
        CHECK(PlutoSource::readersAbandoned() == abandonedBefore);

        const std::vector<std::string> s = daemon.commands(1);
        CHECK(!s.empty() && s.back() == "CLOSE cf-ad9361-lpc");

        // ...and it can be started again, on a fresh connection.
        CHECK(src.start());
        CHECK(waitFor([&daemon] { return daemon.connections() >= 3; },
                      std::chrono::milliseconds(1000)));
        src.stop();
        src.closeDevice();
    }

    // =====================================================================
    // 12. A DAEMON THAT STOPS ANSWERING hits the bounded receive, and the
    //     bound is MEASURED rather than assumed.
    // =====================================================================
    {
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);
        daemon.buffersToServe.store(1);
        daemon.whenEmpty.store(FakeIiod::WhenEmpty::Silent);

        const unsigned long long abandonedBefore = PlutoSource::readersAbandoned();
        PlutoSource src;
        CHECK(src.open(uriFor(daemon.port())));
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(src.start());
        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(6000)));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        // iiod::kReplyWait is 2000 ms and that is what the socket spends. The
        // LOWER edge matters as much as the upper one: a fault that arrived
        // instantly would mean nothing was ever waited for.
        if (!(elapsed >= 1500 && elapsed < 4000)) {
            std::printf("     a silent daemon faulted after %lld ms; expected about %lld\n",
                        (long long)elapsed, (long long)iiod::kReplyWait.count());
        }
        CHECK(elapsed >= 1500);
        CHECK(elapsed < 4000);
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(std::string(src.lastError()).find("timed out") != std::string::npos);

        // AND THE TWO BOUNDS COMPOSE. The reader's longest legitimate stall
        // is one receive (2000 ms) and the join bound is 2500 ms, so a silent
        // board must NOT cost an abandoned thread - which is the whole reason
        // kReaderJoinWait is longer here than on the USB drivers.
        const auto t1 = std::chrono::steady_clock::now();
        src.stop();
        const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t1)
                                .count();
        CHECK(stopMs < 500);
        CHECK(PlutoSource::readersAbandoned() == abandonedBefore);
        src.closeDevice();
    }

    // =====================================================================
    // 13. A SHORT SAMPLE BUFFER faults the source rather than being stitched
    //     together into a picture that is quietly wrong.
    // =====================================================================
    {
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);
        daemon.buffersToServe.store(0);
        daemon.whenEmpty.store(FakeIiod::WhenEmpty::Short);

        PlutoSource src;
        CHECK(src.open(uriFor(daemon.port())));
        CHECK(src.start());
        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(3000)));
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(std::string(src.lastError()).find("short") != std::string::npos);
        src.stop();
        src.closeDevice();
    }

    // =====================================================================
    // 14. WHAT IS NOT THERE fails cleanly, with a sentence.
    // =====================================================================
    {
        // A refused connection: a port nothing is listening on. Bound and
        // released first, so the number is one the machine agreed was free
        // rather than one picked out of the air.
        std::uint16_t deadPort = 0;
        {
            FakeIiod probe;
            std::string error;
            CHECK(probe.start(error));
            deadPort = probe.port();
            probe.stop();
        }
        PlutoSource src;
        const bool opened = src.open(uriFor(deadPort));
        CHECK(!opened);
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("could not reach the Pluto") != std::string::npos);
        // Nothing to stop, nothing to read, nothing to crash on.
        src.stop();
        std::complex<float> buf[4];
        CHECK(src.read(buf, 4) == 0);
        CHECK(!src.start());
        CHECK(!src.setCenterFrequencyHz(100.0e6));
        src.closeDevice();
    }
    {
        // Something that answers but is not an IIO daemon. The fake refuses
        // VERSION with -EINVAL, which is what iiod's own yyerror answers for
        // a line it cannot parse - so this is the shape a wrong service on
        // the right port produces, not an invented one.
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        daemon.refuse("VERSION", -22);
        PlutoSource src;
        CHECK(!src.open(uriFor(daemon.port())));
        CHECK(std::string(src.lastError()).find("not a Pluto") == std::string::npos);
        CHECK(std::string(src.lastError()).find("IIO daemon") != std::string::npos);
        src.closeDevice();
    }
    {
        // An IIO context with no AD9361 in it - a real IIO device that is
        // simply not a radio.
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);
        daemon.refuse("PRINT", -22);
        PlutoSource src;
        CHECK(!src.open(uriFor(daemon.port())));
        CHECK(std::string(src.lastError()).find("describe itself") != std::string::npos);
        src.closeDevice();
    }
    {
        // A board whose CURRENT state cannot be read is not opened: a source
        // that cannot say what frequency it is on has nothing to show.
        FakeIiod daemon;
        std::string error;
        CHECK(daemon.start(error));
        stockAd9363(daemon);
        daemon.refuse("READ ad9361-phy OUTPUT altvoltage0 frequency", -5);
        PlutoSource src;
        CHECK(!src.open(uriFor(daemon.port())));
        CHECK(!src.isOpen());
        src.closeDevice();
    }

    return testSummary("test_pluto_source");
}
