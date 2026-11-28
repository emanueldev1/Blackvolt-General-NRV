using Blackvolt.App.Services;
using Blackvolt.App.Views;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;

namespace Blackvolt.App;

public sealed partial class MainWindow : Window
{
    private static readonly Dictionary<string, Type> Pages = new()
    {
        ["home"] = typeof(HomePage),
        ["player"] = typeof(PlayerPage),
        ["remote"] = typeof(RemotePlayPage),
        ["capture"] = typeof(CapturePage),
        ["export"] = typeof(ExportPage),
        ["runtimes"] = typeof(RuntimesPage),
        ["stats"] = typeof(StatsPage),
        ["settings"] = typeof(SettingsPage),
        ["stream"] = typeof(StreamPage),
    };

    private static readonly Dictionary<string, string> Titles = new()
    {
        ["home"] = "Inicio", ["player"] = "Reproductor", ["remote"] = "PlayStation", ["capture"] = "Captura",
        ["export"] = "Exportar", ["runtimes"] = "Runtimes NVIDIA", ["stats"] = "Rendimiento", ["settings"] = "Ajustes",
    };

    private readonly UiSettings _ui = AppSettings.Current.Ui;

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(TitleBar);
        StyleCaptionButtons();
        var icon = Path.Combine(AppContext.BaseDirectory, "Assets", "Blackvolt.ico");
        if (File.Exists(icon)) AppWindow.SetIcon(icon);
        VersionText.Text = "v" + Updater.CurrentVersion.ToString(3);
        RestoreWindow();
        Nav.IsPaneOpen = _ui.PaneOpen;
        Nav.Loaded += (_, _) =>
        {
            if (Nav.SettingsItem is NavigationViewItem settingsItem) settingsItem.Content = "Ajustes";
            ContentFrame.Focus(FocusState.Programmatic);
        };

        var start = _ui.StartPage == "last" ? _ui.LastPage : _ui.StartPage;
        Go(Pages.ContainsKey(start) && start is not "settings" ? start : "home");

