// The RX888 mk2 driver itself. See rx888_source.hpp for the threading,
// ownership and two-identities arguments; the protocol numbers and the
// arithmetic are in rx888_protocol.hpp, which is where the ExtIO_sddc
// attribution lives, and the firmware image and its format are in
// rx888_firmware.{hpp,cpp}.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rx888_source.hpp"

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/radio_events.hpp"
#include "core/health_events.hpp"
#include "core/leak_on_purpose.hpp"
#include "source/rx888_firmware.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace cascade::source {
namespace rre = ::cascade::core::radioreport;

namespace {

// Abandoned reader threads, process-wide. See readersAbandoned().
std::atomic<unsigned long long> g_readersAbandoned{0};

// Lower-cased copy, for the case-insensitive serial match.
std::string lowered(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool isSddcPid(std::uint16_t pid) {
    return pid == rx888::kPidBootloader || pid == rx888::kPidStreamer;
}

// The sentence BOTH transports report for a control transfer that ran out of
// time (winusb_device.cpp and usbfs_device.cpp; tests/test_winusb_abandon.cpp
// holds the first). The interface has no error code to ask instead.
bool isTimeoutText(const std::string& detail) {
    return detail.find("did not answer within its timeout") != std::string::npos;
}

// The firmware's own name for a request (ExtIO_sddc Interface.h, FX3Command),
// for the log: a line that says STARTADC can be read against the vendor's
// source without a table.
const char* requestName(std::uint8_t request) {
    switch (static_cast<rx888::Command>(request)) {
        case rx888::Command::StartFx3: return "STARTFX3";
        case rx888::Command::StopFx3: return "STOPFX3";
        case rx888::Command::TestFx3: return "TESTFX3";
        case rx888::Command::GpioFx3: return "GPIOFX3";
        case rx888::Command::ResetFx3: return "RESETFX3";
        case rx888::Command::StartAdc: return "STARTADC";
        case rx888::Command::TunerInit: return "TUNERINIT";
        case rx888::Command::TunerTune: return "TUNERTUNE";
        case rx888::Command::SetArgFx3: return "SETARGFX3";
        case rx888::Command::TunerStandby: return "TUNERSTDBY";
    }
    return "request";
}

// " after 3000 ms (GPIOFX3, bound 3000 ms)" - what a failed request's log
// line says beyond its name.
std::string timingText(std::uint8_t request, unsigned tookMs, unsigned boundMs) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), " after %u ms (%s, bound %u ms)", tookMs, requestName(request),
                  boundMs);
    return buf;
}

// WHAT THE LINK IS, from the transport's own description of the sample
// endpoint. A bulk endpoint's packet is 1024 bytes on a SuperSpeed link and
// 512 on a high-speed one (USB 3.x section 9.6.6, USB 2.0 section 5.8.3), and
// the SDDC firmware declares each for its own speed (SDDC_FX3/USBdescriptor.c),
// so the number tells a USB 3 port from a USB 2 one without asking the device
// anything. Empty when the transport said nothing.
std::string linkText(const std::string& layout) {
    if (layout.empty()) { return "USB layout not reported"; }
    const char* speed = "";
    if (layout.find("packet 1024") != std::string::npos) {
        speed = "USB 3 link (1024-byte bulk packets); ";
    } else if (layout.find("packet 512") != std::string::npos) {
        speed = "USB 2 LINK (512-byte bulk packets): cannot carry this radio's 128 MB/s; ";
    }
    return std::string(speed) + layout;
}

}  // namespace

// --- enumeration ----------------------------------------------------------

std::vector<cascade::usb::UsbId> rx888UsbIds() {
    return {{rx888::kUsbVid, rx888::kPidStreamer}, {rx888::kUsbVid, rx888::kPidBootloader}};
}

std::vector<NativeDeviceInfo> rx888DevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices) {
    std::vector<NativeDeviceInfo> out;
    int index = 0;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid != rx888::kUsbVid || !isSddcPid(d.pid)) { continue; }
        NativeDeviceInfo info;
        info.driver = "rx888";
        if (d.pid == rx888::kPidBootloader) {
            // SAID OUT LOUD RATHER THAN HIDDEN. A bootloader is what every
            // RX888 is after a power cycle, it cannot stream, and the remedy
            // is not something the user has to do - we load the image on
            // open. A row that just said "RX888" would be a row that takes
            // five seconds longer to open than the others for no stated
            // reason; a row that was missing would be the RTL-SDR's old
            // "your radio is not in the list" problem all over again.
            info.label = "RX888 (needs firmware, will load on open)";
        } else {
            // The model byte only exists once the firmware is running and is
            // only readable by OPENING the device, which enumeration may
            // never do (usb_device.hpp rule 1). So the label says what
            // SetupAPI knows - the product string the SDDC firmware
            // publishes, "RX888mk2" on a mk2 - and open() is where the model
            // is read and an unsupported one refused.
            info.label = d.description.empty() ? std::string("RX888") : d.description;
        }
        if (!d.serial.empty()) {
            info.label += " (serial " + d.serial + ")";
            info.args = "serial=" + d.serial;
        } else {
            info.args = "index=" + std::to_string(index);
        }
        out.push_back(std::move(info));
        ++index;
    }
    return out;
}

std::vector<NativeDeviceInfo> enumerateRx888() {
    // See hackrf_source.cpp's enumerateHackRf(): enumerateWinUsb() is the one
    // entry point on every platform (WinUSB on Windows, usbfs on Linux).
    return rx888DevicesFrom(cascade::usb::enumerateWinUsb(rx888UsbIds()));
}

// --- construction ---------------------------------------------------------

Rx888Source::~Rx888Source() { closeDevice(); }

void Rx888Source::setTransportForTest(UsbListFn lister, UsbOpenFn opener) {
    std::lock_guard<std::mutex> lk(devMutex_);
    fakeLister_ = std::move(lister);
    fakeOpener_ = std::move(opener);
    useFakeTransport_ = static_cast<bool>(fakeOpener_);
}

void Rx888Source::setClockForTest(ClockFn clock) {
    std::lock_guard<std::mutex> lk(devMutex_);
    clock_ = std::move(clock);
}

std::uint64_t Rx888Source::nowMs() const {
    if (clock_) { return clock_(); }
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::string Rx888Source::openReport() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return openReport_;
}

// --- the error slot -------------------------------------------------------

void Rx888Source::setErrorOn(ReaderLink& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void Rx888Source::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void Rx888Source::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
}

void Rx888Source::noteTransportFaultOn(ReaderLink& link, const char* what,
                                       const std::string& detail, const std::string& timing) {
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
            link.deadDetail = detail;
        }
        link.faulted = true;
        link.deviceDead = true;
    }
    core::diagWarnf("rx888: transfer failed while %s%s%s%s", what, timing.c_str(),
                    detail.empty() ? "" : ": ", detail.c_str());
    rre::noteDeviceFault(link.owner, "rx888", what, detail);
}

void Rx888Source::noteTransportFault(const char* what, const std::string& detail,
                                     const std::string& timing) {
    noteTransportFaultOn(*link_, what, detail, timing);
}

bool Rx888Source::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool Rx888Source::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string Rx888Source::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deadWhat;
}

const char* Rx888Source::lastError() const {
    // A per-THREAD snapshot, for the same reason SoapySource keeps one: the
    // reader thread rewrites the member while the GUI reads the pointer.
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(link_->errorMutex);
        snapshot = link_->lastError;
    }
    return snapshot.c_str();
}

void Rx888Source::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* Rx888Source::name() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        snapshot = name_;
    }
    return snapshot.c_str();
}

// --- control transfers ----------------------------------------------------

