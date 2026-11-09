// input_target.hpp - the scripted input's target: the live Slint window (input_script.hpp has the script and the driver).
//
// A control is found BY ITS ACCESSIBLE LABEL through Slint's in-process testing API (slint-testing.h: ElementHandle, the same
// element tree the Windows accessibility provider lists, docs 6.46), and driven the way the accessibility tree or a pointer
// would: `click` invokes the element's default action (what a screen reader's "invoke" does - keys.slint, fields.slint and the
// rest declare accessible-action-default), `press` and `slider` dispatch a pointer press and release at a point inside the
// element through the window (the real TouchArea path), `type` pointer-focuses the field, selects its text with Ctrl+A and
// types the characters as key events, `key` dispatches one key to whatever has the focus. Nothing of the operating system's
// pointer or keyboard is touched, so a test window on the hidden desktop cannot be steered by anyone's mouse.
//
// THE SDK FLAG. The prebuilt SDK (slint-cpp-1.18.1) was built with the TESTING feature - slint_generated_public.h defines
// SLINT_FEATURE_TESTING and slint_cpp.dll exports the slint_testing_* calls - but its header also asks for
// SLINT_FEATURE_EXPERIMENTAL, which the SDK's CMake config does not define (SlintTargets.cmake lists EXPERIMENTAL among the
// disabled features: that flag governs the compiler's experimental syntax and the Translator hook, not this API). It is
// defined here, for this header's include only, because the functions behind it are in the DLL (verified by reading its
// export table, 2026-11-06). Nothing else in the window depends on it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#ifndef SLINT_FEATURE_EXPERIMENTAL
#define SLINT_FEATURE_EXPERIMENTAL 1
#endif
#include <slint-testing.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "input_script.hpp"

namespace foxsdr::slintdrive {

// `state`: the host's read-back by name ("" when served, else the refusal); `shot`: the host's screenshot to a file.
using StateReader = std::function<std::string(const std::string& name, std::string& out)>;
using ShotWriter = std::function<std::string(const std::string& file)>;

template <class W>
class SlintDriveTarget : public DriveTarget {
public:
    SlintDriveTarget(const slint::ComponentHandle<W>& ui, StateReader state, ShotWriter shot) : ui_(ui), state_(std::move(state)), shot_(std::move(shot)) {}

    std::string click(const std::string& labelSpec) override {
        std::optional<slint::testing::ElementHandle> e;
        std::string why = find(labelSpec, e);
        if (!why.empty()) { return why; }
        if (const auto enabled = e->accessible_enabled(); enabled.has_value() && !*enabled) { return "\"" + labelSpec + "\" is disabled"; }
        e->invoke_accessible_default_action();
        return std::string();
    }

    std::string press(const std::string& labelSpec) override {
        std::optional<slint::testing::ElementHandle> e;
        std::string why = find(labelSpec, e);
        if (!why.empty()) { return why; }
        float x = 0, y = 0;
        why = centre(labelSpec, *e, x, y);
        if (!why.empty()) { return why; }
        pointerClick(x, y);
        return std::string();
    }

    std::string type(const std::string& labelSpec, const std::string& text) override {
        std::optional<slint::testing::ElementHandle> e;
        std::string why = find(labelSpec, e);
        if (!why.empty()) { return why; }
        float x = 0, y = 0;
        why = centre(labelSpec, *e, x, y);
        if (!why.empty()) { return why; }
        pointerClick(x, y);
        // A field inside a PopupWindow: its position is in the popup's frame, so the click above landed OUTSIDE the popup and closed it, and the
        // field is gone. Keys sent now would reach the dropped field in the same tick - the access violation of 2026-11-09 (docs 6.47: the
        // field's focus-grab Timer stopped during the popup's teardown, slint_cpp.dll+0x53D03E), a sequence no pointer and keyboard can
        // produce within one frame. The step is refused instead; a popup's field is typed with `key` (it has the focus when the popup opens).
        if (!e->is_valid()) { return "the pointer click removed \"" + labelSpec + "\" (a popup's field closes with its popup): type into it with key steps"; }
        // select whatever is there, then type over it
        keyDown(slint::platform::key_codes::Control);
        keyTap(u8"a");
        keyUp(slint::platform::key_codes::Control);
        typeText(text);
        return std::string();
    }

