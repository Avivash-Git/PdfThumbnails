// --status --json (pure): the status as one JSON object, schema 1, UTF-8. Scripts rely on the
// field names and their nesting; a change that removes or renames a field needs a new schema
// number. Fields that have no value are null, numbers are JSON numbers, HRESULTs are strings
// ("0x80040154"), times are ISO-8601 in UTC ("2026-10-30T23:26:04Z").
//
//   schema        1
//   product       {name, version, arch}
//   os            {build, arch}
//   context       {elevated, differentUser, packagedAncestor, virtualized}
//                 virtualized: this process may not see the registry File Explorer sees, so
//                 the checks through Windows' association code were skipped
//   install       {scope ("machine", "user", "none"), root, buildFolder}
//   classes       {machine, user}: each null or {dll, dllExists, pdfiumExists, threadingModel,
//                 version, arch}
//   slots         [{key, value, ours, name}]: the .pdf slots in order; key relative to HKCU,
//                 name what the registry calls the handler in value
//   effective     the CLSID Windows resolves for .pdf thumbnails
//   defaultApp    {progId, handler, overlay}
//   pdfcraft      {installed, version, lastSeen}: lastSeen is the version the last check recorded
//   zoom          {value, origin ("registry", "legacyIni", "default")}
//   checks        {task, afterPdfCraft, atSignIn, enabled, lastRun, lastResult}: task is the
//                 check task's path, null when there is none
//   liveTest      {ran, hr, ms, skipReason ("notRequested", "elevated", "virtualized",
//                 "notOurHandler", "noClass", "couldNotStart")}
//   sac           {state ("off", "on", "evaluation", "absent", "unknown"), verdict ("none",
//                 "evaluation", "unknown", "ok", "blocked", "on", "onUnknown"), blocks,
//                 blockLogReadable}
//   findings      [{id, severity ("info", "warning", "error"), breaks, fix ("none", "auto",
//                 "takeBack", "needsAdmin", "userAction"), text}]
//   healthy       no finding keeps thumbnails from working
//   exitCode      the command's exit code

#pragma once

#include <windows.h>

#include <span>
#include <string>
#include <string_view>

#include "evaluate.h"
#include "exit_code.h"
#include "snapshot.h"

namespace pdft {

inline constexpr int kStatusJsonSchema = 1;

// The status as JSON (pure but for the time zone of Task Scheduler's local times), without a
// line end. May throw std::bad_alloc.
std::string StatusJson(const Snapshot& snapshot, std::span<const Finding> findings,
                       ExitCode exitCode);

// |text| as a JSON string, quotes included (pure): '"' and '\' escaped, control characters as
// \b, \f, \n, \r, \t or \u00XX, everything else as UTF-8. Text from the registry may hold a
// lone UTF-16 surrogate; it becomes U+FFFD. May throw std::bad_alloc.
std::string JsonString(std::wstring_view text);

// "2026-10-30T23:26:04Z" (pure); "" for a time SYSTEMTIME cannot hold. May throw std::bad_alloc.
std::string IsoUtcText(const FILETIME& utc);

}  // namespace pdft
