# The program's side: what FoxSDR must do with the link registry

This is a **document only**. Another lane writes the code. The reference for "valid" is `src/validate.js` and its tests in this folder; the C++ validator must refuse exactly what that refuses (the rules are listed in `registry/schema.json` under `x-rules` and enforced in `src/validate.js`).

## 0. A finding the brief did not expect

The brief said to copy the update-manifest signature check from `updater.cpp` at e7e25fc9. **`updater.cpp` has no signature check.** It verifies a host allow-list (`foxsdr.com`), an https URL, a well-formed version and the sha256 of the download (`src/core/updater.cpp`, `urlIsOnAllowedHost`, `fetchVerifiedFile`). The only detached-signature scheme in the tree is `src/core/sys_manifest_sig.{hpp,cpp}` and `sys_ed25519.*` (Ed25519, from the sysmodels lane): a JSON envelope, a trusted-key list, and an owner key constant `kOwnerKeyHex` that is **empty**, so `verifyEnvelope` answers `NoTrustedKey`. Nothing calls it yet. The scheme below is that scheme, with its own format word and its own domain-separation prefix.

## 1. The built-in copy

- The program ships a **built-in copy** of `registry/libraries.json`, generated at build time from the file (never hand-copied), so the Needed-library panels and Radio setup work offline and on first run with no network at all.
- Every lookup is **by library id**. No plugin or bridge carries a web address of its own as the only copy (NEEDED-LIBRARY-RULE.md).
- Installed copies remember the newest valid registry they have accepted (in the profile folder, as the signed envelope text) and **re-validate it every time they load it**.

## 2. Refreshing

- At most **once a day**, and **only if the user has not turned network features off** (the existing setting that governs the program's other network features). Never at the moment a panel is drawn; on a worker thread; with a timeout; failure is silent and keeps what is held.
- Request: `GET https://<links hostname>/v1/libraries.signed.json`, conditional on the held `ETag` (`If-None-Match`). **No install id, no cookie, no query string, no custom header** beyond the User-Agent the update check already sends. This request is not part of the usage report and adds nothing to it (a new telemetry word needs the telemetry Worker deployed first; none is added here).
- The hostname is compiled in; it is an owner decision (DEPLOY.md).
- Bounds: https only; the answer may not redirect to another host; at most 400 KB for the envelope and 256 KB for the registry inside it.
- `304` or any error: keep what is held.

## 3. Accepting a refreshed registry (all of these, in this order; any failure = keep what is held)

1. **Envelope.** JSON object: `{"format":"foxsdr-link-registry-sig","version":1,"alg":"ed25519","keyId":"<16 hex>","payload":"<base64 of the registry bytes>","sig":"<base64, 64 bytes>"}`. Unknown format, version or algorithm: refuse.
2. **Key.** `keyId` is the first 8 bytes of SHA-256 of the 32-byte public key, as 16 hex characters. It must name one of the program's **compiled-in trusted keys** (a list, so a key can be rotated). **With no trusted key compiled in, the program accepts no refreshed registry at all**: it stays on its built-in copy. The Worker is not trusted to steer where users download from; the signature is what is trusted.
3. **Signature.** Ed25519 over `"FoxSDR link registry v1\n"` followed by the payload bytes (the prefix keeps a signature made for the update manifest, `"FoxSDR update manifest v1\n"`, from being replayed here). Verify with `sys::verifyEnvelope`'s Ed25519 code; the C++ side needs a variant of `makeEnvelope`/`verifyEnvelope` that takes the format word and prefix as parameters (a small change to `sys_manifest_sig`, another lane's file) or a copy of the 40 lines that differ.
4. **Validate** the payload with the registry rules (schema version 1, strict fields, https, allowed-host rules, `unverified` means no address, `yes` needs its evidence). One invalid row refuses the whole registry: never accept it row by row.
5. **Not older.** `registry_version` must be **greater than or equal to** the built-in copy's AND the newest one accepted before; store it only when strictly greater than the held one. (A registry older than the program's own is a rollback and is refused.)
6. Store the envelope text; switch the in-memory registry atomically.

