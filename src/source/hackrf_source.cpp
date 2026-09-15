// The HackRF driver itself. See hackrf_source.hpp for the threading and
// ownership argument; the protocol numbers and the arithmetic are in
// hackrf_protocol.hpp, which is where the libhackrf attribution lives.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/hackrf_source.hpp"

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

// The board name for a product id. libhackrf hackrf.c:202-205 and
// hackrf.h:655-671 (the usb_board_id enum's own comments).
const char* boardNameForPid(std::uint16_t pid) {
    switch (pid) {
        case hackrf::kPidHackRfOne: return "HackRF One";
        case hackrf::kPidJawbreaker: return "HackRF Jawbreaker";
        case hackrf::kPidRad1o: return "rad1o";
        default: return "HackRF";
    }
}

// The "key=value, key=value" parser this file used to carry privately now
// lives in source/device_source.hpp as cascade::source::argValue, because the
// Source section has to compare a SAVED SOAPY args string against a NATIVE
// row's args to decide whether they name the same physical radio
// (gui::preferNativeFor). Two parsers that agree today are two parsers that
// can disagree later, and the thing they would disagree about is which dongle
// the user gets. Same syntax, same trimming, same case-insensitive key.

// Hex of a byte range, for the part-id/serial readback. The firmware answers
// six little-endian uint32s; every HackRF tool prints them as eight hex digits
// each, in that order, so this driver's log line can be compared with theirs.
std::string hexWords(const std::uint8_t* data, std::size_t words) {
    std::string out;
    char buf[16];
    for (std::size_t w = 0; w < words; ++w) {
        std::uint32_t v = 0;
        for (std::size_t b = 0; b < 4; ++b) {
            v |= static_cast<std::uint32_t>(data[w * 4 + b]) << (8 * b);
        }
        std::snprintf(buf, sizeof(buf), "%08x", v);
        out += buf;
    }
    return out;
}

}  // namespace

// --- enumeration ----------------------------------------------------------

std::vector<cascade::usb::UsbId> hackRfUsbIds() {
    return {{hackrf::kUsbVid, hackrf::kPidHackRfOne},
            {hackrf::kUsbVid, hackrf::kPidJawbreaker},
            {hackrf::kUsbVid, hackrf::kPidRad1o}};
}

std::vector<NativeDeviceInfo> hackRfDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices) {
    std::vector<NativeDeviceInfo> out;
    int index = 0;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid != hackrf::kUsbVid) { continue; }
        if (d.pid != hackrf::kPidHackRfOne && d.pid != hackrf::kPidJawbreaker &&
            d.pid != hackrf::kPidRad1o) {
            continue;
        }
        NativeDeviceInfo info;
        info.driver = "hackrf";
        info.label = boardNameForPid(d.pid);
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

std::vector<NativeDeviceInfo> enumerateHackRf() {
#if defined(_WIN32)
    return hackRfDevicesFrom(cascade::usb::enumerateWinUsb(hackRfUsbIds()));
#else
    // The transport is WinUSB. A Linux HackRF is reached through SoapySDR
    // until a libusb backend exists behind cascade::usb::UsbDevice; saying so
    // once beats an empty list nobody can explain.
    static bool said = false;
    if (!said) {
        said = true;
        core::diagLogf(
            "hackrf: native enumeration is Windows-only in this build (the USB transport is "
            "WinUSB); use the SoapySDR path on this platform");
    }
    return {};
#endif
}

// --- construction ---------------------------------------------------------

HackRfSource::~HackRfSource() { closeDevice(); }

void HackRfSource::setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices,
                                       UsbOpenFn opener) {
    std::lock_guard<std::mutex> lk(devMutex_);
    fakeDevices_ = std::move(devices);
    fakeOpener_ = std::move(opener);
    useFakeTransport_ = static_cast<bool>(fakeOpener_);
}

// --- the error slot -------------------------------------------------------

void HackRfSource::setErrorOn(ReaderLink& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void HackRfSource::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void HackRfSource::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
}

void HackRfSource::noteTransportFaultOn(ReaderLink& link, const char* what,
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
    core::diagWarnf("hackrf: transfer failed while %s%s%s", what, detail.empty() ? "" : ": ",
                    detail.c_str());
}

