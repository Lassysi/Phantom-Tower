# Put the kept seed-20261112 run (display floor 48, outside) back after trial batches. Game must be closed. ASCII-only.
#   powershell -NoProfile -File E:\PBZ-claude\mods\tower\tools\trial-restore.ps1
$ErrorActionPreference = 'Stop'
if (Get-Process PBZ -ErrorAction SilentlyContinue) { throw 'game is running; close it first' }
$keep = 'E:\Phantom_Blade_Zero\build\keep-seed20261112-f48'
$tower = Join-Path $env:LOCALAPPDATA 'PBZTower'
if (-not (Test-Path "$keep\run.txt")) { throw "nothing kept in $keep" }
$trial = "E:\Phantom_Blade_Zero\build\trial-state-" + (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force $trial | Out-Null
Get-ChildItem $tower -File | Copy-Item -Destination $trial -Force
Get-ChildItem $keep -File | Where-Object Name -ne 'sha256.csv' | Copy-Item -Destination $tower -Force
$bad = @(Import-Csv "$keep\sha256.csv" | Where-Object { (Get-FileHash (Join-Path $tower (Split-Path $_.Path -Leaf))).Hash -ne $_.Hash })
"trial state saved to $trial"
"restored=" + ($bad.Count -eq 0)
