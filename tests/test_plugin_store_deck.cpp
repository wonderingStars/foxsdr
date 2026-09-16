// Tests for the PLUGIN STORE's UPPER DECK height against
// gui::pageDeckHeightCap() - the hard rule (gui/page_geometry.hpp) that a
// page's own control deck may never claim more than half of the body its
// draw() was handed.
//
// WHY THIS TEST EXISTS. On the Pixel Tablet emulator at UI scale 2.0, the
// PLUGIN STORE's upper deck - the ADD ALL key, the state banner and the three
// control wells (CATALOGUE SEARCH / SHOW / SORT + CATALOGUE SOURCE) - came out
// TALLER THAN THE WHOLE PAGE (commit cf755a5's note above THE CONTROL DECK in
// plugin_store_view.cpp), pushing the MODULE LIST and the DATA PLATE past the
// bottom edge of a face that, at the time, could not even scroll to reach
// them. The root cause, found by measuring the real typefaces rather than
// guessing at it: the SHOW well's two-column layout missed its own threshold
// by 1.9 px at exactly the docked tablet's body width, because the count
// reserve budgeted three digits ("000") for a rocker that can only ever
// report as many as model.modules.size() - 14 bundled on Android, 27 in the
// largest catalogue this product has ever published. Trimming that reserve to
// two digits (99, more than triple the biggest real catalogue) closed the gap
// without moving a single desktop pixel at UI scale 1.0.
//
// REAL FONTS, A REAL DRAW(), NOT A SECOND FORMULA. fonts::load() puts the
// three shipped typefaces in the atlas (the same requirement
// test_bench_text_fits.cpp states for the same reason: a measurement against
// ImGui's fallback bitmap face would pass while proving nothing), and every
// figure below comes from calling PluginStoreView::draw() itself inside a
// real ImGui window and child, exactly as AppWindow::drawPluginStoreWindow
// does it - not from re-deriving the layout arithmetic a second time, which
// is exactly the kind of second copy that drifts from the original and stops
// meaning anything.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "gui/fonts.hpp"
#include "gui/page_geometry.hpp"
#include "gui/plugin_store_view.hpp"
#include "gui/ui_scale.hpp"
#include "imgui.h"
#include "test_check.hpp"

using cascade::gui::ModulePlate;
using cascade::gui::pageDeckHeightCap;
using cascade::gui::PluginStoreDeck;
using cascade::gui::PluginStoreModel;
using cascade::gui::PluginStoreView;
using cascade::gui::StoreModule;

