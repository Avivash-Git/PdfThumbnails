// The status text (status_format.h) on made-up snapshots: whole reports line by line for this
// PC's shape, a per-user install and nothing installed, then each problem line with its hint,
// the skipped checks, the test file, Smart App Control's texts and the layout itself.

#include <windows.h>

#include "status_format.h"

#include <gtest/gtest.h>
#include <oleauto.h>

#include <optional>
#include <string>
#include <vector>

#include "evaluate.h"
#include "project_config.h"
#include "snapshot_fixtures.h"
#include "strings.h"

namespace pdft {
namespace {

using namespace test;  // NOLINT(build/namespaces): the fixtures

const std::wstring kProduct = kProductName;
// The separator between the parts of one line: a middle dot between spaces.
const std::wstring kDotText = std::wstring(L" ") + wchar_t{0xB7} + L" ";

std::vector<std::wstring> SplitLines(const std::wstring& text) {
  std::vector<std::wstring> lines;
  size_t start = 0;
  for (;;) {
    const size_t end = text.find(L'\n', start);
    lines.push_back(
        text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
    if (end == std::wstring::npos) return lines;
    start = end + 1;
  }
}

std::vector<std::wstring> Report(const Snapshot& s) {
  const std::vector<Finding> findings = Evaluate(s);
  return SplitLines(RenderStatus(FormatStatus(s, findings)));
}

std::wstring Header(const std::wstring& build = L"26300.9818") {
  return kProduct + L" " + kProductVersion + L" (" + kArch + L"), Windows build " + build;
}

// The line with |label| and |mark| whose text contains |text|; nothing when there is none.
std::optional<StatusLine> FindLine(const std::vector<StatusLine>& lines, const std::wstring& label,
                                   char mark, const std::wstring& text = L"") {
  for (const StatusLine& line : lines) {
    if (line.label == label && line.mark == mark && line.text.find(text) != std::wstring::npos) {
      return line;
    }
  }
  return std::nullopt;
}

std::vector<StatusLine> Lines(const Snapshot& s) { return FormatStatus(s, Evaluate(s)); }

// Every problem line says what fixes it.
void ExpectHintsOnProblems(const Snapshot& s) {
  for (const StatusLine& line : Lines(s)) {
    if (line.mark == '!') EXPECT_FALSE(line.hint.empty()) << line.label << ": " << line.text;
  }
}

std::wstring ToText(const std::vector<std::wstring>& lines) {
  std::wstring text;
  for (const std::wstring& line : lines) text += line + L"\n";
  return text;
}

TEST(StatusFormat, ThisPcsShape) {
  const std::vector<std::wstring> expected = {
      Header(),
      L"     Install        machine-wide (C:\\Program Files\\Product); settings per user",
      L"  OK Handler        C:\\Program Files\\Product\\app\\1.1.0.0-67a1323e\\" +
          std::wstring(kHandlerDll) + L" (1.1.0.0, " + kNativeArchName +
          L", Apartment); a 1.x build (installing this version for all users replaces it)",
      L"  OK PDFium         " + std::wstring(kPdfiumDll) + L" next to the handler",
      L"  OK .pdf slot      .pdf -> " + kProduct,
      L"  OK .pdf slot      SystemFileAssociations\\.pdf -> " + kProduct,
      L"  OK Effective      Windows resolves .pdf thumbnails to " + kProduct,
      L"     Default app    PdfCraft.Document (no icon overlay)" + kDotText + kPdfCraftProductName +
          L" 0.14.0",
      L"  OK Explorer test  thumbnail extracted in a fresh thumbnail process (212 ms)",
      L"     Zoom           130 % (1.x settings.ini)",
      L"     Checks         off (the 1.x task " + std::wstring(kLegacyTask) + L" is still there)",
      L"     Smart App Control  On; not blocking " + kProduct + L" (the Explorer test passed)",
      L"     1.x leftovers  1.x left the task " + std::wstring(kLegacyTask) +
          L", settings.ini (Zoom=130)",
      L"                    --migrate-v1 removes them, once this version is installed",
      L"Result: healthy",
  };
  EXPECT_EQ(ToText(Report(ThisPcSnapshot())), ToText(expected));
}

TEST(StatusFormat, PerUserInstallWithARecordedCheck) {
  Snapshot s = UserInstallSnapshot();
  s.pdfcraft = {true, L"0.14.0", L"", PdfCraftSource::kUninstallKey};
  s.zoom.decision = {120, ZoomOrigin::kRegistry, ZoomValueState::kValid};
  CheckRecord record;
  record.time = {0x5C0E8000, 0x01DD4A2B};
  record.result = ExitCode::kOk;
  record.summary = L"nothing to fix";
  record.pdfcraftVersion = L"0.14.0";
  s.last = record;
  const std::wstring when = LocalTimeText(record.time);
  ASSERT_FALSE(when.empty());

  const std::filesystem::path dll = DllIn(s.userBuilds[0]);
  const std::vector<std::wstring> expected = {
      Header(),
      L"     Install        per user (" + kUserRoot.native() + L")",
      L"  OK Handler        " + dll.native() + L" (2.0.0.0, " + kNativeArchName + L", Apartment)",
      L"  OK PDFium         " + std::wstring(kPdfiumDll) + L" next to the handler",
      L"  OK .pdf slot      .pdf -> " + kProduct,
      L"  OK .pdf slot      SystemFileAssociations\\.pdf -> " + kProduct,
      L"  OK Effective      Windows resolves .pdf thumbnails to " + kProduct,
      L"     Default app    none" + kDotText + kPdfCraftProductName + L" 0.14.0",
      L"  OK Explorer test  thumbnail extracted in a fresh thumbnail process (200 ms)",
      L"     Zoom           120 % (registry)",
      L"  OK Checks         after " + std::wstring(kPdfCraftProductName) + L" updates: on" +
          kDotText + L"at sign-in: on" + kDotText + L"last check " + when + L" (nothing to fix)",
      L"Result: healthy",
  };
  EXPECT_EQ(ToText(Report(s)), ToText(expected));
}

TEST(StatusFormat, NothingInstalled) {
  Snapshot s = EmptySnapshot();
  s.slots = {kOther, std::nullopt};
  const std::vector<Finding> findings = Evaluate(s);
  ASSERT_EQ(ExitCodeFromFindings(findings), ExitCode::kNotInstalled);
  const std::vector<std::wstring> lines = SplitLines(RenderStatus(FormatStatus(s, findings)));
  ASSERT_EQ(lines.size(), 12u) << ToText(lines);
  EXPECT_EQ(lines[1], L"  !! Install        not installed");
  EXPECT_EQ(lines[2].rfind(L"                    Install it with ", 0), 0u) << lines[2];
  EXPECT_EQ(lines[3], L"     Handler        not registered");
  // Not judged without an install: no marks.
  EXPECT_EQ(lines[4],
            L"     .pdf slot      .pdf -> " + kOther + L" (unknown handler), another handler");
  EXPECT_EQ(lines[5], L"     .pdf slot      SystemFileAssociations\\.pdf -> empty");
  EXPECT_EQ(lines[6], L"     Effective      Windows resolves .pdf thumbnails to nothing");
  EXPECT_EQ(lines[7],
            L"     Default app    none" + kDotText + kPdfCraftProductName + L" not found");
  EXPECT_EQ(lines[8], L"     Explorer test  skipped: no class is registered");
  EXPECT_EQ(lines[9], L"     Zoom           100 % (default)");
  EXPECT_EQ(lines[10], L"     Checks         off");
  // Smart App Control absent: no line.
  EXPECT_EQ(lines[11], L"Result: not installed");
  // The install hint names --install only where the build has it, and never as "later".
  const std::wstring hint = lines[2];
  EXPECT_EQ(hint.find(L"later"), std::wstring::npos) << hint;
  EXPECT_EQ(hint.find(L"--install") != std::wstring::npos, kDevInstall) << hint;
}

// Files in use waiting in trash folders for the restart: a neutral line of its own, never "!!",
// and no command to run.
TEST(StatusFormat, FilesWaitingForTheRestart) {
  Snapshot s = MachineInstallSnapshot();
  s.pendingTrash = {L"a", L"b"};
  const std::vector<StatusLine> lines = Lines(s);
  const std::optional<StatusLine> restart = FindLine(lines, L"Restart", ' ');
  ASSERT_TRUE(restart);
  EXPECT_EQ(restart->text,
            L"files of an old build that were in use wait in trash folders next "
            L"to " +
                kMachineRoot.native() + L"; Windows deletes them at the next restart");
  EXPECT_EQ(restart->hint, L"Restart Windows to finish");
  EXPECT_FALSE(FindLine(lines, L"1.x clean-up", '!'));
  EXPECT_EQ(Report(s).back(), L"Result: healthy");
  s.pendingTrash = {L"a"};
  EXPECT_NE(FindLine(Lines(s), L"Restart", ' ')->text.find(L"in a trash folder"),
            std::wstring::npos);
}

TEST(StatusFormat, NoSmartAppControlLineWhenItIsOff) {
  Snapshot s = MachineInstallSnapshot();
  for (SacState state : {SacState::kOff, SacState::kAbsent}) {
    s.sac.state = state;
    s.sacVerdict = DecideSac(state, S_OK, std::nullopt);
    EXPECT_FALSE(FindLine(Lines(s), L"Smart App Control", ' ')) << static_cast<int>(state);
  }
}

TEST(StatusFormat, Slots) {
  Snapshot s = MachineInstallSnapshot();
  s.slots = {std::nullopt, kOther};
  s.slotOwners[1].name = L"Other viewer";
  s.effective = kOther;
  s.effectiveOwner.name = L"Other viewer";
  s.live.reset();
  s.liveSkip = LiveTestSkip::kNotOurHandler;
  const std::vector<StatusLine> lines = Lines(s);
  std::optional<StatusLine> empty = FindLine(lines, L".pdf slot", '!', L".pdf -> empty");
  ASSERT_TRUE(empty);
  EXPECT_EQ(empty->hint, L"--check fills it");
  std::optional<StatusLine> other = FindLine(lines, L".pdf slot", '!', L"SystemFileAssociations");
  ASSERT_TRUE(other);
  EXPECT_EQ(other->text,
            L"SystemFileAssociations\\.pdf -> " + kOther + L" (Other viewer), another handler");
  EXPECT_EQ(other->hint, L"--fix --take-back points it back at " + kProduct);
  std::optional<StatusLine> effective = FindLine(lines, L"Effective", '!');
  ASSERT_TRUE(effective);
  EXPECT_EQ(effective->text, L"Windows resolves .pdf thumbnails to " + kOther + L" (Other viewer)");
  EXPECT_EQ(effective->hint, L"Fix the .pdf slots first (above)");
  EXPECT_TRUE(FindLine(lines, L"Explorer test", ' ',
                       L"skipped: Windows resolves .pdf thumbnails to another handler"));
  EXPECT_EQ(lines.back().text, L"Result: 3 problems keep PDF thumbnails from working");

  const std::wstring text = RenderStatus(lines);
  EXPECT_NE(
      text.find(L"\n  !! .pdf slot      .pdf -> empty\n                    --check fills it\n"),
      std::wstring::npos)
      << text;
  ExpectHintsOnProblems(s);
}

TEST(StatusFormat, DefaultAppBringsItsOwnHandler) {
  Snapshot s = MachineInstallSnapshot();
  s.effective = kOther;
  s.effectiveOwner.name = L"Acme thumbnails";
  s.defaultApp.progId = L"Acme.Pdf";
  s.defaultApp.handler = kOther;
  s.defaultAppHandlerOwner.name = L"Acme thumbnails";
  s.live.reset();
  s.liveSkip = LiveTestSkip::kNotOurHandler;
  const std::vector<StatusLine> lines = Lines(s);
  std::optional<StatusLine> effective = FindLine(lines, L"Effective", '!');
  ASSERT_TRUE(effective);
  EXPECT_NE(effective->hint.find(L"choose another default app"), std::wstring::npos);
  std::optional<StatusLine> app = FindLine(lines, L"Default app", '!');
  ASSERT_TRUE(app);
  EXPECT_EQ(app->text.rfind(L"Acme.Pdf (its icon is drawn on thumbnails)" + kDotText, 0), 0u)
      << app->text;
  EXPECT_NE(app->text.find(L"brings its own thumbnail handler " + kOther + L" (Acme thumbnails)"),
            std::wstring::npos)
      << app->text;
  ExpectHintsOnProblems(s);
}

TEST(StatusFormat, ExplorerTestFailures) {
  Snapshot s = UserInstallSnapshot();
  s.live->hr = REGDB_E_CLASSNOTREG;
  std::optional<StatusLine> line = FindLine(Lines(s), L"Explorer test", '!');
  ASSERT_TRUE(line);
  EXPECT_EQ(line->text,
            L"0x80040154: File Explorer's thumbnail process cannot load the per-user COM class");
  EXPECT_NE(line->hint.find(L"for all users"), std::wstring::npos) << line->hint;

  s = MachineInstallSnapshot();
  struct Case {
    HRESULT hr;
    std::wstring text;
    std::wstring hint;
  };
  for (const Case& c : {
           Case{E_FAIL, L"extraction failed: 0x80004005", L"--test-file"},
           Case{HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                L"no answer from File Explorer's thumbnail process within 60 seconds",
                L"Run --status again"},
           // Smart App Control is off here: an App Control policy or a damaged file blocked it.
           Case{static_cast<HRESULT>(0x800711C7), L"Windows refused to load the DLL, 0x800711C7",
                L"An application control policy (or a damaged file) blocked the DLL"},
           Case{REGDB_E_CLASSNOTREG, L"0x80040154: the COM class is not registered",
                L"--test-file"},
       }) {
    s.live->hr = c.hr;
    line = FindLine(Lines(s), L"Explorer test", '!');
    ASSERT_TRUE(line) << HResultText(c.hr);
    EXPECT_EQ(line->text, c.text);
    EXPECT_NE(line->hint.find(c.hint), std::wstring::npos) << line->hint;
  }
  EXPECT_FALSE(FindLine(Lines(s), L"Smart App Control", ' '));

  // With Smart App Control on, the same error is its block, and its line is there.
  s.live->hr = static_cast<HRESULT>(0x800711C7);
  s.sac.state = SacState::kOn;
  s.sacVerdict = DecideSac(SacState::kOn, s.live->hr, std::nullopt);
  line = FindLine(Lines(s), L"Explorer test", '!');
  ASSERT_TRUE(line);
  EXPECT_EQ(line->hint, L"See the Smart App Control line");
  EXPECT_TRUE(FindLine(Lines(s), L"Smart App Control", '!'));

  s = MachineInstallSnapshot();
  s.live.reset();
  s.liveError = E_OUTOFMEMORY;
  EXPECT_TRUE(FindLine(Lines(s), L"Explorer test", ' ', L"could not run: 0x8007000E"));
  EXPECT_EQ(Lines(s).back().text, L"Result: healthy");

  // Where the command needed the test's answer (--install), a test that could not run leaves
  // the result unconfirmed.
  s.liveRequired = true;
  line = FindLine(Lines(s), L"Explorer test", '!', L"could not run: 0x8007000E");
  ASSERT_TRUE(line);
  EXPECT_EQ(line->hint, L"Run --status to try it again");
  EXPECT_EQ(Lines(s).back().text,
            L"Result: no problem found, but not confirmed (the Explorer test did not run)");
  ExpectHintsOnProblems(s);

  // ... and one skipped because Windows resolves another handler: that problem comes first.
  s.liveError = S_OK;
  s.liveSkip = LiveTestSkip::kNotOurHandler;
  s.effective = kOther;
  line = FindLine(Lines(s), L"Explorer test", '!',
                  L"skipped: Windows resolves .pdf thumbnails to another handler");
  ASSERT_TRUE(line);
  EXPECT_EQ(line->hint, L"Run --status once the problems above are fixed");
  EXPECT_EQ(Lines(s).back().text, L"Result: 1 problem keeps PDF thumbnails from working");
  ExpectHintsOnProblems(s);
}

TEST(StatusFormat, SmartAppControlTexts) {
  Snapshot s = MachineInstallSnapshot();
  s.sac.state = SacState::kOn;
  s.blocks = BlockCount{true, 0, S_OK};
  const std::wstring on =
      L"On; Code Integrity logged no blocks of the installed DLLs in the last "
      L"7 days";
  struct Case {
    SacVerdict verdict;
    char mark;
    std::wstring text;
  };
  for (const Case& c : {
           Case{{SacVerdictKind::kBlocked, 3, std::nullopt},
                '!',
                L"On and blocking " + kProduct +
                    L" (Code Integrity blocked the installed DLLs 3 time(s) in the last 7 days)"},
           Case{{SacVerdictKind::kBlocked, 0, static_cast<HRESULT>(0x800711C7)},
                '!',
                L"On and blocking " + kProduct + L" (Windows refused to load the DLL, 0x800711C7)"},
           Case{{SacVerdictKind::kOn, 0, std::nullopt}, ' ', on},
           Case{{SacVerdictKind::kOnUnknown, 0, std::nullopt},
                ' ',
                L"On; the Code Integrity log could not be read"},
           Case{{SacVerdictKind::kEval, 0, std::nullopt},
                ' ',
                L"in evaluation mode; if Windows switches it ON, it may block these unsigned "
                L"DLLs"},
       }) {
    s.sacVerdict = c.verdict;
    std::optional<StatusLine> line = FindLine(Lines(s), L"Smart App Control", c.mark);
    ASSERT_TRUE(line) << static_cast<int>(c.verdict.kind);
    EXPECT_EQ(line->text, c.text);
    if (c.mark == '!')
      EXPECT_EQ(line->hint, L"Turn off Smart App Control, or use a signed release");
  }

  s.sac = {SacState::kUnknown, 7, REG_DWORD};
  s.sacVerdict = DecideSac(SacState::kUnknown, S_OK, std::nullopt);
  EXPECT_TRUE(FindLine(Lines(s), L"Smart App Control", ' ', L"state unknown (7)"));

  // Without a class, 1.x said only that it is on.
  Snapshot empty = EmptySnapshot();
  empty.sac.state = SacState::kOn;
  empty.sacVerdict = DecideSac(SacState::kOn, std::nullopt, std::nullopt);
  std::optional<StatusLine> line = FindLine(Lines(empty), L"Smart App Control", ' ');
  ASSERT_TRUE(line);
  EXPECT_EQ(line->text, L"On");

  // A class whose DLL is gone: nothing was counted, so nothing is said about blocks.
  s.sac = {SacState::kOn, 1, REG_DWORD};
  s.blocks.reset();
  s.sacVerdict = DecideSac(SacState::kOn, E_FAIL, std::nullopt);
  line = FindLine(Lines(s), L"Smart App Control", ' ');
  ASSERT_TRUE(line);
  EXPECT_EQ(line->text, L"On");
}

TEST(StatusFormat, DamagedClasses) {
  Snapshot s = MachineInstallSnapshot();
  s.machineClass->dllExists = false;
  s.machineClass->pdfiumExists = false;
  std::vector<StatusLine> lines = Lines(s);
  std::optional<StatusLine> handler = FindLine(lines, L"Handler", '!', L"(missing)");
  ASSERT_TRUE(handler);
  // A complete build for all users and the settings executable there: --fix repairs it.
  const std::wstring fix =
      L"--fix points it at the newest complete build for all users (asks for administrator "
      L"rights)";
  const std::wstring reinstall =
      L"Reinstall " + kProduct + L" for all users (needs administrator rights)";
  EXPECT_EQ(handler->hint, fix);
  EXPECT_FALSE(FindLine(lines, L"PDFium", '!')) << "nothing to look for next to a missing DLL";
  ExpectHintsOnProblems(s);

  // A machine-wide class that points outside the install's build folders.
  s = MachineInstallSnapshot();
  const std::wstring elsewhere = (kUserRoot / L"app" / L"2.0.0.0-1a2b3c4d" / kHandlerDll).native();
  s.machineClass->dll = elsewhere;
  lines = Lines(s);
  handler = FindLine(lines, L"Handler", '!');
  ASSERT_TRUE(handler);
  EXPECT_EQ(handler->text, elsewhere + L" (2.0.0.0, " + kNativeArchName +
                               L", Apartment); outside " + kMachineRoot.native() + L"\\app");
  EXPECT_EQ(handler->hint, fix);
  EXPECT_TRUE(FindLine(lines, L"PDFium", 'K'));
  Finding misplaced;
  misplaced.id = FindingId::kClassDamagedMachine;
  misplaced.path = elsewhere;
  EXPECT_EQ(FindingText(s, misplaced), L"the machine-wide class points at " + elsewhere +
                                           L", outside " + kMachineRoot.native() + L"\\app");
  ExpectHintsOnProblems(s);

  // A per-user class with PDFium missing names the build a repair registers; that is not a
  // machine-wide class pointing elsewhere.
  s = UserInstallSnapshot();
  s.userClass->pdfiumExists = false;
  EXPECT_TRUE(FindLine(Lines(s), L"Handler", 'K', L", Apartment)"));
  EXPECT_FALSE(FindLine(Lines(s), L"Handler", 'K', L"outside"));

  s = UserInstallSnapshot();
  s.userClass->pdfiumExists = false;
  lines = Lines(s);
  std::optional<StatusLine> pdfium = FindLine(lines, L"PDFium", '!');
  ASSERT_TRUE(pdfium);
  EXPECT_EQ(pdfium->text, std::wstring(kPdfiumDll) + L" missing next to the handler");
  EXPECT_EQ(pdfium->hint, L"--check registers the newest per-user build again");
  EXPECT_TRUE(FindLine(lines, L"Handler", 'K'));

  s = MachineInstallSnapshot();
  s.machineClass->threading = L"Both";
  handler = FindLine(Lines(s), L"Handler", '!', L", Both)");
  ASSERT_TRUE(handler);
  EXPECT_EQ(handler->hint, fix);
  // Without the settings executable for all users, or without a complete build there, only a
  // new install for all users helps; elevated, --fix must come from a normal prompt.
  s.settingsExeFound = false;
  handler = FindLine(Lines(s), L"Handler", '!', L", Both)");
  ASSERT_TRUE(handler);
  EXPECT_EQ(handler->hint, reinstall);
  s = MachineInstallSnapshot();
  s.machineClass->dllExists = false;
  s.machineBuilds[0].folder.complete = false;
  s.machineBuilds[0].dllMachine.reset();
  handler = FindLine(Lines(s), L"Handler", '!');
  ASSERT_TRUE(handler);
  EXPECT_EQ(handler->hint, reinstall);
  s = MachineInstallSnapshot();
  s.machineClass->dllExists = false;
  s.context.elevated = true;
  handler = FindLine(Lines(s), L"Handler", '!');
  ASSERT_TRUE(handler);
  EXPECT_EQ(handler->hint, fix + L" from a normal, not administrator, prompt");

  s = MachineInstallSnapshot();
  s.machineClass->peMachine = kForeignMachine;
  handler = FindLine(Lines(s), L"Handler", '!', kForeignArchName);
  ASSERT_TRUE(handler);
  EXPECT_EQ(handler->hint,
            L"Reinstall the " + std::wstring(kNativeArchName) + L" build of " + kProduct);

  s = MachineInstallSnapshot();
  s.userBuilds = {MakeBuild(kUserRoot, L"2.0.0.0-1a2b3c4d")};
  s.userClass = MakeClass(DllIn(s.userBuilds[0]));
  std::optional<StatusLine> shadow = FindLine(Lines(s), L"Per-user class", '!');
  ASSERT_TRUE(shadow);
  EXPECT_EQ(shadow->text, DllIn(s.userBuilds[0]).native() + L" hides the machine-wide class");
  EXPECT_EQ(shadow->hint, L"--check removes the per-user class");

  s = EmptySnapshot();
  s.userBuilds = {MakeBuild(kUserRoot, L"2.0.0.0-1a2b3c4d")};
  handler = FindLine(Lines(s), L"Handler", '!');
  ASSERT_TRUE(handler);
  EXPECT_EQ(handler->text, L"not registered; a complete per-user build is ready: " +
                               DllIn(s.userBuilds[0]).native());
  EXPECT_EQ(handler->hint, L"--check registers the newest per-user build again");
  EXPECT_TRUE(FindLine(Lines(s), L"Install", ' ', L"per user ("));
}

TEST(StatusFormat, LinesForFindingsWithoutACheckLine) {
  Snapshot s = MachineInstallSnapshot();
  s.explorer = {true, true, false};
  s.legacy.runOnce = true;
  s.context.differentFromShellUser = true;
  s.userBuilds = {MakeBuild(kUserRoot, L"2.0.0.0-00000000")};
  const std::vector<StatusLine> lines = Lines(s);
  std::optional<StatusLine> icons = FindLine(lines, L"Explorer", '!');
  ASSERT_TRUE(icons);
  EXPECT_EQ(icons->hint, L"Turn it off in File Explorer: Options > View");
  std::optional<StatusLine> policy = FindLine(lines, L"Policy", '!', L"for this user");
  ASSERT_TRUE(policy);
  std::optional<StatusLine> cleanup = FindLine(lines, L"1.x clean-up", '!');
  ASSERT_TRUE(cleanup);
  EXPECT_NE(cleanup->text.find(kLegacyRunOnce), std::wstring::npos);
  EXPECT_EQ(cleanup->hint, L"--check deletes the pending clean-up");
  std::optional<StatusLine> account =
      FindLine(lines, L"Account", '!', L"running as DESK-42\\someone");
  ASSERT_TRUE(account);
  std::optional<StatusLine> old = FindLine(lines, L"Old builds", ' ');
  ASSERT_TRUE(old);
  EXPECT_EQ(old->text, L"1 build folder that nothing uses: 2.0.0.0-00000000");
  EXPECT_FALSE(old->hint.empty());
  // They do not keep thumbnails from working.
  EXPECT_EQ(lines.back().text, L"Result: healthy");
  ExpectHintsOnProblems(s);
}

TEST(StatusFormat, TaskAndZoomProblems) {
  Snapshot s = MachineInstallSnapshot();
  s.task.enabled = false;
  s.task.actionPath = L"C:\\old\\settings.exe";
  s.task.atSignIn = false;
  std::optional<StatusLine> checks = FindLine(Lines(s), L"Checks", '!');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->text.rfind(L"after " + std::wstring(kPdfCraftProductName) + L" updates: on" +
                                   kDotText +
                                   L"at sign-in: off; the check task is disabled; "
                                   L"the check task runs C:\\old\\settings.exe " +
                                   kCheckTaskArguments + L" instead of ",
                               0),
            0u)
      << checks->text;
  // The checkbox the task shows on stays on when it is turned on again.
  EXPECT_NE(checks->hint.find(L"--checks pdfcraft turns it on again"), std::wstring::npos)
      << checks->hint;
  EXPECT_NE(checks->hint.find(L"--check registers it again with the same triggers"),
            std::wstring::npos)
      << checks->hint;

  s = MachineInstallSnapshot();
  s.zoom.decision = {200, ZoomOrigin::kRegistry, ZoomValueState::kOutOfRange};
  s.zoom.registryType = REG_DWORD;
  s.zoom.registryRaw = 500;
  std::optional<StatusLine> zoom = FindLine(Lines(s), L"Zoom", '!');
  ASSERT_TRUE(zoom);
  EXPECT_EQ(zoom->text,
            L"200 % (registry); the Zoom value in the registry is out of range (500); thumbnails "
            L"use 200 %");
  EXPECT_EQ(zoom->hint, L"--check rewrites it");

  s.zoom.decision = {130, ZoomOrigin::kLegacyIni, ZoomValueState::kWrongType};
  s.zoom.registryType = REG_SZ;
  s.zoom.registryRaw.reset();
  zoom = FindLine(Lines(s), L"Zoom", '!');
  ASSERT_TRUE(zoom);
  EXPECT_NE(zoom->text.find(L"of the wrong type (REG_SZ)"), std::wstring::npos) << zoom->text;

  // A zoom saved for 2.x does not reach a registered 1.x handler.
  s = ThisPcSnapshot();
  s.zoom.decision = {150, ZoomOrigin::kRegistry, ZoomValueState::kValid};
  EXPECT_TRUE(FindLine(Lines(s), L"Zoom", ' ',
                       L"150 % (registry); the registered 1.x handler reads only settings.ini "
                       L"(130 %)"));

  // A recorded PdfCraft change shows next to the version.
  s = ThisPcSnapshot();
  s.last = CheckRecord{};
  s.last->pdfcraftVersion = L"0.13.0";
  EXPECT_TRUE(FindLine(Lines(s), L"Default app", ' ',
                       std::wstring(kPdfCraftProductName) + L" 0.14.0 (0.13.0 at the last check)"));
}

// The checks are off: the Checks line names --checks only where it would work, which needs the
// installed settings executable (this PC's 1.x build has none) and a normal prompt.
TEST(StatusFormat, ChecksOffHintOnlyWhereChecksCanTurnThemOn) {
  Snapshot s = MachineInstallSnapshot();
  s.task = {};
  std::optional<StatusLine> checks = FindLine(Lines(s), L"Checks", ' ');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->text, L"off");
  EXPECT_EQ(checks->hint, L"--checks default turns them on");

