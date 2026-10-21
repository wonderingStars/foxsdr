// SoapySource::read must not trust the COUNT a vendor module returns.
//
// readStream(buffs, numElems, ...) is handed a buffer of numElems samples and
// answers with how many it filled. SoapySDR's contract is that the answer is at
// most numElems; a module that answers MORE has already written past the end of
// the caller's buffer, and what read() did with that answer made it worse: it
// returned the count unchanged, sanitizeNonFinite scanned - and rewrote -
// elements past the end, and every caller (the receiver's source thread, the
// patch page's radio reader) then processed that many samples out of a buffer
// sized for fewer.
//
// This is a HARDENING test. It does not claim to explain any field crash; it
// pins that an over-long answer is treated as the module breaking its contract:
// the samples are not touched or used, the device is faulted through the same
// path a driver that throws takes, the reason is recorded, and the radio is not
// read again.
//
// The fake is a registered SoapySDR module (the same public registry
// tests/test_soapy_source.cpp uses), so SoapySource::read runs for real. The
// canary is element [n] of the caller's buffer: the fake "overruns" by writing
// a NaN there, and a read() that sanitises the over-long block rewrites that
// element to zero - a write past the end that this test sees.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_source.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace {

using cascade::source::SoapySource;

enum class Mode { Exact, Under, OverByOne, OverByMany, Timeout };
std::atomic<int> g_mode{static_cast<int>(Mode::Exact)};
std::atomic<int> g_readCalls{0};

constexpr float kGoodI = 0.25f;
constexpr float kGoodQ = -0.5f;

class BoundDevice : public SoapySDR::Device {
public:
    std::string getDriverKey() const override { return "fakereadbound"; }
    std::string getHardwareKey() const override { return "fake read-bound source"; }
    size_t getNumChannels(const int) const override { return 1; }
    SoapySDR::Stream* setupStream(const int, const std::string&, const std::vector<size_t>&,
                                  const SoapySDR::Kwargs&) override {
        return reinterpret_cast<SoapySDR::Stream*>(this);
    }
    void closeStream(SoapySDR::Stream*) override {}
    int activateStream(SoapySDR::Stream*, const int, const long long, const size_t) override {
        return 0;
    }
    int deactivateStream(SoapySDR::Stream*, const int, const long long) override { return 0; }
    double getSampleRate(const int, const size_t) const override { return 2.0e6; }
    double getFrequency(const int, const size_t) const override { return 100.0e6; }

    int readStream(SoapySDR::Stream*, void* const* buffs, const size_t numElems, int&,
                   long long&, const long) override {
        g_readCalls.fetch_add(1);
        float* p = static_cast<float*>(buffs[0]);
        const Mode mode = static_cast<Mode>(g_mode.load());
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const auto fill = [&](size_t count) {
            for (size_t i = 0; i < count; ++i) {
                p[2 * i] = kGoodI;
                p[2 * i + 1] = kGoodQ;
            }
        };
        switch (mode) {
            case Mode::Exact:
                fill(numElems);
                return static_cast<int>(numElems);
            case Mode::Under:
                fill(numElems / 2);
                return static_cast<int>(numElems / 2);
            case Mode::OverByOne:
                // The overrun: one element past what it was given.
                fill(numElems);
                p[2 * numElems] = nan;
                p[2 * numElems + 1] = nan;
                return static_cast<int>(numElems + 1);
            case Mode::OverByMany:
                fill(numElems);
                for (size_t i = numElems; i < numElems + 4; ++i) {
                    p[2 * i] = nan;
                    p[2 * i + 1] = nan;
                }
                return static_cast<int>(numElems + 4);
            case Mode::Timeout:
                return SOAPY_SDR_TIMEOUT;
        }
        return SOAPY_SDR_STREAM_ERROR;
    }
};

SoapySDR::KwargsList findBound(const SoapySDR::Kwargs& args) {
    const auto d = args.find("driver");
    if (d != args.end() && d->second != "fakereadbound") { return {}; }
    SoapySDR::Kwargs k;
    k["driver"] = "fakereadbound";
    k["label"] = "fake read-bound source";
    return SoapySDR::KwargsList{k};
}
SoapySDR::Device* makeBound(const SoapySDR::Kwargs&) { return new BoundDevice(); }

