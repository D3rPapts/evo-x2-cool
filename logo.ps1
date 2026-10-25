# Draws the evo-x2-cool logo (fan inside a temperature arc) and packs it into evo.ico, plus evo-heiss.ico with a red
# background that the tray icon shows while it is too hot or the emergency is active.
# Every size is drawn on its own so the small icons stay sharp. Run: powershell -File logo.ps1
Add-Type -AssemblyName System.Drawing
foreach ($v in @{ name = 'evo'; sizes = 16, 24, 32, 48, 64; hot = 0 }, @{ name = 'evo-heiss'; sizes = 16, 20, 24, 32; hot = 1 }) {
$sizes = $v.sizes
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-Object Drawing.Bitmap $s, $s
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([Drawing.Color]::Transparent)
    $f = $s / 256.0

    # background: rounded dark square
    $r = 56 * $f; $p = New-Object Drawing.Drawing2D.GraphicsPath
    $p.AddArc(0, 0, 2*$r, 2*$r, 180, 90); $p.AddArc($s-2*$r, 0, 2*$r, 2*$r, 270, 90)
    $p.AddArc($s-2*$r, $s-2*$r, 2*$r, 2*$r, 0, 90); $p.AddArc(0, $s-2*$r, 2*$r, 2*$r, 90, 90); $p.CloseFigure()
    $bg = New-Object Drawing.Drawing2D.LinearGradientBrush ([Drawing.PointF]::new(0,0)), ([Drawing.PointF]::new(0,$s)),
        ([Drawing.Color]::FromArgb(255, (38,200)[$v.hot], (48,38)[$v.hot], (66,38)[$v.hot])), ([Drawing.Color]::FromArgb(255, (17,90)[$v.hot], (22,12)[$v.hot], (32,12)[$v.hot]))
    $g.FillPath($bg, $p)

    # temperature arc: cool teal at the bottom left, hot red at the bottom right
    $w = [Math]::Max(1.6, 26 * $f); $m = 34 * $f
    $rect = New-Object Drawing.RectangleF $m, $m, ($s - 2*$m), ($s - 2*$m)
    $cols = @(@(45,212,191), @(250,204,21), @(249,115,22), @(239,68,68))
    $n = 36
    for ($i = 0; $i -lt $n; $i++) {
        $t = $i / ($n - 1); $seg = [Math]::Min(2, [int][Math]::Floor($t * 3)); $u = $t * 3 - $seg
        $a = $cols[$seg]; $b = $cols[$seg + 1]
        $c = [Drawing.Color]::FromArgb(255, [int]($a[0]+($b[0]-$a[0])*$u), [int]($a[1]+($b[1]-$a[1])*$u), [int]($a[2]+($b[2]-$a[2])*$u))
        $pen = New-Object Drawing.Pen $c, $w
        $pen.StartCap = if ($i -eq 0) { 'Round' } else { 'Flat' }
        $pen.EndCap = if ($i -eq $n - 1) { 'Round' } else { 'Flat' }
        $g.DrawArc($pen, $rect, 135 + 270 * $i / $n, 270 / $n + 0.8)
    }

    # fan: four curved blades and a hub
    $cx = $s / 2; $cy = $s / 2
    $blade = [Drawing.Color]::FromArgb(255, 226, 232, 240)
    $br = New-Object Drawing.SolidBrush $blade
    for ($k = 0; $k -lt 4; $k++) {
        $st = $g.Save()
        $g.TranslateTransform($cx, $cy); $g.RotateTransform(90 * $k + 20)
        $bp = New-Object Drawing.Drawing2D.GraphicsPath
        $bp.AddBezier(0, 0, (30*$f), (-10*$f), (62*$f), (-30*$f), (66*$f), (-6*$f))
        $bp.AddBezier((66*$f), (-6*$f), (56*$f), (16*$f), (24*$f), (20*$f), 0, 0)
        $g.FillPath($br, $bp)
        $g.Restore($st)
    }
    $hr = [Math]::Max(1.5, 15 * $f)
    $g.FillEllipse((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255,249,115,22))), $cx-$hr, $cy-$hr, 2*$hr, 2*$hr)

    $g.Dispose()
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    if ($env:EVO_LOGO_DIR) { $bmp.Save("$env:EVO_LOGO_DIR\logo-$s.png", [Drawing.Imaging.ImageFormat]::Png) }
    if ($s -eq 64 -and !$v.hot) { $bmp.Save("$PSScriptRoot\logo-64.png", [Drawing.Imaging.ImageFormat]::Png) }
    $pngs += ,$ms.ToArray()
    $bmp.Dispose()
}

# ICO container with PNG entries (supported since Windows Vista)
$out = New-Object IO.MemoryStream; $bw = New-Object IO.BinaryWriter $out
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$off = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $d = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $bw.Write([byte]$d); $bw.Write([byte]$d); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$off)
    $off += $pngs[$i].Length
}
foreach ($b in $pngs) { $bw.Write($b) }
[IO.File]::WriteAllBytes("$PSScriptRoot\$($v.name).ico", $out.ToArray())
}
