# Security

## Reporting a problem

Please report security problems privately, through GitHub: open this repository's **Security**
tab and choose **Report a vulnerability**. Please don't open a public issue for them.

It helps to include:

- what an attacker could do, and what they would need first;
- the steps to reproduce it, with a made-up PDF if a file is needed;
- the output of `PdfThumbnailsCli.exe --version` and your Windows build.

Please don't attach your own documents. `PdfThumbnailsCli.exe --diagnostics` gives a report with
personal details masked, if one is useful.

This is a project maintained in spare time, so replies may take a while. Fixes go into the newest
release.

## Supported versions

Only the newest release gets security fixes.

## What counts

PdfThumbnails reads PDF files that may come from anyone, so the most interesting problems are:

- a PDF that makes the thumbnail handler run code, or crash the thumbnail process in a way that
  could be exploited;
- a way to make the part that runs as administrator do something the user did not ask for;
- a way to make the program load a DLL from a folder it should not trust.

The thumbnail handler refuses to run with administrator rights, and it reads a PDF only inside
Windows' thumbnail process. The settings program never parses PDFs in its own window: its preview
renders two built-in samples in a short-lived separate process.

## What is out of scope

Problems in PDFium itself belong with [the PDFium project](https://pdfium.googlesource.com/pdfium/).
Please still tell us if PdfThumbnails ships a version with a known problem.
