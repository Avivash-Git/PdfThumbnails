#include "diagnostics.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <cwctype>
#include <utility>

#include "json.h"
#include "paths.h"
#include "pe_info.h"
#include "project_config.h"
#include "registration.h"
#include "status_format.h"
#include "strings.h"

namespace pdft {
namespace {

// How much of the end of a log file is read: 30 lines of a few hundred characters fit easily.
constexpr DWORD kLogTailBytes = 64 * 1024;

std::wstring Widen(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }

std::wstring YesNo(bool value) { return value ? L"yes" : L"no"; }

std::wstring Or(const std::optional<std::wstring>& text, const wchar_t* missing) {
  return text ? L"\"" + *text + L"\"" : std::wstring(missing);
}

bool IsWordChar(wchar_t c) noexcept { return c >= 0x80 || std::iswalnum(c) || c == L'_'; }

bool IsDigit(wchar_t c) noexcept { return c >= L'0' && c <= L'9'; }

// A SID at |at|: "S-1-" (either case), then numbers separated by dashes, wherever it stands
// (also glued to a word: "TaskS-1-5-21-..." must not leak either). Returns its length and fills
// |parts| with the numbers after "S-1-" (authority first); 0 when there is none.
size_t SidAt(std::wstring_view text, size_t at, std::vector<std::wstring_view>* parts) {
  parts->clear();
  if (text.size() - at < 5 || (text[at] != L'S' && text[at] != L's') || text[at + 1] != L'-' ||
      text[at + 2] != L'1' || text[at + 3] != L'-') {
    return 0;
  }
  size_t end = at + 4;
  while (end < text.size() && IsDigit(text[end])) {
    const size_t start = end;
    while (end < text.size() && IsDigit(text[end])) ++end;
    parts->push_back(text.substr(start, end - start));
    // Another number follows only after a dash.
    if (end + 1 < text.size() && text[end] == L'-' && IsDigit(text[end + 1])) {
      ++end;
    } else {
      break;
    }
  }
  return parts->empty() ? 0 : end - at;
}

struct Needle {
  std::wstring_view text;
  const wchar_t* replacement;
};

void AddNeedle(std::wstring_view text, const wchar_t* replacement, std::vector<Needle>* needles) {
  if (text.empty()) return;
  for (const Needle& needle : *needles) {
    if (IEquals(needle.text, text)) return;  // the first meaning stays (<pc> before <domain>)
  }
  needles->push_back(Needle{text, replacement});
}

std::wstring Section(const wchar_t* title) { return std::wstring(L"\n") + title; }

std::wstring ClassText(const std::optional<ClassReg>& reg) {
  if (!reg) return L"none";
  return reg->dll + L" (exists " + YesNo(reg->dllExists) + L", PDFium " + YesNo(reg->pdfiumExists) +
         L", ThreadingModel \"" + reg->threading + L"\", version \"" + reg->fileVersion + L"\", " +
         (reg->peMachine ? std::wstring(ArchName(MachineToArch(*reg->peMachine)))
                         : std::wstring(L"architecture unknown")) +
         L")";
}

std::wstring SlotValueText(const std::optional<std::wstring>& value,
                           const ClsidDescription& owner) {
  if (SlotIsEmpty(value)) return L"empty";
  if (IsOurClsid(*value)) return *value + L" (ours)";
  return *value + L" (" + owner.name.value_or(L"no name") + L", DLL " +
         owner.dll.value_or(L"none") + L")";
}

const wchar_t* ProbeName(ContextProbe probe) {
  switch (probe) {
    case ContextProbe::kNone:
      return L"none";
    case ContextProbe::kReadOnly:
      return L"read-only comparison";
    case ContextProbe::kWrite:
      return L"canary";
  }
  return L"none";
}

const wchar_t* ViewsText(RegistryViews views) {
  switch (views) {
    case RegistryViews::kNotChecked:
      return L"not checked";
    case RegistryViews::kAgree:
      return L"agree";
    case RegistryViews::kDiffer:
      return L"differ";
    case RegistryViews::kUnknown:
      return L"unknown";
  }
  return L"unknown";
}

const wchar_t* VirtualizationText(Virtualization virtualization) {
  switch (virtualization) {
    case Virtualization::kNo:
      return L"no";
    case Virtualization::kYes:
      return L"yes";
    case Virtualization::kUnknown:
      return L"unknown";
  }
  return L"unknown";
}

const wchar_t* SourceText(PdfCraftSource source) {
  switch (source) {
    case PdfCraftSource::kNone:
      return L"none";
    case PdfCraftSource::kUpgradeCode:
      return L"Windows Installer, upgrade code";
    case PdfCraftSource::kProductName:
      return L"Windows Installer, product name";
    case PdfCraftSource::kUninstallKey:
      return L"Uninstall key";
  }
  return L"none";
}

std::wstring BuildsText(const std::vector<BuildInfo>& builds) {
  if (builds.empty()) return L"none";
  std::wstring text;
  for (const BuildInfo& build : builds) {
    if (!text.empty()) text += L", ";
    text += build.folder.name;
    if (!build.folder.complete) text += L" (incomplete)";
    if (build.dllMachine) {
      text += L" (";
      text += ArchName(MachineToArch(*build.dllMachine));
      text += L")";
    }
  }
  return text;
}

// Reads the last |maxBytes| bytes of |file| as UTF-8 (a character that does not decode becomes
// U+FFFD). *missing when there is no such file.
HRESULT ReadFileTail(const std::filesystem::path& file, DWORD maxBytes, std::wstring* text,
                     bool* startsMidLine, bool* missing) noexcept try {
  text->clear();
  *startsMidLine = false;
  *missing = false;
  wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      *missing = true;
      return S_OK;
    }
    return HRESULT_FROM_WIN32(error);
  }
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE),
               GetFileType(handle.get()) != FILE_TYPE_DISK);
  LARGE_INTEGER size = {};
  RETURN_IF_WIN32_BOOL_FALSE(GetFileSizeEx(handle.get(), &size));
  const LONGLONG start = std::max<LONGLONG>(0, size.QuadPart - maxBytes);
  LARGE_INTEGER offset = {};
  offset.QuadPart = start;
  RETURN_IF_WIN32_BOOL_FALSE(SetFilePointerEx(handle.get(), offset, nullptr, FILE_BEGIN));
  std::string bytes(static_cast<size_t>(size.QuadPart - start), '\0');
  DWORD read = 0;
  if (!bytes.empty()) {
    RETURN_IF_WIN32_BOOL_FALSE(
        ReadFile(handle.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr));
  }
  bytes.resize(read);
  *startsMidLine = start > 0;
  if (bytes.empty()) return S_OK;
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
  RETURN_LAST_ERROR_IF(length <= 0);
  text->resize(static_cast<size_t>(length));
  RETURN_LAST_ERROR_IF(MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                                           text->data(), length) <= 0);
  return S_OK;
}
CATCH_RETURN();

}  // namespace

