# FoxSDR usage endpoint

Receives the anonymous, opt-in usage report described in
[../PRIVACY.md](../PRIVACY.md). One report per launch, describing the session
that just ended.

## Deploy

    npx wrangler deploy

**Deploy the Worker before shipping an application that sends `stalls`, `health`
or the unclean-exit split** (see *Display stalls*, *Failures that are not crashes*
and *Unclean exits by cause* below). The
change is additive - blobs and doubles are appended after the existing columns,
and a record without the field is still accepted exactly as before - so the order
is not a matter of breaking anything. It decides what is *lost*: an application
that reports a count to the old Worker gets a 204 and the old Worker discards the
field, and the application, having been told its record was accepted, forgets the
count. Worker first loses nothing; application first loses the counts of every
session until the Worker is deployed. (`worker.test.mjs` runs this against the
0.99.63 Worker kept byte for byte in `test-fixtures/`: a 204, the field gone, the
row exactly what a record without it writes.)

**A word added to the failure vocabulary is the same case, and is added to
`worker.js` first**: the Worker drops a word it does not know, so a client that
sends one before the Worker has it loses that count (and nothing else - the rest
of the record, and the other counts in the string, are stored). Slow frames and
recoveries (`slow.*`, `recovered.*`) are exactly that case: 0.99.64 shipped the
failure counts and the Worker that goes with them (kept byte for byte in
`test-fixtures/worker-health-counts.js`), and 0.99.65 adds the two new families, so
**this Worker is deployed before 0.99.65 ships** - the 0.99.64 Worker drops every
`slow.*` and `recovered.*` word it is sent.

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
| `blob12` | `'1'` when this client sent failure counts (`health`), else `''` |
| `blob13` | the counts, `token=count,token=count` from the fixed vocabulary, canonical order: failures, then `slow.<scope>.<tier>`, then `recovered.<what>`; `''` when nothing was counted - meaningful only where `blob12 = '1'` |
| `blob14` | `'1'` when this client sent the unclean-exit split by cause (`exits_died`, `exits_killed`, `exits_ended`, `exits_unknown`), else `''` (0.99.69) |
| `double1` | launches since install (lifetime) |
| `double2` | unclean exits since install (lifetime) |
| `double3` | length of the session the record describes, seconds |
| `double4` | seconds in the most-used demodulator |
| `double5` | display stalls this record carries (per row, not a lifetime figure) - meaningful only where `blob11 = '1'` |
| `double6` | radio opens that failed: the sum of every `radio_fail.*` count in `blob13` (per row) - only where `blob12 = '1'` |
| `double7` | radio opens that succeeded: the sum of `radio_open.*` (per row) - only where `blob12 = '1'` |
| `double8` | opened radios whose samples reached the display: the sum of `radio_data.*` (per row) - only where `blob12 = '1'` |
| `double9` | sessions in which the speakers played: the sum of `sound_ok.*` (per row) - only where `blob12 = '1'` |
| `double10` | slow frames: the sum of every `slow.*` count in `blob13` (per row) - **measured only from 0.99.65** (see the rule below) |
| `double11` | slow frames of a second or more: the sum of the `slow.*.1s` and `slow.*.5s` counts (per row) - measured only from 0.99.65 |
| `double12` | recoveries: the sum of every `recovered.*` count in `blob13` (per row) - measured only from 0.99.65 |
| `double13` | unclean exits that were a fault in the application - **died** (lifetime, never reset) - only where `blob14 = '1'` |
| `double14` | unclean exits ended from outside while the application was working normally - **killed** (lifetime) - only where `blob14 = '1'` |
| `double15` | unclean exits that were the operating system closing the session - **ended** (lifetime) - only where `blob14 = '1'` |
| `double16` | unclean exits with no evidence of how they ended - **unknown** (lifetime) - only where `blob14 = '1'` |

Column budget: Analytics Engine allows 20 blobs and 20 doubles a row. `blob1`..`blob14`
and `double1`..`double16` are used: 6 blobs and 4 doubles are free. Slow frames and
recoveries took no blob (they ride in `blob13`) and three doubles; the unclean-exit split
took one blob (its marker) and four doubles. (The 16 KB limit on a row's blobs is
nowhere near: `blob13` is never more than 832 characters.)

### Unclean exits by cause

