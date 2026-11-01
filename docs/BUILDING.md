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

Output: `out\build\<preset>\bin\` holds the settings app (`PdfThumbnailsSettings.exe` for the
window, `PdfThumbnailsCli.exe` for terminals and scripts), the handler (`PdfThumbnails.dll`),
its PDFium copy (`PdfThumbnails-pdfium.dll`), the PDBs, the [tools](#tools) and the test programs.

The two executables are one code base (`src\app`): the settings executable is a Windows program
that shows no console, the command-line one is a console program that a shell waits for. Both
are hardened like the handler, import only KnownDLLs statically (anything else is delay-loaded,
see `cmake\Hardening.cmake`), and embed a manifest and a version resource; the gates check all of
that. Every command does its real work except `--refresh` and the thumbnail rebuild
(`--uninstall --rebuild-thumbnails` says so and uninstalls anyway; `--zoom <n>` saves the zoom
without refreshing thumbnails), which a later build adds:

| Command | What it does |
|---|---|
| `--status [--json]`, `--check`, `--fix [--take-back]`, `--checks <list>`, `--diagnostics` | report, repair and schedule the checks, as before; `--fix` now also has the part for all users repaired (the class for all users pointed at the newest complete build), asking for administrator rights through UAC |
| `--install [--machine \| --user] [--checks <list>] [--zoom <n>] [--force]` | developer builds only: installs the build from the executable's own folder (see [Installing a build](#installing-a-build-developer-install)) |
| `--uninstall [--yes]` | removes the install for all users (through UAC) and this user's part: registration, check task, settings, files; with an installer's record, runs that installer's uninstaller instead (`/SILENT` with `--yes`) |
| `--migrate-v1 [--user-only \| --machine-only]` | removes what 1.x left, once 2.0 is installed (exit 6 otherwise, and 1.x stays usable): only 1.x's own files, recognized by their contents, its task, settings file and sign-in clean-up; its build folders for all users through UAC |
| `--zoom [<100-200>] [--no-refresh]` | shows the zoom and where it comes from, or saves a new one for this user |
| `--setup-user`, `--setup-machine`, `--uninstall-cleanup` | the steps an installer runs: per user (quiet, refuses an administrator process), for all users (must already run as administrator), and the clean-up before an uninstall (always exit 0) |

A command that would change settings first checks where it runs: started from a terminal whose
registry is virtualized (one that runs inside an app package), it refuses with exit code 8,
because its changes would not reach Windows. The commands that change the signed-in user's own
settings and files (`--check`, `--fix`, `--checks`, `--zoom <n>`, `--install`, `--uninstall`,
`--migrate-v1`, `--setup-user`) also refuse with exit code 8 from an administrator prompt; the
install, the uninstall, `--fix` and `--migrate-v1` ask for administrator rights themselves when
they need them. `pdft-run-outside` (below) runs a program outside such a terminal.

### Presets

| Preset | Builds | Tests it runs |
|---|---|---|
| `x64-release` | x64 Release | everything except the `system` and `e2e` labels |
| `x64-debug` | x64 Debug | the same as `x64-release` |
| `arm64-release` | ARM64 Release, cross-compiled | `gate` only (ARM64 programs cannot run on an x64 host) |
| `arm64-debug` | ARM64 Debug, cross-compiled | `gate` only |
| `x64-asan` | x64 Debug with AddressSanitizer (`/fsanitize=address`) in every target, for local test runs; not built by CI | like `x64-debug`, without the import, hardening and planting gates (see below) |
| `ci-x64` | x64 Release as CI builds it: Spectre mitigations required, deterministic output (`/Brepro`) | the same as `x64-release` |
| `ci-arm64` | ARM64 Release as CI builds it | `gate` only |

All presets use Ninja, `cl.exe` and the static C runtime, and treat compiler and linker warnings
as errors in the project's own code. Each preset has its own folder, `out\build\<preset>`. Don't
run two builds of the same preset at the same time.

`x64-asan` instruments everything, GoogleTest included (the C++ library refuses to link
instrumented and plain code together). AddressSanitizer rules out the `/RTC` checks and
`/DEPENDENTLOADFLAG:0x800`: every program then imports the AddressSanitizer runtime DLL, which is
never in System32. The build copies that DLL from the compiler's folder next to the programs. For
the same reason the `gate.imports.*`, `gate.hardening.*` and `gate.planting` checks don't apply
there, and its test preset leaves them out.

### Working in a developer prompt

From an "x64 Native Tools Command Prompt" (or "x64_arm64 Cross Tools" for ARM64), the presets can
be used step by step:

```bat
cmake --preset x64-release
cmake --build --preset x64-release
ctest --preset x64-release -L "unit|handler|integration"
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
| `PDFT_PDFIUM_ARCHIVE`, `PDFT_GTEST_ARCHIVE`, `PDFT_WIL_ARCHIVE` | empty | Offline builds: a local copy of the archive. It must match the pinned SHA-256. |
| `PDFT_BUILD_APP` | `ON` | Builds the settings app (`PdfThumbnailsSettings.exe`, `PdfThumbnailsCli.exe`). WIL is downloaded only for it. |
| `PDFT_DEV_INSTALL` | `ON` | The app's developer install commands (install from the build's own folder). Release builds, which an installer puts in place, turn it off. |
| `PDFT_ASAN` | `OFF` (`ON` in `x64-asan`) | AddressSanitizer for every target; for local test runs only. |
| `PDFT_BUILD_TOOLS` | `ON` | Builds the developer tools, `thumbtest` and `pdft-run-outside` (see [Tools](#tools)). `thumbtest` is also in the test bundle; `pdft-run-outside` is in no install component. |
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
ctest --preset x64-release -L "unit|handler|integration"
```

`gate.planting` is the one gate that starts programs (it also has the label `planting`);
`ctest --preset x64-release -L gate -E planting` runs only the static checks. `-LE planting`
does not work with the presets: CTest adds it to the preset's own label exclusion
(`system|e2e`) and leaves out only the tests that match every `-LE` expression. `x64-asan`
leaves `gate.planting` out by itself; don't add `-E` there, because a command-line `-E` replaces
that preset's own name filter.

| Label | What | Where it runs |
|---|---|---|
| `gate` | Static checks of the staged files: exports, imports (static C runtime, Windows DLLs only; the app's executables import only KnownDLLs and delay-load the rest), hardening flags, no build paths inside the binaries, no WIL failure cache in the app's executables, version resources, the app's manifests, the 18 PDFium functions the handler binds, the PDFium hash, license files and notices, and the build id. Native builds also run `gate.planting`: copies of the app's executables, next to a canary DLL planted under the names of DLLs they might load, run `--version` and `--help` (elevated, as on CI, also `--status --no-live-test`, which must exit 0, 1 or 6) and must not load any of them | every preset, both architectures (`gate.planting`: native builds only, not cross-compiled ones) |
| `unit` | The crop math, zoom clamping and generated GUIDs, the test-PDF generator (byte-identical to the 1.x generator), Medium-integrity helpers, temporary folders, the HKCU redirection, and the committed preview samples (`src\app\res`). For the settings app: the command line, text conversions, paths, the log (format, rotation, two writers), output encoding, exit codes, the registry-virtualization verdicts, named-object descriptors, start-up hardening, the COM security descriptor and the executables' string tables; what the checks read, decoded from made-up data (registry values, DLL headers and version resources, build ids, the slots and the zoom, 1.x leftovers, Smart App Control's verdict, the check task's triggers, the Explorer test's probe PDF and `--test-file` paths, Explorer settings, the Windows build); the check's findings, repair plans and exit codes on made-up snapshots; and the reports: the `--status` text line by line (this PC's shape, a per-user install, nothing installed, every problem line with its hint), the `--status --json` output (escaping, every field of schema 1, read back by a strict JSON reader) and the masking in `--diagnostics` (no SID, user, computer or profile path survives). For `pdft-run-outside`: its command line, its request file, the command line it builds and how it turns a run's outcome into an exit code | x64 presets; CI's ARM64 runner |
| `handler` | `PdfThumbnails.dll` loaded in-process at Medium integrity, with HKCU redirected to an empty private key: the crop table, pixels, the COM surface, error codes, where the zoom comes from (preview override, HKCU, `settings.ini`), an 8-thread stress test. Elevated, the program relaunches itself at Medium; `handler.refuses_elevated` checks that the DLL refuses to render when elevated and is skipped when not | x64 presets; CI's ARM64 runner |
| `integration` | The settings app's core against the real system: COM calls under the app's COM security (WMI read-only, Explorer's window list), the operation lock and other named objects across processes, the process tree and the registry-virtualization check; read-only: Smart App Control and the Code Integrity log, PdfCraft, the check task and 1.x's task, Explorer settings, the Windows build, DLL headers and build ids of the built DLLs; the registration, settings, 1.x leftovers and the whole check (gathering, repairs, the record, the operation lock) against private registry keys, folders under `%TEMP%` and fakes for everything Windows-wide; the diagnostics' log reader; the check task written in a Task Scheduler folder and under root-folder names of its own (each checkbox combination, the folder or the root-folder name, one copy per user, the check's repair of a task that runs another program, one run of `pdft-run-medium.exe --exit 0` through Task Scheduler; elevated also the protected task folder, a standard user's task in it and the removal of every user's task); `--checks` against private registry keys and a fake Task Scheduler (each value, the program it registers, its refusals); the install side against folders under `%TEMP%`, private registry keys (with made-up user hives) and fake launchers in place of UAC, so no test starts a real elevation: the verified copy, removal without following links, files in use, the trash folder, the build-folder permission check, RunOnce values, the 1.x clean-up by file contents, `--install`, `--uninstall`, `--uninstall-cleanup`, `--migrate-v1`, `--setup-user`, `--setup-machine` (elevated only) and `--fix`'s administrator step (deleting at the restart only checked to be refused unelevated); and both executables run with their output captured: `--version`, `--help`, `--status` and `--status --json` without the Explorer test (exit 0, 1 or 6, whatever this PC has), `--diagnostics` (nothing of the current user unmasked). Safe on a developer PC: no test starts a thumbnail extraction or runs `--check` or `--fix` against the real registry; it writes nothing to the user's profile except the check's volatile probe key and private test keys, which it deletes again, probe PDFs in `%TEMP%`, which it deletes too, and the start and end lines that the commands it runs unelevated (`--status`, `--diagnostics`, `--checks` where nothing is installed, `--setup-machine`) add to the app's log, which may create the app's data folder (not when elevated); in Task Scheduler it writes only its own test tasks and folder, which it deletes, and it never touches the real check task, its folder or 1.x's task. The executables' `--status` runs also delete their own stale probe PDFs (older than an hour) from `%TEMP%`. A writing command of the executables (`--zoom 120`, `--check`, `--fix`, `--fix --take-back`, `--checks` with each value) runs only where the check shows that its writes would be swallowed (a terminal inside a packaged app), where it must be refused (exit 8, or 2 for `--fix --take-back` without `--yes`), and is skipped everywhere else; `--checks` also runs elevated on a PC whose registry is the real one, where it must refuse (exit 8: CI only), and on a PC whose registry is the real one and where nothing is installed, where it must answer "not installed" (exit 6; elevated, as on CI, it runs at Medium integrity with a standard user's token), and a bad `--checks` value is a usage error (exit 2) everywhere; `--install`, `--uninstall --yes`, `--zoom 130`, `--setup-user` and `--migrate-v1` run only where they must be refused (exit 8: in such a terminal, or elevated on CI), `--setup-machine` only unelevated (exit 7), and their wrong command lines are usage errors (exit 2), and the named objects carry a suffix of the run's own, so a running settings app does not disturb the tests | x64 presets; CI's ARM64 runner |
| `system` | `system.tests` (`pdft_system_tests`): the install for all users from a staged runtime, for real: HKLM and HKCU, Program Files, the check task, every administrator step through UAC. It runs `--install --machine`, `--status --json` (healthy; the Explorer test may fail on a runner without a desktop, which is only reported), `--checks none` and `default`, `--zoom 130 --no-refresh`, `--setup-user`, `--setup-machine`, `--fix` on a damaged class for all users, `--uninstall --yes`; 1.1's state (installed for all users, and for one user: its class, build folder, per-user files with their shipped contents, `settings.ini` with `Zoom=120`, its task and its sign-in clean-up) replaced by `--install --machine`, and 1.1's leftovers next to an installed 2.0 removed by `--migrate-v1`; `--uninstall-cleanup` as an installer runs it. The unelevated commands run at Medium integrity with a standard user's token, so every administrator step goes through UAC, and each step's report must come back through the asking process's pipe. After each test nothing may be left (classes, slots, settings, RunOnce values, task folder and tasks, Program Files, the per-user and data folders; trash folders only when queued for deletion at the restart). The program refuses to run unless `PDFT_SYSTEM_TESTS=1`, it is elevated and nothing of 1.x or 2.x is installed (exit 77, CTest's "skipped", or 3) | CI's `system-x64` job, or a throw-away virtual machine; never on a PC you use |
| `e2e` | Explorer's thumbnail pipeline end to end; added with the thumbnail refresh | — |

Useful environment variables for the handler tests:
- `PDFT_TEST_HANDLER_DLL=<path>` tests another build of the handler (for example an older one).
  A build without the HKCU zoom and the preview interface (any 1.x build) fails some of the
  `ZoomPrecedence` and `PreviewOptions` tests; to compare one, also set
  `GTEST_FILTER=-ZoomPrecedence.*:PreviewOptions.*`.
- `PDFT_KEEP_TEMP=1` keeps the temporary folders under `%TEMP%\pdft-tests`.
- `PDFT_STRESS_FULL=1` also runs the stress test over all 18 test PDFs, including the two slow ones.

Where the task tests run:
- Unelevated (a developer PC): the check task is written under root-folder names of the test's own
  (`\<product>-Test check <pid>-<GUID> <SID>`) and, for some tests, in a folder of its own
  (`\<product>-Test-<pid>-<GUID>`); every test removes what it made, and the first test removes
  what an earlier run that crashed left behind (only names of that shape whose test process is
  gone and that this user owns). The tests of the shared, protected task folder
  (`TaskFolderElevated.*`) need an administrator and are skipped with that reason.
- Elevated (CI): the same, except the two tests that check what an unelevated process may not do
  (`SharedFolderStepsRefuseUnelevated`, `UnelevatedCannotMakeAFolderOwnedByAdministrators`), plus
  the `TaskFolderElevated` tests, which create the protected folder, take over one a standard user
  made, register a task in it with a standard user's token, skip a task there that runs as another
  account, and remove every user's task from it. CI fails the run when one of them did not print
  its own line in the test log (a skipped test counts as passed otherwise).
- One test lets Task Scheduler start `pdft-run-medium.exe --exit 0` in the signed-in user's
  session (a console window may flash). It is skipped when Application Control blocks that
  program, and when Task Scheduler has no session of the test's account to start a task with an
  interactive token in (an elevated runner, or a prompt started with `runas`).

#### Manual check: a standard account

The protected task folder is made only by an administrator. That a standard user cannot make it
that way, and that the folder such a user can make instead is never used (their task goes under
the root-folder name), is checked once by hand from a real standard (not administrator) account,
because CI and developer accounts are administrators. The maintainer creates that account; no
test or script does.
1. Build `x64-release`, then make the test bundle:
   `cmake --install out\build\x64-release --prefix C:\pdft-bundle --component tests`. Use a
   folder the standard account can read, outside any user profile.
2. Sign in to Windows as the standard account (switching users is fine; a prompt started with
   `runas /user:<name> cmd` is not enough, because Task Scheduler then has no session of that
   account to run a task in, and `TaskSchedulerRunsTheTask` skips) and run
   `ctest --test-dir C:\pdft-bundle -L integration --output-on-failure`, or only the task tests:
   `C:\pdft-bundle\pdft_integration_tests.exe --gtest_filter=TaskWriting.*:TaskFolderElevated.*`.
3. Expect: every `TaskWriting` test passes; the `TaskFolderElevated` tests are skipped
   ("needs elevation"); `SharedFolderStepsRefuseUnelevated` shows that the app refuses to make or
   empty the shared folder, `UnelevatedCannotMakeAFolderOwnedByAdministrators` that Task
   Scheduler itself refuses a standard user a folder owned by Administrators, and
   `UntrustedFolderMeansTheFallbackNameAndOneCopy` that a folder a standard user made instead is
   not used: the task goes under the root-folder name.
4. Afterwards, as that account, `schtasks /query /fo list | findstr /i "Test"` lists nothing of
   the tests. Record the date, the Windows build and the result in the pull request, then remove
   the account if it was made only for this.

On a PC with Smart App Control on, Windows sometimes blocks a freshly linked test program the
first time it starts (CTest reports "Not Run"). Run the tests again. Tests that start other
programs themselves (the `pdft-run-medium` tests, `gate.planting`, the integration tests of the
two executables) report "skipped" when Windows blocks one of those, never "passed".
`pdft-run-medium` and `pdft-run-outside` return exit code 126 when Windows blocks the program
they start.

### Tools

In `out\build\<preset>\bin\`:
- `thumbtest makepdfs <dir>` writes the 18 synthetic edge-case PDFs; `makesamples <dir>` writes the
  settings app's two preview samples.
- `thumbtest direct|stress [--zoom <percent>] ...` renders through the DLL directly (`--zoom`
  sets the zoom for the run in a redirected HKCU and a temporary `settings.ini`);
  `cache|cachenew|cacheinproc|shell ...` goes through Explorer's thumbnail pipeline;
  `compare <a> <b>` compares PNG files or folders pixel by pixel.
- `pdft-run-medium [--] <program> [arguments]` runs a program at Medium integrity with a standard
  user's rights; `pdft-run-medium --check-token` prints the current token.
- `pdft-run-outside [--timeout <seconds>] -- <program.exe> [arguments]` runs a program in the
  signed-in user's normal desktop context; see below.

#### pdft-run-outside

Some terminals run inside an app container or a sandbox, for example a terminal that is part of
a packaged app. Programs started from such a terminal can get a private copy of the user's
registry (HKCU) and of AppData: a check run there does not see what Explorer sees, and the
settings app's commands that change something refuse to run there (exit code 8).
`pdft-run-outside` runs a program as the signed-in user in that user's normal context instead,
prints its output and returns its exit code:

```bat
out\build\x64-release\bin\pdft-run-outside.exe -- "%CD%\out\build\x64-release\bin\PdfThumbnailsCli.exe" --version
```

- It registers a temporary scheduled task, `\PdfThumbnails-Dev\run-<GUID>`, for the current user
  (the user's interactive token without elevation, normal priority, a time limit 30 s over
  `--timeout` in case `pdft-run-outside` is gone by then), starts it and waits. Task Scheduler
  starts its tasks outside any such container.
- The task's only action is a second copy of `pdft-run-outside.exe`. It reads the request (the
  program, its arguments and the terminal's current folder) from a folder
  `%TEMP%\PdfThumbnails-Dev-run-<GUID>` that only the user and SYSTEM can open, after checking that
  the folder and the file are the ones the first copy made (owner, no links, location). It
  starts the program directly with `CreateProcessW`: no shell is involved, so the arguments reach
  the program exactly as given. For the same reason only `.exe` programs can be run.
- The program gets the user's own environment (as Task Scheduler builds it, not the terminal's),
  the terminal's current folder, no standard input and no console window. It is not looked up on
  `PATH`. Its standard output and error are printed when it ends.
- `--timeout <seconds>` (1 to 86400, default 120) stops it after that long (exit code 124). A
  task that Task Scheduler never started is reported as such (125), not as timed out.
- Afterwards the task, the task folder (unless another run is using it) and the private folder
  are deleted, also after Ctrl+C. If `pdft-run-outside` itself is killed, its task can stay
  behind; delete it in Task Scheduler.
- Exit codes: the program's own, or 124 (stopped at the time limit), 125 (`pdft-run-outside`
  failed, and says why), 126 (Application Control, such as Smart App Control, blocked the program),
  127 (the program could not be started for another reason), 130 (interrupted).
- On Windows 11 24H2 and later the second copy runs without a console window (its manifest asks
  for no console); earlier versions show one while the program runs.

The program runs with the user's rights against the real profile, so use it only for what you
would also run from a normal terminal.

## Staging (install components)

```bat
cmake --install out\build\x64-release --prefix <folder> --component runtime
```

| Component | Contents |
|---|---|
| `runtime` | What gets installed: the two executables, the two DLLs, `LICENSE.txt`, `THIRD-PARTY-NOTICES.md` and `licenses\` |
| `symbols` | The PDBs of the `runtime` binaries |
| `tests` | A self-contained test bundle: the test programs, `thumbtest` and `pdft-run-medium` with their PDBs, the runtime, the notices, the preview samples, the system tests' 1.x fixture files and a `CTestTestfile.cmake` with relative paths |

The test bundle runs anywhere CMake is installed:

```bat
ctest --test-dir <folder> -L "unit|handler|integration" --output-on-failure
```

It also holds `pdft_system_tests` and, in `system-fixture\`, the three 1.x files its migration
tests plant (`PdfThumbnails.ps1`, `PdfThumbnails.cmd`, `LICENSE`). The system tests install and
uninstall for real, so run them only on CI or on a throw-away virtual machine (UAC on, signed in
as an administrator, nothing of PdfThumbnails installed), from an elevated prompt:

```bat
set PDFT_SYSTEM_TESTS=1
set PDFT_SYSTEM_RUNTIME=<the runtime folder>
ctest --test-dir <folder> -L system -V
```

Each administrator step shows a UAC prompt there, unless `ConsentPromptBehaviorAdmin` is 0 as on
CI's runners. Without `PDFT_SYSTEM_TESTS=1`, unelevated, or on a PC that has 1.x or 2.x
installed, the program changes nothing and exits 77 (skipped) or 3 (refused).

## Installing a build (developer install)

Developer builds (`PDFT_DEV_INSTALL=ON`, the default) install themselves from the folder the
executables are in, laid out like the installer will lay them out:

```bat
cmake --install out\build\x64-release --prefix out\stage\runtime --component runtime
out\stage\runtime\PdfThumbnailsCli.exe --install --machine
```

Run it from a normal (not administrator) prompt; it asks for administrator rights once, through
UAC. `--install --machine` copies the two executables and the notices to
`C:\Program Files\PdfThumbnails` and the DLLs to `app\<version>-<build id>\` there (checking
every copy by its SHA-256), points the class for all users at them, makes the protected
`\PdfThumbnails` task folder, removes 1.x's build folders and older builds for all users (last,
once the new build is in place), then sets this user up: the zoom (1.x's from `settings.ini`
when there is no 2.0 value), the `.pdf` slots, the check task (both triggers by default, or
`--checks`), 1.x's task, files and sign-in clean-up removed. It then prints the status and exits
0 when it is healthy, 1 when it is installed but something is wrong. A file still in use waits in
a trash folder next to the install folder until the next restart; the status says so (Restart).
`--install --user` installs for the current user only, without administrator rights, into
`%LOCALAPPDATA%\Programs\PdfThumbnails`; where a machine-wide install exists it installs for all
users instead, and where that is 1.x's, it refuses unless `--force`.

`--install` refuses (exit 8, with the reason) when:
- the build's folder, one of its binaries or a folder above it can be changed by accounts other
  than you and the administrators (Everyone, Users, Authenticated Users, INTERACTIVE): the
  elevated step copies from that folder, so another account could swap a file in between, and
  with `--user` your thumbnail process and sign-in task would run what it put there. A
  folder inside your user profile, such as `out\` in a clone under `%USERPROFILE%`, passes; a new
  folder directly under `C:\` does not;
- the folder is not a complete 2.x build for this PC's architecture (an ARM64 build on an x64 PC,
  for example), or is the install folder itself;
- an installer has installed PdfThumbnails (its uninstall record exists); uninstall that first;
- it runs as administrator, or in a terminal whose registry is virtualized.

`--uninstall --yes` removes it again (the part for all users through UAC; files still in use
wait for the restart). `--migrate-v1` cleans up what 1.x left where 2.0 is already installed;
`--install` does the same by itself.

### Manual check: replacing 1.1 on your own PC

The system tests prove the migration on CI. On a PC that has 1.1 installed (the maintainer's),
the first `--install --machine` replaces it for real, so back up 1.1's state first:

1. From a normal prompt (`<GUID>` is `{0C3534C4-5B7A-48AB-828D-1CD660DC447F}`; `reg export` of
   HKLM works unelevated):

   ```bat
   mkdir "%USERPROFILE%\pdft-backup"
   reg export "HKLM\Software\Classes\CLSID\<GUID>" "%USERPROFILE%\pdft-backup\clsid.reg"
   reg export "HKCU\Software\Classes\.pdf\ShellEx" "%USERPROFILE%\pdft-backup\pdf-shellex.reg"
   reg export "HKCU\Software\Classes\SystemFileAssociations\.pdf\ShellEx" "%USERPROFILE%\pdft-backup\sfa-pdf-shellex.reg"
   copy "%LOCALAPPDATA%\Programs\PdfThumbnails\settings.ini" "%USERPROFILE%\pdft-backup\"
   ```

2. Build and stage `x64-release` as above, then run
   `out\stage\runtime\PdfThumbnailsCli.exe --install --machine` from a normal prompt (in a
   terminal whose registry is virtualized, through `pdft-run-outside -- "%CD%\out\stage\runtime\PdfThumbnailsCli.exe" --install --machine`).
3. Expect one UAC prompt and exit 0, and then `PdfThumbnailsCli --status`: healthy, your 1.1
   zoom kept (origin "saved setting"), the check task `\PdfThumbnails\Check <SID>` with both
   triggers, no `PdfThumbnails auto-repair` task, 1.1's build folder under
   `C:\Program Files\PdfThumbnails\app` gone (or in a trash folder until the restart), and in
   `%LOCALAPPDATA%\Programs\PdfThumbnails` none of 1.1's files.
4. To go back, from a normal prompt in the clone:

   ```bat
   out\stage\runtime\PdfThumbnailsCli.exe --uninstall --yes
   mkdir "%LOCALAPPDATA%\Programs\PdfThumbnails"
   copy "%USERPROFILE%\pdft-backup\settings.ini" "%LOCALAPPDATA%\Programs\PdfThumbnails\"
   PdfThumbnails.cmd install -Machine -AutoRepair
   ```

   The installed copy of 1.1's manager is gone by then (the migration removed it), so it is the
   clone's `PdfThumbnails.cmd`, which installs the build in `dist\PdfThumbnails\bin`. Restore
   `settings.ini` first: 1.1 keeps its zoom only there and writes 100 when the file is missing,
   and `--uninstall` has removed 2.0's copy of the zoom. The install writes the class and the
   slots again, so the `.reg` files are needed only if something still differs: the two HKCU ones
   import from a normal prompt, `clsid.reg` (an HKLM export) only from an administrator prompt.

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
| `build (ci-x64)` | x64 | always | `scripts\build.cmd ci-x64`: gates, unit, handler and integration tests; artifacts |
| `build (ci-arm64)` | x64 | always | `scripts\build.cmd ci-arm64` (cross-compiled): gates; artifacts |
| `build (x64-debug)` | x64 | not on pull requests, unless called | `scripts\build.cmd x64-debug` |
| `test-arm64` | ARM64 | not on pull requests, unless called | the ARM64 test bundle, run natively with `ctest --test-dir` |
| `system-x64` | x64 | not on pull requests, unless called; informational (never fails the run) | the `system` tests from the `tests-x64` bundle against the `PdfThumbnails-x64-unsigned` artifact: a real install, use and uninstall on the throw-away runner (see [Tests](#tests)) |

It runs on pushes to `main` and on pull requests (except when only Markdown files or `docs\`
changed), by hand (`workflow_dispatch`), and when another workflow calls it (`workflow_call`;
for the later release workflows). A called run builds and tests everything, also when the
calling workflow was started by a pull request. A new push to a pull request cancels its older
run. `test-arm64` uses only the `tests-arm64` artifact, so it still runs when `build (ci-x64)`
or `build (x64-debug)` fails; it fails if `build (ci-arm64)` made no artifact.

Details:
- **Runners.** GitHub-hosted runners run elevated (High integrity). The handler tests relaunch
  themselves at Medium integrity, and a check step fails the job unless the test log shows that
  relaunch, an elevated run of `handler.refuses_elevated` and `gate.planting`'s elevated
  `--status --no-live-test` runs, and the line of every other test that only an elevated runner
  runs (the protected task folder, the writing commands refused elevated, `--setup-machine` in
  test places). The job log also shows the runner image, the UAC settings and `whoami /groups`.
- **System tests.** `system-x64` builds nothing: it downloads the `tests-x64` bundle and the
  staged runtime and runs `ctest -L system` with `PDFT_SYSTEM_TESTS=1`. The runner is elevated,
  with UAC on and `ConsentPromptBehaviorAdmin=0`, so the tests' unelevated commands (Medium
  integrity, a standard user's token) get their administrator steps through UAC without a
  prompt. A step then checks that the tests reached their end and that the elevated step's report
  came back through the asking process's pipe (which accepts only its own child process). Its log
  is uploaded as `logs-system-x64` after every run. While it is informational, a failure shows on
  the job but not on the run.
- **Skipped tests.** CTest reports a test program as passed when GoogleTest skipped some of its
  cases. After the tests, a step lists every skipped test with its reason: CTest's own skips
  from the JUnit file (`Testing\Temporary\junit.xml`, which every test preset writes) and
  GoogleTest's skipped cases from the full test log. The list is in the step's log and in one
  notice on the run's summary page. Skips never fail the job; a missing log or JUnit file does.
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
  | `logs-<job>` | test logs, configure log and `buildinfo.txt`, only when a job fails (`logs-system-x64`: the system tests' log, always) | 7 days |

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
  builds plus the ARM64 test job and the x64 system job (a few minutes: it builds nothing).

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
