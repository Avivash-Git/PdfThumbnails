# Third-party notices

PdfThumbnails is MIT-licensed (see `LICENSE.txt` in a package, `LICENSE` in the source tree).
The binaries include third-party software, listed below with its license. Each package ships
the complete license texts in the `licenses/` folder next to this file, unchanged from their
source; the paths below are relative to the package root.

## PDFium (in `PdfThumbnails-pdfium.dll`)

`PdfThumbnails-pdfium.dll` is `pdfium.dll` from the PDFium build `chromium/8125` published by
[bblanchon/pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) (non-V8, non-XFA),
renamed so that no other program's PDFium can be mixed up with it. It is not modified.

| Component | License | Full text |
| --- | --- | --- |
| [PDFium](https://pdfium.googlesource.com/pdfium/), Copyright 2014 The PDFium Authors | BSD-3-Clause; some files Apache-2.0 (both texts in the file) | `licenses/pdfium/pdfium.txt` |
| [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) build scripts, Copyright 2014-2025 Benoit Blanchon | MIT | `licenses/pdfium-binaries-LICENSE.txt` |

PDFium itself contains the following components:

| Component | License | Full text |
| --- | --- | --- |
| [Abseil](https://abseil.io/) | Apache-2.0 | `licenses/pdfium/abseil.txt` |
| Anti-Grain Geometry 2.3, Copyright 2002-2005 Maxim Shemanarev | AGG 2.3 license (permissive) | `licenses/pdfium/agg23.txt` |
| [Dragonbox](https://github.com/jk-jeon/dragonbox) | Apache-2.0 WITH LLVM-exception, or BSL-1.0 | `licenses/pdfium/dragonbox-Apache2-LLVM.txt`, `licenses/pdfium/dragonbox-Boost.txt` |
| [fast_float](https://github.com/fastfloat/fast_float), Copyright 2021 The fast_float authors | MIT | `licenses/pdfium/fast_float.txt` |
| [FreeType](https://freetype.org/), Copyright David Turner, Robert Wilhelm and Werner Lemberg | FreeType License (FTL) | `licenses/pdfium/freetype.txt` |
| [HarfBuzz](https://harfbuzz.github.io/) | "Old MIT" | `licenses/pdfium/harfbuzz.txt` |
| [Highway](https://github.com/google/highway) | Apache-2.0 | `licenses/pdfium/highway.txt` |
| [ICU](https://icu.unicode.org/), Copyright Unicode, Inc. | Unicode License V3 (with the notices of ICU's own components) | `licenses/pdfium/icu.txt` |
| [Little CMS](https://www.littlecms.com/), Copyright Marti Maria Saguer | MIT | `licenses/pdfium/lcms.txt` |
| [libjpeg-turbo](https://libjpeg-turbo.org/), including the Independent JPEG Group's software | IJG License and BSD-3-Clause (SIMD code: Zlib) | `licenses/pdfium/libjpeg_turbo.md`, `licenses/pdfium/libjpeg_turbo.ijg` |
| [OpenJPEG](https://www.openjpeg.org/) | BSD-2-Clause | `licenses/pdfium/libopenjpeg.txt` |
| [libpng](http://www.libpng.org/pub/png/libpng.html), Copyright The PNG Reference Library Authors | PNG Reference Library License v2 | `licenses/pdfium/libpng.txt` |
| [LLVM libc](https://libc.llvm.org/) | Apache-2.0 WITH LLVM-exception | `licenses/pdfium/llvm-libc.txt` |
| [simdutf](https://github.com/simdutf/simdutf), Copyright 2021 The simdutf authors | MIT | `licenses/pdfium/simdutf.txt` |
| [zlib](https://zlib.net/), Copyright Jean-loup Gailly and Mark Adler | Zlib | `licenses/pdfium/zlib.txt` |

## Settings app

| Component | License | Full text |
| --- | --- | --- |
| [Windows Implementation Libraries (WIL)](https://github.com/microsoft/wil), Copyright Microsoft Corporation; header-only, compiled into `PdfThumbnailsSettings.exe` and `PdfThumbnailsCli.exe` (and, in the test bundle, into the test programs and `thumbtest.exe`) | MIT | `licenses/wil-LICENSE.txt` |

## Test programs (not part of a release)

| Component | License | Full text |
| --- | --- | --- |
| [GoogleTest](https://github.com/google/googletest), Copyright 2008 Google Inc.; linked into the test programs `pdft_*_tests.exe` only | BSD-3-Clause | `licenses/googletest-LICENSE.txt` (test bundle only) |
