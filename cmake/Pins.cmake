# Every file the build downloads: URL and SHA-256. Nothing is fetched without a pin, and the
# CI download cache is keyed on this file. Bumps follow docs/MAINTAINING.md (Updating PDFium),
# one PR each.

include_guard(GLOBAL)

# ---- PDFium: bblanchon/pdfium-binaries chromium/8152 (non-V8, non-XFA) ---------------------
set(PDFT_PDFIUM_TAG "chromium/8152")
set(_pdft_pdfium_release "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium%2F8152")

set(PDFT_PDFIUM_x64_URL          "${_pdft_pdfium_release}/pdfium-win-x64.tgz")
set(PDFT_PDFIUM_x64_SHA256       "cee2b0e9990bc67c0721774932961b9dbf2766046c10eec925acd4cfba6fdfe3")
# DLL_SHA256 = bin/pdfium.dll inside the archive (shipped as PDFT_PDFIUM_DLL)
set(PDFT_PDFIUM_x64_DLL_SHA256   "ba884cc5c6f2c3cd7c2ecca9dce36c2e341f78eb586abd691a2bbc017aa9871e")

set(PDFT_PDFIUM_arm64_URL        "${_pdft_pdfium_release}/pdfium-win-arm64.tgz")
set(PDFT_PDFIUM_arm64_SHA256     "8614b10c6a780acc3ae9fdd425d6b61f9d22ae84301b19e1ac82f573930c217c")
set(PDFT_PDFIUM_arm64_DLL_SHA256 "4efcd99a4eb7415123a7ec7dd52b5dfe0c0513732357004a578c3013eb2b744b")

# Who must have built the archives (GitHub artifact attestations; checked by
# cmake/FetchPdfium.cmake -DVERIFY_ATTESTATION=ON, which CI runs on a download-cache miss).
set(PDFT_PDFIUM_ATTESTATION_REPO     "bblanchon/pdfium-binaries")
set(PDFT_PDFIUM_ATTESTATION_WORKFLOW "bblanchon/pdfium-binaries/.github/workflows/build-all.yml")

unset(_pdft_pdfium_release)

# ---- GoogleTest v1.18.0 (test programs only) ----------------------------------------------
set(PDFT_GTEST_URL    "https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz")
set(PDFT_GTEST_SHA256 "6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5")

# ---- WIL v1.0.260126.7 = commit cbf677fb (header-only; compiled into the settings app) ----
# A GitHub-generated source archive: if its checksum ever drifts, switch to a git-commit pin.
set(PDFT_WIL_URL    "https://github.com/microsoft/wil/archive/cbf677fb0a942557d08fd129f4c106a76247b2ec.tar.gz")
set(PDFT_WIL_SHA256 "d7b3c903b501f6ea73a733ee4cd4215119dc10eee969bda2aa134732b8e8fe6f")
