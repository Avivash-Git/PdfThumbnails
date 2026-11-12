# Roadmap

PdfThumbnails 2.0 is being built in milestones. Dates are not promised: this is a spare-time
project.

| Milestone | What | State |
|---|---|---|
| 1. The build | CMake and MSVC, CI building and testing x64 and ARM64 | Done |
| 2. The settings app | The settings window and the command line replace 1.x's PowerShell manager; the scheduled check; refreshing thumbnails after a zoom change | Done |
| 3. An installer | An Inno Setup installer with an entry in Installed apps, that replaces a 1.x install | Done |
| 4. Code signing | Signed builds, so Smart App Control and SmartScreen stop blocking them; a test pass on a PC with Smart App Control on | In progress: the signing steps are in the build and rehearsed on CI; waiting for a certificate |
| 5. Release 2.0.0 | The first download for everyone, then a winget package | Planned |

Until milestone 5 there is no download: build from source ([BUILDING.md](BUILDING.md)).

## Later ideas

- Choosing which part of the page a zoom keeps (today it always keeps the centre).

Ideas and votes are welcome as [feature requests](https://github.com/Avivash-Git/PdfThumbnails/issues/new/choose).
