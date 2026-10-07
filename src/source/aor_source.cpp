// The AOR digital-I/Q driver itself. See aor_source.hpp for what it is, what
// it was written from, and what has and has not been tested.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/aor_source.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/leak_on_purpose.hpp"
#include "core/serial_port.hpp"
#include "core/utf8_text.hpp"
#include "source/fx2_loader.hpp"

#if defined(_WIN32)
// clang-format off
#include <windows.h>
// clang-format on
#endif

namespace cascade::source {

using cascade::i18n::tr;

namespace {

std::atomic<unsigned long long> g_readersAbandoned{0};

// Where a freshly opened receiver is tuned. A FoxSDR choice, not AOR's: the
// document gives an example (81.3 MHz) but no default, and the application
// retunes to the user's frequency straight after open anyway.
constexpr double kDefaultCenterHz = 100.0e6;

// Per 0xA0 transfer while loading the firmware (see fx2_loader.hpp).
constexpr unsigned kFirmwareWriteTimeoutMs = 1000;

std::string lowered(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::string exeDirectory() {
    namespace fs = std::filesystem;
    fs::path exe;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) { exe = fs::path(std::wstring(buf, n)); }
#else
    std::error_code ec;
    const fs::path link = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) { exe = link; }
#endif
    return exe.empty() ? std::string() : exe.parent_path().string();
}

}  // namespace

// --- enumeration --------------------------------------------------------------

std::vector<cascade::usb::UsbId> aorUsbIds() { return {{aor::kIqVid, aor::kIqPid}}; }

std::vector<NativeDeviceInfo> aorDevicesFrom(const std::vector<cascade::usb::UsbDeviceInfo>& devices) {
    std::vector<NativeDeviceInfo> out;
    int index = 0;
    for (const cascade::usb::UsbDeviceInfo& d : devices) {
        if (d.vid != aor::kIqVid || d.pid != aor::kIqPid) { continue; }
        NativeDeviceInfo info;
        info.driver = "aor";
        // The model is not known until the control port has answered VR, which
        // is an open and never an enumeration (rule 1) - so the row names the
        // interface, not a model it has not been asked about.
        info.label = "AOR digital I/Q";
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

std::vector<NativeDeviceInfo> enumerateAor() {
    return aorDevicesFrom(cascade::usb::enumerateWinUsb(aorUsbIds()));
}

std::string aorFirmwarePath() {
    const std::string dir = exeDirectory();
    const std::filesystem::path base = dir.empty() ? std::filesystem::path() : std::filesystem::path(dir);
    return (base / "resources" / "firmware" / "aor" / "fx2fw.hex").string();
}

std::string aorUnboundAdvice(const std::string& what) {
    return cascade::core::formatText(
        tr("%s is plugged in but is not bound to WinUSB, so nothing can open it. It is an AOR "
           "digital I/Q interface (USB ID 08D0:A001), and AOR's own driver (AorAlpha) and WinUSB "
           "cannot both own it: run Zadig (zadig.akeo.ie), tick Options -> List All Devices, "
           "select it, choose WinUSB and click Replace Driver, then press Refresh."),
        what.c_str());
}

FirmwareStep firmwareDecision(const StreamProbe& probe) {
    // TODO(AOR question 1) - see the header. The ONLY place this is decided.
    if (probe.isoArmed && probe.startAccepted && probe.aligned) { return FirmwareStep::Stream; }
    if (probe.firmwareLoadedThisOpen) { return FirmwareStep::GiveUp; }
    return FirmwareStep::LoadFirmware;
}

// --- construction, errors, name -------------------------------------------------

AorSource::~AorSource() { closeDevice(); }

void AorSource::setTransportForTest(TestTransport t) {
    std::lock_guard<std::mutex> lk(devMutex_);
    fake_ = std::move(t);
    useFake_ = static_cast<bool>(fake_.open);
}

void AorSource::setErrorOn(ReaderLink& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void AorSource::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void AorSource::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
}

void AorSource::noteTransportFaultOn(ReaderLink& link, const char* what, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lk(link.errorMutex);
        if (!link.deviceDead) {
            link.lastError = std::string("the AOR I/Q interface stopped answering while ") + what +
                             (detail.empty() ? std::string() : (": " + detail)) +
                             "; unplug it and plug it back in";
            link.deadWhat = what;
        }
        link.faulted = true;
        link.deviceDead = true;
    }
    core::diagWarnf("aor: transfer failed while %s%s%s", what, detail.empty() ? "" : ": ",
                    detail.c_str());
}

void AorSource::noteTransportFault(const char* what, const std::string& detail) {
    noteTransportFaultOn(*link_, what, detail);
}

bool AorSource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool AorSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string AorSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deadWhat;
}

