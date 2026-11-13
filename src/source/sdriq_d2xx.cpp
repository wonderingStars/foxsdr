// sdriq_d2xx.cpp - see sdriq_d2xx.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdriq_d2xx.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#include "source/ftd2xx_api.hpp"
#include "source/vendor_guard.hpp"
#include "source/vendor_apartment.hpp"
#include "source/vendor_lib.hpp"

namespace cascade::source {

namespace {

namespace ft = ftd2xx;

constexpr std::size_t kNameBytes = 256;     // what the library may write for one string
constexpr std::size_t kCanaryBytes = 64;    // bytes after it that the library must leave alone
constexpr unsigned char kCanary = 0xA5;
constexpr unsigned kMaxDevices = 64;
constexpr unsigned kWriteMs = 2000;
constexpr int kCloseWaitMs = 1500;  // plain ints: the shutdown-budget scan classifies named chrono waits per file
constexpr int kCloseStepMs = 5;  // (the wait itself is measured by the clock)

// A call into FTDI's code that cannot take the application down. False when a fault was absorbed.
template <class F>
bool guarded(F& f) {
    return callGuardingVendorFaults([](void* p) noexcept { (*static_cast<F*>(p))(); }, &f);
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

bool describesSdrIq(const std::string& description) {
    const std::string d = lower(description);
    return d.find("sdr-iq") != std::string::npos || d.find("sdriq") != std::string::npos;
}

std::string statusText(ft::Status s) {
    switch (s) {
        case ft::kInvalidHandle: return "invalid handle";
        case ft::kDeviceNotFound: return "device not found";
        case ft::kDeviceNotOpened: return "device not opened";
        case ft::kIoError: return "I/O error";
        case ft::kInsufficientResources: return "insufficient resources";
        case ft::kInvalidParameter: return "invalid parameter";
        default: return "error " + std::to_string(static_cast<unsigned long>(s));
    }
}

const char* const kNeedsSentence =
    "The SDR-IQ talks through FTDI's D2XX driver (ftd2xx.dll). ";
const char* const kInstallSentence =
    " Install FTDI's D2XX driver from FTDI's own web site (ftdichip.com): FoxSDR does not include it and does not download it.";

// ---------------------------------------------------------------------------------------------------------------
// the link
// ---------------------------------------------------------------------------------------------------------------
class D2xxLink final : public SdriqLink {
public:
    D2xxLink(std::shared_ptr<vendorlib::Library> lib, const ft::Api& api, ft::Handle h) : lib_(std::move(lib)), api_(api), h_(h) {}
    ~D2xxLink() override { close(); }

    bool write(const std::uint8_t* data, std::size_t n, std::string& error) override {
        if (n == 0) { return true; }
        Guard g(*this);
        if (!g.ok) { error = "the SDR-IQ link is closed"; return false; }
        std::size_t done = 0;
        int stalls = 0;
        while (done < n) {
            std::uint32_t put = 0;
            const std::uint32_t want = static_cast<std::uint32_t>(std::min<std::size_t>(n - done, 4096));
            ft::Status st = ft::kOtherError;
            auto call = [&]() noexcept { st = api_.Write(h_, const_cast<std::uint8_t*>(data + done), want, &put); };
            if (!guarded(call)) { faulted_.store(true); return fail(error, "the FTDI driver failed while writing to the SDR-IQ"); }
            if (st != ft::kOk) { return fail(error, "FTDI's driver could not write to the SDR-IQ (" + statusText(st) + ")"); }
            if (put > want) { return fail(error, "FTDI's driver reported more bytes written than were given"); }
            if (put == 0) {
                if (++stalls >= 3) { return fail(error, "the SDR-IQ did not accept the bytes (the write timed out)"); }
                continue;
            }
            done += put;
        }
        return true;
    }

    int read(std::uint8_t* dst, std::size_t cap, unsigned timeoutMs) override {
        if (cap == 0) { return 0; }
        if (intr_.load()) { return 0; }
        Guard g(*this);
        if (!g.ok || dead_.load()) { return -1; }
        // D2XX treats a timeout of 0 as "wait forever": never pass it.
        const unsigned ms = std::clamp(timeoutMs, 1u, 60000u);
        if (ms != readMs_) {
            ft::Status st = ft::kOtherError;
            auto call = [&]() noexcept { st = api_.SetTimeouts(h_, ms, kWriteMs); };
            if (!guarded(call)) { faulted_.store(true); return failRead(); }
            if (st != ft::kOk) { return failRead(); }
            readMs_ = ms;
        }
        std::uint32_t queued = 0;
        ft::Status st = ft::kOtherError;
        {
            auto call = [&]() noexcept { st = api_.GetQueueStatus(h_, &queued); };
            if (!guarded(call)) { faulted_.store(true); return failRead(); }
            if (st != ft::kOk) { return failRead(); }
        }
        // What is already queued is read at once; with nothing queued one byte is asked for, which returns with the first byte or at the timeout.
        const std::uint32_t want = queued > 0 ? static_cast<std::uint32_t>(std::min<std::size_t>(queued, cap)) : 1u;
        std::uint32_t got = 0;
        {
            auto call = [&]() noexcept { st = api_.Read(h_, dst, want, &got); };
            if (!guarded(call)) { faulted_.store(true); return failRead(); }
            if (st != ft::kOk) { return failRead(); }
        }
        if (got > want) { return failRead(); }  // a library that says it wrote more than it was asked for is not trusted
        return static_cast<int>(got);
    }

    void interrupt() override { intr_.store(true); }

    void close() override {
        bool expected = false;
        if (!closing_.compare_exchange_strong(expected, true)) { return; }
        intr_.store(true);
        // Reads end within their poll; wait (bounded, by the clock: a sleep is rounded up to the timer tick) for the calls in flight to leave the
        // library before it is closed under them.
        const std::int64_t deadline = vendorapt::nowMs() + kCloseWaitMs;
        while (inflight_.load() != 0 && vendorapt::nowMs() < deadline) { std::this_thread::sleep_for(std::chrono::milliseconds(kCloseStepMs)); }
        const bool idle = inflight_.load() == 0;
        // FT_Close is skipped only when the library has faulted (its state is not trusted); a failed device is still closed.
        if (h_ != nullptr && !faulted_.load()) {
            auto call = [&]() noexcept { api_.Close(h_); };
            if (!guarded(call)) { faulted_.store(true); }
        }
        h_ = nullptr;
        // A call that never left the library may still be inside it: the module is leaked rather than unloaded under it.
        if (!idle || faulted_.load()) {
            leaked_ = new std::shared_ptr<vendorlib::Library>(lib_);
        }
        lib_.reset();
    }

private:
    struct Guard {
        explicit Guard(D2xxLink& l) : l_(l) {
            l_.inflight_.fetch_add(1);
            ok = !l_.closing_.load();
            if (!ok) { l_.inflight_.fetch_sub(1); }
        }
        ~Guard() {
            if (ok) { l_.inflight_.fetch_sub(1); }
        }
        D2xxLink& l_;
        bool ok = false;
    };
    bool fail(std::string& error, const std::string& why) {
        dead_.store(true);
        error = why;
        return false;
    }
    int failRead() {
        dead_.store(true);
        return -1;
    }

    std::shared_ptr<vendorlib::Library> lib_;
    ft::Api api_;
    ft::Handle h_ = nullptr;
    std::atomic<bool> closing_{false}, intr_{false}, dead_{false}, faulted_{false};
    std::atomic<int> inflight_{0};
    unsigned readMs_ = 0;
    std::shared_ptr<vendorlib::Library>* leaked_ = nullptr;  // never freed on purpose
};

std::unique_ptr<SdriqLink> refuse(std::string& error, D2xxOpenInfo* info, const char* verdict, const std::string& sentence) {
    error = sentence;
    if (info != nullptr) { info->verdict = verdict; }
    return nullptr;
}

// One listed string, bounded and with a canary after the buffer. False when the library wrote past the buffer or left it unterminated.
bool listString(const ft::Api& api, unsigned index, std::uint32_t flag, std::string& out, bool& ok) {
    std::vector<unsigned char> buf(kNameBytes + kCanaryBytes, kCanary);
    std::memset(buf.data(), 0, kNameBytes);
    ft::Status st = ft::kOtherError;
    auto call = [&]() noexcept {
        st = api.ListDevices(reinterpret_cast<void*>(static_cast<std::uintptr_t>(index)), buf.data(), ft::kListByIndex | flag);
    };
    if (!guarded(call)) { ok = false; return false; }
    for (std::size_t i = kNameBytes; i < buf.size(); ++i) {
        if (buf[i] != kCanary) { ok = false; return false; }
    }
    if (st != ft::kOk) { ok = true; out.clear(); return false; }
    std::size_t n = 0;
    while (n < kNameBytes && buf[n] != 0) { ++n; }
    if (n == kNameBytes) { ok = false; return false; }
    out.clear();
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = buf[i];
        out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    ok = true;
    return true;
}

}  // namespace

std::unique_ptr<SdriqLink> openD2xxSdriq(const std::string& libraryWhere, const std::string& serialIn, std::string& error, D2xxOpenInfo* info) {
    if (info != nullptr) { *info = D2xxOpenInfo{}; }
    for (char ch : serialIn) {
        if (ch < 0x20 || ch > 0x7E) { return refuse(error, info, "bad-serial", "the serial number may use printable characters only"); }
    }
    // --- the library -------------------------------------------------------------------------------------------
    std::string path, why;
    for (const char* name : {"ftd2xx.dll", "ftd2xx64.dll"}) {
        path = vendorlib::findLibrary(libraryWhere, name, why);
        if (!path.empty()) { break; }
        // a file the person named is the same file under both names: stop after the first miss
        if (!libraryWhere.empty() && std::string(name) == "ftd2xx.dll" && why.find("does not exist") != std::string::npos) { break; }
    }
    if (path.empty()) {
        return refuse(error, info, vendorlib::verdictWord(vendorlib::Verdict::NotFound),
                      std::string(kNeedsSentence) + "FoxSDR did not find it" + (libraryWhere.empty() ? "." : " where you pointed.") + kInstallSentence);
    }
    auto lib = std::make_shared<vendorlib::Library>();
    const vendorlib::Check c = lib->open(path);
    if (!c.ok()) {
        return refuse(error, info, vendorlib::verdictWord(c.verdict), std::string(kNeedsSentence) + c.sentence + kInstallSentence);
    }
    std::vector<const char*> names(std::begin(ft::kExports), std::end(ft::kExports));
    std::vector<void*> fn;
    const std::string missing = vendorlib::resolveAll(*lib, names, fn);
    if (!missing.empty()) {
        return refuse(error, info, vendorlib::verdictWord(vendorlib::Verdict::SymbolMissing),
                      "That ftd2xx.dll does not export what FoxSDR needs (" + missing + "): it is not a D2XX library this build knows.");
    }
    ft::Api api;
    {
        std::size_t i = 0;
        api.ListDevices = reinterpret_cast<ft::ListDevicesFn>(fn[i++]);
        api.OpenEx = reinterpret_cast<ft::OpenExFn>(fn[i++]);
        api.Close = reinterpret_cast<ft::CloseFn>(fn[i++]);
        api.Read = reinterpret_cast<ft::ReadFn>(fn[i++]);
        api.Write = reinterpret_cast<ft::WriteFn>(fn[i++]);
        api.SetTimeouts = reinterpret_cast<ft::SetTimeoutsFn>(fn[i++]);
        api.Purge = reinterpret_cast<ft::PurgeFn>(fn[i++]);
        api.GetQueueStatus = reinterpret_cast<ft::GetQueueStatusFn>(fn[i++]);
    }

    // --- which device ---------------------------------------------------------------------------------------------
    std::string chosen = serialIn;
    if (chosen.empty()) {
        std::uint32_t count = 0;
        ft::Status st = ft::kOtherError;
        auto call = [&]() noexcept { st = api.ListDevices(&count, nullptr, ft::kListNumberOnly); };
        if (!guarded(call)) {
            return refuse(error, info, "bad-driver", "FTDI's D2XX driver failed while listing the USB devices; unplug the SDR-IQ, plug it in again and retry.");
        }
        if (st != ft::kOk || count == 0) {
            return refuse(error, info, "no-device",
                          "No SDR-IQ was found: FTDI's D2XX driver lists no FTDI USB device. Plug the SDR-IQ in and try again.");
        }
        if (count > kMaxDevices) { count = kMaxDevices; }
        if (info != nullptr) { info->devicesSeen = static_cast<int>(count); }
        for (unsigned i = 0; i < count && chosen.empty(); ++i) {
            std::string desc, ser;
            bool ok = true;
            listString(api, i, ft::kOpenByDescription, desc, ok);
            if (!ok) { return refuse(error, info, "bad-driver", "FTDI's D2XX driver wrote outside the buffer it was given; it is not used."); }
            if (!describesSdrIq(desc)) { continue; }
            if (!listString(api, i, ft::kOpenBySerialNumber, ser, ok) || ser.empty()) {
                if (!ok) { return refuse(error, info, "bad-driver", "FTDI's D2XX driver wrote outside the buffer it was given; it is not used."); }
                continue;
            }
            chosen = ser;
        }
        if (chosen.empty()) {
            return refuse(error, info, "no-sdriq",
                          "No SDR-IQ was found: FTDI's driver lists " + std::to_string(count) + " FTDI USB device" + (count == 1 ? "" : "s") +
                              ", none described as an SDR-IQ. If yours is listed under another name, give its serial number (sdriq=<serial>).");
        }
    }
    // --- open ---------------------------------------------------------------------------------------------------------
    std::vector<char> ser(chosen.begin(), chosen.end());
    ser.push_back('\0');
    ft::Handle h = nullptr;
    ft::Status st = ft::kOtherError;
    {
        auto call = [&]() noexcept { st = api.OpenEx(ser.data(), ft::kOpenBySerialNumber, &h); };
        if (!guarded(call)) {
            return refuse(error, info, "bad-driver", "FTDI's D2XX driver failed while opening the SDR-IQ; unplug it, plug it in again and retry.");
        }
    }
    if (st != ft::kOk || h == nullptr) {
        return refuse(error, info, "open-failed",
                      st == ft::kDeviceNotFound
                          ? std::string("No FTDI device with that serial number is connected.")
                          : "FTDI's driver could not open the SDR-IQ (" + statusText(st) + "): is another program using it?");
    }
    {   // bounded reads and writes, and an empty queue
        auto call = [&]() noexcept {
            st = api.SetTimeouts(h, 100, kWriteMs);
            if (st == ft::kOk) { st = api.Purge(h, ft::kPurgeRx | ft::kPurgeTx); }
        };
        if (!guarded(call) || st != ft::kOk) {
            auto closeIt = [&]() noexcept { api.Close(h); };
            guarded(closeIt);
            return refuse(error, info, "open-failed", "FTDI's driver opened the SDR-IQ but would not accept its settings (" + statusText(st) + ").");
        }
    }
    if (info != nullptr) { info->verdict = "ok"; }
    return std::make_unique<D2xxLink>(std::move(lib), api, h);
}

SdriqOpener d2xxSdriqOpener(const std::string& libraryWhere) {
    return [libraryWhere](const std::string& serial, std::string& error) { return openD2xxSdriq(libraryWhere, serial, error, nullptr); };
}

// ---------------------------------------------------------------------------------------------------------------------
// the needed-library data
// ---------------------------------------------------------------------------------------------------------------------
const needed::Info& ftd2xxNeededInfo() {
    static const needed::Info info = [] {
        needed::Info i;
        i.libraryId = "ftdi-d2xx";
        i.name = "FTDI D2XX driver";
        i.fileNames = {"ftd2xx.dll", "ftd2xx64.dll"};
        i.minVersion = "";  // none is known: the classic calls FoxSDR uses are the oldest ones; "too old" here means one of the eight is missing
        i.needs64Bit = true;
        i.why = "FoxSDR talks to the RFspace SDR-IQ through this driver; once it is installed the SDR-IQ can be chosen as a radio.";
        i.publisher = "FTDI";
        // The only address read in the publisher's own documentation (the D2XX Programmer's Guide names "our web site www.ftdichip.com"). The page of the
        // D2XX downloads on it was NOT read (FTDI's site refused the tool), so no deeper address is given: (unverified).
        i.officialAddress = "https://www.ftdichip.com";
        i.installKind = "installer";
        i.copyInFolder = "";
        i.steps = {
            "Open FTDI's web site with the button below and find the D2XX driver for Windows (the one for 64-bit Windows).",
            "Download it and run FTDI's installer (Windows may ask for your permission: that is FTDI's installer, not FoxSDR).",
            "Unplug the SDR-IQ and plug it in again.",
            "Come back here and press \"Check again\"; FoxSDR does not need to be restarted.",
        };
        i.licence = "(unverified: FTDI's driver licence was not read)";
        i.notSuppliedLine = "FoxSDR does not include or supply this driver.";
        i.fetchAllowed = false;
        return i;
    }();
    return info;
}

needed::Check checkFtd2xx(const std::string& libraryWhere) {
    const needed::Info& info = ftd2xxNeededInfo();
    needed::Check r;
    r.lookedIn = libraryWhere.empty() ? "the folders on your PATH (System32 is one of them)" : "the folder or file you chose";
    std::string path, why;
    for (const std::string& name : info.fileNames) {
        path = vendorlib::findLibrary(libraryWhere, name, why);
        if (!path.empty()) { r.fileName = name; break; }
        if (!libraryWhere.empty() && why.find("does not exist") != std::string::npos) { break; }
    }
    if (path.empty()) {
        r.state = needed::State::NotFound;
        r.sentence = "Not found: FoxSDR looked for ftd2xx.dll and ftd2xx64.dll in " + r.lookedIn + " and neither is there.";
        return r;
    }
    {
        const std::filesystem::path fp{std::u8string(path.begin(), path.end())};
        const std::u8string folder = fp.parent_path().u8string();
        r.foundFolder.assign(folder.begin(), folder.end());
        const std::u8string fname = fp.filename().u8string();
        r.fileName.assign(fname.begin(), fname.end());
    }
    r.version = needed::fileVersion(path);
    vendorlib::Library lib;
    const vendorlib::Check c = lib.open(path);
    if (!c.ok()) {
        switch (c.verdict) {
            case vendorlib::Verdict::Wrong32Bit:
                r.state = needed::State::Wrong32Bit;
                r.sentence = "Found " + r.fileName + ", but it is a 32-bit file and FoxSDR is 64-bit: install FTDI's 64-bit D2XX driver.";
                break;
            case vendorlib::Verdict::LoadFailed:
                r.state = needed::State::WouldNotLoad;
                r.sentence = "Found " + r.fileName + ", but it would not load: " + c.sentence;
                break;
            default:
                r.state = needed::State::NotALibrary;
                r.sentence = "Found a file called " + r.fileName + ", but it is not a library FoxSDR can use: " + c.sentence;
                break;
        }
        return r;
    }
    std::vector<const char*> names(std::begin(ft::kExports), std::end(ft::kExports));
    std::vector<void*> fn;
    r.missing = vendorlib::resolveAll(lib, names, fn);
    lib.close();
    if (!r.missing.empty()) {
        r.state = needed::State::TooOld;
        r.sentence = "Found " + r.fileName + (r.version.empty() ? "" : " (version " + r.version + ")") +
                     ", but it is too old or not FTDI's D2XX driver: it does not offer " + r.missing + ".";
        return r;
    }
    r.state = needed::State::Working;
    r.sentence = "Found and working: " + r.fileName + (r.version.empty() ? " (the file states no version)" : " (version " + r.version + ")") +
                 ", 64-bit, and it offers the calls FoxSDR needs.";
    return r;
}

}  // namespace cascade::source
