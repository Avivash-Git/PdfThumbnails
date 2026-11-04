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
are hardened like the handler, import only KnownDLLs statically (apart from the settings
executable's COMCTL32, which its manifest binds side by side to WinSxS; anything else is
delay-loaded, see `cmake\Hardening.cmake`), and embed a manifest and a version resource; the gates check all of
that. Every command does its real work except `--refresh` without `--full`, which a later build
adds; `--zoom <n>` saves the zoom without refreshing thumbnails and names `--refresh --full` for
those File Explorer already made:

| Command | What it does |
|---|---|
| `--status [--json]`, `--check`, `--fix [--take-back]`, `--checks <list>`, `--diagnostics` | report, repair and schedule the checks, as before; `--fix` now also has the part for all users repaired (the class for all users pointed at the newest complete build), asking for administrator rights through UAC |
| `--install [--machine \| --user] [--checks <list>] [--zoom <n>] [--force]` | developer builds only: installs the build from the executable's own folder (see [Installing a build](#installing-a-build-developer-install)) |
| `--uninstall [--yes] [--rebuild-thumbnails]` | removes the install for all users (through UAC) and this user's part: registration, check task, settings, files; with an installer's record, runs that installer's uninstaller instead (`/SILENT` with `--yes`), which does not rebuild the thumbnail cache (the report says so). `--rebuild-thumbnails` also rebuilds the thumbnail cache as `--refresh --full` does, right after this user's registration is removed and before the part for all users, and the question says that File Explorer restarts (unless the rebuild would be refused before it starts). With it, Ctrl+C stops the uninstall before it removes anything (exit 3); later it only keeps the rebuild from starting, until the rebuild ends the first process or deletes the first file. A rebuild that is refused, stopped or cannot start changes nothing, the report says why and the uninstall goes on; when File Explorer did not come back or a cache file could not be deleted, the uninstall exits 5 |
| `--migrate-v1 [--user-only \| --machine-only]` | removes what 1.x left, once 2.0 is installed (exit 6 otherwise, and 1.x stays usable): only 1.x's own files, recognized by their contents, its task, settings file and sign-in clean-up; its build folders for all users through UAC |
| `--zoom [<100-200>] [--no-refresh]` | shows the zoom and where it comes from, or saves a new one for this user |
| `--refresh --full [--yes]` | rebuilds this user's thumbnail cache, as 1.1's `refresh` did: ends the File Explorer and thumbnail processes of this session that hold it (only `explorer.exe` from the Windows folder and the system's `dllhost.exe`), deletes `%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db`, waits for Windows to start File Explorer again (or starts it, never from inside an app package and never leaving it inside a job), and opens the folder windows again through File Explorer (at most 25; tabs come back as separate windows). It asks first, or takes `--yes`, which it needs where there is no console to ask on (exit 2 otherwise); Ctrl+C stops it until it ends the first process or deletes the first file (exit 3). Refused before it asks (exit 8) as administrator, where File Explorer on this desktop is another account's, and below an app package where Windows does not restart File Explorer by itself (`AutoRestartShell` 0). Elsewhere, where `AutoRestartShell` is 0, it first starts explorer.exe suspended for a moment and ends it before it runs, to make sure this program could start File Explorer again: exit 8 when a File Explorer started from here would stay inside a job of the program it runs in (also, without that start, when that job lets no program leave it), and exit 5 when File Explorer cannot be started from here at all. Nothing changes in either case. It exits 1 while File Explorer copies or moves files (nothing changed), when File Explorer did not come back, a process could not be ended or a cache file could not be deleted, or when no cache file was deleted at all. Cache files that programs it leaves alone keep open (Windows Search, for example) stay until the next sign-in, and the report names those programs |
| `--setup-user`, `--setup-machine`, `--uninstall-cleanup` | the steps an installer runs: per user (quiet, refuses an administrator process), for all users (must already run as administrator; exit 8 below an app package where its test writes do not reach Windows, see below), and the clean-up before an uninstall (always exit 0; unelevated, it leaves out the part for all users without saying so; anything else it left out goes to its error output) |

A command that would change settings first checks where it runs: it writes a test value into
your registry and reads it back through WMI. Where the value does not come back (a terminal whose
programs get a private copy of the registry, as inside some packaged apps), it refuses with exit
code 8 before it changes anything or asks for administrator rights, because its changes would not
reach Windows. Two of the installer's steps differ: `--setup-machine` does not change your
registry and makes only the test below, and `--uninstall-cleanup` makes this test only for the
part for this account, after the part for all users, and leaves that part out where the value
does not come back (it says so on its error output and exits 0). A step for all users (the
administrator step of `--install`, `--fix`, `--migrate-v1` and `--uninstall`, and
`--setup-machine` and `--uninstall-cleanup`) that finds an app package above it, or cannot read
the processes above it, makes the same test itself before it changes anything: under
`HKEY_LOCAL_MACHINE\Software\Classes` and in its own user's registry. Where either value does not
come back, it changes nothing and the command exits 8; `--uninstall` and `--migrate-v1` exit 8 for
this only when nothing else failed, and 5 otherwise (`--uninstall-cleanup` leaves out the part for
all users, says so on its error output and exits 0). Windows Terminal is an app package too, but
its tabs passed both tests where this was checked (Windows Terminal 1.24.12741.0 on Windows 11
build 26300.9818): the test value came back in a tab, and `--install --machine` from a tab
installed for all users, with this line in its log:
`elevated machine-install writes checked: app package above; HKLM real, HKCU real`. The commands
make the tests every time they run, so a terminal whose registry is virtualized is still refused
before the UAC prompt. The administrator step of `--fix`, `--uninstall` and `--migrate-v1` is run
by the `PdfThumbnailsSettings.exe` installed in `C:\Program Files\PdfThumbnails`, not by the copy
the command was started from. One of an earlier build refuses below any app package, a Windows
Terminal tab included, without making the test; the command then prints its refusal and exits 8
the same way. Install the current build first, or run the command from a Command Prompt opened
with Win+R (type `cmd`), which no app package starts. Where Windows Terminal is the default
terminal app, that prompt opens in a Windows Terminal window and still works (Explorer starts its
`cmd`), but a tab opened there with + is a Windows Terminal tab. The steps for all users of an
earlier build of the settings app refuse below any app package too, so run such a build (the
stage of an older commit you go back to, for example) from a Win+R prompt. The commands that
change the signed-in user's own settings and files (`--check`, `--fix`, `--checks`, `--zoom <n>`,
`--install`, `--uninstall`, `--migrate-v1`, `--setup-user`) also refuse with exit code 8 from an
administrator prompt; the install, the uninstall, `--fix` and `--migrate-v1` ask for
administrator rights themselves when they need them. `pdft-run-outside` (below) runs a program
outside a terminal whose registry is virtualized.

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
| `PDFT_PDF_REFRESH_DEFAULT` | `OFF` | Whether Save in the settings window and `--zoom <n>` refresh PDF thumbnails in place, without restarting File Explorer. Off, they offer the full rebuild of the thumbnail cache instead, which restarts File Explorer. The refresh experiment decides the default; the commands that read it come in a later build. |
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
| `gate` | Static checks of the staged files: exports, imports (static C runtime, Windows DLLs only; the app's executables import only KnownDLLs (and the settings executable COMCTL32, which its manifest binds side by side to WinSxS) and delay-load the rest), hardening flags, no build paths inside the binaries, no WIL failure cache in the app's executables, version resources, the app's manifests, the 18 PDFium functions the handler binds, the PDFium hash, license files and notices, and the build id. Native builds also run `gate.planting`: copies of the app's executables, next to a canary DLL planted under the names of DLLs they might load, run `--version` and `--help` (elevated, as on CI, also `--status --no-live-test`, which must exit 0, 1 or 6) and must not load any of them | every preset, both architectures (`gate.planting`: native builds only, not cross-compiled ones) |
| `unit` | The crop math, zoom clamping and generated GUIDs, the test-PDF generator (byte-identical to the 1.x generator), Medium-integrity helpers, temporary folders, the HKCU redirection, and the committed preview samples (`src\app\res`). For the settings app: the command line, text conversions, paths, the log (format, rotation, two writers), output encoding and the display filter (control and bidirectional characters shown as U+FFFD; JSON written as is), exit codes, the registry-virtualization verdicts (the elevated side's too: its writes are tested last, once, and only below an app package or where the processes above it cannot be read), named-object descriptors, start-up hardening, the COM security descriptor and the executables' string tables; what the checks read, decoded from made-up data (registry values, DLL headers and version resources, build ids, the slots and the zoom, 1.x leftovers, Smart App Control's verdict, the check task's triggers, the Explorer test's probe PDF and `--test-file` paths, Explorer settings, the Windows build); the check's findings, repair plans and exit codes on made-up snapshots (an Explorer test that the command needs but that did not run among them), the Explorer test's fields in the log lines; the administrator step's report: the lines of the parts it did not complete, its end line, and how the asking side reads them (an older build's report, one cut short, a part it cannot read); and the reports: the `--status` text line by line (this PC's shape, a per-user install, nothing installed, every problem line with its hint), the `--status --json` output (escaping, every field of schema 1, read back by a strict JSON reader) and the masking in `--diagnostics` (no SID, user, computer or profile path survives). For `pdft-run-outside`: its command line, its request file, the command line it builds and how it turns a run's outcome into an exit code | x64 presets; CI's ARM64 runner |
| `handler` | `PdfThumbnails.dll` loaded in-process at Medium integrity, with HKCU redirected to an empty private key: the crop table, pixels, the COM surface, error codes, where the zoom comes from (preview override, HKCU, `settings.ini`), an 8-thread stress test. Elevated (above Medium, or with the Administrators group enabled), the program relaunches itself at Medium as a standard user (Administrators deny-only); `handler.refuses_elevated` checks that the DLL refuses to render when elevated, and, where Administrators is enabled, in a copy of itself whose token is lowered only to Medium (Administrators still enabled); it is skipped at Medium | x64 presets; CI's ARM64 runner |
| `integration` | The settings app's core against the real system: COM calls under the app's COM security (WMI read-only, Explorer's window list), the operation lock (the session lock it always takes first, which keeps a second holder out even when the `Global\` name was another account's for the first holder and is free again, and holds alone when another account made its `Global\` name first, as a mutex with another owner or a descriptor that keeps the user out, as an object of another type, or as a link that leads to itself) and other named objects across processes and threads, the process tree and the registry-virtualization checks (elevated, on CI, also the one under `HKEY_LOCAL_MACHINE\Software\Classes`); read-only: Smart App Control and the Code Integrity log, PdfCraft, the check task and 1.x's task, Explorer settings, the Windows build, DLL headers and build ids of the built DLLs; the sweep of stale probe PDFs (it stops at a folder that is a junction and leaves a junction or file link with a probe's name); the registration, settings, 1.x leftovers and the whole check (gathering, repairs, the record, the operation lock, its log line with the Explorer test's outcome) against private registry keys, folders under `%TEMP%` and fakes for everything Windows-wide; the diagnostics' log reader; the check task written in a Task Scheduler folder and under root-folder names of its own (each checkbox combination, the folder or the root-folder name, one copy per user, never over another account's task (a made-up user whose names this account's tasks hold, unelevated), the check's repair of a task that runs another program, one run of `pdft-run-medium.exe --exit 0` through Task Scheduler; elevated also the protected task folder, a standard user's task in it, another account's tasks in the folder and under the root-folder name, and the removal of every user's task); `--checks` against private registry keys and a fake Task Scheduler (each value, the program it registers, its refusals); the install side against folders under `%TEMP%`, private registry keys (with made-up user hives) and fake launchers in place of UAC, so no test starts a real elevation: the verified copy, removal without following links, registry trees deleted without following registry links (links made inside the private key), within one call's bounds and only through each key's own handle (a key that may not be deleted is an error only when it is empty), the class for all users written into a fresh key, a class path stored as REG_EXPAND_SZ left alone by the steps for all users, files in use, the trash folder, the build-folder permission check (owners and every account an entry names, the notices and `licenses\` included, a link, junction or mount point on the way refused, and everything copied held open until the copy is done), RunOnce values, the 1.x clean-up by file contents, `--install` (an Explorer test that cannot start leaves it unconfirmed; a part of the step for all users that did not complete, or a report that did not come back, is a failure: exit 1), `--uninstall`, `--uninstall-cleanup` (elevated, CI only, also with its part for all users left out where its test writes were not shown to reach Windows), `--migrate-v1`, `--setup-user`, `--setup-machine` (elevated only, while another thread holds the operation lock: it takes none; and refused where its test writes were not shown to reach Windows) and `--fix`'s administrator step (deleting at the restart only checked to be refused unelevated); the administrator step's report pipe (the reporter refuses a pipe its parent made but another owner owns, elevated only; a parent that never reads holds the step up for about 5 s at most; the asking side reads on past its 256 KiB cap, and stops when it gives up on a client that never stops writing; a pipe name another process took is recorded in the run; the parts a step reports as not done, with their count on its last line, read back into the failures and exit codes of `--install`, `--uninstall`, `--migrate-v1` and `--fix`, also from a report without the count, one cut short, and one naming a part this build cannot read); and both executables run with their output captured: `--version`, `--help`, `--status` and `--status --json` without the Explorer test (exit 0, 1 or 6, whatever this PC has), `--diagnostics` (nothing of the current user unmasked; elevated, CI only, it leaves out the log, and `--diagnostics --out` is refused with exit 8 without writing a file). Safe on a developer PC: no test starts a thumbnail extraction or runs `--check` or `--fix` against the real registry; it writes nothing to the user's profile except the checks' volatile probe keys under `HKCU\Software` and private test keys, which it deletes again, probe PDFs in `%TEMP%`, which it deletes too, and the start and end lines that the commands it runs unelevated (`--status`, `--diagnostics`, `--checks` where nothing is installed, `--setup-machine`) add to the app's log, which may create the app's data folder (not when elevated); outside the profile, elevated (as on CI), it also writes a volatile probe key under `HKEY_LOCAL_MACHINE\Software\Classes`, which it deletes again (unelevated that key cannot be made); in Task Scheduler it writes only its own test tasks and folder, which it deletes, and it never touches the real check task, its folder or 1.x's task. The executables' `--status` runs also delete their own stale probe PDFs (older than an hour) from `%TEMP%` when not elevated (an elevated process never sweeps `%TEMP%`). A writing command of the executables (`--zoom 120`, `--check`, `--fix`, `--fix --take-back`, `--checks` with each value) runs only where the check shows that its writes would be swallowed (a terminal whose registry is virtualized, as inside some packaged apps), where it must be refused (exit 8, or 2 for `--fix --take-back` without `--yes`), and is skipped everywhere else; `--checks` also runs elevated on a PC whose registry is the real one, where it must refuse (exit 8: CI only), and on a PC whose registry is the real one and where nothing is installed, where it must answer "not installed" (exit 6; elevated, as on CI, it runs at Medium integrity with a standard user's token), and a bad `--checks` value is a usage error (exit 2) everywhere; `--install`, `--uninstall --yes`, `--zoom 130`, `--setup-user` and `--migrate-v1` run only where they must be refused (exit 8: in such a terminal, or elevated on CI), `--setup-machine` only unelevated (exit 7), and their wrong command lines are usage errors (exit 2); `--refresh --full --yes` runs only where it must be refused (exit 8: in such a terminal, or elevated on a runner GitHub hosts, never on an elevated prompt of your own), and so do the usage errors of `--refresh` (exit 2: `--full` without `--yes` and without a console, `--folder` with `--full`, `--yes` without `--full`), so no test ends File Explorer or deletes the thumbnail cache; and the named objects carry a suffix of the run's own, so a running settings app does not disturb the tests | x64 presets; CI's ARM64 runner |
| `system` | `system.tests` (`pdft_system_tests`): the install for all users from a staged runtime, for real: HKLM and HKCU, Program Files, the check task, every administrator step through UAC. It runs `--install --machine`, `--status --json` (healthy; the Explorer test may fail on a runner without a desktop, which is only reported, but it must start; whether `--install` and `--fix` exit 1 for it is judged by their own Explorer test, read from their log line), `--checks none` and `default`, `--zoom 130 --no-refresh`, `--setup-user`, `--setup-machine`, `--fix` on a damaged class for all users, `--uninstall --yes`; 1.1's state (installed for all users, and for one user: its class, build folder, per-user files with their shipped contents, `settings.ini` with `Zoom=120`, its task and its sign-in clean-up) replaced by `--install --machine`, and 1.1's leftovers next to an installed 2.0 removed by `--migrate-v1`; `--uninstall-cleanup` as an installer runs it. The user is a local account the program makes for the run: a member of Administrators with a random name (`pdft-sys-` and six hex digits) and a random password that is never shown or stored, logged on interactively with its profile loaded. Its commands run with the limited token of that logon, so every administrator step goes through UAC into the account's full token (without a prompt where `ConsentPromptBehaviorAdmin` is 0), must get administrator rights there, and its report must come back through the asking process's pipe; for the install, `--fix` and `--migrate-v1`, whose reports are read back, it must end with its count of parts not done (`not-done=0`) and show no test of its writes (nothing above it is packaged); the installer's administrator commands run with the program's own elevated token. Everything the tests check about the user (HKCU, the slots, RunOnce, the per-user and data folders, the check task) is that account's. A second local account, a standard user's, is made the same way (the same kind of random name and password) but is not a member of Administrators: its interactive logon gives a token that is not elevated, has no linked token (elevation type 1) and no Administrators group. Before anything is installed it runs the integration tests' task tests (`TaskWriting.*` and `TaskFolderElevated.*`, from a copy of `pdft_integration_tests` in its temp folder): every `TaskWriting` test must pass (only `TaskSchedulerRunsTheTask` may skip, since Task Scheduler starts that task only in a Windows session of the account's own, which this logon does not have), every `TaskFolderElevated` test must skip as needing elevation, the program must exit 0, and nothing of the tests may be left in Task Scheduler, nor anything of the product. After the install for all users it runs its own `--setup-user` (exit 0, no output), whose check task must go into the protected `\PdfThumbnails` folder the install made (not under the root-folder name), with both triggers and the installed settings executable, and its slots and zoom set; the uninstall's step for all users must then take its slots, settings and task out of its loaded hive and the task folder (its data folder, the log of its own commands, is left to it, as to every other user, and the tests remove it). After each test nothing may be left (classes, slots, settings, RunOnce values, task folder and tasks, Program Files, the per-user and data folders; trash folders only when queued for deletion at the restart; the standard account's state is checked the same way); at the end both accounts and their profiles are deleted, also after a failure (an account a run ended from outside left behind is removed by the next run). The privileges the accounts need (backup and restore for their hives, debug to end their programs) are enabled only while used, so the installer's commands run with them disabled, as a real installer's are. The program refuses to run unless `PDFT_SYSTEM_TESTS=1` and either `GITHUB_ACTIONS=true` with `RUNNER_ENVIRONMENT=github-hosted` (set on the runners GitHub hosts) or `PDFT_SYSTEM_TESTS_THROWAWAY_VM=1` (your own throw-away virtual machine), it is elevated, the registry is not virtualized and nothing of 1.x or 2.x is installed (exit 77, CTest's "skipped", or 3) | CI's `system-x64` job, or a throw-away virtual machine; never on a PC you use |
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
  account, update a user's own task in it where it is, keep another account's task under the
  user's name there (neither registered over nor deleted) and use the root-folder name instead,
  answer that both names are taken when another account's tasks hold both (also to a standard
  user), and remove every user's task from it. CI fails the run when one of them did not print
  its own line in the test log (a skipped test counts as passed otherwise), for example
  `[ tasks ] another account's task in the folder kept, the fallback name used: yes` and
  `[ tasks ] standard user with both names taken: apply-task hr=0x800402E8 elevated=0`.
- One test lets Task Scheduler start `pdft-run-medium.exe --exit 0` in the signed-in user's
  session (a console window may flash). It is skipped when Application Control blocks that
  program, and when Task Scheduler has no session of the test's account to start a task with an
  interactive token in (an elevated runner, or a prompt started with `runas`).
- As a standard account (CI's `system-x64`): the system tests make a throw-away standard account
  (see the `system` row above) and run `TaskWriting.*` and `TaskFolderElevated.*` as it, with the
  token of its interactive logon. Every `TaskWriting` test must pass, among them
  `SharedFolderStepsRefuseUnelevated`, `UnelevatedCannotMakeAFolderOwnedByAdministrators` and
  `UntrustedFolderMeansTheFallbackNameAndOneCopy`, except that `TaskSchedulerRunsTheTask` may skip
  because Task Scheduler has no Windows session of that account to start the task in (as under
  `runas`); every `TaskFolderElevated` test must skip ("needs elevation"); and nothing of them may
  be left in Task Scheduler, nor anything of the product.

#### A standard account (automated on CI)

The protected task folder is made only by an administrator. That a standard user cannot make it
that way, and that the folder such a user can make instead is never used (their task goes under
the root-folder name), must be shown from a real standard (not administrator) account, because
CI's and developers' own accounts are administrators. CI's `system-x64` job does this: the system
tests make a standard account of their own, run the task tests as it (the last item of "Where
the task tests run" above) and, after the install for all users, its own `--setup-user`, whose
check task must go into the protected `\PdfThumbnails` folder. By hand, follow the steps below,
and only on a PC where that is wanted: the account the tests make has no Windows session of its
own, so only a real sign-in also runs `TaskSchedulerRunsTheTask`, and step 4, with two real
standard accounts, is not automated. The maintainer creates the account; no test or script does
that on a PC someone uses.
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
4. Optional, with a second standard account, the one check of two real standard accounts. If
   PdfThumbnails is installed for the first account already, first run `--checks none` as that
   account, so that neither name holds its own task (a standard account cannot register over
   another account's task, so the next step would fail and `--checks default` would exit 0).
   Then, from the second account, register a task named
   `\PdfThumbnails check <first account's SID>` in the root folder (and, if the protected folder
   exists, `Check <first account's SID>` in `\PdfThumbnails`). Then, as the first account,
   install PdfThumbnails if it is not installed yet (`--install --user`; it reports the task step
   as not done and exits 1); `--checks default` must exit 5 and say that another account's
   scheduled tasks hold the names and that an administrator can delete them, and `--status` must
   show the Checks line as a problem with that hint. Afterwards, delete those tasks again from
   the second account (or as an administrator).
5. Afterwards, as that account, `schtasks /query /fo list | findstr /i "Test"` lists nothing of
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
settings app's commands that change something refuse to run there (exit code 8, before any
administrator prompt). Windows Terminal is a packaged app too, but where it was checked (see
[Build](#build)) the programs in its tabs used the real registry and the settings app's log went
to the real `%LOCALAPPDATA%`, so its tabs need no `pdft-run-outside`.
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
uninstall for real, and they make two local accounts for the run, an administrator and a standard
user (deleted again at the end, with their profiles); the standard account runs
`pdft_integration_tests` from the bundle. So run them only on CI or on a throw-away virtual
machine (UAC on, signed in as an administrator, nothing of PdfThumbnails installed), from an
elevated prompt. Two variables must ask for them:

- `PDFT_SYSTEM_TESTS=1`: run the system tests at all.
- `GITHUB_ACTIONS=true` with `RUNNER_ENVIRONMENT=github-hosted`, or
  `PDFT_SYSTEM_TESTS_THROWAWAY_VM=1`: this PC is thrown away after the run, so the tests may make,
  use and delete accounts on it. GitHub Actions sets both on the runners GitHub hosts.
  `GITHUB_ACTIONS=true` alone is not enough: GitHub sets it on self-hosted runners too, and tools
  that run a workflow on your own PC set it as well. On your own virtual machine, set
  `PDFT_SYSTEM_TESTS_THROWAWAY_VM` yourself, and never on a PC you use. A run that is ended from
  outside (a timeout, a cancelled job) cannot delete its accounts; the next run removes every
  account with a test account's name and comment, with its profile, before it makes its own.

```bat
set PDFT_SYSTEM_TESTS=1
set PDFT_SYSTEM_TESTS_THROWAWAY_VM=1
set PDFT_SYSTEM_RUNTIME=<the runtime folder>
ctest --test-dir <folder> -L system -V
```

`PDFT_SYSTEM_RUNTIME` must name a folder on a local drive (not a share or a mapped network
drive), with no link, junction or mount point on the way to it or inside it; the tests refuse it
otherwise. They copy it with administrator rights, so only administrators should be able to
change it.

Each administrator step of the test account shows a UAC prompt in your session (approve it), unless
`ConsentPromptBehaviorAdmin` is 0 as on CI's runners. Without both variables, unelevated, where
the registry may be virtualized, or on a PC that has 1.x or 2.x installed, the program changes
nothing and exits 77 (skipped) or 3 (refused). The log names each account (`pdft-sys-` and six
hex digits) and its SID, never its password, and ends with a line for each saying whether the
account and its profile were deleted.

## Installing a build (developer install)

Developer builds (`PDFT_DEV_INSTALL=ON`, the default) install themselves from the folder the
executables are in, laid out like the installer will lay them out:

```bat
cmake --install out\build\x64-release --prefix out\stage\runtime --component runtime
out\stage\runtime\PdfThumbnailsCli.exe --install --machine
```

Run it from a normal (not administrator) prompt, a Windows Terminal tab included (see
[Build](#build)); it asks for administrator rights once, through UAC. `--install --machine`
copies the two executables and the notices to
`C:\Program Files\PdfThumbnails` and the DLLs to `app\<version>-<build id>\` there (checking
every copy by its SHA-256), points the class for all users at them, makes the protected
`\PdfThumbnails` task folder, removes 1.x's build folders and older builds for all users (last,
once the new build is in place), then sets this user up: the zoom (1.x's from `settings.ini`
when there is no 2.0 value), the `.pdf` slots, the check task (both triggers by default, or
`--checks`), 1.x's task, files and sign-in clean-up removed. It then prints the status, with the
Explorer test (one thumbnail made in a fresh thumbnail process, the way File Explorer makes it),
and exits 0 when it is healthy and that test passed. It exits 1 when it is installed but
something is wrong; when the test could not run (the status then ends "Result: no problem found,
but not confirmed (the Explorer test did not run)", and `--status` runs the test again); or when
a part of the step for all users after the class did not complete (the task folder, queuing a
file that was in use for deletion at the restart, removing 1.x's or the older build folders) or
its report did not come back (unless another process had taken the name of its report channel,
which the output then says). Neither undoes the install. Such a part is printed as
`Not done: machine-install <part>: <error code>` after the step's report. A report that did not
come back, or came back cut short, is printed as
`Not done: machine-install: no complete report came back (see the log)`; when nothing came back
there is no step report before it, and the step's line in the log says `no-report`
(`report-incomplete` for a report cut short). The log line
`install ... result=<result> live=<HRESULT> live-ms=<ms>` (or `live=could-not-start
live-hr=<HRESULT>`, or `live=skipped live-skip=<reason>`) in
`%LOCALAPPDATA%\PdfThumbnails\Logs\PdfThumbnails.log` records the Explorer test. A file still
in use waits in a trash folder next to the install folder until the next restart; the status says
so (Restart).
`--install --user` installs for the current user only, without administrator rights, into
`%LOCALAPPDATA%\Programs\PdfThumbnails`; where a machine-wide install exists it installs for all
users instead, and where that is 1.x's, it refuses unless `--force`.

`--install` refuses (exit 8, with the reason) when:
- the build's folder, anything copied from it (the four binaries, `LICENSE.txt`,
  `THIRD-PARTY-NOTICES.md` and everything in `licenses\`) or a folder above it is owned by, or can
  be changed by, any account or group other than you and the administrators (Administrators,
  SYSTEM, TrustedInstaller, and the built-in Administrator account and Domain Admins of this PC,
  of the domain it is joined to or of your own domain, not of any other domain).
  The message names the account or group and its SID. The elevated step copies from that folder,
  so another account could swap a file in between, and with `--user` your thumbnail process and
  sign-in task would run what it put there. A folder inside your user profile, such as `out\` in
  a clone under `%USERPROFILE%`, passes; a new folder directly under `C:\` does not;
- a link, junction or mount point is on the folder's path or among what is copied (a link in the
  path you started the command from is resolved first);
- the folder is on a network drive (a share, a drive letter mapped to one, or a local folder
  linked to one, which is refused before anything in it is read);
- the folder is not a complete 2.x build for this PC's architecture (an ARM64 build on an x64 PC,
  for example), or is the install folder itself;
- an installer has installed PdfThumbnails (its uninstall record exists); uninstall that first;
- it runs as administrator, or in a terminal whose registry is virtualized.

Started below an app package, or where it cannot read the processes above it, the step for all
users also refuses after the UAC prompt (exit 8, nothing installed) when its test values under
`HKEY_LOCAL_MACHINE\Software\Classes` or in its own registry do not come back through WMI; the
output says so and shows what the step found.

While it copies, the step keeps the build's folder, every folder above it and everything it
copies open, so none of it can be renamed, replaced or written until the copy is done. The
elevated step judges the folder for the account it runs as: approving the prompt with another
administrator's password (from a standard account) therefore refuses a build folder in the
standard account's profile.

`--uninstall --yes` removes it again. For an install for all users, the part for all users is
removed through UAC by the `PdfThumbnailsSettings.exe` installed in
`C:\Program Files\PdfThumbnails` (not the staged one); its files still in use, that running
executable among them, wait in a trash folder until the next restart. A part for all users that
fails is printed as `Not done: machine <part>: <error code>` and the command exits 5. When that
installed executable comes from an older build, its failures are one line,
`Not done: machine: <error code>` with the first one's code (`machine left: 0x800402F0` when that
code means registry keys left as they were). `--migrate-v1` cleans up what 1.x left where 2.0 is
already installed; `--install` does the same by itself.

### Manual check: replacing 1.1 on your own PC

The system tests prove the migration on CI. On a PC that has 1.1 installed (the maintainer's),
the first `--install --machine` replaces it for real, so back up 1.1's state first:

1. From a normal `cmd` prompt (the commands in this section are `cmd` syntax, not PowerShell;
   `<GUID>` is `{0C3534C4-5B7A-48AB-828D-1CD660DC447F}`; `reg export` of HKLM works unelevated):

   ```bat
   mkdir "%USERPROFILE%\pdft-backup"
   reg export "HKLM\Software\Classes\CLSID\<GUID>" "%USERPROFILE%\pdft-backup\clsid.reg"
   reg export "HKCU\Software\Classes\.pdf\ShellEx" "%USERPROFILE%\pdft-backup\pdf-shellex.reg"
   reg export "HKCU\Software\Classes\SystemFileAssociations\.pdf\ShellEx" "%USERPROFILE%\pdft-backup\sfa-pdf-shellex.reg"
   copy "%LOCALAPPDATA%\Programs\PdfThumbnails\settings.ini" "%USERPROFILE%\pdft-backup\"
   ```

2. Build and stage `x64-release` as above, then run
   `out\stage\runtime\PdfThumbnailsCli.exe --install --machine` from a normal prompt (a Windows
   Terminal tab works; see [Build](#build)). A terminal whose registry is virtualized refuses it
   with exit code 8 before the UAC prompt; from there, run it through `pdft-run-outside` with a
   longer time limit (its default, 120 s, can stop it while the prompt waits):

   ```bat
   out\build\x64-release\bin\pdft-run-outside.exe --timeout 600 -- "%CD%\out\stage\runtime\PdfThumbnailsCli.exe" --install --machine
   ```

3. Expect one UAC prompt and exit 0 (the log line
   `install ... result=healthy live=0x00000000 live-ms=<n>` shows that the Explorer test passed),
   and then `PdfThumbnailsCli --status`: healthy, your 1.1 zoom kept (the Zoom line ends in
   "(registry)"; `--zoom` calls the same value "saved setting"), the check task
   `\PdfThumbnails\Check <SID>` with both triggers, no `PdfThumbnails auto-repair` task, 1.1's
   build folder under `C:\Program Files\PdfThumbnails\app` gone (or in a trash folder until the
   restart), and in `%LOCALAPPDATA%\Programs\PdfThumbnails` none of 1.1's files.
4. To go back, from a normal `cmd` prompt in the clone (a `cmd` tab in Windows Terminal will do:
   the step for all users is then run by the `PdfThumbnailsSettings.exe` that step 2 installed,
   which tests its writes there the way the install's step does, see [Build](#build); in a
   terminal whose registry is virtualized, `--uninstall` refuses with exit code 8 before it
   removes anything), first save 2.0's log, which `--uninstall` deletes, then uninstall 2.0:

   ```bat
   copy "%LOCALAPPDATA%\PdfThumbnails\Logs\PdfThumbnails.log*" "%USERPROFILE%\pdft-backup\"
   out\stage\runtime\PdfThumbnailsCli.exe --uninstall --yes
   ```

   It removes your part first (the `.pdf` slots, the check task, the zoom), then asks for
   administrator rights once, for the `PdfThumbnailsSettings.exe` installed in
   `C:\Program Files\PdfThumbnails` (not the staged one), which removes the part for all users.
   That executable runs from the folder it removes, so it ends up in a trash folder next to that
   folder (`C:\Program Files\PdfThumbnails.trash-` and 32 hex digits), which Windows deletes at
   the next restart. Last, it deletes `%LOCALAPPDATA%\PdfThumbnails`, the log and its own log
   lines included, so its output is the only record of the run. Expect exit 0 and these two
   lines:

   ```
   Removed the part for all users; files that were in use are deleted at the next restart.
   PdfThumbnails removed. PDF files show their app icon again.
   ```

   If it prints `PDF thumbnails are off for you, but the part for all users is still installed.`
   (exit 1, 5, 7 or 8) or a `Not done:` line instead, stop: 2.0 is not fully removed. Deal with
   what the output says (a declined prompt, for example) and run `--uninstall --yes` again until
   it exits 0 with `PdfThumbnails removed. PDF files show their app icon again.` as its last line
   (once the part for all users is gone, a rerun prints only that line). A rerun starts the step
   for all users again only while the class for all users or `C:\Program Files\PdfThumbnails` is
   still there, as the `PDF thumbnails are off ...` line says, so a `Not done: machine <part>`
   line printed without that line is not retried: remove what it names by hand (the step's
   report, indented above it, says what was left). Only then restore 1.1:

   ```bat
   mkdir "%LOCALAPPDATA%\Programs\PdfThumbnails"
   copy "%USERPROFILE%\pdft-backup\settings.ini" "%LOCALAPPDATA%\Programs\PdfThumbnails\"
   PdfThumbnails.cmd install -Machine -AutoRepair
   ```

   The installed copy of 1.1's manager is gone by then (the migration removed it), so it is the
   clone's `PdfThumbnails.cmd`, which installs the build in `dist\PdfThumbnails\bin`. Restore
   `settings.ini` first: 1.1 keeps its zoom only there and writes 100 when the file is missing,
   and `--uninstall` has removed 2.0's copy of the zoom. 1.1's install asks for administrator
   rights again (a second UAC prompt, for `powershell.exe`). The install writes the class and the
   slots again, so the `.reg` files are needed only if something still differs: the two HKCU ones
   import from a normal prompt, `clsid.reg` (an HKLM export) only from an administrator prompt.
   1.1's `repair.log` is not restored: the migration deleted it, and step 1 does not back it up.

   Once 1.1 is back, never run 2.0's `--uninstall`: 1.1 uses the same class and `.pdf` slots, so
   it would remove 1.1's slots and its `PdfThumbnails auto-repair` task, and, while 2.0's
   `PdfThumbnailsSettings.exe` is still in `C:\Program Files\PdfThumbnails`, 1.1's class for all
   users and that whole folder too. Remove 1.1 with its own `PdfThumbnails.cmd uninstall`.

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
  themselves at Medium integrity as a standard user (Administrators deny-only), and a check step
  fails the job unless the test log shows that relaunch, an elevated run of
  `handler.refuses_elevated` and its refusal of a Medium token with Administrators enabled, and
  `gate.planting`'s elevated
  `--status --no-live-test` runs, and the line of every other test that only an elevated runner
  runs (the protected task folder, the writing commands refused elevated, `--setup-machine` and
  `--uninstall-cleanup` in test places, the registry check under
  `HKEY_LOCAL_MACHINE\Software\Classes`). The job log also shows the runner image, the UAC
  settings and `whoami /groups`.
- **System tests.** `system-x64` builds nothing: it downloads the `tests-x64` bundle and the
  staged runtime and runs `ctest -L system` with `PDFT_SYSTEM_TESTS=1` (GitHub Actions sets
  `GITHUB_ACTIONS=true` and `RUNNER_ENVIRONMENT=github-hosted`, the tests' second opt-in). The
  runner is elevated as the built-in Administrator, whose token has no linked token, so the tests
  make a local administrator account of their own and run the user's commands with the limited
  token of its interactive logon: with UAC on and `ConsentPromptBehaviorAdmin=0`, their
  administrator steps go through UAC into the account's full token without a prompt. They also
  make a standard account (not in Administrators), which runs the task tests of the bundle's
  `pdft_integration_tests` and its own `--setup-user` (see [Tests](#tests)). The job log shows
  the runner's privileges, the Secondary Logon service the tests start the accounts' programs
  through, the second opt-in and the accounts' tokens. A step then checks that the test account
  had a limited token, that UAC elevated its steps (an annotation says so when it did not), that
  the tests reached their end, that the elevated step's report came back through the asking
  process's pipe (which accepts only its own child process), that the standard account had a
  standard user's token, that the task tests came out as a standard user's (every `TaskWriting`
  test passed but `TaskSchedulerRunsTheTask`, which may skip, every `TaskFolderElevated` test
  skipped, nothing left behind), and that the standard account's `--setup-user` put
  its check task into the protected task folder. Its log is uploaded as `logs-system-x64` after
  every run. While it is informational, a failure shows on the job but not on the run.
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

Every day (05:23 UTC), it compares the PDFium release pinned in `cmake/Pins.cmake` with the
latest pdfium-binaries release. While they differ, it keeps one issue open with the label
`security/pdfium`; it edits that issue rather than opening another.

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
