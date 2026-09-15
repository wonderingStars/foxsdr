// The Airspy HF+ driver itself. See airspyhf_source.hpp for the threading and
// ownership argument; the protocol numbers, the tuning arithmetic and the IQ
// balancer are in airspyhf_protocol.hpp, which is where the libairspyhf
// attribution lives.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/airspyhf_source.hpp"

#include "core/diag_log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace cascade::source {

namespace {

// Abandoned reader threads, process-wide. See readersAbandoned().
std::atomic<unsigned long long> g_readersAbandoned{0};

// Lower-cased copy, for the case-insensitive serial match.
std::string lowered(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

// The default frequency a freshly opened receiver is put on: 10 MHz, in the
// middle of the HF band this radio exists for, and legal on every model. The
// HackRF's equivalent is 100 MHz, which on an HF+ is inside the VHF band -
// reachable, but a strange place to leave a shortwave receiver.
constexpr double kDefaultCenterHz = 10.0e6;

// Sixteen hex digits, as airspyhf_info.c prints a serial number
// ("S/N: 0x%08X%08X" of serial_no[0] then serial_no[1], tools/src/
// airspyhf_info.c:47-50) and as airspyhf_list_devices parses one out of the
// USB string. The words are little-endian on the wire.
std::uint32_t wordAt(const std::uint8_t* data, std::size_t word) {
    std::uint32_t v = 0;
    for (std::size_t b = 0; b < 4; ++b) {
        v |= static_cast<std::uint32_t>(data[word * 4 + b]) << (8 * b);
    }
    return v;
}

std::string serialTextFrom(const std::uint8_t* partSerial) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08x%08x", wordAt(partSerial, 1), wordAt(partSerial, 2));
    return std::string(buf);
}

// A float as the firmware sends it: four little-endian bytes of IEEE-754,
// which is what every host this runs on uses natively. memcpy rather than a
// cast because type-punning through a pointer is undefined.
float floatAt(const std::uint8_t* data, std::size_t index) {
    std::uint32_t bits = wordAt(data, index);
    float f = 0.0f;
    static_assert(sizeof(f) == sizeof(bits), "a float is not 32 bits on this target");
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

}  // namespace

// --- enumeration ----------------------------------------------------------

std::vector<cascade::usb::UsbId> airspyHfUsbIds() {
    return {{airspyhf::kUsbVid, airspyhf::kUsbPid}};
}

std::string normalisedSerial(const std::string& raw) {
    const std::string low = lowered(raw);
    const std::string prefix = lowered(airspyhf::kSerialPrefix);
    if (low.size() > prefix.size() && low.compare(0, prefix.size(), prefix) == 0) {
        return low.substr(prefix.size());
    }
    return low;
}

std::vector<NativeDeviceInfo> airspyHfDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices) {
    std::vector<NativeDeviceInfo> out;
    int index = 0;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid != airspyhf::kUsbVid || d.pid != airspyhf::kUsbPid) { continue; }
        NativeDeviceInfo info;
        info.driver = "airspyhf";
        // The bus-reported product string when there is one - it is the only
        // thing available here that can tell a Discovery from a Dual, and it
        // costs nothing to ask because Windows has already asked. A device
        // whose description says nothing about Airspy (an empty string, or a
        // generic one) gets the family name instead of a label that would
        // read as a different product.
        const std::string described = lowered(d.description);
        info.label = described.find("airspy") != std::string::npos ? d.description : "Airspy HF+";
        const std::string serial = normalisedSerial(d.serial);
        if (!serial.empty()) {
            info.label += " (serial " + serial + ")";
            info.args = "serial=" + serial;
        } else {
            // A device with no serial is still openable - the transport
            // addresses it by path - so it gets an index rather than being
            // dropped. Two of them are then told apart by enumeration order,
            // which is stable (the transport sorts by path).
            info.args = "index=" + std::to_string(index);
        }
        out.push_back(std::move(info));
        ++index;
    }
    return out;
}

std::vector<NativeDeviceInfo> enumerateAirspyHf() {
#if defined(_WIN32)
    return airspyHfDevicesFrom(cascade::usb::enumerateWinUsb(airspyHfUsbIds()));
#else
    // The transport is WinUSB. A Linux HF+ is reached through SoapySDR until
    // a libusb backend exists behind cascade::usb::UsbDevice; saying so once
    // beats an empty list nobody can explain.
    static bool said = false;
    if (!said) {
        said = true;
        core::diagLogf(
            "airspyhf: native enumeration is Windows-only in this build (the USB transport is "
            "WinUSB); use the SoapySDR path on this platform");
    }
    return {};
#endif
}

// --- construction ---------------------------------------------------------

AirspyHfSource::~AirspyHfSource() { closeDevice(); }

void AirspyHfSource::setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices,
                                         UsbOpenFn opener) {
    std::lock_guard<std::mutex> lk(devMutex_);
    fakeDevices_ = std::move(devices);
    fakeOpener_ = std::move(opener);
    useFakeTransport_ = static_cast<bool>(fakeOpener_);
}

// --- the error slot -------------------------------------------------------

void AirspyHfSource::setErrorOn(ReaderLink& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void AirspyHfSource::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void AirspyHfSource::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
}

void AirspyHfSource::noteTransportFaultOn(ReaderLink& link, const char* what,
                                          const std::string& detail) {
    {
        std::lock_guard<std::mutex> lk(link.errorMutex);
        // Do not overwrite the FIRST cause. Once a device has gone, every
        // later transfer on it fails too, and the last message is the least
        // informative one there is.
        if (!link.deviceDead) {
            link.lastError = std::string("the radio stopped answering while ") + what +
                             (detail.empty() ? std::string() : (": " + detail)) +
                             "; unplug it and plug it back in";
            link.deadWhat = what;
        }
        link.faulted = true;
        link.deviceDead = true;
    }
    core::diagWarnf("airspyhf: transfer failed while %s%s%s", what, detail.empty() ? "" : ": ",
                    detail.c_str());
}

void AirspyHfSource::noteTransportFault(const char* what, const std::string& detail) {
    noteTransportFaultOn(*link_, what, detail);
}

bool AirspyHfSource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool AirspyHfSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string AirspyHfSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deadWhat;
}

const char* AirspyHfSource::lastError() const {
    // A per-THREAD snapshot, for the same reason SoapySource keeps one: the
    // reader thread rewrites the member while the GUI reads the pointer, and
    // handing out a pointer into a string another thread is assigning is UB.
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(link_->errorMutex);
        snapshot = link_->lastError;
    }
    return snapshot.c_str();
}

