// The PLUGIN STORE's catalogue must be reachable and must scroll, at the size the
// store really opens at - and, since the 0.99.72 redesign, its FIRST CARD must be on
// screen.
//
// THE REPORT (the owner, 2026-09-18, on 0.99.2): "it's not letting me scroll on the
// plugin store to see the plugins." It was the size, not the wheel: at the store's
// design size (1480 x 980) the list takes a wheel and scrolls; the store opens
// clamped inside the main window (gui/page_geometry.hpp, pageOpenInside), a fresh
// install's main window is 1282 x 745, so the store is about 1234 x 697 - and three
// bands above the list took about 590 px of it, leaving the list 65 px, less than
// one card; at 1250 x 600 it began BELOW the window's bottom edge.
//
// THE REDESIGN HAS NO BANDS (one 56 px top bar, then the list), and this keeps it
// that way. At 1480 x 980, 1234 x 697 and 1234 x 560 the first card's rectangle -
// read from the UI census the view writes, not worked out here - lies inside the
// window, below the top bar; the list is tall enough for more than one card; it
// opens at the top; and it scrolls.
//
// WHY A HEADLESS FRAME. Where a wheel goes is decided inside Dear ImGui from the
// whole stack of windows under the pointer and each one's flags and extent; none of
// that is visible from the view's source. So this builds the same stack
// AppWindow::drawPluginStoreWindow and beginPage build - the page (no scrollbar, no
// scroll with mouse), its well, the store face with the flags the application uses
// (storeFaceWindowFlags, shared, not copied), then the view - fills it with a full
// catalogue, and scrolls it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "gui/fonts.hpp"
#include "gui/plugin_store_view.hpp"
#include "gui/ui_census.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

namespace {

namespace fs = std::filesystem;
using cascade::gui::PluginStoreDeck;
using cascade::gui::PluginStoreModel;
using cascade::gui::PluginStoreView;
using cascade::gui::StoreModule;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

PluginStoreModel fullCatalogue() {
    PluginStoreModel m;
    m.haveCatalogue = true;
    m.sourceStatus = "24 plugins in the catalogue";
    m.sourceUrl = "https://example.invalid/index.json";
    static const char* const cats[] = {"aircraft", "marine", "satellites-weather", "broadcast"};
    for (int i = 0; i < 24; ++i) {
        StoreModule sm;
        sm.id = "module-" + std::to_string(i);
        sm.plate.name = "Test Decoder Number " + std::to_string(i);
        sm.plate.version = "1.0." + std::to_string(i);
        sm.plate.maker = "FoxSDR project";
        sm.plate.licence = "MIT";
        sm.plate.category = cats[i % 4];
        sm.plate.summary = "Decodes something on a band a test made up, one line of summary.";
        sm.plate.blurb = sm.plate.summary;
        sm.installableHere = true;
        sm.haveBuildHere = true;
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

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool found = false;
};

// The census rect named `name` in the file the last frames wrote.
Rect censusRect(const fs::path& file, const std::string& name) {
    Rect r;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::string n;
        Rect c;
        ss >> n >> c.x0 >> c.y0 >> c.x1 >> c.y1;
        if (n == name) {
            c.found = true;
            r = c;
        }
    }
    return r;
}

// Whether the census file holds the note `item`.
bool censusHas(const fs::path& file, const std::string& item) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (line == "item " + item) { return true; }
    }
    return false;
}

// A TRANSFER RUNNING draws FITTING... on the key of the plugin it is for and on no other - the key's
// state is a thing the census can say, drawn here through the real view.
void testTheTransferringPluginReadsFitting(const fs::path& censusFile) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(2000.0f, 1400.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    cascade::gui::fonts::load();
    Store s;
    s.model.busy = true;
    s.model.busyId = "module-3";
    s.model.progress = 0.4f;
    for (int i = 0; i < 4; ++i) { s.frame(); }
    CHECK(cascade::gui::census::write());
    CHECK(censusHas(censusFile, "store:key:module-3:fitting"));
    CHECK(!censusHas(censusFile, "store:key:module-2:fitting"));
    CHECK(!censusHas(censusFile, "store:key:module-0:fitting"));
    ImGui::DestroyContext();
}