void HackRfSource::noteTransportFault(const char* what, const std::string& detail) {
    noteTransportFaultOn(*link_, what, detail);
}

bool HackRfSource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool HackRfSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string HackRfSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deadWhat;
}

const char* HackRfSource::lastError() const {
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

void HackRfSource::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* HackRfSource::name() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        snapshot = name_;
    }
    return snapshot.c_str();
}

// --- control transfers ----------------------------------------------------

bool HackRfSource::controlOutLocked(hackrf::VendorRequest r, std::uint16_t value,
                                    std::uint16_t index, const std::uint8_t* data,
                                    std::size_t len, const char* what) {
    if (dev_ == nullptr) {
        setError(std::string("no HackRF is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const int ret = dev_->controlOut(cascade::usb::kRequestTypeVendorOut, hackrf::requestByte(r),
                                     value, index, data, len, hackrf::kControlTimeoutMs);
    if (ret < 0 || static_cast<std::size_t>(ret) != len) {
        noteTransportFault(what, dev_->lastError());
        return false;
    }
    return true;
}

bool HackRfSource::controlInLocked(hackrf::VendorRequest r, std::uint16_t value,
                                   std::uint16_t index, std::uint8_t* data, std::size_t len,
                                   const char* what, std::size_t* moved) {
    if (moved != nullptr) { *moved = 0; }
    if (dev_ == nullptr) {
        setError(std::string("no HackRF is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const int ret = dev_->controlIn(cascade::usb::kRequestTypeVendorIn, hackrf::requestByte(r),
                                    value, index, data, len, hackrf::kControlTimeoutMs);
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

bool HackRfSource::setTransceiverModeLocked(hackrf::TransceiverMode mode, const char* what) {
    // libhackrf hackrf.c:943-966: the mode is the VALUE word, index 0, no
    // payload.
    return controlOutLocked(hackrf::VendorRequest::SetTransceiverMode,
                            static_cast<std::uint16_t>(mode), 0, nullptr, 0, what);
}

bool HackRfSource::programRateLocked(const hackrf::RateSetting& r, const char* what) {
    // libhackrf hackrf.c:1879-1911, hackrf_set_sample_rate_manual: the
    // freq/divider payload, then ALWAYS the filter that goes with it.
    std::uint8_t payload[hackrf::kRatePayloadBytes];
    hackrf::encodeRate(r, payload);
    if (!controlOutLocked(hackrf::VendorRequest::SampleRateSet, 0, 0, payload, sizeof(payload),
                          what)) {
        return false;
    }
    const std::uint32_t bw = hackrf::basebandFilterForRate(r);
    return controlOutLocked(hackrf::VendorRequest::BasebandFilterBandwidthSet,
                            static_cast<std::uint16_t>(bw & 0xFFFFu),
                            static_cast<std::uint16_t>(bw >> 16), nullptr, 0,
                            "setting the baseband filter");
}

bool HackRfSource::programFrequencyLocked(double hz, const char* what) {
    // libhackrf hackrf.c:1775-1805: the MHz/Hz split as two little-endian
    // uint32s, value and index both zero.
    const hackrf::FreqSplit split =
        hackrf::splitFrequency(static_cast<std::uint64_t>(hz + 0.5));
    std::uint8_t payload[hackrf::kFreqPayloadBytes];
    hackrf::encodeFreq(split, payload);
    return controlOutLocked(hackrf::VendorRequest::SetFreq, 0, 0, payload, sizeof(payload), what);
}

bool HackRfSource::programLnaLocked(double db) {
    // libhackrf hackrf.c:2022-2046: the gain rides in the INDEX word and the
    // firmware answers one byte, zero meaning it refused.
    const std::uint32_t value = hackrf::roundLnaGainDb(db);
    std::uint8_t reply = 0;
    if (!controlInLocked(hackrf::VendorRequest::SetLnaGain, 0,
                         static_cast<std::uint16_t>(value), &reply, 1, "setting the LNA gain")) {
        return false;
    }
    if (reply == 0) {
        setError("the radio refused that LNA gain");
        return false;
    }
    lnaDb_.store(static_cast<double>(value), std::memory_order_relaxed);
    return true;
}

bool HackRfSource::programVgaLocked(double db) {
    // libhackrf hackrf.c:2049-2073, the same shape as the LNA.
    const std::uint32_t value = hackrf::roundVgaGainDb(db);
    std::uint8_t reply = 0;
    if (!controlInLocked(hackrf::VendorRequest::SetVgaGain, 0,
                         static_cast<std::uint16_t>(value), &reply, 1, "setting the VGA gain")) {
        return false;
    }
    if (reply == 0) {
        setError("the radio refused that VGA gain");
        return false;
    }
    vgaDb_.store(static_cast<double>(value), std::memory_order_relaxed);
    return true;
}

bool HackRfSource::programAmpLocked(bool on) {
    // libhackrf hackrf.c:1961-1981: on/off in the VALUE word, no payload.
    if (!controlOutLocked(hackrf::VendorRequest::AmpEnable, on ? 1 : 0, 0, nullptr, 0,
                          "switching the front-end amplifier")) {
        return false;
    }
    ampDb_.store(on ? hackrf::kAmpGainDb : 0.0, std::memory_order_relaxed);
    return true;
}

bool HackRfSource::programBiasTLocked(bool on) {
    // libhackrf hackrf.c:2102-2122, hackrf_set_antenna_enable.
    if (!controlOutLocked(hackrf::VendorRequest::AntennaEnable, on ? 1 : 0, 0, nullptr, 0,
                          "switching the bias-T")) {
        return false;
    }
    biasT_.store(on, std::memory_order_relaxed);
    return true;
}

// --- open / close ---------------------------------------------------------

bool HackRfSource::resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                                 std::string& error) {
    std::vector<cascade::usb::UsbDeviceInfo> devices;
    if (useFakeTransport_) {
        devices = fakeDevices_;
    } else {
#if defined(_WIN32)
        devices = cascade::usb::enumerateWinUsb(hackRfUsbIds());
#endif
    }
    // Keep only the HackRF family: a caller may hand us a list from a wider
    // scan, and opening somebody else's dongle with HackRF vendor requests
    // would be worse than finding nothing.
    std::vector<cascade::usb::UsbDeviceInfo> ours;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid == hackrf::kUsbVid &&
            (d.pid == hackrf::kPidHackRfOne || d.pid == hackrf::kPidJawbreaker ||
             d.pid == hackrf::kPidRad1o)) {
            ours.push_back(d);
        }
    }
    if (ours.empty()) {
        error = "no HackRF found (is it plugged in, and bound to WinUSB?)";
        return false;
    }

    const std::string serial = argValue(args, "serial");
    if (!serial.empty()) {
        const std::string want = lowered(serial);
        for (const cascade::usb::UsbDeviceInfo& d : ours) {
            const std::string have = lowered(d.serial);
            // A suffix match as well as an exact one: a HackRF's serial is 32
            // hex digits of which the first half is usually zeros, and the
            // number a user reads off a label or another tool's listing is the
            // tail of it.
            if (have == want || (have.size() >= want.size() &&
                                 have.compare(have.size() - want.size(), want.size(), want) == 0)) {
                out = d;
                return true;
            }
        }
        error = "no HackRF with serial " + serial + " is connected";
        return false;
    }

    const std::string indexText = argValue(args, "index");
    std::size_t index = 0;
    if (!indexText.empty()) {
        char* end = nullptr;
        const long n = std::strtol(indexText.c_str(), &end, 10);
        if (end == indexText.c_str() || *end != '\0' || n < 0 ||
            static_cast<std::size_t>(n) >= ours.size()) {
            error = "there is no HackRF at index " + indexText;
            return false;
        }
        index = static_cast<std::size_t>(n);
    }
    out = ours[index];
    return true;
}

bool HackRfSource::open(const std::string& args) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ != nullptr) {
        setError("this HackRF source already has a device open");
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
        error = "the native HackRF driver needs the WinUSB transport (Windows only in this build)";
#endif
    }
    if (dev == nullptr) {
        setError(error.empty() ? std::string("could not open the HackRF") : error);
        return false;
    }
    dev_ = std::move(dev);
    link_->dev = dev_.get();
    clearError();

    // --- who is this, and what is on it -------------------------------
    // Three reads, in libhackrf's own order (hackrf_info does the same
    // three), before anything is programmed: a device that cannot answer
    // these is not a device this driver should start configuring.
    std::uint8_t boardId = 0;
    std::size_t moved = 0;
    if (!controlInLocked(hackrf::VendorRequest::BoardIdRead, 0, 0, &boardId, 1,
                         "reading the board id", &moved) ||
        moved != 1) {
        if (moved != 1) { setError("the HackRF did not answer its board id"); }
        dev_.reset();
        link_->dev = nullptr;
        return false;
    }
    boardId_ = boardId;

    std::uint8_t version[hackrf::kVersionStringBytes + 1] = {0};
    if (!controlInLocked(hackrf::VendorRequest::VersionStringRead, 0, 0, version,
                         hackrf::kVersionStringBytes, "reading the firmware version", &moved)) {
        dev_.reset();
        link_->dev = nullptr;
        return false;
    }
    // Terminate at what the device ACTUALLY sent - the firmware writes no NUL
    // of its own, and trusting the zero-filled tail of our buffer would make a
    // short reply read as a long string on any future transport that reuses
    // its scratch space.
    if (moved > hackrf::kVersionStringBytes) { moved = hackrf::kVersionStringBytes; }
    version[moved] = 0;
    firmwareVersion_ = reinterpret_cast<const char*>(version);

    std::uint8_t partSerial[hackrf::kPartIdSerialNoBytes] = {0};
    if (!controlInLocked(hackrf::VendorRequest::BoardPartIdSerialNoRead, 0, 0, partSerial,
                         sizeof(partSerial), "reading the part id and serial number", &moved) ||
        moved != hackrf::kPartIdSerialNoBytes) {
        if (moved != hackrf::kPartIdSerialNoBytes) {
            setError("the HackRF answered a short part id / serial number");
        }
        dev_.reset();
        link_->dev = nullptr;
        return false;
    }
    // Words 2..5 are the MCU's unique id, which is what every HackRF tool
    // calls the serial number.
    partIdSerialNo_ = hexWords(partSerial + 8, 4);

    // --- a KNOWN state -------------------------------------------------
    // A HackRF keeps whatever the last application left it at, bias-T
    // included. Programming all of it here is what makes every readout on
    // this object true of the hardware from the first frame, rather than
    // true of whatever ran before us.
    const hackrf::RateSetting rate = hackrf::computeRate(10.0e6);
    const bool configured = programRateLocked(rate, "setting the sample rate") &&
                            programFrequencyLocked(100.0e6, "setting the centre frequency") &&
                            programLnaLocked(16.0) && programVgaLocked(16.0) &&
                            programAmpLocked(false) && programBiasTLocked(false);
    if (!configured) {
        dev_.reset();
        link_->dev = nullptr;
        return false;
    }
    sampleRateHz_.store(10.0e6, std::memory_order_relaxed);
    centerFrequencyHz_.store(100.0e6, std::memory_order_relaxed);

    std::string label = boardNameForPid(info.pid);
    if (!info.serial.empty()) { label += " (serial " + info.serial + ")"; }
    setName("HackRF: " + label);
    openMirror_.store(true, std::memory_order_relaxed);

    core::diagLogf("hackrf: opened %s - board id %u, firmware \"%s\", serial %s", label.c_str(),
                   static_cast<unsigned>(boardId_), firmwareVersion_.c_str(),
                   partIdSerialNo_.c_str());
    return true;
}

