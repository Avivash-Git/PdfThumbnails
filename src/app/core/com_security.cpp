#include "com_security.h"

#include <objbase.h>
#include <sddl.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <atomic>

namespace pdft {
namespace {

// The absolute security descriptor handed to CoInitializeSecurity, with the parts it points to.
// CoInitializeSecurity's documentation does not say that COM copies it, so it lives in static
// storage for the life of the process. Zero-initialized: no static initialiser runs.
struct ComDescriptor {
  SECURITY_DESCRIPTOR descriptor;
  alignas(DWORD) BYTE dacl[256];
  alignas(DWORD) BYTE owner[SECURITY_MAX_SID_SIZE];
  alignas(DWORD) BYTE group[SECURITY_MAX_SID_SIZE];
};
ComDescriptor g_comDescriptor;
std::atomic_flag g_comInitialized;  // set by the first InitializeComSecurity() call

// Builds kComAccessSddl in g_comDescriptor, in the absolute form COM requires.
HRESULT BuildComDescriptor(PSECURITY_DESCRIPTOR* out) noexcept {
  *out = nullptr;
  wil::unique_hlocal_security_descriptor relative;
  RETURN_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      kComAccessSddl, SDDL_REVISION_1, relative.put(), nullptr));
  DWORD descriptorSize = sizeof(g_comDescriptor.descriptor);
  DWORD daclSize = sizeof(g_comDescriptor.dacl);
  DWORD saclSize = 0;  // the descriptor has no SACL
  DWORD ownerSize = sizeof(g_comDescriptor.owner);
  DWORD groupSize = sizeof(g_comDescriptor.group);
  // Fails with ERROR_INSUFFICIENT_BUFFER if a longer descriptor ever outgrows the buffers.
  RETURN_IF_WIN32_BOOL_FALSE(
      MakeAbsoluteSD(relative.get(), &g_comDescriptor.descriptor, &descriptorSize,
                     reinterpret_cast<PACL>(g_comDescriptor.dacl), &daclSize, nullptr, &saclSize,
                     g_comDescriptor.owner, &ownerSize, g_comDescriptor.group, &groupSize));
  RETURN_HR_IF(E_UNEXPECTED, !IsValidSecurityDescriptor(&g_comDescriptor.descriptor));
  *out = &g_comDescriptor.descriptor;
  return S_OK;
}

}  // namespace

HRESULT InitializeComSecurity() noexcept {
  // Once per process: COM may still refer to the descriptor of the first call.
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED), g_comInitialized.test_and_set());
  RETURN_IF_FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
  auto uninitialize = wil::scope_exit([]() noexcept { CoUninitialize(); });
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  RETURN_IF_FAILED(BuildComDescriptor(&descriptor));
  RETURN_IF_FAILED(CoInitializeSecurity(descriptor, -1, nullptr, nullptr,
                                        RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE,
                                        nullptr, EOAC_NONE, nullptr));
  uninitialize.release();
  return S_OK;
}

}  // namespace pdft
