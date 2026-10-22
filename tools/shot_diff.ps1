param(
  [Parameter(Mandatory = $true)][string]$A,
  [Parameter(Mandatory = $true)][string]$B,
  [int]$Thresh = 24          # порог различия по каналу (0..255)
)
# Сравнивает два PNG: считает число отличающихся пикселей и их bbox.
# Используется как объективный тест «применилось ли значение».
Add-Type -AssemblyName System.Drawing

$ia = [System.Drawing.Bitmap]::FromFile($A)
$ib = [System.Drawing.Bitmap]::FromFile($B)
if ($ia.Width -ne $ib.Width -or $ia.Height -ne $ib.Height) {
    Write-Output ("SIZE MISMATCH {0}x{1} vs {2}x{3}" -f $ia.Width, $ia.Height, $ib.Width, $ib.Height)
    $ia.Dispose(); $ib.Dispose()
    exit 2
}
$w = $ia.Width; $h = $ia.Height
$minX = $w; $minY = $h; $maxX = -1; $maxY = -1; $diff = 0
for ($y = 0; $y -lt $h; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
        $pa = $ia.GetPixel($x, $y); $pb = $ib.GetPixel($x, $y)
        $d = [Math]::Abs($pa.R - $pb.R) + [Math]::Abs($pa.G - $pb.G) + [Math]::Abs($pa.B - $pb.B)
        if ($d -gt $Thresh) {
            $diff++
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
$ia.Dispose(); $ib.Dispose()
$total = $w * $h
$pct = [Math]::Round(100.0 * $diff / $total, 3)
if ($diff -eq 0) {
    Write-Output ("IDENTICAL  diff=0 ({0}x{1})" -f $w, $h)
    exit 1
}
Write-Output ("DIFFERENT  diff={0} px ({1}% of {2}x{3})  bbox=({4},{5})-({6},{7})  size={8}x{9}" -f `
    $diff, $pct, $w, $h, $minX, $minY, $maxX, $maxY, ($maxX - $minX + 1), ($maxY - $minY + 1))
exit 0
