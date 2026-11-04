// What thumbtest.cpp shares with the thumbnail refresh experiment's modes (refresh_modes.cpp).

#pragma once

#include <string>
#include <vector>

namespace thumbtest {

std::string Utf8(const std::wstring& text);

// Appends the files named by |arg|: the file itself or, for a directory, the *.pdf
// files directly inside it, sorted by name (ordinal, ignoring case). False when a
// directory holds no PDFs.
bool AddFiles(const std::wstring& arg, std::vector<std::wstring>* files);

// The refresh experiment's modes, built only with the settings app's core
// (PDFT_THUMBTEST_REFRESH): whether |mode| is one of them, running one (|argv|[1] is the mode;
// returns the exit code), and their usage lines.
bool IsRefreshMode(const std::wstring& mode);
int RunRefreshMode(int argc, wchar_t** argv);
void PrintRefreshUsage();

}  // namespace thumbtest
