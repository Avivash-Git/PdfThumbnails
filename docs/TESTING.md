# Testing PdfThumbnails by hand

Most tests run by themselves (see [Tests](BUILDING.md#tests)). This page is for the checks that
need someone at the PC: what they do, what they change on it, and how to stop them.

- [The thumbnail refresh experiment](#the-thumbnail-refresh-experiment): how `--refresh` was
  chosen, and its results.
- [The settings window](#the-settings-window): what only a person can judge.
- [The installer](#the-installer): Setup and its uninstaller on a real PC.
- [The whole pass before a release](#the-whole-pass-before-a-release): install, use, take-over,
  uninstall and reinstall on a real PC.
- [With Smart App Control on](#with-smart-app-control-on-signed-builds): a signed setup,
  downloaded as a user would, on a PC that enforces Smart App Control.

## The thumbnail refresh experiment

`thumbtest experiment` finds out how `--refresh` (the PDF-only refresh) should make the PDF
thumbnails File Explorer already made again, and how it should tell File Explorer, so that a new
zoom shows without restarting File Explorer. It works on your own thumbnail cache and File
Explorer, with synthetic PDFs only, and takes about 30 to 40 minutes with you at the PC.

It decides two things:
- **Pass A**, automatically: after a refresh, the thumbnails the cache held before are still
  there and show the new zoom (or the cache forgot their whole file after the refresh made and
  stored them, which counts neither way). A thumbnail's size shows its zoom (an A4 page at 256
  pixels is 181 x 256 at 100 % and 256 x 256 at 142 %), so the experiment judges them by the
  sizes the cache reports, without looking at pixels. Of the five ways of making a thumbnail
  again it knows, it tries those the product's refresh allows, which is only the product's own
  (`WTS_FORCEEXTRACTION | WTS_REQUIRESURROGATE`): the product only extracts in the thumbnail
  process, and the other four do not ask for it, so the report names them as left out. The first
  way that passes is used for the rest of the run. Pass A also needs the product itself,
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
  makes meanwhile keep that zoom. At the end it prints the command that refreshes a folder of
  PDFs shown meanwhile.
- It adds thousands of thumbnails of its test PDFs to your thumbnail cache (about 14,000 with the
  2,000 timing files, about 4,300 with `--timing-files 500 --instance-files 40`), which keeps them
  after the test PDFs are deleted. Adding thumbnails can make Windows forget others (a new cache
  on CI forgot about one file in five); Windows makes those again when they are shown.
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
     how fast a refresh stops), and `--instance-files <n>` how many of them are made again each
     in a thumbnail process of its own (200; that pass is left out while the product's refresh
     refuses its flags, as it does now).

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
   synthetic PDFs each (two A4 portrait pages, two landscape): one per extraction variant that is
   run, which is only `P-product` now (`F-force`, `R-slowreclaim`, `S-instance` and
   `E-force-then-store` are left out and get none; see step 7); one per round, `N-none`,
   `I-item`, `D-dir`, `T-touch`, `A-assoc` and `B-dialog`; `C-control` with eight (four of each
   page); and `timing` (2,000 copies), `slow` (heavy and very large pages), `probe`, `ladder`
   (two portraits) and `untouched` (20 portraits). They are made in the order the cache is
   filled: `slow`, `probe`, the rounds' folders (then the test PDF on the Desktop), the
   variants' (`P-product` last), and `C-control` last, since on CI the cache forgot files filled
   early and none of those filled last; then the others.
3. **Zoom 100 %**, through `PdfThumbnailsCli --zoom 100 --no-refresh`.
4. **The cache filled**: first the two `ladder` files, one at 256 pixels only and one at 100
   only, each followed by the cache files' sizes; then the `untouched` files once at 256 pixels.
   These stand for your own thumbnails, stored once and never touched: from now on they are only
   asked about, and the report says whether Windows still holds them. Then every other file but
   the timing ones at every size from 16 to 2560 pixels, each size asked about at once; a file
   the cache does not hold at that size is asked about at the sizes before, which tells a file
   forgotten whole from one size gone. Which cache file each size went to is recorded from the
   files' sizes and times (never their contents). Straight after, the baseline: the cache is
   asked about every file of the judged folders at each size, as the refresh asks
   (`WTS_INCACHEONLY`); an answer that is not held is asked again after 2 seconds and after 10
   more (later questions are asked again only if that found anything here, and only for files
   partly held). Then the cache's answers to other flags are recorded, for the `probe` and
   `ladder` files only.
5. **The baseline checked**, at each size the refresh asks about (1024 lands in 1280's cache file,
   so it is filled but not judged). A thumbnail is held when the bitmap's size shows it came from
   its size's own cache file; an answer from another size's never counts. While thumbnails are
   stored, the cache forgets whole files, every size at once, so a folder is judged on the files
   it still holds: at every size it must hold at 100 % at least one portrait and one landscape
   page (`C-control` two of each), and nothing at another zoom. The files it does not hold are
   named and not judged. A folder that does not is left out, by name; without a valid
   `C-control` the run stops, since nothing would then show that a pass came from the refresh.
6. **Zoom 142 %**; then one thumbnail made without storing it must show 142 %, else the thumbnail
   process does not see the new zoom and the run stops.
7. **The extraction variants** (about a minute): a variant whose flags the product's refresh
   refuses is left out, with one line in the report that names them all: the product only
   extracts in the thumbnail process (`WTS_REQUIRESURROGATE`), and only `P-product` asks for it.
   Each other variant's folder is refreshed, and straight after it and `C-control` are asked
   about and judged. A variant passes when its refresh reported no failure; each thumbnail it
   held at 100 % in a file the refresh reached was made by
   the refresh's own extraction at 142 %, and is now held at 142 % or went with every other
   thumbnail of its file after the cache said it stored it (the cache forgot that file: it
   counts neither way); nothing is at 100 % or at neither zoom, no file lost only some of its
   thumbnails, not everything the refresh made went; and the thumbnails held show a portrait and
   a landscape page at every size. Where only files the cache forgot stand in the way, the
   variant is not decided. `C-control` must not change (nothing new, nothing at neither zoom, no
   file partly gone), and must still hold two pages of each kind at 100 % at every size, or the
   run is not valid and stops. The first variant that passes is used for the rest of the run.
   Without one, the steps up to 11 are left out (without the questions, the timing files are
   still filled).
8. **Rounds by eye**. For `N-none`, `I-item` and `D-dir` in turn, you paste the folder's path into
   one File Explorer window and switch to Large icons (Ctrl+Shift+2); the pages must show whole,
   with white at the sides. Just before, the experiment asks the cache about the folder: when it
   has forgotten some of the files, it names the ones to look at (File Explorer makes the others
   at the new zoom at once), and when it already holds new thumbnails, the round is not valid.
   The experiment refreshes the folder (with no notification, then per file, then per folder),
   and you press Enter as soon as the thumbnails turn square; afterwards the files you looked at
   must be held at 142 %, or your answer counts for nothing. If a notified round stays old, a
   ladder follows, one step at a time: the experiment refreshes the window's view (what F5 does)
   and asks whether the selection and the scroll position stayed; it browses the window to the
   folder again; it opens a File Open dialog of its own on the folder; and it asks you to press
   F5. If refreshing the view or browsing again turned the window square, you say whether every
   refresh may do that to open windows; it counts only with your yes. If neither notification
   worked, `T-touch` moves the last-write time of its own files on by 2 seconds and notifies,
   without making a thumbnail (what the cache holds afterwards is only recorded). If nothing so
   far turned a window square, `A-assoc` is refreshed and then Windows is told that file
   associations changed (every open window repaints, so this comes last).
9. **The Desktop and another program's dialog**, when something turned a window square, with that
   same way: the test PDF on the Desktop (with `--desktop`), and a File Open dialog on
   `B-dialog`, each asked about first and checked afterwards as a round is.
10. **Timing**: first the `untouched` files are asked about: if the cache has forgotten any of
    them since they were stored, the timing fill and the timing refreshes are left out, since
    thousands more thumbnails would make it forget more of yours; so they are when it held none
    of them right after their store, since nothing would then show that. Otherwise the timing
    files are filled at 256 (only now, after the variants were judged), and `C-control` and the
    timing files asked about again; then the timing files refreshed while another thread
    measures how long a thumbnail like File Explorer's waits meanwhile (against how long it takes
    before); the first 200 again (`--instance-files`), each in a thumbnail process of its own
    (left out, with one line, while the product's refresh refuses that extraction, as it does
    now), and what the cache forgot across them. Either way, how fast a refresh of the slow folder
    stops when asked to, and whether the cache answers consistently afterwards.
11. **The product**: the experiment first checks that `C-control` still shows 100 % (from here one
    page of each kind held at every size will do; otherwise this step is left out, and Pass A is
    not complete). You open a Windows Terminal tab and run the `PdfThumbnailsCli.exe --refresh
    --folder` command it prints, for `C-control`, with what shows its exit code (`exit=<n>`: in
    PowerShell on the same line, in a Command Prompt as a second command), and type that number.
    The experiment then judges `C-control` (it must now show 142 %; only exit code 0 lets a file
    the cache forgot count neither way, and 3 or 4 leave it not decided) and counts the thumbnail
    cache files in Windows Terminal's private folders (there must be none).
12. **The copy window** (with `--copy-check`).
13. **The end**: File Explorer's processes from the start must still run (it was never
    restarted); the zoom goes back to yours; the cache files' sizes and how much they grew are
    recorded, and the command that refreshes a folder of PDFs shown meanwhile is printed; you
    close the test window; the test files are deleted; the report is written.

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
answers as records (`baseline.tsv`, `right-after-variants.tsv`, `after-variants.tsv` and
others, which `thumbtest judge` reads, and the variants' own extraction answers,
`made-variants.tsv`). Folders are named by their labels and files by their synthetic names, so
the report names no path of yours: the test folder is `<root>`, the Desktop `<Desktop>`.

### Without the questions

`thumbtest experiment --non-interactive` runs the automatic part only (steps 1 to 7, 10 and 13,
without the questions) and decides Pass A for the extraction variants; it fills the timing
files (2,000 by default) even without a variant that passes, and the `untouched` files are only
reported, so nothing holds back the thumbnails it adds to the cache of the account it runs as.
It also runs where File Explorer on the desktop is another account's (a test account), but it
refuses where Smart App Control is on. It changes the zoom like the whole experiment, so run it
only where that and those thumbnails do no harm. It is the `e2e` label's test
([Tests](BUILDING.md#tests)): no CTest test is registered for it, so no preset or `ctest` run
starts it. CI's `system-x64` job runs it as the system tests' throw-away test account, after the
install for all users (with `--timing-files 200 --instance-files 40`): its verdict is in that
job's log, on the lines that start with `[ system ] refresh experiment`, and its whole report on
the lines that start with `[ system ] report.txt`.

### Results (8 November 2026, Windows 11 build 26300.9818)

Three runs at the PC, on synthetic PDFs only; zoom 100 % then 142 %, put back to the saved zoom
each time. One run was closed before its end and has no report.

| | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| File Explorer processes at the start | 1 | 2 | 1 |
| Pass A: thumbnails made again at the new zoom (5 ways) | 162 of 162 | 180 of 180 | 180 of 180 |
| Control folder changed | no (72 entries) | no (72 entries) | no (72 entries) |
| Untouched thumbnails kept | 20 of 20 | 20 of 20 | 20 of 20 |
| No notification | not square in 10 s | not square in 10 s | not square in 10 s |
| `SHCNE_UPDATEITEM` per file | square after 2.2 s | not square in 10 s | square after 1.4 s |
| `SHCNE_UPDATEDIR` per folder | square after 1.0 s | not square in 10 s | not valid |
| Desktop | square after 4.2 s | not comparable | square after 2.6 s |
| Another program's File Open dialog | square after 0.6 s | not comparable | square after 1.2 s |
| 2,000 files cached at 256 | 24.0 s | 21.9 s | 22.3 s |
| Other thumbnail requests during it, against idle | 0.9 times | 0.8 times | 0.8 times |
| A stop asked 1 s into a refresh | 266 ms | 260 ms | 276 ms |

Answers and notes:
- The product's own extraction flags (`WTS_FORCEEXTRACTION | WTS_REQUIRESURROGATE`) passed first
  each time, and `SHCNE_UPDATEITEM` per file is the notification chosen. These runs tried all
  five ways; since then the product's refresh refuses an extraction that does not ask for the
  thumbnail process, so the experiment tries the product's own alone.
- The cache's question must be `WTS_INCACHEONLY` alone: with `WTS_IDEALCACHESIZEONLY` it refuses
  with `E_INVALIDARG`.
- Run 2 had a second File Explorer process, left from a time File Explorer had stopped, and the
  folder windows did not pick up the notifications. `IShellView::Refresh` did turn them square, but
  it lost the selection and the scroll position, which was not acceptable. With one process again
  (run 3), the notification worked. The second process being the cause is likely, not proven.
- `--refresh --folder` from a Windows Terminal tab: exit 0, 93 thumbnails in 20 files of 44
  checked, none failed. Those thumbnails were already at the current zoom, so this shows the
  command runs there; the old-to-new change is what Pass A shows.
- `--refresh --full` from a Windows Terminal tab: File Explorer was back after 109 ms, by itself,
  and its one folder window was reopened. This needed the holders to end with exit code -1: with
  exit code 1, Windows did not start File Explorer again.
- With a copy paused, File Explorer shows a visible window of class `OperationStatusWindow`, the
  one `--refresh --full` looks for; a copy that has finished shows none.

## The settings window

The window's automatic tests (`gui` label) run only on runners GitHub hosts, because the window
uses the real registry. These checks need a person, with a build run from a Command Prompt opened
with Win+R (`out\build\x64-release\bin\PdfThumbnailsSettings.exe`). Save changes your real zoom
and refreshes your PDF thumbnails; Rebuild thumbnails restarts File Explorer; Uninstall removes
the product.

- **Keyboard only:** Tab goes through the controls in order, the underlined letters work, Esc
  closes, Enter saves when Save is on, F5 runs Check now.
- **Narrator** reads the slider, the zoom box, the status lines and the preview tiles, and the
  zoom in the tiles' names follows the slider.
- **Display scaling** of 100, 150 and 200 %, and moving the window between monitors with
  different scaling: nothing is cut off, and the tiles stay sharp.
- **High Contrast** on and off while the window is open.
- **"Run as administrator"** shows why the window does not open, and nothing else.
- **"Run as different user"** shows a banner that names that account. Started from the install
  for all users, it offers to reopen as the signed-in user; started from anywhere else, it says to
  open the settings from File Explorer instead.
- **Save** with a new zoom refreshes thumbnails with progress; Cancel stops it and keeps the zoom.
- **Closing** with unsaved changes asks; closing during a refresh asks whether to stop it.

## The installer

CI runs the setup for real on a throw-away account (`Installer.*`, see
[The installer](BUILDING.md#tests-1)). These checks need a person: the wizard, Installed apps and
the Start menu as Windows shows them, and the upgrade of the build already on the PC. Setup asks
for administrator rights once; the uninstall once more. `<setup>` is
`PdfThumbnails-<version>-setup.exe`, built as [The installer](BUILDING.md#the-installer) says or
taken from a CI run's `PdfThumbnails-setup-unsigned` artifact.

1. **Before.** `"C:\Program Files\PdfThumbnails\PdfThumbnailsCli.exe" --status` and `--zoom`:
   note the zoom. A developer install, 1.x or nothing is fine.
2. **Setup.** Double-click `<setup>`. An unsigned setup shows SmartScreen's "Windows protected
   your PC" (More info, then Run anyway); with Smart App Control on, an unsigned setup that was
   downloaded is blocked (see
   [With Smart App Control on](#with-smart-app-control-on-signed-builds)). One administrator prompt; the wizard asks no folder and
   no Start menu group; it closes PdfThumbnails' own programs if they are running; the last page
   offers to open the settings.
3. **Installed apps** (Settings > Apps > Installed apps) lists PdfThumbnails once, with its
   version, publisher and icon. The Start menu has **PdfThumbnails**, which opens the window.
4. **Status.** `--status` ends with `Result: healthy` and shows the zoom from step 1. Thumbnails
   in a folder of PDFs show at that zoom.
5. **Again over itself.** Run `<setup>` once more: it finishes without asking to close anything
   File Explorer uses, and the status stays healthy.
6. **Uninstall** from the window, with "Also clear the thumbnail cache" checked: the window says
   it waits for the uninstaller, one administrator prompt, then File Explorer restarts and PDFs
   show their app's icon. Or from Installed apps: one administrator prompt, no question about a
   restart, and thumbnails already made stay until File Explorer makes them again. Either way:
   `C:\Program Files\PdfThumbnails` is gone (or holds only a trash folder next to it until the
   restart), the Start menu entry and the Installed apps entry are gone, and PDFs show their
   app's icon once File Explorer makes their thumbnails again.
7. **Reinstall** with `<setup>` and set the zoom again (`--zoom <n>`): the uninstall removed it.

## The whole pass before a release

CI installs, uses and removes the product on a throw-away account (the `system` tests). This pass
does the same on a PC someone really uses, where File Explorer, the thumbnail cache and the
sign-in are real. It takes about half an hour, asks for administrator rights three times, signs
you out once and restarts File Explorer twice. Use a folder of PDFs you may look at, and run every
command from a normal prompt (not an administrator one).

`<stage>` is a staged release build ([Staging](BUILDING.md#staging-install-components)), and
`<installed>` is `C:\Program Files\PdfThumbnails`.

1. **Before.** `<installed>\PdfThumbnailsCli.exe --status` and `--zoom`: note the zoom. Nothing
   installed yet is fine too.
2. **Install over what is there.** `<stage>\PdfThumbnailsCli.exe --install --machine`: one
   administrator prompt, exit code 0, and the status it prints ends with `Result: healthy`. The
   zoom is the one from step 1, and the Checks line names both triggers (or the ones chosen
   before).
3. **Thumbnails.** Open a folder of PDFs in File Explorer with large icons. The pages show at the
   zoom from step 1; at 100 % a portrait page has white bands at its sides.
4. **The window**, started from `<installed>`: the checks in
   [The settings window](#the-settings-window).
5. **Rebuild thumbnails** in the window, with two folder windows open. File Explorer restarts,
   both windows come back, and the thumbnails are made again.
6. **Another program takes `.pdf` thumbnails.** The `.pdf` setting is the default value of the key
   `HKEY_CURRENT_USER\Software\Classes\.pdf\ShellEx\{e357fccd-a995-4576-b01f-234630154e96}`.
   - Delete that key, sign out and sign in again. About a minute after the sign-in, the check has
     put it back: the log (Open log in the window) has a line `check result=healthy fixed=1`, and
     `--status` says healthy.
   - Set the key's default value to another class, for example
     `{11111111-2222-3333-4444-555555555555}`. `--status` names it as another handler, and
     `--check` exits 1 and leaves it alone. The window's status offers "Use PdfThumbnails for
     .pdf", which asks and then takes it back; so does `--fix --take-back`.
7. **Uninstall** from the window, with "Also clear the thumbnail cache" checked: one
   administrator prompt, File Explorer restarts, and PDFs show their app's icon. `--status` from
   `<stage>` says not installed (exit code 6). After a restart of Windows,
   `C:\Program Files\PdfThumbnails` and the `PdfThumbnails.trash-...` folder next to it are gone.
8. **Reinstall** as in step 2, then set the zoom again (`--zoom <n>`): the uninstall removed it.
   Thumbnails are back at that zoom.

With Smart App Control on, steps 2 and 8 also show whether Windows lets the unsigned build run:
the status then has a Smart App Control line, and it says whether PdfThumbnails is being blocked.

## With Smart App Control on (signed builds)

Smart App Control is strictest with files that came from the internet: it blocks a program or DLL
unless Microsoft's cloud knows it or it is signed with a certificate Windows trusts, and the user
gets no way to allow it. Files built on the PC, or copied there without the download mark, may
run anyway, so this pass must download the setup the way a user does. Use a PC or a virtual
machine with a current Windows 11 where Smart App Control is **On** (Windows Security > App &
browser control > Smart App Control), not one where it is only evaluating. A throw-away virtual
machine is best: the pass installs and uninstalls for all users.

`<signed>` is the setup of a signed run (`PdfThumbnails-setup-signed`, or a release), and
`<unsigned>` the setup of an ordinary run (`PdfThumbnails-setup-unsigned`) of the same commit.

1. **Download both with a browser** on the test PC, not through a shared folder, the clipboard,
   `curl` or `Invoke-WebRequest`. A ZIP downloaded from GitHub's artifacts carries the mark onto
   what Explorer extracts from it. In PowerShell,
   `Get-Content <setup> -Stream Zone.Identifier` shows `ZoneId=3` for each.
2. **The control.** Run `<unsigned>`: Smart App Control must block it (a notification that it
   blocked an app; Event Viewer > Applications and Services Logs > Microsoft > Windows >
   CodeIntegrity > Operational has an event 3077 or 3033 for it). If it runs, Smart App Control is
   not enforcing on this PC, and the rest of the pass proves nothing.
3. **Signature.** `Get-AuthenticodeSignature <signed>` shows `Valid` and the publisher's
   certificate; the file's Properties > Digital Signatures shows a timestamp.
4. **Install.** Run `<signed>`. SmartScreen may still say the publisher is new ("unrecognized
   app") while the certificate has little reputation; the publisher's name must show there
   (SignPath Foundation, for a build signed through SignPath).
   Smart App Control must not block Setup, its temporary files or the uninstaller it writes.
5. **Every installed file signed.** In PowerShell:
   `Get-ChildItem 'C:\Program Files\PdfThumbnails' -Recurse -Include *.exe,*.dll | Get-AuthenticodeSignature | Format-Table Status, Path`:
   every line `Valid`, `unins000.exe` and `PdfThumbnails.dll` in `app\<build folder>` included.
   `PdfThumbnails-pdfium.dll` is `Valid` too, except in a build that leaves it unsigned
   (`PDFT_SIGN_PDFIUM=off`, every build signed through SignPath), where it is `NotSigned`.
6. **Thumbnails.** Open a folder of PDFs (downloaded ones too) with large icons: thumbnails show.
   `PdfThumbnailsCli.exe --status` ends with `Result: healthy`, and its Smart App Control line
   says PdfThumbnails is not being blocked. No new CodeIntegrity 3077 or 3033 event names a file
   in `C:\Program Files\PdfThumbnails`. With an unsigned PDFium DLL this is the step that
   matters: it shows whether Smart App Control lets File Explorer's thumbnail process load an
   unsigned DLL that a signed setup wrote (the file has no download mark). Repeat it after a
   restart, and again a day later, and note the result in the issue or pull request of the pass.
7. **The check task and the window.** Sign out and in: the log has a `check` line from the task.
   The window opens from the Start menu; Check now and Save with a new zoom work.
8. **Upgrade.** Download a newer signed setup the same way and run it over the first: healthy,
   the zoom kept, no block.
9. **Uninstall** from Installed apps: no block, `C:\Program Files\PdfThumbnails` gone.
