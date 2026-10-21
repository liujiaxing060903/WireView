param(
    [string]$Source = (Join-Path $PSScriptRoot "..\windows\host\assets\ui\wireview-app-icon-source.png")
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$repositoryRoot = Split-Path $PSScriptRoot -Parent
$windowsAssetDirectory = Join-Path $repositoryRoot "windows\host\assets\ui"
$androidResourceDirectory = Join-Path $repositoryRoot "android\app\src\main\res"

if (-not (Test-Path -LiteralPath $Source)) {
    throw "找不到图标原图：$Source"
}

function Save-Png([System.Drawing.Bitmap]$bitmap, [string]$path) {
    $directory = Split-Path $path -Parent
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $bitmap.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
}

function Resize-Icon([System.Drawing.Bitmap]$source, [int]$size) {
    $result = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($result)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.DrawImage($source, 0, 0, $size, $size)
        return $result
    } finally {
        $graphics.Dispose()
    }
}

$sourceBitmap = [System.Drawing.Bitmap]::FromFile($Source)
try {
    # 原图是透明画布；先取实际非透明内容，避免应用图标上方留下大片空白。
    $left = $sourceBitmap.Width
    $top = $sourceBitmap.Height
    $right = -1
    $bottom = -1
    for ($y = 0; $y -lt $sourceBitmap.Height; $y++) {
        for ($x = 0; $x -lt $sourceBitmap.Width; $x++) {
            if ($sourceBitmap.GetPixel($x, $y).A -gt 8) {
                $left = [Math]::Min($left, $x)
                $top = [Math]::Min($top, $y)
                $right = [Math]::Max($right, $x)
                $bottom = [Math]::Max($bottom, $y)
            }
        }
    }
    if ($right -lt $left -or $bottom -lt $top) {
        throw "图标原图没有可见像素。"
    }

    $contentWidth = $right - $left + 1
    $contentHeight = $bottom - $top + 1
    $icon = [System.Drawing.Bitmap]::new(512, 512, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($icon)
    try {
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $graphics.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $padding = 30
        $scale = [Math]::Min((512 - $padding * 2) / $contentWidth, (512 - $padding * 2) / $contentHeight)
        $targetWidth = [int][Math]::Round($contentWidth * $scale)
        $targetHeight = [int][Math]::Round($contentHeight * $scale)
        $targetX = [int][Math]::Round((512 - $targetWidth) / 2)
        $targetY = [int][Math]::Round((512 - $targetHeight) / 2)
        $graphics.DrawImage($sourceBitmap,
            [System.Drawing.Rectangle]::new($targetX, $targetY, $targetWidth, $targetHeight),
            $left, $top, $contentWidth, $contentHeight,
            [System.Drawing.GraphicsUnit]::Pixel)
    } finally {
        $graphics.Dispose()
    }

    Save-Png $icon (Join-Path $windowsAssetDirectory "wireview-app-icon.png")

    $windowsSizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
    $images = @()
    try {
        foreach ($size in $windowsSizes) {
            $images += ,(Resize-Icon $icon $size)
        }
        $icoPath = Join-Path $windowsAssetDirectory "wireview-app-icon.ico"
        $stream = [System.IO.File]::Open($icoPath, [System.IO.FileMode]::Create)
        $writer = [System.IO.BinaryWriter]::new($stream)
        try {
            $writer.Write([UInt16]0)
            $writer.Write([UInt16]1)
            $writer.Write([UInt16]$images.Count)
            $pngData = @()
            foreach ($image in $images) {
                $pngStream = [System.IO.MemoryStream]::new()
                $image.Save($pngStream, [System.Drawing.Imaging.ImageFormat]::Png)
                $pngData += ,$pngStream.ToArray()
                $pngStream.Dispose()
            }
            $offset = 6 + 16 * $pngData.Count
            for ($index = 0; $index -lt $pngData.Count; $index++) {
                $size = $windowsSizes[$index]
                $dimension = if ($size -ge 256) { [byte]0 } else { [byte]$size }
                $writer.Write($dimension)
                $writer.Write($dimension)
                $writer.Write([byte]0)
                $writer.Write([byte]0)
                $writer.Write([UInt16]1)
                $writer.Write([UInt16]32)
                $writer.Write([UInt32]$pngData[$index].Length)
                $writer.Write([UInt32]$offset)
                $offset += $pngData[$index].Length
            }
            foreach ($data in $pngData) {
                $writer.Write($data)
            }
        } finally {
            $writer.Dispose()
            $stream.Dispose()
        }
    } finally {
        foreach ($image in $images) {
            $image.Dispose()
        }
    }

    $androidSizes = @{
        "mipmap-mdpi" = 48
        "mipmap-hdpi" = 72
        "mipmap-xhdpi" = 96
        "mipmap-xxhdpi" = 144
        "mipmap-xxxhdpi" = 192
    }
    foreach ($density in $androidSizes.Keys) {
        $image = Resize-Icon $icon $androidSizes[$density]
        try {
            Save-Png $image (Join-Path $androidResourceDirectory "$density\ic_wireview.png")
        } finally {
            $image.Dispose()
        }
    }
} finally {
    if ($null -ne $icon) {
        $icon.Dispose()
    }
    $sourceBitmap.Dispose()
}
