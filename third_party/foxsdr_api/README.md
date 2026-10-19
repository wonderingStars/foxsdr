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
SHA-256 of the 0.3 header: `4a6faf8a4ac3d84b33fa41bc15d9ffa6620fb20752a8077a2fe5397b9cff4c3c`.

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
