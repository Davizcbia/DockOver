# Packages the standalone apps from the same Release build used for DockOver.
param(
    [Parameter(Mandatory)][string]$BuildDir
)
$ErrorActionPreference = 'Stop'
$BuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$source = Get-Content (Join-Path $PSScriptRoot 'dockui/CMakeLists.txt') -Raw
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force $dist | Out-Null
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$products = @(
    @{
        Name = 'DockDrop'
        Variable = 'DOCKDROP_VERSION'
        Files = @('DockDrop.exe')
        Instructions = @'
DockDrop for Windows 11 x64

解压后运行 DockDrop.exe，选择附近的接收电脑，或点「配对手机」扫描配对球或备用光纹。电脑互传需要 .NET Desktop Runtime 9 或 10。
从 DockDrop 设置选择接收目录、管理记住的手机及添加资源管理器右键入口。
关闭窗口后 DockDrop 默认在后台运行并随开机启动，已配对的手机仍可发送，
水球会自动弹出；可在设置中关闭「在后台运行」。
电脑需要 Microsoft Edge WebView2 Runtime。

Unzip and run DockDrop.exe. Choose a nearby receiving PC (.NET Desktop Runtime 9 or 10 required), or choose Pair phone and scan the bubble
with a phone browser. Settings manages the receive folder, trusted phones
and the File Explorer entry. After you close the window, DockDrop keeps
running in the background and starts at sign-in, so remembered phones can
still send and the water ball pops up; turn off Run in the background in
Settings to stop that. Microsoft Edge WebView2 Runtime is required.
'@
    },
    @{
        Name = 'DockOver-Music-Player'
        Variable = 'DOCKOVER_MUSIC_VERSION'
        Files = @('DockOverMusic.exe', 'docky-now-playing.exe', 'LiquidDesktopSettings.exe')
        Instructions = @'
DockOver Music Player for Windows 11 x64

解压后运行 DockOverMusic.exe。从通知区域打开「正在播放」「音乐壁纸」和「音乐设置」。
音乐由已有的 Windows 播放器提供。与 DockOver 同时安装时，两套入口共用音乐设置。

Unzip and run DockOverMusic.exe. Use the tray icon for Now Playing, music wallpaper
and Music settings. Play music in an existing Windows player. This edition shares
its music preferences with an installed DockOver.
'@
    }
)
foreach ($product in $products) {
    $version = [regex]::Match($source, ('set\({0} "([^"]+)"' -f $product.Variable)).Groups[1].Value
    if (!$version) { throw "Missing version in $($product.Variable)." }
    $launcher = Get-Item -LiteralPath (Join-Path $BuildDir $product.Files[0])
    if ($launcher.VersionInfo.ProductVersion -ne $version) {
        throw "$($launcher.Name) is not version $version. Rebuild first."
    }
    $stem = "$($product.Name)-Windows-Portable-$version"
    $stage = Join-Path $dist "$stem-build-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory $stage | Out-Null
    foreach ($file in $product.Files) {
        Copy-Item -LiteralPath (Join-Path $BuildDir $file) -Destination $stage
    }
    if ($product.Name -eq 'DockDrop') { Copy-Item (Join-Path $BuildDir 'dockdrop-pc') -Destination (Join-Path $stage 'dockdrop-pc') -Recurse }
    [IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $product.Instructions,
                           [Text.UTF8Encoding]::new($true))
    $archive = Join-Path $dist "$stem.zip"
    $pending = "$archive.pending-$([guid]::NewGuid().ToString('N'))"
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $pending)
    if (Test-Path -LiteralPath $archive) {
        [IO.File]::Replace($pending, $archive, "$archive.previous-$([guid]::NewGuid().ToString('N'))")
    } else {
        [IO.File]::Move($pending, $archive)
    }
    Get-FileHash -LiteralPath $archive -Algorithm SHA256
}
