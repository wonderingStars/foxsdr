// iiod_client.hpp - FoxSDR's own client for the IIOD network protocol, the
// text command language an ADALM-Pluto's `iiod` daemon speaks on TCP port
// 30431. No libiio, no SoapySDR, no vendor library of any kind: a socket, a
// handful of one-line commands, and a parser for the context XML the daemon
// prints.
//
// PROVENANCE. The command grammar and the shape of every reply were taken
// from the IIOD daemon's OWN parser and operation handlers - iiod/lexer.l,
// iiod/parser.y and iiod/ops.c of libiio (LGPL-2.1-or-later, Analog Devices).
// Those files were read as DOCUMENTATION of a wire protocol, the way a
// datasheet is read: nothing here is copied from them, nothing links against
// libiio, and no libiio header is included anywhere in this product. Each
// command below names the handler its reply format came from, so a future
// reader can check the claim rather than trust it. The protocol itself is not
// a secret - iiod's own HELP text (parser.y, the HELP rule) lists every
// command this file sends.
//
// WHY A CLIENT AND NOT A DRIVER. The Pluto is the only radio FoxSDR reaches
// over a network rather than over USB, and the split matters: everything in
// this file is about BYTES ON A SOCKET and can be proven against a scripted
// transport with no sockets at all, or against a fake daemon on 127.0.0.1.
// Everything about receivers, gains and sample conversion lives next door in
// pluto_source.*. The seam is the Transport below: the driver never touches a
// socket, and the test can hand the client anything that moves bytes.
//
// THE THREE RULES, the same ones src/usb/usb_device.hpp states for USB:
//   1. Enumeration never opens a device. A network cannot be walked without
//      probing it, so enumeratePluto() offers the two addresses a Pluto is
//      reachable at and contacts neither (see pluto_source.hpp).
//   2. We own every thread. There is no callback from a vendor library here
//      because there is no vendor library.
//   3. Every wait is bounded. A connect, a send and a receive all carry a
//      timeout, so a Pluto that is unplugged mid-stream gives a socket error
//      within kReplyWait and never a hang.
//
// THREADING. One Client owns one connection and is NOT internally locked: it
// is a serial conversation, and two threads interleaving commands on one
// socket would interleave their replies too. The driver keeps two - a control
// connection and a stream connection - which is what lets a retune happen
// while the reader thread is parked in a READBUF, and is why libiio's own
// network backend does the same.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cascade::source::iiod {

// The daemon's registered port. iiod listens here on the Pluto's USB network
// interface (192.168.2.1 by default) and on its Ethernet/WiFi address.
constexpr std::uint16_t kDefaultPort = 30431;

// --- the bounded waits ----------------------------------------------------
//
// Named rather than written at their call sites because
// tests/test_shutdown_budget.cpp discovers `constexpr std::chrono` constants
// under src/ and refuses to go green until each is classified in its
// kKnownWaits table. A wait the budget has never been introduced to is a wait
// nobody has costed.

// How long a connect attempt is given before it is abandoned. Generous
// because the first thing a user does with a Pluto is plug it in and wait for
// the USB network interface to come up, and a too-eager bound would report
// "not there" to somebody whose board is still booting. It is spent on
// whichever thread called open(), never on the teardown path.
constexpr std::chrono::milliseconds kConnectWait{3000};

// The bound on every send and every receive once connected: the socket's own
// SO_SNDTIMEO / SO_RCVTIMEO. A daemon that stops answering costs one of these
// and then the source is faulted - the "Pluto that disappears gives a socket
// error, never a hang" rule, and the only thing standing between an
// unplugged board and a frozen GUI.
constexpr std::chrono::milliseconds kReplyWait{2000};

// --- the transport --------------------------------------------------------

// A bounded, blocking byte stream. Deliberately tiny: the only things the
// protocol needs are "put these bytes there" and "get exactly this many
// back", both of which must fail rather than block for ever.
class Transport {
public:
    virtual ~Transport() = default;

    // Exactly n bytes, or false with lastError() set. A partial send is a
    // failure, not a short return: half a command is worse than none, because
    // the daemon would then parse the remainder of the next one.
    virtual bool sendAll(const void* data, std::size_t n) = 0;

    // Exactly n bytes, or false. A timeout is a failure here for the same
    // reason: the protocol is self-describing, so a reply that is late is a
    // reply that is lost - there is no resynchronisation point in a stream of
    // raw samples.
    virtual bool recvAll(void* data, std::size_t n) = 0;

    // Idempotent. After it, every send and receive fails.
    virtual void close() = 0;

    virtual const char* lastError() const = 0;
};

// A TCP connection to host:port, bounded by connectWait for the connect and
// by ioWait for every later send and receive. Returns nullptr with `error`
// set. `host` may be a dotted quad or a name (pluto.local, resolved by
// whatever mDNS the machine has - we do not implement one).
std::unique_ptr<Transport> connectTcp(const std::string& host, std::uint16_t port,
                                      std::chrono::milliseconds connectWait,
                                      std::chrono::milliseconds ioWait, std::string& error);

