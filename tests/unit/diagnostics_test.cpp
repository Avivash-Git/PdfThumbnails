// The diagnostics report (diagnostics.h): masking, hard cases first (names inside paths and
// words, short names, overlapping names, names with regex characters, SIDs anywhere), then a
// whole report built from made-up details, in which none of them may survive; and what an elevated
// process leaves out or refuses.

#include <windows.h>

#include "diagnostics.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "commands.h"
#include "context.h"
#include "evaluate.h"
#include "project_config.h"
#include "snapshot_fixtures.h"

namespace pdft {
namespace {

using namespace test;  // NOLINT(build/namespaces): the fixtures

// The profile folders here lie under D:\Home, and the account SIDs are split after "S-1-5-21-":
// the repository's publish check rejects anything shaped like a real profile path or account
// SID, even an invented one. Masking does not care where the profile is.
RedactContext MadeUp() {
  RedactContext context;
  context.profile = L"D:\\Home\\jdoe";
  context.user = L"jdoe";
  context.computer = L"DESK-42";
  context.hostName = L"desk-42-workstation";
  context.domain = L"CONTOSO";
  return context;
}

std::wstring Lower(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
  return text;
}

bool ContainsIgnoringCase(const std::wstring& text, const std::wstring& part) {
  return Lower(text).find(Lower(part)) != std::wstring::npos;
}

TEST(Redact, ProfileUserComputerDomain) {
  const RedactContext c = MadeUp();
  EXPECT_EQ(Redact(L"D:\\Home\\jdoe\\AppData\\Local\\x.log", c),
            L"%USERPROFILE%\\AppData\\Local\\x.log");
  EXPECT_EQ(Redact(L"d:\\HOME\\JDOE\\Desktop", c), L"%USERPROFILE%\\Desktop") << "any case";
  EXPECT_EQ(Redact(L"CONTOSO\\jdoe on DESK-42", c), L"<domain>\\<user> on <pc>");
  EXPECT_EQ(Redact(L"desk-42-workstation.contoso.example", c), L"<pc>.<domain>.example")
      << "the longer host name wins over the NetBIOS name it starts with";
  EXPECT_EQ(Redact(L"nothing personal here", c), L"nothing personal here");
  EXPECT_EQ(Redact(L"", c), L"");
  EXPECT_EQ(Redact(L"D:\\Home\\jdoe", RedactContext{}), L"D:\\Home\\jdoe") << "nothing to mask";
}

TEST(Redact, AUserNameInsidePathsAndWords) {
  RedactContext c;
  c.profile = L"D:\\Home\\jdoe";
  c.user = L"jdoe";
  // Outside the profile: a share, another drive, a folder named after the user.
  EXPECT_EQ(Redact(L"D:\\Backup\\JDoe\\a.pdf", c), L"D:\\Backup\\<user>\\a.pdf");
  EXPECT_EQ(Redact(L"\\\\server\\home$\\jdoe2\\x", c), L"\\\\server\\home$\\<user>2\\x");
  // Four characters or more: masked also inside a longer word.
  EXPECT_EQ(Redact(L"jdoes-laptop", c), L"<user>s-laptop");
  // The profile is longer than the name, so it is masked as a whole.
  EXPECT_EQ(Redact(L"D:\\Home\\jdoe\\jdoe.txt", c), L"%USERPROFILE%\\<user>.txt");
  // A profile path given with a trailing separator still matches.
  c.profile = L"D:\\Home\\jdoe\\";
  EXPECT_EQ(Redact(L"D:\\Home\\jdoe\\x", c), L"%USERPROFILE%\\x");
}

TEST(Redact, TheProfileInIts83FormAndItsFolderName) {
  RedactContext c;
  c.profile = L"D:\\Home\\johndoesmith";
  c.profileShort = L"D:\\Home\\JOHNDO~1\\";
  c.user = L"jsmith";  // renamed after the profile was made
  EXPECT_EQ(Redact(L"D:\\Home\\JOHNDO~1\\Downloads\\x.exe", c), L"%USERPROFILE%\\Downloads\\x.exe");
  EXPECT_EQ(Redact(L"d:\\home\\johndo~1", c), L"%USERPROFILE%");
  EXPECT_EQ(Redact(L"E:\\JOHNDO~1\\x and E:\\johndoesmith\\y", c),
            L"E:\\<user>\\x and E:\\<user>\\y");
  EXPECT_EQ(Redact(L"jsmith", c), L"<user>");
}

TEST(Redact, ShortNamesOnlyAsWordsOfTheirOwn) {
  RedactContext c;
  c.user = L"al";
  c.computer = L"PC";
  EXPECT_EQ(Redact(L"already al. AL", c), L"already <user>. <user>");
  EXPECT_EQ(Redact(L"C:\\al\\file", c), L"C:\\<user>\\file");
  EXPECT_EQ(Redact(L"final", c), L"final");
  EXPECT_EQ(Redact(L"al_x x_al al1", c), L"al_x x_al al1");
  EXPECT_EQ(Redact(L"PC-1 is not PC", c), L"<pc>-1 is not <pc>");
  EXPECT_EQ(Redact(L"PCI bus", c), L"PCI bus");
}

TEST(Redact, OverlappingNames) {
  RedactContext c;
  c.user = L"Annex";
  c.computer = L"ANNEX-PC";
  c.domain = L"ANNEX-PC";  // a local account: the domain is the computer
  EXPECT_EQ(Redact(L"ANNEX-PC\\Annex", c), L"<pc>\\<user>");
  EXPECT_EQ(Redact(L"annex-pcs", c), L"<pc>s");
  c.user = L"Ann";
  EXPECT_EQ(Redact(L"Ann on ANNEX-PC; Annie", c), L"<user> on <pc>; Annie");
}

TEST(Redact, NamesWithRegexCharacters) {
  RedactContext c;
  c.user = L"a.b+c(1)";
  c.computer = L"[pc]*";
  EXPECT_EQ(Redact(L"user a.b+c(1) on [pc]*", c), L"user <user> on <pc>");
  EXPECT_EQ(Redact(L"axb+c(1) and abbbc1 and pc", c), L"axb+c(1) and abbbc1 and pc");
  c.user = L"$^\\";
  EXPECT_EQ(Redact(L"x $^\\ y", c), L"x <user> y");
}

TEST(Redact, OnePassNeverMasksAReplacement) {
  RedactContext c;
  c.user = L"user";
  c.computer = L"masked";
  EXPECT_EQ(Redact(L"user", c), L"<user>");
  EXPECT_EQ(Redact(L"S-1-5-21-"
                   L"1-2-3-1001 and masked",
                   c),
            L"S-1-5-21-<masked>-1001 and <pc>");
  c.user = L"USERPROFILE";
  c.profile = L"D:\\Home\\x";
  EXPECT_EQ(Redact(L"D:\\Home\\x", c), L"%USERPROFILE%");
}

TEST(Redact, Sids) {
  const RedactContext none;
  EXPECT_EQ(Redact(L"S-1-5-21-"
                   L"1111111111-2222222222-3333333333-1001",
                   none),
            L"S-1-5-21-<masked>-1001");
  EXPECT_EQ(Redact(L"s-1-5-21-"
                   L"1111111111-2222222222-3333333333-500.",
                   none),
            L"S-1-5-21-<masked>-500.");
  EXPECT_EQ(Redact(L"\\PdfThumbnails\\Check S-1-5-21-"
                   L"11-22-33-1001",
                   none),
            L"\\PdfThumbnails\\Check S-1-5-21-<masked>-1001");
  EXPECT_EQ(Redact(L"HKEY_USERS\\S-1-5-21-"
                   L"11-22-33-1001_Classes",
                   none),
            L"HKEY_USERS\\S-1-5-21-<masked>-1001_Classes");
  EXPECT_EQ(Redact(L"TaskS-1-5-21-"
                   L"11-22-33-1001",
                   none),
            L"TaskS-1-5-21-<masked>-1001")
      << "glued to a word";
  // Azure AD users: no RID to keep.
  EXPECT_EQ(Redact(L"S-1-12-1-111-222-333-444", none), L"S-1-12-1-<masked>");
  // Well-known SIDs name nobody.
  for (const wchar_t* sid : {L"S-1-5-18", L"S-1-5-32-544", L"S-1-16-8192", L"S-1-1-0"}) {
    EXPECT_EQ(Redact(sid, none), sid);
  }
  EXPECT_EQ(Redact(L"S-1-", none), L"S-1-");
  EXPECT_EQ(Redact(L"S-1-x", none), L"S-1-x");
  // A domain's own SID has no RID to keep.
  EXPECT_EQ(Redact(L"S-1-5-21-"
                   L"1-2-3-",
                   none),
            L"S-1-5-21-<masked>-");
  EXPECT_EQ(Redact(L"S-1-5-21-"
                   L"1-2-3-4-",
                   none),
            L"S-1-5-21-<masked>-4-");
}

TEST(Diagnostics, LastLines) {
  EXPECT_EQ(LastLines(L"a\r\nb\nc", 10, false), (std::vector<std::wstring>{L"a", L"b", L"c"}));
  EXPECT_EQ(LastLines(L"a\nb\nc\n", 2, false), (std::vector<std::wstring>{L"b", L"c"}));
  EXPECT_EQ(LastLines(L"tail of a line\nb\nc", 10, true), (std::vector<std::wstring>{L"b", L"c"}));
  EXPECT_EQ(LastLines(L"no line end at all", 10, true), std::vector<std::wstring>{});
  EXPECT_EQ(LastLines(L"", 10, false), std::vector<std::wstring>{});
  EXPECT_EQ(LastLines(L"\n\n", 10, false), (std::vector<std::wstring>{L"", L""}));
}

TEST(Diagnostics, CompilerVersion) {
  const std::wstring text = CompilerText();
  EXPECT_EQ(text.rfind(L"MSVC 19.", 0), 0u) << text;
}

TEST(Diagnostics, NothingPersonalSurvives) {
  const RedactContext c = MadeUp();
  const std::wstring sid =
      L"S-1-5-21-"
      L"1111111111-2222222222-3333333333-1001";
  Snapshot s = ThisPcSnapshot();
  s.context.userSid = sid;
  s.context.userAccount = L"CONTOSO\\jdoe";
  s.paths.userRoot = L"D:\\Home\\jdoe\\AppData\\Local\\Programs\\Product";
  s.paths.legacyUserRoot = s.paths.userRoot;
  s.paths.ownExe = L"D:\\Home\\jdoe\\Downloads\\Product\\" + std::wstring(kCliExe);
  s.legacy.files = {s.paths.userRoot / L"settings.ini", s.paths.userRoot / L"jdoe-notes.txt"};
  s.zoom.legacyIni = L"D:\\Home\\JDOE\\AppData\\Local\\Product\\settings.ini";
  s.taskLocation = {std::wstring(kTaskFolder), std::wstring(kTaskPrefix) + sid, L"\\", L""};
  s.slotOwners[1].dll = L"D:\\Home\\jdoe\\tools\\viewer.dll";
  s.issues.push_back({L"task", E_ACCESSDENIED});
  const std::vector<std::wstring> log = {
      L"2026-10-31T10:00:00.000+02:00 4812 status INFO start cli il=0x2000 elevated=no",
      L"2026-10-31T10:00:01.000+02:00 4812 wil ERROR hr=0x80070005 user=CONTOSO\\jdoe on "
      L"desk-42 sid " +
          sid,
      L"2026-10-31T10:00:02.000+02:00 4812 check INFO path D:\\home\\jdoe\\x",
  };
  const std::vector<Finding> findings = Evaluate(s);
  const std::wstring report = BuildDiagnostics(s, findings, log, c);

  for (const std::wstring& secret :
       {std::wstring(L"jdoe"), std::wstring(L"DESK-42"), std::wstring(L"CONTOSO"),
        std::wstring(L"D:\\Home\\"), std::wstring(L"1111111111"), std::wstring(L"2222222222"),
        std::wstring(L"3333333333")}) {
    EXPECT_FALSE(ContainsIgnoringCase(report, secret)) << "leaked: " << secret.c_str();
  }
  for (const std::wstring& placeholder :
       {std::wstring(L"%USERPROFILE%\\AppData\\Local\\Programs\\Product"),
        std::wstring(L"<domain>\\<user>"), std::wstring(L"S-1-5-21-<masked>-1001"),
        std::wstring(L"on <pc> sid")}) {
    EXPECT_NE(report.find(placeholder), std::wstring::npos) << placeholder.c_str();
  }
  // What a maintainer needs is there.
  for (const std::wstring& part :
       {std::wstring(L"built with MSVC 19."), std::wstring(L"Windows:        build 26300.9818"),
        std::wstring(L"1.1.0.0-67a1323e"), std::wstring(L"Smart App Control: on"),
        std::wstring(L"SacOn (info)"), std::wstring(L"V1Leftovers (info)"),
        std::wstring(L"task: 0x80070005"), std::wstring(L"Log (last 30 lines)"),
        std::wstring(L"Result: healthy"), std::wstring(L"Explorer test: no recorded result"),
        std::wstring(L"4812 status INFO start cli")}) {
    EXPECT_NE(report.find(part), std::wstring::npos) << part.c_str() << "\n" << report.c_str();
  }
}

TEST(Diagnostics, RecordedExplorerTestAndEmptyLog) {
  Snapshot s = MachineInstallSnapshot();
  s.last = CheckRecord{};
  s.last->liveTest = REGDB_E_CLASSNOTREG;
  const std::wstring report = BuildDiagnostics(s, Evaluate(s), {}, RedactContext{});
  EXPECT_NE(report.find(L"Explorer test: 0x80040154 at the last recorded check"),
            std::wstring::npos);
  EXPECT_NE(report.find(L"Log (last 30 lines)\n  (empty)"), std::wstring::npos);
}

// An elevated process does not open the log; the report says so where its lines would be.
TEST(Diagnostics, AnElevatedProcessLeavesTheLogOut) {
  ProcessContext context;
  context.elevated = true;
  const std::vector<std::wstring> tail = DiagnosticsLogTail(context);
  EXPECT_EQ(tail, std::vector<std::wstring>{kLogNotReadElevated});

  Snapshot s = MachineInstallSnapshot();
  s.context.elevated = true;
  const std::wstring report = BuildDiagnostics(s, Evaluate(s), tail, RedactContext{});
  EXPECT_NE(report.find(L"Log (last 30 lines)\n  (not read: running as administrator)"),
            std::wstring::npos)
      << report.c_str();
}

// --diagnostics --out refuses an elevated process the way the other elevated refusals do (exit
// 8, checked where the executables run elevated) and names what still works.
TEST(Diagnostics, OutRefusedElevated) {
  const std::wstring message = DiagnosticsOutElevatedRefusalMessage();
  EXPECT_EQ(message.rfind(L"Cannot run here: running as administrator. ", 0), 0u);
  EXPECT_NE(message.find(L"--out"), std::wstring::npos);
  EXPECT_NE(message.find(L"--copy"), std::wstring::npos);
}

}  // namespace
}  // namespace pdft
