using System.Diagnostics;
using System.IO.Compression;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Blackvolt.App.Services;

/// <summary>A newer release published on GitHub.</summary>
internal sealed record UpdateInfo(Version Version, string Tag, string Notes, string PageUrl,
                                  string ZipUrl, long ZipSize, string? ShaUrl);

/// <summary>
/// Checks GitHub Releases for a newer build, downloads its zip, verifies the SHA-256 and swaps the
/// files in after the app exits (a small PowerShell script does the copy and relaunches).
/// </summary>
internal static class Updater
{
    private const string Repo = "emanueldev1/Blackvolt-General-NRV";
    private const string AssetSuffix = "-win-x64.zip";

    private static readonly HttpClient Http = CreateClient();
    private static readonly string UpdatesDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Blackvolt", "updates");

    public static Version CurrentVersion { get; } =
        typeof(App).Assembly.GetName().Version is { } v ? new Version(v.Major, v.Minor, Math.Max(0, v.Build)) : new Version(0, 1, 0);

    /// <summary>Set once a background check finds something, so pages can show it.</summary>
    public static UpdateInfo? Available { get; private set; }

    public static event Action<UpdateInfo>? Found;

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("Blackvolt", CurrentVersion.ToString()));
        client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        return client;
    }

    /// <summary>Startup check: at most once every 12 h and only when enabled in settings.</summary>
    public static async Task CheckInBackgroundAsync()
    {
        var settings = AppSettings.Current.Update;
        if (!settings.AutoCheck || DateTime.UtcNow - settings.LastCheckUtc < TimeSpan.FromHours(12)) return;
        try
        {
            var info = await CheckAsync();
            if (info is not null && info.Tag != settings.SkippedTag) Found?.Invoke(info);
        }
        catch (Exception ex) when (ex is HttpRequestException or TaskCanceledException or JsonException)
        {
            // Offline or rate-limited: try again next launch.
        }
    }

    /// <summary>Returns the latest release when it is newer than this build, otherwise null.</summary>
    public static async Task<UpdateInfo?> CheckAsync(CancellationToken ct = default)
    {
        using var response = await Http.GetAsync(Mirrors.Api($"https://api.github.com/repos/{Repo}/releases/latest", AppSettings.Current.Update.Mirror), ct);
        AppSettings.Current.Update.LastCheckUtc = DateTime.UtcNow;
        AppSettings.Current.Save();
        if (response.StatusCode == System.Net.HttpStatusCode.NotFound) return null; // no releases yet
        response.EnsureSuccessStatusCode();

        using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync(ct));
        var root = doc.RootElement;
        if (root.TryGetProperty("draft", out var draft) && draft.GetBoolean()) return null;
        var tag = root.GetProperty("tag_name").GetString() ?? "";
        if (!TryParseVersion(tag, out var version) || version <= CurrentVersion) return null;

        string? zipUrl = null, shaUrl = null;
        long zipSize = 0;
        foreach (var asset in root.GetProperty("assets").EnumerateArray())
        {
            var name = asset.GetProperty("name").GetString() ?? "";
            var url = asset.GetProperty("browser_download_url").GetString() ?? "";
            if (name.EndsWith(AssetSuffix, StringComparison.OrdinalIgnoreCase))
            {
                zipUrl = url;
                zipSize = asset.GetProperty("size").GetInt64();
            }
            else if (name.EndsWith(AssetSuffix + ".sha256", StringComparison.OrdinalIgnoreCase))
            {
                shaUrl = url;
            }
        }
        if (zipUrl is null) return null;

        var info = new UpdateInfo(version, tag, root.TryGetProperty("body", out var body) ? body.GetString() ?? "" : "",
                                  root.GetProperty("html_url").GetString() ?? "", zipUrl, zipSize, shaUrl);
        Available = info;
        return info;
    }

    internal static bool TryParseVersion(string tag, out Version version)
    {
        var text = tag.Trim().TrimStart('v', 'V');
        var dash = text.IndexOfAny(['-', '+']);
        if (dash >= 0) text = text[..dash];
        if (Version.TryParse(text, out var v))
        {
            version = new Version(v.Major, v.Minor, Math.Max(0, v.Build));
            return true;
        }
        version = new Version(0, 0, 0);
        return false;
    }

    /// <summary>
    /// Downloads and verifies the update, then starts the installer script. The caller must exit
    /// the app right after this returns true.
    /// </summary>
    public static async Task<bool> DownloadAndInstallAsync(UpdateInfo info, IProgress<double> progress, CancellationToken ct)
    {
        var workDir = Path.Combine(UpdatesDir, info.Version.ToString());
        if (Directory.Exists(workDir)) Directory.Delete(workDir, recursive: true);
        Directory.CreateDirectory(workDir);
        var zipPath = Path.Combine(workDir, "Blackvolt.zip");

        // 1. Download with progress.
        var mirror = AppSettings.Current.Update.Mirror;
        using (var response = await Http.GetAsync(Mirrors.Asset(info.ZipUrl, mirror), HttpCompletionOption.ResponseHeadersRead, ct))
        {
            response.EnsureSuccessStatusCode();
            var total = response.Content.Headers.ContentLength ?? info.ZipSize;
            await using var input = await response.Content.ReadAsStreamAsync(ct);
            await using var output = File.Create(zipPath);
            var buffer = new byte[1 << 16];
            long done = 0;
            int read;
            while ((read = await input.ReadAsync(buffer, ct)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, read), ct);
                done += read;
                if (total > 0) progress.Report((double)done / total);
            }
        }

        // 2. Integrity: the release publishes the zip's SHA-256 next to it.
        if (info.ShaUrl is not null)
        {
            var expected = (await Http.GetStringAsync(Mirrors.Asset(info.ShaUrl, mirror), ct)).Trim().Split(' ', '\t')[0];
            string actual;
            await using (var stream = File.OpenRead(zipPath))
                actual = Convert.ToHexString(await SHA256.HashDataAsync(stream, ct));
            if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("La descarga no coincide con su SHA-256. Vuelve a intentarlo.");
        }

        // 3. Unpack next to the zip; the app must contain Blackvolt.exe at its root.
        var staging = Path.Combine(workDir, "files");
        ZipFile.ExtractToDirectory(zipPath, staging, overwriteFiles: true);
        var root = FindAppRoot(staging) ?? throw new InvalidDataException("El paquete de la actualización no contiene Blackvolt.exe.");

        // 4. Hand over to a script that waits for us to exit, copies the files and relaunches.
        var installDir = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
        var exe = Environment.ProcessPath ?? Path.Combine(installDir, "Blackvolt.exe");
        var script = Path.Combine(workDir, "install.ps1");
        File.WriteAllText(script, InstallScript, new UTF8Encoding(encoderShouldEmitUTF8Identifier: true));

        var start = new ProcessStartInfo("powershell.exe")
        {
            UseShellExecute = true,
            WindowStyle = ProcessWindowStyle.Hidden,
            Arguments = $"-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"{script}\" " +
                        $"-ProcessId {Environment.ProcessId} -Source \"{root}\" -Target \"{installDir}\" -Exe \"{exe}\"",
        };
        if (!CanWrite(installDir)) start.Verb = "runas"; // e.g. installed under Program Files
        Process.Start(start);
        return true;
    }

    public static void Skip(UpdateInfo info)
    {
        AppSettings.Current.Update.SkippedTag = info.Tag;
        AppSettings.Current.Save();
    }

    private static string? FindAppRoot(string dir)
    {
        if (File.Exists(Path.Combine(dir, "Blackvolt.exe"))) return dir;
        return Directory.GetDirectories(dir).Select(FindAppRoot).FirstOrDefault(r => r is not null);
    }

    private static bool CanWrite(string dir)
    {
        try
        {
            var probe = Path.Combine(dir, $".bv-write-{Guid.NewGuid():N}");
            File.WriteAllText(probe, "");
            File.Delete(probe);
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    // Copies over the install (no purge: user files next to the exe survive) and restarts.
    private const string InstallScript = """
        param([int]$ProcessId, [string]$Source, [string]$Target, [string]$Exe)
        $ErrorActionPreference = 'Stop'
        $log = Join-Path (Split-Path $Source -Parent) 'install.log'
        try {
            Wait-Process -Id $ProcessId -Timeout 60 -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 500
            robocopy $Source $Target /E /R:5 /W:1 /NFL /NDL /NJH /NJS /NP | Out-File $log -Encoding utf8
            if ($LASTEXITCODE -ge 8) { throw "robocopy falló con código $LASTEXITCODE" }
            Add-Content $log "ok"
        } catch {
            Add-Content $log "error: $_"
        }
        Start-Process -FilePath $Exe
        """;
}