bool Rx888Source::controlOutLocked(rx888::Command c, std::uint16_t value, std::uint16_t index,
                                   const std::uint8_t* data, std::size_t len, const char* what,
                                   unsigned timeoutMs) {
    if (dev_ == nullptr) {
        setError(std::string("no RX888 is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const std::uint64_t asked = nowMs();
    const int ret = dev_->controlOut(cascade::usb::kRequestTypeVendorOut, rx888::requestByte(c),
                                     value, index, data, len, timeoutMs);
    const unsigned took = static_cast<unsigned>(nowMs() - asked);
    const bool ok = ret >= 0 && static_cast<std::size_t>(ret) == len;
    if (tracingOpen_) {
        openTrace_.push_back(
            {rx888::requestByte(c), took, ok, !ok && isTimeoutText(dev_->lastError())});
    }
    if (!ok) {
        noteTransportFault(what, dev_->lastError(),
                           timingText(rx888::requestByte(c), took, timeoutMs));
        return false;
    }
    return true;
}

bool Rx888Source::controlInLocked(rx888::Command c, std::uint16_t value, std::uint16_t index,
                                  std::uint8_t* data, std::size_t len, const char* what,
                                  std::size_t* moved, unsigned timeoutMs) {
    if (moved != nullptr) { *moved = 0; }
    if (dev_ == nullptr) {
        setError(std::string("no RX888 is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const std::uint64_t asked = nowMs();
    const int ret = dev_->controlIn(cascade::usb::kRequestTypeVendorIn, rx888::requestByte(c),
                                    value, index, data, len, timeoutMs);
    const unsigned took = static_cast<unsigned>(nowMs() - asked);
    if (tracingOpen_) {
        openTrace_.push_back(
            {rx888::requestByte(c), took, ret >= 0, ret < 0 && isTimeoutText(dev_->lastError())});
    }
    if (ret < 0) {
        noteTransportFault(what, dev_->lastError(),
                           timingText(rx888::requestByte(c), took, timeoutMs));
        return false;
    }
    if (moved != nullptr) { *moved = static_cast<std::size_t>(ret); }
    return true;
}

bool Rx888Source::settleTunerLocked(const char* what) {
    // TESTFX3 with wValue 0, exactly as open() sends it: four bytes back,
    // nothing changed on the radio (SDDC_FX3/USBhandler.c:496-505).
    std::uint8_t info4[4] = {0, 0, 0, 0};
    const std::uint64_t asked = nowMs();
    if (!controlInLocked(rx888::Command::TestFx3, 0, 0, info4, sizeof(info4), what)) {
        return false;
    }
    const unsigned took = static_cast<unsigned>(nowMs() - asked);
    if (took >= rx888::kTunerWorkNoteMs) {
        core::diagLogf("rx888: %s took %u ms", what, took);
    }
    return true;
}

bool Rx888Source::commandLocked(rx888::Command c, const char* what, unsigned timeoutMs) {
    // One zero byte - see the encoding note in rx888_protocol.hpp for why the
    // reference sends a payload at all for a command that carries nothing.
    const std::uint8_t zero = 0;
    return controlOutLocked(c, 0, 0, &zero, rx888::kEmptyPayloadBytes, what, timeoutMs);
}

bool Rx888Source::commandU32Locked(rx888::Command c, std::uint32_t v, const char* what,
                                   unsigned timeoutMs) {
    std::uint8_t payload[rx888::kU32PayloadBytes];
    rx888::encodeU32(v, payload);
    return controlOutLocked(c, 0, 0, payload, sizeof(payload), what, timeoutMs);
}

bool Rx888Source::commandU64Locked(rx888::Command c, std::uint64_t v, const char* what) {
    std::uint8_t payload[rx888::kU64PayloadBytes];
    rx888::encodeU64(v, payload);
    return controlOutLocked(c, 0, 0, payload, sizeof(payload), what);
}

bool Rx888Source::setArgLocked(rx888::Argument a, std::uint16_t value, const char* what) {
    // Core/arch/win32/FX3handler.cpp:171-185: the value is wValue, the
    // argument number is wIndex, and a single zero byte is the payload.
    const std::uint8_t zero = 0;
    return controlOutLocked(rx888::Command::SetArgFx3, value, static_cast<std::uint16_t>(a), &zero,
                            rx888::kEmptyPayloadBytes, what);
}

bool Rx888Source::gpioWriteLocked(std::uint32_t word, const char* what, unsigned timeoutMs) {
    if (!commandU32Locked(rx888::Command::GpioFx3, word, what, timeoutMs)) { return false; }
    gpio_ = word;
    return true;
}

bool Rx888Source::gpioSetLocked(std::uint32_t mask, const char* what) {
    return gpioWriteLocked(gpio_ | mask, what);
}

bool Rx888Source::gpioClearLocked(std::uint32_t mask, const char* what) {
    return gpioWriteLocked(gpio_ & ~mask, what);
}

// --- the radio's own controls ---------------------------------------------

bool Rx888Source::programAdcClockLocked(std::uint32_t hz) {
    // RX888R2Radio.cpp:47-51, Initialize. THE FIRMWARE ANSWERS THIS AT ONCE
    // AND THEN SLEEPS A FULL SECOND (SDDC_FX3/USBhandler.c:264-273), so it is
    // WHATEVER IS SENT NEXT that waits, and it must be given the time: every
    // request this driver sends outside a teardown has rx888::kSetupTimeout
    // for exactly that. open() is the only caller, and it always sends the
    // antenna switch next, which is where the second is spent.
    if (!commandU32Locked(rx888::Command::StartAdc, hz, "setting the ADC clock")) {
        return false;
    }
    adcRateHz_.store(hz, std::memory_order_relaxed);
    return true;
}

bool Rx888Source::programHfAttLocked(double db) {
    const std::uint16_t code = rx888::hfAttCode(db);
    if (!setArgLocked(rx888::Argument::Dat31Att, code, "setting the HF attenuator")) {
        return false;
    }
    hfAttDb_.store(rx888::hfAttDbForCode(code), std::memory_order_relaxed);
    return true;
}

bool Rx888Source::programHfIfLocked(double db) {
    const int index = rx888::hfVgaIndexForDb(db);
    if (!setArgLocked(rx888::Argument::Ad8340Vga, rx888::hfVgaCodeForIndex(index),
                      "setting the HF IF gain")) {
        return false;
    }
    hfIfDb_.store(rx888::hfVgaDbForIndex(index), std::memory_order_relaxed);
    return true;
}

bool Rx888Source::programVhfRfLocked(double db) {
    const int index = rx888::nearestIndex(rx888::vhfRfTable(), rx888::kVhfRfSteps, db);
    if (!setArgLocked(rx888::Argument::R82xxAttenuator, static_cast<std::uint16_t>(index),
                      "setting the VHF tuner's RF gain")) {
        return false;
    }
    vhfRfDb_.store(rx888::vhfRfTable()[index], std::memory_order_relaxed);
    return true;
}

bool Rx888Source::programVhfIfLocked(double db) {
    const int index = rx888::nearestIndex(rx888::vhfIfTable(), rx888::kVhfIfSteps, db);
    if (!setArgLocked(rx888::Argument::R82xxVga, static_cast<std::uint16_t>(index),
                      "setting the VHF tuner's IF gain")) {
        return false;
    }
    vhfIfDb_.store(rx888::vhfIfTable()[index], std::memory_order_relaxed);
    return true;
}

bool Rx888Source::programModeLocked(bool vhf, bool force) {
    if (!force && vhf == vhf_.load(std::memory_order_relaxed)) { return true; }

    if (vhf) {
        // RX888R2Radio.cpp:64-80, UpdatemodeRF(VHFMODE), in its order:
        // the HF attenuator to its maximum first so the direct-sampling path
        // is dead before the antenna switch moves, then the switch, then the
        // AD8340 to its documented "high gain, 0 dB" setting, then the tuner.
        if (!setArgLocked(rx888::Argument::Dat31Att,
                          static_cast<std::uint16_t>(rx888::kHfAttCodeMax),
                          "muting the HF path for VHF")) {
            return false;
        }
        if (!gpioSetLocked(rx888::kGpioVhfEn, "switching to the VHF antenna")) { return false; }
        if (!setArgLocked(rx888::Argument::Ad8340Vga, 0x80 | 3, "setting the VHF IF amplifier")) {
            return false;
        }
        if (!commandU32Locked(rx888::Command::TunerInit, rx888::kTunerReferenceHz,
                              "starting the VHF tuner")) {
            return false;
        }
        // From here the tuner is this session's to stop. TUNERINIT is what
        // fills the firmware's tuner structure (USBhandler.c:111-142,
        // r820_initialize); the two gains below and TUNERTUNE use it, which is
        // why they come after it and never before.
        tunerStarted_ = true;
        vhf_.store(true, std::memory_order_relaxed);
        // The tuner's start-up is the longest I2C work this firmware does
        // (clock B, the R828D's initialisation, its bandwidth): waited for
        // here, by name, rather than by whichever request happens to be next.
        if (!settleTunerLocked("waiting for the VHF tuner to start")) { return false; }
        // The two gains this front end owns, from whatever the user last
        // asked for - the switch above has just overwritten the HF pair's
        // registers, and the tuner has come up at its own defaults.
        return programVhfRfLocked(vhfRfDb_.load(std::memory_order_relaxed)) &&
               programVhfIfLocked(vhfIfDb_.load(std::memory_order_relaxed));
    }

    // RX888R2Radio.cpp:81-86, UpdatemodeRF(HFMODE): stop the tuner, then move
    // the antenna switch back.
    //
    // THE TUNER IS STOPPED ONLY IF THIS SESSION STARTED IT. The reference
    // sends TUNERSTDBY at every start, before any TUNERINIT; 0.99.113 did the
    // same at every open, and it was the request the first field radio's log
    // ended on. That was the firmware's sleep after STARTADC, not this request
    // (rx888_protocol.hpp) - but an HF receiver has no business depending on
    // the VHF tuner either way, so open() no longer touches it.
    if (tunerStarted_) {
        if (!commandLocked(rx888::Command::TunerStandby, "stopping the VHF tuner")) {
            return false;
        }
        tunerStarted_ = false;
    }
    if (!gpioClearLocked(rx888::kGpioVhfEn, "switching to the HF antenna")) { return false; }
    vhf_.store(false, std::memory_order_relaxed);
    return programHfAttLocked(hfAttDb_.load(std::memory_order_relaxed)) &&
           programHfIfLocked(hfIfDb_.load(std::memory_order_relaxed));
}

bool Rx888Source::programTunerLocked(double hz) {
    // RX888R2Radio.cpp:114-127, TuneLo: the WISHED frequency goes to the
    // tuner, and what comes back down the coax sits at a 4.57 MHz IF inside
    // the ADC's band with its spectrum inverted. The digital side of that -
    // centring the conversion on the IF and conjugating it - is
    // reconfigureConversion's half of the same operation.
    //
    // AND THEN THE FIRMWARE IS WAITED FOR. TUNERTUNE is answered before the
    // tuner's I2C work is done, and this is the last request of every tune:
    // without the wait, a stop or a close sent straight afterwards would
    // arrive on its short teardown bound while the firmware was still busy.
    return commandU64Locked(rx888::Command::TunerTune, static_cast<std::uint64_t>(hz + 0.5),
                            "tuning the VHF tuner") &&
           settleTunerLocked("waiting for the VHF tuner to tune");
}

// --- the conversion -------------------------------------------------------

void Rx888Source::reconfigureConversion(double rateHz, double centerHz, bool vhf) {
    rx888::RealToIq::Config c;
    c.adcRateHz = adcRateHz_.load(std::memory_order_relaxed);
    c.outputRateHz = rateHz;
    c.centerHz = vhf ? rx888::kTunerIfHz : centerHz;
    c.invertSpectrum = vhf;
    c.randomised = randomiser_.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(link_->convMutex);
    link_->conv.configure(c);
    sampleRateHz_.store(link_->conv.rateHz(), std::memory_order_relaxed);
}

void Rx888Source::retuneConversion(double centerHz) {
    std::lock_guard<std::mutex> lk(link_->convMutex);
    link_->conv.setCenterHz(centerHz);
}

// --- the firmware ---------------------------------------------------------

bool Rx888Source::uploadFirmwareLocked(cascade::usb::UsbDevice& boot) {
    const rx888::Fx3Image img =
        rx888::parseFx3Image(rx888::sddcFirmwareImage(), rx888::sddcFirmwareImageSize());
    if (!img.valid) {
        // This is OUR file, not the radio's, so it says so: a user who sees
        // this has a broken FoxSDR install, not a broken receiver.
        setError("the FX3 firmware built into FoxSDR is not usable: " + img.error);
        return false;
    }

    core::diagLogf("rx888: uploading firmware to the FX3 bootloader - %zu sections, %zu bytes, "
                   "entry 0x%08x",
                   img.sections.size(), img.totalBytes, static_cast<unsigned>(img.entry));

    for (const rx888::Fx3Section& s : img.sections) {
        std::uint32_t address = s.address;
        const std::uint8_t* p = s.data;
        std::size_t left = s.bytes;
        while (left > 0) {
            const std::size_t chunk = std::min(left, rx888::kFx3UploadChunkBytes);
            const int ret = boot.controlOut(
                cascade::usb::kRequestTypeVendorOut, rx888::kFx3BootRequest,
                static_cast<std::uint16_t>(address & 0xFFFFu),
                static_cast<std::uint16_t>(address >> 16), p, chunk, rx888::kControlTimeoutMs);
            if (ret < 0 || static_cast<std::size_t>(ret) != chunk) {
                // NOT noteTransportFault: this does not condemn the device.
                // A failed upload is very often a user who has bound WinUSB
                // to one of the two identities and not the other, and the
                // remedy is to run Zadig again and try once more - which a
                // latched deviceDead would refuse to let them do.
                char buf[224];
                std::snprintf(buf, sizeof(buf),
                              "the FX3 bootloader refused the firmware at address 0x%08x (%s); an "
                              "RX888 has two USB identities and Zadig has to have bound WinUSB to "
                              "the bootloader (04B4:00F3) as well as to the radio (04B4:00F1)",
                              static_cast<unsigned>(address), boot.lastError().c_str());
                setError(buf);
                return false;
            }
            p += chunk;
            left -= chunk;
            address += static_cast<std::uint32_t>(chunk);
        }
    }

    // The jump. THE DEVICE LEAVES THE BUS WHILE EXECUTING THIS, so a failure
    // here is the expected outcome as often as not - the reference's own
    // Linux path logs it as a warning and carries on
    // (Core/arch/linux/ezusb.c's fx3 jump ignores an I/O error for exactly
    // this reason). What proves the upload worked is the device coming back
    // under the other identity, which is what awaitStreamer waits for.
    (void)boot.controlOut(cascade::usb::kRequestTypeVendorOut, rx888::kFx3BootRequest,
                          static_cast<std::uint16_t>(img.entry & 0xFFFFu),
                          static_cast<std::uint16_t>(img.entry >> 16), nullptr, 0,
                          rx888::kControlTimeoutMs);
    return true;
}

bool Rx888Source::awaitStreamer(cascade::usb::UsbDeviceInfo& out, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + kFirmwareReenumerateBudget;
    bool sawBootloader = false;
    for (;;) {
        for (const cascade::usb::UsbDeviceInfo& d : listDevices()) {
            if (d.vid != rx888::kUsbVid) { continue; }
            if (d.pid == rx888::kPidStreamer) {
                out = d;
                return true;
            }
            if (d.pid == rx888::kPidBootloader) { sawBootloader = true; }
        }
        if (std::chrono::steady_clock::now() >= deadline) { break; }
        std::this_thread::sleep_for(kFirmwarePollInterval);
    }
    // Two different failures, said differently, because the remedies are
    // different: still a bootloader means the image did not take, and
    // nothing at all means the device came back under an identity Windows
    // has not been told to give WinUSB.
    if (sawBootloader) {
        error =
            "the RX888 is still in its bootloader after the firmware was sent; unplug it, plug it "
            "back in and try again";
    } else {
        error =
            "the RX888 took the firmware but did not come back as a radio within five seconds; it "
            "re-enumerates under a second USB identity (04B4:00F1) which also has to be bound to "
            "WinUSB with Zadig";
    }
    return false;
}

std::vector<cascade::usb::UsbDeviceInfo> Rx888Source::listDevices() {
    if (useFakeTransport_) { return fakeLister_ ? fakeLister_() : std::vector<cascade::usb::UsbDeviceInfo>(); }
    return cascade::usb::enumerateWinUsb(rx888UsbIds());
}

// --- open / close ---------------------------------------------------------

bool Rx888Source::resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                                std::string& error) {
    std::vector<cascade::usb::UsbDeviceInfo> ours;
    for (const cascade::usb::UsbDeviceInfo& d : listDevices()) {
        // Keep only the SDDC family: a caller may hand us a list from a wider
        // scan, and sending SDDC vendor requests to somebody else's dongle
        // would be worse than finding nothing.
        if (d.vid == rx888::kUsbVid && isSddcPid(d.pid)) { ours.push_back(d); }
    }
    if (ours.empty()) {
        error = std::string("no RX888 found (is it plugged in, and ") + cascade::usb::kBindHint + "?)";
        return false;
    }

    const std::string serial = argValue(args, "serial");
    if (!serial.empty()) {
        const std::string want = lowered(serial);
        for (const cascade::usb::UsbDeviceInfo& d : ours) {
            const std::string have = lowered(d.serial);
            if (have == want || (have.size() >= want.size() &&
                                 have.compare(have.size() - want.size(), want.size(), want) == 0)) {
                out = d;
                return true;
            }
        }
        error = "no RX888 with serial " + serial + " is connected";
        return false;
    }

    const std::string indexText = argValue(args, "index");
    std::size_t index = 0;
    if (!indexText.empty()) {
        char* end = nullptr;
        const long n = std::strtol(indexText.c_str(), &end, 10);
        if (end == indexText.c_str() || *end != '\0' || n < 0 ||
            static_cast<std::size_t>(n) >= ours.size()) {
            error = "there is no RX888 at index " + indexText;
            return false;
        }
        index = static_cast<std::size_t>(n);
    }
    out = ours[index];
    return true;
}

bool Rx888Source::open(const std::string& args) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ != nullptr) {
        setError("this RX888 source already has a device open");
        return false;
    }
    clearError();

    openTrace_.clear();
    openReport_.clear();
    tracingOpen_ = false;
    usbLayout_.clear();
    openStartMs_ = nowMs();

    cascade::usb::UsbDeviceInfo info;
    std::string error;
    if (!resolveDevice(args, info, error)) {
        setError(error);
        return false;
    }

    const auto openPath = [this](const std::string& path, std::string& err) {
        std::unique_ptr<cascade::usb::UsbDevice> d;
        if (useFakeTransport_) {
            d = fakeOpener_(path, err);
        } else {
            d = cascade::usb::openWinUsb(path, err);
        }
        return d;
    };

    std::unique_ptr<cascade::usb::UsbDevice> dev = openPath(info.path, error);
    if (dev == nullptr) {
        setError(error.empty() ? std::string("could not open the RX888") : error);
        return false;
    }

    firmwareUploaded_ = false;
    if (info.pid == rx888::kPidBootloader) {
        // THE WHOLE REASON THIS DRIVER CARRIES A FIRMWARE. Nothing below this
        // point exists on a bootloader: no bulk endpoint, no vendor requests,
        // no ADC.
        if (!uploadFirmwareLocked(*dev)) { return false; }
        dev.reset();  // the bootloader has gone; its handle is worthless
        cascade::usb::UsbDeviceInfo streamer;
        if (!awaitStreamer(streamer, error)) {
            setError(error);
            return false;
        }
        // DELIBERATELY NOT MATCHED AGAINST THE ARGS STRING. The serial a
        // bootloader reports is Cypress's and the one the running firmware
        // reports is the SDDC image's, so "the same radio" cannot be decided
        // by comparing them across the upload. With one radio on the bus this
        // is exact; with two, the second is opened by index on the retry.
        dev = openPath(streamer.path, error);
        if (dev == nullptr) {
            setError(error.empty() ? std::string("could not open the RX888 after loading its "
                                                 "firmware")
                                   : error);
            return false;
        }
        info = streamer;
        firmwareUploaded_ = true;
        // THE CLOCK STARTS HERE for anyone reading a log: the open's report
        // line, written when the open ends, counts from the start of open().
        core::diagLogf("rx888: firmware loaded, the radio came back as %04x:%04x (%llu ms into "
                       "the open)",
                       static_cast<unsigned>(info.vid), static_cast<unsigned>(info.pid),
                       static_cast<unsigned long long>(nowMs() - openStartMs_));
    }

    dev_ = std::move(dev);
    link_->dev = dev_.get();

    // The transport's description of the endpoints, taken before anything is
    // asked of the radio so that it is in the log whichever way the open goes.
    // Answered from the host's own copy of the descriptors: nothing is sent
    // (usb_device.hpp, endpointLayout).
    usbLayout_ = dev_->endpointLayout();
    tracingOpen_ = true;

    // --- who is this, and what is on it -------------------------------
    // TESTFX3 answers four bytes: model, firmware high, firmware low, and the
    // firmware's own count of vendor requests served
    // (SDDC_FX3/USBhandler.c:496-503; RadioHandler.cpp:97-100 reads the first
    // three exactly this way).
    //
    // THE FIRST REQUEST IS ALSO THE TEST OF WHETHER THE RADIO ANSWERS AT ALL.
    // The first field radio (0.99.113) did not, three opens running, with
    // another program's firmware already on it: on the bus, WinUSB bound, and
    // this request unanswered each time. When a radio is silent like that,
    // failedOpenLocked() says the one thing that helps.
    //
    // IT IS ASKED TWICE BEFORE THE RADIO IS CALLED SILENT. Why that radio was
    // silent is not known. What is known is that it HAD been answering
    // minutes earlier (Windows could not have bound WinUSB to it otherwise:
    // the firmware runs its SET_CONFIGURATION handler before it acknowledges,
    // FX3 SDK cyu3usbpp.c) and that nothing had been sent to it since - so
    // one candidate is a USB link that went idle and lost the request that
    // woke it, and a second request sent at once is what would get through
    // then. The log says when that happens, which is also how the next
    // report tells that case from a firmware that has stopped for good. This
    // request reads and changes nothing, so asking it again changes nothing.
    //
    // EACH ASK IS GIVEN TWO SECONDS - twice the firmware's longest known pause
    // - because giving up on a request early is not harmless here: a request
    // the host has given up on is still taken by the firmware when its thread
    // is free, and what it does then lands on the NEXT request (below, and
    // rx888_protocol.hpp, the timeouts). A radio that is not coming back costs
    // the person four seconds.
    //
    // A REFUSAL IS ASKED AGAIN TOO. The same radio, on 24 Nov at 16:05 UTC,
    // REFUSED this request 18 ms into an open ("a control transfer failed
    // (Windows error 31)"), nine seconds after 0.99.113 had given up on a
    // request inside the firmware's sleep. That is what the image does with a
    // request whose data never came: it reports it not handled and the SDK
    // stalls endpoint 0, so the next request to arrive is refused
    // (rx888_protocol.hpp, the timeouts). A stall on endpoint 0 ends with the
    // request that met it, so the second ask is the one that gets through.
    std::uint8_t info4[4] = {0, 0, 0, 0};
    std::size_t moved = 0;
    {
        const std::uint64_t asked = nowMs();
        const int first = dev_->controlIn(cascade::usb::kRequestTypeVendorIn,
                                          rx888::requestByte(rx888::Command::TestFx3), 0, 0,
                                          info4, sizeof(info4), rx888::kFirstAskTimeoutMs);
        const unsigned took = static_cast<unsigned>(nowMs() - asked);
        openTrace_.push_back({rx888::requestByte(rx888::Command::TestFx3), took, first >= 0,
                              first < 0 && isTimeoutText(dev_->lastError())});
        if (first >= 0) {
            moved = static_cast<std::size_t>(first);
        } else {
            const std::string detail = dev_->lastError();
            if (isTimeoutText(detail)) {
                core::diagWarnf("rx888: the radio did not answer its first request within %u ms; "
                                "asking once more",
                                rx888::kFirstAskTimeoutMs);
            } else {
                core::diagWarnf("rx888: the radio refused its first request after %u ms (%s); "
                                "asking once more",
                                took, detail.c_str());
            }
            const std::uint64_t again = nowMs();
            if (!controlInLocked(rx888::Command::TestFx3, 0, 0, info4, sizeof(info4),
                                 "reading the hardware information", &moved,
                                 rx888::kFirstAskTimeoutMs)) {
                // The transfer itself failed. Its own words are already in the
                // error slot (noteTransportFault) and STAY there: 0.99.113
                // wrote "the RX888 did not answer its hardware id" over them,
                // and the failure report could no longer tell a timeout from
                // a refusal.
                return failedOpenLocked();
            }
            core::diagWarnf("rx888: the radio answered when asked a second time (after %u ms)",
                            static_cast<unsigned>(nowMs() - again));
        }
    }
    if (moved != sizeof(info4)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "the RX888 answered its hardware information request with %zu bytes instead "
                      "of %zu; this is not the SDDC firmware FoxSDR knows",
                      moved, sizeof(info4));
        setError(buf);
        writeOpenReportLocked(false);
        dev_.reset();
        link_->dev = nullptr;
        return false;
    }
    model_ = static_cast<rx888::Model>(info4[0]);
    firmwareVersion_ =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(info4[1]) << 8) | info4[2]);

    if (model_ != rx888::Model::Rx888mk2) {
        // REFUSED, not driven anyway. The gain tables, the antenna switch and
        // the VHF sequence below are RX888R2Radio's; an RX999 or an HF103
        // running the same firmware would answer every one of these requests
        // and be programmed wrongly by all of them.
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "this is an SDDC device but not an RX888 mk2 (it reports \"%s\"); FoxSDR's "
                      "native driver only knows the mk2",
                      rx888::modelName(model_));
        setError(buf);
        writeOpenReportLocked(false);
        dev_.reset();
        link_->dev = nullptr;
        return false;
    }
    if (firmwareVersion_ != rx888::kFirmwareVersion) {
        // Logged, not refused: 2.x has not moved any of the request numbers
        // this driver sends, and a user running a newer SDDC build should not
        // be locked out by a version compare.
        core::diagWarnf("rx888: the radio is running firmware %u.%u; FoxSDR ships %u.%u",
                        static_cast<unsigned>(firmwareVersion_ >> 8),
                        static_cast<unsigned>(firmwareVersion_ & 0xFF),
                        static_cast<unsigned>(rx888::kFirmwareVersion >> 8),
                        static_cast<unsigned>(rx888::kFirmwareVersion & 0xFF));
    }

    // --- a KNOWN state -------------------------------------------------
    // An SDDC device keeps whatever GPIO word the last application left it
    // at, bias tees included. Writing the whole word FIRST is what makes
    // every readout on this object true of the hardware from the first frame
    // rather than true of whatever ran before us.
    //
    // THE ORDER, AND WHERE THE SECOND GOES. The GPIO word, the ADC clock,
    // then HF: the antenna switch and the two HF gains. The firmware sleeps
    // 1000 ms after it has answered STARTADC, so the antenna-switch request
    // that follows is the one that waits for it - which is why nothing in
    // here may be sent with the short teardown bound. The tuner is sent
    // nothing (programModeLocked: tunerStarted_ is false on a fresh open).
    constexpr double kOpenCenterHz = 10.0e6;
    constexpr double kOpenRateHz = 8.0e6;
    tunerStarted_ = false;
    const bool configured =
        gpioWriteLocked(0, "putting the radio into a known state") &&
        programAdcClockLocked(rx888::kDefaultAdcRateHz) && programModeLocked(false, true);
    if (!configured) { return failedOpenLocked(); }
    centerFrequencyHz_.store(kOpenCenterHz, std::memory_order_relaxed);
    reconfigureConversion(kOpenRateHz, kOpenCenterHz, false);

    std::string label = rx888::modelName(model_);
    if (!info.serial.empty()) { label += " (serial " + info.serial + ")"; }
    setName("RX888: " + label);
    openMirror_.store(true, std::memory_order_relaxed);

    core::diagLogf("rx888: opened %s - firmware %u.%u, ADC %u MHz%s", label.c_str(),
                   static_cast<unsigned>(firmwareVersion_ >> 8),
                   static_cast<unsigned>(firmwareVersion_ & 0xFF),
                   static_cast<unsigned>(adcRateHz_.load(std::memory_order_relaxed) / 1000000u),
                   firmwareUploaded_ ? " (firmware loaded by FoxSDR)" : "");
    writeOpenReportLocked(true);
    return true;
}