std::wstring Redact(std::wstring_view text, const RedactContext& context) {
  std::vector<Needle> needles;
  // A profile folder without a trailing separator, so it matches with or without one, and its
  // own name.
  const auto trimmed = [](std::wstring_view path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.remove_suffix(1);
    return path;
  };
  const auto leaf = [](std::wstring_view path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? std::wstring_view() : path.substr(separator + 1);
  };
  const std::wstring_view profile = trimmed(context.profile);
  const std::wstring_view profileShort = trimmed(context.profileShort);
  AddNeedle(profile, L"%USERPROFILE%", &needles);
  AddNeedle(profileShort, L"%USERPROFILE%", &needles);
  AddNeedle(context.user, L"<user>", &needles);
  AddNeedle(leaf(profile), L"<user>", &needles);
  AddNeedle(leaf(profileShort), L"<user>", &needles);
  AddNeedle(context.computer, L"<pc>", &needles);
  AddNeedle(context.hostName, L"<pc>", &needles);
  AddNeedle(context.domain, L"<domain>", &needles);
  // Longest first, so the profile path wins over the user name inside it and a longer computer
  // name over a shorter one it starts with.
  std::stable_sort(needles.begin(), needles.end(),
                   [](const Needle& a, const Needle& b) { return a.text.size() > b.text.size(); });

  std::wstring out;
  out.reserve(text.size());
  std::vector<std::wstring_view> parts;
  size_t at = 0;
  while (at < text.size()) {
    if (const size_t length = SidAt(text, at, &parts)) {
      // Four or more numbers after the authority identify a domain, a PC or a user; the short
      // well-known SIDs (S-1-5-18, S-1-5-32-544, S-1-16-8192) name nobody and stay.
      if (parts.size() >= 5) {
        out += L"S-1-";
        out += parts[0];
        out += L"-";
        out += parts[1];
        out += L"-<masked>";
        if (parts[1] == L"21" && parts.size() >= 6) {
          // An account's SID: the domain's (or PC's) three numbers, then the account's RID,
          // which names nobody and tells the built-in accounts apart.
          out += L"-";
          out += parts.back();
        }
      } else {
        out += text.substr(at, length);
      }
      at += length;
      continue;
    }
    bool replaced = false;
    for (const Needle& needle : needles) {
      const size_t length = needle.text.size();
      if (text.size() - at < length || !IEquals(text.substr(at, length), needle.text)) continue;
      if (length < kRedactAnywhereChars &&
          ((at > 0 && IsWordChar(text[at - 1])) ||
           (at + length < text.size() && IsWordChar(text[at + length])))) {
        continue;
      }
      out += needle.replacement;
      at += length;
      replaced = true;
      break;
    }
    if (!replaced) out += text[at++];
  }
  return out;
}

