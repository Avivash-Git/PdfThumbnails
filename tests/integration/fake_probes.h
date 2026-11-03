// Fakes for everything Windows-wide that the check and the installer's steps ask through
// SystemProbes: the process context, what Windows resolves for .pdf, Smart App Control, the Code
// Integrity log, PdfCraft, the tasks, the Explorer test, and the processes that hold files. The
// writes that no redirection reaches (the check task, 1.x's task, ending a holder) are recorded,
// never made.

#pragma once

#include <windows.h>

#include <cwchar>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "project_config.h"
#include "system_probes.h"

namespace pdft::test {

class FakeQueries final : public AssociationQueries {
 public:
  HRESULT ThumbnailHandler(std::optional<std::wstring>* clsid) const noexcept override {
    ++calls;
    *clsid = handler;
    return S_OK;
  }
  HRESULT ProgId(std::optional<std::wstring>* id) const noexcept override {
    ++calls;
    *id = progId;
    return S_OK;
  }
  std::optional<std::wstring> handler = std::wstring(kHandlerClsidString);
  std::optional<std::wstring> progId;
  mutable int calls = 0;
};

// Windows as the tests want it: by default a Medium process whose registry is Explorer's,
// Windows resolving our handler, the Explorer test passing, Smart App Control off, PdfCraft
// 0.14.0, no tasks, nothing holding any file.
class FakeProbes final : public SystemProbes {
 public:
  FakeProbes() {
#if defined(_M_ARM64)
    constexpr Arch kNative = Arch::kArm64;
#else
    constexpr Arch kNative = Arch::kX64;
#endif
    context.userSid = L"S-1-12-1-1-2-3-4";
    context.userAccount = L"PC\\someone";
    context.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
    context.probe = ContextProbe::kWrite;
    context.virtualization = Virtualization::kNo;
    context.processArch = kNative;
    context.osArch = kNative;
    sac.state = SacState::kOff;
    pdfcraft = {true, L"0.14.0", L"", PdfCraftSource::kUpgradeCode};
  }
  HRESULT GetProcessContext(ContextProbe, ProcessContext* out) const noexcept override {
    *out = context;
    return S_OK;
  }
  const AssociationQueries& Associations() const noexcept override { return queries; }
  HRESULT ReadSacState(const RegistryRoots&, SacReading* reading) const noexcept override {
    *reading = sac;
    return S_OK;
  }
  BlockCount CountCiBlocks(const std::filesystem::path& buildFolder,
                           int days) const noexcept override {
    countedFolder = buildFolder;
    countedDays = days;
    const size_t call = countCalls++;
    return call < counts.size() ? counts[call] : BlockCount{true, 0, S_OK};
  }
  HRESULT DetectPdfCraft(PdfCraftInfo* info) const noexcept override {
    *info = pdfcraft;
    return S_OK;
  }
  HRESULT ReadTask(const TaskLocation&, TaskState* state) const noexcept override {
    *state = task;
    return S_OK;
  }
  HRESULT LegacyTaskExists(bool* exists) const noexcept override {
    *exists = legacyTask;
    return S_OK;
  }
  // Records the change and makes the task look like Task Scheduler would afterwards.
  HRESULT ApplyTaskOptions(const TaskLocation& location, const TaskOptions& options,
                           const std::filesystem::path& program,
                           const ProcessContext& used) const noexcept override {
    applied.push_back(Applied{location, options, program, used.elevated});
    if (FAILED(applyError)) return applyError;
    if (HasNoTriggers(options)) {
      task = {};
      return S_OK;
    }
    task.exists = true;
    task.enabled = options.enabled;
    task.afterPdfCraft = options.afterPdfCraft == TriggerSetting::kEnabled;
    task.atSignIn = options.atSignIn == TriggerSetting::kEnabled;
    task.path = location.folder + L"\\" + location.name;
    task.actionPath = program.native();
    task.actionArgs = kCheckTaskArguments;
    task.options = options;
    return S_OK;
  }
  HRESULT RunLiveTest(UINT size, const std::filesystem::path* file, DWORD,
                      LiveTest* result) const noexcept override {
    ++liveRuns;
    liveSize = size;
    if (file && FAILED(testFileError)) return testFileError;
    *result = live;
    return S_OK;
  }
  void Wait(DWORD ms) const noexcept override { waits.push_back(ms); }
  HRESULT DeleteLegacyTask() const noexcept override {
    ++legacyDeletes;
    if (!legacyTask) return S_FALSE;
    legacyTask = false;
    return S_OK;
  }
  // The holders of each file come from |holders| (by path, ignoring case).
  HRESULT FindHolders(std::span<const std::filesystem::path> files,
                      std::vector<Holder>* found) const noexcept override {
    found->clear();
    ++holderSearches;
    for (const std::filesystem::path& file : files) {
      for (const HeldFile& held : holders) {
        if (!held.ended && _wcsicmp(held.file.c_str(), file.c_str()) == 0) {
          found->push_back(held.holder);
        }
      }
    }
    return S_OK;
  }
  // Records the request; the holder lets go of its files when |endWorks|.
  HRESULT EndHolder(const Holder& holder) const noexcept override {
    ended.push_back(holder.pid);
    if (!endWorks) return E_ACCESSDENIED;
    for (HeldFile& held : holders) {
      if (held.holder.pid == holder.pid) held.ended = true;
    }
    return S_OK;
  }

  ProcessContext context;
  FakeQueries queries;
  SacReading sac;
  std::vector<BlockCount> counts;
  PdfCraftInfo pdfcraft;
  mutable TaskState task;           // ApplyTaskOptions changes it
  mutable bool legacyTask = false;  // DeleteLegacyTask clears it
  HRESULT applyError = S_OK;        // what ApplyTaskOptions returns instead of changing the task
  LiveTest live{S_OK, {96, 96}, 150};
  HRESULT testFileError = S_OK;  // a test file's run cannot start (no probe thread, ...)
  struct HeldFile {
    std::filesystem::path file;
    Holder holder;
    bool ended = false;
  };
  mutable std::vector<HeldFile> holders;
  bool endWorks = true;  // EndHolder succeeds

  mutable size_t countCalls = 0;
  mutable std::filesystem::path countedFolder;
  mutable int countedDays = 0;
  mutable int liveRuns = 0;
  mutable UINT liveSize = 0;
  mutable std::vector<DWORD> waits;
  mutable int legacyDeletes = 0;
  mutable int holderSearches = 0;
  mutable std::vector<DWORD> ended;  // the processes EndHolder was asked to end
  struct Applied {
    TaskLocation location;
    TaskOptions options;
    std::filesystem::path program;
    bool elevated = false;
  };
  mutable std::vector<Applied> applied;
};

}  // namespace pdft::test
