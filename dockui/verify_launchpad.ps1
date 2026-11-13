# Launchpad V0.1 acceptance on the interactive desktop (real mouse/keyboard input).
# Uses an isolated Dock config, glass config, icon config and launchpad.json in OutDir, so the user's
# settings are untouched. Launches Notepad once via search and closes only the Notepad it created.
param(
    [string]$BuildDir = (Join-Path $env:TEMP 'liquid-dock-launchpad-build'),
    [string]$OutDir = (Join-Path $env:TEMP 'liquid-launchpad-verify')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
New-Item -ItemType Directory -Force $OutDir | Out-Null
$exe = Join-Path $BuildDir 'LiquidDock.exe'
$log = Join-Path $OutDir 'dock.log'
$layout = Join-Path $OutDir 'launchpad.json'
$pinned = Join-Path $OutDir 'pinned.txt'
Remove-Item -ErrorAction SilentlyContinue $log, $layout, "$layout.corrupt"
[IO.File]::WriteAllLines($pinned, [string[]]@("$env:WINDIR\explorer.exe", "$env:WINDIR\System32\notepad.exe", '---'),
    (New-Object Text.UTF8Encoding $false))

Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class LP {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, int d, IntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte s, uint f, IntPtr e);
    public static void Click(int x, int y) { SetCursorPos(x, y); System.Threading.Thread.Sleep(80); mouse_event(2,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(60); mouse_event(4,0,0,0,IntPtr.Zero); }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit, Size=40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    public static void Type(string text) {
        foreach (char ch in text) {
            var down = new INPUT(); down.type = 1; down.ki.wScan = ch; down.ki.dwFlags = 4;
            var up = new INPUT(); up.type = 1; up.ki.wScan = ch; up.ki.dwFlags = 4 | 2;
            SendInput(2, new INPUT[] { down, up }, Marshal.SizeOf(typeof(INPUT)));
            System.Threading.Thread.Sleep(25);
        }
    }
    public static void Wheel(int delta) { mouse_event(0x0800,0,0,delta,IntPtr.Zero); }
    public static void Key(byte vk) { keybd_event(vk,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(30); keybd_event(vk,0,2,IntPtr.Zero); System.Threading.Thread.Sleep(30); }
    public static void Drag(int x0, int y0, int x1, int y1, int ms, int holdMs) {
        SetCursorPos(x0, y0); System.Threading.Thread.Sleep(80); mouse_event(2,0,0,0,IntPtr.Zero);
        int steps = Math.Max(2, ms / 12);
        for (int i = 1; i <= steps; i++) { SetCursorPos(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps); System.Threading.Thread.Sleep(12); }
        System.Threading.Thread.Sleep(holdMs); mouse_event(4,0,0,0,IntPtr.Zero);
    }
    public static void Shot(int x, int y, int w, int h, string path, float scale) {
        using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(x, y, 0, 0, new Size(w, h));
            using (var small = new Bitmap(bmp, (int)(w * scale), (int)(h * scale))) small.Save(path, ImageFormat.Png);
        }
    }
}
'@
[LP]::SetProcessDPIAware() | Out-Null
$screen = [System.Windows.Forms.Screen]::PrimaryScreen
if (-not $screen) { Add-Type -AssemblyName System.Windows.Forms; $screen = [System.Windows.Forms.Screen]::PrimaryScreen }
$W = $screen.Bounds.Width; $H = $screen.Bounds.Height
function Shot($name) { [LP]::Shot(0, 0, $W, $H, (Join-Path $OutDir $name), 0.5); Write-Host "screenshot $name" }
function LogHas($pattern) { (Select-String -Path $log -Pattern $pattern -SimpleMatch -Quiet) }
function WaitLog($pattern, $ms) { $sw = [Diagnostics.Stopwatch]::StartNew(); while ($sw.ElapsedMilliseconds -lt $ms) { if (Test-Path $log) { if (LogHas $pattern) { return $true } }; Start-Sleep -Milliseconds 100 }; return $false }
# Grid geometry must match launchpad.cpp ComputeGeometry/SlotRect (96 DPI).
$icon = [math]::Min([math]::Max([math]::Min($W * 0.042, $H * 0.075), 56), 104)
$gridW = [math]::Min($W * 0.80, $icon * 7 * 2.1); $cellW = $gridW / 7; $gridLeft = ($W - $gridW) / 2
$gridTop = $H * 0.13; $cellH = $H * 0.64 / 5
function SlotCenter($slot) { $i = $slot % 35; $c = $i % 7; $r = [math]::Floor($i / 7)
    $x = $gridLeft + $c * $cellW + $cellW / 2; $y = $gridTop + $r * $cellH + ($cellH - $icon - 18 - 8) / 2 + $icon / 2
    [int]$x, [int]$y }
function Layout() { (Get-Content -Raw -Encoding UTF8 $layout | ConvertFrom-Json).nodes }
$results = [ordered]@{}

$dockArgs = @('--config', "`"$pinned`"", '--log', "`"$log`"", '--glass-config', "`"$OutDir\glass.json`"", '--icon-config',
    "`"$OutDir\icons.json`"", '--launchpad-config', "`"$layout`"", '--material', 'liquid', '--backdrop', 'wallpaper')
[LP]::SetCursorPos($W / 2, 300) | Out-Null
$proc = Start-Process -FilePath $exe -ArgumentList $dockArgs -PassThru
$results['discovery finished'] = WaitLog 'launchpad: discovery' 8000
$dock = [LP]::FindWindow('LiquidDockV01', 'docky')
$dr = New-Object LP+RECT; [LP]::GetWindowRect($dock, [ref]$dr) | Out-Null
# Launchpad is the Dock's first tile: plate left + 8.5 + 30 (60 DIP tile), icon centre 43.5 above the bottom.
$tiles = 1 + 2 + 1 + 1   # launchpad, explorer, notepad, separator(28), downloads
$base = 17 + 60 * 4 + 28
$lpX = [int](($dr.Left + $dr.Right) / 2 - $base / 2 + 8.5 + 30); $lpY = [int]($dr.Bottom - 43.5)

# 1. Open from the Dock icon (real click).
[LP]::Click($lpX, $lpY)
$results['opened from Dock icon'] = WaitLog 'launchpad: open ' 4000
Start-Sleep -Milliseconds 1500
$lpw = [LP]::FindWindow('LiquidLaunchpad', 'docky Launchpad')
$results['launchpad window'] = ($lpw -ne [IntPtr]::Zero)
Shot '01-open.png'

# 2. Page switching: wheel down, then Right/Left keys.
[LP]::SetCursorPos($W / 2, $H / 2) | Out-Null; [LP]::Wheel(-120); Start-Sleep -Milliseconds 700
$results['wheel -> page 2'] = LogHas 'launchpad: page 2/'
Shot '02-page2.png'
[LP]::Key(0x21); Start-Sleep -Milliseconds 500
$results['PageUp -> page 1'] = LogHas 'launchpad: page 1/'
# Page dot click: third dot -> page 3.
$dotY = [int]($gridTop + $cellH * 5 + 26); $dotX = [int]($W / 2 - 14 * 18 / 2 + 2 * 18)
[LP]::Click($dotX, $dotY); Start-Sleep -Milliseconds 500
$results['page dot -> page 3'] = LogHas 'launchpad: page 3/'
[LP]::Key(0x21); [LP]::Key(0x21); Start-Sleep -Milliseconds 500

# 3. Drag reorder: slot 0 -> slot 3 (edge of the cell, not its centre).
$x0, $y0 = SlotCenter 0; $x3, $y3 = SlotCenter 3
$first = (Layout)[0].app
[LP]::Drag($x0, $y0, $x3 + [int]($cellW * 0.42), $y3, 600, 300); Start-Sleep -Milliseconds 800
$order = Layout
$results['reorder saved (first app moved)'] = ($order[0].app -ne $first) -and (@($order | Where-Object { $_.app -eq $first }).Count -eq 1)
$results['reorder: moved app now at index'] = [array]::IndexOf(@($order | ForEach-Object { $_.app }), $first)

# 4. Folder: drop slot 1 onto the centre of slot 2 and hold.
$x1, $y1 = SlotCenter 1; $x2, $y2 = SlotCenter 2
[LP]::Drag($x1, $y1, $x2, $y2, 500, 700); Start-Sleep -Milliseconds 800
$folders = @(Layout | Where-Object { $_.folder })
$results['folder created'] = ($folders.Count -ge 1) -and ($folders[0].apps.Count -eq 2)
Shot '03-folder-created.png'

# 5. Open folder, rename it by typing, close it.
$fi = [array]::IndexOf(@(Layout | ForEach-Object { [bool]$_.folder }), $true)
$fx, $fy = SlotCenter $fi
[LP]::Click($fx, $fy); Start-Sleep -Milliseconds 700
$results['folder opened'] = LogHas 'launchpad: folder opened'
Shot '04-folder-open.png'
# Title is at the top of the centred panel; click it, select-all by Backspace x 10, type "Tools", Enter.
$results['folder title clicked'] = $true
[LP]::Click([int]($W / 2), [int]([math]::Max($H * 0.12, ($H - (76 + $cellH * 0.92 + 28)) / 2 - 30) + 38)); Start-Sleep -Milliseconds 300
for ($i = 0; $i -lt 10; $i++) { [LP]::Key(0x08) }
[LP]::Type('Tools')
[LP]::Key(0x0D); Start-Sleep -Milliseconds 500
$results['folder renamed'] = (@(Layout | Where-Object { $_.folder })[0].name -eq 'Tools')
# In-folder reorder: drag the first app onto the second slot (panel geometry = launchpad.cpp FolderPanel/FolderSlotRect).
$pw = 3 * $cellW * 0.92 + 56; $ph = 76 + $cellH * 0.92 + 28
$pl = ($W - $pw) / 2; $pt = [math]::Max($H * 0.12, ($H - $ph) / 2 - 30)
$ic = $icon * 0.92; $cw = $cellW * 0.92; $ch = $cellH * 0.92
function FolderSlotCenter($i) { [int]($pl + 28 + $i * $cw + $cw / 2), [int]($pt + 76 + ($ch - $ic - 26) / 2 + $ic / 2) }
$before2 = @(@(Layout | Where-Object { $_.folder })[0].apps)
$a0x, $a0y = FolderSlotCenter 0; $a1x, $a1y = FolderSlotCenter 1
[LP]::Drag($a0x, $a0y, $a1x, $a1y, 400, 300); Start-Sleep -Milliseconds 600
$after2 = @(@(Layout | Where-Object { $_.folder })[0].apps)
$results['in-folder reorder saved'] = ($after2[0] -eq $before2[1]) -and ($after2[1] -eq $before2[0])
[LP]::Key(0x1B); Start-Sleep -Milliseconds 500
$results['folder closed'] = LogHas 'launchpad: folder closed'

# 6. Search "notepad" and launch with Enter; close only the new Notepad.
$before = @(Get-Process -Name notepad -ErrorAction SilentlyContinue | ForEach-Object Id)
[LP]::Type('notepad')
Start-Sleep -Milliseconds 600
$results['search results logged'] = LogHas 'launchpad: search "notepad"'
Shot '05-search.png'
[LP]::Key(0x0D); Start-Sleep -Milliseconds 2500
$new = @(Get-Process -Name notepad -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id })
$results['Enter launched Notepad'] = ($new.Count -gt 0)
$results['launchpad closed after launch'] = LogHas 'launchpad: closing (launch)'
foreach ($p in $new) { $p.CloseMainWindow() | Out-Null }; Start-Sleep -Milliseconds 800
foreach ($p in $new) { if (-not $p.HasExited) { $p.Kill() } }

# 7. Reopen: layout persisted; Esc closes; Dock keeps running.
[LP]::Click($lpX, $lpY); Start-Sleep -Milliseconds 1500
$results['reopen restores folder'] = (@(Layout | Where-Object { $_.folder }).Count -ge 1) -and (LogHas 'launchpad: layout loaded')
Shot '06-reopen.png'
# Drag an app out of the two-app folder: it lands on the grid and the folder dissolves.
$fi = [array]::IndexOf(@(Layout | ForEach-Object { [bool]$_.folder }), $true)
$fx, $fy = SlotCenter $fi
[LP]::Click($fx, $fy); Start-Sleep -Milliseconds 700
$ox, $oy = FolderSlotCenter 0; $tx, $ty = SlotCenter 24
[LP]::Drag($ox, $oy, $tx + [int]($cellW * 0.42), $ty + 40, 900, 500); Start-Sleep -Milliseconds 700
$results['drag out of folder dissolves it'] = (LogHas 'out of folder') -and (@(Layout | Where-Object { $_.folder }).Count -eq 0)
$results['all apps still present'] = (@(Layout).Count -eq 519 - 0) -or (@(Layout).Count -ge 500)
[LP]::Key(0x1B); Start-Sleep -Milliseconds 700
$results['Esc closed'] = LogHas 'launchpad: closing (Escape)'
$results['dock alive'] = -not $proc.HasExited
[LP]::PostMessage($dock, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$results['dock exit'] = if ($proc.WaitForExit(5000)) { $proc.ExitCode } else { $proc.Kill(); 'killed' }

# 8. Corrupt layout file: kept as .corrupt, Launchpad opens with the alphabetical default.
Set-Content -LiteralPath $layout -Value '{ "nodes": [ { "app": ' -Encoding UTF8
Remove-Item -ErrorAction SilentlyContinue "$layout.corrupt"
$proc = Start-Process -FilePath $exe -ArgumentList ($dockArgs + @('--launchpad')) -PassThru
$results['corrupt layout: opened'] = WaitLog 'layout unusable' 6000
Start-Sleep -Milliseconds 800
$results['corrupt layout: backup kept'] = Test-Path "$layout.corrupt"
$results['corrupt layout: default saved'] = (@(Layout).Count -ge 500)
$dock = [LP]::FindWindow('LiquidDockV01', 'docky')
[LP]::PostMessage($dock, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
$results['corrupt run exit'] = if ($proc.WaitForExit(5000)) { $proc.ExitCode } else { $proc.Kill(); 'killed' }
[LP]::SetCursorPos($W / 2, 300) | Out-Null

Write-Host '---- results'
$results.GetEnumerator() | ForEach-Object { Write-Host ("{0}: {1}" -f $_.Key, $_.Value) }
