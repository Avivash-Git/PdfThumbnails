<#
.SYNOPSIS
    Writes small synthetic PDFs (plus broken files) that exercise the thumbnail renderer:
    orientation, /Rotate, CropBox, first-page selection, annotations, extreme page sizes,
    heavy pages and files that must fail cleanly.
#>
param([Parameter(Mandatory)][string]$OutDir)

Set-StrictMode -Version 3
$ErrorActionPreference = 'Stop'
$latin1 = [Text.Encoding]::GetEncoding(28591)
$invariant = [Globalization.CultureInfo]::InvariantCulture
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Serialises object bodies (object n = $Objects[n-1], object 1 = catalog) with a valid xref.
function ConvertTo-PdfBytes([string[]]$Objects) {
    $sb = New-Object Text.StringBuilder
    [void]$sb.Append("%PDF-1.7`n")
    $offsets = New-Object Collections.Generic.List[int]
    for ($i = 0; $i -lt $Objects.Count; $i++) {
        $offsets.Add($sb.Length)
        [void]$sb.Append("$($i + 1) 0 obj`n$($Objects[$i])`nendobj`n")
    }
    $xref = $sb.Length
    [void]$sb.Append("xref`n0 $($Objects.Count + 1)`n0000000000 65535 f `n")
    foreach ($offset in $offsets) { [void]$sb.Append(('{0:D10} 00000 n ' -f $offset) + "`n") }
    [void]$sb.Append("trailer`n<< /Size $($Objects.Count + 1) /Root 1 0 R >>`nstartxref`n$xref`n%%EOF`n")
    return $latin1.GetBytes($sb.ToString())
}

function New-Stream([string]$Data, [string]$Dict = '') { "<< $Dict /Length $($Data.Length) >>`nstream`n$Data`nendstream" }

# A test card: frame, red/green squares in the top corners, blue square bottom-left, a label.
function New-Card([int]$W, [int]$H, [string]$Label, [int]$LabelY = -1) {
    if ($LabelY -lt 0) { $LabelY = [int]($H / 2) }
    $s = [Math]::Max(10, [Math]::Min(60, [int]([Math]::Min($W, $H) / 8)))
    @(
        "0.2 g 3 w 10 10 $($W - 20) $($H - 20) re S",
        "1 0 0 rg 20 $($H - 20 - $s) $s $s re f",
        "0 0.6 0 rg $($W - 20 - $s) $($H - 20 - $s) $s $s re f",
        "0 0 1 rg 20 20 $s $s re f",
        "0 g BT /F1 36 Tf $([int]($W * 0.1)) $LabelY Td ($Label) Tj ET"
    ) -join "`n"
}

# One-page document; $PageExtra goes into the page dictionary, $More are objects 6, 7, ...
function New-OnePager([string]$MediaBox, [string]$Content, [string]$PageExtra = '', [string[]]$More = @()) {
    $objects = @(
        '<< /Type /Catalog /Pages 2 0 R >>',
        '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        "<< /Type /Page /Parent 2 0 R /MediaBox [$MediaBox] $PageExtra /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        (New-Stream $Content),
        '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>'
    ) + $More
    ConvertTo-PdfBytes $objects
}

function Save([string]$Name, [byte[]]$Bytes) { [IO.File]::WriteAllBytes((Join-Path $OutDir $Name), $Bytes) }

$portrait = New-OnePager '0 0 595 842' (New-Card 595 842 'PORTRAIT')
Save '01-portrait.pdf' $portrait
Save '02-landscape.pdf' (New-OnePager '0 0 842 595' (New-Card 842 595 'LANDSCAPE'))
# /Rotate 90 turns the portrait page clockwise: shown landscape, red square top-right.
Save '03-rotate90.pdf' (New-OnePager '0 0 595 842' (New-Card 595 842 'ROTATE 90') '/Rotate 90')
# Only the top half is visible: red and green squares, label, no blue square.
Save '04-cropbox.pdf' (New-OnePager '0 0 595 842' (New-Card 595 842 'CROPBOX' 600) '/CropBox [0 421 595 842]')

