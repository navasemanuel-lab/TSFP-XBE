# Runs the built game for a while, then prints the log's tail and whether it was still running.
param([int]$Seconds = 40, [int]$Lines = 60)
$out = Join-Path $PSScriptRoot '..\out\build\release' | Resolve-Path
$log = Join-Path $out 'logs\tsfp.log'
$p = Start-Process (Join-Path $out 'TSFP.exe') -WorkingDirectory $out -PassThru
$deadline = (Get-Date).AddSeconds($Seconds)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
$alive = -not $p.HasExited
if ($alive) { Stop-Process $p -Force }
Write-Output "running at the end: $alive"
Get-Content $log -Tail $Lines
