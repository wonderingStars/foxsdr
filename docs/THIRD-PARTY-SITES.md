# Hosting a plugin site for the FoxSDR plugin store

This page is for someone who writes a FoxSDR plugin and wants to publish it themselves, on their
own account, without going through the FoxSDR catalogue. A **plugin site** is one `index.json` in
the plugin store's format, plus the files it points at. A FoxSDR user who trusts you pastes the
address of your `index.json` into the store (**SITES**, in the store's top bar) and the store lists,
installs and updates your plugins beside the official ones, marked **THIRD PARTY**.

Users are told, before they add a site and again on each plugin's page, that FoxSDR has **not
reviewed** what you publish and that it runs with the same access as the application. Nothing about
a site makes it safer than that, and FoxSDR does not vouch for you. What FoxSDR does do is hold every
download to the rules below, so that a mistake or a compromise on your side cannot reach further than
the plugin you published.

## What you publish

1. An `index.json`, served over **https**, at an address you give people. A raw file on GitHub
   (`https://raw.githubusercontent.com/<you>/<repo>/<branch>/index.json`) works; so does any web
   server with a valid certificate. The file may be at most **1 MiB**.
2. The plugin binaries and, if you want them, screenshots, served from **the same host** as the
   `index.json` (see *The same-host rule*).

Nothing else is needed: there is no registration, no key and no review step.

## The index

```json
{
  "schemaVersion": 1,
  "plugins": [
    {
      "id": "hdradio",
      "name": "HD Radio",
      "version": "0.1.0",
      "author": "A. Hobbyist",
      "licence": "GPL-3.0-or-later",
      "summary": "One line for the card.",
      "description": "A paragraph for the plugin's page.",
      "homepage": "https://github.com/ahobbyist/hdradio",
      "legalNotice": "Anything the user must read and accept before fitting it.",
      "category": "broadcast",
      "experimental": false,
      "whatsNew": "0.1.0: first release.",
      "published": "2026-10-12",
      "capabilities": ["CASCADE_CAP_IQ_DECODER"],
      "abiVersion": 3,
      "minSupportedVersion": "",
      "screenshots": [
        {
          "url": "https://raw.githubusercontent.com/ahobbyist/hdradio/master/shots/main.png",
          "sha256": "<64 hex digits>",
          "caption": "The decoder locked on a station.",
          "width": 1040,
          "height": 650
        }
      ],
      "platforms": [
        {
          "os": "windows",
          "arch": "x64",
          "file": "hdradio-0.1.0.dll",
          "url": "https://raw.githubusercontent.com/ahobbyist/hdradio/master/bin/hdradio-0.1.0.dll",
          "sha256": "<64 hex digits>",
          "sizeBytes": 212992
        }
      ]
    }
  ]
}
```

The rules are the store's own, applied to your file exactly as to the official catalogue:

- `schemaVersion` must be the integer `1`. A file with another number, a root that is not an object
  or a `plugins` that is not an array is refused whole.
- Every entry needs `id`, `name` and `version` (non-empty strings). A wrong type in any of the fields
  above refuses the **whole index**, not just that entry; a half-understood file is never partly
  believed. The shop-window fields (`category`, `experimental`, `whatsNew`, `published`,
  `capabilities`, `screenshots`) are gentler: a wrong one costs that field or that picture, and the
  store's log says which.
- `abiVersion` must equal the plugin ABI of the FoxSDR build exactly (see
  [PLUGIN-API.md](PLUGIN-API.md)). An entry for another ABI is listed but cannot be fitted, and
  says why.
- `licence` is required to fit: an entry that states none cannot be installed, because a plugin with
  no declared licence is not loaded either.
- Each `platforms` record needs `os` (`windows`, `linux`, `macos`), `arch` (`x64`, `arm64`, `x86`),
  `file`, `url` and `sha256`. `url` **must be https**. `sha256` is **mandatory**: exactly 64
  hexadecimal digits, the digest of the file at `url`. A download that does not hash to it is
  deleted and reported with both digests. `sizeBytes` is shown to the user and is never trusted for
  anything else; the store reads at most 64 MiB of a plugin whatever it says.
- `file` is a bare file name of letters, digits, `.`, `_` and `-`, starting with a letter or a
  digit, ending in the platform's module extension (`.dll` on Windows), at most 128 characters, and
  not a Windows device name. Anything else is refused at install time.
- A picture needs `url` (https), `sha256` and `caption`; at most **8** per plugin, **4 MiB** each,
  and a PNG.
- `minSupportedVersion`, if you set it, retires installed copies older than that version (they are
  disabled, not deleted). Leave it empty unless you mean it.
- `legalNotice` is shown to the user verbatim, after FoxSDR's own sentence that the plugin comes from
  your site and has not been reviewed, and the user must tick that they have read it before the key
  works. Put the things a user must read there: a licence that places an obligation on them, a note
  about what the decoder may be unlawful to receive where they live, patent or export notes.

## The same-host rule

**Every plugin `url` and every picture `url` must be on the same host as the address of your
`index.json`** - the same name and the same port. The store checks this when it reads your index and
**drops the whole entry** that breaks it (the site's status line then says "N hidden: files not on the
site's own host"), and it checks it again before it downloads anything.

So a site at `https://raw.githubusercontent.com/...` serves its binaries from
`https://raw.githubusercontent.com/...`, and a site at `https://plugins.example.org/index.json`
serves them from `https://plugins.example.org/...`. A GitHub **release asset** URL does not qualify:
it redirects to `objects.githubusercontent.com`, a different host, and the store never follows a
redirect to another host (a redirect to the same host is followed, a few hops at most). Commit the
binaries to the repository and link to the raw file, or serve them from your own server.

The host is the part between `https://` and the first `/`, lower-cased; a different port is a
different host, `host.example` and `cdn.host.example` are different hosts, and an address with a
`user@` part is refused.

## Ids that do not clash

Choose an `id` that is yours: no plugin in the FoxSDR catalogue, and no plugin of a site the user
added before yours, may have the same one. If it does, **your entry is hidden** ("N hidden: same id
as a plugin already listed") - the one already listed wins, always, and nothing you publish can
replace, update or retire it. Inside the store your plugin is known by its site as well as its id,
so installing yours never touches anybody else's, and the update check offers your update only to
people who installed it from your site.

Choose a **declared name** that is yours too. This is the `name` inside the plugin binary's own
descriptor (what FoxSDR's loader reads from the module itself), not the `name` in your index. The
loader keeps one loaded module per declared name, and normally the higher version wins; but a
module that was installed from a site **never takes a declared name from an official plugin**,
whatever the versions. If your binary declares the same name as a plugin in the FoxSDR catalogue,
the official plugin is the one that runs and yours is listed in the Fitted modules window as not
loaded, with the reason: "Not loaded: it declares the same name as the official plugin ...". Two
modules from sites that declare one name are settled by version as before, and nothing is deleted
in either case.

Choose a **file name** that is yours as well. A plugin from a site is never allowed to overwrite a
file in the user's plugins folder that belongs to another plugin, or that was put there by hand; if
your `file` collides, the install is refused with that reason.

## What a user sees

- Your plugins appear with a **THIRD PARTY** badge, "from `<your host>`" under the name, and a
  **BEFORE YOU FIT IT** box with the tick, always - even if you wrote no `legalNotice`.
- **GET EVERYTHING** and **UPDATE ALL** do not include your plugins; the user fits each from its
  page and updates each with its own key. Nothing updates itself.
- If your site cannot be read (down, not https, not a catalogue, over 1 MiB) the store says so in
  amber under its top bar, with the reason, and carries on with everything else. The last good copy
  of your index stays listed until it answers again.
- A site is contacted when the user opens the store (the first time in a session) or presses CHECK
  NOW - never at startup - with a few seconds to answer. The request is a plain GET of your
  `index.json`: it carries no identifier and no cookie, and your server sees the user's IP address
  as any server does.

## Checking your index before you publish it

- Open it in a JSON validator; the store refuses invalid JSON outright.
- `certutil -hashfile <file> SHA256` (Windows) or `sha256sum <file>` (Linux) gives the digest to
  paste. It must be the digest of exactly the bytes served at `url`.
- Add the address to a copy of FoxSDR (**SITES**, then tick the box and **ADD**) and read the status
  line under the top bar: "`<host>`: N plugins" is a clean read; anything amber says what was dropped
  and why. A local file works too (give its path instead of an https address) while you are
  developing; a local index has no host to lock to, which is why it is for your own testing only.
- The store's log (Diagnostics) carries a short note for each thing it dropped from a picture or an
  optional field, naming the plugin and the reason.

## What this does not give you

It does not make your plugin trusted, reviewed or sandboxed, and it does not let you reach a user
who has not added your site. A fitted plugin is native code in the FoxSDR process with every
privilege the application has; the store proves that the bytes the user fitted are the bytes your
index named, from your host, and nothing more.
