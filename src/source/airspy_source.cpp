// The Airspy driver itself. See airspy_source.hpp for the threading and
// ownership argument; the protocol numbers, the packing and the real-to-complex
// conversion are in airspy_protocol.hpp, which is where the libairspy
// attribution lives.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/airspy_source.hpp"

#include "core/diag_log.hpp"
#include "core/leak_on_purpose.hpp"
#include "source/hydrasdr_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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

// Upper-cased copy, for the model substring test - "Mini", "MINI" and "mini"
// all have to answer the same, because the string comes from a firmware build
// nobody here controls.
std::string uppered(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    return s;
}

// Hex of a byte range, for the part-id/serial readback. The firmware answers
// six little-endian uint32s (airspy.h:108-111, airspy_read_partid_serialno_t);
// every Airspy tool prints them as eight hex digits each, in that order, so
// this driver's log line can be compared with theirs.
std::string hexWords(const std::uint8_t* data, std::size_t words) {
    std::string out;
    char buf[16];
    for (std::size_t w = 0; w < words; ++w) {
        std::uint32_t v = airspy::decodeWord(data + w * 4);
        std::snprintf(buf, sizeof(buf), "%08x", v);
        out += buf;
    }
    return out;
}

}  // namespace

// --- enumeration ----------------------------------------------------------

std::vector<cascade::usb::UsbId> airspyUsbIds() { return {{airspy::kUsbVid, airspy::kUsbPid}}; }

namespace {
// libairspy airspy.c:119 area: GET_SAMPLERATES's fallback, airspy.c:902-906.
constexpr std::uint32_t kAirspyFallbackRates[] = {10000000u, 2500000u};
constexpr const char* kAirspyPorts[] = {"RX"};
}  // namespace

// THE AIRSPY'S PROFILE: every value here is what the driver had written into it
// before the profile existed, which is the reason tests/test_airspy_source.cpp
// passes unchanged.
const DeviceProfile& airspyProfile() {
    static const DeviceProfile p = {
        /*driverKey*/ "airspy",
        /*productName*/ "Airspy",
        /*genericModel*/ "Airspy",
        /*namePrefix*/ "Airspy: ",
        /*noDeviceName*/ "Airspy: (no device)",
        /*usbVid*/ airspy::kUsbVid,
        /*usbPid*/ airspy::kUsbPid,
        /*freqPayloadBytes*/ airspy::kFreqPayloadBytes,
        /*minFrequencyHz*/ airspy::kMinFrequencyHz,
        /*maxFrequencyHz*/ airspy::kMaxFrequencyHz,
        /*biasTee*/ BiasTeeVia::GpioWrite,
        /*ports*/ kAirspyPorts,
        /*portCount*/ 1,
        /*firmwarePrefix*/ "",
        /*fallbackRatesHz*/ kAirspyFallbackRates,
        /*fallbackRateCount*/ 2,
        /*modelFrom*/ &airspyModelFrom,
    };
    return p;
}

std::string airspyModelFrom(const std::string& text) {
    const std::string up = uppered(text);
    // "MINI" is the only word either string carries that separates the two
    // boards. An R2's product string is "AIRSPY" and its firmware version
    // string names the NOS build; neither contains "MINI".
    if (up.find("MINI") != std::string::npos) { return "Airspy Mini"; }
    if (up.find("AIRSPY") != std::string::npos) { return "Airspy R2"; }
    return "Airspy";
}

std::vector<NativeDeviceInfo> airspyDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices) {
    return devicesFromProfile(airspyProfile(), devices);
}

std::vector<NativeDeviceInfo> devicesFromProfile(
    const DeviceProfile& profile, const std::vector<cascade::usb::UsbDeviceInfo>& devices) {
    std::vector<NativeDeviceInfo> out;
    int index = 0;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid != profile.usbVid || d.pid != profile.usbPid) { continue; }
        NativeDeviceInfo info;
        info.driver = profile.driverKey;
        // The bus-reported description is the only thing at enumeration time
        // that can tell a Mini from an R2 (see airspyModelFrom in the header
        // for why the board id cannot). When it is empty - which happens on a
        // devnode whose product string never got cached - the label says
        // "Airspy" and open() corrects it from the firmware version string.
        info.label = profile.modelFrom(d.description);
        if (!d.serial.empty()) {
            info.label += " (serial " + d.serial + ")";
            info.args = "serial=" + d.serial;
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

std::vector<NativeDeviceInfo> enumerateAirspy() {
    // See hackrf_source.cpp's enumerateHackRf(): enumerateWinUsb() is the one
    // entry point on every platform (WinUSB on Windows, usbfs on Linux).
    return airspyDevicesFrom(cascade::usb::enumerateWinUsb(airspyUsbIds()));
}

// --- construction ---------------------------------------------------------

AirspySource::AirspySource(const DeviceProfile& profile)
    : profile_(profile),
      link_(std::make_shared<ReaderLink>(kRingCapacitySamples(), profile.driverKey)),
      name_(profile.noDeviceName) {}

AirspySource::~AirspySource() { closeDevice(); }

std::string AirspySource::noDeviceSentence(const char* function) const {
    return std::string(function) + "() called with no " + profile_.productName + " open";
}

void AirspySource::setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices,
                                       UsbOpenFn opener) {
    std::lock_guard<std::mutex> lk(devMutex_);
    fakeDevices_ = std::move(devices);
    fakeOpener_ = std::move(opener);
    useFakeTransport_ = static_cast<bool>(fakeOpener_);
}

// --- the error slot -------------------------------------------------------

void AirspySource::setErrorOn(ReaderLink& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void AirspySource::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void AirspySource::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
}

void AirspySource::noteTransportFaultOn(ReaderLink& link, const char* what,
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
    core::diagWarnf("%s: transfer failed while %s%s%s", link.tag, what, detail.empty() ? "" : ": ",
                    detail.c_str());
}

void AirspySource::noteTransportFault(const char* what, const std::string& detail) {
    noteTransportFaultOn(*link_, what, detail);
}

bool AirspySource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool AirspySource::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string AirspySource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deadWhat;
}

const char* AirspySource::lastError() const {
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

void AirspySource::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* AirspySource::name() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        snapshot = name_;
    }
    return snapshot.c_str();
}

// --- control transfers ----------------------------------------------------

