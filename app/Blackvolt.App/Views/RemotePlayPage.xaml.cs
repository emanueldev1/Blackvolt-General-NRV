using Blackvolt.App.Interop;
using Blackvolt.App.Services;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using Windows.Storage.Pickers;
using Kind = Blackvolt.App.Interop.NativeRemote.ConsoleKind;
using NetState = Blackvolt.App.Interop.NativeRemote.DiscoveredState;

namespace Blackvolt.App.Views;

public sealed class ConsoleRow
{
    public uint Index { get; init; }
    public string Nickname { get; init; } = "";
    public string KindText { get; init; } = "";
    public string Detail { get; init; } = "";
    public string StatusText { get; init; } = "";
    public Brush StatusBrush { get; init; } = new SolidColorBrush(Colors.Gray);
    public string RunningApp { get; init; } = "";
    public Visibility RunningVisibility => string.IsNullOrEmpty(RunningApp) ? Visibility.Collapsed : Visibility.Visible;
    public Visibility WakeVisibility { get; init; } = Visibility.Collapsed;
}

public sealed class NetworkRow
{
    public string Name { get; init; } = "";
    public string Host { get; init; } = "";
    public string KindText { get; init; } = "";
    public string Detail { get; init; } = "";
    internal Kind Kind { get; init; }
}

public sealed partial class RemotePlayPage : Page
{
    private static readonly Brush ReadyBrush = new SolidColorBrush(ColorHelper.FromArgb(0xFF, 0xC6, 0xFF, 0x3D));
    private static readonly Brush StandbyBrush = new SolidColorBrush(ColorHelper.FromArgb(0xFF, 0xFF, 0xB0, 0x20));
    private static readonly Brush OfflineBrush = new SolidColorBrush(ColorHelper.FromArgb(0xFF, 0x70, 0x70, 0x70));

    private readonly DispatcherTimer _refresh = new() { Interval = TimeSpan.FromSeconds(1) };
    private ConsoleDiscovery? _discovery;
    private bool _loadingSettings;
    private string _lastSignature = "";

    public RemotePlayPage()
    {
        InitializeComponent();
        _refresh.Tick += (_, _) => Refresh();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        LoadSettings();
        var library = App.Consoles;
        if (library is null)
        {
            ShowNotice(InfoBarSeverity.Error, "Remote Play no está disponible", App.ConsolesError ?? "");
            return;
        }
        RestartDiscovery();
        _lastSignature = "";
        Refresh();
        _refresh.Start();
        var n = App.ConsolesImportedOnStart;
        if (n > 0)
        {
            ShowNotice(InfoBarSeverity.Success, "Consolas importadas",
                       $"Se importaron {n} consola(s) de chiaki-ng / Veyra. Ya puedes jugar sin volver a emparejar.");
            App.ConsolesImportedOnStart = 0;
        }
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        _refresh.Stop();
        _discovery?.Dispose();
        _discovery = null;
    }

    private void RestartDiscovery()
    {
        _discovery?.Dispose();
        _discovery = null;
        try
        {
            _discovery = ConsoleDiscovery.Start(App.Consoles);
        }
        catch (EngineException ex)
        {
            ShowNotice(InfoBarSeverity.Warning, "No se pudo buscar consolas en la red", ex.Message);
        }
    }

    // ---- lists ---------------------------------------------------------------------------