    std::string key(const std::string& name) override {
        namespace kc = slint::platform::key_codes;
        if (name.size() == 6 && name.rfind("ctrl+", 0) == 0) {
            const char letter[2] = {name[5], '\0'};
            keyDown(kc::Control);
            keyTap(std::u8string(reinterpret_cast<const char8_t*>(letter)));
            keyUp(kc::Control);
            return std::string();
        }
        std::u8string code;
        if (name.size() == 1) { code = std::u8string(1, static_cast<char8_t>(name[0])); }   // one character, as typed
        else if (name == "enter" || name == "return") { code = kc::Return; }
        else if (name == "escape") { code = kc::Escape; }
        else if (name == "tab") { code = kc::Tab; }
        else if (name == "backspace") { code = kc::Backspace; }
        else if (name == "delete") { code = kc::Delete; }
        else if (name == "space") { code = kc::Space; }
        else if (name == "up") { code = kc::UpArrow; }
        else if (name == "down") { code = kc::DownArrow; }
        else if (name == "left") { code = kc::LeftArrow; }
        else if (name == "right") { code = kc::RightArrow; }
        else if (name.size() >= 2 && name[0] == 'f') {
            static const std::u8string_view fkeys[12] = {kc::F1, kc::F2, kc::F3, kc::F4, kc::F5, kc::F6, kc::F7, kc::F8, kc::F9, kc::F10, kc::F11, kc::F12};
            const int n = std::atoi(name.c_str() + 1);
            if (n < 1 || n > 12) { return "no key named " + name; }
            code = fkeys[n - 1];
        } else { return "no key named " + name; }
        keyTap(code);
        return std::string();
    }

    std::string slider(const std::string& labelSpec, double frac) override {
        std::optional<slint::testing::ElementHandle> e;
        std::string why = find(labelSpec, e);
        if (!why.empty()) { return why; }
        const auto pos = e->absolute_position();
        const auto size = e->size();
        if (size.width <= 0 || size.height <= 0) { return "\"" + labelSpec + "\" has no size"; }
        const float x = pos.x + static_cast<float>(frac) * size.width;
        const float y = pos.y + size.height / 2;
        pointerClick(x, y);
        return std::string();
    }

    // A wheel scroll at the control's centre. Slint's Flickable moves its viewport BY the delta (a positive delta_y brings what is above
    // into view, as a wheel rolled away from the user does), so "pixels" positive = show what is below = a negative delta.
    std::string scroll(const std::string& labelSpec, double pixels) override {
        std::optional<slint::testing::ElementHandle> e;
        std::string why = find(labelSpec, e);
        if (!why.empty()) { return why; }
        float x = 0, y = 0;
        why = centre(labelSpec, *e, x, y);
        if (!why.empty()) { return why; }
        auto& w = ui_->window();
        w.dispatch_pointer_move_event(slint::LogicalPosition({x, y}));
        w.dispatch_pointer_scroll_event(slint::LogicalPosition({x, y}), 0.f, static_cast<float>(-pixels));
        return std::string();
    }

    // A dropdown's key: its default action is popup.show() (sections.slint SelectKey, which every bank's DropKey wraps).
    std::string open(const std::string& labelSpec) override { return click(labelSpec); }

