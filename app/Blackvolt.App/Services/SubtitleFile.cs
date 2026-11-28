using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace Blackvolt.App.Services;

internal sealed record SubtitleCue(double Start, double End, string Text);

/// <summary>External subtitle files: SubRip (.srt), WebVTT (.vtt) and ASS/SSA (.ass, .ssa).</summary>
internal static partial class SubtitleFile
{
    public static readonly string[] Extensions = [".srt", ".vtt", ".ass", ".ssa"];

    [GeneratedRegex(@"(?:(\d+):)?(\d{1,2}):(\d{2})[,.](\d{1,3})\s*-->\s*(?:(\d+):)?(\d{1,2}):(\d{2})[,.](\d{1,3})")]
    private static partial Regex TimeLine();

    [GeneratedRegex(@"<[^>]+>")]
    private static partial Regex Tags();

    [GeneratedRegex(@"\{[^}]*\}")]
    private static partial Regex AssTags();

    /// <summary>Same-name subtitle next to a video (movie.srt, movie.es.srt...), or null.</summary>
    public static string? FindNextTo(string videoPath)
    {
        try
        {
            var dir = Path.GetDirectoryName(videoPath);
            if (dir is null || !Directory.Exists(dir)) return null;
            var stem = Path.GetFileNameWithoutExtension(videoPath);
            return Directory.EnumerateFiles(dir, stem + "*")
                .Where(f => Extensions.Contains(Path.GetExtension(f), StringComparer.OrdinalIgnoreCase))
                .OrderBy(f => Path.GetFileNameWithoutExtension(f).Length) // exact name first
                .FirstOrDefault();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    public static IReadOnlyList<SubtitleCue> Load(string path)
    {
        var text = ReadText(path);
        var cues = Path.GetExtension(path).ToLowerInvariant() is ".ass" or ".ssa" ? ParseAss(text) : ParseSrtOrVtt(text);
        return cues.OrderBy(c => c.Start).ToList();
    }

    /// <summary>Text showing at <paramref name="seconds"/> (lines joined), or "".</summary>
    public static string At(IReadOnlyList<SubtitleCue> cues, double seconds)
    {
        // Binary search for the last cue starting at or before t, then look back over overlaps.
        int lo = 0, hi = cues.Count - 1, found = -1;
        while (lo <= hi)
        {
            var mid = (lo + hi) / 2;
            if (cues[mid].Start <= seconds) { found = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        var lines = new List<string>();
        for (var i = found; i >= 0 && i >= found - 8; i--)
            if (cues[i].Start <= seconds && seconds < cues[i].End) lines.Insert(0, cues[i].Text);
        return string.Join('\n', lines);
    }

    private static string ReadText(string path)
    {
        var bytes = File.ReadAllBytes(path);
        // UTF-8 (with or without BOM) is the norm; old SRTs are often Windows-1252.
        try
        {
            return new UTF8Encoding(false, throwOnInvalidBytes: true).GetString(bytes).TrimStart('﻿');
        }
        catch (DecoderFallbackException)
        {
            return Encoding.Latin1.GetString(bytes);
        }
    }

    private static List<SubtitleCue> ParseSrtOrVtt(string text)
    {
        var cues = new List<SubtitleCue>();
        var lines = text.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
        for (var i = 0; i < lines.Length; i++)
        {
            var m = TimeLine().Match(lines[i]);
            if (!m.Success) continue;
            var start = Time(m, 1);
            var end = Time(m, 5);
            var body = new StringBuilder();
            for (i++; i < lines.Length && lines[i].Trim().Length > 0; i++)
            {
                if (body.Length > 0) body.Append('\n');
                body.Append(Tags().Replace(lines[i], "").Trim());
            }
            var content = System.Net.WebUtility.HtmlDecode(body.ToString()).Trim();
            if (content.Length > 0 && end > start) cues.Add(new SubtitleCue(start, end, content));
        }
        return cues;
    }

    private static double Time(Match m, int g)
    {
        var h = m.Groups[g].Success ? int.Parse(m.Groups[g].Value, CultureInfo.InvariantCulture) : 0;
        var min = int.Parse(m.Groups[g + 1].Value, CultureInfo.InvariantCulture);
        var s = int.Parse(m.Groups[g + 2].Value, CultureInfo.InvariantCulture);
        var frac = m.Groups[g + 3].Value;
        var ms = int.Parse(frac, CultureInfo.InvariantCulture) * (frac.Length == 1 ? 100 : frac.Length == 2 ? 10 : 1);
        return h * 3600 + min * 60 + s + ms / 1000.0;
    }

    private static List<SubtitleCue> ParseAss(string text)
    {
        var cues = new List<SubtitleCue>();
        string[]? format = null;
        foreach (var raw in text.Replace("\r\n", "\n").Split('\n'))
        {
            var line = raw.Trim();
            if (line.StartsWith("Format:", StringComparison.OrdinalIgnoreCase) && format is null && cues.Count == 0)
            {
                // The [Events] Format line names the fields; the last one is Text.
                var fields = line[7..].Split(',').Select(f => f.Trim()).ToArray();
                if (fields.Contains("Start", StringComparer.OrdinalIgnoreCase) && fields.Contains("Text", StringComparer.OrdinalIgnoreCase))
                    format = fields;
                continue;
            }
            if (!line.StartsWith("Dialogue:", StringComparison.OrdinalIgnoreCase)) continue;
            format ??= ["Layer", "Start", "End", "Style", "Name", "MarginL", "MarginR", "MarginV", "Effect", "Text"];
            var parts = line[9..].Split(',', format.Length);
            if (parts.Length < format.Length) continue;
            var startAt = Array.FindIndex(format, f => f.Equals("Start", StringComparison.OrdinalIgnoreCase));
            var endAt = Array.FindIndex(format, f => f.Equals("End", StringComparison.OrdinalIgnoreCase));
            if (!TryAssTime(parts[startAt], out var start) || !TryAssTime(parts[endAt], out var end)) continue;
            var content = AssTags().Replace(parts[^1], "").Replace("\\N", "\n").Replace("\\n", "\n").Replace("\\h", " ").Trim();
            if (content.Length > 0 && end > start) cues.Add(new SubtitleCue(start, end, content));
        }
        return cues;
    }

    private static bool TryAssTime(string s, out double seconds)
    {
        seconds = 0;
        var p = s.Trim().Split(':');
        if (p.Length != 3) return false;
        if (!int.TryParse(p[0], out var h) || !int.TryParse(p[1], out var m) ||
            !double.TryParse(p[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var sec)) return false;
        seconds = h * 3600 + m * 60 + sec;
        return true;
    }
}