void Rx888Source::writeOpenReportLocked(bool opened) {
    tracingOpen_ = false;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "rx888: open %s after %llu ms, %s - ",
                  opened ? "succeeded" : "FAILED",
                  static_cast<unsigned long long>(nowMs() - openStartMs_),
                  firmwareUploaded_ ? "firmware loaded by this open"
                                    : "the radio was already running its firmware");
    std::string line = buf;
    unsigned afterAdc = 0;
    bool sawAfterAdc = false;
    for (std::size_t i = 0; i < openTrace_.size(); ++i) {
        const Asked& a = openTrace_[i];
        if (a.answered) {
            std::snprintf(buf, sizeof(buf), "%s%s %u ms", i == 0 ? "" : ", ",
                          requestName(a.request), a.tookMs);
        } else if (a.silent) {
            std::snprintf(buf, sizeof(buf), "%s%s NO ANSWER in %u ms", i == 0 ? "" : ", ",
                          requestName(a.request), a.tookMs);
        } else {
            std::snprintf(buf, sizeof(buf), "%s%s REFUSED after %u ms", i == 0 ? "" : ", ",
                          requestName(a.request), a.tookMs);
        }
        line += buf;
        if (i > 0 && openTrace_[i - 1].answered &&
            openTrace_[i - 1].request == rx888::requestByte(rx888::Command::StartAdc)) {
            afterAdc = a.tookMs;
            sawAfterAdc = true;
        }
    }
    if (openTrace_.empty()) { line += "nothing was asked"; }
    if (sawAfterAdc) {
        // THE NUMBER THE WHOLE FIX RESTS ON. The firmware sleeps 1000 ms after
        // it has answered STARTADC (0x4000536C in the image), so on a real
        // radio this should read about 1000.
        std::snprintf(buf, sizeof(buf), "; the request after the ADC clock waited %u ms", afterAdc);
        line += buf;
    }
    line += "; " + linkText(usbLayout_);
    openReport_ = line;
    if (opened) {
        core::diagLogf("%s", line.c_str());
    } else {
        core::diagWarnf("%s", line.c_str());
    }
}