  s.context.elevated = true;
  checks = FindLine(Lines(s), L"Checks", ' ');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint,
            L"--checks default turns them on (from a normal, not administrator, prompt)");

  // Installed, but without the program the task would run.
  s.context.elevated = false;
  s.settingsExeFound = false;
  checks = FindLine(Lines(s), L"Checks", ' ');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint, L"");

  // This PC: 1.1 installed, 1.x's task still there.
  checks = FindLine(Lines(ThisPcSnapshot()), L"Checks", ' ');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint, L"");
  EXPECT_EQ(checks->text.find(L"--checks"), std::wstring::npos);

  // Nothing installed: no finding, no hint.
  checks = FindLine(Lines(EmptySnapshot()), L"Checks", ' ');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint, L"");
}

// Another account's tasks hold every name the user's check task may have (the gathering keeps
// ReadTask's answer as an issue): the checks are off not by choice, and only an administrator can
// change that, so the Checks line is a problem whose hint names those tasks.
TEST(StatusFormat, ChecksOffWhileAnotherAccountHoldsTheTaskNames) {
  Snapshot s = MachineInstallSnapshot();
  s.task = {};
  ASSERT_HRESULT_SUCCEEDED(DefaultTaskLocation(s.context.userSid, &s.taskLocation));
  s.issues.push_back({L"task", kTaskTakenHr});
  const std::wstring administrator =
      L"An administrator can delete " + std::wstring(kTaskFolder) + L"\\" + kTaskPrefix +
      s.context.userSid + L" and \\" + kTaskRootPrefix + s.context.userSid +
      L" in Task Scheduler (uninstalling " + kProduct + L" for all users deletes them too)";
  EXPECT_EQ(TaskTakenHint(s.taskLocation), administrator);
  std::optional<StatusLine> checks = FindLine(Lines(s), L"Checks", '!');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->text,
            L"off; another account's scheduled tasks hold the names of this user's check task");
  EXPECT_EQ(checks->hint, administrator + L"; then --checks default turns them on");
  ExpectHintsOnProblems(s);
  bool found = false;
  for (const Finding& finding : Evaluate(s)) {
    if (finding.id != FindingId::kChecksOff) continue;
    found = true;
    EXPECT_EQ(FindingText(s, finding), L"no scheduled check: " + TaskTakenText());
    EXPECT_EQ(FindingHint(s, finding), checks->hint);
  }
  EXPECT_TRUE(found);

  // From an administrator prompt, --checks is named for a normal one; without the program the
  // task would run, only the administrator's part is left.
  s.context.elevated = true;
  checks = FindLine(Lines(s), L"Checks", '!');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint, administrator +
                              L"; then --checks default turns them on (from a normal, not "
                              L"administrator, prompt)");
  s.context.elevated = false;
  s.settingsExeFound = false;
  checks = FindLine(Lines(s), L"Checks", '!');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint, administrator);

  // Any other reason the task could not be read is the plain "off".
  s = MachineInstallSnapshot();
  s.task = {};
  s.issues.push_back({L"task", E_ACCESSDENIED});
  checks = FindLine(Lines(s), L"Checks", ' ');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->text, L"off");
  EXPECT_EQ(checks->hint, L"--checks default turns them on");

  // Nothing installed: nothing to turn on, so nothing to say.
  s = EmptySnapshot();
  s.issues.push_back({L"task", kTaskTakenHr});
  checks = FindLine(Lines(s), L"Checks", ' ');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->text, L"off");
  EXPECT_EQ(checks->hint, L"");

  // Without the task's names, the hint still says who can remove the tasks.
  EXPECT_EQ(TaskTakenHint(TaskLocation{}),
            L"An administrator can delete them in Task Scheduler (uninstalling " + kProduct +
                L" for all users deletes them too)");
}

