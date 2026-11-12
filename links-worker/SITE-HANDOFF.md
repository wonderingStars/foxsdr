# Hand-off for the website (the website repository is not part of this lane)

Two things for the site: an **admin page** that shows the links that need a person, and **public links** that never die. Nothing here has been built on the site.

## 1. Admin page: "Library links"

**Source:** `GET https://<links hostname>/admin/links.json` with the header `Authorization: Bearer <ADMIN_TOKEN>`.

- Call it **from the site's server side** (the way the existing `/admin/*.json` pages call their sources), with the token held as a site secret. Never put the token in browser JavaScript, in a page, in a URL or in a query string: the Worker does not read query parameters and answers 401 to anything but the header.
- Answers: `200` the report, `401` wrong or missing token (the body says only `unauthorised`), `503` the Worker has no `ADMIN_TOKEN` set. Treat any other result, or a network error, as **"cannot see the links Worker"** and say so on the page. Never render zeroes or an empty table for that, because "no problems" and "could not look" mean opposite things (the same rule as the usage panel).
- The response is not cacheable (`Cache-Control: no-store`); do not cache it.

**Shape (schema version 1):**

```json
{
  "schema_version": 1,
  "generated": "2026-11-13T09:00:00.000Z",
  "registry_version": 1,
  "last_run": "2026-11-13T04:17:03.120Z",
  "last_run_stats": { "rows_checked": 10, "rows_skipped": 0, "requests": 10 },
  "checker_stale": false,
  "rows_ok": 8,
  "rows_needing_a_look": 7,
  "rows": [
    {
      "id": "sdrplay-api",
      "name": "SDRplay API 3.x (service and RSP driver)",
      "publisher": "SDRplay Limited",
      "status": "moved",
      "registry_status": "ok",
      "registry_note": "…",
      "live_status": "moved",
      "failed_runs_in_a_row": 0,
      "never_checked": false,
      "last_check": "2026-11-13T04:17:03.120Z",
      "last_ok": "2026-11-12T04:17:02.000Z",
      "moved_to_host": "example.net",
      "changed": null,
      "changed_seen": null,
      "page_url": "https://www.sdrplay.com/api/",
      "download_url": null,
      "allowed_hosts": ["www.sdrplay.com", "sdrplay.com"],
      "last_seen": { "page": { "o": "foreign-host", "http": 302, "host": "example.net", "size": null, "etag": null }, "download": null },
      "recent_history": [ { "t": "…", "r": "moved", "page": { "…": "…" }, "download": null } ]
    }
  ]
}
```

**What to show:**

1. A banner at the top when `checker_stale` is `true`: "The daily link check has not run since `<last_run>` (or never)." This is the only way anyone learns the cron stopped.
2. A table of `rows`, worst first (`broken`, `moved`, then `changed`, then `unverified`, then the rest), with: name and publisher, `status` as a coloured word (not colour alone), `failed_runs_in_a_row`, `last_check`, and for `moved` the host it went to (`moved_to_host`) in plain text, **not as a link**. A hijacked destination must not be one click away from the admin page.
3. A row's detail: both addresses, `last_seen` (outcome word `o`: `ok`, `http-error`, `network-error`, `timeout`, `too-many-redirects`, `foreign-host`, `rate-limited`; HTTP status; host reached; size; ETag) and `recent_history`.
4. `changed` set means the download's size or ETag differs from what the registry pins: the file may have been replaced, or the pinned checksum is stale. Say that.
5. One line per row of what to do: "Open the publisher's page yourself, then follow UPDATING-A-LINK.md". The page has no edit form and must not get one: links are changed by editing `libraries.json` and deploying.
6. `rows_ok` as a count ("8 links fine"), shown only when the response was received.

`unverified` rows are listed too (they have no address yet): they are work to do, not faults. A site that wants only faults can filter `status` to `moved`, `broken` and rows with `changed`.

## 2. Public links that never die

Every page that names a library the user must install (the manual, the plugin pages, release notes) should link to

`https://<links hostname>/v1/go/<id>`

instead of to the publisher's address. It answers `302` to the publisher's current official page while the link is healthy, and a plain "This link is being updated" page (status 503, no redirect, naming the publisher) while it is not. The ids are the `id` values in `registry/libraries.json`. No cookie is set, no query parameter is read, nothing about the visitor is recorded.

## 3. Optional: a small public status line

`GET https://<links hostname>/v1/status.json` (no secret) returns, per id, only `{ "status": "ok|moved|broken|unverified", "checked": "<time>|null" }` plus `last_run`, `registry_version` and `stale`. The site may use it to hide a link it knows is dead. It is advisory: it can only make a link less offered.

## 4. Things the site must not do

- Cache the admin response, log the token, or expose the admin page without the site's existing admin login.
- Fetch the registry from the browser to render download buttons: the program, not the site, is the consumer of `/v1/libraries.json`.
- Turn this into an editor. The edit path is UPDATING-A-LINK.md.
