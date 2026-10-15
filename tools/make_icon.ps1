# Generates the app icons (and 256 px PNG previews) for the tray app.
#
# Two states are produced from the same artwork (a white sine wave on a rounded
# badge), only the palette changes:
#   assets/white_sound.ico      lit:     cyan-to-blue gradient badge, white wave
#   assets/white_sound_off.ico  unlit:   dark LED badge with a light rim, grey wave
# The lit icon means "active", the unlit one means "inactive", so the tray shows
# the state at a glance. The unlit badge keeps a light rim because a very dark
# icon would disappear on a dark taskbar.
#
# Run it from anywhere with Windows PowerShell:
#     powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
#
# Widths written: 16, 20, 24, 32, 40, 48, 64, 96, 128 (32bpp BMP) and 256 (PNG),
# matching what Windows asks for at 100%-200% display scaling. Every size is
# rendered 4x and downsampled, so the rounded corners are properly antialiased.

Add-Type -AssemblyName System.Drawing

$assets = Join-Path $PSScriptRoot "..\assets"
New-Item -ItemType Directory -Force -Path $assets | Out-Null
$assets = (Resolve-Path $assets).Path

$ss = 4   # supersampling factor

function New-RoundRectPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2.0 * $r
    $p.AddArc($x,           $y,           $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y,           $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d,   0, 90)
    $p.AddArc($x,           $y + $h - $d, $d, $d,  90, 90)
    $p.CloseFigure()
    return $p
}

function New-Color($rgb, [int]$a) {
    return [System.Drawing.Color]::FromArgb($a, $rgb[0], $rgb[1], $rgb[2])
}

# Draws the icon at $S * $ss pixels; the caller downsamples it to $S.
function Render-Icon([int]$S, $pal) {
    $W = $S * $ss
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList $W, $W, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode   = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)

    # Rounded badge, top-left to bottom-right gradient.
    $pad = 0.045 * $W
    $bw  = $W - 2.0 * $pad
    $badge = New-RoundRectPath $pad $pad $bw $bw (0.24 * $bw)
    $rect  = New-Object System.Drawing.RectangleF -ArgumentList $pad, $pad, $bw, $bw
    $c1 = New-Color $pal.C1 255
    $c2 = New-Color $pal.C2 255
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush -ArgumentList $rect, $c1, $c2, 62.0
    $g.FillPath($brush, $badge)
    $brush.Dispose(); $badge.Dispose()

    # Optional rim: a light outline so the unlit badge still reads on a dark
    # taskbar. Inset by half the stroke so it stays inside the badge.
    if ($pal.Rim -ne $null) {
        $rimW = [Math]::Max(1.0 * $ss, 0.055 * $W)
        $ins  = $rimW / 2.0
        $rimPath = New-RoundRectPath ($pad + $ins) ($pad + $ins) ($bw - 2.0 * $ins) ($bw - 2.0 * $ins) (0.24 * ($bw - 2.0 * $ins))
        $pen = New-Object System.Drawing.Pen (New-Color $pal.Rim 255), ([single]$rimW)
        $g.DrawPath($pen, $rimPath)
        $pen.Dispose(); $rimPath.Dispose()
    }

    # One full sine cycle, drawn as a ribbon of constant vertical thickness so it
    # survives downsampling to 16 px.
    $mid   = $W / 2.0
    $amp   = 0.175 * $W
    $thick = [Math]::Max(2.0 * $ss, 0.105 * $W)
    $x0    = 0.205 * $W
    $x1    = 0.795 * $W
    $steps = 200
    $top = New-Object 'System.Collections.Generic.List[System.Drawing.PointF]'
    $bot = New-Object 'System.Collections.Generic.List[System.Drawing.PointF]'
    for ($i = 0; $i -le $steps; $i++) {
        $t = $i / [double]$steps
        $x = $x0 + ($x1 - $x0) * $t
        $y = $mid - $amp * [Math]::Sin(2.0 * [Math]::PI * $t)
        $top.Add([System.Drawing.PointF]::new([single]$x, [single]($y - $thick / 2.0)))
        $bot.Add([System.Drawing.PointF]::new([single]$x, [single]($y + $thick / 2.0)))
    }
    $ribbon = New-Object System.Drawing.Drawing2D.GraphicsPath
    $ribbon.AddLines($top.ToArray())
    $rb = $bot.ToArray(); [Array]::Reverse($rb)
    $ribbon.AddLines($rb)
    $ribbon.CloseFigure()
    $waveBrush = New-Object System.Drawing.SolidBrush (New-Color $pal.Wave 255)
    $g.FillPath($waveBrush, $ribbon)
    $waveBrush.Dispose(); $ribbon.Dispose()
    $g.Dispose()
    return $bmp
}

