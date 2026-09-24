// The Airspy driver itself. See airspy_source.hpp for the threading and
// ownership argument; the protocol numbers, the packing and the real-to-complex
// conversion are in airspy_protocol.hpp, which is where the libairspy
// attribution lives.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/airspy_source.hpp"

#include "core/diag_log.hpp"

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
    std::vector<NativeDeviceInfo> out;
    int index = 0;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid != airspy::kUsbVid || d.pid != airspy::kUsbPid) { continue; }
        NativeDeviceInfo info;
        info.driver = "airspy";
        // The bus-reported description is the only thing at enumeration time
        // that can tell a Mini from an R2 (see airspyModelFrom in the header
        // for why the board id cannot). When it is empty - which happens on a
        // devnode whose product string never got cached - the label says
        // "Airspy" and open() corrects it from the firmware version string.
        info.label = airspyModelFrom(d.description);
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

AirspySource::~AirspySource() { closeDevice(); }

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
    core::diagWarnf("airspy: transfer failed while %s%s%s", what, detail.empty() ? "" : ": ",
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
        setError(std::string("no Airspy is open (") + what + ")");
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
        setError(std::string("no Airspy is open (") + what + ")");
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
        // libairspy airspy.c:902-906, verbatim: an R2's two rates, in the
        // firmware's own order. Said out loud, because a receiver quietly
        // offering rates it was never told about is a lie the spectrum would
        // not reveal.
        core::diagWarnf(
            "airspy: the firmware did not answer GET_SAMPLERATES; falling back to libairspy's "
            "own default list (10 and 2.5 MS/s), which is right for an R2 and may not be for "
            "this board");
        raw = {10000000u, 2500000u};
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
    // of little-endian Hz.
    std::uint8_t payload[airspy::kFreqPayloadBytes];
    airspy::encodeFreq(static_cast<std::uint32_t>(hz + 0.5), payload);
    return controlOutLocked(airspy::VendorRequest::SetFreq, 0, 0, payload, sizeof(payload), what);
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
    lnaIndex_.store(v, std::memory_order_relaxed);
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
    mixerIndex_.store(v, std::memory_order_relaxed);
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
    vgaIndex_.store(v, std::memory_order_relaxed);
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
    autoGain_.store(false, std::memory_order_relaxed);
    if (!programVgaLocked(g.vga) || !programMixerLocked(g.mixer) || !programLnaLocked(g.lna)) {
        return false;
    }
    const int clamped = std::clamp(index, 0, airspy::kCombinedMaxIndex);
    // Only one of the two curves describes the radio at a time: setting
    // linearity makes the last sensitivity reading meaningless, and a panel
    // showing both would be showing one number that is no longer true.
    if (linearity) {
        linearityIndex_.store(clamped, std::memory_order_relaxed);
        sensitivityIndex_.store(-1, std::memory_order_relaxed);
    } else {
        sensitivityIndex_.store(clamped, std::memory_order_relaxed);
        linearityIndex_.store(-1, std::memory_order_relaxed);
    }
    return true;
}

bool AirspySource::programBiasTLocked(bool on) {
    // libairspy airspy.c:1897-1900 -> airspy_gpio_write (:1357-1382): an OUT,
    // request GPIO_WRITE, the state in the VALUE word and (port << 5) | pin in
    // the INDEX word. See airspy_protocol.hpp's kBiasTPortPin for why this is
    // not request 20.
    if (!controlOutLocked(airspy::VendorRequest::GpioWrite, on ? 1 : 0, airspy::kBiasTPortPin,
                          nullptr, 0, "switching the bias tee")) {
        return false;
    }
    biasT_.store(on, std::memory_order_relaxed);
    return true;
}

// --- open / close ---------------------------------------------------------

