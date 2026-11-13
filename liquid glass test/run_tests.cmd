@echo off
rem Runs the Glass Lab QML tests against the current shader source: the
rem shaders are compiled into core\launcher\project-glass\tests\.qsb first.
rem Results: build\qml-test-results.txt (also printed).
setlocal
cd /d "%~dp0"
call "%~dp0..\core\launcher\project-glass\tests\compile_shaders.cmd" || exit /b 1
set "PATH=C:\Qt\6.12.0\msvc2022_64\bin;%PATH%"
set "QSG_RHI_BACKEND=d3d11"
if not exist build mkdir build
qmltestrunner -input tests\tst_GlassLab.qml -o build\qml-test-results.txt,txt
set "RESULT=%ERRORLEVEL%"
type build\qml-test-results.txt
exit /b %RESULT%
