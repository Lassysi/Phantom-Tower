# 影塔·百战 打开键设置小工具：PBZTowerHotkey.exe（.NET Framework 4.x，玩家不用装运行时）
# 用法：powershell -ExecutionPolicy Bypass -File mods\tower\hotkey\build.ps1 [-OutputDirectory <目录>]
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$inst = Join-Path $here '..\installer'
$out = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $here 'bin' }
$fw = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$csc = Get-ChildItem (Join-Path $env:ProgramFiles 'dotnet\sdk') -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'Roslyn\bincore\csc.dll' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $csc) { throw '找不到 .NET SDK 里的 Roslyn csc.dll' }
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'PBZTowerHotkey.exe'
$refs = 'mscorlib','System','System.Core','System.Drawing','System.Windows.Forms' | ForEach-Object { "/reference:$fw\$_.dll" }
$srcs = @((Get-ChildItem (Join-Path $here 'src') -Filter *.cs | ForEach-Object { $_.FullName })) + @((Join-Path $inst 'src\GameLocator.cs'), (Join-Path $inst 'src\Theme.cs'))
& dotnet $csc /nologo /noconfig /nostdlib /target:winexe /platform:anycpu /optimize+ /langversion:7.3 /utf8output "/win32icon:$(Join-Path $inst 'assets\fox.ico')" "/out:$exe" @refs @srcs
if ($LASTEXITCODE) { throw "编译失败（$LASTEXITCODE）" }
Write-Host "HOTKEY_OK $exe"
