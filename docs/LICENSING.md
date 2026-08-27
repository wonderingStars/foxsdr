# FoxSDR licence keys — format and validation contract

This file is the contract between the SIGNER (the website, Go, `licensing.go`)
and the VERIFIER (the application, C++, `src/core/licence.*`). Both implement
exactly this; the cross-language test vector at the bottom is compiled into
both test suites, so the two ends cannot drift without a red test.

## Model

Perpetual licence, updates included for 12 months. The application NEVER stops
working because a licence "expired" — `updatesUntil` gates which builds the
licence covers, not whether the app runs. Unlicensed, the app runs a 14-day
full-featured trial; after that the audio output is gated (receive, decode and
display all keep working) until a licence is entered.

## Key format (wire form)

    FOXSDR1.<base64url(payload)>.<base64url(signature)>

- `FOXSDR1.` — literal prefix, versions the whole scheme.
- base64url — RFC 4648 URL-safe alphabet, NO padding (Go: `base64.RawURLEncoding`).
- `payload` — canonical JSON, UTF-8, keys sorted alphabetically, no whitespace
  (exactly what Go's `encoding/json.Marshal` of a map produces):

      {"edition":"personal","email":"...","id":"FSL-xxxxxxxx","issued":"YYYY-MM-DD","name":"...","updatesUntil":"YYYY-MM-DD","v":1}

- `signature` — Ed25519 (64 bytes) over the EXACT payload bytes as decoded from
  the wire form. The verifier never re-serialises JSON before verifying;
  canonical means "the bytes that were signed", by construction.

## Validation rules (verifier)

1. Prefix must be `FOXSDR1.`; exactly three dot-separated segments.
2. Both segments must base64url-decode; signature must be exactly 64 bytes.
3. Ed25519-verify(signature, payload bytes, embedded public key) must pass.
4. Payload must parse as JSON with `v == 1` and non-empty `name`, `id`,
   `issued`, `updatesUntil`.
5. Anything else — unknown `v`, extra dots, padding characters, truncation —
   is INVALID, never "partially accepted".

A licence with `updatesUntil` in the past is still a VALID licence (perpetual);
the app reports "updates until <date>" and the update UI may say a newer build
is outside the licence. Nothing else changes.

## Keys

- Production public key (embedded in the app, hex):

      286c58f58fa6436b829a4dcfb70e3f7b44c7392583a45d54390cae68bd6cda77

- Production PRIVATE key: `C:\Users\steve\.foxsdr\licence-signing-key.json` on
  the dev machine and `/home/foxsdr/.foxsdr/licence-signing-key.json` on the
  server. NEVER in a repository. JSON: `{"seed":"<hex 32B>","public":"<hex 32B>"}`;
  the Ed25519 private key is derived from the seed.

## Trial (application side)

- First run writes the trial-start date (YYYY-MM-DD) into the config AND a
  mirror marker; the EARLIEST of the two wins, so deleting one does not reset
  the trial. 14 days full function.
- Clock rollback: the app also records the latest date it has ever seen and
  uses `max(today, latestSeen)` for the expiry test.
- Expired + unlicensed: audio output gated OFF via a dedicated gate in the
  sink (NOT via the volume control — the user's volume must survive
  activation), an amber banner explains why, and the licence entry box is one
  click away. Everything except audible audio keeps working.
- Entering a valid key stores it (config dir, `licence.key`, the raw wire
  string) and lifts the gate immediately, no restart.

## Cross-language test vector (test keypair — safe to embed anywhere)

Test keypair derived from the fixed Ed25519 seed `0x01 0x02 ... 0x20`.

- TEST public key (hex):

      79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664

- TEST payload (exact bytes between the quotes):

      {"edition":"personal","email":"test@example.com","id":"FSL-TEST0001","issued":"2026-08-20","name":"Test User","updatesUntil":"2027-08-20","v":1}

- TEST licence (must VERIFY against the test public key):

      FOXSDR1.eyJlZGl0aW9uIjoicGVyc29uYWwiLCJlbWFpbCI6InRlc3RAZXhhbXBsZS5jb20iLCJpZCI6IkZTTC1URVNUMDAwMSIsImlzc3VlZCI6IjIwMjYtMDgtMjAiLCJuYW1lIjoiVGVzdCBVc2VyIiwidXBkYXRlc1VudGlsIjoiMjAyNy0wOC0yMCIsInYiOjF9.cfO_uUdjGzP8V-0ISjwEaiCnOuG8ZQQ7QUkxgmtO-7t3wYQvG9XmJguhCU_h40Etz9sbwHMN5H89Xp9G7O7XCw

- TEST tampered (same signature, payload's "personal" changed to "Personal" —
  must FAIL verification):

      FOXSDR1.eyJlZGl0aW9uIjoiUGVyc29uYWwiLCJlbWFpbCI6InRlc3RAZXhhbXBsZS5jb20iLCJpZCI6IkZTTC1URVNUMDAwMSIsImlzc3VlZCI6IjIwMjYtMDgtMjAiLCJuYW1lIjoiVGVzdCBVc2VyIiwidXBkYXRlc1VudGlsIjoiMjAyNy0wOC0yMCIsInYiOjF9.cfO_uUdjGzP8V-0ISjwEaiCnOuG8ZQQ7QUkxgmtO-7t3wYQvG9XmJguhCU_h40Etz9sbwHMN5H89Xp9G7O7XCw

## Ed25519 in the application

Vendored TweetNaCl (`third_party/tweetnacl/`, public domain, fetched verbatim
from tweetnacl.cr.yp.to 20140427). TweetNaCl has no detached-verify call;
verify by constructing `sm = signature || payload` and calling
`crypto_sign_open`. `randombytes` is referenced by the (unused) keypair
function — provide an aborting stub so the linker is satisfied and any
accidental signing path dies loudly. The application only ever verifies.
