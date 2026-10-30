// A private temporary directory for one test: %TEMP%\pdft-tests\<prefix>-<pid>-<n>, created
// empty and deleted with everything in it when the object goes away. Set PDFT_KEEP_TEMP=1 to
// keep the directories for a look after a failure.

#pragma once

#include <string>
#include <string_view>

namespace pdft::test {

class TempDir {
 public:
  explicit TempDir(std::wstring_view prefix = L"t");
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir();

  // False when no directory could be created; path() is then empty.
  bool ok() const { return !path_.empty(); }
  const std::wstring& path() const { return path_; }
  // path() + "\" + name.
  std::wstring File(std::wstring_view name) const;

 private:
  std::wstring path_;
};

// Deletes |path| and everything below it (links are removed, never followed). True when the
// path no longer exists afterwards.
bool RemoveTree(const std::wstring& path);

}  // namespace pdft::test
