# FoxSDR usage endpoint

Receives the anonymous, opt-in usage report described in
[../PRIVACY.md](../PRIVACY.md). One report per launch, describing the session
that just ended.

## Deploy

    npx wrangler deploy

## Reading the numbers

Analytics Engine has no dashboard, so a dataset that is never queried is
indistinguishable from one that was never collected. `usage.ps1` runs the
queries below and prints a summary:

    pwsh usage.ps1            # last 30 days
    pwsh usage.ps1 -Days 1    # yesterday
    pwsh usage.ps1 -Raw "SELECT ..."

It needs one thing, once: an API token with the single permission
**Account | Account Analytics | Read** (Cloudflare dashboard -> My Profile ->
API Tokens -> Create Token -> Custom token). Scope it to this account and
nothing else - it needs no zone access, no write anywhere, and it cannot post
to the Worker.

    $env:CLOUDFLARE_API_TOKEN = "<token>"

The account id comes from wrangler's cache automatically; override it with
`CLOUDFLARE_ACCOUNT_ID` if that is not present. Neither value is stored.

An empty result is a real answer - nobody reported in that window - and is
reported as such rather than as an error.

## Useful queries

Unique installs in the last 30 days — `count(DISTINCT index1)`, **not**
`uniq()`, which is approximate and reads low on small samples:

    SELECT count(DISTINCT index1) AS installs
    FROM foxsdr_usage
    WHERE timestamp > NOW() - INTERVAL '30' DAY

Version adoption, which answers "do people update, or do I need auto-update":

    SELECT blob1 AS version, count(DISTINCT index1) AS installs
    FROM foxsdr_usage
    WHERE timestamp > NOW() - INTERVAL '30' DAY
    GROUP BY blob1 ORDER BY installs DESC

Platform mix — the number that decides whether a Linux build is worth doing:

    SELECT blob2 AS os, blob3 AS arch, count(DISTINCT index1) AS installs
    FROM foxsdr_usage
    WHERE timestamp > NOW() - INTERVAL '30' DAY
    GROUP BY blob2, blob3 ORDER BY installs DESC

Which radios to prioritise:

    SELECT blob4 AS sdr, count(DISTINCT index1) AS installs
    FROM foxsdr_usage
    WHERE timestamp > NOW() - INTERVAL '30' DAY AND blob4 != ''
    GROUP BY blob4 ORDER BY installs DESC

Crash rate per install:

    SELECT sum(double2) / count(DISTINCT index1) AS crashes_per_install
    FROM foxsdr_usage
    WHERE timestamp > NOW() - INTERVAL '30' DAY