const char* AorSource::lastError() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(link_->errorMutex);
        snapshot = link_->lastError;
    }
    return snapshot.c_str();
}

void AorSource::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* AorSource::name() const {
    thread_local std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(nameMutex_);
        snapshot = name_;
    }
    return snapshot.c_str();
}

aor::Identity AorSource::identity() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return identity_;
}

std::string AorSource::controlPort() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return controlPort_;
}

bool AorSource::firmwareWasLoaded() const {
    std::lock_guard<std::mutex> lk(devMutex_);
    return firmwareLoaded_;
}

// --- transport helpers -------------------------------------------------------------

std::vector<cascade::usb::UsbDeviceInfo> AorSource::listDevices() {
    std::vector<cascade::usb::UsbDeviceInfo> all =
        useFake_ ? (fake_.list ? fake_.list() : std::vector<cascade::usb::UsbDeviceInfo>())
                 : cascade::usb::enumerateWinUsb(aorUsbIds());
    std::vector<cascade::usb::UsbDeviceInfo> ours;
    for (const auto& d : all) {
        if (d.vid == aor::kIqVid && d.pid == aor::kIqPid) { ours.push_back(d); }
    }
    return ours;
}

std::unique_ptr<cascade::usb::UsbDevice> AorSource::openUsb(const std::string& path,
                                                           std::string& error) {
    if (useFake_) { return fake_.open(path, error); }
    return cascade::usb::openWinUsb(path, error);
}

bool AorSource::resolveDevice(const std::string& args, cascade::usb::UsbDeviceInfo& out,
                              std::string& error) {
    const std::vector<cascade::usb::UsbDeviceInfo> ours = listDevices();
    if (ours.empty()) {
        // PRESENT BUT NOT OURS TO OPEN is its own sentence, never "not found":
        // on Windows the interface ships bound to AOR's AorAlpha.sys, which is
        // exactly the state a new owner is in.
        const std::vector<cascade::usb::UsbDeviceInfo> unbound =
            useFake_ ? (fake_.unbound ? fake_.unbound() : std::vector<cascade::usb::UsbDeviceInfo>())
                     : cascade::usb::enumerateUnbound(aorUsbIds());
        if (!unbound.empty()) {
            error = tr("The AOR receiver's I/Q interface (USB 08D0:A001) is plugged in but is not "
                       "bound to WinUSB - AOR's own driver (AorAlpha) and WinUSB cannot both own "
                       "it. Run Zadig, select the AOR I/Q interface, choose WinUSB and click "
                       "Replace Driver, then try again.");
            return false;
        }
        error = tr("No AOR I/Q interface (USB 08D0:A001) was found. Check that the receiver's I/Q "
                   "USB cable is connected and the receiver is switched on.");
        return false;
    }
    const std::string serial = argValue(args, "serial");
    if (!serial.empty()) {
        for (const auto& d : ours) {
            if (lowered(d.serial) == lowered(serial)) {
                out = d;
                return true;
            }
        }
        error = "no AOR I/Q interface with serial " + serial + " is connected";
        return false;
    }
    const std::string indexText = argValue(args, "index");
    std::size_t index = 0;
    if (!indexText.empty()) {
        char* end = nullptr;
        const long n = std::strtol(indexText.c_str(), &end, 10);
        if (end == indexText.c_str() || *end != '\0' || n < 0 ||
            static_cast<std::size_t>(n) >= ours.size()) {
            error = "there is no AOR I/Q interface at index " + indexText;
            return false;
        }
        index = static_cast<std::size_t>(n);
    }
    out = ours[index];
    return true;
}