    private void Refresh()
    {
        var library = App.Consoles;
        if (library is null) return;
        var paired = library.List();
        var found = _discovery?.List() ?? [];

        var rows = paired.Select(p =>
        {
            var net = found.FirstOrDefault(d => d.PairedIndex == (int)p.Index);
            bool seen = net.Size != 0;
            var state = seen ? net.State : NetState.Unknown;
            var host = seen && net.HostText.Length > 0 ? net.HostText : p.Host;
            return new ConsoleRow
            {
                Index = p.Index,
                Nickname = p.Nickname.Length > 0 ? p.Nickname : (p.Kind == Kind.PS5 ? "PS5" : "PS4"),
                KindText = p.Kind == Kind.PS5 ? "PS5" : "PS4",
                Detail = string.Join("  ·  ", new[] { host.Length > 0 ? host : "sin dirección", SourceLabel(p.Source) }
                                                   .Where(s => s.Length > 0)),
                StatusText = state switch
                {
                    NetState.Ready => "Encendida",
                    NetState.Standby => "En reposo",
                    _ => "No detectada",
                },
                StatusBrush = state switch
                {
                    NetState.Ready => ReadyBrush,
                    NetState.Standby => StandbyBrush,
                    _ => OfflineBrush,
                },
                RunningApp = seen ? net.RunningAppText : "",
                WakeVisibility = state == NetState.Standby ? Visibility.Visible : Visibility.Collapsed,
            };
        }).ToList();

        var unpaired = found.Where(d => d.PairedIndex < 0).Select(d => new NetworkRow
        {
            Name = d.NameText.Length > 0 ? d.NameText : "Consola",
            Host = d.HostText,
            Kind = d.Kind,
            KindText = d.Kind == Kind.PS5 ? "PS5" : "PS4",
            Detail = $"{d.HostText}  ·  {(d.State == NetState.Standby ? "en reposo" : "encendida")}",
        }).ToList();

        // Only rebuild when something visible changed: no flicker, menus stay open.
        var signature = string.Join("|", rows.Select(r => $"{r.Index}{r.Nickname}{r.Detail}{r.StatusText}{r.RunningApp}")) + "#" +
                        string.Join("|", unpaired.Select(u => u.Host + u.Detail));
        if (signature == _lastSignature) return;
        _lastSignature = signature;

        ConsoleGrid.ItemsSource = rows;
        EmptyConsoles.Visibility = rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        NetworkList.ItemsSource = unpaired;
        NetworkEmpty.Text = unpaired.Count == 0
            ? (found.Count > 0
                ? "Todas las consolas encontradas ya están emparejadas."
                : "Buscando consolas… Deben estar encendidas o en reposo con Remote Play activado.")
            : "";
        NetworkEmpty.Visibility = unpaired.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private static string SourceLabel(string source) => source switch
    {
        "chiaki-ng" => "importada de chiaki-ng",
        "veyra" => "importada de Veyra",
        _ => "",
    };

    // ---- actions -------------------------------------------------------------------------

    private void OnPlay(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: uint index } || App.Consoles is null) return;
        var console = App.Consoles.List().FirstOrDefault(c => c.Index == index);
        if (console is null) return;
        if (console.Host.Length == 0)
        {
            ShowNotice(InfoBarSeverity.Warning, "Falta la dirección de la consola",
                       "Enciéndela en la misma red para detectarla, o escribe su IP en Más opciones › Cambiar nombre o dirección.");
            return;
        }
        Frame.Navigate(typeof(StreamPage), new StreamRequest(index, console.Nickname, console.Kind));
    }

    private void OnWakeConsole(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: uint index } || App.Consoles is null) return;
        var error = App.Consoles.Wake(index);
        if (error is null)
            ShowNotice(InfoBarSeverity.Informational, "Despertando la consola", "Suele tardar unos 15 segundos en estar lista.");
        else
            ShowNotice(InfoBarSeverity.Warning, "No se pudo despertar la consola", error);
    }

    private async void OnEditConsole(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: uint index } || App.Consoles is null) return;
        var console = App.Consoles.List().FirstOrDefault(c => c.Index == index);
        if (console is null) return;
        var name = new TextBox { Header = "Nombre", Text = console.Nickname };
        var host = new TextBox { Header = "Dirección IP o nombre de host", Text = console.Host, PlaceholderText = "192.168.1.50" };
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = "Editar consola",
            Content = new StackPanel { Spacing = 12, Children = { name, host } },
            PrimaryButtonText = "Guardar",
            CloseButtonText = "Cancelar",
            DefaultButton = ContentDialogButton.Primary,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        App.Consoles.Update(index, name.Text.Trim(), host.Text.Trim());
        RestartDiscovery(); // ping the new address directly
        _lastSignature = "";
        Refresh();
    }

    private async void OnRemoveConsole(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: uint index } || App.Consoles is null) return;
        var console = App.Consoles.List().FirstOrDefault(c => c.Index == index);
        if (console is null) return;
        var dialog = new ContentDialog
        {
            XamlRoot = XamlRoot,
            Title = $"¿Olvidar {console.Nickname}?",
            Content = "Tendrás que volver a emparejarla (o importarla otra vez) para jugar en ella.",
            PrimaryButtonText = "Olvidar",
            CloseButtonText = "Cancelar",
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;
        App.Consoles.Remove(index);
        RestartDiscovery();
        _lastSignature = "";
        Refresh();
    }

    private void OnImportInstalled(object sender, RoutedEventArgs e)
    {
        if (App.Consoles is null) return;
        try
        {
            var added = App.Consoles.ImportInstalled();
            ShowNotice(added > 0 ? InfoBarSeverity.Success : InfoBarSeverity.Informational, "Importación terminada",
                       added > 0
                           ? $"{added} consola(s) nueva(s). Las ya conocidas se actualizaron."
                           : "No hay consolas nuevas en chiaki-ng ni en Veyra para este usuario de Windows.");
            AfterImport();
        }
        catch (EngineException ex)
        {
            ShowNotice(InfoBarSeverity.Error, "No se pudo importar", ex.Message);
        }
    }

    private async void OnImportIni(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".ini");
        picker.FileTypeFilter.Add(".conf");
        InitPicker(picker);
        var file = await picker.PickSingleFileAsync();
        if (file is not null) ImportPath(file.Path);
    }

    private async void OnImportFolder(object sender, RoutedEventArgs e)
    {
        var picker = new FolderPicker();
        picker.FileTypeFilter.Add("*");
        InitPicker(picker);
        var folder = await picker.PickSingleFolderAsync();
        if (folder is not null) ImportPath(folder.Path);
    }

    private void ImportPath(string path)
    {
        if (App.Consoles is null) return;
        try
        {
            var added = App.Consoles.ImportPath(path);
            if (added is null)
                ShowNotice(InfoBarSeverity.Warning, "Sin emparejamientos",
                           "Ese archivo o carpeta no contiene consolas emparejadas que se puedan leer con este usuario de Windows.");
            else
                ShowNotice(InfoBarSeverity.Success, "Importación terminada", $"{added} consola(s) nueva(s).");
            AfterImport();
        }
        catch (EngineException ex)
        {
            ShowNotice(InfoBarSeverity.Error, "No se pudo importar", ex.Message);
        }
    }

    private void AfterImport()
    {
        RestartDiscovery();
        _lastSignature = "";
        Refresh();
    }

    private async void OnPairManual(object sender, RoutedEventArgs e) => await PairAsync(null, Kind.PS5);

    private async void OnPairDiscovered(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: string host }) return;
        var row = (NetworkList.ItemsSource as IEnumerable<NetworkRow>)?.FirstOrDefault(r => r.Host == host);
        await PairAsync(host, row?.Kind ?? Kind.PS5);
    }

    private async Task PairAsync(string? host, Kind kind)
    {
        if (App.Consoles is null) return;
        bool hasAccount = App.Consoles.List().Any(c => c.HasAccountId);
        var dialog = new PairingDialog(App.Consoles, host, kind, hasAccount) { XamlRoot = XamlRoot };
        if (await dialog.ShowAsync() == ContentDialogResult.Primary && dialog.Paired)
        {
            ShowNotice(InfoBarSeverity.Success, "Consola emparejada", "Ya aparece en tus consolas. Pulsa Jugar para empezar.");
            AfterImport();
        }
    }

    // ---- settings ------------------------------------------------------------------------

    private void LoadSettings()
    {
        _loadingSettings = true;
        var s = AppSettings.Current.RemotePlay;
        Select(ResolutionBox, s.Height.ToString());
        Select(FpsBox, s.Fps.ToString());
        Select(CodecBox, s.Codec.ToString());
        BitrateSlider.Value = s.BitrateKbps / 1000.0;
        ReconnectToggle.IsOn = s.AutoReconnect;
        WakeToggle.IsOn = s.WakeIfAsleep;
        DualSenseToggle.IsOn = s.DualSenseFeatures;
        UpdateBitrateLabel();
        _loadingSettings = false;
    }

    private static void Select(ComboBox box, string tag)
    {
        box.SelectedItem = box.Items.OfType<ComboBoxItem>().FirstOrDefault(i => (string)i.Tag == tag) ?? box.Items[0];
    }

    private void OnQualityChanged(object sender, RoutedEventArgs e) => SaveSettings();

    private void OnBitrateChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
    {
        UpdateBitrateLabel();
        SaveSettings();
    }

    private void UpdateBitrateLabel()
    {
        var mbps = (int)BitrateSlider.Value;
        BitrateLabel.Text = mbps == 0 ? "Bitrate: automático" : $"Bitrate: {mbps} Mbps";
    }

    private void SaveSettings()
    {
        if (_loadingSettings || ResolutionBox is null || FpsBox is null || CodecBox is null) return;
        var s = AppSettings.Current.RemotePlay;
        s.Height = int.Parse((string)((ComboBoxItem)ResolutionBox.SelectedItem).Tag);
        s.Fps = int.Parse((string)((ComboBoxItem)FpsBox.SelectedItem).Tag);
        s.Codec = int.Parse((string)((ComboBoxItem)CodecBox.SelectedItem).Tag);
        s.BitrateKbps = (int)BitrateSlider.Value * 1000;
        s.AutoReconnect = ReconnectToggle.IsOn;
        s.WakeIfAsleep = WakeToggle.IsOn;
        s.DualSenseFeatures = DualSenseToggle.IsOn;
        AppSettings.Current.Save();
    }

    // ---- helpers -------------------------------------------------------------------------

    private void ShowNotice(InfoBarSeverity severity, string title, string message)
    {
        Notice.Severity = severity;
        Notice.Title = title;
        Notice.Message = message;
        Notice.IsOpen = true;
    }

    private static void InitPicker(object picker) =>
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow!));
}

/// <summary>What the stream page needs to start.</summary>
internal sealed record StreamRequest(uint Index, string Nickname, Kind Kind);