TEST(StatusFormat, TaskDisabledAndStaleWithoutTheProgram) {
  Snapshot s = MachineInstallSnapshot();
  s.task.enabled = false;
  s.task.afterPdfCraft = false;
  s.task.atSignIn = false;  // no trigger on: turning it on again asks for both
  std::optional<StatusLine> checks = FindLine(Lines(s), L"Checks", '!');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint, L"--checks default turns it on again");

  s.task.atSignIn = true;
  checks = FindLine(Lines(s), L"Checks", '!');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint, L"--checks signin turns it on again");

  // The installed program is gone: --checks cannot register, the check cannot repair.
  s.settingsExeFound = false;
  s.task.actionPath = L"D:\\Elsewhere\\" + std::wstring(kSettingsExe);
  checks = FindLine(Lines(s), L"Checks", '!');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint, L"Turn it on again in Task Scheduler; Reinstall " + kProduct +
                              L"; until then --checks none removes the task");
  ExpectHintsOnProblems(s);

  // The program is there but the task has neither of the check's triggers: the check cannot
  // register it again with the same ones, --checks can with the ones the user picks.
  s = MachineInstallSnapshot();
  s.task.afterPdfCraft = false;
  s.task.atSignIn = false;
  s.task.options = {TriggerSetting::kAbsent, TriggerSetting::kAbsent, true};
  s.task.actionPath = L"D:\\Elsewhere\\" + std::wstring(kSettingsExe);
  checks = FindLine(Lines(s), L"Checks", '!');
  ASSERT_TRUE(checks);
  EXPECT_EQ(checks->hint, L"--checks default registers it again with both triggers");
  ExpectHintsOnProblems(s);
}