bool AorSource::writeCommandLocked(const std::uint8_t* cmd, const char* what) {
    if (dev_ == nullptr) { return false; }
    const int n = dev_->writeBulk(aor::kCommandEndpoint, cmd, aor::kCommandBytes,
                                  static_cast<unsigned>(kCommandWriteWait.count()));
    if (n != static_cast<int>(aor::kCommandBytes)) {
        const std::string detail =
            dev_->lastError().empty() ? std::string("this USB transport cannot write bulk OUT")
                                      : dev_->lastError();
        core::diagWarnf("aor: %s failed: %s", what, detail.c_str());
        return false;
    }
    return true;
}

// --- the probe and the firmware ------------------------------------------------------

StreamProbe AorSource::probeStreamLocked() {
    StreamProbe p;
    p.firmwareLoadedThisOpen = firmwareLoaded_;
    if (!dev_->beginIsoStream(aor::kIqEndpoint, aor::kIsoPacketBytes, aor::kIsoPacketsPerTransfer,
                              aor::kIsoTransfers)) {
        core::diagLogf("aor: probe: isochronous endpoint 0x86 could not be armed: %s",
                       dev_->lastError().empty() ? "(transport has no isochronous support)"
                                                 : dev_->lastError().c_str());
        return p;
    }
    p.isoArmed = true;
    p.startAccepted = writeCommandLocked(aor::kStartCommand, "START (probe)");
    std::size_t completed = 0;
    std::size_t bytes = 0;
    if (p.startAccepted) {
        aor::Aligner aligner;
        std::vector<std::uint8_t> raw(std::max<std::size_t>(dev_->isoTransferBytes(), 1));
        std::vector<std::complex<float>> out(raw.size() / aor::kWordBytes + 8);
        const auto deadline = std::chrono::steady_clock::now() + kProbeWindow;
        while (!p.aligned) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) { break; }
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            cascade::usb::IsoTransferStats st;
            const auto r = dev_->readIso(raw.data(), raw.size(),
                                         static_cast<unsigned>(std::min<long long>(left, kIsoReadWait.count())),
                                         st);
            if (r == cascade::usb::UsbDevice::IsoRead::Failed) { break; }
            if (r == cascade::usb::UsbDevice::IsoRead::Timeout) { continue; }
            ++completed;
            if (completed == 1) { continue; }  // the startup transfer, discarded as in streaming
            bytes += st.bytes;
            const std::size_t cap = std::min(out.size(), aligner.maxSamplesFor(st.bytes));
            aligner.push(raw.data(), st.bytes, out.data(), cap);
            p.aligned = aligner.locks() > 0;
        }
        // STOP BEFORE the transfers are cancelled, as in streaming.
        writeCommandLocked(aor::kStopCommand, "STOP (probe)");
    }
    dev_->endIsoStream();
    core::diagLogf("aor: probe: armed %s, START %s, %zu transfers / %zu bytes, %s",
                   p.isoArmed ? "yes" : "no", p.startAccepted ? "accepted" : "refused", completed,
                   bytes, p.aligned ? "aligned - firmware is running" : "no alignment");
    return p;
}