bool Rx888Source::failedOpenLocked() {
    std::string what;
    std::string detail;
    bool dead = false;
    {
        std::lock_guard<std::mutex> lk(link_->errorMutex);
        dead = link_->deviceDead;
        what = link_->deadWhat;
        detail = link_->deadDetail;
    }
    if (dead && isTimeoutText(detail)) {
        // ONE PLAIN SENTENCE, THEN THE EVIDENCE. The sentence is for the
        // person and is translated; what follows in brackets is for the
        // failure report and the log and stays English, like every other
        // driver detail - it is also what files the report as a timeout
        // whatever language the sentence is shown in (health_events.cpp,
        // classifyRadioOpen).
        setError(std::string(cascade::i18n::tr("The RX888 is not answering. Unplug the RX888, "
                                               "wait ten seconds, plug it in again, then choose "
                                               "it.")) +
                 " (no answer while " + what + ": " + detail + ")");
    }
    writeOpenReportLocked(false);
    // Sent nothing more: the radio is released as it is. A firmware that
    // answers nothing cannot be put into any state, and one that refused a
    // request is left for the next open to set up from the start.
    dev_.reset();
    link_->dev = nullptr;
    return false;
}

void Rx888Source::closeDevice() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
    if (dev_ != nullptr && !deviceDead()) {
        // Leave the radio quiet and both antenna ports unpowered: the next
        // application to open it inherits whatever we leave behind, exactly
        // as we inherited what came before. The SHDWN bit last, which is what
        // the reference's RadioHardware destructor does
        // (Core/RadioHardware.cpp:17-22). The tuner is stopped only if this
        // session started it. Both on the short teardown bound.
        if (tunerStarted_) {
            commandLocked(rx888::Command::TunerStandby, "stopping the VHF tuner",
                          rx888::kControlTimeoutMs);
            tunerStarted_ = false;
        }
        gpioWriteLocked(rx888::kGpioShdwn, "shutting the front end down",
                        rx888::kControlTimeoutMs);
    }
    // AN ABANDONED READER STILL HOLDS link_->dev AND IS STILL USING IT.
    // stopStreamingLocked releases dev_ without clearing the link's copy,
    // deliberately, so a stranded thread has a live object to be inside;
    // clearing it here anyway would be a data race with that thread's very
    // next readBulk - the exact defect the abandonment path exists to avoid.
    // dev_ being null with link_->dev set is what that state looks like.
    const bool abandoned = dev_ == nullptr && link_->dev != nullptr;
    dev_.reset();
    if (!abandoned) { link_->dev = nullptr; }
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    setName("RX888: (no device)");
}

