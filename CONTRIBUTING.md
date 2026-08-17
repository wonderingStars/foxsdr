# Contributing to FoxSDR

Contributions are welcome. Please read this page before opening a pull request
— there is one condition, and it is not negotiable, because the project's
funding model depends on it.

## Licensing of contributions — read this first

FoxSDR is dual-licensed: free for noncommercial use under the
[PolyForm Noncommercial License 1.0.0](LICENSE), and available commercially
under a paid licence (see [COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md)).
Selling those commercial licences is what pays for the work, and it is what
keeps the software free for everyone else.

That only works if a single party holds the rights to the whole codebase. If
somebody else owns the copyright in even one file, no commercial licence can
lawfully be granted over it — and the free-for-hobbyists arrangement stops
being fundable.

**So: by submitting a contribution, you grant Steven Fairclough a perpetual,
irrevocable, worldwide, royalty-free, non-exclusive licence to use, reproduce,
modify, distribute, license and sub-license your contribution, in source and
object form, under any licence terms whatsoever — including the commercial
licences described above — and to do so without any obligation of attribution,
accounting or payment to you.**

You keep the copyright in your own work. You are not signing it away, and you
remain free to use your contribution however you like elsewhere. You are simply
granting a licence broad enough that the dual-licensing model keeps working.

You must also confirm, by submitting, that:

- you wrote the contribution yourself, or otherwise have the right to grant the
  licence above;
- if you wrote it in the course of employment, your employer has waived its
  rights in it or has authorised you to submit it; and
- the contribution contains no code taken from a GPL, LGPL, AGPL or other
  copyleft-licensed project, and no code you are not entitled to relicense.

That last point is not a formality. The entire codebase is clean-room and
deliberately free of copyleft dependencies — that discipline is the only reason
the licensing choice exists at all. A single GPL-derived function would
contaminate it. If you are adapting an algorithm from a paper, a textbook or a
copyleft project, say so explicitly in the pull request and describe what you
took, so it can be assessed before it lands.

Sign off each commit to confirm all of the above:

```bash
git commit -s -m "your message"
```

## Before you open a pull request

- **Open an issue first for anything substantial.** A rejected PR wastes your
  evening, and there is a roadmap ([PLAN.md](PLAN.md)) that not everything fits.
- **Match the surrounding code.** Comment density, naming, and idiom should read
  as though the existing author wrote it.
- **Every change carries tests.** A bug fix needs a test that fails before the
  fix and passes after. New behaviour needs the happy path, the failure path,
  and the boundary that motivated it. Put them alongside the existing suites in
  `tests/`.
- **Run the full suite** for the code you touched, and paste the real output in
  the pull request:

```bash
ctest --test-dir build -C Release --output-on-failure
```

- **No new dependencies without discussion.** Everything vendored under
  `third_party/` is at a pinned revision under a permissive licence, and it
  stays that way. FFTW and librtlsdr are excluded on purpose.
- **Bump the version** in `CMakeLists.txt` in the same change, and update the
  README and any affected documentation in the same commit as the code.

## Plugins are a different matter

If you want to write a decoder or another extension, you do **not** need to
contribute to this repository and none of the above applies to you. The plugin
interface (`src/core/plugin_abi.h`) is MIT-licensed precisely so that anyone —
including commercial vendors — can build against it and ship under whatever
terms they choose, with no licence from us and no permission required. See the
plugin catalogue documentation for how to publish one.

## Reporting security issues

Do not open a public issue. Email <steven.fairclough@icloud.com> with the
details and allow a reasonable period for a fix before disclosing.

## Contact

Steven Fairclough — <steven.fairclough@icloud.com>