`double2` has always been the number of sessions that never wrote the clean-exit
marker, and that is four different things: the application failing, a task ended
from outside (Task Manager, `taskkill`, an installer, a closed console window),
the operating system closing the session (a log off, a shutdown), and an ending
nothing can explain (a power cut, Diagnostics off). The record carries the four
classes beside it as `exits_died`, `exits_killed`, `exits_ended` and
`exits_unknown` (PRIVACY.md, *How an unclean exit ended*; the rule that sorts an
exit into a class is `src/core/exit_cause.hpp` in the application). **Every
unclean exit from 0.99.69 on is in exactly one of them, so their sum is the
unclean exits counted since then and `double2` minus the sum is how many came
before the split existed.** They are written to `double13`..`double16`, with
`blob14` as the marker.

**An unwritten split is not zero.** This follows the convention the stall count
and the failure counts set - a marker blob that is `'1'` only when the client sent
the field, and doubles that hold 0 otherwise - and not a second one (`-1` in the
doubles would have been two ways of saying one thing). A row from an application
older than 0.99.69, from a Worker that did not know the fields, or whose split was
not usable has `blob14 = ''` and four zeros that mean *not measured*, never *none
ended that way*. **Always ask about the split with `blob14 = '1'`.**

**Validation.** All or nothing, and strict like `stalls`: the four must all be
present and each a JSON **number** that is finite and not negative (`"1"`, `true`,
`null`, arrays and objects are not usable); a fraction is floored and a value above
1,000,000 (the bound `double2` has) is clamped. A split whose four add up to **more
than `crashes`** was not written by the application - a class is only ever counted
together with an unclean exit - and is treated as not reported. An unusable split is
`blob14 = ''`, the doubles are 0, and the rest of the record is still written. A
heartbeat carrying the fields is still just a heartbeat.

**They are lifetime counters, like `double2`**, so a window's figure is a *difference*
and never a sum: for each install, the highest value among its rows of a version
minus the lowest, summed over the installs (`usage.ps1` and the site's reliability
page do this; the install id is grouped by in the query and never selected). That
counts the exits between an install's first and last report of a version, so the
denominator for a share is the sessions whose end is on record - an install's
sessions of that version, minus one.

**Deploy the Worker before an application that sends the split is released.** The
old Worker answers 204 and drops the four fields, and the application, told its
record was accepted, has nothing to resend. Every row written until the Worker is
deployed has `blob14 = ''` - those sessions are unmeasured, whatever the client sent -
and because the counters are cumulative, the exits that happened in between show up
in the first row written after the deploy, which a per-version difference then
attributes to that row's version and not to the sessions they came from. Worker
first loses nothing. `worker.test.mjs` holds the old behaviour against the 0.99.67
Worker kept byte for byte in `test-fixtures/worker-0.99.67.js`.

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

### Failures that are not crashes

A radio that would not open, no sound output, an update or a plugin install that
failed: failures where the application did not crash and so nothing reported
them. The record carries one string, `health`, of `token=count` pairs
(`radio_open.rtlsdr=1,radio_fail.rtlsdr.busy=2,sound_ok.wasapi=1`); PRIVACY.md
(*Failures that are not crashes, in full*) lists every word and what it means.

**Encoding, and why one string.** One blob (`blob13`) for all the tokens plus one
marker blob and seven doubles, instead of a column per event: the vocabulary is
277 tokens wide (13 drivers x 8 reasons alone is 104) and a column per token would
not fit in 20, and every new event would be a schema change. As one string it is
still queryable - `startsWith(blob13, 'radio_fail.rtlsdr.busy=') OR
position(',radio_fail.rtlsdr.busy=' IN blob13) > 0` is "sessions with that failure",
and `usage.ps1` reads the counts out of the distinct strings. The four doubles are
the sums that SQL cannot take out of a string (all radio failures, all radio
opens, all radios that delivered samples, all sessions with sound).

