/*
 * WHICH WINDOW THE CENTRE OF A TABLET'S SCREEN IS SHOWING.
 *
 * WHAT IS BEING PINNED. On Android a plugin window, an instrument face or a
 * map does not float over the spectrum as a page - there is one screen and
 * nowhere to tear a page off to - it becomes a TAB above the centre panel, and
 * the panel under the tabs shows either the spectrum and waterfall or that
 * window's body with the spectrum collapsed to a strip. gui/centre_dock.hpp is
 * the whole of the state behind that: which tabs exist, which is selected,
 * which has been given the full height, and what a second tap and a close key
 * do. None of it needs a screen, a frame or a device, so all of it is checked
 * here rather than by looking at a tablet.
 *
 * THE SHAPE OF THE CHECKS follows the four ways the selection can move, which
 * are the four ways a user gets lost if one is wrong: a window OPENS (it must
 * become the tab you are looking at, whichever of a dozen call sites opened
 * it), a window CLOSES from its own key (the spectrum must come back), a
 * window VANISHES without anyone pressing anything (a plugin removed, a rescan
 * - the spectrum must come back then too), and a tab is TAPPED TWICE (its
 * window takes the whole panel, and a third tap must not take it away again).
 *
 * BREAK-IT CHECK, run while writing this and quoted in the report:
 *   - deleting the "a newly offered window becomes the selection" loop in
 *     endFrame() fails 6 checks, starting at the first open;
 *   - letting the double tap move the clock on (lastTapSec_ = nowSec) instead
 *     of resetting it fails the three-taps case;
 *   - dropping the "selection that is no longer open falls back" line fails
 *     the vanish case (1 check: the close case survives it, because the close
 *     key moves the selection itself a frame earlier);
 *   - taking gui::px() off the tab key's height - the mistake that is
 *     invisible on a desktop, where px(v) == v - fails 3 checks, all of them
 *     at scale 2.0.
 * Each of those is a rule this file is named for, and each went red on its
 * own, so no check here is decoration.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include "gui/centre_dock.hpp"

#include <initializer_list>
#include <string>

#include "test_check.hpp"

using cascade::gui::CentreDock;
using cascade::gui::kDockDoubleTapSeconds;

namespace {

// One frame of the page pass: the ids that asked to be drawn, in draw order.
void frame(CentreDock& d, std::initializer_list<const char*> ids) {
    d.beginFrame();
    for (const char* id : ids) { d.offer(id, id); }
    d.endFrame();
}

const char* kPager = "PAGER (DEMO)###instrument_Demonstration|PAGER (DEMO)";
const char* kScope = "Demod scope###demodscopewindow";
const char* kMap = "ADS-B map###map_ADS-B";

}  // namespace

int main() {
    // -- NOTHING OPEN IS THE SPECTRUM, and that is the launch state: since
    // 0.79.1 the application starts on the main screen alone (core::
    // startupState clears every remembered window), so a fresh dock has to be
    // the radio and nothing else.
    {
        CentreDock d;
        CHECK(d.tabCount() == 0u);
        CHECK(d.spectrumActive());
        CHECK(!d.fullHeight());
        CHECK(d.activeId().empty());
        // A pass with nothing in it changes none of that.
        frame(d, {});
        CHECK(d.tabCount() == 0u);
        CHECK(d.spectrumActive());
    }

    // -- A WINDOW THAT OPENS IS THE WINDOW YOU ARE LOOKING AT ---------------
    //
    // The rail's row, a plugin preset, the DEMOD SCOPE key and a demonstration
    // instrument all open windows and none of them knows this class exists;
    // the dock notices the new id in the frame's pass instead.
    {
        CentreDock d;
        frame(d, {kPager});
        CHECK(d.tabCount() == 1u);
        CHECK(d.tabs()[0].id == kPager);
        CHECK(d.isActive(kPager));
        CHECK(!d.spectrumActive());

        // A SECOND FRAME WITH THE SAME WINDOW CHANGES NOTHING. The pass runs
        // every frame; if "offered" meant "new" the selection would be
        // re-taken sixty times a second and the SPECTRUM key would never hold.
        frame(d, {kPager});
        CHECK(d.isActive(kPager));

        // The spectrum back, with the tab kept - the tab row is the set of
        // open windows, not the set of windows being looked at.
        d.showSpectrum();
        CHECK(d.spectrumActive());
        CHECK(d.tabCount() == 1u);
        frame(d, {kPager});
        CHECK(d.spectrumActive());

        // ...and a window opening while the spectrum is up takes the screen,
        // which is the instruction ("a plugin should occupy the waterfall area
        // when it loads").
        frame(d, {kPager, kScope});
        CHECK(d.tabCount() == 2u);
        CHECK(d.isActive(kScope));
        // DRAW ORDER IS TAB ORDER, and it is stable: the pages are drawn in a
        // fixed sequence, so a tab does not move under a finger between
        // frames.
        CHECK(d.tabs()[0].id == kPager);
        CHECK(d.tabs()[1].id == kScope);
    }

    // -- TWO WINDOWS OPENING IN ONE FRAME land on a defined tab. A plugin
    // preset opens a map, a picture and every panel the module publishes in
    // one press; "the last one drawn" would be an arbitrary answer that
    // changes when the draw order does.
    {
        CentreDock d;
        frame(d, {kMap, kScope});
        CHECK(d.isActive(kMap));
    }

    // -- A SECOND TAP GIVES A TAB THE WHOLE PANEL ---------------------------
    {
        CentreDock d;
        frame(d, {kPager, kMap});
        d.tap(kPager, 10.0);
        CHECK(d.isActive(kPager));
        CHECK(!d.fullHeight());

        // Inside the window: full height.
        d.tap(kPager, 10.0 + kDockDoubleTapSeconds - 0.05);
        CHECK(d.isActive(kPager));
        CHECK(d.fullHeight());
        CHECK(d.fullHeight(kPager));

        // THREE TAPS ARE ONE TOGGLE. The clock is reset by the toggle rather
        // than moved on, so a finger resting on a key cannot flip the layout
        // twice.
        d.tap(kPager, 10.0 + kDockDoubleTapSeconds - 0.02);
        CHECK(d.fullHeight());

        // OUTSIDE the window is a plain selection, whatever it lands on.
        d.tap(kPager, 100.0);
        CHECK(d.fullHeight());  // still full: a slow tap changes nothing
        d.tap(kPager, 100.0 + kDockDoubleTapSeconds - 0.05);
        CHECK(!d.fullHeight());  // and a quick second one puts the strip back

        // A TAP ON A DIFFERENT TAB IS NOT THE SECOND TAP OF A PAIR.
        d.tap(kPager, 200.0);
        d.tap(kMap, 200.05);
        CHECK(d.isActive(kMap));
        CHECK(!d.fullHeight());

        // THE FLAG IS PER TAB. A map wants the height and a pager does not,
        // and switching between them must not carry one window's choice onto
        // the other.
        d.tap(kMap, 300.0);
        d.tap(kMap, 300.05);
        CHECK(d.fullHeight(kMap));
        CHECK(!d.fullHeight(kPager));
        d.tap(kPager, 400.0);
        CHECK(!d.fullHeight());
        CHECK(d.fullHeight(kMap));

        // ...and the spectrum is never "full height": it has no strip to hide.
        d.showSpectrum();
        CHECK(!d.fullHeight());

        // THE MAXIMISE KEY IN THE TAB ROW IS THE SAME STATE BY ANOTHER ROUTE,
        // because a gesture nobody has been told about cannot be the only way
        // to reach one. It does nothing at all while the spectrum is showing.
        d.toggleFullHeight();
        CHECK(!d.fullHeight());
        CHECK(d.fullHeight(kMap));  // and it did not reach into another tab
        d.tap(kPager, 500.0);
        d.toggleFullHeight();
        CHECK(d.fullHeight(kPager));
        d.toggleFullHeight();
        CHECK(!d.fullHeight(kPager));
    }

    // -- THE CLOSE KEY IN THE TAB BAR ---------------------------------------
    //
    // It does not hide the window itself: the request is consumed by the page
    // path on the next frame, which closes it exactly as a floating page's own
    // key does. One close route, not two.
    {
        CentreDock d;
        frame(d, {kPager, kScope});
        d.tap(kScope, 10.0);
        CHECK(d.isActive(kScope));

        d.requestClose(kScope);
        // The selection moves back AT ONCE - a key that visibly does nothing
        // for a frame reads as a key that missed - while the tab is still
        // there, because the window still is.
        CHECK(d.spectrumActive());
        CHECK(d.tabCount() == 2u);
        CHECK(d.closePending(kScope));
        // Taken exactly once.
        CHECK(d.takeCloseRequest(kScope));
        CHECK(!d.takeCloseRequest(kScope));
        CHECK(!d.closePending(kScope));

        // The page path then hides the window, so it stops offering.
        frame(d, {kPager});
        CHECK(d.tabCount() == 1u);
        CHECK(d.tabs()[0].id == kPager);
        CHECK(d.spectrumActive());
    }

    // -- CLOSING THE TAB THAT IS NOT SELECTED leaves the selection alone.
    {
        CentreDock d;
        frame(d, {kPager, kScope});
        d.tap(kPager, 1.0);
        d.requestClose(kScope);
        CHECK(d.isActive(kPager));
        CHECK(d.takeCloseRequest(kScope));
        frame(d, {kPager});
        CHECK(d.isActive(kPager));
    }

    // -- A WINDOW CAN GO WITHOUT ANYONE PRESSING ANYTHING -------------------
    //
    // Its plugin was removed, a rescan came back without it, the rail hid it.
    // A dock still pointing at it would draw an empty panel with no way back
    // to the radio.
    {
        CentreDock d;
        frame(d, {kPager, kMap});
        d.tap(kMap, 5.0);
        d.tap(kMap, 5.1);
        CHECK(d.isActive(kMap));
        CHECK(d.fullHeight());

        frame(d, {kPager});  // the map's plugin has gone
        CHECK(d.tabCount() == 1u);
        CHECK(d.spectrumActive());

        // AND WHAT WAS REMEMBERED ABOUT IT WENT WITH IT: a window re-opened
        // later starts at the strip like any other, rather than at whatever it
        // was left at an hour ago.
        frame(d, {kPager, kMap});
        CHECK(d.isActive(kMap));
        CHECK(!d.fullHeight());
    }

    // -- AN UNCONSUMED CLOSE REQUEST DIES WITH ITS WINDOW -------------------
    //
    // Otherwise a request left behind by a window that closed some other way
    // would fire at whatever re-opened under the same id.
    {
        CentreDock d;
        frame(d, {kPager});
        d.requestClose(kPager);
        CHECK(d.closePending(kPager));
        frame(d, {});
        CHECK(!d.closePending(kPager));
        frame(d, {kPager});
        CHECK(!d.takeCloseRequest(kPager));
        CHECK(d.isActive(kPager));
    }

    // -- THE DEFENSIVE EDGES ------------------------------------------------
    {
        CentreDock d;
        // An empty id is not a window; it is how the spectrum is spelled
        // internally, and nothing may create a tab for it.
        d.beginFrame();
        d.offer("", "NOTHING");
        d.offer(kPager, "PAGER");
        d.offer(kPager, "PAGER AGAIN");  // one identity, one tab
        d.endFrame();
        CHECK(d.tabCount() == 1u);
        CHECK(d.tabs()[0].title == std::string("PAGER"));
        d.tap("", 1.0);
        CHECK(d.isActive(kPager));  // unchanged by a tap on nothing
        d.requestClose("");
        CHECK(d.isActive(kPager));
        // Offers outside a pass are ignored rather than silently published.
        d.offer(kScope, "SCOPE");
        CHECK(d.tabCount() == 1u);
    }

    // -- THE THREE FIGURES, AT 1.0 AND AT 2.0 -------------------------------
    //
    // Every layout figure in src/gui goes through gui::px() at the point of
    // use, so one factor scales the whole interface; a dock figure that
    // forgot to would be a 2 mm touch target on the tablet next to a 4 mm
    // one, or a strip whose floor is a quarter of what it was measured to be.
    // Checked at both scales for the reason test_ui_scale checks the fit at
    // both: px(v) == v exactly at 1.0 (a float times 1.0f is the same bits),
    // so the desktop's numbers are the constants themselves.
    {
        cascade::gui::setUiScale(1.0f);
        // kTinySize is 14, so the lettered height (14 + 9 = 23) wins over the
        // 22 floor - the rail's own bank-key rule.
        CHECK_NEAR(cascade::gui::dockTabKeyHeight(14.0f), 23.0f, 1e-6);
        CHECK_NEAR(cascade::gui::dockTabRowHeight(14.0f), 23.0f + 7.0f + 6.0f, 1e-6);
        // Smaller type than the floor allows for: the floor wins.
        CHECK_NEAR(cascade::gui::dockTabKeyHeight(8.0f), 22.0f, 1e-6);
        // A quarter of the spectrum's height, and the 48 px floor under it.
        CHECK_NEAR(cascade::gui::dockStripHeight(600.0f), 150.0f, 1e-6);
        CHECK_NEAR(cascade::gui::dockStripHeight(100.0f), 48.0f, 1e-6);

        cascade::gui::setUiScale(2.0f);
        CHECK_NEAR(cascade::gui::dockTabKeyHeight(14.0f), 46.0f, 1e-6);
        CHECK_NEAR(cascade::gui::dockTabRowHeight(14.0f), 72.0f, 1e-6);
        CHECK_NEAR(cascade::gui::dockTabKeyHeight(8.0f), 44.0f, 1e-6);
        // THE STRIP'S QUARTER IS ALREADY IN SCREEN PIXELS - it is a quarter of
        // a height the panel measured - so only its FLOOR is scaled. Scaling
        // the quarter as well would take a quarter of a quarter.
        CHECK_NEAR(cascade::gui::dockStripHeight(600.0f), 150.0f, 1e-6);
        CHECK_NEAR(cascade::gui::dockStripHeight(100.0f), 96.0f, 1e-6);

        cascade::gui::setUiScale(1.0f);  // as every other test finds it
    }

    return testSummary("test_centre_dock");
}
