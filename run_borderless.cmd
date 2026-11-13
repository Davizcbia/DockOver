@echo off
rem Run the Manager in this window's console. Using "start" to open a second console
rem while this window closes failed intermittently with 0xc0000142 (console startup race).
rem Keep this window open while docky runs; closing it or Ctrl+C quits docky cleanly.
"%~dp0build-borderless\LiquidDesktopManager.exe" %*
