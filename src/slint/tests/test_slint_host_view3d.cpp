// What a 3D view remembers (src/slint/view3d_prefs.hpp, docs 6.64): the setting's text read and written, leniently and clamped, so the chosen view and the camera survive a restart and a
// setting written by another build never breaks the window. No Slint, no engine, no window.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"
#include "view3d_prefs.hpp"

#include <string>

using namespace foxsdr::slintview3d;
namespace s3 = foxsdr::scene3d;

int main() {
    // an empty setting: every view is at its defaults and the 2D view (on = false) is the default
    {
        const ViewPrefs p = ViewPrefs::parse("");
        CHECK(p.serialize().empty());
        ViewState d;
        d.elevation = 26.0f;
        const ViewState s = p.get(kViewRadar, d);
        CHECK(!s.on);
        CHECK(s.elevation == 26.0f);
    }
    // one view set and written: the same state always gives the same text, and it reads back whole
    {
        ViewPrefs p;
        ViewState s;
        s.on = true;
        s.azimuth = -33.5f;
        s.elevation = 41.25f;
        s.zoom = 1.5f;
        s.opt = {2, 1, 0, 0};
        p.set(kViewWaterfall, s);
        const std::string text = p.serialize();
        CHECK(text == "waterfall:on=1,az=-33.50,el=41.25,zoom=1.50,o0=2,o1=1");
        const ViewPrefs q = ViewPrefs::parse(text);
        CHECK(q == p);
        const ViewState r = q.get(kViewWaterfall, ViewState{});
        CHECK(r == s);
        CHECK(q.serialize() == text);
    }
    // several views, in name order, each its own
    {
        ViewPrefs p;
        ViewState a, b;
        a.on = true;
        b.azimuth = 90.0f;
        p.set(kViewWaterfall, a);
        p.set(kViewSonde, b);
        p.set(kViewRadar, ViewState{});
        const ViewPrefs q = ViewPrefs::parse(p.serialize());
        CHECK(q.get(kViewWaterfall, ViewState{}).on);
        CHECK(!q.get(kViewSonde, ViewState{}).on);
        CHECK(q.get(kViewSonde, ViewState{}).azimuth == 90.0f);
        CHECK(p.serialize().find("radar:") < p.serialize().find("sonde:"));   // sorted by name
        CHECK(p.serialize().find("sonde:") < p.serialize().find("waterfall:"));
    }
    // clamped: a hand-edited or newer-build value cannot put the camera where the window cannot show it
    {
        const ViewPrefs p = ViewPrefs::parse("waterfall:on=1,az=725,el=200,zoom=99,o0=99,o1=-4");
        const ViewState s = p.get(kViewWaterfall, ViewState{});
        CHECK(s.on);
        CHECK(s.azimuth >= -180.0f && s.azimuth <= 180.0f);
        CHECK(s.azimuth > 4.9f && s.azimuth < 5.1f);    // 725 = 5 + 2 turns
        CHECK(s.elevation == s3::kMaxElevationDeg);
        CHECK(s.zoom == s3::kMaxZoom);
        CHECK(s.opt[0] == 15 && s.opt[1] == 0);
        const ViewState low = ViewPrefs::parse("waterfall:el=-9,zoom=0").get(kViewWaterfall, ViewState{});
        CHECK(low.elevation == s3::kMinElevationDeg);
        CHECK(low.zoom == s3::kMinZoom);
    }
    // lenient: garbage in is skipped, the rest still reads; an unlisted key and a stray word change nothing
    {
        const ViewPrefs p = ViewPrefs::parse(";;nonsense;waterfall:on=1,zz=5,az=abc,el=40,=3,o9=2;Bad Name:on=1;:on=1;radar:on=0");
        const ViewState w = p.get(kViewWaterfall, ViewState{});
        CHECK(w.on);
        CHECK(w.azimuth == -58.0f);          // "abc" is not a number: the default stays
        CHECK(w.elevation == 40.0f);
        CHECK(w.opt == (std::array<int, 4>{0, 0, 0, 0}));
        CHECK(p.serialize().find("Bad") == std::string::npos);
        CHECK(!p.get(kViewRadar, ViewState{}).on);
        // a very long name is refused
        CHECK(ViewPrefs::parse("abcdefghijklmnopqrstuvwxyzabcdef:on=1").serialize().empty());
        // NaN and infinity are not numbers
        CHECK(ViewPrefs::parse("waterfall:az=nan,el=inf").get(kViewWaterfall, ViewState{}).elevation == 32.0f);
    }
    // another lane's view name is just a name: it is kept and written back
    {
        ViewPrefs p = ViewPrefs::parse("globe:on=1,az=10,el=20,zoom=2,o2=3");
        CHECK(p.get("globe", ViewState{}).on);
        CHECK(p.get("globe", ViewState{}).opt[2] == 3);
        CHECK(ViewPrefs::parse(p.serialize()) == p);
    }
    // the camera and the state convert both ways
    {
        ViewState s;
        s.azimuth = 12.0f;
        s.elevation = 50.0f;
        s.zoom = 2.0f;
        const s3::Camera c = cameraOf(s);
        CHECK(c.azimuthDeg == 12.0f && c.elevationDeg == 50.0f && c.zoom == 2.0f);
        ViewState t;
        s3::Camera d;
        d.azimuthDeg = -70.0f;
        d.elevationDeg = 10.0f;
        d.zoom = 0.5f;
        setCamera(t, d);
        CHECK(t.azimuth == -70.0f && t.elevation == 10.0f && t.zoom == 0.5f);
    }
    return testSummary("test_slint_host_view3d");
}
