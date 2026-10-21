// Tests for gui/page_resize_id.hpp - the guard in front of
// ImGui::GetWindowResizeCornerID, which the page grip in AppWindow::beginPage
// calls in every frame and which dereferences a dock node's host window
// without looking.
//
// THE FAULT, from the field (crash DE3F72AAE6B69997, 0.99.75; D94CCD4584724FAC,
// 0.99.58): 0xC0000005 at ImGui::GetWindowResizeCornerID+0x1b, called from
// AppWindow::beginPage+0xa3c. ImGui 1.92.8 answers
//     window->DockIsActive ? window->DockNode->HostWindow->ID : window->ID
// and a window being dropped into a dock node is DockIsActive for the frames in
// which the node has no host window yet. These tests build exactly those
// windows and ask.
//
// What each leg proves:
//  1. A null window is 0, not a fault.
//  2. A window that is not docked gets ImGui's own id - the guard changes
//     nothing for the ordinary page, and 0 would mean the grip never lit.
//  3. DockIsActive with NO dock node is 0. (The old code faults here.)
//  4. DockIsActive with a node whose HostWindow is null is 0. (And here: this is
//     the state DockContextProcessDock and BeginDocked leave a window in.)
//  5. DockIsActive with a hosted node gets the id ImGui would have given, built
//     on the HOST's id - the guard must not turn off the grip of a window that
//     is properly docked.
//  6. A window that is NOT DockIsActive is never refused because of its node:
//     a window that remembers a node (DockNode set, one window in it, no tab
//     bar) is an ordinary window and keeps its id.
//  7. pageGripIdHot never lights on 0 == 0: when the corner is unknown and
//     nothing is hovered or active - the common frame - the grip must not
//     light and the cursor must not turn into a resize arrow.
//  8. No source file but the helper calls ImGui's function (or the border one,
//     which has the same line), so the guard cannot be walked around by the
//     next call site.
//
// On MSVC a fault inside the helper is caught and reported as a failed check
// naming the exception code, so one broken case does not hide the rest; the
// unguarded build of this file dies with 0xC0000005 on legs 3 and 4 in the same
// place the field did.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _MSC_VER
#include <windows.h>
#endif

#include "gui/page_resize_id.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

#ifdef _MSC_VER
// Asks the helper, and says so if it FAULTED rather than answering. `out` is
// untouched on a fault. Kept free of C++ objects with destructors, which
// __try cannot unwind.
bool askCorner(ImGuiWindow* w, ImGuiID& out, unsigned long& faultCode) {
    faultCode = 0;
    __try {
        out = cascade::gui::pageResizeCornerId(w);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        faultCode = static_cast<unsigned long>(GetExceptionCode());
        return false;
    }
}
#else
bool askCorner(ImGuiWindow* w, ImGuiID& out, unsigned long& faultCode) {
    faultCode = 0;
    out = cascade::gui::pageResizeCornerId(w);
    return true;
}
#endif

// What the helper says for `w`, or a sentinel that no id equals when it faulted.
constexpr ImGuiID kFaulted = 0xFFFFFFFFu;

ImGuiID cornerOf(ImGuiWindow* w, const char* what) {
    ImGuiID id = kFaulted;
    unsigned long fault = 0;
    if (!askCorner(w, id, fault)) {
        std::printf("FAULT 0x%08lX asking pageResizeCornerId for %s\n", fault, what);
        return kFaulted;
    }
    return id;
}

// The tree root, found the way the other source-scanning tests do: the first
// argument if it names one, else up from the working directory.
fs::path findRepoRoot(int argc, char** argv) {
    std::error_code ec;
    if (argc >= 2) {
        const fs::path given(argv[1]);
        if (fs::exists(given / "src" / "gui" / "page_resize_id.hpp", ec)) { return given; }
    }
    fs::path p = fs::current_path(ec);
    for (int i = 0; i < 12 && !p.empty(); ++i) {
        if (fs::exists(p / "src" / "gui" / "page_resize_id.hpp", ec)) { return p; }
        const fs::path up = p.parent_path();
        if (up == p) { break; }
        p = up;
    }
    return fs::path();
}

// Source with comments cut out: a comment may say the name, only code may not
// call it. Strings are not parsed; neither name appears in one.
std::string withoutComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text.compare(i, 2, "//") == 0) {
            while (i < text.size() && text[i] != '\n') { ++i; }
        } else if (text.compare(i, 2, "/*") == 0) {
            const std::size_t end = text.find("*/", i + 2);
            i = (end == std::string::npos) ? text.size() : end + 2;
        } else {
            out.push_back(text[i++]);
        }
    }
    return out;
}

