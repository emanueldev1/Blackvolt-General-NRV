using System.IO.Compression;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.RegularExpressions;
using Blackvolt.App.Interop;
using NrVariant = Blackvolt.App.Interop.NativeEngine.NrVariant;

namespace Blackvolt.App.Services;

/// <summary>An RTX Video runtime (VSR / TrueHDR) taken from Veyra's NVIDIA package, as Veyra ships it.</summary>
internal sealed record VideoRuntime(NativeEngine.Feature Feature, string Member, string Sha256, long Size, string Label)
{
    public string Dll => Runtimes.DllName(Feature);
    public bool Installed => File.Exists(Path.Combine(Runtimes.UserDir, Dll)) || File.Exists(Path.Combine(Runtimes.BundledDir, Dll));
}

/// <summary>One nvngx_dlssnr.dll build published on RankFTW/rhi-repo (where Veyra's come from).</summary>
internal sealed record NrBuild(string Tag, NrVariant Slot, string Label, string Sha256, long Size, bool Default)
{
    public string Url => $"https://github.com/RankFTW/rhi-repo/releases/download/{Tag}/nvngx_dlssnr_{Tag["dlssnr-".Length..]}.zip";
}

/// <summary>
/// Gets the DLSS NR runtimes (nvngx_dlssnr.dll) from the same place Veyra gets them: the
/// RankFTW/rhi-repo GitHub releases (the RenoDX runtime mirror). Veyra 2.2 ships
/// dlssnr-310.9.16 as its compatible build (runtime/experimental/nr-ampere) and dlssnr-310.8.0 as
/// the NVIDIA original (nr-original); both were checked byte for byte against Veyra's lock.
/// Only the DLL is read out of each zip (HTTP ranges), then CRC, PE header and the exact SHA-256 of
/// the chosen build are checked before the engine installs it. Veyra's own NVIDIA package is the
/// fallback source for the default builds.
/// </summary>
internal static partial class NrRuntimeInstaller
{
    public static readonly NrBuild[] Builds =
    [
        new("dlssnr-310.9.16", NrVariant.Compat, "SF 310.9.16 FP16 · el de Veyra 2.2 · RTX 20–50",
            "cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed", 276162560, true),
        new("dlssnr-310.8.SF-v2", NrVariant.Compat, "SF-v2 310.8.2 · el de Veyra 2.0 · RTX 20–50",
            "6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927", 165830144, false),
        new("dlssnr-310.8.0", NrVariant.Original, "NVIDIA 310.8.0 · el de Veyra · RTX 50",
            "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e", 165840496, true),
        new("dlssnr-310.9.16-nvidia", NrVariant.Original, "NVIDIA 310.9.16 firmado (más reciente) · RTX 50",
            "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f", 266424944, false),
        new("dlssnr-310.8.Lecram", NrVariant.Original, "Lecram 310.8.3 · RTX 50",
            "f95feb54137ea11979f9b4ec4f00afd84b5c98a5624d3388fbf6a87714a39fcc", 165840496, false),
    ];

    /// <summary>
    /// RTX Video SDK 1.1.0 runtimes, exactly the files Veyra 2.2 ships in runtime/experimental
    /// (hashes from its release runtime lock).
    /// </summary>
    public static readonly VideoRuntime[] VideoRuntimes =
    [
        new(NativeEngine.Feature.RtxVideoSuperResolution, "runtime/experimental/nvngx_vsr.dll",
            "c3d88eea5ff7a548edefa66414cf6e77464d0947277c904f324dd23abf58a1ed", 19140144, "RTX Video Super Resolution 1.6.0"),
        new(NativeEngine.Feature.RtxTrueHdr, "runtime/experimental/nvngx_truehdr.dll",
            "9a80575f247190c05fe80eac0c4baa1d0d4d932348f26808310b5ec4bf9eeb4b", 3955752, "RTX Video TrueHDR 1.1.0"),
    ];

    public static IReadOnlyList<VideoRuntime> MissingVideo() => VideoRuntimes.Where(r => !r.Installed).ToList();

    private const string VeyraRepo = "Likely7/Veyra-NRVideo";
    private static string? _veyraPackageUrl;

    private static readonly HttpClient Http = CreateClient();
    private static CancellationTokenSource? _cts;

    public static bool Busy { get; private set; }
    public static double Fraction { get; private set; }
    public static string Message { get; private set; } = "";
    public static bool LastFailed { get; private set; }