void AirspyHfSource::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* AirspyHfSource::name() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        snapshot = name_;
    }
    return snapshot.c_str();
}

// --- control transfers ----------------------------------------------------

bool AirspyHfSource::controlOutLocked(airspyhf::VendorRequest r, std::uint16_t value,
                                      std::uint16_t index, const std::uint8_t* data,
                                      std::size_t len, const char* what) {
    if (dev_ == nullptr) {
        setError(std::string("no Airspy HF+ is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const int ret =
        dev_->controlOut(cascade::usb::kRequestTypeVendorOut, airspyhf::requestByte(r), value,
                         index, data, len, airspyhf::kControlTimeoutMs);
    if (ret < 0 || static_cast<std::size_t>(ret) != len) {
        noteTransportFault(what, dev_->lastError());
        return false;
    }
    return true;
}

bool AirspyHfSource::controlInLocked(airspyhf::VendorRequest r, std::uint16_t value,
                                     std::uint16_t index, std::uint8_t* data, std::size_t len,
                                     const char* what, std::size_t* moved) {
    if (moved != nullptr) { *moved = 0; }
    if (dev_ == nullptr) {
        setError(std::string("no Airspy HF+ is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const int ret =
        dev_->controlIn(cascade::usb::kRequestTypeVendorIn, airspyhf::requestByte(r), value, index,
                        data, len, airspyhf::kControlTimeoutMs);
    if (ret < 0) {
        noteTransportFault(what, dev_->lastError());
        return false;
    }
    if (moved != nullptr) { *moved = static_cast<std::size_t>(ret); }
    // A short answer is not in itself a fault - GET_VERSION_STRING returns
    // however many characters the firmware has - so the callers that need a
    // whole struct back check the count themselves.
    return true;
}

namespace {

// THE ONE THING THIS DRIVER NEEDS AND THE HACKRF'S DID NOT: a control request
// that is allowed to fail.
//
// Five of this firmware's requests are OPTIONAL BY GENERATION.
// GET_SAMPLERATE_ARCHITECTURES, GET_ATT_STEPS, GET_BIAS_TEE_COUNT,
// GET_FILTER_GAIN and CONFIG_READ are all answered by current firmware and
// STALLED by older firmware, and libairspyhf treats every one of those stalls
// as "assume the default and carry on" (airspyhf.c:1002-1008, :1042-1058,
// :1273-1279, :1076-1099, :1728-1751). A stalled control request reaches this
// driver as a negative return from the transport - indistinguishable, at that
// layer, from the device having been unplugged.
//
// So those five go through a path that does NOT condemn the device, and every
// other request goes through controlInLocked, which does. The cost of being
// wrong in this direction is one more failed transfer a moment later on a
// request that IS mandatory, which then faults properly; the cost of being
// wrong in the other direction is refusing to open a perfectly good receiver
// with three-year-old firmware on it.
struct OptionalRead {
    bool ok = false;
    std::size_t moved = 0;
};

OptionalRead optionalControlIn(cascade::usb::UsbDevice* dev, airspyhf::VendorRequest r,
                               std::uint16_t value, std::uint16_t index, std::uint8_t* data,
                               std::size_t len) {
    OptionalRead out;
    if (dev == nullptr) { return out; }
    const int ret =
        dev->controlIn(cascade::usb::kRequestTypeVendorIn, airspyhf::requestByte(r), value, index,
                       data, len, airspyhf::kControlTimeoutMs);
    if (ret < 0) { return out; }
    out.ok = true;
    out.moved = static_cast<std::size_t>(ret);
    return out;
}

}  // namespace

bool AirspyHfSource::setReceiverModeLocked(airspyhf::ReceiverMode mode, const char* what) {
    // airspyhf_set_receiver_mode (airspyhf.c:1287-1306): the mode is the VALUE
    // word, index 0, no payload.
    return controlOutLocked(airspyhf::VendorRequest::ReceiverMode,
                            static_cast<std::uint16_t>(mode), 0, nullptr, 0, what);
}

// --- what the device says about itself ------------------------------------

bool AirspyHfSource::readRatesLocked() {
    rates_.clear();
    architectures_.clear();

    // airspyhf_read_samplerates_from_fw (airspyhf.c:585-604): asked with
    // len 0 it answers the COUNT, asked with len n it answers n rates. The
    // count rides in the INDEX word both times and the byte length is
    // (len ? len : 1) * 4.
    std::uint8_t countBytes[4] = {0};
    std::size_t moved = 0;
    if (!controlInLocked(airspyhf::VendorRequest::GetSampleRates, 0, 0, countBytes,
                         sizeof(countBytes), "asking how many sample rates it has", &moved)) {
        return false;
    }
    std::uint32_t count = moved >= 4 ? wordAt(countBytes, 0) : 0;
    if (count == 0 || count > airspyhf::kMaxSampleRateCount) {
        // The reference's own fallback (airspyhf.c:1016-1026): one rate,
        // zero-IF. A firmware that cannot list its rates still streams.
        core::diagWarnf(
            "airspyhf: the receiver did not list its sample rates (answered %u); assuming %u S/s",
            static_cast<unsigned>(count), static_cast<unsigned>(airspyhf::kDefaultSampleRateHz));
        rates_.push_back(airspyhf::kDefaultSampleRateHz);
        architectures_.push_back(0);
        return true;
    }

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(count) * 4, 0);
    if (!controlInLocked(airspyhf::VendorRequest::GetSampleRates, 0,
                         static_cast<std::uint16_t>(count), raw.data(), raw.size(),
                         "reading the sample rate list", &moved) ||
        moved < raw.size()) {
        setError("the Airspy HF+ answered a short sample rate list");
        return false;
    }
    for (std::uint32_t i = 0; i < count; ++i) { rates_.push_back(wordAt(raw.data(), i)); }

    // airspyhf_read_samplerate_architectures_from_fw (airspyhf.c:627-648).
    // ONE BYTE PER RATE COMES BACK, but the reference asks for count * 4 of
    // them into a buffer of count - the length field and the buffer disagree
    // in the reference itself. The request is sent EXACTLY as the reference
    // sends it, because what the firmware does is keyed on what it is asked;
    // the buffer here is the full count * 4 so that a device answering the
    // length it was given cannot write past anything, and only the first
    // `count` bytes are read out of it.
    std::vector<std::uint8_t> archRaw(static_cast<std::size_t>(count) * 4, 0);
    const OptionalRead arch = optionalControlIn(dev_.get(),
                                                airspyhf::VendorRequest::GetSampleRateArchitectures,
                                                0, static_cast<std::uint16_t>(count),
                                                archRaw.data(), archRaw.size());
    architectures_.assign(count, 0);
    if (arch.ok && arch.moved >= count) {
        for (std::uint32_t i = 0; i < count; ++i) { architectures_[i] = archRaw[i] ? 1 : 0; }
    } else {
        // "Assume Zero IF for all", and the reference clears the error for
        // backward compatibility (airspyhf.c:1002-1008).
        core::diagLogf(
            "airspyhf: this firmware does not report its sample-rate architectures; assuming "
            "zero-IF for all %u of them", static_cast<unsigned>(count));
    }
    return true;
}

bool AirspyHfSource::readAttStepsLocked() {
    // airspyhf_read_att_steps_from_fw (airspyhf.c:606-625), the same
    // count-then-list shape as the rates, with the count in the INDEX word.
    std::uint8_t countBytes[4] = {0};
    const OptionalRead countRead = optionalControlIn(
        dev_.get(), airspyhf::VendorRequest::GetAttSteps, 0, 0, countBytes, sizeof(countBytes));
    std::uint32_t count = (countRead.ok && countRead.moved >= 4) ? wordAt(countBytes, 0) : 0;
    if (count > 0 && count <= airspyhf::kMaxAttStepCount) {
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(count) * 4, 0);
        const OptionalRead list =
            optionalControlIn(dev_.get(), airspyhf::VendorRequest::GetAttSteps, 0,
                              static_cast<std::uint16_t>(count), raw.data(), raw.size());
        if (list.ok && list.moved >= raw.size()) {
            attSteps_.clear();
            for (std::uint32_t i = 0; i < count; ++i) { attSteps_.push_back(floatAt(raw.data(), i)); }
            return true;
        }
    }
    // "Assume ATT steps of the Airspy HF+ Discovery" (airspyhf.c:1049-1058).
    attSteps_ = airspyhf::defaultAttSteps();
    return true;
}

bool AirspyHfSource::readCalibrationLocked() {
    // airspyhf_config_read (airspyhf.c:1483-1506) reads a fixed 256-byte page
    // whose first sixteen bytes are flash_config_t: magic, calibration_ppb,
    // calibration_vctcxo, frontend_options (airspyhf.c:137-143). Without the
    // magic word the page is whatever the chip shipped with and is ignored.
    std::vector<std::uint8_t> page(airspyhf::kConfigBytes, 0);
    const OptionalRead read = optionalControlIn(dev_.get(), airspyhf::VendorRequest::ConfigRead, 0,
                                                0, page.data(), page.size());
    calibrationPpb_.store(0, std::memory_order_relaxed);
    if (!read.ok || read.moved < 16) { return true; }
    if (wordAt(page.data(), 0) != airspyhf::kCalibrationMagic) { return true; }

    const std::int32_t ppb = static_cast<std::int32_t>(wordAt(page.data(), 1));
    const std::uint32_t vctcxo = wordAt(page.data(), 2);
    const std::uint32_t frontend = wordAt(page.data(), 3);
    calibrationPpb_.store(ppb, std::memory_order_relaxed);

    // The reference PUSHES the stored VCTCXO trim and frontend options back
    // into the device at open (airspyhf.c:1079-1082): the flash holds them,
    // the running firmware does not apply them by itself.
    controlOutLocked(airspyhf::VendorRequest::SetVctcxoCalibration,
                     static_cast<std::uint16_t>(vctcxo & 0xFFFFu), 0, nullptr, 0,
                     "restoring the crystal trim");
    controlOutLocked(airspyhf::VendorRequest::SetFrontendOptions,
                     static_cast<std::uint16_t>(frontend & 0xFFFFu),
                     static_cast<std::uint16_t>(frontend >> 16), nullptr, 0,
                     "restoring the frontend options");
    core::diagLogf("airspyhf: calibration from the receiver's own flash - %d ppb, vctcxo %u",
                   static_cast<int>(ppb), static_cast<unsigned>(vctcxo));
    return true;
}

bool AirspyHfSource::readBiasTeeCountLocked() {
    // airspyhf_get_bias_tee_count (airspyhf.c:1728-1751). Absent on older
    // firmware and on boards without one, and absent means "there is no
    // bias tee to offer", not "the device is broken".
    std::uint8_t raw[4] = {0};
    const OptionalRead read = optionalControlIn(
        dev_.get(), airspyhf::VendorRequest::GetBiasTeeCount, 0, 0, raw, sizeof(raw));
    const std::int32_t count =
        (read.ok && read.moved >= 4) ? static_cast<std::int32_t>(wordAt(raw, 0)) : 0;
    biasTeeCount_.store(count > 0 ? count : 0, std::memory_order_relaxed);
    return true;
}

// --- programming the radio ------------------------------------------------

bool AirspyHfSource::programRateIndexLocked(std::size_t index, const char* what) {
    if (index >= rates_.size()) {
        setError("that sample rate index is not one the receiver listed");
        return false;
    }
    const bool lowIf = architectures_[index] != 0;

    // airspyhf_set_samplerate (airspyhf.c:1194-1281), in its own order.
    //
    // The pipe is cleared first (libusb_clear_halt, :1218) because the
    // firmware stalls the bulk endpoint across a rate change, and then - on a
    // ZERO-IF rate whose LO is below the floor - the LO is moved up BEFORE the
    // rate is programmed. That pre-tune looks redundant beside the retune at
    // the end of this function and is not: the synthesiser must already be
    // somewhere legal for the new architecture when the rate lands.
    if (dev_ != nullptr) { dev_->resetPipe(airspyhf::kRxEndpoint); }

    if (!lowIf && loKhz_ < airspyhf::kMinZeroIfLoKhz) {
        std::uint8_t payload[airspyhf::kFreqPayloadBytes];
        airspyhf::encodeFreqKhz(airspyhf::kMinZeroIfLoKhz, payload);
        if (!controlOutLocked(airspyhf::VendorRequest::SetFreq, 0, 0, payload, sizeof(payload),
                              "moving the local oscillator to its zero-IF floor")) {
            return false;
        }
        loKhz_ = airspyhf::kMinZeroIfLoKhz;
    }

    if (!controlOutLocked(airspyhf::VendorRequest::SetSampleRate, 0,
                          static_cast<std::uint16_t>(index), nullptr, 0, what)) {
        return false;
    }
    rateIndex_ = index;
    lowIf_.store(lowIf, std::memory_order_relaxed);
    const double rateHz = static_cast<double>(rates_[index]);
    sampleRateHz_.store(rateHz, std::memory_order_relaxed);
    link_->sampleRateHz.store(rateHz, std::memory_order_relaxed);
    link_->lowIf.store(lowIf, std::memory_order_relaxed);

    // GET_FILTER_GAIN: one byte of decibels the firmware has already taken
    // out in its decimation chain, which the host puts back so that full
    // scale means the same thing at every rate. A firmware that will not
    // answer leaves the gain at unity (airspyhf.c:1277).
    std::uint8_t gainDb = 0;
    const OptionalRead gain = optionalControlIn(dev_.get(), airspyhf::VendorRequest::GetFilterGain,
                                                0, 0, &gainDb, sizeof(gainDb));
    const float filterGain =
        (gain.ok && gain.moved >= 1) ? airspyhf::filterGainFromDb(gainDb) : 1.0f;
    link_->filterGain.store(filterGain, std::memory_order_relaxed);

    // ...and the retune, because the LO floor and the IF offset both depend
    // on the architecture of the rate that has just been programmed
    // (airspyhf.c:1280).
    return programFrequencyLocked(centerFrequencyHz_.load(std::memory_order_relaxed),
                                  "re-tuning after the sample rate change");
}

bool AirspyHfSource::programFrequencyLocked(double hz, const char* what) {
    const airspyhf::Tuning t = airspyhf::computeTuning(
        hz, calibrationPpb_.load(std::memory_order_relaxed),
        lowIf_.load(std::memory_order_relaxed), dspEnabled_.load(std::memory_order_relaxed),
        freqDeltaHz_.load(std::memory_order_relaxed));

    if (t.loKhz != loKhz_) {
        std::uint8_t payload[airspyhf::kFreqPayloadBytes];
        airspyhf::encodeFreqKhz(t.loKhz, payload);
        if (!controlOutLocked(airspyhf::VendorRequest::SetFreq, 0, 0, payload, sizeof(payload),
                              what)) {
            return false;
        }
        loKhz_ = t.loKhz;

        // GET_FREQ_DELTA is the firmware reporting where its synthesiser
        // ACTUALLY landed relative to the kilohertz it was asked for - the
        // last few hertz a fractional-N PLL cannot hit. It goes into the
        // residual the reader rotates out, which is the only reason this
        // radio is accurate to better than the kHz grid it tunes on. Old
        // firmware does not answer, and then the previous delta stands
        // (airspyhf.c:1394-1402).
        std::uint8_t deltaRaw[4] = {0};
        const OptionalRead delta = optionalControlIn(
            dev_.get(), airspyhf::VendorRequest::GetFreqDelta, 0, 0, deltaRaw, sizeof(deltaRaw));
        if (delta.ok && delta.moved >= 4) {
            freqDeltaHz_.store(airspyhf::decodeFreqDelta(deltaRaw), std::memory_order_relaxed);
        }

        // A new frequency is a new imbalance: ask the reader to start the
        // balancer's estimate again on its next block (the reference does
        // this from set_freq as iq_balancer_set_optimal_point, :1404).
        link_->balancerResetWanted.store(true, std::memory_order_relaxed);
    }

    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    republishTuningLocked();
    return true;
}

void AirspyHfSource::republishTuningLocked() {
    // The residual is recomputed from the delta that has just been read, so
    // it reflects where the synthesiser really is rather than where it was
    // asked to go. loKhz_ is not re-derived here: what the device is tuned to
    // is what it was last TOLD, and computeTuning's own clamp is already in
    // it.
    const double hz = centerFrequencyHz_.load(std::memory_order_relaxed);
    const std::int32_t ppb = calibrationPpb_.load(std::memory_order_relaxed);
    const double adjusted = hz * (1.0e9 + static_cast<double>(ppb)) * 1.0e-9;
    const double shift = adjusted - static_cast<double>(loKhz_) * 1e3 +
                         freqDeltaHz_.load(std::memory_order_relaxed);
    link_->freqShiftHz.store(shift, std::memory_order_relaxed);
}

bool AirspyHfSource::programAttLocked(double gainDb) {
    // The sign turn described in the header: the panel's gain is negative, the
    // hardware's attenuation is positive.
    double attenuation = -gainDb;
    if (!(attenuation > 0.0)) { attenuation = 0.0; }  // negated compare so a NaN lands here
    const std::uint16_t index = airspyhf::attIndexFor(attSteps_, attenuation);
    // airspyhf_set_hf_att (airspyhf.c:1789-1809): the index is the VALUE word.
    if (!controlOutLocked(airspyhf::VendorRequest::SetAtt, index, 0, nullptr, 0,
                          "setting the attenuator")) {
        return false;
    }
    const double programmed =
        attSteps_.empty() ? 0.0 : static_cast<double>(attSteps_[index]);
    attDb_.store(-programmed, std::memory_order_relaxed);
    return true;
}

bool AirspyHfSource::programLnaLocked(bool on) {
    // airspyhf_set_hf_lna (airspyhf.c:1811-1831): 0 or 1 in the VALUE word.
    if (!controlOutLocked(airspyhf::VendorRequest::SetLna, on ? 1 : 0, 0, nullptr, 0,
                          "switching the preamp")) {
        return false;
    }
    lnaDb_.store(on ? airspyhf::kLnaGainDb : 0.0, std::memory_order_relaxed);
    return true;
}

bool AirspyHfSource::programAgcLocked(bool on) {
    // airspyhf_set_hf_agc (airspyhf.c:1855-1875).
    if (!controlOutLocked(airspyhf::VendorRequest::SetAgc, on ? 1 : 0, 0, nullptr, 0,
                          "switching the automatic gain control")) {
        return false;
    }
    autoGain_.store(on, std::memory_order_relaxed);
    return true;
}

bool AirspyHfSource::programAgcThresholdLocked(bool high) {
    // airspyhf_set_hf_agc_threshold (airspyhf.c:1877-1897).
    if (!controlOutLocked(airspyhf::VendorRequest::SetAgcThreshold, high ? 1 : 0, 0, nullptr, 0,
                          "setting the AGC threshold")) {
        return false;
    }
    agcHigh_.store(high, std::memory_order_relaxed);
    return true;
}

bool AirspyHfSource::programBiasTLocked(bool on) {
    // airspyhf_set_bias_tee (airspyhf.c:1706-1726).
    if (!controlOutLocked(airspyhf::VendorRequest::SetBiasTee, on ? 1 : 0, 0, nullptr, 0,
                          "switching the bias tee")) {
        return false;
    }
    biasT_.store(on, std::memory_order_relaxed);
    return true;
}

// --- open / close ---------------------------------------------------------

bool AirspyHfSource::resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                                   std::string& error) {
    std::vector<cascade::usb::UsbDeviceInfo> devices;
    if (useFakeTransport_) {
        devices = fakeDevices_;
    } else {
#if defined(_WIN32)
        devices = cascade::usb::enumerateWinUsb(airspyHfUsbIds());
#endif
    }
    // Keep only the HF+ family: a caller may hand us a list from a wider
    // scan, and opening somebody else's dongle with these vendor requests
    // would be worse than finding nothing.
    std::vector<cascade::usb::UsbDeviceInfo> ours;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid == airspyhf::kUsbVid && d.pid == airspyhf::kUsbPid) { ours.push_back(d); }
    }
    if (ours.empty()) {
        error = "no Airspy HF+ found (is it plugged in, and bound to WinUSB?)";
        return false;
    }

    const std::string serial = argValue(args, "serial");
    if (!serial.empty()) {
        // Both sides go through normalisedSerial, so "AIRSPYHF SN:1234..." as
        // Windows reports it and "1234..." as a user reads it off another
        // tool's listing are the same string by the time they are compared.
        const std::string want = normalisedSerial(serial);
        for (const cascade::usb::UsbDeviceInfo& d : ours) {
            const std::string have = normalisedSerial(d.serial);
            // A suffix match as well as an exact one, for a serial quoted
            // without its leading zeros.
            if (have == want || (have.size() >= want.size() && !want.empty() &&
                                 have.compare(have.size() - want.size(), want.size(), want) == 0)) {
                out = d;
                return true;
            }
        }
        error = "no Airspy HF+ with serial " + serial + " is connected";
        return false;
    }

    const std::string indexText = argValue(args, "index");
    std::size_t index = 0;
    if (!indexText.empty()) {
        char* end = nullptr;
        const long n = std::strtol(indexText.c_str(), &end, 10);
        if (end == indexText.c_str() || *end != '\0' || n < 0 ||
            static_cast<std::size_t>(n) >= ours.size()) {
            error = "there is no Airspy HF+ at index " + indexText;
            return false;
        }
        index = static_cast<std::size_t>(n);
    }
    out = ours[index];
    return true;
}

bool AirspyHfSource::open(const std::string& args) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ != nullptr) {
        setError("this Airspy HF+ source already has a device open");
        return false;
    }

    cascade::usb::UsbDeviceInfo info;
    std::string error;
    if (!resolveDevice(args, info, error)) {
        setError(error);
        return false;
    }

    std::unique_ptr<cascade::usb::UsbDevice> dev;
    if (useFakeTransport_) {
        dev = fakeOpener_(info.path, error);
    } else {
#if defined(_WIN32)
        dev = cascade::usb::openWinUsb(info.path, error);
#else
        error =
            "the native Airspy HF+ driver needs the WinUSB transport (Windows only in this build)";
#endif
    }
    if (dev == nullptr) {
        setError(error.empty() ? std::string("could not open the Airspy HF+") : error);
        return false;
    }
    dev_ = std::move(dev);
    link_->dev = dev_.get();
    clearError();

    const auto giveUp = [this]() {
        dev_.reset();
        link_->dev = nullptr;
        return false;
    };

    // --- who is this, and what is on it -------------------------------
    // The part id and serial first, in libairspyhf's own order
    // (airspyhf_info.c calls board_partid_serialno_read then
    // version_string_read then get_samplerates), before anything is
    // programmed: a device that cannot answer these is not a device this
    // driver should start configuring.
    std::uint8_t partSerial[airspyhf::kSerialNoBoardIdBytes] = {0};
    std::size_t moved = 0;
    if (!controlInLocked(airspyhf::VendorRequest::GetSerialNoBoardId, 0, 0, partSerial,
                         sizeof(partSerial), "reading the part id and serial number", &moved) ||
        moved != airspyhf::kSerialNoBoardIdBytes) {
        if (moved != airspyhf::kSerialNoBoardIdBytes) {
            setError("the Airspy HF+ answered a short part id / serial number");
        }
        return giveUp();
    }
    partId_ = wordAt(partSerial, 0);
    serialNo_ = serialTextFrom(partSerial);

    std::uint8_t version[airspyhf::kVersionStringBytes + 1] = {0};
    if (!controlInLocked(airspyhf::VendorRequest::GetVersionString, 0, 0, version,
                         airspyhf::kVersionStringBytes, "reading the firmware version", &moved)) {
        return giveUp();
    }
    // Terminate at what the device ACTUALLY sent - the firmware writes no NUL
    // of its own, and trusting the zero-filled tail of our buffer would make a
    // short reply read as a long string on any future transport that reuses
    // its scratch space.
    if (moved > airspyhf::kVersionStringBytes) { moved = airspyhf::kVersionStringBytes; }
    version[moved] = 0;
    firmwareVersion_ = reinterpret_cast<const char*>(version);

    if (!readRatesLocked()) { return giveUp(); }
    readAttStepsLocked();
    readCalibrationLocked();
    readBiasTeeCountLocked();

    // --- a KNOWN state -------------------------------------------------
    // This radio keeps whatever the last application left it at, attenuator
    // and AGC included. Programming all of it here is what makes every
    // readout on this object true of the hardware from the first frame,
    // rather than true of whatever ran before us.
    centerFrequencyHz_.store(kDefaultCenterHz, std::memory_order_relaxed);
    loKhz_ = 0;
    const bool configured = programRateIndexLocked(0, "setting the sample rate") &&
                            programAttLocked(0.0) && programLnaLocked(false) &&
                            programAgcLocked(false) && programAgcThresholdLocked(false) &&
                            (!biasTeeSupported() || programBiasTLocked(false));
    if (!configured) { return giveUp(); }

    std::string label = "Airspy HF+";
    const std::string described = lowered(info.description);
    if (described.find("airspy") != std::string::npos) { label = info.description; }
    const std::string serial = normalisedSerial(info.serial);
    if (!serial.empty()) { label += " (serial " + serial + ")"; }
    setName(label);
    openMirror_.store(true, std::memory_order_relaxed);

    core::diagLogf(
        "airspyhf: opened %s - part id 0x%08x, serial %s, firmware \"%s\", %zu sample rates, "
        "%zu attenuator steps%s",
        label.c_str(), static_cast<unsigned>(partId_), serialNo_.c_str(),
        firmwareVersion_.c_str(), rates_.size(), attSteps_.size(),
        biasTeeSupported() ? ", bias tee" : "");
    return true;
}

