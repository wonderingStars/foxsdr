# FoxSDR usage endpoint

Receives the anonymous, opt-in usage report described in
[../PRIVACY.md](../PRIVACY.md). One report per launch, describing the session
that just ended.

## Deploy

    npx wrangler deploy

**Deploy the Worker before shipping an application that sends `stalls`** (see
*Display stalls* below). The change is additive - one blob and one double are
appended after the existing columns, and a record without the field is still
accepted exactly as before - so the order is not a matter of breaking anything.
It decides what is *lost*: an application that reports a count to the old Worker
gets a 204 and the old Worker discards the field, and the application, having
been told its record was accepted, forgets the count. Worker first loses
nothing; application first loses the counts of every session until the Worker is
deployed.

## The columns of `foxsdr_usage`

Written in `worker.js`; never renumbered, because a column that changes meaning
corrupts every row written before.

| Column | Holds |
|---|---|
| `index1` | install id (random, made on the user's machine) |
| `blob1` | application version, up to 48 characters |
| `blob2`, `blob3` | operating system and build, architecture |
| `blob4` | SDR model, serial already stripped |
| `blob5`, `blob6`, `blob7` | most-used demodulator, installed plugins, panels opened |
| `blob8`, `blob9`, `blob10` | install channel, first-run day (UTC), version that created the id |
| `blob11` | `'1'` when this client sent a display-stall count, else `''` |
| `double1` | launches since install (lifetime) |
| `double2` | unclean exits since install (lifetime) |
| `double3` | length of the session the record describes, seconds |
| `double4` | seconds in the most-used demodulator |
| `double5` | display stalls this record carries (per row, not a lifetime figure) - meaningful only where `blob11 = '1'` |

### Display stalls

A freeze the hang watchdog classifies as the display driver's rather than the
application's (`kind: stall`) is never uploaded, so how often it happens was
invisible. The record carries one integer, `stalls`: the number the application
has counted and the server has not yet been told. `double5` holds it and
`blob11` says whether it was sent at all.

Both columns, because every row written before this existed, and every row from
an old client, has no value in either, so the count alone cannot tell *reported
zero* from *never reported* - and a reader that counted the latter would print a
confident zero for something nobody measured. (What an unwritten column reads
back as is not documented by Cloudflare, and nothing here relies on it: the
marker is a value that is only ever *written* when the field was sent.) Always
ask about stalls with `blob11 = '1'`.

Validation: only a JSON **number** counts (`"3"`, `true`, `null`, arrays and
objects do not); a negative or non-finite value is not usable; a fraction is
floored; anything above 100000 is clamped to it. An unusable value is treated as
*not reported* (`blob11 = ''`, `double5 = 0`) and the rest of the record is still
written - a bad optional field must not cost a session's length. A heartbeat
carrying a `stalls` field is still just a heartbeat.

Because the application carries a count until a record carrying it is accepted
(HTTP 2xx), `double5` is a per-row *delta*: a window's total is `sum(double5)`,
not a maximum.

## Reading the numbers

Analytics Engine has no dashboard, so a dataset that is never queried is
indistinguishable from one that was never collected. `usage.ps1` runs the
queries below and prints a summary:

    powershell -File usage.ps1            # last 30 days
    powershell -File usage.ps1 -Days 1    # yesterday
    powershell -File usage.ps1 -Raw "SELECT ..."

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

With no token the script stops at once and says so; it never prints a table of
zeroes for data it could not read.

`usage.ps1` ends with **Display stalls, by version**: for each version that
reports a count, how many installs reported one, how many of those reported at
least one stall, and the total number of stalls. Builds that send no count are
listed apart as *not measured* - they are not zero.

The Worker and the reader are held by `worker.test.mjs` (`node --test`, needs
Node 22.7 or later; registered with ctest as `telemetry_worker`). It imports the
handler with a fake `env`, and runs `usage.ps1` against a stand-in for the SQL API
on loopback - no wrangler, no account, no network.

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

Display stalls per version - installs that report a count, then of those the
installs with at least one stall and the stalls in total. Two queries, because
`count(DISTINCT ...)` is the only honest install count and the second needs a
`WHERE` the first must not have:

    SELECT blob1 AS version, count(DISTINCT index1) AS installs
    FROM foxsdr_usage
    WHERE timestamp > NOW() - INTERVAL '30' DAY AND blob11 = '1'
    GROUP BY blob1 ORDER BY installs DESC

    SELECT blob1 AS version, count(DISTINCT index1) AS installs, sum(double5) AS stalls
    FROM foxsdr_usage
    WHERE timestamp > NOW() - INTERVAL '30' DAY AND blob11 = '1' AND double5 > 0
    GROUP BY blob1
