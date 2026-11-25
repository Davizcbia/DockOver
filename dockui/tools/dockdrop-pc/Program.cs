using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

const int discoveryPort = 47731;
const int transferPort = 47732;
const string protocol = "DockDropPc/1";
var utf8 = new UTF8Encoding(false, true);
var localName = Environment.MachineName;
Console.InputEncoding = Encoding.UTF8;
Console.OutputEncoding = Encoding.UTF8;

try
{
    if (args is ["receive", var folder])
        await Receive(Path.GetFullPath(folder), null);
    else if (args is ["receive", var popupFolder, "--popup", var executable])
    {
        await Receive(Path.GetFullPath(popupFolder), PopupPath(executable));
    }
    else if (args is ["send" or "list", ..])
    {
        var sending = args[0] == "send";
        var senderPopup = args is ["send", _, "--popup", var senderExe] ? PopupPath(senderExe) : null;
        if ((sending && args.Length != 2 && senderPopup is null) ||
            (!sending && args.Length != 1))
            return Usage();

        using var file = args[0] == "send" ? File.OpenRead(args[1]) : null;
        var peers = await Discover();
        for (var i = 0; i < peers.Count; i++)
            Console.WriteLine($"{i + 1}. {peers[i].Name} ({string.Join<IPAddress>(", ", peers[i].Addresses)})");
        if (peers.Count == 0)
        {
            Console.WriteLine("No receivers found. Open receive mode on the same network.");
            return 1;
        }
        if (file is null) return 0;

        Console.Write("Device number: ");
        if (!int.TryParse(Console.ReadLine(), out var choice) || choice < 1 || choice > peers.Count)
            throw new InvalidDataException("Invalid device selection.");
        Console.Write("Enter the code shown on the receiving PC: ");
        var pin = (Console.ReadLine() ?? "").Trim().Replace("-", "");
        if (pin.Length != 16 || !pin.All(Uri.IsHexDigit))
            throw new InvalidDataException("The code must contain 16 hexadecimal characters.");
        var peer = peers[choice - 1];
        return await Send(peer.Addresses, file, Convert.FromHexString(pin), senderPopup, peer.Name) ? 0 : 1;
    }
    else return Usage();

    return 0;
}
catch (Exception ex) when (IsTransferError(ex) || ex is UnauthorizedAccessException or ArgumentException)
{
    Console.Error.WriteLine(ex.Message);
    return 1;
}

static int Usage()
{
    Console.WriteLine("DockDropPc receive <folder> [--popup <DockDrop.exe>] | list | send <file> [--popup <DockDrop.exe>]");
    return 2;
}

static string PopupPath(string executable)
{
    var path = Path.GetFullPath(executable);
    if (!File.Exists(path)) throw new FileNotFoundException("The DockDrop popup executable was not found.");
    return path;
}

async Task<List<(string Name, IPAddress[] Addresses)>> Discover()
{
    using var socket = new UdpClient(AddressFamily.InterNetwork) { EnableBroadcast = true };
    var nonce = Convert.ToHexString(RandomNumberGenerator.GetBytes(16));
    var query = utf8.GetBytes($"{protocol} FIND {nonce}");
    var prefix = $"{protocol} HERE {nonce} ";
    var peers = new Dictionary<Guid, (string Name, HashSet<IPAddress> Addresses)>();
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(2));
    try
    {
        foreach (var address in BroadcastAddresses())
        {
            try { await socket.SendAsync(query, new IPEndPoint(address, discoveryPort), timeout.Token); }
            catch (SocketException) { /* An interface may go offline while browsing. */ }
        }
        while (true)
        {
            var reply = await socket.ReceiveAsync(timeout.Token);
            if (reply.RemoteEndPoint.Port != discoveryPort || reply.Buffer.Length > 512) continue;
            string message;
            try { message = utf8.GetString(reply.Buffer); }
            catch (DecoderFallbackException) { continue; }
            if (!message.StartsWith(prefix, StringComparison.Ordinal)) continue;
            var description = message[prefix.Length..];
            var separator = description.IndexOf(' ');
            if (separator != 32 ||
                !Guid.TryParseExact(description[..separator], "N", out var instance)) continue;
            var name = description[(separator + 1)..];
            if (!ValidLabel(name)) continue;
            if (!peers.TryGetValue(instance, out var peer))
            {
                peer = (name, new HashSet<IPAddress>());
                peers.Add(instance, peer);
            }
            peer.Addresses.Add(reply.RemoteEndPoint.Address);
        }
    }
    catch (OperationCanceledException) when (timeout.IsCancellationRequested) { }
    return peers.Values.Select(peer => (peer.Name, Addresses: peer.Addresses
            .OrderBy(address => IPAddress.IsLoopback(address))
            .ThenBy(address => address.ToString(), StringComparer.Ordinal).ToArray()))
        .OrderBy(peer => peer.Name, StringComparer.OrdinalIgnoreCase)
        .ThenBy(peer => peer.Addresses[0].ToString(), StringComparer.Ordinal).ToList();
}

