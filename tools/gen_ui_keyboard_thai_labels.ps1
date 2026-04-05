Add-Type -AssemblyName System.Drawing

function Convert-ToRgb565($color) {
    $r = ($color.R -band 0xF8) -shl 8
    $g = ($color.G -band 0xFC) -shl 3
    $b = ($color.B -shr 3)
    return ($r -bor $g -bor $b)
}

$items = @(
    @{ Name = "kThKbTitle";  Text = ([string]([char]0x0E15)+[char]0x0E31+[char]0x0E49+[char]0x0E07+[char]0x0E0A+[char]0x0E37+[char]0x0E48+[char]0x0E2D+[char]0x0E22+[char]0x0E32); Bg = [System.Drawing.Color]::FromArgb(15,23,42); Fg = [System.Drawing.Color]::White; FontSize = 20 },
    @{ Name = "kThKbCancel"; Text = ([string]([char]0x0E22)+[char]0x0E01+[char]0x0E40+[char]0x0E25+[char]0x0E34+[char]0x0E01); Bg = [System.Drawing.Color]::FromArgb(244,63,94); Fg = [System.Drawing.Color]::White; FontSize = 16 },
    @{ Name = "kThKbSave";   Text = ([string]([char]0x0E1A)+[char]0x0E31+[char]0x0E19+[char]0x0E17+[char]0x0E36+[char]0x0E01); Bg = [System.Drawing.Color]::FromArgb(16,185,129); Fg = [System.Drawing.Color]::White; FontSize = 16 },
    @{ Name = "kThKbDelete"; Text = ([string]([char]0x0E25)+[char]0x0E1A); Bg = [System.Drawing.Color]::FromArgb(244,63,94); Fg = [System.Drawing.Color]::White; FontSize = 16 },
    @{ Name = "kThKbSpace";  Text = ([string]([char]0x0E40)+[char]0x0E27+[char]0x0E49+[char]0x0E19+[char]0x0E27+[char]0x0E23+[char]0x0E23+[char]0x0E04); Bg = [System.Drawing.Color]::FromArgb(30,58,138); Fg = [System.Drawing.Color]::White; FontSize = 16 }
)

$outPath = Join-Path $PSScriptRoot '..\main\ui_keyboard_thai_labels.h'
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine('#include "ui_bitmap_label.h"')

foreach ($item in $items) {
    $font = New-Object System.Drawing.Font("Tahoma", $item.FontSize, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $tmp = New-Object System.Drawing.Bitmap 256, 80
    $g0 = [System.Drawing.Graphics]::FromImage($tmp)
    $g0.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $size = $g0.MeasureString($item.Text, $font, 256, [System.Drawing.StringFormat]::GenericTypographic)
    $g0.Dispose()
    $tmp.Dispose()

    $w = [Math]::Max(8, [int][Math]::Ceiling($size.Width) + 6)
    $h = [Math]::Max(8, [int][Math]::Ceiling($size.Height) + 4)
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear($item.Bg)
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $fmt = [System.Drawing.StringFormat]::GenericTypographic
    $g.DrawString($item.Text, $font, (New-Object System.Drawing.SolidBrush($item.Fg)), 1, 0, $fmt)

    [void]$sb.AppendLine("")
    [void]$sb.AppendLine(("static const uint16_t {0}_pixels[{1}] = {{" -f $item.Name, ($w * $h)))
    for ($y = 0; $y -lt $h; $y++) {
        $line = New-Object System.Collections.Generic.List[string]
        for ($x = 0; $x -lt $w; $x++) {
            $px = $bmp.GetPixel($x, $y)
            $line.Add(("0x{0:X4}" -f (Convert-ToRgb565 $px)))
        }
        [void]$sb.AppendLine(("    {0}," -f ($line -join ", ")))
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine(("static const ui_label_bitmap_t {0} = {{ {1}, {2}, {0}_pixels }};" -f $item.Name, $w, $h))

    $g.Dispose()
    $bmp.Dispose()
}

Set-Content -Path $outPath -Value $sb.ToString() -Encoding UTF8
