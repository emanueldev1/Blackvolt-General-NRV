using Blackvolt.App.Controls;
using Blackvolt.App.Interop;
using Blackvolt.App.Services;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
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
    private readonly PerfOverlay _perf = new();
    private readonly HashSet<VirtualKey> _keys = [];
    private RemoteStream? _stream;
    private StreamRequest? _request;
    private SessionTracker? _session;
    private DateTime _lastPointerMove = DateTime.UtcNow;
    private StreamState _lastState = StreamState.Idle;
    private readonly QuickSheet _sheet;
    private readonly UiSettings _ui = AppSettings.Current.Ui;
    private bool _locked, _cursorHidden;
    private DateTime _hintUntil;
    private readonly TextBlock _liveVideo = Live(), _liveNet = Live(), _liveFrames = Live(), _livePad = Live();
    private ToggleSwitch? _statsToggle;

    public StreamPage()
    {
        InitializeComponent();
        _timer.Tick += (_, _) => Tick();
        _enhance.Changed += () => _stream?.SetEnhance(AppSettings.Current.Video);
        _enhance.AddPage("Sesión", BuildSessionPage());
        _sheet = new QuickSheet("Panel rápido", _enhance);
        _sheet.OpenChanged += open =>
        {
            if (open) ShowChrome();
            else Root.Focus(FocusState.Programmatic);
        };
        Root.Children.Add(_sheet);
        StatsPanel.Child = _perf;
    }

    // ---- quick panel: session tab ----------------------------------------------------------

    private static TextBlock Live() => new() { FontSize = 12.5, FontFamily = new FontFamily("Cascadia Mono, Consolas"), TextWrapping = TextWrapping.Wrap };

    private StackPanel BuildSessionPage()
    {
        var page = new StackPanel { Spacing = 8, Padding = new Thickness(0, 2, 0, 12) };
        TextBlock Caption(string t) => new()
        {
            Text = t, FontSize = 11, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, CharacterSpacing = 60,
            Foreground = (Brush)Application.Current.Resources["BvSubtleBrush"], Margin = new Thickness(0, 6, 0, 0),
        };
        page.Children.Add(Caption("EN VIVO"));
        var live = new Border
        {
            Background = (Brush)Application.Current.Resources["BvSurface2Brush"],
            BorderBrush = (Brush)Application.Current.Resources["BvBorderBrush"],
            BorderThickness = new Thickness(1), CornerRadius = new CornerRadius(10), Padding = new Thickness(12, 10, 12, 10),
        };
        var lines = new StackPanel { Spacing = 4 };
        lines.Children.Add(_liveVideo);
        lines.Children.Add(_liveNet);
        lines.Children.Add(_liveFrames);
        lines.Children.Add(_livePad);
        live.Child = lines;
        page.Children.Add(live);

        page.Children.Add(Caption("CONSOLA"));
        var actions = new Grid { ColumnSpacing = 8, RowSpacing = 8 };
        actions.ColumnDefinitions.Add(new ColumnDefinition());
        actions.ColumnDefinitions.Add(new ColumnDefinition());
        void Act(int i, string glyph, string text, string tip, Action run)
        {
            var b = new Button { HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Left };
            var content = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            content.Children.Add(new FontIcon { Glyph = glyph, FontSize = 13 });
            content.Children.Add(new TextBlock { Text = text, FontSize = 12.5 });
            b.Content = content;
            ToolTipService.SetToolTip(b, tip);
            b.Click += (_, _) => run();
            Grid.SetRow(b, i / 2);
            Grid.SetColumn(b, i % 2);
            if (actions.RowDefinitions.Count <= i / 2) actions.RowDefinitions.Add(new RowDefinition());
            actions.Children.Add(b);
        }
        Act(0, "\uE80F", "Botón PS", "Abre el menú de la consola (P)", () => _stream?.PsButton());
        Act(1, "\uE72C", "Imagen nítida", "Pide un fotograma clave si la imagen se ve con artefactos", () => _stream?.RequestKeyframe());
        Act(2, "\uE722", "Captura", "Guarda una captura en Imágenes (F12)", TakeScreenshot);
        Act(3, "\uE708", "Reposo", "Pone la consola en modo reposo", () => _stream?.SleepConsole());
        page.Children.Add(actions);

        page.Children.Add(Caption("PANTALLA"));
        _statsToggle = new ToggleSwitch { Header = "Estadísticas sobre el juego (F3)", IsOn = AppSettings.Current.RemotePlay.ShowStats };
        _statsToggle.Toggled += (_, _) => { if (_statsToggle.IsOn != (StatsPanel.Visibility == Visibility.Visible)) ToggleStats(); };
        page.Children.Add(_statsToggle);
        var hide = new ComboBox { Header = "Ocultar controles y cursor", HorizontalAlignment = HorizontalAlignment.Stretch };
        var hideOptions = new (string Label, double Seconds)[] { ("Tras 1,5 s", 1.6), ("Tras 3 s", 3), ("Tras 6 s", 6), ("Nunca", 0) };
        foreach (var (label, _) in hideOptions) hide.Items.Add(label);
        hide.SelectedIndex = Math.Max(0, Array.FindIndex(hideOptions, o => Math.Abs(o.Seconds - _ui.AutoHideSeconds) < 0.2));
        hide.SelectionChanged += (_, _) => { _ui.AutoHideSeconds = hideOptions[Math.Max(0, hide.SelectedIndex)].Seconds; AppSettings.Current.Save(); };
        page.Children.Add(hide);
        var fullscreen = new Button { Content = "Pantalla completa (F11 · doble clic)", HorizontalAlignment = HorizontalAlignment.Stretch };
        fullscreen.Click += (_, _) => ToggleFullscreen();
        page.Children.Add(fullscreen);

        page.Children.Add(Caption("TECLADO COMO MANDO"));
        page.Children.Add(new TextBlock
        {
            FontFamily = new FontFamily("Cascadia Mono, Consolas"), FontSize = 12, LineHeight = 19,
            Foreground = (Brush)Application.Current.Resources["BvMutedBrush"],
            Text = "WASD        stick izquierdo\nFlechas     cruceta\nEnter/Esp.  ✕     Retroceso  ○\nQ / E       □ / △\n1 / 3       L1 / R1\n2 / 4       L2 / R2\nO           Options   T  panel táctil\nP           botón PS\n\nInicio      este panel\nV (mant.)   ver el original\nCtrl+L      bloquear controles\nF3 · F10    estadísticas\nF12         captura",
        });

        var disconnect = new Button { Content = "Desconectar", HorizontalAlignment = HorizontalAlignment.Stretch, Margin = new Thickness(0, 10, 0, 0) };
        disconnect.Click += (_, _) => OnBack(this, new RoutedEventArgs());
        page.Children.Add(disconnect);
        return page;
    }

    private void TakeScreenshot()
    {
        if (_stream is not { } stream) return;
        var path = NativeEnhance.NewScreenshotPath();
        _ = Task.Run(() =>
        {
            var ok = stream.Screenshot(path) == NativeEngine.Result.Ok;
            DispatcherQueue.TryEnqueue(() => App.MainWindow?.Toast(ok ? "Captura guardada" : "No se pudo guardar la captura",
                ok ? path : "Inténtalo de nuevo cuando haya imagen.", ok ? InfoBarSeverity.Success : InfoBarSeverity.Warning, 3.5));
        });
    }

    // ---- chrome auto-hide, cursor, lock, hints ----------------------------------------------

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern nint SetCursor(nint cursor);

    private void ShowChrome()
    {
        _lastPointerMove = DateTime.UtcNow;
        if (!_locked) TopBar.Visibility = Visibility.Visible;
        _cursorHidden = false;
    }

    private void Hint(string text, double seconds = 1.8)
    {
        HintText.Text = text;
        HintPill.Visibility = Visibility.Visible;
        _hintUntil = DateTime.UtcNow.AddSeconds(seconds);
    }

    private void SetShowOriginal(bool on)
    {
        var video = AppSettings.Current.Video;
        if (video.ShowOriginal == on) return;
        video.ShowOriginal = on;
        _stream?.SetEnhance(video);
        if (on) Hint("Original · suelta V para volver", 60);
        else HintPill.Visibility = Visibility.Collapsed;
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        _request = e.Parameter as StreamRequest;
        ConsoleName.Text = _request?.Nickname ?? "";
        if (_request is not null)
        {
            AppSettings.Current.Ui.AddRecent("console", _request.Nickname, _request.Index.ToString());
            AppSettings.Current.Save();
        }
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
        AppSettings.Current.Video.ShowOriginal = false;
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
            var extras = (enhance.Flags.HasFlag(NativeEnhance.Flags.NeuralActive) ? " · DLSS NR" : "") +
                         (enhance.Flags.HasFlag(NativeEnhance.Flags.VsrActive) ? " · RTX VSR" : "") +
                         (enhance.Flags.HasFlag(NativeEnhance.Flags.HdrActive) ? " · HDR" : "");
            QualityText.Text = $"{info.Height}p · {info.CodecText} · {info.BitrateMbps:0.0} Mbps{extras}";
            var p99 = App.Engine?.GetStats("stream.latency") is { Samples: > 0 } lat ? lat.P99Us / 1000.0 : 0;
            _session?.Sample(info.Width, info.Height, info.FramesPresented, info.FramesLost, p99,
                             $"{info.CodecText} · {info.BitrateMbps:0.0} Mbps · RTT {info.RttMs:0} ms{extras}");
        }
        PadText.Text = info.GamepadConnected != 0 ? info.GamepadText : "Sin mando (usa el teclado)";

        // Auto-hide the bar and the cursor while playing (not while the panel is open).
        var idle = (DateTime.UtcNow - _lastPointerMove).TotalSeconds;
        var hideAfter = _ui.AutoHideSeconds;
        if (streaming && !_sheet.IsOpen && (_locked || (hideAfter > 0 && idle > hideAfter)))
        {
            TopBar.Visibility = Visibility.Collapsed;
            if (!_cursorHidden && (_locked || hideAfter > 0)) { SetCursor(0); _cursorHidden = true; }
        }
        if (!streaming && !_locked) TopBar.Visibility = Visibility.Visible;
        if (HintPill.Visibility == Visibility.Visible && DateTime.UtcNow > _hintUntil) HintPill.Visibility = Visibility.Collapsed;

        if (_sheet.IsOpen)
        {
            _sheet.Subtitle = streaming
                ? $"{_request?.Nickname} · {info.Width}×{info.Height} · {info.Fps:0} fps" +
                  (enhance.Flags.HasFlag(NativeEnhance.Flags.NeuralActive) ? $" · NR {enhance.GpuNrTotalMs:0.0} ms" : "")
                : "Sin señal todavía";
            _liveVideo.Text = $"Vídeo  {info.Width}×{info.Height} · {info.CodecText} · {info.Fps:0.0} fps · {(info.HardwareDecode != 0 ? "GPU" : "CPU")}";
            _liveNet.Text = $"Red    {info.BitrateMbps:0.0} Mbps · RTT {info.RttMs:0} ms";
            _liveFrames.Text = $"Fotog. {info.FramesLost} perdidos · {info.FramesRecovered} recuperados · {info.Reconnects} reconex.";
            _livePad.Text = $"Mando  {(info.GamepadConnected != 0 ? info.GamepadText : "teclado")}";
        }

        if (StatsPanel.Visibility == Visibility.Visible) UpdateStats(info, enhance);
    }

    private void UpdateStats(NativeRemote.StreamInfo info, in NativeEnhance.Status enhance)
    {
        var latency = App.Engine?.GetStats("stream.latency");
        var decode = App.Engine?.GetStats("stream.decode");
        var lines = new List<PerfLine>
        {
            new("Stream", $"{info.Fps:0.0} fps · {info.BitrateMbps:0.0} Mbps · {(info.HardwareDecode != 0 ? "GPU" : "CPU")}"),
            new("Red (RTT)", $"{info.RttMs:0.0} ms"),
            new("Decodificación", $"{Ms(decode?.AvgUs)} ms"),
            new("Latencia", $"p50 {Ms(latency?.P50Us)} · p95 {Ms(latency?.P95Us)} ms"),
            new("Fotogramas", $"{info.FramesLost} perdidos · {info.FramesRecovered} recuperados"),
            new("Reconexiones", info.Reconnects.ToString()),
        };
        _perf.Update(App.Engine, $"{info.Width}×{info.Height} {info.CodecText}", "stream.latency", stageIsInterval: false,
                     info.Fps > 0 ? 1000.0 / info.Fps : 0, enhance, lines);
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

    private static bool CtrlDown() =>
        Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(VirtualKey.Control)
            .HasFlag(Windows.UI.Core.CoreVirtualKeyStates.Down);

    private void OnKeyDown(object sender, KeyRoutedEventArgs e)
    {
        // The quick panel: Home toggles it, Esc closes it; keys typed in it stay in it.
        if (e.Key == VirtualKey.Home) { _sheet.Toggle(); e.Handled = true; return; }
        if (e.Key == VirtualKey.Escape && _sheet.IsOpen) { _sheet.IsOpen = false; e.Handled = true; return; }
        if (_sheet.Contains(e.OriginalSource as DependencyObject)) return;
        if (e.Key == VirtualKey.L && CtrlDown())
        {
            _locked = !_locked;
            if (_locked) TopBar.Visibility = Visibility.Collapsed;
            else ShowChrome();
            Hint(_locked ? "Controles bloqueados · Ctrl+L para desbloquear" : "Controles desbloqueados");
            e.Handled = true;
            return;
        }
        switch (e.Key)
        {
            case VirtualKey.V: SetShowOriginal(true); e.Handled = true; return;
            case VirtualKey.F10: ToggleStats(); e.Handled = true; return;
            case VirtualKey.F11: ToggleFullscreen(); e.Handled = true; return;
            case VirtualKey.F3: ToggleStats(); e.Handled = true; return;
            case VirtualKey.F12:
                e.Handled = true;
                TakeScreenshot();
                return;
            case VirtualKey.Escape when IsFullscreen: SetFullscreen(false); e.Handled = true; return;
            case VirtualKey.P: _stream?.PsButton(); e.Handled = true; return;
        }
        if (_keys.Add(e.Key)) SendKeyboardState();
        e.Handled = true;
    }

    private void OnKeyUp(object sender, KeyRoutedEventArgs e)
    {
        if (e.Key == VirtualKey.V) { SetShowOriginal(false); e.Handled = true; return; }
        if (_sheet.Contains(e.OriginalSource as DependencyObject)) return;
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
        if (_statsToggle is not null) _statsToggle.IsOn = show;
        AppSettings.Current.RemotePlay.ShowStats = show;
        AppSettings.Current.Save();
    }

    private void OnPointerMoved(object sender, PointerRoutedEventArgs e) => ShowChrome();

    private void OnDoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if (_locked || _sheet.Contains(e.OriginalSource as DependencyObject)) return;
        ToggleFullscreen();
    }

    private void OnToggleSheet(object sender, RoutedEventArgs e) => _sheet.Toggle();
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
