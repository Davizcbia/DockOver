@echo off
setlocal
cd /d "%~dp0"
set "QT_LAB_DIR=C:\Qt\6.12.0\msvc2022_64"
if not exist "%QT_LAB_DIR%\bin\Qt6Core.dll" (
    echo Qt 6.12.0 was not found at "%QT_LAB_DIR%".
    goto failed
)
set "PATH=%QT_LAB_DIR%\bin;C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;%PATH%"
rem Do not let another application's Qt plugin or QML paths override this Qt.
set "QT_PLUGIN_PATH="
set "QT_QPA_PLATFORM_PLUGIN_PATH="
set "QML2_IMPORT_PATH="
set "QML_IMPORT_PATH="
set "LAB_BUILD=%CD%\build"
if not exist "%LAB_BUILD%" mkdir "%LAB_BUILD%"
qsb --glsl "100 es,120,150" --hlsl 50 --msl 12 -o "%LAB_BUILD%\sidebarglow.frag.qsb" "%CD%\..\core\launcher\project-glass\shaders\sidebarglow.frag"
if errorlevel 1 goto failed
if exist "%LAB_BUILD%\LiquidGlassLab.exe" if /i not "%~1"=="--build" if /i not "%~1"=="--no-run" goto run
where cl >nul 2>nul
if errorlevel 1 call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S "%CD%" -B "%LAB_BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/msvc2022_64
if errorlevel 1 goto failed
cmake --build "%LAB_BUILD%" --parallel 8
if errorlevel 1 goto failed
:run
if /i "%~1"=="--no-run" exit /b 0
set "QT_QUICK_BACKEND=rhi"
set "QSG_RHI_BACKEND=d3d11"
if /i "%~1"=="--smoke" goto smoke
start "" "%LAB_BUILD%\LiquidGlassLab.exe"
exit /b 0
:smoke
start "" /wait "%LAB_BUILD%\LiquidGlassLab.exe" --smoke
exit /b %ERRORLEVEL%
:failed
echo Glass Lab build failed.
if /i "%~1"=="--no-run" exit /b 1
pause
exit /b 1
