// aor_protocol.hpp - the AOR digital-I/Q stream (AR5700D; AR2300, AR5001D and
// AR6000 fitted with the IQ5001) as numbers and pure functions, with nothing
// in it that can touch a device.
//
// THE ONE SOURCE. Every fact in this file is taken from AOR's own "AOR Digital
// I/Q USB Interface Developer Information", Revision 1.1, September 2026
// (called "the AOR document" below, with the section it came from). It is a
// CLEAN-ROOM driver: SoapyAOR and AOR-GQRX-RPi are GPL-3 and were not read,
// fetched or copied, and AOR's own AorAlpha.sys interface was not used. Where
// the AOR document leaves a question open the question is written down here
// and in README.md rather than answered by a guess.
//
// THERE IS NO AOR RECEIVER ON THE BENCH THIS WAS WRITTEN ON, and AOR could not
// lend one. Everything below is proven against words built by hand from the
// document's own formula (tests/test_aor_protocol.cpp), never against a radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cascade::source::aor {

// --- identity (AOR document, "USB interfaces") -----------------------------

// The digital-I/Q interface: a Cypress FX2 with volatile firmware. The SAME
// VID/PID before and after the firmware is loaded (see the open question at
// firmwareDecision() in aor_source.hpp).
constexpr std::uint16_t kIqVid = 0x08D0;
constexpr std::uint16_t kIqPid = 0xA001;

// The receiver-control interface: a stock FTDI USB-serial chip. 0403:6001 is
// used by thousands of unrelated products, so it is NEVER enough on its own to
// say "this is an AOR" - the port is paired by asking the receiver VR (see
// aor_control.hpp).
constexpr std::uint16_t kFtdiVid = 0x0403;
constexpr std::uint16_t kFtdiPid = 0x6001;

// --- streaming transport (AOR document, "Streaming transport") -------------

constexpr std::uint8_t kIqInterface = 0;
constexpr std::uint8_t kCommandEndpoint = 0x02;  // bulk OUT
constexpr std::uint8_t kIqEndpoint = 0x86;       // isochronous IN

// Fixed. The AOR document offers no other rate, so neither does the driver.
constexpr double kSampleRateHz = 1125000.0;

// Eight raw bytes per complex sample: bytes 0-3 are I, 4-7 are Q.
constexpr std::size_t kWordBytes = 8;

// The reference buffering, which the AOR document calls "buffering choices,
// not requirements": 1536-byte packet buffers, 128 packets per transfer,
// eight transfers in flight. The PACKET buffer is the most one microframe can
// carry on this endpoint (3 x 512); the ACTUAL lengths vary - 0, 512, 1024 and
// 1536 were observed - and every one of them is honoured as it arrives.
constexpr std::size_t kIsoPacketBytes = 1536;
constexpr std::size_t kIsoPacketsPerTransfer = 128;
constexpr std::size_t kIsoTransfers = 8;

// --- start / stop (AOR document, "Start/stop") -----------------------------

constexpr std::size_t kCommandBytes = 6;
constexpr std::uint8_t kStartCommand[kCommandBytes] = {0x5A, 0xA5, 0x00, 0x02, 0x41, 0x53};
constexpr std::uint8_t kStopCommand[kCommandBytes] = {0x5A, 0xA5, 0x00, 0x02, 0x41, 0x45};

// --- the raw sample word (AOR document, "Raw sample word") -----------------

// Marker-valid: bit 0 of byte 1 is 1, and bit 0 of bytes 3, 5 and 7 is 0.
// `w` must point at eight readable bytes.
constexpr bool markerValid(const std::uint8_t* w) {
    return (w[1] & 0x01u) == 0x01u && (w[3] & 0x01u) == 0 && (w[5] & 0x01u) == 0 &&
           (w[7] & 0x01u) == 0;
}

// The unpack, written EXACTLY as the AOR document gives it and deliberately
// not "simplified" into an endian conversion - it is not one:
//
//   ti = (b0 << 24) | ((b1 & 0xFE) << 16) | (b2 << 9) | ((b3 & 0xFE) << 2)
//   si = (int32_t)ti / 4
//
// and the same for Q from bytes 4-7. The OR is an OR, not an addition: the
// terms (b2 << 9) and ((b3 & 0xFE) << 2) share bit 9, and the document ORs
// them, so this does too. The division is C's, truncating toward zero, not an
// arithmetic shift - (int32_t)-5 / 4 is -1, where -5 >> 2 would be -2.
//
// The conversion from uint32 to int32 is two's-complement wrap-around, which
// every compiler FoxSDR is built with performs (and C++20 requires).
constexpr std::uint32_t packedHalf(std::uint8_t b0, std::uint8_t b1, std::uint8_t b2,
                                   std::uint8_t b3) {
    return (static_cast<std::uint32_t>(b0) << 24) |
           (static_cast<std::uint32_t>(b1 & 0xFEu) << 16) |
           (static_cast<std::uint32_t>(b2) << 9) |
           (static_cast<std::uint32_t>(b3 & 0xFEu) << 2);
}

constexpr std::int32_t unpackHalf(std::uint8_t b0, std::uint8_t b1, std::uint8_t b2,
                                  std::uint8_t b3) {
    return static_cast<std::int32_t>(packedHalf(b0, b1, b2, b3)) / 4;
}

struct RawSample {
    std::int32_t i = 0;
    std::int32_t q = 0;
};

// `w` must point at eight readable bytes.
constexpr RawSample unpackWord(const std::uint8_t* w) {
    return RawSample{unpackHalf(w[0], w[1], w[2], w[3]), unpackHalf(w[4], w[5], w[6], w[7])};
}

