// Tests for the vendor-line half of core/diag_log.{hpp,cpp}: the per-second
// limiter, the scrub, and the stderr capture.
//
// The capture is exercised IN THIS PROCESS: fd 2 and STD_ERROR_HANDLE are
// replaced with the pipe, lines are written through both the CRT and the
// Win32 handle, and the ring is polled for them. It is forced on
// (`evenIfConsole`) because under ctest stderr is a pipe and from a terminal
// it is a console, and the test has to mean the same thing in both. The
// decision function itself is only printed, never asserted, for the same
// reason - its answer depends on who launched the test.
//
// What this does NOT prove: that librtlsdr's or UHD's own fprintf reaches the
// pipe. Those use their own CRT instances; the reasoning for why they still
// land here is in diag_log.cpp, and only a bench with such a driver can check
// it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_log.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "test_check.hpp"

using cascade::core::DiagLog;
using cascade::core::LineRateLimiter;
using cascade::core::scrubVendorLine;

namespace fs = std::filesystem;

namespace {

int countContaining(const std::vector<std::string>& lines, const std::string& needle) {
    int n = 0;
    for (const std::string& l : lines) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

// Polls the ring for `needle` for up to `ms`. The reader is a thread; a
// line written a microsecond ago is not in the ring yet.
bool ringHas(const std::string& needle, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (countContaining(DiagLog::instance().ringSnapshot(), needle) > 0) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path scratchDir() {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    const fs::path dir = base / ("cascade-vendor-lines-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

}  // namespace

int main(int argc, char** argv) {
    // A probe mode for a person, not for ctest: launched in a console of its
    // own (Start-Process from PowerShell gives it one), it reports whether
    // the capture would decline that console. Under ctest it is never used.
    if (argc == 2 && std::strcmp(argv[1], "--probe-console") == 0) {
        std::printf("watched-console: %s\n", cascade::core::stderrIsWatchedConsole() ? "yes" : "no");
        std::printf("captured: %s\n", cascade::core::installStderrCapture(false) ? "yes" : "no");
        return 0;
    }

    // --- The limiter: N a second, the rest counted and reported once ---------
    {
        LineRateLimiter lim(3);
        std::uint64_t sup = 0;
        CHECK(lim.admit(0, sup) && sup == 0);
        CHECK(lim.admit(1, sup) && sup == 0);
        CHECK(lim.admit(2, sup) && sup == 0);
        CHECK(!lim.admit(3, sup) && sup == 0);
        CHECK(!lim.admit(999, sup) && sup == 0);
        // The next second admits again and hands back the count, once.
        CHECK(lim.admit(1000, sup));
        CHECK(sup == 2);
        CHECK(lim.admit(1001, sup));
        CHECK(sup == 0);
        // A second with nothing dropped reports nothing.
        CHECK(lim.admit(2000, sup));
        CHECK(sup == 0);
        // The default is the documented 20.
        LineRateLimiter def;
        int admitted = 0;
        for (int i = 0; i < 100; ++i) {
            if (def.admit(5000, sup)) { ++admitted; }
        }
        CHECK(admitted == 20);  // the documented default, pinned by behaviour
    }

    // --- The scrub -----------------------------------------------------------
    {
        // Serials, in the three spellings drivers use.
        CHECK(scrubVendorLine("Opening B200 serial=EDR04ZDB2 ...") ==
              "Opening B200 serial=<stripped> ...");
        CHECK(scrubVendorLine("Manufacturer: Realtek, Product Name: RTL2838UHIDIR, Serial: 00000001") ==
              "Manufacturer: Realtek, Product Name: RTL2838UHIDIR, Serial: <stripped>");
        CHECK(scrubVendorLine("device serial number 12345678 found") ==
              "device serial number <stripped> found");
        // Two on one line.
        CHECK(scrubVendorLine("serial=AAA1 and serial=BBB2") ==
              "serial=<stripped> and serial=<stripped>");
        // The word alone, with no value, is left alone.
        CHECK(scrubVendorLine("no serial") == "no serial");

        // Frequencies: digits masked on any line that mentions one.
        CHECK(scrubVendorLine("Setting center freq: 101100000") ==
              "Setting center freq: #########");
        // EVERY digit on such a line, the model number included: the model is
        // already in the context block, and a rule with exceptions is a rule
        // that leaks the day a driver formats a frequency the exception
        // happens to match.
        CHECK(scrubVendorLine("[INFO] [B200] Actual RX Freq: 145.500000 MHz...") ==
              "[INFO] [B###] Actual RX Freq: ###.###### MHz...");
        CHECK(scrubVendorLine("tuned to 7074000") == "tuned to #######");
        // Lines that name neither pass untouched - a sample rate, an error
        // code, a device index are the evidence a report is for.
        CHECK(scrubVendorLine("rtlsdr_read_async: dev_lost (-5)") ==
              "rtlsdr_read_async: dev_lost (-5)");
        CHECK(scrubVendorLine("Actual RX Rate: 2.400000 Msps") == "Actual RX Rate: 2.400000 Msps");
        CHECK(scrubVendorLine("Using device 0: Generic RTL2832U OEM") ==
              "Using device 0: Generic RTL2832U OEM");
        CHECK(scrubVendorLine("") == "");
    }

    // --- Uptime is a small non-negative number in a fresh process -------------
    {
        const std::uint64_t up = cascade::core::processUptimeSec();
        CHECK(up < 3600u);
    }

#if defined(_WIN32)
    // --- The stderr capture, in this process ----------------------------------
    {
        const fs::path dir = scratchDir();
        DiagLog& log = DiagLog::instance();
        log.resetForTest();
        log.configure(dir.string(), true);
        CHECK(log.fileEnabled());

        std::printf("watched-console before install: %s\n",
                    cascade::core::stderrIsWatchedConsole() ? "yes" : "no");
        CHECK(!cascade::core::stderrCaptureActive());
        CHECK(cascade::core::originalStderrHandle() == nullptr);

        CHECK(cascade::core::installStderrCapture(true));
        CHECK(cascade::core::stderrCaptureActive());
        // The capture announces itself in the log it feeds.
        CHECK(ringHas("diag: stderr capture on", 1000));

        // Through the CRT, the way librtlsdr and libusb write.
        std::fprintf(stderr, "rtlsdr_read_async: dev_lost\n");
        std::fflush(stderr);
        CHECK(ringHas("vendor: rtlsdr_read_async: dev_lost", 3000));

        // Through the Win32 std handle, the way a module with its own CRT
        // ends up writing after it reads STD_ERROR_HANDLE at load.
        const char kRaw[] = "usb bulk transfer failed\r\n";
        DWORD written = 0;
        CHECK(::WriteFile(::GetStdHandle(STD_ERROR_HANDLE), kRaw,
                          static_cast<DWORD>(sizeof(kRaw) - 1), &written, nullptr) != 0);
        CHECK(ringHas("vendor: usb bulk transfer failed", 3000));

        // The scrub is applied on this path too.
        std::fprintf(stderr, "[INFO] [B200] Actual RX Freq: 145.500000 MHz serial=EDR04ZDB2\n");
        CHECK(ringHas("vendor: [INFO] [B###] Actual RX Freq: ###.###### MHz serial=<stripped>",
                      3000));
        CHECK(countContaining(log.ringSnapshot(), "EDR04ZDB2") == 0);
        CHECK(countContaining(log.ringSnapshot(), "145.5") == 0);

        // A second install is a no-op: one more line arrives once, not twice.
        CHECK(cascade::core::installStderrCapture(true));
        std::fprintf(stderr, "second install probe\n");
        CHECK(ringHas("vendor: second install probe", 3000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(countContaining(log.ringSnapshot(), "vendor: second install probe") == 1);

        // A trailing CR is stripped, and an empty line is not a line.
        std::fprintf(stderr, "\r\n\r\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(countContaining(log.ringSnapshot(), "vendor: \r") == 0);
        {
            const std::vector<std::string> ring = log.ringSnapshot();
            for (const std::string& l : ring) {
                CHECK(l.find("vendor: ") == std::string::npos ||
                      l.size() > l.find("vendor: ") + 8);
            }
        }

        // The original stderr is remembered for the crash handler's one line,
        // and it is not the pipe.
        CHECK(cascade::core::originalStderrHandle() != ::GetStdHandle(STD_ERROR_HANDLE));

        // The file got the same lines the ring did.
        const std::string file = readFile(dir / "foxsdr.log");
        CHECK(file.find("vendor: rtlsdr_read_async: dev_lost") != std::string::npos);
        CHECK(file.find("vendor: usb bulk transfer failed") != std::string::npos);
        CHECK(file.find("EDR04ZDB2") == std::string::npos);

        log.configure(std::string(), false);
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
#else
    // Linux: the capture is deliberately not implemented, and says so.
    CHECK(!cascade::core::installStderrCapture(true));
    CHECK(!cascade::core::stderrCaptureActive());
#endif

    // stdout, not stderr, on purpose: stderr is the pipe now.
    return testSummary("test_vendor_lines");
}
