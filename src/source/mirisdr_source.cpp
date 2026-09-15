// The Mirics driver itself. See mirisdr_source.hpp for the threading and
// ownership argument and for the licence posture; the protocol numbers and the
// arithmetic are in msi2500.hpp and tuner_msi001.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/mirisdr_source.hpp"

#include "core/diag_log.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// The gain names, in one place so the four methods that switch on them cannot
// drift apart.
constexpr const char* kGainLna = "LNA";
constexpr const char* kGainMixer = "MIXER";
constexpr const char* kGainBaseband = "BASEBAND";
constexpr const char* kGainAmBuffer = "AM BUFFER";

// What this driver opens a device at. The reference's own default gain leaves
// the BASEBAND amplifier at zero of its 59 dB, which is the quietest this
// radio can be with its front end fully on; a receiver that opens deaf reads
// as broken, so the baseband starts mid-scale instead - the same choice the
// HackRF driver makes with its LNA and VGA.
constexpr double kOpenSampleRateHz = 2.0e6;
constexpr double kOpenFrequencyHz = 100.0e6;
constexpr int kOpenBasebandReduction = 29;  // 30 dB of 59

}  // namespace

// --- enumeration ----------------------------------------------------------

std::vector<NativeDeviceInfo> miriSdrDevicesFrom(
    const std::vector<cascade::usb::UsbDeviceInfo>& devices) {
    std::vector<NativeDeviceInfo> out;
    int index = 0;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        const msi2500::DeviceModel* model = msi2500::modelFor(d.vid, d.pid);
        if (model == nullptr) { continue; }
        NativeDeviceInfo info;
        info.driver = "mirisdr";
        info.label = model->label;
        if (!d.serial.empty()) {
            info.label += " (serial " + d.serial + ")";
            info.args = "serial=" + d.serial;
        } else {
            // MOST OF THESE DEVICES HAVE NO USB SERIAL AT ALL - they are
            // television sticks, and the reference fabricates one from the
            // enumeration index rather than reading one. So the index form is
            // the ordinary case here rather than the fallback it is for a
            // HackRF, and it is stable because the transport sorts by path.
            info.args = "index=" + std::to_string(index);
        }
        out.push_back(std::move(info));
        ++index;
    }
    return out;
}

std::vector<NativeDeviceInfo> enumerateMiriSdr() {
    // See hackrf_source.cpp's enumerateHackRf(): enumerateWinUsb() is the one
    // entry point on every platform (WinUSB on Windows, usbfs on Linux).
    return miriSdrDevicesFrom(cascade::usb::enumerateWinUsb(msi2500::usbIds()));
}

// --- construction ---------------------------------------------------------

MiriSdrSource::~MiriSdrSource() { closeDevice(); }

void MiriSdrSource::setTransportForTest(std::vector<cascade::usb::UsbDeviceInfo> devices,
                                        UsbOpenFn opener) {
    std::lock_guard<std::mutex> lk(devMutex_);
    fakeDevices_ = std::move(devices);
    fakeOpener_ = std::move(opener);
    useFakeTransport_ = static_cast<bool>(fakeOpener_);
}

// --- the error slot -------------------------------------------------------

void MiriSdrSource::setErrorOn(ReaderLink& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void MiriSdrSource::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void MiriSdrSource::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
}

void MiriSdrSource::noteTransportFaultOn(ReaderLink& link, const char* what,
                                         const std::string& detail) {
    {
        std::lock_guard<std::mutex> lk(link.errorMutex);
        // Do not overwrite the FIRST cause. Once a device has gone, every later
        // transfer on it fails too, and the last message is the least
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
    core::diagWarnf("mirisdr: transfer failed while %s%s%s", what, detail.empty() ? "" : ": ",
                    detail.c_str());
}

void MiriSdrSource::noteTransportFault(const char* what, const std::string& detail) {
    noteTransportFaultOn(*link_, what, detail);
}

bool MiriSdrSource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool MiriSdrSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string MiriSdrSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deadWhat;
}