TEST(StatusFormat, ChecksTexts) {
  EXPECT_EQ(ChecksStateText(true, false), L"after " + std::wstring(kPdfCraftProductName) +
                                              L" updates: on" + kDotText + L"at sign-in: off");
  EXPECT_EQ(std::wstring(ChecksValue(true, true)), L"default");
  EXPECT_EQ(std::wstring(ChecksValue(true, false)), L"pdfcraft");
  EXPECT_EQ(std::wstring(ChecksValue(false, true)), L"signin");
  EXPECT_EQ(std::wstring(ChecksValue(false, false)), L"none");
}

// What --checks prints after it changed the task, or why it did not.
TEST(StatusFormat, ChecksReports) {
  ScheduleResult result;
  result.before = UserInstallSnapshot();
  result.code = ExitCode::kOk;
  result.askedAfterPdfCraft = true;
  result.askedAtSignIn = false;
  result.program = kUserRoot / kSettingsExe;
  result.after.exists = true;
  result.after.enabled = true;
  result.after.afterPdfCraft = true;
  result.after.atFallback = true;
  result.after.path = L"\\" + kProduct + L" check S-1-12-1-1-2-3-4";
  result.after.actionPath = result.program.native();
  result.after.actionArgs = kCheckTaskArguments;
  ScheduleReport report = ScheduleResultReport(result);
  EXPECT_EQ(report.error, L"");
  ASSERT_EQ(report.out.size(), 2u);
  EXPECT_EQ(report.out[0], L"Checks: " + ChecksStateText(true, false));
  EXPECT_EQ(report.out[1], L"Check task: " + result.after.path + L", runs " +
                               result.program.native() + L" " + kCheckTaskArguments);

  // Read back failed: what was asked for, and why the task is not shown.
  result.readError = E_ACCESSDENIED;
  result.after = {};
  report = ScheduleResultReport(result);
  ASSERT_EQ(report.out.size(), 2u);
  EXPECT_EQ(report.out[0], L"Checks: " + ChecksStateText(true, false));
  EXPECT_EQ(report.out[1], L"The check task was changed but could not be read back: 0x80070005");

  // Off: removed, or there was none; 1.x's task is named, never touched.
  result = {};
  result.before = ThisPcSnapshot();
  result.code = ExitCode::kOk;
  result.remove = true;
  report = ScheduleResultReport(result);
  ASSERT_EQ(report.out.size(), 2u);
  EXPECT_EQ(report.out[0], L"Checks: off (there was no check task)");
  EXPECT_EQ(report.out[1], L"The 1.x task " + std::wstring(kLegacyTask) +
                               L" is still there; it was left as it is.");
  result.before = UserInstallSnapshot();
  report = ScheduleResultReport(result);
  EXPECT_EQ(report.out, (std::vector<std::wstring>{L"Checks: off (the check task was removed)"}));

  // Off where another account's tasks hold both names: nothing of this user's was changed or
  // read back, and the names are said in words, with who can free them.
  result.before = UserInstallSnapshot();
  result.before.task = {};
  ASSERT_HRESULT_SUCCEEDED(
      DefaultTaskLocation(result.before.context.userSid, &result.before.taskLocation));
  result.readError = kTaskTakenHr;
  report = ScheduleResultReport(result);
  ASSERT_EQ(report.out.size(), 2u);
  EXPECT_EQ(report.out[0], L"Checks: off (there was no check task)");
  EXPECT_EQ(report.out[1],
            L"The check task cannot be registered while another account's scheduled tasks hold "
            L"the names of this user's check task (" +
                HResultText(kTaskTakenHr) + L"). " + TaskTakenHint(result.before.taskLocation) +
                L".");
  for (const std::wstring& line : report.out) {
    EXPECT_EQ(line.find(L"could not be read back"), std::wstring::npos) << WideToUtf8Lossy(line);
  }
  result.readError = S_OK;

  // Refused: nothing installed, a 1.x build, an install without the program.
  result = {};
  result.refusal = ScheduleRefusal::kNotInstalled;
  result.code = ExitCode::kNotInstalled;
  report = ScheduleResultReport(result);
  EXPECT_TRUE(report.out.empty());
  EXPECT_EQ(report.error.rfind(kProduct + L" is not installed", 0), 0u) << report.error;
  result.refusal = ScheduleRefusal::kNoSettingsExe;
  result.before = ThisPcSnapshot();
  report = ScheduleResultReport(result);
  EXPECT_TRUE(report.out.empty());
  EXPECT_EQ(report.error, L"The installed " + kProduct + L" is a 1.x build, which has no " +
                              (kMachineRoot / kSettingsExe).native() +
                              L" for the scheduled checks to run. Install this version first.");
  result.before = MachineInstallSnapshot();
  result.before.settingsExeFound = false;
  report = ScheduleResultReport(result);
  EXPECT_EQ(report.error, (kMachineRoot / kSettingsExe).native() +
                              L" is not there, so a scheduled check would have nothing to run. "
                              L"Reinstall " +
                              kProduct + L".");
  result.before = EmptySnapshot();
  report = ScheduleResultReport(result);
  EXPECT_NE(report.error.find(L"Run --check first"), std::wstring::npos) << report.error;

  // The change failed.
  result = {};
  result.before = UserInstallSnapshot();
  result.code = ExitCode::kFailed;
  result.error = E_ACCESSDENIED;
  report = ScheduleResultReport(result);
  EXPECT_TRUE(report.out.empty());
  EXPECT_EQ(report.error, L"Could not change the check task: 0x80070005");
  result.error = kSomeTriggersFailed;
  report = ScheduleResultReport(result);
  EXPECT_EQ(report.error.rfind(L"Task Scheduler registered the check task but could not set up "
                               L"all of its triggers (0x8004131B)",
                               0),
            0u)
      << report.error;

  // Another account's tasks hold every name the task may have: in words, which tasks, and who
  // can remove them.
  ASSERT_HRESULT_SUCCEEDED(
      DefaultTaskLocation(result.before.context.userSid, &result.before.taskLocation));
  result.error = kTaskTakenHr;
  report = ScheduleResultReport(result);
  EXPECT_TRUE(report.out.empty());
  EXPECT_EQ(report.error,
            L"Could not register the check task: another account's scheduled tasks hold the names "
            L"of this user's check task (" +
                HResultText(kTaskTakenHr) + L"). " + TaskTakenHint(result.before.taskLocation) +
                L"; then run --checks again.");
}