// Every file under src/ whose code calls one of ImGui's resize-id functions.
std::vector<std::string> directCallers(const fs::path& src) {
    std::vector<std::string> found;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(src, ec)) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h" && ext != ".cc") { continue; }
        std::ifstream in(e.path(), std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string code = withoutComments(ss.str());
        if (code.find("GetWindowResizeCornerID") != std::string::npos ||
            code.find("GetWindowResizeBorderID") != std::string::npos) {
            found.push_back(e.path().filename().string());
        }
    }
    return found;
}

}  // namespace

int main(int argc, char** argv) {
    using cascade::gui::pageGripIdHot;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiContext* ctx = ImGui::GetCurrentContext();

    // --- 1. A null window is 0. -------------------------------------------
    CHECK(cornerOf(nullptr, "a null window") == 0u);

    ImGuiWindow* page = IM_NEW(ImGuiWindow)(ctx, "Decoder output###decoderout");
    ImGuiWindow* host = IM_NEW(ImGuiWindow)(ctx, "##DockHost");
    const ImGuiID plain = ImGui::GetWindowResizeCornerID(page, 0);  // page is not docked yet
    CHECK(plain != 0u);

    // --- 2. Not docked: ImGui's own answer. -------------------------------
    {
        page->DockIsActive = false;
        page->DockNode = nullptr;
        CHECK(cornerOf(page, "an undocked page") == plain);
    }

    // --- 3. DockIsActive and no node at all: 0, not a fault. ---------------
    {
        page->DockIsActive = true;
        page->DockNode = nullptr;
        CHECK(cornerOf(page, "a docked page with no dock node") == 0u);
    }

    // --- 4. DockIsActive and a node with no host window: 0, not a fault. ---
    {
        ImGuiDockNode node(0x2468u);
        CHECK(node.HostWindow == nullptr);  // the constructor's own state
        page->DockIsActive = true;
        page->DockNode = &node;
        CHECK(cornerOf(page, "a docked page whose node has no host window") == 0u);

        // --- 6. The same node, but the window is not DockIsActive: an ----
        // ordinary window. The guard is for the docked state, and must not
        // blank the grip of a window that merely remembers a node.
        page->DockIsActive = false;
        CHECK(cornerOf(page, "an undocked page that remembers a node") == plain);

        // --- 5. A hosted node: the id ImGui gives, built on the host's. ---
        node.HostWindow = host;
        page->DockIsActive = true;
        const ImGuiID hosted = cornerOf(page, "a docked page in a hosted node");
        CHECK(hosted != 0u);
        CHECK(hosted != kFaulted);
        CHECK(hosted == ImGui::GetWindowResizeCornerID(page, 0));
        CHECK(hosted != plain);  // it follows the host window, not the page

        page->DockIsActive = false;
        page->DockNode = nullptr;
    }

    // --- 7. An unknown corner is never "hot". -----------------------------
    {
        const ImGuiID corner = 0x1234ABCDu;
        CHECK(!pageGripIdHot(0u, 0u, 0u));             // nothing hovered, nothing active
        CHECK(!pageGripIdHot(0u, 0u, corner));         // something else active
        CHECK(!pageGripIdHot(0u, corner, 0u));         // something else hovered
        CHECK(!pageGripIdHot(corner, 0u, 0u));         // known corner, idle pointer
        CHECK(!pageGripIdHot(corner, 0x77u, 0x88u));   // other widgets hovered and active
        CHECK(pageGripIdHot(corner, corner, 0u));      // hovered
        CHECK(pageGripIdHot(corner, 0u, corner));      // being dragged
        CHECK(pageGripIdHot(corner, corner, corner));  // both
    }

    IM_DELETE(host);
    IM_DELETE(page);
    ImGui::DestroyContext();

    // --- 8. Nobody else calls ImGui's function. ---------------------------
    {
        const fs::path root = findRepoRoot(argc, argv);
        CHECK(!root.empty());  // a moved tree fails loudly instead of skipping the scan
        if (!root.empty()) {
            const std::vector<std::string> callers = directCallers(root / "src");
            std::string names;
            for (const std::string& c : callers) { names += " " + c; }
            std::printf("files under src/ that call ImGui's resize-id functions:%s\n",
                        names.empty() ? " none" : names.c_str());
            // Exactly the helper's own header.
            CHECK(callers.size() == 1u);
            CHECK(!callers.empty() && callers.front() == "page_resize_id.hpp");
        }
    }

    return testSummary("test_page_resize_id");
}
