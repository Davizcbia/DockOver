@echo off
set "PREVIEW_EXE=%LOCALAPPDATA%\LiquidDock\preview-build\LiquidDock.exe"
if not exist "%PREVIEW_EXE%" (
    echo Preview build not found: %PREVIEW_EXE%
    echo Build the project before starting the preview.
    pause
    exit /b 1
)
start "" "%PREVIEW_EXE%" --material liquid --backdrop live --studio --glass-config "%LOCALAPPDATA%\LiquidDock\preview-glass.json" --log "%LOCALAPPDATA%\LiquidDock\preview.log"
