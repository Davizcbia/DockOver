@echo off
rem DockDrop pairing orb: standalone visual prototype (no networking; starts or changes nothing else in DockOver).
rem Space = simulate a successful scan, R = new code, Esc = quit. Options: dockui\docs\dockdrop-pairing.md
start "" "%~dp0build-orb\docky-orb.exe" %*
