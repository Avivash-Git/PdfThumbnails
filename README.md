# PdfThumbnails

First-page thumbnails for PDF files in Windows File Explorer, working alongside
[PdfCraft](https://github.com/storytold/pdfcraft) or any other PDF app.

**Status: pre-release.** There is an installer, but no signed download yet, so for now you build
it from source. See [Limits](#limits) for what that means with Smart App Control.

## What you get

- Thumbnails of the first page, drawn with [PDFium](https://pdfium.googlesource.com/pdfium/), in
  File Explorer, on the desktop and in File Open dialogs.
- A **zoom** that crops the margins so that a page fills the square.
- A **settings window** with a live preview of the zoom, a status that says whether thumbnails
  really work, and a Fix link when they don't.
- A **scheduled check** that repairs the setup after PdfCraft updates and at sign-in.
- A [command line](docs/CLI.md) that does everything the window does.

It changes nothing in PdfCraft, and it is not affiliated with it.

## Why it isn't a PdfCraft patch

File Explorer doesn't ask the PDF app for thumbnails. It asks a *thumbnail handler*, a small DLL
registered for the `.pdf` file type, and the PDF app never even runs while a folder is drawn.
PdfCraft doesn't ship a handler, so PDFs show a plain icon. This project is that handler. It
leaves PdfCraft's own settings alone, so PdfCraft updates don't disturb it.

## Install

**The installer**, `PdfThumbnails-<version>-setup.exe`, installs for all users of the PC (it asks
for administrator rights once) and puts PdfThumbnails in Settings > Apps > Installed apps and the
Start menu. Every other user of the PC is set up at their next sign-in. The same setup serves x64 and ARM64 PCs. It replaces a 1.x install and keeps its zoom,
and a newer setup installs over an older one and keeps your settings. Until the first release
there is no download: [build the installer](docs/BUILDING.md#the-installer) from source, or install
straight from a build as below. For a silent install: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`.

**From source**, without the installer: you need Visual Studio 2026, version 18.11 or later (the
Build Tools are enough), with the components in `.vsconfig`. From a Command Prompt in the
repository folder:

```bat
scripts\build.cmd x64-release
cmake --install out\build\x64-release --prefix out\stage\runtime --component runtime
out\stage\runtime\PdfThumbnailsCli.exe --install --machine
```

The last command asks for administrator rights once and installs for all users of the PC. Run it
from a normal prompt, not an administrator one. [docs/BUILDING.md](docs/BUILDING.md) has the
details, including ARM64 builds and installing for one user only.

## Use it

Start **PdfThumbnails** from the Start menu (after a developer install: `PdfThumbnailsSettings.exe`
in `C:\Program Files\PdfThumbnails`).

![The settings window: a zoom slider with a preview of a document and a slide, the two checks, and
the status](docs/images/settings-window.png)

**Zoom.** File Explorer fits every thumbnail into a square, so a portrait page leaves empty bands
on both sides. The zoom crops the longer side evenly, so the page fills more of the square:

| Zoom | Cropped from each end (A4 portrait) | |
| --- | --- | --- |
| 100 % | nothing | the whole page (the default) |
| 120 % | about 8 % | mostly margins and running headers |
| 142 % | about 15 % | a square crop; higher values crop no further for A4 |

The same document at 100 % and at 130 %, and a folder at 130 %:

![A document's thumbnail at zoom 100 %: the whole page with white bands at the
sides](docs/images/zoom-100.png)
![The same thumbnail at zoom 130 %: the page fills the square](docs/images/zoom-130.png)

![Eight PDF thumbnails in File Explorer at zoom 130 %: four documents and four
slides](docs/images/thumbnails-130.png)

Save applies a new zoom to the thumbnails File Explorer already made, for PDFs in open folder
windows, on the desktop and in the Windows Search index. **Rebuild thumbnails** starts the whole
thumbnail cache afresh, which restarts File Explorer and reopens its folder windows.

**Staying set up.** The check task repairs what is safe to repair: empty settings and missing
per-user pieces. If another program has taken over `.pdf` thumbnails it leaves that alone, because
you may have chosen it, and the window offers to take them back.

**Uninstall** from Settings > Apps > Installed apps, from the window, or with
`PdfThumbnailsCli.exe --uninstall`. The window's Uninstall (with "Also clear the thumbnail cache")
and `--uninstall --rebuild-thumbnails` also rebuild the thumbnail cache afterwards, which restarts
File Explorer. From Installed apps, thumbnails File Explorer already made stay until it makes them
again.

## Limits

- **Smart App Control.** The program isn't code-signed yet, so with Smart App Control on, Windows
  may block it. The status window says when that is happening. A build you made yourself usually
  isn't blocked, because it doesn't carry the mark Windows puts on downloads.
- These files keep the plain icon: PDFs that need a password to open, dynamic XFA forms made for
  Adobe Reader, files over 256 MiB, and files that take too long to draw.
- 32-bit programs' Open and Save dialogs show icons.
- Use one PDF thumbnailer at a time. PowerToys' PDF thumbnails and SumatraPDF's previewer use the
  same setting.
- PdfCraft may ship its own handler one day (storytold/pdfcraft#1370). If it does, that one takes
  precedence and the status says so.
- An install for one user only (`--install --user`) may not be used by File Explorer: on some
  Windows 11 builds its thumbnail process loads only what is installed for all users. The status
  says so when it happens. Install for all users, which is the default.
- The ARM64 build passes its tests on an ARM64 PC, but it has not been used day to day yet.

## Privacy

PdfThumbnails never uses the network: no telemetry, no update check, no download. It reads a PDF
only to draw its first page, and keeps nothing of it. The log and the diagnostics report hold no
PDF names, and the report masks your user name, PC name and profile folder.

## How it works

The handler implements `IInitializeWithStream` and `IThumbnailProvider`, and runs in Windows'
isolated thumbnail process, so a crash there cannot take File Explorer down. It refuses to run with
administrator rights, reads the file itself, and renders page 1 with PDFium, which ships beside it
under a private name and is loaded only by its full path.

The settings program never parses a PDF in its own window: its preview renders two built-in samples
in a short-lived separate process. The zoom lives in
`HKEY_CURRENT_USER\Software\PdfThumbnails\Zoom`, and the log in
`%LOCALAPPDATA%\PdfThumbnails\Logs`, which never holds PDF names.

## More

- [The command line](docs/CLI.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md): what to check when thumbnails don't show
- [How it is built](docs/ARCHITECTURE.md), [what comes next](docs/ROADMAP.md)
- [Building and testing](docs/BUILDING.md), [manual tests](docs/TESTING.md)
- [Changes since 1.x](CHANGELOG.md): what behaves differently, and what happens to a 1.x install
- [Contributing](CONTRIBUTING.md), [code of conduct](CODE_OF_CONDUCT.md),
  [reporting security problems](SECURITY.md)
- [Maintaining the project](docs/MAINTAINING.md)

## Licenses

This project is MIT (`LICENSE`). PDFium and its components are listed in `THIRD-PARTY-NOTICES.md`.
