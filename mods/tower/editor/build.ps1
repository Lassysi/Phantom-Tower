# 影塔跳层工具（自用）：Roslyn 编译到系统自带 .NET Framework 4.x，输出 dist\tools\PBZTowerFloorEditor.exe。不进安装包 / Mod 压缩包。
# 用法：powershell -ExecutionPolicy Bypass -File mods\tower\editor\build.ps1
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$out = Join-Path $repo 'dist\tools'
New-Item -ItemType Directory -Force $out | Out-Null
$fw = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$csc = Get-ChildItem (Join-Path $env:ProgramFiles 'dotnet\sdk') -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'Roslyn\bincore\csc.dll' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $csc) { throw '找不到 .NET SDK 里的 Roslyn csc.dll' }
$exe = Join-Path $out 'PBZTowerFloorEditor.exe'
$refs = 'mscorlib','System','System.Core','System.Drawing','System.Windows.Forms' | ForEach-Object { "/reference:$fw\$_.dll" }
$icon = Join-Path $repo 'mods\tower\installer\assets\fox.ico'
& dotnet $csc /nologo /noconfig /nostdlib /target:winexe /platform:anycpu /optimize+ /langversion:7.3 /utf8output "/win32icon:$icon" "/out:$exe" @refs (Join-Path $PSScriptRoot 'FloorEditor.cs')
if ($LASTEXITCODE) { throw "编译失败（$LASTEXITCODE）" }
Write-Host "EDITOR_OK $exe"
