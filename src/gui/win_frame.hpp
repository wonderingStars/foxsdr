// win_frame.hpp - the operating system's title bar, taken off the main window,
// and the parts of it that have to be put back by hand when it goes.
//
// WHY THE TITLE BAR GOES. The application is drawn as one instrument: a brass
// cabinet with four screws and the panels sunk into it. Above that cabinet the
// operating system drew its own strip - a white bar with the name of the
// program, a minimise, a maximise and a close - in a style from a different
// decade and a different machine, and the user asked for it to go: "remove all
// the top bars and put the minimise, maximise and close on the metal". So the
// cabinet's own top rail carries the name, engraved, and three brass keys, and
// the window has no frame of its own at all.
//
// WHAT HAS TO BE PUT BACK. A window's title bar is not decoration; it is where
// the window is dragged from, double-clicked to fill the screen, right-clicked
// for its system menu, and where the desktop's own conveniences - snapping to
// an edge, shaking to clear the desktop, the keyboard's Win+arrow - are hooked
// in. Taking the bar off with GLFW_DECORATED alone loses every one of those.
// This module keeps them: the window stays a fully decorated one as far as the
// operating system is concerned, and two messages are answered differently.
// WM_NCCALCSIZE says the client area is the WHOLE window, so the system draws
// no caption and no frame; WM_NCHITTEST says which part of that client area is
// the caption (the cabinet's rail, minus its keys) and which parts are the
// resize borders. Everything else - the move loop, the snap, the maximise
// on double-click, the system menu - is the operating system's own and is
// untouched.
//
// THE HIT TEST IS ARITHMETIC, and it is here as a plain function so a test
// can check it without a window: a corner beats a caption, the keys are never
// the caption, and a maximised window has no borders (it cannot be resized and
// its edges are off the screen).
//
// WINDOWS ONLY. GLFW has no portable hook for a caption region, and X11 and
// Wayland answer the question in ways this application does not yet speak.
// install() returns false there, the window keeps the frame the desktop gave
// it, and the caller draws no keys - a window with two sets of controls is
// worse than one with the old set.
#pragma once

struct GLFWwindow;

namespace cascade::gui::frame {

// A rectangle in the window's own client pixels, y down.
struct Rect {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    bool empty() const { return !(x1 > x0 && y1 > y0); }
};

// What the draw code tells the hit test each frame: how tall the rail across
// the top is, and where the keys on it sit - the one part of the rail that
// must stay a control rather than a handle.
struct CaptionLayout {
    float railHeight = 0.0f;
    Rect keys;
};

enum class Zone {
    Client,
    Caption,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// Which part of the window a point in client pixels is on.
//
//   - BORDERS FIRST, corners before edges, and `border` pixels deep from every
//     side - a point in the rail's top eight pixels is a resize handle, not a
//     drag handle, exactly as on a framed window.
//   - A MAXIMISED WINDOW HAS NO BORDERS: pass border <= 0 or maximised true.
//   - THE RAIL IS THE CAPTION, except over its keys, which stay Client so the
//     click reaches the key.
//   - Everything else is Client.
Zone hitZone(float x, float y, float width, float height, float border,
             const CaptionLayout& layout, bool maximised);

// Take the title bar off `window`, keeping the operating system's own move,
// resize, snap, maximise and system-menu behaviour. Returns false, changing
// nothing, where that is not possible - which is every platform but Windows.
bool install(GLFWwindow* window);

// Whether install() succeeded on this window: the draw code draws the keys
// only when it did.
bool installed();

// Where the rail and its keys are this frame, in client pixels.
void setCaptionLayout(const CaptionLayout& layout);

// The layout last set, for the code that draws it and for tests.
CaptionLayout captionLayout();

}  // namespace cascade::gui::frame
