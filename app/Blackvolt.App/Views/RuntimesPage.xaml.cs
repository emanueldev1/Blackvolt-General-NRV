using Blackvolt.App.Interop;
using Microsoft.UI;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Storage.Pickers;
using Feature = Blackvolt.App.Interop.NativeEngine.Feature;
using State = Blackvolt.App.Interop.NativeEngine.RuntimeState;
using Origin = Blackvolt.App.Interop.NativeEngine.RuntimeOrigin;

namespace Blackvolt.App.Views;

internal sealed record RuntimeRow(int FeatureIndex, string Name, string Detail, string StateText, Brush StateBrush);

internal sealed record NrRow(int Variant, string Name, string Detail, string StateText, Brush StateBrush, bool Installed,
                             string DownloadText, List<string> BuildLabels, int BuildIndex);

public sealed partial class RuntimesPage : Page
{
    private static readonly Brush Ready = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 198, 255, 61));
    private static readonly Brush Warn = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 196, 61));
    private static readonly Brush Off = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 110, 110, 110));

    public RuntimesPage()
    {
        InitializeComponent();
        foreach (var name in Services.Mirrors.Names) NrMirror.Items.Add(name);
        NrMirror.SelectedIndex = Math.Clamp(Services.AppSettings.Current.Update.Mirror, 0, Services.Mirrors.Names.Length - 1);
        Services.NrRuntimeInstaller.Changed += OnInstallerChanged;
        Unloaded += (_, _) => Services.NrRuntimeInstaller.Changed -= OnInstallerChanged;
        Refresh();
        ShowInstaller();
    }

    // ---- DLSS NR -------------------------------------------------------------------------

    private void RefreshNr()
    {
        var rows = new List<NrRow>();
        foreach (var v in new[] { NativeEngine.NrVariant.Compat, NativeEngine.NrVariant.Original })
        {
            var info = Services.NrRuntimeInstaller.Info(v);
            var name = Services.NrRuntimeInstaller.DisplayName(v);
            var builds = Services.NrRuntimeInstaller.BuildsFor(v).ToList();
            var selected = Services.NrRuntimeInstaller.Selected(v);
            var labels = builds.Select(b => b.Label).ToList();
            var at = builds.IndexOf(selected);
            if (info.Installed == 0)
            {
                rows.Add(new NrRow((int)v, name, v == NativeEngine.NrVariant.Compat
                    ? $"Recomendado para todas las RTX. {selected.Size / 1048576.0:0} MB." : $"Solo GeForce RTX 50 (Blackwell). {selected.Size / 1048576.0:0} MB.",
                    "No instalado", Off, false, "Descargar", labels, at));
                continue;
            }
            var build = Services.NrRuntimeInstaller.Identify(info.Sha256Text);
            var verified = info.Origin != Origin.Unknown;
            var hash = info.Sha256Text is { Length: >= 12 } h ? $" · SHA-256 {h[..12]}…" : "";
            var detail = $"{(build?.Label ?? (info.LabelText.Length > 0 && verified ? info.LabelText : "nvngx_dlssnr.dll"))} · versión {info.VersionText} · " +
                         $"{info.Bytes / 1048576.0:0} MB{hash}";
            var differs = build is not null && build.Tag != selected.Tag;
            rows.Add(new NrRow((int)v, name, detail, verified ? "Instalado · verificado" : "Instalado · no catalogado",
                               verified ? Ready : Warn, true, differs ? "Cambiar" : "Reinstalar", labels, at));
        }
        NrList.ItemsSource = rows;
    }

    private void OnInstallerChanged() => DispatcherQueue.TryEnqueue(() =>
    {
        ShowInstaller();
        if (!Services.NrRuntimeInstaller.Busy)
        {
            RefreshNr();
            if (Services.NrRuntimeInstaller.Message.Length > 0)
                Show(Services.NrRuntimeInstaller.LastFailed ? InfoBarSeverity.Error : InfoBarSeverity.Success,
                     Services.NrRuntimeInstaller.Message);
        }
    });

    private void ShowInstaller()
    {
        var busy = Services.NrRuntimeInstaller.Busy;
        NrProgressPanel.Visibility = busy ? Visibility.Visible : Visibility.Collapsed;
        NrProgress.Value = Services.NrRuntimeInstaller.Fraction;
        NrProgressText.Text = Services.NrRuntimeInstaller.Message;
        NrDownloadAll.IsEnabled = !busy;
        NrList.IsEnabled = !busy;
    }

    private async void OnNrDownloadAll(object sender, RoutedEventArgs e)
    {
        // RTX 50 gets both builds; every other RTX only the compatible one.
        var name = App.Engine?.Adapter.Name ?? "";
        var rtx50 = name.Contains("RTX 50", StringComparison.OrdinalIgnoreCase);
        await Services.NrRuntimeInstaller.InstallAsync(rtx50
            ? [NativeEngine.NrVariant.Compat, NativeEngine.NrVariant.Original]
            : [NativeEngine.NrVariant.Compat]);
    }

    private async void OnNrDownload(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: int v }) return;
        await Services.NrRuntimeInstaller.InstallAsync([(NativeEngine.NrVariant)v]);
    }

    private void OnNrCancel(object sender, RoutedEventArgs e) => Services.NrRuntimeInstaller.Cancel();

    private void OnNrBuildChanged(object sender, SelectionChangedEventArgs e)
    {
        if (sender is not ComboBox { Tag: int v } box || box.SelectedIndex < 0) return;
        var variant = (NativeEngine.NrVariant)v;
        var builds = Services.NrRuntimeInstaller.BuildsFor(variant).ToList();
        if (box.SelectedIndex >= builds.Count || builds[box.SelectedIndex].Tag == Services.NrRuntimeInstaller.Selected(variant).Tag) return;
        Services.NrRuntimeInstaller.Select(variant, builds[box.SelectedIndex].Tag);
        RefreshNr();
    }

    private void OnNrMirrorChanged(object sender, SelectionChangedEventArgs e)
    {
        if (NrMirror.SelectedIndex < 0) return;
        Services.AppSettings.Current.Update.Mirror = NrMirror.SelectedIndex;
        Services.AppSettings.Current.Save();
    }

    private async void OnNrImport(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: int v }) return;
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.Downloads };
        picker.FileTypeFilter.Add(".dll");
        InitializePicker(picker);
        var file = await picker.PickSingleFileAsync();
        if (file is null) return;
        var info = new NativeEngine.NrRuntimeInfo { Size = (uint)System.Runtime.InteropServices.Marshal.SizeOf<NativeEngine.NrRuntimeInfo>() };
        var r = NativeEngine.NrRuntimeImport(file.Path, (NativeEngine.NrVariant)v, ref info);
        Show(r == NativeEngine.Result.Ok ? InfoBarSeverity.Success : InfoBarSeverity.Error, r switch
        {
            NativeEngine.Result.Ok => $"DLSS NR {Services.NrRuntimeInstaller.DisplayName((NativeEngine.NrVariant)v)} importado ({info.VersionText}).",
            NativeEngine.Result.Internal => "No se pudo copiar: el runtime está en uso. Cierra el reproductor o el streaming.",
            _ => "Ese archivo no es un nvngx_dlssnr.dll.",
        });
        RefreshNr();
    }

    private void OnNrRemove(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: int v }) return;
        var r = NativeEngine.NrRuntimeRemove((NativeEngine.NrVariant)v);
        if (r != NativeEngine.Result.Ok) Show(InfoBarSeverity.Warning, "No se pudo eliminar: el runtime está en uso o lo trae el paquete de la app.");
        RefreshNr();
    }

    private void Refresh()
    {
        RefreshNr();
        var engine = App.Engine;
        var rows = new List<RuntimeRow>();
        for (int i = 0; i < NativeEngine.FeatureCount; i++)
        {
            var feature = (Feature)i;
            if (feature == Feature.DlssNeuralRendering) continue; // own section above
            var name = Runtimes.DisplayName(feature);
            if (engine is null)
            {
                rows.Add(new RuntimeRow(i, name, Runtimes.DllName(feature), "Motor detenido", Off));
                continue;
            }
            var s = engine.GetRuntimeStatus(feature);
            var origin = s.Origin switch
            {
                Origin.Official => "Oficial NVIDIA",
                Origin.Community => "Build de la comunidad",
                _ => "Versión no verificada",
            };
            var detail = s.State == State.Missing ? $"Falta {Runtimes.DllName(feature)}" : $"{s.LabelText}  ·  {origin}";
            var (text, brush) = s.State switch
            {
                State.Ready => ("Listo", Ready),
                State.Found => ("Disponible", Ready),
                State.UnsupportedGpu => ("Tu GPU no lo soporta", Warn),
                State.DriverTooOld => ("Actualiza el driver", Warn),
                State.NgxUnavailable => ("Solo GPUs NVIDIA", Off),
                _ => ("No instalado", Off),
            };
            rows.Add(new RuntimeRow(i, name, detail, text, brush));
        }
        RuntimeList.ItemsSource = rows;
    }

    private async void OnImportFile(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: int index }) return;
        var feature = (Feature)index;
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.Downloads };
        picker.FileTypeFilter.Add(".dll");
        InitializePicker(picker);
        var file = await picker.PickSingleFileAsync();
        if (file is null) return;

        try
        {
            Runtimes.Import(file.Path, feature);
            App.RestartEngine();
            Show(InfoBarSeverity.Success, $"{Runtimes.DisplayName(feature)} importado.");
        }
        catch (EngineException)
        {
            Show(InfoBarSeverity.Error, $"Ese archivo no es {Runtimes.DllName(feature)} ni una versión conocida.");
        }
        Refresh();
    }

    private async void OnImportFromFolder(object sender, RoutedEventArgs e)
    {
        var picker = new FolderPicker { SuggestedStartLocation = PickerLocationId.Downloads };
        picker.FileTypeFilter.Add("*");
        InitializePicker(picker);
        var folder = await picker.PickSingleFolderAsync();
        if (folder is null) return;

        var candidates = Runtimes.Scan(folder.Path);
        if (candidates.Count == 0)
        {
            Show(InfoBarSeverity.Warning, "No se encontraron DLLs de DLSS ni RTX Video en esa carpeta.");
            return;
        }

        // One file per feature: the scan lists known builds first. DLSS NR builds go to their own
        // slot (Veyra keeps them in nr-ampere / nr-original).
        var imported = new List<string>();
        foreach (var c in candidates.Where(c => c.Feature == Feature.DlssNeuralRendering && c.NrVariant != NativeEngine.NrVariant.Auto)
                                    .GroupBy(c => c.NrVariant).Select(g => g.First()))
        {
            var info = new NativeEngine.NrRuntimeInfo { Size = (uint)System.Runtime.InteropServices.Marshal.SizeOf<NativeEngine.NrRuntimeInfo>() };
            if (NativeEngine.NrRuntimeImport(c.Path, c.NrVariant, ref info) == NativeEngine.Result.Ok)
                imported.Add("DLSS NR " + Services.NrRuntimeInstaller.DisplayName(c.NrVariant));
        }
        foreach (var c in candidates.Where(c => c.Feature != Feature.DlssNeuralRendering)
                                    .GroupBy(c => c.Feature).Select(g => g.First()))
        {
            try
            {
                Runtimes.Import(c.Path, c.Feature);
                imported.Add(Runtimes.DisplayName(c.Feature));
            }
            catch (EngineException) { }
        }
        App.RestartEngine();
        Show(imported.Count > 0 ? InfoBarSeverity.Success : InfoBarSeverity.Warning,
            imported.Count > 0 ? "Importado: " + string.Join(", ", imported) + "." : "No se pudo importar ninguna DLL.");
        Refresh();
    }

    private void Show(InfoBarSeverity severity, string message)
    {
        Notice.Severity = severity;
        Notice.Message = message;
        Notice.IsOpen = true;
    }

    private static void InitializePicker(object picker)
    {
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow!);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
    }
}
