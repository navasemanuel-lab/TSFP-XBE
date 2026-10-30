# Runs the game and saves screenshots of the primary screen at the given seconds, then stops it.
#   tools\shots.ps1 -At 5,15,30 [-Keys @{20='{ENTER}'}]  keys: SendKeys strings sent at those seconds
param([int[]]$At = @(5, 15, 30), [hashtable]$Keys = @{}, [string]$OutDir = "$PSScriptRoot\..\.local\shots")
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null
Get-ChildItem $OutDir -Filter *.png | Remove-Item
$out = Join-Path $PSScriptRoot '..\out\build\release' | Resolve-Path
$p = Start-Process (Join-Path $out 'TSFP.exe') -WorkingDirectory $out -PassThru
$wsh = New-Object -ComObject WScript.Shell
$events = @()
foreach ($s in $At) { $events += [pscustomobject]@{ T = $s; Kind = 'shot'; V = $null } }
foreach ($k in $Keys.Keys) { $events += [pscustomobject]@{ T = [int]$k; Kind = 'key'; V = $Keys[$k] } }
$start = Get-Date
foreach ($e in ($events | Sort-Object T)) {
    while (((Get-Date) - $start).TotalSeconds -lt $e.T) { Start-Sleep -Milliseconds 100; if ($p.HasExited) { break } }
    if ($p.HasExited) { Write-Output "exited at $([int]((Get-Date) - $start).TotalSeconds)s"; break }
    if ($e.Kind -eq 'key') {
        $null = $wsh.AppActivate($p.Id); Start-Sleep -Milliseconds 100
        [System.Windows.Forms.SendKeys]::SendWait($e.V)
        continue
    }
    $null = $wsh.AppActivate($p.Id); Start-Sleep -Milliseconds 300
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
    $small = New-Object System.Drawing.Bitmap $bmp, 960, 540
    $file = Join-Path $OutDir ("shot_{0:D3}.png" -f $e.T)
    $small.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose(); $small.Dispose()
    Write-Output $file
}
if (-not $p.HasExited) { Stop-Process $p -Force }
