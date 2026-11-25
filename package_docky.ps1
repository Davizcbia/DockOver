# Builds DockOver and packs a portable zip.
#   powershell -ExecutionPolicy Bypass -File package_docky.ps1 [-Release] [-NoBuild] [-NoIdentity] [-BuildDir <dir>]
#
# -Release: the package for GitHub (any PC, no package identity, no yellow capture frame: glass uses DXGI duplication).
#   dist\DockOver-Windows-Portable-<version>.zip holds only two programs at the top:
#     DockOver.exe           starts DockOver (Dock, Menu Bar; opens Settings when it already runs)
#     DockOver Desktop.exe   widgets and Stacks (a separate program)
#     runtime\               everything else (Manager, Dock, Menu Bar, Settings, guardian, updater, assets)
#   Publish it as a GitHub release of DOCKY_UPDATE_REPO (dockui\CMakeLists.txt) whose tag is the version alone
#   (DOCKY_VERSION, for example 0.1.0). GitHub records the asset's SHA-256, which the updater checks.
# Without -Release: a development package laid out like build-borderless (identity on unless -NoIdentity), named
#   DockOver-Windows-Dev-<version>.zip, so the update check (which looks for the release name) never offers it.
# The programs link the C/C++ runtime statically: no Visual C++ Redistributable or runtime DLLs are needed.
param([switch]$NoBuild, [switch]$NoIdentity, [switch]$Release, [string]$BuildDir = '')
$ErrorActionPreference = 'Stop'
if ($Release) { $NoIdentity = [switch]$true }
$root = $PSScriptRoot
$source = Join-Path $root 'dockui'
if (-not $BuildDir) { $BuildDir = Join-Path $root ($(if ($NoIdentity) { 'build-release-plain' } else { 'build-release' })) }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)

$cmakeLists = Get-Content (Join-Path $source 'CMakeLists.txt') -Raw
$version = [regex]::Match($cmakeLists, 'set\(DOCKY_VERSION "([^"]+)"').Groups[1].Value
$repo = [regex]::Match($cmakeLists, 'set\(DOCKY_UPDATE_REPO "([^"]+)"').Groups[1].Value
$asset = [regex]::Match($cmakeLists, 'set\(DOCKY_UPDATE_ASSET "([^"]+)"').Groups[1].Value
if (-not $asset) { throw 'DOCKY_UPDATE_ASSET not found in dockui\CMakeLists.txt.' }
if (-not $version) { throw 'DOCKY_VERSION not found in dockui\CMakeLists.txt.' }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw 'MSVC x64 C++ build tools were not found.' }
if (-not $NoBuild) {
    $dev = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $identity = $(if ($NoIdentity) { 'OFF' } else { 'ON' })
    $command = 'call "{0}" -arch=x64 >nul && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DDOCKY_SPARSE_IDENTITY={4} -DDOCKY_PHONE_PAIRING=ON && "{1}" --build "{3}"' -f $dev, $cmake, $source, $BuildDir, $identity
    & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
}
$built = Get-Content (Join-Path $BuildDir 'docky_version.h') -Raw
if ($built -notmatch "DOCKY_VERSION `"$([regex]::Escape($version))`"") { throw "The build in $BuildDir is not version $version. Build again without -NoBuild." }

# Program files only. User settings, packs and logs live in %LOCALAPPDATA% and are never part of the package.
$programs = 'LiquidDesktopManager.exe', 'LiquidDock.exe', 'LiquidMenuBar.exe', 'LiquidDesktopSettings.exe',
            'docky-taskbar-guardian.exe', 'docky-taskbar-restore.exe', 'docky-taskbar-guard-sim.exe',
            'docky-wifi-list.exe', 'docky-ext.exe', 'docky-updater.exe', 'docky-now-playing.exe',
            'docky-bubble.exe', 'docky-blackhole.exe'
$name = $(if ($Release) { "$asset-$version" } else { "DockOver-Windows-Dev-$version" })
$stage = Join-Path $root "dist\$name"
if (Test-Path -LiteralPath $stage) { $stage = "$stage-build-$([guid]::NewGuid().ToString('N'))" }
New-Item $stage -ItemType Directory -Force | Out-Null
$programDir = $(if ($Release) { Join-Path $stage 'runtime' } else { $stage })
New-Item $programDir -ItemType Directory -Force | Out-Null
foreach ($p in $programs) {
    $from = Join-Path $BuildDir $p
    if (-not (Test-Path -LiteralPath $from)) { throw "$p is missing from $BuildDir." }
    Copy-Item -LiteralPath $from -Destination $programDir
}
$appsDir = Join-Path $BuildDir 'applications-menu'
if (-not (Test-Path (Join-Path $appsDir 'DockOverApplications.exe'))) { throw 'Applications menu is missing. Rebuild DockOver.' }
Copy-Item -LiteralPath $appsDir -Destination (Join-Path $programDir 'applications-menu') -Recurse
$sounds = Join-Path $BuildDir 'assets\user-sound'
if (Test-Path -LiteralPath $sounds) {
    New-Item (Join-Path $programDir 'assets') -ItemType Directory | Out-Null
    Copy-Item -LiteralPath $sounds -Destination (Join-Path $programDir 'assets') -Recurse
}
if ($Release) {  # the two programs users start
    foreach ($pair in @(@('DockOver.exe', 'DockOver.exe'), @('DockOverDesktop.exe', 'DockOver Desktop.exe'))) {
        $from = Join-Path $BuildDir $pair[0]
        if (-not (Test-Path -LiteralPath $from)) { throw "$($pair[0]) is missing from $BuildDir." }
        Copy-Item -LiteralPath $from -Destination (Join-Path $stage $pair[1])
    }
}

# manifest.txt: every packaged file. An update replaces exactly these and leaves everything else in the folder.
$files = Get-ChildItem $stage -Recurse -File | ForEach-Object { $_.FullName.Substring($stage.Length + 1).Replace('\', '/') }
[IO.File]::WriteAllText("$stage\manifest.txt", (($files + 'manifest.txt') -join "`n"), (New-Object Text.UTF8Encoding($false)))

# Entries one by one with "/" separators (Windows PowerShell's CreateFromDirectory writes "\").
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = Join-Path $root "dist\$name.zip"
$pending = "$archive.pending-$([guid]::NewGuid().ToString('N'))"
$zip = [IO.Compression.ZipFile]::Open($pending, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entry in $files + 'manifest.txt') {
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$stage\$($entry.Replace('/', '\'))", $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $zip.Dispose() }
if (Test-Path -LiteralPath $archive) { [IO.File]::Replace($pending, $archive, "$archive.previous-$([guid]::NewGuid().ToString('N'))") }
else { [IO.File]::Move($pending, $archive) }
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
$size = [math]::Round((Get-Item -LiteralPath $archive).Length / 1MB, 1)
Write-Host "Packed $name.zip ($size MB, $($files.Count + 1) files, identity $(if ($NoIdentity) { 'off' } else { 'on' }), $(if ($Release) { 'release layout' } else { 'development layout' }))"
Write-Host "SHA-256: $hash"
if ($Release) { Write-Host "Release: tag $version on https://github.com/$repo/releases/new, attach dist\$name.zip" }
