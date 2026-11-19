# Field fixes B (application): 0.99.109 faults reported after release

Branch fix/field-b from engine/multi-radio 03ac57b7. Local only: nothing pushed, nothing published.

## Fault 2: the VOR decoder's "OBS course" control ignored a click (drive row L596)

CAUSE. It is not a slider and not the plugin's settings row. The VOR decoder (vor-decoder 1.0.1) declares an instrument face; the
application draws it as NavBearingFace, whose only control is ObsKnob (src/slint/ui/instruments.slint, ObsKnob, 40 px round).
ObsKnob turned only by wheel, by a RELATIVE press-drag and by Left / Right. Its press handler recorded the starting angle and
nothing else, and there was no release handler, so a click without a drag never produced a course. It carries
accessible-role: slider and the label "OBS course", which is why the drive (and a reader of the accessibility tree) called it a
slider. The scripted `slider "OBS course" 0.5` clicks the exact centre of the 40 px knob, where there is no bearing at all.

Slider (src/slint/ui/sliders.slint) is NOT affected: a press on its track already jumps the value there. Checked with
test_slint_sliders (254 checks, green; track presses at the middle, both overhangs and the Gain lane of SliderGroup).

CHANGE. ObsKnob: a left press that moves 3 px or less and is released takes the course to the bearing of the pointer from the
knob's middle (0 up, clockwise, whole degrees). A click within 6 px of the middle does nothing (no bearing). A drag stays relative,
as before (it never jumps to where the hand took it, and its release does not jump either).

PROOF. src/slint/tests/test_slint_instruments.cpp, checkObs: clicks at 3, 6, 9, 12 o'clock and half past four give 090, 180, 270,
000, 135; the middle gives no event; a press-drag ending where it began adds no jump. RED before the change (4 of 1725 failed:
lines 872, 875, 878, 881: the clicks at 180, 270, 000 and 135 produced no event), GREEN after (1725 checks, 0 failed).

NOT PROVED. Not driven with the real mouse on the VOR page in the real window. The drive script
src/slint/tests/drive/plugin-vor-decoder.txt still presses the knob's centre (`slider "OBS course" 0.5`), which by design still
does nothing; a real-pointer pass needs a click off-centre (for example on the ring at the right, expect 090).