    // The popup's row, the tick after: the visitor walks the active popups, so the row is found by its label once the frame between the two
    // ticks has instantiated it (the same tick found nothing, docs 6.47). It is INVOKED, not pointer-clicked: a PopupWindow is its own item
    // tree and its elements' absolute positions are in the popup's frame, so a pointer event at that point on the main window lands beside
    // the popup and only closes it. menus.slint's MenuItem and the banks' DropRow declare accessible-action-default = the row's click.
    std::string pick(const std::string& item) override {
        std::optional<slint::testing::ElementHandle> row;
        std::string why = find(item, row);
        if (!why.empty()) { return why; }
        if (const auto enabled = row->accessible_enabled(); enabled.has_value() && !*enabled) { return "item \"" + item + "\" is disabled"; }
        row->invoke_accessible_default_action();
        return std::string();
    }

    std::string read(const std::string& labelSpec, const std::string& what, std::string& out) override {
        std::optional<slint::testing::ElementHandle> e;
        std::string why = find(labelSpec, e);
        if (!why.empty()) { return why; }
        std::optional<slint::SharedString> s;
        std::optional<bool> b;
        if (what == "value") { s = e->accessible_value(); }
        else if (what == "label") { s = e->accessible_label(); }
        else if (what == "checked") { b = e->accessible_checked(); }
        else if (what == "enabled") { b = e->accessible_enabled(); }
        else { return "nothing to read called " + what; }
        if (s.has_value()) { out = std::string(*s); return std::string(); }
        if (b.has_value()) { out = *b ? "true" : "false"; return std::string(); }
        return "\"" + labelSpec + "\" has no " + what;
    }

    std::string state(const std::string& name, std::string& out) override { return state_ ? state_(name, out) : "no state reader"; }
    std::string shot(const std::string& file) override { return shot_ ? shot_(file) : "no screenshot writer"; }

    std::string dumpControls() override {
        int n = 0, all = 0, withRole = 0;
        std::fprintf(stderr, "input script: dump of the controls with an accessible label (role \"label\" value checked enabled x,y wxh type)\n");
        slint::testing::ElementHandle::visit_elements(ui_, [&](const slint::testing::ElementHandle& e) {
            ++all;
            const auto label = e.accessible_label();
            if (e.accessible_role().has_value()) { ++withRole; }
            if (!label.has_value() || label->empty()) { return; }
            ++n;
            const auto role = e.accessible_role();
            const auto value = e.accessible_value();
            const auto checked = e.accessible_checked();
            const auto enabled = e.accessible_enabled();
            const auto pos = e.absolute_position();
            const auto size = e.size();
            const auto type = e.type_name();
            std::fprintf(stderr, "  %-12s \"%s\" value=\"%s\" checked=%s enabled=%s at %.0f,%.0f %.0fx%.0f %s\n", roleName(role), std::string(*label).c_str(),
                         value.has_value() ? std::string(*value).c_str() : "", checked.has_value() ? (*checked ? "true" : "false") : "-",
                         enabled.has_value() ? (*enabled ? "true" : "false") : "-", static_cast<double>(pos.x), static_cast<double>(pos.y),
                         static_cast<double>(size.width), static_cast<double>(size.height), type.has_value() ? std::string(*type).c_str() : "");
        });
        // "0 elements visited" = the generated code carries no element debug info (src/slint/CMakeLists.txt sets SLINT_EMIT_DEBUG_INFO)
        std::fprintf(stderr, "input script: %d labelled controls (%d elements visited, %d with a role)\n", n, all, withRole);
        return std::string();
    }

private:
    static const char* roleName(const std::optional<slint::language::AccessibleRole>& r) {
        using R = slint::language::AccessibleRole;
        if (!r.has_value()) { return "-"; }
        switch (*r) {
            case R::Button: return "button";
            case R::Checkbox: return "checkbox";
            case R::Combobox: return "combobox";
            case R::Slider: return "slider";
            case R::Tab: return "tab";
            case R::Text: return "text";
            case R::TextInput: return "textinput";
            case R::ListItem: return "listitem";
            case R::RadioButton: return "radio";
            case R::Switch: return "switch";
            case R::Image: return "image";
            case R::List: return "list";
            case R::Table: return "table";
            case R::Groupbox: return "groupbox";
            default: return "other";
        }
    }

