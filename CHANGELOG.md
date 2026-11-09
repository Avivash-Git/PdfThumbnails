# Changelog

## 2.0.0 (not released yet)

### New

- A settings window: zoom with a live preview, when the checks run, status with a Fix link, Check
  now, Rebuild thumbnails, Open log, Copy diagnostics and Uninstall.
- A new zoom now reaches the PDF thumbnails File Explorer already made, without restarting it.
- Rebuilding the whole thumbnail cache brings back the folder windows that were open.
- `--diagnostics` and Copy diagnostics: a report for bug reports with personal details masked.
- An ARM64 build.

### Changed

- The PowerShell manager is replaced by `PdfThumbnailsSettings.exe` and `PdfThumbnailsCli.exe`.
  [The command line](docs/CLI.md) lists the commands and how the 1.x ones map to them.
- The zoom is stored in `HKEY_CURRENT_USER\Software\PdfThumbnails`. A zoom from 1.x is kept.
- Commands have distinct exit codes (0 to 8) instead of 0 and 1.
- A check never takes the `.pdf` setting from another thumbnail handler. Taking it back is a
  request of its own (`--fix --take-back`, or the Fix link), and it asks first.
- Each user has their own check task, in a task folder of its own, instead of one shared name.
- Every command writes to a log under `%LOCALAPPDATA%\PdfThumbnails\Logs`. It never holds PDF names.
- `--uninstall` says that it removes the product for all users, and can also clear the thumbnail
  cache.
- Built with MSVC, with Control Flow Guard, CET on x64 and a static C runtime.

### Fixed

- Rebuilding the cache ended File Explorer and thumbnail processes in every session. It now
  touches only the current user's in the current session.
- The Explorer test reported a false failure when run as administrator. It is now skipped there,
  and the status says why.
- A clean-up left pending at sign-in could delete a fresh install.
- A build folder was trusted because the files had the right names. It is now reused only when
  their hashes match.
- In a terminal inside an app container, changes could silently never reach Windows. Commands now
  detect that and refuse (exit 8), and the settings window starts again outside the container.
