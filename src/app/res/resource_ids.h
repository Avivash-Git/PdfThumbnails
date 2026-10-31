// Resource IDs shared by the resource scripts (cli.rc, settings.rc) and the C++ code.
// The resource compiler reads this file too: #define only.

#pragma once

// Strings that the window will use. They hold characters outside ASCII, so the resource scripts
// must be read as UTF-8 (#pragma code_page(65001)); a unit test loads them from the built
// executables and compares them with the exact UTF-16 text.
#define IDS_REBUILD_THUMBNAILS 101  // "Rebuild thumbnails" + U+2026 (horizontal ellipsis)
#define IDS_VERSION_CHANGE 102      // "0.14.0 " + U+2192 (rightwards arrow) + " 0.15.0"
#define IDS_SEPARATOR 103           // U+00B7 (middle dot)

// The settings executable's notice while the window is not built yet.
#define IDS_WINDOW_NOT_READY 201
#define IDS_WINDOW_USE_CLI 202  // {cli} is replaced with the command-line executable's name
