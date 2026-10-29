# PdfThumbnails

First-page thumbnails for PDF files in Windows Explorer, like Foxit used to show, working alongside PdfCraft or any other PDF app.

## Why this is not a PdfCraft patch

Explorer does not ask the PDF app for thumbnails. It asks a *thumbnail handler*, a small COM DLL registered for the `.pdf` file type, and `pdfcraft.exe` never even runs when Explorer draws a folder. Foxit installed such a handler (`FoxitThumbnailHndlr`); PdfCraft 0.14.0 does not. This project is that missing DLL, installed separately from PdfCraft:

- PdfCraft is installed by the ArtCraft Launcher (`msiexec /passive`, a plain MSI upgrade), which writes only PdfCraft's own `PdfCraft.Document`, `Applications\pdfcraft.exe` and context-menu keys. It never touches the keys or files used here, so **nothing needs re-applying after a PdfCraft update**.
- PdfCraft already sets `TypeOverlay=""` on its `PdfCraft.Document` type, so thumbnails are not stamped with the PdfCraft icon.

## Use

The installed manager is `%LOCALAPPDATA%\Programs\PdfThumbnails\PdfThumbnails.cmd`. Double-clicking it shows the status.

```
PdfThumbnails.cmd status                    what Explorer uses for .pdf thumbnails, plus a live test
PdfThumbnails.cmd status -TestFile x.pdf    also extract that file's thumbnail through Explorer (bypasses the cache)
PdfThumbnails.cmd install [-Machine] [-AutoRepair]
PdfThumbnails.cmd repair [-AutoRepair | -NoAutoRepair]
PdfThumbnails.cmd zoom [percent]            show or set the magnification (see below)
PdfThumbnails.cmd refresh                   rebuild Explorer's thumbnail cache (restarts Explorer)
PdfThumbnails.cmd uninstall                 remove everything
```

`install` registers everything per-user, so no administrator rights are needed. `install -Machine` registers the COM class machine-wide instead, with the DLLs in admin-protected `C:\Program Files\PdfThumbnails`; that costs one UAC prompt. The `.pdf` association, the settings and the auto-repair task are always per-user.

Run the manager from a normal (non-administrator) prompt; `-Machine` asks for administrator rights itself. Per-user registry changes go through WMI, so they reach your real profile even from a terminal inside an MSIX-packaged app, whose own registry view is virtualized. In such a terminal `status` skips the checks that go through Windows' association code and tells you so; double-click the `.cmd` for those.

## Zoom

Explorer fits every thumbnail into a square. A portrait page is narrower than that square, so it leaves empty bands left and right. `zoom` magnifies the page by cropping its longer side evenly: the top and bottom of a portrait page, or the left and right of a landscape one. The rest then fills more of the square.

| Zoom | Cropped from each end (A4 portrait) | Notes |
| --- | --- | --- |
| 100 | none | whole page (the default) |
| 120 | about 8% | mostly margins and running headers |
| 141 | about 15% | square crop; higher values crop no further for A4 |

The setting lives in `%LOCALAPPDATA%\Programs\PdfThumbnails\settings.ini` (`[Thumbnails] Zoom=120`) and applies to thumbnails extracted from then on. Explorer keeps thumbnails it has already made, so run `refresh` afterwards to redo those. `refresh` restarts Explorer: the taskbar blinks and open File Explorer windows close.

## Where things are, and updating