bool AirspySource::controlOutLocked(airspy::VendorRequest r, std::uint16_t value,
                                    std::uint16_t index, const std::uint8_t* data,
                                    std::size_t len, const char* what) {
    if (dev_ == nullptr) {
        setError(std::string("no ") + profile_.productName + " is open (" + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const int ret = dev_->controlOut(cascade::usb::kRequestTypeVendorOut, airspy::requestByte(r),
                                     value, index, data, len, airspy::kControlTimeoutMs);
    if (ret < 0 || static_cast<std::size_t>(ret) != len) {
        noteTransportFault(what, dev_->lastError());
        return false;
    }
    return true;
}

bool AirspySource::controlInLocked(airspy::VendorRequest r, std::uint16_t value,
                                   std::uint16_t index, std::uint8_t* data, std::size_t len,
                                   const char* what, std::size_t* moved) {
    if (moved != nullptr) { *moved = 0; }
    if (dev_ == nullptr) {
        setError(std::string("no ") + profile_.productName + " is open (" + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const int ret = dev_->controlIn(cascade::usb::kRequestTypeVendorIn, airspy::requestByte(r),
                                    value, index, data, len, airspy::kControlTimeoutMs);
    if (ret < 0) {
        noteTransportFault(what, dev_->lastError());
        return false;
    }
    if (moved != nullptr) { *moved = static_cast<std::size_t>(ret); }
    // A short answer is not in itself a fault - VERSION_STRING_READ returns
    // however many characters the firmware has - so the callers that need a
    // whole struct back check the count themselves.
    return true;
}

// EVERY ONE OF THE AIRSPY'S SETTERS IS A CONTROL *IN*, and that is not a typo
// carried over from somewhere. libairspy sends SET_SAMPLERATE, SET_LNA_GAIN,
// SET_MIXER_GAIN, SET_VGA_GAIN, SET_LNA_AGC, SET_MIXER_AGC and SET_PACKING as
// device-to-host transfers with the new value in the INDEX word and one byte
// of acknowledgement coming back (airspy.c:1151-1159, :1696-1704, :1726-1734,
// :1756-1764, :1783-1791, :1810-1818, :1913-1921). Sending them as OUTs, which
// is what their names suggest, would stall on the real firmware. Only
// RECEIVER_MODE, SET_FREQ and GPIO_WRITE are OUTs.
namespace {
// The one-byte acknowledgement those requests answer with. The reference only
// ever checks that a byte ARRIVED (`result < length`), never its value, so
// neither does this.
constexpr std::size_t kAckBytes = 1;
}  // namespace

bool AirspySource::setReceiverModeLocked(airspy::ReceiverMode mode, const char* what) {
    // libairspy airspy.c:1170-1190, airspy_set_receiver_mode: an OUT, the mode
    // in the VALUE word, index 0, no payload.
    return controlOutLocked(airspy::VendorRequest::ReceiverMode,
                            static_cast<std::uint16_t>(mode), 0, nullptr, 0, what);
}

bool AirspySource::readSampleRatesLocked() {
    // libairspy airspy.c:812-832 and :889-906. Two requests: the first with
    // INDEX 0 asks for the COUNT and gets one word, the second with INDEX
    // equal to that count gets the rates themselves.
    rates_.clear();
    rateIndex_.clear();

    std::uint8_t countBytes[airspy::kSampleRateWordBytes] = {0};
    std::size_t moved = 0;
    bool ok = controlInLocked(airspy::VendorRequest::GetSampleRates, 0, 0, countBytes,
                              sizeof(countBytes), "reading the sample-rate count", &moved) &&
              moved == sizeof(countBytes);
    std::uint32_t count = ok ? airspy::decodeWord(countBytes) : 0;
    if (ok && (count == 0 || count > airspy::kMaxSampleRateCount)) {
        // The reference mallocs whatever the device claims (airspy.c:892).
        // This does not: a wild count is a firmware that is not answering
        // this request, and the fallback below is a better answer than a
        // gigabyte allocation.
        ok = false;
    }

    std::vector<std::uint32_t> raw;
    if (ok) {
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(count) *
                                      airspy::kSampleRateWordBytes);
        ok = controlInLocked(airspy::VendorRequest::GetSampleRates, 0,
                             static_cast<std::uint16_t>(count), buf.data(), buf.size(),
                             "reading the sample-rate list", &moved) &&
             moved == buf.size();
        if (ok) {
            for (std::uint32_t i = 0; i < count; ++i) {
                raw.push_back(airspy::decodeWord(buf.data() + i * airspy::kSampleRateWordBytes));
            }
        }
    }
    // A device that faulted while answering is dead and there is nothing to
    // fall back to; a device that merely answered nonsense gets the
    // reference's own fallback list.
    if (deviceDead()) { return false; }
    if (!ok || raw.empty()) {
        if (profile_.fallbackRateCount == 0) {
            // A PROFILE WITH NO FALLBACK (the HydraSDR's: its host library has
            // none either - the count is simply zero and no rate can be set).
            // A radio that cannot say what it runs at is not opened, and
            // nothing failed on the wire, so the device is not condemned.
            setError(std::string("the ") + profile_.productName +
                     " did not answer GET_SAMPLERATES, so there is no sample rate to run at");
            return false;
        }
        // libairspy airspy.c:902-906, verbatim: an R2's two rates, in the
        // firmware's own order. Said out loud, because a receiver quietly
        // offering rates it was never told about is a lie the spectrum would
        // not reveal.
        core::diagWarnf(
            "%s: the firmware did not answer GET_SAMPLERATES; falling back to libairspy's "
            "own default list (10 and 2.5 MS/s), which is right for an R2 and may not be for "
            "this board",
            profile_.driverKey);
        raw.assign(profile_.fallbackRatesHz, profile_.fallbackRatesHz + profile_.fallbackRateCount);
        clearError();
    }

    // The firmware lists them highest first; the panel wants them ascending,
    // and the INDEX that SET_SAMPLERATE takes is the firmware's, not ours - so
    // the two orders are kept side by side rather than one being recomputed
    // from the other later.
    std::vector<std::size_t> order(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) { order[i] = i; }
    std::sort(order.begin(), order.end(),
              [&raw](std::size_t a, std::size_t b) { return raw[a] < raw[b]; });
    for (const std::size_t i : order) {
        rates_.push_back(static_cast<double>(raw[i]));
        rateIndex_.push_back(i);
    }
    return true;
}

bool AirspySource::programRateIndexLocked(std::size_t index, const char* what) {
    // libairspy airspy.c:1147 clears the halt on the RX pipe before every
    // SET_SAMPLERATE, because the firmware reprograms its clock generator and
    // whatever was in flight is no longer a sample. resetPipe is the
    // transport's word for the same thing.
    if (dev_ != nullptr) { dev_->resetPipe(airspy::kRxEndpoint); }
    std::uint8_t ack = 0;
    return controlInLocked(airspy::VendorRequest::SetSampleRate, 0,
                           static_cast<std::uint16_t>(index), &ack, kAckBytes, what);
}

bool AirspySource::programFrequencyLocked(double hz, const char* what) {
    // libairspy airspy.c:1631-1657: an OUT, value and index zero, four bytes
    // of little-endian Hz - or eight, on the radio whose firmware takes a
    // uint64 (the profile's width; hydrasdr_protocol.hpp has the lines).
    std::uint8_t payload[8] = {0};
    const std::size_t width = std::min<std::size_t>(profile_.freqPayloadBytes, sizeof(payload));
    airspy::encodeFreqWide(static_cast<std::uint64_t>(hz + 0.5), payload, width);
    return controlOutLocked(airspy::VendorRequest::SetFreq, 0, 0, payload, width, what);
}

bool AirspySource::programPackingLocked(bool on) {
    // libairspy airspy.c:1902-1945. Refused while streaming there (:1908) and
    // here, because the transfer SIZE changes with it and a ring queued for
    // one size cannot carry the other.
    std::uint8_t ack = 0;
    if (!controlInLocked(airspy::VendorRequest::SetPacking, 0, on ? 1 : 0, &ack, kAckBytes,
                         "switching 12-bit sample packing on")) {
        return false;
    }
    packing_.store(on, std::memory_order_relaxed);
    return true;
}

bool AirspySource::programLnaLocked(int index) {
    // libairspy airspy.c:1685-1713. Clamped at 14 there; clamped at both ends
    // here, because device_source.hpp's contract is that an out-of-range gain
    // is clamped rather than refused.
    const int v = std::clamp(index, 0, airspy::kLnaMaxIndex);
    std::uint8_t ack = 0;
    if (!controlInLocked(airspy::VendorRequest::SetLnaGain, 0, static_cast<std::uint16_t>(v), &ack,
                         kAckBytes, "setting the LNA gain")) {
        return false;
    }
    return true;
}

bool AirspySource::programMixerLocked(int index) {
    // libairspy airspy.c:1715-1743, the same shape with a ceiling of 15.
    const int v = std::clamp(index, 0, airspy::kMixerMaxIndex);
    std::uint8_t ack = 0;
    if (!controlInLocked(airspy::VendorRequest::SetMixerGain, 0, static_cast<std::uint16_t>(v),
                         &ack, kAckBytes, "setting the mixer gain")) {
        return false;
    }
    return true;
}

bool AirspySource::programVgaLocked(int index) {
    // libairspy airspy.c:1745-1773.
    const int v = std::clamp(index, 0, airspy::kVgaMaxIndex);
    std::uint8_t ack = 0;
    if (!controlInLocked(airspy::VendorRequest::SetVgaGain, 0, static_cast<std::uint16_t>(v), &ack,
                         kAckBytes, "setting the VGA gain")) {
        return false;
    }
    return true;
}

bool AirspySource::programLnaAgcLocked(bool on) {
    // libairspy airspy.c:1775-1800.
    std::uint8_t ack = 0;
    return controlInLocked(airspy::VendorRequest::SetLnaAgc, 0, on ? 1 : 0, &ack, kAckBytes,
                           "switching the LNA AGC");
}

bool AirspySource::programMixerAgcLocked(bool on) {
    // libairspy airspy.c:1802-1827.
    std::uint8_t ack = 0;
    return controlInLocked(airspy::VendorRequest::SetMixerAgc, 0, on ? 1 : 0, &ack, kAckBytes,
                           "switching the mixer AGC");
}

bool AirspySource::programCombinedLocked(int index, bool linearity) {
    // libairspy airspy.c:1829-1861 (linearity) and :1863-1895 (sensitivity).
    // THE ORDER IS THE REFERENCE'S AND IT MATTERS: both AGCs are switched off
    // first, because a table that programs three registers while an AGC is
    // still moving one of them has not set the gain it says it has. Then VGA,
    // then MIXER, then LNA.
    const airspy::CombinedGain g = airspy::combinedGainFor(index, linearity);
    if (!programMixerAgcLocked(false) || !programLnaAgcLocked(false)) { return false; }
    if (!programVgaLocked(g.vga) || !programMixerLocked(g.mixer) || !programLnaLocked(g.lna)) {
        return false;
    }
    // THE MODE'S OWN VALUE, and the mode. The table rewrote all three
    // registers, but Free mode's manual LNA / MIXER / VGA are NOT touched:
    // they are what the user set in Free mode and what Free mode puts back.
    // The AGC flags are Free mode's too, and are left as the user set them
    // for the same reason - the radio's AGCs are off now whatever they say.
    const int clamped = std::clamp(index, 0, airspy::kCombinedMaxIndex);
    (linearity ? linearityIndex_ : sensitivityIndex_).store(clamped, std::memory_order_relaxed);
    gainMode_.store(static_cast<int>(linearity ? GainMode::Linearity : GainMode::Sensitivity),
                    std::memory_order_relaxed);
    return true;
}

bool AirspySource::programFreeLocked() {
    // The reference's order everywhere in this file: mixer AGC, LNA AGC, then
    // VGA, MIXER, LNA (airspy.c:1840-1858). A stage its AGC is driving is not
    // written: the AGC owns that register, and the manual value is kept for
    // the moment the AGC is switched off.
    const bool mAgc = mixerAgc_.load(std::memory_order_relaxed);
    const bool lAgc = lnaAgc_.load(std::memory_order_relaxed);
    if (!programMixerAgcLocked(mAgc) || !programLnaAgcLocked(lAgc)) { return false; }
    if (!programVgaLocked(vgaIndex_.load(std::memory_order_relaxed))) { return false; }
    if (!mAgc && !programMixerLocked(mixerIndex_.load(std::memory_order_relaxed))) { return false; }
    if (!lAgc && !programLnaLocked(lnaIndex_.load(std::memory_order_relaxed))) { return false; }
    gainMode_.store(static_cast<int>(GainMode::Free), std::memory_order_relaxed);
    return true;
}

bool AirspySource::applyGainModeLocked(GainMode mode) {
    switch (mode) {
        case GainMode::Linearity:
            return programCombinedLocked(linearityIndex_.load(std::memory_order_relaxed), true);
        case GainMode::Sensitivity:
            return programCombinedLocked(sensitivityIndex_.load(std::memory_order_relaxed), false);
        case GainMode::Free:
        default:
            return programFreeLocked();
    }
}

bool AirspySource::programBiasTLocked(bool on) {
    // libairspy airspy.c:1897-1900 -> airspy_gpio_write (:1357-1382): an OUT,
    // request GPIO_WRITE, the state in the VALUE word and (port << 5) | pin in
    // the INDEX word. See airspy_protocol.hpp's kBiasTPortPin for why this is
    // not request 20.
    //
    // THE HYDRASDR'S HOST DOES NOT DO THAT: it sends SET_RF_BIAS_CMD (request
    // 20), value 0, the state in the INDEX word (hydrasdr_protocol.hpp), and the
    // profile says which of the two this radio is spoken to with.
    const bool ok =
        profile_.biasTee == BiasTeeVia::RfBiasRequest
            ? controlOutLocked(airspy::VendorRequest::SetRfBiasCmd, 0, on ? 1 : 0, nullptr, 0,
                               "switching the bias tee")
            : controlOutLocked(airspy::VendorRequest::GpioWrite, on ? 1 : 0, airspy::kBiasTPortPin,
                               nullptr, 0, "switching the bias tee");
    if (!ok) { return false; }
    biasT_.store(on, std::memory_order_relaxed);
    return true;
}

bool AirspySource::programRfPortLocked(std::size_t index, const char* what) {
    // The HydraSDR host's hydrasdr_set_rf_port (hydrasdr_shared.c:2221-2242): an
    // IN, value 0, the port in the INDEX word, one byte back that must be 1.
    // The firmware STALLS a port it does not have (m0/usb_req.c:897-913), which
    // is a failed transfer here and condemns the device like any other; this
    // driver only ever sends the profile's own.
    std::uint8_t ack = 0;
    std::size_t moved = 0;
    const std::uint16_t port = static_cast<std::uint16_t>(index);
    if (!controlInLocked(hydrasdr::kSetRfPortRequest, 0, port, &ack, 1, what, &moved)) {
        return false;
    }
    if (moved != 1 || ack != 1) {
        setError(std::string("the ") + profile_.productName + " would not select receive port " +
                 profile_.ports[index]);
        return false;
    }
    portIndex_.store(index, std::memory_order_relaxed);
    return true;
}

// --- open / close ---------------------------------------------------------

bool AirspySource::resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                                 std::string& error) {
    std::vector<cascade::usb::UsbDeviceInfo> devices;
    if (useFakeTransport_) {
        devices = fakeDevices_;
    } else {
        devices = cascade::usb::enumerateWinUsb({{profile_.usbVid, profile_.usbPid}});
    }
    // Keep only OUR radio: a caller may hand us a list from a wider scan, and
    // opening somebody else's dongle with these vendor requests would be worse
    // than finding nothing - an Airspy's id is not a HydraSDR's, and the other
    // way about.
    std::vector<cascade::usb::UsbDeviceInfo> ours;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid == profile_.usbVid && d.pid == profile_.usbPid) { ours.push_back(d); }
    }
    if (ours.empty()) {
        error = std::string("no ") + profile_.productName +
                " found (is it plugged in, and bound to WinUSB?)";
        return false;
    }

    const std::string serial = argValue(args, "serial");
    if (!serial.empty()) {
        const std::string want = lowered(serial);
        for (const cascade::usb::UsbDeviceInfo& d : ours) {
            const std::string have = lowered(d.serial);
            // A suffix match as well as an exact one: an Airspy's serial is 16
            // hex digits of which the first half is a constant prefix, and the
            // number a user reads off another tool's listing is the tail of it.
            if (have == want || (have.size() >= want.size() &&
                                 have.compare(have.size() - want.size(), want.size(), want) == 0)) {
                out = d;
                return true;
            }
        }
        error = std::string("no ") + profile_.productName + " with serial " + serial +
                " is connected";
        return false;
    }

    const std::string indexText = argValue(args, "index");
    std::size_t index = 0;
    if (!indexText.empty()) {
        char* end = nullptr;
        const long n = std::strtol(indexText.c_str(), &end, 10);
        if (end == indexText.c_str() || *end != '\0' || n < 0 ||
            static_cast<std::size_t>(n) >= ours.size()) {
            error = std::string("there is no ") + profile_.productName + " at index " + indexText;
            return false;
        }
        index = static_cast<std::size_t>(n);
    }
    out = ours[index];
    return true;
}

