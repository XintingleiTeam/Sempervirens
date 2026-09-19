param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\sempervirens.ico'),
    [string]$PreviewDirectory = (Join-Path $PSScriptRoot '..\build\icon-preview'),
    [string]$MasterPath = (Join-Path $PSScriptRoot '..\assets\branding\a2-icon.png')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# Work from the checked-in application icon master.
$source = [System.Drawing.Bitmap]::new([IO.Path]::GetFullPath($MasterPath))
try {
    $master = $source.Clone(
        [System.Drawing.Rectangle]::new(0, 0, $source.Width, $source.Height),
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
} finally {
    $source.Dispose()
}

function Resize-Icon([System.Drawing.Bitmap]$Source, [int]$Size) {
    $bitmap = [System.Drawing.Bitmap]::new($Size, $Size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CompositingMode = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.DrawImage($Source, [System.Drawing.Rectangle]::new(0, 0, $Size, $Size),
            0, 0, $Source.Width, $Source.Height, [System.Drawing.GraphicsUnit]::Pixel)
    } finally {
        $graphics.Dispose()
    }
    return $bitmap
}

$outputFullPath = [IO.Path]::GetFullPath($OutputPath)
$previewFullPath = [IO.Path]::GetFullPath($PreviewDirectory)
$masterFullPath = [IO.Path]::GetFullPath($MasterPath)
foreach ($directory in @([IO.Path]::GetDirectoryName($outputFullPath), $previewFullPath, [IO.Path]::GetDirectoryName($masterFullPath))) {
    [IO.Directory]::CreateDirectory($directory) | Out-Null
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$images = @()
try {
    # Installer-page theme derivative only. Preserve every alpha/edge pixel;
    # keep the application, title-bar, tray and shortcut artwork unchanged.
    $header = [System.Drawing.Bitmap]::new($master.Width, $master.Height,
        [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    try {
        for ($y = 0; $y -lt $master.Height; $y++) {
            for ($x = 0; $x -lt $master.Width; $x++) {
                $pixel = $master.GetPixel($x, $y)
                $luminance = 0.2126 * $pixel.R + 0.7152 * $pixel.G + 0.0722 * $pixel.B
                $coverage = [Math]::Max(0, [Math]::Min(1, ($luminance - 32) / 223))
                # Cool light-gray tile (#F1F3F5), charcoal mark (#252A30).
                $red = [int][Math]::Round(241 + (37 - 241) * $coverage)
                $green = [int][Math]::Round(243 + (42 - 243) * $coverage)
                $blue = [int][Math]::Round(245 + (48 - 245) * $coverage)
                $header.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($pixel.A, $red, $green, $blue))
            }
        }
        $header.Save((Join-Path ([IO.Path]::GetDirectoryName($masterFullPath)) 'installer-header-light.png'),
            [System.Drawing.Imaging.ImageFormat]::Png)
        foreach ($size in @(48, 64, 96)) {
            $preview = Resize-Icon $header $size
            try {
                if ($preview.GetPixel(0,0).A -ne 0) { throw 'Installer header lost its transparent rounded corners.' }
                $preview.Save((Join-Path $previewFullPath "installer-header-light-$size.png"),
                    [System.Drawing.Imaging.ImageFormat]::Png)
            } finally { $preview.Dispose() }
        }
    } finally { $header.Dispose() }
    foreach ($size in $sizes) {
        $bitmap = Resize-Icon $master $size
        try {
            foreach ($point in @(@(0,0), @(($size - 1),0), @(0,($size - 1)), @(($size - 1),($size - 1)))) {
                if ($bitmap.GetPixel($point[0], $point[1]).A -ne 0) { throw "Opaque corner in $size px icon." }
            }
            $stream = [IO.MemoryStream]::new()
            try {
                $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
                $images += [pscustomobject]@{ Size = $size; Bytes = $stream.ToArray() }
            } finally { $stream.Dispose() }
            $bitmap.Save((Join-Path $previewFullPath "icon-$size.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $bitmap.Dispose() }
    }
} finally { $master.Dispose() }

$writer = [IO.BinaryWriter]::new([IO.File]::Create($outputFullPath))
try {
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]$images.Count)
    $offset = 6 + 16 * $images.Count
    foreach ($image in $images) {
        $dimension = if ($image.Size -eq 256) { 0 } else { $image.Size }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([UInt16]1)
        $writer.Write([UInt16]32)
        $writer.Write([UInt32]$image.Bytes.Length)
        $writer.Write([UInt32]$offset)
        $offset += $image.Bytes.Length
    }
    foreach ($image in $images) { $writer.Write([byte[]]$image.Bytes) }
} finally { $writer.Dispose() }

Write-Host 'Generated all ten icon sizes with transparent corners.'
Write-Host "Generated $outputFullPath"

# Large installer artwork uses the original full wordmark, not the small A2 icon.
$welcome = [System.Drawing.Bitmap]::new(492, 942, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$layout = [System.Drawing.Graphics]::FromImage($welcome)
$logo = [System.Drawing.Bitmap]::new((Join-Path ([IO.Path]::GetDirectoryName($masterFullPath)) 'wordmark-source.png'))
# Use the supplied black silhouette directly on the light installer panel.
$separator = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(229,231,235))
try {
    $layout.Clear([System.Drawing.Color]::FromArgb(248,249,250))
    $layout.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $layout.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $layout.DrawImage($logo, [System.Drawing.Rectangle]::new(54,377,384,187),
        142, 142, 1765, 861, [System.Drawing.GraphicsUnit]::Pixel)
    $layout.FillRectangle($separator, 489, 0, 3, 942)
    $welcome.Save((Join-Path ([IO.Path]::GetDirectoryName($masterFullPath)) 'installer-welcome.png'), [System.Drawing.Imaging.ImageFormat]::Png)
} finally {
    $separator.Dispose(); $logo.Dispose(); $layout.Dispose(); $welcome.Dispose()
}