std::wstring CompilerText() {
  std::wstring text = L"MSVC " + std::to_wstring(_MSC_VER / 100) + L"." +
                      std::to_wstring(_MSC_VER % 100) + L"." +
                      std::to_wstring(_MSC_FULL_VER % 100000) + L"." + std::to_wstring(_MSC_BUILD);
#if defined(__clang__)
  text += L" (clang-cl " + std::to_wstring(__clang_major__) + L"." +
          std::to_wstring(__clang_minor__) + L")";
#endif
  return text;
}

std::vector<std::wstring> LastLines(std::wstring_view text, size_t maxLines, bool startsMidLine) {
  std::vector<std::wstring> lines;
  size_t start = 0;
  if (startsMidLine) {
    const size_t newline = text.find(L'\n');
    start = newline == std::wstring_view::npos ? text.size() : newline + 1;
  }
  while (start < text.size()) {
    size_t end = text.find(L'\n', start);
    if (end == std::wstring_view::npos) end = text.size();
    std::wstring_view line = text.substr(start, end - start);
    if (line.ends_with(L'\r')) line.remove_suffix(1);
    lines.emplace_back(line);
    start = end + 1;
  }
  if (lines.size() > maxLines) lines.erase(lines.begin(), lines.end() - maxLines);
  return lines;
}