void AirspyHfSource::closeDevice() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
    if (dev_ != nullptr) {
        // Leave the radio quiet and unpowered on the antenna port: the next
        // application to open it inherits whatever we leave behind, exactly
        // as we inherited what came before.
        if (biasTeeSupported()) { programBiasTLocked(false); }
    }
    dev_.reset();
    link_->dev = nullptr;
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    loKhz_ = 0;
    setName("Airspy HF+: (no device)");
}

std::uint32_t AirspyHfSource::partId() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return partId_;
}

std::string AirspyHfSource::serialNo() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return serialNo_;
}

std::string AirspyHfSource::firmwareVersion() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return firmwareVersion_;
}

std::vector<std::uint8_t> AirspyHfSource::rateArchitectures() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return architectures_;
}

std::vector<float> AirspyHfSource::attenuatorStepsDb() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return attSteps_;
}

double AirspyHfSource::frequencyShiftHz() const {
    return link_->freqShiftHz.load(std::memory_order_relaxed);
}

// --- streaming ------------------------------------------------------------

bool AirspyHfSource::startStreamingLocked() {
    // ORDER MATTERS, and it is libairspyhf's own (airspyhf_start,
    // airspyhf.c:1308-1334) with one addition of ours.
    //
    // RECEIVER_MODE is switched OFF first even though nothing has switched it
    // on: an application that died mid-stream leaves this firmware receiving,
    // and the off-then-on is how it is brought back to a known state. Then
    // the bulk pipe is cleared, because a device left streaming has a halted
    // endpoint by the time anyone gets here.
    //
    // The bulk ring is queued BEFORE the receiver is told to start - ours,
    // not the reference's, and for the reason hackrf_source.cpp gives: a
    // radio told to receive with nothing queued fills the firmware's own
    // buffer and overruns before the host's first read, which presents as a
    // stream that starts corrupted and then recovers, the hardest kind of
    // fault to attribute later.
    if (!setReceiverModeLocked(airspyhf::ReceiverMode::Off, "quiescing the receiver")) {
        return false;
    }
    if (dev_ != nullptr) { dev_->resetPipe(airspyhf::kRxEndpoint); }

    if (!dev_->beginBulkStream(airspyhf::kRxEndpoint, airspyhf::kTransferBufferBytes,
                               airspyhf::kTransferCount)) {
        noteTransportFault("queueing the sample transfers", dev_->lastError());
        return false;
    }
    if (!setReceiverModeLocked(airspyhf::ReceiverMode::On, "starting the receiver")) {
        dev_->endBulkStream();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(link_->waitMutex);
        link_->exited = false;
    }
    link_->run.store(true, std::memory_order_relaxed);
    reader_ = std::thread(&AirspyHfSource::readerThreadBody, link_);
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void AirspyHfSource::stopStreamingLocked() {
    if (!reader_.joinable() && !running_.load(std::memory_order_relaxed)) {
        // Nothing to stop, but the receiver may still be on from a
        // half-failed start; endBulkStream is idempotent and cheap.
        if (dev_ != nullptr) { dev_->endBulkStream(); }
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    if (dev_ != nullptr && !deviceDead()) {
        setReceiverModeLocked(airspyhf::ReceiverMode::Off, "stopping the receiver");
    }
    link_->run.store(false, std::memory_order_relaxed);
    link_->waitCv.notify_all();

    if (reader_.joinable()) {
        // A BOUNDED JOIN, not a plain one (see the file header). The reader
        // sets `exited` as its last act, so a flag still clear after
        // kReaderJoinWait means a thread parked somewhere it is not coming
        // back from - and waiting for it on the GUI thread would be the hang
        // the whole transport exists to avoid. The ordinary exit costs one
        // kBulkReadWait at most, an order of magnitude inside this bound.
        bool exited = false;
        {
            std::unique_lock<std::mutex> lk(link_->waitMutex);
            exited = link_->waitCv.wait_for(lk, airspyhf::kReaderJoinWait,
                                            [this] { return link_->exited; });
        }
        if (exited) {
            reader_.join();
        } else {
            // ABANDONED. The thread keeps its copy of the link and the device
            // pointer inside it, so NEITHER may be disturbed: the device is
            // leaked deliberately rather than destroyed under a thread still
            // calling into it, and link_->dev is left pointing at it for the
            // same reason (nulling it would be a data race with the zombie's
            // very next readBulk). This source never touches the radio again.
            g_readersAbandoned.fetch_add(1, std::memory_order_relaxed);
            noteTransportFault("waiting for the sample reader to stop",
                               "the reader did not return; the radio is left to the operating "
                               "system and FoxSDR must be restarted to use it again");
            reader_.detach();
            (void)dev_.release();
            running_.store(false, std::memory_order_relaxed);
            return;
        }
    }

    // Only now is the transport's ring safe to free: no thread of ours is
    // inside readBulk, so nothing can be reading a buffer endBulkStream is
    // about to release.
    if (dev_ != nullptr) { dev_->endBulkStream(); }
    running_.store(false, std::memory_order_relaxed);
}

bool AirspyHfSource::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("start() called with no Airspy HF+ open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running_.load(std::memory_order_relaxed)) { return true; }
    clearError();
    return startStreamingLocked();
}

void AirspyHfSource::stop() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
}

// --- the reader thread ----------------------------------------------------

void AirspyHfSource::readerThreadBody(std::shared_ptr<ReaderLink> link) {
    std::vector<std::uint8_t> raw(airspyhf::kTransferBufferBytes);
    std::vector<std::complex<float>> conv(airspyhf::kSamplesPerTransfer);
    const unsigned timeoutMs = static_cast<unsigned>(airspyhf::kBulkReadWait.count());

    // airspyhf_start (airspyhf.c:1313-1314) restarts the rotation vector at
    // 1 + 0i for every stream: the phase it left off at belongs to a stretch
    // of signal that is now over.
    link->rotator = airspyhf::Rotator{};

    while (link->run.load(std::memory_order_relaxed)) {
        const int got = link->dev->readBulk(raw.data(), raw.size(), timeoutMs);
        if (!link->run.load(std::memory_order_relaxed)) { break; }

        if (got < 0) {
            // A NEGATIVE READ IS THE DEVICE GOING. There is nothing to retry:
            // the pipe has failed, and every further read would fail the same
            // way while the source loop waited for samples that cannot come.
            // Fault, and leave - the loop polls faulted() and stops with this
            // message.
            noteRead(*link, got, 0, false);
            noteTransportFaultOn(*link, "reading samples", link->dev->lastError());
            break;
        }
        const std::size_t samples = static_cast<std::size_t>(got) / airspyhf::kBytesPerSample;
        bool dropped = false;
        if (samples > 0) {
            // THE THREE HOST CORRECTIONS, in libairspyhf's own order
            // (convert_samples, airspyhf.c:316-361): scale by the filter
            // gain, then - only when the host DSP is on - reject the image on
            // a zero-IF rate and rotate the residual out.
            airspyhf::decodeSamples(raw.data(), samples,
                                    link->filterGain.load(std::memory_order_relaxed), conv.data());

            if (link->dspEnabled.load(std::memory_order_relaxed)) {
                if (link->balancerResetWanted.exchange(false, std::memory_order_relaxed)) {
                    link->balancer.setOptimalPoint(
                        link->optimalPoint.load(std::memory_order_relaxed));
                }
                if (!link->lowIf.load(std::memory_order_relaxed)) {
                    // Zero IF requires external IQ correction (the
                    // reference's own comment, airspyhf.c:338).
                    link->balancer.process(conv.data(), static_cast<int>(samples), false);
                }
                airspyhf::rotateBlock(conv.data(), samples,
                                      link->freqShiftHz.load(std::memory_order_relaxed),
                                      link->sampleRateHz.load(std::memory_order_relaxed),
                                      link->rotator);
            }

            const std::size_t written = link->ring.write(conv.data(), samples);
            if (written != samples) {
                // The host fell behind, not the radio. Counted rather than
                // silently tolerated: a spectrum with a gap in it and no
                // number anywhere is how a slow machine looks exactly like a
                // broken one.
                dropped = true;
                link->dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        noteRead(*link, got, samples, dropped);
        if (samples == 0) { continue; }
        {
            // Taken and released so a read() that has just tested the ring and
            // is about to wait cannot miss this notify in between.
            std::lock_guard<std::mutex> lk(link->waitMutex);
        }
        link->waitCv.notify_all();
    }

    // LAST ACT, and the thing stopStreamingLocked's bounded join waits for.
    {
        std::lock_guard<std::mutex> lk(link->waitMutex);
        link->run.store(false, std::memory_order_relaxed);
        link->exited = true;
    }
    link->waitCv.notify_all();
}

void AirspyHfSource::noteRead(ReaderLink& link, int ret, std::size_t samples, bool dropped) {
    const auto now = std::chrono::steady_clock::now();
    std::string line;
    bool warn = false;
    {
        std::lock_guard<std::mutex> lk(link.healthMutex);
        StreamHealth& h = link.health;
        if (!h.windowOpen) {
            h.windowOpen = true;
            h.windowStart = now;
            h.lastSamples = now;
        }
        ++h.reads;
        if (ret > 0 && samples > 0) {
            ++h.withSamples;
            h.samples += samples;
            const auto gap =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - h.lastSamples).count();
            if (gap > h.longestGapMs) { h.longestGapMs = gap; }
            h.lastSamples = now;
        } else if (ret >= 0) {
            // The bounded block expired with nothing ready - the transport's
            // "nothing yet", not a failure. Counted, never reported as an
            // error, exactly as the Soapy path treats SOAPY_SDR_TIMEOUT.
            ++h.timeouts;
        } else {
            ++h.errors;
        }
        // The ring overflowing is the host's problem, not the radio's, and it
        // is classified exactly like a Soapy overflow: recorded, counted in
        // the window, never a fault.
        if (dropped) { ++h.overflows; }

        if (now - h.windowStart < link.healthWindow) { return; }

        const bool nominal =
            h.timeouts == 0 && h.overflows == 0 && h.errors == 0 && h.longestGapMs < 250;
        const bool first = !link.healthEverWritten;
        warn = h.errors > 0 || h.longestGapMs >= 1000;
        line = healthLineLocked(link);
        if (line.empty()) { return; }
        if (!(warn || first || !nominal)) { return; }
        link.healthEverWritten = true;
    }
    if (warn) {
        core::diagWarnf("%s", line.c_str());
    } else {
        core::diagLogf("%s", line.c_str());
    }
}

std::string AirspyHfSource::healthLineLocked(ReaderLink& link) {
    StreamHealth& h = link.health;
    if (!h.windowOpen || h.reads == 0) { return std::string(); }
    const auto now = std::chrono::steady_clock::now();
    const auto openGap =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - h.lastSamples).count();
    if (openGap > h.longestGapMs) { h.longestGapMs = openGap; }
    const auto windowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - h.windowStart).count();
    char buf[192];
    // THE SAME FORMAT SoapySource::streamHealthLine WRITES, word for word. A
    // reader of the diagnostic log should not have to know which driver was
    // open to parse the line, and a report from a native Airspy HF+ should be
    // comparable with one from a Soapy device on the same numbers.
    std::snprintf(buf, sizeof(buf),
                  "source: stream health - reads %llu, with samples %llu, timeouts %llu, "
                  "overflows %llu, errors %llu, longest gap %lld ms, %llu samples in %lld s",
                  static_cast<unsigned long long>(h.reads),
                  static_cast<unsigned long long>(h.withSamples),
                  static_cast<unsigned long long>(h.timeouts),
                  static_cast<unsigned long long>(h.overflows),
                  static_cast<unsigned long long>(h.errors),
                  static_cast<long long>(h.longestGapMs),
                  static_cast<unsigned long long>(h.samples),
                  static_cast<long long>((windowMs + 500) / 1000));
    h = StreamHealth{};
    return std::string(buf);
}