bool AirspySource::open(const std::string& args) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ != nullptr) {
        setError(std::string("this ") + profile_.productName + " source already has a device open");
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
        dev = cascade::usb::openWinUsb(info.path, error);
    }
    if (dev == nullptr) {
        setError(error.empty() ? std::string("could not open the ") + profile_.productName : error);
        return false;
    }
    dev_ = std::move(dev);
    link_->dev = dev_.get();
    clearError();

    const auto giveUp = [this] {
        dev_.reset();
        link_->dev = nullptr;
        return false;
    };

    // --- who is this, and what is on it -------------------------------
    // Three reads, in libairspy's own order, before anything is programmed: a
    // device that cannot answer these is not a device this driver should start
    // configuring.
    std::uint8_t boardId = 0;
    std::size_t moved = 0;
    if (!controlInLocked(airspy::VendorRequest::BoardIdRead, 0, 0, &boardId, 1,
                         "reading the board id", &moved) ||
        moved != 1) {
        if (moved != 1) {
            setError(std::string("the ") + profile_.productName + " did not answer its board id");
        }
        return giveUp();
    }
    boardId_ = boardId;

    // libairspy airspy.c:1556-1590 reads 127 bytes into a 128-byte local. The
    // firmware writes no terminator of its own, so the count is what ends the
    // string; trusting our zero-filled tail would make a short reply read as a
    // long one on any future transport that reuses its scratch space.
    constexpr std::size_t kVersionBytes = 127;
    std::uint8_t version[kVersionBytes + 1] = {0};
    if (!controlInLocked(airspy::VendorRequest::VersionStringRead, 0, 0, version, kVersionBytes,
                         "reading the firmware version", &moved)) {
        return giveUp();
    }
    if (moved > kVersionBytes) { moved = kVersionBytes; }
    version[moved] = 0;
    firmwareVersion_ = reinterpret_cast<const char*>(version);
    // A DEVICE THAT DOES NOT SAY WHAT THE PROFILE EXPECTS is not ours: the
    // HydraSDR host refuses one whose version string does not begin "HydraSDR RF"
    // (hydrasdr.c:187). Nothing has been programmed yet, and the device is
    // handed back as it was found.
    if (profile_.firmwarePrefix[0] != '\0' &&
        firmwareVersion_.compare(0, std::strlen(profile_.firmwarePrefix),
                                 profile_.firmwarePrefix) != 0) {
        setError(std::string("the device on the ") + profile_.productName +
                 "'s USB id did not report a firmware version beginning \"" +
                 profile_.firmwarePrefix + "\" (it said \"" + firmwareVersion_ + "\")");
        return giveUp();
    }

    // airspy.h:108-111: two part-id words then four serial words.
    constexpr std::size_t kPartIdSerialNoBytes = 24;
    std::uint8_t partSerial[kPartIdSerialNoBytes] = {0};
    if (!controlInLocked(airspy::VendorRequest::BoardPartIdSerialNoRead, 0, 0, partSerial,
                         sizeof(partSerial), "reading the part id and serial number", &moved) ||
        moved != kPartIdSerialNoBytes) {
        if (moved != kPartIdSerialNoBytes) {
            setError(std::string("the ") + profile_.productName +
                     " answered a short part id / serial number");
        }
        return giveUp();
    }
    // Words 2..5 are the MCU's unique id, which is what every Airspy tool
    // calls the serial number.
    partIdSerialNo_ = hexWords(partSerial + 8, 4);

    // WHICH BOARD. The bus description first, because that is what the Source
    // section's row already says and a name that changed on open would be a
    // different radio as far as the user is concerned; the firmware version
    // string only when the description had nothing to say.
    model_ = profile_.modelFrom(info.description);
    if (model_ == profile_.genericModel) { model_ = profile_.modelFrom(firmwareVersion_); }

    // --- a KNOWN state -------------------------------------------------
    // RECEIVER_MODE OFF FIRST. An Airspy keeps whatever the last application
    // left it at, and an application that exited without stopping it leaves it
    // streaming into a host that is not listening - after which SET_PACKING,
    // which changes the transfer size, is exactly the wrong thing to send.
    if (!setReceiverModeLocked(airspy::ReceiverMode::Off, "quietening the receiver")) {
        return giveUp();
    }

    // The rate list, which is the only way to know what this board can do.
    if (!readSampleRatesLocked()) { return giveUp(); }

    // Packing on: 12 bits of ADC in 12 bits of USB rather than 16.
    if (!programPackingLocked(true)) { return giveUp(); }

    // The HIGHEST rate the board listed, which is the firmware's own index 0
    // and the widest span the radio has. rates_ is ascending, so that is its
    // back.
    const std::size_t startRate = rates_.empty() ? 0 : rates_.size() - 1;
    // LNA / MIXER / VGA at 8 are OURS, not the reference's: libairspy has no
    // default at all (it programs nothing at open), so something had to be
    // chosen, and mid-scale on all three is the setting least likely to
    // present a user with either a dead band or a wall of intermodulation on
    // the first frame.
    constexpr int kDefaultGainIndex = 8;
    const bool configured =
        !rates_.empty() &&
        programRateIndexLocked(rateIndex_[startRate], "setting the sample rate") &&
        programFrequencyLocked(100.0e6, "setting the centre frequency") &&
        // Mixer AGC then LNA AGC, the reference's own order everywhere in this
        // file (airspy.c:1840, :1844), so a transcript from open() and one
        // from setAutoGain() read the same way.
        programMixerAgcLocked(false) && programLnaAgcLocked(false) &&
        programLnaLocked(kDefaultGainIndex) && programMixerLocked(kDefaultGainIndex) &&
        programVgaLocked(kDefaultGainIndex) && programBiasTLocked(false) &&
        // A radio with several receive ports is put on the first, so what is
        // open is what the combo shows whatever the last program left.
        (profile_.portCount < 2 || programRfPortLocked(0, "selecting the first receive port"));
    if (!configured) { return giveUp(); }
    portIndex_.store(0, std::memory_order_relaxed);
    // FREE MODE, both AGCs off, 8/8/8 - what the transfers above just put on
    // the radio. The table modes keep their defaults until chosen, and the
    // decimation starts at none: a remembered choice is the application's to
    // put back, after the open.
    gainMode_.store(static_cast<int>(GainMode::Free), std::memory_order_relaxed);
    lnaIndex_.store(kDefaultGainIndex, std::memory_order_relaxed);
    mixerIndex_.store(kDefaultGainIndex, std::memory_order_relaxed);
    vgaIndex_.store(kDefaultGainIndex, std::memory_order_relaxed);
    lnaAgc_.store(false, std::memory_order_relaxed);
    mixerAgc_.store(false, std::memory_order_relaxed);
    linearityIndex_.store(kDefaultTableIndex, std::memory_order_relaxed);
    sensitivityIndex_.store(kDefaultTableIndex, std::memory_order_relaxed);
    decimation_.store(1, std::memory_order_relaxed);
    link_->decimation.store(1, std::memory_order_relaxed);
    hardwareRateHz_.store(rates_[startRate], std::memory_order_relaxed);
    sampleRateHz_.store(rates_[startRate], std::memory_order_relaxed);
    centerFrequencyHz_.store(100.0e6, std::memory_order_relaxed);

    std::string label = model_;
    if (!info.serial.empty()) { label += " (serial " + info.serial + ")"; }
    setName(std::string(profile_.namePrefix) + label);
    openMirror_.store(true, std::memory_order_relaxed);

    std::string rateList;
    for (const double r : rates_) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s%.3f", rateList.empty() ? "" : ", ", r / 1e6);
        rateList += buf;
    }
    // TWO LINES, not one. A diagnostic line is cut at DiagLog::kLineBytes
    // (192), and the single line this used to be lost its whole rate list off
    // the end on a real R2 - the first field report's log stops at "serial
    // ...26a464dc28593e93, " - which is exactly the part that says whether
    // GET_SAMPLERATES answered.
    core::diagLogf("%s: opened %s - board id %u, firmware \"%s\", serial %s", profile_.driverKey,
                   label.c_str(), static_cast<unsigned>(boardId_), firmwareVersion_.c_str(),
                   partIdSerialNo_.c_str());
    core::diagLogf("%s: rates %s MS/s complex (packed 12-bit, %.3f MS/s at the ADC)",
                   profile_.driverKey, rateList.c_str(), rates_[startRate] * 2.0 / 1e6);
    return true;
}

