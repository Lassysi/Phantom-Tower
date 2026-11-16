# Send one tower pipe command (QA) and print replies. Keep this file ASCII-only (Windows PowerShell 5 reads BOM-less UTF-8 as ANSI).
#   From a PowerShell prompt (NOT via powershell -File: Windows strips the JSON double quotes):
#   & E:\PBZ-claude\mods\tower\tools\tower-cmd.ps1 -Json '{"op":"tower","cmd":"qa_floor","to":17}' -Expect qa_floor
param([Parameter(Mandatory)][string]$Json, [string]$Expect = "", [int]$Seconds = 4)
$p = New-Object System.IO.Pipes.NamedPipeClientStream(".", "PBZLab", [System.IO.Pipes.PipeDirection]::InOut)
$p.Connect(3000)
Start-Sleep -Milliseconds 800   # the core drops a line written right after connect (observed 2026-11-12)
$b = [System.Text.Encoding]::UTF8.GetBytes($Json + "`n")   # raw bytes + LF (StreamWriter lines were not picked up)
$p.Write($b, 0, $b.Length); $p.Flush()
$r = New-Object System.IO.StreamReader($p, [System.Text.Encoding]::UTF8)
$deadline = (Get-Date).AddSeconds($Seconds)
$t = $null
while ((Get-Date) -lt $deadline) {
  if (-not $t) { $t = $r.ReadLineAsync() }
  if ($t.Wait(500)) {
    $line = $t.Result; $t = $null
    if ($line -and (-not $Expect -or $line -match $Expect)) { $line; if ($Expect) { break } }
  }
}
$p.Dispose()
