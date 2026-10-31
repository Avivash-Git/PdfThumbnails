// Which files of the shared per-user folder are 1.x leftovers (legacy.h ClassifyV1Files), on fake
// listings of the 1.x layout, the 2.x layout, and both at once; and which class DLLs lie in a 1.x
// build folder.

#include <windows.h>

#include "legacy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

#include "project_config.h"

namespace pdft {
namespace {

FolderEntry File(const std::wstring& name) { return {name, false, false}; }
FolderEntry Dir(const std::wstring& name) { return {name, true, false}; }
FolderEntry Link(const std::wstring& name) { return {name, true, true}; }

// The license files 1.x copied into licenses\ (19, as on the developer's PC).
std::vector<FolderEntry> V1Licenses() {
  std::vector<FolderEntry> entries;
  for (const wchar_t* name :
       {L"abseil.txt", L"agg23.txt", L"dragonbox-Apache2-LLVM.txt", L"dragonbox-Boost.txt",
        L"fast_float.txt", L"freetype.txt", L"harfbuzz.txt", L"highway.txt", L"icu.txt",
        L"lcms.txt", L"libjpeg_turbo.ijg", L"libjpeg_turbo.md", L"libopenjpeg.txt", L"libpng.txt",
        L"llvm-libc.txt", L"pdfium-binaries-LICENSE.txt", L"pdfium.txt", L"simdutf.txt",
        L"zlib.txt"}) {
    entries.push_back(File(name));
  }
  return entries;
}

const std::wstring kScript = std::wstring(kLegacyRootName) + L".ps1";
const std::wstring kLauncher = std::wstring(kLegacyRootName) + L".cmd";

// The per-user folder of a 1.1 install, as the 1.1 manager leaves it.
UserFolderListing V1Layout() {
  UserFolderListing listing;
  listing.root = {Dir(L"bin"),         Dir(L"licenses"),      File(L"LICENSE"),
                  File(kLauncher),     File(kScript),         File(L"README.md"),
                  File(L"repair.log"), File(L"settings.ini"), Dir(L"app")};
  listing.app = {Dir(L"1.1.0.0-67a1323e"), Dir(L"1.0.0.0-0badf00d")};
  listing.licenses = V1Licenses();
  return listing;
}

// The per-user folder of a 2.x per-user install (cmake/Install.cmake's runtime tree).
UserFolderListing V2Layout() {
  UserFolderListing listing;
  listing.root = {File(kSettingsExe),   File(kCliExe),
                  File(L"LICENSE.txt"), File(L"THIRD-PARTY-NOTICES.md"),
                  Dir(L"licenses"),     Dir(L"app")};
  listing.app = {Dir(L"2.0.0.0-1a2b3c4d")};
  listing.licenses = {Dir(L"pdfium"), File(L"pdfium-binaries-LICENSE.txt"),
                      File(L"wil-LICENSE.txt")};
  return listing;
}

std::vector<std::wstring> Names(std::initializer_list<std::wstring> names) { return names; }

TEST(V1Leftovers, The1xLayoutIsAllLeftovers) {
  EXPECT_EQ(ClassifyV1Files(V1Layout()),
            Names({L"app\\1.0.0.0-0badf00d", L"app\\1.1.0.0-67a1323e", L"bin", L"LICENSE",
                   L"licenses", kLauncher, kScript, L"README.md", L"repair.log", L"settings.ini"}));
}

TEST(V1Leftovers, The2xLayoutHasNone) {
  EXPECT_TRUE(ClassifyV1Files(V2Layout()).empty());
  // Even with 2.x's licenses folder emptied down to the one name 1.x shared with it.
  UserFolderListing partial = V2Layout();
  partial.licenses = {File(L"pdfium-binaries-LICENSE.txt")};
  EXPECT_TRUE(ClassifyV1Files(partial).empty());
}

TEST(V1Leftovers, BothLayoutsKeepEverythingOf2x) {
  // A 2.x per-user install over 1.1 before the clean-up ran: the shared licenses\ folder now
  // holds both sets, and 2.x's files are there.
  UserFolderListing both = V1Layout();
  const UserFolderListing v2 = V2Layout();
  for (const FolderEntry& entry : v2.root) {
    if (entry.name != L"licenses" && entry.name != L"app") both.root.push_back(entry);
  }
  both.app.push_back(Dir(L"2.0.0.0-1a2b3c4d"));
  both.licenses.insert(both.licenses.end(), v2.licenses.begin(), v2.licenses.end());
  EXPECT_EQ(ClassifyV1Files(both),
            Names({L"app\\1.0.0.0-0badf00d", L"app\\1.1.0.0-67a1323e", L"bin", L"LICENSE",
                   kLauncher, kScript, L"README.md", L"repair.log", L"settings.ini"}));

  // A licenses\ folder of 1.x files only is still kept while anything of 2.x is there.
  UserFolderListing withExe = V1Layout();
  withExe.root.push_back(File(kCliExe));
  const std::vector<std::wstring> found = ClassifyV1Files(withExe);
  EXPECT_EQ(std::count(found.begin(), found.end(), L"licenses"), 0);
  UserFolderListing withUninstaller = V1Layout();
  withUninstaller.root.push_back(File(L"unins000.exe"));
  const std::vector<std::wstring> found2 = ClassifyV1Files(withUninstaller);
  EXPECT_EQ(std::count(found2.begin(), found2.end(), L"licenses"), 0);
}

TEST(V1Leftovers, SharedNamesNeedThe1xManager) {
  // Without the manager script and launcher, README.md, LICENSE and licenses\ could be anyone's.
  UserFolderListing listing = V1Layout();
  std::erase_if(listing.root, [](const FolderEntry& entry) {
    return entry.name == kScript || entry.name == kLauncher;
  });
  EXPECT_EQ(ClassifyV1Files(listing), Names({L"app\\1.0.0.0-0badf00d", L"app\\1.1.0.0-67a1323e",
                                             L"bin", L"repair.log", L"settings.ini"}));
  // Either one of the two is enough.
  listing.root.push_back(File(kLauncher));
  EXPECT_EQ(ClassifyV1Files(listing).size(), 9u);
}

TEST(V1Leftovers, LicensesMustBeA1xSetWithoutSubfolders) {
  UserFolderListing extra = V1Layout();
  extra.licenses.push_back(File(L"wil-LICENSE.txt"));
  UserFolderListing subfolder = V1Layout();
  subfolder.licenses.push_back(Dir(L"pdfium"));
  for (const UserFolderListing& listing : {extra, subfolder}) {
    const std::vector<std::wstring> found = ClassifyV1Files(listing);
    EXPECT_EQ(std::count(found.begin(), found.end(), L"licenses"), 0);
  }
  // A partly cleaned 1.x folder still counts (an interrupted clean-up is finished later), and
  // so does an empty one.
  UserFolderListing partly = V1Layout();
  partly.licenses.resize(3);
  UserFolderListing empty = V1Layout();
  empty.licenses.clear();
  for (const UserFolderListing& listing : {partly, empty}) {
    const std::vector<std::wstring> found = ClassifyV1Files(listing);
    EXPECT_EQ(std::count(found.begin(), found.end(), L"licenses"), 1);
  }
}

TEST(V1Leftovers, LinksAndOddEntriesAreNeverCounted) {
  UserFolderListing listing;
  listing.root = {Link(L"bin"),
                  Link(L"licenses"),
                  Dir(kScript),
                  Dir(L"settings.ini"),
                  File(L"bin"),
                  File(L"app"),
                  File(L"repair.log.old"),
                  File(L"elevated.log"),
                  File(L"SETTINGS.INI"),
                  File(L"notes.txt"),
                  Link(L"app")};
  listing.app = {Dir(L"1.1.0.0-x")};  // ignored: app is not a real folder in the root
  EXPECT_EQ(ClassifyV1Files(listing), Names({L"elevated.log", L"repair.log.old", L"SETTINGS.INI"}));

  UserFolderListing apps;
  apps.root = {Dir(L"app")};
  apps.app = {Dir(L"1.1.0.0-67a1323e"), Link(L"1.0.0.0-11111111"), File(L"1.2.txt"),
              Dir(L"2.0.0.0-1a2b3c4d"), Dir(L"10.0.0.0-22222222"), Dir(L"junk")};
  EXPECT_EQ(ClassifyV1Files(apps), Names({L"app\\1.1.0.0-67a1323e"}));
}

TEST(V1Leftovers, ClassInA1xBuildFolder) {
  const std::wstring user = L"D:\\Home\\u\\AppData\\Local\\Programs\\PdfThumbnails";
  const std::wstring machine = L"C:\\Program Files\\PdfThumbnails";
  EXPECT_TRUE(IsInV1BuildFolder(machine + L"\\app\\1.1.0.0-67a1323e\\PdfThumbnails.dll", machine));
  EXPECT_TRUE(IsInV1BuildFolder(user + L"\\APP\\1.0.0.0-0badf00d\\PdfThumbnails.dll", user));
  EXPECT_TRUE(IsInV1BuildFolder(machine + L"\\app\\1.1.0.0-67a1323e\\sub\\x.dll", machine));
  EXPECT_FALSE(IsInV1BuildFolder(machine + L"\\app\\2.0.0.0-1a2b3c4d\\PdfThumbnails.dll", machine));
  EXPECT_FALSE(IsInV1BuildFolder(machine + L"\\app\\1.1.0.0-67a1323e", machine));  // the folder
  EXPECT_FALSE(IsInV1BuildFolder(machine + L"\\app\\PdfThumbnails.dll", machine));
  EXPECT_FALSE(IsInV1BuildFolder(machine + L"\\bin\\PdfThumbnails.dll", machine));
  EXPECT_FALSE(IsInV1BuildFolder(user + L"\\app\\1.1.0.0-67a1323e\\PdfThumbnails.dll", machine));
  // ".." cannot lead out of a 2.x folder into a 1.x one, or in from elsewhere.
  EXPECT_FALSE(IsInV1BuildFolder(
      machine + L"\\app\\1.1.0.0-67a1323e\\..\\2.0.0.0-1a2b3c4d\\PdfThumbnails.dll", machine));
  EXPECT_TRUE(IsInV1BuildFolder(
      machine + L"\\app\\2.0.0.0-1a2b3c4d\\..\\1.1.0.0-67a1323e\\PdfThumbnails.dll", machine));
  EXPECT_FALSE(IsInV1BuildFolder(L"PdfThumbnails.dll", machine));
  EXPECT_FALSE(IsInV1BuildFolder(L"", machine));
  EXPECT_FALSE(IsInV1BuildFolder(machine + L"\\app\\1.1\\x.dll", L""));
}

TEST(V1Leftovers, MarkClassesAndAnyLeftovers) {
  const std::wstring user = L"D:\\Home\\u\\AppData\\Local\\Programs\\PdfThumbnails";
  const std::wstring machine = L"C:\\Program Files\\PdfThumbnails";
  ClassReg v1;
  v1.dll = machine + L"\\app\\1.1.0.0-67a1323e\\PdfThumbnails.dll";
  ClassReg v2;
  v2.dll = machine + L"\\app\\2.0.0.0-1a2b3c4d\\PdfThumbnails.dll";

  LegacyState state;
  EXPECT_FALSE(HasV1Leftovers(state));
  MarkV1Classes(std::nullopt, v1, user, machine, &state);
  EXPECT_FALSE(state.userClassInV1);
  EXPECT_TRUE(state.machineClassInV1);
  // The machine-wide 1.x install is in use, not left over.
  EXPECT_FALSE(HasV1Leftovers(state));
  MarkV1Classes(v1, v2, user, machine, &state);  // a per-user class into the machine's 1.x folder
  EXPECT_TRUE(state.userClassInV1);
  EXPECT_FALSE(state.machineClassInV1);
  EXPECT_TRUE(HasV1Leftovers(state));

  for (const auto set : std::initializer_list<void (*)(LegacyState&)>{
           [](LegacyState& s) { s.runOnce = true; }, [](LegacyState& s) { s.legacyTask = true; },
           [](LegacyState& s) { s.ini = true; },
           [](LegacyState& s) { s.files.emplace_back(L"C:\\x\\bin"); }}) {
    LegacyState one;
    set(one);
    EXPECT_TRUE(HasV1Leftovers(one));
  }
  LegacyState zoomOnly;
  zoomOnly.iniZoom = 130;  // a zoom without the file cannot happen, and is not a leftover itself
  EXPECT_FALSE(HasV1Leftovers(zoomOnly));
}

}  // namespace
}  // namespace pdft
