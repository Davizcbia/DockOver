param(
    [string]$BuildDir = (Join-Path $env:TEMP 'dock-p01-diagnostic'),
    [string]$LogPath = (Join-Path $env:LOCALAPPDATA 'DockPrototype/diagnostic.log')
)
$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC build tools were not found.' }
$dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path $cmake)) { throw 'Visual Studio CMake was not found.' }
New-Item -ItemType Directory -Force (Split-Path -Parent $LogPath) | Out-Null
$env:DOCK_DIAGNOSTIC_LOG = $LogPath
$command = 'call "{0}" -arch=x64 >nul && "{1}" -S "{2}" -B "{3}" -G Ninja && "{1}" --build "{3}"' -f $dev, $cmake, $source, $BuildDir
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
Write-Host "Log: $LogPath"
Write-Host 'F1 toggles the alpha mask, F2 toggles a checkerboard behind the window, Esc exits. Alpha samples are logged in F1 mode.'
$process = Start-Process -FilePath (Join-Path $BuildDir 'DockPrototype.exe') -PassThru
$process.WaitForExit()
Write-Host "Process exit code: $($process.ExitCode)"
Add-Content -LiteralPath $LogPath -Value "Launcher observed exit code: $($process.ExitCode)"