void AirspySource::closeDevice() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
    if (dev_ != nullptr) {
        // Leave the radio quiet and unpowered on the antenna port: the next
        // application to open it inherits whatever we leave behind, exactly as
        // we inherited what came before.
        programBiasTLocked(false);
    }
    // AN ABANDONED READER STILL HOLDS link_->dev AND IS STILL USING IT.
    // stopStreamingLocked releases dev_ without clearing the link's copy,
    // deliberately, so a stranded thread has a live object to be inside;
    // clearing it here anyway would be a data race with that thread's very
    // next readBulk - and, on the iteration where it has just passed its
    // `run` check, a null dereference. dev_ null with link_->dev set is the
    // fingerprint of that state and of nothing else. Same guard as
    // HackRfSource::closeDevice and Rx888Source::closeDevice.
    const bool abandoned = dev_ == nullptr && link_->dev != nullptr;
    dev_.reset();
    if (!abandoned) { link_->dev = nullptr; }
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    hardwareRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    packing_.store(false, std::memory_order_relaxed);
    portIndex_.store(0, std::memory_order_relaxed);
    setName(profile_.noDeviceName);
}

std::uint8_t AirspySource::boardId() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return boardId_;
}

std::string AirspySource::firmwareVersion() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return firmwareVersion_;
}

