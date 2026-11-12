# Changelog

## 2.0.0 (not released yet)

### New

- A settings window: zoom with a live preview, when the checks run, status with a Fix link, Check
  now, Rebuild thumbnails, Open log, Copy diagnostics and Uninstall.
- A new zoom now reaches the PDF thumbnails File Explorer already made, without restarting it.
- Rebuilding the whole thumbnail cache brings back the folder windows that were open.
- `--diagnostics` and Copy diagnostics: a report for bug reports with personal details masked.
- `--status --json`, for scripts.
- An ARM64 build.
- An installer: one setup for x64 and ARM64 PCs that installs for all users, with an entry in
  Installed apps and a Start menu shortcut. It upgrades an earlier 2.x in place and keeps every
  user's settings, and its uninstaller removes the product for every user who is signed in.
  Silent: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`.
- Every user of the PC is set up at their next sign-in (Active Setup), not only the one who
  installed it. In 1.x each user had to run `install` for themselves.

### Changed

These are the places where 2.0 behaves differently from 1.1 on purpose.

- **The manager.** The PowerShell manager (`PdfThumbnails.cmd` and `PdfThumbnails.ps1`) is
  replaced by `PdfThumbnailsSettings.exe` and `PdfThumbnailsCli.exe`.
  [The command line](docs/CLI.md) lists the commands and how the 1.x ones map to them.
- **Installing.** 1.x was installed with `PdfThumbnails.cmd install`; 2.0 has an installer (or
  `--install` in a developer build), and appears in Installed apps.
- **Upgrading.** Installing 2.0 over 1.x keeps the zoom and removes 1.x: its build folders, its
  task, its sign-in clean-up, and the files it copied into
  `%LOCALAPPDATA%\Programs\PdfThumbnails` (each only while its contents are still 1.x's own).
- **The zoom** is stored in `HKEY_CURRENT_USER\Software\PdfThumbnails` (value `Zoom`) instead of
  `settings.ini`. A zoom from 1.x is kept: it is copied over, and until then `settings.ini` is
  still read.
- **Exit codes.** Commands have distinct exit codes (0 to 8) instead of 0 and 1. A check exits 1
  when another thumbnail handler holds the `.pdf` setting; 1.x's `repair` exited 0 then.
- **Repairs.** A check never takes the `.pdf` setting from another thumbnail handler. Taking it
  back is a request of its own (`--fix --take-back`, or the Fix link), and it asks first. In 1.x,
  running `install` again took it back without asking.
- **The scheduled check.** Each user has their own task, `\PdfThumbnails\Check <SID>`, instead of
  the one name `PdfThumbnails auto-repair` that the PC's users shared. It runs
  `PdfThumbnailsSettings.exe --check --quiet` instead of a PowerShell script, and its two
  triggers (after a PdfCraft update, at sign-in) can be chosen separately (`--checks`).
- **Administrator rights.** The commands and the window are started normally and ask for
  administrator rights themselves, for the part for all users only. That part is run by the copy
  in `C:\Program Files\PdfThumbnails`, never by files in your profile. Started as administrator,
  the window and the commands that change your own settings refuse.
- **A new zoom** is applied to the PDF thumbnails File Explorer already made. In 1.x it showed
  only on thumbnails made afterwards, or after `refresh`.
- **Rebuilding the cache** (`--refresh --full`, 1.x's `refresh`) asks first.
- **`--uninstall`** says that it removes the product for all users, removes every user's check
  task and settings, and can also rebuild the thumbnail cache (`--rebuild-thumbnails`; the
  window's Uninstall offers it, checked).
- **The log.** Every command writes to `%LOCALAPPDATA%\PdfThumbnails\Logs\PdfThumbnails.log`,
  which never holds PDF names. 1.x logged only its scheduled repairs, to `repair.log` in the
  program folder.
- **The status** has a line of its own for a wrong threading model of the class, marks which
  problems stop thumbnails and which do not, and says "unknown" where it cannot read Smart App
  Control's state or log instead of counting no blocks.
- **The build.** MSVC instead of MinGW, with Control Flow Guard, CET on x64 and a static C
  runtime. The version is 2.0.0.0, so build folders are named `app\2.0.0.0-<build id>`.
- **PDFium** is `chromium/8152` (PDFium 159), newer than 1.1's `chromium/8125`.

### Fixed

- Rebuilding the cache ended File Explorer and thumbnail processes in every session. It now
  touches only the current user's in the current session.
- The Explorer test reported a false failure when run as administrator. It is now skipped there,
  and the status says why.
- A clean-up left pending at sign-in could delete a fresh install.
- A build folder was trusted because the files had the right names. It is now reused only when
  their hashes match.
- Of several builds in a folder, the one registered again was the one written last. It is now the
  one with the highest version.
- In a terminal inside an app container, changes could silently never reach Windows. Commands now
  detect that and refuse (exit 8), and the settings window starts again outside the container.

### Removed

- `PdfThumbnails.cmd` and `PdfThumbnails.ps1`, and with them the commands `machine-register` and
  `machine-unregister`.
- `settings.ini` and `repair.log` (see The zoom and The log above).
- The MinGW build scripts (`build.cmd`, `build.ps1`). `scripts\build.cmd` builds with MSVC and
  CMake ([Building](docs/BUILDING.md)).
