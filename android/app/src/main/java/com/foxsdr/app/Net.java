// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
package com.foxsdr.app;

import java.io.IOException;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.MalformedURLException;
import java.net.URL;
import java.util.concurrent.ConcurrentHashMap;

/**
 * The HTTPS transport for the native library, and the ONLY reason it is in
 * Java.
 *
 * <p>The NDK ships no OpenSSL. It is not part of the NDK and never has been,
 * which is why {@code net/web_auth.cpp}, {@code core/plugin_repo.cpp} and
 * cpp-httplib's SSL half are all stubbed out under {@code CASCADE_ANDROID} -
 * there is no TLS stack in native code on this platform. There IS one on the
 * platform itself: {@code javax.net.ssl} behind {@link HttpURLConnection},
 * with the device's own trust store, the operating system's certificate
 * updates and its pinning. Reaching it over JNI is a smaller, safer and far
 * more maintainable thing than shipping a second copy of OpenSSL in the APK.
 *
 * <p>WHAT THIS CLASS IS NOT. It is not an HTTP client for general use. It
 * posts one body to one URL and reads back a status code and one header; it
 * never reads a response body, and that is deliberate rather than lazy - there
 * is nothing the usage or crash endpoint could say that this application
 * should obey (no configuration, no commands, no identifiers), and not reading
 * the body is the simplest way to guarantee that stays true. The same sentence
 * appears against the WinHTTP and cpp-httplib transports in
 * {@code core/telemetry.cpp} and {@code core/crash_upload.cpp}.
 *
 * <p>THE SCHEME RULE IS ENFORCED TWICE, here and in
 * {@code core/net_post.cpp}'s {@code netPostAllowed()}. That is not an
 * oversight either: the native gate is the one the tests pin, and this one is
 * what makes the Java class harmless on its own terms - a future caller that
 * forgot the native gate still cannot use it to put an install id in clear on
 * somebody's network. https always; plain http only to a loopback address,
 * which never leaves the phone.
 *
 * <p>Certificate validation is left entirely at its defaults. No
 * {@code TrustManager}, no {@code HostnameVerifier}, no
 * {@code setSSLSocketFactory} - relaxing any of those is how an application
 * that looks encrypted stops being it, and the local-server testing this
 * transport needed is served by the loopback exception above instead.
 */
public final class Net {

    private Net() {}

    /**
     * Requests in flight, keyed by the token the native caller chose, so
     * {@link #cancel(int)} can reach one from another thread. A
     * {@link ConcurrentHashMap} because the two ends are always different
     * threads: the upload runs on a native worker and the cancel comes from
     * whichever thread is shutting the application down.
     */
    private static final ConcurrentHashMap<Integer, HttpURLConnection> IN_FLIGHT =
            new ConcurrentHashMap<>();

