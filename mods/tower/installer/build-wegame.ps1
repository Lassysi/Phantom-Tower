# Compile a WeGame installer using the published Steam installer's exact embedded assets.
param([Parameter(Mandatory = $true)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$modFiles = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'src\ModFiles.cs'))
$version = [regex]::Match($modFiles, 'const string Version = "([0-9.]+)"').Groups[1].Value
if (-not $version) { throw 'Cannot find the installer version in ModFiles.cs.' }
$published = Join-Path $repo "dist\release\$version\PBZTowerSetup-v$version.exe"
if (-not (Test-Path -LiteralPath $published -PathType Leaf)) { throw "Missing release installer: $published" }
$release = [Reflection.Assembly]::LoadFile($published)
if ($release.GetName().Version.ToString(3) -ne $version) { throw 'Release installer version does not match the source.' }

$fw = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$csc = Get-ChildItem (Join-Path $env:ProgramFiles 'dotnet\sdk') -Directory |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'Roslyn\bincore\csc.dll' } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $csc) { throw 'Cannot find Roslyn csc.dll in the .NET SDK.' }

$output = [IO.Path]::GetFullPath($OutputDirectory)
$null = New-Item -ItemType Directory -Force -Path $output
$exe = Join-Path $output 'PBZTowerSetup_wegame.exe'
$stage = Join-Path $repo ('build\wegame-assets-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $stage
try {
    $assets = @{}
    foreach ($name in $release.GetManifestResourceNames()) {
        $path = Join-Path $stage ($assets.Count.ToString() + '.bin')
        $source = $release.GetManifestResourceStream($name)
        try {
            $dest = [IO.File]::Create($path)
            try { $source.CopyTo($dest) } finally { $dest.Dispose() }
        } finally { $source.Dispose() }
        $assets.Add($name, $path)
    }
    foreach ($name in @('payload.avrt.dll', 'payload.PBZCore.dll', 'payload.PBZTower.dll', 'payload.world.json',
                        'payload.guide.txt', 'payload.changes.txt', 'assets.fox.ico', 'assets.fox.png', 'assets.donate.jpg')) {
        if (-not $assets.ContainsKey($name)) { throw "Release installer resource missing: $name" }
    }
    $refs = 'mscorlib','System','System.Core','System.Drawing','System.Windows.Forms','System.Web.Extensions' |
        ForEach-Object { "/reference:$fw\$_.dll" }
    $resources = $assets.GetEnumerator() | ForEach-Object { "/resource:$($_.Value),$($_.Key)" }
    $codeDirs = (Join-Path $PSScriptRoot 'src'), (Join-Path $PSScriptRoot '..\hotkey\src')
    $sourceFiles = Get-ChildItem -LiteralPath $codeDirs -Filter '*.cs' -File | Select-Object -ExpandProperty FullName
    & dotnet $csc /nologo /noconfig /nostdlib /target:winexe /platform:anycpu /optimize+ /langversion:7.3 /utf8output `
        /define:WEGAME_SETUP /main:PBZTowerSetup.Program "/win32icon:$($assets['assets.fox.ico'])" "/out:$exe" @refs @resources @sourceFiles
    if ($LASTEXITCODE -ne 0) { throw "WeGame installer compilation failed ($LASTEXITCODE)." }
    Get-FileHash -LiteralPath $exe -Algorithm SHA256
} finally {
    Get-ChildItem -LiteralPath $stage -File | Remove-Item
    Remove-Item -LiteralPath $stage
}
