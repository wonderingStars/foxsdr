// GLFW + Dear ImGui application shell.
//
// SPDX-License-Identifier: MIT
#pragma once

namespace cascade::gui {

// Owns the GLFW window, the ImGui context and the top-level panel layout.
// All GLFW/ImGui usage stays behind this interface so main() (and any future
// headless harness) never needs GUI headers.
class AppWindow {
public:
    // Runs the shell until the window is closed, or — when `frames` >= 0 —
    // for exactly that many rendered frames. The bounded mode is the
    // `--frames N` self-test contract that the app_smoke ctest entry relies
    // on: render N frames, shut down cleanly, exit 0.
    //
    // Returns the process exit code: 0 on clean shutdown, 1 when GLFW or the
    // ImGui backends fail to initialize (the reason is printed to stderr).
    int run(int frames = -1);

private:
    void drawUi();
    void drawToolbar();
    void drawFrequencyReadout();
    void drawMenuColumn();
    void drawCenterPanels();
    void drawSpectrumPlaceholder(float width, float height);
    void drawWaterfallPlaceholder(float width, float height);

    // Placeholder UI state. None of it drives DSP yet; it exists so the shell
    // already has the final layout and interactions while the pipeline is
    // built underneath it.
    bool playing_ = false;
    float volume_ = 0.5f;
    unsigned long long frequencyHz_ = 100000000ULL;  // 100 MHz per parity spec
    int modeIndex_ = 1;                              // WFM
    float splitRatio_ = 0.4f;  // spectrum's share of the center area
};

}  // namespace cascade::gui