std::string AirspyHfSource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void AirspyHfSource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

std::uint64_t AirspyHfSource::droppedTransfers() const {
    return link_->dropped.load(std::memory_order_relaxed);
}

unsigned long long AirspyHfSource::readersAbandoned() {
    return g_readersAbandoned.load(std::memory_order_relaxed);
}

// --- read -----------------------------------------------------------------

std::size_t AirspyHfSource::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) { return 0; }
    std::size_t got = link_->ring.read(dst, n);
    if (got > 0) { return got; }
    if (faulted()) { return 0; }
    {
        // Bounded, and short. The pipeline's self-paced loop treats a zero as
        // "nothing yet" and backs off a millisecond of its own, so there is
        // nothing to gain from waiting longer than one chunk period here.
        std::unique_lock<std::mutex> lk(link_->waitMutex);
        link_->waitCv.wait_for(lk, airspyhf::kReadWait, [this] {
            return link_->ring.size() > 0 || !link_->run.load(std::memory_order_relaxed);
        });
    }
    got = link_->ring.read(dst, n);
    return got;
}

// --- rate, frequency, gains ----------------------------------------------

std::vector<double> AirspyHfSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    std::vector<double> out;
    out.reserve(rates_.size());
    for (const std::uint32_t r : rates_) { out.push_back(static_cast<double>(r)); }
    // Ascending, because that is what DeviceSource promises and what a menu
    // wants. The firmware's own list is descending on current Discovery
    // firmware, and the INDEX the rate is programmed by is an index into the
    // firmware's order, never into this one - which is why rateIndex_ is kept
    // separately and this function's order is free to differ.
    std::sort(out.begin(), out.end());
    return out;
}

