// Tests for core/winusb_install.hpp - the one-click driver install that
// replaces Zadig for an RTL-SDR.
//
// What can be proven without touching a driver: WHICH device the installer is
// allowed to touch (only interface 0 of an RTL2832U, judged from the device's
// own hardware ids, never the composite parent, never the IR interface, never
// another radio), that the elevated helper's arguments and result file cannot
// be confused or forged into a success, and that asking it to install onto a
// device that is not there changes nothing. The install itself needs an
// elevated process and a real dongle; it is verified by hand (docs/MANUAL.md,
// "Installing the RTL-SDR driver with one click").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/winusb_install.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace wu = cascade::core::winusb;

namespace {

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The bench dongle's interface 0, as Windows lists it (2026-10-24): code 28,
// no driver. Its hardware ids are the REV_ form first, then the plain one.
const char* kBenchInterface0 = "USB\\VID_0BDA&PID_2838&MI_00\\7&38A180A5&0&0000";
const std::vector<std::string> kBenchHwIds = {"USB\\VID_0BDA&PID_2838&REV_0100&MI_00",
                                              "USB\\VID_0BDA&PID_2838&MI_00"};

void testOnlyInterfaceZeroOfAnRtlIsAllowed() {
    std::string why;
    CHECK(wu::targetAllowed(kBenchInterface0, kBenchHwIds, why));
    // Case does not matter to Windows, so not to this either.
    CHECK(wu::targetAllowed("usb\\vid_0bda&pid_2838&mi_00\\7&38a180a5&0&0000",
                            {"usb\\vid_0bda&pid_2838&mi_00"}, why));
    // Another RTL2832U id the native driver opens.
    CHECK(wu::targetAllowed("USB\\VID_0BDA&PID_2832&MI_00\\7&1&0&0000",
                            {"USB\\VID_0BDA&PID_2832&MI_00"}, why));

    // THE COMPOSITE PARENT: binding it would take the whole dongle from usbccgp.
    CHECK(!wu::targetAllowed("USB\\VID_0BDA&PID_2838\\00000001",
                             {"USB\\VID_0BDA&PID_2838&REV_0100", "USB\\VID_0BDA&PID_2838"}, why));
    CHECK(contains(why, "interface 0"));
    // THE IR RECEIVER, interface 1.
    CHECK(!wu::targetAllowed("USB\\VID_0BDA&PID_2838&MI_01\\7&38A180A5&0&0001",
                             {"USB\\VID_0BDA&PID_2838&MI_01"}, why));
    // BOTH must say interface 0: an instance id naming interface 1 is refused
    // even beside hardware ids that name interface 0 (and the reverse below).
    CHECK(!wu::targetAllowed("USB\\VID_0BDA&PID_2838&MI_01\\7&38A180A5&0&0001", kBenchHwIds, why));
    CHECK(!wu::targetAllowed("USB\\VID_0BDA&PID_2838\\00000001", kBenchHwIds, why));
    // An instance id that says MI_00 on a device whose own ids say otherwise.
    CHECK(!wu::targetAllowed(kBenchInterface0, {"USB\\VID_0BDA&PID_2838&MI_01"}, why));
    CHECK(!wu::targetAllowed(kBenchInterface0, {}, why));
    // Interface 0 of a radio that is not an RTL-SDR (HackRF, Airspy), or of no
    // radio at all: not this installer's business.
    CHECK(!wu::targetAllowed("USB\\VID_1D50&PID_6089&MI_00\\1", {"USB\\VID_1D50&PID_6089&MI_00"},
                             why));
    CHECK(!wu::targetAllowed("USB\\VID_1D50&PID_60A1&MI_00\\1", {"USB\\VID_1D50&PID_60A1&MI_00"},
                             why));
    CHECK(!wu::targetAllowed("USB\\VID_046D&PID_C52B&MI_00\\1", {"USB\\VID_046D&PID_C52B&MI_00"},
                             why));
    // Not USB, empty, too long, or carrying a quote or a control character.
    CHECK(!wu::targetAllowed("PCI\\VEN_8086&DEV_1234\\3&1", {"USB\\VID_0BDA&PID_2838&MI_00"}, why));
    CHECK(!wu::targetAllowed("", kBenchHwIds, why));
    CHECK(!wu::targetAllowed(std::string("USB\\VID_0BDA&PID_2838&MI_00\\") + std::string(300, '1'),
                             kBenchHwIds, why));
    CHECK(!wu::targetAllowed("USB\\VID_0BDA&PID_2838&MI_00\\7\"&x", kBenchHwIds, why));
    CHECK(!wu::targetAllowed("USB\\VID_0BDA&PID_2838&MI_00\\7\n&x", kBenchHwIds, why));
    CHECK(!why.empty());
}

void testHelperArgumentsAreExact() {
    wu::HelperArgs a;
    std::string err;
    const char* ok[] = {"cascade", "--install-winusb", kBenchInterface0, "--result",
                        "C:\\Users\\x\\AppData\\Local\\Temp\\r.txt"};
    CHECK(wu::parseHelperArgs(5, ok, a, err));
    CHECK(a.instanceId == kBenchInterface0);
    CHECK(a.resultPath == "C:\\Users\\x\\AppData\\Local\\Temp\\r.txt");

    const char* missing[] = {"cascade", "--install-winusb", kBenchInterface0};
    CHECK(!wu::parseHelperArgs(3, missing, a, err));
    const char* extra[] = {"cascade", "--install-winusb", kBenchInterface0, "--result", "r.txt",
                           "--frames"};
    CHECK(!wu::parseHelperArgs(6, extra, a, err));
    const char* swapped[] = {"cascade", "--result", "r.txt", "--install-winusb", kBenchInterface0};
    CHECK(!wu::parseHelperArgs(5, swapped, a, err));
    const char* dashId[] = {"cascade", "--install-winusb", "--result", "--result", "r.txt"};
    CHECK(!wu::parseHelperArgs(5, dashId, a, err));
    const char* quoted[] = {"cascade", "--install-winusb", "USB\\X\"", "--result", "r.txt"};
    CHECK(!wu::parseHelperArgs(5, quoted, a, err));
    CHECK(!err.empty());
}

void testHelperParametersQuoteEveryArgument() {
    CHECK(wu::helperParameters(kBenchInterface0, "C:\\T\\r.txt") ==
          std::string("--install-winusb \"") + kBenchInterface0 + "\" --result \"C:\\T\\r.txt\"");
    // A path ending in a backslash would escape its closing quote: doubled.
    CHECK(wu::helperParameters(kBenchInterface0, "C:\\T\\") ==
          std::string("--install-winusb \"") + kBenchInterface0 + "\" --result \"C:\\T\\\\\"");
    CHECK(wu::helperParameters("a\"b", "r.txt").empty());
    CHECK(wu::helperParameters(kBenchInterface0, "r\n.txt").empty());
    CHECK(wu::helperParameters("", "r.txt").empty());
}

void testResultFileRoundTripsAndRefusesForgeries() {
    for (wu::Outcome o : {wu::Outcome::Installed, wu::Outcome::AlreadyWinUsb, wu::Outcome::Refused,
                          wu::Outcome::NotFound, wu::Outcome::NotAdmin, wu::Outcome::Cancelled,
                          wu::Outcome::Failed}) {
        wu::Result r;
        r.outcome = o;
        r.reboot = (o == wu::Outcome::Installed);
        r.detail = "step\nwith a newline";
        wu::Result back;
        CHECK(wu::parseResult(wu::formatResult(r), back));
        CHECK(back.outcome == o);
        CHECK(back.reboot == r.reboot);
        CHECK(back.detail == "step with a newline");
        CHECK(!wu::resultSentence(r).empty());
    }
    wu::Result x;
    CHECK(!wu::parseResult("", x));
    CHECK(!wu::parseResult("outcome=installed\n", x));                         // incomplete
    CHECK(!wu::parseResult("outcome=installd\nreboot=0\ndetail=\n", x));        // unknown word
    CHECK(!wu::parseResult("outcome=installed\nreboot=yes\ndetail=\n", x));     // bad flag
    CHECK(!wu::parseResult("outcome=installed\nreboot=0\ndetail=\nextra=1\n", x));
    CHECK(!wu::parseResult("outcome=installed\noutcome=failed\nreboot=0\n", x));
    CHECK(wu::parseResult("outcome=failed\r\nreboot=0\r\ndetail=x\r\n", x));      // CRLF is fine
    CHECK(x.outcome == wu::Outcome::Failed);
}

void testTheCancelledSentenceSaysNothingChanged() {
    wu::Result r;
    r.outcome = wu::Outcome::Cancelled;
    CHECK(contains(wu::resultSentence(r), "nothing was changed"));
    r.outcome = wu::Outcome::Failed;
    CHECK(contains(wu::resultSentence(r), "Zadig"));
}

// The helper asked about a device that is not on the bus changes nothing and
// says so - on Windows through SetupAPI for real (no rights are needed to fail
// to open a device), elsewhere as "not on this platform".
void testHelperOnAnAbsentDeviceChangesNothing() {
    const std::filesystem::path out =
        std::filesystem::temp_directory_path() / "foxsdr-test-winusb-result.txt";
    std::error_code ec;
    std::filesystem::remove(out, ec);
    const std::string outPath = out.string();
    const char* argv[] = {"cascade", "--install-winusb",
                          "USB\\VID_0BDA&PID_2838&MI_00\\9&NOSUCH&0&0000", "--result",
                          outPath.c_str()};
    const int rc = wu::runHelperMain(5, argv);
    CHECK(rc == 3);
    std::ifstream in(out, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    wu::Result r;
    CHECK(wu::parseResult(text, r));
#if defined(_WIN32)
    CHECK(r.outcome == wu::Outcome::NotFound);
#else
    CHECK(r.outcome == wu::Outcome::Failed);
#endif
    std::filesystem::remove(out, ec);

    const char* bad[] = {"cascade", "--install-winusb"};
    CHECK(wu::runHelperMain(2, bad) == 2);
}

}  // namespace

int main() {
    testOnlyInterfaceZeroOfAnRtlIsAllowed();
    testHelperArgumentsAreExact();
    testHelperParametersQuoteEveryArgument();
    testResultFileRoundTripsAndRefusesForgeries();
    testTheCancelledSentenceSaysNothingChanged();
    testHelperOnAnAbsentDeviceChangesNothing();
    return testSummary("test_winusb_install");
}
