using Blackvolt.App.Interop;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Blackvolt.App.Controls;

/// <summary>One row of extra information under the graph (label, value).</summary>
internal readonly record struct PerfLine(string Label, string Value);

/// <summary>
/// Live performance overlay for the player and Remote Play: FPS with 1 % low, a frame-time graph
/// with the GPU time under it, the GPU cost of every stage as a stacked bar (convert, DLSS NR
/// input / evaluation / composite, RTX VSR, TrueHDR, final pass), and VRAM use. Everything is
/// built once; updates only change text, bar widths and the graph points (no allocations per
/// tick beyond the point collections).
/// </summary>
internal sealed class PerfOverlay : UserControl
{
    private const int GraphSamples = 240;
    private const double GraphWidth = 320, GraphHeight = 74;

    private static readonly (string Label, Color Color)[] Stages =
    [
        ("Conversión", Color.FromArgb(255, 130, 130, 140)),
        ("NR entrada", Color.FromArgb(255, 61, 214, 196)),
        ("DLSS NR", Color.FromArgb(255, 198, 255, 61)),
        ("NR composición", Color.FromArgb(255, 140, 200, 60)),
        ("Interpolación", Color.FromArgb(255, 255, 90, 170)),
        ("Escalado", Color.FromArgb(255, 80, 160, 255)),
        ("TrueHDR", Color.FromArgb(255, 255, 160, 60)),
        ("Presentación", Color.FromArgb(255, 180, 120, 255)),
    ];

    private readonly TextBlock _fps, _fpsSub, _title, _graphMax, _gpuTotal;
    private readonly Polyline _frameLine, _gpuLine;
    private readonly Line _targetLine;
    private readonly Grid _bar;
    private readonly ColumnDefinition[] _barColumns = new ColumnDefinition[Stages.Length];
    private readonly TextBlock[] _stageValues = new TextBlock[Stages.Length];
    private readonly StackPanel[] _stageRows = new StackPanel[Stages.Length];
    private readonly Grid _lines;
    private readonly float[] _history = new float[GraphSamples];
    private readonly float[] _gpuHistory = new float[GraphSamples];
    private readonly float[] _sorted = new float[GraphSamples];