// THE SCALE, and the conversion stated. FoxSDR's pipeline carries
// std::complex<float>. The AOR document's reference scale is si / 2^30
// (1073741824.0), which this uses unchanged. Because si is (int32)ti / 4, its
// range is [-2^29, 2^29 - 1], so the floats land in [-0.5, +0.5): half of the
// +-1.0 full scale some other FoxSDR drivers produce. That is left as the
// document gives it rather than doubled, so a level read here compares
// directly with the same receiver under the reference software.
constexpr double kScale = 1.0 / 1073741824.0;  // 2^-30

inline std::complex<float> toComplex(const RawSample& s) {
    return {static_cast<float>(static_cast<double>(s.i) * kScale),
            static_cast<float>(static_cast<double>(s.q) * kScale)};
}

// --- alignment (AOR document, "Alignment") ---------------------------------
//
// The iso payloads are ONE CONTINUOUS BYTE STREAM: a sample word may start in
// one packet and end in the next, or in the next transfer. USB packet
// boundaries mean nothing to the framing. So the aligner is fed whatever
// bytes arrive, in order, and keeps what it cannot use yet.
//
//   SEARCHING  forward in 2-byte steps for FOUR consecutive marker-valid words
//              (the document's "strong alignment"). The four that prove it are
//              themselves samples and are emitted.
//   LOCKED     one word at a time. A word that is not marker-valid is loss of
//              alignment: the aligner goes back to SEARCHING two bytes further
//              on, and says so in its counters.
//
// ONE ADDITION TO THE DOCUMENT, labelled because it is ours. A 2-byte step
// can only find an alignment of the same parity as where it started; the
// document never says whether the stream can slip by an ODD number of bytes.
// Every payload length it lists is even, so it should not - but if it ever
// did, a pure 2-byte search would never lock again and the radio would go
// silent with no error. So after kParityFlipBytes of searching in one parity
// without a lock, the search moves on by ONE byte and carries on in 2-byte
// steps in the other parity. A stream that behaves as the document says never
// reaches the flip, and nothing about it changes. This is question 3 in the
// open questions for AOR (README.md).
//
// BOUNDS. push() never reads outside [data, data + len) or writes outside
// [out, out + outCap); what it cannot decode yet - a partial word, a search
// window that runs off the end, or words that did not fit in `out` - is kept
// internally and used first on the next call.
class Aligner {
public:
    static constexpr std::size_t kLockWords = 4;
    static constexpr std::size_t kSearchStepBytes = 2;
    static constexpr std::size_t kParityFlipBytes = 4096;

    // Decodes as much of (carry + data) as it can into `out` and returns the
    // number of samples written (<= outCap).
    std::size_t push(const std::uint8_t* data, std::size_t len, std::complex<float>* out,
                     std::size_t outCap);

    // The most samples the next push() of `len` bytes could produce - the
    // capacity a caller needs to be sure nothing is held back for lack of room.
    std::size_t maxSamplesFor(std::size_t len) const { return (pending_.size() + len) / kWordBytes; }

    void reset();

    bool locked() const { return locked_; }
    std::size_t carriedBytes() const { return pending_.size(); }

    // Counters, for the stream-health line and for tests.
    std::uint64_t locks() const { return locks_; }
    std::uint64_t losses() const { return losses_; }
    std::uint64_t parityFlips() const { return parityFlips_; }
    std::uint64_t discardedBytes() const { return discarded_; }

private:
    std::vector<std::uint8_t> pending_;
    bool locked_ = false;
    std::size_t searchedInParity_ = 0;
    std::uint64_t locks_ = 0;
    std::uint64_t losses_ = 0;
    std::uint64_t parityFlips_ = 0;
    std::uint64_t discarded_ = 0;
};

inline void Aligner::reset() {
    pending_.clear();
    locked_ = false;
    searchedInParity_ = 0;
    locks_ = 0;
    losses_ = 0;
    parityFlips_ = 0;
    discarded_ = 0;
}

inline std::size_t Aligner::push(const std::uint8_t* data, std::size_t len,
                                 std::complex<float>* out, std::size_t outCap) {
    if (data != nullptr && len > 0) { pending_.insert(pending_.end(), data, data + len); }
    const std::uint8_t* const buf = pending_.data();
    const std::size_t size = pending_.size();
    std::size_t p = 0;
    std::size_t produced = 0;
    constexpr std::size_t kWindow = kLockWords * kWordBytes;

    while (produced < outCap) {
        if (locked_) {
            if (size - p < kWordBytes) { break; }
            if (!markerValid(buf + p)) {
                locked_ = false;
                ++losses_;
                searchedInParity_ = 0;
                p += kSearchStepBytes;
                discarded_ += kSearchStepBytes;
                continue;
            }
            out[produced++] = toComplex(unpackWord(buf + p));
            p += kWordBytes;
            continue;
        }
        // SEARCHING: need the whole four-word window in hand to decide.
        if (size - p < kWindow) { break; }
        bool all = true;
        for (std::size_t k = 0; k < kLockWords && all; ++k) {
            all = markerValid(buf + p + k * kWordBytes);
        }
        if (all) {
            locked_ = true;
            ++locks_;
            searchedInParity_ = 0;
            continue;  // the window's four words are emitted by the LOCKED branch
        }
        std::size_t step = kSearchStepBytes;
        searchedInParity_ += kSearchStepBytes;
        if (searchedInParity_ >= kParityFlipBytes) {
            step = 1;
            ++parityFlips_;
            searchedInParity_ = 0;
        }
        p += step;
        discarded_ += step;
    }
    // Keep the undecoded tail - at most a partial window or a partial word,
    // unless `out` filled first.
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(p));
    return produced;
}

}  // namespace cascade::source::aor
