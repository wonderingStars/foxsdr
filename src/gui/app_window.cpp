// GLFW + Dear ImGui application shell — implementation.
//
// SPDX-License-Identifier: MIT
#include "gui/app_window.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// After the ImGui backends: glfw3.h pulls in GL/gl.h on Windows, which the
// opengl3 backend must not see before its own embedded loader.
#include <GLFW/glfw3.h>

#include "core/version.hpp"

namespace cascade::gui {

namespace {

constexpr float kMenuWidth = 260.0f;  // left column width per parity spec

// GLFW reports failures through this callback *before* glfwInit/CreateWindow
// return their error codes, so printing here is what gives the user an actual
// reason instead of a bare "init failed".
void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "cascade: GLFW error %d: %s\n", code,
                 description ? description : "(no description)");
}

}  // namespace

int AppWindow::run(int frames) {
    glfwSetErrorCallback(&glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "cascade: glfwInit failed\n");
        return 1;
    }

    const std::string title =
        std::string(cascade::appName()) + " " + cascade::versionString();
    GLFWwindow* window = glfwCreateWindow(1280, 720, title.c_str(), nullptr, nullptr);
    if (window == nullptr) {
        std::fprintf(stderr, "cascade: glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);  // vsync: the GUI thread paces itself off the display

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // Layout is fully code-driven; a stray imgui.ini next to the exe would
    // silently override it and make runs non-reproducible.
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        std::fprintf(stderr, "cascade: ImGui GLFW backend init failed\n");
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 130")) {
        std::fprintf(stderr, "cascade: ImGui OpenGL3 backend init failed\n");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }

    int rendered = 0;
    while (!glfwWindowShouldClose(window)) {
        // Exact-count contract: check before rendering so --frames N produces
        // N frames, and --frames 0 produces none.
        if (frames >= 0 && rendered >= frames) { break; }

        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        drawUi();

        ImGui::Render();
        int fbWidth = 0;
        int fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        glViewport(0, 0, fbWidth, fbHeight);
        glClearColor(0.05f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
        ++rendered;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    // Printed on every clean exit: this is what makes the exact-count half of
    // the --frames contract externally observable — app_smoke matches
    // "rendered 3 frames" via PASS_REGULAR_EXPRESSION, so an off-by-one in the
    // frame bound goes red instead of shipping silently.
    std::printf("cascade: rendered %d frames\n", rendered);
    return 0;
}

void AppWindow::drawUi() {
    // One borderless window pinned to the viewport: the app IS the layout, so
    // nothing is movable or collapsible at this level.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags rootFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##cascade_root", nullptr, rootFlags);

    drawToolbar();
    ImGui::Separator();

    ImGui::BeginChild("##menu_column", ImVec2(kMenuWidth, 0.0f), ImGuiChildFlags_None);
    drawMenuColumn();
    ImGui::EndChild();

    ImGui::SameLine();

    // The center area owns its scrolling (none): the spectrum/waterfall pair
    // always fills whatever space the splitter hands it.
    ImGui::BeginChild("##center", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawCenterPanels();
    ImGui::EndChild();

    ImGui::End();
}

void AppWindow::drawToolbar() {
    if (ImGui::Button(playing_ ? "Stop" : "Play", ImVec2(64.0f, 0.0f))) {
        playing_ = !playing_;  // placeholder: no pipeline to start yet
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("##volume", &volume_, 0.0f, 1.0f, "Vol %.2f");
    ImGui::SameLine(0.0f, 24.0f);
    drawFrequencyReadout();
}

void AppWindow::drawFrequencyReadout() {
    // Fixed 10-digit field grouped in thousands ("0.100.000.000" at 100 MHz).
    // The field width is constant so digits never shift as the tuned
    // frequency changes; the zeros (and separators) ahead of the first
    // significant digit are dimmed so the eye reads only the live value.
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%010llu", frequencyHz_);

    const ImVec4 bright = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImVec4 dim = bright;
    dim.w *= 0.25f;

    // Large monospace look: scale the default font up. Base size (not
    // GetFontSize()) so global scale factors are not applied twice.
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.2f);
    bool significant = false;
    for (int i = 0; i < 10; ++i) {
        if (digits[i] != '0') { significant = true; }
        // Separators after digits 0, 3 and 6 group the field d.ddd.ddd.ddd; a
        // separator inherits the dim state of the run it terminates.
        const bool sepAfter = (i == 0 || i == 3 || i == 6);
        const char cell[3] = {digits[i], sepAfter ? '.' : '\0', '\0'};
        ImGui::PushStyleColor(ImGuiCol_Text, significant ? bright : dim);
        ImGui::TextUnformatted(cell);
        ImGui::PopStyleColor();
        if (i != 9) { ImGui::SameLine(0.0f, 0.0f); }
    }
    ImGui::PopFont();
}

void AppWindow::drawMenuColumn() {
    if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("No source modules loaded yet");
    }
    if (ImGui::CollapsingHeader("Radio", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kModes[] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};
        constexpr int kColumns = 4;
        // Cosmetic mode selector: remembers the pick, drives nothing until
        // the demodulator chain exists.
        const float cellWidth =
            (ImGui::GetContentRegionAvail().x -
             static_cast<float>(kColumns - 1) * ImGui::GetStyle().ItemSpacing.x) /
            static_cast<float>(kColumns);
        for (int i = 0; i < 8; ++i) {
            if (i % kColumns != 0) { ImGui::SameLine(); }
            const bool selected = (i == modeIndex_);
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::Button(kModes[i], ImVec2(cellWidth, 0.0f))) { modeIndex_ = i; }
            if (selected) { ImGui::PopStyleColor(); }
        }
    }
    if (ImGui::CollapsingHeader("Sinks", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Audio sink not wired yet");
    }
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("FFT / waterfall controls pending");
    }
}

void AppWindow::drawCenterPanels() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float splitterThickness = 6.0f;
    const float usable = avail.y - splitterThickness;
    // A squeezed window can drive the region to zero; drawing into negative
    // sizes asserts inside ImGui, so just skip the panels that frame.
    if (usable < 40.0f || avail.x < 40.0f) { return; }

    const float spectrumHeight = splitRatio_ * usable;
    drawSpectrumPlaceholder(avail.x, spectrumHeight);

    // Splitter: an invisible button whose vertical drag re-balances the
    // spectrum/waterfall split. Ratio (not pixels) so a window resize keeps
    // the user's proportions.
    ImGui::InvisibleButton("##vsplitter", ImVec2(avail.x, splitterThickness));
    if (ImGui::IsItemActive()) {
        splitRatio_ = std::clamp(splitRatio_ + ImGui::GetIO().MouseDelta.y / usable,
                                 0.1f, 0.9f);
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    ImGuiCol gripColor = ImGuiCol_Separator;
    if (ImGui::IsItemActive()) {
        gripColor = ImGuiCol_SeparatorActive;
    } else if (ImGui::IsItemHovered()) {
        gripColor = ImGuiCol_SeparatorHovered;
    }
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                              ImGui::GetColorU32(gripColor));

    drawWaterfallPlaceholder(avail.x, usable - spectrumHeight);
}

