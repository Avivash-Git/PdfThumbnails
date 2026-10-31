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

### Resource scripts are UTF-8

Every `.rc` and `.rc.in` file is UTF-8 and starts with

```
#pragma code_page(65001)
```

on its first line. Without it, the resource compiler reads the file in the ANSI code page of the
build machine, and strings such as "Rebuild thumbnails…" or "0.14.0 → 0.15.0" come out garbled (or
differently on another machine). A unit test loads such strings from the built executables and
compares them with the exact text.

### Tests that start freshly built programs

Windows may refuse to start a program that was just built and is not signed: Smart App Control or
another Application Control policy (errors 4551, 577, 4580-4583; `pdft-run-medium` exits 126). A
test that starts one of our programs treats such a refusal as a **skip** (`GTEST_SKIP`, or a CTest
`SKIP_RETURN_CODE`), never as a pass and never as a plain failure, and does not start the refused
program again in the same run. `pdft::test::IsApplicationControlBlock` (`tests/support/medium_il.h`)
recognizes the errors. Where possible a test starts its own program again (a `--pdft-child` mode)
instead of a second one, because a program that is already running has been allowed.
