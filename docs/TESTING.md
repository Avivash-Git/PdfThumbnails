# Testing PdfThumbnails by hand

Most tests run by themselves (see [Tests](BUILDING.md#tests)). This page is for the checks that
need someone at the PC: what they do, what they change on it, and how to stop them.

## The thumbnail refresh experiment

`thumbtest experiment` finds out how `--refresh` (the PDF-only refresh) should make the PDF
thumbnails File Explorer already made again, and how it should tell File Explorer, so that a new
zoom shows without restarting File Explorer. It works on your own thumbnail cache and File
Explorer, with synthetic PDFs only, and takes about 30 to 40 minutes with you at the PC.

It decides two things:
- **Pass A**, automatically: after a refresh, the thumbnails the cache held before are still
  there and show the new zoom. A thumbnail's size shows its zoom (an A4 page at 256 pixels is
  181 x 256 at 100 % and 256 x 256 at 142 %), so the experiment judges them by the sizes the cache
  reports, without looking at pixels. Five ways of making a thumbnail again are tried; the first
  that passes is used for the rest of the run. Pass A also needs the product itself,
  `PdfThumbnailsCli --refresh --folder`, to pass from a Windows Terminal tab.
- **Pass B**, by eye: an open folder window shows the new thumbnails within 10 seconds without a
  restart of File Explorer, and so do the Desktop and another program's File Open dialog. You say
  what you see.

### Before you start

- A release build: `scripts\build.cmd x64-release`. The binaries are in
  `out\build\x64-release\bin\`. The experiment uses the handler that is installed, so nothing is
  installed for it; `PdfThumbnailsCli.exe --status` must say healthy.
- Close the File Explorer windows that show folders with your own PDFs, and don't browse PDFs
  while it runs: the zoom is 100 % and then 142 % meanwhile, and the PDF thumbnails File Explorer
  makes meanwhile keep that zoom.
- If Smart App Control is on, the experiment says so and asks first, and it stops at the first
  block (each block shows a notification).

### Running it

1. Press Win+R, type `cmd` and press Enter. This Command Prompt has no app package above it, even
   where Windows Terminal shows it; a Windows Terminal tab opened with + is below one, and the
   experiment refuses to run there (exit code 8).
2. `cd /d "<the repository>\out\build\x64-release\bin"`
3. `PdfThumbnailsCli.exe --status`: it should say healthy (exit code 0).
4. `thumbtest.exe experiment --cli PdfThumbnailsCli.exe`, then follow the questions. Options:
   - `--desktop`: after asking, one test PDF on your Desktop (never when the Desktop is in
     OneDrive), to see whether the Desktop shows the new thumbnail too;
   - `--copy-check`: a 1 GB file in the test folder (it needs about 2.3 GB free) that you copy in
     File Explorer, to record the class of File Explorer's copy progress window;
   - `--keep` keeps the test files at the end (the one on the Desktop too); `--root <dir>` makes
     the test folder there (a folder that does not exist yet) instead of in `%TEMP%`;
     `--timing-files <n>` sets how many files are timed (2,000; 0 leaves the timing out, but not
     how fast a refresh stops).

Answer with `y` or `n` and Enter; where it says so, press Enter alone as soon as you see the
change. `q` at any question stops the experiment (exit code 3) and puts everything back.

### What it does

1. **Pre-flight**, which only reads. It refuses as administrator, below an app package, where the
   registry is virtualized, where File Explorer on the desktop is another account's, where
   Windows does not use PdfThumbnails for `.pdf` thumbnails, and where File Explorer shows icons
   only. It records the Windows build, the zoom you have saved, File Explorer's processes and what
   `thumbtest shellinfo --windows --holders` sees, and asks how many File Explorer windows and
   tabs you have open.
2. **Files**: a new folder `%TEMP%\PdfThumbnails-refresh-<UTC time>` with folders of four
   synthetic PDFs each (two A4 portrait pages, two landscape): `C-control`; one per extraction
   variant, `P-product`, `F-force`, `R-slowreclaim`, `S-instance` and `E-force-then-store`; one
   per round, `N-none`, `I-item`, `D-dir`, `T-touch`, `A-assoc` and `B-dialog`; and `timing`
   (2,000 copies), `slow` (heavy and very large pages), `probe` and `ladder` (two portraits).
3. **Zoom 100 %**, through `PdfThumbnailsCli --zoom 100 --no-refresh`.
4. **The cache filled**: first the two `ladder` files, one at 256 pixels only and one at 100
   only; then every other file but the timing ones at every size from 16 to 2560 pixels, each
   size asked about at once. Which cache file each size went to is recorded from the files' sizes
   and times (never their contents). Straight after, the baseline: the cache is asked about every
   file of the judged folders at each size, as the refresh asks (`WTS_INCACHEONLY`); an answer
   that is not held is asked again after 2 seconds and after 10 more. Then the cache's answers to
   other flags are recorded, for the `probe` and `ladder` files only.
5. **The baseline checked**: every thumbnail held and at 100 %, at each size the refresh asks
   about (1024 lands in 1280's cache file, so it is filled but not judged). A thumbnail is held
   when the bitmap's size shows it came from its size's own cache file; an answer from another
   size's never counts. A folder that is not is left out, by name; without a valid `C-control`
   the run stops, since nothing would then show that a pass came from the refresh.
6. **Zoom 142 %**; then one thumbnail made without storing it must show 142 %, else the thumbnail
   process does not see the new zoom and the run stops.
7. **The extraction variants** (about a minute): each variant's folder is refreshed and judged;
   `C-control` must stay as it was, or the run is not valid and stops. Without a variant that
   passes, the steps up to 11 are left out.
8. **Rounds by eye**. For `N-none`, `I-item` and `D-dir` in turn, you paste the folder's path into
   one File Explorer window and switch to Large icons (Ctrl+Shift+2); the pages must show whole,
   with white at the sides. The experiment refreshes the folder (with no notification, then per
   file, then per folder), and you press Enter as soon as the thumbnails turn square. If a
   notified round stays old, a ladder follows, one step at a time: the experiment refreshes the
   window's view (what F5 does) and asks whether the selection and the scroll position stayed;
   it browses the window to the folder again; it opens a File Open dialog of its own on the
   folder; and it asks you to press F5. If refreshing the view or browsing again turned the
   window square, you say whether every refresh may do that to open windows; it counts only with
   your yes. If neither notification worked, `T-touch` moves the
   last-write time of its own files on by 2 seconds and notifies, without making a thumbnail. If
   nothing so far turned a window square, `A-assoc` is refreshed and then Windows is told that
   file associations changed (every open window repaints, so this comes last).
9. **The Desktop and another program's dialog**, when something turned a window square, with that
   same way: the test PDF on the Desktop (with `--desktop`), and a File Open dialog on
   `B-dialog`.
10. **Timing**: the timing files filled at 256 (only now, after the variants were judged), and
    `C-control` asked about again; then the timing files refreshed while another thread measures
    how long a thumbnail like File Explorer's waits meanwhile (against how long it takes before);
    the first 200 again, each in a thumbnail process of its own; how fast a refresh of the slow
    folder stops when asked to, and whether the cache answers consistently afterwards.
11. **The product**: you open a Windows Terminal tab and run the `PdfThumbnailsCli.exe --refresh
    --folder` command it prints, for `C-control`. The experiment first checks that `C-control`
    still shows 100 %, then judges it after the command (it must now show 142 %) and counts the thumbnail cache files in Windows Terminal's private folders
    (there must be none).
12. **The copy window** (with `--copy-check`).
13. **The end**: File Explorer's processes from the start must still run (it was never
    restarted); the zoom goes back to yours; you close the test window; the test files are
    deleted; the report is written.

The experiment changes only:
- the saved zoom, for the run;
- its own folder of synthetic PDFs and, with `--desktop` and your yes, one file on the Desktop,
  deleted at the end, each only while its name still leads to the file it wrote;
- the thumbnail cache's entries for those files, made, asked about and made again in the
  thumbnail process (never in thumbtest itself);
- the views of File Explorer windows that show one of its folders (refreshed or browsed again,
  each view checked first), and File Open dialogs of its own.

It never deletes a cache file, never ends or restarts File Explorer or a thumbnail process, and
never opens a file of yours.

### Stopping it, and the zoom

The experiment first prints the command that puts your zoom back, for example:

```bat
"<bin>\PdfThumbnailsCli.exe" --zoom 130 --no-refresh
```

It puts the zoom back by itself at the end, after a failure, when you answer `q`, and on Ctrl+C or
when you close its window (the test files then stay: delete the folder it printed by hand, and
`PdfThumbnails-refresh-test.pdf` from your Desktop if it put one there). A crash, a process ended
from outside, and signing out, shutting down or restarting Windows while it runs (Windows then
ends it without notice) leave the zoom at 100 % or 142 %: then run that command. If it cannot put
the zoom back, it says so, and the exit code is 1, also after a `q`.
Where no zoom of your own was saved, it deletes the value again instead, and prints that command
(`reg delete "HKCU\Software\PdfThumbnails" /v Zoom /f`).

### The report

`out\refresh-experiment\<UTC time>\` (git ignores `out\`) holds `report.txt`, with each step's
counts, sizes, error codes, times, your answers and what the outcome decides, and the cache's
answers as records (`baseline.tsv`, `after-variants.tsv` and others, which `thumbtest judge`
reads). Folders are named by their labels and files by their synthetic names, so the report names
no path of yours: the test folder is `<root>`, the Desktop `<Desktop>`.

### Without the questions

`thumbtest experiment --non-interactive` runs the automatic part only (steps 1 to 7, 10 and 13,
without the questions) and decides Pass A for the extraction variants. It also runs where File
Explorer on the desktop is another account's (a test account), but it refuses where Smart App
Control is on. It changes the zoom like the whole experiment, so run it only where that does no
harm. It is the `e2e` label's test ([Tests](BUILDING.md#tests)): no CTest test is registered for
it, so no preset or `ctest` run starts it. CI's `system-x64` job runs it as the system tests'
throw-away test account, after the install for all users (with `--timing-files 200`): its
verdict is in that job's log, on the lines that start with `[ system ] refresh experiment`, and
its whole report on the lines that start with `[ system ] report.txt`.
