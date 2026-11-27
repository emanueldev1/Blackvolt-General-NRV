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
        ("enhance", "Mejora (GPU)"),
        ("present", "Presentación"),
    ];

    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(500) };

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
    }

    private static string Ms(double microseconds) => $"{microseconds / 1000.0:0.00} ms";
}
