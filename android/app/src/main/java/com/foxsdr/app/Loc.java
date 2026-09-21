// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
package com.foxsdr.app;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.pm.PackageManager;
import android.location.GnssStatus;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/**
 * The device's own position, for the receiver position FoxSDR needs.
 *
 * <p>WHY THIS IS IN JAVA. Android's location APIs are Java-only: there is no
 * NDK {@code LocationManager}, no native way to ask for a runtime permission,
 * and no native access to the fused provider. The whole sequence therefore
 * lives here and reaches native code through the five methods at the bottom of
 * this file, bound by {@code src/core/device_location_platform.cpp}. The same
 * arrangement as {@link Usb} for {@code UsbManager} and {@link Net} for TLS.
 *
 * <p>WHAT IT DOES, ONCE. {@link #start} asks for the permission if it does not
 * have it, turns on every provider the device actually has, and hands the
 * first position that arrives to native code - which cancels the request. It
 * is not a tracker: nothing here follows the device around, and no request is
 * ever left running. An antenna does not move while you are listening to it,
 * and a location request left on is a GNSS chip left powered on somebody's
 * battery.
 *
 * <p>BOTH PROVIDERS, ON PURPOSE. {@code GPS_PROVIDER} is accurate to a few
 * metres and needs sky; {@code NETWORK_PROVIDER} answers indoors in a second
 * and can be a kilometre out. A receiver position that is a kilometre wrong
 * still predicts a satellite pass badly, so which one answered is passed to
 * native code with the fix and the interface says so - the user can then
 * decide whether to go outside and press it again. Taking only GPS would leave
 * an indoor tablet with nothing at all; taking only network would quietly be
 * wrong.
 *
 * <p>THE LAST KNOWN POSITION IS NOT USED. {@code getLastKnownLocation} returns
 * whatever the platform cached, which can be hours old and hundreds of miles
 * away - a position from the airport you flew in from is exactly the kind of
 * plausible wrong answer this application must not set a receiver to. The
 * request waits for a fresh one.
 *
 * <p>PRIVACY. This class never passes a coordinate to {@link #nativeLog} and
 * never writes one anywhere. The only place latitude and longitude appear is
 * {@link #nativeFix}, which hands them to the object that is going to apply
 * them. {@code core/device_location.hpp} states the same rule for the native
 * half and {@code tests/test_device_location.cpp} searches the log for digits.
 */
public final class Loc {

    /** Matches {@code DeviceLocation::Provider} in core/device_location.hpp. */
    private static final int PROVIDER_UNKNOWN = 0;
    private static final int PROVIDER_SATELLITES = 1;
    private static final int PROVIDER_NETWORK = 2;

    /** The request code for our one permission prompt. */
    public static final int PERMISSION_REQUEST = 4711;

    private static LocationManager manager;
    private static Activity activity;
    private static Handler handler;
    private static boolean listening;
    private static long timeoutMs;

    /** The listener is held so the same instance can be removed again. */
    private static final LocationListener listener = new LocationListener() {
        @Override
        public void onLocationChanged(Location location) {
            deliver(location);
        }

        // The three-argument overloads are abstract before API 30. Empty on
        // purpose: a provider being switched off mid-request is covered by the
        // timeout, and a status change is not a position.
        @Override
        public void onStatusChanged(String provider, int status, Bundle extras) {}

        @Override
        public void onProviderEnabled(String provider) {}

        @Override
        public void onProviderDisabled(String provider) {}
    };

    private static Object gnssCallback;  // GnssStatus.Callback, API 24+

    private Loc() {}

    /**
     * Asks the device for one position. Safe to call again while a request is
     * outstanding - the previous one is cancelled first. Called from native
     * code on the frame thread; every Android call it makes is posted to the
     * main looper, because LocationManager wants one.
     */
    public static void start(final Activity act, final long timeout) {
        activity = act;
        timeoutMs = timeout;
        if (handler == null) { handler = new Handler(Looper.getMainLooper()); }
        handler.post(new Runnable() {
            @Override
            public void run() {
                startOnMain();
            }
        });
    }

    /** Cancels an outstanding request. Safe when there is none. */
    public static void stop() {
        if (handler == null) { return; }
        handler.post(new Runnable() {
            @Override
            public void run() {
                stopOnMain();
            }
        });
    }

    /**
     * Called by {@code MainActivity.onRequestPermissionsResult}. A grant
     * starts the request that was waiting for it; a refusal is reported as
     * such, because a control that simply did nothing after the user tapped
     * "Deny" would look broken rather than refused.
     */
    public static void onPermissionResult(int requestCode, String[] permissions,
                                          int[] grantResults) {
        if (requestCode != PERMISSION_REQUEST) { return; }
        boolean granted = false;
        for (int i = 0; i < grantResults.length; ++i) {
            if (grantResults[i] == PackageManager.PERMISSION_GRANTED) { granted = true; }
        }
        if (granted) {
            nativeLog("permission granted; asking for a position");
            startOnMain();
            return;
        }
        nativeLog("permission refused");
        nativeDenied("permission to use this device's location was refused");
    }

