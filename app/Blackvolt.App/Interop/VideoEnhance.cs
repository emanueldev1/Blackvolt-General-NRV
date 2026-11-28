using System.Runtime.InteropServices;
using Blackvolt.App.Services;

namespace Blackvolt.App.Interop;

/// <summary>DLSS NR, RTX Video Super Resolution and TrueHDR controls shared by the player and
/// Remote Play. Mirrors the "Video enhancement" section of bv_api.h.</summary>
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
        NeuralActive = 1u << 6,
        NeuralUnavailable = 1u << 7,
        InterpolationActive = 1u << 8,
        NisActive = 1u << 9,
        HdrSource = 1u << 10,
        HdrPassthrough = 1u << 11,
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Settings
    {
        public uint Size;
        public uint Vsr, VsrQuality, TrueHdr;
        public uint HdrContrast, HdrSaturation, HdrMiddleGray, HdrPeakNits;
        public uint Neural, NeuralIntensity, NeuralTemporal;
        // API 9: DLSS NR
        public uint NrRuntime, NrPerformanceModel, NrSize;
        public int NrStyle;
        public float NrTone, NrStructure, NrSkin;
        public uint NrAutoMask, NrUiCorrection;
        public float NrTotal, NrDarken, NrBrighten, NrColor, NrLuminance;
        public uint NrCorrection, NrCorrectionAuto;
        public float NrCorrectionAmount, NrHue, NrChroma, NrHighlight, NrCompression;
        public float NrNeutral, NrColorKeep, NrLumaKeep, NrShadow;
        public uint NrProtect;
        public float NrFeather;
        public uint NrAntiFlicker;
        public float NrStability;
        public uint Interpolation, Compare, Denoise, Scaler;
        public float Sharpness;
        public float Brightness, Contrast, Saturation, Vibrance, Temperature, Tint, Gamma;
        public uint Fxaa;
        public uint HdrTonemap;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct Status
    {
        public uint Size;
        public Flags Flags;
        public uint InputWidth, InputHeight, OutputWidth, OutputHeight;
        public float DisplayPeakNits;
        public float GpuConvertMs, GpuVsrMs, GpuHdrMs, GpuTotalMs;
        public float GpuNeuralMs;
        public fixed byte NeuralNote[160];
        // API 9
        public uint NrWidth, NrHeight;
        public float GpuNrPrepareMs, GpuNrCompositeMs;
        public NativeEngine.NrVariant NrVariant;
        public fixed byte NrLabel[96];
        public float GpuPresentMs;
        public float GpuInterpMs;
        public ulong InterpolatedFrames;

        public readonly string NeuralNoteText { get { fixed (byte* p = NeuralNote) return Utf8.Read(p, 160); } }
        public readonly string NrLabelText { get { fixed (byte* p = NrLabel) return Utf8.Read(p, 96); } }
        public readonly float GpuNrTotalMs => GpuNrPrepareMs + GpuNeuralMs + GpuNrCompositeMs;
    }

    [LibraryImport(Dll, EntryPoint = "bv_player_set_enhance")]
    public static partial NativeEngine.Result PlayerSetEnhance(nint player, in Settings settings);

    [LibraryImport(Dll, EntryPoint = "bv_player_enhance_status")]
    public static partial NativeEngine.Result PlayerEnhanceStatus(nint player, ref Status status);

    [LibraryImport(Dll, EntryPoint = "bv_stream_set_enhance")]
    public static partial NativeEngine.Result StreamSetEnhance(nint stream, in Settings settings);

    [LibraryImport(Dll, EntryPoint = "bv_stream_enhance_status")]
    public static partial NativeEngine.Result StreamEnhanceStatus(nint stream, ref Status status);

    [LibraryImport(Dll, EntryPoint = "bv_player_screenshot", StringMarshalling = StringMarshalling.Utf8)]
    public static partial NativeEngine.Result PlayerScreenshot(nint player, string pngPath);

    [LibraryImport(Dll, EntryPoint = "bv_stream_screenshot", StringMarshalling = StringMarshalling.Utf8)]
    public static partial NativeEngine.Result StreamScreenshot(nint stream, string pngPath);

    /// <summary>Pictures\Blackvolt\Blackvolt_yyyyMMdd_HHmmss.png</summary>
    public static string NewScreenshotPath()
    {
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyPictures), "Blackvolt");
        Directory.CreateDirectory(dir);
        return Path.Combine(dir, $"Blackvolt_{DateTime.Now:yyyyMMdd_HHmmss_fff}.png");
    }

    [LibraryImport(Dll, EntryPoint = "bv_display_hdr")]
    public static partial uint DisplayHdr(out float peakNits);

    public static Settings From(VideoSettings s)
    {
        var n = s.Nr;
        return new Settings
        {
            Size = (uint)Marshal.SizeOf<Settings>(),
            Vsr = s.SuperResolution ? 1u : 0u,
            VsrQuality = (uint)Math.Clamp(s.SuperResolutionQuality, 1, 4),
            TrueHdr = s.TrueHdr ? 1u : 0u,
            HdrContrast = (uint)Math.Clamp(s.HdrContrast, 0, 200),
            HdrSaturation = (uint)Math.Clamp(s.HdrSaturation, 0, 200),
            HdrMiddleGray = (uint)Math.Clamp(s.HdrMiddleGray, 10, 100),
            HdrPeakNits = s.HdrPeakNits <= 0 ? 0u : (uint)Math.Clamp(s.HdrPeakNits, 400, 2000),
            Neural = n.Enabled ? 1u : 0u,
            NeuralIntensity = (uint)Math.Clamp(n.Intensity, 0, 100),
            NrRuntime = (uint)Math.Clamp(n.Runtime, 0, 2),
            NrPerformanceModel = n.PerformanceModel ? 1u : 0u,
            NrSize = (uint)n.Size,
            NrStyle = Math.Clamp(n.Style, 0, 2),
            NrTone = (float)n.Tone,
            NrStructure = (float)n.Structure,
            NrSkin = n.SkinEnabled ? (float)n.Skin : -1f,
            NrAutoMask = n.AutoMask ? 1u : 0u,
            NrUiCorrection = n.UiCorrection ? 1u : 0u,
            NrTotal = (float)n.Total,
            NrDarken = (float)n.Darken,
            NrBrighten = (float)n.Brighten,
            NrColor = (float)n.Color,
            NrLuminance = (float)n.Luminance,
            NrCorrection = n.Correction ? 1u : 0u,
            NrCorrectionAuto = n.CorrectionAuto ? 1u : 0u,
            NrCorrectionAmount = (float)n.CorrectionAmount,
            NrHue = (float)n.Hue,
            NrChroma = (float)n.Chroma,
            NrHighlight = (float)n.Highlight,
            NrCompression = (float)n.Compression,
            NrNeutral = (float)n.Neutral,
            NrColorKeep = (float)n.ColorKeep,
            NrLumaKeep = (float)n.LumaKeep,
            NrShadow = (float)n.Shadow,
            NrProtect = (uint)Math.Clamp(n.Protect, 0, 4),
            NrFeather = (float)n.Feather,
            NrAntiFlicker = (uint)Math.Clamp(n.AntiFlicker, 0, 4),
            NrStability = (float)n.Stability,
            Interpolation = (uint)Math.Clamp(s.Interpolation, 1, 4),
            Compare = s.Compare ? 1u : 0u,
            Denoise = (uint)Math.Clamp(s.Denoise, 0, 3),
            Scaler = s.Scaler == 1 ? 1u : 0u,
            Sharpness = (float)Math.Clamp(s.Sharpness, 0, 1),
            Brightness = (float)s.Brightness,
            Contrast = (float)s.Contrast,
            Saturation = (float)s.Saturation,
            Vibrance = (float)s.Vibrance,
            Temperature = (float)s.Temperature,
            Tint = (float)s.Tint,
            Gamma = (float)s.Gamma,
            Fxaa = s.Fxaa ? 1u : 0u,
            HdrTonemap = s.HdrTonemap ? 1u : 0u,
        };
    }

    public static Status NewStatus() => new() { Size = (uint)Marshal.SizeOf<Status>() };

    /// <summary>Short human explanation of why a requested stage is not running, or null.</summary>
    public static string? Explain(in Status s, VideoSettings wanted)
    {
        if (wanted.Nr.Enabled && s.Flags.HasFlag(Flags.NeuralUnavailable))
            return "DLSS NR: " + (s.NeuralNoteText is { Length: > 0 } note ? note : "no disponible.");
        if (wanted.SuperResolution && s.Flags.HasFlag(Flags.VsrUnavailable))
            return "Super Resolution no está disponible: necesita una GeForce RTX y un driver reciente.";
        if (wanted.SuperResolution && wanted.Scaler == 0 && s.Flags.HasFlag(Flags.NisActive))
            return "RTX VSR no está disponible en esta GPU: se usa NVIDIA Image Scaling.";
        if (wanted.TrueHdr && s.Flags.HasFlag(Flags.DisplaySdr))
            return "TrueHDR necesita que actives HDR en Windows (Configuración › Pantalla).";
        if (wanted.TrueHdr && s.Flags.HasFlag(Flags.HdrUnavailable))
            return "TrueHDR no está disponible: necesita una GeForce RTX y un driver reciente.";
        if (wanted.SuperResolution && s.Flags.HasFlag(Flags.VsrBypassed))
            return "La ventana es más pequeña que el video: no hace falta escalar.";
        return null;
    }

    /// <summary>Whether the note means the NR runtime file is missing (offer a download).</summary>
    public static bool NrMissing(in Status s) =>
        s.Flags.HasFlag(Flags.NeuralUnavailable) && s.NeuralNoteText.Contains("Descárgalo", StringComparison.Ordinal);
}
