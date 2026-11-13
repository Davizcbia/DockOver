# P0.3 Liquid Glass checks on the interactive desktop. Backdrops are SIMULATED (static wallpaper or
# generated test images) until P0.2's WGC capture is integrated. Uses a separate glass config and
# pinned list in OutDir so the user's settings are untouched. Screenshots are cropped to the Dock.
param(
    [string]$Exe = (Join-Path $env:TEMP 'liquid-dock-v01-build\LiquidDock.exe'),
    [string]$OutDir = (Join-Path $env:TEMP 'liquid-dock-glass-verify'),
    [switch]$SkipBenchmark,
    [switch]$MaterialOnly,
    [switch]$StudioOnly
)
$runMaterial = -not $StudioOnly
$runStudio = -not $MaterialOnly
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$config = Join-Path $OutDir 'pinned.txt'
$glassConfig = Join-Path $OutDir 'glass.json'
Remove-Item -ErrorAction SilentlyContinue $glassConfig

Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class GlassTest {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static void Shot(int x, int y, int w, int h, string path) {
        using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(x, y, 0, 0, new Size(w, h));
            bmp.Save(path, ImageFormat.Png);
        }
    }
}
'@
[GlassTest]::SetProcessDPIAware() | Out-Null

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

function Start-Dock([string]$name, [string[]]$extra) {
    $log = Join-Path $OutDir "$name.log"
    Remove-Item -ErrorAction SilentlyContinue $log
    $args = @('--config', "`"$config`"", '--log', "`"$log`"", '--glass-config', "`"$glassConfig`"") + $extra
    [GlassTest]::SetCursorPos(1280, 300) | Out-Null
    $p = Start-Process -FilePath $Exe -ArgumentList $args -PassThru
    Start-Sleep -Milliseconds 2200   # backdrop is built on a worker thread
    $h = [GlassTest]::FindWindow('LiquidDockV01', 'docky')
    if ($h -eq [IntPtr]::Zero) { throw "Dock window not found ($name)" }
    @{ Process = $p; Hwnd = $h; Log = $log }
}
function Stop-Dock($d) {
    [GlassTest]::PostMessage($d.Hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $d.Process.WaitForExit(5000)) { $d.Process.Kill(); return 'killed' }
    return $d.Process.ExitCode
}
function Shot($d, [string]$file) {
    $r = New-Object GlassTest+RECT
    [GlassTest]::GetWindowRect($d.Hwnd, [ref]$r) | Out-Null
    [GlassTest]::Shot($r.Left - 24, $r.Top - 24, ($r.Right - $r.Left) + 48, ($r.Bottom - $r.Top) + 24, (Join-Path $OutDir $file))
}
function Hover($d, [double]$fraction) {
    $r = New-Object GlassTest+RECT
    [GlassTest]::GetWindowRect($d.Hwnd, [ref]$r) | Out-Null
    $x = [int]($r.Left + ($r.Right - $r.Left) * $fraction)
    [GlassTest]::SetCursorPos($x, $r.Bottom - 44) | Out-Null
    Start-Sleep -Milliseconds 700
}

$results = [ordered]@{}
if ($runMaterial) {
foreach ($bk in @('grid', 'wallpaper', 'gradient')) {
    $d = Start-Dock "liquid-$bk" @('--material', 'liquid', '--backdrop', $bk)
    Shot $d "liquid-$bk-idle.png"
    Hover $d 0.42
    Shot $d "liquid-$bk-hover.png"
    $results["liquid-$bk exit"] = Stop-Dock $d
    $results["liquid-$bk material"] = (Select-String -Path $d.Log -Pattern 'material Liquid' -SimpleMatch -Quiet)
}
$d = Start-Dock 'smoked' @('--material', 'smoked', '--backdrop', 'wallpaper')
Shot $d 'smoked-idle.png'
$results['smoked exit'] = Stop-Dock $d
foreach ($v in 1..4) {
    $d = Start-Dock "debug$v" @('--material', 'liquid', '--backdrop', 'grid', '--debug-view', "$v")
    Shot $d "debug$v.png"
    $results["debug$v exit"] = Stop-Dock $d
}
if (-not $SkipBenchmark) {
    foreach ($m in @('liquid', 'smoked')) {
        $d = Start-Dock "bench-$m" @('--material', $m, '--backdrop', 'wallpaper', '--continuous')
        Start-Sleep -Seconds 4
        $results["bench-$m exit"] = Stop-Dock $d
    }
}
}