bool AorSource::loadFirmwareLocked(cascade::usb::UsbDeviceInfo& info, std::string& error) {
    const std::string path =
        (useFake_ && !fake_.firmwarePath.empty()) ? fake_.firmwarePath : aorFirmwarePath();
    bool found = false;
    const fx2::HexImage image = fx2::readIntelHexFile(path, found);
    if (!found) {
        error = cascade::core::formatText(
            tr("The AOR I/Q interface needs AOR's FX2 firmware, and the firmware file is not "
               "installed: %s was not found. The receiver was not opened."),
            path.c_str());
        return false;
    }
    if (!image.valid) {
        error = cascade::core::formatText(
            tr("AOR's FX2 firmware file %s could not be used (%s). The receiver was not opened."),
            path.c_str(), image.error.c_str());
        return false;
    }
    std::string why;
    if (!fx2::loadImage(*dev_, image, kFirmwareWriteTimeoutMs, why)) {
        error = cascade::core::formatText(
            tr("Loading AOR's FX2 firmware into the I/Q interface failed (%s). Unplug the I/Q USB "
               "cable, plug it back in and try again."),
            why.c_str());
        return false;
    }
    firmwareLoaded_ = true;
    core::diagLogf("aor: loaded %zu bytes of FX2 firmware (%zu records) and released the CPU",
                   image.totalBytes, image.dataRecords);

    // Our handle goes BEFORE the wait: the interface is about to drop off the
    // bus, and a re-enumerated device must not find us still claiming it.
    const std::string oldPath = info.path;
    dev_.reset();
    link_->dev = nullptr;

    fx2::ReenumerationTiming timing;
    timing.settle = kFirmwareSettle;
    timing.budget = kFirmwareReenumerateBudget;
    timing.poll = kFirmwarePollInterval;
    const auto sleeper = (useFake_ && fake_.sleep)
                             ? fake_.sleep
                             : std::function<void(std::chrono::milliseconds)>(
                                   [](std::chrono::milliseconds m) { std::this_thread::sleep_for(m); });
    const fx2::Reenumeration r =
        fx2::awaitReenumeration(oldPath, [this]() { return listDevices(); }, sleeper, timing, info);
    if (r == fx2::Reenumeration::Gone) {
        error = cascade::core::formatText(
            tr("The AOR I/Q interface did not come back within %d seconds after its firmware was "
               "loaded. Unplug the I/Q USB cable, plug it back in and try again."),
            static_cast<int>(kFirmwareReenumerateBudget.count() / 1000));
        return false;
    }
    core::diagLogf("aor: after the firmware load the interface %s",
                   r == fx2::Reenumeration::Returned ? "re-enumerated"
                                                     : "stayed listed at its old path");
    std::string openError;
    dev_ = openUsb(info.path, openError);
    if (dev_ == nullptr) {
        error = openError.empty() ? std::string("could not reopen the AOR I/Q interface") : openError;
        return false;
    }
    link_->dev = dev_.get();
    return true;
}

// --- open / close ----------------------------------------------------------------------

bool AorSource::open(const std::string& args) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ != nullptr) {
        setError("this AOR source already has a device open");
        return false;
    }
    clearError();
    firmwareLoaded_ = false;

    cascade::usb::UsbDeviceInfo info;
    std::string error;
    if (!resolveDevice(args, info, error)) {
        core::diagLogf("aor: open refused: %s", error.c_str());
        setError(error);
        return false;
    }
    dev_ = openUsb(info.path, error);
    if (dev_ == nullptr) {
        setError(error.empty() ? std::string("could not open the AOR I/Q interface") : error);
        return false;
    }
    link_->dev = dev_.get();
    // A port named in the args ("control=COM5") is text anybody could have
    // typed - a path, say - and the reasons below name it as given. The
    // screen keeps it; the log names it the way every port is logged
    // (loggableSerialPortName: a port's name, or the kind and length of
    // anything else), because the log rides into crash reports.
    const std::string requestedPort = argValue(args, "control");
    const auto giveUp = [this, requestedPort](const std::string& why) {
        std::string logged = why;
        if (!requestedPort.empty()) {
            const std::string safe = cascade::core::loggableSerialPortName(requestedPort);
            for (std::size_t at = logged.find(requestedPort); at != std::string::npos;
                 at = logged.find(requestedPort, at + safe.size())) {
                logged.replace(at, requestedPort.size(), safe);
            }
        }
        core::diagLogf("aor: open abandoned: %s", logged.c_str());
        setError(why);
        dev_.reset();  // releases interface 0
        link_->dev = nullptr;
        control_.reset();
        return false;
    };

    // 1. THE CONTROL PORT, paired by what it answers - before anything is
    //    written into the I/Q interface, so a missing receiver costs nothing.
    const std::vector<std::string> candidates =
        useFake_ ? (fake_.controlPorts ? fake_.controlPorts() : std::vector<std::string>())
                 : aor::ftdiControlPortCandidates();
    const aor::LinkOpener opener =
        (useFake_ && fake_.openControl) ? fake_.openControl : aor::LinkOpener(&aor::openSerialControlLink);
    const aor::Pairing pairing = aor::pairControlPort(candidates, requestedPort, opener);
    for (const std::string& t : pairing.tried) { core::diagLogf("aor: control port %s", t.c_str()); }
    if (!pairing.ok) { return giveUp(pairing.error); }

    // 2. FIRMWARE, only if the interface does not already stream.
    for (;;) {
        const StreamProbe probe = probeStreamLocked();
        const FirmwareStep step = firmwareDecision(probe);
        if (step == FirmwareStep::Stream) { break; }
        if (step == FirmwareStep::GiveUp) {
            return giveUp(tr("The AOR I/Q interface did not start streaming, even after its "
                             "firmware was loaded. Unplug the I/Q USB cable, plug it back in and "
                             "try again."));
        }
        if (!loadFirmwareLocked(info, error)) { return giveUp(error); }
    }

    // 3. THE DOCUMENTED CONTROL START-UP on the paired port, then the default
    //    frequency.
    control_ = opener(pairing.port, error);
    if (control_ == nullptr) { return giveUp(error); }
    aor::ControlSession session(*control_);
    aor::Identity id;
    if (!session.initialise(id, error)) { return giveUp(error); }
    if (!session.tune(kDefaultCenterHz, error)) { return giveUp(error); }

    identity_ = id;
    controlPort_ = pairing.port;
    centerFrequencyHz_.store(kDefaultCenterHz, std::memory_order_relaxed);
    sampleRateHz_.store(aor::kSampleRateHz, std::memory_order_relaxed);
    std::string label = std::string("AOR ") + aor::modelName(id.model);
    if (!id.verified) { label += " (unverified model)"; }
    setName(label);
    openMirror_.store(true, std::memory_order_relaxed);
    core::diagLogf("aor: opened %s - VR \"%s\", control on %s, I/Q at %s%s", label.c_str(),
                   id.reply.c_str(), cascade::core::loggableSerialPortName(pairing.port).c_str(),
                   aor::kSampleRateHz == 1125000.0 ? "1.125 MS/s" : "?",
                   firmwareLoaded_ ? ", firmware loaded by this open" : "");
    return true;
}