void AppWindow::drawSpectrumPlaceholder(float width, float height) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);
    drawList->AddRectFilled(p0, p1, IM_COL32(8, 10, 14, 255));

    // Fake-but-plausible static spectrum: a jittered noise floor plus three
    // Gaussian tone humps. Fixed-seed LCG, re-run identically every frame, so
    // the placeholder is stable and smoke-test runs render identically.
    constexpr int kPoints = 256;
    std::uint32_t lcg = 0x12345678u;
    auto next01 = [&lcg]() {
        lcg = lcg * 1664525u + 1013904223u;
        return static_cast<float>(lcg >> 8) / 16777216.0f;
    };
    struct Peak {
        float center, amplitudeDb, sigma;
    };
    static constexpr Peak kPeaks[] = {
        {0.22f, 48.0f, 0.010f},
        {0.50f, 34.0f, 0.020f},
        {0.74f, 22.0f, 0.008f},
    };
    ImVec2 points[kPoints];
    for (int i = 0; i < kPoints; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(kPoints - 1);
        float db = -92.0f + 6.0f * next01();  // noise floor around -90 dBFS
        for (const Peak& peak : kPeaks) {
            const float d = (u - peak.center) / peak.sigma;
            db += peak.amplitudeDb * std::exp(-0.5f * d * d);
        }
        // Map [-100, 0] dB onto the panel, 0 dB at the top edge.
        const float yNorm = std::clamp(-db / 100.0f, 0.0f, 1.0f);
        points[i] = ImVec2(p0.x + u * width, p0.y + yNorm * height);
    }
    drawList->AddPolyline(points, kPoints, IM_COL32(94, 189, 255, 255), ImDrawFlags_None, 1.5f);
    ImGui::Dummy(ImVec2(width, height));
}

void AppWindow::drawWaterfallPlaceholder(float width, float height) {
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);
    // Near-black with a hint of blue — the low end of the eventual waterfall
    // colormap, so the real GL-texture widget can replace this without a
    // visual jump.
    ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, IM_COL32(4, 5, 10, 255));
    ImGui::Dummy(ImVec2(width, height));
}

}  // namespace cascade::gui
