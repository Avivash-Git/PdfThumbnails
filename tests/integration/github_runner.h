// Whether the integration tests run on a runner GitHub hosts: the tests that may only run on a PC
// thrown away after the job (opening File Explorer windows, starting copies of this test program
// inside jobs it makes) check it first.

#pragma once

#include <windows.h>

#include <string>

namespace pdft::test {

// The value of the environment variable |name|; empty when it is not set or longer than 31.
inline std::wstring EnvironmentValue(const wchar_t* name) {
  wchar_t value[32] = {};
  const DWORD length = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
  return length > 0 && length < ARRAYSIZE(value) ? std::wstring(value, length) : std::wstring();
}

// A runner GitHub hosts, thrown away after the job. GITHUB_ACTIONS=true alone is not enough: it is
// also set on a self-hosted runner, which may be a PC someone uses, and by tools that run a
// workflow locally.
inline bool OnGitHubHostedRunner() {
  return EnvironmentValue(L"GITHUB_ACTIONS") == L"true" &&
         EnvironmentValue(L"RUNNER_ENVIRONMENT") == L"github-hosted";
}

}  // namespace pdft::test
