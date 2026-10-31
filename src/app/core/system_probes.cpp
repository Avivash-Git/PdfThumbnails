#include "system_probes.h"

namespace pdft {
namespace {

class RealProbes final : public SystemProbes {
 public:
  HRESULT GetProcessContext(ContextProbe probe, ProcessContext* context) const noexcept override {
    return pdft::GetProcessContext(probe, context);
  }
  const AssociationQueries& Associations() const noexcept override {
    return SystemAssociationQueries();
  }
  HRESULT ReadSacState(const RegistryRoots& roots, SacReading* reading) const noexcept override {
    return pdft::ReadSacState(roots, reading);
  }
  BlockCount CountCiBlocks(std::wstring_view leaf, int days) const noexcept override {
    return pdft::CountCiBlocks(leaf, days);
  }
  HRESULT DetectPdfCraft(PdfCraftInfo* info) const noexcept override {
    return pdft::DetectPdfCraft(info);
  }
  HRESULT ReadTask(const TaskLocation& location, TaskState* state) const noexcept override {
    return pdft::ReadTask(location, state);
  }
  HRESULT LegacyTaskExists(bool* exists) const noexcept override {
    return pdft::LegacyTaskExists(exists);
  }
  HRESULT RunLiveTest(UINT size, const std::filesystem::path* file, DWORD timeoutMs,
                      LiveTest* result) const noexcept override {
    // Leftovers of runs that crashed or whose surrogate never returned; best effort.
    (void)SweepStaleProbes();
    return pdft::RunLiveTest(size, file, timeoutMs, result);
  }
  void Wait(DWORD ms) const noexcept override { Sleep(ms); }
};

}  // namespace

const SystemProbes& RealSystemProbes() noexcept {
  static const RealProbes probes;
  return probes;
}

}  // namespace pdft
