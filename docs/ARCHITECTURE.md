# How PdfThumbnails is built

A short map for contributors. The comments at the top of each header in `src/` have the details.

## The pieces

| File | What it is | Source |
|---|---|---|
| `PdfThumbnails.dll` | The thumbnail handler: a COM class File Explorer loads into its thumbnail process | `src/handler` |
| `PdfThumbnails-pdfium.dll` | PDFium from [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries), unchanged but renamed | pinned in `cmake/Pins.cmake` |
| `PdfThumbnailsSettings.exe` | The settings window; also runs the scheduled check and the administrator steps | `src/app` |
| `PdfThumbnailsCli.exe` | The command line ([CLI.md](CLI.md)) | `src/app` |

Both executables are built from the same code (`pdft_appcore`, `src/app/core`) and differ only in
how they talk to the user (`mode.h`). `src/common` holds what the handler and the app share: the
zoom limits and the crop maths. Every name, file name, registry key and GUID comes from
`cmake/ProjectInfo.cmake`.

## The handler

File Explorer asks for a thumbnail through the thumbnail cache, which creates the handler in an
isolated surrogate process (`dllhost.exe`), hands it the file as an `IStream`
(`IInitializeWithStream`) and asks for a bitmap (`IThumbnailProvider`). The handler:

1. refuses to do anything in a process above Medium integrity or with the Administrators group
   enabled;
2. reads the whole file into memory (at most 256 MiB, within a time budget), so PDFium never calls
   back into COM;
3. loads PDFium once, by its full path next to the DLL and under its private name, so no other
   extension in the same process can mix up the two copies;
4. renders page 1 under one process-wide lock (PDFium is not thread-safe), with annotations and
   form fields, within a time budget;
5. crops the page's longer side evenly by the zoom (`crop_math.h`), read from
   `HKEY_CURRENT_USER\Software\PdfThumbnails\Zoom` for every thumbnail.

If a fault happens while the PDFium lock is held, the handler ends the process rather than carry
on with a broken PDFium; Windows starts a new thumbnail process. The settings window's preview
sets a zoom of its own through a private interface (`preview_override.h`).

## Where things go

An install for all users:

```
C:\Program Files\PdfThumbnails\
  PdfThumbnailsSettings.exe, PdfThumbnailsCli.exe, LICENSE.txt, THIRD-PARTY-NOTICES.md, licenses\
  app\<file version>-<build id>\PdfThumbnails.dll, PdfThumbnails-pdfium.dll
  unins000.exe, unins000.dat (only when the installer installed it)
```

The thumbnail process keeps the DLLs it loaded open, so every build goes into a folder of its own
(`build_folder.h`; the build id is a hash of the two DLLs) and an older folder is removed once
nothing holds it. A file that is still in use is moved to a trash folder next to the install
folder and deleted at the next restart.

An install for one user goes to `%LOCALAPPDATA%\Programs\PdfThumbnails` with the same layout.

| What | Where |
|---|---|
| The COM class | `HKLM\Software\Classes\CLSID\{...}` for all users, or the same under HKCU |
| The `.pdf` thumbnail slots | per user, under `HKCU\Software\Classes\.pdf` and `SystemFileAssociations\.pdf` |
| The zoom and the last check | `HKCU\Software\PdfThumbnails` |
| The log | `%LOCALAPPDATA%\PdfThumbnails\Logs\PdfThumbnails.log` (never PDF names) |
| The scheduled check | Task Scheduler, `\PdfThumbnails\Check <user SID>`, one task per user |
| The entry in Installed apps | `HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\{B4584E00-A586-4C24-996D-6032BFDEDE2F}_is1`, written by the installer |

## The check engine

Every command and the window go through the same steps (`src/app/core`):

1. **Snapshot** (`snapshot.h`): everything the check looks at, read once and read-only: the
   registration, what Windows itself resolves for `.pdf`, File Explorer's settings, the user's
   settings, the task, PdfCraft, Smart App Control, what 1.x left behind. Optionally the
   **Explorer test** (`live_test.h`): one thumbnail of a generated PDF, made in a fresh thumbnail
   process the way File Explorer makes it.
