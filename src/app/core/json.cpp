#include "json.h"

#include <oleauto.h>

#include <cstdio>
#include <optional>
#include <vector>

#include "pe_info.h"
#include "project_config.h"
#include "registration.h"
#include "status_format.h"
#include "strings.h"

namespace pdft {
namespace {

// A small writer: it places the commas and colons, so the code below only names fields and
// values. Keys are ASCII constants from this file and are written as they are.
class JsonWriter {
 public:
  void BeginObject() { Open('{'); }
  void EndObject() { Close('}'); }
  void BeginArray() { Open('['); }
  void EndArray() { Close(']'); }

  JsonWriter& Key(const char* key) {
    Separate();
    out_ += '"';
    out_ += key;
    out_ += "\":";
    afterKey_ = true;
    return *this;
  }

  void Null() { Raw("null"); }
  void Bool(bool value) { Raw(value ? "true" : "false"); }
  void Number(long long value) { Raw(std::to_string(value)); }
  void String(std::wstring_view text) { Raw(JsonString(text)); }
  void Hresult(HRESULT hr) { String(HResultText(hr)); }
  void Time(const std::string& iso) {
    if (iso.empty()) {
      Null();
    } else {
      Raw("\"" + iso + "\"");
    }
  }
  void OptionalString(const std::optional<std::wstring>& text) {
    if (text) {
      String(*text);
    } else {
      Null();
    }
  }
  // Empty text is written as null: "not known".
  void StringOrNull(std::wstring_view text) {
    if (text.empty()) {
      Null();
    } else {
      String(text);
    }
  }

  std::string Take() { return std::move(out_); }

 private:
  void Separate() {
    if (afterKey_) {
      afterKey_ = false;
      return;
    }
    if (!first_.empty()) {
      if (!first_.back()) out_ += ',';
      first_.back() = false;
    }
  }
  void Raw(std::string_view text) {
    Separate();
    out_ += text;
  }
  void Open(char bracket) {
    Separate();
    out_ += bracket;
    first_.push_back(true);
  }
  void Close(char bracket) {
    out_ += bracket;
    first_.pop_back();
  }