// Each size gets a FRESH context, so a scroll position left by one case cannot make the next one pass.
void testTheListIsReachableAndScrolls(float w, float h, const fs::path& censusFile) {
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
    ImGuiWindow* list = findWindowContaining("##storelist");
    CHECK(face != nullptr && list != nullptr);
    if (face == nullptr || list == nullptr) {
        ImGui::DestroyContext();
        return;
    }
    const float faceBottom = face->Pos.y + face->Size.y;
    const float faceRight = face->Pos.x + face->Size.x;
    std::printf("  store %.0f x %.0f: list %.0f tall, list top %.0f, face %.0f..%.0f\n", w, h,
                list->Size.y, list->Pos.y, face->Pos.y, faceBottom);

    // 0. IT OPENS AT THE TOP. A pane that can scroll must not arrive already scrolled: the search
    //    field and the first cards are at its head. Sixty idle frames, no input.
    for (int i = 0; i < 60; ++i) { s.frame(); }
    std::printf("     list scroll after 60 idle frames: %.0f (max %.0f)\n", list->Scroll.y, list->ScrollMax.y);
    CHECK(list->Scroll.y == 0.0f);
    CHECK(face->Scroll.y == 0.0f);

    // 1. THE TOP BAR IS ABOVE THE LIST, AND THE LIST HAS ROOM FOR MORE THAN ONE CARD. A card is about
    //    100 px; 65 px is what the report's window gave the list.
    CHECK(list->Pos.y > face->Pos.y + 40.0f);
    CHECK(list->Size.y >= 300.0f);

    // 2. THE FIRST CARD IS ON SCREEN: inside the window, below the top bar, read from the census the
    //    view wrote this frame. RED WHEN the list starts below the window's bottom edge, or the card is
    //    laid out under the bar.
    CHECK(cascade::gui::census::write());
    const Rect card = censusRect(censusFile, "store:card:module-0");
    CHECK(card.found);
    if (card.found) {
        std::printf("     first card %.0f,%.0f .. %.0f,%.0f\n", static_cast<double>(card.x0),
                    static_cast<double>(card.y0), static_cast<double>(card.x1), static_cast<double>(card.y1));
        CHECK(card.x0 >= face->Pos.x);
        CHECK(card.x1 <= faceRight + 1.0f);
        CHECK(card.y0 >= list->Pos.y);
        CHECK(card.y1 <= faceBottom + 1.0f);
        CHECK(card.y1 > card.y0 + 60.0f);
    }

    // 3. THE LIST ITSELF SCROLLS, with the pointer on it. Twenty-four plugins never fit in this list,
    //    so there is always somewhere to go.
    CHECK(list->ScrollMax.y > 0.0f);
    const float before = list->Scroll.y;
    wheelAt(s, list->Pos.x + list->Size.x * 0.5f, list->Pos.y + list->Size.y * 0.5f, -3.0f);
    std::printf("     list scroll %.0f -> %.0f\n", static_cast<double>(before), static_cast<double>(list->Scroll.y));
    CHECK(list->Scroll.y > before);

    // 4. ...AND ALL THE WAY: the wheel turned until nothing more moves brings the last card into the
    //    window, so nothing is unreachable.
    for (int i = 0; i < 40; ++i) {
        wheelAt(s, list->Pos.x + list->Size.x * 0.5f, list->Pos.y + list->Size.y * 0.5f, -5.0f);
    }
    CHECK(list->Scroll.y >= list->ScrollMax.y - 1.0f);

    ImGui::DestroyContext();
}

}  // namespace

int main() {
    const fs::path censusFile =
        fs::temp_directory_path() / ("cascade-store-scroll-" + std::to_string(std::rand()) + ".census");
    std::error_code ec;
    fs::remove(censusFile, ec);
    // BEFORE THE FIRST FRAME: whether the census is on is read once, on first use.
    setEnv("FOXSDR_UI_CENSUS", censusFile.string());

    // The design size; the size a fresh install opens it at (1282 x 745 less pageOpenInside's 24 px
    // margins); and a shorter window again.
    testTheListIsReachableAndScrolls(1480.0f, 980.0f, censusFile);
    testTheListIsReachableAndScrolls(1234.0f, 697.0f, censusFile);
    testTheListIsReachableAndScrolls(1234.0f, 560.0f, censusFile);
    testTheTransferringPluginReadsFitting(censusFile);
    fs::remove(censusFile, ec);
    return testSummary("test_plugin_store_scroll");
}
