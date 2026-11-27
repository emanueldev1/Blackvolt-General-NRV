using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace Blackvolt.App.Views;

public sealed record StageRow(string Label, string Avg, string P95, string P99, string Max);

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
        _timer.Start();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e) => _timer.Stop();

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
