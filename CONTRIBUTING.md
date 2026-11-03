# Contributing

How to build and test is in [docs/BUILDING.md](docs/BUILDING.md): `scripts\build.cmd <preset>` from a
plain Command Prompt in the repository root. Format C++ with clang-format 23.x (the repository's
`.clang-format`); every preset builds with warnings as errors.

## Code rules

These apply to the settings app (`src/app`) and to the tests that drive it.

### Nothing throws out of a `noexcept` function

The app's functions that do I/O return an `HRESULT` and are `[[nodiscard]] ... noexcept`. An
exception escaping one of them ends the process at once (`std::terminate`), with no log line and an
exit code nobody can read, so:

- Use only the `std::error_code` overloads of `std::filesystem` (`exists(path, error)`,
  `create_directories(path, error)`, ...), never the throwing ones.
- Wrap the body of every `noexcept` function that touches the standard library (strings, vectors,
  paths: anything that allocates) in `try { ... } CATCH_RETURN();` from WIL, for example as a
  function-try-block:

  ```cpp
  HRESULT GetLogPath(std::filesystem::path* out) noexcept try {
    RETURN_IF_FAILED(GetDataDir(out));
    *out /= L"Logs";
    return S_OK;
  }
  CATCH_RETURN();
  ```

- `wmain`, `wWinMain` and every thread procedure catch everything (`catch (...)`) and turn it into an
  exit code.
- No static initializer does real work: globals are constants or trivially initialized. The
  executables restrict where DLLs load from as their very first statement, and a constructor that
  runs before `main` would load DLLs before that.
- WIL is compiled with `RESULT_SUPPRESS_STATIC_INITIALIZERS` (`cmake/Dependencies.cmake`), and
  `wil::WilInitialize_Result` is never called: both would set up a failure cache that other
  processes in the session can tamper with.

### "Elevated"

A process counts as elevated when the Administrators group is enabled in its token
(`CheckTokenMembership`) **or** its integrity level is above Medium. Either way it must not do the
per-user work meant for the signed-in user (that user's registry, Explorer's thumbnail cache). Use
`ProcessContext::elevated` (`src/app/core/context.h`); do not test only one of the two conditions.
The handler applies the same rule itself, without WIL (`IsAboveMediumOrAdministrator` in
`src/handler/PdfThumbnails.cpp`), and then refuses to render. Neither uses `TokenElevation`:
Windows also reports it for a Medium token of the built-in Administrator with Administrators
deny-only and no privileges, which has no administrator rights.

### Resource scripts are UTF-8

Every `.rc` and `.rc.in` file is UTF-8 and starts with

```
#pragma code_page(65001)
```

on its first line. Without it, the resource compiler reads the file in the ANSI code page of the
build machine, and strings such as "Rebuild thumbnails…" or "0.14.0 → 0.15.0" come out garbled (or
differently on another machine). A unit test loads such strings from the built executables and
compares them with the exact text.

### Reports and personal data

- The texts the reports show (`--status`, the JSON `text` fields, `--check`/`--fix` output) are
  built in `src/app/core/status_format.cpp`, so they can move into the string table with the
  settings window. Write a non-ASCII character in C++ source as a `\u` escape (`L" \u00B7 "`).
- Everything `--diagnostics` shows goes through `BuildDiagnostics`
  (`src/app/core/diagnostics.cpp`), which masks the whole report at the end: the profile folder,
  the user, computer and domain names and every SID. Add new details there, never next to it.
- Everything the executables print goes through `Output::Line` and `Output::Error`
  (`src/app/core/output.cpp`). They show control and bidirectional-control characters as U+FFFD
  (`DisplayText`), so text from the registry, paths or tasks cannot steer the console. Only
  `--status --json` is written as is (`Output::Json`), because the JSON writer escapes those
  characters itself.
- The log never gets PDF paths or names, or anything else the user typed (a `--test-file` or
  `--out` path is shown on the console only).

### Tests and commands that change the PC

The install side (`--install`, `--uninstall`, `--migrate-v1`, the installer's steps, `--fix`'s
administrator step) changes Program Files, HKLM, every user's settings and Task Scheduler. On a
PC you use:

- `unit` and `integration` tests run those commands' code only against private places: folders
  under `%TEMP%\pdft-tests`, registry keys through `RegistryRoots` (`ScopedRegistrySandbox`,
  RunOnce included), Task Scheduler folders and names of the test's own. The administrator steps
  run in the test process through a fake `ElevationLauncher`: no test may start a real UAC
  elevation, queue a deletion at the restart (other than checking that it is refused
  unelevated) or end a process it did not start.
- The executables' writing commands are started by tests only where they must be refused (exit 8)
  or the command line is wrong (exit 2).
- A test that only an elevated runner can run prints a line of its own, and CI's
  "Check the elevated test paths" step looks for it, because a skipped test counts as passed.
- The `system` tests do it all for real. They run on CI's `system-x64` job, or on a throw-away
  virtual machine (docs/BUILDING.md), and refuse anywhere else.
- Don't run `--install`, `--uninstall`, `--migrate-v1`, `--setup-*`, `--uninstall-cleanup` or
  `--elevated` on your own PC to try a change. The one exception is the documented manual check
  of replacing 1.1 (docs/BUILDING.md, "Manual check: replacing 1.1 on your own PC"), after its
  backup step.

### Tests that start freshly built programs

Windows may refuse to start a program that was just built and is not signed: Smart App Control or
another Application Control policy (errors 4551, 577, 4580-4583; `pdft-run-medium` exits 126). A
test that starts one of our programs treats such a refusal as a **skip** (`GTEST_SKIP`, or a CTest
`SKIP_RETURN_CODE`), never as a pass and never as a plain failure, and does not start the refused
program again in the same run. `pdft::test::IsApplicationControlBlock` (`tests/support/medium_il.h`)
recognizes the errors. Where possible a test starts its own program again (a `--pdft-child` mode)
instead of a second one, because a program that is already running has been allowed.