    /**
     * Posts one body and returns {@code {status, retryAfterSeconds}}.
     *
     * <p>Called from a native WORKER thread - never the render thread. See
     * {@code TelemetryReporter::send} and the crash sweep, both of which have
     * always run their transport on a thread of their own.
     *
     * @param url              the absolute URL to post to.
     * @param body             the request body, exactly the bytes the native
     *                         side built. Not re-encoded, not wrapped.
     * @param contentType      the {@code Content-Type} header value.
     * @param userAgent        the {@code User-Agent} header value.
     * @param connectTimeoutMs connect timeout, milliseconds.
     * @param readTimeoutMs    read timeout, milliseconds.
     * @param token            the key {@link #cancel(int)} will use.
     * @return {@code null} if the request was refused before any I/O (a URL
     *         that would not parse, a scheme this class will not send on) -
     *         the caller records that as "not attempted". Otherwise a
     *         two-element array: the HTTP status, or 0 if the request was sent
     *         and the connection then failed, and the {@code Retry-After}
     *         delta-seconds value, or 0 when the header is absent or is not a
     *         plain number.
     */
    public static int[] post(String url, byte[] body, String contentType, String userAgent,
                             int connectTimeoutMs, int readTimeoutMs, int token) {
        if (url == null || body == null) {
            return null;
        }
        final URL parsed;
        try {
            parsed = new URL(url);
        } catch (MalformedURLException e) {
            return null;
        }
        final String protocol = parsed.getProtocol();
        if (!"https".equals(protocol)) {
            // See the class comment: https always, plain http only to
            // loopback. A string comparison, NOT InetAddress.isLoopbackAddress
            // - resolving the name first would mean a DNS lookup decided
            // whether this application may send in clear.
            if (!"http".equals(protocol) || !isLoopbackHost(parsed.getHost())) {
                return null;
            }
        }

        HttpURLConnection connection = null;
        try {
            connection = (HttpURLConnection) parsed.openConnection();
            connection.setConnectTimeout(connectTimeoutMs);
            connection.setReadTimeout(readTimeoutMs);
            connection.setRequestMethod("POST");
            // NEVER follow a redirect, the same way the WinHTTP and
            // cpp-httplib transports never ask to. A server that wants to move
            // its endpoint gets a new compiled-in URL, not a client that will
            // follow it anywhere.
            connection.setInstanceFollowRedirects(false);
            connection.setUseCaches(false);
            connection.setDoInput(true);
            connection.setDoOutput(true);
            // Fixed length rather than chunked: the bodies here are small, and
            // a fixed Content-Length is what the endpoints already receive
            // from the other two transports.
            connection.setFixedLengthStreamingMode(body.length);
            if (contentType != null) {
                connection.setRequestProperty("Content-Type", contentType);
            }
            if (userAgent != null && !userAgent.isEmpty()) {
                connection.setRequestProperty("User-Agent", userAgent);
            }

            IN_FLIGHT.put(token, connection);

            OutputStream out = connection.getOutputStream();
            try {
                out.write(body);
                out.flush();
            } finally {
                out.close();
            }

            final int status = connection.getResponseCode();
            final int retryAfter = parseRetryAfterSeconds(connection.getHeaderField("Retry-After"));
            return new int[] {status, retryAfter};
        } catch (IOException e) {
            // The request WAS opened and is therefore an attempt, which is
            // what the crash uploader's retry bookkeeping counts. A cancel
            // lands here too: disconnect() from another thread turns the
            // blocked call into an IOException.
            //
            // LOGGED, because on a phone this is the only account anyone gets.
            // The native side can report that a post failed but not WHY - a
            // refused connection, a certificate the device does not trust and
            // a missing INTERNET permission are three completely different
            // problems that all arrive here as "status 0". The message names
            // the class and the endpoint and nothing else.
            android.util.Log.w("FoxSDR", "net: post to " + url + " failed: " + e);
            return new int[] {0, 0};
        } catch (RuntimeException e) {
            android.util.Log.w("FoxSDR", "net: post to " + url + " failed: " + e);
            return new int[] {0, 0};
        } finally {
            IN_FLIGHT.remove(token);
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    /**
     * Aborts the request posted under {@code token}, if one is still in
     * flight. Best effort: {@code disconnect()} closes the socket, so a call
     * blocked in {@link #post} fails immediately instead of sitting out its
     * read timeout. Doing nothing - because the request already finished, or
     * never started - is a normal outcome, not an error.
     */
    public static void cancel(int token) {
        final HttpURLConnection connection = IN_FLIGHT.remove(token);
        if (connection == null) {
            return;
        }
        try {
            connection.disconnect();
        } catch (RuntimeException e) {
            // Nothing useful to do: the caller is already tearing down.
        }
    }

    /**
     * The same three names {@code core/net_post.cpp}'s
     * {@code netPostLoopbackHost()} accepts, plus the bracketed form
     * {@link URL#getHost()} returns for a literal IPv6 address.
     */
    private static boolean isLoopbackHost(String host) {
        return "127.0.0.1".equals(host) || "localhost".equals(host) || "::1".equals(host)
                || "[::1]".equals(host);
    }

    /**
     * The delta-seconds form only, the same restriction both desktop
     * transports apply: an HTTP-date would need a parser and a clock
     * comparison for a header the endpoint controls, and an unparsed value
     * falling back to the default backoff is the safe direction.
     */
    private static int parseRetryAfterSeconds(String value) {
        if (value == null || value.isEmpty()) {
            return 0;
        }
        for (int i = 0; i < value.length(); ++i) {
            final char c = value.charAt(i);
            if (c < '0' || c > '9') {
                return 0;
            }
        }
        try {
            final long seconds = Long.parseLong(value);
            return (seconds > Integer.MAX_VALUE) ? Integer.MAX_VALUE : (int) seconds;
        } catch (NumberFormatException e) {
            return 0;
        }
    }
}