bool AirspyHfSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setSampleRateHz() called with no Airspy HF+ open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz > 0.0)) {  // negated compare so a NaN lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    if (rates_.empty()) {
        setError("the Airspy HF+ has not listed any sample rates");
        return false;
    }

    const std::size_t index = airspyhf::nearestRateIndex(rates_, hz);
    const double chosen = static_cast<double>(rates_[index]);
    if (std::fabs(chosen - hz) > 0.5) {
        // COERCED, not refused, and said out loud. The firmware takes an
        // index into its own list; there is no fractional divider to ask for
        // anything else, so a caller that wants 1 MS/s gets the nearest thing
        // the hardware has and is told which.
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "the Airspy HF+ has no %.3f kS/s; the nearest rate it offers is %.3f kS/s",
                      hz / 1e3, chosen / 1e3);
        setError(buf);
    }

    const bool wasRunning = running_.load(std::memory_order_relaxed);
    if (wasRunning) {
        // A QUIET RADIO for the change (see the header): the receiver is
        // switched off, our reader is joined and the bulk ring torn down
        // before the clock underneath it moves.
        stopStreamingLocked();
    }
    if (!programRateIndexLocked(index, "setting the sample rate")) {
        if (wasRunning && !deviceDead()) { startStreamingLocked(); }
        return false;
    }
    if (wasRunning) { return startStreamingLocked(); }
    return true;
}

