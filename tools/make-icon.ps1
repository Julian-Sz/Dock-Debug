# Generates the app icon (winui\Assets\DockDebug.ico) and the MSIX package logos (winui\Assets\*.png)
# referenced by winui\Package.appxmanifest.
#
# The lightning bolt is the "zap" icon from Feather Icons (https://feathericons.com),
# MIT License, Copyright (c) 2013-2017 Cole Bemis. See THIRD-PARTY-NOTICES.md.
# It is drawn filled, with Feather's 2px round-joined stroke, on a rounded blue tile.
#
# Usage (from the repository root):
#   powershell -ExecutionPolicy Bypass -File tools\make-icon.ps1 [-PreviewPath preview.png]
param(
    [string]$AssetsDir = (Join-Path $PSScriptRoot '..\winui\Assets'),
    [string]$PreviewPath = ''
)
Add-Type -AssemblyName System.Drawing
$AssetsDir = [System.IO.Path]::GetFullPath($AssetsDir)

# Feather "zap": <polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/> in a 24x24 viewBox.
$ZapPoints = @(@(13, 2), @(3, 14), @(12, 14), @(11, 22), @(21, 10), @(12, 10))

# Draws the icon tile of $size pixels with its top-left corner at ($x, $y).
function DrawTile($g, [float]$x, [float]$y, [int]$size) {
    # Rounded square, slightly inset like Windows 11 app icons.
    $inset = [Math]::Max(0.5, $size * 0.06)
    $side = $size - 2 * $inset
    $r = $side * 0.24
    $left = $x + $inset; $top = $y + $inset
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($left, $top, 2 * $r, 2 * $r, 180, 90)
    $path.AddArc($left + $side - 2 * $r, $top, 2 * $r, 2 * $r, 270, 90)
    $path.AddArc($left + $side - 2 * $r, $top + $side - 2 * $r, 2 * $r, 2 * $r, 0, 90)
    $path.AddArc($left, $top + $side - 2 * $r, 2 * $r, 2 * $r, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.RectangleF $left, $top, $side, $side
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
        $bolt[$i] = New-Object System.Drawing.PointF ($x + $offset + $ZapPoints[$i][0] * $unit), ($y + $offset + $ZapPoints[$i][1] * $unit)
    }
    $g.FillPolygon([System.Drawing.Brushes]::White, $bolt)
    $stroke = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), (2 * $unit)
    $stroke.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
    $g.DrawPolygon($stroke, $bolt)
}

# A transparent $width x $height image with a centered tile of $tile pixels.
function Render([int]$width, [int]$height, [int]$tile) {
    $bmp = New-Object System.Drawing.Bitmap $width, $height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    DrawTile $g ([Math]::Round(($width - $tile) / 2)) ([Math]::Round(($height - $tile) / 2)) $tile
    $g.Dispose()
    return $bmp
}

function SavePng($bmp, [string]$name) {
    $bmp.Save((Join-Path $AssetsDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# --- DockDebug.ico: window, taskbar and .exe icon (app.rc) ---
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($s in $sizes) {
    $bmp = Render $s $s $s
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
[System.IO.File]::WriteAllBytes((Join-Path $AssetsDir 'DockDebug.ico'), $out.ToArray())

# --- MSIX logos (Package.appxmanifest). The tile has its own background, so the manifest uses
# BackgroundColor="transparent" and the "unplated" variants are the same image. ---
Get-ChildItem $AssetsDir -Filter *.png | Remove-Item

# Square44x44Logo: Start menu app list, taskbar, search, Settings. Scaled variants plus fixed
# target sizes (used by the taskbar and Explorer).
foreach ($scale in 100, 200, 400) {
    $s = [int](44 * $scale / 100)
    SavePng (Render $s $s $s) "Square44x44Logo.scale-$scale.png"
}
foreach ($s in 16, 24, 32, 48, 256) {
    SavePng (Render $s $s $s) "Square44x44Logo.targetsize-$s.png"
    SavePng (Render $s $s $s) "Square44x44Logo.targetsize-${s}_altform-unplated.png"
}

# StoreLogo: package logo shown by the Store and App Installer.
foreach ($scale in 100, 200, 400) {
    $s = [int](50 * $scale / 100)
    SavePng (Render $s $s $s) "StoreLogo.scale-$scale.png"
}

# Square150x150Logo and Wide310x150Logo: Start tiles; the icon fills half the tile height.
foreach ($scale in 100, 200, 400) {
    $f = $scale / 100
    SavePng (Render ([int](150 * $f)) ([int](150 * $f)) ([int](75 * $f))) "Square150x150Logo.scale-$scale.png"
    SavePng (Render ([int](310 * $f)) ([int](150 * $f)) ([int](75 * $f))) "Wide310x150Logo.scale-$scale.png"
}

# SplashScreen: required by the manifest schema; WinUI desktop apps do not show it.
foreach ($scale in 100, 200) {
    $f = $scale / 100
    SavePng (Render ([int](620 * $f)) ([int](300 * $f)) ([int](120 * $f))) "SplashScreen.scale-$scale.png"
}

"wrote icon and logos to $AssetsDir"
