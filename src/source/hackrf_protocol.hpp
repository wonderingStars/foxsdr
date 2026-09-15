// hackrf_protocol.hpp - the HackRF's USB vendor protocol, as numbers and pure
// functions, with nothing in it that can touch a device.
//
// WHY IT IS A SEPARATE HEADER FROM THE DRIVER. Everything here is arithmetic
// the firmware and libhackrf agree on: which request number means "set the
// sample rate", how a requested rate in Hz becomes the freq/divider pair the
// firmware actually programs, which of the MAX2837's sixteen filter widths
// goes with it, how a 64-bit tuning frequency splits into the MHz and Hz
// halves the SET_FREQ payload carries, and where a gain in dB lands once the
// hardware's step has been taken out of it. None of that needs a radio to be
// checked, and there is no HackRF on the bench this was written on - so it is
// separated out and tested against values computed by the reference
// implementation itself (tests/test_hackrf_source.cpp names the libhackrf
// function each expectation came from, and the numbers were produced by
// compiling libhackrf's own bodies and printing them, not by reading them off
// by eye).
//
// PORTED FROM libhackrf, WHICH IS BSD-3-Clause. hackrf.c/hackrf.h carry the
// three-clause notice in their file banner (the repository's COPYING covers
// the firmware, not the host library). The notice is reproduced in
// installer/THIRD-PARTY-LICENSES.txt under COMPONENT: libhackrf. Nothing from
// libhackrf is linked or shipped: what is taken is the wire protocol, which is
// the only way to talk to the hardware at all.
//
// THE ONE PLACE FAITHFULNESS BEATS TASTE. hackrf_set_sample_rate's rational
// approximation (the loop over the mantissa below) is not how anyone would
// write a rate solver today, and its answer for a given double is not
// obviously the best one. It is, however, the answer every other HackRF
// application gives, and the firmware is tuned around the pairs it produces.
// A "better" solver would put this receiver at a rate no other tool would
// choose, so the reference's arithmetic is transcribed rather than improved.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cascade::source::hackrf {

// --- identity -------------------------------------------------------------

// libhackrf hackrf.c:202-205 (hackrf_usb_vid and the three product ids). The
// rad1o is the CCC badge that runs HackRF firmware; the Jawbreaker is the
// beta board. All three speak this protocol.
constexpr std::uint16_t kUsbVid = 0x1D50;
constexpr std::uint16_t kPidHackRfOne = 0x6089;
constexpr std::uint16_t kPidJawbreaker = 0x604B;
constexpr std::uint16_t kPidRad1o = 0xCC15;

// --- the vendor requests --------------------------------------------------

// libhackrf hackrf.c:64-125, hackrf_vendor_request. Only the ones a receiver
// needs are named here: the transmit, CPLD, SPI-flash, Opera Cake and sweep
// requests are deliberately absent, because a request number this driver
// cannot name is a request number it cannot send by mistake.
enum class VendorRequest : std::uint8_t {
    SetTransceiverMode = 1,
    SampleRateSet = 6,
    BasebandFilterBandwidthSet = 7,
    BoardIdRead = 14,
    VersionStringRead = 15,
    SetFreq = 16,
    AmpEnable = 17,
    BoardPartIdSerialNoRead = 18,
    SetLnaGain = 19,
    SetVgaGain = 20,
    AntennaEnable = 23,
};

constexpr std::uint8_t requestByte(VendorRequest r) { return static_cast<std::uint8_t>(r); }

// libhackrf hackrf.c:136-143, hackrf_transceiver_mode. Transmit is named only
// so the numbering is readable; this driver never sends it.
enum class TransceiverMode : std::uint16_t {
    Off = 0,
    Receive = 1,
    Transmit = 2,
};

// libhackrf hackrf.c:130 - RX_ENDPOINT_ADDRESS, LIBUSB_ENDPOINT_IN | 1.
constexpr std::uint8_t kRxEndpoint = 0x81;

// libhackrf hackrf.c:146-147, TRANSFER_BUFFER_SIZE and TRANSFER_COUNT. Four
// transfers of a quarter-megabyte each is what every HackRF application
// queues, and at 20 MS/s one buffer is only 3.3 ms of signal - the ring has
// to be this deep for a host that is occasionally late not to lose samples.
constexpr std::size_t kTransferBufferBytes = 262144;
constexpr std::size_t kTransferCount = 4;