rx888::Model Rx888Source::model() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return model_;
}

std::uint16_t Rx888Source::firmwareVersion() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return firmwareVersion_;
}

bool Rx888Source::firmwareWasUploaded() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return firmwareUploaded_;
}

// --- streaming ------------------------------------------------------------

bool Rx888Source::startStreamingLocked() {
    // ORDER MATTERS. The bulk ring is queued FIRST: a GPIF producer told to
    // run with nothing queued fills the FX3's own DMA buffers and overruns
    // before the host's first read, which presents as a stream that starts
    // corrupted and then recovers - the hardest kind of fault to attribute
    // later. At 128 MB/s that window is microseconds wide.
    if (!dev_->beginBulkStream(rx888::kRxEndpoint, rx888::kTransferBufferBytes,
                               rx888::kTransferCount)) {
        noteTransportFault("queueing the sample transfers", dev_->lastError());
        return false;
    }
    if (!commandLocked(rx888::Command::StartFx3, "starting the sample stream")) {
        dev_->endBulkStream();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(link_->waitMutex);
        link_->exited = false;
    }
    {
        // A fresh stream starts from a filter history of zeros, so the first
        // milliseconds are the filters' transient and not the radio's.
        std::lock_guard<std::mutex> lk(link_->convMutex);
        link_->conv.reset();
    }
    link_->run.store(true, std::memory_order_relaxed);
    reader_ = std::thread(&Rx888Source::readerThreadBody, link_);
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void Rx888Source::stopStreamingLocked(unsigned stopTimeoutMs) {
    if (!reader_.joinable() && !running_.load(std::memory_order_relaxed)) {
        if (dev_ != nullptr) { dev_->endBulkStream(); }
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    if (dev_ != nullptr && !deviceDead()) {
        // The short teardown bound when this IS a stop (the shutdown path,
        // rx888_protocol.hpp kControlTimeout); the patient one when it is the
        // first half of a retune or a rate change.
        commandLocked(rx888::Command::StopFx3, "stopping the sample stream", stopTimeoutMs);
    }
    link_->run.store(false, std::memory_order_relaxed);
    link_->waitCv.notify_all();

    if (reader_.joinable()) {
        // A BOUNDED JOIN, not a plain one (see the file header). The reader
        // sets `exited` as its last act, so a flag still clear after
        // kReaderJoinWait means a thread parked somewhere it is not coming
        // back from - and waiting for it on the GUI thread would be the hang
        // the whole transport exists to avoid.
        bool exited = false;
        {
            std::unique_lock<std::mutex> lk(link_->waitMutex);
            exited = link_->waitCv.wait_for(lk, kReaderJoinWait, [this] { return link_->exited; });
        }
        if (exited) {
            reader_.join();
        } else {
            // ABANDONED. The thread keeps its own copy of the link and the
            // device pointer inside it, so NEITHER may be disturbed: the
            // device is leaked deliberately rather than destroyed under a
            // thread still calling into it, and link_->dev is left pointing
            // at it for the same reason.
            g_readersAbandoned.fetch_add(1, std::memory_order_relaxed);
            core::health::raiseRecovered(core::health::Recovered::ReaderLeak);
            noteTransportFault("waiting for the sample reader to stop",
                               "the reader did not return; the radio is left to the operating "
                               "system and FoxSDR must be restarted to use it again");
            reader_.detach();
            // Marked for LeakSanitizer (core/leak_on_purpose.hpp).
            cascade::core::leakOnPurpose(dev_.release());
            running_.store(false, std::memory_order_relaxed);
            return;
        }
    }

    // Only now is the transport's ring safe to free: no thread of ours is
    // inside readBulk.
    if (dev_ != nullptr) { dev_->endBulkStream(); }
    running_.store(false, std::memory_order_relaxed);
}

bool Rx888Source::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("start() called with no RX888 open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running_.load(std::memory_order_relaxed)) { return true; }
    clearError();
    return startStreamingLocked();
}

void Rx888Source::stop() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
}

// --- the reader thread ----------------------------------------------------

void Rx888Source::readerThreadBody(std::shared_ptr<ReaderLink> link) {
    std::vector<std::uint8_t> raw(rx888::kTransferBufferBytes);
    std::vector<std::int16_t> adc(rx888::kAdcSamplesPerTransfer);
    // The widest the conversion can ever answer with is one complex sample
    // per two ADC samples (no decimation past stage 1), plus the one the
    // capacity bound allows for a grid that has not landed yet.
    std::vector<std::complex<float>> conv(rx888::kAdcSamplesPerTransfer / 2 + 2);
    const unsigned timeoutMs = static_cast<unsigned>(kBulkReadWait.count());

    while (link->run.load(std::memory_order_relaxed)) {
        const int got = link->dev->readBulk(raw.data(), raw.size(), timeoutMs);
        if (!link->run.load(std::memory_order_relaxed)) { break; }

        if (got < 0) {
            // A NEGATIVE READ IS THE DEVICE GOING. There is nothing to retry:
            // the pipe has failed, and every further read would fail the same
            // way while the source loop waited for samples that cannot come.
            noteRead(*link, got, 0, false);
            noteTransportFaultOn(*link, "reading samples", link->dev->lastError());
            break;
        }

        // A SHORT TRANSFER IS NOT AN ERROR, it is the last transfer of a
        // stopping stream; an ODD one would be, so the odd byte is dropped
        // rather than being allowed to shift every sample after it by one.
        const std::size_t adcCount = static_cast<std::size_t>(got) / rx888::kBytesPerAdcSample;
        std::size_t samples = 0;
        bool dropped = false;
        if (adcCount > 0) {
            for (std::size_t i = 0; i < adcCount; ++i) {
                // Little-endian on the wire, assembled rather than cast: the
                // transfer is a byte stream, not this machine's memory.
                adc[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(raw[2 * i]) |
                                                   (static_cast<std::uint16_t>(raw[2 * i + 1])
                                                    << 8));
            }
            {
                std::lock_guard<std::mutex> lk(link->convMutex);
                samples = link->conv.process(adc.data(), adcCount, conv.data());
            }
            if (samples > 0) {
                const std::size_t written = link->ring.write(conv.data(), samples);
                if (written != samples) {
                    // The host fell behind, not the radio. Counted rather
                    // than silently tolerated: a spectrum with a gap in it
                    // and no number anywhere is how a slow machine looks
                    // exactly like a broken one - and on a radio delivering
                    // 128 MB/s this is the number that will move first.
                    dropped = true;
                    link->dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        noteRead(*link, got, samples, dropped);
        if (samples == 0) { continue; }
        {
            // Taken and released so a read() that has just tested the ring
            // and is about to wait cannot miss this notify in between.
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

void Rx888Source::noteRead(ReaderLink& link, int ret, std::size_t samples, bool dropped) {
    const auto now = std::chrono::steady_clock::now();
    std::string line;
    bool warn = false;
    rre::StreamTrouble trouble;
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
            // "nothing yet", not a failure.
            ++h.timeouts;
        } else {
            ++h.errors;
        }
        if (dropped) { ++h.overflows; }

        if (now - h.windowStart < link.healthWindow) { return; }

        const bool nominal =
            h.timeouts == 0 && h.overflows == 0 && h.errors == 0 && h.longestGapMs < 250;
        const bool first = !link.healthEverWritten;
        warn = h.errors > 0 || h.longestGapMs >= 1000;
        trouble = {std::chrono::duration<double>(now - h.windowStart).count(), h.samples, h.errors, h.overflows, h.timeouts,
                   static_cast<long long>(h.longestGapMs)};
        line = healthLineLocked(link);
        if (line.empty()) { return; }
        if (!(warn || first || !nominal)) { return; }
        link.healthEverWritten = true;
    }
    if (warn) {
        core::diagWarnf("%s", line.c_str());
        rre::noteStreamTrouble(link.owner, "rx888", trouble);
    } else {
        core::diagLogf("%s", line.c_str());
    }
}

std::string Rx888Source::healthLineLocked(ReaderLink& link) {
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
    // open to parse the line.
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

std::string Rx888Source::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void Rx888Source::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

std::uint64_t Rx888Source::droppedTransfers() const {
    return link_->dropped.load(std::memory_order_relaxed);
}

unsigned long long Rx888Source::readersAbandoned() {
    return g_readersAbandoned.load(std::memory_order_relaxed);
}

// --- read -----------------------------------------------------------------

std::size_t Rx888Source::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) { return 0; }
    std::size_t got = link_->ring.read(dst, n);
    if (got > 0) { return got; }
    if (faulted()) { return 0; }
    {
        std::unique_lock<std::mutex> lk(link_->waitMutex);
        link_->waitCv.wait_for(lk, kReadWait, [this] {
            return link_->ring.size() > 0 || !link_->run.load(std::memory_order_relaxed);
        });
    }
    got = link_->ring.read(dst, n);
    return got;
}

// --- rate, frequency, gains ----------------------------------------------

std::vector<double> Rx888Source::supportedSampleRatesHz() const {
    return rx888::supportedRatesHz(adcRateHz_.load(std::memory_order_relaxed));
}

bool Rx888Source::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setSampleRateHz() called with no RX888 open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz > 0.0)) {  // negated compare so a NaN lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }

    const std::uint32_t adc = adcRateHz_.load(std::memory_order_relaxed);
    const std::vector<double> rates = rx888::supportedRatesHz(adc);
    double want = rates.front();
    double bestErr = -1.0;
    for (const double r : rates) {
        const double err = std::fabs(r - hz);
        if (bestErr < 0.0 || err < bestErr) {
            bestErr = err;
            want = r;
        }
    }
    const bool vhf = vhf_.load(std::memory_order_relaxed);
    if (vhf && want > rx888::kMaxVhfRateHz) {
        // COERCED, and said. A complex band wider than twice the tuner's
        // 4.57 MHz IF reaches below 0 Hz in the ADC's band and folds its own
        // mirror into itself, which on a spectrum looks like a second copy of
        // every signal rather than like a rate that was refused.
        want = rx888::kMaxVhfRateHz;
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "the RX888's VHF tuner puts its output at a %.2f MHz IF, so the widest rate "
                      "above %.0f MHz is %.0f MS/s; %.3f MS/s was coerced to it",
                      rx888::kTunerIfHz / 1e6, rx888::kHfCeilingHz(adc) / 1e6,
                      rx888::kMaxVhfRateHz / 1e6, hz / 1e6);
        setError(buf);
    }

    // AN HF BAND HAS TO FIT INSIDE WHAT THE ADC DIGITISES. Widening the rate
    // where it is tuned may not leave room, and the centre is what moves:
    // clamped to the middle of whatever the new width allows, which is the
    // only place a band of that width can be.
    double center = centerFrequencyHz_.load(std::memory_order_relaxed);
    if (!vhf) {
        const double half = want / 2.0;
        const double ceiling = rx888::kHfCeilingHz(adc);
        if (center - half < 0.0) { center = half; }
        if (center + half > ceiling) { center = ceiling - half; }
    }

    const bool wasRunning = running_.load(std::memory_order_relaxed);
    if (wasRunning) {
        // A QUIET RADIO for the change: the producer is stopped, our reader
        // joined and the bulk ring torn down before the conversion underneath
        // it is rebuilt. Not a shutdown: the patient bound.
        stopStreamingLocked(rx888::kSetupTimeoutMs);
    }
    reconfigureConversion(want, center, vhf);
    centerFrequencyHz_.store(center, std::memory_order_relaxed);
    if (wasRunning) { return startStreamingLocked(); }
    return true;
}

bool Rx888Source::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setCenterFrequencyHz() called with no RX888 open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz >= rx888::kMinFrequencyHz && hz <= rx888::kMaxFrequencyHz)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "the RX888 tunes %.3f MHz to %.0f MHz; %.6f MHz is outside that",
                      rx888::kMinFrequencyHz / 1e6, rx888::kMaxFrequencyHz / 1e6, hz / 1e6);
        rre::noteControlRefused(this, "rx888", rre::Control::Frequency, "outside the range this radio can do", true);
        setError(buf);
        return false;
    }

    const std::uint32_t adc = adcRateHz_.load(std::memory_order_relaxed);
    // RX888R2Radio.cpp:53-62: the ADC's own Nyquist is the boundary between
    // direct sampling and the tuner.
    const bool wantVhf = hz >= rx888::kHfCeilingHz(adc);
    double rate = sampleRateHz_.load(std::memory_order_relaxed);

    if (!wantVhf) {
        const double half = rate / 2.0;
        if (hz - half < 0.0 || hz + half > rx888::kHfCeilingHz(adc)) {
            // REFUSED with the arithmetic in it rather than clamped, because
            // a tune that silently lands somewhere else is worse than one
            // that does not happen - and the remedy (a narrower rate) is
            // something only the caller can choose.
            char buf[224];
            std::snprintf(buf, sizeof(buf),
                          "at %.3f MS/s the RX888's %.6f MHz band would not fit inside the ADC's "
                          "0 - %.0f MHz; choose a narrower sample rate or a centre between %.3f "
                          "and %.3f MHz",
                          rate / 1e6, hz / 1e6, rx888::kHfCeilingHz(adc) / 1e6, half / 1e6,
                          (rx888::kHfCeilingHz(adc) - half) / 1e6);
            rre::noteControlRefused(this, "rx888", rre::Control::Frequency, "outside the range this radio can do", true);
            setError(buf);
            return false;
        }
    }

    const bool wasVhf = vhf_.load(std::memory_order_relaxed);
    const bool modeChange = wantVhf != wasVhf;
    const bool needNarrower = wantVhf && rate > rx888::kMaxVhfRateHz;

    if (modeChange || needNarrower) {
        // A front-end switch is several control transfers and a filter
        // rebuild; both are done with the radio quiet, for the same reason a
        // rate change is.
        const bool wasRunning = running_.load(std::memory_order_relaxed);
        if (wasRunning) { stopStreamingLocked(rx888::kSetupTimeoutMs); }
        if (modeChange && !programModeLocked(wantVhf, false)) {
            if (wasRunning && !deviceDead()) { startStreamingLocked(); }
            return false;
        }
        if (needNarrower) {
            rate = rx888::kMaxVhfRateHz;
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "the RX888's VHF tuner puts its output at a %.2f MHz IF, so the sample "
                          "rate was narrowed to %.0f MS/s for this tune",
                          rx888::kTunerIfHz / 1e6, rx888::kMaxVhfRateHz / 1e6);
            setError(buf);
        }
        if (wantVhf && !programTunerLocked(hz)) {
            if (wasRunning && !deviceDead()) { startStreamingLocked(); }
            return false;
        }
        reconfigureConversion(rate, hz, wantVhf);
        centerFrequencyHz_.store(hz, std::memory_order_relaxed);
        if (wasRunning) { return startStreamingLocked(); }
        return true;
    }

    // Inside one front end this is live: an HF retune is an NCO frequency and
    // no USB traffic at all, and a VHF retune is one TUNERTUNE while the
    // digital side stays put on the tuner's IF.
    if (wantVhf) {
        if (!programTunerLocked(hz)) { return false; }
    } else {
        retuneConversion(hz);
    }
    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    return true;
}

