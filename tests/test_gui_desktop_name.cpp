// test_gui_desktop_name.cpp - the Linux half of the gui desktop guard (tests/gui_desktop_guard.hpp, linuxDesktopName + judge): which display counts as one nobody is sitting at.
// A pure function over three environment values, so it runs on every platform (test_gui_desktop_guard, which drives the Windows branch, is Windows-only).
//
// The rule it holds: only the display `xvfb-run` makes is proven virtual; a Wayland session, a real X display, an Xvfb nobody here started and "no display at all" are all refused,
// so a gui-labelled test can never open a window on a person's real desktop by accident.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>

#include "gui_desktop_guard.hpp"
#include "test_check.hpp"

using cascade::testguard::judge;
using cascade::testguard::linuxDesktopName;

namespace {

bool allowed(const char* display, const char* wayland, const char* xauth) { return judge(linuxDesktopName(display, wayland, xauth), nullptr).allowed; }

}  // namespace

int main() {
    // what xvfb-run sets (xvfb-run -a: DISPLAY=:99 XAUTHORITY=/tmp/xvfb-run.XXXXXX/Xauthority): allowed, and the name says which display
    CHECK(allowed(":99", nullptr, "/tmp/xvfb-run.VfpBVU/Xauthority"));
    CHECK(linuxDesktopName(":99", nullptr, "/tmp/xvfb-run.VfpBVU/Xauthority") == "xvfb-run:99");
    CHECK(allowed(":7", "", "/var/tmp/xvfb-run.a1B2c3/Xauthority"));   // an empty WAYLAND_DISPLAY is unset; any temp folder holds the run's directory

    // a real X desktop: the usual display, its own authority file or none
    CHECK(!allowed(":0", nullptr, nullptr));
    CHECK(!allowed(":0", nullptr, "/home/person/.Xauthority"));
    CHECK(!allowed(":1", nullptr, "/run/user/1000/gdm/Xauthority"));
    // an Xvfb that something else started is not proven virtual (name it with FOXSDR_GUARD_DESKTOP_NAME)
    CHECK(!allowed(":99", nullptr, nullptr));
    CHECK(!allowed(":99", nullptr, ""));
    CHECK(!allowed(":99", nullptr, "/tmp/Xvfb-auth"));
    // lookalikes of the xvfb-run path are not it
    CHECK(!allowed(":99", nullptr, "/home/person/xvfb-run.x/Xauthority/../../.Xauthority"));
    CHECK(!allowed(":99", nullptr, "/tmp/xvfb-run./Xauthority"));   // no random part: not made by mktemp
    CHECK(!allowed(":99", nullptr, "/tmp/xvfb-run.VfpBVU/Xauthority.old"));
    CHECK(!allowed(":99", nullptr, "/tmp/xvfb-run.VfpBVU/sub/Xauthority"));
    CHECK(!allowed(":99", nullptr, "xvfb-run.VfpBVU/Xauthority"));   // no leading folder: not the shape xvfb-run makes
    CHECK(!allowed(":99", nullptr, "/Xauthority"));
    CHECK(!allowed(":99", nullptr, "Xauthority"));

    // a Wayland session is refused even under an xvfb-run display (a client may pick Wayland and reach the compositor)
    CHECK(!allowed(":99", "wayland-0", "/tmp/xvfb-run.VfpBVU/Xauthority"));
    CHECK(!allowed(nullptr, "wayland-0", nullptr));
    CHECK(linuxDesktopName(":99", "wayland-0", "/tmp/xvfb-run.VfpBVU/Xauthority") == "Default");

    // no display at all: the name cannot be read, and the guard refuses that too
    CHECK(linuxDesktopName(nullptr, nullptr, nullptr).empty());
    CHECK(linuxDesktopName("", nullptr, nullptr).empty());
    CHECK(!allowed(nullptr, nullptr, "/tmp/xvfb-run.VfpBVU/Xauthority"));

    // the override still allows a run on a desktop a person is watching, and nothing else does
    CHECK(judge(linuxDesktopName(":0", nullptr, nullptr), "1").allowed);
    CHECK(!judge(linuxDesktopName(":0", nullptr, nullptr), "0").allowed);

    return testSummary("test_gui_desktop_name");
}