// Two bytes per sample: signed 8-bit I then signed 8-bit Q, interleaved
// (hackrf.h's sample-format prose, and every reference consumer of
// hackrf_transfer::buffer).
constexpr std::size_t kBytesPerSample = 2;
constexpr std::size_t kSamplesPerTransfer = kTransferBufferBytes / kBytesPerSample;

// libhackrf hackrf.c:57 - DEFAULT_REQUEST_TIMEOUT. Every control transfer in
// this driver uses it; a HackRF that has not answered a 100 ms control
// request has not answered.
//
// A std::chrono duration rather than a bare unsigned so that
// tests/test_shutdown_budget.cpp's scan can DISCOVER it: it is a bounded wait
// a thread of ours can be inside, and that file's whole point is that a wait
// it has not been introduced to is a wait nothing holds the shutdown budget
// to. The transport takes milliseconds as an unsigned, so the call sites
// convert; the constant stays the discoverable thing.
constexpr std::chrono::milliseconds kControlTimeout{100};
constexpr unsigned kControlTimeoutMs = static_cast<unsigned>(kControlTimeout.count());

// SET_FREQ carries two little-endian uint32s (libhackrf set_freq_params_t,
// hackrf.c:1767-1770) and SAMPLE_RATE_SET two more (set_fracrate_params_t,
// hackrf.c:1868-1872).
constexpr std::size_t kFreqPayloadBytes = 8;
constexpr std::size_t kRatePayloadBytes = 8;

// BOARD_PARTID_SERIALNO_READ returns read_partid_serialno_t: two part-id
// words and four serial words (hackrf.h:981-990).
constexpr std::size_t kPartIdSerialNoBytes = 24;

// VERSION_STRING_READ is read into a caller-sized buffer (hackrf.c:1734-1756).
// libhackrf's own hackrf_info uses 255; the firmware's string is far shorter.
constexpr std::size_t kVersionStringBytes = 255;

// --- the limits the hardware documents ------------------------------------

// hackrf.h:1790-1822: "Sample rate should be in the range 2-20MHz". Below 2
// MS/s the reference says performance is not guaranteed; this driver refuses
// it outright rather than deliver samples it cannot stand behind.
constexpr double kMinSampleRateHz = 2.0e6;
constexpr double kMaxSampleRateHz = 20.0e6;

// hackrf.h:1767 and :235: 1-6000 MHz is the documented tuning range (the
// hardware will go further, with no guarantee).
constexpr double kMinFrequencyHz = 1.0e6;
constexpr double kMaxFrequencyHz = 6.0e9;

// hackrf.h:1856 - "Must be in range 0-40dB, with 8dB steps"; libhackrf
// hackrf.c:2022-2031 enforces it as `value > 40` refused and `value &= ~0x07`.
constexpr double kLnaMinDb = 0.0;
constexpr double kLnaMaxDb = 40.0;
constexpr double kLnaStepDb = 8.0;

// libhackrf hackrf.c:2049-2058: `value > 62` refused, `value &= ~0x01`.
constexpr double kVgaMinDb = 0.0;
constexpr double kVgaMaxDb = 62.0;
constexpr double kVgaStepDb = 2.0;

// The front-end amplifier is a switch, not a gain control: hackrf.h:1827
// calls it the "14dB RF amplifier". It is presented as a gain whose only two
// legal values are 0 and 14 so the Source panel can show it beside the other
// two without needing a second kind of control.
constexpr double kAmpGainDb = 14.0;

// --- the MAX2837 baseband filter ------------------------------------------

// libhackrf hackrf.c:178-196, max2837_ft[]. The sixteen widths the filter can
// be switched to, ascending. The reference terminates the array with a zero
// entry; that sentinel is kept out of the data here and its one observable
// effect is reproduced in computeBasebandFilterBw below, because a sentinel
// in C++ is a bug waiting for someone to loop past it.
constexpr std::uint32_t kBasebandFilterBandwidthsHz[] = {
    1750000,  2500000,  3500000,  5000000,  5500000,  6000000,  7000000,  8000000,
    9000000,  10000000, 12000000, 14000000, 15000000, 20000000, 24000000, 28000000};
