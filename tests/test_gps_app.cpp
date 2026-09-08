// test_gps_app.cpp - a GPS on a serial device becomes the receiver position,
// through the real application.
//
// THE FEATURE (beta tester, 0.86.0): "Read NMEA messages over a COM port.
// This way if you have a GPS receiver you can pull the location straight
// from there instead of typing it in." The parser (test_nmea), the port
// (test_serial_port) and the reader (test_gps_reader) each have a unit
// suite; this is the wiring test none of them can be - bytes written on a
// device end up as rxLatDeg/rxLonDeg in the config file the application
// writes back, having passed through the startup hook, the worker thread,
// the per-frame poll and applyReceiverPosition, in that order.
//
// THE DEVICE IS A NAMED PIPE. There is no COM port and no GPS on the bench
// this is written on, so the test is the GPS: it serves
// \\.\pipe\foxsdr-gps-<pid>, and the application is pointed at that name
// through FOXSDR_GPS_PORT. SerialPort::open passes any name beginning with
// a backslash through untouched and treats the comm-setup refusal a pipe
// gives as a warning, which is the seam that makes this possible (pinned in
// test_serial_port.cpp). The pipe is created OVERLAPPED so that the server
// thread can give up on a client that never comes - a broken hook would
// otherwise turn this into a 120 s ctest timeout rather than a named
// failure.
//
// THE POSITION IS ONE NO CONFIG COULD HOLD BY ACCIDENT: 53.48 N, 2.24 W,
// written by hand into the ddmm.mmmm fields (0.48 deg * 60 = 28.8000',
// 0.24 deg * 60 = 14.4000') and framed with nmeaFrame(); the test asserts
// nmeaChecksumValid on every sentence it sends, so a slip in the framing
// fails HERE, not as a mysterious "no fix" in the application.
//
// PRIVACY, END TO END. The diagnostic log rides inside crash reports that are
// uploaded, and PRIVACY.md promises a position is never sent. The unit test
// scans the log RING; this one scans the log FILE the application actually
// wrote, for the coordinate in decimal and in NMEA form and for any sentence
// text. The scan looks only at the message after the "hh:mm:ss.mmm level"
// prefix, because a wall-clock stamp such as 12:34:52.245 contains "2.24"
// and would fail the run for the time of day (the reader test hit exactly
// that with 0.127).
//
// Windows only: the application is built and run here, and named pipes are
// a Windows device. Elsewhere the test says so and passes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/config.hpp"
#include "core/nmea.hpp"
#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

#if defined(_WIN32)

constexpr double kLatDeg = 53.48;
constexpr double kLonDeg = -2.24;

fs::path scratchDir() {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
    return base / (std::string("cascade-gps-") + std::to_string(::GetCurrentProcessId()));
}

// One environment variable, set for the child and put back afterwards
// (tests/test_startup_state.cpp).
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        char buf[4096];
        const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
        had_ = n > 0 && n < sizeof(buf);
        if (had_) { old_.assign(buf, n); }
        ::SetEnvironmentVariableA(name, value.c_str());
    }
    ~ScopedEnv() { ::SetEnvironmentVariableA(name_, had_ ? old_.c_str() : nullptr); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    bool had_ = false;
    std::string old_;
};

std::wstring widen(const std::string& s) {
    std::wstring w;
    for (const char c : s) { w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c))); }
    return w;
}

// The sentences a receiver sends: two GGAs before it has a fix (quality 0,
// empty position - exactly what a cold receiver emits), then a GGA and an
// RMC carrying the fix. 53 deg 28.8000' N, 2 deg 14.4000' W.
std::vector<std::string> noFixSentences() {
    return {cascade::core::nmeaFrame("GPGGA,120000.00,,,,,0,00,99.99,,,,,,") + "\r\n",
            cascade::core::nmeaFrame("GPGGA,120001.00,,,,,0,00,99.99,,,,,,") + "\r\n"};
}

std::vector<std::string> fixSentences() {
    return {cascade::core::nmeaFrame("GPGGA,120002.00,5328.8000,N,00214.4000,W,1,08,1.2,45.0,M,"
                                     "48.0,M,,") +
                "\r\n",
            cascade::core::nmeaFrame("GPRMC,120002.00,A,5328.8000,N,00214.4000,W,0.0,0.0,080926,"
                                     ",,A") +
                "\r\n"};
}

// The GPS. Serves one byte-mode pipe: waits for the client (giving up when
// told to), writes `first` once at 100 ms intervals, then `repeat` over and
// over at the same cadence until the client goes away or the test says stop.
// The fix is re-sent continuously because a hidden-window --frames run may
// be faster than a vsync one, and the reader has to see it before the last
// frame; the application closes the pipe the moment it has an acceptable
// fix, at which point the next write fails and the server is done.
class PipeGps {
public:
    PipeGps(std::string name, std::vector<std::string> first, std::vector<std::string> repeat)
        : name_(std::move(name)), first_(std::move(first)), repeat_(std::move(repeat)) {}

