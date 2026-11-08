// What Windows Search knows, read-only: the PDFs in its index (SystemIndex through OLE DB) and
// whether a folder is in its crawl scope. Everything runs on a thread of its own in a
// multi-threaded apartment with a bounded wait, so a hung Windows Search cannot hang the caller:
// after a timeout the thread finishes on its own and owns everything it uses. Paths found stay in
// memory: never logged, never printed.

#pragma once

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <vector>

namespace pdft {

// The PDFs Windows Search knows, newest first, at most |cap|. |under| (optional) limits the query
// to that folder and what is below it. S_OK with *available false when Windows Search is off,
// missing, refuses or does not answer in time (not a failure: the caller looks elsewhere);
// HRESULT_FROM_WIN32(ERROR_CANCELLED) when stopped. Any other failure only when out of memory or
// no thread could be started.
[[nodiscard]] HRESULT QueryIndexedPdfs(size_t cap, const std::filesystem::path* under,
                                       std::stop_token stop,
                                       std::vector<std::filesystem::path>* files,
                                       bool* available) noexcept;

// Whether |folder| is in Windows Search's crawl scope. A failure where Windows Search is off or
// missing, or does not answer in time.
[[nodiscard]] HRESULT IsInIndexScope(const std::filesystem::path& folder, bool* included) noexcept;

}  // namespace pdft