bool AirspyHfSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setCenterFrequencyHz() called with no Airspy HF+ open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!airspyhf::frequencyCovered(hz)) {
        char buf[208];
        std::snprintf(buf, sizeof(buf),
                      "the Airspy HF+ covers %.3f kHz to %.0f MHz and %.0f to %.0f MHz; "
                      "%.6f MHz is in neither band",
                      airspyhf::kHfLowHz / 1e3, airspyhf::kHfHighHz / 1e6,
                      airspyhf::kVhfLowHz / 1e6, airspyhf::kVhfHighHz / 1e6, hz / 1e6);
        setError(buf);
        return false;
    }
    return programFrequencyLocked(hz, "setting the centre frequency");
}

bool AirspyHfSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    // The ENVELOPE of the two bands, because this interface has room for one
    // span. It is deliberately not narrowed to the HF band: a caller using
    // this to bound a slider must be able to reach 260 MHz, and
    // setCenterFrequencyHz refuses the gap in the middle with a message that
    // names both bands.
    loHz = airspyhf::kHfLowHz;
    hiHz = airspyhf::kVhfHighHz;
    return true;
}

std::vector<GainInfo> AirspyHfSource::gains() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    // The attenuator as a NEGATIVE gain - see the header for why the sign is
    // turned over here rather than left to the panel.
    double worstDb = 0.0;
    double stepDb = static_cast<double>(airspyhf::kDefaultAttStepDb);
    if (!attSteps_.empty()) {
        worstDb = static_cast<double>(attSteps_.back());
        if (attSteps_.size() >= 2) {
            stepDb = static_cast<double>(attSteps_[1]) - static_cast<double>(attSteps_[0]);
        }
    }
    if (!(stepDb > 0.0)) { stepDb = static_cast<double>(airspyhf::kDefaultAttStepDb); }
    return {
        GainInfo{"ATT", -worstDb, 0.0, stepDb},
        GainInfo{"LNA", 0.0, airspyhf::kLnaGainDb, airspyhf::kLnaGainDb},
    };
}

