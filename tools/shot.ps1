param(
  [string]$Out = "C:\Users\sfx\AppData\Local\Temp\opencode\shot.png",
  [int]$Height = 120,   # сколько пикселей снизу экрана снять
  [int]$X = 0,          # X начала кропа (в пикселях экрана), -1 = от правого края
  [int]$W = 0           # ширина кропа, 0 = до правого края
)
# Снимок нижней полосы экрана (панель задач) для визуальной проверки.
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$full = New-Object System.Drawing.Bitmap($vs.Width, $vs.Height)
$g = [System.Drawing.Graphics]::FromImage($full)
$g.CopyFromScreen($vs.Left, $vs.Top, 0, 0, $full.Size)
$g.Dispose()

$y = [Math]::Max(0, $vs.Height - $Height)
if ($X -lt 0) { $X = $vs.Width + $X } elseif ($X -eq 0) { $X = $vs.Left }
$cropW = if ($W -gt 0) { $W } else { $vs.Left + $vs.Width - $X }
$cropW = [Math]::Min($cropW, $vs.Left + $vs.Width - $X)
$cropH = [Math]::Min($Height, $vs.Height - $y)

$rect = New-Object System.Drawing.Rectangle($X, $y, $cropW, $cropH)
$crop = $full.Clone($rect, $full.PixelFormat)
$crop.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$crop.Dispose()
$full.Dispose()
Write-Output "saved $Out ($cropW x $cropH) from ($X,$y)"
