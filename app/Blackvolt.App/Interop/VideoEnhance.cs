using System.Runtime.InteropServices;
using Blackvolt.App.Services;

namespace Blackvolt.App.Interop;

/// <summary>RTX Video Super Resolution / TrueHDR controls shared by the player and Remote Play.
/// Mirrors the "Video enhancement" section of bv_api.h.</summary>
internal static partial class NativeEnhance
{
    private const string Dll = "blackvolt_engine";

    [Flags]
    public enum Flags : uint
    {
        None = 0,
        VsrActive = 1u << 0,
        HdrActive = 1u << 1,
        VsrUnavailable = 1u << 2,
        HdrUnavailable = 1u << 3,
        DisplaySdr = 1u << 4,
        VsrBypassed = 1u << 5,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Settings
    {
        public uint Size;
        public uint Vsr, VsrQuality, TrueHdr;
        public uint HdrContrast, HdrSaturation, HdrMiddleGray, HdrPeakNits;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        public uint Size;
        public Flags Flags;
        public uint InputWidth, InputHeight, OutputWidth, OutputHeight;
        public float DisplayPeakNits;
        public float GpuConvertMs, GpuVsrMs, GpuHdrMs, GpuTotalMs;
    }

    [LibraryImport(Dll, EntryPoint = "bv_player_set_enhance")]
    public static partial NativeEngine.Result PlayerSetEnhance(nint player, in Settings settings);

    [LibraryImport(Dll, EntryPoint = "bv_player_enhance_status")]
    public static partial NativeEngine.Result PlayerEnhanceStatus(nint player, ref Status status);

    [LibraryImport(Dll, EntryPoint = "bv_stream_set_enhance")]
    public static partial NativeEngine.Result StreamSetEnhance(nint stream, in Settings settings);

    [LibraryImport(Dll, EntryPoint = "bv_stream_enhance_status")]
    public static partial NativeEngine.Result StreamEnhanceStatus(nint stream, ref Status status);

    [LibraryImport(Dll, EntryPoint = "bv_display_hdr")]
    public static partial uint DisplayHdr(out float peakNits);

    public static Settings From(VideoSettings s) => new()
    {
        Size = (uint)Marshal.SizeOf<Settings>(),
        Vsr = s.SuperResolution ? 1u : 0u,
        VsrQuality = (uint)Math.Clamp(s.SuperResolutionQuality, 1, 4),
        TrueHdr = s.TrueHdr ? 1u : 0u,
        HdrContrast = (uint)Math.Clamp(s.HdrContrast, 0, 200),
        HdrSaturation = (uint)Math.Clamp(s.HdrSaturation, 0, 200),
        HdrMiddleGray = (uint)Math.Clamp(s.HdrMiddleGray, 10, 100),
        HdrPeakNits = s.HdrPeakNits <= 0 ? 0u : (uint)Math.Clamp(s.HdrPeakNits, 400, 2000),
    };

    public static Status NewStatus() => new() { Size = (uint)Marshal.SizeOf<Status>() };

    /// <summary>One line per active stage, for the stats overlays.</summary>
    public static string Describe(in Status s)
    {
        var lines = new List<string>();
        if (s.Flags.HasFlag(Flags.VsrActive))
            lines.Add($"RTX VSR    {s.InputWidth}×{s.InputHeight} → {s.OutputWidth}×{s.OutputHeight}  {s.GpuVsrMs:0.00} ms");
        if (s.Flags.HasFlag(Flags.HdrActive))
            lines.Add($"TrueHDR    {s.GpuHdrMs,7:0.00} ms  (pico {s.DisplayPeakNits:0} nits)");
        if (s.GpuTotalMs > 0)
            lines.Add($"GPU total  {s.GpuTotalMs,7:0.00} ms");
        return string.Join('\n', lines);
    }

    /// <summary>Short human explanation of why a requested stage is not running, or null.</summary>
    public static string? Explain(in Status s, VideoSettings wanted)
    {
        if (wanted.SuperResolution && s.Flags.HasFlag(Flags.VsrUnavailable))
            return "Super Resolution no está disponible: necesita una GeForce RTX y un driver reciente.";
        if (wanted.TrueHdr && s.Flags.HasFlag(Flags.DisplaySdr))
            return "TrueHDR necesita que actives HDR en Windows (Configuración › Pantalla).";
        if (wanted.TrueHdr && s.Flags.HasFlag(Flags.HdrUnavailable))
            return "TrueHDR no está disponible: necesita una GeForce RTX y un driver reciente.";
        if (wanted.SuperResolution && s.Flags.HasFlag(Flags.VsrBypassed))
            return "La ventana es más pequeña que el video: no hace falta escalar.";
        return null;
    }
}