bool AirspyHfSource::setGainDb(const std::string& gainName, double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setGainDb() called with no Airspy HF+ open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (gainName == "ATT") { return programAttLocked(db); }
    // The preamp is a switch: anything at or above half its step is on. There
    // is no reference function for this - libairspyhf takes a 0/1 - so the
    // rounding rule is ours, and it is the one a 6 dB step implies.
    if (gainName == "LNA") { return programLnaLocked(db >= airspyhf::kLnaGainDb * 0.5); }
    setError("the Airspy HF+ has no gain called \"" + gainName + "\"");
    return false;
}

double AirspyHfSource::gainDb(const std::string& gainName) const {
    if (gainName == "ATT") { return attDb_.load(std::memory_order_relaxed); }
    if (gainName == "LNA") { return lnaDb_.load(std::memory_order_relaxed); }
    return 0.0;
}

bool AirspyHfSource::setAutoGain(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setAutoGain() called with no Airspy HF+ open");
        return false;
    }
    if (deviceDead()) { return false; }
    return programAgcLocked(on);
}

bool AirspyHfSource::setAgcThresholdHigh(bool high) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setAgcThresholdHigh() called with no Airspy HF+ open");
        return false;
    }
    if (deviceDead()) { return false; }
    return programAgcThresholdLocked(high);
}

