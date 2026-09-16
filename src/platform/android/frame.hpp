// frame.hpp - the activity's lifecycle and the render loop.
//
// THE ONE THING THAT MAKES THIS DIFFERENT FROM THE DESKTOP LOOP. AppWindow's
// loop on the desktop runs from the moment the window opens until the user
// closes it, and glfwWaitEventsTimeout handles the rest. Here the loop outlives
// the drawing surface: an Android activity is told when it has a window, when
// it has focus, and when both are taken away, and it must not touch GL between
// those. So the loop has three states rather than one -
//
//   no window        block in ALooper_pollOnce until the framework gives one
//   window, no focus block as well: the screen is off or another app is in
//                    front, and a frame drawn now is a frame nobody sees
//   window + focus   poll without blocking, draw, present at the panel's rate
//
// - and the DSP worker is stopped in the two idle states. That is the whole
// battery story: an SDR that keeps running its FFT in a pocket is a phone that
// is warm and flat by lunchtime.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_PLATFORM_ANDROID_FRAME_HPP
#define CASCADE_PLATFORM_ANDROID_FRAME_HPP

#include <android/input.h>

#include <cstdint>

struct android_app;

namespace cascade::platform::android {

// Installs the callbacks on `app`, runs until the framework asks the activity
// to go away, and tears everything down on the way out. This is the entire body
// of android_main.
void runFrameLoop(android_app* app);

// The android_app callbacks, exposed only because runFrameLoop installs them
// and a reader looking for "what happens on APP_CMD_INIT_WINDOW" should be able
// to find the answer from this header.
void onAppCmd(android_app* app, int32_t cmd);
int32_t onInputEvent(android_app* app, AInputEvent* event);

}  // namespace cascade::platform::android

#endif  // CASCADE_PLATFORM_ANDROID_FRAME_HPP
