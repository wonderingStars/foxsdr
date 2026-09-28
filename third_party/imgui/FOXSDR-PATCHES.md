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

## mouse-passthrough-cache - skip the per-frame GLFW_MOUSE_PASSTHROUGH syscall when it has not changed

**Files:** `backends/imgui_impl_glfw.cpp` (a small cache,
`ImGui_ImplGlfw_ShouldSetMousePassthrough` and
`ImGui_ImplGlfw_ClearMousePassthroughCache`, all just above
`ImGui_ImplGlfw_UpdateMouseData`; one `if` at the call site inside that
function; a reset call at the end of `ImGui_ImplGlfw_Shutdown`; a clear call in
`ImGui_ImplGlfw_DestroyWindow`), `backends/imgui_impl_glfw.h` (four test-only
declarations).

**Why.** Upstream calls `glfwSetWindowAttrib(window, GLFW_MOUSE_PASSTHROUGH,
window_no_input)` for every platform viewport on every single frame,
unconditionally. On Windows that is `GetWindowLongW` plus
`SetWindowLongW(GWL_EXSTYLE)` and, when enabling, `SetLayeredWindowAttributes`
too - real syscalls through win32u.dll - and `window_no_input` is only ever
true for a viewport mid-drag under multi-viewport docking, so on the
overwhelming majority of frames this sets a window's style bits back to what
they already are. Field report "hang win32u.dll @
_glfwSetWindowMousePassthroughWin32" (0.99.26, nine reports from one session,
foxsdr.com crash store): the GUI thread was captured with its top frame inside
this exact call, past the watchdog's 5 s threshold, while other threads in the
same capture sat inside dwmapi.dll and the Intel display driver
(ig9icd64.dll) - the desktop compositor and GPU driver were themselves
congested, and a call with nothing to justify running that frame is what the
GUI thread happened to be inside when an ordinary win32u syscall turned slow.

**What it changes.** A fixed-capacity table (`GLFWwindow*` to the last value
set, 32 entries) records what was last asked for on each window.
`ImGui_ImplGlfw_ShouldSetMousePassthrough(window, wanted)` returns false - skip
the call - when a record exists and already matches; otherwise it records the
new value and returns true, so the call still fires the first time a window is
seen and every time the wanted value actually changes. The table is plain data
with no destructor, cleared wholesale in `ImGui_ImplGlfw_Shutdown` (a fresh
backend life gets a fresh cache), AND `ImGui_ImplGlfw_ClearMousePassthroughCache`
drops one window's own slot, called from `ImGui_ImplGlfw_DestroyWindow` right
before `glfwDestroyWindow` (0.99.44 repair - see below for what this replaced).
If the table's 32 slots are ever all in use, a further window simply gets the
call every frame, exactly as upstream always has for every window - the cache
never skips a call it has not recorded a matching prior value for.

**0.99.44 repair - the per-destroy clear was missing, and the reasoning for
leaving it out was wrong.** This patch originally cleared the table only
wholesale, in `Shutdown`, reasoning that a window pointer reused by the
allocator "can only make the cache skip a call that would have set the SAME
value the new window already starts with (GLFW resets `GLFW_MOUSE_PASSTHROUGH`
to false on every new window)". That reasoning only covers a destroyed window
whose last recorded value was already false. A window destroyed while its
cached value was TRUE leaves that record in the table; a later window created
at the SAME address (ordinary heap reuse - `GLFWwindow*` on this backend is a
plain allocation) inherits it, and the very next frame that wants passthrough
TRUE on the new window sees a "match" and skips the call - on a window whose
real Win32 style has never had that bit set at all, because it is brand new.
`ImGui_ImplGlfw_ClearMousePassthroughCache` removes the slot the moment the
window it describes stops existing, so a reused address always starts from "no
record" like any other new window.

**What it does not change.** The value passed to `glfwSetWindowAttrib` is
unchanged, and it is still called at least once per window and on every frame
the wanted value differs from the last one applied - a multi-viewport drag
still gets pass-through set and cleared exactly when it did before. Only the
redundant repeats, which upstream never checked for, are removed.

**Re-applying on an ImGui upgrade.**
1. Vendor the new upstream unmodified first (`THIRD_PARTY.md`).
2. `grep -n "FOXSDR PATCH.*mouse-passthrough-cache"` in the previous copy
   finds all five places (four in the .cpp, one block of declarations in the
   .h). Put the cache struct, `ImGui_ImplGlfw_ShouldSetMousePassthrough` and
   `ImGui_ImplGlfw_ClearMousePassthroughCache` just above
   `ImGui_ImplGlfw_UpdateMouseData`; wrap the `glfwSetWindowAttrib(window,
   GLFW_MOUSE_PASSTHROUGH, window_no_input)` call inside
   `ImGui_ImplGlfw_UpdateMouseData` in the `if`; add the reset call at the end
   of `ImGui_ImplGlfw_Shutdown`; add the clear call in
   `ImGui_ImplGlfw_DestroyWindow` right before `glfwDestroyWindow`; the four
   test-only declarations at the end of the public block in the header.
   If upstream starts caching this itself, drop the patch.
3. Run `test_mouse_passthrough_cache`: a steady frame loop with nothing asking
   for pass-through must show exactly 1 call, a value that changes every frame
   must show one call per change, and a window destroyed then "recreated" at
   the identical key (a fabricated pointer stands in for a reused address)
   must not inherit its predecessor's last value.