std::string AirspySource::partIdSerialNo() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return partIdSerialNo_;
}

std::string AirspySource::model() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return model_;
}

// --- streaming ------------------------------------------------------------

bool AirspySource::startStreamingLocked() {
    // ORDER MATTERS, AND IT IS libairspy's: airspy_start_rx (airspy.c:1192-1218
    // at airspyone_host fc61ab6) sends RECEIVER_MODE OFF, clears the halt on
    // 0x81, sends RECEIVER_MODE RX, and only then does create_io_threads
    // (:549-559) submit the bulk transfers.
    //
    // UP TO 0.99.31 THIS QUEUED THE RING FIRST, on the theory that a receiver
    // streaming into nothing overruns. That theory is wrong for this firmware
    // and the order it produced is what broke every real R2 in the field
    // (0.99.27/0.99.28 reports: opened fine, first bulk read "Windows error
    // 31", radio dead for the session). The firmware DISABLES bulk endpoint
    // 0x81 on every receiver-mode change and enables it again only for RX
    // (airspyone_firmware airspy_m0/airspy_rx.c set_receiver_mode ->
    // usb_streaming_disable -> common/usb.c usb_endpoint_disable; RX ->
    // usb_endpoint_init). Before RX, then, the endpoint is off, and a ring
    // queued against it gets no working answer: the host halts the pipe and
    // the first completion is ERROR_GEN_FAILURE. With RX first there is
    // nothing on the pipe while the endpoint is disabled, and the firmware
    // re-primes it before the host asks for anything.
    //
    // OFF first even though open() already sent it: this is also the restart
    // after a stop, and a radio a crashed session left in RX is exactly the
    // state the reference's own OFF exists to clear.
    if (!setReceiverModeLocked(airspy::ReceiverMode::Off, "quietening the receiver")) {
        return false;
    }
    // libusb_clear_halt's return is ignored by the reference too: on a pipe
    // that was never halted there is nothing to clear, and the transport
    // resets the pipe again as it queues the ring.
    dev_->resetPipe(airspy::kRxEndpoint);
    if (!setReceiverModeLocked(airspy::ReceiverMode::Rx, "starting the receiver")) {
        return false;
    }
    if (!dev_->beginBulkStream(airspy::kRxEndpoint, airspy::kPackedTransferBytes,
                               airspy::kTransferCount)) {
        const std::string why = dev_->lastError();
        // The receiver is in RX with nothing to take its samples. Switched off
        // again directly - controlOutLocked would refuse, because the fault
        // noted below makes the device dead - so the radio is not left
        // streaming into a host that is not listening.
        dev_->controlOut(cascade::usb::kRequestTypeVendorOut,
                         airspy::requestByte(airspy::VendorRequest::ReceiverMode),
                         static_cast<std::uint16_t>(airspy::ReceiverMode::Off), 0, nullptr, 0,
                         airspy::kControlTimeoutMs);
        noteTransportFault("queueing the sample transfers", why);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(link_->waitMutex);
        link_->exited = false;
    }
    link_->run.store(true, std::memory_order_relaxed);
    reader_ = std::thread(&AirspySource::readerThreadBody, link_);
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void AirspySource::stopStreamingLocked() {
    if (!reader_.joinable() && !running_.load(std::memory_order_relaxed)) {
        // Nothing to stop, but the receiver may still be in RX from a
        // half-failed start; endBulkStream is idempotent and cheap.
        if (dev_ != nullptr) { dev_->endBulkStream(); }
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    // THE READER IS TOLD FIRST, THEN THE RADIO. RECEIVER_MODE OFF disables
    // bulk endpoint 0x81 in the firmware (see startStreamingLocked) while this
    // driver's reads are still queued on it, and those reads FAIL. With the
    // flag lowered first the reader treats that as the stop it is; up to
    // 0.99.31 the flag came down after OFF had been acknowledged, and a read
    // failing in between was recorded as a dead radio - which then refused the
    // rate change that had stopped the stream. libairspy's airspy_stop_rx sets
    // stop_requested before it sends OFF (airspy.c:1220-1235) for this reason.
    //
    // Under rearmMutex, so a re-arm in progress on the reader either finishes
    // before this (and the OFF below then switches off what it restarted) or
    // sees the flag down and restarts nothing.
    {
        std::lock_guard<std::mutex> rk(link_->rearmMutex);
        link_->run.store(false, std::memory_order_relaxed);
    }
    link_->waitCv.notify_all();
    if (dev_ != nullptr && !deviceDead()) {
        setReceiverModeLocked(airspy::ReceiverMode::Off, "stopping the receiver");
    }

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
            exited = link_->waitCv.wait_for(lk, kReaderJoinWait, [this] { return link_->exited; });
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
            // Marked for LeakSanitizer, which a test that lets the zombie go
            // would otherwise see as a leak (core/leak_on_purpose.hpp).
            cascade::core::leakOnPurpose(dev_.release());
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

bool AirspySource::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("start() called with no Airspy open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running_.load(std::memory_order_relaxed)) { return true; }
    clearError();
    return startStreamingLocked();
}

void AirspySource::stop() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
}

// --- the reader thread ----------------------------------------------------

AirspySource::Rearm AirspySource::rearmStreamOn(ReaderLink& link, std::string& why) {
    cascade::usb::UsbDevice* dev = link.dev;
    const auto receiverMode = [dev](airspy::ReceiverMode m) {
        return dev->controlOut(cascade::usb::kRequestTypeVendorOut,
                               airspy::requestByte(airspy::VendorRequest::ReceiverMode),
                               static_cast<std::uint16_t>(m), 0, nullptr, 0,
                               airspy::kControlTimeoutMs) == 0;
    };

    // The old ring first. Safe here and nowhere else on this path: THIS is the
    // only thread that ever calls readBulk, so nothing can be inside the ring
    // that endBulkStream frees (usb_device.hpp's ordering rule).
    dev->endBulkStream();
    if (!link.run.load(std::memory_order_relaxed)) { return Rearm::Stopped; }

    // Then airspy_start_rx's three steps (airspy.c:1202-1210) and its
    // prepare_transfers (:559). Raw transport calls: the driver's *Locked
    // helpers want devMutex_, which the GUI thread may be holding while it
    // waits for this thread to leave.
    if (!receiverMode(airspy::ReceiverMode::Off)) {
        why = "receiver off: " + dev->lastError();
        return Rearm::Failed;
    }
    dev->resetPipe(airspy::kRxEndpoint);

    std::lock_guard<std::mutex> rk(link.rearmMutex);
    // stop() lowers `run` under this same lock, so from here to the end it
    // cannot slip in between the check and the RX: either it already has
    // (restart nothing) or it will send its OFF after we are done.
    if (!link.run.load(std::memory_order_relaxed)) { return Rearm::Stopped; }
    if (!receiverMode(airspy::ReceiverMode::Rx)) {
        why = "receiver on: " + dev->lastError();
        return Rearm::Failed;
    }
    if (!dev->beginBulkStream(airspy::kRxEndpoint, airspy::kPackedTransferBytes,
                              airspy::kTransferCount)) {
        why = "queueing the sample transfers: " + dev->lastError();
        receiverMode(airspy::ReceiverMode::Off);
        return Rearm::Failed;
    }
    return Rearm::Done;
}

void AirspySource::readerThreadBody(std::shared_ptr<ReaderLink> link) {
    std::vector<std::uint8_t> raw(airspy::kPackedTransferBytes);
    std::vector<std::uint16_t> codes(airspy::kRealSamplesPerTransfer);
    std::vector<float> reals(airspy::kRealSamplesPerTransfer);
    std::vector<std::complex<float>> conv(airspy::kComplexSamplesPerTransfer + 1);

    // THE CONVERTER IS THE READER'S OWN and is constructed fresh here, which
    // is what libairspy's iqconverter_float_reset at every start_rx
    // (airspy.c:1196) amounts to: the filter history and the DC average belong
    // to one stream, and carrying them across a stop would put a burst of the
    // previous session at the front of the new one. It also means nothing
    // outside this thread can touch the filter state.
    airspy::IqConverter cnv;

    // THE DECIMATOR, the reader's own for the same reason as the converter:
    // its filter history belongs to this stream. Configured once, here - the
    // factor only changes while the reader is stopped (setDecimation).
    airspy::PowerOfTwoDecimator decim;
    decim.configure(link->decimation.load(std::memory_order_relaxed));

    const unsigned timeoutMs = static_cast<unsigned>(kBulkReadWait.count());

    // The re-arm budget (kMaxStreamRearms) and when it was last spent.
    int rearms = 0;
    auto lastRearm = std::chrono::steady_clock::now();

    while (link->run.load(std::memory_order_relaxed)) {
        const int got = link->dev->readBulk(raw.data(), raw.size(), timeoutMs);
        if (!link->run.load(std::memory_order_relaxed)) { break; }

        if (got < 0) {
            // A FAILED READ IS A HALTED PIPE, which is not yet a dead radio.
            // Up to 0.99.31 this faulted on the spot - as libairspy's own
            // transfer callback gives up on the first incomplete transfer -
            // and a user whose R2 had hit one halt was told to unplug it. A
            // halt is cleared by the start sequence, so the reader runs it
            // again (rearmStreamOn), a bounded number of times. A radio that
            // really has gone fails the first control transfer of that and
            // faults at once, with the read as the cause.
            noteRead(*link, got, 0, false);
            const std::string detail = link->dev->lastError();
            if (rearms >= kMaxStreamRearms) {
                char tail[96];
                std::snprintf(tail, sizeof(tail), ", and again after %d restarts of the stream",
                              kMaxStreamRearms);
                noteTransportFaultOn(*link, "reading samples", detail + tail);
                break;
            }
            ++rearms;
            lastRearm = std::chrono::steady_clock::now();
            core::diagWarnf(
                "%s: %s; restarting the stream the way libairspy starts it - receiver off, "
                "clear the halt, receiver on (attempt %d of %d)",
                link->tag, detail.c_str(), rearms, kMaxStreamRearms);
            std::string why;
            const Rearm r = rearmStreamOn(*link, why);
            if (r == Rearm::Stopped) { break; }
            if (r == Rearm::Failed) {
                noteTransportFaultOn(*link, "reading samples",
                                     detail + "; restarting the stream failed: " + why);
                break;
            }
            // A fresh stream, so a fresh converter - the same reset libairspy
            // makes at every airspy_start_rx (airspy.c:1196). The filter
            // history belongs to the samples before the break.
            cnv.reset();
            decim.reset();
            continue;
        }
        if (got > 0 && rearms > 0 &&
            std::chrono::steady_clock::now() - lastRearm >= kRearmForgiveAfter) {
            rearms = 0;
        }

        std::size_t samples = 0;
        bool dropped = false;
        if (got > 0) {
            const std::size_t realCount = airspy::unpackSamples(
                raw.data(), static_cast<std::size_t>(got), codes.data(), codes.size());
            for (std::size_t i = 0; i < realCount; ++i) {
                reals[i] = airspy::sampleToFloat(codes[i]);
            }
            samples = cnv.process(reals.data(), realCount, conv.data());
            if (decim.factor() > 1) { samples = decim.process(conv.data(), samples, conv.data()); }
            if (samples > 0) {
                const std::size_t written = link->ring.write(conv.data(), samples);
                if (written != samples) {
                    // The host fell behind, not the radio. Counted rather than
                    // silently tolerated: a spectrum with a gap in it and no
                    // number anywhere is how a slow machine looks exactly like
                    // a broken one.
                    dropped = true;
                    link->dropped.fetch_add(1, std::memory_order_relaxed);
                }
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

void AirspySource::noteRead(ReaderLink& link, int ret, std::size_t samples, bool dropped) {
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

        const bool nominal = h.timeouts == 0 && h.overflows == 0 && h.errors == 0 &&
                             h.longestGapMs < 250;
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

std::string AirspySource::healthLineLocked(ReaderLink& link) {
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
    // open to parse the line, and a report from a native Airspy should be
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

std::string AirspySource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void AirspySource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

std::uint64_t AirspySource::droppedTransfers() const {
    return link_->dropped.load(std::memory_order_relaxed);
}

unsigned long long AirspySource::readersAbandoned() {
    return g_readersAbandoned.load(std::memory_order_relaxed);
}

bool AirspySource::linkHoldsDeviceForTest() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return link_->dev != nullptr;
}

// --- read -----------------------------------------------------------------

std::size_t AirspySource::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) { return 0; }
    std::size_t got = link_->ring.read(dst, n);
    if (got > 0) { return got; }
    if (faulted()) { return 0; }
    {
        // Bounded, and short. The pipeline's self-paced loop treats a zero as
        // "nothing yet" and backs off a millisecond of its own, so there is
        // nothing to gain from waiting longer than one chunk period here.
        std::unique_lock<std::mutex> lk(link_->waitMutex);
        link_->waitCv.wait_for(lk, kReadWait, [this] {
            return link_->ring.size() > 0 || !link_->run.load(std::memory_order_relaxed);
        });
    }
    got = link_->ring.read(dst, n);
    return got;
}

// --- rate, frequency, gains ----------------------------------------------

std::vector<double> AirspySource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    // THE RATES THIS SOURCE DELIVERS, which with decimation on are the
    // board's own divided by the factor - the only numbers sampleRateHz() can
    // ever read back, and the ones the Rate combo must offer. Whole hertz by
    // construction (decimationChoicesLocked).
    const double d = static_cast<double>(decimation_.load(std::memory_order_relaxed));
    std::vector<double> out;
    out.reserve(rates_.size());
    for (const double r : rates_) { out.push_back(r / d); }
    return out;
}

std::vector<unsigned> AirspySource::decimationChoicesLocked() const {
    std::vector<unsigned> out;
    for (const unsigned d : airspy::kDecimations) {
        bool whole = !rates_.empty();
        for (const double r : rates_) {
            const double q = r / static_cast<double>(d);
            if (q != std::floor(q)) { whole = false; }
        }
        if (whole || d == 1) { out.push_back(d); }
    }
    return out;
}

std::vector<unsigned> AirspySource::decimationChoices() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return decimationChoicesLocked();
}

bool AirspySource::setDecimation(unsigned factor) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setDecimation"));
        return false;
    }
    if (deviceDead()) { return false; }
    const std::vector<unsigned> choices = decimationChoicesLocked();
    if (std::find(choices.begin(), choices.end(), factor) == choices.end()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "the %s cannot decimate by %u here: it takes 1, 2, 4 ... up to %u, where "
                      "every sample rate it lists stays a whole number of hertz",
                      profile_.productName, factor, choices.empty() ? 1u : choices.back());
        setError(buf);
        return false;
    }
    if (factor == decimation_.load(std::memory_order_relaxed)) { return true; }
    // A QUIET RADIO for the change, exactly as a rate change: the reader owns
    // the decimator and builds it when it starts, so it is stopped, the factor
    // changed, and started again. Nothing is sent to the radio - decimation is
    // this end's arithmetic - but a reader running across the change would
    // hand the ring samples at two rates.
    const bool wasRunning = running_.load(std::memory_order_relaxed);
    if (wasRunning) { stopStreamingLocked(); }
    decimation_.store(factor, std::memory_order_relaxed);
    link_->decimation.store(factor, std::memory_order_relaxed);
    sampleRateHz_.store(hardwareRateHz_.load(std::memory_order_relaxed) / factor,
                        std::memory_order_relaxed);
    if (wasRunning) { return startStreamingLocked(); }
    return true;
}