void AorSource::closeDevice() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
    const bool abandoned = dev_ == nullptr && link_->dev != nullptr;
    dev_.reset();  // the transport releases interface 0 as it closes
    if (!abandoned) { link_->dev = nullptr; }
    control_.reset();
    openMirror_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
    sampleRateHz_.store(0.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(0.0, std::memory_order_relaxed);
    setName("AOR: (no device)");
}

// --- streaming -------------------------------------------------------------------------

bool AorSource::startStreamingLocked() {
    // THE ORDER. The AOR document's reference order is "prepare iso
    // transfers, send START, submit transfers". This driver prepares AND
    // submits (arms) the transfers first and sends START after, which is the
    // order the task asks for and the one every other native driver here
    // keeps: an isochronous IN transfer armed before the device has anything
    // to send just completes with empty packets, whereas a START with nothing
    // armed loses whatever the interface sends before the first submission.
    link_->aligner.reset();
    if (!dev_->beginIsoStream(aor::kIqEndpoint, aor::kIsoPacketBytes, aor::kIsoPacketsPerTransfer,
                              aor::kIsoTransfers)) {
        noteTransportFault("arming the isochronous transfers", dev_->lastError());
        return false;
    }
    if (!writeCommandLocked(aor::kStartCommand, "START")) {
        dev_->endIsoStream();
        noteTransportFault("sending START", dev_->lastError());
        return false;
    }
    link_->transfers.store(0);
    link_->discarded.store(0);
    link_->losses.store(0);
    {
        std::lock_guard<std::mutex> lk(link_->waitMutex);
        link_->exited = false;
    }
    link_->run.store(true, std::memory_order_relaxed);
    reader_ = std::thread(&AorSource::readerThreadBody, link_);
    running_.store(true, std::memory_order_relaxed);
    core::diagLogf("aor: streaming - %zu-byte packet slots, %zu per transfer, %zu transfers",
                   dev_->isoPacketBytes(), aor::kIsoPacketsPerTransfer, aor::kIsoTransfers);
    return true;
}

