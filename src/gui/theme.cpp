// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "gui/theme.hpp"

namespace cascade::gui::theme {

ImVec4 vec(ImU32 packed) {
    // IM_COL32 packs as ABGR on every platform ImGui supports, which is why
    // this shifts rather than memcpy-ing a struct: the byte order of an ImU32
    // is defined by the macro, not by the machine.
    const float s = 1.0f / 255.0f;
    return ImVec4(static_cast<float>((packed >> IM_COL32_R_SHIFT) & 0xFFu) * s,
                  static_cast<float>((packed >> IM_COL32_G_SHIFT) & 0xFFu) * s,
                  static_cast<float>((packed >> IM_COL32_B_SHIFT) & 0xFFu) * s,
                  static_cast<float>((packed >> IM_COL32_A_SHIFT) & 0xFFu) * s);
}

ImU32 withAlpha(ImU32 packed, float alpha) {
    float a = alpha;
    if (!(a >= 0.0f)) { a = 0.0f; }
    if (a > 1.0f) { a = 1.0f; }
    const ImU32 rgb = packed & ~(0xFFu << IM_COL32_A_SHIFT);
    return rgb | (static_cast<ImU32>(a * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
}

void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    // --- shape ---------------------------------------------------------------
    // A machined key has a small radius. Nothing in this design is round except
    // the things that are actually round - knobs, lamps, screws, the scope.
    s.WindowRounding = 0.0f;  // load-bearing: see the header
    s.ChildRounding = kPanelRounding;
    s.FrameRounding = kKeyRounding;
    s.PopupRounding = kPanelRounding;
    s.ScrollbarRounding = kKeyRounding;
    s.GrabRounding = kKeyRounding;
    s.TabRounding = kKeyRounding;

    s.WindowBorderSize = kHairline;
    s.ChildBorderSize = kHairline;
    s.PopupBorderSize = kHairline;
    s.FrameBorderSize = kHairline;

    s.WindowPadding = ImVec2(10.0f, 10.0f);
    s.FramePadding = ImVec2(8.0f, 4.0f);
    s.ItemSpacing = ImVec2(8.0f, 6.0f);
    s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 11.0f;

    // --- colour --------------------------------------------------------------
    ImVec4* c = s.Colors;

    // The ground everything sits on. Alpha 1.0 on WindowBg is load-bearing.
    c[ImGuiCol_WindowBg] = vec(kEnamelDark);
    c[ImGuiCol_ChildBg] = vec(withAlpha(kWell, 0.55f));
    c[ImGuiCol_PopupBg] = vec(kEnamel);
    c[ImGuiCol_MenuBarBg] = vec(kBrassDark);

    // Lettering. Ivory for anything operable; the muted tone for prose.
    c[ImGuiCol_Text] = vec(kIvory);
    c[ImGuiCol_TextDisabled] = vec(kInkFaint);
    c[ImGuiCol_TextSelectedBg] = vec(withAlpha(kAmber, 0.35f));

    // Borders are brass hairlines, not black gaps.
    c[ImGuiCol_Border] = vec(withAlpha(kBrassBright, 0.55f));
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    // A FIELD IS A WELL - something cut into the panel, so it is darker than
    // the panel and never lighter. Fields hold readings, and a reading lives
    // on glass.
    c[ImGuiCol_FrameBg] = vec(kEnamel);
    c[ImGuiCol_FrameBgHovered] = vec(kEngraved);
    c[ImGuiCol_FrameBgActive] = vec(kBrassDark);

    // A KEY IS BRASS AND SLIGHTLY PROUD: mid at rest, bright under the hand,
    // dark when pressed. That ordering is what makes it read as metal being
    // pushed rather than a rectangle changing colour.
    c[ImGuiCol_Button] = vec(kBrassMid);
    c[ImGuiCol_ButtonHovered] = vec(kBrassBright);
    c[ImGuiCol_ButtonActive] = vec(kBrassDark);

    // Headers - the collapsing sections of the left rail - are plates screwed
    // to the panel, so they read as brass too.
    c[ImGuiCol_Header] = vec(kBrassDark);
    c[ImGuiCol_HeaderHovered] = vec(kBrassMid);
    c[ImGuiCol_HeaderActive] = vec(kBrassBright);

    c[ImGuiCol_TitleBg] = vec(kEnamelDark);
    c[ImGuiCol_TitleBgActive] = vec(kBrassDark);
    c[ImGuiCol_TitleBgCollapsed] = vec(kEnamelDark);

    // Anything that MOVES to show a value takes the amber a reading uses: the
    // slider grab, the check mark, the progress fill. The eye then learns one
    // rule - amber means a number - instead of a colour per widget.
    c[ImGuiCol_CheckMark] = vec(kAmber);
    c[ImGuiCol_SliderGrab] = vec(kAmber);
    c[ImGuiCol_SliderGrabActive] = vec(kGold);
    c[ImGuiCol_PlotHistogram] = vec(kAmber);
    c[ImGuiCol_PlotHistogramHovered] = vec(kGold);

    // The trace of something received is phosphor, not amber - it is a picture
    // the radio made, not a figure the application computed.
    c[ImGuiCol_PlotLines] = vec(kPhosphor);
    c[ImGuiCol_PlotLinesHovered] = vec(kPhosphorDim);

    c[ImGuiCol_ScrollbarBg] = vec(withAlpha(kVoid, 0.6f));
    c[ImGuiCol_ScrollbarGrab] = vec(kBrassDark);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(kBrassMid);
    c[ImGuiCol_ScrollbarGrabActive] = vec(kBrassBright);

    c[ImGuiCol_Separator] = vec(withAlpha(kBrassBright, 0.45f));
    c[ImGuiCol_SeparatorHovered] = vec(kBrassBright);
    c[ImGuiCol_SeparatorActive] = vec(kGold);

    c[ImGuiCol_ResizeGrip] = vec(withAlpha(kBrassMid, 0.5f));
    c[ImGuiCol_ResizeGripHovered] = vec(kBrassBright);
    c[ImGuiCol_ResizeGripActive] = vec(kGold);

    c[ImGuiCol_Tab] = vec(kBrassDark);
    c[ImGuiCol_TabHovered] = vec(kBrassBright);
    c[ImGuiCol_TabSelected] = vec(kBrassMid);
    c[ImGuiCol_TabSelectedOverline] = vec(kAmber);
    c[ImGuiCol_TabDimmed] = vec(kEnamel);
    c[ImGuiCol_TabDimmedSelected] = vec(kBrassDark);

    c[ImGuiCol_TableHeaderBg] = vec(kBrassDark);
    c[ImGuiCol_TableBorderStrong] = vec(withAlpha(kBrassBright, 0.55f));
    c[ImGuiCol_TableBorderLight] = vec(withAlpha(kBrassBright, 0.25f));
    c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = vec(withAlpha(kBrassBright, 0.05f));

    c[ImGuiCol_DragDropTarget] = vec(kGold);
    c[ImGuiCol_NavCursor] = vec(kAmber);
    c[ImGuiCol_NavWindowingHighlight] = vec(withAlpha(kIvory, 0.7f));
    c[ImGuiCol_NavWindowingDimBg] = vec(withAlpha(kVoid, 0.55f));
    c[ImGuiCol_ModalWindowDimBg] = vec(withAlpha(kVoid, 0.6f));

    // A TORN-OFF WINDOW IS A REAL OS WINDOW in this application, so its
    // background must be fully opaque. ImGui uses WindowBg for viewports; the
    // alpha set above is what keeps a map page from rendering see-through.
    c[ImGuiCol_WindowBg].w = 1.0f;
}

}  // namespace cascade::gui::theme
