// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
package com.foxsdr.app;

/**
 * GENERATED FILE - DO NOT EDIT BY HAND.
 *
 * <p>Written by {@code tools/gen-android-usb-ids.py} from
 * {@code installer/linux/99-foxsdr-sdr.rules}, which is the text form of the
 * native drivers' own id tables ({@code rtlSdrUsbIds()} and the five beside
 * it). {@code tests/test_usb_android_ids.cpp} pins this table, the udev rules,
 * {@code res/xml/usb_device_filter.xml} and the compiled-in driver tables to
 * the same set, so a radio cannot become openable on desktop Linux and stay
 * invisible on a phone.
 *
 * <p>WHY JAVA NEEDS THE LIST AT ALL when the manifest already has the xml:
 * {@code UsbManager.getDeviceList()} returns EVERY attached device - a
 * keyboard, a hub, a phone charger - and asking the user for permission for
 * each of them would be indefensible. The xml decides what Android LAUNCHES
 * this app for; this table decides what the app asks about once it is
 * running. They must agree.
 */
final class UsbIds {

    private UsbIds() {}

    /** {vendorId, productId} pairs, hex, in the rules file's own order. */
    static final int[][] SUPPORTED = {

        // RTL2832U-based RTL-SDR dongles
        {0x0bda, 0x2832},
        {0x0bda, 0x2838},
        {0x0413, 0x6680},
        {0x0413, 0x6f0f},
        {0x0458, 0x707f},
        {0x0ccd, 0x00a9},
        {0x0ccd, 0x00b3},
        {0x0ccd, 0x00b4},
        {0x0ccd, 0x00b5},
        {0x0ccd, 0x00b7},
        {0x0ccd, 0x00b8},
        {0x0ccd, 0x00b9},
        {0x0ccd, 0x00c0},
        {0x0ccd, 0x00c6},
        {0x0ccd, 0x00d3},
        {0x0ccd, 0x00d7},
        {0x0ccd, 0x00e0},
        {0x1554, 0x5020},
        {0x15f4, 0x0131},
        {0x15f4, 0x0133},
        {0x185b, 0x0620},
        {0x185b, 0x0650},
        {0x185b, 0x0680},
        {0x1b80, 0xd393},
        {0x1b80, 0xd394},
        {0x1b80, 0xd395},
        {0x1b80, 0xd397},
        {0x1b80, 0xd398},
        {0x1b80, 0xd39d},
        {0x1b80, 0xd3a4},
        {0x1b80, 0xd3a8},
        {0x1b80, 0xd3af},
        {0x1b80, 0xd3b0},
        {0x1d19, 0x1101},
        {0x1d19, 0x1102},
        {0x1d19, 0x1103},
        {0x1d19, 0x1104},
        {0x1f4d, 0xa803},
        {0x1f4d, 0xb803},
        {0x1f4d, 0xc803},
        {0x1f4d, 0xd286},
        {0x1f4d, 0xd803},

        // HackRF (One, Jawbreaker, rad1o)
        {0x1d50, 0x6089},
        {0x1d50, 0x604b},
        {0x1d50, 0xcc15},

        // RX888 mk2 (SDDC/FX3 firmware) - the bootloader id AND the streamer id
        {0x04b4, 0x00f1},
        {0x04b4, 0x00f3},

        // Mirics MSi2500 + MSi001 (and the television sticks built on it)
        {0x1df7, 0x2500},
        {0x1df7, 0x3000},
        {0x1df7, 0x3010},
        {0x2040, 0xd300},
        {0x07ca, 0x8591},
        {0x04bb, 0x0537},
        {0x0511, 0x0037},

        // RTL-SDR and the in-tree DVB driver
        {0x1d50, 0x60a1},
        {0x03eb, 0x800c},
    };

    /** True when this vendor/product pair is one a driver in this app opens. */
    static boolean supported(int vendorId, int productId) {
        for (int[] id : SUPPORTED) {
            if (id[0] == vendorId && id[1] == productId) {
                return true;
            }
        }
        return false;
    }
}
