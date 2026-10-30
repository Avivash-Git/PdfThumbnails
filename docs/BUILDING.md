# Building PdfThumbnails

PdfThumbnails builds with MSVC, CMake and Ninja through one entry point, `scripts\build.cmd`.
CI runs the same script, so a local build is what CI builds.

## Prerequisites

- **Windows 10 or 11**, x64 or ARM64.
- **Visual Studio 2026, version 18.11 or later.** The free Build Tools are enough.
  Install the components listed in `.vsconfig`:
  - the C++ build tools for x64 and ARM64;
  - the Spectre-mitigated runtime libraries for x64 and ARM64;
  - the Windows 11 SDK 10.0.26100;
  - the C++ CMake tools, which bring CMake and Ninja.

  To add them to an existing installation, run this from the repository root (it asks for
  administrator rights):

  ```bat
  "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe" modify ^
    --installPath "<Visual Studio folder>" --config "%CD%\.vsconfig" --passive
  ```

  The build needs CMake 4.3 or later; Visual Studio 2026 18.11 brings CMake 4.3.1. A CMake
  installed separately and already on `PATH` is used instead of Visual Studio's, so it must be
  4.3 or later too. Visual Studio 2022 is not supported.
- **Git.**
- **Internet access for the first build.** CMake downloads PDFium, GoogleTest and WIL from the
  URLs pinned in `cmake/Pins.cmake` and checks every file's SHA-256 (see
  [Downloads](#downloads)).
- Optional: **clang-format 23.x** for formatting, and the **GitHub CLI** (`gh`) for checking the
  provenance of a new PDFium release.

Keep the clone inside your user profile, for example `%USERPROFILE%\src\PdfThumbnails`. Don't
create it directly under `C:\`: new folders there are writable by every local account, and
development installs run the built files elevated. Paths with spaces work.

## Build

```bat
scripts\build.cmd x64-release
```

The script:
1. finds Visual Studio with `vswhere` (the newest 18.11 or later with the needed components);
2. sets up the compiler environment with `vcvarsall` (x64, or x64-to-ARM64 for `arm64` presets);
3. writes the toolchain versions to `out\build\<preset>\buildinfo.txt` and prints them;
4. runs `cmake --workflow --preset <preset>`: configure, build, then test.

It returns 0 when everything built and every test passed. Run it from a plain `cmd` prompt; no
developer prompt is needed.

To use a particular Visual Studio when several are installed, set `PDFT_VS_VERSION` to a
`vswhere` version range first, for example `set PDFT_VS_VERSION=[18.11,19.0)` to stay on
Visual Studio 2026 once a newer one is installed too.

Output: `out\build\<preset>\bin\` holds the handler (`PdfThumbnails.dll`), its PDFium copy
(`PdfThumbnails-pdfium.dll`), the PDBs, the tools and the test programs.

### Presets

| Preset | Builds | Tests it runs |
|---|---|---|
| `x64-release` | x64 Release | everything except the `system` and `e2e` labels |
| `x64-debug` | x64 Debug | the same as `x64-release` |
| `arm64-release` | ARM64 Release, cross-compiled | `gate` only (ARM64 programs cannot run on an x64 host) |
| `ci-x64` | x64 Release as CI builds it: Spectre mitigations required, deterministic output (`/Brepro`) | the same as `x64-release` |
| `ci-arm64` | ARM64 Release as CI builds it | `gate` only |

All presets use Ninja, `cl.exe` and the static C runtime, and treat compiler and linker warnings
as errors in the project's own code. Each preset has its own folder, `out\build\<preset>`. Don't
run two builds of the same preset at the same time.

### Working in a developer prompt

From an "x64 Native Tools Command Prompt" (or "x64_arm64 Cross Tools" for ARM64), the presets can
be used step by step:

```bat
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release -L "unit|handler"
```

### Build options

Set these with `-D<name>=<value>` on `cmake --preset`, or in a `CMakeUserPresets.json` (ignored
by Git):

| Option | Default | Meaning |
|---|---|---|
| `PDFT_SPECTRE` | `AUTO` (`ON` in `ci-*`) | `/Qspectre` and the Spectre-mitigated runtime. `AUTO` uses them when installed and warns otherwise; `ON` fails without them; `OFF` never uses them. |
| `PDFT_REPRODUCIBLE` | `OFF` (`ON` in `ci-*`) | `/Brepro`: no time stamps, so a rebuild of the same source gives the same bytes. |
| `PDFT_WARNINGS_AS_ERRORS` | `ON` | `/WX` for the compiler and the linker, on the project's own targets only. |
| `PDFT_VERSION_SUFFIX` | empty | Appended to the ProductVersion string only (CI uses `-ci+g<commit>`). The file version stays `2.0.0.0`. The presets take it from the `PDFT_VERSION_SUFFIX` environment variable. |
| `PDFT_DOWNLOAD_DIR` | `.cache\downloads` | Where verified downloads are kept. |
| `PDFT_PDFIUM_ARCHIVE`, `PDFT_GTEST_ARCHIVE` | empty | Offline builds: a local copy of the archive. It must match the pinned SHA-256. |
| `PDFT_BUILD_TOOLS` | `ON` | Builds `thumbtest`. |
| `BUILD_TESTING` | `ON` | Builds the tests and registers the gates. |

### Downloads

`cmake/Pins.cmake` lists every download with its URL and SHA-256: PDFium (per architecture, plus
the SHA-256 of the `pdfium.dll` inside each archive), GoogleTest and WIL. Files are stored in
`.cache\downloads` as `<sha256>-<name>`. A download, or a cached file, that doesn't match its pin
stops the build. Nothing is fetched without a pin, and no binaries are committed.

## Tests

`scripts\build.cmd` runs the tests of its preset. To run a subset, use `ctest` with a label:

```bat
ctest --preset x64-release -L gate
ctest --preset x64-release -L "unit|handler"
```

| Label | What | Where it runs |
|---|---|---|
| `gate` | Static checks of the staged files: exports, imports (static C runtime, Windows DLLs only), hardening flags, no build paths inside the binaries, version resources, the 18 PDFium functions the handler binds, the PDFium hash, license files and notices, and the build id | every preset, both architectures |
| `unit` | The test-PDF generator (byte-identical to the 1.x generator), Medium-integrity helpers, temporary folders, and the committed preview samples (`src\app\res`) | x64 presets; CI's ARM64 runner |
| `handler` | `PdfThumbnails.dll` loaded in-process at Medium integrity: the crop table, pixels, the COM surface, error codes, an 8-thread stress test. Elevated, the program relaunches itself at Medium; `handler.refuses_elevated` checks that the DLL refuses to render when elevated and is skipped when not | x64 presets; CI's ARM64 runner |
| `integration`, `system`, `e2e` | Added with the settings app. `system` and `e2e` change real machine state and never run in a developer build. | — |

Useful environment variables for the handler tests:
- `PDFT_TEST_HANDLER_DLL=<path>` tests another build of the handler (for example an older one).
- `PDFT_KEEP_TEMP=1` keeps the temporary folders under `%TEMP%\pdft-tests`.
- `PDFT_STRESS_FULL=1` also runs the stress test over all 18 test PDFs, including the two slow ones.

On a PC with Smart App Control on, Windows sometimes blocks a freshly linked test program the
first time it starts (CTest reports "Not Run"). Run the tests again.

### Tools

In `out\build\<preset>\bin\`:
- `thumbtest makepdfs <dir>` writes the 18 synthetic edge-case PDFs; `makesamples <dir>` writes the
  settings app's two preview samples.
- `thumbtest direct|stress ...` renders through the DLL directly; `cache|cachenew|cacheinproc|shell
  ...` goes through Explorer's thumbnail pipeline; `compare <a> <b>` compares PNG files or
  folders pixel by pixel.
- `pdft-run-medium [--] <program> [arguments]` runs a program at Medium integrity with a standard
  user's rights; `pdft-run-medium --check-token` prints the current token.

## Staging (install components)

```bat
cmake --install out\build\x64-release --prefix <folder> --component runtime
```

| Component | Contents |
|---|---|
| `runtime` | What gets installed: the two DLLs, `LICENSE.txt`, `THIRD-PARTY-NOTICES.md` and `licenses\` |
| `symbols` | The PDBs of the `runtime` binaries |
| `tests` | A self-contained test bundle: the test programs and tools with their PDBs, the runtime, the notices, the preview samples and a `CTestTestfile.cmake` with relative paths |

The test bundle runs anywhere CMake is installed:

```bat
ctest --test-dir <folder> -L "unit|handler" --output-on-failure
```

## Formatting

The C++ code follows `.clang-format` (Google style, 100 columns). Any clang-format 23.x gives
the same result:

```bat
git ls-files "*.cpp" "*.h" "*.inc" | xargs clang-format --dry-run --Werror
```

Use `-i` instead of `--dry-run --Werror` to reformat. CI pins clang-format 23.1.3 by hash in
`.github/tools/requirements-format.txt`.

## Continuous integration

GitHub Actions runs three workflows. Every action is pinned to a full commit SHA, every
workflow starts with no token permissions and grants each job only what it needs, and checkouts
don't keep the token.

### `build.yml`

| Job | Runner | When | What |
|---|---|---|---|
| `build (ci-x64)` | x64 | always | `scripts\build.cmd ci-x64`: gates, unit and handler tests; artifacts |
| `build (ci-arm64)` | x64 | always | `scripts\build.cmd ci-arm64` (cross-compiled): gates; artifacts |
| `build (x64-debug)` | x64 | not on pull requests, unless called | `scripts\build.cmd x64-debug` |
| `test-arm64` | ARM64 | not on pull requests, unless called | the ARM64 test bundle, run natively with `ctest --test-dir` |

It runs on pushes to `main` and on pull requests (except when only Markdown files or `docs\`
changed), by hand (`workflow_dispatch`), and when another workflow calls it (`workflow_call`;
for the later release workflows). A called run builds and tests everything, also when the
calling workflow was started by a pull request. A new push to a pull request cancels its older
run. `test-arm64` uses only the `tests-arm64` artifact, so it still runs when `build (ci-x64)`
or `build (x64-debug)` fails; it fails if `build (ci-arm64)` made no artifact.

Details:
- **Runners.** GitHub-hosted runners run elevated (High integrity). The handler tests relaunch
  themselves at Medium integrity, and a check step fails the job unless the test log shows that
  relaunch and an elevated run of `handler.refuses_elevated`. The job log also shows the runner
  image, the UAC settings and `whoami /groups`.
- **Download cache.** `.cache\downloads` is cached per architecture, keyed on
  `cmake/Pins.cmake`. When the cache misses (a new pin), the job first runs
  `cmake -DARCH=<arch> -DVERIFY_ATTESTATION=ON -P cmake/FetchPdfium.cmake`, which downloads the
  PDFium archive and checks its GitHub artifact attestation: the archive must come from
  pdfium-binaries' own release workflow. Only then does the build extract or load it.
- **Artifacts** (unsigned developer builds, not releases):

  | Artifact | Contents | Kept |
  |---|---|---|
  | `PdfThumbnails-x64-unsigned`, `PdfThumbnails-arm64-unsigned` | the `runtime` component | 14 days |
  | `symbols-x64`, `symbols-arm64` | the `symbols` component | 14 days |
  | `tests-x64`, `tests-arm64` | the `tests` component (`test-arm64` downloads it) | 3 days |
  | `logs-<job>` | test logs, configure log and `buildinfo.txt`, only when a job fails | 7 days |

  Each artifact contains `BUILDINFO.txt`: the commit, the runner image, and the Visual Studio,
  MSVC, `cl`, `link`, CMake and Ninja versions.

Repository variables (Settings > Secrets and variables > Actions > Variables), all optional:

| Variable | Default | Use |
|---|---|---|
| `PDFT_WIN_X64_RUNNER` | `windows-2025` | The x64 runner label. `windows-2025` has Visual Studio 2026 (MSVC v145), private repositories included; the build uses the image's own CMake 4.x, which comes first on `PATH`. |
| `PDFT_VS_VERSION` | empty (newest 18.11 or later) | Passed to `scripts\build.cmd`, e.g. `[18.11,19.0)` to stay on Visual Studio 2026. |
| `PDFT_ARM64_RUNNER` | `windows-11-arm` | The ARM64 runner label (`windows-11-vs2026-arm` works too; the job needs only CTest). |
| `PDFT_ARM64_TESTS` | on | Set to `off` to skip `test-arm64`, for example when the account's plan has no ARM64 runners. |

### `format.yml`

On every push to `main` and every pull request, on `ubuntu-24.04`:
- installs clang-format 23.1.3 into a virtual environment, with
  `pip install --require-hashes --only-binary=:all:`, and runs the formatting check above;
- fails if a tracked file contains a personal Windows or Linux profile path, a user SID, a
  default PC name or a personal mail address. Placeholders such as `%USERPROFILE%` are fine.

### `pdfium-watch.yml`

Every Monday, it compares the PDFium release pinned in `cmake/Pins.cmake` with the latest
pdfium-binaries release. While they differ, it keeps one issue open with the label
`security/pdfium`.

### Dependabot

`.github/dependabot.yml` updates the pinned actions once a week, in one grouped pull request,
after a 7-day cooldown.

### Repository settings

- Actions > General: allow only actions pinned to a full-length commit SHA; default
  `GITHUB_TOKEN` permissions "Read repository contents"; don't let GitHub Actions create or
  approve pull requests.
- If a branch ruleset ever requires the `build` checks, note that pull requests that change only
  Markdown or `docs\` don't run `build.yml`, so those checks would never report.
- Private repositories have a monthly budget of Actions minutes, and Windows minutes cost more
  than Linux ones. A pull request costs two Windows builds; a push to `main` costs three
  builds plus the ARM64 test job.

## Updating PDFium

Each PDFium update is a pull request of its own:
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
