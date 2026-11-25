# DockDrop 浏览器收发预览

只有创建临时网络的一台电脑需要运行 DockDrop。另一台电脑用系统 Wi‑Fi 菜单和普通浏览器即可。两台电脑不必预先连到同一个网络，临时网络也不需要联网。

## 使用

在支持 Wi‑Fi Direct 的 Windows x64 电脑上解压 ZIP，并在解压目录中打开 PowerShell。主机需要 ASP.NET Core Runtime 10（也可使用 9）。

要让对方下载主机上的文件，运行：

```powershell
.\DockDropBrowser.exe send "C:\path\to\file.zip"
```

要让对方从浏览器上传文件到主机，运行：

```powershell
.\DockDropBrowser.exe receive "$env:USERPROFILE\Downloads\DockDrop"
```

1. 对方在系统 Wi‑Fi 菜单中选择控制台显示的网络，输入临时密码。系统可能提示该网络没有互联网，这时仍保持连接。
2. 用浏览器打开控制台完整显示的 `http://` 链接，点击下载，或选文件后点击发送。
3. 完成后，主机按 Ctrl+C 关闭网页和临时 Wi‑Fi。

网页仅绑定临时 Wi‑Fi Direct 网卡，使用带随机访问码的本地 HTTP 链接。持有完整链接且能访问临时网络的人可进行该会话允许的操作。每次上传选一个文件，最大 2 GiB。上传完成才写入最终文件名，同名文件保留。若第二台电脑无法打开网页，确认它仍连在临时网络上，并检查主机防火墙是否允许该接口的 TCP 47733。

## 构建

使用 .NET 9 SDK 和 Visual Studio x64 开发人员命令提示符。从本目录运行：

```cmd
dotnet build -c Release
build-host.cmd bin\Release\net9.0
```

C++ 辅助进程负责启动及关闭 Wi‑Fi Direct。运行时把 `WifiDirectHost.exe` 放在 `DockDropBrowser.exe` 同一目录。