bool AirspySource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    return setSampleRateHzLocked(hz, /*crossDecimation=*/false);
}

bool AirspySource::setSampleRateHzExplicit(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    return setSampleRateHzLocked(hz, /*crossDecimation=*/true);
}

bool AirspySource::setSampleRateHzLocked(double hz, bool crossDecimation) {
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setSampleRateHz"));
        return false;
    }
    if (deviceDead()) { return false; }
    if (rates_.empty()) {
        setError(std::string("this ") + profile_.productName +
                 " has not told us which sample rates it supports");
        return false;
    }
    if (!(hz > 0.0)) {  // negated compare so a NaN lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }

    // NEAREST, not refused: the hardware has a menu of two or three native
    // rates and every one of them is a long way from every other, so "the
    // closest thing I have" is the only answer that is ever useful.
    const unsigned currentDecimation = decimation_.load(std::memory_order_relaxed);
    std::size_t bestRate = 0;
    unsigned bestDecimation = currentDecimation;

    if (!crossDecimation) {
        // WITHIN THE CURRENT DECIMATION ONLY (0.99.44 repair) - this is the
        // generic path every IqSource shares (see the header: a Source-list
        // pick's fixed default, a fault-recovery reopen, a config restore),
        // and none of those is a caller asking for a specific rate on
        // purpose. A tie goes to the lower rate, which is the one that asks
        // less of the USB bus. Compared at the DELIVERED rate: with
        // decimation on, a request is for a rate after the divider (the Rate
        // combo lists those).
        const double d = static_cast<double>(currentDecimation);
        double bestGap = -1.0;
        for (std::size_t i = 0; i < rates_.size(); ++i) {
            const double gap = std::abs(rates_[i] / d - hz);
            if (bestGap < 0.0 || gap < bestGap) {
                bestGap = gap;
                bestRate = i;
            }
        }
        bestDecimation = currentDecimation;
    } else {
        // NEAREST OVER EVERY (NATIVE RATE, DECIMATION) PAIR, not just the
        // CURRENT decimation - a beta report found ADS-B decoding nothing on
        // an Airspy once decimation had been raised for something else: at
        // decimation 8 an R2's only two candidates were 10 MS/s/8 = 1.25 MS/s
        // and 2.5 MS/s/8 = 312.5 kS/s, and the plugin's 2.4 MS/s preset
        // landed on 1.25 MS/s - below its own 2 MS/s floor - because 2.5 MS/s
        // at decimation 1 was never considered. Every whole-hertz decimation
        // (decimationChoicesLocked) of every native rate is a candidate here,
        // and setDecimation() itself is untouched - a caller who wants a
        // specific decimation still gets exactly that by calling it directly.
        //
        // AT OR ABOVE WINS A TIE WITH BELOW: among candidates equally near
        // the request, one that delivers at least what was asked for is
        // preferred over one that delivers less - a decoder given LESS than
        // it asked for may refuse to run at all (see the 2 MS/s floor
        // above), where one given MORE just decodes a wider slice. Only when
        // nothing reaches the request does the nearest-below candidate
        // answer.
        //
        // A TIE IN DELIVERED RATE GOES TO THE CURRENT DECIMATION FIRST, then
        // to the lower one. The same delivered rate is often reachable two
        // ways (a 10 MS/s radio at /4 delivers the same 2.5 MS/s its 2.5
        // MS/s native rate does at /1), and picking whichever a caller's own
        // decimation already matches is what keeps a request that happens to
        // land exactly on the radio's current setting from silently changing
        // it. Failing that tie, the lower decimation is the one that asks
        // least of the USB bus and keeps the most bandwidth in reserve.
        const std::vector<unsigned> decimations = decimationChoicesLocked();
        // True if (gapA, decA) should win over (gapB, decB).
        const auto better = [currentDecimation](double gapA, unsigned decA, double gapB,
                                                unsigned decB) {
            if (gapA != gapB) { return gapA < gapB; }
            if ((decA == currentDecimation) != (decB == currentDecimation)) {
                return decA == currentDecimation;
            }
            return decA < decB;
        };
        double bestAboveGap = -1.0;
        std::size_t bestAboveRate = 0;
        unsigned bestAboveDecimation = 1;
        double bestBelowGap = -1.0;
        std::size_t bestBelowRate = 0;
        unsigned bestBelowDecimation = 1;
        for (std::size_t i = 0; i < rates_.size(); ++i) {
            for (const unsigned dec : decimations) {
                const double delivered = rates_[i] / static_cast<double>(dec);
                const double gap = std::abs(delivered - hz);
                if (delivered >= hz) {
                    if (bestAboveGap < 0.0 ||
                        better(gap, dec, bestAboveGap, bestAboveDecimation)) {
                        bestAboveGap = gap;
                        bestAboveRate = i;
                        bestAboveDecimation = dec;
                    }
                } else {
                    if (bestBelowGap < 0.0 ||
                        better(gap, dec, bestBelowGap, bestBelowDecimation)) {
                        bestBelowGap = gap;
                        bestBelowRate = i;
                        bestBelowDecimation = dec;
                    }
                }
            }
        }
        if (bestAboveGap >= 0.0) {
            bestRate = bestAboveRate;
            bestDecimation = bestAboveDecimation;
        } else {
            bestRate = bestBelowRate;
            bestDecimation = bestBelowDecimation;
        }
    }

    const bool wasRunning = running_.load(std::memory_order_relaxed);
    if (wasRunning) {
        // A QUIET RADIO for the change (see the header): the receiver is
        // switched off, our reader is joined and the bulk ring torn down
        // before the clock underneath it moves.
        stopStreamingLocked();
    }
    if (!programRateIndexLocked(rateIndex_[bestRate], "setting the sample rate")) {
        if (wasRunning && !deviceDead()) { startStreamingLocked(); }
        return false;
    }
    // The decimator is this end's arithmetic, same as setDecimation() (see
    // its own comment) - nothing more is sent to the radio for it.
    decimation_.store(bestDecimation, std::memory_order_relaxed);
    link_->decimation.store(bestDecimation, std::memory_order_relaxed);
    hardwareRateHz_.store(rates_[bestRate], std::memory_order_relaxed);
    sampleRateHz_.store(rates_[bestRate] / static_cast<double>(bestDecimation),
                        std::memory_order_relaxed);
    if (wasRunning) { return startStreamingLocked(); }
    return true;
}