void AorSource::stopStreamingLocked() {
    if (!reader_.joinable() && !running_.load(std::memory_order_relaxed)) {
        if (dev_ != nullptr) { dev_->endIsoStream(); }
        running_.store(false, std::memory_order_relaxed);
        return;
    }
    // STOP BEFORE CANCELLING the transfers (AOR document, "Start/stop").
    if (dev_ != nullptr && !deviceDead()) { writeCommandLocked(aor::kStopCommand, "STOP"); }
    link_->run.store(false, std::memory_order_relaxed);
    link_->waitCv.notify_all();
    if (reader_.joinable()) {
        bool exited = false;
        {
            std::unique_lock<std::mutex> lk(link_->waitMutex);
            exited = link_->waitCv.wait_for(lk, kReaderJoinWait, [this] { return link_->exited; });
        }
        if (exited) {
            reader_.join();
        } else {
            g_readersAbandoned.fetch_add(1, std::memory_order_relaxed);
            noteTransportFault("waiting for the sample reader to stop",
                               "the reader did not return; the interface is left to the operating "
                               "system and FoxSDR must be restarted to use it again");
            reader_.detach();
            // Marked for LeakSanitizer (core/leak_on_purpose.hpp).
            cascade::core::leakOnPurpose(dev_.release());
            running_.store(false, std::memory_order_relaxed);
            return;
        }
    }
    if (dev_ != nullptr) { dev_->endIsoStream(); }
    running_.store(false, std::memory_order_relaxed);
    core::diagLogf("aor: stopped - %llu transfers, first %llu discarded, %llu alignment losses",
                   static_cast<unsigned long long>(link_->transfers.load()),
                   static_cast<unsigned long long>(link_->discarded.load()),
                   static_cast<unsigned long long>(link_->losses.load()));
}

bool AorSource::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (dev_ == nullptr) {
        setError("start() called with no AOR receiver open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running_.load(std::memory_order_relaxed)) { return true; }
    clearError();
    return startStreamingLocked();
}

void AorSource::stop() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
}