TEST(StatusFormat, WhereTheWindowsChecksCannotRun) {
  Snapshot s = MachineInstallSnapshot();
  s.context.views = RegistryViews::kDiffer;
  s.windowsChecksSkipped = true;
  s.windowsChecksSkip = WindowsChecksSkip::kViewsDiffer;
  s.effective.reset();
  s.live.reset();
  s.liveSkip = LiveTestSkip::kVirtualized;
  const std::vector<StatusLine> lines = Lines(s);
  std::optional<StatusLine> effective = FindLine(lines, L"Effective", ' ');
  ASSERT_TRUE(effective);
  EXPECT_EQ(effective->text,
            L"not checked: this process sees a private copy of the registry (it runs inside an "
            L"app package)");
  EXPECT_EQ(effective->hint,
            L"Run it from File Explorer or from a terminal that is not part of an app package");
  EXPECT_TRUE(FindLine(lines, L"Default app", ' ',
                       L"not checked" + kDotText + kPdfCraftProductName + L" not found"));
  EXPECT_TRUE(FindLine(lines, L"Explorer test", ' ',
                       L"skipped: this process may not see the registry File Explorer sees"));
  EXPECT_EQ(lines.back().text, L"Result: healthy");
  EXPECT_TRUE(FindLine(lines, L".pdf slot", ' ',
                       L".pdf -> " + kProduct + L" (as this process sees it; not checked)"));
  EXPECT_FALSE(FindLine(lines, L".pdf slot", 'K'));

  // The slots this process reads may be its private copy: an empty one is not a problem there.
  s.slots = {std::nullopt, kOther};
  const std::vector<StatusLine> privateCopy = Lines(s);
  EXPECT_TRUE(FindLine(privateCopy, L".pdf slot", ' ',
                       L".pdf -> empty (as this process sees it; not checked)"));
  EXPECT_FALSE(FindLine(privateCopy, L".pdf slot", '!'));
  EXPECT_EQ(privateCopy.back().text, L"Result: healthy");

  s = MachineInstallSnapshot();
  s.context.elevated = true;
  s.live.reset();
  s.liveSkip = LiveTestSkip::kElevated;
  const std::vector<StatusLine> elevated = Lines(s);
  EXPECT_TRUE(FindLine(elevated, L"Explorer test", ' ', L"skipped: this process is elevated"));
  std::optional<StatusLine> note = FindLine(elevated, L"Note", ' ', L"running as administrator");
  ASSERT_TRUE(note);
  EXPECT_FALSE(note->hint.empty());

  s = MachineInstallSnapshot();
  s.live.reset();
  s.liveSkip = LiveTestSkip::kNotRequested;
  EXPECT_TRUE(
      FindLine(Lines(s), L"Explorer test", ' ', L"skipped: not requested (--no-live-test)"));
}

