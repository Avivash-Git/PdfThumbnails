// Resource IDs shared by the resource scripts (cli.rc, settings.rc) and the C++ code.
// The resource compiler reads this file too: #define only.

#pragma once

// Strings that the window will use. They hold characters outside ASCII, so the resource scripts
// must be read as UTF-8 (#pragma code_page(65001)); a unit test loads them from the built
// executables and compares them with the exact UTF-16 text.
#define IDS_REBUILD_THUMBNAILS 101  // "Rebuild thumbnails" + U+2026 (horizontal ellipsis)
#define IDS_VERSION_CHANGE 102      // "0.14.0 " + U+2192 (rightwards arrow) + " 0.15.0"
#define IDS_SEPARATOR 103           // U+00B7 (middle dot)

// The sample PDFs the settings window's preview renders (RCDATA, settings executable only).
#define IDR_SAMPLE_DOCUMENT 301
#define IDR_SAMPLE_SLIDE 302

// The settings window.
#define IDI_APP 1
#define IDD_SETTINGS 1
#define IDA_SETTINGS 1
#define kFooterTopDlu 229  // where the footer band starts, in dialog units

#define IDC_HEAD_ZOOM 1001
#define IDC_ZOOM_LABEL 1002
#define IDC_ZOOM_SLIDER 1003
#define IDC_ZOOM_EDIT_NAME 1004
#define IDC_ZOOM_EDIT 1005
#define IDC_ZOOM_SPIN 1006
#define IDC_ZOOM_PERCENT 1007
#define IDC_ZOOM_HINT 1008
#define IDC_PREVIEW_DOC 1009
#define IDC_PREVIEW_SLIDE 1010
#define IDC_PREVIEW_DOC_CAPTION 1011
#define IDC_PREVIEW_SLIDE_CAPTION 1012
#define IDC_HEAD_CHECKS 1013
#define IDC_CHECK_PDFCRAFT 1014
#define IDC_CHECK_SIGNIN 1015
#define IDC_HEAD_STATUS 1016
#define IDC_STATUS_GLYPH1 1017
#define IDC_STATUS_LINE1 1018
#define IDC_STATUS_GLYPH2 1019
#define IDC_STATUS_LINE2 1020
#define IDC_STATUS_GLYPH3 1021
#define IDC_STATUS_LINE3 1022
#define IDC_STATUS_FIX 1023
#define IDC_STATUS_HINT 1024
#define IDC_INSTALL_MACHINE 1025
#define IDC_INSTALL_USER 1026
#define IDC_BANNER 1027
#define IDC_BANNER_LINK 1028
#define IDC_PROGRESS 1029
#define IDC_PROGRESS_CANCEL 1030
#define IDC_CHECK_NOW 1031
#define IDC_REBUILD 1032
#define IDC_OPEN_LOG 1033
#define IDC_COPY_DIAG 1034
#define IDC_UNINSTALL 1035

#define IDS_READING_STATE 301
#define IDS_STATE_UNREADABLE 302   // {hr}
#define IDS_BANNER_SANDBOXED 303   // {product}
#define IDS_BANNER_OTHER_USER 304  // {account}
#define IDS_BANNER_BUSY 305
#define IDS_CHECKING 307
#define IDS_CLOSING_AFTER_WORK 308
#define IDS_FIX 309
#define IDS_FIX_ADMIN 310
#define IDS_FIX_TAKE_BACK 311    // {product}
#define IDS_TAKE_BACK_TITLE 312  // {product}
#define IDS_TAKE_BACK_TEXT 313
#define IDS_DIAG_COPIED 314
#define IDS_DIAG_FAILED 315  // {hr}
#define IDS_LOG_MISSING 316
#define IDS_LOG_FAILED 317  // {hr}
#define IDS_SAVING 318
#define IDS_STOPPING 319
#define IDS_PDFCRAFT_NOT_FOUND 320
#define IDS_ASK_SAVE_TITLE 321
#define IDS_ASK_SAVE_SAVE 322
#define IDS_ASK_SAVE_DONT 323
#define IDS_ASK_STOP_TITLE 324
#define IDS_ASK_STOP_TEXT 325
#define IDS_ASK_STOP_STOP 326
#define IDS_ASK_STOP_KEEP 327
#define IDS_REBUILD_NOW 328
#define IDS_DONT_ASK_AGAIN 329
#define IDS_ALSO_CLEAR_CACHE 330
#define IDS_UNINSTALL_NOW 331
#define IDS_CHILD_TITLE_REBUILD 332
#define IDS_CHILD_TITLE_UNINSTALL 333
#define IDS_CHILD_TITLE_INSTALL 334
#define IDS_CHILD_NOT_STARTED 335  // {hr}

// The settings executable's notice while the window is not built yet.
#define IDS_WINDOW_NOT_READY 201
#define IDS_WINDOW_USE_CLI 202  // {cli} is replaced with the command-line executable's name
