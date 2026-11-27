using System.Text.Json;

namespace Blackvolt.App.Services;

/// <summary>One finished playback, capture, Remote Play or export session.</summary>
public sealed record SessionRecord
{
    public string Kind { get; init; } = "";        // "Reproductor", "Captura", "Remote Play", "Exportar"
    public string Title { get; init; } = "";
    public DateTime StartedUtc { get; init; }
    public double DurationS { get; init; }
    public uint Width { get; init; }
    public uint Height { get; init; }
    public double AvgFps { get; init; }
    public double FrameP99Ms { get; init; }        // worst p99 seen during the session
    public ulong Presented { get; init; }
    public ulong Dropped { get; init; }
    public string Detail { get; init; } = "";      // codec, enhancements, bitrate…
}

/// <summary>Session history, kept in %LOCALAPPDATA%\Blackvolt\history.json (newest first, capped).</summary>
internal static class SessionHistory
{
    private const int MaxEntries = 300;
    private static readonly string FilePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Blackvolt", "history.json");
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = false };
    private static List<SessionRecord>? _entries;

    public static event Action? Changed;

    public static IReadOnlyList<SessionRecord> Entries => _entries ??= Load();

    public static void Add(SessionRecord record)
    {
        var list = _entries ??= Load();
        list.Insert(0, record);
        if (list.Count > MaxEntries) list.RemoveRange(MaxEntries, list.Count - MaxEntries);
        Save(list);
        Changed?.Invoke();
    }

    public static void Clear()
    {
        _entries = [];
        Save(_entries);
        Changed?.Invoke();
    }

    private static List<SessionRecord> Load()
    {
        try
        {
            if (File.Exists(FilePath))
                return JsonSerializer.Deserialize<List<SessionRecord>>(File.ReadAllText(FilePath), Options) ?? [];
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException)
        {
        }
        return [];
    }

    private static void Save(List<SessionRecord> list)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            var tmp = FilePath + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(list, Options));
            File.Move(tmp, FilePath, overwrite: true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
        }
    }
}

/// <summary>Collects samples while a session runs and stores a summary when it ends.</summary>
internal sealed class SessionTracker
{
    private readonly string _kind;
    private readonly string _title;
    private readonly DateTime _started = DateTime.UtcNow;
    private readonly System.Diagnostics.Stopwatch _clock = System.Diagnostics.Stopwatch.StartNew();
    private double _firstAt = -1, _lastAt;
    private ulong _firstPresented, _lastPresented, _firstDropped, _lastDropped;
    private uint _width, _height;
    private double _worstP99;
    private string _detail = "";
    private bool _finished;

    public SessionTracker(string kind, string title)
    {
        _kind = kind;
        _title = title;
    }

    public void Sample(uint width, uint height, ulong presented, ulong dropped, double frameP99Ms, string detail)
    {
        var now = _clock.Elapsed.TotalSeconds;
        if (_firstAt < 0 || presented < _lastPresented)
        {
            _firstAt = now;
            _firstPresented = presented;
            _firstDropped = dropped;
        }
        _lastAt = now;
        _lastPresented = presented;
        _lastDropped = dropped;
        if (width > 0) (_width, _height) = (width, height);
        // Ignore the first seconds: opening and clock sync inflate the tail.
        if (now - _firstAt > 2 && frameP99Ms > _worstP99) _worstP99 = frameP99Ms;
        if (detail.Length > 0) _detail = detail;
    }

    /// <summary>Stores the session if it lasted long enough to mean anything.</summary>
    public void Finish()
    {
        if (_finished) return;
        _finished = true;
        var span = _lastAt - _firstAt;
        if (_firstAt < 0 || span < 5) return;
        var presented = _lastPresented - _firstPresented;
        SessionHistory.Add(new SessionRecord
        {
            Kind = _kind,
            Title = _title,
            StartedUtc = _started,
            DurationS = _clock.Elapsed.TotalSeconds,
            Width = _width,
            Height = _height,
            AvgFps = presented / span,
            FrameP99Ms = _worstP99,
            Presented = presented,
            Dropped = _lastDropped >= _firstDropped ? _lastDropped - _firstDropped : 0,
            Detail = _detail,
        });
    }
}