HRESULT ReadLogTailFrom(const std::filesystem::path& log, size_t maxLines,
                        std::vector<std::wstring>* lines) noexcept try {
  lines->clear();
  std::wstring text;
  bool midLine = false;
  bool missing = false;
  RETURN_IF_FAILED(ReadFileTail(log, kLogTailBytes, &text, &midLine, &missing));
  *lines = LastLines(text, maxLines, midLine);
  if (lines->size() < maxLines) {
    // Just after a rotation the current file is short; the lines before are in the old one.
    std::filesystem::path older = log;
    older += L".1";
    std::wstring olderText;
    bool olderMidLine = false;
    bool olderMissing = false;
    if (SUCCEEDED(ReadFileTail(older, kLogTailBytes, &olderText, &olderMidLine, &olderMissing))) {
      std::vector<std::wstring> before =
          LastLines(olderText, maxLines - lines->size(), olderMidLine);
      lines->insert(lines->begin(), std::make_move_iterator(before.begin()),
                    std::make_move_iterator(before.end()));
    }
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT ReadLogTail(size_t maxLines, std::vector<std::wstring>* lines) noexcept try {
  lines->clear();
  std::filesystem::path log;
  RETURN_IF_FAILED(GetLogPath(&log));
  return ReadLogTailFrom(log, maxLines, lines);
}
CATCH_RETURN();

HRESULT GetRedactContext(const ProcessContext& context, RedactContext* redact) noexcept try {
  *redact = {};
  wil::unique_cotaskmem_string profile;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DONT_VERIFY, nullptr, &profile))) {
    redact->profile = profile.get();
    // A program started through a short path shows the profile in its 8.3 form.
    wchar_t shortPath[MAX_PATH] = {};
    const DWORD length = GetShortPathNameW(profile.get(), shortPath, ARRAYSIZE(shortPath));
    if (length > 0 && length < ARRAYSIZE(shortPath) && !IEquals(shortPath, redact->profile)) {
      redact->profileShort = shortPath;
    }
  }
  // "DOMAIN\user"; only the SID when the account could not be looked up (masked as a SID).
  const size_t backslash = context.userAccount.find(L'\\');
  if (backslash != std::wstring::npos) {
    redact->domain = context.userAccount.substr(0, backslash);
    redact->user = context.userAccount.substr(backslash + 1);
  } else if (!context.userAccount.starts_with(L"S-1-")) {
    redact->user = context.userAccount;
  }
  for (const auto& [format, field] :
       {std::pair{ComputerNameNetBIOS, &RedactContext::computer},
        std::pair{ComputerNameDnsHostname, &RedactContext::hostName}}) {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 256] = {};
    DWORD size = ARRAYSIZE(name);
    if (GetComputerNameExW(format, name, &size)) redact->*field = name;
  }
  return S_OK;
}
CATCH_RETURN();

