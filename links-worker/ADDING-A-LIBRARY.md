# Adding a library to the registry

Other lanes write a "NEEDED LIBRARIES" table in their feature record. Each row of such a table becomes one row of `registry/libraries.json`. It takes a few minutes; the validator tells you what is wrong.

## 1. Pick the id

Lower-case words joined by hyphens, at most 40 characters: `ftdi-d2xx`, `mbelib`, `codec2`, `vc-runtime-x64`. **Two lanes that need the same library use the same id.** Look in the registry first (`id` fields), then in the other lanes' tables (`D:/foxsdr-build/prep/lane-results/`). Never rename an id that has shipped: the program and the manual find the library by it.

## 2. Map the table to the row

| Table column in the record | Registry field | Notes |
|---|---|---|
| library name | `name` | what the user will read |
| publisher | `publisher` | the publisher's name; shown to the user when the link is out of date |
| file names looked for | `files` | plain file names, no folders (`["sdrplay_api.dll"]`); `[]` for something run by hand |
| minimum version | `min_version` | `"3.0"` or `null` |
| official page | `page_url` | `https://`, the publisher's own page; `null` while unverified |
| (a direct file, if the licence allows fetching) | `download` | `{ "url": ..., "size_bytes": ..., "sha256": ... }`, `null` when none; each of the three may be `null` when unknown |
| licence | `licence` | name, and where it was read |
| `fetch allowed` | `fetch_allowed` | `yes`, `no (patent)`, `no (terms)`, `no (unverified)` |
| the evidence in that row | `fetch_evidence` | for `yes`: the licence name, the sentence relied on and its address, at least 20 characters |
| install kind | `install_kind` | `installer`, `copy-in`, `helper-program` |
| (hosts the page and file are served from) | `allowed_hosts` | every host the address and its redirects use; lower-case host names; the host of `page_url` and `download.url` must be listed |
| (today's knowledge) | `status` | `ok` only if the address came from the publisher's own documentation that a lane actually read; else `unverified` |
| | `last_checked` | the day somebody opened the address, else `null` |
| | `last_changed` | today |

## 3. The two rules that matter

- **An address nobody verified is not written.** Use `"status": "unverified"`, `"page_url": null`, `"download": null`, `"allowed_hosts": []`. The validator rejects an `unverified` row that has an address, and FoxSDR shows users only the written steps for it. The address is added later, in a separate edit, by someone who opened the publisher's page.
- **`fetch_allowed: "yes"` is rare.** It needs a `download`, status `ok`, an installer or a copy-in file (not a helper program), and the licence sentence quoted in `fetch_evidence`. The orchestrator lists every `yes` row in the release checklist for the owner. Anything with a patent, an agreement, an account or a payment is a `no`. The program additionally refuses to fetch unless the download host is in a list compiled into the program for that id (CLIENT-CONTRACT.md), so a `yes` row also needs the program's lane to add that host.

## 4. A template (unverified)

```json
{
  "id": "example-lib",
  "name": "Example library (the file the plugin loads)",
  "publisher": "unverified",
  "files": ["example.dll"],
  "min_version": null,
  "page_url": null,
  "download": null,
  "licence": "unverified",
  "fetch_allowed": "no (unverified)",
  "fetch_evidence": "No official address has been verified.",
  "install_kind": "copy-in",
  "allowed_hosts": [],
  "status": "unverified",
  "status_note": "Added for the example plugin lane.",
  "last_checked": null,
  "last_changed": "2026-11-12"
}
```

## 5. Finish

1. Raise `registry_version` by one and set `updated`.
2. `node tools/validate.mjs`, then `node --test "test/*.test.mjs"`.
3. Rebuild the bundle (`node tools/build-bundle.mjs`) so the committed `src/registry-bundle.js` matches (a test fails if it does not).
4. Tell the program's lane the new id, so its built-in copy and its host list for the id are updated in the same change.

Nothing else is needed: the daily checker, `/v1/go/<id>`, `/v1/status.json` and the admin list pick the new row up by themselves.