static IEnumerable<IPAddress> BroadcastAddresses()
{
    // Loopback also lets two independent processes test the whole protocol on one PC.
    var addresses = new HashSet<IPAddress> { IPAddress.Loopback };
    foreach (var adapter in NetworkInterface.GetAllNetworkInterfaces())
    {
        if (adapter.OperationalStatus != OperationalStatus.Up ||
            adapter.NetworkInterfaceType is NetworkInterfaceType.Loopback or NetworkInterfaceType.Tunnel)
            continue;
        foreach (var ip in adapter.GetIPProperties().UnicastAddresses)
        {
            if (ip.Address.AddressFamily != AddressFamily.InterNetwork ||
                ip.PrefixLength is < 1 or > 30) continue;
            var bytes = ip.Address.GetAddressBytes();
            var mask = ip.IPv4Mask.GetAddressBytes();
            for (var i = 0; i < bytes.Length; i++) bytes[i] |= (byte)~mask[i];
            addresses.Add(new IPAddress(bytes));
        }
    }
    return addresses;
}

async Task Advertise(UdpClient socket, CancellationToken stopping)
{
    var prefix = $"{protocol} FIND ";
    var instance = Guid.NewGuid();
    while (!stopping.IsCancellationRequested)
    {
        var packet = await socket.ReceiveAsync(stopping);
        if (packet.Buffer.Length != prefix.Length + 32) continue;
        string message;
        try { message = utf8.GetString(packet.Buffer); }
        catch (DecoderFallbackException) { continue; }
        if (!message.StartsWith(prefix, StringComparison.Ordinal)) continue;
        var nonce = message[prefix.Length..];
        if (!nonce.All(Uri.IsHexDigit)) continue;
        var response = utf8.GetBytes($"{protocol} HERE {nonce} {instance:N} {localName}");
        try { await socket.SendAsync(response, packet.RemoteEndPoint, stopping); }
        catch (SocketException) { /* The sender may leave the network during discovery. */ }
    }
}

async Task Receive(string folder, string? popup)
{
    Directory.CreateDirectory(folder);
    using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
    var request = new CertificateRequest("CN=DockDrop PC", key, HashAlgorithmName.SHA256);
    using var certificate = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5),
        DateTimeOffset.UtcNow.AddDays(1));
    // Schannel needs a private key it can acquire from a Windows key container.
    using var serverCertificate = X509CertificateLoader.LoadPkcs12(
        certificate.Export(X509ContentType.Pfx), null, X509KeyStorageFlags.UserKeySet);
    var pin = Convert.ToHexString(serverCertificate.GetCertHash(HashAlgorithmName.SHA256).AsSpan(0, 8));

    using var server = new TcpListener(IPAddress.Any, transferPort);
    using var discovery = new UdpClient(new IPEndPoint(IPAddress.Any, discoveryPort));
    using var stopping = new CancellationTokenSource();
    server.Start(4);
    var advertise = Advertise(discovery, stopping.Token);
    Console.WriteLine($"Ready on {localName}. Receiving into {folder}");
    Console.WriteLine($"Device code: {pin[..4]}-{pin[4..8]}-{pin[8..12]}-{pin[12..]}");
    try
    {
        while (true)
        {
            using var client = await server.AcceptTcpClientAsync();
            try { await HandleOffer(client, serverCertificate, folder, popup); }
            catch (Exception ex) when (IsTransferError(ex) || ex is UnauthorizedAccessException)
            {
                Console.Error.WriteLine($"Incoming transfer failed: {ex.Message}");
            }
        }
    }
    finally
    {
        await stopping.CancelAsync();
        try { await advertise; }
        catch (OperationCanceledException) when (stopping.IsCancellationRequested) { }
    }
}

