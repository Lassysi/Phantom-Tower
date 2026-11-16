param([string]$EngineRoot = 'E:\PBZ-claude\build\release\tower-engine', [string]$EngineVersion = 'v0.3.0-abi4')
$ErrorActionPreference = 'Stop'
# Claude freezes a new immutable directory for an update. Change only this version
# when a new approved engine release is handed over; never select build/core.
if (-not $EngineVersion) { throw 'Supply -EngineVersion for a frozen ABI 4 release; v0.2.0-abi3 is retired' }
$LoaderVersion = '0.3.0'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$engine = [IO.Path]::GetFullPath($EngineRoot)
if (-not $engine.EndsWith('\build\release\tower-engine', [StringComparison]::OrdinalIgnoreCase)) { throw 'Only build\release\tower-engine frozen inputs are accepted' }
$finalDir = Join-Path $workspace 'dist\tower'
$working = Join-Path $workspace ('build\tower-ui\package-' + [Guid]::NewGuid().ToString('N'))
if (-not ([IO.Path]::GetFullPath($working)).StartsWith((Join-Path $workspace 'build\tower-ui\'), [StringComparison]::OrdinalIgnoreCase)) { throw 'Working path escaped workspace' }
New-Item -ItemType Directory -Force $working,$finalDir | Out-Null
Push-Location $workspace
try {
    $scoped = @(& git status --porcelain --untracked-files=all -- mods/tower/ui mods/tower/tools/package.ps1 mods/tower/tools/build_loader_payload.py)
    if ($scoped.Count) { throw 'Commit the loader and packaging source before producing a release candidate' }
    $source = (& git rev-parse HEAD).Trim()
    $payload = Join-Path $working 'payload.zip'
    $folder = Join-Path $working 'PBZTower'
    & python -I mods/tower/tools/build_loader_payload.py --engine-root $engine --engine-version $EngineVersion --output $payload --folder $folder
    if ($LASTEXITCODE) { throw 'Frozen engine/payload validation failed' }
    $single = Join-Path $working 'single'
    $loose = Join-Path $working 'loose'
    & dotnet publish mods/tower/ui/PBZTower.Loader.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true "-p:TowerPayload=$payload" -o $single --nologo
    if ($LASTEXITCODE) { throw 'Standalone publish failed' }
    & dotnet publish mods/tower/ui/PBZTower.Loader.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true -p:TowerPayload= -o $loose --nologo
    if ($LASTEXITCODE) { throw 'Folder loader publish failed' }
    foreach ($publish in @($single,$loose)) {
        $files = @(Get-ChildItem -LiteralPath $publish -Recurse -File)
        if ($files.Count -ne 1 -or $files[0].Name -ne 'PBZTower.exe') { throw 'Unexpected publish files; inspect before release' }
    }
    Copy-Item -LiteralPath (Join-Path $loose 'PBZTower.exe') -Destination (Join-Path $folder 'PBZTower.exe')
    if ((& git rev-parse HEAD).Trim() -ne $source) { throw 'Source commit changed during packaging' }
    if (@(& git status --porcelain --untracked-files=all -- mods/tower/ui mods/tower/tools/package.ps1 mods/tower/tools/build_loader_payload.py).Count) { throw 'Loader source changed during packaging' }
    $exeName = "PBZTower-$LoaderVersion-win-x64.exe"
    $zipName = "PBZTower-$LoaderVersion-win-x64.zip"
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($folder, (Join-Path $working $zipName), [IO.Compression.CompressionLevel]::Optimal, $true)
    foreach ($name in @($exeName,$zipName,'SHA256SUMS.txt')) {
        $existing = Join-Path $finalDir $name
        if (Test-Path -LiteralPath $existing) { Move-Item -LiteralPath $existing -Destination (Join-Path $working ('previous-' + $name)) }
    }
    Copy-Item -LiteralPath (Join-Path $single 'PBZTower.exe') -Destination (Join-Path $finalDir $exeName)
    Copy-Item -LiteralPath (Join-Path $working $zipName) -Destination (Join-Path $finalDir $zipName)
    $hashLines = [Collections.Generic.List[string]]::new()
    foreach ($name in @($exeName,$zipName)) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $finalDir $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        $hashLines.Add("$hash  $name")
    }
    foreach ($file in Get-ChildItem -LiteralPath $folder -Recurse -File | Sort-Object FullName) {
        $relative = [IO.Path]::GetRelativePath($folder,$file.FullName).Replace('\','/')
        $hashLines.Add((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  PBZTower/' + $relative)
    }
    [IO.File]::WriteAllLines((Join-Path $finalDir 'SHA256SUMS.txt'),$hashLines,[Text.UTF8Encoding]::new($false))
    $hashLines
    [PSCustomObject]@{ Source=$source; Engine=$EngineVersion; Output=$finalDir; Working=$working } | ConvertTo-Json
} finally { Pop-Location }
