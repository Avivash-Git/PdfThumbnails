// Where things live: the install roots, the app's data folder and log, this executable; and
// path comparisons that hold up against links, case and "..".

#pragma once

#include <windows.h>

#include <filesystem>

namespace pdft {

// <FOLDERID_ProgramFiles>\<Product>: the root of a machine-wide install.
[[nodiscard]] HRESULT GetMachineRoot(std::filesystem::path* out) noexcept;

// <FOLDERID_UserProgramFiles>\<Product> (%LOCALAPPDATA%\Programs\<Product>): the root of a
// per-user install.
[[nodiscard]] HRESULT GetUserRoot(std::filesystem::path* out) noexcept;

// <FOLDERID_LocalAppData>\<Product>: the app's own files (the log), per user, never roamed.
[[nodiscard]] HRESULT GetDataDir(std::filesystem::path* out) noexcept;

// <data dir>\Logs\<Product>.log
[[nodiscard]] HRESULT GetLogPath(std::filesystem::path* out) noexcept;

// The full path of this process's executable, however long.
[[nodiscard]] HRESULT GetOwnExe(std::filesystem::path* out) noexcept;

// The final path of an existing file or folder, as Windows resolves it: links and junctions
// followed, short (8.3) names expanded, the case as stored on disk, and without the \\?\ prefix
// (\\?\UNC\server\share becomes \\server\share). Fails for a path that does not exist.
[[nodiscard]] HRESULT Canonicalize(const std::filesystem::path& in,
                                   std::filesystem::path* out) noexcept;

// True when |child| is inside the folder |parent| after both went through Canonicalize, so a
// junction or a different spelling cannot lead out of |parent|. A path is not inside itself.
// False when either path cannot be canonicalized (for example because it does not exist).
bool IsUnder(const std::filesystem::path& child, const std::filesystem::path& parent) noexcept;

// The comparison IsUnder makes, on the paths as given (pure, no file system access): both must
// be absolute; "." and ".." are resolved lexically; components are compared whole, ignoring
// case. So "C:\a\bc" is not under "C:\a\b", and "C:\a\b\..\bc" is not either.
bool IsUnderLexically(const std::filesystem::path& child,
                      const std::filesystem::path& parent) noexcept;

}  // namespace pdft