    // The n-th element (from 1) carrying the label; the refusal names the count when it is not there.
    std::string find(const std::string& labelSpec, std::optional<slint::testing::ElementHandle>& out) {
        std::string label = labelSpec;
        int index = 1;
        if (!splitLabelIndex(label, index)) { return "a label's #index counts from 1: \"" + labelSpec + "\""; }
        const bool indexed = label != labelSpec;   // "Band plan"#1 says which on purpose; a bare label with two controls is the author's mistake
        const auto all = slint::testing::ElementHandle::find_by_accessible_label(ui_, label);
        // A plain Text carries its own words as its accessible label (Slint's default), so a key's caption, a chip or a readout shares the
        // label of the control that shows it. A control (any role but text) wins; a text matches only when no control carries the label.
        std::vector<slint::testing::ElementHandle> found;
        for (const auto& e : all) { if (e.accessible_role() != slint::language::AccessibleRole::Text) { found.push_back(e); } }
        if (found.empty()) { for (const auto& e : all) { found.push_back(e); } }
        if (found.empty()) { return "no control labelled \"" + label + "\""; }
        if (static_cast<int>(found.size()) < index) { return std::to_string(found.size()) + " controls labelled \"" + label + "\", not " + std::to_string(index); }
        if (!indexed && found.size() > 1) { return std::to_string(found.size()) + " controls labelled \"" + label + "\" (say which with \"" + label + "\"#n)"; }
        out = found[static_cast<std::size_t>(index - 1)];
        return std::string();
    }

    std::string centre(const std::string& label, const slint::testing::ElementHandle& e, float& x, float& y) {
        const auto pos = e.absolute_position();
        const auto size = e.size();
        if (size.width <= 0 || size.height <= 0) { return "\"" + label + "\" has no size"; }
        x = pos.x + size.width / 2;
        y = pos.y + size.height / 2;
        const auto win = ui_->window().size();
        const float sf = std::max(0.5f, ui_->window().scale_factor());
        if (x < 0 || y < 0 || x > static_cast<float>(win.width) / sf || y > static_cast<float>(win.height) / sf) {
            return "\"" + label + "\" is outside the window at " + std::to_string(static_cast<int>(x)) + "," + std::to_string(static_cast<int>(y));
        }
        return std::string();
    }

    void pointerClick(float x, float y) {
        auto& w = ui_->window();
        w.dispatch_pointer_move_event(slint::LogicalPosition({x, y}));
        w.dispatch_pointer_press_event(slint::LogicalPosition({x, y}), slint::PointerEventButton::Left);
        w.dispatch_pointer_release_event(slint::LogicalPosition({x, y}), slint::PointerEventButton::Left);
    }
    void keyDown(std::u8string_view t) { ui_->window().dispatch_key_press_event(slint::SharedString(t)); }
    void keyUp(std::u8string_view t) { ui_->window().dispatch_key_release_event(slint::SharedString(t)); }
    void keyTap(std::u8string_view t) { keyDown(t); keyUp(t); }
    void typeText(const std::string& text) {
        // one key event per UTF-8 character (a multi-byte sequence stays one event)
        std::size_t i = 0;
        while (i < text.size()) {
            std::size_t n = 1;
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 0xF0) { n = 4; } else if (c >= 0xE0) { n = 3; } else if (c >= 0xC0) { n = 2; }
            const std::string ch = text.substr(i, n);
            keyTap(std::u8string(reinterpret_cast<const char8_t*>(ch.data()), ch.size()));
            i += n;
        }
    }

    slint::ComponentHandle<W> ui_;
    StateReader state_;
    ShotWriter shot_;
};

}  // namespace foxsdr::slintdrive
