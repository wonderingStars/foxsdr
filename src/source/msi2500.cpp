// The MSi2500's tables and arithmetic. See msi2500.hpp for where every number
// came from and for the licence posture; this file is only the bodies.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/msi2500.hpp"

namespace cascade::source::msi2500 {

namespace {

// Sign-extends a `bits`-wide two's-complement value sitting in the low bits of
// `raw`. Written once rather than per format because getting it wrong in one
// of four places would produce a spectrum that is subtly wrong only for the
// rates that select that format - the hardest kind of fault to attribute.
inline int signExtend(std::uint32_t raw, int bits) {
    const std::uint32_t signBit = 1u << (bits - 1);
    const std::uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    raw &= mask;
    return (raw & signBit) != 0 ? static_cast<int>(raw) - static_cast<int>(signBit << 1)
                                : static_cast<int>(raw);
}

}  // namespace

// --- identity -------------------------------------------------------------

const std::vector<DeviceModel>& deviceModels() {
    // Mirics' own id, the SDRplay units built on the same pair, and the
    // television sticks that shipped it. The SDRplay units take the other band
    // plan (tuner_msi001.hpp): the same silicon behind a different input
    // network wants different switch words and different band edges, and using
    // the wrong plan does not fail - it quietly tunes with the wrong filter in
    // circuit, which is exactly the kind of fault nobody attributes.
    //
    // THE PRODUCT IDS WERE WRONG UNTIL 0.99.9, and the mistake was the kind
    // that reads as support. SDRplay's own udev rules (their Linux API
    // package, and the copy in srcejon/sdrplayapi's install_lib.sh) map them:
    //
    //   1df7:2500  RSP1            1df7:3020  RSPduo
    //   1df7:3000  RSP1A           1df7:3030  RSPdx
    //   1df7:3010  RSP2 / RSP2pro  1df7:3050  RSP1B
    //                              1df7:3060  RSPdx-R2
    //
    // So 3000 was lettered "RSP1" while it is an RSP1A, and - the half that
    // actually mistuned - the real RSP1 fell through to 2500, which was
    // flagged as a television dongle and therefore took the WRONG BAND PLAN.
    // 2500 is shared: it is both the RSP1 and the Mirics reference design, so
    // the id alone cannot separate them and resolveModel() below reads the
    // device description instead.
    //
    // THE FOUR THAT ARE NOT HERE - RSPduo, RSPdx, RSP1B, RSPdx-R2 - are left
    // out ON PURPOSE. Each adds front-end hardware (switched filters, LNA
    // steps, antenna ports, notches; the RSPdx a different front end
    // altogether) that this driver has no way to drive and no device here to
    // learn from, and a native row that opens a radio and then hears very
    // little is worse than no row at all: it looks like FoxSDR failing, not
    // like a driver that was never written. Those models are served by the
    // SDRplay API path (source/sdrplay_source.cpp), which is the supported
    // route for every RSP.
    static const std::vector<DeviceModel> models = {
        {0x1DF7, 0x2500, "Mirics MSi2500", false},
        {0x1DF7, 0x3000, "SDRplay RSP1A", true},
        {0x1DF7, 0x3010, "SDRplay RSP2", true},
        {0x2040, 0xD300, "Hauppauge WinTV 133559 LF", false},
        {0x07CA, 0x8591, "AverMedia A859 Pure DVB-T", false},
        {0x04BB, 0x0537, "IO-DATA GV-TV100", false},
        {0x0511, 0x0037, "Logitec LDT-1S310U/J", false},
    };
    return models;
}

bool descriptionNamesSdrPlay(const std::string& description) {
    // Case-insensitive, and only on the two words that can only come from
    // SDRplay: a television stick's description says Hauppauge, AverMedia or
    // nothing at all. A device that says neither keeps the default.
    std::string lower;
    lower.reserve(description.size());
    for (const char c : description) {
        lower.push_back(static_cast<char>(
            (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c));
    }
    return lower.find("sdrplay") != std::string::npos || lower.find("rsp") != std::string::npos;
}

ResolvedModel resolveModel(const DeviceModel& model, const std::string& description) {
    ResolvedModel out{model.label, model.sdrPlayFlavour};
    // ONLY THE SHARED ID IS IN DOUBT. Every other pair in the table names one
    // product, and a description that disagrees with a unique id is a bus
    // string somebody renamed, not a different radio.
    if (model.vid != 0x1DF7 || model.pid != 0x2500) { return out; }
    if (!descriptionNamesSdrPlay(description)) { return out; }
    out.label = "SDRplay RSP1";
    out.sdrPlayFlavour = true;
    return out;
}

const DeviceModel* modelFor(std::uint16_t vid, std::uint16_t pid) {
    for (const DeviceModel& m : deviceModels()) {
        if (m.vid == vid && m.pid == pid) { return &m; }
    }
    return nullptr;
}

std::vector<cascade::usb::UsbId> usbIds() {
    std::vector<cascade::usb::UsbId> ids;
    ids.reserve(deviceModels().size());
    for (const DeviceModel& m : deviceModels()) { ids.push_back({m.vid, m.pid}); }
    return ids;
}

// --- the initialisation sequence ------------------------------------------

const std::vector<RegSet>& adcInitSequence() {
    // adc.c mirisdr_adc_init, in its order. The two writes to register 8
    // BRACKET the other three, and the order is kept even though neither value
    // is one this driver ever writes again: the second is overwritten by the
    // first band switch a moment later, so its only purpose is to be written
    // HERE, and a sequence whose steps are reordered because two of them look
    // redundant is a sequence nobody can compare against the reference again.
    static const std::vector<RegSet> init = {
        {kRegBandSwitch, 0x006080u},   //
        {kRegAdc, 0x00000Cu},          //
        {kRegReset, 0x000200u},        //
        {kRegTunerClock, 0x004801u},   //
        {kRegBandSwitch, 0x00F380u},   //
    };
    return init;
}

// --- unpacking ------------------------------------------------------------

std::size_t unpackBlock(Format f, const std::uint8_t* block, std::complex<float>* dst) {
    if (block == nullptr || dst == nullptr) { return 0; }
    const std::uint8_t* p = block + kBlockHeaderBytes;
    const std::size_t pairs = pairsPerBlock(f);
    const float scale = 1.0f / fullScale(f);

    switch (f) {
        case Format::Bits16: {
            // Two little-endian int16 a pair. Assembled byte by byte rather
            // than cast through an int16 pointer: the block is not guaranteed
            // aligned, and a cast would also be wrong on a big-endian host.
            for (std::size_t i = 0; i < pairs; ++i) {
                const std::uint32_t iRaw =
                    static_cast<std::uint32_t>(p[4 * i + 0]) |
                    (static_cast<std::uint32_t>(p[4 * i + 1]) << 8);
                const std::uint32_t qRaw =
                    static_cast<std::uint32_t>(p[4 * i + 2]) |
                    (static_cast<std::uint32_t>(p[4 * i + 3]) << 8);
                dst[i] = std::complex<float>(static_cast<float>(signExtend(iRaw, 16)) * scale,
                                             static_cast<float>(signExtend(qRaw, 16)) * scale);
            }
            return pairs;
        }
        case Format::Bits8: {
            for (std::size_t i = 0; i < pairs; ++i) {
                dst[i] = std::complex<float>(
                    static_cast<float>(signExtend(p[2 * i + 0], 8)) * scale,
                    static_cast<float>(signExtend(p[2 * i + 1], 8)) * scale);
            }
            return pairs;
        }
        case Format::Bits12: {
            // Two twelve-bit components in three bytes, low component first
            // and its low byte first - the conventional little-endian packing.
            // THE ORDER IS DERIVED, NOT MEASURED (see the header): the count
            // is fixed by arithmetic, the direction is not.
            for (std::size_t i = 0; i < pairs; ++i) {
                const std::uint8_t* g = p + 3 * i;
                const std::uint32_t iRaw =
                    static_cast<std::uint32_t>(g[0]) |
                    (static_cast<std::uint32_t>(g[1] & 0x0Fu) << 8);
                const std::uint32_t qRaw =
                    static_cast<std::uint32_t>(g[1] >> 4) |
                    (static_cast<std::uint32_t>(g[2]) << 4);
                dst[i] = std::complex<float>(static_cast<float>(signExtend(iRaw, 12)) * scale,
                                             static_cast<float>(signExtend(qRaw, 12)) * scale);
            }
            return pairs;
        }
        case Format::Bits10: {
            // Four ten-bit components in five bytes, same direction and the
            // same caveat. Two PAIRS come out of each group, so the loop runs
            // over half the pairs and the last forty-eight bytes of the
            // payload are never read.
            for (std::size_t i = 0; i < pairs; i += 2) {
                const std::uint8_t* g = p + (i / 2) * 5;
                const std::uint32_t c0 =
                    static_cast<std::uint32_t>(g[0]) |
                    (static_cast<std::uint32_t>(g[1] & 0x03u) << 8);
                const std::uint32_t c1 =
                    static_cast<std::uint32_t>(g[1] >> 2) |
                    (static_cast<std::uint32_t>(g[2] & 0x0Fu) << 6);
                const std::uint32_t c2 =
                    static_cast<std::uint32_t>(g[2] >> 4) |
                    (static_cast<std::uint32_t>(g[3] & 0x3Fu) << 4);
                const std::uint32_t c3 =
                    static_cast<std::uint32_t>(g[3] >> 6) |
                    (static_cast<std::uint32_t>(g[4]) << 2);
                dst[i] = std::complex<float>(static_cast<float>(signExtend(c0, 10)) * scale,
                                             static_cast<float>(signExtend(c1, 10)) * scale);
                dst[i + 1] = std::complex<float>(static_cast<float>(signExtend(c2, 10)) * scale,
                                                 static_cast<float>(signExtend(c3, 10)) * scale);
            }
            return pairs;
        }
    }
    return 0;
}

std::size_t unpackTransfer(Format f, const std::uint8_t* src, std::size_t bytes,
                           std::complex<float>* dst, std::size_t dstCap) {
    if (src == nullptr || dst == nullptr) { return 0; }
    const std::size_t pairs = pairsPerBlock(f);
    const std::size_t blocks = std::min(bytes / kBlockBytes, dstCap / pairs);
    std::size_t out = 0;
    for (std::size_t b = 0; b < blocks; ++b) {
        out += unpackBlock(f, src + b * kBlockBytes, dst + out);
    }
    return out;
}

// --- the sample rate ------------------------------------------------------

RateSetting computeRate(double hz) {
    RateSetting r;

    // The clamp first, because every number below is computed FROM the rate
    // and a rate outside the chip's range produces a divider the chip cannot
    // take. NaN lands on the floor: the comparison against the ceiling is
    // false for it, and so is the one against the floor, so the explicit
    // !(hz > 0) test is what catches it.
    double want = hz;
    if (!(want > 0.0)) { want = kMinSampleRateHz; }
    if (want > kMaxSampleRateHz) { want = kMaxSampleRateHz; }
    if (want < kMinSampleRateHz) { want = kMinSampleRateHz; }
    r.rateHz = static_cast<std::uint32_t>(want + 0.5);

    // The format follows the rate, at the reference's own boundaries: each one
    // is the top rate that format's packing can carry at 24000 blocks a
    // second (6.048, 8.064, 9.216 MS/s), so this is not a preference but the
    // only format that fits.
    if (r.rateHz <= 6048000u) {
        r.format = Format::Bits16;
    } else if (r.rateHz <= 8064000u) {
        r.format = Format::Bits12;
    } else if (r.rateHz <= 9216000u) {
        r.format = Format::Bits10;
    } else {
        r.format = Format::Bits8;
    }
    r.formatRegister = formatRegisterValue(r.format);

    // THE DIVIDER SEARCH. The VCO runs at rate * 12 * an even multiplier, and
    // the multiplier is the first one that carries it over 202 MHz - the
    // lowest frequency the synthesiser will lock at. The multiplier is
    // recorded in the clock register as (multiplier / 2 - 1), which is why the
    // walk is in twos.
    std::uint64_t multiplier = 4;
    std::uint64_t vco = 0;
    for (; multiplier < 16; multiplier += 2) {
        vco = static_cast<std::uint64_t>(r.rateHz) * multiplier * 12ULL;
        if (vco >= 202000000ULL) { break; }
    }
    // Falling out of the walk without clearing 202 MHz cannot happen for a
    // rate at or above the floor - 1.3 MS/s clears it at multiplier 14 - but
    // the value the loop leaves behind is kept rather than replaced, so this
    // agrees with the reference even in the case neither of us can reach.
    r.vcoHz = vco;
    r.n = vco / 48000000ULL;
    r.fraction = 0x200000ULL * (vco % 48000000ULL) / 48000000ULL;

    std::uint32_t divider = 0;
    divider |= 0x03u & 3u;                                                       // bits 0-1
    divider |= static_cast<std::uint32_t>(0x07ULL & (multiplier / 2 - 1)) << 2;   // bits 2-4
    divider |= 0u << 5;                                                          // bits 5-6
    divider |= static_cast<std::uint32_t>(0x01ULL & (r.fraction >> 20)) << 7;     // the half step
    divider |= static_cast<std::uint32_t>(0x0FULL & r.n) << 8;                    // bits 8-11
    divider |= formatClockNibble(r.format) << 12;                                 // bits 12-15
    divider |= 1u << 16;                                                          // bit 16
    r.clockDivider = divider;

    // The low twenty bits of the fraction get a register to themselves; the
    // twenty-first is the half-step flag already folded into the divider.
    r.clockFraction = static_cast<std::uint32_t>(0xFFFFFULL & r.fraction);
    return r;
}

}  // namespace cascade::source::msi2500
