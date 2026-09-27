param(
    [string]$Cjpeg = "C:\msys64\ucrt64\bin\cjpeg.exe"
)

$ErrorActionPreference = "Stop"
$fixtureRoot = Join-Path $PSScriptRoot "fixtures"
$pattern = Join-Path $fixtureRoot "pattern.ppm"
$gray = Join-Path $fixtureRoot "gray.pgm"

if (-not (Test-Path -LiteralPath $Cjpeg)) {
    throw "cjpeg not found: $Cjpeg"
}

& $Cjpeg -quality 85 -sample "2x2,1x1,1x1" -outfile (Join-Path $fixtureRoot "baseline_420.jpg") $pattern
& $Cjpeg -quality 85 -sample "2x1,1x1,1x1" -outfile (Join-Path $fixtureRoot "baseline_422.jpg") $pattern
& $Cjpeg -quality 85 -sample "1x1,1x1,1x1" -outfile (Join-Path $fixtureRoot "baseline_444.jpg") $pattern
& $Cjpeg -quality 85 -grayscale -outfile (Join-Path $fixtureRoot "baseline_gray.jpg") $gray
& $Cjpeg -quality 85 -sample "1x1,1x1,1x1" -restart 1B -outfile (Join-Path $fixtureRoot "baseline_restart.jpg") $pattern
& $Cjpeg -quality 85 -progressive -outfile (Join-Path $fixtureRoot "progressive_app0.jpg") $pattern

Add-Type -AssemblyName System.Drawing
$bitmap = [System.Drawing.Bitmap]::new(16, 16,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
try {
    for ($y = 0; $y -lt 16; ++$y) {
        for ($x = 0; $x -lt 16; ++$x) {
            $alpha = 32 + (($x + $y) * 7)
            if ($alpha -gt 255) { $alpha = 255 }
            $bitmap.SetPixel($x, $y,
                [System.Drawing.Color]::FromArgb($alpha, $x * 16, $y * 16, 180))
        }
    }
    $bitmap.Save((Join-Path $fixtureRoot "alpha.png"),
        [System.Drawing.Imaging.ImageFormat]::Png)
}
finally {
    $bitmap.Dispose()
}

# A display-sized progressive/JFIF object exercises the same SOF2 rejection
# path as the reported ~10 KiB broadcast object without embedding station data.
$largeBitmap = [System.Drawing.Bitmap]::new(320, 240,
    [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$temporaryBmp = [System.IO.Path]::GetTempFileName()
try {
    for ($y = 0; $y -lt 240; ++$y) {
        for ($x = 0; $x -lt 320; ++$x) {
            $checker = (([int]($x / 12) + [int]($y / 12)) % 2) * 72
            $red = ($x * 3 + $y + $checker) % 256
            $green = ($x + $y * 2 + 40) % 256
            $blue = ($x * 2 + $y * 5 + 90 - $checker) % 256
            $largeBitmap.SetPixel($x, $y,
                [System.Drawing.Color]::FromArgb($red, $green, $blue))
        }
    }
    $largeBitmap.Save($temporaryBmp, [System.Drawing.Imaging.ImageFormat]::Bmp)
    & $Cjpeg -quality 40 -sample "2x2,1x1,1x1" -outfile `
        (Join-Path $fixtureRoot "baseline_320x240_420.jpg") $temporaryBmp
    & $Cjpeg -quality 40 -progressive -outfile `
        (Join-Path $fixtureRoot "progressive_320x240_app0.jpg") $temporaryBmp
}
finally {
    $largeBitmap.Dispose()
    Remove-Item -LiteralPath $temporaryBmp -Force -ErrorAction SilentlyContinue
}