bool Rx888Source::frequencyRangeHz(double& loHz, double& hiHz) const {
    loHz = rx888::kMinFrequencyHz;
    hiHz = rx888::kMaxFrequencyHz;
    return true;
}

std::vector<GainInfo> Rx888Source::gains() const {
    const double* vhfRf = rx888::vhfRfTable();
    const double* vhfIf = rx888::vhfIfTable();
    return {
        GainInfo{"HF ATT", rx888::kHfAttMinDb, rx888::kHfAttMaxDb, rx888::kHfAttStepDb},
        // The AD8340's and the R828D's scales are UNEVEN - the AD8340 alone
        // runs from a 6 dB step at the bottom to 0.08 dB at the top - so the
        // step declared here is a sensible increment for a control, not a
        // claim about the hardware. setGainDb snaps to the nearest step the
        // chip actually has and gainDb() reports that.
        GainInfo{"HF IF", rx888::hfVgaDbForIndex(0), rx888::hfVgaDbForIndex(rx888::kHfVgaSteps - 1),
                 0.5},
        GainInfo{"VHF RF", vhfRf[0], vhfRf[rx888::kVhfRfSteps - 1], 0.5},
        GainInfo{"VHF IF", vhfIf[0], vhfIf[rx888::kVhfIfSteps - 1], 0.5},
    };
}

