#include "system_probes.h"

#include <wil/result_macros.h>

#include <string>
#include <string_view>

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
  BlockCount CountCiBlocks(const std::filesystem::path& buildFolder,
                           int days) const noexcept override {
    return pdft::CountCiBlocks(buildFolder, days);
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
  HRESULT ApplyTaskOptions(const TaskLocation& location, const TaskOptions& options,
                           const std::filesystem::path& program,
                           const ProcessContext& context) const noexcept override {
    return pdft::ApplyTaskOptions(location, options, program, context);
  }
  HRESULT RunLiveTest(UINT size, const std::filesystem::path* file, DWORD timeoutMs,
                      LiveTest* result) const noexcept override {
    // Leftovers of runs that crashed or whose surrogate never returned; best effort. Never
    // reached in an elevated process: the snapshot skips the Explorer test there
    // (DecideLiveTestSkip), as SweepsProbes would.
    (void)SweepStaleProbes();
    return pdft::RunLiveTest(size, file, timeoutMs, result);
  }
  void Wait(DWORD ms) const noexcept override { Sleep(ms); }
  HRESULT DeleteLegacyTask() const noexcept override { return pdft::DeleteLegacyTask(); }
  HRESULT FindHolders(std::span<const std::filesystem::path> files,
                      std::vector<Holder>* holders) const noexcept override {
    return pdft::FindHolders(files, holders);
  }
  HRESULT EndHolder(const Holder& holder) const noexcept override try {
    std::wstring surrogate;
    RETURN_IF_FAILED(SurrogateImagePath(&surrogate));
    const std::wstring_view allowed[] = {surrogate};
    return TerminateOwnSessionHolder(holder, allowed);
  }
  CATCH_RETURN();
};

}  // namespace

const SystemProbes& RealSystemProbes() noexcept {
  static const RealProbes probes;
  return probes;
}

}  // namespace pdft
