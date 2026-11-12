# feat/webmodels: hand-off for the web page and the web server

Everything in `src/net/web_*.cpp` and `src/net/upnp_igd.cpp` added by this lane is a **standalone model**. Nothing calls them yet. The web page's HTML and
JavaScript, and every new route, live in `src/net/web_server.cpp`, which this lane was forbidden to edit (the `unblocked-web` lane, branch
`feat/web-secure`, is in it). This file is the exact text to paste when that lane has merged. Page code references below were read in
`src/net/web_server.cpp` at the base commit: `drawSpectrum` (line ~2686), `drawWaterfall` (~2714), `pollSpectrumOnce` (~2735), the spectrum click handler
(~2753), `$('playstop')` (~2506), the audio scheduler (`audioCtx = new AudioContext`, ~2472), the cookie (`SameSite=Strict`, line ~74).

Viewer tokens in every model are the session cookie value (or any 8-128 character `[A-Za-z0-9_-]` string): memory only, never in a log, a setting or
a report. The server must pass the token to a model and must not print it, store it, or put it in a URL.

---

## 1. Waterfall zoom and pan (model: `net/web_viewport.hpp`)

No server change is needed: the page already receives the whole row (`bins`, `spanHz`, `centerHz`) and can show any window of it. Port these functions
to the page's JavaScript (same rules, same names; the C++ file is the reference and its tests are the spec).

```js
// ---- waterfall view: a window [first, first+count) of the bins row ------------------------------------------------------------
const MIN_VISIBLE_BINS = 16, WHEEL_PER_NOTCH = 1.25, MAX_ZOOM_STEP = 4;
let wfView = null;                                   // null = the whole row
function clampView(v, total) {
  if (total <= 0) return { first: 0, count: 0 };
  if (!v || !isFinite(v.first) || !isFinite(v.count)) return { first: 0, count: total };
  const count = Math.min(Math.max(v.count, Math.min(MIN_VISIBLE_BINS, total)), total);
  return { first: Math.min(Math.max(v.first, 0), total - count), count };
}
function zoomAt(v, total, anchor, factor) {         // factor > 1 zooms in; keeps the bin under the pointer fixed
  v = clampView(v, total);
  if (total <= 0 || !isFinite(factor) || factor <= 0) return v;
  anchor = isFinite(anchor) ? Math.min(Math.max(anchor, 0), 1) : 0.5;
  const under = v.first + anchor * v.count, count = v.count / factor;
  return clampView({ first: under - anchor * count, count }, total);
}
function wheelFactor(deltaY) {
  if (!isFinite(deltaY) || deltaY === 0) return 1;
  const e = Math.min(Math.max(-deltaY / 100, -50), 50);
  return Math.min(Math.max(Math.pow(WHEEL_PER_NOTCH, e), 1 / MAX_ZOOM_STEP), MAX_ZOOM_STEP);
}
function panByPixels(v, total, dx, width) {         // drag right: the picture follows, lower bins appear
  v = clampView(v, total);
  if (total <= 0 || !isFinite(dx) || !isFinite(width) || width <= 0) return v;
  return clampView({ first: v.first - dx / width * v.count, count: v.count }, total);
}
function pointerToOffsetHz(v, total, spanHz, x, width) {   // offset from the CENTRE, what vfoOffsetHz means
  v = clampView(v, total);
  x = Math.min(Math.max(isFinite(x) ? x : 0, 0), width);
  return ((v.first + x / width * v.count) / total - 0.5) * spanHz;
}
```

Page hooks:
* `drawSpectrum` and `drawWaterfall`: replace `i / (n - 1)` and `Math.floor((x * bins.length) / w)` by indexes inside `v = clampView(wfView, n)`:
  trace `i` from `Math.floor(v.first)` to `Math.ceil(v.first + v.count) - 1`, x = `(i - v.first) / v.count * w`; waterfall pixel `x` reads bin
  `Math.floor(v.first + x / w * v.count)`.
