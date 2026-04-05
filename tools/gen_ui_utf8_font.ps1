Add-Type -AssemblyName System.Drawing

$font = New-Object System.Drawing.Font("Tahoma", 18, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$lineHeight = [int][Math]::Ceiling($font.GetHeight()) + 2
$thaiAnchor = [string][char]0x0E01

$codepoints = New-Object System.Collections.Generic.List[int]
for ($cp = 0x20; $cp -le 0x7E; $cp++) { [void]$codepoints.Add($cp) }
for ($cp = 0x0E01; $cp -le 0x0E5B; $cp++) { [void]$codepoints.Add($cp) }

$aboveMarks = @(0x0E31,0x0E34,0x0E35,0x0E36,0x0E37,0x0E47,0x0E48,0x0E49,0x0E4A,0x0E4B,0x0E4C,0x0E4D,0x0E4E)
$belowMarks = @(0x0E38,0x0E39,0x0E3A)

$outPath = Join-Path $PSScriptRoot "..\main\ui_utf8_font_data.h"
$sb = New-Object System.Text.StringBuilder
$glyphSpecs = @{}

function New-RenderBitmap([string]$text, $font) {
    $bmp = New-Object System.Drawing.Bitmap 48, 48
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::White)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::SingleBitPerPixelGridFit
    $g.DrawString($text, $font, [System.Drawing.Brushes]::Black, 0, 0)
    $g.Dispose()
    return $bmp
}

function Get-PixelBounds($bmp, $baseBmp = $null) {
    $minX = 48
    $minY = 48
    $maxX = -1
    $maxY = -1

    for ($y = 0; $y -lt 48; $y++) {
        for ($x = 0; $x -lt 48; $x++) {
            $pixel = $bmp.GetPixel($x, $y)
            $isOn = ($pixel.R -lt 200 -or $pixel.G -lt 200 -or $pixel.B -lt 200)
            if ($baseBmp) {
                $basePixel = $baseBmp.GetPixel($x, $y)
                $baseOn = ($basePixel.R -lt 200 -or $basePixel.G -lt 200 -or $basePixel.B -lt 200)
                $isOn = $isOn -and (-not $baseOn)
            }
            if ($isOn) {
                if ($x -lt $minX) { $minX = $x }
                if ($y -lt $minY) { $minY = $y }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -gt $maxY) { $maxY = $y }
            }
        }
    }

    return @{
        MinX = $minX
        MinY = $minY
        MaxX = $maxX
        MaxY = $maxY
    }
}

function Get-PackedBitmap($bmp, $bounds, $baseBmp = $null) {
    $packed = New-Object System.Collections.Generic.List[byte]
    $bitCount = 0
    $current = 0

    for ($y = $bounds.MinY; $y -le $bounds.MaxY; $y++) {
        for ($x = $bounds.MinX; $x -le $bounds.MaxX; $x++) {
            $pixel = $bmp.GetPixel($x, $y)
            $isOn = ($pixel.R -lt 200 -or $pixel.G -lt 200 -or $pixel.B -lt 200)
            if ($baseBmp) {
                $basePixel = $baseBmp.GetPixel($x, $y)
                $baseOn = ($basePixel.R -lt 200 -or $basePixel.G -lt 200 -or $basePixel.B -lt 200)
                $isOn = $isOn -and (-not $baseOn)
            }

            $current = $current -shl 1
            if ($isOn) { $current = $current -bor 1 }
            $bitCount++
            if ($bitCount -eq 8) {
                [void]$packed.Add([byte]$current)
                $bitCount = 0
                $current = 0
            }
        }
    }

    if ($bitCount -gt 0) {
        $current = $current -shl (8 - $bitCount)
        [void]$packed.Add([byte]$current)
    }

    return $packed
}

