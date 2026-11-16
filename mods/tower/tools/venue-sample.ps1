# QA venue sample helper (needs the PBZ_HEX_TEST build). Keep this file ASCII-only.
# Reads the player's world first, picks the matching Rongshu venue from notes\rongshu-venues-20261112.json,
# then samples its spawners and fog door. Every reply is appended to notes\venue-samples-20261112.jsonl.
#   & E:\PBZ-claude\mods\tower\tools\venue-sample.ps1 -Label inside-entry
#   & E:\PBZ-claude\mods\tower\tools\venue-sample.ps1 -Label door-inner -Stage 4
#   & E:\PBZ-claude\mods\tower\tools\venue-sample.ps1 -Stage 2 -Warp landing   (outside the tower, same world)
# Sampling is read-only. -Warp uses the core point teleport (waits for streaming); no story entry, no save write.
param([string]$Label = 'sample', [int]$Stage = 0, [string]$Warp = '', [int]$Seconds = 12)
$ErrorActionPreference = 'Stop'
$root = 'E:\PBZ-claude'
$venues = (Get-Content -LiteralPath "$root\notes\rongshu-venues-20261112.json" -Raw -Encoding UTF8 | ConvertFrom-Json).venues
$log = "$root\notes\venue-samples-20261112.jsonl"

function Send-Tower([hashtable]$req, [string]$event = 'qa_venue_sample') {
  $json = $req | ConvertTo-Json -Compress
  $p = New-Object System.IO.Pipes.NamedPipeClientStream(".", "PBZLab", [System.IO.Pipes.PipeDirection]::InOut)
  $p.Connect(3000)
  Start-Sleep -Milliseconds 800   # the core drops a line written right after connect
  $b = [System.Text.Encoding]::UTF8.GetBytes($json + "`n")
  $p.Write($b, 0, $b.Length); $p.Flush()
  $r = New-Object System.IO.StreamReader($p, [System.Text.Encoding]::UTF8)
  $deadline = (Get-Date).AddSeconds($Seconds); $t = $null; $hit = $null
  while ((Get-Date) -lt $deadline -and -not $hit) {
    if (-not $t) { $t = $r.ReadLineAsync() }
    if ($t.Wait(500)) { $line = $t.Result; $t = $null; if ($line -match ('"event":"' + $event + '"')) { $hit = $line } }
  }
  $p.Dispose()
  return $hit
}

if ($Warp) {
  # Point-teleport inside the same world to a recorded spot: landing | center | inside_entry | outside
  $v = $venues | Where-Object stage -eq $Stage
  $s = $v.interior_live_samples
  if (-not $s -or -not $s.$Warp) { throw "stage $Stage has no recorded spot '$Warp'" }
  $pt = $s.$Warp; $yaw = $s."$($Warp)_yaw"; if ($null -eq $yaw) { $yaw = 0 }
  $reply = Send-Tower @{ op='tower'; cmd='qa_venue_warp'; world=$v.world; x=[string]$pt[0]; y=[string]$pt[1]; z=[string]$pt[2]; yaw=[string]$yaw } 'qa_venue_warp'
  if (-not $reply) { throw 'no qa_venue_warp reply' }
  Add-Content -LiteralPath $log -Value $reply -Encoding UTF8
  $reply; return
}
$v = $null
if ($Stage -gt 0) { $v = $venues | Where-Object stage -eq $Stage }
else {
  $probe = Send-Tower @{ op='tower'; cmd='qa_venue_sample'; label="$Label-probe"; actors='' }
  if (-not $probe) { throw 'no qa_venue_sample reply (QA build not loaded or player not ready)' }
  $world = ($probe | ConvertFrom-Json).world
  $v = $venues | Where-Object world -eq $world
  if (-not $v) {
    Add-Content -LiteralPath $log -Value $probe -Encoding UTF8
    "world=$world has no Rongshu record; player-only sample logged"; $probe; return
  }
}
$actors = (@($v.members.actor) + @($v.doors.name)) -join ','
$reply = Send-Tower @{ op='tower'; cmd='qa_venue_sample'; label="rongshu-$($v.stage)-$Label"; world=$v.world; actors=$actors }
if (-not $reply) { throw 'sample timed out' }
Add-Content -LiteralPath $log -Value $reply -Encoding UTF8
"stage=$($v.stage) world=$($v.world)"
$reply