constexpr std::size_t kBasebandFilterCount =
    sizeof(kBasebandFilterBandwidthsHz) / sizeof(kBasebandFilterBandwidthsHz[0]);

// libhackrf hackrf.c:2666-2682, hackrf_compute_baseband_filter_bw. The widest
// setting that is no wider than the request, except that a request narrower
// than the narrowest setting still gets the narrowest one.
//
// The reference returns 0 for a request wider than 28 MHz, because its loop
// walks onto the zero terminator; that is transcribed rather than "fixed"
// (see the file header), and it is unreachable from this driver, whose widest
// possible request is 0.75 x 20 MS/s = 15 MHz.
constexpr std::uint32_t computeBasebandFilterBw(std::uint32_t bandwidthHz) {
    std::size_t i = 0;
    while (i < kBasebandFilterCount && kBasebandFilterBandwidthsHz[i] < bandwidthHz) { ++i; }
    if (i >= kBasebandFilterCount) { return 0; }  // the reference's zero terminator
    if (i != 0 && kBasebandFilterBandwidthsHz[i] > bandwidthHz) { --i; }
    return kBasebandFilterBandwidthsHz[i];
}

// --- the sample rate ------------------------------------------------------

// What SAMPLE_RATE_SET actually carries: the rate is freqHz / divider.
struct RateSetting {
    std::uint32_t freqHz = 0;
    std::uint32_t divider = 1;
};

// libhackrf hackrf.c:1920-1959, hackrf_set_sample_rate. Transcribed, bit
// operation for bit operation - see the file header for why this and not a
// cleaner solver. It walks the mantissa of (1 + frac) looking for the
// smallest multiplier that drives the low bits either all clear or all set,
// i.e. the smallest denominator that represents the requested rate closely
// enough at this magnitude.
//
// Two mechanical differences from the reference, neither of which changes an
// answer (the test pins every value against the reference's own output):
//   - the union that punned a double into a uint64 is a memcpy, because
//     reading the other member of a union is undefined in C++;
//   - `~((1 << (e + 4)) - 1)` is written out with the sign extension the C
//     version gets for free from int-to-uint64 promotion, so the intent is
//     visible rather than inherited.
inline RateSetting computeRate(double hz) {
    constexpr int kMaxN = 32;

    const double freqFrac = 1.0 + hz - static_cast<double>(static_cast<int>(hz));

    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(double), "a double is not 64 bits on this target");
    std::memcpy(&bits, &hz, sizeof(bits));
    const int e = static_cast<int>((bits >> 52) & 0x7FF) - 1023;

    std::uint64_t m = (1ULL << 52) - 1ULL;

    double fracCopy = freqFrac;
    std::uint64_t v = 0;
    std::memcpy(&v, &fracCopy, sizeof(v));
    v &= m;

    // The reference's `m &= ~((1 << (e + 4)) - 1)` with the promotion spelled
    // out. e is bounded by the rates this driver accepts (2-20 MS/s puts e in
    // 20..24), so the shift is always in range; the clamp exists so a caller
    // that reaches this with nonsense cannot shift by 64.
    const int shift = e + 4;
    if (shift >= 0 && shift < 64) {
        m &= ~((1ULL << shift) - 1ULL);
    } else if (shift >= 64) {
        m = 0;
    }

    std::uint64_t a = 0;
    int i = 1;
    for (; i < kMaxN; ++i) {
        a += v;
        if ((a & m) == 0 || ((~a) & m) == 0) { break; }
    }
    if (i == kMaxN) { i = 1; }

    RateSetting out;
    out.freqHz = static_cast<std::uint32_t>(hz * i + 0.5);
    out.divider = static_cast<std::uint32_t>(i);
    return out;
}

// libhackrf hackrf.c:1907-1910: every sample-rate change is followed by a
// filter set to the widest width no more than 75% of the resulting rate,
// with the 0.75 x fs product truncated to an integer before the lookup.
inline std::uint32_t basebandFilterForRate(const RateSetting& r) {
    if (r.divider == 0) { return 0; }
    const double fs = static_cast<double>(r.freqHz) / static_cast<double>(r.divider);
    return computeBasebandFilterBw(static_cast<std::uint32_t>(0.75 * fs));
}