bool AirspySource::resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                                 std::string& error) {
    std::vector<cascade::usb::UsbDeviceInfo> devices;
    if (useFakeTransport_) {
        devices = fakeDevices_;
    } else {
        devices = cascade::usb::enumerateWinUsb(airspyUsbIds());
    }
    // Keep only Airspys: a caller may hand us a list from a wider scan, and
    // opening somebody else's dongle with Airspy vendor requests would be
    // worse than finding nothing.
    std::vector<cascade::usb::UsbDeviceInfo> ours;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid == airspy::kUsbVid && d.pid == airspy::kUsbPid) { ours.push_back(d); }
    }
    if (ours.empty()) {
        error = "no Airspy found (is it plugged in, and bound to WinUSB?)";
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
        error = "no Airspy with serial " + serial + " is connected";
        return false;
    }

    const std::string indexText = argValue(args, "index");
    std::size_t index = 0;
    if (!indexText.empty()) {
        char* end = nullptr;
        const long n = std::strtol(indexText.c_str(), &end, 10);
        if (end == indexText.c_str() || *end != '\0' || n < 0 ||
            static_cast<std::size_t>(n) >= ours.size()) {
            error = "there is no Airspy at index " + indexText;
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
        setError("this Airspy source already has a device open");
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
        setError(error.empty() ? std::string("could not open the Airspy") : error);
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
        if (moved != 1) { setError("the Airspy did not answer its board id"); }
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

    // airspy.h:108-111: two part-id words then four serial words.
    constexpr std::size_t kPartIdSerialNoBytes = 24;
    std::uint8_t partSerial[kPartIdSerialNoBytes] = {0};
    if (!controlInLocked(airspy::VendorRequest::BoardPartIdSerialNoRead, 0, 0, partSerial,
                         sizeof(partSerial), "reading the part id and serial number", &moved) ||
        moved != kPartIdSerialNoBytes) {
        if (moved != kPartIdSerialNoBytes) {
            setError("the Airspy answered a short part id / serial number");
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
    model_ = airspyModelFrom(info.description);
    if (model_ == "Airspy") { model_ = airspyModelFrom(firmwareVersion_); }

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
        programVgaLocked(kDefaultGainIndex) && programBiasTLocked(false);
    if (!configured) { return giveUp(); }
    sampleRateHz_.store(rates_[startRate], std::memory_order_relaxed);
    centerFrequencyHz_.store(100.0e6, std::memory_order_relaxed);
    autoGain_.store(false, std::memory_order_relaxed);

    std::string label = model_;
    if (!info.serial.empty()) { label += " (serial " + info.serial + ")"; }
    setName("Airspy: " + label);
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
    core::diagLogf("airspy: opened %s - board id %u, firmware \"%s\", serial %s", label.c_str(),
                   static_cast<unsigned>(boardId_), firmwareVersion_.c_str(),
                   partIdSerialNo_.c_str());
    core::diagLogf("airspy: rates %s MS/s complex (packed 12-bit, %.3f MS/s at the ADC)",
                   rateList.c_str(), rates_[startRate] * 2.0 / 1e6);
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
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    packing_.store(false, std::memory_order_relaxed);
    setName("Airspy: (no device)");
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
                "airspy: %s; restarting the stream the way libairspy starts it - receiver off, "
                "clear the halt, receiver on (attempt %d of %d)",
                detail.c_str(), rearms, kMaxStreamRearms);
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
    return rates_;
}

bool AirspySource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setSampleRateHz() called with no Airspy open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (rates_.empty()) {
        setError("this Airspy has not told us which sample rates it supports");
        return false;
    }
    if (!(hz > 0.0)) {  // negated compare so a NaN lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }

    // NEAREST, not nearest-below and not refused: the hardware has a menu of
    // two or three entries and every one of them is a long way from every
    // other, so "the closest thing I have" is the only answer that is ever
    // useful. A tie goes to the lower rate, which is the one that asks less of
    // the USB bus.
    std::size_t best = 0;
    double bestGap = -1.0;
    for (std::size_t i = 0; i < rates_.size(); ++i) {
        const double gap = std::abs(rates_[i] - hz);
        if (bestGap < 0.0 || gap < bestGap) {
            bestGap = gap;
            best = i;
        }
    }

    const bool wasRunning = running_.load(std::memory_order_relaxed);
    if (wasRunning) {
        // A QUIET RADIO for the change (see the header): the receiver is
        // switched off, our reader is joined and the bulk ring torn down
        // before the clock underneath it moves.
        stopStreamingLocked();
    }
    if (!programRateIndexLocked(rateIndex_[best], "setting the sample rate")) {
        if (wasRunning && !deviceDead()) { startStreamingLocked(); }
        return false;
    }
    sampleRateHz_.store(rates_[best], std::memory_order_relaxed);
    if (wasRunning) { return startStreamingLocked(); }
    return true;
}

bool AirspySource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setCenterFrequencyHz() called with no Airspy open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz >= airspy::kMinFrequencyHz && hz <= airspy::kMaxFrequencyHz)) {
        char buf[176];
        std::snprintf(buf, sizeof(buf),
                      "the Airspy tunes %.0f MHz to %.0f MHz; %.6f MHz is outside that",
                      airspy::kMinFrequencyHz / 1e6, airspy::kMaxFrequencyHz / 1e6, hz / 1e6);
        setError(buf);
        return false;
    }
    if (!programFrequencyLocked(hz, "setting the centre frequency")) { return false; }
    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    return true;
}