const char* MiriSdrSource::lastError() const {
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

void MiriSdrSource::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* MiriSdrSource::name() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        snapshot = name_;
    }
    return snapshot.c_str();
}

// --- control transfers ----------------------------------------------------

bool MiriSdrSource::writeRegisterLocked(std::uint8_t reg, std::uint32_t val, const char* what) {
    if (dev_ == nullptr) {
        setError(std::string("no Mirics device is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const msi2500::RegWrite w = msi2500::encodeRegWrite(reg, val);
    // NO DATA STAGE: the whole register rides in the setup packet, which is
    // why the length is zero and a return of zero is the success case here
    // rather than a short transfer.
    const int ret = dev_->controlOut(msi2500::kRequestTypeVendorOutEndpoint,
                                     msi2500::requestByte(msi2500::VendorRequest::WriteRegister),
                                     w.value, w.index, nullptr, 0, msi2500::kControlTimeoutMs);
    if (ret < 0) {
        noteTransportFault(what, dev_->lastError());
        return false;
    }
    return true;
}

bool MiriSdrSource::commandLocked(msi2500::VendorRequest r, const char* what,
                                  unsigned timeoutMs) {
    if (dev_ == nullptr) {
        setError(std::string("no Mirics device is open (") + what + ")");
        return false;
    }
    if (deviceDead()) { return false; }
    const int ret = dev_->controlOut(msi2500::kRequestTypeVendorOutEndpoint,
                                     msi2500::requestByte(r), 0, 0, nullptr, 0, timeoutMs);
    if (ret < 0) {
        noteTransportFault(what, dev_->lastError());
        return false;
    }
    return true;
}

bool MiriSdrSource::programRateLocked(const msi2500::RateSetting& r, const char* what) {
    // The format FIRST, then the fraction, then the divider - which is the
    // order the reference writes them in, and the divider is last because it
    // is the register that carries the "go" bit for the whole clock.
    if (!writeRegisterLocked(msi2500::kRegFormat, r.formatRegister, what)) { return false; }
    if (!writeRegisterLocked(msi2500::kRegClockFraction, r.clockFraction, what)) { return false; }
    return writeRegisterLocked(msi2500::kRegClockDivider, r.clockDivider, what);
}

bool MiriSdrSource::programBandSwitchLocked(const char* what) {
    return writeRegisterLocked(msi2500::kRegBandSwitch,
                               msi2500::bandSwitchValue(bandSelectWord_,
                                                        biasT_.load(std::memory_order_relaxed)),
                               what);
}

bool MiriSdrSource::programTuneLocked(double hz, const char* what) {
    const msi001::TuneSetting t = msi001::computeTune(hz, plan_, bandwidth_, ifMode_);
    bandSelectWord_ = t.bandSelectWord;
    band_ = t.band;

    // The switch first - it puts the right filter in front of the tuner before
    // the tuner is told where to go - then the five tuner words in the
    // reference's own order. The AFC trim goes out BEFORE the mode word and
    // the synthesiser words, which looks backwards and is not: the trim is a
    // correction to the previous setting until the new one lands, and the
    // sequence is what the silicon is known to take.
    if (!programBandSwitchLocked(what)) { return false; }
    if (!writeRegisterLocked(msi2500::kRegTunerWord, msi001::kTunePreambleWord, what)) {
        return false;
    }
    if (!writeRegisterLocked(msi2500::kRegTunerWord, t.reg3, what)) { return false; }
    if (!writeRegisterLocked(msi2500::kRegTunerWord, t.reg0, what)) { return false; }
    if (!writeRegisterLocked(msi2500::kRegTunerWord, t.reg5, what)) { return false; }
    return writeRegisterLocked(msi2500::kRegTunerWord, t.reg2, what);
}

bool MiriSdrSource::programGainLocked(const char* what) {
    const msi001::GainSetting g = msi001::computeGain(stages_, band_);
    if (!writeRegisterLocked(msi2500::kRegTunerWord, g.reg1, what)) { return false; }
    return writeRegisterLocked(msi2500::kRegTunerWord, g.reg6, what);
}

// --- open / close ---------------------------------------------------------

bool MiriSdrSource::resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                                  std::string& error) {
    std::vector<cascade::usb::UsbDeviceInfo> devices;
    if (useFakeTransport_) {
        devices = fakeDevices_;
    } else {
        devices = cascade::usb::enumerateWinUsb(msi2500::usbIds());
    }
    // Keep only devices we recognise: a caller may hand us a list from a wider
    // scan, and sending MSi2500 register writes to somebody else's dongle
    // would be worse than finding nothing.
    std::vector<cascade::usb::UsbDeviceInfo> ours;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (msi2500::modelFor(d.vid, d.pid) != nullptr) { ours.push_back(d); }
    }
    if (ours.empty()) {
        error = "no Mirics MSi2500 device found (is it plugged in, and bound to WinUSB?)";
        return false;
    }

    const std::string serial = argValue(args, "serial");
    if (!serial.empty()) {
        const std::string want = lowered(serial);
        for (const cascade::usb::UsbDeviceInfo& d : ours) {
            const std::string have = lowered(d.serial);
            // A suffix match as well as an exact one, so a short form read off
            // a label or another tool's listing still finds the device. An
            // EMPTY serial never matches: every device would otherwise match
            // every request, and most of these have no serial at all.
            if (!have.empty() &&
                (have == want || (have.size() >= want.size() &&
                                  have.compare(have.size() - want.size(), want.size(), want) ==
                                      0))) {
                out = d;
                return true;
            }
        }
        error = "no Mirics device with serial " + serial + " is connected";
        return false;
    }

    const std::string indexText = argValue(args, "index");
    std::size_t index = 0;
    if (!indexText.empty()) {
        char* end = nullptr;
        const long n = std::strtol(indexText.c_str(), &end, 10);
        if (end == indexText.c_str() || *end != '\0' || n < 0 ||
            static_cast<std::size_t>(n) >= ours.size()) {
            error = "there is no Mirics device at index " + indexText;
            return false;
        }
        index = static_cast<std::size_t>(n);
    }
    out = ours[index];
    return true;
}

bool MiriSdrSource::open(const std::string& args) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ != nullptr) {
        setError("this Mirics source already has a device open");
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
        setError(error.empty() ? std::string("could not open the Mirics device") : error);
        return false;
    }
    dev_ = std::move(dev);
    link_->dev = dev_.get();
    clearError();

    const msi2500::DeviceModel* model = msi2500::modelFor(info.vid, info.pid);
    plan_ = (model != nullptr && model->sdrPlayFlavour) ? msi001::Plan::SdrPlay
                                                        : msi001::Plan::Default;

    // --- QUIETEN IT FIRST ------------------------------------------------
    // This chip has NO IDENTITY REGISTERS to read back - there is no board id,
    // no firmware string and no serial to ask for, so an open cannot begin by
    // confirming who answered. What it can do instead is make sure nothing is
    // in flight: a device left streaming by whatever had it before us would
    // deliver somebody else's transfer as our first one, and a half-programmed
    // ADC would deliver it at the wrong rate.
    if (!commandLocked(msi2500::VendorRequest::StopStreaming, "stopping any stream in progress") ||
        !writeRegisterLocked(msi2500::kRegClockDivider, msi2500::kAdcStopValue,
                             "putting the ADC to sleep")) {
        dev_.reset();
        link_->dev = nullptr;
        return false;
    }

    for (const msi2500::RegSet& s : msi2500::adcInitSequence()) {
        if (!writeRegisterLocked(s.reg, s.val, "initialising the ADC")) {
            dev_.reset();
            link_->dev = nullptr;
            return false;
        }
    }

    // --- a KNOWN state ---------------------------------------------------
    const msi2500::RateSetting rate = msi2500::computeRate(kOpenSampleRateHz);
    stages_ = msi001::GainStages{};
    stages_.basebandReduction = kOpenBasebandReduction;
    bandwidth_ = msi001::bandwidthForRate(static_cast<double>(rate.rateHz));

    const bool configured =
        programRateLocked(rate, "setting the sample rate") &&
        programTuneLocked(kOpenFrequencyHz, "setting the centre frequency") &&
        programGainLocked("setting the gain");
    if (!configured) {
        dev_.reset();
        link_->dev = nullptr;
        return false;
    }
    format_ = rate.format;
    link_->format.store(static_cast<int>(rate.format), std::memory_order_relaxed);
    sampleRateHz_.store(static_cast<double>(rate.rateHz), std::memory_order_relaxed);
    centerFrequencyHz_.store(kOpenFrequencyHz, std::memory_order_relaxed);

    std::string label = model != nullptr ? model->label : "Mirics MSi2500";
    if (!info.serial.empty()) { label += " (serial " + info.serial + ")"; }
    setName("Mirics: " + label);
    openMirror_.store(true, std::memory_order_relaxed);

    core::diagLogf("mirisdr: opened %s - %.3f MS/s, %s packing, %s band, %.0f kHz filter",
                   label.c_str(), static_cast<double>(rate.rateHz) / 1e6,
                   rate.format == msi2500::Format::Bits16   ? "16-bit"
                   : rate.format == msi2500::Format::Bits12 ? "12-bit"
                   : rate.format == msi2500::Format::Bits10 ? "10-bit"
                                                            : "8-bit",
                   msi001::bandName(band_), msi001::bandwidthHz(bandwidth_) / 1e3);
    return true;
}

