// test_win_frame.cpp - the hit test that stands in for the title bar.
//
// The operating system's caption is gone from the main window (0.78.0), and
// with it went the one thing that decided whether a mouse-down on the top of
// the window drags it, resizes it, or presses something. hitZone is that
// decision now, as arithmetic, and these are the rules it has to keep:
//
//   - a corner beats an edge, and an edge beats the caption: the top eight
//     pixels of the rail resize the window, as they do on a framed one;
//   - the rail is the caption EXCEPT over its keys, which stay client so the
//     click reaches the key rather than starting a drag;
//   - a maximised window has no borders - its edges are off the screen;
//   - below the rail is client, whatever is drawn there.
//
// RED WHEN any of those is lost: a key that starts a drag, a rail that cannot
// be dragged, or a maximised window whose top strip is a resize handle nobody
// can reach.
#include "gui/win_frame.hpp"
#include "test_check.hpp"

using cascade::gui::frame::CaptionLayout;
using cascade::gui::frame::hitZone;
using cascade::gui::frame::Rect;
using cascade::gui::frame::Zone;

int main() {
    CaptionLayout lay;
    lay.railHeight = 24.0f;
    lay.keys = Rect{1500.0f, 3.0f, 1590.0f, 21.0f};
    const float w = 1600.0f;
    const float h = 1000.0f;
    const float b = 8.0f;

    // --- borders, corners first ------------------------------------------------
    {
        CHECK(hitZone(0.0f, 0.0f, w, h, b, lay, false) == Zone::TopLeft);
        CHECK(hitZone(7.9f, 7.9f, w, h, b, lay, false) == Zone::TopLeft);
        CHECK(hitZone(w - 1.0f, 0.0f, w, h, b, lay, false) == Zone::TopRight);
        CHECK(hitZone(0.0f, h - 1.0f, w, h, b, lay, false) == Zone::BottomLeft);
        CHECK(hitZone(w - 1.0f, h - 1.0f, w, h, b, lay, false) == Zone::BottomRight);
        CHECK(hitZone(3.0f, 500.0f, w, h, b, lay, false) == Zone::Left);
        CHECK(hitZone(w - 3.0f, 500.0f, w, h, b, lay, false) == Zone::Right);
        CHECK(hitZone(800.0f, 2.0f, w, h, b, lay, false) == Zone::Top);
        CHECK(hitZone(800.0f, h - 2.0f, w, h, b, lay, false) == Zone::Bottom);
        // The top border runs across the rail AND across the keys: a resize
        // from the very top edge works everywhere, as on a framed window.
        CHECK(hitZone(1550.0f, 2.0f, w, h, b, lay, false) == Zone::Top);
    }

    // --- the rail is the caption, except over its keys -------------------------
    {
        CHECK(hitZone(200.0f, 12.0f, w, h, b, lay, false) == Zone::Caption);
        CHECK(hitZone(200.0f, 8.0f, w, h, b, lay, false) == Zone::Caption);   // just under the border
        CHECK(hitZone(200.0f, 23.9f, w, h, b, lay, false) == Zone::Caption);  // last rail pixel
        CHECK(hitZone(200.0f, 24.0f, w, h, b, lay, false) == Zone::Client);   // first pixel below it
        CHECK(hitZone(1550.0f, 12.0f, w, h, b, lay, false) == Zone::Client);  // on a key
        CHECK(hitZone(1499.9f, 12.0f, w, h, b, lay, false) == Zone::Caption); // just left of the keys
        CHECK(hitZone(1590.0f, 12.0f, w, h, b, lay, false) == Zone::Caption); // just right of them
        // The keys' own top and bottom edges: inside is the key, outside the
        // rail. The key's top three pixels sit under the resize border, which
        // wins there (checked above); its first pixel below the border is the
        // key's.
        CHECK(hitZone(1550.0f, 8.0f, w, h, b, lay, false) == Zone::Client);
        CHECK(hitZone(1550.0f, 21.0f, w, h, b, lay, false) == Zone::Caption);
    }

    // --- below the rail is client, whatever is drawn there ---------------------
    {
        CHECK(hitZone(800.0f, 500.0f, w, h, b, lay, false) == Zone::Client);
        CHECK(hitZone(20.0f, 100.0f, w, h, b, lay, false) == Zone::Client);
    }

    // --- a maximised window has no borders ---------------------------------------
    {
        CHECK(hitZone(0.0f, 0.0f, w, h, b, lay, true) == Zone::Caption);
        CHECK(hitZone(800.0f, 2.0f, w, h, b, lay, true) == Zone::Caption);
        CHECK(hitZone(3.0f, 500.0f, w, h, b, lay, true) == Zone::Client);
        CHECK(hitZone(w - 1.0f, h - 1.0f, w, h, b, lay, true) == Zone::Client);
        CHECK(hitZone(1550.0f, 12.0f, w, h, b, lay, true) == Zone::Client);   // the key still works
        // A zero border says the same thing without the flag.
        CHECK(hitZone(0.0f, 0.0f, w, h, 0.0f, lay, false) == Zone::Caption);
    }

    // --- no keys laid out yet: the whole rail drags ------------------------------
    {
        CaptionLayout bare;
        bare.railHeight = 24.0f;
        CHECK(bare.keys.empty());
        CHECK(hitZone(1550.0f, 12.0f, w, h, b, bare, false) == Zone::Caption);
        // And no rail at all - the frame not installed, say - is all client.
        CaptionLayout none;
        CHECK(hitZone(200.0f, 12.0f, w, h, b, none, false) == Zone::Client);
    }

    // --- outside the window, or a window with no size, is never a handle ---------
    {
        CHECK(hitZone(-1.0f, 12.0f, w, h, b, lay, false) == Zone::Client);
        CHECK(hitZone(200.0f, -1.0f, w, h, b, lay, false) == Zone::Client);
        CHECK(hitZone(w, 12.0f, w, h, b, lay, false) == Zone::Client);
        CHECK(hitZone(200.0f, 12.0f, 0.0f, 0.0f, b, lay, false) == Zone::Client);
    }

    // --- maximised: the WORK area, with the invisible frame parked off-screen --
    //
    // Measured on the development desk before the fix: monitor 5120x1440, work
    // area 5120x1392 (a 48 px taskbar at the bottom), and the maximised window
    // came out -7,-7 5134x1454 - a client 1440 tall, the bottom 48 px of it
    // under the taskbar. A window with no caption in its style is maximised to
    // the whole screen by default; this box is what a captioned window gets.
    {
        using cascade::gui::frame::maximisedBox;
        using cascade::gui::frame::Box;
        const Box b1 = maximisedBox(0, 0, 0, 0, 5120, 1392, 7, 7, 7, 7);
        CHECK(b1.x == -7 && b1.y == -7 && b1.w == 5134 && b1.h == 1406);
        // Client = box inset by the frame = the work area exactly.
        CHECK(b1.x + 7 == 0 && b1.y + 7 == 0 && b1.w - 14 == 5120 && b1.h - 14 == 1392);
        // A taskbar at the TOP: the client starts at the work area's top, not
        // the monitor's, which is what hid a tester's rail keys.
        const Box b2 = maximisedBox(0, 0, 0, 48, 5120, 1440, 7, 7, 7, 7);
        CHECK(b2.y + 7 == 48 && b2.h - 14 == 1392);
        // A second monitor to the right: the box is relative to ITS origin.
        const Box b3 = maximisedBox(5120, 0, 5120, 0, 7040, 1032, 8, 8, 8, 8);
        CHECK(b3.x == -8 && b3.y == -8 && b3.w == 1936 && b3.h == 1048);
        // No frame at all: the box is the work area itself.
        const Box b4 = maximisedBox(0, 0, 0, 0, 1920, 1040, 0, 0, 0, 0);
        CHECK(b4.x == 0 && b4.y == 0 && b4.w == 1920 && b4.h == 1040);
        // A frame that cannot be right (negative, or wider than any frame is)
        // counts as none rather than throwing the window off the screen.
        const Box b5 = maximisedBox(0, 0, 0, 0, 1920, 1040, -3, 200, 7, 7);
        CHECK(b5.x == 0 && b5.y == 0 && b5.w == 1927 && b5.h == 1047);
    }

    return testSummary("test_win_frame");
}