bool AirspySource::frequencyRangeHz(double& loHz, double& hiHz) const {
    loHz = airspy::kMinFrequencyHz;
    hiHz = airspy::kMaxFrequencyHz;
    return true;
}

std::vector<GainInfo> AirspySource::gains() const {
    // The units are the hardware's register steps, not decibels - see the
    // header. Step 1 everywhere, because every one of these is an index, and
    // GainUnit::Steps on every one of them so the panel, the deck and the
    // browser print "LNA 7" rather than "LNA 7.0 dB": the figure is a
    // register position, and nothing in the world measured it in decibels.
    return {
        GainInfo{"LNA", 0.0, static_cast<double>(airspy::kLnaMaxIndex), 1.0, GainUnit::Steps},
        GainInfo{"MIXER", 0.0, static_cast<double>(airspy::kMixerMaxIndex), 1.0, GainUnit::Steps},
        GainInfo{"VGA", 0.0, static_cast<double>(airspy::kVgaMaxIndex), 1.0, GainUnit::Steps},
        GainInfo{"LINEARITY", 0.0, static_cast<double>(airspy::kCombinedMaxIndex), 1.0,
                 GainUnit::Steps},
        GainInfo{"SENSITIVITY", 0.0, static_cast<double>(airspy::kCombinedMaxIndex), 1.0,
                 GainUnit::Steps},
    };
}

bool AirspySource::setGainDb(const std::string& gainName, double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setGainDb() called with no Airspy open");
        return false;
    }
    if (deviceDead()) { return false; }
    // Rounded to nearest rather than truncated: a slider that reports 7.6 and
    // programs 7 is a control that lies by a whole step at every position.
    const int index = static_cast<int>(std::lround(db));
    if (gainName == "LNA") { return programLnaLocked(index); }
    if (gainName == "MIXER") { return programMixerLocked(index); }
    if (gainName == "VGA") { return programVgaLocked(index); }
    if (gainName == "LINEARITY") { return programCombinedLocked(index, true); }
    if (gainName == "SENSITIVITY") { return programCombinedLocked(index, false); }
    setError("the Airspy has no gain called \"" + gainName + "\"");
    return false;
}

double AirspySource::gainDb(const std::string& gainName) const {
    if (gainName == "LNA") { return static_cast<double>(lnaIndex_.load(std::memory_order_relaxed)); }
    if (gainName == "MIXER") {
        return static_cast<double>(mixerIndex_.load(std::memory_order_relaxed));
    }
    if (gainName == "VGA") { return static_cast<double>(vgaIndex_.load(std::memory_order_relaxed)); }
    // The combined curves report -1 until one of them has been used and 0
    // again once the OTHER one has, because after a sensitivity change the
    // last linearity number describes a radio that no longer exists.
    if (gainName == "LINEARITY") {
        return static_cast<double>(linearityIndex_.load(std::memory_order_relaxed));
    }
    if (gainName == "SENSITIVITY") {
        return static_cast<double>(sensitivityIndex_.load(std::memory_order_relaxed));
    }
    return 0.0;
}

bool AirspySource::setAutoGain(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setAutoGain() called with no Airspy open");
        return false;
    }
    if (deviceDead()) { return false; }
    // BOTH AGCs, in the reference's own order (mixer then LNA, airspy.c:1840
    // and :1844). Half an AGC is a configuration nobody asked for and nothing
    // on screen could explain.
    if (!programMixerAgcLocked(on) || !programLnaAgcLocked(on)) { return false; }
    autoGain_.store(on, std::memory_order_relaxed);
    return true;
}

bool AirspySource::setAntenna(const std::string& antennaName) {
    if (antennaName == "RX") { return true; }
    setError("the Airspy has one receive port, \"RX\"");
    return false;
}

bool AirspySource::setBiasT(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setBiasT() called with no Airspy open");
        return false;
    }
    if (deviceDead()) { return false; }
    return programBiasTLocked(on);
}

}  // namespace cascade::source
