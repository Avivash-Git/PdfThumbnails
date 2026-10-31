#include "startup.h"

namespace pdft {
namespace {

// GetLastError() as an HRESULT; never S_OK, because a few functions fail without setting it.
HRESULT LastError() noexcept {
  const DWORD error = GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

}  // namespace

HRESULT HardenProcess() noexcept {
  // Plain Win32 on purpose: this runs before anything else, so it relies on KERNEL32 alone.
  HRESULT result = S_OK;
  const auto check = [&result](BOOL ok) noexcept {
    if (!ok && SUCCEEDED(result)) result = LastError();
  };
  check(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32));
  check(SetDllDirectoryW(L""));
  check(SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT));
  check(HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0));
  PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy = {};
  policy.NoRemoteImages = 1;
  policy.NoLowMandatoryLabelImages = 1;
  policy.PreferSystem32Images = 1;
  check(SetProcessMitigationPolicy(ProcessImageLoadPolicy, &policy, sizeof(policy)));
  return result;
}

}  // namespace pdft