async Task HandleOffer(TcpClient client, X509Certificate2 certificate, string folder, string? popup)
{
    using var tls = OpenStream(client);
    using (var handshake = new CancellationTokenSource(TimeSpan.FromSeconds(10)))
        await tls.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
        {
            ServerCertificate = certificate,
            EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13
        }, handshake.Token);

    using var reader = new BinaryReader(tls, utf8, leaveOpen: true);
    using var writer = new BinaryWriter(tls, utf8, leaveOpen: true);
    if (ReadLabel(reader) != protocol) throw new InvalidDataException("Unsupported protocol.");
    var sender = ReadLabel(reader);
    var name = ReadLabel(reader);
    var length = reader.ReadInt64();
    if (!ValidFileName(name) || length < 0) throw new InvalidDataException("Invalid file offer.");

    using var prompt = popup is null ? null : TransferPopup.Start(popup, sender, name, length, folder);
    bool accept;
    if (popup is null)
    {
        Console.Write($"Accept {name} ({length:N0} bytes) from {sender} [{client.Client.RemoteEndPoint}]? [y/N] ");
        accept = string.Equals(Console.ReadLine()?.Trim(), "y", StringComparison.OrdinalIgnoreCase);
    }
    else accept = prompt is not null && await prompt.Accepted();
    if (!accept)
    {
        writer.Write((byte)0);
        Console.WriteLine("Declined.");
        return;
    }

    var finalPath = Path.Combine(folder, name);
    if (File.Exists(finalPath) || Directory.Exists(finalPath))
    {
        writer.Write((byte)2);
        prompt?.Update("failed");
        Console.Error.WriteLine($"A file or folder already exists at {finalPath}.");
        return;
    }
    var staging = Path.Combine(folder, $".dockdrop-{Guid.NewGuid():N}.part");
    var ownsStaging = false;
    var saved = false;
    try
    {
        using (var file = new FileStream(staging, FileMode.CreateNew, FileAccess.Write, FileShare.None))
        {
            ownsStaging = true;
            writer.Write((byte)1);
            prompt?.Update("p 0");
            var computed = CopyFile(tls, file, length, prompt is null ? null : remaining => prompt.Progress(length, remaining));
            prompt?.Update("p 100");
            var expected = new byte[32];
            tls.ReadExactly(expected);
            if (!CryptographicOperations.FixedTimeEquals(computed, expected))
                throw new InvalidDataException("File hash did not match.");
            file.Flush(flushToDisk: true);
        }
        File.Move(staging, finalPath);
        ownsStaging = false;
        saved = true;
        prompt?.Update("done");
        writer.Write((byte)1);
        Console.WriteLine($"Received {finalPath}");
    }
    finally
    {
        if (!saved) prompt?.Update("failed");
        if (ownsStaging) File.Delete(staging);
    }
}

async Task<bool> Send(IPAddress[] addresses, FileStream file, byte[] pin, string? popup, string peerName)
{
    using var client = new TcpClient(AddressFamily.InterNetwork);
    using (var connect = new CancellationTokenSource(TimeSpan.FromSeconds(4 * addresses.Length)))
        await client.ConnectAsync(addresses, transferPort, connect.Token);
    using var tls = OpenStream(client);
    using (var handshake = new CancellationTokenSource(TimeSpan.FromSeconds(10)))
        await tls.AuthenticateAsClientAsync(new SslClientAuthenticationOptions
        {
            TargetHost = "DockDrop PC",
            EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
            RemoteCertificateValidationCallback = (_, remote, _, _) => remote is not null &&
                CryptographicOperations.FixedTimeEquals(pin, remote.GetCertHash(HashAlgorithmName.SHA256).AsSpan(0, 8))
        }, handshake.Token);

    var name = Path.GetFileName(file.Name);
    var length = file.Length;
    using var window = popup is null ? null : TransferPopup.Start(popup, peerName, name, length, "", sending: true);
    using var writer = new BinaryWriter(tls, utf8, leaveOpen: true);
    WriteLabel(writer, protocol);
    WriteLabel(writer, localName);
    WriteLabel(writer, name);
    writer.Write(length);

    Console.WriteLine($"Waiting for {client.Client.RemoteEndPoint} to accept...");
    var decision = tls.ReadByte();
    if (decision is 0 or 2)
    {
        window?.Update(decision == 0 ? "declined" : "failed");
        Console.WriteLine(decision == 0 ? "Transfer declined." : "A file with that name already exists.");
        return false;
    }
    if (decision != 1) throw new InvalidDataException("Connection closed before acceptance.");
    window?.Update("accepted");
    var received = false;
    try
    {
        tls.Write(CopyFile(file, tls, length, window is null ? null : remaining => window.Progress(length, remaining)));
        received = tls.ReadByte() == 1;
        if (!received) throw new InvalidDataException("The receiver did not confirm that it saved the file.");
        window?.Update("done");
    }
    finally
    {
        if (!received) window?.Update("failed");
    }
    Console.WriteLine($"Sent {name} ({length:N0} bytes).");
    return true;
}

