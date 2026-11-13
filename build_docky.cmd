@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_docky.ps1" %*
set "rc=%ERRORLEVEL%"
if not "%rc%"=="0" echo Build failed with exit code %rc%.
exit /b %rc%
