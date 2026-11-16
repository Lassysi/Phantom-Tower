# Capacity sweep (QA build): for each venue in mods\tower\data\capacity_sweep_20261112.json send qa_cap cases one by one.
# Needs: tower save loaded, run on the ready page (any trial / audit run), nothing in progress. Keep this file ASCII-only.
#   & E:\PBZ-claude\mods\tower\tools\cap-sweep.ps1                 # all venues
#   & E:\PBZ-claude\mods\tower\tools\cap-sweep.ps1 -From 10 -To 15 -Stand inward -BossMode player
# Results append to E:\PBZ-claude\notes\capacity-sweep-20261112.jsonl (one qa_cap event per line).
param([int]$From = 0, [int]$To = 999, [string]$Stand = 'inward', [string]$BossMode = 'player', [int]$Seconds = 260)
$ErrorActionPreference = 'Stop'
$data = Get-Content -LiteralPath 'E:\PBZ-claude\mods\tower\data\capacity_sweep_20261112.json' -Raw -Encoding UTF8 | ConvertFrom-Json
$log = 'E:\PBZ-claude\notes\capacity-sweep-20261112.jsonl'

function Send-Cap([hashtable]$req) {
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
    if ($t.Wait(500)) { $line = $t.Result; $t = $null; if ($line -match '"event":"qa_cap"') { $hit = $line } }
  }
  $p.Dispose()
  return $hit
}

$venues = @($data.venues)
for ($i = [Math]::Max(0, $From); $i -le [Math]::Min($To, $venues.Count - 1); $i++) {
  $v = $venues[$i]
  foreach ($c in $v.cases) {
    $req = @{ op='tower'; cmd='qa_cap'; label=$c.label; venue=$v.venue; stand=$Stand; bossmode=$BossMode;
              bosses=(@($c.bosses) -join ','); mobs=(@($c.mobs) -join ',') }
    $res = $null
    for ($try = 0; $try -lt 6 -and -not $res; $try++) {
      $res = Send-Cap $req
      if ($res -and $res -match '"error":"busy"') { $res = $null; Start-Sleep -Seconds 5 }
    }
    if (-not $res) { $res = '{"op":"tower","event":"qa_cap","ok":false,"label":"' + $c.label + '","error":"no_reply"}' }
    Add-Content -LiteralPath $log -Value $res -Encoding UTF8
    $o = $res | ConvertFrom-Json
    "{0} {1} pass={2} spawned={3}/{4} out={5} soft={6} overlap={7} stand={8} {9}{10}" -f $c.label, $v.venue.Substring($v.venue.LastIndexOf('.') + 1), $o.pass, $o.spawned, $o.wanted, $o.outside, $o.soft, $o.overlaps, $o.standing, $o.why, $o.error
    if ($o.error -eq 'only_tower_ready_page' -or $o.error -eq 'capture_in_progress') { 'stopped: tower is not on the ready page'; return }
    Start-Sleep -Seconds 2
  }
}
'sweep done'
