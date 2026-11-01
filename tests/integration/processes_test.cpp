// Processes (processes.h) against the real system, on processes this test starts itself: Restart
// Manager finding a child that holds a file under %TEMP%\pdft-tests, ending that child only
// when every check passes (with this test program's own image on the allow list instead of the
// thumbnail process's), and a child finding its parent. Never ends a process it did not start:
// each holder is matched to the child's process id before anything is ended.

#include <windows.h>

#include "processes.h"

#include <gtest/gtest.h>
#include <wil/resource.h>

#include <filesystem>
#include <string>
#include <vector>

#include "child_process.h"
#include "medium_il.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

using std::filesystem::path;

bool WriteFileText(const path& file, const std::string& text) {
  wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
  DWORD written = 0;
  return handle &&
         WriteFile(handle.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
         written == text.size();
}

const Holder* FindPid(const std::vector<Holder>& holders, DWORD pid) {
  for (const Holder& holder : holders) {
    if (holder.pid == pid) return &holder;
  }
  return nullptr;
}

TEST(Holders, NobodyHoldsANewFile) {
  test::TempDir dir(L"holders");
  ASSERT_TRUE(dir.ok());
  const path file = path(dir.path()) / L"free.txt";
  ASSERT_TRUE(WriteFileText(file, "free"));
  std::vector<Holder> holders;
  const path files[] = {file};
  ASSERT_EQ(FindHolders(files, &holders), S_OK);
  // Others may look at a new file for a moment (a virus scanner); this process does not hold it.
  EXPECT_EQ(FindPid(holders, GetCurrentProcessId()), nullptr);
  ASSERT_EQ(FindHolders({}, &holders), S_OK);
  EXPECT_TRUE(holders.empty());
}

TEST(Holders, FindsAndEndsOnlyItsOwnChild) {
  test::TempDir dir(L"holders");
  ASSERT_TRUE(dir.ok());
  const path file = path(dir.path()) / L"held.txt";
  ASSERT_TRUE(WriteFileText(file, "held"));
  test::PipedChild child;
  ASSERT_EQ(child.Start(test::SelfPath(),
                        L"--pdft-child hold-file " + test::QuoteArgument(file.native())),
            S_OK);
  ASSERT_EQ(child.ReadLine(), "held");

  std::vector<Holder> holders;
  const path files[] = {file};
  ASSERT_EQ(FindHolders(files, &holders), S_OK);
  const Holder* found = FindPid(holders, child.Pid());
  ASSERT_NE(found, nullptr) << holders.size() << " holder(s), none of them the child";
  const Holder holder = *found;
  DWORD ownSession = 0;
  ASSERT_TRUE(ProcessIdToSessionId(GetCurrentProcessId(), &ownSession));
  EXPECT_EQ(holder.session, ownSession);
  EXPECT_TRUE(IEquals(holder.image, test::SelfPath())) << WideToUtf8Lossy(holder.image);
  FILETIME created = {};
  FILETIME unused[3] = {};
  ASSERT_TRUE(GetProcessTimes(child.Process(), &created, &unused[0], &unused[1], &unused[2]));
  EXPECT_EQ(CompareFileTime(&created, &holder.start), 0);

  // The thumbnail process's allow list does not cover it.
  std::wstring surrogate;
  ASSERT_EQ(SurrogateImagePath(&surrogate), S_OK);
  const std::wstring_view onlySurrogate[] = {surrogate};
  EXPECT_EQ(TerminateOwnSessionHolder(holder, onlySurrogate), E_ACCESSDENIED);
  EXPECT_EQ(WaitForSingleObject(child.Process(), 0), static_cast<DWORD>(WAIT_TIMEOUT));

  // A holder whose start time no longer matches is someone else now.
  const std::wstring self = test::SelfPath();
  const std::wstring_view ownImage[] = {self};
  Holder stale = holder;
  stale.start.dwLowDateTime ^= 1;
  EXPECT_EQ(TerminateOwnSessionHolder(stale, ownImage), S_FALSE);
  EXPECT_EQ(WaitForSingleObject(child.Process(), 0), static_cast<DWORD>(WAIT_TIMEOUT));

  // With its image allowed, it is ended, and the file is free.
  EXPECT_EQ(TerminateOwnSessionHolder(holder, ownImage), S_OK);
  EXPECT_EQ(child.Wait(5000), 1u);
  ASSERT_EQ(FindHolders(files, &holders), S_OK);
  EXPECT_EQ(FindPid(holders, holder.pid), nullptr);
  EXPECT_EQ(TerminateOwnSessionHolder(holder, ownImage), S_FALSE);  // gone already
}

TEST(Holders, NeverThisProcess) {
  test::TempDir dir(L"holders");
  ASSERT_TRUE(dir.ok());
  const path file = path(dir.path()) / L"mine.txt";
  ASSERT_TRUE(WriteFileText(file, "mine"));
  wil::unique_hfile mine(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(mine);
  std::vector<Holder> holders;
  const path files[] = {file};
  ASSERT_EQ(FindHolders(files, &holders), S_OK);
  const Holder* found = FindPid(holders, GetCurrentProcessId());
  ASSERT_NE(found, nullptr);
  const std::wstring self = test::SelfPath();
  const std::wstring_view ownImage[] = {self};
  EXPECT_EQ(TerminateOwnSessionHolder(*found, ownImage), E_ACCESSDENIED);
}

TEST(Parent, AChildFindsThisProcess) {
  test::PipedChild child;
  ASSERT_EQ(child.Start(test::SelfPath(), L"--pdft-child parent"), S_OK);
  EXPECT_EQ(child.ReadLine(), "parent=" + std::to_string(GetCurrentProcessId()));
  EXPECT_EQ(child.Wait(10000), 0u);
}

}  // namespace
}  // namespace pdft