# Two pages; the thumbnail must show the green first page.
Save '05-multipage.pdf' (ConvertTo-PdfBytes @(
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>',
    (New-Stream "0 0.7 0 rg 0 0 595 842 re f`n1 g BT /F1 60 Tf 120 400 Td (PAGE ONE) Tj ET"),
    '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>',
    (New-Stream "1 0 0 rg 0 0 595 842 re f`n1 g BT /F1 60 Tf 120 400 Td (PAGE TWO) Tj ET")
))

# 30 x 3000 pt strip of coloured bands: extreme aspect ratio.
$bands = (0..29 | ForEach-Object { [string]::Format($invariant, '{0:0.00} 0.4 {1:0.00} rg 0 {2} 30 100 re f', ($_ / 29), (1 - $_ / 29), ($_ * 100)) }) -join "`n"
Save '06-tall-strip.pdf' (New-OnePager '0 0 30 3000' $bands)

# A square annotation with its own appearance stream (needs FPDF_ANNOT to show up).
Save '07-annotation.pdf' (New-OnePager '0 0 595 842' (New-Card 595 842 'ANNOTATION' 700) '/Annots [6 0 R]' @(
    '<< /Type /Annot /Subtype /Square /Rect [150 150 450 450] /C [1 0.5 0] /F 4 /AP << /N 7 0 R >> >>',
    (New-Stream '1 0.5 0 rg 0 0 300 300 re f' '/Type /XObject /Subtype /Form /BBox [0 0 300 300]')
))

# ~120,000 filled rectangles: a slow page for timing.
$heavy = New-Object Text.StringBuilder
for ($y = 0; $y -lt 400; $y++) {
    for ($x = 0; $x -lt 300; $x++) {
        [void]$heavy.AppendFormat($invariant, "{0:0.00} {1:0.00} 0.5 rg {2:0.0} {3:0.0} 1.7 1.7 re f`n",
            ($x / 300), ($y / 400), (10 + $x * 1.9), (20 + $y * 2))
    }
}
Save '08-heavy.pdf' (New-OnePager '0 0 595 842' $heavy.ToString())

Save '09-max-page-14400.pdf' (New-OnePager '0 0 14400 14400' (New-Card 14400 14400 'HUGE PAGE'))
Save '10-zero-size-page.pdf' (New-OnePager '0 0 0 0' 'BT ET')
Save '11-no-pages.pdf' (ConvertTo-PdfBytes @('<< /Type /Catalog /Pages 2 0 R >>', '<< /Type /Pages /Kids [] /Count 0 >>'))
Save '12-empty.pdf' ([byte[]]@())
$random = New-Object Random 1234
$garbage = New-Object byte[] 16384
$random.NextBytes($garbage)
Save '13-garbage.pdf' $garbage
Save '14-text-file.pdf' ($latin1.GetBytes("This is not a PDF.`n"))
Save '15-truncated.pdf' ($portrait[0..([int]($portrait.Length * 0.55))])
# XFA forms. Dynamic XFA (/NeedsRendering true) must keep the icon: its page 1 is only a
# placeholder. Static XFA renders from its ordinary page content like any other PDF.
function New-XfaPdf([string]$Label, [string]$CatalogExtra) {
    ConvertTo-PdfBytes @(
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [] /XFA 6 0 R >> $CatalogExtra >>",
        '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>',
        (New-Stream (New-Card 595 842 $Label)),
        '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>',
        (New-Stream '<xdp:xdp xmlns:xdp="http://ns.adobe.com/xdp/"><template xmlns="http://www.xfa.org/schema/xfa-template/3.3/"/></xdp:xdp>')
    )
}
Save '17-xfa-dynamic.pdf' (New-XfaPdf 'XFA DYNAMIC' '/NeedsRendering true')
Save '18-xfa-static.pdf' (New-XfaPdf 'XFA STATIC' '')

# Non-ASCII file name (Greek and Japanese), built from code points because PowerShell 5.1 reads BOM-less scripts as ANSI.
Save ('16-unicode-' + (-join [char[]](0x03B4, 0x03BF, 0x03BA, 0x03B9, 0x03BC, 0x03AE, 0x002D, 0x30C6, 0x30B9, 0x30C8)) + '.pdf') $portrait

Get-ChildItem -LiteralPath $OutDir -Filter *.pdf | ForEach-Object { '{0,10:N0}  {1}' -f $_.Length, $_.Name }
