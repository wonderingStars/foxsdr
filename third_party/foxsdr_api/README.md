# foxsdr_api.h - the FoxSDR engine API, vendored

`foxsdr_api.h` is **API 0.3 (draft)**: the header copied **unmodified** from
the `foxsdr-api` repository, commit `8fb6522027cb580725fc2707355c5541324fe41e`
(API 0.2, `include/foxsdr_api.h`, SHA-256
`c522f2abd8bdaae020816b9f75afff411624ef95a66ec74732eb0192a59e49a1`), then
EXTENDED in this tree to 0.3 by the owner's decision of 2026-09-29
(docs/engine-stage4.md section 3, item 3 - "extend the API draft for what the
table has no read for, in a new vendored header version"). Every 0.3 change
is APPENDED, per the header's own rule 4, and each is documented in
`docs/api-0.3-additions.md` in API.md's style, to be merged upstream; until
that commit exists, this file and that document are the 0.3 contract.
SHA-256 of the 0.3 header: `7cb5dc0a3b08650c9fad06cffb19e1a958c6aa256eaa51d513ffa36d716cb3f0`.
(Refreshed at stage 5a, which added `FOXAPI_LIST_SERVERS` (49, docs/api-0.3-additions.md 8.16.5) - the only change; the hash is of the file with LF line ends, as git stores it. The value recorded before it, `f60701c2...`, had not been refreshed since the pump move's additions and matched no committed state of the header.)
(Refreshed by stub campaign B3, which added `FOXAPI_LIST_STORE_PICTURES` (50, docs/api-0.3-additions.md 5.1 and 9.4). The value recorded before it, `91c101de...`, was stale: the header had changed since stage 5a (one line in the 0.99.84 merge's second pass, B2's) without the hash being refreshed. This hash is again that of the file as git stores it, LF line ends.)
(Refreshed by stub campaign B6, which added `FOXAPI_LIST_AIRBAND_CHANNELS` (51), `FOXAPI_ITEM_TICKED` and `FOXAPI_ITEM_ON_AIR` and the `BOOKMARKS` `value[3]` word (docs/api-0.3-additions.md 5.1 - 5.3 and 9.6). The value recorded before it, `fe98c053...`, was B3's. The hash is again that of the file as git stores it, LF line ends.)
(Refreshed by stub campaign B9, which added `FOXAPI_LIST_PLUGIN_SETTINGS` (52, docs/api-0.3-additions.md 5.1 and 9.7). The value recorded before it, `c5e97f86...`, was B6's. The hash is again that of the file as git stores it, LF line ends.)
(Refreshed by stub campaign B10, which added `FOXAPI_LIST_FREQ_MARKERS` (53, docs/api-0.3-additions.md 5.1 and 9.8). The value recorded before it, `7f77c6d4...`, was B9's. The hash is again that of the file as git stores it, LF line ends.)

Licence: MIT, in the header's own licence block.

What the application uses it for (engine extraction, stage 1,
`docs/engine-stage1.md`): the `FoxCommand` struct and the `FOXAPI_OP_*`
operations are the ONE vocabulary every change to the receiver is spoken in -
desktop widgets, key bindings, the web remote, CAT and plugins all turn into
commands that `AppWindow::applyCommand` applies. Operations the desktop needs
and API 0.2 does not have yet are app-internal extensions in
`src/core/app_commands.hpp` (`FOXAPP_OP_*`, range 0x8000-0x8FFF); none of them
is added here.

Do not edit this copy except as a new API version: once 0.3 lands upstream,
the header is copied again from the commit that defines it, and the commit
and hash above and in `third_party/THIRD_PARTY.md` are updated.
