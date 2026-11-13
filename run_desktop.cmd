@echo off
rem DockOver Desktop: desktop widgets and file Stacks. A separate program from DockOver (Dock + Menu Bar);
rem it runs with or without DockOver. Running it again opens the widget gallery of the running copy.
rem Quit it from a widget's right-click menu or its tray icon (in DockOver's Hidden Icons, or the Windows notification area).
start "" "%~dp0build-desktop\DockOverDesktop.exe" %*
