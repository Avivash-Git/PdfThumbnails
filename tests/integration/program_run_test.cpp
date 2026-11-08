// Running a child with one output pipe (program_run.h), against this test program's own child
// modes: nothing else is started.

#include <windows.h>

#include "program_run.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "child_process.h"
#include "strings.h"

namespace pdft {
namespace {

std::unique_ptr<RunningProgram> StartSelf(const std::wstring& arguments) {
  ProgramRequest request;
  request.program = test::SelfPath();
  request.arguments = arguments;
  std::unique_ptr<RunningProgram> program;
  EXPECT_HRESULT_SUCCEEDED(RunningProgram::Start(request, &program));
  return program;
}

TEST(ProgramRun, GivesTheOutputAndTheExitCode) {
  const auto program = StartSelf(L"--pdft-child write-output 5 7");
  ASSERT_TRUE(program);
  std::string kept;
  bool truncated = true;
  std::vector<std::wstring> lines;
  ASSERT_HRESULT_SUCCEEDED(ReadProgramOutput(
      program->output(), 65536, [&lines](std::wstring_view line) { lines.emplace_back(line); },
      &kept, &truncated));
  DWORD code = 0;
  ASSERT_HRESULT_SUCCEEDED(program->WaitForExit(5000, &code));
  EXPECT_EQ(code, 7u);
  EXPECT_FALSE(truncated);
  ASSERT_EQ(lines.size(), 5u);
  EXPECT_EQ(lines[4], L"line 4");
}

TEST(ProgramRun, KeepsOnlyTheCapAndNeverBlocksTheProgram) {
  // Far more than a pipe's buffer: the program ends only when all of it was read.
  const auto program = StartSelf(L"--pdft-child write-output 40000 0");
  ASSERT_TRUE(program);
  std::string kept;
  bool truncated = false;
  ASSERT_HRESULT_SUCCEEDED(ReadProgramOutput(program->output(), 1000, {}, &kept, &truncated));
  DWORD code = 1;
  ASSERT_HRESULT_SUCCEEDED(program->WaitForExit(5000, &code));
  EXPECT_EQ(code, 0u);
  EXPECT_TRUE(truncated);
  EXPECT_EQ(kept.size(), 1000u);
}

TEST(ProgramRun, ATerminatedProgramEndsItsOutput) {
  const auto program = StartSelf(L"--pdft-child sleep-exit 60000 0");
  ASSERT_TRUE(program);
  DWORD code = 0;
  EXPECT_EQ(program->WaitForExit(0, &code), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
  program->Terminate();
  std::string kept;
  bool truncated = false;
  EXPECT_HRESULT_SUCCEEDED(ReadProgramOutput(program->output(), 100, {}, &kept, &truncated));
  ASSERT_HRESULT_SUCCEEDED(program->WaitForExit(5000, &code));
  EXPECT_EQ(code, static_cast<DWORD>(ERROR_CANCELLED));
}

TEST(ProgramRun, AProgramThatIsNotThereIsAFailure) {
  ProgramRequest request;
  request.program = std::filesystem::path(test::SelfFolder()) / L"pdft-no-such-program.exe";
  std::unique_ptr<RunningProgram> program;
  EXPECT_EQ(RunningProgram::Start(request, &program), HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
  EXPECT_FALSE(program);
}

}  // namespace
}  // namespace pdft