  std::string out_;
  std::vector<bool> first_;  // per open object or array: nothing written in it yet
  bool afterKey_ = false;
};

const char* ScopeName(Scope scope) {
  switch (scope) {
    case Scope::kMachine:
      return "machine";
    case Scope::kUser:
      return "user";
    case Scope::kNone:
      return "none";
  }
  return "none";
}

const char* ZoomOriginName(ZoomOrigin origin) {
  switch (origin) {
    case ZoomOrigin::kRegistry:
      return "registry";
    case ZoomOrigin::kLegacyIni:
      return "legacyIni";
    case ZoomOrigin::kDefault:
      return "default";
  }
  return "default";
}

const char* SkipName(LiveTestSkip skip) {
  switch (skip) {
    case LiveTestSkip::kNone:
      return nullptr;
    case LiveTestSkip::kNotRequested:
      return "notRequested";
    case LiveTestSkip::kElevated:
      return "elevated";
    case LiveTestSkip::kVirtualized:
      return "virtualized";
    case LiveTestSkip::kNotOurHandler:
      return "notOurHandler";
    case LiveTestSkip::kNoClass:
      return "noClass";
  }
  return nullptr;
}

const char* SacStateName(SacState state) {
  switch (state) {
    case SacState::kOff:
      return "off";
    case SacState::kOn:
      return "on";
    case SacState::kEvaluation:
      return "evaluation";
    case SacState::kAbsent:
      return "absent";
    case SacState::kUnknown:
      return "unknown";
  }
  return "unknown";
}

const char* SacVerdictName(SacVerdictKind kind) {
  switch (kind) {
    case SacVerdictKind::kNone:
      return "none";
    case SacVerdictKind::kEval:
      return "evaluation";
    case SacVerdictKind::kUnknown:
      return "unknown";
    case SacVerdictKind::kOk:
      return "ok";
    case SacVerdictKind::kBlocked:
      return "blocked";
    case SacVerdictKind::kOn:
      return "on";
    case SacVerdictKind::kOnUnknown:
      return "onUnknown";
  }
  return "unknown";
}

const char* SeverityName(Severity severity) {
  switch (severity) {
    case Severity::kInfo:
      return "info";
    case Severity::kWarning:
      return "warning";
    case Severity::kError:
      return "error";
  }
  return "info";
}

const char* FixName(FixKind fix) {
  switch (fix) {
    case FixKind::kNone:
      return "none";
    case FixKind::kAuto:
      return "auto";
    case FixKind::kTakeBack:
      return "takeBack";
    case FixKind::kNeedsAdmin:
      return "needsAdmin";
    case FixKind::kUserAction:
      return "userAction";
  }
  return "none";
}

void Ascii(JsonWriter& json, const char* text) {
  if (text) {
    json.String(std::wstring(text, text + std::char_traits<char>::length(text)));
  } else {
    json.Null();
  }
}

void ClassObject(JsonWriter& json, const std::optional<ClassReg>& reg) {
  if (!reg) {
    json.Null();
    return;
  }
  json.BeginObject();
  json.Key("dll").String(reg->dll);
  json.Key("dllExists").Bool(reg->dllExists);
  json.Key("pdfiumExists").Bool(reg->pdfiumExists);
  json.Key("threadingModel").StringOrNull(reg->threading);
  json.Key("version").StringOrNull(reg->fileVersion);
  if (reg->peMachine) {
    json.Key("arch").String(ArchName(MachineToArch(*reg->peMachine)));
  } else {
    json.Key("arch").Null();
  }
  json.EndObject();
}

// Task Scheduler's times are local; JSON carries UTC.
std::string TaskTimeIso(DATE local) {
  SYSTEMTIME localTime = {};
  SYSTEMTIME utcTime = {};
  FILETIME utc = {};
  if (local <= 0 || !VariantTimeToSystemTime(local, &localTime) ||
      !TzSpecificLocalTimeToSystemTime(nullptr, &localTime, &utcTime) ||
      !SystemTimeToFileTime(&utcTime, &utc)) {
    return "";
  }
  return IsoUtcText(utc);
}

void AppendUtf8(char32_t code, std::string* out) {
  if (code < 0x80) {
    *out += static_cast<char>(code);
  } else if (code < 0x800) {
    *out += static_cast<char>(0xC0 | (code >> 6));
    *out += static_cast<char>(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    *out += static_cast<char>(0xE0 | (code >> 12));
    *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (code & 0x3F));
  } else {
    *out += static_cast<char>(0xF0 | (code >> 18));
    *out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    *out += static_cast<char>(0x80 | (code & 0x3F));
  }
}

}  // namespace

std::string JsonString(std::wstring_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  out += '"';
  for (size_t i = 0; i < text.size(); ++i) {
    char32_t code = text[i];
    if (code >= 0xD800 && code <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
        text[i + 1] <= 0xDFFF) {
      code = 0x10000 + ((code - 0xD800) << 10) + (text[i + 1] - 0xDC00);
      ++i;
    } else if (code >= 0xD800 && code <= 0xDFFF) {
      code = 0xFFFD;  // a lone surrogate has no UTF-8 form
    }
    switch (code) {
      case U'"':
        out += "\\\"";
        break;
      case U'\\':
        out += "\\\\";
        break;
      case U'\b':
        out += "\\b";
        break;
      case U'\f':
        out += "\\f";
        break;
      case U'\n':
        out += "\\n";
        break;
      case U'\r':
        out += "\\r";
        break;
      case U'\t':
        out += "\\t";
        break;
      default:
        if (code < 0x20) {
          char escaped[8] = {};
          std::snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned>(code));
          out += escaped;
        } else {
          AppendUtf8(code, &out);
        }
        break;
    }
  }
  out += '"';
  return out;
}

std::string IsoUtcText(const FILETIME& utc) {
  SYSTEMTIME time = {};
  if (!FileTimeToSystemTime(&utc, &time)) return "";
  char text[32] = {};
  std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", time.wYear, time.wMonth,
                time.wDay, time.wHour, time.wMinute, time.wSecond);
  return text;
}

