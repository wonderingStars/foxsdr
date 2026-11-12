# Deploying the links Worker

**Nothing in this folder has been deployed.** No Cloudflare resource exists for it yet, nobody has logged in on its behalf, and no request was ever made to an outside address while it was built. Deploying needs the owner's explicit yes (owner decisions below). Run everything from `links-worker/`.

Cloudflare facts used here were read from Cloudflare's own documentation on 2026-11-12 (Workers limits, KV limits, Cron Triggers); the exact command syntax is Wrangler 4's and should be confirmed with `npx wrangler <command> --help` at deploy time, because `npx` fetches whatever version is current.

## Owner decisions (before step 1)

| # | Decision | Recommendation |
|---|---|---|
| 1 | **Hostname.** The telemetry Worker answers on `telemetry.foxsdr.com`. This one is proposed as `links.foxsdr.com`. The name is compiled into every shipped program, so it must be one FoxSDR can repoint. It needs the `foxsdr.com` zone on the same Cloudflare account. | `links.foxsdr.com` |
| 2 | **Signing key for the registry.** Who generates and holds the Ed25519 seed, offline; whether it is the same key as the update-manifest signature (`kOwnerKeyHex`, still empty in `sys_manifest_sig.cpp`) or a second one. The envelope carries a key id, so a key list can rotate. No key is, or may ever be, in this repository or in Worker code. | One key, held offline (the two uses are domain-separated, so a manifest signature cannot be replayed as a registry). Until the key is in the program, the program uses its built-in registry only (CLIENT-CONTRACT.md). |
| 3 | **Cloudflare plan.** The free plan allows 50 subrequests per invocation and KV calls count; the daily run is budgeted at 47 (`CHECK_BUDGET = 45`) so it fits, and the seed (15 rows, 10 of them with an address) uses about 10 requests. Beyond about 40 addresses the free plan covers the list over several days instead of one (oldest-checked rows go first). The paid plan removes the limit. | Say which plan the account is on (this lane cannot see it); raise `CHECK_BUDGET` if paid. |
| 4 | **Who reads the admin list.** The website's admin page calls it with a secret (SITE-HANDOFF.md). | The existing site admin. |
| 5 | **Alerting.** Only a pull page exists. A push (e-mail, message) when a link goes `moved` or `broken` is NOT built. | Look at the page weekly at first; decide after a month of real results. |

## 1. Check the folder (no network, no login)

```bash
cd links-worker
node tools/validate.mjs
node --test "test/*.test.mjs"
```

Both must pass. `npm install` is not needed: the Worker and its tests have no dependencies.

## 2. Create the KV namespace (creates a Cloudflare resource: needs `wrangler login` and the owner's yes)

```bash
npx wrangler login
npx wrangler kv namespace create LINKS_STATE
```

Wrangler prints the namespace id. Paste it into `wrangler.toml`, replacing `REPLACE_WITH_KV_NAMESPACE_ID`. (Why KV: see the comment above `[[kv_namespaces]]`: one key, one read and one write per run.)

## 3. Choose the hostname

In `wrangler.toml`, uncomment the `[[routes]]` block and put the decided hostname in `pattern`. Without it the Worker has no public address at all (`workers_dev = false`), which is the safe state.

## 4. Set the admin secret

```bash
npx wrangler secret put ADMIN_TOKEN
```

Wrangler asks for the value: paste a long random string (at least 16 characters; the Worker treats a shorter one as "not set" and refuses everyone). A way to make one that prints it once, to the terminal only:

```bash
node -e "console.log(require('node:crypto').randomBytes(32).toString('hex'))"
```

Store it where the site's other admin secrets are kept. Never put it in a file in this repository.

## 5. Sign the registry (owner's step; skip until the key decision is made)

The signing step is `tools/sign-registry.mjs`, written but never run here. The owner runs it, with the seed file kept offline:

```bash
node tools/sign-registry.mjs --seed-file <path to the offline seed file>
```

That writes `registry/libraries.signed.json`. Before the key exists, leave the file absent: `/v1/libraries.signed.json` answers 404 and the program keeps its built-in list. To verify the envelope against the public key during the next step, set `LINKS_PUBKEY_HEX` (64 hex characters) in that shell.

## 6. Build, gate, deploy

```bash
node tools/build-bundle.mjs
node tools/predeploy.mjs
npx wrangler deploy --dry-run
npx wrangler deploy
```

`predeploy` refuses (exit 1, says why) if the registry does not validate, the bundle is out of date, the KV id is still the placeholder, or no route is set. `npm run deploy` runs `predeploy` and then `wrangler deploy`.

The cron trigger (`17 4 * * *`, UTC) is part of the same deploy. A schedule change can take up to 15 minutes to reach the network.

## 7. Check it is alive

```bash
curl -s https://<hostname>/health
curl -s https://<hostname>/v1/status.json
curl -sI https://<hostname>/v1/go/zadig
```

`status.json` says `"stale": true` and `last_run: null` until the first daily run has happened. To see the admin list (in PowerShell, so the secret is not typed into the command line):

```powershell
$env:LINKS_ADMIN_TOKEN = Read-Host "admin token"
curl.exe -s -H "Authorization: Bearer $env:LINKS_ADMIN_TOKEN" https://<hostname>/admin/links.json
```

To run the checker once locally without deploying: `npx wrangler dev --test-scheduled`, then request the scheduled address Wrangler prints (Cloudflare's current docs give `/cdn-cgi/local/scheduled`). That run makes real requests to the registry's addresses.

## Roll back

`npx wrangler rollback` returns to the previous version. The KV state is separate and is not rolled back (it only describes what the checker saw).

## What can go wrong, and what you will see

| Symptom | Meaning |
|---|---|
| `admin endpoint is not configured` (503) | `ADMIN_TOKEN` is not set, or shorter than 16 characters. |
| `status.json` has `"stale": true` after a day | The cron did not run: check the trigger is listed in the dashboard and `wrangler tail` during the next 04:17 UTC. |
| a row stays `unverified` | It has no address yet: add one (ADDING-A-LIBRARY.md). |
| the Worker answers 1101 for every request | The bundled registry does not validate (the Worker refuses to start with one). `predeploy` normally prevents this. |
