using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.Server.Kestrel.Core;

const int port = 47733;
const long uploadLimit = 2L * 1024 * 1024 * 1024;
if (args.Length is < 2 or > 3 || args[0] is not ("send" or "receive") ||
    (args.Length == 3 && args[2] != "--local-test"))
{
    Console.Error.WriteLine("DockDropBrowser send <file> | receive <folder>");
    return 2;
}

Console.OutputEncoding = Encoding.UTF8;
var sending = args[0] == "send";
var localTest = args.Length == 3;
Process? wifi = null;

try
{
    var path = Path.GetFullPath(args[1]);
    // Keep the selected file stable for the lifetime of the sharing session.
    using var file = sending ? File.OpenRead(path) : null;
    if (!sending) Directory.CreateDirectory(path);

    var address = IPAddress.Loopback;
    string? ssid = null, passphrase = null;
    if (!localTest)
    {
        var previouslyActive = ActiveDirectAddresses().ToHashSet();
        var start = new ProcessStartInfo(Path.Combine(AppContext.BaseDirectory, "WifiDirectHost.exe"))
        {
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            StandardOutputEncoding = Encoding.UTF8,
            CreateNoWindow = true
        };
        wifi = Process.Start(start) ?? throw new IOException("Could not start the Wi-Fi Direct host.");
        using var startup = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        ssid = await wifi.StandardOutput.ReadLineAsync(startup.Token);
        passphrase = await wifi.StandardOutput.ReadLineAsync(startup.Token);
        if (string.IsNullOrEmpty(ssid) || string.IsNullOrEmpty(passphrase))
            throw new IOException("Wi-Fi Direct did not return a network name and password.");
        while (true)
        {
            var candidates = ActiveDirectAddresses().Except(previouslyActive).Take(2).ToArray();
            if (candidates.Length == 1)
            {
                address = candidates[0];
                break;
            }
            if (candidates.Length > 1)
                throw new IOException("Multiple new Wi-Fi Direct addresses appeared. Close other Wi-Fi Direct sessions and retry.");
            await Task.Delay(150, startup.Token);
        }
    }

    var authority = $"{address}:{port}";
    var accessPath = "/" + Convert.ToHexString(RandomNumberGenerator.GetBytes(16));
    var scriptNonce = Convert.ToBase64String(RandomNumberGenerator.GetBytes(16));
    var builder = WebApplication.CreateSlimBuilder(Array.Empty<string>());
    builder.Logging.ClearProviders();
    builder.WebHost.ConfigureKestrel(options =>
    {
        options.Configure(new ConfigurationBuilder().Build());
        options.Listen(address, port, listener => listener.Protocols = HttpProtocols.Http1);
        options.AddServerHeader = false;
        options.Limits.MaxRequestBodySize = uploadLimit;
        options.Limits.MaxConcurrentConnections = 16;
    });
    await using var app = builder.Build();
    using var singleUpload = new SemaphoreSlim(1);
    using var page = new StreamReader(typeof(Program).Assembly.GetManifestResourceStream("DockDropBrowser.transfer.html")!);
    var html = (await page.ReadToEndAsync()).Replace("SCRIPT_NONCE", scriptNonce)
        .Replace("MODE_PLACEHOLDER", sending ? "download" : "upload")
        .Replace("DOWNLOAD_NAME", System.Text.Encodings.Web.HtmlEncoder.Default.Encode(
            sending ? Path.GetFileName(path) : ""));
    app.Use(async (context, next) =>
    {
        var headers = context.Response.Headers;
        headers.CacheControl = "no-store";
        headers["Referrer-Policy"] = "no-referrer";
        headers["X-Content-Type-Options"] = "nosniff";
        headers.ContentSecurityPolicy = $"default-src 'none'; style-src 'nonce-{scriptNonce}'; " +
            $"script-src 'nonce-{scriptNonce}'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'";
        if (!string.Equals(context.Request.Host.Value, authority, StringComparison.OrdinalIgnoreCase))
        {
            context.Response.StatusCode = StatusCodes.Status400BadRequest;
            return;
        }
        await next(context);
    });

    app.MapGet(accessPath, () => Results.Content(html, "text/html; charset=utf-8"));

    if (sending)
    {
        app.MapGet(accessPath + "/file", () =>
            Results.File(path, "application/octet-stream", Path.GetFileName(path), enableRangeProcessing: true));
    }
    else
    {
        app.MapPost(accessPath + "/file", async (HttpRequest request, CancellationToken aborted) =>
        {
            if (request.Headers["X-DockDrop-Name"].Count != 1 ||
                !string.Equals(request.ContentType, "application/octet-stream", StringComparison.OrdinalIgnoreCase))
                return Results.Text("Invalid upload.", statusCode: 400);
            string name;
            try { name = Uri.UnescapeDataString(request.Headers["X-DockDrop-Name"].ToString()); }
            catch (UriFormatException) { return Results.Text("Invalid filename.", statusCode: 400); }
            if (!ValidFileName(name)) return Results.Text("Invalid filename.", statusCode: 400);
            if (request.ContentLength is null)
                return Results.Text("The browser must send a file length.", statusCode: 411);
            if (request.ContentLength > uploadLimit)
                return Results.Text("Files must be 2 GiB or smaller.", statusCode: 413);
            if (!await singleUpload.WaitAsync(0, aborted))
                return Results.Text("Another upload is in progress.", statusCode: 409);
            try { return await SaveUpload(request, name, path, aborted); }
            finally { singleUpload.Release(); }
        });
    }

    await app.StartAsync();
    if (!localTest)
    {
        Console.WriteLine($"Join Wi-Fi: {ssid}");
        Console.WriteLine($"Wi-Fi password: {passphrase}");
    }
    Console.WriteLine($"Browser link: http://{authority}{accessPath}");
    Console.WriteLine(sending ? $"Offering: {path}" : $"Saving uploads to: {path}");
    Console.WriteLine("Press Ctrl+C to stop sharing.");
    var stopping = app.WaitForShutdownAsync();
    if (wifi is not null && await Task.WhenAny(stopping, wifi.WaitForExitAsync()) != stopping)
        app.Lifetime.StopApplication();
    await stopping;
    return 0;
}
catch (Exception error) when (error is IOException or SocketException or UnauthorizedAccessException
    or ArgumentException or System.ComponentModel.Win32Exception or OperationCanceledException)
{
    Console.Error.WriteLine(error is OperationCanceledException
        ? "No usable Wi-Fi Direct IPv4 address became available in time." : error.Message);
    return 1;
}
finally
{
    if (wifi is not null)
    {
        wifi.StandardInput.Close();
        if (!wifi.WaitForExit(5000))
        {
            try { wifi.Kill(); }
            catch (InvalidOperationException) { /* Exited after the wait. */ }
        }
        wifi.Dispose();
    }
}