TEST(StatusFormat, TestFile) {
  Snapshot s = MachineInstallSnapshot();
  TestFileRun run;
  run.requested = L"D:\\Home\\someone\\private.pdf";
  run.resolved = run.requested;
  run.result = LiveTest{S_OK, {181, 256}, 40};
  s.testFile = run;
  std::optional<StatusLine> line = FindLine(Lines(s), L"Test file", 'K');
  ASSERT_TRUE(line);
  EXPECT_EQ(line->text, L"181x256 thumbnail extracted in 40 ms");
  // The status never names the user's file.
  EXPECT_EQ(RenderStatus(Lines(s)).find(L"private.pdf"), std::wstring::npos);

  s.testFile->result->hr = E_FAIL;
  line = FindLine(Lines(s), L"Test file", '!');
  ASSERT_TRUE(line);
  EXPECT_EQ(line->text, L"no thumbnail: extraction failed: 0x80004005");
  EXPECT_FALSE(line->hint.empty());

  // The test could not even start: a problem line, and the result says so too.
  s.testFile->result.reset();
  s.testFile->error = E_OUTOFMEMORY;
  line = FindLine(Lines(s), L"Test file", '!');
  ASSERT_TRUE(line);
  EXPECT_EQ(line->text, L"could not run: 0x8007000E");
  EXPECT_FALSE(line->hint.empty());
  EXPECT_NE(Report(s).back(), L"Result: healthy");

  s.testFile->error = S_OK;
  s.testFile->skip = LiveTestSkip::kVirtualized;
  EXPECT_TRUE(FindLine(Lines(s), L"Test file", ' ', L"skipped: this process may not see"));

  s.testFile->skip = LiveTestSkip::kNone;
  s.testFile->problem = TestFileProblem::kNotFound;
  line = FindLine(Lines(s), L"Test file", '!');
  ASSERT_TRUE(line);
  EXPECT_EQ(line->text, L"the file does not exist");
  ExpectHintsOnProblems(s);
}