// The payload SAMPLE_RATE_SET carries: freq_hz then divider, each a
// little-endian uint32 (set_fracrate_params_t, hackrf.c:1868-1872; the
// reference's TO_LE is a no-op on every little-endian host and a byte swap on
// a big-endian one, which is what writing the bytes out by hand gives us on
// every host).
inline void encodeRate(const RateSetting& r, std::uint8_t out[kRatePayloadBytes]) {
    for (std::size_t b = 0; b < 4; ++b) {
        out[b] = static_cast<std::uint8_t>((r.freqHz >> (8 * b)) & 0xFF);
        out[4 + b] = static_cast<std::uint8_t>((r.divider >> (8 * b)) & 0xFF);
    }
}

// --- the tuning frequency -------------------------------------------------

// libhackrf hackrf.c:1775-1790: the 64-bit request splits into whole MHz and
// the remaining Hz, and the firmware adds them back together.
struct FreqSplit {
    std::uint32_t mhz = 0;
    std::uint32_t hz = 0;
};

constexpr std::uint64_t kOneMhz = 1000000ULL;

inline FreqSplit splitFrequency(std::uint64_t freqHz) {
    FreqSplit s;
    s.mhz = static_cast<std::uint32_t>(freqHz / kOneMhz);
    s.hz = static_cast<std::uint32_t>(freqHz - (static_cast<std::uint64_t>(s.mhz) * kOneMhz));
    return s;
}

inline void encodeFreq(const FreqSplit& s, std::uint8_t out[kFreqPayloadBytes]) {
    for (std::size_t b = 0; b < 4; ++b) {
        out[b] = static_cast<std::uint8_t>((s.mhz >> (8 * b)) & 0xFF);
        out[4 + b] = static_cast<std::uint8_t>((s.hz >> (8 * b)) & 0xFF);
    }
}

// --- the gains ------------------------------------------------------------

// libhackrf hackrf.c:2022-2031. The reference REFUSES anything above 40 and
// then clears the low three bits; the Source panel's contract
// (device_source.hpp) is that an out-of-range gain is clamped rather than
// refused, so the clamp happens here and the reference's mask is applied to
// what comes out of it. Both end at a multiple of 8 in 0..40.
inline std::uint32_t roundLnaGainDb(double db) {
    if (!(db > kLnaMinDb)) { return 0; }  // negated compare so NaN lands here
    const double clamped = std::min(db, kLnaMaxDb);
    return static_cast<std::uint32_t>(clamped) & ~0x07u;
}

// libhackrf hackrf.c:2049-2058, the same shape with `> 62` and `& ~0x01`.
inline std::uint32_t roundVgaGainDb(double db) {
    if (!(db > kVgaMinDb)) { return 0; }
    const double clamped = std::min(db, kVgaMaxDb);
    return static_cast<std::uint32_t>(clamped) & ~0x01u;
}

// The amplifier is a switch: anything at or above half its step is on. There
// is no reference function for this - libhackrf takes a 0/1 - so the rounding
// rule is ours, and it is the one the Source panel's step of 14 dB implies.
inline bool ampOnForDb(double db) { return db >= kAmpGainDb * 0.5; }

// --- the sample format ----------------------------------------------------

// The bulk stream is signed 8-bit I/Q, and 256 entries of float is a smaller
// table than one cache line per two samples of branching. -128 maps to -1.0
// and +127 to +0.9921875, i.e. into [-1, 1) - the asymmetry is the two's
// complement range's, not a choice: dividing by 127 instead would put a
// full-scale negative sample outside the interval every other stage assumes.
struct SampleTable {
    float v[256];
};

inline const SampleTable& sampleTable() {
    static const SampleTable t = [] {
        SampleTable table{};
        for (int raw = 0; raw < 256; ++raw) {
            const int signedValue = (raw < 128) ? raw : raw - 256;
            table.v[raw] = static_cast<float>(signedValue) / 128.0f;
        }
        return table;
    }();
    return t;
}

}  // namespace cascade::source::hackrf
