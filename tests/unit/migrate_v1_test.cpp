// The 1.x clean-up's pure parts (migrate_v1.h, setup.h): the committed table of 1.x files, the
// plan for 1.x's per-user folder on fake trees with the 1.1 layout, the 2.x layout and both at
// once (shared names, a 2.x file with a 1.x name, licenses\ with more than 1.x's files, held and
// used build folders, links, idempotence), the plans for build folders for all users, and the
// installer step's task options and old-build list. Nothing here reads or writes anything.

#include <windows.h>

#include "migrate_v1.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "commands.h"
#include "project_config.h"
#include "setup.h"
#include "snapshot_fixtures.h"
#include "strings.h"

namespace pdft {
namespace {

using Kind = V1FileStep::Kind;

const std::wstring kScript = std::wstring(kLegacyRootName) + L".ps1";
const std::wstring kLauncher = std::wstring(kLegacyRootName) + L".cmd";

// The license files 1.x copied into licenses\.
const std::vector<std::wstring>& V1LicenseNames() {
  static const std::vector<std::wstring> names = {L"abseil.txt",
                                                  L"agg23.txt",
                                                  L"dragonbox-Apache2-LLVM.txt",
                                                  L"dragonbox-Boost.txt",
                                                  L"fast_float.txt",
                                                  L"freetype.txt",
                                                  L"harfbuzz.txt",
                                                  L"highway.txt",
                                                  L"icu.txt",
                                                  L"lcms.txt",
                                                  L"libjpeg_turbo.ijg",
                                                  L"libjpeg_turbo.md",
                                                  L"libopenjpeg.txt",
                                                  L"libpng.txt",
                                                  L"llvm-libc.txt",
                                                  L"pdfium-binaries-LICENSE.txt",
                                                  L"pdfium.txt",
                                                  L"simdutf.txt",
                                                  L"zlib.txt"};
  return names;
}

Sha256Digest Shipped(const std::wstring& path) {
  const std::vector<Sha256Digest> digests = V1ShippedDigests(path);
  EXPECT_FALSE(digests.empty()) << WideToUtf8Lossy(path);
  return digests.empty() ? Sha256Digest{} : digests.front();
}

Sha256Digest Other() {
  Sha256Digest digest = {};
  digest.fill(0x5A);
  return digest;
}

class TreeBuilder {
 public:
  // A file whose contents are 1.x's, as the table has them.
  TreeBuilder& V1File(const std::wstring& path) {
    tree_.entries.push_back({path, false, false, Shipped(path)});
    return *this;
  }
  // A file with other contents (hashed only when the table names it, like ReadV1Tree).
  TreeBuilder& OtherFile(const std::wstring& path) {
    V1TreeEntry entry{path, false, false};
    if (IsV1ShippedFile(path)) entry.sha = Other();
    tree_.entries.push_back(entry);
    return *this;
  }
  TreeBuilder& Dir(const std::wstring& path) {
    tree_.entries.push_back({path, true, false});
    return *this;
  }
  TreeBuilder& Link(const std::wstring& path) {
    tree_.entries.push_back({path, true, true});
    return *this;
  }
  bool Has(const std::wstring& path) const {
    return std::any_of(tree_.entries.begin(), tree_.entries.end(),
                       [&](const V1TreeEntry& entry) { return entry.path == path; });
  }
  TreeBuilder& DirOnce(const std::wstring& path) { return Has(path) ? *this : Dir(path); }
  TreeBuilder& Remove(const std::wstring& path) {
    std::erase_if(tree_.entries, [&](const V1TreeEntry& entry) { return entry.path == path; });
    return *this;
  }
  const V1Tree& tree() const { return tree_; }