- **Manager:** `%LOCALAPPDATA%\Programs\PdfThumbnails\` holds `PdfThumbnails.cmd/.ps1`, `settings.ini`, `repair.log`, `README.md`, the licenses, and in `bin\` a copy of the installed build.
- **DLLs Explorer loads:** `...\PdfThumbnails\app\<version>-<hash>\` (per-user), or `C:\Program Files\PdfThumbnails\app\<version>-<hash>\` (with `-Machine`).
- **Updating after a rebuild:** run the new build's `dist\PdfThumbnails\PdfThumbnails.cmd install` with the same options you used before (here `install -Machine -AutoRepair`). On a machine-wide install, a plain `install` updates the machine-wide copy anyway; `uninstall` first if you want to switch to per-user.

## How it works

- `PdfThumbnails.dll` implements `IInitializeWithStream` + `IThumbnailProvider` and runs in Explorer's isolated thumbnail process (`dllhost.exe /Processid:{AB8902B4-…}`). A crash there cannot take Explorer down. The handler refuses to run inside elevated processes, so administrator tools show the plain icon.
- It reads the file (up to 256 MiB) and renders page 1 with [PDFium](https://pdfium.googlesource.com/pdfium/), the engine in Chrome, which started life as Foxit's own engine. Annotations, filled-in form fields and visible signatures are included; `/Rotate` and the crop box are honoured. The page is drawn on white and keeps its aspect ratio, apart from the zoom crop.
- **Time limits:**
  - 8 s to read the file.
  - Up to 15 s from the request to wait while other PDFs are being drawn.
  - 8 s to draw the page.

  Opening a document and parsing its first page cannot be interrupted, so a very complex page can run longer.
- PDFium ships next to the DLL as `PdfThumbnails-pdfium.dll` (a private name, so it cannot clash with other copies of PDFium) and is loaded only by full path.
- **Registration:**
  - The COM class `{0C3534C4-5B7A-48AB-828D-1CD660DC447F}` (InprocServer32, `ThreadingModel=Apartment`), under `HKCU\Software\Classes\CLSID`, or under `HKLM` with `-Machine`.
  - `HKCU\Software\Classes\.pdf\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}`
  - `HKCU\Software\Classes\SystemFileAssociations\.pdf\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}`. This second slot survives tools such as PowerToys or SumatraPDF that delete the first one on uninstall.
- Each build is installed to its own `app\<version>-<hash>` folder, because the thumbnail process keeps the loaded DLLs open. Old folders are deleted by later installs, by repair (per-user folders) and by uninstall, once nothing has them loaded.

## Auto-repair (optional)

`install -AutoRepair` adds a per-user scheduled task, **PdfThumbnails auto-repair**. It runs `repair -Quiet` at sign-in, and 30 seconds after Windows Installer reports that PdfCraft was installed or updated (Application log, MsiInstaller event 1033 for "PdfCraft").

- **What it fixes:** missing per-user pieces, such as the COM class or an empty `.pdf` slot.
- **What it leaves alone:** a slot that now holds another thumbnail handler, since you may have chosen that one. `install` takes the slot back.
- **What it can't fix:** a damaged machine-wide part, which needs admin. It logs an error instead.

Results go to `repair.log`. To turn the task off, run `repair -NoAutoRepair`.

## Limits and caveats

- **Smart App Control.** The DLLs are not code-signed, so with Smart App Control **On** Windows *may* block them. New or changed PDFs would then show icons, while thumbnails Explorer already cached keep showing, so check with `status` rather than by looking at a folder.
  - On the maintainer's PC, a copy built there kept working with Smart App Control On, also after a restart. That includes the PDFium DLL, which `build.ps1` downloads unsigned from GitHub. The likely reason (not confirmed) is that neither file carries the "downloaded from the internet" mark that a browser adds. Files with that mark, such as ones extracted from a ZIP downloaded in a browser, are checked strictly.
  - `status` tells you which case you're in. It runs a live test and reads Windows' block log, and only reports a problem when there is evidence of one.
  - If it is blocking, you can turn Smart App Control off. Or run `PdfThumbnails.cmd uninstall` and enable the Microsoft-signed PDF thumbnails in PowerToys (File Explorer add-ons). Uninstall first: otherwise this handler's per-user `.pdf` entry outranks PowerToys.
- These files keep the plain icon:
  - PDFs that need a password to open (files with only an owner password render fine)
  - dynamic XFA forms made for Adobe Reader (other viewers show only a "Please wait..." page)
  - files over 256 MiB
  - files that hit one of the time limits above
- 64-bit only: the Open/Save dialogs of 32-bit apps show icons. The manager itself refuses to run in 32-bit PowerShell; `PdfThumbnails.cmd` always starts the 64-bit one.
- Use only one PDF thumbnailer. Don't also enable the PowerToys PDF thumbnails or SumatraPDF's previewer; they share the same registry slot.
- PdfCraft has an open pull request (storytold/pdfcraft#1370) for its own thumbnail handler, registered on `PdfCraft.Document`. If a release ships it, that handler takes precedence and `status` says so; you can then keep it or uninstall this one.

## Build from source

```
winget install --id MartinStorsjo.LLVM-MinGW.UCRT --exact --version 23.1.4-20261020
build.cmd
dist\PdfThumbnails\PdfThumbnails.cmd install -Machine -AutoRepair
```

- `build.ps1` downloads PDFium `chromium/8125` (`pdfium-win-x64.tgz` from [bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries)) and checks its SHA-256. It then compiles the DLL statically, so the DLL depends only on Windows DLLs. Finally it checks the exports, the imports, every PDFium function the DLL binds and the license notices.
- Tests:
  - `powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-test-pdfs.ps1 -OutDir <dir>` writes synthetic edge cases.
  - `dist\tools\thumbtest.exe` renders through the DLL directly (`direct`, `stress`) or through Explorer's pipeline (`cache`, `cachenew`, `cacheinproc`, `shell`).
  - Set `PDFTHUMBNAILS_SETTINGS` to another `settings.ini` to test zoom levels in `direct` mode.
- To remove the toolchain afterwards: `winget uninstall --id MartinStorsjo.LLVM-MinGW.UCRT --exact --purge`. The installed handler does not need it.

## Why not reverse-engineer Foxit (REA)?

[REA](https://github.com/morluto/rea) drives Ghidra/IDA for an AI agent. Pointed at Foxit, it would only give us decompiled code, and we can neither use nor need it:

- Foxit's license (§2.1) forbids reverse engineering.
- Foxit is not installed.
- The thumbnail-handler contract is fully documented by Microsoft.
- Foxit's rendering engine is open source as PDFium, which is what this project uses.

## Licenses

- This project: MIT (`LICENSE`).
- PDFium: BSD-3-Clause / Apache-2.0, plus the notices of its third-party components (FreeType, ICU, libjpeg-turbo, libpng, zlib and others), in `licenses\`.
- The pdfium-binaries build: MIT.