bool AirspyHfSource::setAntenna(const std::string& antennaName) {
    if (antennaName == "RX") { return true; }
    setError("the Airspy HF+ has one receive path, \"RX\"");
    return false;
}

bool AirspyHfSource::setBiasT(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setBiasT() called with no Airspy HF+ open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!biasTeeSupported()) {
        setError("this Airspy HF+ did not report a bias tee");
        return false;
    }
    return programBiasTLocked(on);
}

bool AirspyHfSource::setCalibrationPpb(std::int32_t ppb) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setCalibrationPpb() called with no Airspy HF+ open");
        return false;
    }
    if (deviceDead()) { return false; }
    calibrationPpb_.store(ppb, std::memory_order_relaxed);
    // airspyhf_set_calibration (airspyhf.c:1519-1523) re-tunes, because every
    // tune is scaled by this number.
    return programFrequencyLocked(centerFrequencyHz_.load(std::memory_order_relaxed),
                                  "re-tuning after a calibration change");
}

bool AirspyHfSource::setHostDspEnabled(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    dspEnabled_.store(on, std::memory_order_relaxed);
    link_->dspEnabled.store(on, std::memory_order_relaxed);
    if (dev_ == nullptr || deviceDead()) { return dev_ != nullptr; }
    // The IF offset is part of the DSP: switching it changes where the LO
    // belongs, so this is a retune and not just a flag
    // (airspyhf.c:1367 reads enable_dsp inside the tuning arithmetic).
    return programFrequencyLocked(centerFrequencyHz_.load(std::memory_order_relaxed),
                                  "re-tuning after a host DSP change");
}

}  // namespace cascade::source