if ($runStudio) {
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class StudioTest {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
    public static string Text(IntPtr h) { var sb = new StringBuilder(512); SendMessage(h, 0x000D, (IntPtr)512, sb); return sb.ToString(); }
    public static void Click(int x, int y) { SetCursorPos(x, y); System.Threading.Thread.Sleep(80); mouse_event(2,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(50); mouse_event(4,0,0,0,IntPtr.Zero); }
    public static void RightClick(int x, int y) { SetCursorPos(x, y); System.Threading.Thread.Sleep(80); mouse_event(8,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(50); mouse_event(16,0,0,0,IntPtr.Zero); }
    public static void Key(byte vk) { keybd_event(vk,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(25); keybd_event(vk,0,2,IntPtr.Zero); System.Threading.Thread.Sleep(25); }
    public static void Chord(byte mod, byte vk) { keybd_event(mod,0,0,IntPtr.Zero); Key(vk); keybd_event(mod,0,2,IntPtr.Zero); }
    public static void Drag(int x0, int y0, int x1, int y1, int ms) {
        SetCursorPos(x0, y0); System.Threading.Thread.Sleep(60); mouse_event(2,0,0,0,IntPtr.Zero);
        int steps = Math.Max(1, ms / 10);
        for (int i = 1; i <= steps; i++) { SetCursorPos(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps); System.Threading.Thread.Sleep(10); }
        mouse_event(4,0,0,0,IntPtr.Zero);
    }
}
'@
    # Control ids (glass_studio.cpp): panel 118; parameter i -> trackbar 1000+4i+1, edit 1000+4i+2.
    $paramIndex = @{ radius = 0; frost = 3; dispersion = 6; brightness = 21 }
    function Track($panel, $name) { [StudioTest]::GetDlgItem($panel, 1000 + 4 * $paramIndex[$name] + 1) }
    function EditBox($panel, $name) { [StudioTest]::GetDlgItem($panel, 1000 + 4 * $paramIndex[$name] + 2) }
    function Center($h) { $r = New-Object StudioTest+RECT; [StudioTest]::GetWindowRect($h, [ref]$r) | Out-Null; [int](($r.Left + $r.Right) / 2), [int](($r.Top + $r.Bottom) / 2) }
    function Cmd($studio, $id, $code, $ctl) { [StudioTest]::SendMessage($studio, 0x0111, [IntPtr](($code -shl 16) -bor $id), $ctl) | Out-Null }
    function Confirm-Dialog([int]$button) {
        for ($i = 0; $i -lt 30; $i++) {
            Start-Sleep -Milliseconds 100
            $dlg = [StudioTest]::FindWindow('#32770', 'Glass Studio')
            if ($dlg -ne [IntPtr]::Zero) { [StudioTest]::SendMessage([StudioTest]::GetDlgItem($dlg, $button), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; return $true }
        }
        return $false
    }
    function DockShotDiff($a, $b) {
        $x = [System.Drawing.Bitmap]::FromFile($a); $y = [System.Drawing.Bitmap]::FromFile($b); $diff = 0
        for ($j = 0; $j -lt $x.Height; $j += 3) { for ($i = 0; $i -lt $x.Width; $i += 3) { if ($x.GetPixel($i, $j).ToArgb() -ne $y.GetPixel($i, $j).ToArgb()) { $diff++ } } }
        $x.Dispose(); $y.Dispose(); $diff
    }

    Remove-Item -ErrorAction SilentlyContinue $glassConfig, "$glassConfig.corrupt"
    $d = Start-Dock 'studio' @('--material', 'liquid', '--backdrop', 'grid')
    Shot $d 'studio-dock-before.png'
    # Open Glass Studio from the Dock's right-click menu (real input): plate padding, then first item.
    $r = New-Object GlassTest+RECT; [GlassTest]::GetWindowRect($d.Hwnd, [ref]$r) | Out-Null
    $cx = [int](($r.Left + $r.Right) / 2)
    $padX = [int]($cx - (17 + 6 * 60 + 28) / 2 + 4)
    [StudioTest]::RightClick($padX, $r.Bottom - 44)
    Start-Sleep -Milliseconds 400
    [StudioTest]::Key(0x28); [StudioTest]::Key(0x0D)   # Down, Enter -> "Glass Settings..."
    Start-Sleep -Milliseconds 1200
    $studio = [StudioTest]::FindWindow('LiquidDockGlassStudio', [NullString]::Value)
    $results['studio opened from context menu'] = ($studio -ne [IntPtr]::Zero)
    if ($studio -eq [IntPtr]::Zero) { throw 'Glass Studio did not open' }
    $panel = [StudioTest]::GetDlgItem($studio, 118)
    Start-Sleep -Milliseconds 300
    $sr = New-Object StudioTest+RECT; [StudioTest]::GetWindowRect($studio, [ref]$sr) | Out-Null
    [GlassTest]::Shot($sr.Left, $sr.Top, $sr.Right - $sr.Left, $sr.Bottom - $sr.Top, (Join-Path $OutDir 'studio-window.png'))

    # Slider by keyboard (real input): focus frost trackbar, Home -> 0; edit box must follow.
    $tx, $ty = Center (Track $panel 'frost')
    [StudioTest]::Click($tx, $ty); [StudioTest]::Key(0x24)   # Home
    Start-Sleep -Milliseconds 300
    $results['frost slider -> edit'] = [StudioTest]::Text((EditBox $panel 'frost'))
    # Edit box (real typing): radius 38 -> trackbar must move to max (1000).
    $ex, $ey = Center (EditBox $panel 'radius')
    [StudioTest]::Click($ex, $ey); [StudioTest]::Chord(0x11, 0x41); [StudioTest]::Key(0x33); [StudioTest]::Key(0x38); [StudioTest]::Key(0x0D)
    Start-Sleep -Milliseconds 300
    $results['radius edit -> trackbar pos (max 1000)'] = [int][StudioTest]::SendMessage((Track $panel 'radius'), 0x0400, [IntPtr]::Zero, [IntPtr]::Zero)
    # Out-of-range and invalid input are clamped / rejected.
    [StudioTest]::Click($ex, $ey); [StudioTest]::Chord(0x11, 0x41); foreach ($k in 0x39,0x39,0x39) { [StudioTest]::Key($k) }; [StudioTest]::Key(0x0D)
    Start-Sleep -Milliseconds 200
    $results['radius 999 clamped to'] = [StudioTest]::Text((EditBox $panel 'radius'))
    Start-Sleep -Milliseconds 400
    Shot $d 'studio-dock-after-edits.png'
    $results['dock updated live (changed sampled pixels)'] = DockShotDiff (Join-Path $OutDir 'studio-dock-before.png') (Join-Path $OutDir 'studio-dock-after-edits.png')

    # Material switch Liquid -> Smoked -> Liquid via the combo's selection notification.
    $mat = [StudioTest]::GetDlgItem($studio, 100)
    [StudioTest]::SendMessage($mat, 0x014E, [IntPtr]1, [IntPtr]::Zero) | Out-Null; Cmd $studio 100 1 $mat
    Start-Sleep -Milliseconds 500
    Shot $d 'studio-dock-smoked.png'
    $results['switch to Smoked changes dock'] = DockShotDiff (Join-Path $OutDir 'studio-dock-after-edits.png') (Join-Path $OutDir 'studio-dock-smoked.png')
    [StudioTest]::SendMessage($mat, 0x014E, [IntPtr]0, [IntPtr]::Zero) | Out-Null; Cmd $studio 100 1 $mat
    Start-Sleep -Milliseconds 300

    # Presets: save, rename, load built-in, load saved, delete.
    $name = [StudioTest]::GetDlgItem($studio, 106); $combo = [StudioTest]::GetDlgItem($studio, 105)
    [StudioTest]::SendMessage($name, 0x000C, [IntPtr]::Zero, 'Test Preset A') | Out-Null
    [StudioTest]::SendMessage([StudioTest]::GetDlgItem($studio, 108), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # Save
    Start-Sleep -Milliseconds 300
    $results['preset saved in glass.json'] = (Get-Content -Raw $glassConfig) -match 'Test Preset A'
    $count = [int][StudioTest]::SendMessage($combo, 0x0146, [IntPtr]::Zero, [IntPtr]::Zero)
    [StudioTest]::SendMessage($combo, 0x014E, [IntPtr]($count - 1), [IntPtr]::Zero) | Out-Null; Cmd $studio 105 1 $combo
    [StudioTest]::SendMessage($name, 0x000C, [IntPtr]::Zero, 'Renamed Preset B') | Out-Null
    [StudioTest]::SendMessage([StudioTest]::GetDlgItem($studio, 109), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # Rename
    Start-Sleep -Milliseconds 300
    $json = Get-Content -Raw $glassConfig
    $results['preset renamed'] = ($json -match 'Renamed Preset B') -and -not ($json -match 'Test Preset A')
    [StudioTest]::SendMessage($combo, 0x014E, [IntPtr]1, [IntPtr]::Zero) | Out-Null; Cmd $studio 105 1 $combo   # [Built-in] Launcher tuned
    [StudioTest]::SendMessage([StudioTest]::GetDlgItem($studio, 107), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # Load
    Start-Sleep -Milliseconds 300
    $results['load Launcher tuned: brightness'] = [StudioTest]::Text((EditBox $panel 'brightness'))
    $results['load Launcher tuned: frost'] = [StudioTest]::Text((EditBox $panel 'frost'))
    $count = [int][StudioTest]::SendMessage($combo, 0x0146, [IntPtr]::Zero, [IntPtr]::Zero)
    [StudioTest]::SendMessage($combo, 0x014E, [IntPtr]($count - 1), [IntPtr]::Zero) | Out-Null; Cmd $studio 105 1 $combo
    [StudioTest]::SendMessage([StudioTest]::GetDlgItem($studio, 107), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # Load saved
    Start-Sleep -Milliseconds 300
    $results['load saved preset: frost back to'] = [StudioTest]::Text((EditBox $panel 'frost'))
    [StudioTest]::PostMessage([StudioTest]::GetDlgItem($studio, 110), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # Delete (confirm dialog)
    $results['delete confirm dialog answered'] = Confirm-Dialog 6
    Start-Sleep -Milliseconds 300
    $results['preset deleted'] = -not ((Get-Content -Raw $glassConfig) -match 'Renamed Preset B')

    # Save as default, change, reset to default.
    [StudioTest]::PostMessage([StudioTest]::GetDlgItem($studio, 112), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $results['save-as-default dialog answered'] = Confirm-Dialog 2
    [StudioTest]::Click($tx, $ty); [StudioTest]::Key(0x23)   # End -> frost 1
    Start-Sleep -Milliseconds 200
    [StudioTest]::SendMessage([StudioTest]::GetDlgItem($studio, 111), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # Reset
    Start-Sleep -Milliseconds 200
    $results['reset to default: frost'] = [StudioTest]::Text((EditBox $panel 'frost'))

    # Paste Glass Lab JSON (v3 keys) from the clipboard.
    Set-Clipboard -Value '{"version":3,"frost":0.12,"dispersion":0.2,"refraction":0.9,"staticReflection":true,"radius":45}'
    [StudioTest]::SendMessage([StudioTest]::GetDlgItem($studio, 113), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 300
    $results['paste Lab JSON: frost/dispersion/radius(ignored)'] = ([StudioTest]::Text((EditBox $panel 'frost')) + ' / ' + [StudioTest]::Text((EditBox $panel 'dispersion')) + ' / ' + [StudioTest]::Text((EditBox $panel 'radius')))
    Set-Clipboard -Value '{"version":3,"frost":"bad"}'
    [StudioTest]::PostMessage([StudioTest]::GetDlgItem($studio, 113), 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $results['invalid Lab JSON rejected (warning shown)'] = Confirm-Dialog 2
    $results['invalid JSON left frost unchanged'] = [StudioTest]::Text((EditBox $panel 'frost'))

    # Performance while tuning: continuous render on, hold a slider key and move the Studio window.
    $cont = [StudioTest]::GetDlgItem($studio, 104)
    [StudioTest]::SendMessage($cont, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 1500
    $dx, $dy = Center (Track $panel 'dispersion')
    [StudioTest]::Click($dx, $dy)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 2500) { [StudioTest]::Key(0x27) }   # Right arrow: continuous slider changes
    [StudioTest]::Drag($sr.Left + 200, $sr.Top + 12, $sr.Left - 200, $sr.Top + 60, 2500)   # move the window by its title bar
    Start-Sleep -Milliseconds 1200
    [StudioTest]::SendMessage($cont, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 300

    # Close Studio: Dock keeps running and the parameters persist.
    [StudioTest]::PostMessage($studio, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
    $results['studio closed'] = -not [StudioTest]::IsWindow($studio)
    $results['dock alive after studio closed'] = [StudioTest]::IsWindow($d.Hwnd) -and -not $d.Process.HasExited
    $results['persisted frost in glass.json'] = [math]::Abs((Get-Content -Raw $glassConfig | ConvertFrom-Json).current.frost - 0.12) -lt 1e-4
    $results['studio dock exit'] = Stop-Dock $d

    # Corrupt settings file: safe fallback to defaults, original kept as .corrupt.
    Set-Content -LiteralPath $glassConfig -Value '{ "current": { "frost": 0.5, ' -Encoding UTF8
    $d = Start-Dock 'corrupt' @()
    $results['corrupt config backed up'] = Test-Path "$glassConfig.corrupt"
    $results['corrupt config logged'] = (Select-String -Path $d.Log -Pattern 'invalid' -SimpleMatch -Quiet)
    $results['corrupt dock exit'] = Stop-Dock $d
}
Write-Host '---- results'
$results.GetEnumerator() | ForEach-Object { Write-Host ("{0}: {1}" -f $_.Key, $_.Value) }