TEST(StatusFormat, EveryFindingHasAText) {
  const Snapshot s = ThisPcSnapshot();
  for (int id = 0; id <= static_cast<int>(FindingId::kVirtualized); ++id) {
    Finding finding;
    finding.id = static_cast<FindingId>(id);
    const std::wstring text = FindingText(s, finding);
    EXPECT_FALSE(text.empty()) << FindingName(finding.id);
    EXPECT_EQ(text.find(L'\n'), std::wstring::npos) << FindingName(finding.id);
    EXPECT_NE(text, FindingName(finding.id)) << "no text of its own";
  }
}

TEST(StatusFormat, Layout) {
  const std::vector<StatusLine> lines = {
      {' ', L"", L"Header", L""},
      {'K', L"Handler", L"fine", L""},
      {'!', L".pdf slot", L"broken", L"fix it"},
      {' ', L"Smart App Control", L"On", L""},
      {' ', L"A label of 15ch", L"text", L""},
      {' ', L"", L"Result: healthy", L""},
  };
  EXPECT_EQ(RenderStatus(lines),
            L"Header\n"
            L"  OK Handler        fine\n"
            L"  !! .pdf slot      broken\n"
            L"                    fix it\n"
            L"     Smart App Control  On\n"
            L"     A label of 15ch  text\n"
            L"Result: healthy");
}

