# synthetic.yml - a weekly synthetic report through the whole early-warning pipe

An early-warning system that has silently stopped looks exactly like a quiet week. Once a week
this workflow posts a clearly marked, fake crash report to the real endpoint on foxsdr.com and
expects it to be taken. If the owner's phone then shows a message that says **SYNTHETIC TEST**,
report -> ingestion -> alert works, without anyone crashing FoxSDR.

## What it sends

`POST https://foxsdr.com/api/crash` with the header `X-Foxsdr-Synthetic: <token>` and a body that
is an ordinary crash report on the ordinary wire contract (`schema` 1) with:

| Field | Value |
|---|---|
| `signature` | `SYNTHETIC-PIPELINE-CHECK` (the only signature the site accepts with the header) |
| `installId` | `5afe5afe5afe5afe5afe5afe5afe5afe` (the reserved id, also the uptime canary's) |
| `version` | `0.0.0-synthetic` |
| `log` | `SYNTHETIC TEST - NOT A REAL FAULT` |

Nothing in it is real, and nothing in it comes from a user.

## What the site does with it

It goes through the **real** endpoint: method, per-address and total rate limits, the size cap, JSON
and schema validation, the kind and signature checks and the sanitiser. Then, because it carries the
header, it is **diverted before the store**: the shared token is compared in constant time (a wrong
token, or a site with no token configured, answers 403 and stores and announces nothing), the report
is run through the sanitiser and dropped, and the alert engine sends the owner a push and an email
that say **SYNTHETIC TEST** on every line. It is in no fault group, no crash-free or working rate,
no digest, no channel figure and no fault alert. (What this leaves unexercised is the store's own
`AddObserved`, which the site's unit tests cover.)

## The other direction

The site watches this workflow too: if the token is set and no synthetic report has arrived for nine
days, the owner is told. GitHub disables a scheduled workflow after 60 days without repository
activity and delays or skips scheduled runs, and a workflow that never runs fails silently - that has
to be noticed from the receiving end.

## Setting it up (nothing here is done by committing the file)

1. Make a long random value: `openssl rand -hex 24`.
2. Put it in this repository's **Actions secrets** as `FOXSDR_SYNTHETIC_TOKEN`.
3. Put the **same value** in the site's `/etc/foxsdr-site.env` as `FOXSDR_SYNTHETIC_TOKEN` and restart
   the site. (24 to 128 letters, digits or `. _ ~ -`; anything else is refused and said on
   `/admin/watchers`.)
4. The workflow only runs on a schedule from the **default branch**; run it once by hand (**Actions ->
   Synthetic -> Run workflow**) and confirm the SYNTHETIC TEST message arrives.

Order matters little: with the secret set here and not on the site the run fails with HTTP 403 and
says so; with it set on the site and not here the run fails and says the secret is missing; either
way nothing is stored.

## What it cannot do

- It proves the pipe at the moment it ran, not the minutes around it.
- The schedule is best effort. The nine-day rule on the site is the net under it.
- A passing run proves the site took the report; the *message reaching a phone* is the owner's to
  see - the job cannot read the owner's ntfy topic (it is the only secret there) and does not try.
