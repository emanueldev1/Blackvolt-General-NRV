using System.Text.Json;

namespace Blackvolt.App.Services;

/// <summary>User preferences, saved as JSON in %LOCALAPPDATA%\Blackvolt\settings.json.</summary>
internal sealed class AppSettings
{
    public RemotePlaySettings RemotePlay { get; set; } = new();
    public VideoSettings Video { get; set; } = new();
    public CaptureSettings Capture { get; set; } = new();
    public SubtitleSettings Subtitles { get; set; } = new();
    public ExportSettings Export { get; set; } = new();
    public UpdateSettings Update { get; set; } = new();
    /// <summary>User presets: every video enhancement setting under a name.</summary>
    public Dictionary<string, VideoSettings> Presets { get; set; } = new(StringComparer.OrdinalIgnoreCase);

    private static readonly string FilePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Blackvolt", "settings.json");

    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    private static AppSettings? _current;

    public static AppSettings Current => _current ??= Load();

    private static AppSettings Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath), Options) ?? new AppSettings();
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
            // Corrupt or locked file: fall back to defaults, the next save rewrites it.
        }
        return new AppSettings();
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            var tmp = FilePath + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(this, Options));
            File.Move(tmp, FilePath, overwrite: true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
        }
    }
}

internal sealed class RemotePlaySettings
{
    public int Height { get; set; } = 1080;   // 360, 540, 720, 1080
    public int Fps { get; set; } = 60;
    public int BitrateKbps { get; set; } = 0;  // 0 = automatic
    public int Codec { get; set; } = 1;        // 0 H.264, 1 HEVC, 2 HEVC HDR
    public bool AutoReconnect { get; set; } = true;
    public bool WakeIfAsleep { get; set; } = true;
    public bool DualSenseFeatures { get; set; } = true;
    public double Volume { get; set; } = 1.0;
    public bool ShowStats { get; set; }

    public int Width => Height switch { 360 => 640, 540 => 960, 720 => 1280, _ => 1920 };
}

/// <summary>Video enhancement shared by the player and Remote Play.</summary>
internal sealed class VideoSettings
{
    public bool ShowStats { get; set; }
    public int Interpolation { get; set; } = 1;          // frames shown per source frame, 1 = off .. 4
    public bool Compare { get; set; }                    // A/B: original on the left half
    public int Denoise { get; set; }                     // 0 off, 1 gentle, 2 medium, 3 strong
    public int Scaler { get; set; }                      // 0 RTX VSR (NIS fallback), 1 NVIDIA Image Scaling
    public double Sharpness { get; set; } = 0.25;        // NIS 0..1
    // Colour (after NR)
    public double Brightness { get; set; }
    public double Contrast { get; set; } = 1;
    public double Saturation { get; set; } = 1;
    public double Vibrance { get; set; }
    public double Temperature { get; set; }
    public double Tint { get; set; }
    public double Gamma { get; set; } = 1;
    public bool Fxaa { get; set; }
    public bool HdrTonemap { get; set; }                 // show HDR10/HLG as SDR even on an HDR display

    public void ResetColor() =>
        (Brightness, Contrast, Saturation, Vibrance, Temperature, Tint, Gamma) = (0, 1, 1, 0, 0, 0, 1);
    public bool SuperResolution { get; set; }
    public int SuperResolutionQuality { get; set; } = 4; // 1 fastest .. 4 best
    public bool TrueHdr { get; set; }
    public int HdrPeakNits { get; set; }                 // 0 = what the display reports
    public int HdrContrast { get; set; } = 100;
    public int HdrSaturation { get; set; } = 100;
    public int HdrMiddleGray { get; set; } = 50;
    public NrSettings Nr { get; set; } = new();

    public VideoSettings Clone()
    {
        var copy = (VideoSettings)MemberwiseClone();
        copy.Nr = Nr.Clone();
        return copy;
    }

    /// <summary>Takes every enhancement value from <paramref name="other"/> (not the overlay switch).</summary>
    public void CopyFrom(VideoSettings other)
    {
        foreach (var p in typeof(VideoSettings).GetProperties())
        {
            if (!p.CanWrite || p.Name is nameof(ShowStats) or nameof(Nr)) continue;
            p.SetValue(this, p.GetValue(other));
        }
        foreach (var p in typeof(NrSettings).GetProperties())
        {
            if (p.CanWrite && p.Name is not (nameof(NrSettings.AutoInstall))) p.SetValue(Nr, p.GetValue(other.Nr));
        }
    }
}

