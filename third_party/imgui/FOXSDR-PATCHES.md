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