void HackRfSource::closeDevice() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
    if (dev_ != nullptr) {
        // Leave the radio quiet and unpowered on the antenna port: the next
        // application to open it inherits whatever we leave behind, exactly
        // as we inherited what came before.
        programBiasTLocked(false);
        programAmpLocked(false);
    }
    // AN ABANDONED READER STILL HOLDS link_->dev AND IS STILL USING IT.
    // stopStreamingLocked releases dev_ without clearing the link's copy,
    // deliberately, so a stranded thread has a live object to be inside;
    // clearing it here anyway would be a data race with that thread's very
    // next readBulk - and on the iteration where it has just passed its `run`
    // check and not yet dereferenced, a null one. The exact defect the
    // abandonment path exists to avoid, reintroduced two functions later.
    // dev_ being null with link_->dev set is what that state looks like;
    // Rx888Source::closeDevice has had the same guard since it was written.
    const bool abandoned = dev_ == nullptr && link_->dev != nullptr;
    dev_.reset();
    if (!abandoned) { link_->dev = nullptr; }
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    setName("HackRF: (no device)");
}

std::uint8_t HackRfSource::boardId() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return boardId_;
}

std::string HackRfSource::firmwareVersion() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return firmwareVersion_;
}

std::string HackRfSource::partIdSerialNo() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return partIdSerialNo_;
}

