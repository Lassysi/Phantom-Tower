# Start a QA trial batch (audit plan) from mods\tower\data\trial_batches_20261112.json. Keep this file ASCII-only.
#   & E:\PBZ-claude\mods\tower\tools\trial-start.ps1 -Batch B
# Needs the PBZ_HEX_TEST build, the player inside a loaded save, tower outside (no run in the tower save).
# Starting a run replaces %LOCALAPPDATA%\PBZTower\run.txt: the seed-20261112 run must already be kept in
# E:\Phantom_Blade_Zero\build\keep-seed20261112-f48 (trial-restore.ps1 puts it back with the game closed).
param([Parameter(Mandatory)][ValidateSet('A','B','C','E','F','G','H')][string]$Batch, [int]$Seconds = 8)
$ErrorActionPreference = 'Stop'
$keep = 'E:\Phantom_Blade_Zero\build\keep-seed20261112-f48'
if (-not (Test-Path "$keep\run.txt") -or -not (Test-Path "$keep\growth.txt")) { throw "seed run not kept in $keep; refuse to overwrite it" }
$data = Get-Content -LiteralPath 'E:\PBZ-claude\mods\tower\data\trial_batches_20261112.json' -Raw -Encoding UTF8 | ConvertFrom-Json
$b = $data.batches.$Batch
$req = @{ op='tower'; cmd='start'; plan=$b.plan; pool='audit'; seed=[string](20261100 + [int][char]$Batch) } | ConvertTo-Json -Compress
$p = New-Object System.IO.Pipes.NamedPipeClientStream(".", "PBZLab", [System.IO.Pipes.PipeDirection]::InOut)
$p.Connect(3000)
Start-Sleep -Milliseconds 800
$bytes = [System.Text.Encoding]::UTF8.GetBytes($req + "`n")
$p.Write($bytes, 0, $bytes.Length); $p.Flush()
$r = New-Object System.IO.StreamReader($p, [System.Text.Encoding]::UTF8)
$deadline = (Get-Date).AddSeconds($Seconds); $t = $null
while ((Get-Date) -lt $deadline) {
  if (-not $t) { $t = $r.ReadLineAsync() }
  if ($t.Wait(500)) { $line = $t.Result; $t = $null; if ($line -match '"(info|err)\.(started|enter_reload|plan|boss_denied|unknown_boss|growth_enter|exit_pending|not_ready)') { $line; break } }
}
$p.Dispose()
"batch $Batch : $($b.floors.Count) floors"
