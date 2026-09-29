# Generates winui\Assets\DockDebug.ico (the app icon).
#
# The lightning bolt is the "zap" icon from Feather Icons (https://feathericons.com),
# MIT License, Copyright (c) 2013-2017 Cole Bemis. See THIRD-PARTY-NOTICES.md.
# It is drawn filled, with Feather's 2px round-joined stroke, on a rounded blue tile.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1 [-PreviewPath preview.png]
param(
    [string]$IcoPath = (Join-Path $PSScriptRoot '..\winui\Assets\DockDebug.ico'),
    [string]$PreviewPath = ''
)
Add-Type -AssemblyName System.Drawing

# Feather "zap": <polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/> in a 24x24 viewBox.
$ZapPoints = @(@(13, 2), @(3, 14), @(12, 14), @(11, 22), @(21, 10), @(12, 10))

function Render([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Rounded square, slightly inset like Windows 11 app icons.
    $inset = [Math]::Max(0.5, $size * 0.06)
    $side = $size - 2 * $inset
    $r = $side * 0.24
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($inset, $inset, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($inset + $side - 2 * $r, $inset, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($inset + $side - 2 * $r, $inset + $side - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc($inset, $inset + $side - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.RectangleF $inset, $inset, $side, $side
    $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 58, 160, 255)), ([System.Drawing.Color]::FromArgb(255, 0, 84, 196)), 45.0
    $g.FillPath($fill, $path)
    if ($size -ge 32) {
        $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(60, 0, 0, 0)), ([Math]::Max(1.0, $size / 64.0))
        $g.DrawPath($pen, $path)
    }

    # Map the 24x24 viewBox onto the tile; the glyph gets relatively larger at small sizes for legibility.
    $box = $size * $(if ($size -le 24) { 0.84 } else { 0.70 })
    $offset = ($size - $box) / 2
    $unit = $box / 24.0
    $bolt = New-Object 'System.Drawing.PointF[]' $ZapPoints.Count
    for ($i = 0; $i -lt $ZapPoints.Count; $i++) {
        $bolt[$i] = New-Object System.Drawing.PointF ($offset + $ZapPoints[$i][0] * $unit), ($offset + $ZapPoints[$i][1] * $unit)
    }
    $g.FillPolygon([System.Drawing.Brushes]::White, $bolt)
    $stroke = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), (2 * $unit)
    $stroke.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawPolygon($stroke, $bolt)
    $g.Dispose()
    return $bmp
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($s in $sizes) {
    $bmp = Render $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    if ($s -eq 256 -and $PreviewPath) { $bmp.Save($PreviewPath, [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
    ,$ms.ToArray()
}

# ICO container with PNG-compressed entries (supported since Windows Vista).
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $b = if ($s -ge 256) { 0 } else { $s }
    $w.Write([byte]$b); $w.Write([byte]$b); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$images[$i].Length); $w.Write([UInt32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
$IcoPath = [System.IO.Path]::GetFullPath($IcoPath)
[System.IO.File]::WriteAllBytes($IcoPath, $out.ToArray())
"wrote $IcoPath ($($out.Length) bytes)"