    // --- the main-looper half --------------------------------------------

    private static void startOnMain() {
        stopOnMain();
        if (activity == null) {
            nativeFailed("the device's location service could not be reached");
            return;
        }
        if (manager == null) {
            manager = (LocationManager) activity.getSystemService(Context.LOCATION_SERVICE);
        }
        if (manager == null) {
            nativeFailed("this device has no location service");
            return;
        }

        // THE PERMISSION, ASKED FOR ONLY WHEN THE USER PRESSED THE KEY. FoxSDR
        // does not ask at startup: a radio application that wants your
        // location the moment it opens is one nobody should trust, and until
        // the key is pressed it genuinely does not need it.
        if (activity.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                != PackageManager.PERMISSION_GRANTED) {
            nativeLog("asking for the location permission");
            activity.requestPermissions(
                    new String[] {Manifest.permission.ACCESS_FINE_LOCATION,
                                  Manifest.permission.ACCESS_COARSE_LOCATION},
                    PERMISSION_REQUEST);
            return;  // onPermissionResult continues, or reports the refusal
        }

        int started = 0;
        try {
            if (manager.isProviderEnabled(LocationManager.GPS_PROVIDER)) {
                manager.requestLocationUpdates(LocationManager.GPS_PROVIDER, 0L, 0.0f, listener,
                                               Looper.getMainLooper());
                ++started;
            }
            if (manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER)) {
                manager.requestLocationUpdates(LocationManager.NETWORK_PROVIDER, 0L, 0.0f,
                                               listener, Looper.getMainLooper());
                ++started;
            }
        } catch (SecurityException e) {
            // The permission was revoked between the check and the call, or a
            // policy blocks it. Reported as a denial rather than a failure:
            // it is the same thing from the user's side.
            nativeDenied("permission to use this device's location was refused");
            return;
        } catch (IllegalArgumentException e) {
            nativeFailed("this device does not offer a location provider");
            return;
        }

        if (started == 0) {
            // Location is switched off for the whole device. Said as the thing
            // the user can act on.
            nativeFailed("location is switched off on this device - turn it on in Android's "
                         + "own Settings and press this again");
            return;
        }

        listening = true;
        nativeLog("listening on " + started + " provider(s)");
        startGnssStatus();

        // The native half owns the timeout (it has the frame clock), but a
        // listener left running if native code ever stopped asking would be a
        // chip left powered on. This is the belt: one post, cancelled by
        // stopOnMain, that ends the request whatever else happens.
        handler.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (listening) {
                    nativeLog("no position within the time allowed; the request was ended");
                    stopOnMain();
                }
            }
        }, timeoutMs > 0 ? timeoutMs + 2000L : 62000L);
    }

    private static void stopOnMain() {
        if (manager != null && listening) {
            try {
                manager.removeUpdates(listener);
            } catch (SecurityException e) {
                // Nothing to do and nothing to say: the request is over
                // either way.
            }
            stopGnssStatus();
        }
        listening = false;
    }

    private static void deliver(Location location) {
        if (location == null || !listening) { return; }
        final int provider =
                LocationManager.GPS_PROVIDER.equals(location.getProvider()) ? PROVIDER_SATELLITES
                : LocationManager.NETWORK_PROVIDER.equals(location.getProvider())
                        ? PROVIDER_NETWORK
                        : PROVIDER_UNKNOWN;
        final float accuracy = location.hasAccuracy() ? location.getAccuracy() : -1.0f;
        // Ends the request BEFORE handing the position over: native code will
        // ask for a stop anyway, and doing it here means a provider that
        // delivers twice in the same millisecond cannot produce two fixes.
        stopOnMain();
        nativeFix(location.getLatitude(), location.getLongitude(), accuracy, provider);
    }

    // --- the satellite count, which is only a status line ------------------

    private static void startGnssStatus() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.N || manager == null) { return; }
        try {
            GnssStatus.Callback cb = new GnssStatus.Callback() {
                @Override
                public void onSatelliteStatusChanged(GnssStatus status) {
                    int used = 0;
                    for (int i = 0; i < status.getSatelliteCount(); ++i) {
                        if (status.usedInFix(i)) { ++used; }
                    }
                    nativeSatellites(used);
                }
            };
            manager.registerGnssStatusCallback(cb, handler);
            gnssCallback = cb;
        } catch (SecurityException e) {
            gnssCallback = null;  // advisory only; the fix does not depend on it
        }
    }

    private static void stopGnssStatus() {
        if (gnssCallback == null || manager == null) { return; }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            manager.unregisterGnssStatusCallback((GnssStatus.Callback) gnssCallback);
        }
        gnssCallback = null;
    }

    // --- what native code binds (device_location_platform.cpp) -------------

    private static native void nativeLog(String message);

    private static native void nativeFix(double latDeg, double lonDeg, float accuracyM,
                                         int provider);

    private static native void nativeFailed(String reason);

    private static native void nativeDenied(String reason);

    private static native void nativeSatellites(int used);
}
