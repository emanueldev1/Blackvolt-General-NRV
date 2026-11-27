using Feature = Blackvolt.App.Interop.NativeEngine.Feature;

namespace Blackvolt.App.Interop;

/// <summary>Where NVIDIA runtime DLLs live and how they are imported.</summary>
internal static class Runtimes
{
    /// <summary>Read-only folder shipped next to Blackvolt.exe (Release package).</summary>
    public static string BundledDir => Path.Combine(AppContext.BaseDirectory, "runtime");

    /// <summary>Writable folder for runtimes the user imports.</summary>
    public static string UserDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Blackvolt", "runtime");

    public static string ManifestPath => Path.Combine(BundledDir, "manifest.json");

    public static void Configure()
    {
        Directory.CreateDirectory(UserDir);
        NativeEngine.SetRuntimePaths(BundledDir, UserDir, ManifestPath).ThrowIfFailed();
    }

    public static string DisplayName(Feature f) => f switch
    {
        Feature.DlssSuperResolution => "DLSS Super Resolution",
        Feature.DlssRayReconstruction => "DLSS Ray Reconstruction",
        Feature.DlssFrameGeneration => "DLSS Frame Generation",
        Feature.DlssNeuralRendering => "Neural Rendering",
        Feature.RtxVideoSuperResolution => "RTX Video Super Resolution",
        Feature.RtxTrueHdr => "RTX Video TrueHDR",
        _ => f.ToString(),
    };

    public static string DllName(Feature f) => f switch
    {
        Feature.DlssSuperResolution => "nvngx_dlss.dll",
        Feature.DlssRayReconstruction => "nvngx_dlssd.dll",
        Feature.DlssFrameGeneration => "nvngx_dlssg.dll",
        Feature.DlssNeuralRendering => "nvngx_dlssnr.dll",
        Feature.RtxVideoSuperResolution => "nvngx_vsr.dll",
        Feature.RtxTrueHdr => "nvngx_truehdr.dll",
        _ => "",
    };

    public static NativeEngine.RuntimeStatus Import(string path, Feature feature)
    {
        NativeEngine.ImportRuntime(path, feature, out var status).ThrowIfFailed();
        return status;
    }

    public static unsafe IReadOnlyList<RuntimeCandidate> Scan(string folder)
    {
        NativeEngine.ScanRuntimes(folder, null, 0, out uint count).ThrowIfFailed();
        if (count == 0) return [];
        var buffer = new NativeEngine.RuntimeCandidate[count];
        fixed (NativeEngine.RuntimeCandidate* p = buffer)
        {
            NativeEngine.ScanRuntimes(folder, p, count, out count).ThrowIfFailed();
        }
        return buffer.Take((int)count)
            .Select(c => new RuntimeCandidate(c.Feature, c.Origin, c.LabelText, c.PathText))
            .ToList();
    }
}

internal sealed record RuntimeCandidate(Feature Feature, NativeEngine.RuntimeOrigin Origin, string Label, string Path);
