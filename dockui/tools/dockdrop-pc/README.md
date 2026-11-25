# DockDrop PC transfer preview

Build on Windows with the .NET 9 SDK from this directory.

```powershell
dotnet build -c Release
```

Copy the contents of the Release output directory to both PCs. The preview can use the .NET 9 or 10 Runtime. Open PowerShell in the copied directory on each PC, on the same trusted IPv4 LAN.

On the receiving PC, choose a destination folder and start listening.

```powershell
.\DockDropPc.exe receive "$env:USERPROFILE\Downloads\DockDrop"
```

With the current Windows DockDrop preview, opt in to the desktop receive prompt:

```powershell
.\DockDropPc.exe receive "$env:USERPROFILE\Downloads\DockDrop" --popup ".\DockDrop.exe"
```

On the receiving PC, open **DockDrop → Settings** and change **Water-ball popup for PC transfers**.
It defaults to on. The floating ball contains the request and its **Confirm** target.
Hover over the ball to reveal its close button. Closing an unanswered request declines
it, as does pressing Escape while focused. Turning the setting off uses the DockOver
Notification Center banner layout.
Either layout supports accepting, declining, and viewing the receive status. Keep the
receiver running while expecting transfers. The regular command without `--popup`
accepts or declines in the console.

On the sending PC, list receivers or select a file to send.

```powershell
.\DockDropPc.exe list
.\DockDropPc.exe send "C:\path\to\file.zip"
```

Add `--popup ".\DockDrop.exe"` to the send command to also show the sender animation.
After you select the device and provide its code, the sender waits for the receiver.
The two balls merge only after the receiver accepts, then the main ball shows transfer
progress. It shows completion after the receiver confirms the file was saved.

Pick a receiver, type the code displayed on that PC, then accept the offer on the receiving PC. The receiver stays available until Ctrl+C. Each send handles one file. The sender reports success after the receiver verifies the SHA-256 digest and saves the file. Existing files are kept.

On a trusted Windows private network, the receiver needs inbound UDP 47731 for discovery and TCP 47732 for transfer. Client isolation or blocked broadcasts can prevent discovery. This preview uses IPv4 LAN discovery and encrypted direct transfer with other instances of this preview.