[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <stdint.h>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("typedef struct {")
[void]$sb.AppendLine("    uint32_t codepoint;")
[void]$sb.AppendLine("    uint8_t width;")
[void]$sb.AppendLine("    uint8_t height;")
[void]$sb.AppendLine("    uint8_t advance;")
[void]$sb.AppendLine("    int8_t x_offset;")
[void]$sb.AppendLine("    int8_t y_offset;")
[void]$sb.AppendLine("    const uint8_t *bitmap;")
[void]$sb.AppendLine("} ui_utf8_font_glyph_t;")
[void]$sb.AppendLine("")

foreach ($cp in $codepoints) {
    $glyphName = ("g_{0:X4}" -f $cp)
    $isCombining = ($aboveMarks -contains $cp) -or ($belowMarks -contains $cp)

    if ($isCombining) {
        $bmp = New-RenderBitmap ($thaiAnchor + [string][char]$cp) $font
        $baseBmp = New-RenderBitmap $thaiAnchor $font
        $bounds = Get-PixelBounds $bmp $baseBmp
    } else {
        $bmp = New-RenderBitmap ([string][char]$cp) $font
        $baseBmp = $null
        $bounds = Get-PixelBounds $bmp
    }

    if ($bounds.MaxX -lt $bounds.MinX -or $bounds.MaxY -lt $bounds.MinY) {
        $emptyAdvance = 0
        if ($cp -eq 0x20) { $emptyAdvance = 8 }
        $glyphSpecs[$cp] = @{
            Width = 1; Height = 1; Advance = $emptyAdvance; XOffset = 0; YOffset = 0; Bytes = "0x00"
        }
        [void]$sb.AppendLine("static const uint8_t ${glyphName}[] = {0x00};")
        [void]$sb.AppendLine("")
        if ($baseBmp) { $baseBmp.Dispose() }
        $bmp.Dispose()
        continue
    }

    $width = $bounds.MaxX - $bounds.MinX + 1
    $height = $bounds.MaxY - $bounds.MinY + 1
    $packed = Get-PackedBitmap $bmp $bounds $baseBmp

    $measureBmp = New-Object System.Drawing.Bitmap 1, 1
    $measureGraphics = [System.Drawing.Graphics]::FromImage($measureBmp)
    $size = [System.Drawing.SizeF]$measureGraphics.MeasureString([string][char]$cp, $font, 256, [System.Drawing.StringFormat]::GenericTypographic)
    $measureGraphics.Dispose()
    $measureBmp.Dispose()

    $advance = [int][Math]::Ceiling($size.Width)
    if ($advance -lt ($width + 1) -and -not $isCombining) { $advance = $width + 1 }
    if ($advance -gt 31) { $advance = 31 }
    if ($isCombining) { $advance = 0 }
    if ($cp -eq 0x20) { $advance = 8 }

    $bytes = ($packed | ForEach-Object { "0x{0:X2}" -f $_ }) -join ", "
    $glyphSpecs[$cp] = @{
        Width = $width
        Height = $height
        Advance = $advance
        XOffset = $bounds.MinX
        YOffset = $bounds.MinY
        Bytes = $bytes
    }

    [void]$sb.AppendLine("static const uint8_t ${glyphName}[] = {$bytes};")
    [void]$sb.AppendLine("")

    if ($baseBmp) { $baseBmp.Dispose() }
    $bmp.Dispose()
}

[void]$sb.AppendLine("static const ui_utf8_font_glyph_t kUiUtf8FontGlyphs[] = {")
foreach ($cp in $codepoints) {
    $glyphName = ("g_{0:X4}" -f $cp)
    $spec = $glyphSpecs[$cp]
    [void]$sb.AppendLine(("    {{0x{0:X4}, {1}, {2}, {3}, {4}, {5}, {6}}}," -f $cp, $spec.Width, $spec.Height, $spec.Advance, $spec.XOffset, $spec.YOffset, $glyphName))
}
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine(("static const uint16_t kUiUtf8FontGlyphCount = {0};" -f $codepoints.Count))
[void]$sb.AppendLine(("static const uint8_t kUiUtf8FontLineHeight = {0};" -f $lineHeight))

Set-Content -Path $outPath -Value $sb.ToString() -Encoding UTF8
