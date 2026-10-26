# FoxSDR 0.99.93

**FoxSDR 0.99.93 adds bookmark groups of your own. Type a group name once - "MILANO MALPENSA", say - and every frequency you add with Add current lands in it; right-click a bookmark to move it to another group, to none, or to a new one; and a chosen group can now be renamed as well as removed. Asked for by an airband listener in Milan who keeps each airport's tower and ground frequencies together.**

This is a feature release; nothing else changes. The Microsoft Store copy stays on its current version until the next submission is certified.

---

**SHA-256 of `foxsdr-setup-0.99.93.exe`:**
`26902dc3f0d24c7ce1b09a4821e5105336e7c10f288a72bda17bf31e802c85d9`

**SHA-256 of `FoxSDR-0.99.93-x86_64.AppImage`:**
`8819d0f1d9cde410abb5d274da04a797a676afc9d039b5ff915df4f57929bc7b`

**SHA-256 of `foxsdr-0.99.93-linux-x64.tar.gz`:**
`cd1643e61f46791b5d4ae657072cef20b26f9f98c26cacb85f544fc1f1db4292`

**SHA-256 of `FoxSDR-0.99.93-aarch64.AppImage`:**
`efc9aee1b1dbd1a24c47e22a53bfcfb8becb573b9b44f33cf827cacf1c27d196`

**SHA-256 of `foxsdr-0.99.93-linux-arm64.tar.gz`:**
`5d6c21da1f5c91a6af9eee1d479a2e9a77fa10546c62226c58fa7fe056f1d26d`

## Features and fixes, 2026-10-26

**Groups of your own in the Bookmarks section.** Groups have been part of the bookmark list since 0.99.19, but a group could only arrive with an imported file or an airport lookup - there was no way to make one by hand. A tester in Milan asked for exactly that: "MILANO MALPENSA" with Malpensa's tower, ground and approach, "MILANO LINATE" with Linate's, "LUGANO" with Lugano's. Now there is a "group (optional)" box under the bookmark name: whatever it says is the group the next Add current joins, and the box is not cleared afterwards, so a run of frequencies for one airport is a run of presses. Its arrow lists the groups the list already has.

**Move a bookmark with a right-click.** Every row's menu offers No group, each existing group, and a field for a new name typed there (blanks around it are not part of the name).

**Rename this group.** Beside Remove this group, for the group chosen in the group list; renaming onto a name the list already has merges the two groups.

The browser control's Add bookmark still takes a name only; a group is set from the application.
