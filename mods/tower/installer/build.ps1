# 影塔·百战 发布构建：纯 Mod 运行库 + 影塔模块（发布版日志）+ avrt.dll 代理 → 安装器 EXE 与手动安装 ZIP
# 用法：powershell -ExecutionPolicy Bypass -File mods\tower\installer\build.ps1
param([switch]$InstallerOnly, [string]$OutputDirectory)  # 只重编安装器界面（沿用 build\release-mod 里已构建的 DLL）
$ErrorActionPreference = 'Stop'
# 版本号只在 src\ModFiles.cs 的 Version 一处改；文件名、手动安装说明都从这里取
$Version = [regex]::Match([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'src\ModFiles.cs')), 'const string Version = "([0-9.]+)"').Groups[1].Value
if (-not $Version) { throw '读不到 src\ModFiles.cs 里的 Version' }
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$here = $PSScriptRoot
$native = Join-Path $repo 'build\release-mod'
$out = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repo "dist\release\latest" }

function Step($name, [scriptblock]$body) { Write-Host "== $name"; & $body; if ($LASTEXITCODE) { throw "$name 失败（$LASTEXITCODE）" } }

if (-not $InstallerOnly) {
    Step '离线测试' { cmd /c "`"$repo\mods\tower\tests\run.cmd`" >nul" }
    Step '运行库（纯 Mod）' { cmd /c "`"$repo\src\core\build.bat`" release" | Select-String 'BUILD_OK' }
    Step '影塔模块（发布版）' { cmd /c "set PBZ_MODULE_DEFS=/DPBZ_RELEASE /bigobj&& set PBZ_MODULE_LINK=/DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll delayimp.lib&& `"$repo\src\core\sdk\build_module.bat`" PBZTower `"$repo\mods\tower\src`" `"$native\core`"" | Select-String 'MODULE_OK' }
    Step '代理 avrt.dll' { cmd /c "`"$repo\src\core\proxy\build_proxy.bat`"" | Select-String 'PROXY_OK' }
}

$payload = @{
    'payload.avrt.dll'     = Join-Path $native 'avrt.dll'
    'payload.PBZCore.dll'  = Join-Path $native 'core\PBZCore.dll'
    'payload.PBZTower.dll' = Join-Path $native 'core\PBZTower.dll'
    'payload.guide.txt'    = Join-Path $here 'assets\guide.txt'
    'payload.changes.txt'  = Join-Path $here 'assets\changes.txt'
    'payload.world.json'   = Join-Path $here 'assets\world-template.json'
    'assets.fox.png'       = Join-Path $here 'assets\fox.png'
    'assets.fox.ico'       = Join-Path $here 'assets\fox.ico'
    'assets.donate.jpg'    = Join-Path $here 'assets\donate.jpg'
}
foreach ($f in $payload.Values) { if (-not (Test-Path -LiteralPath $f)) { throw "缺少 $f" } }

# 安装器：Roslyn 直接编译到系统自带的 .NET Framework 4.x（Windows 10/11 自带，玩家不用装运行时，EXE 体积小）
$fw = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$csc = Get-ChildItem (Join-Path $env:ProgramFiles 'dotnet\sdk') -Directory | Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'Roslyn\bincore\csc.dll' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $csc) { throw '找不到 .NET SDK 里的 Roslyn csc.dll' }
New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out "PBZTowerSetup-v$Version.exe"  # 正式版带版本后缀（用户 2026-11-06：v1.0.0 正式版第一版）
$refs = 'mscorlib','System','System.Core','System.Drawing','System.Windows.Forms','System.Web.Extensions' | ForEach-Object { "/reference:$fw\$_.dll" }
$res = $payload.GetEnumerator() | ForEach-Object { "/resource:$($_.Value),$($_.Key)" }
$srcs = Get-ChildItem (Join-Path $here 'src') -Filter *.cs | ForEach-Object { $_.FullName }
$srcs = @($srcs) + @((Get-ChildItem (Join-Path $here '..\hotkey\src') -Filter *.cs | ForEach-Object { $_.FullName }))
Step '安装器 EXE' { & dotnet $csc /nologo /noconfig /nostdlib /target:winexe /platform:anycpu /optimize+ /langversion:7.3 /utf8output `
        /main:PBZTowerSetup.Program "/win32icon:$(Join-Path $here 'assets\fox.ico')" "/out:$exe" @refs @res @srcs }

# 手动安装包（N 网 / GitHub）：解压后把 platform 文件夹合并进游戏根目录
$stage = Join-Path $native 'zip'
$stage = [IO.Path]::GetFullPath($stage)
if (-not $stage.StartsWith(([IO.Path]::GetFullPath($native).TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid staging path' }
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$target = Join-Path $stage 'platform\steam'
New-Item -ItemType Directory -Force (Join-Path $target 'PBZTower') | Out-Null
Copy-Item $payload['payload.avrt.dll'] (Join-Path $target 'avrt.dll')
Copy-Item $payload['payload.PBZCore.dll'] (Join-Path $target 'PBZTower\PBZCore.dll')
Copy-Item $payload['payload.PBZTower.dll'] (Join-Path $target 'PBZTower\PBZTower.dll')
Copy-Item $payload['payload.guide.txt'] (Join-Path $target 'PBZTower\README.txt')
Copy-Item $payload['payload.world.json'] (Join-Path $target 'PBZTower\world-template.json')
$manual = [IO.File]::ReadAllText((Join-Path $here 'assets\README-manual.txt')).Replace('{VERSION}', $Version)
[IO.File]::WriteAllText((Join-Path $stage 'README-install.txt'), $manual, [Text.UTF8Encoding]::new($true))
# 打开键设置工具仅单独输出，Nexus 的手动 Mod 压缩包不包含任何 EXE。
Step '打开键设置 EXE' { & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here '..\hotkey\build.ps1') -OutputDirectory $out | Select-String 'HOTKEY_OK' }
$zip = Join-Path $out "PBZTower-v$Version-mod.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# 逐文件写入：条目路径用正斜杠（ZIP 规范；Vortex / 7-Zip / macOS 才能正确解出目录）；文件名全用 ASCII（.NET Framework 不写 UTF-8 标记，中文名在非中文系统会乱码）
$zs = [IO.File]::Open($zip, [IO.FileMode]::CreateNew)
$za = New-Object IO.Compression.ZipArchive($zs, [IO.Compression.ZipArchiveMode]::Create, $false, [Text.UTF8Encoding]::new($false))
try {
    foreach ($f in Get-ChildItem -LiteralPath $stage -Recurse -File) {
        $name = $f.FullName.Substring($stage.TrimEnd([char]92).Length + 1).Replace([char]92, [char]47)
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $za.Dispose(); $zs.Dispose() }

[IO.File]::WriteAllText((Join-Path $out "更新说明-v$Version.txt"), [IO.File]::ReadAllText($payload['payload.changes.txt']), [Text.UTF8Encoding]::new($true))
$sums = Get-ChildItem $out -File | Where-Object Name -ne 'SHA256SUMS.txt' | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower() + '  ' + $_.Name }
[IO.File]::WriteAllLines((Join-Path $out 'SHA256SUMS.txt'), $sums, [Text.UTF8Encoding]::new($false))
$sums
Write-Host "RELEASE_OK $out"
