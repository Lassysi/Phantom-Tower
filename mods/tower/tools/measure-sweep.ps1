# 3.6.0 venue measure sweep (test build PBZ_PLAYTEST): for each venue in mods\tower\data\capacity_sweep_20261112.json send qa_measure.
# Needs: tower save loaded, run on the ready page, nothing in progress. Keep this file ASCII-only.
#   & E:\PBZ-claude\mods\tower\tools\measure-sweep.ps1                # all venues
#   & E:\PBZ-claude\mods\tower\tools\measure-sweep.ps1 -From 10 -To 15 -Dirs 48
# Results append to E:\PBZ-claude\notes\venue-measure-20261114.jsonl (one qa_measure event per line).
param([int]$From = 0, [int]$To = 999, [int]$Dirs = 36, [int]$Max = 70, [int]$Seconds = 200)
$ErrorActionPreference = 'Stop'
$data = Get-Content -LiteralPath 'E:\PBZ-claude\mods\tower\data\capacity_sweep_20261112.json' -Raw -Encoding UTF8 | ConvertFrom-Json
$log = 'E:\PBZ-claude\notes\venue-measure-20261114.jsonl'

function Send-Measure([hashtable]$req) {
  $json = $req | ConvertTo-Json -Compress
  $p = New-Object System.IO.Pipes.NamedPipeClientStream(".", "PBZLab", [System.IO.Pipes.PipeDirection]::InOut)
  $p.Connect(3000)
  Start-Sleep -Milliseconds 800
  $b = [System.Text.Encoding]::UTF8.GetBytes($json + "`n")
  $p.Write($b, 0, $b.Length); $p.Flush()
  $r = New-Object System.IO.StreamReader($p, [System.Text.Encoding]::UTF8)
  $deadline = (Get-Date).AddSeconds($Seconds); $t = $null; $hit = $null
  while ((Get-Date) -lt $deadline -and -not $hit) {
    if (-not $t) { $t = $r.ReadLineAsync() }
    if ($t.Wait(500)) { $line = $t.Result; $t = $null; if ($line -match '"event":"qa_measure"') { $hit = $line } }
  }
  $p.Dispose()
  return $hit
}

$venues = @($data.venues)
for ($i = [Math]::Max(0, $From); $i -le [Math]::Min($To, $venues.Count - 1); $i++) {
  $v = $venues[$i]
  $req = @{ op='tower'; cmd='qa_measure'; label=('m' + $i); venue=$v.venue; dirs=$Dirs; max=$Max }
  $res = $null
  for ($try = 0; $try -lt 6 -and -not $res; $try++) {
    $res = Send-Measure $req
    if ($res -and $res -match '"error":"busy"') { $res = $null; Start-Sleep -Seconds 5 }
  }
  if (-not $res) { $res = '{"op":"tower","event":"qa_measure","ok":false,"label":"m' + $i + '","error":"no_reply"}' }
  Add-Content -LiteralPath $log -Value $res -Encoding UTF8
  $o = $res | ConvertFrom-Json
  "{0} {1} min={2} p10={3} med={4} max={5} ratio={6}/{7} area={8} walls={9} edges={10} {11}{12}" -f $o.label, $o.venue, $o.min_m, $o.p10_m, $o.median_m, $o.max_m, $o.ratio, $o.ratio_p10_p90, $o.area_m2, $o.walls, $o.edges, $o.why, $o.error
  if ($o.error -eq 'only_tower_ready_page' -or $o.error -eq 'measure_in_progress') { 'stopped: tower is not on the ready page'; return }
  Start-Sleep -Seconds 2
}
'sweep done'