## 4. Where the signature comes from

Made at deploy time by the owner's signing step (`tools/sign-registry.mjs`, dormant, never run in this repository), from an Ed25519 seed the owner keeps **offline**. No private key is in this repository, in Worker code, in the build, in CI or in the program. Who holds the seed, and whether it is the same key as the update manifest's, are **owner decisions** (DEPLOY.md). The Worker does not sign and does not verify: it serves the envelope file.

## 5. What the user sees for a library (effective status)

`effective = the worse of (registry row status, status.json status for that id)` with the order `ok` < `unverified` < `moved` < `broken`. `status.json` is **advisory and unsigned**: it may only make a library *less* offered, never more, so a forged one can hide a button but cannot create one. Fetch it with the same daily refresh (`GET /v1/status.json`); if it is missing or stale (`"stale": true`) use the registry status alone.

| Effective status | The panel |
|---|---|
| `ok` | Items 1 to 6 of the Needed-library rule, with the official page as text and the "Open the download page" button (opens the user's browser, on the user's click only). |
| `unverified` | The written steps only. **No address is shown and no button.** |
| `moved` or `broken` | Says, in plain words, that **the link may be out of date**, names the **publisher**, and falls back to the written steps. **No address is shown and no button**: a link that moved may be somewhere that is not the publisher's any more. |

English strings for the lane to translate: "This link may be out of date." / "Look for the download on the publisher's own site: <publisher>." / "No web address is shown until it has been checked."

## 6. One-click download: offered only when all of these hold

- the row's `fetch_allowed` is exactly `yes`; and
- the effective status is `ok`; and
- the row has a `download` and its URL is https; and
- the **download host is in an allowed-host list COMPILED INTO THE PROGRAM for that id**. The registry may change a path, a version, a size or a checksum; it can **never** change the host. A registry (even a signed one) that names a different host for a fetch-allowed id gets no one-click button. (The registry's own `allowed_hosts` is for the checker; the program does not use it to widen its own list.); and
- the download follows no redirect to a host outside that compiled list (the updater's no-cross-host-redirect rule); and
- before the click the user sees what will be downloaded, from which address, its size if known and its licence, and starts it themselves; an installer that needs administrator rights is started only by the user's click, never silently; and
- afterwards the file is checked (the sha256 when the row pins one, else the file checks of the rule: name, 64-bit, exports), and a failed or refused download falls back to the written steps.

Today the seed has **no** `fetch_allowed: "yes"` row. The orchestrator lists each future `yes` row in the release checklist for the owner.

## 7. The RTL-SDR driver

The first registry row, `zadig` (the by-hand route named in Radio setup), is a page address only. FoxSDR's own one-click RTL-SDR driver (`src/core/winusb_install.hpp`) **downloads nothing**: it binds the in-box Windows WinUSB driver, so it needs no registry row and no host list. The `kZadigUrl` and the other vendor addresses in `src/core/radio_setup.cpp` should be replaced by lookups of `zadig`, `sdrplay-api`, `uhd`, `airspy-drivers`, `lime-suite` and `pothossdr` (a test in this folder fails if those constants and the registry disagree while both exist).

## 8. Tests the program's lane needs (loopback stand-in only, never a real download)

Refuse: unsigned; wrong key id; bad signature; manifest-prefix signature; payload altered by one byte; unknown format/version/alg; one invalid row; older `registry_version` than built-in; older than held; a fetch-allowed row whose host is not in the compiled list (no button); `moved`/`broken`/`unverified` (no address, no button); no trusted key compiled in (built-in only). Accept: a newer valid registry; `304`. And: network features off means no request at all; no request carries an install id or cookie.

## 9. Owner decisions touched by this contract

1. The signing key: who holds the seed, one key or two, when `kOwnerKeyHex` is filled in. Until then refreshes are not applied (section 3.2).
2. The hostname compiled into the program.
3. That `updater.cpp` still has no manifest signature (a separate decision, recorded in docs/feature-records/sysmodels.md).
