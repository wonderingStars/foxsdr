// Tests for core/driver_policy.hpp: the one-click driver allow-list as data and
// its pure lookup. Nothing is installed, downloaded or opened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/driver_policy.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "core/radio_setup.hpp"
#include "core/winusb_install.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"

using namespace cascade::core;
namespace dp = cascade::core::driverpolicy;

namespace {

std::string hex4(unsigned v) {
    char b[8];
    std::snprintf(b, sizeof b, "%04X", v & 0xffffu);
    return b;
}

}  // namespace

int main() {
    // Every known radio family has exactly one reviewed row; every row names a real family.
    {
        std::set<std::string> families;
        for (const auto& d : radiosetup::knownDevices()) { families.insert(d.family); }
        for (const std::string& f : families) { CHECK(dp::findPolicy(f) != nullptr); }
        std::set<std::string> seen;
        for (const auto& r : dp::policyTable()) {
            CHECK(families.count(r.family) == 1);
            CHECK(seen.insert(r.family).second);           // no duplicate rows
            CHECK(std::string(r.source).size() > 10);       // every row says where it was read
            CHECK(std::string(r.note).size() > 10);
            // Only a verified row may allow an install.
            if (r.action == dp::Action::InstallWinUsb) { CHECK(r.evidenceVerified); }
        }
    }

    // RTL-SDR: every id the native driver opens, interface 0 only.
    int rtlRows = 0;
    for (const auto& id : cascade::source::rtlSdrUsbIds()) {
        const dp::Decision d0 = dp::decide(id.vid, id.pid, 0);
        CHECK(d0.action == dp::Action::InstallWinUsb);
        CHECK(d0.evidenceVerified);
        CHECK(dp::decide(id.vid, id.pid, 1).action == dp::Action::NotAllowed);   // the IR interface
        CHECK(dp::decide(id.vid, id.pid, -1).action == dp::Action::NotAllowed);  // the composite parent
        ++rtlRows;
    }
    CHECK(rtlRows >= 2);

    // Agreement with the shipped installer's own check, for EVERY known id and
    // interface 0: the policy says install exactly when winusb::targetAllowed does.
    for (const auto& dev : radiosetup::knownDevices()) {
        const std::string hw = "USB\\VID_" + hex4(dev.vid) + "&PID_" + hex4(dev.pid) + "&MI_00";
        const std::string inst = hw + "\\7&38A180A5&0&0000";
        std::string why;
        const bool shipped = winusb::targetAllowed(inst, {hw}, why);
        const bool policy = dp::decide(dev.vid, dev.pid, 0).action == dp::Action::InstallWinUsb;
        CHECK(shipped == policy);
    }

    // Airspy: the vendor says no install is needed; HackRF: not allowed (no vendor description).
    {
        const dp::Decision a = dp::decide(0x1d50, 0x60a1, 0);
        CHECK(a.action == dp::Action::NoInstallNeeded);
        CHECK(a.evidenceVerified);
        CHECK(a.family == radiosetup::kFamilyAirspy);
        const dp::Decision h = dp::decide(0x1d50, 0x6089, 0);
        CHECK(h.action == dp::Action::NotAllowed);
        CHECK(!h.evidenceVerified);
        CHECK(dp::decide(0x1d50, 0x6089, -1).action == dp::Action::NotAllowed);
        // Vendor-installer families are never replaced.
        CHECK(dp::decide(0x1df7, 0x3030, 0).action == dp::Action::NotAllowed);  // RSPdx
        CHECK(dp::decide(0x2500, 0x0020, 0).action == dp::Action::NotAllowed);  // B200
    }

    // Fake and unknown ids: refused, with a reason, whatever the interface.
    {
        const std::pair<unsigned, unsigned> fakes[] = {{0x0000, 0x0000}, {0xffff, 0xffff}, {0x1234, 0x5678},
                                                      {0x0bda, 0x0000}, {0x04b4, 0x00f1}};
        for (const auto& f : fakes) {
            for (int iface = -1; iface <= 2; ++iface) {
                const dp::Decision d = dp::decide(static_cast<std::uint16_t>(f.first), static_cast<std::uint16_t>(f.second), iface);
                CHECK(d.action == dp::Action::NotAllowed);
                CHECK(d.family.empty());
                CHECK(!d.reason.empty());
            }
        }
    }

    CHECK(std::string(dp::actionName(dp::Action::InstallWinUsb)) == "install-winusb");
    CHECK(std::string(dp::actionName(dp::Action::NoInstallNeeded)) == "no-install-needed");
    CHECK(std::string(dp::actionName(dp::Action::NotAllowed)) == "not-allowed");
    return testSummary("test_driver_policy");
}
