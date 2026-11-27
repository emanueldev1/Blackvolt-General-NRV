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
    private bool _scrubbing;
    private bool _updatingTimeline;
    private DateTime _lastPointerMove = DateTime.UtcNow;

    public PlayerPage()
    {
        InitializeComponent();
        _timer.Tick += (_, _) => Tick();
        _enhance.Changed += () => _player?.SetEnhance(AppSettings.Current.Video);
        EnhanceButton.Flyout = new Flyout { Content = _enhance };
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
        }
        catch (EngineException ex)
        {
            ShowError(ex.Message);
        }
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        _timer.Stop();
        _player?.Dispose();
        _player = null;
    }

    private async Task OpenFileAsync(string path)
    {
        if (_player is null) return;
        ErrorBar.IsOpen = false;
        _player.Open(path);
        SourceText.Text = Path.GetFileName(path);
        EmptyState.Visibility = Visibility.Collapsed;
        Transport.Visibility = Visibility.Visible;
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
        if (!_scrubbing && info.DurationS > 0)
        {
            _updatingTimeline = true;
            Timeline.Maximum = info.DurationS;
            Timeline.Value = Math.Min(info.PositionS, info.DurationS);
            _updatingTimeline = false;
        }
        TimeText.Text = $"{Format(info.PositionS)} / {Format(info.DurationS)}";

        // Hide the transport after 3 s without mouse movement while playing.
        if (info.State == State.Playing && Transport.Visibility == Visibility.Visible &&
            (DateTime.UtcNow - _lastPointerMove).TotalSeconds > 3 && !_scrubbing)
        {
            Transport.Visibility = Visibility.Collapsed;
        }

        var enhance = _player.EnhanceStatus;
        _enhance.ShowStatus(enhance);
        if (StatsPanel.Visibility == Visibility.Visible) UpdateStats(info, enhance);
    }

    private void UpdateStats(NativeEngine.PlayerInfo info, in NativeEnhance.Status enhance)
    {
        var engine = App.Engine;
        var frame = engine?.GetStats("frame");
        var decode = engine?.GetStats("decode");
        var present = engine?.GetStats("present");
        double fps = frame is { Samples: > 0 } f && f.AvgUs > 0 ? 1_000_000.0 / f.AvgUs : 0;
        StatsText.Text =
            $"{info.Width}×{info.Height}  {info.CodecText}  {(info.HardwareDecode != 0 ? "GPU (D3D12)" : "CPU")}\n" +
            $"fuente     {info.Fps,7:0.00} fps\n" +
            $"pantalla   {fps,7:0.00} fps\n" +
            $"frame p99  {Ms(frame?.P99Us),7} ms\n" +
            $"decode     {Ms(decode?.AvgUs),7} ms\n" +
            $"present    {Ms(present?.AvgUs),7} ms\n" +
            $"mostrados  {info.FramesPresented,7}\n" +
            $"perdidos   {info.FramesDropped,7}" +
            (NativeEnhance.Describe(enhance) is { Length: > 0 } gpu ? "\n" + gpu : "");
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
        _player.Seek(Math.Clamp(info.PositionS + delta, 0, Math.Max(0, info.DurationS - 0.1)));
    }

    private void OnPlayClick(object sender, RoutedEventArgs e) => TogglePlay();
    private void OnTogglePlay(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { TogglePlay(); e.Handled = true; }
    private void OnSeekBack(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { SeekBy(-5); e.Handled = true; }
    private void OnSeekForward(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { SeekBy(5); e.Handled = true; }
    private void OnToggleStats(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { ToggleStats(); e.Handled = true; }
    private void OnToggleStatsClick(object sender, RoutedEventArgs e) => ToggleStats();
    private async void OnOpenAccel(KeyboardAccelerator s, KeyboardAcceleratorInvokedEventArgs e) { e.Handled = true; await PickAsync(); }
    private async void OnOpen(object sender, RoutedEventArgs e) => await PickAsync();

    private void ToggleStats() =>
        StatsPanel.Visibility = StatsPanel.Visibility == Visibility.Visible ? Visibility.Collapsed : Visibility.Visible;

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