* The VFO marker in `drawSpectrum` uses `x = ((lastVfoHz / lastSpanHz) + 0.5) * w`; with a view it is `((lastVfoHz / lastSpanHz + 0.5) * n - v.first) / v.count * w`, drawn only when inside `[0, w]`.
* The spectrum click handler (`$('spectrum').addEventListener('click'` ...) becomes `control({ vfoOffsetHz: Math.round(pointerToOffsetHz(wfView, n, lastSpanHz, x, w)) })` with `x` in canvas pixels.
* Events on `#spectrum` and `#waterfall`: `wheel` (preventDefault, `wfView = zoomAt(wfView, n, anchor, wheelFactor(e.deltaY))`), pointer drag
  (`panByPixels`, a drag longer than 4 px must not also fire the click), `dblclick` resets `wfView = null`, keys `+`, `-`, arrow left/right (8 bins) when the canvas has focus (`tabindex="0"`), two-finger pinch (zoom by the ratio of the two touch distances, anchor at their midpoint).
* A view change clears the waterfall (the existing resize does the same: `fitCanvas($('waterfall'))`) because the rows already drawn belong to the old view.
* Add a small read-out ("x4 - 600 kHz") and a "Fit" button that sets `wfView = null`.

Strings (English; not translated here): `Fit`, `Zoom in`, `Zoom out`.

## 2. Browser bookmarks (model: `net/web_bookmarks.hpp`, `WebBookmarkBook`)

The server owns one `WebBookmarkBook` (a member of the web server object) and calls it with the session token. New routes, all behind the existing
`authorised(req)` gate and the same-origin cookie; a LISTENER role may use them (they never touch the receiver):

