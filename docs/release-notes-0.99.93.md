# FoxSDR 0.99.93

**FoxSDR 0.99.93 adds bookmark groups of your own. Type a group name once - "MILANO MALPENSA", say - and every frequency you add with Add current lands in it; right-click a bookmark to move it to another group, to none, or to a new one; and a chosen group can now be renamed as well as removed. Asked for by an airband listener in Milan who keeps each airport's tower and ground frequencies together.**

This is a feature release; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.93.exe`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.93-x86_64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.93-linux-x64.tar.gz`:**
`TBD`

**SHA-256 of `FoxSDR-0.99.93-aarch64.AppImage`:**
`TBD`

**SHA-256 of `foxsdr-0.99.93-linux-arm64.tar.gz`:**
`TBD`

## Features and fixes, 2026-10-26

**Groups of your own in the Bookmarks section.** Groups have been part of the bookmark list since 0.99.19, but a group could only arrive with an imported file or an airport lookup - there was no way to make one by hand. A tester in Milan asked for exactly that: "MILANO MALPENSA" with Malpensa's tower, ground and approach, "MILANO LINATE" with Linate's, "LUGANO" with Lugano's. Now there is a "group (optional)" box under the bookmark name: whatever it says is the group the next Add current joins, and the box is not cleared afterwards, so a run of frequencies for one airport is a run of presses. Its arrow lists the groups the list already has.

**Move a bookmark with a right-click.** Every row's menu offers No group, each existing group, and a field for a new name typed there (blanks around it are not part of the name).

**Rename this group.** Beside Remove this group, for the group chosen in the group list; renaming onto a name the list already has merges the two groups.

The browser control's Add bookmark still takes a name only; a group is set from the application.
