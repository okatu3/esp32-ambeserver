# Renders icon.html (256 x 256) with Edge and wraps the PNG into m17_pc.ico (one 256 px PNG image; Windows scales it).
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$png = Join-Path $env:TEMP "m17_pc_icon.png"
$prof = Join-Path $env:TEMP "m17_pc_icon_edge"
$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
& $edge --headless=new --disable-gpu --hide-scrollbars --user-data-dir="$prof" --force-device-scale-factor=1 `
    --default-background-color=00000000 --window-size=256,256 --screenshot="$png" "file:///$($here -replace '\\','/')/icon.html" 2>$null | Out-Null
Start-Sleep -Seconds 2
$data = [IO.File]::ReadAllBytes($png)
$ms = New-Object IO.MemoryStream
$w = New-Object IO.BinaryWriter($ms)
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]1)          # ICONDIR: reserved, type 1, one image
$w.Write([Byte]0); $w.Write([Byte]0); $w.Write([Byte]0); $w.Write([Byte]0)   # 256 x 256 (0 = 256), no palette
$w.Write([UInt16]1); $w.Write([UInt16]32)                               # planes, bits per pixel
$w.Write([UInt32]$data.Length); $w.Write([UInt32]22)                    # size, offset
$w.Write($data)
[IO.File]::WriteAllBytes((Join-Path $here "m17_pc.ico"), $ms.ToArray())
Copy-Item $png (Join-Path $here "m17_pc_icon.png") -Force
Write-Output "m17_pc.ico: $($data.Length) bytes of PNG"
