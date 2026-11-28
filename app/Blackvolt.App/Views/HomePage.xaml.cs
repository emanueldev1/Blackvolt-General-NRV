using Blackvolt.App.Interop;
using Blackvolt.App.Services;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;
using NrVariant = Blackvolt.App.Interop.NativeEngine.NrVariant;

namespace Blackvolt.App.Views;

public sealed partial class HomePage : Page
{
    private static readonly string[] VideoTypes =
        [".mp4", ".mkv", ".mov", ".avi", ".webm", ".ts", ".m2ts", ".wmv", ".flv", ".m4v", ".mpg", ".mpeg"];

    private readonly UiSettings _ui = AppSettings.Current.Ui;

    public HomePage()
    {
        InitializeComponent();
        Greeting.Text = DateTime.Now.Hour switch
        {
            < 6 => "Buenas noches",
            < 13 => "Buenos días",
            < 20 => "Buenas tardes",
            _ => "Buenas noches",
        };
        BuildTiles();
        BuildShortcuts();
        Refresh();
        if (Updater.Available is { } update) ShowUpdate(update);
        Updater.Found += OnUpdateFound;
        NrRuntimeInstaller.Changed += OnInstallerChanged;
        Unloaded += (_, _) =>
        {
            Updater.Found -= OnUpdateFound;
            NrRuntimeInstaller.Changed -= OnInstallerChanged;
        };
        SizeChanged += (_, e) => HeroArt.Visibility = e.NewSize.Width < 900 ? Visibility.Collapsed : Visibility.Visible;
    }

    private void OnInstallerChanged() => DispatcherQueue.TryEnqueue(Refresh);

    private void Refresh()
    {
        ShowGpu(App.Engine?.Adapter, App.EngineError);
        ShowRecent();
    }

    // ------------------------------------------------------------------ hero + GPU

    private void ShowGpu(GpuAdapter? gpu, string? error)
    {
        if (gpu is null)
        {
            GpuCard.Visibility = Visibility.Collapsed;
            EngineErrorBar.Message = error ?? "Error desconocido.";
            EngineErrorBar.IsOpen = true;
            Notices.Visibility = Visibility.Visible;
            HeroStatus.Text = "Motor no disponible";
            HeroDot.Fill = Res<Brush>("BvDangerBrush");
            return;
        }

        GpuName.Text = gpu.Name;
        var driver = string.IsNullOrEmpty(gpu.DriverVersion) ? "" : $" · driver {gpu.DriverVersion}";
        GpuDetails.Text = $"{gpu.VramGiB:0.#} GB VRAM{driver}";

        var nvidia = gpu.Vendor == NativeEngine.Vendor.Nvidia;
        var nr = NrRuntimeInstaller.Installed(NrVariant.Compat) || NrRuntimeInstaller.Installed(NrVariant.Original);
        var vsr = NrRuntimeInstaller.VideoRuntimes[0].Installed;
        var hdr = NrRuntimeInstaller.VideoRuntimes[1].Installed;
        var busy = NrRuntimeInstaller.Busy;

        FeatureList.Children.Clear();
        FeatureList.Children.Add(Feature("DLSS NR", "Reconstrucción neuronal", nvidia ? (nr ? 2 : busy ? 1 : 0) : -1));
        FeatureList.Children.Add(Feature("RTX Video Super Resolution", "Escalado con IA", nvidia ? (vsr ? 2 : busy ? 1 : 0) : -1));
        FeatureList.Children.Add(Feature("RTX Video TrueHDR", "SDR a HDR", nvidia ? (hdr ? 2 : busy ? 1 : 0) : -1));
        FeatureList.Children.Add(Feature("Interpolación de fotogramas", "Flujo óptico propio, cualquier GPU", 2));
        FeatureList.Children.Add(Feature("NVIDIA Image Scaling", "Escalado y nitidez, cualquier GPU", 2));

        var ready = new List<string>();
        if (nvidia && nr) ready.Add("DLSS NR");
        if (nvidia && vsr) ready.Add("VSR");
        if (nvidia && hdr) ready.Add("TrueHDR");
        HeroStatus.Text = busy ? $"{ShortName(gpu.Name)} · instalando runtimes…"
            : ready.Count > 0 ? $"{ShortName(gpu.Name)} · {string.Join(" · ", ready)} listos"
            : $"{ShortName(gpu.Name)} lista";
        HeroDot.Fill = Res<Brush>(busy ? "BvWarningBrush" : "BvSuccessBrush");
    }

