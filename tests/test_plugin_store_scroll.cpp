// The PLUGIN STORE's module list must be reachable and must scroll, at the size
// the store really opens at.
//
// THE REPORT (the owner, 2026-09-18, on 0.99.2): "it's not letting me scroll on
// the plugin store to see the plugins."
//
// WHAT IT TURNED OUT TO BE. Not the wheel: at the store's design size (1480 x
// 980) the list takes a wheel and scrolls. It is the size. The store opens
// clamped inside the main window (gui/page_geometry.hpp, pageOpenInside), a
// fresh install's main window is 1282 x 745, so the store is about 1234 x 697 -
// and the three bands above the list (the ADD ALL well, the updates banner, the
// control deck) took about 590 px of it. Measured with this file before the
// fix: the list was 65 px tall at 1250 x 700, less than one module card, and at
// 1250 x 600 it began BELOW the window's bottom edge, inside a pane marked
// NoScrollbar | NoScrollWithMouse, so nothing could bring it into view.
//
// WHY A HEADLESS FRAME. Where a wheel goes is decided inside Dear ImGui from the
// whole stack of windows under the pointer and each one's flags and extent;
// none of that is visible from the view's source. So this builds the same stack
// AppWindow::drawPluginStoreWindow and beginPage build - the page (no
// scrollbar, no scroll with mouse), its well, the store face with the flags the
// application uses (storeFaceWindowFlags, shared, not copied), then the view -
// fills it with a full catalogue, and scrolls it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>

#include "gui/fonts.hpp"
#include "gui/plugin_store_view.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

namespace {

using cascade::gui::PluginStoreDeck;
using cascade::gui::PluginStoreModel;
using cascade::gui::PluginStoreView;
using cascade::gui::StoreModule;

PluginStoreModel fullCatalogue() {
    PluginStoreModel m;
    m.haveCatalogue = true;
    m.sourceStatus = "24 plugins in the catalogue";
    m.sourceUrl = "https://example.invalid/index.json";
    for (int i = 0; i < 24; ++i) {
        StoreModule sm;
        sm.id = "module-" + std::to_string(i);
        sm.plate.name = "Test Decoder Number " + std::to_string(i);
        sm.plate.version = "1.0." + std::to_string(i);
        sm.plate.maker = "FoxSDR project";
        sm.plate.licence = "MIT";
        sm.plate.summary = "Decodes something on a band a test made up, one line of summary.";
        sm.plate.blurb = sm.plate.summary;
        sm.installableHere = true;
        m.modules.push_back(sm);
    }
    return m;
}

ImGuiWindow* findWindowContaining(const char* part) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    for (ImGuiWindow* w : g.Windows) {
        if (w != nullptr && std::string(w->Name).find(part) != std::string::npos) { return w; }
    }
    return nullptr;
}

struct Store {
    float w = 1480.0f;
    float h = 980.0f;
    PluginStoreView view;
    PluginStoreDeck deck;
    PluginStoreModel model = fullCatalogue();

    // One frame, nested exactly as the application nests it.
    void frame() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(100.0f, 50.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("Plugin store###pluginstorewindow", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::BeginChild("##pagewell", avail, ImGuiChildFlags_None, ImGuiWindowFlags_None);
        const ImVec2 face = ImGui::GetContentRegionAvail();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##storeface", face, ImGuiChildFlags_None,
                          cascade::gui::storeFaceWindowFlags());
        ImGui::PopStyleVar();
        view.draw(face.x, face.y, model, deck);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
    }
};

void wheelAt(Store& s, float x, float y, float notches) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    s.frame();
    io.AddMouseWheelEvent(0.0f, notches);
    s.frame();
    s.frame();
}

// Each size gets a FRESH context, so a scroll position left by one case cannot
// make the next one pass.
void testTheListIsReachableAndScrolls(float w, float h) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(2000.0f, 1400.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    cascade::gui::fonts::load();

    Store s;
    s.w = w;
    s.h = h;
    for (int i = 0; i < 4; ++i) { s.frame(); }

    ImGuiWindow* face = findWindowContaining("##storeface");
    ImGuiWindow* list = findWindowContaining("##modlist");
    CHECK(face != nullptr && list != nullptr);
    if (face == nullptr || list == nullptr) {
        ImGui::DestroyContext();
        return;
    }
    const float faceBottom = face->Pos.y + face->Size.y;
    std::printf("  store %.0f x %.0f: list %.0f tall, list top %.0f, face bottom %.0f\n", w, h,
                list->Size.y, list->Pos.y, faceBottom);

    // 0. IT OPENS AT THE TOP. A pane that can scroll must not arrive already
    //    scrolled: the search field and the ADD ALL key are at its head, and a
    //    store that opens with them above the edge hides the two controls a
    //    first visit is most likely to want. Sixty idle frames, no input.
    for (int i = 0; i < 60; ++i) { s.frame(); }
    std::printf("     pane scroll after 60 idle frames: %.0f (max %.0f)\n", face->Scroll.y,
                face->ScrollMax.y);
    CHECK(face->Scroll.y == 0.0f);

    // 1. THE LIST HAS ROOM FOR MORE THAN ONE MODULE. Two cards are about
    //    260 px at these sizes; 65 px is what the report's window gave it.
    //    RED WHEN the body's floor goes back to 120.
    CHECK(list->Size.y >= 300.0f);

    // 2. IT CAN BE BROUGHT INTO VIEW. The pointer on the control deck, well
    //    above the list, and the wheel turned until nothing more moves: the
    //    whole list must then lie inside the pane. RED WHEN the pane cannot
    //    scroll and the list starts below its edge.
    const float deckY = face->Pos.y + 40.0f;
    for (int i = 0; i < 20; ++i) { wheelAt(s, face->Pos.x + 40.0f, deckY, -3.0f); }
    const float listBottom = list->Pos.y + list->Size.y;
    std::printf("     after scrolling the pane: list %.0f..%.0f, face %.0f..%.0f\n", list->Pos.y,
                listBottom, face->Pos.y, faceBottom);
    CHECK(list->Pos.y >= face->Pos.y - 1.0f);
    CHECK(listBottom <= faceBottom + 1.0f);

    // 3. AND THE LIST ITSELF SCROLLS, with the pointer on it. Twenty-four
    //    modules never fit in this list, so there is always somewhere to go.
    CHECK(list->ScrollMax.y > 0.0f);
    const float before = list->Scroll.y;
    wheelAt(s, list->Pos.x + list->Size.x * 0.5f, list->Pos.y + list->Size.y * 0.5f, -3.0f);
    std::printf("     list scroll %.0f -> %.0f\n", before, list->Scroll.y);
    CHECK(list->Scroll.y > before);

    ImGui::DestroyContext();
}

}  // namespace

int main() {
    // The design size; the size a fresh install opens it at (1282 x 745 less
    // pageOpenInside's 24 px margins); and a shorter window again.
    testTheListIsReachableAndScrolls(1480.0f, 980.0f);
    testTheListIsReachableAndScrolls(1234.0f, 697.0f);
    testTheListIsReachableAndScrolls(1234.0f, 560.0f);
    return testSummary("test_plugin_store_scroll");
}
