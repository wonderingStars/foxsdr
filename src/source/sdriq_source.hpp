// sdriq_source.hpp - RFspace SDR-IQ (USB, 14-bit, 0 to 30 MHz) as a source: the protocol layer and the control
// and streaming logic, up to the FTDI driver call. The SDR-IQ reaches the PC through an FTDI FT245RL; RFspace's
// own specification (section 4.3) says the host uses FTDI's driver "read and write functions". THE OWNER
// DECIDED that this build stops at that call: SdriqLink below is the seam, nothing in this repository loads
// FTDI's D2XX library, and defaultSdriqOpener() answers with a plain sentence saying so.
//
// HAND-OFF for the call itself (a small job once the owner permits it): an SdriqLink over FTDI's D2XX API -
// FT_ListDevices / FT_OpenEx by serial number, FT_SetTimeouts(read, write), FT_Purge(RX|TX), FT_Read / FT_Write,
// FT_Close, loaded at run time from the driver the USER installed (ftd2xx.dll), never shipped. interrupt() can
// be a short read timeout (the pump polls every kPollMs). Whether to use D2XX or FTDI's VCP is the owner's call;
// the spec says D2XX-style read/write.
//
// Receive only. The firmware-update, RS232 pass-through and security-code items of the document are never sent.
// Off until the user picks it; nothing here raises a dialog; an absent radio is reported plainly by open().
// Uses NetSampleSource's reader thread, ring, fault latch and bounded join (no new thread kind).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/net_sample_source.hpp"
#include "source/sdriq_protocol.hpp"

namespace cascade::source {

// The byte link to the radio. Implemented over FTDI's driver (not in this build) and by test stand-ins.
class SdriqLink {
public:
    virtual ~SdriqLink() = default;
    // False with `error` set when the bytes could not all be written.
    virtual bool write(const std::uint8_t* data, std::size_t n, std::string& error) = 0;
    // Up to `cap` bytes, waiting at most timeoutMs: >0 bytes read, 0 nothing yet, <0 the link has failed.
    virtual int read(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) = 0;
    // Makes a blocked read() return soon. Called from another thread.
    virtual void interrupt() = 0;
    virtual void close() = 0;
};

// Opens the link for the radio with this serial number (empty = the first one found). Null with `error` set
// when there is no such radio or no way to reach it.
using SdriqOpener = std::function<std::unique_ptr<SdriqLink>(const std::string& serial, std::string& error)>;

// The production opener of this build: always null, with the sentence
// "the SDR-IQ talks through FTDI's D2XX driver, which this build does not include".
std::unique_ptr<SdriqLink> defaultSdriqOpener(const std::string& serial, std::string& error);

class SdriqSource : public NetSampleSource {
public:
    SdriqSource() = default;
    ~SdriqSource() override;

    // TESTS (and the future D2XX wiring): the opener this object will use instead of the default.
    void setOpener(SdriqOpener opener);

    const char* driverKey() const override { return "sdriq"; }
    // args: sdriq=<serial> (empty or absent = the first radio), timeout=<ms> (control replies, 100..60000).
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;

    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return std::string(); }
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double& lo, double& hi) const override;

    std::string serialNumber() const;
    std::uint64_t dataBlocks() const;
    std::uint64_t otherBlocks() const;  // data items 1..3 and acknowledgements: counted, never used
    std::uint64_t overloadReports() const;

private:
    struct Conn;
    bool failOpen(const std::string& why);
    void teardown();
    bool transact(ascp::MsgType type, std::uint16_t item, const std::vector<std::uint8_t>& params,
                  std::vector<std::uint8_t>& reply, bool& nak);
    bool transactMandatory(ascp::MsgType type, std::uint16_t item, const std::vector<std::uint8_t>& params,
                           std::vector<std::uint8_t>& reply, const char* what);

    SdriqOpener opener_;
    mutable std::mutex ctlMutex_;
    mutable std::mutex connMutex_;
    std::shared_ptr<Conn> conn_;
    int timeoutMs_ = 3000;
    bool active_ = false;
    std::string serial_;
    std::uint32_t rateCode_ = sdriq::kPowerUpRate;
    std::uint64_t freqHz_ = 0;
    ascp::FreqRange range_{0, 30000000};
    bool haveRange_ = false;
    int rfGainDb_ = 0, ifGainDb_ = 0;
};

}  // namespace cascade::source
