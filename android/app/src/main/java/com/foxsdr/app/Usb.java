// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
package com.foxsdr.app;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.ParcelFileDescriptor;

import java.io.File;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * THE JAVA HALF OF THE USB PATH, AND THE ONLY REASON IT IS IN JAVA.
 *
 * <p>An Android application may not open a USB device from native code. There
 * is no NDK equivalent of {@code UsbManager}: the device list, the permission
 * dialog and the file descriptor are all framework objects, and
 * {@code /dev/bus/usb/*} is not readable by an app on any Android since 4.x
 * whatever its manifest says. What native code CAN do - and what this program
 * already does on desktop Linux - is talk usbfs on an ALREADY-OPEN file
 * descriptor. So this class does exactly the part only Java can do, and hands
 * the descriptor down:
 *
 * <ol>
 *   <li>enumerate {@link UsbManager#getDeviceList()} and keep the devices a
 *       driver in this app can actually open ({@link UsbIds});
 *   <li>ask the user for permission for each ({@link
 *       UsbManager#requestPermission});
 *   <li>on grant, {@link UsbManager#openDevice} and pass
 *       {@link UsbDeviceConnection#getFileDescriptor()} to the native
 *       registry, which from that moment reports the radio through the same
 *       {@code enumerateWinUsb()} every driver already calls - see
 *       {@code src/usb/usb_android_bridge.h} for the whole contract;
 *   <li>on detach, unregister and close.
 * </ol>
 *
 * <p>NATIVE CODE CALLS THIS CLASS FIRST, not the other way round. The five
 * {@code native} methods below are bound by {@code RegisterNatives} from
 * {@code android_main} (see {@code src/usb/usb_android_jni.hpp} for why they
 * cannot be resolved by name under a NativeActivity), and until that has
 * happened every one of them would throw {@code UnsatisfiedLinkError}. So
 * nothing here runs until {@link #onNativeReady} is called from native code;
 * {@link #onResume} and {@link #stop} check the same flag, because the
 * activity's lifecycle is not synchronised with the native thread's start.
 *
 * <p>THE CONNECTION STAYS OPEN FOR AS LONG AS THE DEVICE IS REGISTERED. That
 * is the contract the native side is written against: it never closes or
 * dup()s the descriptor at registration time - it dup()s when a driver
 * actually opens the radio - so {@link #OPEN} holds every
 * {@link UsbDeviceConnection} until the device is detached or the activity is
 * destroyed. Closing one earlier would pull the descriptor out from under a
 * running capture.
 *
 * <p>EVERY STEP IS LOGGED, through {@link #nativeLog} into
 * {@code core::diagLogf} rather than {@code android.util.Log}, so the Java
 * half's account of an attach lands in the same ordered log as the native
 * half's - in logcat AND in the in-memory ring the Diagnostics page shows.
 * On a tablet with a dongle that does not work, that log is the entire
 * evidence anyone will ever have.
 */
public final class Usb {

    private Usb() {}

    /** Our own broadcast, sent back to us by the permission dialog. */
    private static final String ACTION_PERMISSION = "com.foxsdr.app.USB_PERMISSION";

    /**
     * The launch extra that runs the on-device self-test. DEBUGGABLE BUILDS
     * ONLY - see {@link #maybeSelfTest}.
     */
    private static final String EXTRA_SELFTEST = "foxsdr_usb_selftest";

    /**
     * Open connections by {@link UsbDevice#getDeviceName()} (the kernel's own
     * {@code /dev/bus/usb/BBB/DDD} path, which is stable for as long as the
     * device is attached and is what a detach broadcast carries). The value
     * holds both halves that have to be released together.
     */
    private static final Map<String, Adopted> OPEN = new ConcurrentHashMap<>();

    /** One adopted device: Java's connection, and the native registration. */
    private static final class Adopted {
        final UsbDeviceConnection connection;
        final String nativePath;

        Adopted(UsbDeviceConnection connection, String nativePath) {
            this.connection = connection;
            this.nativePath = nativePath;
        }
    }

    /**
     * Devices a permission request is already outstanding for, so a rescan
     * (onResume, a second attach broadcast) cannot stack a second dialog on
     * the same radio. Cleared when the answer arrives, whichever way.
     */
    private static final Map<String, Boolean> ASKED = new ConcurrentHashMap<>();

    private static volatile boolean sNativeReady = false;
    private static BroadcastReceiver sReceiver = null;

    // --- the native seam ---------------------------------------------------
    //
    // Bound by RegisterNatives from android_main, NOT resolved by name: this
    // library is loaded by the framework (android.app.lib_name in the
    // manifest), not by System.loadLibrary, so there is no loadLibrary call
    // here and adding one would fail - see src/usb/usb_android_jni.hpp.

    static native void nativeLog(String line);

    /**
     * @return the path the native transport will know this device by, to be
     *         handed back to {@link #nativeUnregister}, or {@code null} if the
     *         descriptor was refused.
     */
    static native String nativeRegister(int fd, int vendorId, int productId, String serial,
                                        String product, String deviceName);

    static native void nativeUnregister(String path);

    static native int nativeRegisteredCount();

    /** What the Source section's own scan would list right now. */
    static native String nativeDescribeRadios();

    // --- lifecycle ---------------------------------------------------------

    /**
     * Called FROM NATIVE CODE (see {@code androidUsbInit}) once the native
     * methods above are bound. Registers the receiver and runs the first
     * scan; everything after this point is driven by Android's own
     * broadcasts.
     *
     * <p>Marshalled onto the main thread: this arrives on the
     * {@code android_main} thread, and while {@code registerReceiver} is
     * documented as safe from any thread, a permission dialog raised from a
     * thread the framework did not create is the kind of thing that works on
     * one Android version and not the next.
     */
    static void onNativeReady(final Activity activity) {
        sNativeReady = true;
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                nativeLog("java: armed; " + nativeRegisteredCount() + " device(s) adopted");
                registerReceiver(activity);
                scan(activity);
                maybeSelfTest(activity);
            }
        });
    }

    /**
     * Rescans from {@code MainActivity.onResume}. Cheap and idempotent: a
     * device already open is skipped, and a device already asked about is not
     * asked again. This is what covers the case Android's broadcasts do not -
     * a dongle plugged in while the app was in the background, or permission
     * granted from the system's own dialog after a launch.
     */
    static void onResume(Activity activity) {
        if (!sNativeReady) {
            return;
        }
        scan(activity);
    }

    /**
     * Closes everything, from {@code MainActivity.onDestroy}. Unregisters
     * before closing, in that order: a native driver holding a dup() of the
     * descriptor keeps working until it closes its own copy, but a
     * registration left behind would offer the Source section a device whose
     * descriptor is gone.
     */
    static void stop(Activity activity) {
        if (sReceiver != null) {
            try {
                activity.unregisterReceiver(sReceiver);
            } catch (IllegalArgumentException e) {
                // Never registered, or already unregistered. Not an error:
                // onDestroy can follow a failed onCreate.
            }
            sReceiver = null;
        }
        if (!sNativeReady) {
            return;
        }
        for (Map.Entry<String, Adopted> entry : new HashMap<>(OPEN).entrySet()) {
            release(entry.getKey(), "the activity is being destroyed");
        }
        ASKED.clear();
        nativeLog("java: stopped; " + nativeRegisteredCount() + " device(s) adopted");
    }

    // --- the scan / permission / open sequence -----------------------------

    private static void registerReceiver(Activity activity) {
        if (sReceiver != null) {
            return;
        }
        sReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                final String action = intent.getAction();
                if (action == null) {
                    return;
                }
                final UsbDevice device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                if (UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(action)) {
                    if (device == null) {
                        return;
                    }
                    nativeLog("java: attached " + describe(device));
                    requestOrOpen(context, device);
                } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                    if (device == null) {
                        return;
                    }
                    nativeLog("java: detached " + describe(device));
                    ASKED.remove(device.getDeviceName());
                    release(device.getDeviceName(), "the device was detached");
                } else if (ACTION_PERMISSION.equals(action)) {
                    if (device == null) {
                        return;
                    }
                    ASKED.remove(device.getDeviceName());
                    final boolean granted =
                            intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                    if (granted) {
                        nativeLog("java: permission GRANTED for " + describe(device));
                        open(context, device);
                    } else {
                        // THE ONE FAILURE A USER CAN FIX, so it is said
                        // plainly. Android gives an app no second chance at a
                        // denied device until it is replugged (or the app is
                        // restarted), and there is nothing this code can do
                        // about it but say so.
                        nativeLog("java: permission DENIED for " + describe(device)
                                + "; unplug and replug it, or clear this app's USB defaults,"
                                + " to be asked again");
                    }
                }
            }
        };
        final IntentFilter filter = new IntentFilter();
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        filter.addAction(ACTION_PERMISSION);
        // NOT EXPORTED, and on API 33+ that has to be said out loud: an app
        // targeting 34 that registers a receiver for a non-system broadcast
        // (ACTION_PERMISSION is ours) without naming the export state is
        // killed with a SecurityException. The system's own USB broadcasts
        // reach a not-exported receiver perfectly well - the flag governs
        // other APPS, not the platform.
        if (Build.VERSION.SDK_INT >= 33) {
            activity.registerReceiver(sReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            activity.registerReceiver(sReceiver, filter);
        }
        nativeLog("java: listening for USB attach/detach");
    }

    /**
     * Every attached device, filtered to the ones a driver here can open.
     * {@link UsbManager#getDeviceList()} returns EVERYTHING - keyboards,
     * hubs, the charger - and asking the user about each would be
     * indefensible, which is why {@link UsbIds} exists.
     */
    private static void scan(Activity activity) {
        final UsbManager manager = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
        if (manager == null) {
            // A device with no USB host support at all. The manifest declares
            // the feature not-required on purpose (the signal generator and
            // recorded I/Q need no radio), so this is a normal state, not a
            // fault - but it is worth one line, because "no radio offered" and
            // "this tablet cannot do USB host" look identical from the
            // outside.
            nativeLog("java: no UsbManager on this device; no USB radio can be offered");
            return;
        }
        final Map<String, UsbDevice> devices = manager.getDeviceList();
        int matched = 0;
        for (UsbDevice device : devices.values()) {
            if (!UsbIds.supported(device.getVendorId(), device.getProductId())) {
                continue;
            }
            ++matched;
            requestOrOpen(activity, device);
        }
        nativeLog("java: scan found " + devices.size() + " USB device(s), " + matched
                + " this app can open");
    }

    /** Opens the device if permission is already held, otherwise asks. */
    private static void requestOrOpen(Context context, UsbDevice device) {
        if (!UsbIds.supported(device.getVendorId(), device.getProductId())) {
            // A device that reached us through the manifest's attach filter
            // but is not in the Java table would be a generator bug, not a
            // user's problem - so it names the ids rather than being ignored.
            nativeLog("java: " + describe(device) + " is not a device any driver here opens");
            return;
        }
        if (OPEN.containsKey(device.getDeviceName())) {
            return;  // already adopted; a rescan must not reopen it
        }
        final UsbManager manager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        if (manager == null) {
            return;
        }
        if (manager.hasPermission(device)) {
            // The ordinary case for a device that launched us through the
            // manifest's USB_DEVICE_ATTACHED filter: Android grants
            // permission with the launch and there is no dialog at all.
            open(context, device);
            return;
        }
        if (ASKED.putIfAbsent(device.getDeviceName(), Boolean.TRUE) != null) {
            return;  // a dialog for this device is already in front of the user
        }
        // FLAG_IMMUTABLE ALWAYS, not only on API 31+. Android 12 made a
        // mutable PendingIntent without an explicit flag a hard error, and
        // the flag itself has existed since API 23 - minSdk here is 26, so
        // there is no version this has to be conditional for. setPackage()
        // keeps the broadcast to this app, which is what makes it safe to
        // receive as a not-exported receiver.
        final Intent intent = new Intent(ACTION_PERMISSION).setPackage(context.getPackageName());
        final PendingIntent pending = PendingIntent.getBroadcast(context, 0, intent,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        nativeLog("java: asking for permission for " + describe(device));
        manager.requestPermission(device, pending);
    }

    /** Opens a permission-granted device and registers its descriptor. */
    private static void open(Context context, UsbDevice device) {
        final UsbManager manager = (UsbManager) context.getSystemService(Context.USB_SERVICE);
        if (manager == null) {
            return;
        }
        if (OPEN.containsKey(device.getDeviceName())) {
            return;
        }
        final UsbDeviceConnection connection = manager.openDevice(device);
        if (connection == null) {
            // openDevice() answers null for a device another app holds, one
            // whose permission was revoked between the grant and here, and one
            // that has already been unplugged. It does not say which, so
            // neither does this - but it says WHICH DEVICE, which is what a
            // bug report needs.
            nativeLog("java: openDevice() returned null for " + describe(device)
                    + "; another app may hold it, or it was unplugged");
            return;
        }
        final int fd = connection.getFileDescriptor();
        if (fd < 0) {
            nativeLog("java: " + describe(device) + " opened but has no file descriptor");
            connection.close();
            return;
        }
        final String path = nativeRegister(fd, device.getVendorId(), device.getProductId(),
                serialOf(device), device.getProductName(), device.getDeviceName());
        if (path == null) {
            nativeLog("java: the native transport refused " + describe(device));
            connection.close();
            return;
        }
        OPEN.put(device.getDeviceName(), new Adopted(connection, path));
        // WHETHER IT ACTUALLY REACHED THE DRIVERS, asked rather than assumed.
        // A registration that lands but does not appear here would be a
        // native-side id-table mismatch, and this line is the only place it
        // would ever be visible on a real tablet.
        nativeLog("java: Source now lists: " + nativeDescribeRadios());
    }

    /** Unregisters and closes one device, by the key {@link #OPEN} uses. */
    private static void release(String deviceName, String why) {
        final Adopted adopted = OPEN.remove(deviceName);
        if (adopted == null) {
            return;
        }
        nativeUnregister(adopted.nativePath);
        adopted.connection.close();
        nativeLog("java: released " + deviceName + " (" + adopted.nativePath + ") because "
                + why + "; Source now lists: " + nativeDescribeRadios());
    }

    // --- diagnostics -------------------------------------------------------

    /** "0bda:2838 RTL2838UHIDIR at /dev/bus/usb/001/003" - one log line. */
    private static String describe(UsbDevice device) {
        final String product = device.getProductName();
        return String.format("%04x:%04x %s at %s", device.getVendorId(), device.getProductId(),
                (product == null || product.isEmpty()) ? "(no product name)" : product,
                device.getDeviceName());
    }

    /**
     * {@link UsbDevice#getSerialNumber()} throws a {@link SecurityException}
     * on API 29+ for an app without permission for the device, and returns
     * null for a dongle with no serial EEPROM. Both are normal; neither is
     * worth losing a radio over.
     */
    private static String serialOf(UsbDevice device) {
        try {
            return device.getSerialNumber();
        } catch (SecurityException e) {
            return null;
        }
    }

    // --- the on-device self-test -------------------------------------------

    /**
     * REGISTERS A FAKE DEVICE AND PROVES THE WHOLE PATH, on a machine with no
     * radio - which is every emulator and this project's whole CI story for
     * Android.
     *
     * <p>There is no USB device on an emulator and there is no radio on the
     * build box, so the one thing that cannot otherwise be verified before a
     * real dongle arrives is the JNI round trip itself: that Java's call
     * reaches {@code foxsdr_usb_register}, that the native transport then
     * offers the device to the drivers, and that unregistering takes it away
     * again. This opens {@code /dev/null} - an ordinary file descriptor,
     * which is all the registry wants until something opens the radio - claims
     * it is the commonest RTL-SDR in the world, and reads the answer back
     * through {@link #nativeDescribeRadios}.
     *
     * <p>DEBUGGABLE BUILDS ONLY, and gated twice: the launch intent must carry
     * {@code --ez foxsdr_usb_selftest true} AND the installed package must
     * carry {@code FLAG_DEBUGGABLE}. A release apk ignores the extra
     * entirely. Run it with:
     *
     * <pre>
     * adb shell am start -S -n com.foxsdr/com.foxsdr.app.MainActivity \
     *     --ez foxsdr_usb_selftest true
     * adb logcat -s FoxSDR | grep 'usb:'
     * </pre>
     *
     * <p>{@code -S} matters: the activity is {@code singleTop}, so without it
     * a running instance keeps its original intent and the extra is never
     * seen.
     */
    private static void maybeSelfTest(Activity activity) {
        final Intent intent = activity.getIntent();
        if (intent == null || !intent.getBooleanExtra(EXTRA_SELFTEST, false)) {
            return;
        }
        final ApplicationInfo info = activity.getApplicationInfo();
        if ((info.flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0) {
            nativeLog("selftest: refused - this is not a debuggable build");
            return;
        }
        nativeLog("selftest: begin (no real radio is involved)");
        ParcelFileDescriptor pfd = null;
        boolean ok = true;
        try {
            pfd = ParcelFileDescriptor.open(new File("/dev/null"),
                    ParcelFileDescriptor.MODE_READ_WRITE);
            final int fd = pfd.getFd();
            final int before = nativeRegisteredCount();
            nativeLog("selftest: /dev/null is fd " + fd + "; " + before + " device(s) adopted");

            final String path = nativeRegister(fd, 0x0bda, 0x2838, "FOXSDR-SELFTEST",
                    "FoxSDR self-test RTL-SDR", "selftest:/dev/null");
            if (path == null) {
                nativeLog("selftest: FAIL - nativeRegister returned null");
                ok = false;
            } else {
                final int after = nativeRegisteredCount();
                final String listed = nativeDescribeRadios();
                nativeLog("selftest: registered as " + path + "; " + after
                        + " device(s) adopted; Source lists: " + listed);
                if (after != before + 1) {
                    nativeLog("selftest: FAIL - the adopted count did not rise by one");
                    ok = false;
                }
                // The serial is what the driver's label is built from, so
                // finding it in the Source listing is the proof that the
                // registration reached the drivers and not merely the
                // registry.
                if (!listed.contains("FOXSDR-SELFTEST")) {
                    nativeLog("selftest: FAIL - the Source listing does not name the"
                            + " registered device");
                    ok = false;
                }

                nativeUnregister(path);
                final int removed = nativeRegisteredCount();
                final String afterListed = nativeDescribeRadios();
                nativeLog("selftest: unregistered; " + removed + " device(s) adopted;"
                        + " Source lists: " + afterListed);
                if (removed != before) {
                    nativeLog("selftest: FAIL - the adopted count did not return to " + before);
                    ok = false;
                }
                if (afterListed.contains("FOXSDR-SELFTEST")) {
                    nativeLog("selftest: FAIL - the device is still listed after unregister");
                    ok = false;
                }
            }
        } catch (Exception e) {
            nativeLog("selftest: FAIL - " + e);
            ok = false;
        } finally {
            if (pfd != null) {
                try {
                    pfd.close();
                } catch (Exception e) {
                    nativeLog("selftest: could not close the descriptor: " + e);
                }
            }
        }
        nativeLog(ok ? "selftest: PASS" : "selftest: FAILED");
    }
}