// --- streaming ------------------------------------------------------------

bool HackRfSource::startStreamingLocked() {
    // ORDER MATTERS. The bulk ring is queued FIRST: a transceiver told to
    // receive with nothing queued fills the firmware's own buffer and
    // overruns before the host's first read, which presents as a stream that
    // starts corrupted and then recovers - the hardest kind of fault to
    // attribute later.
    if (!dev_->beginBulkStream(hackrf::kRxEndpoint, hackrf::kTransferBufferBytes,
                               hackrf::kTransferCount)) {
        noteTransportFault("queueing the sample transfers", dev_->lastError());
        return false;
    }
    if (!setTransceiverModeLocked(hackrf::TransceiverMode::Receive, "starting the receiver")) {
        dev_->endBulkStream();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(link_->waitMutex);
        link_->exited = false;
    }
    link_->run.store(true, std::memory_order_relaxed);
    reader_ = std::thread(&HackRfSource::readerThreadBody, link_);
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void HackRfSource::stopStreamingLocked() {
    if (!reader_.joinable() && !running_.load(std::memory_order_relaxed)) {
        // Nothing to stop, but the transceiver may still be in RECEIVE from a
        // half-failed start; endBulkStream is idempotent and cheap.
        if (dev_ != nullptr) { dev_->endBulkStream(); }
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    if (dev_ != nullptr && !deviceDead()) {
        setTransceiverModeLocked(hackrf::TransceiverMode::Off, "stopping the receiver");
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

bool HackRfSource::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("start() called with no HackRF open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running_.load(std::memory_order_relaxed)) { return true; }
    clearError();
    return startStreamingLocked();
}

void HackRfSource::stop() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
}

// --- the reader thread ----------------------------------------------------

void HackRfSource::readerThreadBody(std::shared_ptr<ReaderLink> link) {
    const hackrf::SampleTable& lut = hackrf::sampleTable();
    std::vector<std::uint8_t> raw(hackrf::kTransferBufferBytes);
    std::vector<std::complex<float>> conv(hackrf::kSamplesPerTransfer);
    const unsigned timeoutMs = static_cast<unsigned>(kBulkReadWait.count());

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
        const std::size_t samples = static_cast<std::size_t>(got) / hackrf::kBytesPerSample;
        bool dropped = false;
        if (samples > 0) {
            for (std::size_t i = 0; i < samples; ++i) {
                conv[i] = std::complex<float>(lut.v[raw[2 * i]], lut.v[raw[2 * i + 1]]);
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

void HackRfSource::noteRead(ReaderLink& link, int ret, std::size_t samples, bool dropped) {
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
            const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - h.lastSamples).count();
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

std::string HackRfSource::healthLineLocked(ReaderLink& link) {
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
    // open to parse the line, and a report from a native HackRF should be
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

std::string HackRfSource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void HackRfSource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

std::uint64_t HackRfSource::droppedTransfers() const {
    return link_->dropped.load(std::memory_order_relaxed);
}

unsigned long long HackRfSource::readersAbandoned() {
    return g_readersAbandoned.load(std::memory_order_relaxed);
}

bool HackRfSource::linkHoldsDeviceForTest() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return link_->dev != nullptr;
}

// --- read -----------------------------------------------------------------

std::size_t HackRfSource::read(std::complex<float>* dst, std::size_t n) {
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

std::vector<double> HackRfSource::supportedSampleRatesHz() const {
    // A menu, not a constraint - see the header. These are the round numbers
    // HackRF applications offer; anything between 2 and 20 MS/s works.
    return {2.0e6, 4.0e6, 6.0e6, 8.0e6, 10.0e6, 12.5e6, 16.0e6, 20.0e6};
}

bool HackRfSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setSampleRateHz() called with no HackRF open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz > 0.0)) {  // negated compare so a NaN lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    if (hz < hackrf::kMinSampleRateHz) {
        // REFUSED, not raised. libhackrf documents 2-20 MS/s and says
        // anything outside it is unguaranteed; quietly giving a caller that
        // asked for 1 MS/s twice the bandwidth it wanted would be a lie
        // nothing on screen could reveal.
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "the HackRF's lowest sample rate is %.0f MS/s; %.3f MS/s was refused",
                      hackrf::kMinSampleRateHz / 1e6, hz / 1e6);
        setError(buf);
        return false;
    }
    // Above the top the rate IS coerced, because a caller asking for more
    // bandwidth than the radio has can be given all of it and told so.
    double want = hz;
    if (want > hackrf::kMaxSampleRateHz) {
        want = hackrf::kMaxSampleRateHz;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "the HackRF's highest sample rate is %.0f MS/s; %.3f MS/s was coerced to it",
                      hackrf::kMaxSampleRateHz / 1e6, hz / 1e6);
        setError(buf);
    }

    const hackrf::RateSetting rate = hackrf::computeRate(want);
    const bool wasRunning = running_.load(std::memory_order_relaxed);
    if (wasRunning) {
        // A QUIET RADIO for the change (see the header): the transceiver is
        // switched off, our reader is joined and the bulk ring torn down
        // before the clock underneath it moves.
        stopStreamingLocked();
    }
    if (!programRateLocked(rate, "setting the sample rate")) {
        if (wasRunning && !deviceDead()) { startStreamingLocked(); }
        return false;
    }
    const double actual =
        static_cast<double>(rate.freqHz) / static_cast<double>(rate.divider == 0 ? 1 : rate.divider);
    sampleRateHz_.store(actual, std::memory_order_relaxed);
    if (wasRunning) { return startStreamingLocked(); }
    return true;
}

bool HackRfSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setCenterFrequencyHz() called with no HackRF open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz >= hackrf::kMinFrequencyHz && hz <= hackrf::kMaxFrequencyHz)) {
        char buf[176];
        std::snprintf(buf, sizeof(buf),
                      "the HackRF tunes %.0f MHz to %.0f MHz; %.6f MHz is outside that",
                      hackrf::kMinFrequencyHz / 1e6, hackrf::kMaxFrequencyHz / 1e6, hz / 1e6);
        setError(buf);
        return false;
    }
    if (!programFrequencyLocked(hz, "setting the centre frequency")) { return false; }
    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    return true;
}

