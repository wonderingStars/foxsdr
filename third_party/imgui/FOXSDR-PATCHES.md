# FoxSDR patches to the vendored Dear ImGui

Dear ImGui is vendored unmodified (`third_party/THIRD_PARTY.md`) with the
exceptions listed here. Each is fenced in the source by `FOXSDR PATCH` comments
naming it, so `grep -n "FOXSDR PATCH" third_party/imgui/*` finds every changed
line. Anything not fenced is upstream, byte for byte.

## cjk-wrap - word wrapping for Chinese and Japanese

**File:** `imgui_draw.cpp`, around `ImFontCalcWordWrapPositionEx` (the one
routine every wrapped text in ImGui goes through: `TextWrapped`,
`PushTextWrapPos`, `CalcTextSize` with a wrap width, `ImDrawList::AddText`
with a wrap width).

**Why.** Upstream breaks a line only at a space, or after `.,;!?"` and the
ideographic comma and full stop. Chinese and Japanese put no space between
words, so a whole sentence was one "word": ImGui either cut it wherever the
line ran out (putting a `。` or `，` at the start of the next line, and cutting
a Latin word such as `GPS` in two) or, when the "word" fitted a line on its
own, moved it whole to the next line and left the line before it short. All
three were seen on the first rendered check of a Chinese catalogue (0.99.28).
FoxSDR has no wrapping of its own - some 135 call sites wrap through ImGui - so
the fix has to be here.

**What it changes.** Four things, and only for text that contains CJK:

1. A line may break before or after any CJK character: ideographs, kana, the
   CJK symbols and punctuation block, bopomofo, compatibility ideographs and
   the fullwidth forms. Hangul is deliberately NOT included - Korean puts
   spaces between words and keeps breaking at them, exactly as upstream.
2. Never before a closing mark: `、。〉》」』】〕〗〙！），．：；？］｝`, and, after a
   CJK character, ASCII `, . ! ? ) ; :` and the closing quotes `” ’`.
3. Never after an opening mark: `〈《「『【〔〖〘（［｛`, `“ ‘`, and the ASCII
   `( [ {` - the catalogues set an ASCII bracket straight before CJK text
   ("模式: NFM (窄带调频)"), and a break after it left the bracket alone at a
   line's end (34-language screenshot review). No English line changes: a
   break after an ASCII bracket is only ever offered by the CJK rule.
4. A Latin word inside CJK text stays whole: a break is allowed where it meets
   CJK, and upstream's break after `.` is suppressed inside a word that
   touches CJK, so `foxsdr.com` is one word. A word longer than the line is
   still cut, as upstream cuts one.

**What it does not change.** Text with no CJK character: none of the new
conditions can become true for it, so it wraps exactly as upstream -
`tests/test_cjk_wrap.cpp` wraps every English catalogue key (about 1,500
strings) and a set of corner-case paragraphs at four widths with both the
patched routine and a verbatim copy of the upstream one, and requires every
break on the same byte; it does the same for Korean. Not implemented (not
requested): Japanese kinsoku for small kana and `ー` at a line start.

**The code.** Five static helpers above the routine (`ImFoxWrapIsCjk`,
`ImFoxWrapIsCjkClosing`, `ImFoxWrapIsOpening`, `ImFoxWrapIsAsciiClosing`,
`ImFoxWrapIsAsciiWord`, `ImFoxWrapCjkAhead`); two state variables after
`keep_blanks`; one block after the character is classified that works out
`fox_break_before` / `fox_no_break`; `&& !fox_no_break` on upstream's
"End span: '.X'" condition; and one `else if (fox_break_before)` branch that
ends the span the same way upstream's branches do.

**Re-applying on an ImGui upgrade.**
1. Vendor the new upstream unmodified first (`THIRD_PARTY.md`).
2. Find the routine: `grep -n "ImFontCalcWordWrapPositionEx" imgui_draw.cpp`.
   If upstream has gained CJK line breaking of its own, test it with
   `test_cjk_wrap` BEFORE re-applying anything - the patch may be obsolete.
