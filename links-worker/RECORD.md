# Record: links-worker (lane `links-worker`, branch `feat/links-worker` from e7e25fc9)

Everything is in `links-worker/`. Nothing outside it was touched; `src/`, `telemetry-worker/`, CMake and every shared file are unchanged. **Not deployed, no Cloudflare resource created, nobody logged in, no `npm install`, no request to any outside address** (tests use scripted fetches and a server bound to 127.0.0.1).

## Built

- Registry `registry/libraries.json` (15 rows), `registry/schema.json`, strict validator `src/validate.js`, `tools/validate.mjs`.
- Worker `src/worker.js`: `/v1/libraries.json`, `/v1/libraries.signed.json`, `/v1/status.json`, `/v1/go/<id>`, `/health`, `/admin/links.json`, and the daily `scheduled` handler (cron `17 4 * * *`).
- Checker modules (`probe.js`, `decide.js`, `check-run.js`): manual redirect following with the host test BEFORE the request; HEAD then one ranged GET; 8 s timeout; `User-Agent: FoxSDR-link-check`; each address once per run (shared between rows); subrequest budget (default 45 + 2 KV calls, under the free plan's 50); rows oldest-checked first and never half-started; three failed runs in a row = `broken`; foreign host = `moved` (address never edited); `changed` flag for a pinned-checksum download; last 30 results per id; a new address starts clean (the overlay carries the address it was observed at).
- Storage: ONE KV key `state`. Why KV: one read + one write per run keeps the run inside the 50-subrequest free-plan limit (KV calls count as subrequests), the value is far under the 25 MiB limit, only the cron writes it (no write races; KV allows 1 write per second per key), and eventual consistency only means a request may see yesterday's status. D1 would add a schema and migrations for queries nobody makes; R2 and Durable Objects fit other shapes. (Limits read from developers.cloudflare.com on 2026-11-12.)
- Admin protection: bearer secret `ADMIN_TOKEN` (a Worker secret, never in a file), compared as SHA-256 digests with a constant-time loop, fails closed (503 when unset or shorter than 16 characters), 401 body says nothing. **The telemetry Worker has no protected read endpoint to copy** (it is POST-only; its data is read through Cloudflare's SQL API with a token the reader script holds), so this is a new, small mechanism, not "the same way". The site's existing admin pages may already have a convention for calling a source with a secret; SITE-HANDOFF.md says to match it.
- Tools: `build-bundle.mjs` (registry text → `src/registry-bundle.js`, so the registry is served byte for byte; verifies an envelope's payload equals the registry), `predeploy.mjs` (refuses deploy on an invalid registry, stale bundle, placeholder KV id, no route), `envelope.mjs` + `sign-registry.mjs` (the signature envelope, dormant signer, never run).
- Documents: README, DEPLOY, UPDATING-A-LINK, ADDING-A-LIBRARY, SITE-HANDOFF, CLIENT-CONTRACT, this record.

## Registry rows seeded (all from this tree; nothing invented)

Addresses in code or documents: `zadig` (src/core/radio_setup.cpp `kZadigUrl`; this is the "RTL-SDR driver" row: the program's own one-click RTL-SDR install downloads nothing, it binds the in-box WinUSB driver, `src/core/winusb_install.hpp`), `sdrplay-api`, `uhd`, `airspy-drivers`, `lime-suite`, `pothossdr` (the other five `k*Url` constants of `radio_setup.cpp`; its comment records HTTP 200 on 2026-10-06, kept as `last_checked`), `libfobos`, `librfnm`, `winradio-g39ddc`, `winradio-g33ddc` (pages read in docs/feature-records/oss-libs.md). `status: unverified` with NO address: `mbelib`, `codec2`, `ftdi-d2xx`, `vc-runtime-x64`, `ffmpeg` (ids named in NEEDED-LIBRARY-RULE.md). **No row has `fetch_allowed: yes`**; no row has a download address.

## Tests (real output, run five times)

`node --test "test/*.test.mjs"` (Node v24.20.0): five runs, each `ℹ tests 79 ℹ pass 79 ℹ fail 0`.

| File | Covers |
|---|---|
| validate.test.mjs | shipped registry valid and invented-nothing; malformed JSON/oversize; every field, enum, shape; address rules (http, user-info, port, fragment, case, spaces, host not allowed); download object; `unverified` = no address; `yes` rules |
| probe.test.mjs | good path; HEAD refused then ranged GET (4 refusal codes); redirect to allowed host (absolute and relative); redirect to foreign host never requested; look-alike hosts; http downgrade; loop limit; no Location; timeout; network error, 404/410/500/502, 429; host not allowed; budget; garbage address |
| decide.test.mjs | three strikes; consecutive only; recovery; moved persists and is not a strike; foreign host on the download; skipped and 429; precedence; history of 30; `changed` with and without a pin; effective status; a new address starts clean; stale checker |
| check-run.test.mjs | good run; shared request; strikes across runs via state; moved leaves the registry untouched; budget (not half-started, oldest first next day); removed rows forgotten; timeout |
| worker.test.mjs | every route; byte-for-byte body, ETag/304; no cookie, no query read; 302; "being updated" page escaped; KV failure; unknown ids and 14 odd paths 404; method 405; status.json content; admin refusals (no header, wrong, Basic, query token, extra char, other header) and 503 unconfigured; admin content; stale; scheduled run (1 read, 1 write, each address once); KV error stops the run; invalid registry refuses to start; real bundle; source scan (no console, no connection data, no cookies, no query reading) |
| envelope-bundle.test.mjs | bundle current; radio_setup.cpp constants agree with the registry; sign/verify; tamper, wrong key, no key, key list; malformed envelopes; manifest-prefix signature refused; build refuses stale/invalid; dormant signer refusals; no key material in the folder |
| loopback.test.mjs | real fetch against 127.0.0.1: HEAD, ranged GET fallback, redirects, a foreign name (`localhost`) never requested, 404, timeout |
| schema-sync.test.mjs | schema.json and validate.js agree |

### Break-it checks

| Behaviour removed | Test | Result |
|---|---|---|
| three-strikes limit (`>= FAIL_LIMIT` made `>= 1000`) | decide.test.mjs "BREAK-IT three strikes" | red: `three failed runs in a row are broken`; restored green |
| limit made too eager (`FAIL_LIMIT = 2`) | same file | red: `two failed runs are not broken` |
| moved-host rule (the allowed-host test replaced by `false`) | decide.test.mjs "BREAK-IT moved host" | red: the foreign host IS requested (2 calls, not 1) |

The mutants are made by copying `src/` to a scratch folder, applying one text replacement (the test fails if the text is absent) and importing the copy; the real source is never edited.

## NOT RUN / UNVERIFIED

- **Wrangler**: not installed here and not downloaded. `wrangler.toml` (syntax, the KV binding, the cron, `workers_dev`, `observability`) and the esbuild bundling of `src/worker.js` with the generated `registry-bundle.js` have never been through `wrangler deploy --dry-run`. First thing to run at deploy time.
- **Handler tests inside the Workers runtime** (`@cloudflare/vitest-pool-workers`, `wrangler dev --test-scheduled`): not installed, not run. The telemetry Worker's tests are plain `node --test` like these, so nothing equivalent was skipped; the real Workers `fetch`, `AbortSignal.timeout`, KV and `crypto.subtle` were exercised only through Node 24's.
- Whether Cloudflare's runtime lets a Worker set the `User-Agent` header on `fetch` was not verified.
- The real addresses were never requested (by rule), so which of them answer, redirect, or refuse `HEAD` is unknown. The first daily run will show it; GitHub and vendor sites often redirect to a file-host name that then has to be added to `allowed_hosts` (UPDATING-A-LINK.md step 3).
- The program's side is a document only; no C++ was written or compiled.
- `compatibility_date = "2026-08-18"` was copied from the telemetry Worker, not looked up.
- Admin `Authorization` handling was tested through `Request` objects, not over a real connection.
- An incidental slip: `certutil -v` was run once (output discarded) while checking file hashes; it printed usage and raised nothing, but it is on the forbidden list and should not have been run.

## Findings for the orchestrator

1. `updater.cpp` at e7e25fc9 has **no signature check**; the detached-signature scheme in the tree is `sys_manifest_sig` (Ed25519, owner key constant still empty, nothing calls it). CLIENT-CONTRACT.md specifies the registry envelope on that scheme with its own format word and domain prefix, and requires the program to **accept no refreshed registry until a trusted key is compiled in**.
2. The telemetry Worker has no protected read endpoint (above).
3. The program's own one-click RTL-SDR driver install downloads nothing; the "RTL-SDR driver" registry row is the Zadig by-hand page.
4. `foxsdr_manifest_sign` (tools/manifest_sign.cpp) hard-codes the manifest prefix; signing the registry with it needs a small option (another lane's file) or the Node signer in this folder.

## Owner decisions

Hostname (proposed `links.foxsdr.com`); who holds the Ed25519 seed and whether it is the same key as the update manifest's; Cloudflare plan (free = 50 subrequests, KV counts); who reads the admin list and whether alerts beyond a pull page are wanted. Details in DEPLOY.md.

## Sources opened

Cloudflare documentation pages read on 2026-11-12: Workers platform limits, KV limits, Cron Triggers (developers.cloudflare.com). In the tree (read only): telemetry-worker/ (README, wrangler.toml, worker.js, worker.test.mjs), src/core/radio_setup.cpp, src/core/winusb_install.hpp, src/core/updater.{hpp,cpp}, src/core/sys_manifest_sig.hpp, docs/feature-records/oss-libs.md, oss-radios.md, sysmodels.md, D:/foxsdr-build/prep/queued-briefs/{STANDARD-RULES,NEEDED-LIBRARY-RULE}.md. `%APPDATA%/foxsdr/config.json` sha256 at the end: `c21ce98dc30e50327f7666f7ee29047c0983d130ca3f21716d0c0e20e04603c3` (unchanged).