**Strict.** The Worker keeps a token only if it is exactly an event of its
vocabulary (`HEALTH_EVENTS` in `worker.js`, between two markers) followed by one
word from each of that event's lists; anything else in the string is dropped and
what is stored is the canonical re-encoding (vocabulary order, repeats merged,
counts 1..999, at most 24 distinct failure tokens of which at most 8 are radio
failures, at most 8 distinct `slow` tokens and at most 8 distinct `recovered` ones).
Over its cap a family keeps the **worst**: `slow` the higher tier first (`5s`, then
`1s`, then `250ms`), then the higher count, then the earlier in the written order
(the scope's place in the frame, then the tier); `recovered` the higher count, then
the earlier in the written order. The application writes the same selection
(`health::selectForRecord`), and `test-fixtures/health-cases.json` is read by
both its test and the Worker's, so the two cannot drift.
The doubles are computed from those validated tokens, never from a number the
client sends beside them. `tests/test_health_events.cpp` holds `HEALTH_EVENTS`
to the application's own table and to PRIVACY.md, and `worker.test.mjs` throws
hostile text at it (a device name, a serial, a path, an injection, a number in
another script).

**Not reported is not zero.** `blob12 = '1'` is written only when the client
sent a usable string - a JSON string; the empty string is a real "nothing
failed". A number, `null`, an array, an object, a string with no legal pair in it,
or one longer than the application ever writes is *not reported*: the record is
still stored (a bad optional field must not cost a session's length), `blob12 = ''`,
`blob13 = ''` and the doubles are 0. Always ask about failures with
`blob12 = '1'`; builds that do not send them are *unmeasured*, not clean.

**`blob12 = '1'` does not say that slow frames and recoveries were measured.** 0.99.64
sent the failure counts and nothing of the two families that 0.99.65 added, so a
0.99.64 row has `blob12 = '1'`, a `blob13` with failures only and **zeros** in
`double10..12` - zeros that mean "never measured", not "none happened". `blob12`
keeps the meaning the website already queries (the client sent failure counts) and
nothing about it changed; the rule that tells the two apart is on the version in
`blob1`. **For anyone writing a reader: slow frames and recoveries are measured from
0.99.65; a row from an earlier build is unmeasured whatever its doubles say.** Compare
versions as versions (0.99.100 is later than 0.99.65; as text it is earlier), and
treat a `blob1` that is not plain `major.minor.patch` - a pre-release such as
`0.99.65-rc1`, a nightly, an empty or odd string - as *not measured*: it cannot be
ordered reliably, and that is the safe side. `usage.ps1` holds the rule in one named
constant, `$FirstVersionThatMeasuresSlowFramesAndRecoveries`.

**A row is a record, not necessarily one session.** Counts ride until a record
carrying them is accepted, so after a failed send a record can carry more than
one session's counts, and every count is per row (like `double5`). Some events are
counted once per session by the application (`scan_none`, `sound_ok.*`,
`sound_fail.*`, `plug_load.*`), so for those a count of 1 means "this session had
it".

**The column map for readers of the dataset** (the website reads it too):

| To get | Ask for |
|---|---|
| sessions that reported failure counts | `blob12 = '1'` |
| sessions with one failure | `blob12 = '1' AND (startsWith(blob13, 'TOKEN=') OR position(',TOKEN=' IN blob13) > 0)` |
| installs with one failure | the same, with `count(DISTINCT index1)` |
| radio open failure rate by version | `sum(double6) / (sum(double6) + sum(double7))` where `blob12 = '1'`, grouped by `blob1` |
| opened radios that never delivered | `sum(double7) - sum(double8)` where `blob12 = '1'` |
| sessions that had sound | `sum(double9)` where `blob12 = '1'` (a session count: `sound_ok` is once a session) |
| builds that do not measure it | every `blob1` seen, minus those with `blob12 = '1'` |
| slow frames by version | `sum(double10)` and `sum(double11)` where `blob12 = '1'`, grouped by `blob1`, **keeping only the versions that measure them (0.99.65 and later)**; per 1,000 records: `1000 * sum(double10) / count()` |
| installs with any slow frame | `count(DISTINCT index1)` where `blob12 = '1' AND double10 > 0` (a 0.99.64 row never qualifies, so this needs no version rule) |
| installs with slow frames in one scope | `blob12 = '1' AND (startsWith(blob13, 'slow.SCOPE.') OR position(',slow.SCOPE.' IN blob13) > 0)`, with `count(DISTINCT index1)` |
| installs that recovered from anything | `count(DISTINCT index1)` where `blob12 = '1' AND double12 > 0` (likewise) |
| installs that recovered at one place | the failure query above with `TOKEN` = `recovered.WORD` |
| builds that do not measure slow frames or recoveries | every `blob1` seen, minus the versions 0.99.65 and later (as versions; a pre-release or odd `blob1` is *not measured*) |

**The rule, in one line: slow frames and recoveries are measured from 0.99.65; a row
from an earlier build is unmeasured whatever its doubles say.** Any average, rate or
"none in this window" over `double10..12` or over `slow.*` / `recovered.*` tokens must
leave 0.99.64 and earlier rows out of its denominator, not count them as zero.

Compatibility, as `worker.test.mjs` measures it:

| Client | Worker | Status | What is stored |
|---|---|---|---|
| 0.99.61 (no `stalls`, no `health`) | new | 204 | the eleven old blobs and five doubles as before; `blob12`, `blob13` empty, `double6..9` = 0 - not reported |
| 0.99.62 (`stalls`) | new | 204 | as above, with the stall count |
| 0.99.64 (`health`: failure counts only) | new | 204 | the row the 0.99.64 Worker wrote, byte for byte, plus `double10..12` = 0 - **which is unmeasured, not zero** (the rule above) |
| 0.99.65 (`health`, with `slow` and `recovered`) | new | 204 | everything, validated; `double10..12` the three sums, measured |
| 0.99.65, malformed `health` | new | 204 | everything except `health`: not reported |
| 0.99.64 | **old** (0.99.63) | 204 | the old columns only; `health` is discarded, and the client, told 2xx, forgets it |
| 0.99.65 | the 0.99.64 Worker (`test-fixtures/worker-health-counts.js`, the one deployed today) | 204 | the failure tokens as before, `slow` and `recovered` tokens dropped, `double6..9` right, no `double10..12`; a record with only the new families is *not reported* there - which is why the new Worker is deployed first |
| 0.99.65, the longest record the client writes (832 characters) | the 0.99.64 Worker | 204 | every failure token read - a record is never longer than that Worker's own limit |
| a bad id, GET, bad JSON, over 4096 bytes | new | 400, 405, 400, 413 | nothing (unchanged) |

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

After it, **What fails, by version** (0.99.64): for each version that sends the
failure counts, its installs and records and the four headline sums (radio opens
that failed and that succeeded, radios whose samples arrived, sessions with
sound), then every failure event seen - installs (`count(DISTINCT index1)`),
records carrying it and the total count - and, apart, the builds that send no
failure counts, which are *not measured*.

Then **Slow frames, by version** (0.99.65): per version, the installs that
reported, the installs with at least one slow frame, slow frames per 1,000 records
and those of a second or more per 1,000, and the top five scopes (installs, records,
slow frames, of a second or more); and **Recovered from, by version**: per version,
the installs that reported, those with at least one recovery, the sum, and the top
five words (installs, records, count). The same rule: only rows with `blob12 = '1'`
**and a version that measures them** (the constant
`$FirstVersionThatMeasuresSlowFramesAndRecoveries`, 0.99.65, compared as a version),
installs from `count(DISTINCT index1)`; every other version seen - 0.99.64, whose
rows carry zeros it never measured, a pre-release, an odd string, a build with no
`health` at all - is listed apart as *not measured*, never printed as a zero, and
moves no figure of the versions that did measure (every figure is per version).

It then prints **Unclean exits by cause, by version** (0.99.69): for each version
that reports the split (`blob14 = '1'`), the installs, the *ends seen* (their
sessions minus one each, the sessions whose end is on record), and the rise of the
died, killed, ended and unknown counters summed over the installs, with died as a
share of ends seen. Builds that send no split are listed apart as *not measured*.

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

Unclean exits by cause (0.99.69) - one row per install and version, each counter
the rise of a lifetime counter across the window; add the rows up per version (the
install id is grouped by and not selected, and only rows with the marker are
asked about, because every other row is unmeasured):

    SELECT blob1 AS version, count() AS sessions,
           max(double13) - min(double13) AS exitsDied,
           max(double14) - min(double14) AS exitsKilled,
           max(double15) - min(double15) AS exitsEnded,
           max(double16) - min(double16) AS exitsUnknown
    FROM foxsdr_usage
    WHERE timestamp > NOW() - INTERVAL '30' DAY AND blob14 = '1'
    GROUP BY blob1, index1

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
