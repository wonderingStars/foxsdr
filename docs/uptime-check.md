# uptime.yml - an outside check on the FoxSDR website

The website cannot report its own death, and a redeploy leaves it answering
nothing for about a minute. This workflow asks the site and the telemetry
endpoint, from GitHub's machines, whether they are up.

## Install

Copy `uptime.yml` to `.github/workflows/uptime.yml` in the **public application
repository** and commit it to the **default branch** (GitHub runs `schedule`
triggers from the default branch only). Then run it once by hand - the **Actions**
tab, **Uptime**, **Run workflow** - and confirm it is green. That first run is also
the check of the telemetry probe below.

No secrets, no checkout, no third-party actions. A healthy run takes a few seconds.

## What it checks

Every 15 minutes (`schedule`) and on demand (`workflow_dispatch`):

1. `https://foxsdr.com/healthz` answers **200** with JSON `ok: true` and a
   non-empty `version`.
2. `https://foxsdr.com/` answers **200**.
3. `https://telemetry.foxsdr.com/` answers **400** to a **POST of the text
   `not json`**. This is the only harmless probe: the Worker (`telemetry-worker/worker.js`)
   parses the body first and answers `bad json` before it reads an install id or
   writes anything, so no usage row is stored. Nothing that would be stored is ever
   sent. (Read from `worker.js` in the application repository; not exercised
   against the live endpoint from where this was written - the first manual run
   does that.)

**It rides out a restart.** All three checks are repeated, 15 seconds apart, for
about three minutes (a redeploy is silent for about one) and the run passes the
moment all three pass at once. It fails only if a check is still failing when the
three minutes are up.

**On failure** the run fails and prints one line per failed check - which check and
the HTTP status, `000` meaning no answer at all - and nothing the server said:

```
Error: healthz: HTTP 502
Error: telemetry: HTTP 000 (a POST of invalid JSON should be answered 400)
```

It was run against local stand-ins for every one of these: all good, a site silent
for 8 seconds that then came back (passes), a site that never came back, a telemetry
endpoint answering 200, a health answer that is not `{ok: true, version}`, a home
page 500 and nothing listening.

## What must be enabled for you to be emailed

The workflow only **fails**. Sending the email is GitHub's notification system, and
that depends on settings this file cannot make:

- GitHub Actions must be enabled for the repository.
- The notification for a failed scheduled run goes to **one user**: as GitHub
  documents it, the user who last changed the `cron` line of the workflow file. That
  person needs email notifications for Actions turned on in their own **Settings,
  Notifications, Actions** (and "failed workflows only" is the quiet choice). I am
  working from memory of GitHub's documentation here and could not check the
  current menu text or the rule from where this was written: confirm both on your
  settings page, and have the person who should be told be the one who commits the
  file (or who last edits its `cron` line).
- A run that fails every 15 minutes while the site is down is an email each time.

If you want more than GitHub's email (a phone push, a chat message), that needs a
secret and a service, which this deliberately does not have.

## What it cannot do

- **GitHub delays and skips scheduled runs.** `schedule` is best effort: runs at
  busy times such as the top of the hour are often late and are sometimes dropped.
  "Every 15 minutes" is a target. Do not read a gap between runs as an outage, and do
  not read a green run as proof of the minutes around it.
- **GitHub disables it when the repository is quiet.** In a public repository,
  scheduled workflows are switched off automatically after 60 days without
  repository activity. Any commit counts, but a quiet repository will go silent
  without saying so - the check that nobody is watching is the one to worry about.
  Re-enable it from the Actions tab, or look at it on the first of each month.
- It checks from one provider's network at a time: a fault that only some regions
  see (a Cloudflare edge problem) can pass.
- A 200 from the home page and `ok: true` from `/healthz` say the process is
  answering, not that mail, report intake or the admin pages work. The site's own
  alert emails (README, **Alerts**) cover those, and cannot cover a dead site.
- If GitHub itself is down, nothing runs and nothing says so.
