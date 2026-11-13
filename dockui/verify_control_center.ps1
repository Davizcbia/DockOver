# Control Center V0.1 acceptance on the interactive desktop (real mouse/keyboard input).
# Changes only volume, mute and brightness, and restores all three at the end (also on failure).
# Never switches Wi-Fi/Bluetooth radios or Wi-Fi connections. The panel is excluded from screen
# capture (WDA_EXCLUDEFROMCAPTURE), so visuals are checked through its own GPU snapshots.
param(
    [string]$BuildDir = (Join-Path $env:TEMP 'liquid-dock-launchpad-build'),
    [string]$OutDir = (Join-Path $env:TEMP 'liquid-control-center-verify')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
New-Item -ItemType Directory -Force $OutDir | Out-Null
$bar = Join-Path $BuildDir 'LiquidMenuBar.exe'
$tool = Join-Path $BuildDir 'LiquidSystemControlsTest.exe'
$log = Join-Path $OutDir 'menu.log'
$shots = Join-Path $OutDir 'snapshots'
New-Item -ItemType Directory -Force $shots | Out-Null
Remove-Item -ErrorAction SilentlyContinue $log

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CC {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, int d, IntPtr e);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte s, uint f, IntPtr e);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static void Click(int x, int y) { SetCursorPos(x, y); System.Threading.Thread.Sleep(90); mouse_event(2,0,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(60); mouse_event(4,0,0,0,IntPtr.Zero); }
    public static void Key(byte vk) { keybd_event(vk,0,0,IntPtr.Zero); System.Threading.Thread.Sleep(30); keybd_event(vk,0,2,IntPtr.Zero); }
    public static void Drag(int x0, int y, int x1, int ms) {
        SetCursorPos(x0, y); System.Threading.Thread.Sleep(90); mouse_event(2,0,0,0,IntPtr.Zero);
        int steps = Math.Max(2, ms / 12);
        for (int i = 1; i <= steps; i++) { SetCursorPos(x0 + (x1 - x0) * i / steps, y); System.Threading.Thread.Sleep(12); }
        System.Threading.Thread.Sleep(120); mouse_event(4,0,0,0,IntPtr.Zero);
    }
}
'@
[CC]::SetProcessDPIAware() | Out-Null

