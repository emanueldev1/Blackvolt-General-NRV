using System.Runtime.InteropServices;
using System.Text;

namespace Blackvolt.App.Services;

/// <summary>A monitor or a window that can be captured (engine URL "screen:...", FFmpeg gdigrab).</summary>
internal sealed record ScreenSource(string Label, bool IsWindow, int X, int Y, int Width, int Height, string Title)
{
    public string Url(int fps, bool cursor)
    {
        var query = $"fps={fps}&cursor={(cursor ? 1 : 0)}";
        return IsWindow
            ? $"screen:title={Title}?{query}"
            : $"screen:desktop?{query}&region={X},{Y},{Width},{Height}";
    }
}

/// <summary>Monitors (physical pixels) and visible top-level windows with a title.</summary>
internal static partial class ScreenEnumerator
{
    public static IReadOnlyList<ScreenSource> List(nint ownWindow)
    {
        var list = new List<ScreenSource>();
        var monitors = new List<(RECT Rect, bool Primary)>();
        EnumDisplayMonitors(0, 0, (nint monitor, nint _, ref RECT _, nint _) =>
        {
            var info = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
            if (GetMonitorInfoW(monitor, ref info)) monitors.Add((info.rcMonitor, (info.dwFlags & 1) != 0));
            return true;
        }, 0);
        var i = 1;
        foreach (var (r, primary) in monitors.OrderByDescending(m => m.Primary).ThenBy(m => m.Rect.Left))
        {
            list.Add(new ScreenSource($"Pantalla {i++}{(primary ? " (principal)" : "")} · {r.Right - r.Left}×{r.Bottom - r.Top}",
                                      false, r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top, ""));
        }

        var windows = new List<ScreenSource>();
        EnumWindows((hwnd, _) =>
        {
            if (hwnd == ownWindow || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return true;
            if (DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, out int cloaked, sizeof(int)) == 0 && cloaked != 0) return true;
            var length = GetWindowTextLengthW(hwnd);
            if (length == 0) return true;
            var text = new StringBuilder(length + 1);
            GetWindowTextW(hwnd, text, text.Capacity);
            var title = text.ToString();
            if (title is "Program Manager" or "Blackvolt" || GetWindowRect(hwnd, out var r) == false) return true;
            if (r.Right - r.Left < 160 || r.Bottom - r.Top < 120) return true;
            windows.Add(new ScreenSource($"Ventana · {title}", true, r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top, title));
            return true;
        }, 0);
        list.AddRange(windows.OrderBy(w => w.Label, StringComparer.CurrentCultureIgnoreCase));
        return list;
    }

    private const int DWMWA_CLOAKED = 14;

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO { public int cbSize; public RECT rcMonitor, rcWork; public uint dwFlags; }

    private delegate bool MonitorEnumProc(nint monitor, nint hdc, ref RECT rect, nint data);
    private delegate bool WindowEnumProc(nint hwnd, nint data);

    [DllImport("user32.dll")]
    private static extern bool EnumDisplayMonitors(nint hdc, nint clip, MonitorEnumProc proc, nint data);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern bool GetMonitorInfoW(nint monitor, ref MONITORINFO info);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(WindowEnumProc proc, nint data);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(nint hwnd);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(nint hwnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextLengthW(nint hwnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(nint hwnd, StringBuilder text, int max);

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(nint hwnd, out RECT rect);

    [DllImport("dwmapi.dll")]
    private static extern int DwmGetWindowAttribute(nint hwnd, int attribute, out int value, int size);
}
