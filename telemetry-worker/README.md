# FoxSDR usage endpoint

Receives the anonymous, opt-in usage report described in
[../PRIVACY.md](../PRIVACY.md). One report per launch, describing the session
that just ended.

## Deploy

    npx wrangler deploy

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