constexpr std::size_t kN = 512;
constexpr std::size_t kSlack = 8;  // elements of caller buffer beyond kN, all sentinel

std::vector<std::complex<float>> freshBuffer() {
    return std::vector<std::complex<float>>(kN + kSlack, std::complex<float>(7.0f, 7.0f));
}

bool isNanSample(const std::complex<float>& c) { return std::isnan(c.real()) && std::isnan(c.imag()); }

}  // namespace

int main() {
    SoapySDR::Registry reg("fakereadbound", &findBound, &makeBound, SOAPY_SDR_ABI_VERSION);

    const auto openedAndStarted = [](SoapySource& src) {
        const bool ok = src.open("driver=fakereadbound") && src.start();
        CHECK(ok);
        return ok;
    };

    // --- the boundary and the ordinary answers are untouched ------------------
    {
        SoapySource src;
        if (openedAndStarted(src)) {
            // EXACTLY n is the contract's limit, not a violation.
            g_mode = static_cast<int>(Mode::Exact);
            std::vector<std::complex<float>> buf = freshBuffer();
            CHECK(src.read(buf.data(), kN) == kN);
            CHECK(!src.faulted());
            CHECK(buf[0] == std::complex<float>(kGoodI, kGoodQ));
            CHECK(buf[kN - 1] == std::complex<float>(kGoodI, kGoodQ));
            // ...and nothing past it was written.
            CHECK(buf[kN] == std::complex<float>(7.0f, 7.0f));

            // Fewer than asked is legal and is returned as is.
            g_mode = static_cast<int>(Mode::Under);
            buf = freshBuffer();
            CHECK(src.read(buf.data(), kN) == kN / 2);
            CHECK(!src.faulted());

            // A timeout is still the quiet "retry" answer.
            g_mode = static_cast<int>(Mode::Timeout);
            CHECK(src.read(buf.data(), kN) == 0);
            CHECK(!src.faulted());
        }
    }

    // --- ONE ELEMENT TOO MANY --------------------------------------------------
    {
        SoapySource src;
        if (openedAndStarted(src)) {
            g_mode = static_cast<int>(Mode::OverByOne);
            std::vector<std::complex<float>> buf = freshBuffer();
            const std::size_t got = src.read(buf.data(), kN);
            // Nothing is handed on: the module has broken its contract and the
            // samples it returned cannot be trusted.
            CHECK(got == 0u);
            // The element the module wrote past the end is exactly as the
            // module left it: read() did not "sanitise" memory it was never
            // given.
            CHECK(isNanSample(buf[kN]));
            // Faulted through the path a throwing driver takes, with a reason.
            CHECK(src.faulted());
            const std::string why = src.lastError();
            CHECK(why.find("more samples than") != std::string::npos);
            // ...and the radio is not read again.
            const int callsAfter = g_readCalls.load();
            std::vector<std::complex<float>> again = freshBuffer();
            CHECK(src.read(again.data(), kN) == 0u);
            CHECK(g_readCalls.load() == callsAfter);
            CHECK(again[0] == std::complex<float>(7.0f, 7.0f));
        }
    }

    // --- SEVERAL ELEMENTS TOO MANY ---------------------------------------------
    {
        SoapySource src;
        if (openedAndStarted(src)) {
            g_mode = static_cast<int>(Mode::OverByMany);
            std::vector<std::complex<float>> buf = freshBuffer();
            CHECK(src.read(buf.data(), kN) == 0u);
            for (std::size_t i = kN; i < kN + 4; ++i) { CHECK(isNanSample(buf[i])); }
            CHECK(src.faulted());
            CHECK(std::string(src.lastError()).find("more samples than") != std::string::npos);
        }
    }

    g_mode = static_cast<int>(Mode::Exact);
    return testSummary("test_soapy_read_bound");
}
