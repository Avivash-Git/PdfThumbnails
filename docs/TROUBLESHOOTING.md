# Troubleshooting

Start with the status. From a normal (not administrator) prompt:

```bat
"C:\Program Files\PdfThumbnails\PdfThumbnailsCli.exe" --status
```

It checks every piece File Explorer needs, makes one test thumbnail the way File Explorer does,
and marks each problem with `!!` and what fixes it. The settings window shows the same, with a
Fix link. Most problems below show up there by name.

## PDFs show a plain icon

- **The status says a problem remains.** Run `PdfThumbnailsCli.exe --fix`. It repairs what is
  missing and asks for administrator rights only if the part for all users needs it.
- **Another program has taken over PDF thumbnails** (a `.pdf slot` line points at another
  handler).
  PowerToys, SumatraPDF, Adobe Acrobat or a PDF app's own handler use the same setting, and only
  one can win. The check leaves it alone, because you may have chosen it. To take it back:
  `PdfThumbnailsCli.exe --fix --take-back`, or the window's link.
- **"Always show icons, never thumbnails" is on.** File Explorer > **...** > **Options** >
  **View**, and clear it. A group policy can turn thumbnails off too; the status says so.
- **Installed for one user only.** On some Windows 11 builds File Explorer's thumbnail process
  uses only what is installed for all users. The status says when that happens. Install for all
  users instead.
- **Smart App Control or another Application Control policy blocked it.** The builds are not
  code-signed yet. The status has a Smart App Control line and says when it blocked the handler.
- **A restart is pending.** After an uninstall or an update, files that were in use are removed at
  the next restart; the status then has a Restart line.
- **Only some files.** These always keep the plain icon: PDFs that need a password, dynamic XFA
  forms, files over 256 MiB, and files that take more than a few seconds to draw. To see what
  happens with one file: `PdfThumbnailsCli.exe --status --test-file "<the PDF>"`.
- **Only in some programs.** The Open and Save dialogs of 32-bit programs can't load a 64-bit
  handler, so they show icons.

## The zoom changed but old thumbnails stay

Windows keeps thumbnails in a cache. Saving a zoom refreshes the PDFs in open folder windows, on
the desktop and in the Windows Search index. For other folders:

```bat
PdfThumbnailsCli.exe --refresh --folder "D:\Scans"
```

If that doesn't help, **Rebuild thumbnails** in the window, or `PdfThumbnailsCli.exe --refresh
--full`, starts the whole cache afresh. It restarts File Explorer and reopens its folder windows.

## A command refuses to run (exit code 8)

The message says why, and nothing has been changed:

- **Started as administrator.** Run it from a normal prompt. A command that needs administrator
  rights for a part of its work asks for them itself.
- **Inside an app container whose registry is virtualized**, as in some terminals that are
  themselves packaged apps. Its changes would never reach Windows. Use a Command Prompt started
  from the Start menu, or a terminal that is not part of an app package.
- **File Explorer on the desktop belongs to another account.** Run it as the user who is signed
  in.

Exit code 4 means another check or change was still running; try again in a moment. The other
codes are in [CLI.md](CLI.md#exit-codes).

## Setup did not finish

Setup's log is in `%TEMP%`, named `Setup Log <date> #<n>.txt`; it has the output and exit codes
of the two steps Setup runs. Its own exit codes:

- **200 + n, with a message that Setup was started as administrator** (208). The part for all
  users is installed, but your own account is not set up. From a normal (not administrator)
  prompt, run `"C:\Program Files\PdfThumbnails\PdfThumbnailsCli.exe" --setup-user`.
- **Another 200 + n.** Your own account's step failed. Open PdfThumbnails from the Start menu: the
  status says what is missing, and Fix repairs it.
- **100 + n.** The part for all users is not complete. Run Setup again; if that fails too, open
  a bug report with the log.

## Garbled characters in PowerShell

The program's own words are plain ASCII. A path with other characters (Greek, Hebrew, Japanese,
...) can look garbled when you pipe the output in Windows PowerShell 5.1, which decodes it in the
console's code page. Set this first:

```powershell
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
```

## The scheduled check

`PdfThumbnailsCli.exe --status` shows the Checks line: whether the task runs after PdfCraft
updates and at sign-in, and when it last ran. Its exit code is the task's "Last Run Result" in
Task Scheduler (`\PdfThumbnails`). `PdfThumbnailsCli.exe --checks default` registers it again.

## Reporting a problem

1. Run `PdfThumbnailsCli.exe --diagnostics --copy`. It puts a report on the clipboard with your
   user name, PC name and profile folder masked, and no PDF names. Read it before you share it.
2. [Open a bug report](https://github.com/Avivash-Git/PdfThumbnails/issues/new/choose) and paste
   it. Please don't attach your own PDFs; a made-up PDF that shows the same problem is best.

The log is `%LOCALAPPDATA%\PdfThumbnails\Logs\PdfThumbnails.log`. Security problems go to the
Security tab instead ([SECURITY.md](../SECURITY.md)).
