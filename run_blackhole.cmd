@echo off
rem DockOver Black Hole Visualizer: standalone test entry for the future DockDrop visuals (no file transfer).
rem It does not start or change the Dock, Menu Bar, Control Center or Now Playing.
rem Close it with the panel's close button, or run:  run_blackhole.cmd --quit
rem Options: dockui\docs\blackhole-visualizer-v01.md
start "" "%~dp0build-blackhole\docky-blackhole.exe" %*
