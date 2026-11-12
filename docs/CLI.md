# The command line

`PdfThumbnailsCli.exe` does everything the settings window does, for terminals and scripts.
`PdfThumbnailsSettings.exe` understands the same commands, and shows no console of its own.

```
PdfThumbnailsCli.exe <command> [options]
```

- Exactly one command per call, and the command comes first.
- Options are lowercase and belong to their command.
- Values are written `--name value` or `--name=value`.
- `-h`, `-?` and `/?` are other spellings of `--help`.
- Without a command, a double-clicked `PdfThumbnailsCli.exe` starts the settings window, and in a
  terminal it prints the help.

## Commands

| Command | What it does | Exit codes |
|---|---|---|
| `--status [--json] [--no-live-test] [--test-file <pdf>]` | Reports whether PDF thumbnails work, and why not. Changes nothing. | 0, 1, 6 |
| `--check [--quiet] [--no-repair] [--no-live-test]` | Checks, and repairs what is safe to repair. This is what the scheduled checks run. | 0, 1, 4, 6, 7, 8 |
| `--fix [--take-back] [--yes]` | Repairs what `--check` repairs. `--take-back` also takes the `.pdf` settings back from another thumbnail handler, after asking. The part for all users asks for administrator rights. | 0, 1, 3, 4, 5, 6, 7, 8 |
| `--zoom [<100-200>] [--no-refresh]` | Shows the zoom and where it comes from, or sets it and refreshes the PDF thumbnails File Explorer already made. | 0, 1, 3, 4, 5, 8 |
| `--checks <list>` | Chooses when the scheduled checks run. | 0, 4, 5, 6, 8 |
| `--refresh [--folder <folder>]... [--yes] [--full]` | Refreshes the PDF thumbnails File Explorer already made, without restarting it. `--full` rebuilds the whole thumbnail cache instead. | 0, 1, 2, 3, 4, 5, 6, 8 |
| `--uninstall [--rebuild-thumbnails] [--yes]` | Removes the product for all users of this PC, after asking. | 0, 1, 2, 3, 5, 6, 7, 8 |
| `--diagnostics [--copy \| --out <file>]` | Prints a report for bug reports, with personal details masked. | 0, 2, 5, 8 |
| `--migrate-v1 [--user-only \| --machine-only]` | Removes what version 1.x left behind, keeping its zoom. | 0, 1, 3, 5, 7, 8 |
| `--version`, `--help` | Print the version, or the help. | 0 |

Installers use `--setup-user [--checks <list>] [--zoom <100-200>]`, `--setup-machine [--build <folder>]` and
`--uninstall-cleanup`. Developer builds also have
`--install [--machine | --user] [--checks <list>] [--zoom <100-200>] [--force]`, which installs
from the folder the program is in ([Building](BUILDING.md#installing-a-build-developer-install)).

`<list>` is one of `pdfcraft,signin`, `pdfcraft`, `signin`, `none` or `default` (the same as
`pdfcraft,signin`).

### Examples

```bat
PdfThumbnailsCli.exe --status
PdfThumbnailsCli.exe --zoom 130
PdfThumbnailsCli.exe --refresh --folder "D:\Scans" --folder "E:\Archive"
PdfThumbnailsCli.exe --checks signin
PdfThumbnailsCli.exe --diagnostics --copy
```

### Notes on some commands

**`--status`** prints what is installed and for whom, the zoom, the `.pdf` settings, the
scheduled checks, and the result of the Explorer test: one thumbnail of a generated PDF, made in
a fresh thumbnail process the way File Explorer makes it. `--no-live-test` leaves that test out.
`--json` prints the same as one JSON object with `"schema": 1`; its field names stay the same
while the schema number does.

**`--check`** is the safe repair: it fills in what is missing for the signed-in user and never
takes the `.pdf` settings from another thumbnail handler. `--quiet` prints nothing (the exit code
and the log say what happened), and `--no-repair` only looks.

**`--zoom`** without a number shows the zoom and where it comes from (the saved setting, 1.x's
`settings.ini`, or the default). With a number it saves the zoom and then does what `--refresh`
does; `--no-refresh` only saves it, and new thumbnails use it from then on.

**`--refresh`.** Without `--folder` it covers the PDFs in open folder windows, on the desktops and
the ones Windows Search knows. PDFs anywhere else keep their old thumbnail until Windows makes
it again; name their folder with `--folder`, or use `--full`.

**`--refresh --full`** ends File Explorer and the thumbnail processes of this session and user,
deletes the thumbnail cache, lets Windows start File Explorer again and reopens its folder
windows. It asks first, or takes `--yes`.

**`--status --test-file <pdf>`** shows what File Explorer does with one particular file.

**`--uninstall`** removes the product for every user of the PC, not only for you: your own
settings first, then, after one administrator prompt, the part for all users. Files that are in
use are deleted at the next restart. The log goes too, so the command's output is the only
record.

**Questions.** Commands that ask (`--fix --take-back`, `--uninstall`, `--refresh --full`) need
`--yes` when there is no console to ask on, or they exit 2.

**Administrator rights.** Run every command from a normal prompt, not an administrator one. A
command that needs administrator rights for a part of its work asks for them itself, once, and
runs only that part with them. Started as administrator, the commands that change the signed-in
user's settings refuse (exit 8), and so does the settings window.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | OK |
| 1 | Problems remain, or a step is incomplete |
| 2 | Usage error |
| 3 | Cancelled: the administrator prompt was declined, the answer was no, or Ctrl+C stopped it before anything changed |
| 4 | Busy: another check or change did not finish within 30 seconds |
| 5 | Failed; the log has the error code |
| 6 | Not installed |
| 7 | Needs administrator rights, and nobody can be asked |
| 8 | Cannot run here: see below |

Exit 8 means the command refused to run where it could not work or would be unsafe: inside an app
container whose registry writes would not reach Windows, as administrator where the change belongs
to the signed-in user, or where File Explorer on the desktop belongs to another account. The
message says which, and nothing has been changed.

A scheduled check is `PdfThumbnailsSettings.exe --check --quiet`, so its exit code is the task's
"Last Run Result" in Task Scheduler.

## Output in scripts

On a console the commands print through the console itself, so every character shows. Piped or
redirected, the output is UTF-8 without a byte order mark, with CR LF line ends. The program's
own words are plain ASCII; only paths and names that came from your PC can contain other
characters. Windows PowerShell 5.1 decodes a program's output in the console's code page, so to
read such a path right, set this first:

```powershell
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
```

`--status --json` follows the same rules.

## Where things are

- The zoom: `HKEY_CURRENT_USER\Software\PdfThumbnails`, value `Zoom`.
- The log: `%LOCALAPPDATA%\PdfThumbnails\Logs\PdfThumbnails.log`. It never holds PDF names.

## Coming from 1.x

| 1.x | 2.0 |
|---|---|
| `status` | `--status` |
| `status -TestFile x` | `--status --test-file x` |
| `install [-Machine]` | the installer, or `--install [--machine \| --user]` in a developer build |
| `-AutoRepair` / `-NoAutoRepair` | `--checks default` / `--checks none` |
| `repair` | `--check` (taking settings back is `--fix --take-back`) |
| `repair -Quiet` | `--check --quiet` |
| `zoom [n]` | `--zoom [n]` |
| `refresh` | `--refresh --full` (it now asks first, and touches only your own session) |
| `uninstall` | `--uninstall` |
| `machine-register`, `machine-unregister` | none: the commands ask for administrator rights themselves |

1.x's exit codes were 0 and 1 only. A script that tested for "not 0" keeps working; one that
tested for 1 should now test for "not 0".
