# Code signing policy

**State today: releases are not signed yet.** The project intends to sign its releases through
[SignPath Foundation](https://signpath.org)'s free code signing for open-source projects. That
programme accepts only projects that have already been released, so PdfThumbnails 2.0.0 comes out
unsigned first, and the project applies once it is out ([ROADMAP.md](ROADMAP.md)). This page is
the policy those signed releases will follow; it changes only through this repository's history.

Once the project is accepted:

> Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
> [SignPath Foundation](https://signpath.org).

Signed files will then name **SignPath Foundation** as their publisher.

## What is signed

Only binaries built by this repository's GitHub Actions workflow
(`.github/workflows/build.yml`), from this repository's source and build scripts, at the commit
of the release, are submitted for signing. Nothing built on a personal PC is.

| File | Signed |
|---|---|
| `PdfThumbnails.dll`, `PdfThumbnailsSettings.exe`, `PdfThumbnailsCli.exe` (x64 and ARM64) | Yes |
| The setup, its uninstaller (`unins000.exe`), and Setup's temporary copy of itself | Yes |
| `PdfThumbnails-pdfium.dll` | **No.** It is PDFium, an upstream project's code in an unmodified build published by [pdfium-binaries](https://github.com/bblanchon/pdfium-binaries) ([THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md)). It ships unsigned inside the signed setup, checked against a pinned SHA-256 at every build. |

How the files are signed, step by step: [BUILDING.md, "Code signing"](BUILDING.md#code-signing).
SignPath's settings for the two signing requests are kept in this repository:
[`signpath/runtimes.xml`](../signpath/runtimes.xml) and [`signpath/setup.xml`](../signpath/setup.xml).

## Who does what

| Role | Who |
|---|---|
| Committers and reviewers (changes from anyone else are reviewed before they are merged) | [Avivash-Git](https://github.com/Avivash-Git) |
| Approvers (each signing request is approved by hand, release by release) | [Avivash-Git](https://github.com/Avivash-Git) |

Everyone in these roles uses multi-factor authentication for GitHub and for SignPath.

## Privacy

PdfThumbnails does not send any information to any other networked system unless specifically
requested by the user or the person installing or operating it. In fact it never uses the network
at all: no telemetry, no update check, no download ([README.md, "Privacy"](../README.md#privacy)).

## Changes to the system

The setup says what it installs and asks for administrator rights before it does. It sets
PdfThumbnails as File Explorer's thumbnail handler for PDF files, for all users, and adds a
scheduled check per user. Its entry in Installed apps, the uninstaller in the install folder and
the settings window's **Uninstall** button all remove everything it added.

## Reporting a signed file

If you find a signed PdfThumbnails file that you think should not have been signed, report it as
described in [SECURITY.md](../SECURITY.md).
