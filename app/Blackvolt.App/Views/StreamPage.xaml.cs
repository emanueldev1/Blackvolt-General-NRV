using Blackvolt.App.Controls;
using Blackvolt.App.Interop;
using Blackvolt.App.Services;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Navigation;
using Windows.System;
using StreamState = Blackvolt.App.Interop.NativeRemote.StreamState;

namespace Blackvolt.App.Views;

public sealed partial class StreamPage : Page
{
    // DualSense button bits (bv_gamepad_state.buttons).
    private const uint Cross = 1 << 0, Moon = 1 << 1, Box = 1 << 2, Pyramid = 1 << 3;
    private const uint DLeft = 1 << 4, DRight = 1 << 5, DUp = 1 << 6, DDown = 1 << 7;
    private const uint L1 = 1 << 8, R1 = 1 << 9, Options = 1 << 12, Touchpad = 1 << 14;

    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromMilliseconds(200) };
    private readonly EnhancePanel _enhance = new();
    private readonly HashSet<VirtualKey> _keys = [];
    private RemoteStream? _stream;
    private StreamRequest? _request;
    private SessionTracker? _session;
    private DateTime _lastPointerMove = DateTime.UtcNow;
    private StreamState _lastState = StreamState.Idle;

    public StreamPage()
    {
        InitializeComponent();
        _timer.Tick += (_, _) => Tick();
        _enhance.Changed += () => _stream?.SetEnhance(AppSettings.Current.Video);
        EnhanceButton.Flyout = new Flyout { Content = _enhance };
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        _request = e.Parameter as StreamRequest;
        ConsoleName.Text = _request?.Nickname ?? "";
        var settings = AppSettings.Current.RemotePlay;
        Volume.Value = settings.Volume * 100;
        StatsPanel.Visibility = settings.ShowStats ? Visibility.Visible : Visibility.Collapsed;
        App.MainWindow?.SetImmersive(true);

        if (App.Engine is null || App.Consoles is null || _request is null)
        {
            ShowFailure("Remote Play no está disponible", App.EngineError ?? App.ConsolesError ?? "");
            return;
        }
        try
        {
            _stream = RemoteStream.Create(App.Engine, Video);
            ResizeSwapChain();
            _stream.SetVolume((float)settings.Volume);
            _stream.SetEnhance(AppSettings.Current.Video);
            Start();
            _timer.Start();
            Root.Focus(FocusState.Programmatic);
        }
        catch (EngineException ex)
        {
            ShowFailure("No se pudo iniciar el streaming", ex.Message);
        }
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        _timer.Stop();
        _session?.Finish();
        _session = null;
        _stream?.Dispose(); // stops the session, threads and audio
        _stream = null;
        SetFullscreen(false);
        App.MainWindow?.SetImmersive(false);
        AppSettings.Current.Save();
    }

    private void Start()
    {
        if (_stream is null || _request is null || App.Consoles is null) return;
        var s = AppSettings.Current.RemotePlay;
        var codec = _request.Kind == NativeRemote.ConsoleKind.PS4 ? 0 : s.Codec; // PS4: H.264 only
        _stream.Start(App.Consoles, _request.Index, new NativeRemote.StreamSettings
        {
            Width = (uint)s.Width,
            Height = (uint)s.Height,
            Fps = (uint)s.Fps,
            BitrateKbps = (uint)Math.Max(0, s.BitrateKbps),
            Codec = (uint)codec,
            AutoReconnect = s.AutoReconnect ? 1u : 0u,
            WakeIfAsleep = s.WakeIfAsleep ? 1u : 0u,
            DualSenseFeatures = s.DualSenseFeatures ? 1u : 0u,
        });
        _lastState = StreamState.Idle;
        ShowStatus("Conectando…", "");
        _session?.Finish();
        _session = new SessionTracker("Remote Play", _request.Nickname);
    }

    private void Tick()
    {
        if (_stream is null) return;
        var info = _stream.Info;

        if (info.State != _lastState || info.State is StreamState.Reconnecting)
        {
            _lastState = info.State;
            switch (info.State)
            {
                case StreamState.Waking:
                    ShowStatus("Despertando la consola…", "Suele tardar unos segundos.");
                    break;
                case StreamState.Connecting:
                    ShowStatus("Conectando…", "");
                    break;
                case StreamState.Reconnecting:
                    ShowStatus($"Reconectando… (intento {info.Reconnects})", info.ErrorText);
                    break;
                case StreamState.Streaming:
                    StatusPanel.Visibility = Visibility.Collapsed;
                    break;
                case StreamState.Ended:
                    ShowFailure("La sesión terminó", info.ErrorText);
                    break;
                case StreamState.Error:
                    ShowFailure("No se pudo conectar", info.ErrorText);
                    break;
            }
        }

        var streaming = info.State == StreamState.Streaming;
        var enhance = _stream.EnhanceStatus;
        _enhance.ShowStatus(enhance);
        QualityChip.Visibility = streaming ? Visibility.Visible : Visibility.Collapsed;
        if (streaming)
        {
            var extras = (enhance.Flags.HasFlag(NativeEnhance.Flags.VsrActive) ? " · RTX VSR" : "") +
                         (enhance.Flags.HasFlag(NativeEnhance.Flags.HdrActive) ? " · HDR" : "");
            QualityText.Text = $"{info.Height}p · {info.CodecText} · {info.BitrateMbps:0.0} Mbps{extras}";
            var p99 = App.Engine?.GetStats("stream.latency") is { Samples: > 0 } lat ? lat.P99Us / 1000.0 : 0;
            _session?.Sample(info.Width, info.Height, info.FramesPresented, info.FramesLost, p99,
                             $"{info.CodecText} · {info.BitrateMbps:0.0} Mbps · RTT {info.RttMs:0} ms{extras}");
        }
        PadText.Text = info.GamepadConnected != 0 ? info.GamepadText : "Sin mando (usa el teclado)";

        // Auto-hide the bar and cursor-free view after 3 s while playing.
        if (streaming && (DateTime.UtcNow - _lastPointerMove).TotalSeconds > 3) TopBar.Visibility = Visibility.Collapsed;
        if (!streaming) TopBar.Visibility = Visibility.Visible;

        if (StatsPanel.Visibility == Visibility.Visible) UpdateStats(info, enhance);
    }

    private void UpdateStats(NativeRemote.StreamInfo info, in NativeEnhance.Status enhance)
    {
        var latency = App.Engine?.GetStats("stream.latency");
        var decode = App.Engine?.GetStats("stream.decode");
        StatsText.Text =
            $"{info.Width}×{info.Height}  {info.CodecText}  {(info.HardwareDecode != 0 ? "GPU (D3D12)" : "CPU")}\n" +
            $"fps          {info.Fps,8:0.0}\n" +
            $"bitrate      {info.BitrateMbps,8:0.0} Mbps\n" +
            $"red (RTT)    {info.RttMs,8:0.0} ms\n" +
            $"decodif.     {Ms(decode?.AvgUs),8} ms\n" +
            $"latencia p50 {Ms(latency?.P50Us),8} ms\n" +
            $"latencia p95 {Ms(latency?.P95Us),8} ms\n" +
            $"perdidos     {info.FramesLost,8}\n" +
            $"recuperados  {info.FramesRecovered,8}\n" +
            $"reconexiones {info.Reconnects,8}" +
            (NativeEnhance.Describe(enhance) is { Length: > 0 } gpu ? "\n" + gpu : "");
    }

    private static string Ms(double? us) => us is > 0 ? (us.Value / 1000.0).ToString("0.00") : "–";

    private void ShowStatus(string title, string detail)
    {
        StatusPanel.Visibility = Visibility.Visible;
        StatusRing.IsActive = true;
        StatusRing.Visibility = Visibility.Visible;
        StatusActions.Visibility = Visibility.Collapsed;
        StatusTitle.Text = title;
        StatusDetail.Text = detail;
    }

    private void ShowFailure(string title, string detail)
    {
        StatusPanel.Visibility = Visibility.Visible;
        StatusRing.IsActive = false;
        StatusRing.Visibility = Visibility.Collapsed;
        StatusActions.Visibility = Visibility.Visible;
        StatusTitle.Text = title;
        StatusDetail.Text = detail;
        TopBar.Visibility = Visibility.Visible;
    }

    // ---- swapchain -----------------------------------------------------------------------

    private void OnVideoSizeChanged(object sender, SizeChangedEventArgs e) => ResizeSwapChain();
    private void OnCompositionScaleChanged(SwapChainPanel sender, object args) => ResizeSwapChain();

    private void ResizeSwapChain()
    {
        if (_stream is null) return;
        var sx = Video.CompositionScaleX;
        var sy = Video.CompositionScaleY;
        var w = (uint)Math.Max(1, Math.Round(Video.ActualWidth * sx));
        var h = (uint)Math.Max(1, Math.Round(Video.ActualHeight * sy));
        _stream.Resize(w, h, sx, sy);
    }

    // ---- keyboard as a gamepad -------------------------------------------------------------

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        switch (e.Key)
        {
            case VirtualKey.F11: ToggleFullscreen(); e.Handled = true; return;
            case VirtualKey.F3: ToggleStats(); e.Handled = true; return;
            case VirtualKey.Escape when IsFullscreen: SetFullscreen(false); e.Handled = true; return;
            case VirtualKey.P: _stream?.PsButton(); e.Handled = true; return;
        }
        if (_keys.Add(e.Key)) SendKeyboardState();
        e.Handled = true;
    }

    private void OnKeyUp(object sender, KeyRoutedEventArgs e)
    {
        if (_keys.Remove(e.Key)) SendKeyboardState();
        e.Handled = true;
    }

    private void SendKeyboardState()
    {
        if (_stream is null) return;
        bool Down(VirtualKey k) => _keys.Contains(k);
        uint b = 0;
        if (Down(VirtualKey.Enter) || Down(VirtualKey.Space)) b |= Cross;
        if (Down(VirtualKey.Back)) b |= Moon;
        if (Down(VirtualKey.Q)) b |= Box;
        if (Down(VirtualKey.E)) b |= Pyramid;
        if (Down(VirtualKey.Left)) b |= DLeft;
        if (Down(VirtualKey.Right)) b |= DRight;
        if (Down(VirtualKey.Up)) b |= DUp;
        if (Down(VirtualKey.Down)) b |= DDown;
        if (Down(VirtualKey.Number1)) b |= L1;
        if (Down(VirtualKey.Number3)) b |= R1;
        if (Down(VirtualKey.O)) b |= Options;
        if (Down(VirtualKey.T)) b |= Touchpad;
        short Axis(VirtualKey neg, VirtualKey pos) => (short)((Down(pos) ? short.MaxValue : 0) - (Down(neg) ? short.MaxValue : 0));
        _stream.SetInput(new NativeRemote.GamepadState
        {
            Buttons = b,
            L2 = Down(VirtualKey.Number2) ? (byte)255 : (byte)0,
            R2 = Down(VirtualKey.Number4) ? (byte)255 : (byte)0,
            LeftX = Axis(VirtualKey.A, VirtualKey.D),
            LeftY = Axis(VirtualKey.W, VirtualKey.S),
        });
    }

    // ---- chrome --------------------------------------------------------------------------

    private bool IsFullscreen => App.MainWindow?.AppWindow.Presenter.Kind == AppWindowPresenterKind.FullScreen;

    private void SetFullscreen(bool on)
    {
        var window = App.MainWindow?.AppWindow;
        if (window is null) return;
        window.SetPresenter(on ? AppWindowPresenterKind.FullScreen : AppWindowPresenterKind.Default);
        FullscreenIcon.Glyph = on ? "" : "";
    }

    private void ToggleFullscreen() => SetFullscreen(!IsFullscreen);

    private void ToggleStats()
    {
        var show = StatsPanel.Visibility != Visibility.Visible;
        StatsPanel.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        AppSettings.Current.RemotePlay.ShowStats = show;
        AppSettings.Current.Save();
    }

    private void OnPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        _lastPointerMove = DateTime.UtcNow;
        TopBar.Visibility = Visibility.Visible;
    }

    private void OnDoubleTapped(object sender, DoubleTappedRoutedEventArgs e) => ToggleFullscreen();
    private void OnFullscreen(object sender, RoutedEventArgs e) => ToggleFullscreen();
    private void OnToggleStatsClick(object sender, RoutedEventArgs e) => ToggleStats();
    private void OnPsButton(object sender, RoutedEventArgs e) => _stream?.PsButton();
    private void OnKeyframe(object sender, RoutedEventArgs e) => _stream?.RequestKeyframe();
    private void OnSleepConsole(object sender, RoutedEventArgs e) => _stream?.SleepConsole();

    private void OnRetry(object sender, RoutedEventArgs e)
    {
        try { Start(); }
        catch (EngineException ex) { ShowFailure("No se pudo iniciar el streaming", ex.Message); }
    }

    private void OnBack(object sender, RoutedEventArgs e)
    {
        if (Frame.CanGoBack) Frame.GoBack();
    }

    private void OnVolumeChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        var v = e.NewValue / 100.0;
        _stream?.SetVolume((float)v);
        AppSettings.Current.RemotePlay.Volume = v; // saved when leaving the page
    }
}