void AorSource::readerThreadBody(std::shared_ptr<ReaderLink> link) {
    std::vector<std::uint8_t> raw(std::max<std::size_t>(link->dev->isoTransferBytes(), 1));
    // Room for a whole transfer plus the aligner's carried tail.
    std::vector<std::complex<float>> conv(raw.size() / aor::kWordBytes + 8);
    const unsigned timeoutMs = static_cast<unsigned>(kIsoReadWait.count());
    bool first = true;
    while (link->run.load(std::memory_order_relaxed)) {
        cascade::usb::IsoTransferStats st;
        const auto r = link->dev->readIso(raw.data(), raw.size(), timeoutMs, st);
        if (!link->run.load(std::memory_order_relaxed)) { break; }
        if (r == cascade::usb::UsbDevice::IsoRead::Failed) {
            noteRead(*link, false, 0, false, true);
            noteTransportFaultOn(*link, "reading samples", link->dev->lastError());
            break;
        }
        if (r == cascade::usb::UsbDevice::IsoRead::Timeout) {
            noteRead(*link, false, 0, false, false);
            continue;
        }
        link->transfers.fetch_add(1, std::memory_order_relaxed);
        if (first) {
            // THE AOR DOCUMENT'S STARTUP NOTE: the reference implementation
            // discards the first completed raw transfer. So does this one.
            first = false;
            link->discarded.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const std::uint64_t lossesBefore = link->aligner.losses();
        const std::size_t cap = std::min(conv.size(), link->aligner.maxSamplesFor(st.bytes));
        const std::size_t samples = link->aligner.push(raw.data(), st.bytes, conv.data(), cap);
        link->losses.fetch_add(link->aligner.losses() - lossesBefore, std::memory_order_relaxed);
        bool dropped = false;
        if (samples > 0) {
            const std::size_t written = link->ring.write(conv.data(), samples);
            if (written != samples) {
                dropped = true;
                link->dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        noteRead(*link, true, samples, dropped, false);
        if (samples == 0) { continue; }
        {
            std::lock_guard<std::mutex> lk(link->waitMutex);
        }
        link->waitCv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(link->waitMutex);
        link->run.store(false, std::memory_order_relaxed);
        link->exited = true;
    }
    link->waitCv.notify_all();
}

void AorSource::noteRead(ReaderLink& link, bool completed, std::size_t samples, bool dropped,
                         bool failed) {
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
        if (completed && samples > 0) {
            ++h.withSamples;
            h.samples += samples;
            const auto gap =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - h.lastSamples).count();
            if (gap > h.longestGapMs) { h.longestGapMs = gap; }
            h.lastSamples = now;
        } else if (failed) {
            ++h.errors;
        } else if (!completed) {
            ++h.timeouts;
        }
        if (dropped) { ++h.overflows; }
        if (now - h.windowStart < link.healthWindow) { return; }
        const bool nominal =
            h.timeouts == 0 && h.overflows == 0 && h.errors == 0 && h.longestGapMs < 250;
        const bool firstLine = !link.healthEverWritten;
        warn = h.errors > 0 || h.longestGapMs >= 1000;
        line = healthLineLocked(link);
        if (line.empty()) { return; }
        if (!(warn || firstLine || !nominal)) { return; }
        link.healthEverWritten = true;
    }
    if (warn) {
        core::diagWarnf("%s", line.c_str());
    } else {
        core::diagLogf("%s", line.c_str());
    }
}

std::string AorSource::healthLineLocked(ReaderLink& link) {
    StreamHealth& h = link.health;
    if (!h.windowOpen || h.reads == 0) { return std::string(); }
    const auto now = std::chrono::steady_clock::now();
    const auto openGap =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - h.lastSamples).count();
    if (openGap > h.longestGapMs) { h.longestGapMs = openGap; }
    const auto windowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - h.windowStart).count();
    char buf[192];
    // The same format every other source writes (SoapySource::streamHealthLine).
    std::snprintf(buf, sizeof(buf),
                  "source: stream health - reads %llu, with samples %llu, timeouts %llu, "
                  "overflows %llu, errors %llu, longest gap %lld ms, %llu samples in %lld s",
                  static_cast<unsigned long long>(h.reads),
                  static_cast<unsigned long long>(h.withSamples),
                  static_cast<unsigned long long>(h.timeouts),
                  static_cast<unsigned long long>(h.overflows),
                  static_cast<unsigned long long>(h.errors), static_cast<long long>(h.longestGapMs),
                  static_cast<unsigned long long>(h.samples),
                  static_cast<long long>((windowMs + 500) / 1000));
    h = StreamHealth{};
    return std::string(buf);
}

std::string AorSource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void AorSource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

unsigned long long AorSource::readersAbandoned() {
    return g_readersAbandoned.load(std::memory_order_relaxed);
}

std::size_t AorSource::read(std::complex<float>* dst, std::size_t n) {
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
    return link_->ring.read(dst, n);
}

// --- rate, frequency, and the controls there are none of --------------------------------

bool AorSource::setSampleRateHz(double hz) {
    if (!(hz > 0.0)) {
        setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    if (std::fabs(hz - aor::kSampleRateHz) > 0.5) {
        // COERCED, and said: the interface has exactly one rate.
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "an AOR digital I/Q interface runs at 1125.000 kS/s only; %.3f kS/s is not "
                      "available",
                      hz / 1e3);
        setError(buf);
    }
    return true;
}

bool AorSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (control_ == nullptr) {
        setError("setCenterFrequencyHz() called with no AOR receiver open");
        return false;
    }
    aor::ControlSession session(*control_);
    std::string error;
    if (!session.tune(hz, error)) {
        setError(error);
        return false;
    }
    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    return true;
}

bool AorSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    (void)loHz;
    (void)hiHz;
    return false;
}

bool AorSource::setGainDb(const std::string& gainName, double) {
    setError("an AOR digital I/Q interface has no gain called \"" + gainName + "\"");
    return false;
}

bool AorSource::setAutoGain(bool on) {
    if (!on) { return true; }
    setError("an AOR digital I/Q interface has no automatic gain control FoxSDR can switch");
    return false;
}

bool AorSource::setAntenna(const std::string& antennaName) {
    if (antennaName == "RX") { return true; }
    setError("an AOR digital I/Q interface has one receive path, \"RX\"");
    return false;
}

}  // namespace cascade::source