TEST(StatusFormat, Results) {
  EXPECT_EQ(ResultText({}), L"Result: healthy");
  Finding breaks;
  breaks.id = FindingId::kSlotEmpty;
  breaks.breaksThumbnails = true;
  Finding info;
  info.id = FindingId::kV1Leftovers;
  EXPECT_EQ(ResultText(std::vector<Finding>{info}), L"Result: healthy");
  EXPECT_EQ(ResultText(std::vector<Finding>{breaks, info}),
            L"Result: 1 problem keeps PDF thumbnails from working");
  EXPECT_EQ(ResultText(std::vector<Finding>{breaks, breaks}),
            L"Result: 2 problems keep PDF thumbnails from working");
  Finding notInstalled;
  notInstalled.id = FindingId::kNotInstalled;
  notInstalled.breaksThumbnails = true;
  EXPECT_EQ(ResultText(std::vector<Finding>{notInstalled}), L"Result: not installed");
  // An Explorer test the command needed that did not run: unconfirmed, unless a problem is
  // already known.
  Finding unconfirmed;
  unconfirmed.id = FindingId::kLiveTestSkipped;
  unconfirmed.severity = Severity::kWarning;
  EXPECT_EQ(ResultText(std::vector<Finding>{unconfirmed}),
            L"Result: no problem found, but not confirmed (the Explorer test did not run)");
  EXPECT_EQ(ResultText(std::vector<Finding>{breaks, unconfirmed}),
            L"Result: 1 problem keeps PDF thumbnails from working");
  Finding skipped;
  skipped.id = FindingId::kLiveTestSkipped;
  EXPECT_EQ(ResultText(std::vector<Finding>{skipped}), L"Result: healthy");
}

TEST(StatusFormat, Times) {
  SYSTEMTIME time = {2026, 10, 5, 30, 1, 26, 4, 512};
  EXPECT_EQ(ShortDateTimeText(time), L"30 Oct 2026 01:26");
  time.wMonth = 13;
  EXPECT_EQ(ShortDateTimeText(time), L"");
  time.wMonth = 1;
  time.wDay = 2;
  time.wHour = 23;
  time.wMinute = 5;
  EXPECT_EQ(ShortDateTimeText(time), L"2 Jan 2026 23:05");

  SYSTEMTIME local = {2026, 10, 5, 30, 1, 26, 0, 0};
  DATE date = 0;
  ASSERT_TRUE(SystemTimeToVariantTime(&local, &date));
  EXPECT_EQ(TaskTimeText(date), L"30 Oct 2026 01:26");
  EXPECT_EQ(TaskTimeText(0), L"");
  EXPECT_EQ(TaskTimeText(-1), L"");
}

TEST(StatusFormat, RepairOutcomes) {
  RepairOutcome outcome;
  outcome.step.kind = RepairStep::Kind::kFillSlot;
  outcome.step.slot = 0;
  outcome.result = RepairResult::kDone;
  EXPECT_EQ(RepairOutcomeText(outcome), L"Done: point the empty .pdf slot at " + kProduct);
  outcome.step.kind = RepairStep::Kind::kRemoveUserBuild;
  outcome.step.path = kUserRoot / L"app" / L"2.0.0.0-00000000";
  outcome.result = RepairResult::kKept;
  EXPECT_EQ(RepairOutcomeText(outcome),
            L"Left for the next check (still in use): remove the old build folder "
            L"2.0.0.0-00000000");
  outcome.step.kind = RepairStep::Kind::kReRegisterTask;
  outcome.step.path = kUserRoot / kSettingsExe;
  outcome.result = RepairResult::kDone;
  EXPECT_EQ(RepairOutcomeText(outcome),
            L"Done: register the check task again for " + (kUserRoot / kSettingsExe).native());
  outcome.result = RepairResult::kNotApplied;
  outcome.notApplied = NotApplied::kElevated;
  EXPECT_EQ(RepairOutcomeText(outcome).rfind(
                L"Not done, running as administrator (a normal check does it): ", 0),
            0u);
  outcome.notApplied = NotApplied::kNoProgram;
  EXPECT_EQ(RepairOutcomeText(outcome).rfind(L"Not done, the installed program is not there: ", 0),
            0u);
  outcome.notApplied = NotApplied::kNoTrigger;
  EXPECT_NE(RepairOutcomeText(outcome).find(L"(choose them with --checks)"), std::wstring::npos);
  outcome.step.kind = RepairStep::Kind::kRemoveUserBuild;
  outcome.step.path = kUserRoot / L"app" / L"2.0.0.0-00000000";
  outcome.notApplied = NotApplied::kElevated;
  EXPECT_EQ(RepairOutcomeText(outcome),
            L"Not done, running as administrator (a normal check removes it): remove the old "
            L"build folder 2.0.0.0-00000000");
  outcome.notApplied = NotApplied::kNone;
  outcome.step.kind = RepairStep::Kind::kRewriteZoom;
  outcome.step.zoom = 200;
  outcome.result = RepairResult::kFailed;
  outcome.hr = E_ACCESSDENIED;
  EXPECT_EQ(RepairOutcomeText(outcome), L"Failed (0x80070005): rewrite the zoom value as 200 %");
  // The task's names held by another account's tasks: why, in words.
  outcome.step.kind = RepairStep::Kind::kReRegisterTask;
  outcome.step.path = kUserRoot / kSettingsExe;
  outcome.hr = kTaskTakenHr;
  EXPECT_EQ(RepairOutcomeText(outcome),
            L"Failed, " + TaskTakenText() + L" (" + HResultText(kTaskTakenHr) +
                L"): register the check task again for " + (kUserRoot / kSettingsExe).native());
  EXPECT_EQ(ElevatedVerbText(ElevatedVerb::kMachineRepairClass),
            L"point the machine-wide class at a complete build again");
}

}  // namespace
}  // namespace pdft