        RefreshStatus();
        NrRuntimeInstaller.Changed += () => DispatcherQueue.TryEnqueue(OnInstallerChanged);
        Closed += (_, _) => SaveWindow();
    }

    // ------------------------------------------------------------------ window

    private void StyleCaptionButtons()
    {
        if (!AppWindowTitleBar.IsCustomizationSupported()) return;
        var bar = AppWindow.TitleBar;
        bar.ButtonBackgroundColor = Colors.Transparent;
        bar.ButtonInactiveBackgroundColor = Colors.Transparent;
        bar.ButtonForegroundColor = Windows.UI.Color.FromArgb(255, 200, 206, 220);
        bar.ButtonInactiveForegroundColor = Windows.UI.Color.FromArgb(255, 93, 101, 120);
        bar.ButtonHoverBackgroundColor = Windows.UI.Color.FromArgb(255, 24, 28, 38);
        bar.ButtonHoverForegroundColor = Colors.White;
        bar.ButtonPressedBackgroundColor = Windows.UI.Color.FromArgb(255, 30, 35, 48);
        bar.PreferredHeightOption = TitleBarHeightOption.Tall;
    }

    private void RestoreWindow()
    {
        var w = Math.Max(960, _ui.WindowWidth);
        var h = Math.Max(620, _ui.WindowHeight);
        if (_ui.RememberWindow && _ui.WindowX != int.MinValue && OnScreen(_ui.WindowX, _ui.WindowY, w, h))
            AppWindow.MoveAndResize(new Windows.Graphics.RectInt32(_ui.WindowX, _ui.WindowY, w, h));
        else
            AppWindow.Resize(new Windows.Graphics.SizeInt32(w, h));
        if (_ui.RememberWindow && _ui.WindowMaximized && AppWindow.Presenter is OverlappedPresenter p) p.Maximize();
    }

    private static bool OnScreen(int x, int y, int w, int h)
    {
        var area = DisplayArea.GetFromRect(new Windows.Graphics.RectInt32(x, y, w, h), DisplayAreaFallback.None);
        return area is not null;
    }

    private void SaveWindow()
    {
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            _ui.WindowMaximized = p.State == OverlappedPresenterState.Maximized;
            if (p.State == OverlappedPresenterState.Restored)
            {
                _ui.WindowX = AppWindow.Position.X;
                _ui.WindowY = AppWindow.Position.Y;
                _ui.WindowWidth = AppWindow.Size.Width;
                _ui.WindowHeight = AppWindow.Size.Height;
            }
        }
        AppSettings.Current.Save();
    }

    private void OnPaneChanged(NavigationView sender, object args)
    {
        // PaneOpening / PaneClosing fire before IsPaneOpen changes.
        DispatcherQueue.TryEnqueue(() => { _ui.PaneOpen = Nav.IsPaneOpen; });
    }

    // ------------------------------------------------------------------ status (GPU, runtimes, installs)

    /// <summary>Refreshes the sidebar's GPU card and runtime chips.</summary>
    internal void RefreshStatus()
    {
        var adapter = App.Engine?.Adapter;
        GpuName.Text = adapter?.Name ?? "Sin GPU compatible";
        GpuDot.Fill = Brush(adapter is null ? "BvDangerBrush" : "BvSuccessBrush");
        ToolTipService.SetToolTip(GpuCard, adapter is null
            ? App.EngineError ?? "El motor no pudo iniciar."
            : $"{adapter.Name}\n{adapter.VramGiB:0.#} GB · driver {adapter.DriverVersion}");

        RuntimeChips.Children.Clear();
        var nvidia = adapter?.Vendor == Interop.NativeEngine.Vendor.Nvidia;
        if (!nvidia)
        {
            RuntimeChips.Children.Add(Chip("NIS · interpolación", true));
            return;
        }
        var nr = NrRuntimeInstaller.Installed(Interop.NativeEngine.NrVariant.Compat) ||
                 NrRuntimeInstaller.Installed(Interop.NativeEngine.NrVariant.Original);
        RuntimeChips.Children.Add(Chip("NR", nr));
        RuntimeChips.Children.Add(Chip("VSR", NrRuntimeInstaller.VideoRuntimes[0].Installed));
        RuntimeChips.Children.Add(Chip("HDR", NrRuntimeInstaller.VideoRuntimes[1].Installed));
    }

    private static Border Chip(string text, bool ok)
    {
        var chip = new Border
        {
            Padding = new Thickness(7, 1, 7, 2),
            CornerRadius = new CornerRadius(999),
            Background = Brush(ok ? "BvPrimarySoftBrush" : "BvSurface3Brush"),
            Child = new TextBlock
            {
                Text = text,
                FontSize = 11,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = Brush(ok ? "BvVoltBrush" : "BvSubtleBrush"),
            },
        };
        ToolTipService.SetToolTip(chip, ok ? $"{text}: instalado" : $"{text}: no instalado (se descarga solo, o desde Runtimes NVIDIA)");
        return chip;
    }

    private bool _installerWasBusy;

    private void OnInstallerChanged()
    {
        var busy = NrRuntimeInstaller.Busy;
        var finished = _installerWasBusy && !busy;
        _installerWasBusy = busy;
        ActivityChip.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        ActivityText.Text = NrRuntimeInstaller.Message;
        if (finished)
        {
            RefreshStatus();
            if (NrRuntimeInstaller.Message.Length > 0)
                Toast(NrRuntimeInstaller.LastFailed ? "No se pudieron instalar los runtimes" : "Runtimes NVIDIA listos",
                      NrRuntimeInstaller.Message, NrRuntimeInstaller.LastFailed ? InfoBarSeverity.Warning : InfoBarSeverity.Success);
        }
    }

    /// <summary>Small notification in the bottom-right corner that closes itself.</summary>
    internal void Toast(string title, string message, InfoBarSeverity severity = InfoBarSeverity.Informational, double seconds = 5)
    {
        var bar = new InfoBar
        {
            Title = title,
            Message = message,
            Severity = severity,
            IsOpen = true,
            Background = Brush("BvSurfaceBrush"),
            BorderBrush = Brush("BvBorderStrongBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(12),
        };
        bar.CloseButtonClick += (_, _) => ToastHost.Children.Remove(bar);
        ToastHost.Children.Add(bar);
        while (ToastHost.Children.Count > 3) ToastHost.Children.RemoveAt(0);
        var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(seconds) };
        timer.Tick += (_, _) => { timer.Stop(); ToastHost.Children.Remove(bar); };
        timer.Start();
    }

    private static Brush Brush(string key) => (Brush)Application.Current.Resources[key];

    // ------------------------------------------------------------------ navigation

    /// <summary>Hides the navigation pane and title bar while streaming or watching: the video gets the window.</summary>
    internal void SetImmersive(bool on)
    {
        Nav.IsPaneVisible = !on;
        Nav.IsPaneToggleButtonVisible = !on;
    }

    /// <summary>Selects a section of the navigation pane by its tag, as if the user clicked it.</summary>
    internal void Go(string tag)
    {
        if (tag == "settings")
        {
            Nav.SelectedItem = Nav.SettingsItem;
            return;
        }
        var item = Nav.MenuItems.OfType<NavigationViewItem>().FirstOrDefault(i => i.Tag as string == tag);
        if (item is not null) Nav.SelectedItem = item;
        else if (Pages.TryGetValue(tag, out var page)) ContentFrame.Navigate(page);
    }

    /// <summary>Navigates to a page with a parameter (e.g. the player with a file to open).</summary>
    internal void Go(string tag, object parameter)
    {
        if (!Pages.TryGetValue(tag, out var page)) return;
        _suppressNavigate = true;
        Go(tag == "stream" ? "remote" : tag);
        _suppressNavigate = false;
        ContentFrame.Navigate(page, parameter, new SuppressNavigationTransitionInfo());
    }

    private bool _suppressNavigate;

    private void OnNavSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        var tag = args.IsSettingsSelected ? "settings" : (args.SelectedItemContainer?.Tag as string ?? "home");
        if (!Pages.TryGetValue(tag, out var page)) return;
        PageCrumb.Text = Titles.GetValueOrDefault(tag, "");
        if (tag != "settings") _ui.LastPage = tag;
        if (_suppressNavigate) return;
        var transition = _ui.ReduceMotion
            ? (NavigationTransitionInfo)new SuppressNavigationTransitionInfo()
            : new EntranceNavigationTransitionInfo();
        ContentFrame.Navigate(page, Titles.GetValueOrDefault(tag, ""), transition);
    }
}