    bool create() {
        pipe_ = ::CreateNamedPipeW(widen(name_).c_str(),
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 4096, 4096,
                                   0, nullptr);
        return pipe_ != INVALID_HANDLE_VALUE;
    }

    void serve() { thread_ = std::thread(&PipeGps::run, this); }

    void stop() {
        stop_ = true;
        if (thread_.joinable()) { thread_.join(); }
        if (pipe_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    ~PipeGps() { stop(); }

    bool connected() const { return connected_; }
    int written() const { return written_; }

private:
    // One overlapped operation, waited for in 50 ms slices so the stop flag
    // is honoured while the pipe is idle. Returns false when told to stop or
    // when the operation failed outright.
    bool waitOverlapped(OVERLAPPED& ov, DWORD lastError, DWORD* bytes) {
        if (lastError != ERROR_IO_PENDING) { return false; }
        for (;;) {
            const DWORD w = ::WaitForSingleObject(ov.hEvent, 50);
            if (w == WAIT_OBJECT_0) { break; }
            if (stop_) {
                ::CancelIoEx(pipe_, &ov);
                ::GetOverlappedResult(pipe_, &ov, bytes, TRUE);
                return false;
            }
        }
        return ::GetOverlappedResult(pipe_, &ov, bytes, FALSE) != 0;
    }

    bool write(const std::string& s) {
        OVERLAPPED ov{};
        ov.hEvent = event_;
        ::ResetEvent(event_);
        DWORD n = 0;
        if (::WriteFile(pipe_, s.data(), static_cast<DWORD>(s.size()), &n, &ov)) { return true; }
        return waitOverlapped(ov, ::GetLastError(), &n);
    }

    void run() {
        event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        OVERLAPPED ov{};
        ov.hEvent = event_;
        bool ok = ::ConnectNamedPipe(pipe_, &ov) != 0;
        if (!ok) {
            const DWORD err = ::GetLastError();
            DWORD ignored = 0;
            ok = (err == ERROR_PIPE_CONNECTED) || waitOverlapped(ov, err, &ignored);
        }
        if (ok) {
            connected_ = true;
            for (const std::string& s : first_) {
                if (stop_ || !write(s)) { ok = false; break; }
                ++written_;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            while (ok && !stop_) {
                for (const std::string& s : repeat_) {
                    if (stop_ || !write(s)) { ok = false; break; }
                    ++written_;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        }
        ::CloseHandle(event_);
        event_ = nullptr;
    }

    std::string name_;
    std::vector<std::string> first_;
    std::vector<std::string> repeat_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> written_{0};
};

// Runs the application for `frames` frames against `cfgPath`, the GPS hook
// pointed at `device`, and returns what it printed. Nothing of the run
// leaves the scratch directory: the config is the one named, the profile
// directories point into the scratch tree (so the user's own fitted plugins
// stay out of it and no log lands in the real %LOCALAPPDATA%), the
// diagnostics tree is redirected so the log file this test reads is the one
// this run wrote, and every network endpoint is a closed local port.
std::string runApp(const fs::path& dir, const fs::path& cfgPath, const std::string& device,
                   int frames) {
    const ScopedEnv cfg("CASCADE_CONFIG_TEST", cfgPath.string());
    const ScopedEnv appdata("APPDATA", (dir / "appdata").string());
    const ScopedEnv local("LOCALAPPDATA", (dir / "localappdata").string());
    const ScopedEnv diag("FOXSDR_DIAG_DIR", (dir / "diag").string());
    const ScopedEnv t("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9/");
    const ScopedEnv c("FOXSDR_CRASH_URL", "http://127.0.0.1:9/");
    const ScopedEnv u("FOXSDR_UPDATE_URL", "http://127.0.0.1:9/");
    const ScopedEnv r("FOXSDR_REPORTS_URL", "http://127.0.0.1:9/");
    const ScopedEnv port("FOXSDR_GPS_PORT", device);
    const ScopedEnv baud("FOXSDR_GPS_BAUD", "9600");
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
    const std::string cmd =
        "\"\"" + exe + "\" --frames " + std::to_string(frames) + " 2>&1\"";
    std::string out;
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    if (p != nullptr) { _pclose(p); }
    return out;
}

void writeConfig(const fs::path& cfgPath) {
    std::ofstream cfg(cfgPath, std::ios::binary | std::ios::trunc);
    // Diagnostics ON, because the log FILE is what the privacy half reads.
    cfg << "{\n  \"diagnosticsEnabled\": true\n}\n";
}

// The message halves of every line of the log this run wrote: everything
// after "hh:mm:ss.mmm level ".
std::vector<std::string> logMessages(const fs::path& dir) {
    std::vector<std::string> out;
    std::ifstream in(dir / "diag" / "logs" / "foxsdr.log", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t sp1 = line.find(' ');
        const std::size_t sp2 = sp1 == std::string::npos ? sp1 : line.find(' ', sp1 + 1);
        out.push_back(sp2 == std::string::npos ? line : line.substr(sp2 + 1));
    }
    return out;
}

bool anyContains(const std::vector<std::string>& lines, const char* needle) {
    for (const std::string& l : lines) {
        if (l.find(needle) != std::string::npos) { return true; }
    }
    return false;
}

#endif  // _WIN32

}  // namespace

int main() {
#if defined(_WIN32)
    const fs::path dir = scratchDir();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "appdata", ec);
    fs::create_directories(dir / "localappdata", ec);
    fs::create_directories(dir / "diag", ec);
    const std::string pid = std::to_string(::GetCurrentProcessId());

    // What the GPS will say, and that it says it correctly framed.
    const std::vector<std::string> quiet = noFixSentences();
    const std::vector<std::string> fix = fixSentences();
    for (const std::string& s : quiet) { CHECK(cascade::core::nmeaChecksumValid(s)); }
    for (const std::string& s : fix) { CHECK(cascade::core::nmeaChecksumValid(s)); }
    CHECK(fix[0].find(",5328.8000,N,00214.4000,W,1,") != std::string::npos);
    CHECK(fix[1].find(",A,5328.8000,N,00214.4000,W,") != std::string::npos);
    {
        // The hand-written fields agree with the parser about the position:
        // if they did not, "no fix" downstream would be the wrong diagnosis.
        cascade::core::NmeaFix parsed;
        CHECK(cascade::core::parseNmeaSentence(fix[0], parsed) == cascade::core::NmeaStatus::Ok);
        CHECK(parsed.valid);
        CHECK_NEAR(parsed.latDeg, kLatDeg, 1e-9);
        CHECK_NEAR(parsed.lonDeg, kLonDeg, 1e-9);
        cascade::core::NmeaFix none;
        CHECK(cascade::core::parseNmeaSentence(quiet[0], none) == cascade::core::NmeaStatus::Ok);
        CHECK(!none.valid);
    }

    // --- a receiver with a fix sets the position -------------------------
    {
        std::printf("--- fix over \\\\.\\pipe\\foxsdr-gps-%s ---\n", pid.c_str());
        const fs::path cfgPath = dir / "fix.json";
        writeConfig(cfgPath);
        {
            cascade::core::AppConfig before;
            std::string err;
            CHECK(cascade::core::ConfigStore::load(cfgPath.string(), before, err));
            CHECK(!before.rxPositionSet);
        }
        const std::string pipe = "\\\\.\\pipe\\foxsdr-gps-" + pid;
        PipeGps gps(pipe, quiet, fix);
        CHECK(gps.create());
        gps.serve();
        const std::string out = runApp(dir, cfgPath, pipe, 240);
        std::printf("%s", out.c_str());
        gps.stop();
        std::printf("gps server: connected=%d sentences written=%d\n", gps.connected() ? 1 : 0,
                    gps.written());
        CHECK(out.find("config applied") != std::string::npos);
        CHECK(out.find("rendered 240 frames") != std::string::npos);
        CHECK(gps.connected());
        CHECK(gps.written() >= 3);  // the two quiet ones and at least one fix

        cascade::core::AppConfig after;
        std::string err;
        CHECK(cascade::core::ConfigStore::load(cfgPath.string(), after, err));
        CHECK(err.empty());
        CHECK(after.rxPositionSet);
        CHECK_NEAR(after.rxLatDeg, kLatDeg, 1e-5);
        CHECK_NEAR(after.rxLonDeg, kLonDeg, 1e-5);
        // The hook never chooses a port on the user's behalf.
        CHECK(after.gpsPort.empty());

        // The log tells the story in counts and names, never in degrees.
        const std::vector<std::string> log = logMessages(dir);
        std::printf("log: %zu lines\n", log.size());
        for (const std::string& l : log) {
            if (l.find("gps:") != std::string::npos) { std::printf("  %s\n", l.c_str()); }
        }
        CHECK(anyContains(log, "gps: startup read requested by FOXSDR_GPS_PORT"));
        CHECK(anyContains(log, "gps: listening on"));
        CHECK(anyContains(log, "gps: fix after"));
        CHECK(anyContains(log, "gps: fix applied as the receiver position"));
        CHECK(!anyContains(log, "53.48"));
        CHECK(!anyContains(log, "5328.8"));
        CHECK(!anyContains(log, "-2.24"));
        CHECK(!anyContains(log, "214.4"));
        CHECK(!anyContains(log, "$GP"));
        CHECK(!anyContains(log, "GPGGA"));
        CHECK(!anyContains(log, "GPRMC"));
        // Nor the device's name: a pipe is a device PATH, and the same lines
        // with a typed file path in them would carry a user name into a
        // crash report. The log says what kind of thing it was and how long.
        CHECK(!anyContains(log, "foxsdr-gps-"));
        CHECK(!anyContains(log, "\\\\.\\pipe"));
        CHECK(anyContains(log, "gps: listening on (a typed device path,"));
        CHECK(anyContains(log, "FOXSDR_GPS_PORT on (a typed device path,"));
    }

    // --- a device nobody serves leaves the position unset ----------------
    {
        std::printf("--- unserved pipe ---\n");
        const fs::path cfgPath = dir / "nobody.json";
        writeConfig(cfgPath);
        // A fresh diagnostics tree, so the log read back is this run's alone.
        fs::remove_all(dir / "diag", ec);
        fs::create_directories(dir / "diag", ec);
        const std::string pipe = "\\\\.\\pipe\\foxsdr-gps-nobody-" + pid;
        const std::string out = runApp(dir, cfgPath, pipe, 3);
        std::printf("%s", out.c_str());
        CHECK(out.find("rendered 3 frames") != std::string::npos);

        cascade::core::AppConfig after;
        std::string err;
        CHECK(cascade::core::ConfigStore::load(cfgPath.string(), after, err));
        CHECK(!after.rxPositionSet);
        const std::vector<std::string> log = logMessages(dir);
        for (const std::string& l : log) {
            if (l.find("gps:") != std::string::npos) { std::printf("  %s\n", l.c_str()); }
        }
        CHECK(anyContains(log, "could not be opened"));
        CHECK(!anyContains(log, "gps: fix after"));
    }

    // --- the hook's value is sanitised like every other way into the reader --
    //
    // The GUI field and the config loader both run a port name through
    // sanitiseSerialPortName; the first build handed FOXSDR_GPS_PORT to the
    // log and the reader raw, so a trailing tab (a script that appended one,
    // a tester pasting from the README) reached the log verbatim and a
    // newline would have forged an extra log line. The trimmed name is what
    // is logged and asked for; a value that sanitises to nothing starts no
    // read and says so.
    {
        std::printf("--- FOXSDR_GPS_PORT with a trailing tab ---\n");
        const fs::path cfgPath = dir / "tab.json";
        writeConfig(cfgPath);
        fs::remove_all(dir / "diag", ec);
        fs::create_directories(dir / "diag", ec);
        // COM250: a port name this machine does not have (Windows assigns
        // COM1..COM256; testEnumerate in test_serial_port prints what exists).
        const std::string out = runApp(dir, cfgPath, "COM250 \t", 3);
        std::printf("%s", out.c_str());
        CHECK(out.find("rendered 3 frames") != std::string::npos);
        const std::vector<std::string> log = logMessages(dir);
        for (const std::string& l : log) {
            if (l.find("gps:") != std::string::npos) { std::printf("  %s\n", l.c_str()); }
        }
        CHECK(anyContains(log, "gps: startup read requested by FOXSDR_GPS_PORT on COM250 at 9600"));
        CHECK(anyContains(log, "gps: COM250 could not be opened"));
        // The raw value, space and tab, must appear nowhere.
        CHECK(!anyContains(log, "COM250 \t"));
        CHECK(!anyContains(log, "COM250\t"));
    }
    {
        std::printf("--- FOXSDR_GPS_PORT that names nothing ---\n");
        const fs::path cfgPath = dir / "nothing.json";
        writeConfig(cfgPath);
        fs::remove_all(dir / "diag", ec);
        fs::create_directories(dir / "diag", ec);
        const std::string out = runApp(dir, cfgPath, " \x01 ", 3);
        std::printf("%s", out.c_str());
        CHECK(out.find("rendered 3 frames") != std::string::npos);
        const std::vector<std::string> log = logMessages(dir);
        for (const std::string& l : log) {
            if (l.find("gps:") != std::string::npos) { std::printf("  %s\n", l.c_str()); }
        }
        CHECK(anyContains(log, "gps: FOXSDR_GPS_PORT names no usable port"));
        CHECK(!anyContains(log, "gps: startup read requested"));
        CHECK(!anyContains(log, "could not be opened: no port chosen"));
    }

    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
#else
    std::printf("test_gps_app: skipped (named pipes are Windows-only; a FIFO variant is "
                "future work)\n");
#endif
    return testSummary("test_gps_app");
}
