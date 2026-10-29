# Every file the build downloads: URL and SHA-256. Nothing is fetched without a pin, and the
# CI download cache is keyed on this file. Bumps follow docs/MAINTAINING.md (one PR each).

include_guard(GLOBAL)

# ---- PDFium: bblanchon/pdfium-binaries chromium/8125 (non-V8, non-XFA) ---------------------
set(PDFT_PDFIUM_TAG "chromium/8125")
set(_pdft_pdfium_release "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F8125")

set(PDFT_PDFIUM_x64_URL          "${_pdft_pdfium_release}/pdfium-win-x64.tgz")
set(PDFT_PDFIUM_x64_SHA256       "9a964261760cff52570aabf038ab346d3a5034dd8b30d0d5fe8861235d356b8b")
# DLL_SHA256 = bin/pdfium.dll inside the archive (shipped as PDFT_PDFIUM_DLL)
set(PDFT_PDFIUM_x64_DLL_SHA256   "4b0dec92555d69437bef0df37be630181ab3e83a6823ca264ede355a4f2ef994")

set(PDFT_PDFIUM_arm64_URL        "${_pdft_pdfium_release}/pdfium-win-arm64.tgz")
set(PDFT_PDFIUM_arm64_SHA256     "d394ba61991fb53f4d87f70988035f337d9e73f2bd2c3c1b966df8c43fcabb05")
set(PDFT_PDFIUM_arm64_DLL_SHA256 "61b3500520d2f7ec2f8074851ba941b1470e85b8849e6efe5966ef2ce676c1f3")

unset(_pdft_pdfium_release)

# ---- GoogleTest v1.18.0 (tests only; used from WP3) ---------------------------------------
set(PDFT_GTEST_URL    "https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz")
set(PDFT_GTEST_SHA256 "6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5")

# ---- WIL v1.0.260126.7 = commit cbf677fb (header-only, settings app; used from WP6) --------
# A GitHub-generated source archive: if its checksum ever drifts, switch to a git-commit pin.
set(PDFT_WIL_URL    "https://github.com/microsoft/wil/archive/cbf677fb0a942557d08fd129f4c106a76247b2ec.tar.gz")
set(PDFT_WIL_SHA256 "d7b3c903b501f6ea73a733ee4cd4215119dc10eee969bda2aa134732b8e8fe6f")
