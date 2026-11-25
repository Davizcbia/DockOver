@echo off
if "%~1"=="" exit /b 2
cl /nologo /std:c++20 /O2 /W4 /EHsc /MT /Fo"%~1\WifiDirectHost.obj" /Fe"%~1\WifiDirectHost.exe" "%~dp0WifiDirectHost.cpp" windowsapp.lib
