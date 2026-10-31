// The check's questions that only Windows itself can answer and that RegistryRoots cannot
// redirect: the process context (who runs, and whether its registry view is Explorer's), what
// Windows resolves for .pdf, Smart App Control, the Code Integrity log, Windows Installer, Task
// Scheduler and the Explorer test. The code that gathers the state takes them through this
// interface, so its tests can answer every question with a fake and never read the PC's real
// state, start a thumbnail surrogate or depend on what is installed. Everything that reads the
// registry takes RegistryRoots instead. The one write here is the check task's (registering
// it, or deleting it), which no redirection reaches either: tests record it instead.

#pragma once

#include <windows.h>

#include <filesystem>
#include <string_view>

#include "context.h"
#include "live_test.h"
#include "pdfcraft.h"
#include "registration.h"
#include "registry_roots.h"
#include "sac.h"
#include "tasks.h"

namespace pdft {

class SystemProbes {
 public:
  virtual ~SystemProbes() = default;
  // This process's context with the registry check |probe| (GetProcessContext).
  [[nodiscard]] virtual HRESULT GetProcessContext(ContextProbe probe,
                                                  ProcessContext* context) const noexcept = 0;
  // Windows' answers for .pdf: the thumbnail handler it resolves and the default app's ProgID.
  virtual const AssociationQueries& Associations() const noexcept = 0;
  // Smart App Control's state. The real one reads it under roots.machine.
  [[nodiscard]] virtual HRESULT ReadSacState(const RegistryRoots& roots,
                                             SacReading* reading) const noexcept = 0;
  virtual BlockCount CountCiBlocks(std::wstring_view leaf, int days) const noexcept = 0;
  [[nodiscard]] virtual HRESULT DetectPdfCraft(PdfCraftInfo* info) const noexcept = 0;
  [[nodiscard]] virtual HRESULT ReadTask(const TaskLocation& location,
                                         TaskState* state) const noexcept = 0;
  [[nodiscard]] virtual HRESULT LegacyTaskExists(bool* exists) const noexcept = 0;
  // Registers the check task, or deletes it when |options| has no trigger (ApplyTaskOptions).
  [[nodiscard]] virtual HRESULT ApplyTaskOptions(const TaskLocation& location,
                                                 const TaskOptions& options,
                                                 const std::filesystem::path& program,
                                                 const ProcessContext& context) const noexcept = 0;
  // The real one first deletes probe PDFs that earlier runs left in %TEMP% (SweepStaleProbes).
  [[nodiscard]] virtual HRESULT RunLiveTest(UINT size, const std::filesystem::path* file,
                                            DWORD timeoutMs, LiveTest* result) const noexcept = 0;
  // Waits |ms| milliseconds (the pause before the Code Integrity log is read a second time).
  virtual void Wait(DWORD ms) const noexcept = 0;
};

// The real answers: the functions of the same names.
const SystemProbes& RealSystemProbes() noexcept;

}  // namespace pdft