bool Rx888Source::setGainDb(const std::string& gainName, double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setGainDb() called with no RX888 open");
        return false;
    }
    if (deviceDead()) { return false; }
    const bool vhf = vhf_.load(std::memory_order_relaxed);

    // A GAIN ON THE FRONT END THAT IS NOT IN CIRCUIT IS REMEMBERED, NOT SENT.
    // The R828D is in standby in HF mode and the DAT-31 is muted in VHF mode;
    // writing to either then is a register write whose effect nobody can
    // observe, and on the tuner it is an I2C transaction to a chip that has
    // been told to stop. programModeLocked programs both of a side's gains
    // when that side comes into circuit, which is where these land.
    if (gainName == "HF ATT") {
        if (vhf) {
            hfAttDb_.store(rx888::hfAttDbForCode(rx888::hfAttCode(db)), std::memory_order_relaxed);
            return true;
        }
        return programHfAttLocked(db);
    }
    if (gainName == "HF IF") {
        if (vhf) {
            hfIfDb_.store(rx888::hfVgaDbForIndex(rx888::hfVgaIndexForDb(db)),
                          std::memory_order_relaxed);
            return true;
        }
        return programHfIfLocked(db);
    }
    if (gainName == "VHF RF") {
        if (!vhf) {
            vhfRfDb_.store(
                rx888::vhfRfTable()[rx888::nearestIndex(rx888::vhfRfTable(), rx888::kVhfRfSteps,
                                                        db)],
                std::memory_order_relaxed);
            return true;
        }
        // A tuner argument is I2C work done after the answer, like TUNERTUNE:
        // waited for, so nothing sent next can run out of time behind it.
        return programVhfRfLocked(db) &&
               settleTunerLocked("waiting for the VHF tuner to take its gain");
    }
    if (gainName == "VHF IF") {
        if (!vhf) {
            vhfIfDb_.store(
                rx888::vhfIfTable()[rx888::nearestIndex(rx888::vhfIfTable(), rx888::kVhfIfSteps,
                                                        db)],
                std::memory_order_relaxed);
            return true;
        }
        return programVhfIfLocked(db) &&
               settleTunerLocked("waiting for the VHF tuner to take its gain");
    }
    setError("the RX888 has no gain called \"" + gainName + "\"");
    return false;
}