bool HackRfSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    loHz = hackrf::kMinFrequencyHz;
    hiHz = hackrf::kMaxFrequencyHz;
    return true;
}

std::vector<GainInfo> HackRfSource::gains() const {
    return {
        GainInfo{"LNA", hackrf::kLnaMinDb, hackrf::kLnaMaxDb, hackrf::kLnaStepDb},
        GainInfo{"VGA", hackrf::kVgaMinDb, hackrf::kVgaMaxDb, hackrf::kVgaStepDb},
        // The amplifier as a two-position gain: 0 or 14, step 14.
        GainInfo{"AMP", 0.0, hackrf::kAmpGainDb, hackrf::kAmpGainDb},
    };
}

bool HackRfSource::setGainDb(const std::string& gainName, double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setGainDb() called with no HackRF open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (gainName == "LNA") { return programLnaLocked(db); }
    if (gainName == "VGA") { return programVgaLocked(db); }
    if (gainName == "AMP") { return programAmpLocked(hackrf::ampOnForDb(db)); }
    setError("the HackRF has no gain called \"" + gainName + "\"");
    return false;
}

double HackRfSource::gainDb(const std::string& gainName) const {
    if (gainName == "LNA") { return lnaDb_.load(std::memory_order_relaxed); }
    if (gainName == "VGA") { return vgaDb_.load(std::memory_order_relaxed); }
    if (gainName == "AMP") { return ampDb_.load(std::memory_order_relaxed); }
    return 0.0;
}

bool HackRfSource::setAutoGain(bool on) {
    (void)on;
    setError("the HackRF has no automatic gain control");
    return false;
}

bool HackRfSource::setAntenna(const std::string& antennaName) {
    if (antennaName == "RX") { return true; }
    setError("the HackRF has one receive port, \"RX\"");
    return false;
}

bool HackRfSource::setBiasT(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setBiasT() called with no HackRF open");
        return false;
    }
    if (deviceDead()) { return false; }
    return programBiasTLocked(on);
}

}  // namespace cascade::source
