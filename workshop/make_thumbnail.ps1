# Archon Mode - draws workshop/archon_mode.png (512x512), original art with no game assets.
# Usage: pwsh -NoProfile -File workshop/make_thumbnail.ps1
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$out  = Join-Path $PSScriptRoot 'archon_mode.png'
$size = 512
$bmp  = New-Object System.Drawing.Bitmap $size, $size
$g    = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode     = 'AntiAlias'
$g.TextRenderingHint = 'AntiAliasGridFit'
$g.PixelOffsetMode   = 'HighQuality'

function C($hex) { [System.Drawing.ColorTranslator]::FromHtml($hex) }
function P([double]$x, [double]$y) { New-Object System.Drawing.PointF ([float]$x), ([float]$y) }

# background: oxblood glow behind the figures, near black at the edges
$g.Clear((C '#0c0708'))
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
$path.AddEllipse(-110, -130, 732, 732)
$glow = New-Object System.Drawing.Drawing2D.PathGradientBrush $path
$glow.CenterPoint    = P 256 215
$glow.CenterColor    = C '#7a1620'
$glow.SurroundColors = [System.Drawing.Color[]]@((C '#0c0708'))
$g.FillRectangle($glow, 0, 0, $size, $size)

# frame
$gold = C '#d9b24c'
$g.DrawRectangle((New-Object System.Drawing.Pen $gold, 5), 14, 14, 483, 483)
$g.DrawRectangle((New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(130, $gold)), 1.5), 24, 24, 463, 463)

# the shared banner both figures stand on
$banner = [System.Drawing.PointF[]]@((P 118 84), (P 394 84), (P 394 296), (P 256 352), (P 118 296))
$g.FillPolygon((New-Object System.Drawing.SolidBrush (C '#2a0d12')), $banner)
$g.DrawPolygon((New-Object System.Drawing.Pen $gold, 4), $banner)

# one commander: crown, head, shoulders
function Figure([double]$cx, [double]$top, [double]$s, $fill, $edge) {
  $brush = New-Object System.Drawing.SolidBrush $fill
  $pen   = New-Object System.Drawing.Pen $edge, 3
  $crown = [System.Drawing.PointF[]]@(
    (P ($cx - 34 * $s) ($top + 42 * $s)), (P ($cx - 40 * $s) ($top + 2 * $s)), (P ($cx - 19 * $s) ($top + 24 * $s)), (P $cx $top),
    (P ($cx + 19 * $s) ($top + 24 * $s)), (P ($cx + 40 * $s) ($top + 2 * $s)), (P ($cx + 34 * $s) ($top + 42 * $s)))
  $body = New-Object System.Drawing.Drawing2D.GraphicsPath
  $body.AddBezier((P ($cx - 74 * $s) ($top + 176 * $s)), (P ($cx - 70 * $s) ($top + 124 * $s)), (P ($cx - 42 * $s) ($top + 112 * $s)), (P $cx ($top + 112 * $s)))
  $body.AddBezier((P $cx ($top + 112 * $s)), (P ($cx + 42 * $s) ($top + 112 * $s)), (P ($cx + 70 * $s) ($top + 124 * $s)), (P ($cx + 74 * $s) ($top + 176 * $s)))
  $body.CloseFigure()
  $g.FillPath($brush, $body); $g.DrawPath($pen, $body)
  $d = 74 * $s
  $g.FillEllipse($brush, [float]($cx - $d / 2), [float]($top + 40 * $s), [float]$d, [float]$d)
  $g.DrawEllipse($pen, [float]($cx - $d / 2), [float]($top + 40 * $s), [float]$d, [float]$d)
  $g.FillPolygon($brush, $crown); $g.DrawPolygon($pen, $crown)
}
Figure 206 110 0.98 (C '#b9c2cc') (C '#1a0a0d')
Figure 306 110 0.98 $gold (C '#1a0a0d')

# title
$fmt = New-Object System.Drawing.StringFormat
$fmt.Alignment = 'Center'
$title = New-Object System.Drawing.Font 'Georgia', 66, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
$box   = New-Object System.Drawing.RectangleF 0, 374, $size, 80
$g.DrawString('ARCHON', $title, (New-Object System.Drawing.SolidBrush (C '#000000')), (New-Object System.Drawing.RectangleF 3, 377, $size, 80), $fmt)
$g.DrawString('ARCHON', $title, (New-Object System.Drawing.SolidBrush $gold), $box, $fmt)

$g.Dispose()
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "wrote $out ($((Get-Item $out).Length) bytes)"