std::wstring BuildDiagnostics(const Snapshot& s, std::span<const Finding> findings,
                              std::span<const std::wstring> logTail, const RedactContext& redact) {
  std::vector<std::wstring> lines;
  const auto add = [&](std::wstring line) { lines.push_back(std::move(line)); };
  const ProcessContext& c = s.context;

  add(std::wstring(kProductName) + L" diagnostics (personal details are masked)");
  add(L"Product:        " + std::wstring(kProductName) + L" " + kProductVersion + L" (" + kArch +
      L"), file version " + kFileVersion + L", built with " + CompilerText());
  add(L"Program:        " + s.paths.ownExe.native());
  add(L"Windows:        build " + (s.osBuild.empty() ? std::wstring(L"unknown") : s.osBuild) +
      L", " + ArchName(c.osArch));
  wchar_t integrity[16] = {};
  swprintf_s(integrity, L"0x%04lX", c.integrityRid);
  add(L"Process:        " + std::wstring(ArchName(c.processArch)) + L", integrity " + integrity +
      L", elevated " + YesNo(c.elevated) + L", another user than the desktop's " +
      YesNo(c.differentFromShellUser));
  add(L"Account:        " + c.userAccount + L" (" + c.userSid + L")");
  add(L"Package above:  " + c.packagedAncestor.value_or(L"none"));
  std::wstring registry = L"Registry check: " + std::wstring(ProbeName(c.probe));
  if (c.probe == ContextProbe::kReadOnly)
    registry += L", views " + std::wstring(ViewsText(c.views));
  if (c.probe == ContextProbe::kWrite) {
    registry += L", virtualized " + std::wstring(VirtualizationText(c.virtualization));
  }
  if (FAILED(c.probeError)) registry += L", error " + HResultText(c.probeError);
  if (s.windowsChecksSkipped) registry += L"; the Windows checks were skipped";
  add(std::move(registry));

  add(Section(L"Install"));
  add(L"  Scope:        " +
      std::wstring(s.scope == Scope::kMachine ? L"machine-wide"
                   : s.scope == Scope::kUser  ? L"per user"
                                              : L"none") +
      (s.installRoot.empty() ? std::wstring() : L", root " + s.installRoot.native()));
  add(L"  Machine class: " + ClassText(s.machineClass));
  add(L"  User class:   " + ClassText(s.userClass));
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    add(L"  Slot " + std::wstring(SlotName(slot)) + L": " +
        SlotValueText(s.slots[slot], s.slotOwners[slot]));
  }
  add(L"  Machine-wide slots: " + Or(s.machineSlots[0], L"none") + L", " +
      Or(s.machineSlots[1], L"none"));
  add(L"  Per-user builds: " + BuildsText(s.userBuilds));
  add(L"  Machine-wide builds: " + BuildsText(s.machineBuilds));

  add(Section(L"Windows"));
  if (s.windowsChecksSkipped) {
    add(L"  Effective:    not checked");
  } else {
    add(L"  Effective:    " + SlotValueText(s.effective, s.effectiveOwner));
    add(L"  Default app:  ProgID " + Or(s.defaultApp.progId, L"none") + L", handler " +
        SlotValueText(s.defaultApp.handler, s.defaultAppHandlerOwner) + L", TypeOverlay " +
        Or(s.defaultApp.overlay, L"none"));
  }
  add(L"  Explorer:     IconsOnly " + YesNo(s.explorer.iconsOnly) +
      L", DisableThumbnails for the user " + YesNo(s.explorer.thumbnailsOffByUserPolicy) +
      L", for the PC " + YesNo(s.explorer.thumbnailsOffByMachinePolicy));
  add(L"  " + std::wstring(kPdfCraftProductName) + L":     " +
      (s.pdfcraft.installed
           ? L"installed, version \"" + s.pdfcraft.version + L"\", product code \"" +
                 s.pdfcraft.productCode + L"\", found by " + SourceText(s.pdfcraft.source)
           : std::wstring(L"not found")));

  add(Section(L"Explorer test and Smart App Control"));
  if (s.last && s.last->liveTest) {
    add(L"  Explorer test: " + HResultText(*s.last->liveTest) +
        L" at the last recorded check (this report does not run it)");
  } else {
    add(L"  Explorer test: no recorded result (this report does not run it)");
  }
  std::wstring sac = L"  Smart App Control: ";
  switch (s.sac.state) {
    case SacState::kOff:
      sac += L"off";
      break;
    case SacState::kOn:
      sac += L"on";
      break;
    case SacState::kEvaluation:
      sac += L"evaluation";
      break;
    case SacState::kAbsent:
      sac += L"absent";
      break;
    case SacState::kUnknown:
      sac += L"unknown (" + SacValueText(s.sac) + L")";
      break;
  }
  if (s.blocks) {
    sac += s.blocks->readable
               ? L", blocks in the last " + std::to_wstring(kSacBlockDays) + L" days: " +
                     std::to_wstring(s.blocks->count)
               : L", Code Integrity log unreadable (" + HResultText(s.blocks->error) + L")";
  } else {
    sac += L", blocks not counted";
  }
  add(std::move(sac));
  for (const Finding& finding : findings) {
    if (finding.id == FindingId::kSacBlocked || finding.id == FindingId::kSacOn ||
        finding.id == FindingId::kSacEvaluation || finding.id == FindingId::kSacUnknown) {
      add(L"  Verdict:      " + FindingText(s, finding));
    }
  }

  add(Section(L"Settings and checks"));
  add(L"  Zoom:         " + std::to_wstring(s.zoom.decision.zoom) + L" %, registry value " +
      (s.zoom.registryRaw                ? std::to_wstring(*s.zoom.registryRaw)
       : s.zoom.registryType == REG_NONE ? std::wstring(L"missing")
                                         : L"of type " + std::to_wstring(s.zoom.registryType)) +
      L", settings.ini " +
      (s.zoom.legacyIniZoom ? std::to_wstring(*s.zoom.legacyIniZoom) : std::wstring(L"none")) +
      L" (" + s.zoom.legacyIni.native() + L")");
  const std::optional<std::filesystem::path> program = InstalledSettingsExe(s);
  add(L"  Task program: " + (s.installRoot.empty() ? std::wstring(L"none (nothing installed)")
                             : program ? program->native() + L" (there)"
                                       : (s.installRoot / kSettingsExe).native() +
                                             (s.settingsExeFound ? L" (there)" : L" (missing)")));
  if (s.task.exists) {
    add(L"  Check task:   " + s.task.path + (s.task.atFallback ? L" (root-folder name)" : L"") +
        L", enabled " + YesNo(s.task.enabled) + L", after " + kPdfCraftProductName + L" updates " +
        YesNo(s.task.afterPdfCraft) + L", at sign-in " + YesNo(s.task.atSignIn) + L", runs \"" +
        s.task.actionPath + L"\" " + s.task.actionArgs);
    add(L"  Last run:     " +
        (s.task.lastRun ? TaskTimeText(*s.task.lastRun) : std::wstring(L"never")) +
        (s.task.lastResult ? L", result " + HResultText(static_cast<HRESULT>(*s.task.lastResult))
                           : std::wstring()));
  } else {
    std::wstring looked = s.taskLocation.folder + L"\\" + s.taskLocation.name;
    if (!s.taskLocation.fallbackName.empty()) {
      looked += L" and " + s.taskLocation.fallbackFolder +
                (s.taskLocation.fallbackFolder.ends_with(L'\\') ? L"" : L"\\") +
                s.taskLocation.fallbackName;
    }
    add(L"  Check task:   none (looked for " + looked + L")");
  }
  if (s.last) {
    add(L"  Last check:   " + Widen(IsoUtcText(s.last->time)) + L", exit code " +
        std::to_wstring(static_cast<int>(s.last->result)) + L", \"" + s.last->summary + L"\", " +
        kPdfCraftProductName + L" \"" + s.last->pdfcraftVersion + L"\"");
  } else {
    add(L"  Last check:   none recorded");
  }
  add(L"  1.x clean-up done: " + YesNo(s.migratedFromV1));

  add(Section(L"1.x leftovers"));
  const LegacyState& legacy = s.legacy;
  add(L"  Task " + YesNo(legacy.legacyTask) + L", RunOnce value " + YesNo(legacy.runOnce) +
      L", settings.ini " + YesNo(legacy.ini) +
      (legacy.iniZoom ? L" (Zoom=" + std::to_wstring(*legacy.iniZoom) + L")" : std::wstring()) +
      L", machine-wide class in a 1.x build " + YesNo(legacy.machineClassInV1) +
      L", per-user class in a 1.x build " + YesNo(legacy.userClassInV1));
  for (const std::filesystem::path& file : legacy.files) add(L"  " + file.native());

  if (!s.issues.empty()) {
    add(Section(L"Could not be read"));
    for (const GatherIssue& issue : s.issues) {
      add(L"  " + std::wstring(issue.part) + L": " + HResultText(issue.hr));
    }
  }

  add(Section(L"Findings"));
  if (findings.empty()) add(L"  none");
  for (const Finding& finding : findings) {
    const wchar_t* severity = finding.severity == Severity::kError     ? L"error"
                              : finding.severity == Severity::kWarning ? L"warning"
                                                                       : L"info";
    add(L"  " + std::wstring(FindingName(finding.id)) + L" (" + severity +
        (finding.breaksThumbnails ? L", breaks thumbnails" : L"") + L"): " +
        FindingText(s, finding));
  }

  add(Section(L"Status"));
  const std::vector<StatusLine> status = FormatStatus(s, findings);
  add(RenderStatus(status));

  add(Section((L"Log (last " + std::to_wstring(kDiagnosticsLogLines) + L" lines)").c_str()));
  if (logTail.empty()) add(L"  (empty)");
  for (const std::wstring& line : logTail) add(L"  " + line);

  std::wstring text;
  for (const std::wstring& line : lines) {
    if (!text.empty()) text += L"\n";
    text += line;
  }
  return Redact(text, redact);
}

}  // namespace pdft