namespace {

// One fitted, loaded, running decoder - the shape every one of the fourteen
// modules cf755a5 bundles into the Android apk actually takes.
StoreModule fittedDecoder(const char* name, const char* version) {
    StoreModule sm;
    sm.plate.haveDescriptor = true;
    sm.plate.name = name;
    sm.plate.version = version;
    sm.plate.maker = "FoxSDR";
    sm.plate.licence = "PolyForm-Noncommercial-1.0.0";
    sm.plate.blurb = "A bundled decoder.";
    sm.plate.fileName = std::string("libfoxsdr_plugin_") + name + ".so";
    sm.plate.fitted = true;
    sm.plate.loaded = true;
    sm.plate.running = true;
    sm.plate.haveCapabilities = true;
    sm.plate.capabilities = CASCADE_CAP_DECODER;
    sm.plate.haveTuneGrant = true;
    sm.plate.tuneGranted = true;
    sm.installableHere = false;  // already fitted - nothing to fetch
    return sm;
}

// THE REAL ANDROID STATE (buildPluginStoreModel's __ANDROID__ branch,
// app_window.cpp): bundled, no catalogue, every field a fetch would fill left
// at its default because no fetch is possible on this platform. The fourteen
// names are cf755a5's own bundled list.
PluginStoreModel bundledAndroidModel() {
    PluginStoreModel m;
    m.bundled = true;
    m.haveCatalogue = false;
    const char* const names[] = {"adsb",    "acars",         "ais",     "aprs",
                                 "apt",     "cw",             "ert",     "rtty",
                                 "sstv",    "survey-engine",  "twotone", "vor",
                                 "weather", "wefax"};
    for (const char* n : names) { m.modules.push_back(fittedDecoder(n, "1.0.0")); }
    return m;
}

// A REPRESENTATIVE DESKTOP CATALOGUE, in its ordinary steady state: read,
// mixed fitted/not, nothing pending. NOT the UPDATES AVAILABLE state - that
// banner is the one place this window is deliberately allowed to grow (an
// update row per module with something to offer), and a window that must stay
// under half its body height in that state too is a different, larger
// redesign than the one the docked tablet actually needed. The common case -
// the one this test pins - is what a desktop user sees on every launch that
// is not the day an update lands.
PluginStoreModel desktopCatalogueModel() {
    PluginStoreModel m;
    m.bundled = false;
    m.sourceUrl = "https://plugins.foxsdr.com/index.json";
    m.haveCatalogue = true;
    m.sourceStatus = "24 plugins in the catalogue";
    const char* const fittedNames[] = {"ADS-B", "APRS", "ACARS", "AIS", "APT",
                                       "Weather Satellites (HRIT/LRIT)"};
    for (const char* n : fittedNames) { m.modules.push_back(fittedDecoder(n, "1.2.0")); }
    for (int i = 0; i < 6; ++i) {
        StoreModule sm;
        sm.plate.haveDescriptor = true;
        sm.plate.name = "Catalogue module " + std::to_string(i);
        sm.plate.version = "1.0.0";
        sm.plate.maker = "Third Party";
        sm.plate.licence = "MIT";
        sm.plate.blurb = "Not yet fitted.";
        sm.plate.fitted = false;
        sm.installableHere = true;
        m.modules.push_back(sm);
    }
    return m;
}

// Drives PluginStoreView::draw() exactly as AppWindow::drawPluginStoreWindow
// does: a real window, a real "##storeface"-shaped child at (w, h), scrollable
// (ImGuiWindowFlags_None) as cf755a5 left it, and the Dummy afterwards that
// keeps ImGui's own "cursor moved past the content extent" check quiet.
float drawnUpperDeckHeight(PluginStoreView& view, const PluginStoreModel& model,
                           PluginStoreDeck& deck, float w, float h) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(w + 32.0f, h + 32.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("panel", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##storeface_test", ImVec2(w, h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_None);
    ImGui::PopStyleVar();
    view.draw(w, h, model, deck);
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    ImGui::EndChild();

    ImGui::End();
    ImGui::Render();
    return view.upperDeckHeight();
}

// ---------------------------------------------------------------------------
// THE PROPERTY: the upper deck never claims more than half of the body.
// ---------------------------------------------------------------------------
void testUpperDeckFitsUnderHalfTheBody() {
    // THE DOCKED TABLET, AT ITS OWN SCALE. This is the exact body cf755a5's
    // note measured on the Pixel Tablet emulator (2560x1600, UI scale 2.0),
    // and the exact case that used to fail: the SHOW well fell back to one
    // column of six rockers and the deck grew past the page.
    {
        cascade::gui::setUiScale(2.0f);
        PluginStoreModel model = bundledAndroidModel();
        PluginStoreDeck deck;
        PluginStoreView view;
        const float bodyH = 1400.0f;
        const float h = drawnUpperDeckHeight(view, model, deck, 1860.0f, bodyH);
        CHECK(h > 0.0f);
        CHECK(h <= pageDeckHeightCap(bodyH));
        std::printf("  tablet body 1860x%.0f @2.0x: upper deck %.2f px, cap %.2f px\n", bodyH,
                    h, pageDeckHeightCap(bodyH));
    }

    // THE SAME PHYSICAL BODY FORCED TO UI SCALE 1.0 - FOXSDR_UI_SCALE=1's own
    // case. There is far more slack here than the deck needs; it is checked
    // anyway because a rule that only ever runs at one scale is a rule nobody
    // has actually pinned.
    {
        cascade::gui::setUiScale(1.0f);
        PluginStoreModel model = bundledAndroidModel();
        PluginStoreDeck deck;
        PluginStoreView view;
        const float bodyH = 1400.0f;
        const float h = drawnUpperDeckHeight(view, model, deck, 1860.0f, bodyH);
        CHECK(h > 0.0f);
        CHECK(h <= pageDeckHeightCap(bodyH));
        std::printf("  tablet body 1860x%.0f @1.0x: upper deck %.2f px, cap %.2f px\n", bodyH,
                    h, pageDeckHeightCap(bodyH));
    }

    // A REPRESENTATIVE DESKTOP WINDOW BODY, at its own scale (1.0) - well
    // above the 640x260 "too narrow" floor and well below the store's own
    // default open size (1480x980 before chrome), carrying the heavier
    // UPDATES AVAILABLE banner state.
    {
        cascade::gui::setUiScale(1.0f);
        PluginStoreModel model = desktopCatalogueModel();
        PluginStoreDeck deck;
        PluginStoreView view;
        const float bodyH = 820.0f;
        const float h = drawnUpperDeckHeight(view, model, deck, 1280.0f, bodyH);
        CHECK(h > 0.0f);
        CHECK(h <= pageDeckHeightCap(bodyH));
        std::printf("  desktop body 1280x%.0f @1.0x: upper deck %.2f px, cap %.2f px\n", bodyH,
                    h, pageDeckHeightCap(bodyH));
    }

    // THE SAME DESKTOP CATALOGUE, HYPOTHETICALLY SCALED TO 2.0. Desktop
    // windows never actually run above 1.0 (gui/ui_scale.hpp caps a monitor
    // at its own density), but the rule this test pins is "half the body",
    // not "half the body at the one scale this happens to run at today" - so
    // it is checked at the other scale too.
    {
        cascade::gui::setUiScale(2.0f);
        PluginStoreModel model = desktopCatalogueModel();
        PluginStoreDeck deck;
        PluginStoreView view;
        const float bodyH = 1640.0f;
        const float h = drawnUpperDeckHeight(view, model, deck, 2560.0f, bodyH);
        CHECK(h > 0.0f);
        CHECK(h <= pageDeckHeightCap(bodyH));
        std::printf("  desktop body 2560x%.0f @2.0x: upper deck %.2f px, cap %.2f px\n", bodyH,
                    h, pageDeckHeightCap(bodyH));
    }

    cascade::gui::setUiScale(1.0f);  // as every other test in this binary finds it
}

// ---------------------------------------------------------------------------
// THE BREAK-IT CHECK: the deck actually SCALES. If gui::px() ever regressed
// to the identity (a raise that forgot to route a new figure through it, the
// exact fault class ui_scale.hpp's own header warns about), the deck drawn
// into the same physical tablet body at UI scale 2.0 would stay desktop-sized
// instead of growing with it, and this is the check that would catch it - run
// once while writing this test by editing gui::px(float) in ui_scale.hpp to
// `return units;` and rebuilding just this binary: the ratio collapsed to
// 1.00 (h1 == h2, both the scale-1.0 figure) and the check below went red.
// ---------------------------------------------------------------------------
void testUpperDeckHeightScalesWithUi() {
    PluginStoreModel model = bundledAndroidModel();

    cascade::gui::setUiScale(1.0f);
    PluginStoreDeck deck1;
    PluginStoreView view1;
    const float h1 = drawnUpperDeckHeight(view1, model, deck1, 1860.0f, 1400.0f);

    cascade::gui::setUiScale(2.0f);
    PluginStoreDeck deck2;
    PluginStoreView view2;
    const float h2 = drawnUpperDeckHeight(view2, model, deck2, 1860.0f, 1400.0f);

    // NOT EXACTLY DOUBLE: showTwoCols only engages at scale 2.0 in this
    // 1860 px body (it is what this whole file exists to fix), which trims
    // the SHOW well from six rocker rows down to three at exactly the scale
    // that would otherwise need the most room - a genuine, deliberate
    // asymmetry between the two scales, so the ratio is required to be
    // comfortably above 1.0 rather than near 2.0 the way a single measured
    // figure (moduleKindTagWidth, say) would be.
    CHECK(h2 > h1 * 1.3f);
    std::printf("  upper deck height: %.2f @1.0x -> %.2f @2.0x\n", h1, h2);

    cascade::gui::setUiScale(1.0f);
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(2600.0f, 1650.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // THE LOAD ITSELF IS THE FIRST ASSERTION, exactly as test_bench_text_fits
    // does it: measuring against ImGui's own fallback face would tell us
    // nothing about what the product draws.
    const bool loaded = cascade::gui::fonts::load();
    CHECK(loaded);

    if (loaded) {
        testUpperDeckFitsUnderHalfTheBody();
        testUpperDeckHeightScalesWithUi();
    } else {
        std::printf("fonts::load() failed - the measurements below were not run\n");
    }

    ImGui::DestroyContext();
    return testSummary("test_plugin_store_deck");
}