| Route | Body / query | Calls | Reply |
|---|---|---|---|
| `GET /api/mybookmarks?text=&group=&mode=&tag=&min=&max=` | query | `search(token, q)` (`list` when all empty) | `{"bookmarks":[{"id","name","freqHz","mode","bandwidthHz","group","tags"}],"groups":[..]}` |
| `POST /api/mybookmarks` | `{"name","freqHz","mode","bandwidthHz","group","tags"}` (the page fills freq/mode/bandwidth from the status) | `add` | `{"ok":true,"id":N}` or `{"ok":false,"error":bookmarkResultText(r)}` |
| `PUT /api/mybookmarks/<id>` | same body | `update` | same |
| `DELETE /api/mybookmarks/<id>` | | `remove` | same |
| `GET /api/mybookmarks/export` | | `exportJson` (served as a download, `Content-Disposition: attachment; filename="foxsdr-bookmarks.json"`) | the JSON |
| `POST /api/mybookmarks/import?replace=0|1` | the JSON file (the server's body limit applies; the model refuses over 256 KB) | `importJson` | `{"imported":N,"skipped":N}` |

`forgetViewer(token)` on sign-out and on session expiry. To tune to a bookmark the page sends the existing control request
(`vfoOffsetHz` or the receiver's tune field, and `mode`), exactly as the host bookmark rows do (`control({ bookmarkTune: i })` is for the HOST list).
The page keeps a copy of the viewer's set in `localStorage` (try/catch around every access) so it survives a server restart: on load, if the server set is
empty and the local copy is not, offer "Restore my bookmarks" which POSTs the copy to `/import`.

Page controls (English): `My bookmarks`, `Save here`, `Name`, `Group`, `Tags (comma separated)`, `Search`, `All groups`, `All modes`, `Export`, `Import`, `Replace my list`, `Restore my bookmarks`.

## 3. Recording in the browser and downloading a clip (model: `net/web_clip.hpp`, `WebClipBroker`)

`WebClipBroker` is constructed with the recordings root (the core recorder's recordings folder, absolute). The recording itself is the core clip
recorder, which this lane may not touch: the server needs ONE call into it. Proposed contract (a host call on the engine side, in the table the web
session already uses): `start_clip(radio, path, seconds, &sampleRate, &bytesPerSample, &channels)` records the demodulated audio of the receiver the page
controls to `path` (a WAV) for `seconds`, then reports completion `(bytes, ok)`. The web server's code is:

```
ClipPlan plan;
r = clips.request(token, {seconds, name, rate, width, channels}, plan);   // refusal text: clipResultText(r)
if (r == Ok) engine.start_clip(plan.path, plan.seconds, onDone = [&](bytes, ok){ clips.finished(token, plan.clipId, bytes, ok); });
...
GET /api/clip/<id>: r = clips.resolveDownload(token, id, path)  ->  stream `path` with Content-Type audio/wav and
                    Content-Disposition: attachment; filename="<fileName from list()>"
GET /api/clips: clips.list(token)  ->  [{"id","fileName","finished","ok","bytes"}]
POST /api/clip {"seconds":N,"name":"..."}   DELETE of the files is NOT offered (they are the user's recordings).
```
On sign-out or session expiry: `clips.abandon(token)` (and tell the recorder to stop), then `clips.forgetViewer(token)`.
Page controls: `Record clip`, `Length (seconds, 1-300)`, `Name`, `Recording...`, `Download` (per finished clip). A LISTENER may record (it records what they hear)
only if the owner's switch says so: that is a policy for the web-secure lane's roles, not this model's.

## 4. Scroll-wheel bandpass and a tuning-step control

No model was needed (it is page arithmetic over existing control fields). Exact behaviour proposed for the page:
* Wheel over the spectrum with **Shift** held changes the receive bandwidth (`bandwidthHz` in the control request) by 5 % per notch (min 50 Hz, max the mode's maximum), without Shift it zooms (section 1).
* A step selector next to the frequency read-out: `1 Hz`, `10 Hz`, `100 Hz`, `500 Hz`, `1 kHz`, `5 kHz`, `6.25 kHz`, `9 kHz`, `10 kHz`, `12.5 kHz`, `25 kHz`, `100 kHz`; default `1 kHz`; stored in `localStorage`; the existing per-digit wheel tuning (the `d.addEventListener('wheel'` code at ~1434) is unchanged, the step applies to arrow keys and Page Up/Down on the frequency read-out.
Strings: `Step`, `Bandwidth`.
UNVERIFIED: the existing per-digit wheel handler was not run in a browser by this lane.

## 5. Named profiles (model: `net/web_profiles.hpp`, `WebProfileBook`)

The host knows the profile names (the settings profiles the window already offers; the engine side must publish the list and the active one in the
status, e.g. `profiles: ["HF night","Airband"]`, `profile: "Airband"`: a status field is an engine/status change, not here). The server:
```
book.setNames(statusNames, statusActive);                        // on every status, cheap
GET  /api/profiles               ->  book.list()                  [{"name","active"}]
POST /api/profile {"name":"X"}   ->  r = book.requestSwitch(name, nowMs, canonical);
                                     Ok: send the engine's switch-profile op with `canonical` (NEVER the viewer's text), on its result call
                                         book.completeSwitch(ok, nowMs);   refusal text: profileResultText(r)
```
A switch should also pass the arbiter (section 6): only a viewer who may act now may switch profiles. Page: a `Profile` drop-down with the names, `Switch`.

## 6. Control arbitration (model: `net/web_arbiter.hpp`, `ControlArbiter`)

Roles are NOT here: `feat/web-secure`'s `WebRoom` decides owner/listener. The server creates one `ControlArbiter` and, per signed-in session:
```
arb.join(identity.role == Owner, identity.role == Owner || listenersMayTune, nowMs, viewerId)   // viewerId is stored with the SESSION, never sent to other viewers
arb.heartbeat(viewerId, nowMs)         // on every authorised /api/status
arb.leave(viewerId, nowMs)             // sign-out, expiry, kick, ban (WebRoom's end-of-session hook)
arb.setRights(viewerId, isOwner, mayTune, nowMs)   // when listenersMayTune is switched
before EVERY control request that tunes or changes settings:  d = arb.mayAct(viewerId, nowMs);  if (!d.allowed) refuse with arbResultText(d.why)
```
Routes (owner-only unless noted): `GET /api/control/viewers` -> `arb.viewers(now)` + `arb.mode(now)` + `arb.holder(now)` (ids and flags only; the page marks the caller's own id);
`POST /api/control/mode {"exclusive":bool}` -> `setMode`; `POST /api/control/grant {"id"}` -> `grant`; `POST /api/control/revoke` -> `revoke`.
Any viewer who may tune: `POST /api/control/request` -> `requestControl`, `POST /api/control/release` -> `releaseControl`, `POST /api/control/cancel` -> `cancelRequest`.
The page polls `arb.epoch()` (add `"controlEpoch"` to the status) and refetches the viewer list only when it changes.
Page text: `Exclusive control`, `Request control`, `Release control`, `Hand over to...`, `Take control back`, `You have control`, `Waiting (n)`, `Someone else has control`, `Connected: n`.
Admin override is by design: the owner is never locked out (`viaOverride`); the record lists it as a decision.

## 7a. CORS allow-list (model: `net/web_cors.hpp`, `CorsPolicy`)

The server configures one `CorsPolicy` from settings (`webCorsOrigins`: a list of origins, empty = off; `webCorsAllowCredentials`: bool, default false) and
installs a pre-routing handler:
```
CorsRequest r; r.method = req.method;
for each Origin header: r.originValues.push_back(value);           // ALL of them (the model denies two)
r.hasRequestMethod = req.has_header("Access-Control-Request-Method"); ... requestHeaders likewise
CorsDecision d = policy.evaluate(r);
for (h : d.headers) res.set_header(h.first, h.second);
if (d.verdict == Preflight) { res.status = 204; return Handled; }
```
A denied request is NOT stopped by CORS (CORS is not authentication; the sign-in gate does that). The session cookie is `SameSite=Strict`
(web_server.cpp:74), so a cross-origin page's credentialed request would not carry it: allowing credentials needs that cookie changed to `SameSite=None; Secure`
(and therefore HTTPS: the web-secure lane's TLS), which is the owner's decision. Without credentials a cross-origin page reads only what an unauthenticated request may (the sign-in
page and the 401). Settings text: `Allowed web origins (one per line)`, `Allow cross-origin sign-in (needs HTTPS)`.

## 7b. UPnP port mapping (model: `net/upnp_igd.hpp`, `UpnpMapper`) - OWNER DECISION, NOT WIRED

OFF by default and not called from anywhere. If the owner decides to wire it: settings `Open this port on my router automatically (UPnP)` (bool, default off,
only enabled when the web page has a password AND HTTPS is on), the port is the web server's configured port (the only entry in `allowedPorts`), `internalClient` is this
machine's LAN address (from the OS, an IPv4 in 10/8, 172.16/12 or 192.168/16, else the switch is refused), `discover()` then `addMapping(port)` on a worker thread when the server starts; `~UpnpMapper` /
`removeAll()` on server stop AND on application exit (add it to the shutdown-budget list with a bound of `timeoutMs` per mapping); show the router's external address from `getExternalAddress()` and a plain warning
"This makes the receiver reachable from the internet". It has never been run against a real router.

---

## 8. Lock-screen media controls and a screen wake-lock (page only, text for the page)

FACTS read 2026-11-11 (MDN, Screen Wake Lock API page): the Wake Lock API exists **only in secure contexts (HTTPS)**; the lock is released when the page is hidden and must be
requested again on `visibilitychange`; a `WakeLockSentinel` is single-use and fires `release`; feature detection is `"wakeLock" in navigator`; a request can be refused for system reasons
(power saving, low battery). MDN's Media Session API page gives `navigator.mediaSession.metadata = new MediaMetadata({title, artist, album, artwork})`, `setActionHandler(action, handler)` (unsupported
actions can throw: wrap in try/catch) and `playbackState`. **Consequence for this product: a page served over plain HTTP on the LAN is not a secure context, so the wake lock will be unavailable there; it
needs the HTTPS work (web-secure lane). `localhost` is a secure context in browsers, so the same machine works.** UNVERIFIED (MDN did not say): whether a lock-screen control appears only while an
`HTMLMediaElement` is playing. The page plays through a Web Audio scheduler (an `AudioContext`, not an `<audio>` element, on purpose: see the comment at ~2424), so a lock-screen control may need an `HTMLMediaElement` fed from a `MediaStreamAudioDestinationNode` (a design to try on a phone, not
verified here). The code below sets the session either way and degrades silently.

```js
// ---- Media Session (lock-screen / notification controls) -------------------------------------------------------------------
function setupMediaSession() {
  if (!('mediaSession' in navigator)) return;
  try {
    navigator.mediaSession.metadata = new MediaMetadata({
      title: 'FoxSDR', artist: 'Receiver', album: 'Live',          // the frequency/mode line is written by updateMediaSession()
      artwork: [{ src: '/icon-192.png', sizes: '192x192', type: 'image/png' }]   // only if the page serves that file
    });
  } catch (e) { return; }
  const act = (name, fn) => { try { navigator.mediaSession.setActionHandler(name, fn); } catch (e) { /* unsupported here */ } };
  act('play',  () => { if (!lastRunning) $('playstop').click(); });   // lastRunning: the page's last status `running`
  act('pause', () => { if (lastRunning)  $('playstop').click(); });
  act('stop',  () => { if (lastRunning)  $('playstop').click(); });
}
function updateMediaSession(s) {                                       // call from showStatus(s)
  if (!('mediaSession' in navigator) || !navigator.mediaSession.metadata) return;
  const mhz = (s.freqHz / 1e6).toFixed(4);
  navigator.mediaSession.metadata.title = mhz + ' MHz ' + (s.mode || '');
  navigator.mediaSession.playbackState = s.running ? 'playing' : 'paused';
}

// ---- Screen Wake Lock (HTTPS or localhost only) ----------------------------------------------------------------------------
let wakeLock = null, wakeWanted = false;
async function acquireWakeLock() {
  if (!('wakeLock' in navigator) || document.visibilityState !== 'visible') return false;
  try {
    wakeLock = await navigator.wakeLock.request('screen');
    wakeLock.addEventListener('release', () => { wakeLock = null; refreshWakeButton(); });
    return true;
  } catch (e) { wakeLock = null; return false; }                       // power saving, battery, permissions policy
}
async function setWakeWanted(on) {
  wakeWanted = on;
  try { localStorage.setItem('wakeWanted', on ? '1' : '0'); } catch (e) { /* private window */ }
  if (on) await acquireWakeLock(); else if (wakeLock) { try { await wakeLock.release(); } catch (e) {} wakeLock = null; }
  refreshWakeButton();
}
document.addEventListener('visibilitychange', () => { if (wakeWanted && document.visibilityState === 'visible' && !wakeLock) acquireWakeLock(); });
function refreshWakeButton() {                                         // a toggle `Keep screen on`; hidden when unsupported
  const b = $('wakebtn'); if (!b) return;
  b.hidden = !('wakeLock' in navigator);
  b.setAttribute('aria-pressed', wakeLock ? 'true' : 'false');
  b.textContent = wakeLock ? 'Screen stays on' : 'Keep screen on';
}
```
Call `setupMediaSession()` from the existing first-user-gesture path (the code that creates the `AudioContext`, ~2470), `updateMediaSession(s)` from `showStatus`, and restore `wakeWanted` from
`localStorage` on load (a lock can only be requested while the page is visible; browsers may require a user gesture, so a failed restore leaves the button off).
Strings: `Keep screen on`, `Screen stays on`.

The "phone manifest is already planned" (REMAINING-GAPS.md): `display: "standalone"` in the manifest does not by itself keep the screen on; the wake lock above does that.

---

## What was NOT built, with the reason

* **Roll back a plugin version**: "missing by choice" at `src/core/plugin_repo.hpp:806`. The recorded reason, in that block: "NEVER a downgrade ... A catalogue that has gone backwards is either a mistake or an attack, and neither is a reason to replace a working newer plugin with an older one" (lines 806-812), and the VERSION POLICY block at lines 78-112 of the same file: updates are never automatic because "native code arriving from the network with no human in the loop" is the supply-chain shape the product refuses, and a per-plugin `minSupportedVersion` floor (`PluginCatalogEntry`, `plugin_repo_data.hpp:92`) RETIRES old installed versions so they are no longer loaded. A rollback button would reinstall exactly the older native code those two rules exist to keep out, and any version below a floor would be blocked again at once. The reason is security, so it is not built. (If the owner ever wants it, the safe form is "keep the previous version's file locally and re-enable it", never a download of an older one: a different feature, not scoped here.)
* **Plugin ratings** (collection): needs a server that stores votes and an identity to stop repeat voting (an account system and a Worker). The sort model is built and waits for the data (`StoreStats`).
