#!/usr/bin/env python3
"""Generates the Android USB id table from installer/linux/99-foxsdr-sdr.rules.

WHY THIS SCRIPT EXISTS. Three files have to carry the same list of USB
vendor/product ids or a radio is invisible on one platform and not the other:

  installer/linux/99-foxsdr-sdr.rules          desktop Linux permissions
  android/app/src/main/res/xml/usb_device_filter.xml   the launch/permission
                                                       filter Android matches
                                                       an attached device
                                                       against
  android/app/src/main/java/com/foxsdr/app/UsbIds.java the same list in Java,
                                                       because getDeviceList()
                                                       returns EVERY attached
                                                       device and the app must
                                                       only ask for permission
                                                       for the ones a driver
                                                       here can open

The rules file is the text form of the native drivers' own id tables
(rtlSdrUsbIds(), hackRfUsbIds(), airspyUsbIds(), airspyHfUsbIds(),
msi2500::usbIds(), rx888UsbIds() - the set was verified equal to them when
this script was written), so it is the one file a person edits and the other
two are written from it. tests/test_usb_android_ids.cpp then pins ALL FOUR
sets equal, including the compiled-in driver tables, so neither a hand-edit
here nor a new id added to a driver can drift without a test going red.

Usage:
  tools/gen-android-usb-ids.py            rewrite both generated files
  tools/gen-android-usb-ids.py --check    exit 1 if either is out of date

SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RULES = os.path.join(REPO, "installer", "linux", "99-foxsdr-sdr.rules")
XML = os.path.join(REPO, "android", "app", "src", "main", "res", "xml", "usb_device_filter.xml")
JAVA = os.path.join(
    REPO, "android", "app", "src", "main", "java", "com", "foxsdr", "app", "UsbIds.java"
)

# One udev line, with the section comment above it carried through so the
# generated files stay readable: "# --- RTL2832U-based ..." becomes a comment
# in both outputs.
RULE_RE = re.compile(
    r'ATTR\{idVendor\}=="([0-9a-fA-F]{4})".*?ATTR\{idProduct\}=="([0-9a-fA-F]{4})"'
)
SECTION_RE = re.compile(r"^#\s*---\s*(.*?)\s*-*\s*$")


def read_rules():
    """[(section, vid, pid)] in the order the rules file lists them."""
    out = []
    section = ""
    with open(RULES, "r", encoding="utf-8") as f:
        for line in f:
            sec = SECTION_RE.match(line)
            if sec is not None:
                section = sec.group(1)
                continue
            m = RULE_RE.search(line)
            if m is not None:
                out.append((section, int(m.group(1), 16), int(m.group(2), 16)))
    return out


def render_xml(ids):
    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!--",
        "  SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0",
        "",
        "  GENERATED FILE - DO NOT EDIT BY HAND.",
        "  Written by tools/gen-android-usb-ids.py from",
        "  installer/linux/99-foxsdr-sdr.rules, which is the text form of the native",
        "  drivers' own id tables. tests/test_usb_android_ids.cpp pins this file, that",
        "  one, UsbIds.java and the compiled-in driver tables to the same set.",
        "",
        "  WHAT AN ENTRY DOES. Android matches an attached device against these",
        "  <usb-device> entries: a match offers to launch FoxSDR when the device is",
        "  plugged in AND grants this application permission to open it without the",
        "  system dialog. The ids are DECIMAL - Android rejects 0x notation here,",
        "  which is the single most common mistake in this file - so each line",
        "  carries the hex form in a comment beside it.",
        "-->",
        "<resources>",
    ]
    section = None
    for sec, vid, pid in ids:
        if sec != section:
            section = sec
            lines.append("")
            lines.append("    <!-- %s -->" % sec)
        lines.append(
            '    <usb-device vendor-id="%d" product-id="%d" />  <!-- %04x:%04x -->'
            % (vid, pid, vid, pid)
        )
    lines.append("</resources>")
    return "\n".join(lines) + "\n"


def render_java(ids):
    lines = [
        "// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0",
        "package com.foxsdr.app;",
        "",
        "/**",
        " * GENERATED FILE - DO NOT EDIT BY HAND.",
        " *",
        " * <p>Written by {@code tools/gen-android-usb-ids.py} from",
        " * {@code installer/linux/99-foxsdr-sdr.rules}, which is the text form of the",
        " * native drivers' own id tables ({@code rtlSdrUsbIds()} and the five beside",
        " * it). {@code tests/test_usb_android_ids.cpp} pins this table, the udev rules,",
        " * {@code res/xml/usb_device_filter.xml} and the compiled-in driver tables to",
        " * the same set, so a radio cannot become openable on desktop Linux and stay",
        " * invisible on a phone.",
        " *",
        " * <p>WHY JAVA NEEDS THE LIST AT ALL when the manifest already has the xml:",
        " * {@code UsbManager.getDeviceList()} returns EVERY attached device - a",
        " * keyboard, a hub, a phone charger - and asking the user for permission for",
        " * each of them would be indefensible. The xml decides what Android LAUNCHES",
        " * this app for; this table decides what the app asks about once it is",
        " * running. They must agree.",
        " */",
        "final class UsbIds {",
        "",
        "    private UsbIds() {}",
        "",
        "    /** {vendorId, productId} pairs, hex, in the rules file's own order. */",
        "    static final int[][] SUPPORTED = {",
    ]
    section = None
    for sec, vid, pid in ids:
        if sec != section:
            section = sec
            lines.append("")
            lines.append("        // %s" % sec)
        lines.append("        {0x%04x, 0x%04x}," % (vid, pid))
    lines += [
        "    };",
        "",
        "    /** True when this vendor/product pair is one a driver in this app opens. */",
        "    static boolean supported(int vendorId, int productId) {",
        "        for (int[] id : SUPPORTED) {",
        "            if (id[0] == vendorId && id[1] == productId) {",
        "                return true;",
        "            }",
        "        }",
        "        return false;",
        "    }",
        "}",
    ]
    return "\n".join(lines) + "\n"


def main():
    check = "--check" in sys.argv[1:]
    ids = read_rules()
    if not ids:
        print("gen-android-usb-ids: no ids parsed from %s" % RULES, file=sys.stderr)
        return 2
    seen = {}
    for sec, vid, pid in ids:
        key = (vid, pid)
        if key in seen:
            print(
                "gen-android-usb-ids: %04x:%04x listed twice in the rules file" % key,
                file=sys.stderr,
            )
            return 2
        seen[key] = sec

    stale = False
    for path, text in ((XML, render_xml(ids)), (JAVA, render_java(ids))):
        current = None
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8", newline="") as f:
                current = f.read()
        if current == text:
            print("gen-android-usb-ids: %s is current" % os.path.relpath(path, REPO))
            continue
        stale = True
        if check:
            print("gen-android-usb-ids: %s is OUT OF DATE" % os.path.relpath(path, REPO))
            continue
        # newline="\n" on purpose: these files are read by a Linux build and by
        # aapt2, and this script is run from Windows as often as not.
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("gen-android-usb-ids: wrote %s" % os.path.relpath(path, REPO))

    print("gen-android-usb-ids: %d USB ids" % len(ids))
    if check and stale:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