static IEnumerable<IPAddress> ActiveDirectAddresses() =>
    NetworkInterface.GetAllNetworkInterfaces()
        .Where(adapter => adapter.OperationalStatus == OperationalStatus.Up &&
            adapter.Description.StartsWith("Microsoft Wi-Fi Direct Virtual Adapter", StringComparison.OrdinalIgnoreCase))
        .SelectMany(adapter => adapter.GetIPProperties().UnicastAddresses)
        .Select(item => item.Address)
        .Where(address => address.AddressFamily == AddressFamily.InterNetwork &&
            !IPAddress.IsLoopback(address) && !address.GetAddressBytes().AsSpan().StartsWith(new byte[] { 169, 254 }));

static bool ValidFileName(string name)
{
    if (name.Length is 0 or > 255 || name.EndsWith('.') || name.EndsWith(' ') ||
        name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
        name.Any(c => char.IsControl(c) || char.GetUnicodeCategory(c) == UnicodeCategory.Format))
        return false;
    var stem = name.Split('.')[0].TrimEnd().ToUpperInvariant();
    return stem is not ("CON" or "PRN" or "AUX" or "NUL" or "CONIN$" or "CONOUT$") &&
        !(stem.Length == 4 && (stem.StartsWith("COM") || stem.StartsWith("LPT")) &&
            "0123456789¹²³".Contains(stem[3]));
}

static async Task<IResult> SaveUpload(HttpRequest request, string name, string folder, CancellationToken aborted)
{
    var finalPath = Path.Combine(folder, name);
    if (File.Exists(finalPath) || Directory.Exists(finalPath))
        return Results.Text("A file or folder with that name already exists.", statusCode: 409);

    var staging = Path.Combine(folder, $".dockdrop-{Guid.NewGuid():N}.part");
    var ownsStaging = false;
    try
    {
        await using (var file = new FileStream(staging, FileMode.CreateNew, FileAccess.Write,
                         FileShare.None, 64 * 1024, FileOptions.Asynchronous))
        {
            ownsStaging = true;
            await request.Body.CopyToAsync(file, aborted);
            if (file.Length != request.ContentLength)
                return Results.Text("The upload was incomplete.", statusCode: 400);
            await file.FlushAsync(aborted);
        }
        aborted.ThrowIfCancellationRequested();
        File.Move(staging, finalPath);
        ownsStaging = false;
        Console.WriteLine($"Received: {finalPath}");
        return Results.Text("File saved.");
    }
    catch (IOException) when (File.Exists(finalPath) || Directory.Exists(finalPath))
    {
        return Results.Text("A file or folder with that name already exists.", statusCode: 409);
    }
    catch (Exception error) when (error is IOException or UnauthorizedAccessException)
    {
        return Results.Text("The host could not save the file.", statusCode: 500);
    }
    finally
    {
        if (ownsStaging) File.Delete(staging);
    }
}