static SslStream OpenStream(TcpClient client)
{
    var tls = new SslStream(client.GetStream());
    tls.ReadTimeout = 120_000;
    tls.WriteTimeout = 120_000;
    return tls;
}

static byte[] CopyFile(Stream source, Stream destination, long remaining, Action<long>? progress = null)
{
    using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
    var buffer = new byte[64 * 1024];
    while (remaining > 0)
    {
        var count = source.Read(buffer, 0, (int)Math.Min(remaining, buffer.Length));
        if (count == 0) throw new EndOfStreamException("File ended before its advertised size.");
        destination.Write(buffer, 0, count);
        hash.AppendData(buffer, 0, count);
        remaining -= count;
        progress?.Invoke(remaining);
    }
    return hash.GetHashAndReset();
}

string ReadLabel(BinaryReader reader)
{
    var length = reader.ReadUInt16();
    if (length is < 1 or > 1024) throw new InvalidDataException("Invalid text field.");
    var bytes = new byte[length];
    reader.BaseStream.ReadExactly(bytes);
    var value = utf8.GetString(bytes);
    if (!ValidLabel(value)) throw new InvalidDataException("Invalid text field.");
    return value;
}

void WriteLabel(BinaryWriter writer, string value)
{
    var bytes = utf8.GetBytes(value);
    if (!ValidLabel(value) || bytes.Length > 1024) throw new InvalidDataException("Invalid text field.");
    writer.Write((ushort)bytes.Length);
    writer.Write(bytes);
}

static bool ValidLabel(string value) =>
    value.Length > 0 && !value.Any(c => char.IsControl(c) || char.GetUnicodeCategory(c) == System.Globalization.UnicodeCategory.Format);

static bool ValidFileName(string name)
{
    if (name.Length > 255 || name.EndsWith('.') || name.EndsWith(' ') ||
        name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || name.Contains(':') ||
        name.Contains('/') || name.Contains('\\')) return false;
    var stem = name.Split('.')[0].TrimEnd().ToUpperInvariant();
    if (stem is "CON" or "PRN" or "AUX" or "NUL" or "CONIN$" or "CONOUT$") return false;
    return !(stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) &&
        "0123456789¹²³".Contains(stem[3]));
}

static bool IsTransferError(Exception ex) =>
    ex is IOException or InvalidDataException or SocketException or AuthenticationException or OperationCanceledException or DecoderFallbackException;

sealed class TransferPopup(Process child) : IDisposable
{
    private int lastPercent = -1;

    public static TransferPopup? Start(string executable, string peer, string filename, long length, string folder, bool sending = false)
    {
        var start = new ProcessStartInfo(executable)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            StandardInputEncoding = new UTF8Encoding(false)
        };
        foreach (var argument in new[] { sending ? "--pc-send-progress" : "--pc-receive-offer", peer, filename,
                    length.ToString("N0", CultureInfo.CurrentCulture) + " B", folder })
            start.ArgumentList.Add(argument);
        try
        {
            var process = Process.Start(start);
            return process is null ? null : new TransferPopup(process);
        }
        catch (Win32Exception ex)
        {
            Console.Error.WriteLine($"Could not open the DockDrop receive popup: {ex.Message}");
            return null;
        }
    }

    public async Task<bool> Accepted()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(90));
        try
        {
            return string.Equals(await child.StandardOutput.ReadLineAsync(timeout.Token), "accept", StringComparison.Ordinal);
        }
        catch (Exception ex) when (ex is IOException or OperationCanceledException)
        {
            return false;
        }
    }

    public void Progress(long length, long remaining)
    {
        var percent = Math.Min(99, (int)((length - remaining) * 100.0 / length));
        if (percent == lastPercent) return;
        lastPercent = percent;
        Update($"p {percent}");
    }

    public void Update(string status)
    {
        try
        {
            child.StandardInput.WriteLine(status);
            child.StandardInput.Flush();
        }
        catch (IOException) { }
        catch (InvalidOperationException) { }
    }

    public void Dispose()
    {
        try { child.StandardInput.Close(); }
        catch (IOException) { }
        child.Dispose();
    }
}


