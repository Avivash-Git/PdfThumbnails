# Roadmap

PdfThumbnails 2.0 is being built in milestones. Dates are not promised: this is a spare-time
project.

| Milestone | What | State |
|---|---|---|
| 1. The build | CMake and MSVC, CI building and testing x64 and ARM64 | Done |
| 2. The settings app | The settings window and the command line replace 1.x's PowerShell manager; the scheduled check; refreshing thumbnails after a zoom change | Done |
| 3. An installer | An Inno Setup installer with an entry in Installed apps, that replaces a 1.x install | Done |
| 4. Release 2.0.0 | The first download for everyone: the setup on GitHub Releases, unsigned | Next |
| 5. Code signing | Signed releases through SignPath Foundation's open-source programme, so Smart App Control and SmartScreen stop blocking them; a test pass on a PC with Smart App Control on; then a winget package | Planned: the build can already sign and is rehearsed on CI. The programme accepts only projects that are already released, so the project applies after 2.0.0 ([CODE_SIGNING.md](CODE_SIGNING.md)) |

Until milestone 4 there is no download: build from source ([BUILDING.md](BUILDING.md)).

Until releases are signed (milestone 5), the setup is unsigned, which costs users this:

- **SmartScreen** shows "Windows protected your PC" when the setup starts; **More info**, then
  **Run anyway**, lets it run.
- **Smart App Control**, where it is on, blocks the downloaded setup, and Windows offers no way to
  allow it. On such a PC, build from source, or wait for a signed release.

## Later ideas

- Choosing which part of the page a zoom keeps (today it always keeps the centre).

Ideas and votes are welcome as [feature requests](https://github.com/Avivash-Git/PdfThumbnails/issues/new/choose).