 private:
  V1Tree tree_;
};

// 1.x's per-user folder as the 1.1 manager leaves it.
TreeBuilder V1Layout() {
  TreeBuilder tree;
  tree.Dir(L"bin").Dir(L"licenses").Dir(L"app");
  tree.V1File(L"LICENSE").V1File(kLauncher).V1File(kScript).V1File(L"README.md");
  tree.OtherFile(L"repair.log").OtherFile(L"settings.ini");
  for (const std::wstring& name : V1LicenseNames()) tree.V1File(L"licenses\\" + name);
  tree.V1File(L"bin\\" + std::wstring(kLegacyRootName) + L".dll");
  tree.V1File(L"bin\\" + std::wstring(kLegacyRootName) + L"-pdfium.dll");
  tree.Dir(L"app\\1.1.0.0-67a1323e");
  return tree;
}

// The same folder holding a 2.x per-user install (cmake/Install.cmake's runtime tree). Its
// PDFium license file has the same name as 1.x's, and here even the same contents.
void Add2xLayout(TreeBuilder* tree) {
  tree->OtherFile(kSettingsExe).OtherFile(kCliExe).OtherFile(L"LICENSE.txt");
  tree->OtherFile(L"THIRD-PARTY-NOTICES.md");
  tree->DirOnce(L"licenses").Dir(L"licenses\\pdfium").OtherFile(L"licenses\\wil-LICENSE.txt");
  tree->DirOnce(L"app").Dir(L"app\\2.0.0.0-1a2b3c4d");
}

std::vector<std::wstring> Paths(const V1FilePlan& plan, Kind kind) {
  std::vector<std::wstring> paths;
  for (const V1FileStep& step : plan.steps) {
    if (step.kind == kind) paths.push_back(step.path);
  }
  return paths;
}

bool Plans(const V1FilePlan& plan, const std::wstring& path) {
  return std::any_of(plan.steps.begin(), plan.steps.end(),
                     [&](const V1FileStep& step) { return IEquals(step.path, path); });
}

std::optional<V1KeepReason> Kept(const V1FilePlan& plan, const std::wstring& path) {
  for (const V1Kept& kept : plan.kept) {
    if (IEquals(kept.path, path)) return kept.reason;
  }
  return std::nullopt;
}

V1PlanInput ZoomSaved() {
  V1PlanInput input;
  input.zoomSaved = true;
  return input;
}

// ---- The table --------------------------------------------------------------------------------

TEST(V1FileTable, NamesWhat1xShipped) {
  for (const wchar_t* path : {L"LICENSE", L"README.md", L"PdfThumbnails.ps1", L"PdfThumbnails.cmd",
                              L"bin\\PdfThumbnails.dll", L"bin\\PdfThumbnails-pdfium.dll"}) {
    EXPECT_TRUE(IsV1ShippedFile(path)) << WideToUtf8Lossy(path);
  }
  for (const std::wstring& name : V1LicenseNames()) {
    EXPECT_TRUE(IsV1ShippedFile(L"licenses\\" + name)) << WideToUtf8Lossy(name);
    // Text files: the contents as shipped, as LF and as CRLF (some coincide).
    const size_t count = V1ShippedDigests(L"licenses\\" + name).size();
    EXPECT_GE(count, 2u) << WideToUtf8Lossy(name);
    EXPECT_LE(count, 3u) << WideToUtf8Lossy(name);
  }
  EXPECT_TRUE(IsV1ShippedFile(L"LICENSES\\ZLIB.TXT"));                // ignoring case
  EXPECT_EQ(V1ShippedDigests(L"bin\\PdfThumbnails.dll").size(), 1u);  // binary: one form
  // Names 1.x never shipped, and what it only wrote.
  for (const wchar_t* path :
       {L"LICENSE.txt", L"THIRD-PARTY-NOTICES.md", L"licenses\\wil-LICENSE.txt", L"settings.ini",
        L"repair.log", L"zlib.txt", L"bin", L""}) {
    EXPECT_FALSE(IsV1ShippedFile(path)) << WideToUtf8Lossy(path);
  }
}

TEST(V1FileTable, DigestsParse) {
  EXPECT_TRUE(DigestFromHex(std::string(64, 'a')));
  EXPECT_TRUE(DigestFromHex(std::string(64, 'F')));
  EXPECT_FALSE(DigestFromHex(std::string(63, 'a')));
  EXPECT_FALSE(DigestFromHex(std::string(65, 'a')));
  EXPECT_FALSE(DigestFromHex(std::string(63, 'a') + "g"));
  EXPECT_FALSE(DigestFromHex(""));
  const std::optional<Sha256Digest> digest =
      DigestFromHex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
  ASSERT_TRUE(digest);
  for (size_t i = 0; i < kSha256Bytes; ++i) EXPECT_EQ((*digest)[i], i);
}

// ---- The per-user folder's plan ---------------------------------------------------------------

TEST(V1FilePlan, The11LayoutGoesInOrderWithTheManagerLast) {
  const V1FilePlan plan = PlanV1UserFiles(V1Layout().tree(), ZoomSaved());
  EXPECT_TRUE(plan.kept.empty());
  std::vector<std::wstring> order;
  for (const V1FileStep& step : plan.steps) order.push_back(step.path);
  ASSERT_GE(order.size(), 6u);
  EXPECT_EQ(order[0], L"repair.log");
  EXPECT_EQ(order[1], L"settings.ini");
  EXPECT_EQ(order[2], L"app\\1.1.0.0-67a1323e");
  // The manager goes last, the launcher just before it, both marked.
  EXPECT_EQ(order[order.size() - 2], kLauncher);
  EXPECT_EQ(order[order.size() - 1], kScript);
  EXPECT_TRUE(plan.steps[order.size() - 1].marker);
  EXPECT_TRUE(plan.steps[order.size() - 2].marker);
  // Every license file and both DLLs by contents, then their folders, once empty.
  EXPECT_EQ(Paths(plan, Kind::kRemoveEmptyFolder),
            (std::vector<std::wstring>{L"licenses", L"bin"}));
  EXPECT_EQ(Paths(plan, Kind::kRemoveBuildFolder),
            (std::vector<std::wstring>{L"app\\1.1.0.0-67a1323e"}));
  size_t byContent = 0;
  for (const V1FileStep& step : plan.steps) {
    if (step.byContent) ++byContent;
    if (step.kind == Kind::kDeleteFile && !step.byContent) {
      EXPECT_TRUE(IsV1GeneratedFileName(step.path)) << WideToUtf8Lossy(step.path);
    }
    if (!step.byContent) EXPECT_FALSE(step.marker);
  }
  EXPECT_EQ(byContent, V1LicenseNames().size() + 2 + 4);  // licenses, bin, 4 top-level files
  const auto at = [&](const std::wstring& path) {
    return std::find(order.begin(), order.end(), path) - order.begin();
  };
  EXPECT_LT(at(L"licenses\\zlib.txt"), at(L"licenses"));
  EXPECT_LT(at(L"bin\\PdfThumbnails.dll"), at(L"bin"));
  EXPECT_LT(at(L"bin"), at(L"README.md"));
  // Never the folder itself, nor the shared app\ folder.
  EXPECT_FALSE(Plans(plan, L"app"));
  EXPECT_FALSE(Plans(plan, L""));
}

TEST(V1FilePlan, The2xLayoutLosesNothing) {
  TreeBuilder tree;
  Add2xLayout(&tree);
  tree.V1File(L"licenses\\pdfium-binaries-LICENSE.txt");  // the same contents as 1.x's
  const V1FilePlan plan = PlanV1UserFiles(tree.tree(), ZoomSaved());
  EXPECT_TRUE(plan.steps.empty());
  // A file with 1.x's contents is reported, never deleted: without the manager its name proves
  // nothing.
  EXPECT_EQ(Kept(plan, L"licenses\\pdfium-binaries-LICENSE.txt"), V1KeepReason::kNoManager);
  EXPECT_FALSE(Kept(plan, L"licenses\\wil-LICENSE.txt"));
  EXPECT_FALSE(Kept(plan, L"licenses"));
}

TEST(V1FilePlan, BothLayoutsKeepEveryShippedFileWhile2xIsThere) {
  TreeBuilder tree = V1Layout();
  Add2xLayout(&tree);
  const V1FilePlan plan = PlanV1UserFiles(tree.tree(), ZoomSaved());
  // Only what 1.x alone writes, and its build folder.
  EXPECT_EQ(Paths(plan, Kind::kDeleteFile),
            (std::vector<std::wstring>{L"repair.log", L"settings.ini"}));
  EXPECT_EQ(Paths(plan, Kind::kRemoveBuildFolder),
            (std::vector<std::wstring>{L"app\\1.1.0.0-67a1323e"}));
  EXPECT_TRUE(Paths(plan, Kind::kRemoveEmptyFolder).empty());
  EXPECT_EQ(Kept(plan, L"licenses\\zlib.txt"), V1KeepReason::k2xInstall);
  EXPECT_EQ(Kept(plan, kScript), V1KeepReason::k2xInstall);
  EXPECT_EQ(Kept(plan, L"README.md"), V1KeepReason::k2xInstall);
  for (const wchar_t* path : {L"licenses\\pdfium", L"licenses\\wil-LICENSE.txt", L"LICENSE.txt",
                              L"app\\2.0.0.0-1a2b3c4d", L"licenses"}) {
    EXPECT_FALSE(Plans(plan, path)) << WideToUtf8Lossy(path);
  }
}

// licenses\ with a file 1.x never shipped: 1.x's files go, the folder and that file stay. A
// README.md with other contents (edited, or a later product's) stays too, and the manager still
// goes: a file that differs is not a run that was interrupted.
TEST(V1FilePlan, LicensesWithMoreThan1xsFilesAndAChangedReadme) {
  TreeBuilder tree = V1Layout();
  tree.OtherFile(L"licenses\\wil-LICENSE.txt");
  tree.Remove(L"README.md").OtherFile(L"README.md");
  const V1FilePlan plan = PlanV1UserFiles(tree.tree(), ZoomSaved());
  EXPECT_TRUE(Plans(plan, L"licenses\\zlib.txt"));
  EXPECT_FALSE(Plans(plan, L"licenses\\wil-LICENSE.txt"));
  EXPECT_FALSE(Plans(plan, L"licenses"));
  EXPECT_EQ(Kept(plan, L"licenses"), V1KeepReason::kNotEmpty);
  EXPECT_FALSE(Kept(plan, L"licenses\\wil-LICENSE.txt"));  // not 1.x's: not reported
  EXPECT_FALSE(Plans(plan, L"README.md"));
  EXPECT_EQ(Kept(plan, L"README.md"), V1KeepReason::kContentDiffers);
  EXPECT_TRUE(Plans(plan, kScript));
}

// A 2.x file under one of 1.x's names, in a folder the 1.x manager still claims: its contents
// decide.
TEST(V1FilePlan, A2xFileWithA1xNameStays) {
  TreeBuilder tree = V1Layout();
  tree.Remove(L"licenses\\pdfium-binaries-LICENSE.txt")
      .OtherFile(L"licenses\\pdfium-binaries-LICENSE.txt");
  tree.Remove(L"LICENSE").OtherFile(L"LICENSE");
  const V1FilePlan plan = PlanV1UserFiles(tree.tree(), ZoomSaved());
  EXPECT_FALSE(Plans(plan, L"licenses\\pdfium-binaries-LICENSE.txt"));
  EXPECT_EQ(Kept(plan, L"licenses\\pdfium-binaries-LICENSE.txt"), V1KeepReason::kContentDiffers);
  EXPECT_FALSE(Plans(plan, L"LICENSE"));
  EXPECT_FALSE(Plans(plan, L"licenses"));
  // A file whose contents could not be read is never deleted either.
  TreeBuilder unread = V1Layout();
  unread.Remove(L"bin\\PdfThumbnails.dll");
  V1Tree copy = unread.tree();
  copy.entries.push_back({L"bin\\PdfThumbnails.dll", false, false, std::nullopt});
  const V1FilePlan second = PlanV1UserFiles(copy, ZoomSaved());
  EXPECT_FALSE(Plans(second, L"bin\\PdfThumbnails.dll"));
  EXPECT_EQ(Kept(second, L"bin\\PdfThumbnails.dll"), V1KeepReason::kNotHashed);
  EXPECT_EQ(Kept(second, L"bin"), V1KeepReason::kNotEmpty);
}

TEST(V1FilePlan, HeldOrUsedBuildFoldersStay) {
  TreeBuilder tree = V1Layout();
  tree.Dir(L"app\\1.0.0.0-0badf00d").Dir(L"app\\1.1.0.0-cafe0001");
  V1PlanInput input = ZoomSaved();
  input.heldBuilds = {L"1.1.0.0-67A1323E"};  // ignoring case
  input.classBuilds = {L"1.0.0.0-0badf00d"};
  const V1FilePlan plan = PlanV1UserFiles(tree.tree(), input);
  EXPECT_EQ(Paths(plan, Kind::kRemoveBuildFolder),
            (std::vector<std::wstring>{L"app\\1.1.0.0-cafe0001"}));
  EXPECT_EQ(Kept(plan, L"app\\1.1.0.0-67a1323e"), V1KeepReason::kHeld);
  EXPECT_EQ(Kept(plan, L"app\\1.0.0.0-0badf00d"), V1KeepReason::kClassUsesIt);
}

TEST(V1FilePlan, SettingsWaitForTheZoom) {
  const V1FilePlan plan = PlanV1UserFiles(V1Layout().tree(), V1PlanInput{});
  EXPECT_FALSE(Plans(plan, L"settings.ini"));
  EXPECT_EQ(Kept(plan, L"settings.ini"), V1KeepReason::kZoomNotSaved);
  EXPECT_TRUE(Plans(plan, L"repair.log"));
}

TEST(V1FilePlan, LinksAreNeverTouched) {
  TreeBuilder tree;
  tree.V1File(kScript).Link(L"licenses").Link(L"bin").Dir(L"app").Link(L"app\\1.1.0.0-67a1323e");
  tree.Link(L"settings.ini");
  V1Tree links = tree.tree();
  links.entries.push_back({L"LICENSE", false, true, std::nullopt});  // a file symbolic link
  const V1FilePlan plan = PlanV1UserFiles(links, ZoomSaved());
  for (const wchar_t* path :
       {L"licenses", L"bin", L"app\\1.1.0.0-67a1323e", L"settings.ini", L"LICENSE"}) {
    EXPECT_FALSE(Plans(plan, path)) << WideToUtf8Lossy(path);
    EXPECT_EQ(Kept(plan, path), V1KeepReason::kLink) << WideToUtf8Lossy(path);
  }
  EXPECT_TRUE(Plans(plan, kScript));
}

TEST(V1FilePlan, NothingLeftMeansNothingToDo) {
  EXPECT_TRUE(PlanV1UserFiles(V1Tree{}, ZoomSaved()).steps.empty());
  // After a run that removed everything but the folder in use, a second plan has only that.
  TreeBuilder after;
  after.Dir(L"app").Dir(L"app\\1.1.0.0-67a1323e");
  const V1FilePlan plan = PlanV1UserFiles(after.tree(), ZoomSaved());
  EXPECT_EQ(Paths(plan, Kind::kRemoveBuildFolder),
            (std::vector<std::wstring>{L"app\\1.1.0.0-67a1323e"}));
  EXPECT_EQ(plan.steps.size(), 1u);
  // The same plan twice is the same plan.
  const V1FilePlan once = PlanV1UserFiles(V1Layout().tree(), ZoomSaved());
  const V1FilePlan twice = PlanV1UserFiles(V1Layout().tree(), ZoomSaved());
  ASSERT_EQ(once.steps.size(), twice.steps.size());
  for (size_t i = 0; i < once.steps.size(); ++i) EXPECT_EQ(once.steps[i].path, twice.steps[i].path);
}

// ---- Build folders for all users ----------------------------------------------------------------

std::vector<FolderEntry> MachineApp() {
  return {{L"1.1.0.0-67a1323e", true, false},
          {L"1.0.0.0-0badf00d", true, false},
          {L"2.0.0.0-1a2b3c4d", true, false},
          {L"2.0.0.0-00000001", true, false},
          {L"1.2-link", true, true},
          {L"2.0.0.0-0000000f", true, true},
          {L"notes", true, false},
          {L"1.txt", false, false}};
}

TEST(MachineBuilds, V1FoldersGoExceptTheOneTheClassUses) {
  const std::filesystem::path root = L"C:\\Program Files\\Product";
  const std::wstring dll = (root / L"app" / L"1.1.0.0-67a1323e" / kHandlerDll).native();
  MachineBuildPlan plan = PlanMachineV1Removal(MachineApp(), root, dll);
  EXPECT_EQ(plan.remove, (std::vector<std::wstring>{L"1.0.0.0-0badf00d"}));
  EXPECT_EQ(plan.keep, L"1.1.0.0-67a1323e");
  EXPECT_EQ(plan.links, (std::vector<std::wstring>{L"1.2-link"}));
  // No class: every 1.x folder; never a 2.x one.
  plan = PlanMachineV1Removal(MachineApp(), root, std::nullopt);
  EXPECT_EQ(plan.remove, (std::vector<std::wstring>{L"1.1.0.0-67a1323e", L"1.0.0.0-0badf00d"}));
  // A class elsewhere keeps nothing here.
  plan = PlanMachineV1Removal(MachineApp(), root, L"D:\\elsewhere\\app\\1.1.0.0-67a1323e\\x.dll");
  EXPECT_FALSE(plan.keep);
  EXPECT_EQ(plan.remove.size(), 2u);
}

TEST(MachineBuilds, Old2xBuildsGoOnlyBesideACompleteClassBuild) {
  const std::filesystem::path root = L"C:\\Program Files\\Product";
  const std::wstring dll = (root / L"app" / L"2.0.0.0-1a2b3c4d" / kHandlerDll).native();
  MachineBuildPlan plan = PlanOldMachineBuilds(MachineApp(), root, dll, true);
  EXPECT_EQ(plan.remove, (std::vector<std::wstring>{L"2.0.0.0-00000001"}));
  EXPECT_EQ(plan.links, (std::vector<std::wstring>{L"2.0.0.0-0000000f"}));
  EXPECT_TRUE(PlanOldMachineBuilds(MachineApp(), root, dll, false).remove.empty());
  EXPECT_TRUE(PlanOldMachineBuilds(MachineApp(), root, std::nullopt, true).remove.empty());
  const std::wstring v1 = (root / L"app" / L"1.1.0.0-67a1323e" / kHandlerDll).native();
  EXPECT_TRUE(PlanOldMachineBuilds(MachineApp(), root, v1, true).remove.empty());
}

TEST(MachineBuilds, PerUserOver1xMachineInstall) {
  LegacyState legacy;
  EXPECT_FALSE(PerUserInstallBlockedByV1Machine(legacy));
  legacy.machineClassInV1 = true;
  EXPECT_TRUE(PerUserInstallBlockedByV1Machine(legacy));
}

// ---- The installer's per-user step ----------------------------------------------------------

TEST(SetupUserPlan, TaskOptions) {
  TaskState none;
  EXPECT_EQ(SetupTaskOptions(std::nullopt, none).afterPdfCraft, TriggerSetting::kEnabled);
  EXPECT_EQ(SetupTaskOptions(std::nullopt, none).atSignIn, TriggerSetting::kEnabled);
  // What the user's task has stays, a disabled trigger and a disabled task included.
  TaskState current;
  current.exists = true;
  current.options = {TriggerSetting::kDisabled, TriggerSetting::kEnabled, false};
  TaskOptions kept = SetupTaskOptions(std::nullopt, current);
  EXPECT_EQ(kept.afterPdfCraft, TriggerSetting::kDisabled);
  EXPECT_EQ(kept.atSignIn, TriggerSetting::kEnabled);
  EXPECT_FALSE(kept.enabled);
  // A task with neither trigger gets both.
  current.options = {TriggerSetting::kAbsent, TriggerSetting::kAbsent, true};
  EXPECT_EQ(SetupTaskOptions(std::nullopt, current).atSignIn, TriggerSetting::kEnabled);
  // --checks decides when given.
  const TaskOptions asked = SetupTaskOptions(ChecksSelection{false, true}, current);
  EXPECT_EQ(asked.afterPdfCraft, TriggerSetting::kAbsent);
  EXPECT_EQ(asked.atSignIn, TriggerSetting::kEnabled);
  EXPECT_TRUE(HasNoTriggers(SetupTaskOptions(ChecksSelection{false, false}, current)));
}

TEST(SetupUserPlan, OldBuildDllsLeaveOutTheActiveBuild) {
  Snapshot s;
  BuildInfo active;
  active.folder.dir = test::kMachineRoot / L"app" / L"2.0.0.0-1a2b3c4d";
  BuildInfo old;
  old.folder.dir = test::kMachineRoot / L"app" / L"1.1.0.0-67a1323e";
  BuildInfo user;
  user.folder.dir = test::kUserRoot / L"app" / L"2.0.0.0-00000001";
  s.machineBuilds = {active, old};
  s.userBuilds = {user};
  const std::vector<std::filesystem::path> dlls =
      OldBuildDlls(s, (active.folder.dir / kHandlerDll).native());
  EXPECT_EQ(dlls, (std::vector<std::filesystem::path>{
                      user.folder.dir / kHandlerDll, user.folder.dir / kPdfiumDll,
                      old.folder.dir / kHandlerDll, old.folder.dir / kPdfiumDll}));
  EXPECT_EQ(OldBuildDlls(s, L"").size(), 6u);  // no class: every build is old
}

TEST(SetupUserPlan, ElevatedRefusalNamesTheCommand) {
  const std::wstring message = SetupElevatedRefusalMessage(L"--setup-user");
  EXPECT_EQ(message.rfind(L"Cannot run here: ", 0), 0u);
  EXPECT_NE(message.find(L"--setup-user"), std::wstring::npos);
}

}  // namespace
}  // namespace pdft