bool AirspySource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setCenterFrequencyHz"));
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz >= profile_.minFrequencyHz && hz <= profile_.maxFrequencyHz)) {
        char buf[176];
        std::snprintf(buf, sizeof(buf),
                      "the %s tunes %.0f MHz to %.0f MHz; %.6f MHz is outside that",
                      profile_.productName, profile_.minFrequencyHz / 1e6,
                      profile_.maxFrequencyHz / 1e6, hz / 1e6);
        setError(buf);
        return false;
    }
    if (!programFrequencyLocked(hz, "setting the centre frequency")) { return false; }
    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    return true;
}

bool AirspySource::frequencyRangeHz(double& loHz, double& hiHz) const {
    loHz = profile_.minFrequencyHz;
    hiHz = profile_.maxFrequencyHz;
    return true;
}

std::vector<GainInfo> AirspySource::gains() const {
    // The units are the hardware's register steps, not decibels - see the
    // header. Step 1 everywhere, because every one of these is an index, and
    // GainUnit::Steps on every one of them so the panel, the deck and the
    // browser print "LNA 7" rather than "LNA 7.0 dB": the figure is a
    // register position, and nothing in the world measured it in decibels.
    //
    // ONLY THE CHOSEN MODE'S (see the header): a slider for a mode that is
    // not in use is a control whose number describes nothing on the radio.
    switch (gainMode()) {
        case GainMode::Linearity:
            return {GainInfo{"LINEARITY", 0.0, static_cast<double>(airspy::kCombinedMaxIndex), 1.0,
                             GainUnit::Steps}};
        case GainMode::Sensitivity:
            return {GainInfo{"SENSITIVITY", 0.0, static_cast<double>(airspy::kCombinedMaxIndex),
                             1.0, GainUnit::Steps}};
        case GainMode::Free:
        default:
            return {
                GainInfo{"LNA", 0.0, static_cast<double>(airspy::kLnaMaxIndex), 1.0,
                         GainUnit::Steps},
                GainInfo{"MIXER", 0.0, static_cast<double>(airspy::kMixerMaxIndex), 1.0,
                         GainUnit::Steps},
                GainInfo{"VGA", 0.0, static_cast<double>(airspy::kVgaMaxIndex), 1.0,
                         GainUnit::Steps},
            };
    }
}