double Rx888Source::gainDb(const std::string& gainName) const {
    if (gainName == "HF ATT") { return hfAttDb_.load(std::memory_order_relaxed); }
    if (gainName == "HF IF") { return hfIfDb_.load(std::memory_order_relaxed); }
    if (gainName == "VHF RF") { return vhfRfDb_.load(std::memory_order_relaxed); }
    if (gainName == "VHF IF") { return vhfIfDb_.load(std::memory_order_relaxed); }
    return 0.0;
}

bool Rx888Source::setAutoGain(bool on) {
    (void)on;
    setError("the RX888 has no automatic gain control");
    return false;
}

std::string Rx888Source::antenna() const {
    return vhf_.load(std::memory_order_relaxed) ? "VHF" : "HF";
}

bool Rx888Source::setAntenna(const std::string& antennaName) {
    const std::string have = antenna();
    if (antennaName == have) { return true; }
    if (antennaName != "HF" && antennaName != "VHF") {
        setError("the RX888 has two receive ports, \"HF\" and \"VHF\"");
        return false;
    }
    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "the RX888 chooses its front end from the tuning: below %.0f MHz the HF port is "
                  "sampled directly and above it the VHF tuner is used, so \"%s\" follows the "
                  "centre frequency rather than the other way round",
                  rx888::kHfCeilingHz(adcRateHz_.load(std::memory_order_relaxed)) / 1e6,
                  antennaName.c_str());
    rre::noteControlRefused(this, "rx888", rre::Control::Antenna, "no such antenna on this radio", true);
    setError(buf);
    return false;
}

// --- the GPIO controls ----------------------------------------------------

bool Rx888Source::setDither(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setDither() called with no RX888 open");
        return false;
    }
    if (deviceDead()) { return false; }
    // RadioHandler.cpp:301-309.
    const bool ok = on ? gpioSetLocked(rx888::kGpioDither, "switching the ADC dither on")
                       : gpioClearLocked(rx888::kGpioDither, "switching the ADC dither off");
    if (ok) { dither_.store(on, std::memory_order_relaxed); }
    return ok;
}

bool Rx888Source::setRandomiser(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setRandomiser() called with no RX888 open");
        return false;
    }
    if (deviceDead()) { return false; }
    // RadioHandler.cpp:321-330 - and note that the reference changes the CHIP
    // and the CONVERTER together in that one function, as this does. Turning
    // the randomiser on at the ADC without telling the conversion to undo it
    // turns the whole band into noise, so the two are one switch here.
    const bool ok = on ? gpioSetLocked(rx888::kGpioRandom, "switching the ADC randomiser on")
                       : gpioClearLocked(rx888::kGpioRandom, "switching the ADC randomiser off");
    if (!ok) { return false; }
    randomiser_.store(on, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> ck(link_->convMutex);
        link_->conv.setRandomised(on);
    }
    return true;
}

bool Rx888Source::setPga(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setPga() called with no RX888 open");
        return false;
    }
    if (deviceDead()) { return false; }
    // RadioHandler.cpp:311-319.
    const bool ok = on ? gpioSetLocked(rx888::kGpioPgaEn, "switching the HF PGA on")
                       : gpioClearLocked(rx888::kGpioPgaEn, "switching the HF PGA off");
    if (ok) { pga_.store(on, std::memory_order_relaxed); }
    return ok;
}

bool Rx888Source::setBiasT(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setBiasT() called with no RX888 open");
        return false;
    }
    if (deviceDead()) { return false; }
    // RadioHandler.cpp:398-406, UpdBiasT_HF.
    const bool ok = on ? gpioSetLocked(rx888::kGpioBiasHf, "switching the HF bias tee on")
                       : gpioClearLocked(rx888::kGpioBiasHf, "switching the HF bias tee off");
    if (ok) { biasHf_.store(on, std::memory_order_relaxed); }
    return ok;
}

bool Rx888Source::setVhfBiasT(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setVhfBiasT() called with no RX888 open");
        return false;
    }
    if (deviceDead()) { return false; }
    // RadioHandler.cpp:408-415, UpdBiasT_VHF.
    const bool ok = on ? gpioSetLocked(rx888::kGpioBiasVhf, "switching the VHF bias tee on")
                       : gpioClearLocked(rx888::kGpioBiasVhf, "switching the VHF bias tee off");
    if (ok) { biasVhf_.store(on, std::memory_order_relaxed); }
    return ok;
}

}  // namespace cascade::source