$results = New-Object System.Collections.Generic.List[string]
$failed = 0
function Report([string]$name, [bool]$ok, [string]$detail = '') {
    $script:results.Add(('{0} {1} {2}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name, $detail))
    if (-not $ok) { $script:failed++ }
    Write-Host ('{0} {1} {2}' -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name, $detail)
}
function LogLines { if (Test-Path $log) { @(Get-Content $log -Encoding UTF8) } else { @() } }
function Count([string]$pattern) { @(LogLines | Where-Object { $_ -match $pattern }).Count }
function WaitFor([string]$pattern, [int]$atLeast = 1, [int]$ms = 4000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ms) { if ((Count $pattern) -ge $atLeast) { return $true }; Start-Sleep -Milliseconds 100 }
    return $false
}
function LastMatch([string]$pattern) { $m = LogLines | Where-Object { $_ -match $pattern } | Select-Object -Last 1; if ($m) { [regex]::Match($m, $pattern) } }
function ReadAudio {
    $o = & $tool --read
    $m = [regex]::Match(($o -join ' '), 'volume=(\d+) muted=(\d) brightness=(\d)/(\d+)')
    [pscustomobject]@{ Volume = [int]$m.Groups[1].Value; Muted = $m.Groups[2].Value -eq '1'; Brightness = $m.Groups[3].Value -eq '1'; Percent = [int]$m.Groups[4].Value }
}

$orig = ReadAudio
Write-Host "original: volume=$($orig.Volume) muted=$($orig.Muted) brightness=$($orig.Brightness)/$($orig.Percent)"
$env:LIQUID_CC_SNAPSHOT_DIR = $shots
$proc = Start-Process $bar -ArgumentList '--log', $log, '--exit-after', '180' -PassThru
$form = $null
try {
    Report 'menu bar starts' (WaitFor 'menu: control center icon at') ''
    $icon = LastMatch 'control center icon at (-?\d+),(-?\d+)'
    $ix = [int]$icon.Groups[1].Value; $iy = [int]$icon.Groups[2].Value
    Start-Sleep -Milliseconds 600

    # 1. Click the Control Center icon -> panel opens with real state
    [CC]::Click($ix, $iy)
    $opened = (WaitFor 'cc: open at' 1) -and (WaitFor 'cc: state wifi' 1)
    Report 'icon click opens panel' $opened ((LastMatch 'cc: open at .*').Value)
    Report 'GPU snapshot rendered with Liquid Glass' (WaitFor 'cc: snapshot .*\(glass 1\)' 1 5000) ((LastMatch 'cc: snapshot .*').Value -replace '.*\\', '')
    Start-Sleep -Milliseconds 400
    $t = LastMatch 'targets page=main sound=(\d+),(\d+),(\d+) mute=(\d+),(\d+) wifi=(\d+),(\d+) display=(\d) (\d+),(\d+),(\d+)'
    $s0 = [int]$t.Groups[1].Value; $s1 = [int]$t.Groups[2].Value; $sy = [int]$t.Groups[3].Value
    $mx = [int]$t.Groups[4].Value; $my = [int]$t.Groups[5].Value
    $wx = [int]$t.Groups[6].Value; $wy = [int]$t.Groups[7].Value
    $hasDisplay = $t.Groups[8].Value -eq '1'
    $d0 = [int]$t.Groups[9].Value; $d1 = [int]$t.Groups[10].Value; $dy = [int]$t.Groups[11].Value

    # 2. Volume slider drag sets the Windows volume (and unmutes, as on macOS)
    [CC]::Drag([int]($s0 + ($s1 - $s0) * 0.15), $sy, [int]($s0 + ($s1 - $s0) * 0.30), 400)
    Start-Sleep -Milliseconds 700
    $a = ReadAudio
    Report 'volume slider drag -> Windows volume ~30' ([Math]::Abs($a.Volume - 30) -le 2 -and -not $a.Muted) "volume=$($a.Volume) muted=$($a.Muted)"

    # 3. Mute button toggles the Windows mute state both ways
    [CC]::Click($mx, $my); Start-Sleep -Milliseconds 600
    $b = ReadAudio
    [CC]::Click($mx, $my); Start-Sleep -Milliseconds 600
    $c = ReadAudio
    Report 'mute button mutes and unmutes' ($b.Muted -and -not $c.Muted) "after1=$($b.Muted) after2=$($c.Muted)"

    # 4. External volume change (another process) is reflected in the panel
    & $tool --set-volume 42 | Out-Null
    Report 'external volume change syncs to panel' (WaitFor 'state sync .*vol=42' 1 3000) ''

    # 5. Brightness slider (only when the display supports it)
    if ($hasDisplay) {
        [CC]::Drag([int]($d0 + ($d1 - $d0) * 0.9), $dy, [int]($d0 + ($d1 - $d0) * 0.80), 300)
        Start-Sleep -Milliseconds 1200
        $d = ReadAudio
        Report 'brightness slider drag -> display ~80%' ([Math]::Abs($d.Percent - 80) -le 6) "brightness=$($d.Percent)"
    } else {
        Report 'brightness hidden on unsupported display' (-not $orig.Brightness) 'no slider shown'
    }

    # 6. Wi-Fi detail page and Esc back
    [CC]::Click($wx, $wy)
    $wifiPage = (WaitFor 'cc: page wifi' 1) -and (WaitFor 'targets page=wifi' 1)
    Start-Sleep -Milliseconds 1500
    [CC]::Key(0x1B)
    Report 'Wi-Fi page opens; Esc returns to main' ($wifiPage -and (WaitFor 'cc: page main' 1)) ((LastMatch 'targets page=wifi.*').Value)

    # 7. Esc on the main page closes the panel
    Start-Sleep -Milliseconds 400
    [CC]::Key(0x1B)
    Report 'Esc closes panel' ((WaitFor 'closing \(Escape\)' 1) -and (WaitFor 'cc: closed' 1)) ''

    # 8. Icon toggles open/closed
    Start-Sleep -Milliseconds 500
    [CC]::Click($ix, $iy); $o2 = WaitFor 'cc: open at' 2
    Start-Sleep -Milliseconds 700
    [CC]::Click($ix, $iy)
    Report 'icon click toggles panel closed' ($o2 -and (WaitFor 'closing \(toggle\)' 1) -and (WaitFor 'cc: closed' 2)) ''

    # 9. Click outside (another window) closes the panel
    Start-Sleep -Milliseconds 500
    [CC]::Click($ix, $iy); $o3 = WaitFor 'cc: open at' 3
    Start-Sleep -Milliseconds 800
    $form = New-Object System.Windows.Forms.Form
    $form.StartPosition = 'Manual'; $form.Location = New-Object Drawing.Point(80, 300); $form.Size = New-Object Drawing.Size(300, 200)
    $form.Text = 'CC outside click target'; $form.TopMost = $true; $form.Show(); [Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 300
    [CC]::Click(230, 400); [Windows.Forms.Application]::DoEvents()
    Report 'outside click closes panel' ($o3 -and (WaitFor 'closing \(clicked outside\)' 1) -and (WaitFor 'cc: closed' 3)) ''
    $form.Close(); $form = $null

    Report 'no radio was switched by the test' ((Count 'radio ->') -eq 0) ''
    Report 'menu bar still running' (-not $proc.HasExited) ''
} finally {
    if ($form) { $form.Close() }
    & $tool --set-volume $orig.Volume | Out-Null
    & $tool --set-mute $(if ($orig.Muted) { 1 } else { 0 }) | Out-Null
    if ($orig.Brightness) { & $tool --set-brightness $orig.Percent | Out-Null }
    $h = [CC]::FindWindow('LiquidDesktopMenuBarV01', [NullString]::Value)
    if ($h -ne [IntPtr]::Zero) { [CC]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
    if (-not $proc.WaitForExit(5000)) { Stop-Process -Id $proc.Id -Force }
    Remove-Item Env:LIQUID_CC_SNAPSHOT_DIR -ErrorAction SilentlyContinue
}
$r = ReadAudio
Report 'volume/mute/brightness restored' ($r.Volume -eq $orig.Volume -and $r.Muted -eq $orig.Muted -and (-not $orig.Brightness -or [Math]::Abs($r.Percent - $orig.Percent) -le 1)) "volume=$($r.Volume) muted=$($r.Muted) brightness=$($r.Percent)"
Report 'menu bar exited cleanly' ((Count 'menu: exit') -ge 1) ''
$results | Set-Content (Join-Path $OutDir 'results.txt') -Encoding UTF8
Write-Host ("control center acceptance: {0} passed, {1} failed" -f ($results.Count - $failed), $failed)
exit $failed
