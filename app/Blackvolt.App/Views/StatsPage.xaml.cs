using Blackvolt.App.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace Blackvolt.App.Views;

public sealed record StageRow(string Label, string Avg, string P95, string P99, string Max);

public sealed record HistoryRow(string Kind, string When, string Title, string Detail, string Duration,
                                string Resolution, string Fps, string TailLabel, string Tail);

public sealed partial class StatsPage : Page
{
    private static readonly (string Key, string Label)[] Stages =
    [
        ("frame", "Fotograma total"),
        ("decode", "Decodificación"),
        ("present", "Presentación"),
        ("stream.latency", "Latencia Remote Play"),
        ("stream.decode", "Decodificación stream"),
        ("gpu.frame", "GPU por fotograma"),
        ("gpu.convert", "GPU · conversión"),
        ("gpu.nr", "GPU · DLSS NR"),
        ("gpu.nr.composite", "GPU · NR composición"),
        ("gpu.vsr", "GPU · RTX VSR"),
        ("gpu.truehdr", "GPU · TrueHDR"),
    ];

    private readonly float[] _frames = new float[480];
    private readonly float[] _gpu = new float[480];

    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(250) };

    public StatsPage()
    {
        InitializeComponent();
        _timer.Tick += (_, _) => Refresh();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        Refresh();
        RefreshHistory();
        SessionHistory.Changed += OnHistoryChanged;
        _timer.Start();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        _timer.Stop();
        SessionHistory.Changed -= OnHistoryChanged;
    }

    private void OnHistoryChanged() => DispatcherQueue.TryEnqueue(RefreshHistory);

    private void OnClearHistory(object sender, RoutedEventArgs e) => SessionHistory.Clear();

    private void RefreshHistory()
    {
        var entries = SessionHistory.Entries;
        var viewed = entries.Where(r => r.Kind != "Exportar").ToList();
        SumSessions.Text = entries.Count.ToString();
        SumTime.Text = Clock(entries.Sum(r => r.DurationS));
        var seconds = viewed.Sum(r => r.DurationS);
        SumFps.Text = seconds > 0 ? (viewed.Sum(r => r.AvgFps * r.DurationS) / seconds).ToString("0.0") : "–";
        var presented = viewed.Sum(r => (double)r.Presented);
        var dropped = viewed.Sum(r => (double)r.Dropped);
        SumDropped.Text = presented + dropped > 0 ? $"{dropped / (presented + dropped) * 100:0.00}%" : "–";

        HistoryList.ItemsSource = entries.Take(100).Select(r => new HistoryRow(
            r.Kind,
            r.StartedUtc.ToLocalTime().ToString("d MMM, HH:mm"),
            r.Title,
            r.Detail,
            Clock(r.DurationS),
            r.Height > 0 ? $"{r.Width}×{r.Height}" : "–",
            r.AvgFps > 0 ? r.AvgFps.ToString("0.0") : "–",
            r.Kind == "Exportar" ? "FOTOGRAMAS" : r.Kind == "Remote Play" ? "LATENCIA P99" : "FOTOGRAMA P99",
            r.Kind == "Exportar" ? r.Presented.ToString() : r.FrameP99Ms > 0 ? $"{r.FrameP99Ms:0.0} ms" : "–")).ToList();
        HistoryEmpty.Visibility = entries.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        SummaryGrid.Visibility = entries.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        ClearButton.IsEnabled = entries.Count > 0;
    }

    private static string Clock(double seconds)
    {
        var t = TimeSpan.FromSeconds(Math.Max(0, seconds));
        return t.TotalHours >= 1 ? $"{(int)t.TotalHours}h {t.Minutes:00}m" : t.ToString(@"m\:ss");
    }

    private void Refresh()
    {
        var engine = App.Engine;
        if (engine is null) return;

        var rows = new List<StageRow>();
        foreach (var (key, label) in Stages)
        {
            var s = engine.GetStats(key);
            if (s.Samples == 0) continue;
            rows.Add(new StageRow(label, Ms(s.AvgUs), Ms(s.P95Us), Ms(s.P99Us), Ms(s.MaxUs)));
        }
        StageList.ItemsSource = rows;
        EmptyHint.Visibility = rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        RefreshLive(engine);
    }

    private void RefreshLive(Interop.EngineSession engine)
    {
        int n = 0, ng = 0;
        try
        {
            n = engine.GetHistory("frame", _frames);
            if (n == 0) n = engine.GetHistory("stream.latency", _frames);
            ng = engine.GetHistory("gpu.frame", _gpu);
            var mem = engine.GetGpuMemory();
            if (mem.BudgetBytes > 0)
            {
                LiveVram.Text = $"{mem.UsageBytes / 1048576.0:0} MB";
                VramBar.Value = Math.Min(1, (double)mem.UsageBytes / mem.BudgetBytes);
                ToolTipService.SetToolTip(VramBar, $"Presupuesto {mem.BudgetBytes / 1048576.0:0} MB · tarjeta {mem.DedicatedBytes / 1048576.0:0} MB");
            }
        }
        catch (Interop.EngineException)
        {
            return;
        }

        if (n > 1)
        {
            var sorted = _frames.Take(n).OrderBy(x => x).ToArray();
            var avg = sorted.Average();
            var p99 = sorted[Math.Min(n - 1, (int)Math.Ceiling(n * 0.99) - 1)];
            LiveFps.Text = avg > 0 ? $"{1_000_000 / avg:0} · {1_000_000 / Math.Max(1, p99):0}" : "–";
        }
        else
        {
            LiveFps.Text = "–";
        }

        var width = Math.Max(10, GraphHost.ActualWidth);
        var height = GraphHost.ActualHeight > 0 ? GraphHost.ActualHeight : 140;
        double max = 4;
        for (int i = 0; i < n; i++) max = Math.Max(max, _frames[i] / 1000.0);
        for (int i = 0; i < ng; i++) max = Math.Max(max, _gpu[i] / 1000.0);
        max = Math.Ceiling(max / 4) * 4;
        GraphScale.Text = $"0 – {max:0} ms";
        Plot(FrameLine, _frames, n, max, width, height);
        Plot(GpuLine, _gpu, ng, max, width, height);
    }

    private static void Plot(Microsoft.UI.Xaml.Shapes.Polyline line, float[] data, int n, double maxMs, double width, double height)
    {
        var points = new Microsoft.UI.Xaml.Media.PointCollection();
        var step = width / (data.Length - 1);
        var x0 = width - (n - 1) * step;
        for (int i = 0; i < n; i++)
            points.Add(new Windows.Foundation.Point(x0 + i * step, height - Math.Min(1, data[i] / 1000.0 / maxMs) * height));
        line.Points = points;
    }

    private static string Ms(double microseconds) => $"{microseconds / 1000.0:0.00} ms";
}