std::string StatusJson(const Snapshot& s, std::span<const Finding> findings, ExitCode exitCode) {
  JsonWriter json;
  json.BeginObject();
  json.Key("schema").Number(kStatusJsonSchema);

  json.Key("product").BeginObject();
  json.Key("name").String(kProductName);
  json.Key("version").String(kProductVersion);
  json.Key("arch").String(kArch);
  json.EndObject();

  json.Key("os").BeginObject();
  json.Key("build").StringOrNull(s.osBuild);
  json.Key("arch").String(ArchName(s.context.osArch));
  json.EndObject();

  json.Key("context").BeginObject();
  json.Key("elevated").Bool(s.context.elevated);
  json.Key("differentUser").Bool(s.context.differentFromShellUser);
  json.Key("packagedAncestor").OptionalString(s.context.packagedAncestor);
  json.Key("virtualized").Bool(s.windowsChecksSkipped);
  json.EndObject();

  json.Key("install").BeginObject();
  Ascii(json.Key("scope"), ScopeName(s.scope));
  json.Key("root").StringOrNull(s.installRoot.native());
  const ClassReg* active = s.ActiveClass();
  if (active && !active->dll.empty()) {
    json.Key("buildFolder")
        .StringOrNull(std::filesystem::path(active->dll).parent_path().filename().native());
  } else {
    json.Key("buildFolder").Null();
  }
  json.EndObject();

  json.Key("classes").BeginObject();
  ClassObject(json.Key("machine"), s.machineClass);
  ClassObject(json.Key("user"), s.userClass);
  json.EndObject();

  json.Key("slots").BeginArray();
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    const std::optional<std::wstring>& value = s.slots[slot];
    const bool empty = SlotIsEmpty(value);
    const bool ours = !empty && IsOurClsid(*value);
    json.BeginObject();
    json.Key("key").String(SlotKeyPath(slot));
    if (empty) {
      json.Key("value").Null();
    } else {
      json.Key("value").String(*value);
    }
    json.Key("ours").Bool(ours);
    if (ours) {
      json.Key("name").String(kProductName);
    } else {
      json.Key("name").OptionalString(s.slotOwners[slot].name);
    }
    json.EndObject();
  }
  json.EndArray();

  if (s.windowsChecksSkipped) {
    json.Key("effective").Null();
  } else {
    json.Key("effective").OptionalString(s.effective);
  }

  json.Key("defaultApp").BeginObject();
  json.Key("progId").OptionalString(s.defaultApp.progId);
  json.Key("handler").OptionalString(s.defaultApp.handler);
  json.Key("overlay").OptionalString(s.defaultApp.overlay);
  json.EndObject();

  json.Key("pdfcraft").BeginObject();
  json.Key("installed").Bool(s.pdfcraft.installed);
  if (s.pdfcraft.installed) {
    json.Key("version").StringOrNull(s.pdfcraft.version);
  } else {
    json.Key("version").Null();
  }
  if (s.last) {
    json.Key("lastSeen").StringOrNull(s.last->pdfcraftVersion);
  } else {
    json.Key("lastSeen").Null();
  }
  json.EndObject();

  json.Key("zoom").BeginObject();
  json.Key("value").Number(s.zoom.decision.zoom);
  Ascii(json.Key("origin"), ZoomOriginName(s.zoom.decision.origin));
  json.EndObject();

  json.Key("checks").BeginObject();
  if (s.task.exists) {
    json.Key("task").StringOrNull(s.task.path);
  } else {
    json.Key("task").Null();
  }
  json.Key("afterPdfCraft").Bool(s.task.exists && s.task.afterPdfCraft);
  json.Key("atSignIn").Bool(s.task.exists && s.task.atSignIn);
  json.Key("enabled").Bool(s.task.exists && s.task.enabled);
  json.Key("lastRun").Time(s.task.exists && s.task.lastRun ? TaskTimeIso(*s.task.lastRun) : "");
  if (s.task.exists && s.task.lastResult) {
    json.Key("lastResult").Hresult(static_cast<HRESULT>(*s.task.lastResult));
  } else {
    json.Key("lastResult").Null();
  }
  json.EndObject();

  json.Key("liveTest").BeginObject();
  json.Key("ran").Bool(s.live.has_value());
  if (s.live) {
    json.Key("hr").Hresult(s.live->hr);
    json.Key("ms").Number(s.live->ms);
    json.Key("skipReason").Null();
  } else if (FAILED(s.liveError)) {
    json.Key("hr").Hresult(s.liveError);
    json.Key("ms").Null();
    Ascii(json.Key("skipReason"), "couldNotStart");
  } else {
    json.Key("hr").Null();
    json.Key("ms").Null();
    Ascii(json.Key("skipReason"), SkipName(s.liveSkip));
  }
  json.EndObject();

  json.Key("sac").BeginObject();
  Ascii(json.Key("state"), SacStateName(s.sac.state));
  Ascii(json.Key("verdict"), SacVerdictName(s.sacVerdict.kind));
  if (s.blocks && s.blocks->readable) {
    json.Key("blocks").Number(s.blocks->count);
  } else {
    json.Key("blocks").Null();
  }
  if (s.blocks) {
    json.Key("blockLogReadable").Bool(s.blocks->readable);
  } else {
    json.Key("blockLogReadable").Null();
  }
  json.EndObject();

  json.Key("findings").BeginArray();
  for (const Finding& finding : findings) {
    json.BeginObject();
    json.Key("id").String(FindingName(finding.id));
    Ascii(json.Key("severity"), SeverityName(finding.severity));
    json.Key("breaks").Bool(finding.breaksThumbnails);
    Ascii(json.Key("fix"), FixName(finding.fix));
    json.Key("text").String(FindingText(s, finding));
    json.EndObject();
  }
  json.EndArray();

  json.Key("healthy").Bool(HealthyFromFindings(findings));
  json.Key("exitCode").Number(static_cast<int>(exitCode));
  json.EndObject();
  return json.Take();
}

}  // namespace pdft
