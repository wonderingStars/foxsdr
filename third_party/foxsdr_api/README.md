# foxsdr_api.h - the FoxSDR engine API, vendored

`foxsdr_api.h` is copied **unmodified** from the `foxsdr-api` repository,
commit `8fb6522027cb580725fc2707355c5541324fe41e` (API 0.2, draft),
`include/foxsdr_api.h`. SHA-256 of the file:
`c522f2abd8bdaae020816b9f75afff411624ef95a66ec74732eb0192a59e49a1`.

Licence: MIT, in the header's own licence block.

What the application uses it for (engine extraction, stage 1,
`docs/engine-stage1.md`): the `FoxCommand` struct and the `FOXAPI_OP_*`
operations are the ONE vocabulary every change to the receiver is spoken in -
desktop widgets, key bindings, the web remote, CAT and plugins all turn into
commands that `AppWindow::applyCommand` applies. Operations the desktop needs
and API 0.2 does not have yet are app-internal extensions in
`src/core/app_commands.hpp` (`FOXAPP_OP_*`, range 0x8000-0x8FFF); none of them
is added here.

Do not edit this copy. A new API version is vendored by copying the header
again from the commit that defines it and updating the commit and hash above
and in `third_party/THIRD_PARTY.md`.
