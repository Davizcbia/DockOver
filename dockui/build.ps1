param(
    # Default: one build directory per worktree (name + hash of this source path), so different
    # worktrees never share a CMake cache. -BuildDir still overrides it; other caches are never touched.
    [string]$BuildDir = '',
    [switch]$Run,
    [string]$Config = '',
    [string]$Log = '',
    [switch]$Diag
)
$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
if (-not $BuildDir) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $hash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($source.ToLowerInvariant())) | Select-Object -First 4 | ForEach-Object { $_.ToString('x2') })
    $leaf = Split-Path -Leaf (Split-Path -Parent $source)
    $BuildDir = Join-Path $env:TEMP ("liquid-build-{0}-{1}" -f $leaf, $hash)
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$env:Path = "$(Split-Path -Parent $vswhere);$env:Path"  # VsDevCmd looks up vswhere.exe on PATH
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC build tools were not found.' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path $cmake)) { throw 'Visual Studio CMake was not found.' }
$command = 'call "{0}" -arch=x64 >nul && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_BUILD_TYPE=Release && "{1}" --build "{3}"' -f $dev, $cmake, $source, $BuildDir
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
$exe = Join-Path $BuildDir 'LiquidDesktopManager.exe'
Write-Host "Built docky: $exe"
if ($Run) {
    Write-Host 'Starting docky (Dock and Menu Bar). Windows taskbar remains unchanged by default.'
    $argList = @()
    if ($Config) { $argList += @('--dock-config', ('"' + ($Config -replace '(\\+)$','$1$1') + '"')) }
    if ($Log) { $argList += @('--dock-log', ('"' + ($Log -replace '(\\+)$','$1$1') + '"')) }
    if ($Diag) { $argList += '--dock-diag' }
    if ($argList.Count) { Start-Process -FilePath $exe -ArgumentList $argList } else { Start-Process -FilePath $exe }
}