    /// <summary>Raised (on a worker thread) whenever Busy/Fraction/Message change.</summary>
    public static event Action? Changed;

    [GeneratedRegex(@"^Veyra-[\d.]+(-[\w.]+)?-NVIDIA-win64-portable\.zip$", RegexOptions.IgnoreCase)]
    private static partial Regex VeyraPackage();

    private static HttpClient CreateClient()
    {
        var client = new HttpClient(new SocketsHttpHandler { AutomaticDecompression = System.Net.DecompressionMethods.None })
        {
            Timeout = TimeSpan.FromMinutes(30),
        };
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("Blackvolt", Updater.CurrentVersion.ToString()));
        return client;
    }

    public static IEnumerable<NrBuild> BuildsFor(NrVariant v) => Builds.Where(b => b.Slot == v);

    /// <summary>The build the user picked for a slot (Runtimes page), or the default one.</summary>
    public static NrBuild Selected(NrVariant v)
    {
        var nr = AppSettings.Current.Video.Nr;
        var tag = v == NrVariant.Original ? nr.OriginalBuild : nr.CompatBuild;
        return Builds.FirstOrDefault(b => b.Slot == v && b.Tag == tag) ?? Builds.First(b => b.Slot == v && b.Default);
    }

    public static void Select(NrVariant v, string tag)
    {
        var nr = AppSettings.Current.Video.Nr;
        if (v == NrVariant.Original) nr.OriginalBuild = tag;
        else nr.CompatBuild = tag;
        AppSettings.Current.Save();
    }

    /// <summary>The catalogued build an installed file is, by SHA-256, or null.</summary>
    public static NrBuild? Identify(string sha256) =>
        Builds.FirstOrDefault(b => b.Sha256.Equals(sha256, StringComparison.OrdinalIgnoreCase));

    public static string DisplayName(NrVariant v) => v == NrVariant.Original
        ? "NVIDIA original (RTX 50)"
        : "Compatible SF (RTX 20 · 30 · 40 · 50)";

    public static NativeEngine.NrRuntimeInfo Info(NrVariant v)
    {
        var info = new NativeEngine.NrRuntimeInfo { Size = (uint)System.Runtime.InteropServices.Marshal.SizeOf<NativeEngine.NrRuntimeInfo>() };
        try { NativeEngine.NrRuntimeGet(v, ref info); }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException) { }
        return info;
    }

    public static bool Installed(NrVariant v) => Info(v).Installed != 0;

    /// <summary>
    /// First launch on an RTX GPU: fetch the compatible NR runtime in the background, so DLSS NR
    /// just works, as it does out of Veyra's package. Retries at most once a day after a failure.
    /// </summary>
    public static void AutoInstallIfNeeded()
    {
        var nr = AppSettings.Current.Video.Nr;
        var adapter = App.Engine?.Adapter;
        if (!nr.AutoInstall || adapter is null || adapter.Vendor != NativeEngine.Vendor.Nvidia ||
            !adapter.Name.Contains("RTX", StringComparison.OrdinalIgnoreCase)) return;
        var nrMissing = !Installed(NrVariant.Compat) && !Installed(NrVariant.Original);
        if (!nrMissing && MissingVideo().Count == 0) return;
        var last = AppSettings.Current.Update.LastNrAttemptUtc;
        if (DateTime.UtcNow - last < TimeSpan.FromHours(20) && nrMissing) return;
        AppSettings.Current.Update.LastNrAttemptUtc = DateTime.UtcNow;
        AppSettings.Current.Save();
        _ = InstallAsync(nrMissing ? [NrVariant.Compat] : []);
    }

    /// <summary>
    /// Installer request: the builds this GPU can use (RTX 50 gets the NVIDIA original too), unless
    /// they are already there. No-op on GPUs without DLSS.
    /// </summary>
    public static void InstallForThisGpu()
    {
        var adapter = App.Engine?.Adapter;
        if (adapter is null || adapter.Vendor != NativeEngine.Vendor.Nvidia ||
            !adapter.Name.Contains("RTX", StringComparison.OrdinalIgnoreCase)) return;
        var wanted = new List<NrVariant>();
        if (!Installed(NrVariant.Compat)) wanted.Add(NrVariant.Compat);
        if (adapter.Name.Contains("RTX 50", StringComparison.OrdinalIgnoreCase) && !Installed(NrVariant.Original))
            wanted.Add(NrVariant.Original);
        if (wanted.Count == 0 && MissingVideo().Count == 0) return;
        AppSettings.Current.Update.LastNrAttemptUtc = DateTime.UtcNow;
        AppSettings.Current.Save();
        _ = InstallAsync(wanted);
    }

    public static void Cancel() => _cts?.Cancel();

    /// <summary>
    /// Downloads and installs the selected NR build of each variant, plus the RTX Video runtimes
    /// (VSR, TrueHDR) that are missing. True when everything was installed.
    /// </summary>
    public static async Task<bool> InstallAsync(IReadOnlyList<NrVariant> variants, bool video = true)
    {
        var videoJobs = video ? MissingVideo() : [];
        if (Busy || variants.Count + videoJobs.Count == 0) return false;
        Busy = true;
        LastFailed = false;
        _cts = new CancellationTokenSource();
        var ct = _cts.Token;
        var mirror = AppSettings.Current.Update.Mirror;
        var temp = Path.Combine(Path.GetTempPath(), "Blackvolt-nr-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try
        {
            var installed = new List<string>();
            for (int i = 0; i < variants.Count; i++)
            {
                var build = Selected(variants[i]);
                var target = Path.Combine(temp, $"{(int)build.Slot}-nvngx_dlssnr.dll");
                var label = $"DLSS NR {build.Label} ({build.Size / 1048576.0:0} MB)";
                int index = i;
                int count = variants.Count + videoJobs.Count;
                var progress = new Progress<double>(f => Report((index + f) / count, $"Descargando {label}… {f * 100:0}%"));
                Report((double)i / count, $"Conectando con rhi-repo para {build.Tag}…");

                string sha;
                try
                {
                    sha = await FetchFromZipAsync(Mirrors.Asset(build.Url, mirror), target, progress, ct);
                }
                catch (Exception ex) when (build.Default && ex is HttpRequestException or InvalidDataException)
                {
                    // rhi-repo unreachable: Veyra's NVIDIA package carries the same default builds.
                    Report(i / (double)(variants.Count + videoJobs.Count), "rhi-repo no respondió; probando el paquete de Veyra…");
                    var member = "runtime/experimental/" + (build.Slot == NrVariant.Original ? "nr-original" : "nr-ampere") + "/nvngx_dlssnr.dll";
                    sha = await FetchFromVeyraAsync(member, target, progress, mirror, ct);
                }

                if (!sha.Equals(build.Sha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"{build.Tag}: la DLL descargada no coincide con su SHA-256 publicado.");
                CheckPe(target);
                var info = new NativeEngine.NrRuntimeInfo { Size = (uint)System.Runtime.InteropServices.Marshal.SizeOf<NativeEngine.NrRuntimeInfo>() };
                var r = NativeEngine.NrRuntimeImport(target, build.Slot, ref info);
                if (r == NativeEngine.Result.Internal)
                    throw new IOException("No se pudo reemplazar el runtime: está en uso. Cierra el reproductor o el streaming e inténtalo de nuevo.");
                r.ThrowIfFailed();
                installed.Add(build.Label);
            }
            int total = variants.Count + videoJobs.Count;
            for (int j = 0; j < videoJobs.Count; j++)
            {
                var rt = videoJobs[j];
                int index = variants.Count + j;
                var target = Path.Combine(temp, rt.Dll);
                var progress = new Progress<double>(f => Report((index + f) / total, $"Descargando {rt.Label} ({rt.Size / 1048576.0:0} MB)… {f * 100:0}%"));
                Report((double)index / total, $"Preparando {rt.Label}…");
                var sha = await FetchFromVeyraAsync(rt.Member, target, progress, mirror, ct);
                if (!sha.Equals(rt.Sha256, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException($"{rt.Label}: el archivo descargado no coincide con su SHA-256.");
                CheckPe(target);
                Runtimes.Import(target, rt.Feature);
                installed.Add(rt.Label);
            }
            Report(1, "Listo: " + string.Join("; ", installed) + ".");
            return true;
        }
        catch (OperationCanceledException)
        {
            LastFailed = true;
            Report(0, "Descarga cancelada.");
            return false;
        }
        catch (Exception ex) when (ex is HttpRequestException or IOException or InvalidDataException or JsonException
                                       or EngineException or UnauthorizedAccessException or NotSupportedException)
        {
            LastFailed = true;
            Report(0, "No se pudieron instalar los runtimes de NVIDIA: " + ex.Message +
                      (ex is HttpRequestException ? " Prueba otro servidor de descarga en Ajustes." : ""));
            return false;
        }
        finally
        {
            Busy = false;
            try { Directory.Delete(temp, recursive: true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            Changed?.Invoke();
        }
    }

    /// <summary>Extracts the first nvngx_dlssnr.dll of a remote zip (ranges, or the whole zip). Returns its SHA-256.</summary>
    private static async Task<string> FetchFromZipAsync(string url, string target, IProgress<double> progress, CancellationToken ct,
                                                        string memberSuffix = "nvngx_dlssnr.dll")
    {
        try
        {
            var zip = await RemoteZip.OpenAsync(Http, url, ct);
            var entry = zip.Entries.FirstOrDefault(e => e.Name.EndsWith(memberSuffix, StringComparison.OrdinalIgnoreCase))
                        ?? throw new InvalidDataException("El zip no contiene nvngx_dlssnr.dll.");
            return await zip.ExtractAsync(entry, target, progress, ct);
        }
        catch (NotSupportedException)
        {
            // No range support on this route: download the whole zip once.
            var zipPath = target + ".zip";
            using (var response = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct))
            {
                response.EnsureSuccessStatusCode();
                var total = response.Content.Headers.ContentLength ?? 0;
                await using var input = await response.Content.ReadAsStreamAsync(ct);
                await using var output = File.Create(zipPath);
                var buffer = new byte[1 << 20];
                long done = 0;
                int read;
                while ((read = await input.ReadAsync(buffer, ct)) > 0)
                {
                    await output.WriteAsync(buffer.AsMemory(0, read), ct);
                    done += read;
                    if (total > 0) progress.Report(Math.Min(0.99, (double)done / total));
                }
            }
            try
            {
                using var archive = ZipFile.OpenRead(zipPath);
                var entry = archive.Entries.FirstOrDefault(e => e.FullName.Replace('\\', '/').EndsWith(memberSuffix, StringComparison.OrdinalIgnoreCase))
                            ?? throw new InvalidDataException("El zip no contiene nvngx_dlssnr.dll.");
                await using var data = entry.Open();
                return await RemoteZip.WriteVerifiedAsync(data, target, entry.Length, entry.Crc32, ct);
            }
            finally
            {
                File.Delete(zipPath);
            }
        }
    }

    /// <summary>A member of Veyra's latest NVIDIA portable package (only that file is read).</summary>
    private static async Task<string> FetchFromVeyraAsync(string member, string target, IProgress<double> progress, int mirror,
                                                         CancellationToken ct)
    {
        _veyraPackageUrl ??= await FindVeyraPackageAsync(mirror, ct);
        return await FetchFromZipAsync(Mirrors.Asset(_veyraPackageUrl, mirror), target, progress, ct, member);
    }

    private static async Task<string> FindVeyraPackageAsync(int mirror, CancellationToken ct)
    {
        var api = Mirrors.Api($"https://api.github.com/repos/{VeyraRepo}/releases?per_page=15", mirror);
        using var request = new HttpRequestMessage(HttpMethod.Get, api);
        request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        using var response = await Http.SendAsync(request, ct);
        response.EnsureSuccessStatusCode();
        using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync(ct));
        foreach (var release in doc.RootElement.EnumerateArray())
        {
            if (release.TryGetProperty("draft", out var d) && d.GetBoolean()) continue;
            foreach (var asset in release.GetProperty("assets").EnumerateArray())
            {
                if (VeyraPackage().IsMatch(asset.GetProperty("name").GetString() ?? ""))
                    return asset.GetProperty("browser_download_url").GetString()!;
            }
        }
        throw new InvalidDataException("No se encontró ningún paquete NVIDIA de Veyra publicado.");
    }

    private static void CheckPe(string path)
    {
        using var f = File.OpenRead(path);
        if (f.Length < 1 << 20 || f.ReadByte() != 'M' || f.ReadByte() != 'Z')
            throw new InvalidDataException("El archivo descargado no es una DLL válida.");
    }

    private static void Report(double fraction, string message)
    {
        Fraction = Math.Clamp(fraction, 0, 1);
        Message = message;
        Changed?.Invoke();
    }
}