    public PerfOverlay()
    {
        HorizontalAlignment = HorizontalAlignment.Left;
        VerticalAlignment = VerticalAlignment.Top;
        IsHitTestVisible = false;
        var frame = new Border
        {
            Padding = new Thickness(14, 12, 14, 12),
            CornerRadius = new CornerRadius(8),
            Background = new SolidColorBrush(Color.FromArgb(0xC8, 8, 10, 12)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(0x40, 198, 255, 61)),
            BorderThickness = new Thickness(1),
            Width = GraphWidth + 30,
        };

        var mono = new FontFamily("Cascadia Mono, Consolas");
        var dim = new SolidColorBrush(Color.FromArgb(0x99, 255, 255, 255));
        var white = new SolidColorBrush(Color.FromArgb(0xE6, 255, 255, 255));
        var volt = new SolidColorBrush(Color.FromArgb(255, 198, 255, 61));
        var root = new StackPanel { Spacing = 8 };

        // FPS header
        var header = new Grid();
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        header.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _fps = new TextBlock { FontFamily = mono, FontSize = 30, FontWeight = FontWeights.SemiBold, Foreground = volt, Text = "–" };
        var fpsBlock = new StackPanel();
        fpsBlock.Children.Add(_fps);
        fpsBlock.Children.Add(new TextBlock { Text = "FPS", FontSize = 10, Foreground = dim, CharacterSpacing = 120 });
        header.Children.Add(fpsBlock);
        var right = new StackPanel { HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Center };
        _title = new TextBlock { FontFamily = mono, FontSize = 12, Foreground = white, HorizontalAlignment = HorizontalAlignment.Right, TextTrimming = TextTrimming.CharacterEllipsis };
        _fpsSub = new TextBlock { FontFamily = mono, FontSize = 11, Foreground = dim, HorizontalAlignment = HorizontalAlignment.Right };
        right.Children.Add(_title);
        right.Children.Add(_fpsSub);
        Grid.SetColumn(right, 1);
        header.Children.Add(right);
        root.Children.Add(header);

        // Graph
        var graph = new Grid { Width = GraphWidth, Height = GraphHeight, Background = new SolidColorBrush(Color.FromArgb(0x30, 255, 255, 255)), CornerRadius = new CornerRadius(4) };
        var canvas = new Canvas { Width = GraphWidth, Height = GraphHeight };
        _targetLine = new Line { X1 = 0, X2 = GraphWidth, Stroke = new SolidColorBrush(Color.FromArgb(0x55, 255, 255, 255)), StrokeThickness = 1, StrokeDashArray = [3, 3] };
        _gpuLine = new Polyline { Stroke = new SolidColorBrush(Color.FromArgb(0xC0, 80, 160, 255)), StrokeThickness = 1.2 };
        _frameLine = new Polyline { Stroke = volt, StrokeThickness = 1.5 };
        canvas.Children.Add(_targetLine);
        canvas.Children.Add(_gpuLine);
        canvas.Children.Add(_frameLine);
        graph.Children.Add(canvas);
        _graphMax = new TextBlock { FontFamily = mono, FontSize = 10, Foreground = dim, Margin = new Thickness(4, 2, 4, 0) };
        graph.Children.Add(_graphMax);
        var legend = new TextBlock { FontSize = 10, Foreground = dim, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom, Margin = new Thickness(4, 0, 4, 2) };
        legend.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "■ ", Foreground = volt });
        legend.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "fotograma  " });
        legend.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "■ ", Foreground = _gpuLine.Stroke });
        legend.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "GPU" });
        graph.Children.Add(legend);
        root.Children.Add(graph);

        // GPU stacked bar
        var gpuHeader = new Grid();
        gpuHeader.Children.Add(new TextBlock { Text = "GPU POR ETAPA", FontSize = 10, Foreground = dim, CharacterSpacing = 100 });
        _gpuTotal = new TextBlock { FontFamily = mono, FontSize = 11, Foreground = white, HorizontalAlignment = HorizontalAlignment.Right };
        gpuHeader.Children.Add(_gpuTotal);
        root.Children.Add(gpuHeader);
        _bar = new Grid { Height = 8, CornerRadius = new CornerRadius(4), Background = new SolidColorBrush(Color.FromArgb(0x30, 255, 255, 255)) };
        for (int i = 0; i < Stages.Length; i++)
        {
            _barColumns[i] = new ColumnDefinition { Width = new GridLength(0, GridUnitType.Star) };
            _bar.ColumnDefinitions.Add(_barColumns[i]);
            var seg = new Rectangle { Fill = new SolidColorBrush(Stages[i].Color) };
            Grid.SetColumn(seg, i);
            _bar.Children.Add(seg);
        }
        root.Children.Add(_bar);
        var stageGrid = new VariableSizedWrapGrid { Orientation = Orientation.Horizontal, ItemWidth = (GraphWidth) / 2, MaximumRowsOrColumns = 2 };
        for (int i = 0; i < Stages.Length; i++)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
            row.Children.Add(new Ellipse { Width = 7, Height = 7, Fill = new SolidColorBrush(Stages[i].Color), VerticalAlignment = VerticalAlignment.Center });
            row.Children.Add(new TextBlock { Text = Stages[i].Label, FontSize = 11, Foreground = dim, Width = 98 });
            _stageValues[i] = new TextBlock { FontFamily = mono, FontSize = 11, Foreground = white };
            row.Children.Add(_stageValues[i]);
            _stageRows[i] = row;
            stageGrid.Children.Add(row);
        }
        root.Children.Add(stageGrid);

        // Extra lines
        _lines = new Grid { ColumnSpacing = 10, RowSpacing = 1 };
        _lines.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _lines.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        root.Children.Add(_lines);

        frame.Child = root;
        Content = frame;
    }

    /// <param name="frameStage">Telemetry stage graphed in volt: frame intervals ("frame") or latency.</param>
    /// <param name="targetMs">Dashed reference line, e.g. the source frame interval.</param>
    public void Update(EngineSession? engine, string title, string frameStage, bool stageIsInterval, double targetMs,
                       in NativeEnhance.Status gpu, IReadOnlyList<PerfLine> lines)
    {
        _title.Text = title;
        int n = 0, ng = 0;
        if (engine is not null)
        {
            try
            {
                n = engine.GetHistory(frameStage, _history);
                ng = engine.GetHistory("gpu.frame", _gpuHistory);
            }
            catch (EngineException) { }
        }

        // FPS and 1 % low from the frame intervals actually measured.
        if (n > 1)
        {
            double sum = 0;
            for (int i = 0; i < n; i++) sum += _history[i];
            var avgUs = sum / n;
            Array.Copy(_history, _sorted, n);
            Array.Sort(_sorted, 0, n);
            var p99 = _sorted[Math.Min(n - 1, (int)Math.Ceiling(n * 0.99) - 1)];
            if (stageIsInterval && avgUs > 0)
            {
                _fps.Text = (1_000_000.0 / avgUs).ToString("0");
                _fpsSub.Text = $"1% bajo {1_000_000.0 / Math.Max(1, p99):0} · p99 {p99 / 1000:0.0} ms";
            }
            else
            {
                _fps.Text = targetMs > 0 ? (1000.0 / targetMs).ToString("0") : "–";
                _fpsSub.Text = $"media {avgUs / 1000:0.0} ms · p99 {p99 / 1000:0.0} ms";
            }
        }
        else
        {
            _fps.Text = "–";
            _fpsSub.Text = "esperando datos";
        }

        // Graph scale: the larger of 2× the target and the window maximum, rounded up.
        double maxMs = targetMs * 2;
        for (int i = 0; i < n; i++) maxMs = Math.Max(maxMs, _history[i] / 1000.0);
        for (int i = 0; i < ng; i++) maxMs = Math.Max(maxMs, _gpuHistory[i] / 1000.0);
        maxMs = Math.Max(4, Math.Ceiling(maxMs / 4) * 4);
        _graphMax.Text = $"{maxMs:0} ms";
        Plot(_frameLine, _history, n, maxMs);
        Plot(_gpuLine, _gpuHistory, ng, maxMs);
        var ty = targetMs > 0 ? GraphHeight - targetMs / maxMs * GraphHeight : -10;
        _targetLine.Y1 = _targetLine.Y2 = ty;
        _targetLine.Visibility = targetMs > 0 ? Visibility.Visible : Visibility.Collapsed;

        // Stage breakdown.
        Span<float> ms = [gpu.GpuConvertMs, gpu.GpuNrPrepareMs, gpu.GpuNeuralMs, gpu.GpuNrCompositeMs, gpu.GpuInterpMs,
                          gpu.GpuVsrMs, gpu.GpuHdrMs, gpu.GpuPresentMs];
        var nrOn = gpu.Flags.HasFlag(NativeEnhance.Flags.NeuralActive);
        bool[] on =
        [
            true, nrOn && gpu.GpuNrPrepareMs > 0.005, nrOn, nrOn, gpu.Flags.HasFlag(NativeEnhance.Flags.InterpolationActive),
            gpu.Flags.HasFlag(NativeEnhance.Flags.VsrActive) || gpu.Flags.HasFlag(NativeEnhance.Flags.NisActive),
            gpu.Flags.HasFlag(NativeEnhance.Flags.HdrActive), true,
        ];
        for (int i = 0; i < Stages.Length; i++)
        {
            var v = on[i] ? Math.Max(0, ms[i]) : 0;
            _barColumns[i].Width = new GridLength(v, GridUnitType.Star);
            _stageValues[i].Text = $"{v:0.00} ms";
            _stageRows[i].Visibility = on[i] ? Visibility.Visible : Visibility.Collapsed;
        }
        _gpuTotal.Text = gpu.GpuTotalMs > 0 ? $"{gpu.GpuTotalMs:0.00} ms" : "–";

        // Extra info, plus VRAM.
        var all = new List<PerfLine>(lines);
        if (gpu.Flags.HasFlag(NativeEnhance.Flags.HdrSource))
            all.Add(new("HDR", gpu.Flags.HasFlag(NativeEnhance.Flags.HdrPassthrough) ? "HDR → pantalla HDR" : "HDR → SDR (tone mapping)"));
        if (nrOn) all.Add(new("DLSS NR", $"{gpu.NrLabelText}"));
        if (nrOn) all.Add(new("NR interno", $"{gpu.NrWidth}×{gpu.NrHeight}"));
        if (engine is not null)
        {
            try
            {
                var mem = engine.GetGpuMemory();
                if (mem.BudgetBytes > 0)
                    all.Add(new("VRAM", $"{mem.UsageBytes / 1048576.0:0} / {mem.BudgetBytes / 1048576.0:0} MB ({mem.UsageBytes * 100.0 / mem.BudgetBytes:0}%)"));
            }
            catch (EngineException) { }
        }
        SetLines(all);
    }

    private static void Plot(Polyline line, float[] data, int n, double maxMs)
    {
        var points = line.Points;
        if (n < 2)
        {
            points.Clear();
            return;
        }
        // Reuse the collection: replace in place, trim or grow as needed.
        var step = GraphWidth / (GraphSamples - 1);
        var x0 = GraphWidth - (n - 1) * step;
        for (int i = 0; i < n; i++)
        {
            var y = GraphHeight - Math.Min(1.0, data[i] / 1000.0 / maxMs) * GraphHeight;
            var p = new Point(x0 + i * step, y);
            if (i < points.Count) points[i] = p;
            else points.Add(p);
        }
        while (points.Count > n) points.RemoveAt(points.Count - 1);
    }

    private void SetLines(List<PerfLine> lines)
    {
        // Grow the grid only when needed; reuse text blocks.
        while (_lines.RowDefinitions.Count < lines.Count)
        {
            var row = _lines.RowDefinitions.Count;
            _lines.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var l = new TextBlock { FontSize = 11, Foreground = new SolidColorBrush(Color.FromArgb(0x99, 255, 255, 255)) };
            var v = new TextBlock { FontSize = 11, FontFamily = new FontFamily("Cascadia Mono, Consolas"), Foreground = new SolidColorBrush(Colors.White), TextTrimming = TextTrimming.CharacterEllipsis };
            Grid.SetRow(l, row);
            Grid.SetRow(v, row);
            Grid.SetColumn(v, 1);
            _lines.Children.Add(l);
            _lines.Children.Add(v);
        }
        for (int r = 0; r < _lines.RowDefinitions.Count; r++)
        {
            var label = (TextBlock)_lines.Children[r * 2];
            var value = (TextBlock)_lines.Children[r * 2 + 1];
            var visible = r < lines.Count;
            label.Visibility = value.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
            if (!visible) continue;
            label.Text = lines[r].Label;
            value.Text = lines[r].Value;
        }
    }
}
