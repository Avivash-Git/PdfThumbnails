# Maintaining PdfThumbnails

## Before every release

1. CI is green on the commit, including the window tests in `build (ci-x64)` and in `system-x64`.
   `system-x64` is allowed to fail without failing the run, so read its steps.
2. [The whole pass before a release](TESTING.md#the-whole-pass-before-a-release) and
   [the installer's checks](TESTING.md#the-installer) pass on a real PC, with the setup from the
   commit's CI run (`installer-x64` green; `installer-arm64` read, since it may fail without
   failing the run).
3. `CHANGELOG.md` says what changed.
4. The version is raised in `cmake/ProjectInfo.cmake` (`PDFT_VERSION`). Build folders are named after it and the build id.

## Making a release

Releases are made by `.github/workflows/release.yml`, started by hand, and are published by a
person. The workflow never publishes.

1. On `main`: `CHANGELOG.md` has the heading `## <version> (YYYY-MM-DD)` with the release day,
   and `docs/releases/<version>.md` holds the release notes (the workflow adds the setup's SHA-256
   and how to check it). Push, and wait for CI to be green.
2. Run the workflow on `main` with the tag: `gh workflow run release.yml --ref main -f tag=v<version>`.
   It checks the tag against `PDFT_VERSION`, runs the whole of `build.yml` with `release: true`
   (a version string without the `-ci+g<commit>` suffix), attests the setup's provenance, and
   makes a **draft** release targeting the built commit, with the setup and its `.sha256`.
   A tag such as `v<version>-rc.1` makes a draft prerelease instead, which is also how to rehearse
   (delete the draft afterwards: `gh release delete <tag> --yes`).
3. Read the draft, download its setup and run the installer's checks on a real PC.
4. Tag the commit the draft names (`git tag -a v<version> <commit> -m "PdfThumbnails <version>"`,
   `git push origin v<version>`), then publish the draft (`gh release edit v<version> --draft=false --latest`).

Pushing the tag starts no workflow. Until releases are signed, run it with `sign` off; with the
signing set-up of [CODE_SIGNING.md](CODE_SIGNING.md), `sign` on attaches the signed setup.

## Updating PDFium

`pdfium-watch.yml` opens an issue when a new PDFium build appears. Each update is a change of its
own:

1. In `cmake/Pins.cmake`, update the tag, both archive URLs and SHA-256s, and both inner
   `pdfium.dll` SHA-256s (`cmake -E sha256sum` on `bin\pdfium.dll` in each extracted archive).
2. Check the provenance of both archives:
   `cmake -DARCH=x64 -DVERIFY_ATTESTATION=ON -P cmake/FetchPdfium.cmake` and the same with
   `-DARCH=arm64` (needs `gh`, signed in).
3. Build and test `x64-release` and `arm64-release`. The `gate.pdfium.binds` gate checks that
   the new DLL still exports every function the handler uses.
4. Compare thumbnails of the test PDFs with the previous build
   (`thumbtest makepdfs`, `thumbtest direct` with both DLLs, then `thumbtest compare`).
5. If the archive's license files changed, update `THIRD-PARTY-NOTICES.md`; `gate.licenses`
   names any file that is missing from it.

A security problem in PDFium is the usual reason to update out of turn, since the handler reads
PDFs that may come from anyone.

## Keeping names in one place

Every product name, file name, registry key and GUID comes from `cmake/ProjectInfo.cmake`. The
names under "1.x names" there are fixed forever, because migration finds 1.x through them. The
handler's class ID and the task names must not change between versions either.

## Commits

- Every commit date is in UTC (`TZ=UTC git commit ...`), and author and committer use the GitHub
  no-reply address, so that neither a time zone nor an email address is published.
- Nothing private goes into tracked files or the history: no personal names, profile paths or
  notes. Check a change before pushing it, because the history is public.
- Comments say why in plain words, and never point at a private plan.
- Format C++ with clang-format 23.x; `format.yml` fails otherwise.

## CI

Every action is pinned to a full commit SHA, and Dependabot proposes updates. In a private
repository the Actions minutes are limited, and Windows minutes count for more, so push changes in
batches. Standard runners are free for public repositories.

The `gui` tests start the real settings window and change the runner's real settings, so they run
only on runners GitHub hosts. The `system` tests install and uninstall for real, and only run on a
throw-away machine.

## Security reports

Reports arrive through GitHub's private vulnerability reporting (see [SECURITY.md](../SECURITY.md)).
It has to be switched on in the repository settings, and works only for a public repository.
