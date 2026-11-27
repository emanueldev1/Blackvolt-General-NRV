using System.Text.Json;

namespace Blackvolt.App.Services;

/// <summary>User preferences, saved as JSON in %LOCALAPPDATA%\Blackvolt\settings.json.</summary>
internal sealed class AppSettings
{
    public RemotePlaySettings RemotePlay { get; set; } = new();
    public VideoSettings Video { get; set; } = new();

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
    public bool SuperResolution { get; set; }
    public int SuperResolutionQuality { get; set; } = 4; // 1 fastest .. 4 best
    public bool TrueHdr { get; set; }
    public int HdrPeakNits { get; set; }                 // 0 = what the display reports
    public int HdrContrast { get; set; } = 100;
    public int HdrSaturation { get; set; } = 100;
    public int HdrMiddleGray { get; set; } = 50;
    // Reserved for NVIDIA's official DLSS NR (no implementation ships yet).
    public bool NeuralRendering { get; set; }
    public int NeuralIntensity { get; set; } = 100;
    public bool NeuralTemporal { get; set; } = true;
}
