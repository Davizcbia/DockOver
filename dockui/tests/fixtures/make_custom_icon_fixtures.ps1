# Generates the image fixtures expected by LiquidCustomIconsTest.exe <dir> (basic mode).
# Usage: powershell -NoProfile -File make_custom_icon_fixtures.ps1 -OutDir <empty folder>
#   red-wide.png    160x80, red at alpha 128            -> loads as 160x160, transparent padding, centre premultiplied (128,128,0,0)
#   cyan-multi.ico  PNG frames 32 and 256, cyan alpha 160 with a transparent 16 px border -> loader picks 256
#   large-wide.png  1024x512, green at alpha 128        -> downscaled to 512x512 (padded), centre alpha 128 / green 128
#   图标.png         32x32 (Unicode file name)
#   broken.png      not an image
#   wrong.ico       a real PNG with an .ico name (spoofed container must be rejected)
param([Parameter(Mandatory=$true)][string]$OutDir)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null

function New-Png([int]$w, [int]$h, [System.Drawing.Color]$fill, [int]$inset = 0) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $brush = New-Object System.Drawing.SolidBrush $fill
    $g.FillRectangle($brush, $inset, $inset, $w - 2 * $inset, $h - 2 * $inset)
    $brush.Dispose(); $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    return ,$ms.ToArray()
}

[IO.File]::WriteAllBytes((Join-Path $OutDir 'red-wide.png'), (New-Png 160 80 ([System.Drawing.Color]::FromArgb(128, 255, 0, 0))))
[IO.File]::WriteAllBytes((Join-Path $OutDir 'large-wide.png'), (New-Png 1024 512 ([System.Drawing.Color]::FromArgb(128, 0, 255, 0))))
[IO.File]::WriteAllBytes((Join-Path $OutDir ('{0}.png' -f ([string][char]0x56FE + [char]0x6807))), (New-Png 32 32 ([System.Drawing.Color]::FromArgb(255, 30, 120, 220))))
[IO.File]::WriteAllBytes((Join-Path $OutDir 'broken.png'), [Text.Encoding]::ASCII.GetBytes('this is not a PNG file'))
[IO.File]::WriteAllBytes((Join-Path $OutDir 'wrong.ico'), (New-Png 64 64 ([System.Drawing.Color]::FromArgb(255, 200, 0, 200))))

# Multi-frame ICO with PNG-compressed frames (Vista+ format): ICONDIR + ICONDIRENTRY[n] + image data.
$frames = @(@{ Side = 32; Data = (New-Png 32 32 ([System.Drawing.Color]::FromArgb(160, 0, 255, 255)) 2) },
            @{ Side = 256; Data = (New-Png 256 256 ([System.Drawing.Color]::FromArgb(160, 0, 255, 255)) 16) })
$ms = New-Object System.IO.MemoryStream; $bw = New-Object System.IO.BinaryWriter $ms
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$frames.Count)
$offset = 6 + 16 * $frames.Count
foreach ($f in $frames) {
    $dim = if ($f.Side -ge 256) { 0 } else { $f.Side }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32); $bw.Write([UInt32]$f.Data.Length); $bw.Write([UInt32]$offset)
    $offset += $f.Data.Length
}
foreach ($f in $frames) { $bw.Write($f.Data) }
$bw.Flush(); [IO.File]::WriteAllBytes((Join-Path $OutDir 'cyan-multi.ico'), $ms.ToArray()); $bw.Dispose()
"fixtures written to $OutDir"