// --- the context ----------------------------------------------------------
//
// What PRINT returns, parsed. This is an IIO context: devices, each with
// attributes and channels, each channel with attributes and - for a capture
// device - a scan element describing how its samples are laid out. We keep
// the NAMES only, never cached values: an attribute's value is read when it
// is wanted, because a cached sample rate that the board has since changed is
// a number that lies.

struct Channel {
    std::string id;    // "voltage0", "altvoltage0" - what the commands use
    std::string name;  // "RX_LO" and friends; empty for most
    bool output = false;
    std::vector<std::string> attrs;
    // The scan element, present only on a capture device's channels.
    bool hasScanElement = false;
    int scanIndex = -1;
    std::string scanFormat;  // verbatim, e.g. "le:S12/16>>0"

    bool hasAttr(const std::string& attr) const;
};

struct Device {
    std::string id;    // "iio:device0"
    std::string name;  // "ad9361-phy", "cf-ad9361-lpc"
    std::vector<std::string> attrs;
    std::vector<Channel> channels;

    bool hasAttr(const std::string& attr) const;
    // By id first, then by name, because the daemon's own lexer resolves a
    // channel token either way (lexer.l, the WANT_CHN rule, which calls
    // iio_device_find_channel).
    const Channel* findChannel(const std::string& idOrName, bool output) const;
    std::size_t channelCount() const { return channels.size(); }
};

struct Context {
    std::vector<Device> devices;
    // The context attributes iiod prints above the devices: hw_model,
    // hw_serial, the firmware version. Carried because "which board is this
    // and what is on it" is the first question any Pluto problem report needs
    // answered.
    std::vector<std::pair<std::string, std::string>> attrs;

    const Device* findDevice(const std::string& name) const;
    std::string attr(const std::string& name) const;
};

// A deliberately small XML reader, not a general one. The context document is
// machine-generated by one program, has no namespaces, no CDATA and no mixed
// content, and we only want four element names out of it - so a scanner that
// understands exactly that is less code and less risk than a parser that
// understands everything. Returns false with `error` on malformed input; an
// unknown element is SKIPPED rather than rejected, because a newer iiod
// adding one must not stop an older-looking radio from opening.
bool parseContext(const std::string& xml, Context& out, std::string& error);

// --- the sample layout ----------------------------------------------------

// A scan element's format string, e.g. "le:S12/16>>0": little-endian, signed,
// 12 significant bits inside a 16-bit word, shifted right by 0. The AD9361
// capture device reports exactly that, but the conversion below is driven by
// what was PARSED rather than by that knowledge - a board that reports
// something else must convert as it says, not as we assumed.
struct SampleFormat {
    bool littleEndian = true;
    bool isSigned = true;
    int bits = 16;
    int storageBits = 16;
    int shift = 0;

    std::size_t storageBytes() const { return static_cast<std::size_t>((storageBits + 7) / 8); }
};

bool parseSampleFormat(const std::string& text, SampleFormat& out);

// One stored word of `fmt` from little- or big-endian bytes, shifted and
// sign-extended, scaled into [-1, 1). Named and separate so a test can pin
// the arithmetic against scripted words without a socket anywhere near it.
float convertSample(const std::uint8_t* word, const SampleFormat& fmt);

// --- attribute value shapes ----------------------------------------------

// The "[min step max]" triple an IIO *_available attribute uses for a
// continuous range. False when the text is not that shape - in which case the
// caller must say the limit is unknown rather than invent one.
struct Range {
    double min = 0.0;
    double step = 0.0;
    double max = 0.0;
};
bool parseRange(const std::string& text, Range& out);

// Whitespace-separated words, for the list-shaped *_available attributes
// ("manual fast_attack slow_attack hybrid").
std::vector<std::string> splitWords(const std::string& text);

// The channel mask OPEN takes: one 8-hex-digit word per 32 channels, most
// significant word first, exactly `(channelCount + 31) / 32 * 8` characters.
// The daemon REJECTS any other length outright (ops.c open_dev: `if (len !=
// nb_words * 8) return -EINVAL`), which is why this is computed from the
// channel count in the context rather than written as a literal.
std::string channelMask(const std::vector<std::size_t>& enabled, std::size_t channelCount);

// --- the client -----------------------------------------------------------

class Client {
public:
    explicit Client(std::unique_ptr<Transport> transport);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool valid() const { return transport_ != nullptr; }
    void close();

    // VERSION (parser.y, the VERSION rule): one line, "major.minor.git", with
    // no length prefix. Sent first because it is the cheapest possible proof
    // that what answered the socket is an iiod and not, say, an ssh server.
    bool version(std::string& out);

    // PRINT (parser.y, the PRINT rule): a decimal length line, then that many
    // bytes of XML, then a newline the length does NOT count.
    bool print(std::string& xml);