    private static string ShortName(string name) => name.Replace("NVIDIA ", "").Replace("GeForce ", "");

    /// <summary>state: -1 not for this GPU, 0 missing, 1 installing, 2 ready.</summary>
    private static Grid Feature(string title, string detail, int state)
    {
        var row = new Grid { ColumnSpacing = 10 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(new Ellipse
        {
            Width = 8, Height = 8, VerticalAlignment = VerticalAlignment.Center,
            Fill = Res<Brush>(state switch { 2 => "BvSuccessBrush", 1 => "BvWarningBrush", 0 => "BvSubtleBrush", _ => "BvBorderStrongBrush" }),
        });
        var text = new StackPanel();
        text.Children.Add(new TextBlock { Text = title, FontSize = 13, FontWeight = FontWeights.SemiBold });
        text.Children.Add(new TextBlock { Text = detail, FontSize = 11.5, Foreground = Res<Brush>("BvMutedBrush") });
        Grid.SetColumn(text, 1);
        row.Children.Add(text);
        var badge = new TextBlock
        {
            Text = state switch { 2 => "Listo", 1 => "Instalando", 0 => "Falta", _ => "Solo RTX" },
            FontSize = 11, VerticalAlignment = VerticalAlignment.Center,
            Foreground = Res<Brush>(state == 2 ? "BvSuccessBrush" : state == 1 ? "BvWarningBrush" : "BvSubtleBrush"),
        };
        Grid.SetColumn(badge, 2);
        row.Children.Add(badge);
        return row;
    }

    // ------------------------------------------------------------------ tiles

    private void BuildTiles()
    {
        var consoles = App.Consoles?.List() ?? [];
        var ps = consoles.FirstOrDefault();
        var tiles = new (string Glyph, string Title, string Detail, Action Run)[]
        {
            ("", "Abrir vídeo", "Archivos locales con NR, VSR y HDR", () => _ = OpenFileAsync()),
            ("", "PlayStation", ps is null ? "Remote Play de PS5 y PS4" : ps.Host.Length == 0 ? $"{ps.Nickname} · sin dirección" : ps.Nickname.Contains(ps.Host) ? ps.Nickname : $"{ps.Nickname} · {ps.Host}", () => App.MainWindow?.Go("remote")),
            ("", "Capturadora", "Consola o cámara con latencia mínima", () => App.MainWindow?.Go("capture")),
            ("", "Pantalla", "Captura un monitor o una ventana", () => App.MainWindow?.Go("capture")),
            ("", "URL o red", "HTTP, HLS, RTSP, SRT, UDP…", () => _ = OpenUrlAsync()),
            ("", "Exportar", "Recodifica con las mejoras aplicadas", () => App.MainWindow?.Go("export")),
        };
        for (int i = 0; i < tiles.Length; i++)
        {
            var (glyph, title, detail, run) = tiles[i];
            var content = new StackPanel { Spacing = 10 };
            content.Children.Add(new Border
            {
                Width = 38, Height = 38, CornerRadius = new CornerRadius(10), HorizontalAlignment = HorizontalAlignment.Left,
                Background = Res<Brush>("BvPrimarySoftBrush"),
                Child = new FontIcon { Glyph = glyph, FontSize = 17, Foreground = Res<Brush>("BvVoltBrush") },
            });
            var text = new StackPanel { Spacing = 2 };
            text.Children.Add(new TextBlock { Text = title, FontWeight = FontWeights.SemiBold, FontSize = 14 });
            text.Children.Add(new TextBlock { Text = detail, FontSize = 12, Foreground = Res<Brush>("BvMutedBrush"), TextWrapping = TextWrapping.Wrap });
            content.Children.Add(text);
            var button = new Button { Style = Res<Style>("BvTileButtonStyle"), Content = content, MinHeight = 128 };
            button.Click += (_, _) => run();
            Grid.SetRow(button, i / 3);
            Grid.SetColumn(button, i % 3);
            Tiles.Children.Add(button);
        }
    }

    private void BuildShortcuts()
    {
        var items = new (string Key, string What)[]
        {
            ("Inicio", "Panel rápido de mejoras"), ("F11 · doble clic", "Pantalla completa"), ("Espacio", "Reproducir / pausa"),
            ("V (mantener)", "Ver el original"), ("F3", "Estadísticas"), ("S · F12", "Captura de pantalla"), ("Ctrl+L", "Bloquear controles"),
        };
        foreach (var (key, what) in items)
        {
            var row = new Grid();
            row.Children.Add(new TextBlock { Text = what, FontSize = 12.5, Foreground = Res<Brush>("BvMutedBrush") });
            var k = new Border
            {
                Style = Res<Style>("BvBadgeStyle"), HorizontalAlignment = HorizontalAlignment.Right, Padding = new Thickness(7, 1, 7, 2),
                Child = new TextBlock { Text = key, FontSize = 11, FontFamily = new FontFamily("Cascadia Mono, Consolas") },
            };
            row.Children.Add(k);
            ShortcutList.Children.Add(row);
        }
    }

    // ------------------------------------------------------------------ recent / continue

    private void ShowRecent()
    {
        RecentList.Children.Clear();
        var recent = _ui.Recent.Where(Usable).Take(8).ToList();
        RecentEmpty.Visibility = recent.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        ClearRecent.Visibility = recent.Count == 0 ? Visibility.Collapsed : Visibility.Visible;
        foreach (var item in recent) RecentList.Children.Add(RecentRow(item));

        var last = recent.FirstOrDefault();
        ContinueCard.Visibility = last is null ? Visibility.Collapsed : Visibility.Visible;
        if (last is null) return;
        ContinueTitle.Text = last.Title;
        ContinueIcon.Glyph = KindGlyph(last.Kind);
        var position = last.Kind == "file" && AppSettings.Current.Capture.LastUrl == last.Target && AppSettings.Current.Capture.LastPosition > 1
            ? $" · desde {TimeSpan.FromSeconds(AppSettings.Current.Capture.LastPosition):h\\:mm\\:ss}" : "";
        ContinueDetail.Text = KindLabel(last.Kind) + position + " · " + Ago(last.WhenUtc);
    }

    private static bool Usable(RecentItem r) => r.Kind != "file" || File.Exists(r.Target);

    private Button RecentRow(RecentItem item)
    {
        var grid = new Grid { ColumnSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.Children.Add(new FontIcon { Glyph = KindGlyph(item.Kind), FontSize = 15, Foreground = Res<Brush>("BvMutedBrush") });
        var text = new StackPanel();
        text.Children.Add(new TextBlock { Text = item.Title, FontSize = 13, TextTrimming = TextTrimming.CharacterEllipsis });
        text.Children.Add(new TextBlock
        {
            Text = item.Kind == "console" ? "PlayStation" : item.Target, FontSize = 11.5,
            Foreground = Res<Brush>("BvSubtleBrush"), TextTrimming = TextTrimming.CharacterEllipsis,
        });
        Grid.SetColumn(text, 1);
        grid.Children.Add(text);
        var when = new TextBlock { Text = Ago(item.WhenUtc), FontSize = 11.5, Foreground = Res<Brush>("BvSubtleBrush"), VerticalAlignment = VerticalAlignment.Center };
        Grid.SetColumn(when, 2);
        grid.Children.Add(when);
        var button = new Button
        {
            Content = grid, Style = Res<Style>("BvGhostButtonStyle"), HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Stretch, Padding = new Thickness(12, 8, 12, 8),
        };
        button.Click += (_, _) => Launch(item);
        return button;
    }

    private void OnContinue(object sender, RoutedEventArgs e)
    {
        if (_ui.Recent.FirstOrDefault(Usable) is { } item) Launch(item);
    }

    private void OnClearRecent(object sender, RoutedEventArgs e)
    {
        _ui.Recent.Clear();
        AppSettings.Current.Save();
        ShowRecent();
    }

    private static void Launch(RecentItem item)
    {
        if (item.Kind == "console")
        {
            if (uint.TryParse(item.Target, out var index) && App.Consoles?.List().FirstOrDefault(c => c.Index == index) is { } console)
                App.MainWindow?.Go("stream", new StreamRequest(console.Index, console.Nickname, console.Kind));
            else
                App.MainWindow?.Go("remote");
            return;
        }
        App.MainWindow?.Go("player", new OpenMedia(item.Target, item.Title));
    }

    private static string KindGlyph(string kind) => kind switch
    {
        "console" => "", "capture" => "", "url" => "", "screen" => "", _ => "",
    };

    private static string KindLabel(string kind) => kind switch
    {
        "console" => "PlayStation", "capture" => "Capturadora", "url" => "Stream de red", "screen" => "Pantalla", _ => "Vídeo",
    };

    private static string Ago(DateTime utc)
    {
        var d = DateTime.UtcNow - utc;
        if (d.TotalMinutes < 1) return "ahora";
        if (d.TotalHours < 1) return $"hace {d.TotalMinutes:0} min";
        if (d.TotalDays < 1) return $"hace {d.TotalHours:0} h";
        if (d.TotalDays < 2) return "ayer";
        return utc.ToLocalTime().ToString("d MMM");
    }

    // ------------------------------------------------------------------ open

    private async Task OpenFileAsync()
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.VideosLibrary };
        foreach (var t in VideoTypes) picker.FileTypeFilter.Add(t);
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow!));
        var file = await picker.PickSingleFileAsync();
        if (file is not null) App.MainWindow?.Go("player", new OpenMedia(file.Path, file.Name));
    }

    private async Task OpenUrlAsync()
    {
        var box = new TextBox { PlaceholderText = "https://…  ·  rtsp://…  ·  srt://…  ·  udp://…", MinWidth = 420 };
        var dialog = new ContentDialog
        {
            Title = "Abrir URL",
            Content = box,
            PrimaryButtonText = "Abrir",
            CloseButtonText = "Cancelar",
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = XamlRoot,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        var url = box.Text.Trim();
        if (url.Length == 0) return;
        App.MainWindow?.Go("player", new OpenMedia(url, url));
    }

    private void OnOpenFile(object sender, RoutedEventArgs e) => _ = OpenFileAsync();
    private void OnOpenUrl(object sender, RoutedEventArgs e) => _ = OpenUrlAsync();

    private void OnGo(object sender, RoutedEventArgs e)
    {
        if (sender is FrameworkElement { Tag: string tag }) App.MainWindow?.Go(tag);
    }

    private void OnDragOver(object sender, DragEventArgs e)
    {
        if (e.DataView.Contains(StandardDataFormats.StorageItems)) e.AcceptedOperation = DataPackageOperation.Link;
    }

    private async void OnDrop(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        var items = await e.DataView.GetStorageItemsAsync();
        if (items.FirstOrDefault() is StorageFile file) App.MainWindow?.Go("player", new OpenMedia(file.Path, file.Name));
    }

    // ------------------------------------------------------------------ updates

    private void OnUpdateFound(UpdateInfo info) => DispatcherQueue.TryEnqueue(() => ShowUpdate(info));

    private void ShowUpdate(UpdateInfo info)
    {
        if (info.Tag == AppSettings.Current.Update.SkippedTag) return;
        Notices.Visibility = Visibility.Visible;
        UpdateBar.Message = $"Versión {info.Version} disponible (tienes la {Updater.CurrentVersion}).";
        UpdateBar.IsOpen = true;
    }

    private void OnOpenUpdate(object sender, RoutedEventArgs e) => App.MainWindow?.Go("settings");

    private static T Res<T>(string key) => (T)Application.Current.Resources[key];
}