void MiriSdrSource::closeDevice() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
    if (dev_ != nullptr && !deviceDead()) {
        // Leave the device quiet and its antenna port unpowered: the next
        // application to open it inherits whatever we leave behind, exactly as
        // we inherited what came before. The bias-T goes off through the band
        // switch it shares a register with.
        biasT_.store(false, std::memory_order_relaxed);
        programBandSwitchLocked("switching the bias-T off");
        writeRegisterLocked(msi2500::kRegClockDivider, msi2500::kAdcStopValue,
                            "putting the ADC to sleep");
    }
    dev_.reset();
    link_->dev = nullptr;
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    setName("Mirics: (no device)");
}

msi001::Band MiriSdrSource::band() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return band_;
}

double MiriSdrSource::bandwidthHz() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return msi001::bandwidthHz(bandwidth_);
}

msi2500::Format MiriSdrSource::sampleFormat() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return format_;
}

// --- streaming ------------------------------------------------------------

bool MiriSdrSource::startStreamingLocked() {
    // ORDER MATTERS. The bulk ring is queued FIRST: a device told to stream
    // with nothing queued fills its own buffer and overruns before the host's
    // first read, which presents as a stream that starts corrupted and then
    // recovers - the hardest kind of fault to attribute later.
    if (!dev_->beginBulkStream(msi2500::kRxEndpoint, msi2500::kTransferBytes,
                               msi2500::kTransferCount)) {
        noteTransportFault("queueing the sample transfers", dev_->lastError());
        return false;
    }
    if (!commandLocked(msi2500::VendorRequest::StartStreaming, "starting the stream")) {
        dev_->endBulkStream();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(link_->waitMutex);
        link_->exited = false;
    }
    link_->format.store(static_cast<int>(format_), std::memory_order_relaxed);
    link_->run.store(true, std::memory_order_relaxed);
    reader_ = std::thread(&MiriSdrSource::readerThreadBody, link_);
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void MiriSdrSource::stopStreamingLocked() {
    if (!reader_.joinable() && !running_.load(std::memory_order_relaxed)) {
        // Nothing to stop, but the device may still be streaming from a
        // half-failed start; endBulkStream is idempotent and cheap.
        if (dev_ != nullptr) { dev_->endBulkStream(); }
        running_.store(false, std::memory_order_relaxed);
        return;
    }

    // THE ONE CONTROL TRANSFER THIS DRIVER SPENDS ON A TEARDOWN, and it takes
    // the SHORT bound: msi2500::kTeardownControlTimeout carries the arithmetic
    // and the reason a shorter one is safe here. The reference sleeps 20 ms
    // before this write and that sleep is deliberately NOT reproduced - a
    // sleep on the teardown thread buys nothing a bounded join does not
    // already provide.
    if (dev_ != nullptr && !deviceDead()) {
        commandLocked(msi2500::VendorRequest::StopStreaming, "stopping the stream",
                      msi2500::kTeardownControlTimeoutMs);
    }
    link_->run.store(false, std::memory_order_relaxed);
    link_->waitCv.notify_all();

    if (reader_.joinable()) {
        // A BOUNDED JOIN, not a plain one (see the file header). The reader
        // sets `exited` as its last act, so a flag still clear after
        // kReaderJoinWait means a thread parked somewhere it is not coming back
        // from - and waiting for it on the GUI thread would be the hang the
        // whole transport exists to avoid.
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

bool MiriSdrSource::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("start() called with no Mirics device open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running_.load(std::memory_order_relaxed)) { return true; }
    clearError();
    return startStreamingLocked();
}

void MiriSdrSource::stop() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
}

// --- the reader thread ----------------------------------------------------

void MiriSdrSource::readerThreadBody(std::shared_ptr<ReaderLink> link) {
    std::vector<std::uint8_t> raw(msi2500::kTransferBytes);
    std::vector<std::complex<float>> conv(msi2500::kMaxSamplesPerTransfer);
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
        const msi2500::Format format =
            static_cast<msi2500::Format>(link->format.load(std::memory_order_relaxed));
        const std::size_t samples =
            got > 0 ? msi2500::unpackTransfer(format, raw.data(), static_cast<std::size_t>(got),
                                              conv.data(), conv.size())
                    : 0;
        bool dropped = false;
        if (samples > 0) {
            const std::size_t written = link->ring.write(conv.data(), samples);
            if (written != samples) {
                // The host fell behind, not the radio. Counted rather than
                // silently tolerated: a spectrum with a gap in it and no number
                // anywhere is how a slow machine looks exactly like a broken
                // one.
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

void MiriSdrSource::noteRead(ReaderLink& link, int ret, std::size_t samples, bool dropped) {
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

std::string MiriSdrSource::healthLineLocked(ReaderLink& link) {
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

std::string MiriSdrSource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void MiriSdrSource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

std::uint64_t MiriSdrSource::droppedTransfers() const {
    return link_->dropped.load(std::memory_order_relaxed);
}

unsigned long long MiriSdrSource::readersAbandoned() {
    return g_readersAbandoned.load(std::memory_order_relaxed);
}

// --- read -----------------------------------------------------------------

std::size_t MiriSdrSource::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) { return 0; }
    std::size_t got = link_->ring.read(dst, n);
    if (got > 0) { return got; }
    if (faulted()) { return 0; }
    {
        // Bounded, and short. The pipeline's self-paced loop treats a zero as
        // "nothing yet" and backs off a millisecond of its own.
        std::unique_lock<std::mutex> lk(link_->waitMutex);
        link_->waitCv.wait_for(lk, kReadWait, [this] {
            return link_->ring.size() > 0 || !link_->run.load(std::memory_order_relaxed);
        });
    }
    got = link_->ring.read(dst, n);
    return got;
}

// --- rate, frequency, gains ----------------------------------------------

std::vector<double> MiriSdrSource::supportedSampleRatesHz() const {
    // A menu, not a constraint - see the header - and it deliberately steps
    // over the 8.064 to 9.216 MS/s window, which is the one packing whose bit
    // layout could not be derived exactly from the reference.
    return {2.0e6, 3.0e6, 4.0e6, 5.0e6, 6.0e6, 8.0e6, 10.0e6, 12.0e6};
}

bool MiriSdrSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setSampleRateHz() called with no Mirics device open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz > 0.0)) {  // negated compare so a NaN lands here
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    if (hz < msi2500::kMinSampleRateHz) {
        // REFUSED, not raised. Quietly giving a caller that asked for 1 MS/s a
        // third more bandwidth than it wanted would be a lie nothing on screen
        // could reveal.
        char buf[176];
        std::snprintf(buf, sizeof(buf),
                      "the Mirics tuner's lowest sample rate is %.1f MS/s; %.3f MS/s was refused",
                      msi2500::kMinSampleRateHz / 1e6, hz / 1e6);
        setError(buf);
        return false;
    }
    // Above the top the rate IS coerced, because a caller asking for more
    // bandwidth than the radio has can be given all of it and told so.
    if (hz > msi2500::kMaxSampleRateHz) {
        char buf[184];
        std::snprintf(buf, sizeof(buf),
                      "the Mirics tuner's highest sample rate is %.0f MS/s; %.3f MS/s was coerced "
                      "to it",
                      msi2500::kMaxSampleRateHz / 1e6, hz / 1e6);
        setError(buf);
    }

    const msi2500::RateSetting rate = msi2500::computeRate(hz);
    const bool wasRunning = running_.load(std::memory_order_relaxed);
    if (wasRunning) {
        // A QUIET RADIO for the change (see the header): streaming off, our
        // reader joined and the bulk ring torn down before the clock
        // underneath it moves, and then the ADC asleep while it moves.
        stopStreamingLocked();
        if (!writeRegisterLocked(msi2500::kRegClockDivider, msi2500::kAdcStopValue,
                                 "putting the ADC to sleep")) {
            return false;
        }
    }
    if (!programRateLocked(rate, "setting the sample rate")) {
        if (wasRunning && !deviceDead()) { startStreamingLocked(); }
        return false;
    }
    format_ = rate.format;
    link_->format.store(static_cast<int>(rate.format), std::memory_order_relaxed);
    sampleRateHz_.store(static_cast<double>(rate.rateHz), std::memory_order_relaxed);

    // THE FILTER LIVES IN THE TUNER, so a rate change is also a retune - and a
    // retune is also a gain write, because the band may have moved under it.
    // See the file header; neither of these is optional.
    bandwidth_ = msi001::bandwidthForRate(static_cast<double>(rate.rateHz));
    const double centre = centerFrequencyHz_.load(std::memory_order_relaxed);
    if (!programTuneLocked(centre, "setting the baseband filter") ||
        !programGainLocked("restoring the gain")) {
        if (wasRunning && !deviceDead()) { startStreamingLocked(); }
        return false;
    }

    if (wasRunning) { return startStreamingLocked(); }
    return true;
}

bool MiriSdrSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setCenterFrequencyHz() called with no Mirics device open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (!(hz >= msi001::kMinFrequencyHz && hz <= msi001::kMaxFrequencyHz)) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "the Mirics tuner covers %.3f MHz to %.0f MHz; %.6f MHz is outside that",
                      msi001::kMinFrequencyHz / 1e6, msi001::kMaxFrequencyHz / 1e6, hz / 1e6);
        setError(buf);
        return false;
    }
    if (!programTuneLocked(hz, "setting the centre frequency")) { return false; }
    // THE GAIN FOLLOWS THE TUNE, always. Crossing 50 MHz moves the tuner onto
    // an AM input where the low-noise amplifier is not in circuit and the
    // buffer that stands in for it is; the gain register's meaning changes
    // with it, so a tune that did not re-send it would leave the radio
    // programmed for the band it has just left.
    if (!programGainLocked("restoring the gain")) { return false; }
    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    return true;
}