    // TIMEOUT <ms> (ops.c set_timeout): how long the DAEMON waits on the
    // hardware before it gives up on a buffer. Ours is the socket timeout;
    // this one stops the daemon parking for ever on a board that has stopped
    // producing, which would strand our READBUF for exactly as long.
    bool setTimeoutMs(long ms);

    // READ <dev> <attr> and READ <dev> INPUT|OUTPUT <chn> <attr> (ops.c
    // read_dev_attr / read_chn_attr): a decimal length line - or a negative
    // errno, which is the failure - then that many bytes and a newline. The
    // value is returned with any trailing NUL and whitespace removed, because
    // the daemon passes on whatever sysfs gave it and sysfs numbers carry a
    // trailing newline of their own.
    bool readDeviceAttr(const std::string& device, const std::string& attr, std::string& value);
    bool readChannelAttr(const std::string& device, bool output, const std::string& channel,
                         const std::string& attr, std::string& value);

    // WRITE ... <bytes_count> followed by that many bytes (ops.c
    // write_dev_attr / write_chn_attr, both of which read_all() exactly the
    // count and hand the bytes straight to the attribute). We send the value
    // with its terminating NUL so the count is unambiguous and the daemon
    // never has to guess where a number ends. The reply is the byte count
    // written, or a negative errno.
    bool writeDeviceAttr(const std::string& device, const std::string& attr,
                         const std::string& value);
    bool writeChannelAttr(const std::string& device, bool output, const std::string& channel,
                          const std::string& attr, const std::string& value);

    // SET <dev> BUFFERS_COUNT <n> (ops.c set_buffers_count). MUST be sent
    // BEFORE the OPEN it applies to: the count is stashed on the device and
    // only read when the buffer is created, which happens inside OPEN
    // (create_buf_and_blocks reads dev_pdata->nb_blocks). Sent afterwards it
    // succeeds, changes nothing, and the stream runs with the default depth.
    bool setBuffersCount(const std::string& device, long count);

    // OPEN <dev> <samples_count> <mask> (ops.c open_dev) and CLOSE <dev>
    // (ops.c close_dev). Both answer one line: 0, or a negative errno.
    bool openBuffer(const std::string& device, std::size_t samplesCount,
                    const std::string& mask);
    bool closeBuffer(const std::string& device);

    // READBUF <dev> <bytes_count> (ops.c rw_dev -> rw_buffer -> send_data).
    // The reply on success is THREE parts, in this order:
    //   1. a decimal length line - how many sample bytes follow;
    //   2. the enabled-channel mask, as hex words and a newline. This is sent
    //      on EVERY readbuf, not only the first: rw_buffer sets
    //      thd->new_client = true each time it is called, and send_data emits
    //      the mask whenever that flag is set. A client that expected it once
    //      would be one mask line out of step from the second buffer onward,
    //      and would decode sample data starting nine bytes late for ever.
    //   3. the raw sample bytes.
    // On failure it is a single negative errno line with nothing after it.
    //
    // We ask for exactly one buffer's worth, which is what makes the reply
    // that simple: send_data delivers min(block, requested), so a request no
    // larger than the block is satisfied in one go and the daemon's
    // short-read path (a trailing "0" line, rw_buffer's `ret > 0 && ret < nb`)
    // cannot be reached. A SHORT reply is therefore a fault, and is treated
    // as one rather than stitched together.
    bool readBuf(const std::string& device, std::size_t bytes, std::vector<std::uint8_t>& out,
                 std::string* mask = nullptr);

    // The negative errno the daemon last answered, 0 when the last failure
    // was not a protocol one (a socket error, a malformed reply). Kept apart
    // from lastError() because "-19" and "the connection dropped" are
    // different remedies for the user.
    long lastStatus() const { return status_; }
    const char* lastError() const { return error_.c_str(); }

private:
    // One command line, terminated with the '\n' the lexer's END rule needs.
    bool sendLine(const std::string& line);
    // Up to and including a '\n', which is removed. Bounded in length: a peer
    // that answers megabytes without a newline is a peer we stop talking to
    // rather than one we buffer for.
    bool recvLine(std::string& out);
    // A reply line as a decimal. False (with error_ set) on a negative errno
    // or unparseable text; `value` is the non-negative number otherwise.
    bool recvStatus(long& value, const char* what);
    // The READ half both attribute readers share.
    bool readAttrReply(std::string& value, const char* what);
    // The WRITE half both attribute writers share.
    bool writeAttrPayload(const std::string& value, const char* what);

    void fail(std::string message);

    std::unique_ptr<Transport> transport_;
    std::string error_;
    long status_ = 0;
};

// The errno name for a negative status, for a message a user can act on
// ("-19" means nothing; "ENODEV - no such device" means the device name is
// wrong). Empty for a number this table does not know, which is then reported
// as the bare number rather than as a guess.
std::string errnoName(long negativeStatus);

}  // namespace cascade::source::iiod