function Downscale($hi, [int]$S) {
    $lo = New-Object System.Drawing.Bitmap -ArgumentList $S, $S, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gl = [System.Drawing.Graphics]::FromImage($lo)
    $gl.CompositingMode   = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
    $gl.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $gl.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $dst = New-Object System.Drawing.Rectangle -ArgumentList 0, 0, $S, $S
    $gl.DrawImage($hi, $dst, 0, 0, $hi.Width, $hi.Height, [System.Drawing.GraphicsUnit]::Pixel)
    $gl.Dispose()
    return $lo
}

function Get-Bgra($bmp) {
    $w = $bmp.Width; $h = $bmp.Height
    $r = New-Object System.Drawing.Rectangle -ArgumentList 0, 0, $w, $h
    $d = $bmp.LockBits($r, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $n = $w * $h * 4
    $buf = New-Object byte[] $n
    [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0, $buf, 0, $n)
    $bmp.UnlockBits($d)
    return $buf
}

# Renders every size for one palette and writes <name>.ico + <name>.png.
function Build-Icon([string]$name, $pal) {
    $sizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
    $entries = @()

    foreach ($S in $sizes) {
        $hi = Render-Icon $S $pal
        $lo = Downscale $hi $S
        $hi.Dispose()

        if ($S -ge 256) {
            # PNG keeps the 256 px frame small; Windows accepts PNG frames in .ico.
            $ms = New-Object System.IO.MemoryStream
            $lo.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
            $entries += [pscustomobject]@{ Size = $S; Data = $ms.ToArray(); Png = $true }
            $ms.Dispose()
        } else {
            # 32 bpp DIB (bottom-up BGRA) followed by an all-zero AND mask.
            $bgra = Get-Bgra $lo
            $ms = New-Object System.IO.MemoryStream
            $bw = New-Object System.IO.BinaryWriter $ms
            $bw.Write([uint32]40); $bw.Write([int32]$S); $bw.Write([int32](2 * $S))
            $bw.Write([uint16]1);  $bw.Write([uint16]32); $bw.Write([uint32]0)
            $bw.Write([uint32]0);  $bw.Write([int32]0);   $bw.Write([int32]0)
            $bw.Write([uint32]0);  $bw.Write([uint32]0)
            for ($y = $S - 1; $y -ge 0; $y--) { $bw.Write($bgra, $y * $S * 4, $S * 4) }
            $maskRow = [int](([Math]::Floor(($S + 31) / 32)) * 4)
            $zero = New-Object byte[] ($maskRow * $S)
            $bw.Write($zero, 0, $zero.Length)
            $bw.Flush()
            $entries += [pscustomobject]@{ Size = $S; Data = $ms.ToArray(); Png = $false }
            $bw.Dispose(); $ms.Dispose()
        }

        # A ready-to-use 256 px PNG for docs/README use.
        if ($S -ge 256) { $lo.Save((Join-Path $assets "$name.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
        $lo.Dispose()
    }

    $icoPath = Join-Path $assets "$name.ico"
    $ofs = [System.IO.File]::Create($icoPath)
    $ow  = New-Object System.IO.BinaryWriter $ofs
    $ow.Write([uint16]0); $ow.Write([uint16]1); $ow.Write([uint16]$entries.Count)
    $offset = 6 + 16 * $entries.Count
    foreach ($e in $entries) {
        $dim = if ($e.Size -ge 256) { 0 } else { $e.Size }
        $ow.Write([byte]$dim); $ow.Write([byte]$dim); $ow.Write([byte]0); $ow.Write([byte]0)
        $ow.Write([uint16]1); $ow.Write([uint16]32)
        $ow.Write([uint32]$e.Data.Length); $ow.Write([uint32]$offset)
        $offset += $e.Data.Length
    }
    foreach ($e in $entries) { $ow.Write($e.Data, 0, $e.Data.Length) }
    $ow.Flush(); $ow.Dispose(); $ofs.Dispose()

    Write-Output ("wrote {0} ({1:N1} KB)" -f $icoPath, ((Get-Item $icoPath).Length / 1KB))
    $entries | ForEach-Object { Write-Output ("   {0,3}x{0,-3} {1,7} bytes  {2}" -f $_.Size, $_.Data.Length, $(if ($_.Png) { "PNG" } else { "BMP" })) }
    return (Get-FileHash $icoPath -Algorithm SHA256).Hash
}

$lit = @{
    C1   = @(62, 200, 255)     # cyan, top left
    C2   = @(28, 104, 222)     # blue, bottom right
    Wave = @(255, 255, 255)
    Rim  = $null
}

$unlit = @{
    C1   = @(58, 74, 96)       # dark LED, top left
    C2   = @(34, 45, 62)       # dark LED, bottom right
    Wave = @(170, 178, 194)
    Rim  = @(150, 162, 180)    # keeps the silhouette visible on a dark taskbar
}

Build-Icon "white_sound" $lit
Build-Icon "white_sound_off" $unlit
