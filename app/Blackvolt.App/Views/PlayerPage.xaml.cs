using Blackvolt.App.Controls;
using Blackvolt.App.Services;
using Blackvolt.App.Interop;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Navigation;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;
using State = Blackvolt.App.Interop.NativeEngine.PlayerState;

namespace Blackvolt.App.Views;

public sealed partial class PlayerPage : Page
{
    private static readonly string[] VideoExtensions =
        [".mp4", ".mkv", ".mov", ".avi", ".webm", ".m4v", ".ts", ".m2ts", ".wmv", ".flv"];

    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(100) };
    private PlayerSession? _player;
    private readonly EnhancePanel _enhance = new();
    private readonly PerfOverlay _perf = new();
    private bool _scrubbing;
    private bool _updatingTimeline;
    private SessionTracker? _session;
    private DateTime _lastPointerMove = DateTime.UtcNow;

    public PlayerPage()
    {
        InitializeComponent();
        _timer.Tick += (_, _) => Tick();
        _enhance.Changed += () => _player?.SetEnhance(AppSettings.Current.Video);
        EnhanceButton.Flyout = new Flyout { Content = _enhance };
        StatsPanel.Child = _perf;
        var speeds = new MenuFlyout();
        foreach (var r in Speeds)
        {
            var item = new MenuFlyoutItem { Text = $"{r:0.##}×" };
            item.Click += (_, _) => SetSpeed(r);
            speeds.Items.Add(item);
        }
        SpeedButton.Flyout = speeds;
        var captions = new MenuFlyout();
        captions.Opening += (_, _) => BuildCaptionsMenu(captions);
        CaptionsButton.Flyout = captions;
        ApplySubtitleStyle();
        StatsPanel.Visibility = AppSettings.Current.Video.ShowStats ? Visibility.Visible : Visibility.Collapsed;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        if (App.Engine is null)
        {
            ShowError(App.EngineError ?? "El motor no está disponible.");
            return;
        }
        try
        {
            _player ??= PlayerSession.Create(App.Engine, Video);
            _player.SetEnhance(AppSettings.Current.Video);
            ResizeSwapChain();
            _timer.Start();
            if (e.Parameter is OpenMedia media) OpenUrl(media.Url, media.Title);
            else if (_resumeOnce && AppSettings.Current.Capture is { ResumeLastSource: true, LastUrl.Length: > 0 } last)
            {
                // "Resume last source at startup": the file at its position, or the capture device.
                _resumeOnce = false;
                if (last.LastUrl.Contains(':') && (File.Exists(last.LastUrl) || last.LastUrl.StartsWith("capture:", StringComparison.Ordinal) ||
                                                   last.LastUrl.StartsWith("screen:", StringComparison.Ordinal)))
                {
                    OpenUrl(last.LastUrl, last.LastTitle);
                    if (File.Exists(last.LastUrl) && last.LastPosition > 1) _player.Seek(last.LastPosition);
                }
            }
        }
        catch (EngineException ex)
        {
            ShowError(ex.Message);
        }
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        SetFullscreen(false);
        RememberPosition();
        _timer.Stop();
        _session?.Finish();
        _session = null;
        _player?.Dispose();
        _player = null;
    }

    private void OpenUrl(string url, string title)
    {
        if (_player is null) return;
        ErrorBar.IsOpen = false;
        try
        {
            _session?.Finish();
            _player.Open(url);
            _player.Play();
            _session = new SessionTracker(url.StartsWith("capture:", StringComparison.Ordinal) ? "Captura" : "Reproductor", title);
        }
        catch (EngineException ex)
        {
            ShowError(ex.Message);
            return;
        }
        SourceText.Text = title;
        EmptyState.Visibility = Visibility.Collapsed;
        Transport.Visibility = Visibility.Visible;
        Remember(url, title);
        SetSpeed(1.0);
        PrepareSubtitles(url);
    }

    // ---- subtitles and tracks --------------------------------------------------------------

    private IReadOnlyList<SubtitleCue>? _externalCues;
    private string _externalName = "";
    private bool _embeddedChosen;

    private void PrepareSubtitles(string url)
    {
        _externalCues = null;
        _externalName = "";
        _embeddedChosen = false;
        SubtitleBox.Visibility = Visibility.Collapsed;
        if (AppSettings.Current.Subtitles.AutoLoad && File.Exists(url) && SubtitleFile.FindNextTo(url) is { } file)
            LoadSubtitleFile(file);
    }

    private void LoadSubtitleFile(string path)
    {
        try
        {
            _externalCues = SubtitleFile.Load(path);
            _externalName = Path.GetFileName(path);
            _player?.SelectTrack(1, -1);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            _externalCues = null;
        }
    }

    private void UpdateSubtitle(NativeEngine.PlayerInfo info)
    {
        if (_player is null) return;
        // First embedded text track, once the tracks are known and no file was loaded.
        if (!_embeddedChosen && info.State == State.Playing)
        {
            _embeddedChosen = true;
            if (_externalCues is null && AppSettings.Current.Subtitles.AutoEmbedded &&
                _player.Tracks().FirstOrDefault(t => t.Kind == 1) is { Size: > 0 } first)
                _player.SelectTrack(1, first.Index);
        }
        var text = _externalCues is not null ? SubtitleFile.At(_externalCues, info.PositionS) : _player.Subtitle();
        SubtitleText.Text = text;
        SubtitleBox.Visibility = text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
        // Stay clear of the transport bar when it is showing.
        if (!AppSettings.Current.Subtitles.Top)
            SubtitleBox.Margin = new Thickness(40, 40, 40, Transport.Visibility == Visibility.Visible ? 110 : 48);
    }

    private void ApplySubtitleStyle()
    {
        var s = AppSettings.Current.Subtitles;
        SubtitleText.FontSize = Math.Clamp(s.Size, 14, 80);
        SubtitleBox.Background = new Microsoft.UI.Xaml.Media.SolidColorBrush(
            s.Background ? Windows.UI.Color.FromArgb(0xA6, 0, 0, 0) : Windows.UI.Color.FromArgb(0, 0, 0, 0));
        SubtitleBox.VerticalAlignment = s.Top ? VerticalAlignment.Top : VerticalAlignment.Bottom;
        SubtitleBox.Margin = new Thickness(40, s.Top ? 40 : 40, 40, 48);
    }

    private static string TrackLabel(NativeEngine.Track t, int n)
    {
        var parts = new List<string>();
        if (t.TitleText.Length > 0) parts.Add(t.TitleText);
        if (t.LanguageText.Length > 0) parts.Add(t.LanguageText.ToUpperInvariant());
        if (parts.Count == 0) parts.Add($"Pista {n}");
        return $"{string.Join(" · ", parts)} ({t.CodecText})";
    }

    private void BuildCaptionsMenu(MenuFlyout menu)
    {
        menu.Items.Clear();
        if (_player is null) return;
        var info = _player.Info;
        var tracks = _player.Tracks();

        menu.Items.Add(new MenuFlyoutItem { Text = "SUBTÍTULOS", IsEnabled = false });
        var off = new ToggleMenuFlyoutItem { Text = "Desactivados", IsChecked = _externalCues is null && info.SubtitleTrack < 0 };
        off.Click += (_, _) => { _externalCues = null; _player?.SelectTrack(1, -1); };
        menu.Items.Add(off);
        var n = 1;
        foreach (var t in tracks.Where(t => t.Kind == 1))
        {
            var index = t.Index;
            var item = new ToggleMenuFlyoutItem { Text = TrackLabel(t, n++), IsChecked = _externalCues is null && info.SubtitleTrack == index };
            item.Click += (_, _) => { _externalCues = null; _player?.SelectTrack(1, index); };
            menu.Items.Add(item);
        }
        if (_externalCues is not null)
            menu.Items.Add(new ToggleMenuFlyoutItem { Text = $"Archivo · {_externalName}", IsChecked = true });
        var load = new MenuFlyoutItem { Text = "Cargar archivo de subtítulos…", Icon = new FontIcon { Glyph = "\uE8E5" } };
        load.Click += async (_, _) => await PickSubtitleAsync();
        menu.Items.Add(load);

        var audio = tracks.Where(t => t.Kind == 0).ToList();
        if (audio.Count > 1)
        {
            menu.Items.Add(new MenuFlyoutSeparator());
            menu.Items.Add(new MenuFlyoutItem { Text = "AUDIO", IsEnabled = false });
            n = 1;
            foreach (var t in audio)
            {
                var index = t.Index;
                var item = new ToggleMenuFlyoutItem { Text = TrackLabel(t, n++), IsChecked = info.AudioTrack == index };
                item.Click += (_, _) => _player?.SelectTrack(0, index);
                menu.Items.Add(item);
            }
        }

        menu.Items.Add(new MenuFlyoutSeparator());
        var style = new MenuFlyoutSubItem { Text = "Estilo" };
        var s = AppSettings.Current.Subtitles;
        foreach (var (label, size) in new[] { ("Pequeño", 22.0), ("Normal", 30.0), ("Grande", 40.0), ("Muy grande", 54.0) })
        {
            var item = new ToggleMenuFlyoutItem { Text = label, IsChecked = Math.Abs(s.Size - size) < 0.5 };
            item.Click += (_, _) => { s.Size = size; SaveSubtitleStyle(); };
            style.Items.Add(item);
        }
        style.Items.Add(new MenuFlyoutSeparator());
        var bg = new ToggleMenuFlyoutItem { Text = "Fondo oscuro", IsChecked = s.Background };
        bg.Click += (_, _) => { s.Background = !s.Background; SaveSubtitleStyle(); };
        style.Items.Add(bg);
        var top = new ToggleMenuFlyoutItem { Text = "Arriba", IsChecked = s.Top };
        top.Click += (_, _) => { s.Top = !s.Top; SaveSubtitleStyle(); };
        style.Items.Add(top);
        menu.Items.Add(style);
    }

    private void SaveSubtitleStyle()
    {
        AppSettings.Current.Save();
        ApplySubtitleStyle();
    }

    private async Task PickSubtitleAsync()
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.VideosLibrary };
        foreach (var ext in SubtitleFile.Extensions) picker.FileTypeFilter.Add(ext);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow!));
        var file = await picker.PickSingleFileAsync();
        if (file is not null) LoadSubtitleFile(file.Path);
    }

    private void OnCycleSubtitles(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e)
    {
        e.Handled = true;
        if (_player is null) return;
        // Off -> each embedded track -> off.
        var subs = _player.Tracks().Where(t => t.Kind == 1).Select(t => t.Index).ToList();
        _externalCues = null;
        var current = _player.Info.SubtitleTrack;
        var next = current < 0 ? (subs.Count > 0 ? subs[0] : -1) : subs.IndexOf(current) is var i && i >= 0 && i + 1 < subs.Count ? subs[i + 1] : -1;
        _player.SelectTrack(1, next);
    }

    // ---- speed, fullscreen, resume ---------------------------------------------------------

    private static readonly double[] Speeds = [0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0, 4.0];
    private static bool _resumeOnce = true; // only the first visit after launch
    private double _speed = 1.0;

    private void SetSpeed(double rate)
    {
        _speed = rate;
        _player?.SetRate(rate);
        SpeedText.Text = $"{rate:0.##}×";
    }

    private void StepSpeed(int direction)
    {
        var i = Array.FindIndex(Speeds, s => Math.Abs(s - _speed) < 1e-6);
        if (i < 0) i = Array.IndexOf(Speeds, 1.0);
        SetSpeed(Speeds[Math.Clamp(i + direction, 0, Speeds.Length - 1)]);
    }

    private void OnFaster(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; StepSpeed(+1); }
    private void OnSlower(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; StepSpeed(-1); }

    private static bool IsFullscreen =>
        App.MainWindow?.AppWindow.Presenter.Kind == Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen;

    private void SetFullscreen(bool on)
    {
        var window = App.MainWindow;
        if (window is null || IsFullscreen == on) return;
        window.AppWindow.SetPresenter(on ? Microsoft.UI.Windowing.AppWindowPresenterKind.FullScreen
                                         : Microsoft.UI.Windowing.AppWindowPresenterKind.Default);
        window.SetImmersive(on);
        FullscreenIcon.Glyph = on ? "\uE73F" : "\uE740";
    }

    private void OnFullscreenAccel(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; SetFullscreen(!IsFullscreen); }
    private void OnEscapeAccel(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { if (IsFullscreen) { e.Handled = true; SetFullscreen(false); } }
    private void OnFullscreenClick(object sender, RoutedEventArgs e) => SetFullscreen(!IsFullscreen);
    private void OnDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if (EmptyState.Visibility == Visibility.Collapsed) SetFullscreen(!IsFullscreen);
    }

    private static void Remember(string url, string title)
    {
        var c = AppSettings.Current.Capture;
        c.LastUrl = url;
        c.LastTitle = title;
        c.LastPosition = 0;
        AppSettings.Current.Save();
    }

    private void RememberPosition()
    {
        if (_player is null) return;
        var info = _player.Info;
        if (info.Live == 0 && info.DurationS > 0)
        {
            AppSettings.Current.Capture.LastPosition = info.PositionS < info.DurationS - 5 ? info.PositionS : 0;
            AppSettings.Current.Save();
        }
    }

    private async Task OpenFileAsync(string path)
    {
        if (_player is null) return;
        ErrorBar.IsOpen = false;
        _session?.Finish();
        _player.Open(path);
        _session = new SessionTracker("Reproductor", Path.GetFileName(path));
        SourceText.Text = Path.GetFileName(path);
        EmptyState.Visibility = Visibility.Collapsed;
        Transport.Visibility = Visibility.Visible;
        Remember(path, Path.GetFileName(path));
        SetSpeed(1.0);
        PrepareSubtitles(path);
        await Task.CompletedTask;
    }

    private void Tick()
    {
        if (_player is null) return;
        var info = _player.Info;

        if (info.State == State.Error)
        {
            ShowError(info.ErrorText);
        }

        PlayIcon.Glyph = info.State == State.Playing ? "" : "";
        bool live = info.Live != 0;
        Timeline.Visibility = live ? Visibility.Collapsed : Visibility.Visible;
        if (live)
        {
            TimeText.Text = "EN DIRECTO";
        }
        else if (!_scrubbing && info.DurationS > 0)
        {
            _updatingTimeline = true;
            Timeline.Maximum = info.DurationS;
            Timeline.Value = Math.Min(info.PositionS, info.DurationS);
            _updatingTimeline = false;
        }
        if (!live) TimeText.Text = $"{Format(info.PositionS)} / {Format(info.DurationS)}";

        // Hide the transport after 3 s without mouse movement while playing.
        if (info.State == State.Playing && Transport.Visibility == Visibility.Visible &&
            (DateTime.UtcNow - _lastPointerMove).TotalSeconds > 3 && !_scrubbing)
        {
            Transport.Visibility = Visibility.Collapsed;
        }

        UpdateSubtitle(info);

        var enhance = _player.EnhanceStatus;
        _enhance.ShowStatus(enhance);
        if (info.State == State.Playing && _session is not null)
        {
            var p99 = App.Engine?.GetStats("frame") is { Samples: > 0 } fs ? fs.P99Us / 1000.0 : 0;
            _session.Sample(info.Width, info.Height, info.FramesPresented, info.FramesDropped, p99,
                            SessionDetail(info, enhance));
        }
        if (StatsPanel.Visibility == Visibility.Visible) UpdateStats(info, enhance);
    }

    private static string SessionDetail(in NativeEngine.PlayerInfo info, in NativeEnhance.Status enhance)
    {
        var parts = new List<string> { info.CodecText, info.HardwareDecode != 0 ? "GPU" : "CPU" };
        if (enhance.Flags.HasFlag(NativeEnhance.Flags.NeuralActive)) parts.Add("DLSS NR");
        if (enhance.Flags.HasFlag(NativeEnhance.Flags.VsrActive)) parts.Add($"VSR → {enhance.OutputHeight}p");
        if (enhance.Flags.HasFlag(NativeEnhance.Flags.HdrActive)) parts.Add("TrueHDR");
        return string.Join(" · ", parts.Where(x => x.Length > 0));
    }

    private void UpdateStats(NativeEngine.PlayerInfo info, in NativeEnhance.Status enhance)
    {
        var engine = App.Engine;
        var decode = engine?.GetStats("decode");
        var present = engine?.GetStats("present");
        var output = enhance.OutputWidth > 0 ? $" → {enhance.OutputWidth}×{enhance.OutputHeight}" : "";
        var lines = new List<PerfLine>
        {
            new("Fuente", $"{info.Fps:0.00} fps · {(info.HardwareDecode != 0 ? "GPU (D3D12)" : "CPU")}"),
            new("Decodificación", $"{Ms(decode?.AvgUs)} ms · p99 {Ms(decode?.P99Us)} ms"),
            new("Presentación", $"{Ms(present?.AvgUs)} ms"),
            new("Fotogramas", $"{info.FramesPresented} mostrados · {info.FramesDropped} perdidos" +
                               (info.FramesGenerated > 0 ? $" · {info.FramesGenerated} generados" : "")),
        };
        _perf.Update(engine, $"{info.Width}×{info.Height}{output} {info.CodecText}", "frame", stageIsInterval: true,
                     info.Fps > 0 ? 1000.0 / info.Fps : 0, enhance, lines);
    }

    private static string Ms(double? us) => us is > 0 ? (us.Value / 1000.0).ToString("0.00") : "  –";

    private static string Format(double seconds)
    {
        if (double.IsNaN(seconds) || seconds < 0) seconds = 0;
        var t = TimeSpan.FromSeconds(seconds);
        return t.TotalHours >= 1 ? t.ToString(@"h\:mm\:ss") : t.ToString(@"m\:ss");
    }

    private void ShowError(string message)
    {
        ErrorBar.Title = "No se pudo reproducir";
        ErrorBar.Message = message;
        ErrorBar.IsOpen = true;
    }

    // ---- input ---------------------------------------------------------------------------

    private void TogglePlay()
    {
        if (_player is null) return;
        var info = _player.Info;
        if (info.State == State.Playing) _player.Pause();
        else if (info.State == State.Ended) { _player.Seek(0); _player.Play(); }
        else _player.Play();
    }

    private void SeekBy(double delta)
    {
        if (_player is null) return;
        var info = _player.Info;
        if (info.Live != 0) return;
        _player.Seek(Math.Clamp(info.PositionS + delta, 0, Math.Max(0, info.DurationS - 0.1)));
    }

    private void OnPlayClick(object sender, RoutedEventArgs e) => TogglePlay();
    private void OnTogglePlay(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { TogglePlay(); e.Handled = true; }
    private void OnSeekBack(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { SeekBy(-5); e.Handled = true; }
    private void OnSeekForward(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { SeekBy(5); e.Handled = true; }
    private void OnToggleStats(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { ToggleStats(); e.Handled = true; }
    private void OnToggleStatsClick(object sender, RoutedEventArgs e) => ToggleStats();
    private async void OnScreenshotAccel(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; await ScreenshotAsync(); }
    private async void OnScreenshot(object sender, RoutedEventArgs e) => await ScreenshotAsync();

    private void OnCompareAccel(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e)
    {
        e.Handled = true;
        AppSettings.Current.Video.Compare = !AppSettings.Current.Video.Compare;
        AppSettings.Current.Save();
        _player?.SetEnhance(AppSettings.Current.Video);
    }

    private async Task ScreenshotAsync()
    {
        if (_player is null) return;
        var path = NativeEnhance.NewScreenshotPath();
        var handle = _player.Handle;
        var r = await Task.Run(() => NativeEnhance.PlayerScreenshot(handle, path));
        ShotBar.Severity = r == NativeEngine.Result.Ok ? InfoBarSeverity.Success : InfoBarSeverity.Warning;
        ShotBar.Title = r == NativeEngine.Result.Ok ? "Captura guardada" : "No se pudo guardar la captura";
        ShotBar.Message = r == NativeEngine.Result.Ok ? path : "Reproduce un video primero.";
        ShotBar.IsOpen = true;
    }
    private async void OnOpenAccel(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; await PickAsync(); }
    private async void OnOpen(object sender, RoutedEventArgs e) => await PickAsync();

    private void ToggleStats()
    {
        var show = StatsPanel.Visibility != Visibility.Visible;
        StatsPanel.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        AppSettings.Current.Video.ShowStats = show;
        AppSettings.Current.Save();
    }

    private async Task PickAsync()
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.VideosLibrary };
        foreach (var ext in VideoExtensions) picker.FileTypeFilter.Add(ext);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow!));
        var file = await picker.PickSingleFileAsync();
        if (file is not null) await OpenFileAsync(file.Path);
    }

    private void OnTimelineChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        if (_updatingTimeline || _player is null) return;
        _scrubbing = true;
        _player.Seek(e.NewValue); // live scrubbing: seeks are cheap and cancel each other
    }

    private void OnTimelineReleased(object sender, PointerRoutedEventArgs e) => _scrubbing = false;

    private void OnVolumeChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e) =>
        _player?.SetVolume((float)(e.NewValue / 100.0));

    private void OnPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        _lastPointerMove = DateTime.UtcNow;
        if (EmptyState.Visibility == Visibility.Collapsed) Transport.Visibility = Visibility.Visible;
    }

    private void OnDragOver(object sender, DragEventArgs e)
    {
        if (e.DataView.Contains(StandardDataFormats.StorageItems))
        {
            e.AcceptedOperation = DataPackageOperation.Link;
            e.DragUIOverride.Caption = "Reproducir con Blackvolt";
        }
    }

    private async void OnDrop(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        var items = await e.DataView.GetStorageItemsAsync();
        if (items.FirstOrDefault() is StorageFile file) await OpenFileAsync(file.Path);
    }

    // ---- sizing --------------------------------------------------------------------------

    private void OnVideoSizeChanged(object sender, SizeChangedEventArgs e) => ResizeSwapChain();
    private void OnCompositionScaleChanged(SwapChainPanel sender, object args) => ResizeSwapChain();

    private void ResizeSwapChain()
    {
        if (_player is null) return;
        var w = (uint)Math.Max(1, Video.ActualWidth * Video.CompositionScaleX);
        var h = (uint)Math.Max(1, Video.ActualHeight * Video.CompositionScaleY);
        _player.Resize(w, h, Video.CompositionScaleX, Video.CompositionScaleY);
    }
}