bool AirspySource::setGainState(const GainState& st) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setGainState"));
        return false;
    }
    if (deviceDead()) { return false; }
    linearityIndex_.store(std::clamp(st.linearity, 0, airspy::kCombinedMaxIndex),
                          std::memory_order_relaxed);
    sensitivityIndex_.store(std::clamp(st.sensitivity, 0, airspy::kCombinedMaxIndex),
                            std::memory_order_relaxed);
    lnaIndex_.store(std::clamp(st.lna, 0, airspy::kLnaMaxIndex), std::memory_order_relaxed);
    mixerIndex_.store(std::clamp(st.mixer, 0, airspy::kMixerMaxIndex), std::memory_order_relaxed);
    vgaIndex_.store(std::clamp(st.vga, 0, airspy::kVgaMaxIndex), std::memory_order_relaxed);
    lnaAgc_.store(st.lnaAgc, std::memory_order_relaxed);
    mixerAgc_.store(st.mixerAgc, std::memory_order_relaxed);
    return applyGainModeLocked(st.mode);
}

AirspySource::GainState AirspySource::gainState() const {
    GainState st;
    st.mode = gainMode();
    st.linearity = linearityIndex_.load(std::memory_order_relaxed);
    st.sensitivity = sensitivityIndex_.load(std::memory_order_relaxed);
    st.lna = lnaIndex_.load(std::memory_order_relaxed);
    st.mixer = mixerIndex_.load(std::memory_order_relaxed);
    st.vga = vgaIndex_.load(std::memory_order_relaxed);
    st.lnaAgc = lnaAgc();
    st.mixerAgc = mixerAgc();
    return st;
}

bool AirspySource::setGainMode(GainMode mode) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setGainMode"));
        return false;
    }
    if (deviceDead()) { return false; }
    return applyGainModeLocked(mode);
}

bool AirspySource::setGainDb(const std::string& gainName, double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setGainDb"));
        return false;
    }
    if (deviceDead()) { return false; }
    // Rounded to nearest rather than truncated: a slider that reports 7.6 and
    // programs 7 is a control that lies by a whole step at every position.
    const int index = static_cast<int>(std::lround(db));
    if (gainName == "LINEARITY") { return programCombinedLocked(index, true); }
    if (gainName == "SENSITIVITY") { return programCombinedLocked(index, false); }

    // A FREE-MODE STAGE. Stored (clamped at both ends, device_source.hpp's
    // contract), then either the whole of Free mode is put on the radio -
    // when this is what switches to it - or just this stage, unless its own
    // AGC is driving it, in which case the value waits for the AGC to go off.
    const bool fromOtherMode = gainMode() != GainMode::Free;
    if (gainName == "LNA") {
        lnaIndex_.store(std::clamp(index, 0, airspy::kLnaMaxIndex), std::memory_order_relaxed);
        if (fromOtherMode) { return programFreeLocked(); }
        return lnaAgc() || programLnaLocked(lnaIndex_.load(std::memory_order_relaxed));
    }
    if (gainName == "MIXER") {
        mixerIndex_.store(std::clamp(index, 0, airspy::kMixerMaxIndex), std::memory_order_relaxed);
        if (fromOtherMode) { return programFreeLocked(); }
        return mixerAgc() || programMixerLocked(mixerIndex_.load(std::memory_order_relaxed));
    }
    if (gainName == "VGA") {
        vgaIndex_.store(std::clamp(index, 0, airspy::kVgaMaxIndex), std::memory_order_relaxed);
        if (fromOtherMode) { return programFreeLocked(); }
        return programVgaLocked(vgaIndex_.load(std::memory_order_relaxed));
    }
    setError(std::string("the ") + profile_.productName + " has no gain called \"" + gainName +
             "\"");
    return false;
}

double AirspySource::gainDb(const std::string& gainName) const {
    // Each mode's OWN value, whichever mode is in use: LNA / MIXER / VGA are
    // Free mode's manual settings, never what a table last wrote.
    if (gainName == "LNA") { return static_cast<double>(lnaIndex_.load(std::memory_order_relaxed)); }
    if (gainName == "MIXER") {
        return static_cast<double>(mixerIndex_.load(std::memory_order_relaxed));
    }
    if (gainName == "VGA") { return static_cast<double>(vgaIndex_.load(std::memory_order_relaxed)); }
    if (gainName == "LINEARITY") {
        return static_cast<double>(linearityIndex_.load(std::memory_order_relaxed));
    }
    if (gainName == "SENSITIVITY") {
        return static_cast<double>(sensitivityIndex_.load(std::memory_order_relaxed));
    }
    return 0.0;
}

bool AirspySource::setLnaAgc(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setLnaAgc"));
        return false;
    }
    if (deviceDead()) { return false; }
    lnaAgc_.store(on, std::memory_order_relaxed);
    if (gainMode() != GainMode::Free) { return programFreeLocked(); }
    // Off hands the stage back to its manual value, which was not sent while
    // the AGC had it.
    return programLnaAgcLocked(on) &&
           (on || programLnaLocked(lnaIndex_.load(std::memory_order_relaxed)));
}

bool AirspySource::setMixerAgc(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setMixerAgc"));
        return false;
    }
    if (deviceDead()) { return false; }
    mixerAgc_.store(on, std::memory_order_relaxed);
    if (gainMode() != GainMode::Free) { return programFreeLocked(); }
    return programMixerAgcLocked(on) &&
           (on || programMixerLocked(mixerIndex_.load(std::memory_order_relaxed)));
}

bool AirspySource::setAutoGain(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setAutoGain"));
        return false;
    }
    if (deviceDead()) { return false; }
    // BOTH AGCs, in the reference's own order (mixer then LNA, airspy.c:1840
    // and :1844). Half an AGC is a configuration the generic switch cannot
    // describe. ON is Free mode - neither table has an AGC - so from a table
    // mode the whole of Free mode goes on the radio; OFF leaves the mode as
    // it is and, in Free mode, hands both stages back to their manual values.
    mixerAgc_.store(on, std::memory_order_relaxed);
    lnaAgc_.store(on, std::memory_order_relaxed);
    if (gainMode() != GainMode::Free) { return on ? programFreeLocked() : true; }
    if (!programMixerAgcLocked(on) || !programLnaAgcLocked(on)) { return false; }
    if (!on) {
        return programMixerLocked(mixerIndex_.load(std::memory_order_relaxed)) &&
               programLnaLocked(lnaIndex_.load(std::memory_order_relaxed));
    }
    return true;
}

std::vector<std::string> AirspySource::antennas() const {
    return std::vector<std::string>(profile_.ports, profile_.ports + profile_.portCount);
}

std::string AirspySource::antenna() const {
    const std::size_t i = portIndex_.load(std::memory_order_relaxed);
    return profile_.ports[i < profile_.portCount ? i : 0];
}

bool AirspySource::setAntenna(const std::string& antennaName) {
    std::size_t want = profile_.portCount;
    for (std::size_t i = 0; i < profile_.portCount; ++i) {
        if (antennaName == profile_.ports[i]) { want = i; }
    }
    if (want == profile_.portCount) {
        if (profile_.portCount == 1) {
            setError(std::string("the ") + profile_.productName + " has one receive port, \"" +
                     profile_.ports[0] + "\"");
        } else {
            std::string names;
            for (std::size_t i = 0; i < profile_.portCount; ++i) {
                names += std::string(i == 0 ? "" : ", ") + "\"" + profile_.ports[i] + "\"";
            }
            setError(std::string("the ") + profile_.productName + " has no receive port \"" +
                     antennaName + "\"; it has " + names);
        }
        return false;
    }
    // ONE PORT: nothing to send, and nothing to have gone wrong - the answer
    // was always just the name (and still works with no radio open).
    if (profile_.portCount == 1) { return true; }
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setAntenna"));
        return false;
    }
    if (deviceDead()) { return false; }
    if (want == portIndex_.load(std::memory_order_relaxed)) { return true; }
    return programRfPortLocked(want, "selecting the receive port");
}

bool AirspySource::setBiasT(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError(noDeviceSentence("setBiasT"));
        return false;
    }
    if (deviceDead()) { return false; }
    return programBiasTLocked(on);
}

}  // namespace cascade::source