/// <summary>DLSS NR (neural rendering) controls, the same set Veyra exposes per NR layer.</summary>
internal sealed class NrSettings
{
    public bool Enabled { get; set; }
    // Runtime and model (changing them recreates NR)
    public int Runtime { get; set; }                 // 0 auto, 1 compatible SF (RTX 20-50), 2 NVIDIA original (RTX 50)
    public bool PerformanceModel { get; set; } = true; // compatible build: model B (newer) / A
    public int Size { get; set; } = 2;               // 0 native, 1 1440p, 2 1080p, 3 720p, 4 480p, 125..200 = 25..100 %
    // Model parameters
    public int Intensity { get; set; } = 100;        // 0..100
    public int Style { get; set; }                   // 0..2
    public double Tone { get; set; } = 1;            // 0..1
    public double Structure { get; set; } = 1;       // 0..1
    public bool SkinEnabled { get; set; }
    public double Skin { get; set; } = 1;            // 0..2
    public bool AutoMask { get; set; }
    public bool UiCorrection { get; set; }
    // How much of the change is applied
    public double Total { get; set; } = 1;           // 0..5
    public double Darken { get; set; } = 1;          // 0..2
    public double Brighten { get; set; } = 1;
    public double Color { get; set; } = 1;
    public double Luminance { get; set; } = 1;
    // Picture correction
    public bool Correction { get; set; }
    public bool CorrectionAuto { get; set; } = true;
    public double CorrectionAmount { get; set; } = 1;
    public double Hue { get; set; } = 1;
    public double Chroma { get; set; } = 0.75;
    public double Highlight { get; set; } = 1;
    public double Compression { get; set; } = 0.75;
    public double Neutral { get; set; }
    public double ColorKeep { get; set; }
    public double LumaKeep { get; set; }
    public double Shadow { get; set; }
    // Protected regions
    public int Protect { get; set; }                 // 0 none, 1 bottom, 2 top, 3 top+bottom, 4 centre
    public double Feather { get; set; } = 8;
    // Anti-flicker (NR change accumulated where the picture is still)
    public int AntiFlicker { get; set; }             // 0 off, 1/2 still soft/strong, 3/4 following motion
    public double Stability { get; set; } = 1;       // 0..1
    // Runtime download
    public bool AutoInstall { get; set; } = true;    // fetch the NR runtime on first launch with an RTX GPU
    public string CompatBuild { get; set; } = "";     // rhi-repo tag for the compatible slot ("" = default)
    public string OriginalBuild { get; set; } = "";   // rhi-repo tag for the NVIDIA original slot

    public NrSettings Clone() => (NrSettings)MemberwiseClone();

    /// <summary>Resets everything but on/off, runtime and download preferences.</summary>
    public void ResetLook()
    {
        var d = new NrSettings();
        (PerformanceModel, Size, Intensity, Style, Tone, Structure) = (d.PerformanceModel, d.Size, d.Intensity, d.Style, d.Tone, d.Structure);
        (SkinEnabled, Skin, AutoMask, UiCorrection) = (d.SkinEnabled, d.Skin, d.AutoMask, d.UiCorrection);
        (Total, Darken, Brighten, Color, Luminance) = (d.Total, d.Darken, d.Brighten, d.Color, d.Luminance);
        (Correction, CorrectionAuto, CorrectionAmount) = (d.Correction, d.CorrectionAuto, d.CorrectionAmount);
        (Hue, Chroma, Highlight, Compression) = (d.Hue, d.Chroma, d.Highlight, d.Compression);
        (Neutral, ColorKeep, LumaKeep, Shadow) = (d.Neutral, d.ColorKeep, d.LumaKeep, d.Shadow);
        (Protect, Feather, AntiFlicker, Stability) = (d.Protect, d.Feather, d.AntiFlicker, d.Stability);
    }

    /// <summary>Starting points. Every value stays editable afterwards.</summary>
    public void ApplyPreset(string preset)
    {
        ResetLook();
        switch (preset)
        {
            case "natural":   // subtle: half the change, colour and lightness kept close to the source
                Total = 0.6; Correction = true; CorrectionAuto = true; CorrectionAmount = 1; Style = 0; AntiFlicker = 1;
                break;
            case "intenso":   // strongest look the model gives
                Total = 1.5; Style = 1; Correction = false;
                break;
            case "rendimiento": // lowest GPU cost
                Size = 3; PerformanceModel = true;
                break;
            default:            // "equilibrado": Veyra defaults
                break;
        }
    }
}

/// <summary>How subtitles look and which ones load by themselves.</summary>
internal sealed class SubtitleSettings
{
    public double Size { get; set; } = 30;          // text size (DIP)
    public bool Background { get; set; } = true;    // dark box behind the text
    public bool Top { get; set; }                   // show at the top instead of the bottom
    public bool AutoLoad { get; set; } = true;      // movie.srt next to movie.mkv
    public bool AutoEmbedded { get; set; } = true;  // first embedded track when there is no file
}

/// <summary>Last capture device and mode the user picked.</summary>
internal sealed class CaptureSettings
{
    public bool ResumeLastSource { get; set; }      // reopen the last file / capture at startup
    public string LastUrl { get; set; } = "";
    public string LastTitle { get; set; } = "";
    public double LastPosition { get; set; }
    public string VideoDevice { get; set; } = "";
    public string AudioDevice { get; set; } = "";   // "" = no audio
    public int Height { get; set; }                 // 0 = device default
    public int Fps { get; set; }                    // 0 = device default

    public int Width => Height switch { 720 => 1280, 1080 => 1920, 1440 => 2560, 2160 => 3840, _ => 0 };
}

/// <summary>Last export choices.</summary>
internal sealed class ExportSettings
{
    public int Codec { get; set; } = 1;          // 0 H.264, 1 HEVC, 2 AV1
    public int Height { get; set; }              // 0 = keep
    public int Quality { get; set; } = 70;       // 0..100
    public bool Audio { get; set; } = true;
    public string Container { get; set; } = ".mp4";
    public bool Nr { get; set; }                  // apply DLSS NR with the player's settings
}

/// <summary>Automatic update checks against GitHub Releases.</summary>
internal sealed class UpdateSettings
{
    public int Mirror { get; set; }                 // 0 GitHub, 1 gh-proxy, 2 ghfast (also used for NR downloads)
    public DateTime LastNrAttemptUtc { get; set; }   // last automatic DLSS NR runtime download
    public bool AutoCheck { get; set; } = true;
    public DateTime LastCheckUtc { get; set; }
    public string SkippedTag { get; set; } = "";
}
