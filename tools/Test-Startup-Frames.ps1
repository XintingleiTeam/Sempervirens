$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$project = Split-Path $PSScriptRoot -Parent
$names = 'start','mid','orbit','before-arrow','end'
$frames = @()
try {
    foreach ($name in $names) {
        $frames += [Drawing.Bitmap]::new((Join-Path $project ("build\ui-splash-$name.png")))
    }
    # Fully visible lettering must be pixel-identical throughout the remaining
    # timeline: catches accidental movement, scaling, rotation or moving light.
    for ($frame = 2; $frame -lt $frames.Count; $frame++) {
        for ($y = 86; $y -lt 141; $y++) {
            for ($x = 198; $x -lt 537; $x++) {
                if ($frames[1].GetPixel($x,$y).ToArgb() -ne $frames[$frame].GetPixel($x,$y).ToArgb()) {
                    throw "Stationary wordmark changed in frame $frame at ($x,$y)."
                }
            }
        }
    }
    function Count-Ink([Drawing.Bitmap]$Bitmap, [int]$Left, [int]$Top, [int]$Right, [int]$Bottom) {
        $count = 0
        for ($y=$Top; $y -lt $Bottom; $y++) {
            for ($x=$Left; $x -lt $Right; $x++) {
                if ($Bitmap.GetPixel($x,$y).R -gt 100) { $count++ }
            }
        }
        return $count
    }
    $lower = @($frames | ForEach-Object { Count-Ink $_ 230 149 458 220 })
    $upper = @($frames | ForEach-Object { Count-Ink $_ 399 15 500 78 })
    if ($lower[0] -ne 0 -or $lower[1] -le 0 -or $lower[2] -le $lower[1] -or
        $lower[2] -ne $lower[3] -or $lower[3] -ne $lower[4]) { throw 'Lower orbit reveal order is incorrect.' }
    if ($upper[0] -ne 0 -or $upper[1] -ne 0 -or $upper[2] -le 0 -or
        $upper[3] -le $upper[2] -or $upper[3] -ne $upper[4]) { throw 'Upper orbit reveal order is incorrect.' }
    $beforeArrow = Count-Ink $frames[3] 380 41 390 51
    $afterArrow = Count-Ink $frames[4] 380 41 390 51
    if ($beforeArrow -ne 0 -or $afterArrow -le 5) { throw 'Arrow must appear only after the complete orbit.' }
    Write-Output 'Startup frames passed: fixed lettering, sequential orbit, arrow last.'
} finally {
    foreach ($frame in $frames) { $frame.Dispose() }
}