2. **Evaluate** (`evaluate.h`, pure): the findings, each with its severity and what can fix it.
3. **Report** (`status_format.h`, `json.h`, pure): the text of `--status` and the window, or
   JSON.
4. **Repair** (`repair.h`): the plan the findings call for. The safe part (missing per-user
   pieces, empty slots) runs for the signed-in user; taking `.pdf` back from another handler only
   on request; the part for all users through one administrator step.

Registry access goes through `RegistryRoots`, so the tests run the whole engine against a private
key.

## The administrator boundary

Nothing runs elevated except one closed set of steps (`elevation.h`). An unelevated command
starts `PdfThumbnailsSettings.exe --elevated <step>` from Program Files through UAC and gets the
step's report back over a pipe that accepts only that child. The elevated process trusts nothing it
is given: it checks its own token, its exact command line and its own location, computes what it
writes from the installed files, and never touches the user's profile. Commands that change the
signed-in user's settings refuse to run elevated (exit 8), because they would change the wrong
user.

## Refreshing thumbnails

- **Refresh** (`refresh.h`): for each PDF in open folder windows, on the desktops, in the Windows
  Search index or in the folders named, the cache is asked which sizes it holds, and only those
  are extracted again, out of process. File Explorer is told the file changed. Nothing restarts.
- **Rebuild** (`rebuild.h`): ends File Explorer and the thumbnail processes of this session and
  user, deletes the cache files, lets Windows start File Explorer again and reopens its folder
  windows.

## The settings window

A modeless dialog (`src/app/gui`) whose rules are pure and unit-tested (`window_model.h`). Its
work runs on a worker thread (`window_jobs.h`). The window never loads the handler or parses a PDF
itself: the preview runs `PdfThumbnailsSettings.exe --render-preview` as a short-lived child that
renders two embedded sample PDFs, and Rebuild, Uninstall and Install run as child processes too.
It refuses to start as administrator.

## Start-up hardening

The first statement of each executable (`startup.h`) restricts where DLLs load from, so a DLL
planted next to the program or in the current folder is never loaded. No static initializer does
real work before that. The build adds the usual mitigations (`/sdl`, Control Flow Guard, ASLR
with high entropy, DEP, CET shadow stack compatibility on x64, Spectre mitigations on CI), and the
`gate` tests check every binary's imports and hardening flags.

## The installer

`installer/PdfThumbnails.iss` (Inno Setup), compiled by `cmake/Installer.cmake`, which passes it
every name from `cmake/ProjectInfo.cmake` and each architecture's build folder name. It installs
for all users only, and its rules:

- **It writes none of the product's registry values.** It lays the files down and calls the
  app's own steps: `--setup-machine --build <folder>` (elevated) and `--setup-user` (as the user
  who started Setup); its uninstaller calls `--uninstall-cleanup` first. Setup and the developer
  `--install` therefore cannot drift apart.
- **Never queue the `app\<id>` files for deletion at their install path** (no
  `uninsrestartdelete` or `restartreplace`). If the same build is installed again before the
  restart, Windows would delete the new files at boot and thumbnails would stop working. Files in
  use are moved to the trash folder first, and only that folder is queued, which is what the
  app's steps do. A DLL already in its build folder is never replaced (`onlyifdoesntexist`): its
  folder name says it is the same build.
- **No AppMutex.** The app holds its named objects while it waits for the operation lock, so a
  scheduled check would stop Setup or the uninstaller on a question. Running programs in the
  install folder are closed through the Restart Manager instead, and only `*.exe`, never the
  thumbnail process.
- **The uninstaller's own files stay** for it to delete: `--uninstall-cleanup` empties the
  install folder but for `unins000.*` while the installer's record exists.

## Tests

CTest labels: `unit` and `handler` (no side effects), `integration` (real APIs in private
places), `gate` (checks on the built files), `gui` (the real window, CI only), `system` (real
install and uninstall, CI or a throw-away VM only). [BUILDING.md](BUILDING.md#tests) says which are
safe to run on your own PC, and [TESTING.md](TESTING.md) has the manual passes.
