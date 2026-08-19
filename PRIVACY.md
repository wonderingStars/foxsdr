# Privacy

FoxSDR collects nothing about you unless you switch it on, and this document
lists exactly what it sends when you do.

## The update check

Once per launch, FoxSDR asks `https://foxsdr.com/api/update` whether a newer
version exists. The whole request is:

    GET /api/update?v=0.55.0

That is the entire payload: the version running. No identifier of any kind is
sent, no cookie is stored, and the answer is not cached anywhere that could be
correlated with a later request. It is a separate thing from the usage report
and shares no state with it.

The answer names the newest build, its checksum, and what changed since your
version - which is shown to you in the application, so you can decide whether
an update is worth the interruption rather than being told only that one
exists.

**Nothing is downloaded or installed unless you press the button.** When you
do, the installer is fetched over https and its SHA-256 is checked against the
digest the server published *before* the file is given a name anything could
run. A download that does not match is deleted.

Turn it off under **Settings -> Updates**. With it off the application never
contacts the update service, and never learns that a new version exists.

Why it defaults to on: version 0.55.0 fixed a fault that stopped every earlier
build from detecting any radio at all. Of the 49 people who had downloaded one
of those builds, 46 never returned to the website, and there was no way to
reach them - the downloads are anonymous, which is the point. Most of them are
probably still running it. A check that defaulted to off would have reached
exactly as many.

## The short version

- **Usage reporting is ON by default, and you can turn it off.** It sends the
  anonymous counts listed below and nothing else. Switching it off stops all
  reporting and deletes the identifier described below.
- **No personal data is collected**, and no IP address or location is recorded.
- **Nothing about what you listen to is ever collected** — no frequencies, no
  positions, no decoded messages. Not when reporting is on, not ever.
- **The update check is ON by default, and you can turn it off.** Once per
  launch the application asks foxsdr.com whether a newer version exists. It
  sends **the version you are running and nothing else** — no identifier, no
  install id, no cookie kept — and it is not the usage report; the two share
  nothing. Nothing is downloaded or installed without you pressing a button.
- The application makes one other network request, only when you press
  **Browse** in the plugin panel, to fetch the plugin catalogue.

## What is sent when usage reporting is enabled

One report per launch, describing the session that just finished:

| Field | Example | Why |
|---|---|---|
| Install identifier | `4f9c…` (32 random hex characters) | Tells 100 users apart from one user launching 100 times. Generated at random on your machine on first run, and **deleted when you turn reporting off**, so a later change of mind cannot be joined back to it; not derived from your hardware, network or name. |
| Application version | `0.48.0` | Whether people update, and whether an auto-updater is needed. |
| Operating system | `Windows 10.0.22631` | Which platforms are actually used, and whether a Linux build is worth building. |
| Architecture | `x64` | As above. |
| Launch count | `12` | Whether the software gets used more than once. |
| Crash count | `1` | How often it fails. |
| Session length | `3600` seconds | Whether sessions are minutes or hours. |
| SDR model | `uhd b200` | Which radios to prioritise. **Serial numbers are stripped** before sending. |
| Demodulators used | `WFM: 3000s` | Which modes justify further work. |
| Panels opened | `map, decoded` | Which features are used. |
| Installed plugins | `ADS-B 1.0.0` | Which decoders justify further work. |

That is the complete list. The payload is asserted field-by-field by an
automated test (`tests/test_telemetry.cpp`), so a new field cannot be added
without that test failing and this document being updated with it.

## What is never sent

These are design constraints, not current policy:

- **Frequencies you tune to.** What somebody listens to is the most sensitive
  thing this software knows. In the United Kingdom, intercepting a message you
  are not authorised to receive, or disclosing its contents, is an offence
  under section 48 of the Wireless Telegraphy Act 2006.
- **Anything decoded** — pager messages, satellite traffic, aircraft, vessels.
- **Your position**, or the position of anything you receive.
- **Your IP address or any location derived from it.** A network request
  necessarily reaches the server from an address, because that is how the
  internet works; nothing records or stores it, the receiving endpoint reads no
  connection information, and it writes no request logs.
- **Hardware serial numbers.** The SDR model is useful; the serial identifies
  your individual radio, and is removed.
- Your name, your machine's name, your user account, or any file path.

## Turning it off

**Settings → Usage reporting**, and untick it. Reporting is on by default, so
this is the switch that stops it. Turning it off deletes the install
identifier, so if you ever turn it back on you get a new one that cannot be
linked to the old one.

Nothing else in the application is affected: no feature depends on reporting
being on, and nothing nags you about having turned it off.

## Where the data goes

To a Cloudflare Worker operated by the FoxSDR project, which aggregates the
counters above. The source is in `telemetry-worker/` in this repository so you
can read what the receiving end does with it.

## Data protection

The reports contain no personal data, so there is nothing to request access
to, correct or erase — there is no record anywhere that can be connected to
you. If you would like the install identifier removed from future reports,
turn usage reporting off; if you would like it removed from past ones, contact
us with the identifier and it will be deleted.
