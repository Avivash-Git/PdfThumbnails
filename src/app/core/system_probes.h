// The check's questions that only Windows itself can answer and that RegistryRoots cannot
// redirect: the process context (who runs, and whether its registry view is Explorer's), what
// Windows resolves for .pdf, Smart App Control, the Code Integrity log, Windows Installer, Task
// Scheduler and the Explorer test. The code that gathers the state takes them through this
// interface, so its tests can answer every question with a fake and never read the PC's real
// state, start a thumbnail surrogate or depend on what is installed. Everything that reads the
// registry takes RegistryRoots instead. The writes here are the ones no redirection reaches either:
// the check task (registering or deleting it), deleting 1.x's task, and ending a thumbnail
// process that holds an old build's DLLs; tests record them instead.

#pragma once

#include <windows.h>

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "context.h"
#include "live_test.h"
#include "pdfcraft.h"
#include "processes.h"
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
  // Deletes 1.x's task (DeleteLegacyTask): S_FALSE when it is not there or not this user's.
  [[nodiscard]] virtual HRESULT DeleteLegacyTask() const noexcept = 0;
  // The processes that hold |files| (FindHolders, Restart Manager).
  [[nodiscard]] virtual HRESULT FindHolders(std::span<const std::filesystem::path> files,
                                            std::vector<Holder>* holders) const noexcept = 0;
  // Ends |holder| if it is Windows' thumbnail process of this session and user
  // (TerminateOwnSessionHolder with SurrogateImagePath as the only image allowed).
  [[nodiscard]] virtual HRESULT EndHolder(const Holder& holder) const noexcept = 0;
};

// The real answers: the functions of the same names.
const SystemProbes& RealSystemProbes() noexcept;

}  // namespace pdft
