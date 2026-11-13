@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dockui\build.ps1" -Run %*
exit /b %ERRORLEVEL%
