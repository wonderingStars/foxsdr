// raw_iq_net_source.hpp - a radio that is a stream of raw I/Q on a network socket (engine serial lane A, row R5).
//
// WHAT IT IS. Anything that can send interleaved I, Q, I, Q ... over TCP or UDP becomes a radio: another FoxSDR's network I/Q out (net/iq_out.hpp: this is its receiving end), an rtl_sdr or
// rx_sdr pipe through netcat, a GNU Radio or SoapySDR sink, a script. There is no standard framing for this; the one chosen is the one FoxSDR's own sender uses, so the pair works with no setting
// but the address, and a stream from anything else needs only to say its format and rate.
//
// ADDRESS (the text after "open-rawiq:"):
//     [tcp://|udp://]host:port[?key=value&key=value ...]
//   tcp (the default): FoxSDR connects to host:port and reads the stream.
//   udp: FoxSDR listens on host:port (the local address and port to bind: say 127.0.0.1 to hear this computer only; 0.0.0.0 is spelled out when it is meant) and reads datagrams.
//   fmt=u8|s16|f32     the sample format when there is no header: u8 = unsigned bytes, (u - 127.5) / 127.5 (rtl_sdr's); s16 = little-endian signed, s / 32767; f32 = little-endian IEEE single (default)
//   rate=<Hz>          the sample rate; REQUIRED without a header (1 000 to 100 000 000)
//   freq=<Hz>          the centre frequency to show (default 0; a stream has no tuner: the number is a label)
//   hdr=1              the stream carries FoxSDR's 28-byte FXIQ header (see net/iq_out.hpp): sent ONCE at the start of a TCP connection, at the start of EVERY UDP datagram. The format, the
//                      rate and the centre then come from the header (the keys above are ignored); a header that changes the format or the rate mid-stream is a fault. The first header is
//                      waited for (3 s) when the source is opened, so the rate is known before the first sample.
//   timeout=ms         how long the sender may be silent before the radio is declared lost (default 3000)
//
// ON THE WIRE (headerless): the samples, nothing else, little-endian; a TCP read may end in the middle of a sample and the rest comes with the next (the odd bytes are carried); an odd tail of a UDP
// datagram is dropped. With a header, every UDP datagram's sequence number is checked: a gap counts the datagrams missing (droppedBuffers()), a repeat or a stray datagram that is not a FXIQ one
// is dropped and counted, never fatal; a TCP stream that stops, closes or breaks is a fault with a sentence. Non-finite floats are read as zero.
//
// The pump is NetSampleSource's: one reader thread, the ring, the keep-or-discard gate, the bounded join. Nothing personal is read or kept: this is a radio's own samples.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/net_sample_source.hpp"

namespace cascade::source {

namespace rawiq {

enum class Format : int { None = 0, U8 = 1, S16 = 2, F32 = 3 };
inline constexpr std::size_t kHeaderBytes = 28;
inline constexpr double kMinRateHz = 1000.0;
inline constexpr double kMaxRateHz = 100e6;

struct Header {
    Format format = Format::None;
    std::uint32_t rateHz = 0;
    double centreHz = 0.0;
    std::uint32_t sequence = 0;
};

// FXIQ version 1: false (out untouched) for a short buffer, a wrong magic or version, a format outside 1..3, a rate outside [kMinRateHz, kMaxRateHz], a centre that is not finite or beyond 1e13.
bool parseHeader(const unsigned char* p, std::size_t n, Header& out);
// Bytes in one I/Q pair of this format (0 for None).
std::size_t pairBytes(Format f);
// Decodes `pairs` whole I/Q pairs (nothing is read past pairs * pairBytes(f)).
void decode(Format f, const unsigned char* bytes, std::size_t pairs, std::complex<float>* out);

struct Spec {
    bool udp = false;
    std::string host;
    std::uint16_t port = 0;
    bool header = false;
    Format format = Format::F32;
    double rateHz = 0.0;
    double centreHz = 0.0;
    int timeoutMs = 3000;
};
// False with a sentence in `error` for anything the grammar above does not take (an unknown key, a missing or odd rate, a port outside 1..65535, a format that is not one of the three, ...).
bool parseSpec(const std::string& args, Spec& out, std::string& error);

}  // namespace rawiq

class RawIqNetSource : public NetSampleSource {
public:
    RawIqNetSource() = default;
    ~RawIqNetSource() override;

    const char* driverKey() const override { return "rawiq"; }
    bool open(const std::string& args) override;
    // The same with a transport the caller made (tests): the address is parsed as usual and tells the framing, the transport is used instead of connecting.
    bool openWithTransportForTest(const std::string& args, std::shared_ptr<iiod::Transport> t);
    void closeDevice() override;

    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;   // never: the sender decides
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;   // a label: stored and reported, nothing is tuned

    std::vector<GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string&, double) override { return false; }
    double gainDb(const std::string&) const override { return 0.0; }
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return std::string(); }
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double&, double&) const override { return false; }

    // Datagrams (or bytes' worth of them) the sender's sequence said were missing, and stray datagrams dropped.
    std::uint64_t strayDatagrams() const { return stray_.load(std::memory_order_relaxed); }

private:
    struct Reader;

    bool openImpl(const std::string& args, std::shared_ptr<iiod::Transport> injected);
    bool failOpen(const std::string& why);
    void teardown();

    mutable std::mutex mutex_;
    rawiq::Spec spec_;
    rawiq::Header header_;          // the first header seen (header streams)
    double rateHz_ = 0.0;
    double centreHz_ = 0.0;
    bool declaredCentre_ = false;      // the sender said where it is (header, or freq=): a retune cannot move it
    std::string lastCentreMsg_;
    std::shared_ptr<iiod::Transport> transport_;
    std::shared_ptr<Reader> reader_;
    std::atomic<std::uint64_t> stray_{0};
};

}  // namespace cascade::source
