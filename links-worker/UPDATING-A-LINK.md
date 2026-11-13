# Updating a link (in plain words)

A link changes when the admin list shows a row as **moved** or **broken**, or when you hear that a publisher has a new download page. There is no web form for this on purpose: a link is changed by editing one file, checking it, and deploying, so every change leaves a trace and nobody can steer FoxSDR's links from a web page.

**The checker never changes an address by itself.** If a publisher's site starts sending people somewhere else (a sold domain, a hijack), the checker only marks the row `moved`, FoxSDR stops offering that link, and you decide.

## What the statuses mean

- **ok**: the page answered normally at the address in the file.
- **moved**: the address redirected to a site that is not in the row's `allowed_hosts`. Open the address yourself first; this is either a real move (good) or something to be careful of (a domain that changed hands).
- **broken**: three daily checks in a row failed (not found, an error, or no answer).
- **unverified**: nobody has confirmed the publisher's address yet; the row has none, and FoxSDR shows only the written steps.

A new address starts clean: the day you deploy it, the old `moved` or `broken` mark no longer applies to it, and the next daily check looks at the new address.

## Steps

1. **Find the row.** Open `links-worker/registry/libraries.json` and find the `id` shown in the admin list.
2. **Confirm the new address yourself.** Open the publisher's own site in a browser and find the download or documentation page. It must be the publisher's own site (not a mirror, not a forum post). If the library has a one-click download (`"fetch_allowed": "yes"`), also reconfirm the licence sentence quoted in `fetch_evidence` on the new page.
3. **Edit the row:**
   - `page_url`: the new address (starts with `https://`).
   - `download.url`, `download.size_bytes`, `download.sha256`, if the row has a download: the publisher's new file and its published checksum. Leave `sha256` as `null` when the publisher publishes none.
   - `allowed_hosts`: every host name the old and the new address use, including any host the page redirects to (for example `github.com` pages that send you to a release-file host). Host names only, lower case.
   - `status`: set to `"ok"` once you have confirmed it yourself. If the library is still being sorted out, leave `"moved"` or `"broken"`, and FoxSDR keeps saying the link may be out of date.
   - `status_note`: one line saying what you changed and why (optional, shown only to you).
   - `last_checked`: today's date, the day you looked yourself. `last_changed`: today's date.
4. **Raise the numbers at the top:** `registry_version` goes up by exactly one, `updated` is today. The program refuses a registry whose number is lower than the one it already holds, so skipping this means installed copies ignore your change.
5. **Check the file:** `node tools/validate.mjs`. It prints every problem it finds and changes nothing.
6. **Sign it** (the owner's step, DEPLOY.md step 5), then **build and deploy**: `node tools/build-bundle.mjs`, `node tools/predeploy.mjs`, `npx wrangler deploy`.
7. **Look at the result:** `https://<hostname>/v1/go/<id>` should now send you to the new page, and the library's row should be gone from the admin list after the next daily check (up to a day).

## If you are not sure

Leave the row as `moved` (or `broken`). FoxSDR then tells the user "this link may be out of date", names the publisher, and shows the written steps. That is always safe; a wrong address that looks right is not.

To retire a library, set `status` to `unverified`, set `page_url` and `download` to `null` and `allowed_hosts` to `[]`. To delete a row completely, remove it and raise `registry_version`; a plugin that still names that id will show the written steps only.