bool MiriSdrSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    loHz = msi001::kMinFrequencyHz;
    hiHz = msi001::kMaxFrequencyHz;
    return true;
}

std::vector<GainInfo> MiriSdrSource::gains() const {
    return {
        GainInfo{kGainLna, 0.0, msi001::kLnaGainDb, msi001::kLnaGainDb},
        GainInfo{kGainMixer, 0.0, msi001::kMixerGainDb, msi001::kMixerGainDb},
        GainInfo{kGainBaseband, 0.0, msi001::kBasebandMaxDb, 1.0},
        // STEPS, not decibels - see the header. Its range is the register
        // field's, and what a step is worth depends on which AM port the band
        // plan selected.
        GainInfo{kGainAmBuffer, 0.0, static_cast<double>(msi001::kMixbufferSteps - 1), 1.0,
                 GainUnit::Steps},
    };
}

bool MiriSdrSource::setGainDb(const std::string& gainName, double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setGainDb() called with no Mirics device open");
        return false;
    }
    if (deviceDead()) { return false; }

    // CLAMPED AND ROUNDED, never refused (the DeviceSource contract). Each of
    // these becomes a REDUCTION, which counts the other way: no reduction is
    // full gain.
    if (gainName == kGainLna) {
        const double want = std::clamp(db, 0.0, msi001::kLnaGainDb);
        stages_.lnaReduction = want >= msi001::kLnaGainDb / 2.0 ? 0 : 1;
    } else if (gainName == kGainMixer) {
        const double want = std::clamp(db, 0.0, msi001::kMixerGainDb);
        stages_.mixerReduction = want >= msi001::kMixerGainDb / 2.0 ? 0 : 1;
    } else if (gainName == kGainBaseband) {
        const double want = std::clamp(db, 0.0, msi001::kBasebandMaxDb);
        stages_.basebandReduction =
            static_cast<int>(msi001::kBasebandMaxDb - std::floor(want + 0.5));
    } else if (gainName == kGainAmBuffer) {
        const double want = std::clamp(db, 0.0, static_cast<double>(msi001::kMixbufferSteps - 1));
        stages_.mixbufferReduction = static_cast<int>(std::floor(want + 0.5));
    } else {
        setError("the Mirics tuner has no gain called \"" + gainName + "\"");
        return false;
    }
    return programGainLocked("setting the gain");
}