3. Otherwise copy the fenced blocks from the previous `imgui_draw.cpp` (or
   from `git log -p -- third_party/imgui/imgui_draw.cpp`) into the same
   places: helpers before the routine, state after `keep_blanks`, the
   decision block after `curr_type` is set, the `!fox_no_break` on the
   `'.X'` condition, the new `else if` after upstream's keep-blanks branch.
4. Update the verbatim copy of the upstream routine in
   `tests/test_cjk_wrap.cpp` (`refWrap`) to the NEW upstream code, so the
   English comparison is against the version actually vendored.
5. Run `test_cjk_wrap`: English and Korean must show `0 differ`, Chinese and
   Japanese `0 faults`.

## popup-colours - a colour table for every popup-like window

**Files:** `imgui.h` (one declaration, at the end of `namespace ImGui`),
`imgui_internal.h` (one field, `FoxPopupColorsPushed`, at the end of
`ImGuiWindowStackData`), `imgui.cpp` (`ImGui::FoxSetPopupColors` and its
table just above `ImGui::Begin`, a push loop in `Begin`, a pop in `End`).

**Why.** ImGui letters a popup - a combo's list, a context menu, a modal, a
tooltip - in the same `ImGuiCol_Text` as every window, and there is no hook
between "a popup window begins" and "its contents are drawn". FoxSDR's
themes (`src/gui/theme.hpp`) give menus their own ground and ink
(foxsdr-ui/1's menuBg / menuText / menuHi / menuBorder), and on Field Radio -
cream menus over olive panels - the shared Text colour put cream words on a
cream list at 1.09:1 in every combo, tooltip and context menu (found by the
themes review, 2026-09-25). Pushing the menu ink at each call site would have
meant well over a hundred tooltips plus every combo and popup, and ImGui's
own internal popups, with nothing to stop the next one being missed.

**What it changes.** `FoxSetPopupColors(idx, col, count)` copies a table of
at most 32 `(ImGuiCol, ImVec4)` pairs. `Begin()` pushes the whole table,
straight after it records the window's stack sizes, for any window with
`ImGuiWindowFlags_Popup` or `ImGuiWindowFlags_Tooltip` (popups, combo lists,
menus and submenus, modals, tooltips) and counts the pushes in the window's
stack entry; `End()` pops exactly that many just before its own stack-size
check, so the pushes belong to the window's scope and never trip ImGui's
"PopStyleColor too many / too few" error checks. `theme::applyTheme()` sets
the table; `theme::popupColours()` returns it for a test.

**What it does not change.** With the table empty (count 0, the default, and
what FoxSDR's default "today" bench sets) nothing is pushed or popped: the
library behaves byte for byte as upstream. Ordinary windows and child windows
are never touched.

**Re-applying on an ImGui upgrade.**
1. Vendor the new upstream unmodified first (`THIRD_PARTY.md`).
2. `grep -n "FOXSDR PATCH (popup-colours)"` in the previous copies finds the
   four places. Put the table and `FoxSetPopupColors` above `ImGui::Begin`;
   the push loop directly after `ErrorRecoveryStoreState(&window_stack_data.
   StackSizesInBegin)` / `g.StackSizesInBeginForCurrentWindow = ...` in
   `Begin` (it must come AFTER the stored sizes); the pop directly before
   `ErrorRecoveryTryToRecoverWindowState` in `End` (it must come BEFORE it);
   the field in `ImGuiWindowStackData`; the declaration in `imgui.h`.
   If upstream has gained per-window-kind style colours of its own, prefer
   those and drop the patch.
3. Run `test_theme`: its "every pair ImGui draws - window, tooltip, popup,
   modal" section reads the colours a live tooltip, popup and modal draw with,
   and fails for Field Radio (1.09:1) if the push does not happen.
