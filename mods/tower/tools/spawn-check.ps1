# 3.6.0 新收录 Boss 实测：同一连接里先 trainer_unlock，再按类路径在玩家附近生成 1 只（敌方）。Keep ASCII-only.
#   & E:\PBZ-claude\mods\tower\tools\spawn-check.ps1 -Path '/Game/RenewalContent/Actors/Mobs/Normal/BP_Monster_WuXuan.BP_Monster_WuXuan_C'
param([Parameter(Mandatory)][string]$Path, [int]$Distance = 900, [int]$Seconds = 8)
$p = New-Object System.IO.Pipes.NamedPipeClientStream(".", "PBZLab", [System.IO.Pipes.PipeDirection]::InOut)
$p.Connect(3000)
Start-Sleep -Milliseconds 800
$r = New-Object System.IO.StreamReader($p, [System.Text.Encoding]::UTF8)
function Send($json) { $b = [System.Text.Encoding]::UTF8.GetBytes($json + "`n"); $p.Write($b, 0, $b.Length); $p.Flush() }
Send '{"op":"trainer_unlock"}'
Start-Sleep -Milliseconds 500
$id = "spawncheck-" + [guid]::NewGuid().ToString("N").Substring(0, 8)
Send ('{"op":"enemy","cmd":"spawn","request_id":"' + $id + '","path":"' + $Path + '","count":1,"distance_cm":' + $Distance + ',"faction":"enemy"}')
$deadline = (Get-Date).AddSeconds($Seconds); $t = $null
while ((Get-Date) -lt $deadline) {
  if (-not $t) { $t = $r.ReadLineAsync() }
  if ($t.Wait(500)) { $line = $t.Result; $t = $null; if ($line -match '"op":"enemy"|trainer_unlock') { $line } }
}
$p.Dispose()
