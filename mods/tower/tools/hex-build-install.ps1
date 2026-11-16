# 影塔海克斯：单测 → QA 包 + 正式包 → 等游戏退出 → 备份 → 安装 QA 包 → 打哈希
# 用法：powershell -File E:\PBZ-claude\mods\tower\tools\hex-build-install.ps1 -Name hex-batchH [-NoInstall]
param([Parameter(Mandatory)][string]$Name, [switch]$NoInstall)
$r = "E:\PBZ-claude"
$log = "$r\build\$Name\tests.log"
New-Item -ItemType Directory -Force "$r\build\$Name" | Out-Null
cmd /c "$r\mods\tower\tests\run.cmd" *> $log
"tests exit=$LASTEXITCODE"
if ($LASTEXITCODE -ne 0) { Get-Content $log | Select-String "FAIL|error" | Select-Object -Last 5; exit 1 }
$link = "/DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll delayimp.lib"
foreach ($v in @(@{d="/DPBZ_HEX_TEST /bigobj"; o="qa"}, @{d="/DPBZ_RELEASE /bigobj"; o="release"})) {
  Remove-Item "$r\build\$Name\$($v.o)\PBZTower.dll" -ErrorAction SilentlyContinue
  cmd /c "set PBZ_MODULE_DEFS=$($v.d)&& set PBZ_MODULE_LINK=$link&& `"$r\src\core\sdk\build_module.bat`" PBZTower `"$r\mods\tower\src`" `"$r\build\$Name\$($v.o)`"" 2>&1 | Select-String "MODULE_OK|error" | Select-Object -First 10
}
$src = "$r\build\$Name\qa\PBZTower.dll"; $rel = "$r\build\$Name\release\PBZTower.dll"
if (-not (Test-Path $src) -or -not (Test-Path $rel)) { "build failed"; exit 1 }
if (Select-String -Path $rel -Pattern "tower-qa" -SimpleMatch -Quiet) { "release contains tower-qa: refuse"; exit 1 }
"qa=" + (Get-FileHash $src).Hash
"BUILD_DONE"
if ($NoInstall) { exit 0 }
$dst = "E:\steam\steamapps\common\PBZ\platform\steam\PBZTower\PBZTower.dll"; $bk = "E:\Phantom_Blade_Zero\build\$Name\before-install"
while (Get-Process PBZ -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 3 }   # 只在新进程部署：等游戏完全退出
Start-Sleep -Seconds 3
New-Item -ItemType Directory -Force $bk | Out-Null
Copy-Item $dst "$bk\PBZTower.dll" -Force
# 塔局状态（顶层文本档：run / growth / tower-session 等；大目录 sandbox、save-backup 不复制）
$tower = Join-Path $env:LOCALAPPDATA "PBZTower"
if (Test-Path $tower) {
  New-Item -ItemType Directory -Force "$bk\PBZTower-state" | Out-Null
  Get-ChildItem $tower -File | Copy-Item -Destination "$bk\PBZTower-state" -Force
  "state-backup=" + (Get-ChildItem "$bk\PBZTower-state" -File).Count + " files"
}
Copy-Item $src $dst -Force
"backup=" + (Get-FileHash "$bk\PBZTower.dll").Hash
"installed=" + (Get-FileHash $dst).Hash