double MiriSdrSource::gainDb(const std::string& gainName) const {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (gainName == kGainLna) {
        // ZERO ON THE AM INPUTS whatever was asked for, because the amplifier
        // is not in circuit there and computeGain() forces its bit clear. A
        // panel that showed 24 dB of it on medium wave would be showing a gain
        // the radio does not have.
        if (band_ == msi001::Band::Am1 || band_ == msi001::Band::Am2) { return 0.0; }
        return msi001::lnaGainDb(stages_);
    }
    if (gainName == kGainMixer) { return msi001::mixerGainDb(stages_); }
    if (gainName == kGainBaseband) { return msi001::basebandGainDb(stages_); }
    if (gainName == kGainAmBuffer) { return static_cast<double>(stages_.mixbufferReduction); }
    return 0.0;
}

bool MiriSdrSource::setAutoGain(bool on) {
    (void)on;
    setError("the Mirics tuner has no automatic gain control this driver can program");
    return false;
}

bool MiriSdrSource::setAntenna(const std::string& antennaName) {
    if (antennaName == "RX") { return true; }
    setError("the Mirics tuner has one receive port, \"RX\"");
    return false;
}

bool MiriSdrSource::setBiasT(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("setBiasT() called with no Mirics device open");
        return false;
    }
    if (deviceDead()) { return false; }
    const bool previous = biasT_.load(std::memory_order_relaxed);
    biasT_.store(on, std::memory_order_relaxed);
    if (!programBandSwitchLocked("switching the bias-T")) {
        biasT_.store(previous, std::memory_order_relaxed);
        return false;
    }
    return true;
}

}  // namespace cascade::source
