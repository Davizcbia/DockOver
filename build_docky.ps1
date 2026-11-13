param([string]$BuildDir = '')
$ErrorActionPreference = 'Stop'
if (-not $BuildDir) { $BuildDir = Join-Path $PSScriptRoot 'build' }
$source = Join-Path $PSScriptRoot 'dockui'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) not found. Install VS 2022 C++ build tools and Windows SDK.' }
$env:Path = "$(Split-Path -Parent $vswhere);$env:Path"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw 'MSVC x64 C++ build tools were not found.' }
$dev = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $dev) -or -not (Test-Path -LiteralPath $cmake)) { throw 'Visual Studio developer environment or bundled CMake is unavailable.' }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$command = 'call "{0}" -arch=x64 >nul && "{1}" -S "{2}" -B "{3}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release && "{1}" --build "{3}"' -f $dev,$cmake,$source,$BuildDir
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }
Write-Host "Build complete. Manager: $(Join-Path $BuildDir 'LiquidDesktopManager.exe')"
Write-Host "Settings: $(Join-Path $BuildDir 'LiquidDesktopSettings.exe')"
Write-Host 'Build only: no application was started and no packs or user settings were changed.'
