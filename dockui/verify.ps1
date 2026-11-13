# Interactive-desktop acceptance run for Liquid Dock V0.1.
# Uses real cursor/keyboard input (SetCursorPos, mouse_event, keybd_event) and crops screenshots to the
# Dock window only. It launches Notepad from the Dock and closes only the Notepad processes it created.
param(
    [string]$Exe = (Join-Path $env:TEMP 'liquid-dock-v01-build\LiquidDock.exe'),
    [string]$OutDir = (Join-Path $env:TEMP 'liquid-dock-verify')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$log = Join-Path $OutDir 'dock.log'
$config = Join-Path $OutDir 'pinned.txt'
Remove-Item -ErrorAction SilentlyContinue $log
$results = [ordered]@{}

Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class DockTest {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    public static void Click() { mouse_event(0x2, 0, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(60); mouse_event(0x4, 0, 0, 0, IntPtr.Zero); }
    public static void Key(byte vk) { keybd_event(vk, 0, 0, IntPtr.Zero); System.Threading.Thread.Sleep(40); keybd_event(vk, 0, 2, IntPtr.Zero); }
    public static IntPtr TopAt(int x, int y) { POINT p; p.X = x; p.Y = y; return GetAncestor(WindowFromPoint(p), 2); }
    public static void Shot(int x, int y, int w, int h, string path) {
        using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(x, y, 0, 0, new Size(w, h));
            bmp.Save(path, ImageFormat.Png);
        }
    }
}
'@
[DockTest]::SetProcessDPIAware() | Out-Null

function Get-DockRect($hwnd) { $r = New-Object DockTest+RECT; [DockTest]::GetWindowRect($hwnd, [ref]$r) | Out-Null; $r }
function Shot($name) {
    $r = Get-DockRect $script:hwnd
    $pad = 24
    $path = Join-Path $OutDir $name
    [DockTest]::Shot($r.Left - $pad, $r.Top - $pad, ($r.Right - $r.Left) + 2 * $pad, ($r.Bottom - $r.Top) + $pad, $path)
    Write-Host "screenshot $path"
}
# V0.2 resting geometry (DIP == px at 96 DPI): icon tile 60, separator tile 28, plate padding to first tile 8.5,
# icon centre 5.5 + 15 + 23 above the window bottom.
function TileCenters($tiles) {
    $w = 17.0; foreach ($t in $tiles) { $w += $(if ($t -eq 's') { 28 } else { 60 }) }
    $edge = 8.5; $centers = @()
    foreach ($t in $tiles) { $tw = $(if ($t -eq 's') { 28 } else { 60 }); $centers += ($edge + $tw / 2.0); $edge += $tw }
    @{ Width = $w; Centers = $centers }
}
function ItemCenter($index, $tiles) {
    $r = Get-DockRect $script:hwnd
    $layout = TileCenters $tiles
    $left = ($r.Left + $r.Right) / 2.0 - $layout.Width / 2.0
    [int][math]::Round($left + $layout.Centers[$index]), [int][math]::Round($r.Bottom - 5.5 - 15 - 23)
}
function PlatePadding($tiles) {
    $r = Get-DockRect $script:hwnd
    $layout = TileCenters $tiles
    [int][math]::Round(($r.Left + $r.Right) / 2.0 - $layout.Width / 2.0 + 5), [int][math]::Round($r.Bottom - 5.5 - 38)
}

# Deterministic test configuration (real apps present on every Windows 11 machine).
$targets = @(
    "$env:WINDIR\explorer.exe",
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
    "$env:WINDIR\System32\notepad.exe",
    '---',
    'shell:AppsFolder\Microsoft.WindowsCalculator_8wekyb3d8bbwe!App',
    'shell:AppsFolder\windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel',
    "$env:WINDIR\System32\cmd.exe"
) | Where-Object { $_ -eq '---' -or $_ -like 'shell:*' -or (Test-Path $_) }
[IO.File]::WriteAllLines($config, [string[]]$targets, (New-Object Text.UTF8Encoding $false))
$tiles = @($targets | ForEach-Object { if ($_ -eq '---') { 's' } else { 'i' } })
$iconIdx = @(for ($k = 0; $k -lt $tiles.Count; $k++) { if ($tiles[$k] -eq 'i') { $k } })
$notepadIndex = [array]::IndexOf([string[]]$targets, "$env:WINDIR\System32\notepad.exe")

[DockTest]::SetCursorPos(1280, 300) | Out-Null
$proc = Start-Process -FilePath $Exe -ArgumentList @('--config', "`"$config`"", '--log', "`"$log`"") -PassThru
Start-Sleep -Milliseconds 1500
$script:hwnd = [DockTest]::FindWindow('LiquidDockV01', 'docky')
if ($script:hwnd -eq [IntPtr]::Zero) { throw 'Dock window not found' }
$r = Get-DockRect $script:hwnd
$results['window'] = "($($r.Left),$($r.Top))-($($r.Right),$($r.Bottom))"
Shot '01-idle.png'

# Idle click-through: a point inside the window rectangle but outside the plate must hit another window.
$probe = [DockTest]::TopAt($r.Left + 4, $r.Top + 4)
$results['idle click-through outside plate'] = ($probe -ne $script:hwnd)
$cx, $cy = ItemCenter 0 $tiles
$results['plate hit-test inside'] = ([DockTest]::TopAt($cx, $cy) -eq $script:hwnd)

# Give the dock keyboard focus by clicking its plate padding (not an icon) so F-keys reach it.
$fx, $fy = PlatePadding $tiles
[DockTest]::SetCursorPos($fx, $fy) | Out-Null
Start-Sleep -Milliseconds 200
[DockTest]::Click()
Start-Sleep -Milliseconds 300

# Hover sweep across the dock (continuous real-time animation).
$x0, $y = ItemCenter $iconIdx[0] $tiles
$x1, $y = ItemCenter $iconIdx[-1] $tiles
for ($i = 0; $i -le 120; $i++) {
    [DockTest]::SetCursorPos([int]($x0 + ($x1 - $x0) * $i / 120), $y) | Out-Null
    Start-Sleep -Milliseconds 12
    if ($i -eq 60) { Shot '02-hover-moving.png' }
}
$nx, $ny = ItemCenter $notepadIndex $tiles
[DockTest]::SetCursorPos($nx, $ny) | Out-Null
Start-Sleep -Milliseconds 600
[DockTest]::Key(0x73)  # F4: dump magnified geometry for comparison with the macOS captures
Shot '03-hover-notepad.png'
$results['hover full-window hit-test'] = ([DockTest]::TopAt($r.Left + 30, $r.Top + 30) -eq $script:hwnd)

# Click to launch Notepad; close only the new processes afterwards.
$before = @(Get-Process -Name notepad -ErrorAction SilentlyContinue | ForEach-Object Id)
[DockTest]::Click()
Start-Sleep -Milliseconds 260
Shot '04-launch-bounce.png'
Start-Sleep -Milliseconds 2500
$new = @(Get-Process -Name notepad -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id })
$results['click launched notepad (new pids)'] = ($new | ForEach-Object Id) -join ','
$results['click launched notepad'] = ($new.Count -gt 0)
Start-Sleep -Milliseconds 1200  # allow the 1 s running poll to pick it up
Shot '05-running-indicator.png'
foreach ($p in $new) { $p.CloseMainWindow() | Out-Null }
Start-Sleep -Milliseconds 800
foreach ($p in $new) { if (-not $p.HasExited) { $p.Kill() } }

# Leave and settle back to idle (region shrinks back to the plate).
[DockTest]::SetCursorPos(1280, 300) | Out-Null
Start-Sleep -Milliseconds 900
Shot '06-after-leave.png'
$results['idle click-through after leave'] = ([DockTest]::TopAt($r.Left + 4, $r.Top + 4) -ne $script:hwnd)

# Resize: add an item to the config, focus the dock via its plate padding, press F3.
Add-Content -LiteralPath $config -Value "$env:WINDIR\System32\mstsc.exe" -Encoding UTF8
$px, $py = PlatePadding $tiles   # left plate padding, not an icon
[DockTest]::SetCursorPos($px, $py) | Out-Null
Start-Sleep -Milliseconds 300
[DockTest]::Click()
Start-Sleep -Milliseconds 300
[DockTest]::Key(0x72)  # F3
Start-Sleep -Milliseconds 1200
$r2 = Get-DockRect $script:hwnd
$results['resized window'] = "($($r2.Left),$($r2.Top))-($($r2.Right),$($r2.Bottom))"
$results['window grew with item count'] = (($r2.Right - $r2.Left) -gt ($r.Right - $r.Left))
[DockTest]::SetCursorPos(1280, 300) | Out-Null
Start-Sleep -Milliseconds 900
Shot '07-resized-idle.png'

# F1 diagnostics overlay on, screenshot, off.
$tiles += 'i'
$px, $py = PlatePadding $tiles
[DockTest]::SetCursorPos($px, $py) | Out-Null
Start-Sleep -Milliseconds 200
[DockTest]::Click()
Start-Sleep -Milliseconds 200
[DockTest]::Key(0x70)  # F1
[DockTest]::Key(0x73)  # F4 state dump
Start-Sleep -Milliseconds 700
Shot '08-diag-f1.png'
[DockTest]::Key(0x70)  # F1 off

# Escape closes the dock.
[DockTest]::Key(0x1B)
$exited = $proc.WaitForExit(5000)
$results['escape closed process'] = $exited
if ($exited) { $results['exit code'] = $proc.ExitCode } else { $proc.Kill(); $results['exit code'] = 'killed' }
[DockTest]::SetCursorPos(1280, 300) | Out-Null

Write-Host '---- results'
$results.GetEnumerator() | ForEach-Object { Write-Host ("{0}: {1}" -f $_.Key, $_.Value) }
Write-Host "log: $log"
